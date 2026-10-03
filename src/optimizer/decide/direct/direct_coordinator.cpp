#include "duckdb/optimizer/decide/direct/direct_solve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/decide/direct/direct_result_boundary.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/s1_rule.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

InsertionOrderPreservingMap<string> DirectSolveDecisionRecord::Render() const {
	InsertionOrderPreservingMap<string> result;
	result["Direct solve mode"] = mode;
	result["Direct solve rule"] = rule;
	result["Direct solve proof"] = proof;
	result["Direct solve guards"] = guards;
	return result;
}

DirectSolveMode GetDirectSolveMode(ClientContext &context) {
	Value value;
	if (!context.TryGetCurrentSetting("decide_direct_solve", value) || value.IsNull()) {
		return DirectSolveMode::AUTO;
	}
	auto mode = StringUtil::Lower(value.GetValue<string>());
	if (mode == "auto") {
		return DirectSolveMode::AUTO;
	}
	if (mode == "require") {
		return DirectSolveMode::REQUIRE;
	}
	return DirectSolveMode::OFF;
}

namespace {

//! The `require` error for a query no rule proved: why each rule missed, and that no solver ran.
string RequireMissText(const string &reason) {
	return StringUtil::Format("%s (rule=none; solver skipped=true)", reason);
}

const char *ModeName(DirectSolveMode mode) {
	return mode == DirectSolveMode::AUTO ? "auto" : mode == DirectSolveMode::REQUIRE ? "require" : "off";
}

void DirectSolveModeSetCallback(ClientContext &, SetScope, Value &parameter) {
	auto value = StringUtil::Lower(parameter.GetValue<string>());
	if (value != "off" && value != "auto" && value != "require") {
		throw InvalidInputException("decide_direct_solve must be off, auto, or require");
	}
	parameter = Value(value);
}

} // namespace

vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules() {
	vector<unique_ptr<DirectSolveRule>> rules;
	rules.push_back(MakeS1CardinalityRule());
	return rules;
}

void RegisterDirectSolve(DBConfig &config) {
	config.AddExtensionOption("decide_direct_solve",
	                          "DECIDE direct solve mode: auto (default) uses a proved relational plan and otherwise the "
	                          "solver; off always uses the solver; require errors when no relational plan is proved",
	                          LogicalType::VARCHAR, Value("auto"), DirectSolveModeSetCallback);
	config.operator_extensions.push_back(MakeDirectResultExtension());
}

unique_ptr<LogicalOperator> TryDirectSolve(unique_ptr<LogicalOperator> op, Optimizer &optimizer, DirectSolveMode mode) {
	if (mode == DirectSolveMode::OFF) {
		return op;
	}
	DecideProfileScope direct_profile("optimizer.direct.analyze");
	auto &decide = op->Cast<LogicalDecide>();
	auto forced_solver = std::getenv("DECIDB_FORCE_SOLVER");
	if (decide.diagnose || (forced_solver && *forced_solver)) {
		if (mode == DirectSolveMode::REQUIRE) {
			auto reason = decide.diagnose ? "policy_diagnose: DIAGNOSE requires the solver"
			                              : "policy_forced_solver: DECIDB_FORCE_SOLVER is set";
			throw InvalidInputException("decide_direct_solve=require conflicts with %s: %s",
			                            decide.diagnose ? "DIAGNOSE" : "DECIDB_FORCE_SOLVER", RequireMissText(reason));
		}
		// A DIAGNOSE query or a forced backend keeps the solver path; there is nothing to report.
		return op;
	}
	auto facts = DirectProblemFacts::Read(decide);
	struct ProvedCandidate {
		const DirectSolveRule *rule;
		unique_ptr<DirectRuleProof> proof;
		double cost;
	};
	vector<ProvedCandidate> proved;
	vector<string> misses;
	auto rules = RegisteredDirectRules();
	for (auto &rule : rules) {
		string reason;
		auto match = rule->Match(facts, reason);
		if (!match) {
			misses.push_back(rule->Name() + string(": ") + reason);
			continue;
		}
		auto proof = rule->Prove(facts, *match, optimizer.context, reason);
		if (!proof) {
			misses.push_back(rule->Name() + string(": ") + reason);
			continue;
		}
		auto cost = rule->Cost(*proof);
		if (!std::isfinite(cost)) {
			throw InternalException("Direct solve rule %s returned a non-finite cost", rule->Name());
		}
		proved.push_back({rule.get(), std::move(proof), cost});
	}
	if (proved.empty()) {
		if (mode == DirectSolveMode::REQUIRE) {
			throw InvalidInputException("decide_direct_solve=require: %s",
			                            RequireMissText(StringUtil::Join(misses, "; ")));
		}
		// A miss under auto leaves the DECIDE node, and its EXPLAIN output, exactly as the solver path has it.
		// `require` is how a user asks why a query was not proved.
		return op;
	}
	auto selected = std::min_element(proved.begin(), proved.end(),
	                                 [](const ProvedCandidate &left, const ProvedCandidate &right) {
		                                 return left.cost < right.cost;
	                                 });
	DirectSolveDecisionRecord record;
	record.mode = ModeName(mode);
	record.rule = selected->rule->Name();
	selected->rule->Explain(*selected->proof, record);
	direct_profile.Next("optimizer.direct.construct");
	if (decide.children.size() != 1) {
		throw InternalException("Direct solve expected one DECIDE input");
	}
	decide.children[0]->ResolveOperatorTypes();
	auto output_bindings = decide.GetColumnBindings();
	if (output_bindings != selected->proof->output_bindings) {
		throw InternalException("Direct solve proof does not preserve DECIDE output bindings");
	}
	auto output_types = decide.children[0]->types;
	for (auto &variable : decide.decide_variables) {
		output_types.push_back(variable->return_type);
	}
	auto decide_index = decide.decide_index;
	auto proposal = selected->rule->Rewrite(std::move(decide.children[0]), optimizer, *selected->proof);
	return MapDirectResult(std::move(proposal), std::move(output_bindings), std::move(output_types), decide_index,
	                       std::move(record));
}

} // namespace duckdb
