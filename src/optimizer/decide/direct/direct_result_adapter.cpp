#include "duckdb/optimizer/decide/direct/direct_result_adapter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

namespace {

constexpr const char *DIRECT_RESULT_EXTENSION = "decidb_direct_solve_result_v3";

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

LogicalDirectResultAdapter::LogicalDirectResultAdapter(idx_t decide_index_p, vector<ColumnBinding> output_bindings_p,
                                                       vector<LogicalType> output_types_p,
                                                       idx_t required_dependency_count_p,
                                                       vector<uint8_t> prunable_outputs_p,
                                                       DirectExplainAnnotation annotation_p)
    : decide_index(decide_index_p), output_bindings(std::move(output_bindings_p)),
      output_types(std::move(output_types_p)), required_dependency_count(required_dependency_count_p),
      prunable_outputs(std::move(prunable_outputs_p)), annotation(std::move(annotation_p)) {
}

vector<ColumnBinding> LogicalDirectResultAdapter::GetColumnBindings() {
	return output_bindings;
}

void LogicalDirectResultAdapter::PruneUnusedOutputs(const vector<bool> &referenced_outputs) {
	if (referenced_outputs.size() != output_types.size() || prunable_outputs.size() != output_types.size() ||
	    expressions.size() != ExpectedDependencyCount()) {
		throw InternalException("Direct solve result adapter has invalid output dependencies");
	}
	bool movable_output_read = false;
	for (idx_t i = 0; i < referenced_outputs.size(); i++) {
		if (!referenced_outputs[i] && prunable_outputs[i]) {
			expressions[i] = make_uniq<BoundConstantExpression>(Value(output_types[i]));
		}
		if (referenced_outputs[i] && i < stored_columns.size() && stored_columns[i] != DConstants::INVALID_INDEX) {
			movable_output_read = true;
		}
	}
	if (has_rowid_dependency && !movable_output_read) {
		// No parent reads a stored column the late materialization could take over, so the row id has no use.
		auto &rowid = expressions.back();
		rowid = make_uniq<BoundConstantExpression>(Value(rowid->return_type));
	}
}

vector<idx_t> LogicalDirectResultAdapter::GetTableIndex() const {
	return {decide_index};
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
	serializer.WriteProperty(201, "decide_index", decide_index);
	serializer.WriteProperty(202, "output_bindings", output_bindings);
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
	result->decide_index = deserializer.ReadProperty<idx_t>(201, "decide_index");
	result->output_bindings = deserializer.ReadProperty<vector<ColumnBinding>>(202, "output_bindings");
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
	if (children.size() != 1 || output_bindings.size() != output_types.size() ||
	    prunable_outputs.size() != output_types.size() || expressions.size() != ExpectedDependencyCount()) {
		throw InternalException("Direct solve result adapter has an invalid output map");
	}
	auto child = generator.CreatePlanChild(*children[0]);
	vector<unique_ptr<Expression>> select_list;
	select_list.reserve(output_types.size());
	for (idx_t i = 0; i < expressions.size(); i++) {
		auto &dependency = *expressions[i];
		bool is_rowid = has_rowid_dependency && i + 1 == expressions.size();
		if (dependency.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			auto slot = dependency.Cast<BoundReferenceExpression>().index;
			if (slot >= child->types.size() || child->types[slot] != dependency.return_type) {
				throw InternalException("Direct solve dependency refers to the wrong child slot");
			}
		} else if ((i >= output_types.size() && !is_rowid) ||
		           dependency.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
		           !dependency.Cast<BoundConstantExpression>().value.IsNull()) {
			throw InternalException("Direct solve output or validation dependency was not bound");
		}
		if (i < output_types.size()) {
			if (dependency.return_type != output_types[i]) {
				throw InternalException("Direct solve output dependency has the wrong type");
			}
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

unique_ptr<LogicalOperator> AdaptDirectResult(DirectSubstitute substitute, vector<ColumnBinding> output_bindings,
                                              vector<LogicalType> output_types, vector<uint8_t> prunable_sources,
                                              idx_t decide_index, DirectExplainAnnotation annotation,
                                              const DirectLateMaterialization &late_materialization) {
	if (!substitute.child || output_bindings.size() != output_types.size() ||
	    substitute.slot_map.size() != output_bindings.size() ||
	    prunable_sources.size() + substitute.prunable_decisions.size() != output_bindings.size()) {
		throw InternalException("Direct solve rule returned an incomplete result substitute");
	}
	auto prunable_outputs = std::move(prunable_sources);
	prunable_outputs.insert(prunable_outputs.end(), substitute.prunable_decisions.begin(),
	                        substitute.prunable_decisions.end());
	substitute.child->ResolveOperatorTypes();
	auto child_bindings = substitute.child->GetColumnBindings();
	auto &child_types = substitute.child->types;
	if (child_bindings.size() != child_types.size()) {
		throw InternalException("Direct solve child bindings and types differ");
	}
	for (idx_t i = 0; i < substitute.slot_map.size(); i++) {
		auto slot = substitute.slot_map[i];
		if (slot >= child_types.size() || child_types[slot] != output_types[i]) {
			throw InternalException("Direct solve rule returned a mismatched output slot");
		}
	}
	for (auto slot : substitute.validated_columns) {
		if (slot >= child_types.size()) {
			throw InternalException("Direct solve rule lost a required validated column");
		}
	}
	if (late_materialization.Enabled() &&
	    (late_materialization.rowid_slot >= child_types.size() ||
	     late_materialization.stored_columns.size() + substitute.prunable_decisions.size() != output_bindings.size())) {
		throw InternalException("Direct solve rule lost the source row id");
	}
	auto adapter = make_uniq<LogicalDirectResultAdapter>(decide_index, std::move(output_bindings),
	                                                     std::move(output_types), substitute.validated_columns.size(),
	                                                     std::move(prunable_outputs), std::move(annotation));
	// The positional map is materialized as one dependency per advertised output.
	// The required suffix stays live even when all outputs are unused by a parent.
	for (auto slot : substitute.slot_map) {
		adapter->expressions.push_back(DirectColumn(child_types[slot], child_bindings[slot]));
	}
	for (auto slot : substitute.validated_columns) {
		adapter->expressions.push_back(DirectColumn(child_types[slot], child_bindings[slot]));
	}
	if (late_materialization.Enabled()) {
		adapter->has_rowid_dependency = true;
		adapter->scan_table_index = late_materialization.scan_table_index;
		adapter->stored_columns = late_materialization.stored_columns;
		adapter->stored_columns.resize(adapter->output_types.size(), DConstants::INVALID_INDEX);
		adapter->expressions.push_back(DirectColumn(child_types[late_materialization.rowid_slot],
		                                            child_bindings[late_materialization.rowid_slot]));
	}
	adapter->children.push_back(std::move(substitute.child));
	return std::move(adapter);
}

} // namespace duckdb
