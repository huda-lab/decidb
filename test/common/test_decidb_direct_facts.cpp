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

	auto plan = con.ExtractPlan("SELECT id, x FROM ("
	                            " FROM (VALUES (1, 9.0::DOUBLE), (2, -1.0::DOUBLE)) s(id,score)"
	                            " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)"
	                            ") q");
	auto *decide = FindDirectFactsInput(*plan);
	REQUIRE(decide != nullptr);
	auto facts = DirectProblemFacts::Read(*decide);
	REQUIRE(facts.decisions_status == DirectFactStatus::KNOWN);
	REQUIRE(facts.decisions.size() == 1);
	REQUIRE(facts.decisions[0].boolean_domain);
	REQUIRE(facts.decisions[0].scope == DecideVarScope::ROW);
	REQUIRE(facts.objective_status == DirectFactStatus::KNOWN);
	REQUIRE(facts.objective_terms.size() == 1);
	REQUIRE(facts.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(facts.source_clause_count == 1);
	REQUIRE(facts.constraint_factors.size() == 1);
	REQUIRE(facts.constraint_factors[0].source_status == DirectFactStatus::KNOWN);
	REQUIRE(facts.constraint_factors[0].source_clause_id == 0);
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
	auto damaged = DirectProblemFacts::Read(*decide);
	REQUIRE(damaged.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(damaged.constraint_factors.size() == 1);
	REQUIRE(damaged.constraint_factors[0].source_status == DirectFactStatus::UNKNOWN);
	reason.clear();
	REQUIRE(s1->Match(damaged, reason) == nullptr);
	REQUIRE(reason.find("constraint_provenance") != string::npos);
}

TEST_CASE("S1 facts preserve both interval bounds and exact equality", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
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
		auto facts = DirectProblemFacts::Read(*decide);
		REQUIRE(facts.constraints_status == DirectFactStatus::KNOWN);
		REQUIRE(facts.constraint_factors.size() == facts.source_clause_count);
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
		auto facts = DirectProblemFacts::Read(*decide);
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
		auto facts = DirectProblemFacts::Read(*decide);
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

	auto constraint_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                       " FROM (VALUES (1, 9.0::DOUBLE, TRUE)) s(id,score,active)"
	                                       " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 WHEN active"
	                                       " MAXIMIZE SUM(score*x)"
	                                       ") q");
	auto *constraint_decide = FindDirectFactsInput(*constraint_plan);
	REQUIRE(constraint_decide != nullptr);
	auto constraint_facts = DirectProblemFacts::Read(*constraint_decide);
	REQUIRE(constraint_facts.constraints_status == DirectFactStatus::KNOWN);
	REQUIRE(constraint_facts.constraint_factors.size() == 1);
	REQUIRE(constraint_facts.constraint_factors[0].when_condition != nullptr);
	REQUIRE(constraint_facts.constraint_factors[0].per_keys.empty());

	auto per_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                " FROM (VALUES (1, 9.0::DOUBLE, 'A'::VARCHAR),"
	                                " (2, -1.0::DOUBLE, 'B'::VARCHAR)) s(id,score,dept)"
	                                " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 PER dept"
	                                " MAXIMIZE SUM(score*x)"
	                                ") q");
	auto *per_decide = FindDirectFactsInput(*per_plan);
	REQUIRE(per_decide != nullptr);
	auto per_facts = DirectProblemFacts::Read(*per_decide);
	// ExtractPlan precedes the optimizer's reference resolution. Its PER key is
	// still a positional BoundReferenceExpression; do not invent a binding.
	// End-to-end direct tests exercise the resolved key at the DECIDE boundary.
	REQUIRE(per_facts.constraints_status == DirectFactStatus::UNKNOWN);
	REQUIRE(per_facts.constraint_factors.empty());

	auto objective_plan = con.ExtractPlan("SELECT id, x FROM ("
	                                      " FROM (VALUES (1, 9.0::DOUBLE, TRUE)) s(id,score,active)"
	                                      " DECIDE x(BOOL) SUCH THAT SUM(x)<=1"
	                                      " MAXIMIZE SUM(score*x) WHEN active"
	                                      ") q");
	auto *objective_decide = FindDirectFactsInput(*objective_plan);
	REQUIRE(objective_decide != nullptr);
	auto objective_facts = DirectProblemFacts::Read(*objective_decide);
	REQUIRE(objective_facts.objective_status == DirectFactStatus::UNKNOWN);
	REQUIRE(objective_facts.objective_terms.empty());
}
