#include "duckdb/optimizer/decide/direct/a1_separable_box_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/s1_uniform_matroid_rule.hpp"

namespace duckdb {

vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules() {
	vector<unique_ptr<DirectSolveRule>> rules;
	rules.push_back(MakeS1UniformMatroidRule());
	rules.push_back(MakeA1SeparableBoxRule());
	return rules;
}

} // namespace duckdb
