#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

namespace duckdb {

vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules() {
	vector<unique_ptr<DirectSolveRule>> rules;
	rules.push_back(MakeS1UniformMatroidRule());
	rules.push_back(MakeS2LaminarMatroidRule());
	rules.push_back(MakeA1SeparableBoxRule());
	rules.push_back(MakeR1ContinuousKnapsackRule());
	rules.push_back(MakeN1EuclideanBallRule());
	rules.push_back(MakeA2SingleFacilityLocationRule());
	rules.push_back(MakeO1MongeAssignmentRule());
	rules.push_back(MakeS3MaxCardinalityKnapsackRule());
	rules.push_back(MakeR4MaxMinFairnessRule());
	return rules;
}

} // namespace duckdb
