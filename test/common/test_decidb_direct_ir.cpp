#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

using namespace duckdb;

namespace {

LogicalDecide *FindDirectIRInput(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE) {
		return &op.Cast<LogicalDecide>();
	}
	for (auto &child : op.children) {
		if (auto *found = FindDirectIRInput(*child)) {
			return found;
		}
	}
	return nullptr;
}

//! IR for one DECIDE query, read before any DECIDE rewrite.
struct QueryIR {
	QueryIR(Connection &con, const string &sql) {
		REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
		// Reading the IR can rebind a coefficient, which reads the catalog; the optimizer always has a transaction.
		if (!con.HasActiveTransaction()) {
			con.BeginTransaction();
		}
		plan = con.ExtractPlan(sql);
		decide = FindDirectIRInput(*plan);
		REQUIRE(decide != nullptr);
		client = con.context.get();
		ir = DirectIR::Read(*client, *decide);
	}
	//! Rewrites every norm tag in the constraints to `payload` and reads the IR again.
	void RetagNorms(const string &payload) {
		ExpressionIterator::EnumerateExpression(decide->decide_constraints, [&](Expression &expression) {
			string old_payload;
			if (ExtractDecideTagPayload(expression.alias, NORM_MARKER_TAG_PREFIX, old_payload)) {
				RemoveDecideTag(expression.alias, string(NORM_MARKER_TAG_PREFIX) + old_payload + "__");
				AddDecideTag(expression.alias, string(NORM_MARKER_TAG_PREFIX) + payload + "__");
			}
		});
		ir = DirectIR::Read(*client, *decide);
	}
	duckdb::unique_ptr<LogicalOperator> plan;
	LogicalDecide *decide = nullptr;
	ClientContext *client = nullptr;
	DirectIR ir;
};

//! A query over three rows of (id, w, cap, flag) with the given declaration, constraints and objective.
string IRQuery(const string &declaration, const string &constraints, const string &objective) {
	return "SELECT id FROM (FROM (VALUES (1, 2.0::DOUBLE, 3, TRUE), (2, 1.0::DOUBLE, 1, FALSE),"
	       " (3, 4.0::DOUBLE, 2, TRUE)) t(id, w, cap, flag) DECIDE " +
	       declaration + " SUCH THAT " + constraints + " " + objective + ") q";
}

DirectSolveRule *FindRuleByName(duckdb::vector<duckdb::unique_ptr<DirectSolveRule>> &rules, const string &name) {
	for (auto &rule : rules) {
		if (string(rule->Name()) == name) {
			return rule.get();
		}
	}
	return nullptr;
}

DirectSolveRule *FindS1Rule(duckdb::vector<duckdb::unique_ptr<DirectSolveRule>> &rules) {
	return FindRuleByName(rules, "S1_UNIFORM_MATROID");
}

//! Whether `rule` proves the query, and why not when it does not. A rule that does not apply comes back as false.
bool RuleProves(DirectSolveRule &rule, Connection &con, const string &declaration, const string &constraints,
                const string &objective, string &reason, string *condition_text = nullptr) {
	QueryIR query(con, IRQuery(declaration, constraints, objective));
	reason.clear();
	auto condition = rule.CheckCondition(query.ir, *con.context, reason);
	if (!condition) {
		return false;
	}
	if (condition_text) {
		*condition_text = rule.Summary();
	}
	return true;
}

} // namespace

TEST_CASE("Direct-solve IR retains complete bound-clause attribution", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();

	auto plan = con.ExtractPlan("SELECT id, x FROM ("
	                            " FROM (VALUES (1, 9.0::DOUBLE), (2, -1.0::DOUBLE)) s(id,score)"
	                            " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)"
	                            ") q");
	auto *decide = FindDirectIRInput(*plan);
	REQUIRE(decide != nullptr);
	auto ir = DirectIR::Read(*con.context, *decide);
	REQUIRE(ir.decisions_status == DirectAnalysisStatus::PROVED);
	REQUIRE(ir.decisions.size() == 1);
	REQUIRE(ir.decisions[0].domain == DirectDomain::BOOL);
	REQUIRE(ir.decisions[0].scope == DecideVarScope::ROW);
	REQUIRE(ir.objective.status == DirectAnalysisStatus::PROVED);
	REQUIRE(ir.objective.parts.size() == 1);
	REQUIRE(ir.constraints_status == DirectAnalysisStatus::PROVED);
	REQUIRE(ir.source_clause_count == 1);
	REQUIRE(ir.constraints.size() == 1);
	REQUIRE(ir.constraints[0].status == DirectAnalysisStatus::PROVED);
	REQUIRE(ir.constraints[0].source_clause_id == 0);
	REQUIRE(ir.constraints[0].clause_text == "SUM(x) <= 1");
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	string reason;
	auto condition = s1->CheckCondition(ir, *con.context, reason);
	REQUIRE(condition != nullptr);
	REQUIRE(s1->Summary() == "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi");

	// Missing provenance must become a not-proved IR node, never an invented source id.
	decide->decide_constraints->SetAlias("");
	auto damaged = DirectIR::Read(*con.context, *decide);
	REQUIRE(damaged.constraints_status == DirectAnalysisStatus::PROVED);
	REQUIRE(damaged.constraints.size() == 1);
	REQUIRE(damaged.constraints[0].status == DirectAnalysisStatus::NOT_PROVED);
	REQUIRE(damaged.constraints[0].source_clause_id == DConstants::INVALID_INDEX);
	reason.clear();
	REQUIRE(s1->CheckCondition(damaged, *con.context, reason) == nullptr);
	REQUIRE(reason.find("constraint_provenance") != string::npos);
}

TEST_CASE("S1 IR preserves both interval bounds and exact equality", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	for (auto &constraint : {"SUM(x)>=1 AND SUM(x)<=2", "SUM(x)=2", "SUM(x)>=1"}) {
		auto plan = con.ExtractPlan("SELECT id,x FROM ("
		                            " FROM (VALUES (1, 9.0::DOUBLE), (2, -1.0::DOUBLE)) s(id,score)"
		                            " DECIDE x(BOOL) SUCH THAT " + string(constraint) +
		                            " MAXIMIZE SUM(score*x)) q");
		auto *decide = FindDirectIRInput(*plan);
		REQUIRE(decide != nullptr);
		auto ir = DirectIR::Read(*con.context, *decide);
		REQUIRE(ir.constraints_status == DirectAnalysisStatus::PROVED);
		REQUIRE(ir.constraints.size() == ir.source_clause_count);
		string reason;
		auto condition = s1->CheckCondition(ir, *con.context, reason);
		REQUIRE(condition != nullptr);
		REQUIRE(s1->Summary() == "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi");
	}
}

TEST_CASE("S1 condition normalizes finite count bounds", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	for (auto &constraint : {"SUM(x)<=1.5", "SUM(x)<2", "SUM(x)=1.5"}) {
		auto plan = con.ExtractPlan("SELECT id, x FROM ("
		                            " FROM (VALUES (1, 9.0::DOUBLE)) s(id,score)"
		                            " DECIDE x(BOOL) SUCH THAT " +
		                            string(constraint) + " MAXIMIZE SUM(score*x)) q");
		auto *decide = FindDirectIRInput(*plan);
		REQUIRE(decide != nullptr);
		auto ir = DirectIR::Read(*con.context, *decide);
		string reason;
		auto condition = s1->CheckCondition(ir, *con.context, reason);
		REQUIRE(condition != nullptr);
		REQUIRE(s1->Summary() == "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi");
	}
}

TEST_CASE("S1 condition rejects an unrepresentable bound and volatile coefficient", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	struct RejectedCase {
		string constraint;
		string objective;
		//! What the check this case is about writes, so an early shape check cannot pass for it.
		string reason_prefix;
	};
	const duckdb::vector<RejectedCase> cases = {
	    {"SUM(x)<=9007199254740993", "SUM(score*x)",
	     "constraint_shape: cardinality bound must be a finite consistent foldable numeric expression"},
	    {"SUM(x)<=1", "SUM((score+random())*x)",
	     "coefficient_shape: expected a deterministic numeric expression without decisions"},
	};
	for (auto &test : cases) {
		auto plan = con.ExtractPlan("SELECT id, x FROM ("
		                            " FROM (VALUES (1, 9.0::DOUBLE)) s(id,score)"
		                            " DECIDE x(BOOL) SUCH THAT " +
		                            test.constraint + " MAXIMIZE " + test.objective + ") q");
		auto *decide = FindDirectIRInput(*plan);
		REQUIRE(decide != nullptr);
		auto ir = DirectIR::Read(*con.context, *decide);
		string reason;
		REQUIRE(s1->CheckCondition(ir, *con.context, reason) == nullptr);
		INFO(reason);
		REQUIRE(reason.find(test.reason_prefix) == 0);
	}
}

TEST_CASE("Direct-solve IR retains WHEN and fail closed on unresolved PER keys", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();

	auto constraint_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                       " FROM (VALUES (1, 9.0::DOUBLE, TRUE)) s(id,score,active)"
	                                       " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 WHEN active"
	                                       " MAXIMIZE SUM(score*x)"
	                                       ") q");
	auto *constraint_decide = FindDirectIRInput(*constraint_plan);
	REQUIRE(constraint_decide != nullptr);
	auto constraint_ir = DirectIR::Read(*con.context, *constraint_decide);
	REQUIRE(constraint_ir.constraints_status == DirectAnalysisStatus::PROVED);
	REQUIRE(constraint_ir.constraints.size() == 1);
	REQUIRE(constraint_ir.constraints[0].scope.when != nullptr);
	REQUIRE(constraint_ir.constraints[0].scope.per_key_slots.empty());

	auto per_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                " FROM (VALUES (1, 9.0::DOUBLE, 'A'::VARCHAR),"
	                                " (2, -1.0::DOUBLE, 'B'::VARCHAR)) s(id,score,dept)"
	                                " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 PER dept"
	                                " MAXIMIZE SUM(score*x)"
	                                ") q");
	auto *per_decide = FindDirectIRInput(*per_plan);
	REQUIRE(per_decide != nullptr);
	auto per_ir = DirectIR::Read(*con.context, *per_decide);
	// ExtractPlan precedes the optimizer's reference resolution. Its PER key is
	// still a positional BoundReferenceExpression; do not invent a binding. The
	// clause is not proved and says why; the rest of the problem is still read.
	// End-to-end direct tests exercise the resolved key at the DECIDE boundary.
	REQUIRE(per_ir.constraints_status == DirectAnalysisStatus::PROVED);
	REQUIRE(per_ir.constraints.size() == 1);
	REQUIRE(per_ir.constraints[0].status == DirectAnalysisStatus::NOT_PROVED);
	REQUIRE(per_ir.constraints[0].reason.find("PER keys") != string::npos);
	REQUIRE(per_ir.FirstNotProvedReason().find("SUM(x) <= 1") != string::npos);

	auto objective_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                      " FROM (VALUES (1, 9.0::DOUBLE, TRUE)) s(id,score,active)"
	                                      " DECIDE x(BOOL) SUCH THAT SUM(x)<=1"
	                                      " MAXIMIZE SUM(score*x) WHEN active"
	                                      ") q");
	auto *objective_decide = FindDirectIRInput(*objective_plan);
	REQUIRE(objective_decide != nullptr);
	auto objective_ir = DirectIR::Read(*con.context, *objective_decide);
	// Objective WHEN is a modelled IR node now, not a not-proved one.
	REQUIRE(objective_ir.objective.status == DirectAnalysisStatus::PROVED);
	REQUIRE(objective_ir.objective.scope.when != nullptr);
	REQUIRE(objective_ir.objective.parts.size() == 1);
}

TEST_CASE("Direct-solve IR models every decision domain and scope", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryIR query(con, IRQuery("x(BOOL), n(INT), r(REAL), scalar s(INT), t.e(BOOL)",
	                           "x <= 1 AND n <= 3 AND r <= 2 AND s <= 4 AND e <= 1",
	                           "MAXIMIZE SUM(w*x) + SUM(n) + SUM(r) + s + SUM(e)"));
	auto &ir = query.ir;
	REQUIRE(ir.decisions_status == DirectAnalysisStatus::PROVED);
	REQUIRE(ir.decisions.size() == 5);
	REQUIRE(ir.decisions[0].domain == DirectDomain::BOOL);
	REQUIRE(ir.decisions[0].output_type == LogicalType::INTEGER);
	REQUIRE(ir.decisions[1].domain == DirectDomain::INT);
	REQUIRE(ir.decisions[2].domain == DirectDomain::REAL);
	REQUIRE(ir.decisions[3].scope == DecideVarScope::SCALAR);
	REQUIRE(ir.decisions[4].scope == DecideVarScope::ENTITY);
	REQUIRE(ir.decisions[4].entity_scope == 0);
	REQUIRE(ir.entity_scopes.size() == 1);
	REQUIRE(ir.entity_scopes[0].status == DirectAnalysisStatus::PROVED);
	REQUIRE(!ir.entity_scopes[0].key_slots.empty());
	// The query-wide decision is a row-level part beside the reducers.
	REQUIRE(ir.objective.parts.size() == 5);
	REQUIRE(ir.objective.parts[3].reducer == DirectReducer::NONE);
}

TEST_CASE("Direct-solve IR models comparisons, membership, and right-hand sides", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryIR query(con, IRQuery("x(INT), y(INT)",
	                           "x + y <= 3 AND x <> 1 AND y IN (0, 2) AND SUM(x) <= cap AND "
	                           "SUM(y) <= (SELECT 5) AND SUM(x) BETWEEN 1 AND 2",
	                           "MAXIMIZE SUM(w*x)"));
	auto &constraints = query.ir.constraints;
	REQUIRE(query.ir.constraints_status == DirectAnalysisStatus::PROVED);
	REQUIRE(constraints.size() == 7);
	for (auto &constraint : constraints) {
		REQUIRE(constraint.status == DirectAnalysisStatus::PROVED);
	}
	// One constraint per row, holding both decisions.
	REQUIRE(!constraints[0].aggregate);
	REQUIRE(constraints[0].lhs.size() == 1);
	REQUIRE(constraints[0].lhs[0].reducer == DirectReducer::NONE);
	REQUIRE(constraints[0].lhs[0].terms.size() == 2);
	REQUIRE(constraints[1].comparison == ExpressionType::COMPARE_NOTEQUAL);
	REQUIRE(constraints[2].comparison == ExpressionType::COMPARE_IN);
	REQUIRE(constraints[3].aggregate);
	// BETWEEN is two written comparisons, each with its own clause.
	REQUIRE(constraints[6].source_clause_id == constraints[5].source_clause_id + 1);
	REQUIRE(constraints[5].comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO);
	REQUIRE(constraints[6].comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO);
}

TEST_CASE("Direct-solve IR models reducers, filters, qualifiers, scales and norms", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryIR query(con, IRQuery("t.x(INT), y(INT)",
	                           "AVG(y) <= 2 AND MIN(y) >= 0 AND MAX(y) <= 3 AND SUM(y) WHEN flag <= 2 AND "
	                           "SUM(t: x) <= 4 AND 2 * SUM(y) <= 9 AND norm(y, 0) <= 2 AND "
	                           "norm(y - cap, 1) <= 5",
	                           "MINIMIZE SUM(w*y)"));
	auto &constraints = query.ir.constraints;
	REQUIRE(constraints.size() == 8);
	for (idx_t i = 0; i < constraints.size(); i++) {
		// `norm(y, 0)` needs indicator variables: it reads as a not-proved part, not as a reducer.
		REQUIRE(constraints[i].status == (i == 6 ? DirectAnalysisStatus::NOT_PROVED : DirectAnalysisStatus::PROVED));
		REQUIRE(constraints[i].lhs.size() == 1);
	}
	REQUIRE(constraints[0].lhs[0].reducer == DirectReducer::AVG);
	REQUIRE(constraints[1].lhs[0].reducer == DirectReducer::MIN);
	REQUIRE(constraints[2].lhs[0].reducer == DirectReducer::MAX);
	REQUIRE(constraints[3].lhs[0].filter != nullptr);
	REQUIRE(constraints[4].lhs[0].qualifier == 0);
	REQUIRE(constraints[5].lhs[0].scale != nullptr);
	REQUIRE(constraints[6].lhs[0].status == DirectAnalysisStatus::NOT_PROVED);
	REQUIRE(constraints[6].lhs[0].reason == "norm of order 0 is not modelled");
	REQUIRE(constraints[6].reason == constraints[6].lhs[0].reason);
	// L1 reads as its definition: SUM of an absolute value.
	REQUIRE(constraints[7].lhs[0].reducer == DirectReducer::SUM);
	REQUIRE(constraints[7].lhs[0].terms.size() == 1);
	REQUIRE(constraints[7].lhs[0].terms[0].kind == DecideTermKind::ABS);
}

TEST_CASE("Direct-solve IR reads an unexpected norm tag as a not-proved part, never an exception", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryIR query(con, IRQuery("y(INT)", "norm(y, 0) <= 2", "MINIMIZE SUM(w*y)"));
	REQUIRE(query.ir.constraints.size() == 1);
	REQUIRE(query.ir.constraints[0].status == DirectAnalysisStatus::NOT_PROVED);
	// An order canonicalization would have expanded, a malformed order 0, and a tag that is not a number.
	for (string payload : {"2", "0_not_a_number", "garbage"}) {
		DYNAMIC_SECTION("payload '" << payload << "'") {
			REQUIRE_NOTHROW(query.RetagNorms(payload));
			auto &constraint = query.ir.constraints[0];
			REQUIRE(constraint.status == DirectAnalysisStatus::NOT_PROVED);
			REQUIRE(constraint.lhs[0].status == DirectAnalysisStatus::NOT_PROVED);
			REQUIRE(!constraint.lhs[0].reason.empty());
		}
	}
}

TEST_CASE("Direct-solve IR models products, squares, and nested objective reducers", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryIR products(con,
	                 IRQuery("x(INT), y(INT)", "SUM(x*y) <= 4 AND SUM(POWER(x - cap, 2)) <= 9", "MAXIMIZE SUM(w*x)"));
	auto &constraints = products.ir.constraints;
	REQUIRE(constraints.size() == 2);
	REQUIRE(constraints[0].lhs[0].terms[0].kind == DecideTermKind::PRODUCT);
	REQUIRE(constraints[0].degree == 2);
	REQUIRE(constraints[1].lhs[0].terms[0].kind == DecideTermKind::SQUARE);

	QueryIR nested(con, IRQuery("x(INT)", "x <= 3", "MINIMIZE SUM(MAX(w*x)) PER id"));
	auto &parts = nested.ir.objective.parts;
	REQUIRE(parts.size() == 1);
	REQUIRE(parts[0].reducer == DirectReducer::SUM);
	REQUIRE(parts[0].inner != nullptr);
	REQUIRE(parts[0].inner->reducer == DirectReducer::MAX);
	REQUIRE(parts[0].terms.empty());
}

TEST_CASE("Direct-solve IR reports what it cannot model as not proved, with a reason", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// A decision under ABS inside a square is not linear algebra the splitter can read.
	QueryIR query(con, IRQuery("x(INT)", "x <= 3 AND SUM(POWER(ABS(x - cap), 2)) <= 9", "MAXIMIZE SUM(w*x)"));
	auto &ir = query.ir;
	REQUIRE(ir.constraints_status == DirectAnalysisStatus::PROVED);
	REQUIRE(ir.constraints.size() == 2);
	REQUIRE(ir.constraints[0].status == DirectAnalysisStatus::PROVED);
	REQUIRE(ir.constraints[1].status == DirectAnalysisStatus::NOT_PROVED);
	REQUIRE(!ir.constraints[1].reason.empty());
	REQUIRE(ir.FirstNotProvedReason().find("clause `") != string::npos);
}

TEST_CASE("Direct-solve IR says which bounds the solver folds into a variable's box", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);

	// One decision, constraints that each produce one IR node in order; `expected` is the solver's absorption of each.
	auto check = [&](const string &declaration, const string &constraints, duckdb::vector<bool> expected) {
		QueryIR query(con, IRQuery(declaration, constraints, "MINIMIZE SUM(x)"));
		auto &ir = query.ir;
		INFO(declaration << " | " << constraints);
		REQUIRE(ir.constraints.size() == expected.size());
		for (idx_t i = 0; i < expected.size(); i++) {
			REQUIRE(ir.constraints[i].variable_bound == expected[i]);
		}
	};

	// A bare decision against a finite constant is folded into the box, whatever the side or the spelling of the
	// constant. BETWEEN arrives as two comparisons.
	check("x(REAL)", "x >= -3", {true});
	check("x(REAL)", "-3 <= x", {true});
	check("x(REAL)", "x <= 5", {true});
	check("x(REAL)", "x = 2", {true});
	check("x(REAL)", "x >= CAST(-3 AS DOUBLE)", {true});
	check("x(REAL)", "x >= -(3)", {true});
	check("x(REAL)", "x BETWEEN -3 AND 5", {true, true});
	check("x(INT)", "x < 3 AND x > -2", {true, true});
	check("x(BOOL)", "x <= 1", {true});

	// Anything else only intersects with the default box: a column, a WHEN, a coefficient or offset on the decision, an
	// arithmetic right side, and a comparison that is not a bound.
	check("x(REAL)", "x >= cap", {false});
	check("x(REAL)", "x >= w", {false});
	check("x(REAL)", "x >= -3 WHEN flag", {false});
	check("x(REAL)", "2*x >= -6", {false});
	check("x(REAL)", "x + 1 >= -3", {false});
	check("x(REAL)", "-x <= 3", {false});
	check("x(REAL)", "x >= 0 - 3", {false});
	check("x(INT)", "x <> 3", {false});
	check("x(INT)", "x < cap", {false});

	// A bound mixed with ones that are not folded keeps each clause's own answer.
	check("x(REAL)", "x >= -3 AND x <= cap AND x >= -1 WHEN flag", {true, false, false});
}

TEST_CASE("A1 IR reads each objective spelling the solver accepts", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);

	// A square carries its scale and its linear inner terms: x - w is [LINEAR x, CONSTANT w with sign -1].
	QueryIR square(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2))"));
	REQUIRE(square.ir.objective.status == DirectAnalysisStatus::PROVED);
	REQUIRE(square.ir.objective.parts.size() == 1);
	auto &terms = square.ir.objective.parts[0].terms;
	REQUIRE(terms.size() == 1);
	REQUIRE(terms[0].kind == DecideTermKind::SQUARE);
	REQUIRE(terms[0].square_scale == 1.0);
	REQUIRE(terms[0].inner.size() == 2);
	REQUIRE(terms[0].inner[0].kind == DecideTermKind::LINEAR);
	REQUIRE(terms[0].inner[1].kind == DecideTermKind::CONSTANT);
	REQUIRE(terms[0].inner[1].sign == -1);

	// A constant factor inside the SUM folds into the square's scale; one outside stays on the part.
	QueryIR inside(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(3*POWER(x - w, 2))"));
	REQUIRE(inside.ir.objective.parts[0].terms[0].square_scale == 3.0);
	QueryIR outside(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE 2*SUM(POWER(x - w, 2))"));
	REQUIRE(outside.ir.objective.parts[0].scale != nullptr);
	REQUIRE(!outside.ir.objective.parts[0].scale_divides);
	QueryIR divided(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2))/2"));
	REQUIRE(divided.ir.objective.parts[0].scale_divides);
	QueryIR negated(con, IRQuery("x(REAL)", "x <= cap", "MAXIMIZE -SUM(POWER(x - w, 2))"));
	REQUIRE(negated.ir.objective.parts[0].sign == -1);

	// x*x - 2*w*x is a square plus a negative linear term; a constant stays beside the square.
	QueryIR expanded(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(x*x - 2*w*x + 5)"));
	auto &expanded_terms = expanded.ir.objective.parts[0].terms;
	REQUIRE(expanded_terms.size() == 3);
	REQUIRE(expanded_terms[0].kind == DecideTermKind::SQUARE);
	REQUIRE(expanded_terms[1].kind == DecideTermKind::LINEAR);
	REQUIRE(expanded_terms[1].sign == -1);
	REQUIRE(expanded_terms[2].kind == DecideTermKind::CONSTANT);

	// A row-valued coefficient inside the square is allowed; one outside it, or x*x with a coefficient, is not
	// modelled.
	QueryIR inner_weight(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(w*x - 3, 2))"));
	REQUIRE(inner_weight.ir.objective.status == DirectAnalysisStatus::PROVED);
	QueryIR outer_weight(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(w*POWER(x - 1, 2))"));
	REQUIRE(outer_weight.ir.objective.status == DirectAnalysisStatus::NOT_PROVED);
	QueryIR cubic(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(w*x*x)"));
	REQUIRE(cubic.ir.objective.status == DirectAnalysisStatus::NOT_PROVED);

	// An objective-level WHEN is a scope on the objective; no objective is no part at all.
	QueryIR when(con, IRQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2)) WHEN flag"));
	REQUIRE(when.ir.objective.scope.when != nullptr);
	QueryIR none(con, IRQuery("x(REAL)", "x <= cap", ""));
	REQUIRE(none.ir.objective.sense == DecideSense::FEASIBILITY);
	REQUIRE(none.ir.objective.parts.empty());

	// A per-row bound with a coefficient keeps the coefficient in the term; the constant moves to the right side.
	QueryIR affine(con, IRQuery("x(REAL)", "2*x + 1 <= cap", "MAXIMIZE SUM(x)"));
	REQUIRE(affine.ir.constraints[0].lhs[0].terms.size() == 1);
	REQUIRE(affine.ir.constraints[0].lhs[0].terms[0].kind == DecideTermKind::LINEAR);

	// The output type follows the declared type.
	QueryIR types(con, IRQuery("x(REAL), n(INT), b(BOOL)", "x <= cap AND n <= cap AND b <= 1", "MAXIMIZE SUM(x)"));
	REQUIRE(types.ir.decisions[0].output_type == LogicalType::DOUBLE);
	REQUIRE(types.ir.decisions[1].output_type == LogicalType::BIGINT);
	REQUIRE(types.ir.decisions[2].output_type == LogicalType::INTEGER);
}

TEST_CASE("A1 proves a separable box-constrained problem and names why it declines anything else", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto rules = RegisteredDirectRules();
	auto *a1 = FindRuleByName(rules, "A1_SEPARABLE_BOX");
	REQUIRE(a1 != nullptr);
	string reason, condition;

	// What it proves.
	REQUIRE(
	    RuleProves(*a1, con, "x(REAL)", "x BETWEEN 0 AND cap", "MINIMIZE SUM(POWER(x - w, 2))", reason, &condition));
	REQUIRE(condition == "min/max SUM(c*x), l <= x <= u, each row independent");
	REQUIRE(RuleProves(*a1, con, "x(INT)", "x <= cap AND x >= 0", "MAXIMIZE SUM(w*x)", reason));
	REQUIRE(RuleProves(*a1, con, "x(BOOL)", "x <= flag", "MAXIMIZE SUM(w*x)", reason));
	REQUIRE(RuleProves(*a1, con, "x(REAL), y(INT)", "x <= cap AND y <= 5", "MINIMIZE SUM(x) + SUM(w*y)", reason));
	REQUIRE(RuleProves(*a1, con, "x(REAL)", "x <= cap", "", reason));
	REQUIRE(
	    RuleProves(*a1, con, "x(REAL)", "2*x <= cap AND w*x >= -1 AND x >= -3 WHEN flag", "MAXIMIZE SUM(x)", reason));
	REQUIRE(RuleProves(*a1, con, "x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(w*x - 3, 2)) WHEN flag", reason));
	// A cast that can throw is read per row and raises as the solver does, so it is admitted.
	REQUIRE(RuleProves(*a1, con, "x(REAL)", "x <= CAST(cap AS TINYINT)", "MAXIMIZE SUM(x)", reason));

	// What it declines, and the reason it names.
	auto declines = [&](const string &declaration, const string &constraints, const string &objective,
	                    const string &expected) {
		INFO(declaration << " | " << constraints << " | " << objective);
		REQUIRE(!RuleProves(*a1, con, declaration, constraints, objective, reason));
		REQUIRE(reason.find(expected) != string::npos);
	};
	declines("x(REAL)", "SUM(x) <= 5", "MAXIMIZE SUM(w*x)", "found an aggregate constraint");
	declines("x(REAL), y(REAL)", "x + y <= 3", "MAXIMIZE SUM(x)", "constraint_shape");
	declines("x(REAL), y(REAL)", "x <= cap AND y <= cap", "MAXIMIZE SUM(x*y)", "objective_shape");
	declines("x(INT)", "x <= cap AND x <> 2", "MAXIMIZE SUM(w*x)", "found <> or IN");
	declines("x(INT)", "x IN (1, 2, 3)", "MAXIMIZE SUM(w*x)", "found <> or IN");
	declines("x(REAL)", "x <= cap", "MAXIMIZE SUM(w*x) PER id", "objective_not_proved");
	declines("x(REAL)", "x <= cap", "MAXIMIZE AVG(w*x)", "objective_shape");
	declines("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2)) + SUM(POWER(x - cap, 2))",
	         "at most one squared term");
	declines("x(REAL)", "x <= cap", "MINIMIZE SUM(w*POWER(x - 1, 2))", "objective_not_proved");
	declines("x(REAL)", "x <= cap", "MAXIMIZE SUM(random()*x)", "coefficient_shape");
	declines("x(REAL)", "x <= cap WHEN (random() < 2)", "MAXIMIZE SUM(x)", "constraint_scope");
	declines("x(REAL)", "POWER(x, 2) <= 9", "MAXIMIZE SUM(x)", "constraint_shape");
	declines("x(REAL)", "ABS(x) <= 3", "MAXIMIZE SUM(x)", "constraint_");
	declines("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(w*x - 1, 2)) + SUM(POWER(x - 1, 2))",
	         "at most one squared term");
}

TEST_CASE("S1 and A1 never both prove one query", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	auto *a1 = FindRuleByName(rules, "A1_SEPARABLE_BOX");
	REQUIRE(s1 != nullptr);
	REQUIRE(a1 != nullptr);
	string reason;
	struct Shape {
		string declaration, constraints, objective;
		bool s1, a1;
	};
	duckdb::vector<Shape> shapes = {
	    // S1 needs one count bound on SUM(x); A1 forbids any aggregate constraint.
	    {"x(BOOL)", "SUM(x) <= 1", "MAXIMIZE SUM(w*x)", true, false},
	    {"x(BOOL)", "SUM(x) >= 1 AND SUM(x) <= 2", "MAXIMIZE SUM(w*x)", true, false},
	    {"x(BOOL)", "SUM(x) <= 1 AND x <= 1", "MAXIMIZE SUM(w*x)", true, false},
	    {"x(BOOL)", "x <= flag", "MAXIMIZE SUM(w*x)", false, true},
	    {"x(BOOL)", "x <= 1", "MAXIMIZE SUM(w*x)", false, true},
	    {"x(BOOL)", "x = 1", "MINIMIZE SUM(w*x)", false, true},
	    {"x(BOOL)", "SUM(x) <= 1", "MAXIMIZE SUM(POWER(x - w, 2))", false, false},
	    {"x(INT)", "SUM(x) <= 5", "MAXIMIZE SUM(w*x)", false, false},
	    {"x(INT)", "x <= 5", "MAXIMIZE SUM(w*x)", false, true},
	    {"x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2))", false, true},
	    {"x(BOOL), y(BOOL)", "SUM(x) <= 1 AND y <= 1", "MAXIMIZE SUM(w*x)", false, false},
	};
	for (auto &shape : shapes) {
		INFO(shape.declaration << " | " << shape.constraints << " | " << shape.objective);
		bool proves_s1 = RuleProves(*s1, con, shape.declaration, shape.constraints, shape.objective, reason);
		bool proves_a1 = RuleProves(*a1, con, shape.declaration, shape.constraints, shape.objective, reason);
		REQUIRE(!(proves_s1 && proves_a1));
		REQUIRE(proves_s1 == shape.s1);
		REQUIRE(proves_a1 == shape.a1);
	}
}
