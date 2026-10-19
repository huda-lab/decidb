#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>

#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
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

void MarkNotProved(DirectAnalysisStatus &status, string &reason, const string &why) {
	if (status == DirectAnalysisStatus::PROVED) {
		status = DirectAnalysisStatus::NOT_PROVED;
		reason = why;
	}
}

//! Reads one DECIDE node into the IR. It is the only direct-solve code that knows LogicalDecide's layout and the
//! bound-tree spelling of DECIDE constructs.
class DirectFrontEnd {
public:
	DirectFrontEnd(ClientContext &context_p, LogicalDecide &decide_p)
	    : context(context_p), decide(decide_p), splitter(context_p, decide_p.decide_index, decide_p.decide_variables),
	      canonicalizer(context_p, decide_p.decide_index, decide_p.variable_scopes) {
	}

	DirectIR Read() {
		// The IR describes what the user wrote. After OptimizeDecide the tree holds a solver formulation instead.
		if (decide.optimized || decide.num_auxiliary_vars) {
			throw InternalException("Direct solve IR must be read before DECIDE rewrites the node");
		}
		DirectIR ir;
		ir.decide_index = decide.decide_index;
		ir.source_clause_count = decide.constraint_sources.size();
		if (decide.children.size() == 1 && decide.children[0]) {
			source_bindings = decide.children[0]->GetColumnBindings();
			ir.source_bindings = source_bindings;
			ir.source_status = DirectAnalysisStatus::PROVED;
		}
		ReadDecisions(ir);
		ReadEntityScopes(ir);
		ReadConstraints(ir);
		ReadObjective(ir.objective);
		return ir;
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

	void ReadDecisions(DirectIR &ir) const {
		auto count = decide.decide_variables.size();
		if (decide.is_boolean_var.size() != count || decide.variable_scopes.size() != count) {
			return;
		}
		for (idx_t i = 0; i < count; i++) {
			auto &variable = decide.decide_variables[i];
			if (!variable) {
				ir.decisions.clear();
				return;
			}
			DirectIRDecision decision;
			decision.name = variable->GetName();
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
				ir.decisions.clear();
				return;
			}
			ir.decisions.push_back(std::move(decision));
		}
		ir.decisions_status = DirectAnalysisStatus::PROVED;
	}

	void ReadEntityScopes(DirectIR &ir) const {
		for (auto &scope : decide.entity_scopes) {
			DirectIREntityScope ir_node;
			ir_node.relations = scope.source_table_indices;
			for (auto &binding : scope.entity_key_bindings) {
				auto found = std::find(source_bindings.begin(), source_bindings.end(), binding);
				if (found == source_bindings.end()) {
					ir_node.status = DirectAnalysisStatus::NOT_PROVED;
					continue;
				}
				ir_node.key_slots.push_back(NumericCast<idx_t>(found - source_bindings.begin()));
			}
			ir.entity_scopes.push_back(std::move(ir_node));
		}
	}

	//===--------------------------------------------------------------------===//
	// Parts: one additive term of a left side or the objective
	//===--------------------------------------------------------------------===//

	void ReadReducer(const BoundAggregateExpression &aggregate, DirectPart &part) const {
		if (aggregate.IsDistinct() || aggregate.order_bys || aggregate.children.size() != 1) {
			MarkNotProved(part.status, part.reason, "a reducer with DISTINCT, ORDER BY or several arguments");
			return;
		}
		string order;
		auto name = StringUtil::Lower(aggregate.function.name);
		if (ExtractDecideTagPayload(aggregate.GetAlias(), NORM_MARKER_TAG_PREFIX, order)) {
			// Canonicalization replaced every order but 0 with its definition. Order 0 needs indicator variables, so no
			// rule models it, and any other tag is not one this reader knows: both decline instead of throwing.
			MarkNotProved(part.status, part.reason,
			              order.rfind("0_", 0) == 0 ? string("norm of order 0 is not modelled")
			                                        : StringUtil::Format("norm tag '%s' is not modelled", order));
			return;
		} else if (name == "sum") {
			part.reducer = DirectReducer::SUM;
		} else if (name == "avg") {
			part.reducer = DirectReducer::AVG;
		} else if (name == "min") {
			part.reducer = DirectReducer::MIN;
		} else if (name == "max") {
			part.reducer = DirectReducer::MAX;
		} else {
			MarkNotProved(part.status, part.reason, StringUtil::Format("reducer '%s' is not modelled", name));
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
			if (part.inner->status == DirectAnalysisStatus::NOT_PROVED) {
				MarkNotProved(part.status, part.reason, part.inner->reason);
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
			MarkNotProved(part.status, part.reason, reason);
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
			MarkNotProved(part.status, part.reason, reason);
		}
		return part;
	}

	void ReadScope(const vector<const Expression *> &per_keys, const Expression *when, DirectIRScope &scope,
	               DirectAnalysisStatus &status, string &reason) const {
		for (auto key : per_keys) {
			idx_t slot;
			if (!SourceSlot(*key, slot)) {
				MarkNotProved(status, reason, "PER keys must be source columns");
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

	DirectIRConstraint ReadConstraint(const Expression &leaf, const WrapperScope &wrappers) const {
		DirectIRConstraint ir_node;
		auto not_proved = [&](const string &why) {
			MarkNotProved(ir_node.status, ir_node.reason, why);
		};
		idx_t source_id;
		if (TryParseSourceClauseTag(leaf.GetAlias(), source_id) && source_id < decide.constraint_sources.size()) {
			ir_node.source_clause_id = source_id;
			auto &source = decide.constraint_sources[source_id];
			ir_node.clause_text = source.written_lhs + " " + source.written_cmp + " " + source.written_rhs;
			if (!source.qualifier.empty()) {
				ir_node.clause_text += " " + source.qualifier;
			}
		} else {
			not_proved("the clause carries no source attribution");
		}
		ReadScope(wrappers.per_keys, wrappers.when, ir_node.scope, ir_node.status, ir_node.reason);

		if (leaf.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR && leaf.type == ExpressionType::COMPARE_IN) {
			auto &membership = leaf.Cast<BoundOperatorExpression>();
			ir_node.comparison = ExpressionType::COMPARE_IN;
			ir_node.degree = 1;
			ir_node.lhs.push_back(ReadRowPart(*membership.children[0]));
		} else if (leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			auto &comparison = leaf.Cast<BoundComparisonExpression>();
			ir_node.comparison = comparison.type;
			auto shape = canonicalizer.ClassifyCanonicalComparison(comparison);
			if (shape == CanonicalConstraintClass::INVALID) {
				not_proved("the clause is neither one constraint per row nor one reduced constraint");
			}
			ir_node.aggregate = shape == CanonicalConstraintClass::AGGREGATE;
			if (ir_node.aggregate) {
				ReadParts(*comparison.left, ir_node.lhs);
			} else {
				ir_node.lhs.push_back(ReadRowPart(*comparison.left));
			}
			ir_node.rhs = comparison.right->Copy();
			ir_node.degree = DecideExpressionDegree(*comparison.left, decide.decide_index).degree;
			// Folding never reads a clause under a WHEN: `x <= 0 WHEN c` does not bound `x` everywhere.
			ir_node.variable_bound = !wrappers.when && DecideIsSingletonRowBound(context, decide, comparison);
		} else {
			not_proved("the clause is not a comparison or an IN list");
		}
		for (auto &part : ir_node.lhs) {
			if (part.status == DirectAnalysisStatus::NOT_PROVED) {
				not_proved(part.reason);
			}
		}
		return ir_node;
	}

	bool CollectConstraints(const Expression &expression, const WrapperScope &wrappers, vector<DirectIRConstraint> &out,
	                        string &reason) const {
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

	void ReadConstraints(DirectIR &ir) const {
		if (!decide.decide_constraints) {
			ir.constraints_status = DirectAnalysisStatus::PROVED;
			return;
		}
		if (CollectConstraints(*decide.decide_constraints, {}, ir.constraints, ir.constraints_reason)) {
			ir.constraints_status = DirectAnalysisStatus::PROVED;
		} else {
			ir.constraints.clear();
		}
	}

	//===--------------------------------------------------------------------===//
	// Objective
	//===--------------------------------------------------------------------===//

	void ReadObjective(DirectIRObjective &objective) const {
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
			MarkNotProved(objective.status, objective.reason, "the objective holds an unrecognised wrapper");
			return;
		}
		ReadParts(*body, objective.parts);
		for (auto &part : objective.parts) {
			if (part.status == DirectAnalysisStatus::NOT_PROVED) {
				MarkNotProved(objective.status, objective.reason, part.reason);
			}
		}
	}
};

} // namespace

DirectIR DirectIR::Read(ClientContext &context, LogicalDecide &decide) {
	return DirectFrontEnd(context, decide).Read();
}

string DirectIR::FirstNotProvedReason() const {
	if (constraints_status == DirectAnalysisStatus::NOT_PROVED) {
		return "constraints: " + constraints_reason;
	}
	for (auto &constraint : constraints) {
		if (constraint.status == DirectAnalysisStatus::NOT_PROVED) {
			if (constraint.clause_text.empty()) {
				return "a clause: " + constraint.reason;
			}
			return StringUtil::Format("clause `%s`: %s", constraint.clause_text, constraint.reason);
		}
	}
	if (objective.status == DirectAnalysisStatus::NOT_PROVED) {
		return "objective: " + objective.reason;
	}
	return string();
}

const DirectPart *DirectIRConstraint::PlainSum(bool allow_filter) const {
	if (!aggregate || lhs.size() != 1) {
		return nullptr;
	}
	auto &part = lhs[0];
	if (part.sign != 1 || part.scale || part.reducer != DirectReducer::SUM ||
	    part.qualifier != DConstants::INVALID_INDEX || part.inner || (!allow_filter && part.filter)) {
		return nullptr;
	}
	return &part;
}

} // namespace duckdb
