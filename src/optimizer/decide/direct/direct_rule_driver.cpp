#include "duckdb/optimizer/decide/direct/direct_solve.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/decide/decide_optimizer.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_result_adapter.hpp"
#include "duckdb/optimizer/decide/direct/direct_row_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_late_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

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
	// The option is registered with a default, and its callback rejects any value but the three modes.
	bool found = context.TryGetCurrentSetting("decide_direct_solve", value);
	D_ASSERT(found && !value.IsNull());
	(void)found;
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
	// An `off` query leaves the rule driver before anything is annotated.
	D_ASSERT(mode != DirectSolveMode::OFF);
	return mode == DirectSolveMode::AUTO ? "auto" : "require";
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

bool ContainsBinding(const vector<ColumnBinding> &bindings, const ColumnBinding &binding) {
	return std::find(bindings.begin(), bindings.end(), binding) != bindings.end();
}

//! The solver consumes every source output. Skipping an unreferenced output must not suppress a computed error
//! or volatile expression. This proves only stored columns, constants, and their passthrough aliases safe,
//! including passthrough bindings from one child of an inner comparison join. The binding is always an output of
//! `source`: the caller took it from the source's own bindings or from a reference in the operator above.
bool DirectCanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding) {
	switch (source.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = source.Cast<LogicalGet>();
		if (get.function.function != TableScanFunction::GetFunction().function) {
			return false;
		}
		// The table scan function reads a stored table only (the late materialization asserts the same).
		D_ASSERT(get.GetTable());
		return ContainsBinding(get.GetColumnBindings(), binding);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = source.Cast<LogicalProjection>();
		D_ASSERT(binding.table_index == projection.table_index && binding.column_index < projection.expressions.size());
		auto &expression = *projection.expressions[binding.column_index];
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			return true;
		}
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &child_ref = expression.Cast<BoundColumnRefExpression>();
		// A projection has one child, and a correlated reference is gone before the optimizer runs.
		D_ASSERT(source.children.size() == 1 && child_ref.depth == 0);
		return DirectCanSkipSourceOutput(*source.children[0], child_ref.binding);
	}
	case LogicalOperatorType::LOGICAL_FILTER:
		D_ASSERT(source.children.size() == 1);
		return DirectCanSkipSourceOutput(*source.children[0], binding);
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = source.Cast<LogicalComparisonJoin>();
		if (join.join_type != JoinType::INNER) {
			return false;
		}
		// An inner join outputs its two children's columns, so the binding comes from exactly one of them.
		D_ASSERT(source.children.size() == 2 && ContainsBinding(source.GetColumnBindings(), binding));
		bool on_right = ContainsBinding(source.children[1]->GetColumnBindings(), binding);
		D_ASSERT(ContainsBinding(source.children[0]->GetColumnBindings(), binding) != on_right);
		return DirectCanSkipSourceOutput(*source.children[on_right ? 1 : 0], binding);
	}
	default:
		return false;
	}
}

//! Strips the pipeline's tags from every expression of the operators above `source`. The row checks and the rule copy
//! tagged expressions into those operators; nothing reads the tags there, and a plan prints an expression by its
//! alias, so a tag left there would reach EXPLAIN. The source keeps its own aliases.
void StripTagsAbove(LogicalOperator &op, const LogicalOperator &source) {
	if (&op == &source) {
		return;
	}
	LogicalOperatorVisitor::EnumerateExpressions(op, [](unique_ptr<Expression> *expr) { DirectStripTags(**expr); });
	for (auto &child : op.children) {
		StripTagsAbove(*child, source);
	}
}

} // namespace

bool DirectIRIsReadable(const DirectIR &ir) {
	if (ir.status != DirectAnalysisStatus::PROVED) {
		return false;
	}
	if (ir.objective.status != DirectAnalysisStatus::PROVED || !std::isfinite(ir.objective.offset)) {
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

bool ApplyDirectSolveRules(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root, Optimizer &optimizer,
                           DirectSolveMode mode) {
	D_ASSERT(mode != DirectSolveMode::OFF);
	DecideProfileScope direct_profile("optimizer.direct.analyze");
	auto &decide = op->Cast<LogicalDecide>();
	D_ASSERT(decide.children.size() == 1 && decide.children[0]);
	auto ir = DirectIR::Read(optimizer.context, decide);
	if (ir.bound_windows_needed) {
		InstallReducedBoundWindows(optimizer, decide);
		ir = DirectIR::Read(optimizer.context, decide);
	}
	auto &source = *decide.children[0];
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
		// A query no rule applies to leaves the DECIDE node, and its EXPLAIN output, exactly as the solver path has it.
		return false;
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
	// The replacement answers with the DECIDE node's outputs: every source column, then one column per decision.
	source.ResolveOperatorTypes();
	auto output_types = source.types;
	vector<uint8_t> prunable_sources;
	for (auto &binding : source.GetColumnBindings()) {
		prunable_sources.push_back(DirectCanSkipSourceOutput(source, binding) ? 1 : 0);
	}
	for (auto &variable : decide.decide_variables) {
		output_types.push_back(variable->return_type);
	}
	// A stored source carries its row id as one more source column, so a column the rule never uses can be read again
	// later, past the plan that holds rows. The rule passes it along like any other source column.
	auto source_columns = prunable_sources.size();
	DirectLateMaterialization late_materialization;
	bool carries_rowid = DirectExposeStoredRowId(optimizer.context, source, late_materialization);
	if (carries_rowid) {
		source.ResolveOperatorTypes();
	}
	if (DirectAddRowChecks(optimizer, *root, ir.row_checks, decide)) {
		// A row id is exposed only on a repeatable scan chain (DirectExposeStoredRowId), which the row checks' reader
		// recomputes and never copies (DirectSourceCopy::NEVER), so the source kept its bindings.
		D_ASSERT(!carries_rowid);
		// The node reads its source under new bindings: the rule applies to the IR read from them.
		ir = DirectIR::Read(optimizer.context, decide);
		selected.condition = selected.rule->CheckCondition(ir, optimizer.context);
		if (!selected.condition) {
			throw InternalException("Direct solve rule %s does not apply once the row checks are installed",
			                        selected.rule->Name());
		}
	}
	// The source's columns, under the bindings the gate passes them on with.
	auto output_bindings = decide.children[0]->GetColumnBindings();
	output_bindings.resize(source_columns);
	for (idx_t i = 0; i < decide.decide_variables.size(); i++) {
		output_bindings.emplace_back(decide.decide_index, i);
	}
	D_ASSERT(output_bindings.size() == output_types.size());
	D_ASSERT(carries_rowid || output_bindings == decide.GetColumnBindings());
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
	auto adapter = AdaptDirectResult(optimizer.context, std::move(substitute), optimizer.binder.GenerateTableIndex(),
	                                 std::move(output_types), std::move(prunable_sources), std::move(annotation),
	                                 late_materialization);
	auto &installed = *adapter;
	StripTagsAbove(installed, source);
	op = std::move(adapter);
	// The operators above read the DECIDE node's bindings; from here they read the adapter's own.
	RebindAboveAdapter(*root, installed, output_bindings);
	return true;
}

} // namespace duckdb
