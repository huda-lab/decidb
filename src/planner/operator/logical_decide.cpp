// src/planner/operator/logical_decide.cpp
#include "duckdb/planner/operator/logical_decide.hpp"

#include "duckdb/decidb/utility/debug.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

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

void LogicalDecide::CollectTaggedExpressionStrings(const Expression &expr, vector<string> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
			// PER wrapper: child[0] is the constraint, children[1..N] are PER columns
			string per_suffix = " PER ";
			bool parenthesize = conj.children.size() > 2;
			if (parenthesize) {
				per_suffix += "(";
			}
			for (idx_t i = 1; i < conj.children.size(); i++) {
				if (i > 1) {
					per_suffix += ", ";
				}
				per_suffix += conj.children[i]->GetName();
			}
			if (parenthesize) {
				per_suffix += ")";
			}
			// Recurse into child[0]; append PER suffix to each leaf
			vector<string> inner;
			CollectTaggedExpressionStrings(*conj.children[0], inner);
			for (auto &s : inner) {
				out.push_back(s + per_suffix);
			}
			return;
		}
		if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
			// WHEN wrapper: child[0] is the constraint, child[1] is the condition
			string when_suffix = " WHEN " + conj.children[1]->GetName();
			vector<string> inner;
			CollectTaggedExpressionStrings(*conj.children[0], inner);
			for (auto &s : inner) {
				out.push_back(s + when_suffix);
			}
			return;
		}
		// Regular AND conjunction: recurse on each child
		for (auto &child : conj.children) {
			CollectTaggedExpressionStrings(*child, out);
		}
		return;
	}
	// Leaf node (comparison or other): use ToString for the full expression
	out.push_back(expr.GetName());
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
		CollectTaggedExpressionStrings(*decide_objective, objective_strs);
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
		CollectTaggedExpressionStrings(*decide_constraints, constraint_strs);
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

} // namespace duckdb
