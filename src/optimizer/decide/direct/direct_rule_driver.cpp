#include "duckdb/optimizer/decide/direct/direct_solve.hpp"

#include <cstdlib>

#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/optimizer/decide/direct/direct_result_boundary.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_second_read.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

InsertionOrderPreservingMap<string> DirectSolveDecisionRecord::Render() const {
	InsertionOrderPreservingMap<string> result;
	result["Direct solve mode"] = mode;
	result["Direct solve rule"] = rule;
	result["Direct solve condition"] = condition;
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

//! The `require` error for a query no rule proved: why each rule does not apply, and that no solver ran.
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

//! The rules this connection tries: the registered ones, unless a rule driver test installed its own.
vector<unique_ptr<DirectSolveRule>> RulesFor(ClientContext &context) {
	auto override_rules = context.registered_state->Get<DirectRuleTestSeam>(DIRECT_RULE_TEST_SEAM_KEY);
	return override_rules ? override_rules->make_rules() : RegisteredDirectRules();
}

} // namespace

void RegisterDirectSolve(DBConfig &config) {
	config.AddExtensionOption("decide_direct_solve",
	                          "DECIDE direct solve mode: auto (default) uses a proved relational plan and otherwise the "
	                          "solver; off always uses the solver; require errors when no relational plan is proved",
	                          LogicalType::VARCHAR, Value("auto"), DirectSolveModeSetCallback);
	config.operator_extensions.push_back(MakeDirectResultExtension());
}

unique_ptr<LogicalOperator> ApplyDirectSolveRules(unique_ptr<LogicalOperator> op, Optimizer &optimizer,
                                                  DirectSolveMode mode) {
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
	auto ir = DirectIR::Read(optimizer.context, decide);
	struct ProvedCandidate {
		const DirectSolveRule *rule;
		unique_ptr<DirectApplicabilityCondition> condition;
	};
	vector<ProvedCandidate> proved;
	vector<string> misses;
	auto rules = RulesFor(optimizer.context);
	for (auto &rule : rules) {
		string reason;
		auto condition = rule->CheckCondition(ir, optimizer.context, reason);
		if (!condition) {
			misses.push_back(rule->Name() + string(": ") + reason);
			continue;
		}
		proved.push_back({rule.get(), std::move(condition)});
	}
	if (proved.empty()) {
		if (mode == DirectSolveMode::REQUIRE) {
			throw InvalidInputException("decide_direct_solve=require: %s",
			                            RequireMissText(StringUtil::Join(misses, "; ")));
		}
		// A query no rule applies to under auto leaves the DECIDE node, and its EXPLAIN output, exactly as the solver
		// path has it. `require` is how a user asks why a query was not proved.
		return op;
	}
	// Every rule ran, so a second rule that applies is seen. Rules tell queries apart by the IR they require; two rules
	// that apply to one query mean the rules overlap, which no ordering or cost can settle safely.
	if (proved.size() > 1) {
		vector<string> names;
		for (auto &candidate : proved) {
			names.push_back(candidate.rule->Name());
		}
		throw InternalException("Direct solve rules %s %s proved one query; rules must be disjoint",
		                        StringUtil::Join(names, ", "), names.size() == 2 ? "both" : "all");
	}
	auto &selected = proved[0];
	DirectSolveDecisionRecord record;
	record.mode = ModeName(mode);
	record.rule = selected.rule->Name();
	record.condition = selected.rule->Summary();
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
	// A stored source carries its row id as one more source column, so a column the rule never uses can be read again
	// later, past the plan that holds rows. The rule passes it along like any other source column.
	auto source_columns = prunable_sources.size();
	DirectSecondRead second_read;
	bool carries_rowid = DirectExposeStoredRowId(source, second_read);
	if (carries_rowid) {
		source.ResolveOperatorTypes();
	}
	auto substitute = selected.rule->Apply(std::move(decide.children[0]), optimizer, *selected.condition);
	if (carries_rowid) {
		if (substitute.output_slots.size() != output_bindings.size() + 1) {
			throw InternalException("Direct solve rule did not pass the source row id through");
		}
		second_read.rowid_slot = substitute.output_slots[source_columns];
		substitute.output_slots.erase(substitute.output_slots.begin() + NumericCast<int64_t>(source_columns));
		if (substitute.pipelined) {
			// Nothing is held, so there is nothing to gain from a second read.
			second_read = DirectSecondRead();
		}
	}
	return MapDirectResult(std::move(substitute), std::move(output_bindings), std::move(output_types),
	                       std::move(prunable_sources), decide_index, std::move(record), second_read);
}

} // namespace duckdb
