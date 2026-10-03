#include "duckdb/optimizer/decide/direct/direct_result_boundary.hpp"

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

//! Preserve the bindings the DECIDE node advertised. The leading expressions
//! map each output to a child binding; the optional suffix pins validation
//! dependencies. Unused outputs can become typed NULL placeholders without
//! dropping the suffix. Physical planning lowers this to an ordinary projection.
class LogicalDirectSolveResult final : public LogicalExtensionOperator {
public:
	LogicalDirectSolveResult() = default;
	LogicalDirectSolveResult(idx_t decide_index_p, vector<ColumnBinding> output_bindings_p,
	                         vector<LogicalType> output_types_p, idx_t required_dependency_count_p,
	                         vector<uint8_t> prunable_outputs_p, DirectSolveDecisionRecord record_p)
	    : decide_index(decide_index_p), output_bindings(std::move(output_bindings_p)),
	      output_types(std::move(output_types_p)), required_dependency_count(required_dependency_count_p),
	      prunable_outputs(std::move(prunable_outputs_p)), record(std::move(record_p)) {
	}

	idx_t decide_index;
	vector<ColumnBinding> output_bindings;
	vector<LogicalType> output_types;
	idx_t required_dependency_count = 0;
	vector<uint8_t> prunable_outputs;
	DirectSolveDecisionRecord record;

	vector<ColumnBinding> GetColumnBindings() override {
		return output_bindings;
	}
	void PruneUnusedOutputs(const vector<bool> &referenced_outputs) override {
		if (referenced_outputs.size() != output_types.size() || prunable_outputs.size() != output_types.size() ||
		    expressions.size() != output_types.size() + required_dependency_count) {
			throw InternalException("Direct solve result boundary has invalid output dependencies");
		}
		for (idx_t i = 0; i < referenced_outputs.size(); i++) {
			if (!referenced_outputs[i] && prunable_outputs[i]) {
				expressions[i] = make_uniq<BoundConstantExpression>(Value(output_types[i]));
			}
		}
	}
	vector<idx_t> GetTableIndex() const override {
		return {decide_index};
	}
	string GetName() const override {
		return "DIRECT_SOLVE_RESULT";
	}
	string GetExtensionName() const override {
		return DIRECT_RESULT_EXTENSION;
	}
	InsertionOrderPreservingMap<string> ParamsToString() const override {
		auto result = record.Render();
		SetParamsEstimatedCardinality(result);
		return result;
	}
	void Serialize(Serializer &serializer) const override {
		LogicalExtensionOperator::Serialize(serializer);
		serializer.WriteProperty(201, "decide_index", decide_index);
		serializer.WriteProperty(202, "output_bindings", output_bindings);
		serializer.WriteProperty(203, "output_types", output_types);
		serializer.WriteProperty(204, "required_dependency_count", required_dependency_count);
		serializer.WriteProperty(205, "dependencies", expressions);
		serializer.WriteProperty(206, "mode", record.mode);
		serializer.WriteProperty(207, "rule", record.rule);
		serializer.WriteProperty(209, "proof", record.proof);
		serializer.WriteProperty(210, "guards", record.guards);
		serializer.WriteProperty(213, "prunable_outputs", prunable_outputs);
	}
	static unique_ptr<LogicalExtensionOperator> Read(Deserializer &deserializer) {
		auto result = make_uniq<LogicalDirectSolveResult>();
		result->decide_index = deserializer.ReadProperty<idx_t>(201, "decide_index");
		result->output_bindings = deserializer.ReadProperty<vector<ColumnBinding>>(202, "output_bindings");
		result->output_types = deserializer.ReadProperty<vector<LogicalType>>(203, "output_types");
		result->required_dependency_count = deserializer.ReadProperty<idx_t>(204, "required_dependency_count");
		result->expressions = deserializer.ReadProperty<vector<unique_ptr<Expression>>>(205, "dependencies");
		result->record.mode = deserializer.ReadProperty<string>(206, "mode");
		result->record.rule = deserializer.ReadProperty<string>(207, "rule");
		result->record.proof = deserializer.ReadProperty<string>(209, "proof");
		result->record.guards = deserializer.ReadProperty<string>(210, "guards");
		result->prunable_outputs = deserializer.ReadProperty<vector<uint8_t>>(213, "prunable_outputs");
		return std::move(result);
	}
	unique_ptr<PhysicalOperator> CreatePlan(ClientContext &, PhysicalPlanGenerator &generator) override {
		if (children.size() != 1 || output_bindings.size() != output_types.size() ||
		    prunable_outputs.size() != output_types.size() ||
		    expressions.size() != output_types.size() + required_dependency_count) {
			throw InternalException("Direct solve result boundary has an invalid output map");
		}
		auto child = generator.CreatePlanChild(*children[0]);
		vector<unique_ptr<Expression>> select_list;
		select_list.reserve(output_types.size());
		for (idx_t i = 0; i < expressions.size(); i++) {
			auto &dependency = *expressions[i];
			if (dependency.GetExpressionClass() == ExpressionClass::BOUND_REF) {
				auto slot = dependency.Cast<BoundReferenceExpression>().index;
				if (slot >= child->types.size() || child->types[slot] != dependency.return_type) {
					throw InternalException("Direct solve dependency refers to the wrong child slot");
				}
			} else if (i >= output_types.size() || dependency.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
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
		result->explain_metadata = make_uniq<InsertionOrderPreservingMap<string>>(record.Render());
		result->children.push_back(std::move(child));
		return std::move(result);
	}

protected:
	void ResolveTypes() override {
		types = output_types;
	}
};

class DirectResultExtension final : public OperatorExtension {
public:
	DirectResultExtension() {
		// Planner calls every registered operator extension on a binding error.
		// This extension only deserializes the internal result boundary.
		Bind = [](ClientContext &, Binder &, OperatorExtensionInfo *, SQLStatement &) { return BoundStatement(); };
	}
	string GetName() override {
		return DIRECT_RESULT_EXTENSION;
	}
	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer) override {
		return LogicalDirectSolveResult::Read(deserializer);
	}
};

} // namespace

unique_ptr<OperatorExtension> MakeDirectResultExtension() {
	return make_uniq<DirectResultExtension>();
}

unique_ptr<LogicalOperator> MapDirectResult(DirectRelationalProposal proposal, vector<ColumnBinding> output_bindings,
                                           vector<LogicalType> output_types, vector<uint8_t> prunable_sources,
                                           idx_t decide_index, DirectSolveDecisionRecord record) {
	if (!proposal.child || output_bindings.size() != output_types.size() ||
	    proposal.output_slots.size() != output_bindings.size() ||
	    prunable_sources.size() + proposal.prunable_decisions.size() != output_bindings.size()) {
		throw InternalException("Direct solve rule returned an incomplete result proposal");
	}
	auto prunable_outputs = std::move(prunable_sources);
	prunable_outputs.insert(prunable_outputs.end(), proposal.prunable_decisions.begin(),
	                        proposal.prunable_decisions.end());
	proposal.child->ResolveOperatorTypes();
	auto child_bindings = proposal.child->GetColumnBindings();
	auto &child_types = proposal.child->types;
	if (child_bindings.size() != child_types.size()) {
		throw InternalException("Direct solve child bindings and types differ");
	}
	for (idx_t i = 0; i < proposal.output_slots.size(); i++) {
		auto slot = proposal.output_slots[i];
		if (slot >= child_types.size() || child_types[slot] != output_types[i]) {
			throw InternalException("Direct solve rule returned a mismatched output slot");
		}
	}
	for (auto slot : proposal.validation_slots) {
		if (slot >= child_types.size()) {
			throw InternalException("Direct solve rule lost a required validation slot");
		}
	}
	auto boundary = make_uniq<LogicalDirectSolveResult>(decide_index, std::move(output_bindings),
	                                                   std::move(output_types), proposal.validation_slots.size(),
	                                                   std::move(prunable_outputs), std::move(record));
	// The positional map is materialized as one dependency per advertised output.
	// The required suffix stays live even when all outputs are unused by a parent.
	for (auto slot : proposal.output_slots) {
		boundary->expressions.push_back(DirectColumn(child_types[slot], child_bindings[slot]));
	}
	for (auto slot : proposal.validation_slots) {
		boundary->expressions.push_back(DirectColumn(child_types[slot], child_bindings[slot]));
	}
	boundary->children.push_back(std::move(proposal.child));
	return std::move(boundary);
}

} // namespace duckdb
