#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

namespace {

struct ConstraintScope {
	vector<const Expression *> per_keys;
	const Expression *when_condition = nullptr;
};

bool CollectConstraints(const Expression &expression, idx_t source_count, vector<DirectConstraintFactor> &factors,
                        const ConstraintScope &scope) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		idx_t source_id = DConstants::INVALID_INDEX;
		auto known = TryParseSourceClauseTag(expression.GetAlias(), source_id) && source_id < source_count;
		factors.push_back({&expression, known ? DirectFactStatus::KNOWN : DirectFactStatus::UNKNOWN, source_id,
		                   scope.per_keys, scope.when_condition});
		return true;
	}
	auto &conjunction = expression.Cast<BoundConjunctionExpression>();
	if (conjunction.type != ExpressionType::CONJUNCTION_AND || conjunction.children.empty()) {
		return false;
	}
	if (IsPerConstraintWrapper(conjunction)) {
		if (!scope.per_keys.empty() || conjunction.children.size() < 2) {
			return false;
		}
		ConstraintScope nested = scope;
		for (idx_t i = 1; i < conjunction.children.size(); i++) {
			if (conjunction.children[i]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return false;
			}
			nested.per_keys.push_back(conjunction.children[i].get());
		}
		return CollectConstraints(*conjunction.children[0], source_count, factors, nested);
	}
	if (IsWhenConstraintWrapper(conjunction)) {
		if (scope.when_condition || conjunction.children.size() != 2) {
			return false;
		}
		ConstraintScope nested = scope;
		nested.when_condition = conjunction.children[1].get();
		return CollectConstraints(*conjunction.children[0], source_count, factors, nested);
	}
	for (auto &child : conjunction.children) {
		if (!CollectConstraints(*child, source_count, factors, scope)) {
			return false;
		}
	}
	return true;
}

bool CollectObjective(const Expression &expression, idx_t decide_index, int sign,
                      vector<DirectObjectiveTerm> &terms) {
	auto root = UnwrapDecideCasts(expression, decide_index);
	if (root->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		// WHEN/PER/qualifier wrappers need their own exact semantic representation.
		return false;
	}
	if (root->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = root->Cast<BoundFunctionExpression>();
		if (function.children.size() == 1 && function.function.name == "-") {
			return CollectObjective(*function.children[0], decide_index, -sign, terms);
		}
		if (function.children.size() == 2 &&
		    (function.function.name == "+" || function.function.name == "-")) {
			return CollectObjective(*function.children[0], decide_index, sign, terms) &&
			       CollectObjective(*function.children[1], decide_index,
			                        function.function.name == "+" ? sign : -sign, terms);
		}
	}
	terms.push_back({root, sign});
	return true;
}

} // namespace

DirectProblemFacts DirectProblemFacts::Read(LogicalDecide &decide) {
	DirectProblemFacts facts;
	facts.decide_index = decide.decide_index;
	facts.auxiliary_variables = decide.num_auxiliary_vars;
	facts.has_entity_scopes = !decide.entity_scopes.empty();
	facts.has_entity_keys = !decide.entity_key_expressions.empty();
	facts.sense = decide.decide_sense;
	facts.objective_offset = decide.objective_constant_offset;
	facts.source_clause_count = decide.constraint_sources.size();
	if (decide.children.size() == 1 && decide.children[0]) {
		facts.source_bindings = decide.children[0]->GetColumnBindings();
		facts.source_status = DirectFactStatus::KNOWN;
	}
	if (decide.is_boolean_var.size() == decide.decide_variables.size() &&
	    decide.variable_scopes.size() == decide.decide_variables.size()) {
		facts.decisions_status = DirectFactStatus::KNOWN;
		for (idx_t i = 0; i < decide.decide_variables.size(); i++) {
			if (!decide.decide_variables[i]) {
				facts.decisions_status = DirectFactStatus::UNKNOWN;
				facts.decisions.clear();
				break;
			}
			facts.decisions.push_back({decide.decide_variables[i]->return_type, decide.variable_scopes[i].scope,
			                           decide.is_boolean_var[i]});
		}
	}
	if (decide.decide_objective &&
	    CollectObjective(*decide.decide_objective, decide.decide_index, 1, facts.objective_terms)) {
		facts.objective_status = DirectFactStatus::KNOWN;
	} else {
		facts.objective_terms.clear();
	}
	if (decide.decide_constraints &&
	    CollectConstraints(*decide.decide_constraints, facts.source_clause_count, facts.constraint_factors, {})) {
		facts.constraints_status = DirectFactStatus::KNOWN;
	} else {
		facts.constraint_factors.clear();
	}
	return facts;
}

} // namespace duckdb
