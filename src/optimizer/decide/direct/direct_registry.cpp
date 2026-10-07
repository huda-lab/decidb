#include "duckdb/optimizer/decide/direct/a1_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/s1_rule.hpp"

namespace duckdb {

vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules() {
	vector<unique_ptr<DirectSolveRule>> rules;
	rules.push_back(MakeS1CardinalityRule());
	rules.push_back(MakeA1BoundedRule());
	return rules;
}

} // namespace duckdb
