// src/planner/operator/logical_decide.cpp
#include "duckdb/planner/operator/logical_decide.hpp"

#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

LogicalDecide::LogicalDecide(idx_t decide_index, vector<unique_ptr<Expression>> decide_variables,
                             unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                             unique_ptr<Expression> decide_objective)
    : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE), decide_index(decide_index),
      decide_variables(std::move(decide_variables)), decide_constraints(std::move(decide_constraints)),
      decide_sense(decide_sense), decide_objective(std::move(decide_objective)) {
}

LogicalDecide::LogicalDecide() : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE) {
}

vector<ColumnBinding> LogicalDecide::GetColumnBindings() {
    // Return all child columns plus ALL decide variables (including auxiliary).
    // Auxiliary vars (e.g. from ABS linearization) must be visible for column binding
    // resolution in constraint/objective expressions. The projection above prunes them.
    auto result = children[0]->GetColumnBindings();
    for (idx_t i = 0; i < decide_variables.size(); i++) {
        result.emplace_back(decide_index, i);
    }
    return result;
}

void LogicalDecide::ResolveTypes() {
    types = children[0]->types;
    // Include ALL decide variable types (user + auxiliary).
    // Auxiliary vars are pruned by the projection operator above.
    for (idx_t i = 0; i < decide_variables.size(); i++) {
        types.push_back(decide_variables[i]->return_type);
    }
}

vector<idx_t> LogicalDecide::GetTableIndex() const {
	return vector<idx_t> {decide_index};
}

string LogicalDecide::GetName() const {
	return "DECIDE";
}

void LogicalDecide::AddConstraint(ClientContext &context, unique_ptr<Expression> constraint) {
	DecideCanonicalizer canonicalizer(context, decide_index, variable_scopes);
	auto canonical = canonicalizer.CanonicalizeTree(*constraint);
	canonicalizer.VerifyCanonical(*canonical);

	if (!decide_constraints) {
		decide_constraints = std::move(canonical);
	} else {
		auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		conj->children.push_back(std::move(decide_constraints));
		conj->children.push_back(std::move(canonical));
		decide_constraints = std::move(conj);
	}
}

void LogicalDecide::SetObjective(ClientContext &context, unique_ptr<Expression> objective) {
	if (!objective) {
		decide_objective = nullptr;
		return;
	}
	DecideCanonicalizer canonicalizer(context, decide_index, variable_scopes);
	decide_objective = canonicalizer.CanonicalizeObjective(*objective, objective_constant_offset);
	canonicalizer.VerifyCanonicalObjective(*decide_objective);
}

void LogicalDecide::EnumerateExpressions(const std::function<void(unique_ptr<Expression> *)> &callback) {
	for (auto &expr : decide_variables) {
		callback(&expr);
	}
	if (decide_constraints) {
		callback(&decide_constraints);
	}
	if (decide_objective) {
		callback(&decide_objective);
	}
	// The composed MIN/MAX rewrite (DecideOptimizer) lifts sub-expressions out of the
	// objective/constraint trees into these vectors and leaves a placeholder behind, so
	// visiting decide_objective/decide_constraints above no longer reaches them.
	for (auto &term : composed_minmax_objective_terms) {
		if (term.inner_expr) {
			callback(&term.inner_expr);
		}
		if (term.filter) {
			callback(&term.filter);
		}
		if (term.scale) {
			callback(&term.scale);
		}
	}
	for (auto &spec : composed_minmax_constraints) {
		for (auto &term : spec.terms) {
			if (term.inner_expr) {
				callback(&term.inner_expr);
			}
			if (term.filter) {
				callback(&term.filter);
			}
			if (term.scale) {
				callback(&term.scale);
			}
		}
		if (spec.rhs_expr) {
			callback(&spec.rhs_expr);
		}
	}
	for (auto &expr : entity_key_expressions) {
		if (expr) {
			callback(&expr);
		}
	}
}

InsertionOrderPreservingMap<string> LogicalDecide::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;

	// Variables (exclude auxiliary variables)
	string vars_info;
	idx_t user_var_count = decide_variables.size() - num_auxiliary_vars;
	for (idx_t i = 0; i < user_var_count; i++) {
		if (i > 0) {
			vars_info += "\n";
		}
		vars_info += decide_variables[i]->GetName();
	}
	result["Variables"] = vars_info;

	// Objective: render through the same tagged-expression walker as constraints so
	// WHEN/PER wrappers print as postfix suffixes (e.g. "SUM(x) WHEN c") rather than
	// the generic conjunction form "(SUM(x) AND c)".
	if (decide_objective) {
		string obj_info = (decide_sense == DecideSense::MAXIMIZE) ? "MAXIMIZE " : "MINIMIZE ";
		vector<string> objective_strs;
		CollectDecideExpressionStrings(*decide_objective, source_fragments, entity_scopes, objective_strs);
		for (idx_t i = 0; i < objective_strs.size(); i++) {
			if (i > 0) {
				obj_info += "\n";
			}
			obj_info += objective_strs[i];
		}
		result["Objective"] = obj_info;
	} else {
		result["Objective"] = "FEASIBILITY";
	}

	// Constraints: walk the AND-tree and collect individual constraints
	if (decide_constraints) {
		vector<string> constraint_strs;
		CollectDecideExpressionStrings(*decide_constraints, source_fragments, entity_scopes, constraint_strs);
		string constraints_info;
		for (idx_t i = 0; i < constraint_strs.size(); i++) {
			if (i > 0) {
				constraints_info += "\n";
			}
			constraints_info += constraint_strs[i];
		}
		result["Constraints"] = constraints_info;
	}

	SetParamsEstimatedCardinality(result);
	return result;
}

// Marked "custom_implementation": true in logical_operator.json, so
// scripts/generate_serialization.py skips this class entirely (same as
// LogicalGet, LogicalCopyToFile, ...) and hand-maintained code here is the
// only definition. entity_scopes (vector<EntityScopeInfo>) and
// variable_scopes (vector<DecideVarScopeInfo>) are structs the generator's
// direct member-mapping can't express, so they're flattened into parallel
// primitive vectors on the wire; ids 200-238 (below) are the single
// authoritative registry of this operator's serialized fields.
void LogicalDecide::Serialize(Serializer &serializer) const {
	LogicalOperator::Serialize(serializer);
	serializer.WritePropertyWithDefault<idx_t>(200, "decide_index", decide_index);
	serializer.WritePropertyWithDefault<vector<unique_ptr<Expression>>>(201, "decide_variables", decide_variables);
	serializer.WritePropertyWithDefault<unique_ptr<Expression>>(202, "decide_constraints", decide_constraints);
	serializer.WriteProperty<DecideSense>(203, "decide_sense", decide_sense);
	serializer.WritePropertyWithDefault<unique_ptr<Expression>>(204, "decide_objective", decide_objective);
	serializer.WritePropertyWithDefault<idx_t>(205, "num_auxiliary_vars", num_auxiliary_vars);
	serializer.WritePropertyWithDefault<vector<idx_t>>(207, "ne_indicator_indices", ne_indicator_indices);
	serializer.WritePropertyWithDefault<vector<string>>(208, "minmax_clause_labels", minmax_clause_labels);
	// Serialize bilinear_links as three parallel vectors (aux_idx, bool_var_idx, other_var_idx)
	{
		vector<idx_t> bl_aux, bl_bool, bl_other;
		for (auto &link : bilinear_links) {
			bl_aux.push_back(link.aux_idx);
			bl_bool.push_back(link.bool_var_idx);
			bl_other.push_back(link.other_var_idx);
		}
		serializer.WritePropertyWithDefault<vector<idx_t>>(225, "bilinear_link_aux", bl_aux);
		serializer.WritePropertyWithDefault<vector<idx_t>>(226, "bilinear_link_bool", bl_bool);
		serializer.WritePropertyWithDefault<vector<idx_t>>(227, "bilinear_link_other", bl_other);
	}
	serializer.WritePropertyWithDefault<vector<bool>>(228, "is_boolean_var", is_boolean_var);
	serializer.WritePropertyWithDefault<double>(229, "objective_constant_offset", objective_constant_offset);
	{
		vector<idx_t> am_aux, am_y;
		for (auto &l : abs_maximize_links) {
			am_aux.push_back(l.aux_idx);
			am_y.push_back(l.y_idx);
		}
		serializer.WritePropertyWithDefault<vector<idx_t>>(230, "abs_maximize_link_aux", am_aux);
		serializer.WritePropertyWithDefault<vector<idx_t>>(231, "abs_maximize_link_y", am_y);
	}
	serializer.WritePropertyWithDefault<vector<pair<idx_t, string>>>(232, "aux_var_expressions", aux_var_expressions);
	serializer.WritePropertyWithDefault<uint8_t>(209, "flat_objective_agg", static_cast<uint8_t>(flat_objective_agg));
	serializer.WritePropertyWithDefault<bool>(210, "flat_objective_is_easy", flat_objective_is_easy);
	serializer.WritePropertyWithDefault<uint8_t>(211, "per_inner_agg", static_cast<uint8_t>(per_inner_agg));
	serializer.WritePropertyWithDefault<uint8_t>(212, "per_outer_agg", static_cast<uint8_t>(per_outer_agg));
	serializer.WritePropertyWithDefault<bool>(213, "per_inner_is_easy", per_inner_is_easy);
	serializer.WritePropertyWithDefault<bool>(214, "per_outer_is_easy", per_outer_is_easy);
	serializer.WritePropertyWithDefault<bool>(215, "per_inner_was_avg", per_inner_was_avg);
	// Table-scoped / query-wide variable metadata. Flattened into parallel
	// vectors: 216 keeps its original meaning (entity index, INVALID for
	// non-entity vars) and 233 adds the scope kind.
	vector<idx_t> var_scope_entity_indices;
	vector<uint8_t> var_scope_kinds;
	for (auto &vs : variable_scopes) {
		var_scope_entity_indices.push_back(vs.entity_scope_idx);
		var_scope_kinds.push_back(static_cast<uint8_t>(vs.scope));
	}
	serializer.WritePropertyWithDefault<vector<idx_t>>(216, "variable_entity_scope", var_scope_entity_indices);
	serializer.WritePropertyWithDefault<vector<uint8_t>>(233, "variable_scope_kinds", var_scope_kinds);
	// Flatten entity_scopes into parallel vectors for serialization
	idx_t num_scopes = entity_scopes.size();
	serializer.WritePropertyWithDefault<idx_t>(217, "num_entity_scopes", num_scopes);
	vector<string> scope_aliases;
	vector<idx_t> scope_table_indices;
	vector<idx_t> scope_binding_counts; // how many bindings per scope
	vector<idx_t> scope_binding_tables; // flattened table_index from each ColumnBinding
	vector<idx_t> scope_binding_cols;   // flattened column_index from each ColumnBinding
	vector<idx_t> scope_var_counts;     // how many scoped variables per scope
	vector<idx_t> scope_var_indices;    // flattened scoped_variable_indices
	for (auto &scope : entity_scopes) {
		scope_aliases.push_back(scope.table_alias);
		scope_table_indices.push_back(scope.source_table_index);
		scope_binding_counts.push_back(scope.entity_key_bindings.size());
		for (auto &b : scope.entity_key_bindings) {
			scope_binding_tables.push_back(b.table_index);
			scope_binding_cols.push_back(b.column_index);
		}
		scope_var_counts.push_back(scope.scoped_variable_indices.size());
		for (auto &vi : scope.scoped_variable_indices) {
			scope_var_indices.push_back(vi);
		}
	}
	serializer.WritePropertyWithDefault<vector<string>>(218, "scope_aliases", scope_aliases);
	serializer.WritePropertyWithDefault<vector<idx_t>>(219, "scope_table_indices", scope_table_indices);
	serializer.WritePropertyWithDefault<vector<idx_t>>(220, "scope_binding_counts", scope_binding_counts);
	serializer.WritePropertyWithDefault<vector<idx_t>>(221, "scope_binding_tables", scope_binding_tables);
	serializer.WritePropertyWithDefault<vector<idx_t>>(222, "scope_binding_cols", scope_binding_cols);
	serializer.WritePropertyWithDefault<vector<idx_t>>(223, "scope_var_counts", scope_var_counts);
	serializer.WritePropertyWithDefault<vector<idx_t>>(224, "scope_var_indices", scope_var_indices);
	vector<string> source_lhs;
	vector<string> source_rhs;
	vector<string> source_qualifiers;
	vector<uint8_t> source_rhs_kinds;
	for (auto &source : constraint_sources) {
		source_lhs.push_back(source.canonical_lhs);
		source_rhs.push_back(source.canonical_rhs);
		source_qualifiers.push_back(source.qualifier);
		source_rhs_kinds.push_back(static_cast<uint8_t>(source.rhs_kind));
	}
	serializer.WritePropertyWithDefault<vector<string>>(234, "constraint_source_lhs", source_lhs);
	serializer.WritePropertyWithDefault<vector<string>>(235, "constraint_source_rhs", source_rhs);
	serializer.WritePropertyWithDefault<vector<string>>(236, "constraint_source_qualifiers", source_qualifiers);
	serializer.WritePropertyWithDefault<vector<uint8_t>>(237, "constraint_source_rhs_kinds", source_rhs_kinds);
	serializer.WritePropertyWithDefault<vector<string>>(238, "source_fragments", source_fragments);
}

unique_ptr<LogicalOperator> LogicalDecide::Deserialize(Deserializer &deserializer) {
	auto result = duckdb::unique_ptr<LogicalDecide>(new LogicalDecide());
	deserializer.ReadPropertyWithDefault<idx_t>(200, "decide_index", result->decide_index);
	deserializer.ReadPropertyWithDefault<vector<unique_ptr<Expression>>>(201, "decide_variables", result->decide_variables);
	deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(202, "decide_constraints", result->decide_constraints);
	deserializer.ReadProperty<DecideSense>(203, "decide_sense", result->decide_sense);
	deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(204, "decide_objective", result->decide_objective);
	deserializer.ReadPropertyWithDefault<idx_t>(205, "num_auxiliary_vars", result->num_auxiliary_vars);
	deserializer.ReadPropertyWithDefault<vector<idx_t>>(207, "ne_indicator_indices", result->ne_indicator_indices);
	deserializer.ReadPropertyWithDefault<vector<string>>(208, "minmax_clause_labels", result->minmax_clause_labels);
	// Deserialize bilinear_links from three parallel vectors
	{
		vector<idx_t> bl_aux, bl_bool, bl_other;
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(225, "bilinear_link_aux", bl_aux);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(226, "bilinear_link_bool", bl_bool);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(227, "bilinear_link_other", bl_other);
		for (idx_t i = 0; i < bl_aux.size(); i++) {
			LogicalDecide::BilinearLink link;
			link.aux_idx = bl_aux[i];
			link.bool_var_idx = bl_bool[i];
			link.other_var_idx = bl_other[i];
			result->bilinear_links.push_back(link);
		}
	}
	deserializer.ReadPropertyWithDefault<vector<bool>>(228, "is_boolean_var", result->is_boolean_var);
	deserializer.ReadPropertyWithDefault<double>(229, "objective_constant_offset", result->objective_constant_offset);
	// Deserialize abs_maximize_links from two parallel vectors
	{
		vector<idx_t> am_aux, am_y;
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(230, "abs_maximize_link_aux", am_aux);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(231, "abs_maximize_link_y", am_y);
		D_ASSERT(am_aux.size() == am_y.size());
		for (idx_t i = 0; i < am_aux.size(); i++) {
			result->abs_maximize_links.push_back({am_aux[i], am_y[i]});
		}
	}
	deserializer.ReadPropertyWithDefault<vector<pair<idx_t, string>>>(232, "aux_var_expressions", result->aux_var_expressions);
	uint8_t flat_agg_val = 0;
	deserializer.ReadPropertyWithDefault<uint8_t>(209, "flat_objective_agg", flat_agg_val);
	result->flat_objective_agg = static_cast<ObjectiveAggregateType>(flat_agg_val);
	deserializer.ReadPropertyWithDefault<bool>(210, "flat_objective_is_easy", result->flat_objective_is_easy);
	uint8_t inner_agg_val = 0;
	deserializer.ReadPropertyWithDefault<uint8_t>(211, "per_inner_agg", inner_agg_val);
	result->per_inner_agg = static_cast<ObjectiveAggregateType>(inner_agg_val);
	uint8_t outer_agg_val = 0;
	deserializer.ReadPropertyWithDefault<uint8_t>(212, "per_outer_agg", outer_agg_val);
	result->per_outer_agg = static_cast<ObjectiveAggregateType>(outer_agg_val);
	deserializer.ReadPropertyWithDefault<bool>(213, "per_inner_is_easy", result->per_inner_is_easy);
	deserializer.ReadPropertyWithDefault<bool>(214, "per_outer_is_easy", result->per_outer_is_easy);
	deserializer.ReadPropertyWithDefault<bool>(215, "per_inner_was_avg", result->per_inner_was_avg);
	// Table-scoped / query-wide variable metadata (see Serialize for the layout).
	vector<idx_t> var_scope_entity_indices;
	vector<uint8_t> var_scope_kinds;
	deserializer.ReadPropertyWithDefault<vector<idx_t>>(216, "variable_entity_scope", var_scope_entity_indices);
	deserializer.ReadPropertyWithDefault<vector<uint8_t>>(233, "variable_scope_kinds", var_scope_kinds);
	result->variable_scopes.resize(var_scope_entity_indices.size());
	for (idx_t i = 0; i < var_scope_entity_indices.size(); i++) {
		auto &vs = result->variable_scopes[i];
		vs.entity_scope_idx = var_scope_entity_indices[i];
		// Plans written before 233 existed carry only the entity index, where
		// INVALID meant row-scoped and anything else meant entity-scoped.
		vs.scope = i < var_scope_kinds.size()
		               ? static_cast<DecideVarScope>(var_scope_kinds[i])
		               : (vs.entity_scope_idx == DConstants::INVALID_INDEX ? DecideVarScope::ROW
		                                                                  : DecideVarScope::ENTITY);
	}
	idx_t num_scopes = 0;
	deserializer.ReadPropertyWithDefault<idx_t>(217, "num_entity_scopes", num_scopes);
	if (num_scopes > 0) {
		vector<string> scope_aliases;
		vector<idx_t> scope_table_indices;
		vector<idx_t> scope_binding_counts;
		vector<idx_t> scope_binding_tables;
		vector<idx_t> scope_binding_cols;
		vector<idx_t> scope_var_counts;
		vector<idx_t> scope_var_indices;
		deserializer.ReadPropertyWithDefault<vector<string>>(218, "scope_aliases", scope_aliases);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(219, "scope_table_indices", scope_table_indices);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(220, "scope_binding_counts", scope_binding_counts);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(221, "scope_binding_tables", scope_binding_tables);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(222, "scope_binding_cols", scope_binding_cols);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(223, "scope_var_counts", scope_var_counts);
		deserializer.ReadPropertyWithDefault<vector<idx_t>>(224, "scope_var_indices", scope_var_indices);
		// Reconstruct EntityScopeInfo objects
		idx_t binding_offset = 0;
		idx_t var_offset = 0;
		for (idx_t s = 0; s < num_scopes; s++) {
			EntityScopeInfo scope;
			scope.table_alias = scope_aliases[s];
			scope.source_table_index = scope_table_indices[s];
			idx_t num_bindings = scope_binding_counts[s];
			for (idx_t b = 0; b < num_bindings; b++) {
				scope.entity_key_bindings.emplace_back(
				    scope_binding_tables[binding_offset + b],
				    scope_binding_cols[binding_offset + b]);
			}
			binding_offset += num_bindings;
			idx_t num_vars = scope_var_counts[s];
			for (idx_t v = 0; v < num_vars; v++) {
				scope.scoped_variable_indices.push_back(scope_var_indices[var_offset + v]);
			}
			var_offset += num_vars;
			result->entity_scopes.push_back(std::move(scope));
		}
	}
	vector<string> source_lhs;
	vector<string> source_rhs;
	vector<string> source_qualifiers;
	vector<uint8_t> source_rhs_kinds;
	deserializer.ReadPropertyWithDefault<vector<string>>(234, "constraint_source_lhs", source_lhs);
	deserializer.ReadPropertyWithDefault<vector<string>>(235, "constraint_source_rhs", source_rhs);
	deserializer.ReadPropertyWithDefault<vector<string>>(236, "constraint_source_qualifiers", source_qualifiers);
	deserializer.ReadPropertyWithDefault<vector<uint8_t>>(237, "constraint_source_rhs_kinds", source_rhs_kinds);
	if (!source_lhs.empty()) {
		idx_t count = source_lhs.size();
		if (source_rhs.size() != count || source_qualifiers.size() != count || source_rhs_kinds.size() != count) {
			throw SerializationException("LogicalDecide constraint source provenance has inconsistent field lengths");
		}
		for (idx_t i = 0; i < count; i++) {
			if (source_rhs_kinds[i] > static_cast<uint8_t>(ConstraintSourceRhsKind::DATA_EXPRESSION)) {
				throw SerializationException("LogicalDecide constraint source provenance contains an invalid RHS kind");
			}
			ConstraintSourceInfo info;
			info.canonical_lhs = std::move(source_lhs[i]);
			info.canonical_rhs = std::move(source_rhs[i]);
			info.qualifier = std::move(source_qualifiers[i]);
			info.rhs_kind = static_cast<ConstraintSourceRhsKind>(source_rhs_kinds[i]);
			result->constraint_sources.push_back(std::move(info));
		}
	}
	deserializer.ReadPropertyWithDefault<vector<string>>(238, "source_fragments", result->source_fragments);
	return std::move(result);
}

} // namespace duckdb
