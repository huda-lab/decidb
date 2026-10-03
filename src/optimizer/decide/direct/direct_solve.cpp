#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

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

constexpr const char *DIRECT_RESULT_EXTENSION = "decidb_direct_solve_result_v2";
constexpr const char *S1_RULE = "S1_CARDINALITY_INTERVAL";

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

//! Preserve the bindings the DECIDE node advertised. The leading expressions
//! map each output to a child binding; the suffix pins validation dependencies.
//! Unused outputs can become typed NULL placeholders without dropping the
//! suffix. Physical planning lowers this to an ordinary projection.
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
		    expressions.size() != output_types.size() + required_dependency_count || !required_dependency_count) {
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
		serializer.WriteProperty(208, "reason", record.reason);
		serializer.WriteProperty(209, "proof", record.proof);
		serializer.WriteProperty(210, "guards", record.guards);
		serializer.WriteProperty(211, "skipped_solver", record.skipped_solver);
		serializer.WriteProperty(212, "hit", record.hit);
		serializer.WriteProperty(213, "prunable_outputs", prunable_outputs);
	}
	static unique_ptr<LogicalExtensionOperator> Read(Deserializer &deserializer) {
		auto result = make_uniq<LogicalDirectSolveResult>();
		result->decide_index = deserializer.ReadProperty<idx_t>(201, "decide_index");
		result->output_bindings = deserializer.ReadProperty<vector<ColumnBinding>>(202, "output_bindings");
		result->output_types = deserializer.ReadProperty<vector<LogicalType>>(203, "output_types");
		result->required_dependency_count = deserializer.ReadProperty<idx_t>(204, "required_dependency_count");
		result->expressions = deserializer.ReadProperty<vector<unique_ptr<Expression>>>(205, "dependencies");
		result->record.attempted = true;
		result->record.mode = deserializer.ReadProperty<string>(206, "mode");
		result->record.rule = deserializer.ReadProperty<string>(207, "rule");
		result->record.reason = deserializer.ReadProperty<string>(208, "reason");
		result->record.proof = deserializer.ReadProperty<string>(209, "proof");
		result->record.guards = deserializer.ReadProperty<string>(210, "guards");
		result->record.skipped_solver = deserializer.ReadProperty<bool>(211, "skipped_solver");
		result->record.hit = deserializer.ReadProperty<bool>(212, "hit");
		result->prunable_outputs = deserializer.ReadProperty<vector<uint8_t>>(213, "prunable_outputs");
		return std::move(result);
	}
	unique_ptr<PhysicalOperator> CreatePlan(ClientContext &, PhysicalPlanGenerator &generator) override {
		if (children.size() != 1 || output_bindings.size() != output_types.size() ||
		    prunable_outputs.size() != output_types.size() ||
		    !required_dependency_count || expressions.size() != output_types.size() + required_dependency_count) {
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

struct S1Match final : DirectRuleMatch {
	vector<const DirectConstraintFactor *> bound_factors;
	vector<const DirectConstraintFactor *> local_fixes;
};

struct S1Proof final : DirectRuleProof {
	struct SourceBound {
		idx_t source_clause_id;
		idx_t source_slot;
		LogicalType source_type;
		ExpressionType comparison;
		unique_ptr<Expression> value;
		bool rhs_all_group_rows;
	};
	struct SourcePin {
		idx_t source_clause_id;
		unique_ptr<Expression> value;
	};
	struct ObjectivePart {
		int sign;
		unique_ptr<Expression> coefficient;
	};

	idx_t decide_index;
	idx_t lower = 0;
	idx_t upper = 0;
	bool has_upper = false;
	bool impossible = false;
	DecideSense sense;
	DecideVarScope decision_scope;
	LogicalType decision_type;
	bool boolean_domain;
	vector<ObjectivePart> objective_parts;
	vector<idx_t> group_key_slots;
	vector<LogicalType> group_key_types;
	unique_ptr<Expression> when_condition;
	vector<unique_ptr<Expression>> fixed_one_conditions;
	vector<unique_ptr<Expression>> fixed_zero_conditions;
	vector<SourceBound> source_bounds;
	vector<SourcePin> source_pins;
	bool scoped = false;
	bool validate_every_input_row;
};

const BoundAggregateExpression *PlainSum(const Expression &expr, idx_t decide_index, bool allow_filter = false) {
	auto root = UnwrapDecideCasts(expr, decide_index);
	if (root->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return nullptr;
	}
	auto &aggregate = root->Cast<BoundAggregateExpression>();
	if (StringUtil::Lower(aggregate.function.name) != "sum" || aggregate.children.size() != 1 ||
	    (!allow_filter && aggregate.filter) ||
	    aggregate.order_bys || aggregate.IsDistinct()) {
		return nullptr;
	}
	return &aggregate;
}

bool IsExactlyVariable(const Expression &expr, idx_t decide_index) {
	auto variable = GetBareDecideColumnRef(expr, decide_index);
	return variable && variable->binding.column_index == 0 && variable->depth == 0;
}

bool SafeCoefficientTree(const Expression &expr, idx_t decide_index) {
	if (BoundExpressionReferencesDecide(expr, decide_index) || !expr.IsConsistent() || expr.IsVolatile() ||
	    expr.HasSubquery() || expr.HasParameter() || expr.IsAggregate() || expr.IsWindow()) {
		return false;
	}
	bool safe = true;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!SafeCoefficientTree(child, decide_index)) {
			safe = false;
		}
	});
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.Cast<BoundColumnRefExpression>().depth != 0) {
		return false;
	}
	return safe;
}

bool SafeCoefficient(const Expression &expr, idx_t decide_index) {
	return expr.return_type.IsNumeric() && SafeCoefficientTree(expr, decide_index);
}

bool ReferencesOnlySource(const Expression &expr, const vector<ColumnBinding> &source_bindings) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		return ref.depth == 0 &&
		       std::find(source_bindings.begin(), source_bindings.end(), ref.binding) != source_bindings.end();
	}
	bool valid = true;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!ReferencesOnlySource(child, source_bindings)) {
			valid = false;
		}
	});
	return valid;
}

bool HasSourceReference(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { found |= HasSourceReference(child); });
	return found;
}

bool RemapSourceReferences(Expression &expr, const vector<ColumnBinding> &source_bindings, idx_t projection_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		auto found = std::find(source_bindings.begin(), source_bindings.end(), ref.binding);
		if (ref.depth != 0 || found == source_bindings.end()) {
			return false;
		}
		ref.binding = ColumnBinding(projection_index, found - source_bindings.begin());
		return true;
	}
	bool valid = true;
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		if (!RemapSourceReferences(child, source_bindings, projection_index)) {
			valid = false;
		}
	});
	return valid;
}

bool FiniteExactFoldableDouble(ClientContext &context, const Expression &expr, double &result) {
	if (!expr.IsFoldable() || !expr.IsConsistent() || expr.HasParameter() || expr.HasSubquery() ||
	    expr.IsAggregate() || expr.IsWindow()) {
		return false;
	}
	try {
		Value value;
		if (!ExpressionExecutor::TryEvaluateScalar(context, expr, value) || value.IsNull() ||
		    !value.type().IsNumeric()) {
			return false;
		}
		result = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		return std::isfinite(result) && Value::DOUBLE(result).DefaultCastAs(value.type()) == value;
	} catch (Exception &) {
		return false;
	}
}

bool SourceNumericColumnBound(const Expression &expr, const vector<ColumnBinding> &source_bindings, idx_t &slot,
                              LogicalType &type) {
	const Expression *current = &expr;
	while (current->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		if (!current->return_type.IsNumeric() || current->CanThrow()) {
			return false;
		}
		current = current->Cast<BoundCastExpression>().child.get();
	}
	if (current->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &ref = current->Cast<BoundColumnRefExpression>();
	if (ref.depth != 0 || !ref.return_type.IsNumeric()) {
		return false;
	}
	auto found = std::find(source_bindings.begin(), source_bindings.end(), ref.binding);
	if (found == source_bindings.end()) {
		return false;
	}
	slot = found - source_bindings.begin();
	type = ref.return_type;
	return true;
}

bool SourceExpressionMayThrow(const Expression &expr) {
	// TRY_CAST turns conversion failures into NULL; the solver validates that result on every row.
	// BoundCastExpression::CanThrow conservatively treats it like a regular narrowing cast.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	    expr.Cast<BoundFunctionExpression>().function.errors == FunctionErrors::CAN_THROW_RUNTIME_ERROR) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		if (!cast.try_cast && cast.return_type.id() != cast.child->return_type.id() &&
		    LogicalType::ForceMaxLogicalType(cast.return_type, cast.child->return_type) ==
		        cast.child->return_type.id()) {
			return true;
		}
	}
	bool may_throw = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { may_throw |= SourceExpressionMayThrow(child); });
	return may_throw;
}

bool SourceNumericExpressionBound(const Expression &expr, idx_t decide_index,
                                  const vector<ColumnBinding> &source_bindings) {
	return expr.return_type.IsNumeric() && !SourceExpressionMayThrow(expr) && SafeCoefficientTree(expr, decide_index) &&
	       ReferencesOnlySource(expr, source_bindings) && HasSourceReference(expr);
}

bool IsUnitDecisionTerm(ClientContext &context, const Expression &expr, idx_t decide_index) {
	auto root = UnwrapDecideCasts(expr, decide_index);
	if (IsExactlyVariable(*root, decide_index)) {
		return true;
	}
	if (root->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &product = root->Cast<BoundFunctionExpression>();
	if (product.function.name != "*" || product.children.size() != 2) {
		return false;
	}
	const Expression *coefficient = nullptr;
	if (IsExactlyVariable(*product.children[0], decide_index)) {
		coefficient = product.children[1].get();
	} else if (IsExactlyVariable(*product.children[1], decide_index)) {
		coefficient = product.children[0].get();
	}
	double value;
	return coefficient && FiniteExactFoldableDouble(context, *coefficient, value) && value == 1.0;
}

bool NormalizeCapacity(ClientContext &context, const Expression &expr, ExpressionType comparison, idx_t &capacity,
                       bool &impossible) {
	double as_double;
	if (!FiniteExactFoldableDouble(context, expr, as_double)) {
		return false;
	}
	try {
		double inclusive;
		switch (comparison) {
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			inclusive = std::ceil(as_double);
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			if (as_double >= 9007199254740992.0) {
				return false;
			}
			inclusive = std::floor(as_double) + 1.0;
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			inclusive = std::floor(as_double);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			inclusive = std::ceil(as_double) - 1.0;
			break;
		default:
			return false;
		}
		if (comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
		    comparison == ExpressionType::COMPARE_GREATERTHAN) {
			if (inclusive <= 0.0) {
				capacity = 0;
				return true;
			}
		} else if (inclusive < 0.0) {
			impossible = true;
			capacity = 0;
			return true;
		}
		if (!std::isfinite(inclusive) || inclusive > 9007199254740992.0) {
			return false;
		}
		capacity = static_cast<idx_t>(inclusive);
		return true;
	} catch (Exception &) {
		return false;
	}
}

bool FixedBooleanValue(ClientContext &context, const BoundComparisonExpression &comparison, idx_t &fixed_value) {
	double bound;
	if (!FiniteExactFoldableDouble(context, *comparison.right, bound) || (bound != 0.0 && bound != 1.0)) {
		return false;
	}
	switch (comparison.GetExpressionType()) {
	case ExpressionType::COMPARE_EQUAL:
		fixed_value = bound == 1.0 ? 1 : 0;
		return true;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		if (bound == 0.0) {
			fixed_value = 0;
			return true;
		}
		return false;
	case ExpressionType::COMPARE_LESSTHAN:
		if (bound == 1.0) {
			fixed_value = 0;
			return true;
		}
		return false;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		if (bound == 1.0) {
			fixed_value = 1;
			return true;
		}
		return false;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (bound == 0.0) {
			fixed_value = 1;
			return true;
		}
		return false;
	default:
		return false;
	}
}

const Expression *SourceBooleanPinValue(const Expression &expr, idx_t decide_index,
                                        const vector<ColumnBinding> &source_bindings) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return nullptr;
	}
	auto &cast = expr.Cast<BoundCastExpression>();
	if (cast.child->return_type != LogicalType::BOOLEAN || !expr.return_type.IsNumeric() ||
	    SourceExpressionMayThrow(*cast.child) || !SafeCoefficientTree(*cast.child, decide_index) ||
	    !ReferencesOnlySource(*cast.child, source_bindings) || !HasSourceReference(*cast.child)) {
		return nullptr;
	}
	return cast.child.get();
}

class S1CardinalityRule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return S1_RULE;
	}
	unique_ptr<DirectRuleMatch> Match(const DirectProblemFacts &facts, string &reason) const override {
		if (facts.decisions_status != DirectFactStatus::KNOWN || facts.decisions.size() != 1 ||
		    facts.auxiliary_variables != 0 || !facts.decisions[0].boolean_domain ||
		    facts.decisions[0].scope != DecideVarScope::ROW || facts.decisions[0].output_type != LogicalType::INTEGER ||
		    facts.has_entity_scopes || facts.has_entity_keys || facts.source_status != DirectFactStatus::KNOWN) {
			reason = "variable_shape: expected one row-scoped BOOL";
			return nullptr;
		}
		if (!std::isfinite(facts.objective_offset) ||
		    (facts.sense != DecideSense::MAXIMIZE && facts.sense != DecideSense::MINIMIZE)) {
			reason = "problem_shape: expected one linear objective and cardinality bounds";
			return nullptr;
		}
		if (facts.constraints_status != DirectFactStatus::KNOWN) {
			reason = "constraint_facts_unknown: DECIDE constraint wrapper is not modeled";
			return nullptr;
		}
		if (facts.constraint_factors.empty() || facts.source_clause_count != facts.constraint_factors.size()) {
			reason = "constraint_shape: expected attributed cardinality bounds and optional Boolean pins";
			return nullptr;
		}
		auto result = make_uniq<S1Match>();
		vector<uint8_t> seen_sources(facts.source_clause_count, 0);
		for (auto &factor : facts.constraint_factors) {
			if (factor.source_status != DirectFactStatus::KNOWN ||
			    factor.source_clause_id >= seen_sources.size() || seen_sources[factor.source_clause_id]++) {
				reason = "constraint_provenance: expected one distinct attributed source clause per factor";
				return nullptr;
			}
			if (factor.expression->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
				reason = "constraint_shape: expected SUM(x) bounds or Boolean pins";
				return nullptr;
			}
			auto &comparison = factor.expression->Cast<BoundComparisonExpression>();
			if (IsExactlyVariable(*comparison.left, facts.decide_index)) {
				result->local_fixes.push_back(&factor);
				continue;
			}
			switch (comparison.GetExpressionType()) {
			case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			case ExpressionType::COMPARE_LESSTHAN:
			case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			case ExpressionType::COMPARE_GREATERTHAN:
			case ExpressionType::COMPARE_EQUAL:
				result->bound_factors.push_back(&factor);
				break;
			default:
				reason = "constraint_shape: expected SUM(x) cardinality bounds";
				return nullptr;
			}
		}
		if (result->bound_factors.empty()) {
			reason = "constraint_shape: expected a SUM(x) cardinality bound";
			return nullptr;
		}
		if (facts.objective_status != DirectFactStatus::KNOWN) {
			reason = "objective_facts_unknown: DECIDE objective wrapper is not modeled";
			return nullptr;
		}
		if (facts.objective_terms.empty()) {
			reason = "objective_shape: expected additive SUM(coefficient * x) terms";
			return nullptr;
		}
		return std::move(result);
	}
	unique_ptr<DirectRuleProof> Prove(const DirectProblemFacts &facts, const DirectRuleMatch &candidate,
	                                  ClientContext &context, string &reason) const override {
		auto &match = static_cast<const S1Match &>(candidate);
		auto proof = make_uniq<S1Proof>();
		proof->decide_index = facts.decide_index;
		proof->sense = facts.sense;
		proof->decision_scope = facts.decisions[0].scope;
		proof->decision_type = facts.decisions[0].output_type;
		proof->boolean_domain = facts.decisions[0].boolean_domain;
		proof->output_bindings = facts.source_bindings;
		proof->output_bindings.emplace_back(facts.decide_index, 0);
		const auto &first_scope = *match.bound_factors.front();
		auto &first_comparison = first_scope.expression->Cast<BoundComparisonExpression>();
		auto first_sum = PlainSum(*first_comparison.left, facts.decide_index, true);
		if (!first_sum || (first_sum->filter && first_scope.when_condition)) {
			reason = "constraint_shape: expected one SUM(x) membership filter";
			return nullptr;
		}
		const Expression *first_when = first_sum->filter ? first_sum->filter.get() : first_scope.when_condition;
		for (auto factor : match.bound_factors) {
			auto &comparison = factor->expression->Cast<BoundComparisonExpression>();
			auto sum = PlainSum(*comparison.left, facts.decide_index, true);
			if (!sum || (sum->filter && factor->when_condition)) {
				reason = "constraint_shape: expected one SUM(x) membership filter";
				return nullptr;
			}
			const Expression *when = sum->filter ? sum->filter.get() : factor->when_condition;
			if (factor->per_keys.size() != first_scope.per_keys.size() ||
			    static_cast<bool>(when) != static_cast<bool>(first_when)) {
				reason = "constraint_scope: cardinality bounds use different PER or WHEN membership";
				return nullptr;
			}
			if (when && !Expression::Equals(*when, *first_when)) {
				reason = "constraint_scope: cardinality bounds use different WHEN membership";
				return nullptr;
			}
			for (idx_t i = 0; i < factor->per_keys.size(); i++) {
				if (!Expression::Equals(*factor->per_keys[i], *first_scope.per_keys[i])) {
					reason = "constraint_scope: cardinality bounds use different PER membership";
					return nullptr;
				}
			}
		}
		for (auto key : first_scope.per_keys) {
			if (key->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				reason = "constraint_scope: PER keys must be source columns";
				return nullptr;
			}
			auto &ref = key->Cast<BoundColumnRefExpression>();
			auto found = std::find(facts.source_bindings.begin(), facts.source_bindings.end(), ref.binding);
			if (ref.depth != 0 || found == facts.source_bindings.end()) {
				reason = "constraint_scope: PER keys must be source columns";
				return nullptr;
			}
			proof->group_key_slots.push_back(found - facts.source_bindings.begin());
			proof->group_key_types.push_back(ref.return_type);
		}
		if (first_when) {
			if (first_when->return_type != LogicalType::BOOLEAN || first_when->CanThrow() ||
			    !SafeCoefficientTree(*first_when, facts.decide_index) ||
			    !ReferencesOnlySource(*first_when, facts.source_bindings)) {
				reason = "constraint_scope: WHEN must be a deterministic source-only predicate";
				return nullptr;
			}
			proof->when_condition = first_when->Copy();
		}
		for (auto factor : match.local_fixes) {
			if (!factor->per_keys.empty()) {
				reason = "constraint_scope: Boolean pins must be per-row without PER keys";
				return nullptr;
			}
			auto &comparison = factor->expression->Cast<BoundComparisonExpression>();
			if (factor->when_condition &&
			    (factor->when_condition->return_type != LogicalType::BOOLEAN || factor->when_condition->CanThrow() ||
			     !SafeCoefficientTree(*factor->when_condition, facts.decide_index) ||
			     !ReferencesOnlySource(*factor->when_condition, facts.source_bindings))) {
				reason = "constraint_scope: Boolean pin WHEN must be deterministic and source-only";
				return nullptr;
			}
			idx_t fixed_value;
			if (FixedBooleanValue(context, comparison, fixed_value)) {
				auto condition = factor->when_condition ? factor->when_condition->Copy()
				                                        : make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
				if (fixed_value) {
					proof->fixed_one_conditions.push_back(std::move(condition));
				} else {
					proof->fixed_zero_conditions.push_back(std::move(condition));
				}
				continue;
			}
			auto source_value = SourceBooleanPinValue(*comparison.right, facts.decide_index, facts.source_bindings);
			auto type = comparison.GetExpressionType();
			if (!source_value || (type != ExpressionType::COMPARE_EQUAL &&
			                      type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
			                      type != ExpressionType::COMPARE_GREATERTHANOREQUALTO)) {
				reason = "constraint_shape: expected a Boolean pin at zero or one or a source Boolean equality/bound";
				return nullptr;
			}
			auto active = [&](unique_ptr<Expression> value) -> unique_ptr<Expression> {
				return factor->when_condition
				           ? make_uniq<BoundCaseExpression>(factor->when_condition->Copy(), std::move(value),
				                                            make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)))
				           : std::move(value);
			};
			if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
				proof->fixed_one_conditions.push_back(active(source_value->Copy()));
			}
			if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
				proof->fixed_zero_conditions.push_back(active(make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL, source_value->Copy(),
				    make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)))));
			}
			proof->source_pins.push_back({factor->source_clause_id, source_value->Copy()});
		}
		proof->scoped = !proof->group_key_slots.empty() || proof->when_condition != nullptr;
		proof->lower = 0;
		auto prove_bound = [&](const BoundComparisonExpression *comparison, ExpressionType side, idx_t &target) {
			bool impossible = false;
			if (!NormalizeCapacity(context, *comparison->right, side, target, impossible)) {
				reason = "constraint_shape: cardinality bound must be a finite consistent foldable numeric expression "
				         "with an inclusive limit in [0, 2^53], or a deterministic nonthrowing source-only numeric expression";
				return false;
			}
			proof->impossible |= impossible;
			return true;
		};
		for (auto factor : match.bound_factors) {
			auto &comparison = factor->expression->Cast<BoundComparisonExpression>();
			auto sum = PlainSum(*comparison.left, facts.decide_index, true);
			if (!sum || !IsUnitDecisionTerm(context, *sum->children[0], facts.decide_index)) {
				reason = "constraint_shape: expected SUM(x) with unit contribution";
				return nullptr;
			}
			auto type = comparison.GetExpressionType();
			idx_t source_slot = DConstants::INVALID_INDEX;
			LogicalType source_type;
			bool source_column = SourceNumericColumnBound(*comparison.right, facts.source_bindings,
			                                             source_slot, source_type);
			if (!comparison.right->IsFoldable() &&
			    (source_column || SourceNumericExpressionBound(*comparison.right, facts.decide_index,
			                                                    facts.source_bindings))) {
			proof->source_bounds.push_back({factor->source_clause_id, source_slot,
			                                source_column ? source_type : comparison.right->return_type,
				                                type, comparison.right->Copy(), sum->filter != nullptr});
				continue;
			}
			if (type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
			    type == ExpressionType::COMPARE_GREATERTHAN || type == ExpressionType::COMPARE_EQUAL) {
				idx_t lower;
				auto side = type == ExpressionType::COMPARE_EQUAL ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : type;
				if (!prove_bound(&comparison, side, lower)) {
					return nullptr;
				}
				proof->lower = std::max(proof->lower, lower);
			}
			if (type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
			    type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_EQUAL) {
				idx_t upper;
				auto side = type == ExpressionType::COMPARE_EQUAL ? ExpressionType::COMPARE_LESSTHANOREQUALTO : type;
				if (!prove_bound(&comparison, side, upper)) {
					return nullptr;
				}
				if (proof->has_upper) {
					proof->upper = std::min(proof->upper, upper);
				} else {
					proof->upper = upper;
					proof->has_upper = true;
				}
			}
		}
		if (proof->has_upper && proof->lower > proof->upper) {
			proof->impossible = true;
		}
		for (auto &objective_term : facts.objective_terms) {
			if (objective_term.sign != 1 && objective_term.sign != -1) {
				reason = "objective_shape: expected signed linear objective terms";
				return nullptr;
			}
			auto objective_sum = PlainSum(*objective_term.expression, facts.decide_index);
			if (!objective_sum) {
				reason = "objective_shape: expected unfiltered SUM(coefficient * x) terms";
				return nullptr;
			}
			auto term = UnwrapDecideCasts(*objective_sum->children[0], facts.decide_index);
			unique_ptr<Expression> coefficient;
			if (IsExactlyVariable(*term, facts.decide_index)) {
				coefficient = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
			} else {
				if (term->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
					reason = "objective_shape: expected a linear product with x";
					return nullptr;
				}
				auto &product = term->Cast<BoundFunctionExpression>();
				if (product.function.name != "*" || product.children.size() != 2) {
					reason = "objective_shape: expected a coefficient times x";
					return nullptr;
				}
				const Expression *source_coefficient = nullptr;
				if (IsExactlyVariable(*product.children[0], facts.decide_index)) {
					source_coefficient = product.children[1].get();
				} else if (IsExactlyVariable(*product.children[1], facts.decide_index)) {
					source_coefficient = product.children[0].get();
				}
				if (!source_coefficient || !SafeCoefficient(*source_coefficient, facts.decide_index)) {
					reason = "coefficient_shape: expected a deterministic numeric expression without decisions";
					return nullptr;
				}
				coefficient = source_coefficient->Copy();
			}
			if (!ReferencesOnlySource(*coefficient, facts.source_bindings) ||
			    (facts.objective_terms.size() > 1 && SourceExpressionMayThrow(*coefficient))) {
				reason = "coefficient_shape: multiple terms need nonthrowing source-only numeric coefficients";
				return nullptr;
			}
			proof->objective_parts.push_back({objective_term.sign, std::move(coefficient)});
		}
		proof->validate_every_input_row = true;
		return std::move(proof);
	}
	double Cost(const DirectRuleProof &) const override {
		// The first rule has no competing proved alternative yet.
		return 0.0;
	}
	void Explain(const DirectRuleProof &candidate, DirectSolveDecisionRecord &record) const override {
		auto &proof = static_cast<const S1Proof &>(candidate);
		auto has_fixes = !proof.fixed_one_conditions.empty() || !proof.fixed_zero_conditions.empty();
		auto scope = proof.group_key_slots.empty() ? (proof.when_condition ? "WHEN-scoped" : "global")
		                                           : (proof.when_condition ? "grouped WHEN-scoped" : "grouped");
		if (!proof.source_bounds.empty()) {
			record.proof = StringUtil::Format("one row-scoped Boolean; exact %s source-numeric cardinality "
			                                  "bounds with any constant interval", scope);
			for (auto &bound : proof.source_bounds) {
				if (bound.rhs_all_group_rows) {
					record.proof += "; aggregate-local count filter with full-group RHS reduction";
					break;
				}
			}
			record.guards = "active membership checked; every source bound checked for NULL/NaN before score; "
			                "rank and feasibility retained";
			if (has_fixes) {
				record.proof += "; source-only Boolean pins";
				record.guards += "; pin conflicts checked";
			}
			return;
		}
		if (proof.impossible) {
			record.proof = StringUtil::Format("one row-scoped Boolean; exact %s cardinality bounds "
			                                  "infeasible for a nonempty eligible group", scope);
			record.guards = "score checked on all rows; active membership checked; rank retained";
			if (has_fixes) {
				record.proof += "; source-only Boolean pins";
				record.guards += "; pin conflicts checked";
			}
			return;
		}
		record.proof = proof.has_upper
		                   ? StringUtil::Format("one row-scoped Boolean; exact %s cardinality "
		                                        "interval [%llu, %llu]; %s linear score", scope,
		                                        static_cast<unsigned long long>(proof.lower),
		                                        static_cast<unsigned long long>(proof.upper),
		                                        proof.sense == DecideSense::MAXIMIZE ? "maximizing" : "minimizing")
		                   : StringUtil::Format("one row-scoped Boolean; exact %s lower bound %llu; "
		                                        "%s linear score", scope,
		                                        static_cast<unsigned long long>(proof.lower),
		                                        proof.sense == DecideSense::MAXIMIZE ? "maximizing" : "minimizing");
		record.guards = proof.lower ? "score checked on all rows; active count checked; rank retained"
		                            : "score NULL/non-finite checked on all input rows; rank retained";
		if (has_fixes) {
			record.proof += "; source-only Boolean pins";
			record.guards += "; pin conflicts and residual counts checked";
		}
	}
	DirectRelationalProposal Rewrite(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                                 const DirectRuleProof &candidate) const override;
};

unique_ptr<Expression> Column(const LogicalType &type, ColumnBinding binding) {
	return make_uniq<BoundColumnRefExpression>(type, binding);
}

unique_ptr<Expression> ConstantBool(bool value) {
	return make_uniq<BoundConstantExpression>(Value::BOOLEAN(value));
}

unique_ptr<Expression> AnyCondition(const vector<unique_ptr<Expression>> &conditions) {
	unique_ptr<Expression> result = ConstantBool(false);
	for (auto &condition : conditions) {
		result = make_uniq<BoundCaseExpression>(condition->Copy(), ConstantBool(true), std::move(result));
	}
	return result;
}

unique_ptr<BoundWindowExpression> WindowMatchingCount(unique_ptr<Expression> predicate) {
	auto count = make_uniq<BoundWindowExpression>(
	    ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
	    make_uniq<AggregateFunction>(CountFun::GetFunctions().GetFunctionByOffset(0)), nullptr);
	count->start = WindowBoundary::UNBOUNDED_PRECEDING;
	count->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	count->children.push_back(make_uniq<BoundCaseExpression>(
	    std::move(predicate), make_uniq<BoundConstantExpression>(Value::INTEGER(1)),
	    make_uniq<BoundConstantExpression>(Value(LogicalType::INTEGER))));
	return count;
}

unique_ptr<BoundWindowExpression> WindowExtremum(Optimizer &optimizer, const char *name,
                                                unique_ptr<Expression> value) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(value));
	auto bound = optimizer.BindAggregateFunction(name, std::move(children));
	auto &aggregate = bound->Cast<BoundAggregateExpression>();
	auto result = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, aggregate.return_type,
	                                                make_uniq<AggregateFunction>(aggregate.function),
	                                                std::move(aggregate.bind_info));
	result->children = std::move(aggregate.children);
	result->start = WindowBoundary::UNBOUNDED_PRECEDING;
	result->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	return result;
}

unique_ptr<Expression> ErrorPredicate(Optimizer &optimizer, const string &message) {
	auto error = optimizer.BindScalarFunction("error", make_uniq<BoundConstantExpression>(Value(message)));
	return BoundCastExpression::AddCastToType(optimizer.context, std::move(error), LogicalType::BOOLEAN);
}

unique_ptr<Expression> ValidScorePredicate(Optimizer &optimizer, const LogicalType &score_type,
	                                         ColumnBinding score_binding) {
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->children.push_back(Column(score_type, score_binding));
	auto is_finite = optimizer.BindScalarFunction("isfinite", Column(score_type, score_binding));
	auto finite_or_error = make_uniq<BoundCaseExpression>(
	    std::move(is_finite), ConstantBool(true),
	    ErrorPredicate(optimizer, "Direct solve coefficient is non-finite"));
	return make_uniq<BoundCaseExpression>(std::move(is_null),
	                                      ErrorPredicate(optimizer, "Direct solve coefficient is NULL"),
	                                      std::move(finite_or_error));
}

//! The solver consumes every source output. Skipping an unreferenced output
//! must not suppress a computed error or volatile expression. This deliberately
//! proves only stored columns, constants, and their passthrough aliases safe,
//! including passthrough bindings from one child of an inner comparison join.
bool CanSkipSourceOutput(LogicalOperator &source, ColumnBinding binding) {
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
		return child_ref.depth == 0 && CanSkipSourceOutput(*source.children[0], child_ref.binding);
	}
	case LogicalOperatorType::LOGICAL_FILTER:
		return source.children.size() == 1 && CanSkipSourceOutput(*source.children[0], binding);
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
		return CanSkipSourceOutput(*source.children[on_right ? 1 : 0], binding);
	}
	default:
		return false;
	}
}

DirectRelationalProposal S1CardinalityRule::Rewrite(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                                                const DirectRuleProof &candidate) const {
	auto &proof = static_cast<const S1Proof &>(candidate);
	if (proof.objective_parts.empty() || !proof.validate_every_input_row || !proof.boolean_domain ||
	    proof.decision_scope != DecideVarScope::ROW || proof.decision_type != LogicalType::INTEGER ||
	    proof.output_bindings.empty() || proof.output_bindings.back() != ColumnBinding(proof.decide_index, 0) ||
	    proof.group_key_slots.size() != proof.group_key_types.size()) {
		throw InternalException("S1 direct solve received an incomplete proof");
	}
	// The DECIDE optimizer runs before the physical planner's type-resolution
	// pass. Resolve the unchanged input before constructing typed bound refs.
	source->ResolveOperatorTypes();
	auto source_bindings = source->GetColumnBindings();
	auto source_types = source->types;
	if (source_bindings.size() != source_types.size()) {
		throw InternalException("Direct solve source bindings and types differ");
	}
	vector<uint8_t> prunable_outputs;
	prunable_outputs.reserve(source_bindings.size() + 1);
	for (auto &binding : source_bindings) {
		prunable_outputs.push_back(CanSkipSourceOutput(*source, binding) ? 1 : 0);
	}
	prunable_outputs.push_back(1); // x is pure once score validation and ranking remain live
	auto &binder = optimizer.binder;
	auto score_index = binder.GenerateTableIndex();
	auto window_index = binder.GenerateTableIndex();
	auto result_index = binder.GenerateTableIndex();
	vector<ColumnBinding> input_bindings = source_bindings;
	ColumnBinding source_eligible_binding;
	ColumnBinding source_fixed_one_binding;
	ColumnBinding source_fixed_zero_binding;
	auto has_fixes = !proof.fixed_one_conditions.empty() || !proof.fixed_zero_conditions.empty();
	idx_t state_index = DConstants::INVALID_INDEX;
	if (proof.scoped || has_fixes) {
		state_index = binder.GenerateTableIndex();
		vector<unique_ptr<Expression>> state_expressions;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			state_expressions.push_back(Column(source_types[i], source_bindings[i]));
			input_bindings[i] = ColumnBinding(state_index, i);
		}
		if (proof.scoped) {
			unique_ptr<Expression> eligible = proof.when_condition
			                                      ? make_uniq<BoundCaseExpression>(proof.when_condition->Copy(),
			                                                                       ConstantBool(true), ConstantBool(false))
			                                      : ConstantBool(true);
			for (idx_t i = 0; i < proof.group_key_slots.size(); i++) {
				auto slot = proof.group_key_slots[i];
				if (slot >= source_bindings.size() || source_types[slot] != proof.group_key_types[i]) {
					throw InternalException("S1 direct solve received an invalid PER key slot");
				}
				auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
				is_null->children.push_back(Column(source_types[slot], source_bindings[slot]));
				eligible = make_uniq<BoundCaseExpression>(std::move(is_null), ConstantBool(false), std::move(eligible));
			}
			state_expressions.push_back(std::move(eligible));
			source_eligible_binding = ColumnBinding(state_index, source_bindings.size());
		}
		if (has_fixes) {
			auto fixed_one_slot = state_expressions.size();
			state_expressions.push_back(AnyCondition(proof.fixed_one_conditions));
			state_expressions.push_back(AnyCondition(proof.fixed_zero_conditions));
			source_fixed_one_binding = ColumnBinding(state_index, fixed_one_slot);
			source_fixed_zero_binding = ColumnBinding(state_index, fixed_one_slot + 1);
		}
		auto state = make_uniq<LogicalProjection>(state_index, std::move(state_expressions));
		state->children.push_back(std::move(source));
		source = std::move(state);
	}
	if (proof.scoped) {
		auto active_window_index = binder.GenerateTableIndex();
		auto active_count = WindowMatchingCount(Column(LogicalType::BOOLEAN, source_eligible_binding));
		auto active_window = make_uniq<LogicalWindow>(active_window_index);
		active_window->expressions.push_back(std::move(active_count));
		active_window->children.push_back(std::move(source));
		auto has_active_rows = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHAN, Column(LogicalType::BIGINT, ColumnBinding(active_window_index, 0)),
		    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
		auto active_predicate = make_uniq<BoundCaseExpression>(
		    std::move(has_active_rows), ConstantBool(true),
		    ErrorPredicate(optimizer, "DECIDE empty row set for aggregate in constraint. "
		                              "An empty aggregate has no well-defined value; check your WHEN clause."));
		auto active_guard = make_uniq<LogicalFilter>(std::move(active_predicate));
		active_guard->children.push_back(std::move(active_window));
		source = std::move(active_guard);
	}
	struct SourceBoundSlots {
		idx_t invalid_window = DConstants::INVALID_INDEX;
		idx_t min_window = DConstants::INVALID_INDEX;
		idx_t max_window = DConstants::INVALID_INDEX;
		idx_t min_score = DConstants::INVALID_INDEX;
		idx_t max_score = DConstants::INVALID_INDEX;
	};
	vector<SourceBoundSlots> bound_slots(proof.source_bounds.size());
	vector<idx_t> pin_invalid_windows(proof.source_pins.size(), DConstants::INVALID_INDEX);
	idx_t source_bound_window_index = DConstants::INVALID_INDEX;
	if (!proof.source_bounds.empty() || !proof.source_pins.empty()) {
		source_bound_window_index = binder.GenerateTableIndex();
		auto bound_window = make_uniq<LogicalWindow>(source_bound_window_index);
		for (idx_t i = 0; i < proof.source_bounds.size(); i++) {
			auto &bound = proof.source_bounds[i];
			auto &slots = bound_slots[i];
			if (!bound.value || (bound.source_slot != DConstants::INVALID_INDEX &&
			                     (bound.source_slot >= source_types.size() ||
			                      source_types[bound.source_slot] != bound.source_type))) {
				throw InternalException("S1 direct solve received an invalid source bound slot");
			}
			auto cap = [&]() {
				auto value = bound.value->Copy();
				if (state_index != DConstants::INVALID_INDEX &&
				    !RemapSourceReferences(*value, source_bindings, state_index)) {
					throw InternalException("S1 direct solve could not remap its proved bound");
				}
				return BoundCastExpression::AddCastToType(optimizer.context, std::move(value), LogicalType::DOUBLE);
			};
			auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
			is_null->children.push_back(bound.source_slot == DConstants::INVALID_INDEX
			                                ? cap()
			                                : Column(bound.source_type, input_bindings[bound.source_slot]));
			unique_ptr<Expression> is_invalid = std::move(is_null);
			if (bound.source_slot == DConstants::INVALID_INDEX || bound.source_type == LogicalType::FLOAT ||
			    bound.source_type == LogicalType::DOUBLE) {
				is_invalid = make_uniq<BoundCaseExpression>(
				    std::move(is_invalid), ConstantBool(true), optimizer.BindScalarFunction("isnan", cap()));
			}
			slots.invalid_window = bound_window->expressions.size();
			bound_window->expressions.push_back(WindowMatchingCount(std::move(is_invalid)));
			auto add_extremum = [&](const char *name) {
				unique_ptr<Expression> value = cap();
				if (proof.scoped && !bound.rhs_all_group_rows) {
					value = make_uniq<BoundCaseExpression>(Column(LogicalType::BOOLEAN, source_eligible_binding),
					                                       std::move(value),
					                                       make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE)));
				}
				auto extremum = WindowExtremum(optimizer, name, std::move(value));
				if (proof.scoped && !bound.rhs_all_group_rows) {
					extremum->partitions.push_back(Column(LogicalType::BOOLEAN, source_eligible_binding));
				}
				for (auto slot : proof.group_key_slots) {
					extremum->partitions.push_back(Column(source_types[slot], input_bindings[slot]));
				}
				auto result_slot = bound_window->expressions.size();
				bound_window->expressions.push_back(std::move(extremum));
				return result_slot;
			};
			if (bound.comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
			    bound.comparison == ExpressionType::COMPARE_LESSTHAN ||
			    bound.comparison == ExpressionType::COMPARE_EQUAL) {
				slots.min_window = add_extremum("min");
			}
			if (bound.comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
			    bound.comparison == ExpressionType::COMPARE_GREATERTHAN ||
			    bound.comparison == ExpressionType::COMPARE_EQUAL) {
				slots.max_window = add_extremum("max");
			}
		}
		for (idx_t i = 0; i < proof.source_pins.size(); i++) {
			auto value = proof.source_pins[i].value->Copy();
			if (state_index != DConstants::INVALID_INDEX &&
			    !RemapSourceReferences(*value, source_bindings, state_index)) {
				throw InternalException("S1 direct solve could not remap its proved Boolean pin");
			}
			auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
			is_null->children.push_back(std::move(value));
			pin_invalid_windows[i] = bound_window->expressions.size();
			bound_window->expressions.push_back(WindowMatchingCount(std::move(is_null)));
		}
		bound_window->children.push_back(std::move(source));
		unique_ptr<LogicalOperator> bounds_ready = std::move(bound_window);
		vector<idx_t> equality_slots(proof.source_bounds.size(), DConstants::INVALID_INDEX);
		idx_t equality_window_index = DConstants::INVALID_INDEX;
		unique_ptr<LogicalWindow> equality_window;
		for (idx_t i = 0; i < proof.source_bounds.size(); i++) {
			if (proof.source_bounds[i].comparison != ExpressionType::COMPARE_EQUAL) {
				continue;
			}
			if (!equality_window) {
				equality_window_index = binder.GenerateTableIndex();
				equality_window = make_uniq<LogicalWindow>(equality_window_index);
			}
			auto &slots = bound_slots[i];
			unique_ptr<Expression> varies = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_NOTEQUAL,
			    Column(LogicalType::DOUBLE, ColumnBinding(source_bound_window_index, slots.min_window)),
			    Column(LogicalType::DOUBLE, ColumnBinding(source_bound_window_index, slots.max_window)));
			if (proof.scoped) {
				varies = make_uniq<BoundCaseExpression>(Column(LogicalType::BOOLEAN, source_eligible_binding),
				                                       std::move(varies), ConstantBool(false));
			}
			equality_slots[i] = equality_window->expressions.size();
			equality_window->expressions.push_back(WindowMatchingCount(std::move(varies)));
		}
		if (equality_window) {
			equality_window->children.push_back(std::move(bounds_ready));
			bounds_ready = std::move(equality_window);
		}
		vector<pair<idx_t, unique_ptr<Expression>>> clause_checks;
		for (idx_t i = 0; i < proof.source_bounds.size(); i++) {
			auto &bound = proof.source_bounds[i];
			auto &slots = bound_slots[i];
			auto no_invalid = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    Column(LogicalType::BIGINT, ColumnBinding(source_bound_window_index, slots.invalid_window)),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
			unique_ptr<Expression> equality_valid = ConstantBool(true);
			if (bound.comparison == ExpressionType::COMPARE_EQUAL) {
				auto no_variation = make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    Column(LogicalType::BIGINT, ColumnBinding(equality_window_index, equality_slots[i])),
				    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
				equality_valid = make_uniq<BoundCaseExpression>(
				    std::move(no_variation), ConstantBool(true),
				    ErrorPredicate(optimizer, "DECIDE source-valued equality bound varies within a group"));
			}
			auto clause_valid = make_uniq<BoundCaseExpression>(
			    std::move(no_invalid), std::move(equality_valid),
			    ErrorPredicate(optimizer, "DECIDE constraint right-hand side contains NULL or NaN"));
			clause_checks.emplace_back(bound.source_clause_id, std::move(clause_valid));
		}
		for (idx_t i = 0; i < proof.source_pins.size(); i++) {
			auto no_invalid = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    Column(LogicalType::BIGINT, ColumnBinding(source_bound_window_index, pin_invalid_windows[i])),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
			auto clause_valid = make_uniq<BoundCaseExpression>(
			    std::move(no_invalid), ConstantBool(true),
			    ErrorPredicate(optimizer, "DECIDE per-row Boolean bound contains NULL"));
			clause_checks.emplace_back(proof.source_pins[i].source_clause_id, std::move(clause_valid));
		}
		std::sort(clause_checks.begin(), clause_checks.end(),
		          [](const pair<idx_t, unique_ptr<Expression>> &left,
		             const pair<idx_t, unique_ptr<Expression>> &right) { return left.first < right.first; });
		unique_ptr<Expression> valid_bound = ConstantBool(true);
		for (idx_t i = clause_checks.size(); i > 0; i--) {
			valid_bound = make_uniq<BoundCaseExpression>(std::move(clause_checks[i - 1].second),
			                                         std::move(valid_bound), ConstantBool(false));
		}
		auto bound_guard = make_uniq<LogicalFilter>(std::move(valid_bound));
		bound_guard->children.push_back(std::move(bounds_ready));
		source = std::move(bound_guard);
	}
	vector<unique_ptr<Expression>> score_expressions;
	for (idx_t i = 0; i < source_bindings.size(); i++) {
		score_expressions.push_back(Column(source_types[i], input_bindings[i]));
	}
	unique_ptr<Expression> combined_score;
	if (proof.objective_parts.size() > 1 || proof.objective_parts[0].sign < 0) {
		combined_score = make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
	}
	for (auto &part : proof.objective_parts) {
		auto coefficient = part.coefficient->Copy();
		if (state_index != DConstants::INVALID_INDEX &&
		    !RemapSourceReferences(*coefficient, source_bindings, state_index)) {
			throw InternalException("S1 direct solve could not remap its proved coefficient");
		}
		auto term = BoundCastExpression::AddCastToType(optimizer.context, std::move(coefficient), LogicalType::DOUBLE);
		if (part.sign < 0) {
			term = optimizer.BindScalarFunction("*", std::move(term),
			                                    make_uniq<BoundConstantExpression>(Value::DOUBLE(-1.0)));
		}
		combined_score = combined_score ? optimizer.BindScalarFunction("+", std::move(combined_score), std::move(term))
		                                : std::move(term);
	}
	score_expressions.push_back(std::move(combined_score));
	if (proof.scoped) {
		score_expressions.push_back(Column(LogicalType::BOOLEAN, source_eligible_binding));
	}
	if (has_fixes) {
		score_expressions.push_back(Column(LogicalType::BOOLEAN, source_fixed_one_binding));
		score_expressions.push_back(Column(LogicalType::BOOLEAN, source_fixed_zero_binding));
	}
	for (idx_t i = 0; i < bound_slots.size(); i++) {
		auto &slots = bound_slots[i];
		if (slots.min_window != DConstants::INVALID_INDEX) {
			slots.min_score = score_expressions.size();
			score_expressions.push_back(Column(LogicalType::DOUBLE,
			                                   ColumnBinding(source_bound_window_index, slots.min_window)));
		}
		if (slots.max_window != DConstants::INVALID_INDEX) {
			slots.max_score = score_expressions.size();
			score_expressions.push_back(Column(LogicalType::DOUBLE,
			                                   ColumnBinding(source_bound_window_index, slots.max_window)));
		}
	}
	auto score = make_uniq<LogicalProjection>(score_index, std::move(score_expressions));
	score->children.push_back(std::move(source));
	auto score_binding = ColumnBinding(score_index, source_bindings.size());
	auto eligible_binding = ColumnBinding(score_index, source_bindings.size() + 1);
	auto fixed_one_binding = ColumnBinding(score_index, source_bindings.size() + 1 + (proof.scoped ? 1 : 0));
	auto fixed_zero_binding = ColumnBinding(score_index, source_bindings.size() + 2 + (proof.scoped ? 1 : 0));
	bool source_lower = false;
	bool source_upper = false;
	for (auto &bound : proof.source_bounds) {
		source_lower |= bound.comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
		                bound.comparison == ExpressionType::COMPARE_GREATERTHAN ||
		                bound.comparison == ExpressionType::COMPARE_EQUAL;
		source_upper |= bound.comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		                bound.comparison == ExpressionType::COMPARE_LESSTHAN ||
		                bound.comparison == ExpressionType::COMPARE_EQUAL;
	}
	auto has_lower = proof.lower || source_lower;
	auto has_upper = proof.has_upper || source_upper;
	auto source_limit = [&](idx_t index, bool upper) -> unique_ptr<Expression> {
		auto &bound = proof.source_bounds[index];
		auto &slots = bound_slots[index];
		auto slot = upper ? slots.min_score : slots.max_score;
		if (slot == DConstants::INVALID_INDEX) {
			throw InternalException("S1 direct solve lost its source-valued count limit");
		}
		auto cap = [&]() { return Column(LogicalType::DOUBLE, ColumnBinding(score_index, slot)); };
		bool strict = bound.comparison ==
		              (upper ? ExpressionType::COMPARE_LESSTHAN : ExpressionType::COMPARE_GREATERTHAN);
		auto rounded = optimizer.BindScalarFunction(upper == strict ? "ceil" : "floor", cap());
		unique_ptr<Expression> inclusive =
		    BoundCastExpression::AddCastToType(optimizer.context, std::move(rounded), LogicalType::BIGINT);
		if (strict) {
			inclusive = optimizer.BindScalarFunction(
			    upper ? "-" : "+", std::move(inclusive), make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
		}
		auto beyond_bigint = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO, cap(),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(9223372036854775808.0)));
		auto bounded = make_uniq<BoundCaseExpression>(
		    std::move(beyond_bigint),
		    make_uniq<BoundConstantExpression>(Value::BIGINT(std::numeric_limits<int64_t>::max())),
		    std::move(inclusive));
		auto below_zero = make_uniq<BoundComparisonExpression>(
		    strict == upper ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_LESSTHAN,
		    cap(), make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
		return make_uniq<BoundCaseExpression>(
		    std::move(below_zero), make_uniq<BoundConstantExpression>(Value::BIGINT(upper ? -1 : 0)),
		    std::move(bounded));
	};
	auto lower_limit = [&]() -> unique_ptr<Expression> {
		vector<unique_ptr<Expression>> limits;
		limits.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(proof.lower))));
		for (idx_t i = 0; i < bound_slots.size(); i++) {
			if (bound_slots[i].max_score != DConstants::INVALID_INDEX) {
				limits.push_back(source_limit(i, false));
			}
		}
		return limits.size() == 1 ? std::move(limits[0]) : optimizer.BindScalarFunction("greatest", std::move(limits));
	};
	auto upper_limit = [&]() -> unique_ptr<Expression> {
		vector<unique_ptr<Expression>> limits;
		if (proof.has_upper) {
			limits.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(proof.upper))));
		}
		for (idx_t i = 0; i < bound_slots.size(); i++) {
			if (bound_slots[i].min_score != DConstants::INVALID_INDEX) {
				limits.push_back(source_limit(i, true));
			}
		}
		if (limits.empty()) {
			return nullptr;
		}
		return limits.size() == 1 ? std::move(limits[0]) : optimizer.BindScalarFunction("least", std::move(limits));
	};
	auto add_scope = [&](BoundWindowExpression &expression) {
		if (!proof.scoped) {
			return;
		}
		expression.partitions.push_back(Column(LogicalType::BOOLEAN, eligible_binding));
		for (auto slot : proof.group_key_slots) {
			expression.partitions.push_back(Column(source_types[slot], ColumnBinding(score_index, slot)));
		}
	};
	auto free_row = [&]() -> unique_ptr<Expression> {
		return make_uniq<BoundCaseExpression>(
		    Column(LogicalType::BOOLEAN, fixed_one_binding), ConstantBool(false),
		    make_uniq<BoundCaseExpression>(Column(LogicalType::BOOLEAN, fixed_zero_binding), ConstantBool(false),
		                                     ConstantBool(true)));
	};
	auto add_order = [&](BoundWindowExpression &expression) {
		if (has_fixes) {
			expression.orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, free_row());
		}
		expression.orders.emplace_back(proof.sense == DecideSense::MAXIMIZE ? OrderType::DESCENDING : OrderType::ASCENDING,
		                               OrderByNullType::NULLS_LAST, Column(LogicalType::DOUBLE, score_binding));
	};
	auto guard = make_uniq<LogicalFilter>(ValidScorePredicate(optimizer, LogicalType::DOUBLE, score_binding));
	guard->children.push_back(std::move(score));
	auto rank = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
	rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
	rank->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	add_order(*rank);
	add_scope(*rank);
	auto window = make_uniq<LogicalWindow>(window_index);
	window->expressions.push_back(std::move(rank));
	idx_t fixed_count_slot = DConstants::INVALID_INDEX;
	idx_t free_count_slot = DConstants::INVALID_INDEX;
	if (has_fixes) {
		fixed_count_slot = window->expressions.size();
		auto count = WindowMatchingCount(Column(LogicalType::BOOLEAN, fixed_one_binding));
		add_order(*count);
		add_scope(*count);
		window->expressions.push_back(std::move(count));
		if (has_lower && !proof.impossible) {
			free_count_slot = window->expressions.size();
			auto free_count = WindowMatchingCount(free_row());
			add_order(*free_count);
			add_scope(*free_count);
			window->expressions.push_back(std::move(free_count));
		}
	} else if (has_lower && !proof.impossible) {
		// Match the rank's order so the full-partition count can share its sort.
		auto count = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
		                                              make_uniq<AggregateFunction>(CountStarFun::GetFunction()), nullptr);
		count->start = WindowBoundary::UNBOUNDED_PRECEDING;
		count->end = WindowBoundary::UNBOUNDED_FOLLOWING;
		add_order(*count);
		add_scope(*count);
		window->expressions.push_back(std::move(count));
	}
	window->children.push_back(std::move(guard));
	auto rank_binding = ColumnBinding(window_index, 0);
	unique_ptr<LogicalOperator> ranked = std::move(window);
	if (proof.impossible || has_lower || has_fixes || source_upper) {
		auto infeasible_error = [&]() {
			return ErrorPredicate(optimizer, "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE "
			                                 "to see which clause to change.");
		};
		unique_ptr<Expression> feasible_predicate = ConstantBool(true);
		if (proof.impossible) {
			auto has_row = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_GREATERTHANOREQUALTO, Column(LogicalType::BIGINT, rank_binding),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
			feasible_predicate = make_uniq<BoundCaseExpression>(std::move(has_row), infeasible_error(), ConstantBool(true));
		} else if (has_lower) {
			unique_ptr<Expression> available_rows =
			    has_fixes
			        ? optimizer.BindScalarFunction(
			              "+", Column(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)),
			              Column(LogicalType::BIGINT, ColumnBinding(window_index, free_count_slot)))
			        : Column(LogicalType::BIGINT, ColumnBinding(window_index, 1));
			auto enough_rows = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(available_rows),
			    lower_limit());
			feasible_predicate = make_uniq<BoundCaseExpression>(std::move(enough_rows), ConstantBool(true),
			                                                     infeasible_error());
		}
		if (!proof.impossible && (source_lower || source_upper) && has_upper) {
			auto interval_valid = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_LESSTHANOREQUALTO, lower_limit(), upper_limit());
			feasible_predicate = make_uniq<BoundCaseExpression>(std::move(interval_valid),
			                                                     std::move(feasible_predicate), infeasible_error());
		}
		if (!proof.impossible && source_lower) {
			for (auto &slots : bound_slots) {
				if (slots.max_score == DConstants::INVALID_INDEX) {
					continue;
				}
				auto too_large = make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
				    Column(LogicalType::DOUBLE, ColumnBinding(score_index, slots.max_score)),
				    make_uniq<BoundConstantExpression>(Value::DOUBLE(9223372036854775808.0)));
				feasible_predicate = make_uniq<BoundCaseExpression>(std::move(too_large), infeasible_error(),
				                                                     std::move(feasible_predicate));
			}
		}
		if (!proof.impossible && has_fixes && has_upper) {
			auto fixed_within_upper = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_LESSTHANOREQUALTO,
			    Column(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)),
			    upper_limit());
			feasible_predicate = make_uniq<BoundCaseExpression>(std::move(fixed_within_upper),
			                                                     std::move(feasible_predicate), infeasible_error());
		}
		if (proof.scoped) {
			feasible_predicate = make_uniq<BoundCaseExpression>(Column(LogicalType::BOOLEAN, eligible_binding),
			                                                     std::move(feasible_predicate), ConstantBool(true));
		}
		if (has_fixes) {
			auto conflict = make_uniq<BoundCaseExpression>(Column(LogicalType::BOOLEAN, fixed_one_binding),
			                                               Column(LogicalType::BOOLEAN, fixed_zero_binding),
			                                               ConstantBool(false));
			feasible_predicate = make_uniq<BoundCaseExpression>(std::move(conflict), infeasible_error(),
			                                                     std::move(feasible_predicate));
		}
		auto feasible = make_uniq<LogicalFilter>(std::move(feasible_predicate));
		feasible->children.push_back(std::move(ranked));
		ranked = std::move(feasible);
	}
	vector<unique_ptr<Expression>> result_expressions;
	for (idx_t i = 0; i < source_bindings.size(); i++) {
		result_expressions.push_back(Column(source_types[i], ColumnBinding(score_index, i)));
	}
	auto improving = make_uniq<BoundComparisonExpression>(
	    proof.sense == DecideSense::MAXIMIZE ? ExpressionType::COMPARE_GREATERTHAN
	                                                  : ExpressionType::COMPARE_LESSTHAN,
	    Column(LogicalType::DOUBLE, score_binding), make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
	auto free_choose = improving->Copy();
	unique_ptr<Expression> choose = std::move(improving);
	if (has_lower) {
		unique_ptr<Expression> remaining_lower = lower_limit();
		if (has_fixes) {
			remaining_lower = optimizer.BindScalarFunction(
			    "-", std::move(remaining_lower),
			    Column(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)));
		}
		auto within_lower = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHANOREQUALTO, Column(LogicalType::BIGINT, rank_binding),
		    std::move(remaining_lower));
		choose = make_uniq<BoundCaseExpression>(std::move(within_lower), ConstantBool(true), std::move(choose));
	}
	if (has_upper) {
		unique_ptr<Expression> remaining_upper = upper_limit();
		if (has_fixes) {
			remaining_upper = optimizer.BindScalarFunction(
			    "-", std::move(remaining_upper),
			    Column(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)));
		}
		auto within_upper = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHANOREQUALTO, Column(LogicalType::BIGINT, rank_binding),
		    std::move(remaining_upper));
		choose = make_uniq<BoundCaseExpression>(std::move(within_upper), std::move(choose), ConstantBool(false));
	}
	if (proof.scoped) {
		choose = make_uniq<BoundCaseExpression>(Column(LogicalType::BOOLEAN, eligible_binding), std::move(choose),
		                                        std::move(free_choose));
	}
	if (has_fixes) {
		choose = make_uniq<BoundCaseExpression>(Column(LogicalType::BOOLEAN, fixed_zero_binding), ConstantBool(false),
		                                        std::move(choose));
		choose = make_uniq<BoundCaseExpression>(Column(LogicalType::BOOLEAN, fixed_one_binding), ConstantBool(true),
		                                        std::move(choose));
	}
	result_expressions.push_back(make_uniq<BoundCaseExpression>(
	    std::move(choose), make_uniq<BoundConstantExpression>(Value::INTEGER(1)),
	    make_uniq<BoundConstantExpression>(Value::INTEGER(0))));
	// The otherwise unused rank is a blocking dependency. It forces a complete
	// value read before an outer LIMIT, COUNT, or column-pruning parent can return.
	result_expressions.push_back(Column(LogicalType::BIGINT, rank_binding));
	auto result = make_uniq<LogicalProjection>(result_index, std::move(result_expressions));
	result->children.push_back(std::move(ranked));
	DirectRelationalProposal proposal;
	proposal.prunable_outputs = std::move(prunable_outputs);
	proposal.required_slots.push_back(source_bindings.size() + 1); // hidden rank enforces the score guard
	for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
		proposal.output_slots.push_back(i);
	}
	proposal.child = std::move(result);
	return proposal;
}

unique_ptr<LogicalOperator> MapDirectResult(DirectRelationalProposal proposal, vector<ColumnBinding> output_bindings,
                                           vector<LogicalType> output_types, idx_t decide_index,
                                           DirectSolveDecisionRecord record) {
	if (!proposal.child || output_bindings.size() != output_types.size() ||
	    proposal.output_slots.size() != output_bindings.size() ||
	    proposal.prunable_outputs.size() != output_bindings.size() || proposal.required_slots.empty()) {
		throw InternalException("Direct solve rule returned an incomplete result proposal");
	}
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
	for (auto slot : proposal.required_slots) {
		if (slot >= child_types.size()) {
			throw InternalException("Direct solve rule lost a required validation slot");
		}
	}
	auto boundary = make_uniq<LogicalDirectSolveResult>(decide_index, std::move(output_bindings),
	                                                   std::move(output_types), proposal.required_slots.size(),
	                                                   std::move(proposal.prunable_outputs), std::move(record));
	// The positional map is materialized as one dependency per advertised output.
	// The required suffix stays live even when all outputs are unused by a parent.
	for (auto slot : proposal.output_slots) {
		boundary->expressions.push_back(Column(child_types[slot], child_bindings[slot]));
	}
	for (auto slot : proposal.required_slots) {
		boundary->expressions.push_back(Column(child_types[slot], child_bindings[slot]));
	}
	boundary->children.push_back(std::move(proposal.child));
	return std::move(boundary);
}

} // namespace

vector<unique_ptr<DirectSolveRule>> RegisteredDirectRules() {
	vector<unique_ptr<DirectSolveRule>> rules;
	rules.push_back(make_uniq<S1CardinalityRule>());
	return rules;
}

void RegisterDirectSolve(DBConfig &config) {
	config.AddExtensionOption("decide_direct_solve", "DECIDE direct solve mode: off, auto, or require",
	                          LogicalType::VARCHAR, Value("off"), DirectSolveModeSetCallback);
	config.operator_extensions.push_back(make_uniq<DirectResultExtension>());
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
