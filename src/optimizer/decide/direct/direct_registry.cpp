#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

namespace duckdb {

vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules() {
	vector<unique_ptr<DirectSolveRule>> rules;
	rules.push_back(MakeS1UniformMatroidRule());
	rules.push_back(MakeA1SeparableBoxRule());
	rules.push_back(MakeR1ContinuousKnapsackRule());
	rules.push_back(MakeN1EuclideanBallRule());
	return rules;
}

} // namespace duckdb
