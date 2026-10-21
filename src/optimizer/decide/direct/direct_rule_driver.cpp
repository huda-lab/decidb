#include "duckdb/optimizer/decide/direct/direct_solve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/decide/direct/direct_result_adapter.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_late_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

InsertionOrderPreservingMap<string> DirectExplainAnnotation::Render() const {
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
	auto seam_rules = context.registered_state->Get<DirectRuleTestSeam>(DIRECT_RULE_TEST_SEAM_KEY);
	return seam_rules ? seam_rules->make_rules() : RegisteredDirectRules();
}

//! The solver consumes every source output. Skipping an unreferenced output must not suppress a computed error
//! or volatile expression. This proves only stored columns, constants, and their passthrough aliases safe,
//! including passthrough bindings from one child of an inner comparison join.
bool DirectCanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding) {
	switch (source.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = source.Cast<LogicalGet>();
		if (get.function.function != TableScanFunction::GetFunction().function || !get.GetTable()) {
			return false;
		}
		auto bindings = get.GetColumnBindings();
		return std::find(bindings.begin(), bindings.end(), binding) != bindings.end();
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = source.Cast<LogicalProjection>();
		if (binding.table_index != projection.table_index || binding.column_index >= projection.expressions.size()) {
			return false;
		}
		auto &expression = *projection.expressions[binding.column_index];
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			return true;
		}
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF || source.children.size() != 1) {
			return false;
		}
		auto &child_ref = expression.Cast<BoundColumnRefExpression>();
		return child_ref.depth == 0 && DirectCanSkipSourceOutput(*source.children[0], child_ref.binding);
	}
	case LogicalOperatorType::LOGICAL_FILTER:
		return source.children.size() == 1 && DirectCanSkipSourceOutput(*source.children[0], binding);
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = source.Cast<LogicalComparisonJoin>();
		if (join.join_type != JoinType::INNER || source.children.size() != 2) {
			return false;
		}
		auto output_bindings = source.GetColumnBindings();
		if (std::find(output_bindings.begin(), output_bindings.end(), binding) == output_bindings.end()) {
			return false;
		}
		auto left_bindings = source.children[0]->GetColumnBindings();
		auto right_bindings = source.children[1]->GetColumnBindings();
		bool on_left = std::find(left_bindings.begin(), left_bindings.end(), binding) != left_bindings.end();
		bool on_right = std::find(right_bindings.begin(), right_bindings.end(), binding) != right_bindings.end();
		if (on_left == on_right) {
			return false;
		}
		return DirectCanSkipSourceOutput(*source.children[on_right ? 1 : 0], binding);
	}
	default:
		return false;
	}
}

} // namespace

bool DirectIRIsReadable(const DirectIR &ir) {
	if (ir.decisions_status != DirectAnalysisStatus::PROVED || ir.source_status != DirectAnalysisStatus::PROVED ||
	    ir.constraints_status != DirectAnalysisStatus::PROVED || ir.objective.status != DirectAnalysisStatus::PROVED ||
	    !std::isfinite(ir.objective.offset)) {
		return false;
	}
	// No objective (`DecideSense::FEASIBILITY`) is a sense the rules know.
	if (ir.objective.sense != DecideSense::MAXIMIZE && ir.objective.sense != DecideSense::MINIMIZE &&
	    ir.objective.sense != DecideSense::FEASIBILITY) {
		return false;
	}
	for (auto &constraint : ir.constraints) {
		if (constraint.status != DirectAnalysisStatus::PROVED) {
			return false;
		}
	}
	return true;
}

void RegisterDirectSolve(DBConfig &config) {
	config.AddExtensionOption(
	    "decide_direct_solve",
	    "DECIDE direct solve mode: auto (default) uses a relational plan from a rule that applies and "
	    "otherwise the solver; off always uses the solver; require errors when no rule applies",
	    LogicalType::VARCHAR, Value("auto"), DirectSolveModeSetCallback);
	config.operator_extensions.push_back(MakeDirectResultAdapterExtension());
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
			throw InvalidInputException("decide_direct_solve=require conflicts with %s: the solver is required",
			                            decide.diagnose ? "DIAGNOSE" : "DECIDB_FORCE_SOLVER");
		}
		// A DIAGNOSE query or a forced backend keeps the solver path; there is nothing to report.
		return op;
	}
	if (decide.children.size() != 1 || !decide.children[0]) {
		throw InternalException("Direct solve expected one DECIDE input");
	}
	auto &source = *decide.children[0];
	auto ir = DirectIR::Read(optimizer.context, decide);
	struct AppliedCandidate {
		const DirectSolveRule *rule;
		unique_ptr<DirectApplicabilityCondition> condition;
	};
	vector<AppliedCandidate> applied;
	// An IR the front end could not read is offered to no rule, which leaves the node as a query no rule applies to.
	auto rules = DirectIRIsReadable(ir) ? RulesFor(optimizer.context) : vector<unique_ptr<DirectSolveRule>>();
	for (auto &rule : rules) {
		auto condition = rule->CheckCondition(ir, optimizer.context);
		if (!condition) {
			continue;
		}
		applied.push_back({rule.get(), std::move(condition)});
	}
	if (applied.empty()) {
		if (mode == DirectSolveMode::REQUIRE) {
			throw InvalidInputException("decide_direct_solve=require: direct solve did not apply; the solver is required");
		}
		// A query no rule applies to under auto leaves the DECIDE node, and its EXPLAIN output, exactly as the solver
		// path has it.
		return op;
	}
	// Every rule ran, so a second rule that applies is seen. Rules tell queries apart by the IR they require; two rules
	// that apply to one query mean the rules overlap, which no ordering or cost can settle safely.
	if (applied.size() > 1) {
		vector<string> names;
		for (auto &candidate : applied) {
			names.push_back(candidate.rule->Name());
		}
		throw InternalException("Direct solve rules %s %s applied to one query; rules must be disjoint",
		                        StringUtil::Join(names, ", "), names.size() == 2 ? "both" : "all");
	}
	auto &selected = applied[0];
	DirectExplainAnnotation annotation;
	annotation.mode = ModeName(mode);
	annotation.rule = selected.rule->Name();
	annotation.condition = selected.rule->Summary();
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
	DirectLateMaterialization late_materialization;
	bool carries_rowid = DirectExposeStoredRowId(source, late_materialization);
	if (carries_rowid) {
		source.ResolveOperatorTypes();
	}
	auto substitute = selected.rule->Apply(std::move(decide.children[0]), optimizer, *selected.condition);
	if (carries_rowid) {
		if (substitute.slot_map.size() != output_bindings.size() + 1) {
			throw InternalException("Direct solve rule did not pass the source row id through");
		}
		late_materialization.rowid_slot = substitute.slot_map[source_columns];
		substitute.slot_map.erase(substitute.slot_map.begin() + NumericCast<int64_t>(source_columns));
		if (substitute.pipelined) {
			// Nothing is held, so there is nothing to gain from a late materialization.
			late_materialization = DirectLateMaterialization();
		}
	}
	return AdaptDirectResult(std::move(substitute), std::move(output_bindings), std::move(output_types),
	                         std::move(prunable_sources), decide_index, std::move(annotation), late_materialization);
}

} // namespace duckdb
