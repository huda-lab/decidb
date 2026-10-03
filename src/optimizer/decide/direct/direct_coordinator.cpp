#include "duckdb/optimizer/decide/direct/direct_solve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/optimizer/decide/direct/direct_result_boundary.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
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

//! The rules this connection tries: the registered ones, unless a coordinator test installed its own.
vector<unique_ptr<DirectSolveRule>> RulesFor(ClientContext &context) {
	auto override_rules = context.registered_state->Get<DirectRuleOverride>(DIRECT_RULE_OVERRIDE_KEY);
	return override_rules ? override_rules->make_rules() : RegisteredDirectRules();
}

struct CachedEstimate {
	LogicalOperator *op;
	bool has_estimate;
	idx_t estimate;
};

void SaveEstimates(LogicalOperator &op, vector<CachedEstimate> &saved) {
	saved.push_back({&op, op.has_estimated_cardinality, op.estimated_cardinality});
	for (auto &child : op.children) {
		SaveEstimates(*child, saved);
	}
}

//! DuckDB's row estimate for the DECIDE input. Estimating caches a value on every operator it visits; this runs
//! before join ordering, so a cached value would outlive the plan it described and reach EXPLAIN and physical
//! planning. The cache is put back as it was.
DirectCostContext MakeCostContext(ClientContext &context, LogicalOperator &source) {
	vector<CachedEstimate> saved;
	SaveEstimates(source, saved);
	DirectCostContext result;
	result.estimated_source_rows = source.EstimateCardinality(context);
	for (auto &entry : saved) {
		entry.op->has_estimated_cardinality = entry.has_estimate;
		entry.op->estimated_cardinality = entry.estimate;
	}
	return result;
}

} // namespace

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
	if (decide.children.size() != 1 || !decide.children[0]) {
		throw InternalException("Direct solve expected one DECIDE input");
	}
	auto &source = *decide.children[0];
	auto facts = DirectProblemFacts::Read(decide);
	struct ProvedCandidate {
		const DirectSolveRule *rule;
		unique_ptr<DirectRuleProof> proof;
		double cost;
	};
	vector<ProvedCandidate> proved;
	vector<string> misses;
	unique_ptr<DirectCostContext> cost_context;
	auto rules = RulesFor(optimizer.context);
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
		if (!cost_context) {
			cost_context = make_uniq<DirectCostContext>(MakeCostContext(optimizer.context, source));
		}
		auto cost = rule->Cost(*proof, *cost_context);
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
	// min_element keeps the first of equal costs, which is the earlier registered rule.
	auto selected = std::min_element(proved.begin(), proved.end(),
	                                 [](const ProvedCandidate &left, const ProvedCandidate &right) {
		                                 return left.cost < right.cost;
	                                 });
	DirectSolveDecisionRecord record;
	record.mode = ModeName(mode);
	record.rule = selected->rule->Name();
	selected->rule->Explain(*selected->proof, record);
	direct_profile.Next("optimizer.direct.construct");
	// The replacement answers to the DECIDE node's identity: every source column, then one column per decision.
	source.ResolveOperatorTypes();
	auto output_bindings = source.GetColumnBindings();
	auto output_types = source.types;
	if (output_bindings.size() != output_types.size()) {
		throw InternalException("Direct solve source bindings and types differ");
	}
	vector<uint8_t> prunable_sources;
	for (auto &binding : output_bindings) {
		prunable_sources.push_back(DirectCanSkipSourceOutput(source, binding) ? 1 : 0);
	}
	for (idx_t i = 0; i < decide.decide_variables.size(); i++) {
		output_bindings.emplace_back(decide.decide_index, i);
		output_types.push_back(decide.decide_variables[i]->return_type);
	}
	if (output_bindings != decide.GetColumnBindings()) {
		throw InternalException("Direct solve derived bindings differ from the DECIDE output");
	}
	auto decide_index = decide.decide_index;
	auto proposal = selected->rule->Rewrite(std::move(decide.children[0]), optimizer, *selected->proof);
	return MapDirectResult(std::move(proposal), std::move(output_bindings), std::move(output_types),
	                       std::move(prunable_sources), decide_index, std::move(record));
}

} // namespace duckdb
