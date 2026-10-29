// src/planner/operator/logical_decide.cpp
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"

namespace duckdb {

DecideKeyElement DecideKeyElement::Copy() const {
	DecideKeyElement result;
	result.expression = expression->Copy();
	result.name = name;
	result.null_mode = null_mode;
	if (collated) {
		result.collated = collated->Copy();
	}
	return result;
}

//! Whether `expr` holds a bound subquery, which equals nothing, not even itself.
static bool HoldsSubquery(const Expression &expr) {
	bool found = expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY;
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { found = found || HoldsSubquery(child); });
	return found;
}

//! The written column wins over the same column a relation element stands for: its name,
//! and its NULL mode, which is no group (NO_GROUP) rather than a value (VALUE).
static bool WrittenWins(const DecideKeyElement &candidate, const DecideKeyElement &kept) {
	return candidate.null_mode == DecideKeyNullMode::NO_GROUP && kept.null_mode == DecideKeyNullMode::VALUE;
}

//! One order on key elements, so a key's elements are the same list however it was
//! written: columns by binding, then any other expression by its text.
static bool ElementBefore(const DecideKeyElement &a, const DecideKeyElement &b) {
	auto is_column = [](const DecideKeyElement &element) {
		return element.expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
	};
	if (is_column(a) != is_column(b)) {
		return is_column(a);
	}
	if (is_column(a)) {
		auto &left = a.expression->Cast<BoundColumnRefExpression>().binding;
		auto &right = b.expression->Cast<BoundColumnRefExpression>().binding;
		return left.table_index != right.table_index ? left.table_index < right.table_index
		                                             : left.column_index < right.column_index;
	}
	return a.expression->ToString() < b.expression->ToString();
}

idx_t FindOrCreateDecideKey(vector<DecideKeyElement> elements, const string &text, vector<DecideKey> &keys) {
	vector<DecideKeyElement> unique;
	for (auto &element : elements) {
		auto kept = std::find_if(unique.begin(), unique.end(), [&](const DecideKeyElement &member) {
			return member.expression->Equals(*element.expression);
		});
		if (kept == unique.end()) {
			unique.push_back(std::move(element));
		} else if (WrittenWins(element, *kept)) {
			*kept = std::move(element);
		}
	}
	if (unique.empty()) {
		return DConstants::INVALID_INDEX;
	}
	std::stable_sort(unique.begin(), unique.end(), ElementBefore);
	auto holds = [](const vector<DecideKeyElement> &set, const DecideKeyElement &element) {
		for (auto &member : set) {
			if (member.null_mode == element.null_mode && member.expression->Equals(*element.expression)) {
				return true;
			}
		}
		return false;
	};
	bool subquery = false;
	for (auto &element : unique) {
		subquery = subquery || HoldsSubquery(*element.expression);
	}
	for (idx_t i = 0; i < keys.size(); i++) {
		auto &existing = keys[i].elements;
		bool equal = existing.size() == unique.size();
		for (auto &element : unique) {
			equal = equal && holds(existing, element);
		}
		// A key holding a subquery is recognised by its text: written the same way in one
		// query, it is the same key.
		if (equal || (subquery && keys[i].text == text)) {
			return i;
		}
	}
	DecideKey key;
	key.elements = std::move(unique);
	key.text = text;
	keys.push_back(std::move(key));
	return keys.size() - 1;
}

const vector<DecideKeyElement> &DecideKeyElements(const vector<DecideKey> &keys, idx_t key) {
	static const vector<DecideKeyElement> no_elements;
	return key == DConstants::INVALID_INDEX ? no_elements : keys[key].elements;
}

string DecideKeyText(const vector<DecideKey> &keys, idx_t key) {
	return key == DConstants::INVALID_INDEX ? string("()") : keys[key].text;
}

vector<string> DecideKey::Names() const {
	vector<string> names;
	for (auto &element : elements) {
		names.push_back(element.name);
	}
	return names;
}

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
	DecideCanonicalizer canonicalizer(context, decide_index, variable_scopes, entity_scopes, is_boolean_var);
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
	DecideCanonicalizer canonicalizer(context, decide_index, variable_scopes, entity_scopes, is_boolean_var);
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
		if (spec.scope.when) {
			callback(&spec.scope.when);
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
	// A key's columns are live even where no clause reads them: a decision keyed by a
	// column the query never mentions still needs it in the scan.
	for (auto &key : keys) {
		for (auto &element : key.elements) {
			callback(&element.expression);
			if (element.collated) {
				callback(&element.collated);
			}
		}
	}
	for (auto &check : scope_checks) {
		for (auto *expr : {&check.rows, &check.value, &check.filter}) {
			if (*expr) {
				callback(expr);
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
	auto names = DecidePlanNames(source_fragments, entity_scopes, keys, decide_index, aux_var_expressions);
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
		// extremum, or what the composed objective keeps (its 0.0 placeholder or the terms
		// beside its reducers) -- that its accounts describe and a row would misread. An AVG's sum stays, under its
		// account, as in a constraint.
		if (flat_objective_agg != ObjectiveAggregateType::NONE || !composed_minmax_objective_terms.empty() ||
		    per_inner_agg == ObjectiveAggregateType::MIN_AGG || per_inner_agg == ObjectiveAggregateType::MAX_AGG) {
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
