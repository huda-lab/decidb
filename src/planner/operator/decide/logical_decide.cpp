// src/planner/operator/logical_decide.cpp
#include "duckdb/planner/operator/decide/logical_decide.hpp"

#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"

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
		for (auto &column : spec.class_key) {
			callback(&column);
		}
		if (spec.when) {
			callback(&spec.when);
		}
		if (spec.rhs_expr) {
			callback(&spec.rhs_expr);
		}
	}
	// The value rewrite lifts each reducer it replaces out of the constraint tree too.
	for (auto &link : reducer_value_links) {
		for (auto *expr : {&link.inner_expr, &link.filter, &link.when}) {
			if (*expr) {
				callback(expr);
			}
		}
	}
	for (auto &expr : entity_key_expressions) {
		if (expr) {
			callback(&expr);
		}
	}
	for (auto &group_key : group_keys) {
		for (auto &expr : group_key.expressions) {
			callback(&expr);
		}
	}
	for (auto &frame : frames) {
		for (auto *expr : {&frame.fallback, &frame.order, &frame.rows}) {
			if (*expr) {
				callback(expr);
			}
		}
		for (auto &expr : frame.within) {
			callback(&expr);
		}
	}
	for (auto &check : scope_checks) {
		for (auto *expr : {&check.rows, &check.filter}) {
			if (*expr) {
				callback(expr);
			}
		}
		for (auto &expr : check.key) {
			callback(&expr);
		}
	}
	for (auto &signature : signatures) {
		if (signature.rows) {
			callback(&signature.rows);
		}
		for (auto &expr : signature.key) {
			callback(&expr);
		}
		for (auto &field : signature.fields) {
			if (field.value) {
				callback(&field.value);
			}
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

	// The objective and the constraints, through the one clause walker, so `when`/`per`
	// wrappers print as written rather than as a generic conjunction. What the optimizer
	// made of them: a column that stands for an expression prints it, and each rewrite's
	// account follows its clause, the objective's its own.
	auto names = DecidePlanNames(source_fragments, entity_scopes, group_keys, decide_index, aux_var_expressions);
	vector<string> objective_accounts;
	for (auto &account : rewrite_accounts) {
		if (account.source_clause_id == DConstants::INVALID_INDEX) {
			objective_accounts.push_back(RenderDecideRewriteAccount(account));
		}
	}
	// Constraints first: they also hand back the rows the rewrites added for the objective.
	vector<string> unattributed;
	vector<string> objective_rows;
	string constraints_text;
	if (decide_constraints || !constraint_sources.empty()) {
		auto layers = CollectDecideClauseLayers(decide_constraints.get(), names, constraint_sources, unattributed,
		                                        objective_rows, rewrite_accounts);
		constraints_text = RenderDecideClauseLayers(layers, unattributed);
	}
	if (decide_objective) {
		string sense_prefix = (decide_sense == DecideSense::MAXIMIZE) ? "MAXIMIZE " : "MINIMIZE ";
		vector<string> objective_strs;
		CollectDecideExpressionStrings(*decide_objective, names, objective_strs, nullptr, nullptr);
		// A MIN/MAX objective leaves a carrier in the tree -- a SUM standing for the
		// extremum, or the composed objective's 0.0 placeholder -- that its accounts
		// describe and a row would misread. An AVG's sum stays, under its account, as in a
		// constraint.
		if (flat_objective_agg != ObjectiveAggregateType::NONE || !composed_minmax_objective_terms.empty()) {
			objective_strs.clear();
		}
		result["Objective"] = RenderDecideObjectiveLayers(sense_prefix, written_objective, canonical_objective,
		                                                  objective_strs, objective_accounts, objective_rows);
	} else {
		result["Objective"] = "FEASIBILITY";
	}
	if (decide_constraints || !constraint_sources.empty()) {
		result["Constraints"] = constraints_text;
	}

	SetParamsEstimatedCardinality(result);
	return result;
}

} // namespace duckdb
