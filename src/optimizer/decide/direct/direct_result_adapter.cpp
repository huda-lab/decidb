#include "duckdb/optimizer/decide/direct/direct_result_adapter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

namespace {

constexpr const char *DIRECT_RESULT_EXTENSION = "decidb_direct_solve_result_v4";

class DirectResultAdapterExtension final : public OperatorExtension {
public:
	DirectResultAdapterExtension() {
		// Planner calls every registered operator extension on a binding error.
		// This extension only deserializes the internal result adapter.
		Bind = [](ClientContext &, Binder &, OperatorExtensionInfo *, SQLStatement &) {
			return BoundStatement();
		};
	}
	string GetName() override {
		return DIRECT_RESULT_EXTENSION;
	}
	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer) override {
		return LogicalDirectResultAdapter::Read(deserializer);
	}
};

} // namespace

const char *DirectResultExtensionName() {
	return DIRECT_RESULT_EXTENSION;
}

LogicalDirectResultAdapter::LogicalDirectResultAdapter(idx_t table_index_p, vector<LogicalType> output_types_p,
                                                       idx_t required_dependency_count_p,
                                                       vector<uint8_t> prunable_outputs_p,
                                                       DirectExplainAnnotation annotation_p)
    : table_index(table_index_p), output_types(std::move(output_types_p)),
      required_dependency_count(required_dependency_count_p), prunable_outputs(std::move(prunable_outputs_p)),
      annotation(std::move(annotation_p)) {
}

vector<ColumnBinding> LogicalDirectResultAdapter::GetColumnBindings() {
	return GenerateColumnBindings(table_index, output_types.size());
}

void LogicalDirectResultAdapter::PruneUnusedOutputs(const vector<bool> &referenced_outputs) {
	D_ASSERT(referenced_outputs.size() == output_types.size() && prunable_outputs.size() == output_types.size() &&
	         expressions.size() == ExpectedDependencyCount());
	bool movable_output_read = false;
	for (idx_t i = 0; i < referenced_outputs.size(); i++) {
		if (!referenced_outputs[i] && prunable_outputs[i]) {
			// Hand-built: an overridden DuckDB method has no ClientContext, which the builder needs.
			expressions[i] = make_uniq<BoundConstantExpression>(Value(output_types[i]));
		}
		if (referenced_outputs[i] && i < stored_columns.size() && stored_columns[i] != DConstants::INVALID_INDEX) {
			movable_output_read = true;
		}
	}
	if (has_rowid_dependency && !movable_output_read) {
		// No parent reads a stored column the late materialization could take over, so the row id has no use.
		auto &rowid = expressions.back();
		// Hand-built, like the typed NULL above.
		rowid = make_uniq<BoundConstantExpression>(Value(rowid->return_type));
	}
}

vector<idx_t> LogicalDirectResultAdapter::GetTableIndex() const {
	return {table_index};
}

string LogicalDirectResultAdapter::GetName() const {
	return "DIRECT_RESULT_ADAPTER";
}

string LogicalDirectResultAdapter::GetExtensionName() const {
	return DIRECT_RESULT_EXTENSION;
}

InsertionOrderPreservingMap<string> LogicalDirectResultAdapter::ParamsToString() const {
	auto result = annotation.Render();
	SetParamsEstimatedCardinality(result);
	return result;
}

void LogicalDirectResultAdapter::Serialize(Serializer &serializer) const {
	LogicalExtensionOperator::Serialize(serializer);
	serializer.WriteProperty(201, "table_index", table_index);
	serializer.WriteProperty(203, "output_types", output_types);
	serializer.WriteProperty(204, "required_dependency_count", required_dependency_count);
	serializer.WriteProperty(205, "dependencies", expressions);
	serializer.WriteProperty(206, "mode", annotation.mode);
	serializer.WriteProperty(207, "rule", annotation.rule);
	serializer.WriteProperty(209, "proof", annotation.condition);
	serializer.WriteProperty(213, "prunable_outputs", prunable_outputs);
	serializer.WriteProperty(214, "has_rowid_dependency", has_rowid_dependency);
	serializer.WriteProperty(215, "scan_table_index", scan_table_index);
	serializer.WriteProperty(216, "stored_columns", stored_columns);
}

unique_ptr<LogicalExtensionOperator> LogicalDirectResultAdapter::Read(Deserializer &deserializer) {
	auto result = make_uniq<LogicalDirectResultAdapter>();
	result->table_index = deserializer.ReadProperty<idx_t>(201, "table_index");
	result->output_types = deserializer.ReadProperty<vector<LogicalType>>(203, "output_types");
	result->required_dependency_count = deserializer.ReadProperty<idx_t>(204, "required_dependency_count");
	result->expressions = deserializer.ReadProperty<vector<unique_ptr<Expression>>>(205, "dependencies");
	result->annotation.mode = deserializer.ReadProperty<string>(206, "mode");
	result->annotation.rule = deserializer.ReadProperty<string>(207, "rule");
	result->annotation.condition = deserializer.ReadProperty<string>(209, "proof");
	result->prunable_outputs = deserializer.ReadProperty<vector<uint8_t>>(213, "prunable_outputs");
	result->has_rowid_dependency = deserializer.ReadProperty<bool>(214, "has_rowid_dependency");
	result->scan_table_index = deserializer.ReadProperty<idx_t>(215, "scan_table_index");
	result->stored_columns = deserializer.ReadProperty<vector<idx_t>>(216, "stored_columns");
	return std::move(result);
}

unique_ptr<PhysicalOperator> LogicalDirectResultAdapter::CreatePlan(ClientContext &, PhysicalPlanGenerator &generator) {
	D_ASSERT(children.size() == 1 && prunable_outputs.size() == output_types.size() &&
	         expressions.size() == ExpectedDependencyCount());
	auto child = generator.CreatePlanChild(*children[0]);
	vector<unique_ptr<Expression>> select_list;
	select_list.reserve(output_types.size());
	for (idx_t i = 0; i < expressions.size(); i++) {
		auto &dependency = *expressions[i];
		// A dependency is a slot of the child, or a typed NULL standing in for an output (or the row id) that nothing
		// reads. The validated columns that follow the outputs are always slots.
		D_ASSERT(dependency.GetExpressionClass() == ExpressionClass::BOUND_REF
		             ? dependency.Cast<BoundReferenceExpression>().index < child->types.size() &&
		                   child->types[dependency.Cast<BoundReferenceExpression>().index] == dependency.return_type
		             : dependency.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		                   dependency.Cast<BoundConstantExpression>().value.IsNull());
		if (i < output_types.size()) {
			D_ASSERT(dependency.return_type == output_types[i]);
			select_list.push_back(dependency.Copy());
		}
	}
	auto result = make_uniq<PhysicalProjection>(output_types, std::move(select_list), estimated_cardinality);
	result->explain_metadata = make_uniq<InsertionOrderPreservingMap<string>>(annotation.Render());
	result->children.push_back(std::move(child));
	return std::move(result);
}

void LogicalDirectResultAdapter::ResolveTypes() {
	types = output_types;
}

unique_ptr<OperatorExtension> MakeDirectResultAdapterExtension() {
	return make_uniq<DirectResultAdapterExtension>();
}

unique_ptr<LogicalDirectResultAdapter> AdaptDirectResult(ClientContext &context, DirectSubstitute substitute,
                                                         idx_t table_index, vector<LogicalType> output_types,
                                                         vector<uint8_t> prunable_sources,
                                                         DirectExplainAnnotation annotation,
                                                         const DirectLateMaterialization &late_materialization) {
	if (!substitute.child || substitute.slot_map.size() != output_types.size() ||
	    prunable_sources.size() + substitute.prunable_decisions.size() != output_types.size()) {
		throw InternalException("Direct solve rule returned an incomplete result substitute");
	}
	auto prunable_outputs = std::move(prunable_sources);
	prunable_outputs.insert(prunable_outputs.end(), substitute.prunable_decisions.begin(),
	                        substitute.prunable_decisions.end());
	substitute.child->ResolveOperatorTypes();
	auto child_bindings = substitute.child->GetColumnBindings();
	auto &child_types = substitute.child->types;
	D_ASSERT(child_bindings.size() == child_types.size());
	for (idx_t i = 0; i < substitute.slot_map.size(); i++) {
		auto slot = substitute.slot_map[i];
		if (slot >= child_types.size() || child_types[slot] != output_types[i]) {
			throw InternalException("Direct solve rule returned a slot map that does not match the DECIDE outputs");
		}
	}
	for (auto slot : substitute.validated_columns) {
		if (slot >= child_types.size()) {
			throw InternalException("Direct solve rule lost a required validated column");
		}
	}
	// The driver builds the stored columns, and the check above fixes the decision count.
	D_ASSERT(!late_materialization.Enabled() ||
	         late_materialization.stored_columns.size() + substitute.prunable_decisions.size() == output_types.size());
	if (late_materialization.Enabled() && late_materialization.rowid_slot >= child_types.size()) {
		throw InternalException("Direct solve rule lost the source row id");
	}
	ExpressionBuilder calc(context);
	auto adapter =
	    make_uniq<LogicalDirectResultAdapter>(table_index, std::move(output_types), substitute.validated_columns.size(),
	                                          std::move(prunable_outputs), std::move(annotation));
	// The positional map is materialized as one dependency per advertised output.
	// The required suffix stays live even when all outputs are unused by a parent.
	for (auto slot : substitute.slot_map) {
		adapter->expressions.push_back(calc.Out(calc.Col(child_types[slot], child_bindings[slot])));
	}
	for (auto slot : substitute.validated_columns) {
		adapter->expressions.push_back(calc.Out(calc.Col(child_types[slot], child_bindings[slot])));
	}
	if (late_materialization.Enabled()) {
		adapter->has_rowid_dependency = true;
		adapter->scan_table_index = late_materialization.scan_table_index;
		adapter->stored_columns = late_materialization.stored_columns;
		adapter->stored_columns.resize(adapter->output_types.size(), DConstants::INVALID_INDEX);
		adapter->expressions.push_back(calc.Out(
		    calc.Col(child_types[late_materialization.rowid_slot], child_bindings[late_materialization.rowid_slot])));
	}
	adapter->children.push_back(std::move(substitute.child));
	return adapter;
}

void RebindAboveAdapter(LogicalOperator &root, LogicalDirectResultAdapter &adapter,
                        const vector<ColumnBinding> &replaced) {
	auto bindings = adapter.GetColumnBindings();
	D_ASSERT(replaced.size() == bindings.size());
	ColumnBindingReplacer replacer;
	for (idx_t i = 0; i < bindings.size(); i++) {
		replacer.replacement_bindings.emplace_back(replaced[i], bindings[i]);
	}
	replacer.stop_operator = &adapter;
	replacer.VisitOperator(root);
}

} // namespace duckdb
