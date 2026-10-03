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
	result["Direct solve rule"] = rule.empty() ? "none" : rule;
	result["Direct solve decision"] = hit ? "hit" : "miss";
	if (!reason.empty()) {
		result["Direct solve reason"] = reason;
	}
	if (!proof.empty()) {
		result["Direct solve proof"] = proof;
	}
	if (!guards.empty()) {
		result["Direct solve guards"] = guards;
	}
	result["Solver skipped"] = skipped_solver ? "true" : "false";
	return result;
}

DirectSolveMode GetDirectSolveMode(ClientContext &context) {
	Value value;
	if (!context.TryGetCurrentSetting("decide_direct_solve", value) || value.IsNull()) {
		return DirectSolveMode::OFF;
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

string RequireMissText(const DirectSolveDecisionRecord &record) {
	return StringUtil::Format("%s (rule=%s; solver skipped=true)", record.reason,
	                          record.rule.empty() ? "none" : record.rule);
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
	config.AddExtensionOption("decide_direct_solve", "DECIDE direct solve mode: off, auto, or require",
	                          LogicalType::VARCHAR, Value("off"), DirectSolveModeSetCallback);
	config.operator_extensions.push_back(MakeDirectResultExtension());
}

unique_ptr<LogicalOperator> TryDirectSolve(unique_ptr<LogicalOperator> op, Optimizer &optimizer, DirectSolveMode mode) {
	if (mode == DirectSolveMode::OFF) {
		return op;
	}
	DecideProfileScope direct_profile("optimizer.direct.analyze");
	auto &decide = op->Cast<LogicalDecide>();
	DirectSolveDecisionRecord record;
	record.attempted = true;
	record.mode = ModeName(mode);
	auto forced_solver = std::getenv("DECIDB_FORCE_SOLVER");
	if (decide.diagnose || (forced_solver && *forced_solver)) {
		record.reason = decide.diagnose ? "policy_diagnose: DIAGNOSE requires the solver"
		                               : "policy_forced_solver: DECIDB_FORCE_SOLVER is set";
		if (mode == DirectSolveMode::REQUIRE) {
			record.skipped_solver = true;
			throw InvalidInputException("decide_direct_solve=require conflicts with %s: %s",
			                            decide.diagnose ? "DIAGNOSE" : "DECIDB_FORCE_SOLVER", RequireMissText(record));
		}
		decide.direct_solve_record = std::move(record);
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
		record.reason = StringUtil::Join(misses, "; ");
		if (mode == DirectSolveMode::REQUIRE) {
			record.skipped_solver = true;
			throw InvalidInputException("decide_direct_solve=require: %s", RequireMissText(record));
		}
		decide.direct_solve_record = std::move(record);
		return op;
	}
	auto selected = std::min_element(proved.begin(), proved.end(),
	                                 [](const ProvedCandidate &left, const ProvedCandidate &right) {
		                                 return left.cost < right.cost;
	                                 });
	record.rule = selected->rule->Name();
	selected->rule->Explain(*selected->proof, record);
	record.hit = true;
	record.skipped_solver = true;
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
