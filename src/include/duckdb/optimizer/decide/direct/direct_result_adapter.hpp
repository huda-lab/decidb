#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator_extension.hpp"

namespace duckdb {

//! What the rule driver learned about a DECIDE source that reads one stored table, so the result adapter can later
//! move a column the rule never uses out of the rule's plan and read it from the table again
//! (direct_late_materialization.hpp).
struct DirectLateMaterialization {
	//! Child slot of the rule's plan that carries the table's row id. Invalid when there is no late materialization.
	idx_t rowid_slot = DConstants::INVALID_INDEX;
	//! Table index of the scan the source reads.
	idx_t scan_table_index = DConstants::INVALID_INDEX;
	//! Per source column: the stored column it forwards unchanged, or invalid for a computed column.
	vector<idx_t> stored_columns;

	bool Enabled() const {
		return rowid_slot != DConstants::INVALID_INDEX;
	}
};

//! The logical-only operator that gives a direct-solve plan the bindings and types of the DECIDE node it replaces.
//! The leading expressions map each output to a child binding; the next `required_dependency_count` pin validation
//! dependencies; an optional last one carries the stored table's row id for the late materialization. Unused outputs
//! can become typed NULL placeholders without dropping the validation suffix. Physical planning lowers this to an
//! ordinary projection.
class LogicalDirectResultAdapter final : public LogicalExtensionOperator {
public:
	LogicalDirectResultAdapter() = default;
	LogicalDirectResultAdapter(idx_t decide_index_p, vector<ColumnBinding> output_bindings_p,
	                           vector<LogicalType> output_types_p, idx_t required_dependency_count_p,
	                           vector<uint8_t> prunable_outputs_p, DirectExplainAnnotation annotation_p);

	idx_t decide_index;
	vector<ColumnBinding> output_bindings;
	vector<LogicalType> output_types;
	idx_t required_dependency_count = 0;
	vector<uint8_t> prunable_outputs;
	DirectExplainAnnotation annotation;
	//! True while `expressions` ends with the source row id. It stays live only while a parent reads an output that
	//! `stored_columns` marks as movable; the late-materialization step removes it either way.
	bool has_rowid_dependency = false;
	idx_t scan_table_index = DConstants::INVALID_INDEX;
	//! Per output: the stored column it forwards unchanged, or invalid.
	vector<idx_t> stored_columns;

	vector<ColumnBinding> GetColumnBindings() override;
	void PruneUnusedOutputs(const vector<bool> &referenced_outputs) override;
	vector<idx_t> GetTableIndex() const override;
	string GetName() const override;
	string GetExtensionName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalExtensionOperator> Read(Deserializer &deserializer);
	unique_ptr<PhysicalOperator> CreatePlan(ClientContext &context, PhysicalPlanGenerator &generator) override;

	//! The dependencies the operator must own: one per output, the validation suffix, and the row id when present.
	idx_t ExpectedDependencyCount() const {
		return output_types.size() + required_dependency_count + (has_rowid_dependency ? 1 : 0);
	}

protected:
	void ResolveTypes() override;
};

//! Wraps a rule's relational substitute in the result adapter. The adapter keeps DECIDE's external bindings and
//! types, checks the substitute's output-slot map against its child, and pins validation-only slots as live.
//! `prunable_sources` says, per source column, whether a parent that does not read it may skip evaluating it.
//! An enabled `late_materialization` also carries the source row id as a dependency that a parent-read stored column
//! can use.
unique_ptr<LogicalOperator> AdaptDirectResult(DirectSubstitute substitute, vector<ColumnBinding> output_bindings,
                                              vector<LogicalType> output_types, vector<uint8_t> prunable_sources,
                                              idx_t decide_index, DirectExplainAnnotation annotation,
                                              const DirectLateMaterialization &late_materialization);

//! Lets a serialized plan containing the result adapter be read back.
unique_ptr<OperatorExtension> MakeDirectResultAdapterExtension();

//! The extension name of the result adapter, to recognize it in a plan.
const char *DirectResultExtensionName();

} // namespace duckdb
