#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>

#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

namespace {

//! The WHEN and PER wrappers above a constraint, collected on the way down.
struct WrapperScope {
	vector<const Expression *> per_keys;
	const Expression *when = nullptr;
};

//! The reason of the first unknown term, including terms nested in a square or an absolute value.
bool FindUnknownTerm(const vector<DecideSplitTerm> &terms, string &reason) {
	for (auto &term : terms) {
		if (term.kind == DecideTermKind::UNKNOWN) {
			reason = term.reason;
			return true;
		}
		if (FindUnknownTerm(term.inner, reason)) {
			return true;
		}
	}
	return false;
}

void MarkUnknown(DirectFactStatus &status, string &reason, const string &why) {
	if (status == DirectFactStatus::KNOWN) {
		status = DirectFactStatus::UNKNOWN;
		reason = why;
	}
}

//! Reads one DECIDE node into facts. It is the only direct-solve code that knows LogicalDecide's layout and the
//! bound-tree spelling of DECIDE constructs.
class FactReader {
public:
	FactReader(ClientContext &context_p, LogicalDecide &decide_p)
	    : context(context_p), decide(decide_p), splitter(context_p, decide_p.decide_index, decide_p.decide_variables),
	      canonicalizer(context_p, decide_p.decide_index, decide_p.variable_scopes) {
	}

	DirectProblemFacts Read() {
		// Facts describe what the user wrote. After OptimizeDecide the tree holds a solver formulation instead.
		if (decide.optimized || decide.num_auxiliary_vars) {
			throw InternalException("Direct solve facts must be read before DECIDE rewrites the node");
		}
		DirectProblemFacts facts;
		facts.decide_index = decide.decide_index;
		facts.source_clause_count = decide.constraint_sources.size();
		if (decide.children.size() == 1 && decide.children[0]) {
			source_bindings = decide.children[0]->GetColumnBindings();
			facts.source_bindings = source_bindings;
			facts.source_status = DirectFactStatus::KNOWN;
		}
		ReadDecisions(facts);
		ReadEntityScopes(facts);
		ReadConstraints(facts);
		ReadObjective(facts.objective);
		return facts;
	}

private:
	ClientContext &context;
	LogicalDecide &decide;
	DecideTermSplitter splitter;
	DecideCanonicalizer canonicalizer;
	vector<ColumnBinding> source_bindings;

	//! The source slot `expr` reads when it is exactly a source column.
	bool SourceSlot(const Expression &expr, idx_t &slot) const {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		auto found = std::find(source_bindings.begin(), source_bindings.end(), ref.binding);
		if (ref.depth != 0 || found == source_bindings.end()) {
			return false;
		}
		slot = NumericCast<idx_t>(found - source_bindings.begin());
		return true;
	}

	void ReadDecisions(DirectProblemFacts &facts) const {
		auto count = decide.decide_variables.size();
		if (decide.is_boolean_var.size() != count || decide.variable_scopes.size() != count) {
			return;
		}
		for (idx_t i = 0; i < count; i++) {
			auto &variable = decide.decide_variables[i];
			if (!variable) {
				facts.decisions.clear();
				return;
			}
			DirectDecisionFact decision;
			decision.output_type = variable->return_type;
			decision.scope = decide.variable_scopes[i].scope;
			if (decision.scope == DecideVarScope::ENTITY) {
				decision.entity_scope = decide.variable_scopes[i].entity_scope_idx;
			}
			if (decide.is_boolean_var[i]) {
				decision.domain = DirectDomain::BOOL;
			} else if (variable->return_type == LogicalType::DOUBLE) {
				decision.domain = DirectDomain::REAL;
			} else if (variable->return_type.IsIntegral()) {
				decision.domain = DirectDomain::INT;
			} else {
				facts.decisions.clear();
				return;
			}
			facts.decisions.push_back(std::move(decision));
		}
		facts.decisions_status = DirectFactStatus::KNOWN;
	}

	void ReadEntityScopes(DirectProblemFacts &facts) const {
		for (auto &scope : decide.entity_scopes) {
			DirectEntityScopeFact fact;
			fact.relations = scope.source_table_indices;
			for (auto &binding : scope.entity_key_bindings) {
				auto found = std::find(source_bindings.begin(), source_bindings.end(), binding);
				if (found == source_bindings.end()) {
					fact.status = DirectFactStatus::UNKNOWN;
					continue;
				}
				fact.key_slots.push_back(NumericCast<idx_t>(found - source_bindings.begin()));
			}
			facts.entity_scopes.push_back(std::move(fact));
		}
	}

	//===--------------------------------------------------------------------===//
	// Parts: one additive term of a left side or the objective
	//===--------------------------------------------------------------------===//

	void ReadReducer(const BoundAggregateExpression &aggregate, DirectPart &part) const {
		if (aggregate.IsDistinct() || aggregate.order_bys || aggregate.children.size() != 1) {
			MarkUnknown(part.status, part.reason, "a reducer with DISTINCT, ORDER BY or several arguments");
			return;
		}
		string order;
		auto name = StringUtil::Lower(aggregate.function.name);
		if (ExtractDecideTagPayload(aggregate.GetAlias(), NORM_MARKER_TAG_PREFIX, order)) {
			// Canonicalization replaced every other order with its definition; L0 needs indicator variables.
			if (order != "0_auto" && order.rfind("0_", 0) != 0) {
				throw InternalException("DECIDE norm of order %s reached direct-solve facts", order);
			}
			part.reducer = DirectReducer::COUNT_NONZERO;
			part.l0_tolerance = GetDecideL0Tolerance(context);
			part.l0_bound = order == "0_auto" ? 0.0 : std::stod(order.substr(2));
		} else if (name == "sum") {
			part.reducer = DirectReducer::SUM;
		} else if (name == "avg") {
			part.reducer = DirectReducer::AVG;
		} else if (name == "min") {
			part.reducer = DirectReducer::MIN;
		} else if (name == "max") {
			part.reducer = DirectReducer::MAX;
		} else {
			MarkUnknown(part.status, part.reason, StringUtil::Format("reducer '%s' is not modelled", name));
			return;
		}
		if (aggregate.filter) {
			part.filter = aggregate.filter->Copy();
		}
		TryParseQualifiedReducerTag(aggregate.GetAlias(), part.qualifier);
		auto body = UnwrapDecideCasts(*aggregate.children[0], decide.decide_index);
		if (body->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			part.inner = make_uniq<DirectPart>();
			ReadReducer(body->Cast<BoundAggregateExpression>(), *part.inner);
			if (part.inner->status == DirectFactStatus::UNKNOWN) {
				MarkUnknown(part.status, part.reason, part.inner->reason);
			}
			return;
		}
		splitter.Split(*aggregate.children[0], 1, part.terms);
	}

	DirectPart ReadPart(const CanonicalAtom &atom) const {
		DirectPart part;
		part.sign = atom.sign;
		const Expression *term = atom.term;
		if (atom.scaled) {
			part.scale = atom.scale.scale->Copy();
			part.scale_divides = atom.scale.divides;
			term = atom.scale.aggregate;
		}
		auto root = UnwrapDecideCasts(*term, decide.decide_index);
		if (root->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			ReadReducer(root->Cast<BoundAggregateExpression>(), part);
		} else {
			splitter.Split(*root, 1, part.terms);
		}
		string reason;
		if (FindUnknownTerm(part.terms, reason)) {
			MarkUnknown(part.status, part.reason, reason);
		}
		return part;
	}

	void ReadParts(const Expression &side, vector<DirectPart> &parts) const {
		for (auto &atom : ReadCanonicalAtoms(side, decide.decide_index)) {
			parts.push_back(ReadPart(atom));
		}
	}

	//! One row-level part holding every term of a per-row side.
	DirectPart ReadRowPart(const Expression &side) const {
		DirectPart part;
		splitter.Split(side, 1, part.terms);
		string reason;
		if (FindUnknownTerm(part.terms, reason)) {
			MarkUnknown(part.status, part.reason, reason);
		}
		return part;
	}

	void ReadScope(const vector<const Expression *> &per_keys, const Expression *when, DirectScopeFact &scope,
	               DirectFactStatus &status, string &reason) const {
		for (auto key : per_keys) {
			idx_t slot;
			if (!SourceSlot(*key, slot)) {
				MarkUnknown(status, reason, "PER keys must be source columns");
				continue;
			}
			scope.per_key_slots.push_back(slot);
		}
		if (when) {
			scope.when = when->Copy();
		}
	}

	//===--------------------------------------------------------------------===//
	// Constraints
	//===--------------------------------------------------------------------===//

	DirectConstraintFact ReadConstraint(const Expression &leaf, const WrapperScope &wrappers) const {
		DirectConstraintFact fact;
		auto unknown = [&](const string &why) { MarkUnknown(fact.status, fact.reason, why); };
		idx_t source_id;
		if (TryParseSourceClauseTag(leaf.GetAlias(), source_id) && source_id < decide.constraint_sources.size()) {
			fact.source_clause_id = source_id;
			auto &source = decide.constraint_sources[source_id];
			fact.clause_text = source.written_lhs + " " + source.written_cmp + " " + source.written_rhs;
			if (!source.qualifier.empty()) {
				fact.clause_text += " " + source.qualifier;
			}
		} else {
			unknown("the clause carries no source attribution");
		}
		ReadScope(wrappers.per_keys, wrappers.when, fact.scope, fact.status, fact.reason);

		if (leaf.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR && leaf.type == ExpressionType::COMPARE_IN) {
			auto &membership = leaf.Cast<BoundOperatorExpression>();
			fact.comparison = ExpressionType::COMPARE_IN;
			fact.degree = 1;
			fact.lhs.push_back(ReadRowPart(*membership.children[0]));
			for (idx_t i = 1; i < membership.children.size(); i++) {
				fact.members.push_back(membership.children[i]->Copy());
			}
		} else if (leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			auto &comparison = leaf.Cast<BoundComparisonExpression>();
			fact.comparison = comparison.type;
			auto shape = canonicalizer.ClassifyCanonicalComparison(comparison);
			if (shape == CanonicalConstraintClass::INVALID) {
				unknown("the clause is neither one constraint per row nor one reduced constraint");
			}
			fact.aggregate = shape == CanonicalConstraintClass::AGGREGATE;
			if (fact.aggregate) {
				ReadParts(*comparison.left, fact.lhs);
			} else {
				fact.lhs.push_back(ReadRowPart(*comparison.left));
			}
			fact.rhs = comparison.right->Copy();
			fact.rhs_provenance = comparison.right->IsFoldable()                               ? DirectProvenance::CONSTANT
			                      : HasDecideTag(comparison.right->GetAlias(), QUERY_WIDE_BOUND_TAG) ? DirectProvenance::QUERY_WIDE
			                                                                                  : DirectProvenance::ROW_VARYING;
			fact.degree = DecideExpressionDegree(*comparison.left, decide.decide_index).degree;
		} else {
			unknown("the clause is not a comparison or an IN list");
		}
		for (auto &part : fact.lhs) {
			if (part.status == DirectFactStatus::UNKNOWN) {
				unknown(part.reason);
			}
		}
		return fact;
	}

	bool CollectConstraints(const Expression &expression, const WrapperScope &wrappers,
	                        vector<DirectConstraintFact> &out, string &reason) const {
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			out.push_back(ReadConstraint(expression, wrappers));
			return true;
		}
		auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		if (conjunction.type != ExpressionType::CONJUNCTION_AND || conjunction.children.empty()) {
			reason = "the constraint tree holds a conjunction that is not AND";
			return false;
		}
		if (IsPerConstraintWrapper(conjunction)) {
			if (!wrappers.per_keys.empty() || conjunction.children.size() < 2) {
				reason = "a PER wrapper is nested or has no keys";
				return false;
			}
			WrapperScope nested = wrappers;
			for (idx_t i = 1; i < conjunction.children.size(); i++) {
				nested.per_keys.push_back(conjunction.children[i].get());
			}
			return CollectConstraints(*conjunction.children[0], nested, out, reason);
		}
		if (IsWhenConstraintWrapper(conjunction)) {
			if (wrappers.when || conjunction.children.size() != 2) {
				reason = "a WHEN wrapper is nested or malformed";
				return false;
			}
			WrapperScope nested = wrappers;
			nested.when = conjunction.children[1].get();
			return CollectConstraints(*conjunction.children[0], nested, out, reason);
		}
		for (auto &child : conjunction.children) {
			if (!CollectConstraints(*child, wrappers, out, reason)) {
				return false;
			}
		}
		return true;
	}

	void ReadConstraints(DirectProblemFacts &facts) const {
		if (!decide.decide_constraints) {
			facts.constraints_status = DirectFactStatus::KNOWN;
			return;
		}
		if (CollectConstraints(*decide.decide_constraints, {}, facts.constraints, facts.constraints_reason)) {
			facts.constraints_status = DirectFactStatus::KNOWN;
		} else {
			facts.constraints.clear();
		}
	}

	//===--------------------------------------------------------------------===//
	// Objective
	//===--------------------------------------------------------------------===//

	void ReadObjective(DirectObjectiveFact &objective) const {
		objective.sense = decide.decide_sense;
		objective.offset = decide.objective_constant_offset;
		if (!decide.decide_objective) {
			return;
		}
		auto index = decide.decide_index;
		const Expression *body = UnwrapDecideCasts(*decide.decide_objective, index);
		// PER is the outer wrapper and WHEN sits inside it, as the binder builds them.
		vector<const Expression *> per_keys;
		const Expression *when = nullptr;
		if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
		    IsPerConstraintWrapper(body->Cast<BoundConjunctionExpression>())) {
			auto &per = body->Cast<BoundConjunctionExpression>();
			for (idx_t i = 1; i < per.children.size(); i++) {
				per_keys.push_back(per.children[i].get());
			}
			body = UnwrapDecideCasts(*per.children[0], index);
		}
		if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
		    IsWhenConstraintWrapper(body->Cast<BoundConjunctionExpression>()) &&
		    body->Cast<BoundConjunctionExpression>().children.size() == 2) {
			auto &wrapper = body->Cast<BoundConjunctionExpression>();
			when = wrapper.children[1].get();
			body = UnwrapDecideCasts(*wrapper.children[0], index);
		}
		ReadScope(per_keys, when, objective.scope, objective.status, objective.reason);
		if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			MarkUnknown(objective.status, objective.reason, "the objective holds an unrecognised wrapper");
			return;
		}
		ReadParts(*body, objective.parts);
		for (auto &part : objective.parts) {
			if (part.status == DirectFactStatus::UNKNOWN) {
				MarkUnknown(objective.status, objective.reason, part.reason);
			}
		}
	}
};

} // namespace

DirectProblemFacts DirectProblemFacts::Read(ClientContext &context, LogicalDecide &decide) {
	return FactReader(context, decide).Read();
}

string DirectProblemFacts::FirstUnknownReason() const {
	if (constraints_status == DirectFactStatus::UNKNOWN) {
		return "constraints: " + constraints_reason;
	}
	for (auto &constraint : constraints) {
		if (constraint.status == DirectFactStatus::UNKNOWN) {
			if (constraint.clause_text.empty()) {
				return "a clause: " + constraint.reason;
			}
			return StringUtil::Format("clause `%s`: %s", constraint.clause_text, constraint.reason);
		}
	}
	if (objective.status == DirectFactStatus::UNKNOWN) {
		return "objective: " + objective.reason;
	}
	return string();
}

} // namespace duckdb
