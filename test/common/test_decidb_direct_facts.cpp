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

DirectSolveRule *FindS1Rule(duckdb::vector<duckdb::unique_ptr<DirectSolveRule>> &rules) {
	for (auto &rule : rules) {
		if (string(rule->Name()) == "S1_CARDINALITY_INTERVAL") {
			return rule.get();
		}
	}
	return nullptr;
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
