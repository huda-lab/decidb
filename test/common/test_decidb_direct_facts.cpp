#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

using namespace duckdb;

namespace {

LogicalDecide *FindDirectFactsInput(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE) {
		return &op.Cast<LogicalDecide>();
	}
	for (auto &child : op.children) {
		if (auto *found = FindDirectFactsInput(*child)) {
			return found;
		}
	}
	return nullptr;
}

//! Facts for one DECIDE query, read before any DECIDE rewrite.
struct QueryFacts {
	QueryFacts(Connection &con, const string &sql) {
		REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
		// Reading facts can rebind a coefficient, which reads the catalog; the optimizer always has a transaction.
		if (!con.HasActiveTransaction()) {
			con.BeginTransaction();
		}
		plan = con.ExtractPlan(sql);
		auto decide = FindDirectFactsInput(*plan);
		REQUIRE(decide != nullptr);
		facts = DirectProblemFacts::Read(*con.context, *decide);
	}
	duckdb::unique_ptr<LogicalOperator> plan;
	DirectProblemFacts facts;
};

//! A query over three rows of (id, w, cap, flag) with the given declaration, constraints and objective.
string FactsQuery(const string &declaration, const string &constraints, const string &objective) {
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
	return FindRuleByName(rules, "S1_CARDINALITY_INTERVAL");
}

//! Whether `rule` proves the query, and why not when it does not. A miss in Match or in Prove both come back as false.
bool RuleProves(DirectSolveRule &rule, Connection &con, const string &declaration, const string &constraints,
                const string &objective, string &reason, string *proof_text = nullptr) {
	QueryFacts query(con, FactsQuery(declaration, constraints, objective));
	reason.clear();
	auto match = rule.Match(query.facts, reason);
	if (!match) {
		return false;
	}
	auto proof = rule.Prove(query.facts, *match, *con.context, reason);
	if (!proof) {
		return false;
	}
	if (proof_text) {
		DirectSolveDecisionRecord record;
		rule.Explain(*proof, record);
		*proof_text = record.proof;
	}
	return true;
}

} // namespace

TEST_CASE("Direct-solve facts retain complete bound-clause attribution", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();

	auto plan = con.ExtractPlan("SELECT id, x FROM ("
	                            " FROM (VALUES (1, 9.0::DOUBLE), (2, -1.0::DOUBLE)) s(id,score)"
	                            " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)"
	                            ") q");
	auto *decide = FindDirectFactsInput(*plan);
	REQUIRE(decide != nullptr);
	auto facts = DirectProblemFacts::Read(*con.context, *decide);
	REQUIRE(facts.decisions_status == DirectFactStatus::KNOWN);
	REQUIRE(facts.decisions.size() == 1);
	REQUIRE(facts.decisions[0].domain == DirectDomain::BOOL);
	REQUIRE(facts.decisions[0].scope == DecideVarScope::ROW);
	REQUIRE(facts.objective.status == DirectFactStatus::KNOWN);
	REQUIRE(facts.objective.parts.size() == 1);
	REQUIRE(facts.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(facts.source_clause_count == 1);
	REQUIRE(facts.constraints.size() == 1);
	REQUIRE(facts.constraints[0].status == DirectFactStatus::KNOWN);
	REQUIRE(facts.constraints[0].source_clause_id == 0);
	REQUIRE(facts.constraints[0].clause_text == "SUM(x) <= 1");
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	string reason;
	auto match = s1->Match(facts, reason);
	REQUIRE(match != nullptr);
	auto proof = s1->Prove(facts, *match, *con.context, reason);
	REQUIRE(proof != nullptr);
	DirectSolveDecisionRecord record;
	s1->Explain(*proof, record);
	REQUIRE(record.proof.find("cardinality interval [0, 1]") != string::npos);

	// Missing provenance must become an unknown fact, never an invented source id.
	decide->decide_constraints->SetAlias("");
	auto damaged = DirectProblemFacts::Read(*con.context, *decide);
	REQUIRE(damaged.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(damaged.constraints.size() == 1);
	REQUIRE(damaged.constraints[0].status == DirectFactStatus::UNKNOWN);
	REQUIRE(damaged.constraints[0].source_clause_id == DConstants::INVALID_INDEX);
	reason.clear();
	REQUIRE(s1->Match(damaged, reason) == nullptr);
	REQUIRE(reason.find("constraint_provenance") != string::npos);
}

TEST_CASE("S1 facts preserve both interval bounds and exact equality", "[decidb]") {
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
		auto *decide = FindDirectFactsInput(*plan);
		REQUIRE(decide != nullptr);
		auto facts = DirectProblemFacts::Read(*con.context, *decide);
		REQUIRE(facts.constraints_status == DirectFactStatus::KNOWN);
		REQUIRE(facts.constraints.size() == facts.source_clause_count);
		string reason;
		auto match = s1->Match(facts, reason);
		REQUIRE(match != nullptr);
		auto proof = s1->Prove(facts, *match, *con.context, reason);
		REQUIRE(proof != nullptr);
		DirectSolveDecisionRecord record;
		s1->Explain(*proof, record);
		REQUIRE(record.proof.find("global") != string::npos);
	}
}

TEST_CASE("S1 proof normalizes finite count bounds", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	for (auto &test : duckdb::vector<duckdb::pair<string, string>> {
	         {"SUM(x)<=1.5", "cardinality interval [0, 1]"},
	         {"SUM(x)<2", "cardinality interval [0, 1]"},
	         {"SUM(x)=1.5", "infeasible for a nonempty eligible group"},
	     }) {
		auto plan = con.ExtractPlan("SELECT id, x FROM ("
		                            " FROM (VALUES (1, 9.0::DOUBLE)) s(id,score)"
		                            " DECIDE x(BOOL) SUCH THAT " +
		                            test.first + " MAXIMIZE SUM(score*x)) q");
		auto *decide = FindDirectFactsInput(*plan);
		REQUIRE(decide != nullptr);
		auto facts = DirectProblemFacts::Read(*con.context, *decide);
		string reason;
		auto match = s1->Match(facts, reason);
		REQUIRE(match != nullptr);
		auto proof = s1->Prove(facts, *match, *con.context, reason);
		REQUIRE(proof != nullptr);
		DirectSolveDecisionRecord record;
		s1->Explain(*proof, record);
		REQUIRE(record.proof.find(test.second) != string::npos);
	}
}

TEST_CASE("S1 proof rejects an unrepresentable bound and volatile coefficient", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	const duckdb::vector<duckdb::pair<string, string>> cases = {
	    {"SUM(x)<=9007199254740993", "SUM(score*x)"},
	    {"SUM(x)<=1", "SUM((score+random())*x)"},
	};
	for (auto &test : cases) {
		auto plan = con.ExtractPlan("SELECT id, x FROM ("
		                            " FROM (VALUES (1, 9.0::DOUBLE)) s(id,score)"
		                            " DECIDE x(BOOL) SUCH THAT " +
		                            test.first + " MAXIMIZE " + test.second + ") q");
		auto *decide = FindDirectFactsInput(*plan);
		REQUIRE(decide != nullptr);
		auto facts = DirectProblemFacts::Read(*con.context, *decide);
		string reason;
		auto match = s1->Match(facts, reason);
		REQUIRE(match != nullptr);
		REQUIRE(s1->Prove(facts, *match, *con.context, reason) == nullptr);
	}
}

TEST_CASE("Direct-solve facts retain WHEN and fail closed on unresolved PER keys", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();

	auto constraint_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                       " FROM (VALUES (1, 9.0::DOUBLE, TRUE)) s(id,score,active)"
	                                       " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 WHEN active"
	                                       " MAXIMIZE SUM(score*x)"
	                                       ") q");
	auto *constraint_decide = FindDirectFactsInput(*constraint_plan);
	REQUIRE(constraint_decide != nullptr);
	auto constraint_facts = DirectProblemFacts::Read(*con.context, *constraint_decide);
	REQUIRE(constraint_facts.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(constraint_facts.constraints.size() == 1);
	REQUIRE(constraint_facts.constraints[0].scope.when != nullptr);
	REQUIRE(constraint_facts.constraints[0].scope.per_key_slots.empty());

	auto per_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                " FROM (VALUES (1, 9.0::DOUBLE, 'A'::VARCHAR),"
	                                " (2, -1.0::DOUBLE, 'B'::VARCHAR)) s(id,score,dept)"
	                                " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 PER dept"
	                                " MAXIMIZE SUM(score*x)"
	                                ") q");
	auto *per_decide = FindDirectFactsInput(*per_plan);
	REQUIRE(per_decide != nullptr);
	auto per_facts = DirectProblemFacts::Read(*con.context, *per_decide);
	// ExtractPlan precedes the optimizer's reference resolution. Its PER key is
	// still a positional BoundReferenceExpression; do not invent a binding. The
	// clause is unknown and says why; the rest of the problem is still read.
	// End-to-end direct tests exercise the resolved key at the DECIDE boundary.
	REQUIRE(per_facts.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(per_facts.constraints.size() == 1);
	REQUIRE(per_facts.constraints[0].status == DirectFactStatus::UNKNOWN);
	REQUIRE(per_facts.constraints[0].reason.find("PER keys") != string::npos);
	REQUIRE(per_facts.FirstUnknownReason().find("SUM(x) <= 1") != string::npos);

	auto objective_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                      " FROM (VALUES (1, 9.0::DOUBLE, TRUE)) s(id,score,active)"
	                                      " DECIDE x(BOOL) SUCH THAT SUM(x)<=1"
	                                      " MAXIMIZE SUM(score*x) WHEN active"
	                                      ") q");
	auto *objective_decide = FindDirectFactsInput(*objective_plan);
	REQUIRE(objective_decide != nullptr);
	auto objective_facts = DirectProblemFacts::Read(*con.context, *objective_decide);
	// Objective WHEN is a modelled fact now, not an unknown one.
	REQUIRE(objective_facts.objective.status == DirectFactStatus::KNOWN);
	REQUIRE(objective_facts.objective.scope.when != nullptr);
	REQUIRE(objective_facts.objective.parts.size() == 1);
}

TEST_CASE("Direct-solve facts model every decision domain and scope", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryFacts query(con, FactsQuery("x(BOOL), n(INT), r(REAL), scalar s(INT), t.e(BOOL)",
	                                 "x <= 1 AND n <= 3 AND r <= 2 AND s <= 4 AND e <= 1",
	                                 "MAXIMIZE SUM(w*x) + SUM(n) + SUM(r) + s + SUM(e)"));
	auto &facts = query.facts;
	REQUIRE(facts.decisions_status == DirectFactStatus::KNOWN);
	REQUIRE(facts.decisions.size() == 5);
	REQUIRE(facts.decisions[0].domain == DirectDomain::BOOL);
	REQUIRE(facts.decisions[0].output_type == LogicalType::INTEGER);
	REQUIRE(facts.decisions[1].domain == DirectDomain::INT);
	REQUIRE(facts.decisions[2].domain == DirectDomain::REAL);
	REQUIRE(facts.decisions[3].scope == DecideVarScope::SCALAR);
	REQUIRE(facts.decisions[4].scope == DecideVarScope::ENTITY);
	REQUIRE(facts.decisions[4].entity_scope == 0);
	REQUIRE(facts.entity_scopes.size() == 1);
	REQUIRE(facts.entity_scopes[0].status == DirectFactStatus::KNOWN);
	REQUIRE(!facts.entity_scopes[0].key_slots.empty());
	// The query-wide decision is a row-level part beside the reducers.
	REQUIRE(facts.objective.parts.size() == 5);
	REQUIRE(facts.objective.parts[3].reducer == DirectReducer::NONE);
}

TEST_CASE("Direct-solve facts model comparisons, membership, and right-hand sides", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryFacts query(con, FactsQuery("x(INT), y(INT)",
	                                 "x + y <= 3 AND x <> 1 AND y IN (0, 2) AND SUM(x) <= cap AND "
	                                 "SUM(y) <= (SELECT 5) AND SUM(x) BETWEEN 1 AND 2",
	                                 "MAXIMIZE SUM(w*x)"));
	auto &constraints = query.facts.constraints;
	REQUIRE(query.facts.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(constraints.size() == 7);
	for (auto &constraint : constraints) {
		REQUIRE(constraint.status == DirectFactStatus::KNOWN);
	}
	// One constraint per row, holding both decisions.
	REQUIRE(!constraints[0].aggregate);
	REQUIRE(constraints[0].lhs.size() == 1);
	REQUIRE(constraints[0].lhs[0].reducer == DirectReducer::NONE);
	REQUIRE(constraints[0].lhs[0].terms.size() == 2);
	REQUIRE(constraints[0].rhs_provenance == DirectProvenance::CONSTANT);
	REQUIRE(constraints[1].comparison == ExpressionType::COMPARE_NOTEQUAL);
	REQUIRE(constraints[2].comparison == ExpressionType::COMPARE_IN);
	REQUIRE(constraints[2].members.size() == 2);
	REQUIRE(constraints[3].aggregate);
	REQUIRE(constraints[3].rhs_provenance == DirectProvenance::ROW_VARYING);
	REQUIRE(constraints[4].rhs_provenance == DirectProvenance::QUERY_WIDE);
	// BETWEEN is two written comparisons, each with its own clause.
	REQUIRE(constraints[6].source_clause_id == constraints[5].source_clause_id + 1);
	REQUIRE(constraints[5].comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO);
	REQUIRE(constraints[6].comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO);
}

TEST_CASE("Direct-solve facts model reducers, filters, qualifiers, scales and norms", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryFacts query(con, FactsQuery("t.x(INT), y(INT)",
	                                 "AVG(y) <= 2 AND MIN(y) >= 0 AND MAX(y) <= 3 AND SUM(y) WHEN flag <= 2 AND "
	                                 "SUM(t: x) <= 4 AND 2 * SUM(y) <= 9 AND norm(y, 0) <= 2 AND "
	                                 "norm(y - cap, 1) <= 5",
	                                 "MINIMIZE SUM(w*y)"));
	auto &constraints = query.facts.constraints;
	REQUIRE(constraints.size() == 8);
	for (auto &constraint : constraints) {
		REQUIRE(constraint.status == DirectFactStatus::KNOWN);
		REQUIRE(constraint.lhs.size() == 1);
	}
	REQUIRE(constraints[0].lhs[0].reducer == DirectReducer::AVG);
	REQUIRE(constraints[1].lhs[0].reducer == DirectReducer::MIN);
	REQUIRE(constraints[2].lhs[0].reducer == DirectReducer::MAX);
	REQUIRE(constraints[3].lhs[0].filter != nullptr);
	REQUIRE(constraints[4].lhs[0].qualifier == 0);
	REQUIRE(constraints[5].lhs[0].scale != nullptr);
	REQUIRE(constraints[6].lhs[0].reducer == DirectReducer::COUNT_NONZERO);
	REQUIRE(constraints[6].lhs[0].l0_tolerance > 0);
	REQUIRE(constraints[6].lhs[0].l0_bound == 0);
	// L1 reads as its definition: SUM of an absolute value.
	REQUIRE(constraints[7].lhs[0].reducer == DirectReducer::SUM);
	REQUIRE(constraints[7].lhs[0].terms.size() == 1);
	REQUIRE(constraints[7].lhs[0].terms[0].kind == DecideTermKind::ABS);
}

TEST_CASE("Direct-solve facts model products, squares, and nested objective reducers", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryFacts products(con, FactsQuery("x(INT), y(INT)", "SUM(x*y) <= 4 AND SUM(POWER(x - cap, 2)) <= 9",
	                                    "MAXIMIZE SUM(w*x)"));
	auto &constraints = products.facts.constraints;
	REQUIRE(constraints.size() == 2);
	REQUIRE(constraints[0].lhs[0].terms[0].kind == DecideTermKind::PRODUCT);
	REQUIRE(constraints[0].degree == 2);
	REQUIRE(constraints[1].lhs[0].terms[0].kind == DecideTermKind::SQUARE);

	QueryFacts nested(con, FactsQuery("x(INT)", "x <= 3", "MINIMIZE SUM(MAX(w*x)) PER id"));
	auto &parts = nested.facts.objective.parts;
	REQUIRE(parts.size() == 1);
	REQUIRE(parts[0].reducer == DirectReducer::SUM);
	REQUIRE(parts[0].inner != nullptr);
	REQUIRE(parts[0].inner->reducer == DirectReducer::MAX);
	REQUIRE(parts[0].terms.empty());
}

TEST_CASE("Direct-solve facts report what they cannot model as unknown, with a reason", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// A decision under ABS inside a square is not linear algebra the splitter can read.
	QueryFacts query(con, FactsQuery("x(INT)", "x <= 3 AND SUM(POWER(ABS(x - cap), 2)) <= 9", "MAXIMIZE SUM(w*x)"));
	auto &facts = query.facts;
	REQUIRE(facts.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(facts.constraints.size() == 2);
	REQUIRE(facts.constraints[0].status == DirectFactStatus::KNOWN);
	REQUIRE(facts.constraints[1].status == DirectFactStatus::UNKNOWN);
	REQUIRE(!facts.constraints[1].reason.empty());
	REQUIRE(facts.FirstUnknownReason().find("clause `") != string::npos);
}

TEST_CASE("Direct-solve facts say which bounds the solver folds into a variable's box", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);

	// One decision, constraints that each produce one fact in order; `expected` is the solver's absorption of each.
	auto check = [&](const string &declaration, const string &constraints, duckdb::vector<bool> expected) {
		QueryFacts query(con, FactsQuery(declaration, constraints, "MINIMIZE SUM(x)"));
		auto &facts = query.facts;
		INFO(declaration << " | " << constraints);
		REQUIRE(facts.constraints.size() == expected.size());
		for (idx_t i = 0; i < expected.size(); i++) {
			REQUIRE(facts.constraints[i].variable_bound == expected[i]);
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

TEST_CASE("A1 facts read each objective spelling the solver accepts", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);

	// A square carries its scale and its linear inner terms: x - w is [LINEAR x, CONSTANT w with sign -1].
	QueryFacts square(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2))"));
	REQUIRE(square.facts.objective.status == DirectFactStatus::KNOWN);
	REQUIRE(square.facts.objective.parts.size() == 1);
	auto &terms = square.facts.objective.parts[0].terms;
	REQUIRE(terms.size() == 1);
	REQUIRE(terms[0].kind == DecideTermKind::SQUARE);
	REQUIRE(terms[0].square_scale == 1.0);
	REQUIRE(terms[0].inner.size() == 2);
	REQUIRE(terms[0].inner[0].kind == DecideTermKind::LINEAR);
	REQUIRE(terms[0].inner[1].kind == DecideTermKind::CONSTANT);
	REQUIRE(terms[0].inner[1].sign == -1);

	// A constant factor inside the SUM folds into the square's scale; one outside stays on the part.
	QueryFacts inside(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(3*POWER(x - w, 2))"));
	REQUIRE(inside.facts.objective.parts[0].terms[0].square_scale == 3.0);
	QueryFacts outside(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE 2*SUM(POWER(x - w, 2))"));
	REQUIRE(outside.facts.objective.parts[0].scale != nullptr);
	REQUIRE(!outside.facts.objective.parts[0].scale_divides);
	QueryFacts divided(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2))/2"));
	REQUIRE(divided.facts.objective.parts[0].scale_divides);
	QueryFacts negated(con, FactsQuery("x(REAL)", "x <= cap", "MAXIMIZE -SUM(POWER(x - w, 2))"));
	REQUIRE(negated.facts.objective.parts[0].sign == -1);

	// x*x - 2*w*x is a square plus a negative linear term; a constant stays beside the square.
	QueryFacts expanded(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(x*x - 2*w*x + 5)"));
	auto &expanded_terms = expanded.facts.objective.parts[0].terms;
	REQUIRE(expanded_terms.size() == 3);
	REQUIRE(expanded_terms[0].kind == DecideTermKind::SQUARE);
	REQUIRE(expanded_terms[1].kind == DecideTermKind::LINEAR);
	REQUIRE(expanded_terms[1].sign == -1);
	REQUIRE(expanded_terms[2].kind == DecideTermKind::CONSTANT);

	// A row-valued coefficient inside the square is allowed; one outside it, or x*x with a coefficient, is not
	// modelled.
	QueryFacts inner_weight(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(w*x - 3, 2))"));
	REQUIRE(inner_weight.facts.objective.status == DirectFactStatus::KNOWN);
	QueryFacts outer_weight(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(w*POWER(x - 1, 2))"));
	REQUIRE(outer_weight.facts.objective.status == DirectFactStatus::UNKNOWN);
	QueryFacts cubic(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(w*x*x)"));
	REQUIRE(cubic.facts.objective.status == DirectFactStatus::UNKNOWN);

	// An objective-level WHEN is a scope on the objective; no objective is no part at all.
	QueryFacts when(con, FactsQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2)) WHEN flag"));
	REQUIRE(when.facts.objective.scope.when != nullptr);
	QueryFacts none(con, FactsQuery("x(REAL)", "x <= cap", ""));
	REQUIRE(none.facts.objective.sense == DecideSense::FEASIBILITY);
	REQUIRE(none.facts.objective.parts.empty());

	// A per-row bound with a coefficient keeps the coefficient in the term; the constant moves to the right side.
	QueryFacts affine(con, FactsQuery("x(REAL)", "2*x + 1 <= cap", "MAXIMIZE SUM(x)"));
	REQUIRE(affine.facts.constraints[0].lhs[0].terms.size() == 1);
	REQUIRE(affine.facts.constraints[0].lhs[0].terms[0].kind == DecideTermKind::LINEAR);

	// The output type follows the declared type.
	QueryFacts types(con,
	                 FactsQuery("x(REAL), n(INT), b(BOOL)", "x <= cap AND n <= cap AND b <= 1", "MAXIMIZE SUM(x)"));
	REQUIRE(types.facts.decisions[0].output_type == LogicalType::DOUBLE);
	REQUIRE(types.facts.decisions[1].output_type == LogicalType::BIGINT);
	REQUIRE(types.facts.decisions[2].output_type == LogicalType::INTEGER);
}

TEST_CASE("A1 proves independent bounded decisions and names why it declines anything else", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto rules = RegisteredDirectRules();
	auto *a1 = FindRuleByName(rules, "A1_INDEPENDENT_BOUNDS");
	REQUIRE(a1 != nullptr);
	string reason, proof;

	// What it proves.
	REQUIRE(RuleProves(*a1, con, "x(REAL)", "x BETWEEN 0 AND cap", "MINIMIZE SUM(POWER(x - w, 2))", reason, &proof));
	REQUIRE(proof.find("1 independent row-scoped continuous decision(s)") != string::npos);
	REQUIRE(proof.find("quadratic objective") != string::npos);
	REQUIRE(RuleProves(*a1, con, "x(INT)", "x <= cap AND x >= 0", "MAXIMIZE SUM(w*x)", reason, &proof));
	REQUIRE(proof.find("whole-number") != string::npos);
	REQUIRE(proof.find("linear objective") != string::npos);
	REQUIRE(RuleProves(*a1, con, "x(BOOL)", "x <= flag", "MAXIMIZE SUM(w*x)", reason));
	REQUIRE(
	    RuleProves(*a1, con, "x(REAL), y(INT)", "x <= cap AND y <= 5", "MINIMIZE SUM(x) + SUM(w*y)", reason, &proof));
	REQUIRE(proof.find("2 independent") != string::npos);
	REQUIRE(proof.find("mixed") != string::npos);
	REQUIRE(RuleProves(*a1, con, "x(REAL)", "x <= cap", "", reason, &proof));
	REQUIRE(proof.find("no objective") != string::npos);
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
	declines("x(REAL)", "x <= cap", "MAXIMIZE SUM(w*x) PER id", "objective_facts_unknown");
	declines("x(REAL)", "x <= cap", "MAXIMIZE AVG(w*x)", "objective_shape");
	declines("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2)) + SUM(POWER(x - cap, 2))",
	         "at most one squared term");
	declines("x(REAL)", "x <= cap", "MINIMIZE SUM(w*POWER(x - 1, 2))", "objective_facts_unknown");
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
	auto *a1 = FindRuleByName(rules, "A1_INDEPENDENT_BOUNDS");
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
