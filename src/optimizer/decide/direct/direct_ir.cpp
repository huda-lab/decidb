#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
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

//! The `when` wrapper above a constraint, collected on the way down.
struct WrapperScope {
	const Expression *when = nullptr;
};

//! Whether any term is unknown, including terms nested in a square or an absolute value.
bool HasUnknownTerm(const vector<DecideSplitTerm> &terms) {
	for (auto &term : terms) {
		if (term.kind == DecideTermKind::UNKNOWN) {
			return true;
		}
		if (HasUnknownTerm(term.inner)) {
			return true;
		}
	}
	return false;
}

void MarkNotProved(DirectAnalysisStatus &status) {
	status = DirectAnalysisStatus::NOT_PROVED;
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
		D_ASSERT(!decide.optimized && !decide.num_auxiliary_vars);
		DirectIR ir;
		ir.decide_index = decide.decide_index;
		ir.source_clause_count = decide.constraint_sources.size();
		D_ASSERT(decide.children.size() == 1 && decide.children[0]);
		source_bindings = decide.children[0]->GetColumnBindings();
		ir.source_bindings = source_bindings;
		ReadDecisions(ir);
		ReadEntityScopes(ir);
		ReadConstraints(ir);
		ReadObjective(ir.objective);
		ReadRowChecks(ir);
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
		// A resolved group key is a column of the source, never of an outer query.
		D_ASSERT(ref.depth == 0 && found != source_bindings.end());
		slot = NumericCast<idx_t>(found - source_bindings.begin());
		return true;
	}

	//! A predicate a row check can evaluate under the rule's plan: it reads only source columns. One that raises does
	//! so there, before any value is read, as in the solver's scope check.
	bool CheckablePredicate(const Expression *predicate) const {
		return !predicate || DirectReferencesOnlySource(*predicate, source_bindings);
	}

	void ReadRowChecks(DirectIR &ir) const {
		for (auto &check : decide.scope_checks) {
			DirectRowCheck row_check;
			if (check.kind == DecideScopeCheckKind::KEEPS_A_ROW) {
				row_check.filter = check.rows->Copy();
				row_check.message = DecideWhenMatchesNoRowsMessage(check.subject);
			} else if (check.kind == DecideScopeCheckKind::READS_A_ROW && check.key.empty()) {
				row_check.rows = check.rows ? check.rows->Copy() : nullptr;
				row_check.filter = check.filter->Copy();
				row_check.message = DecideReducerHasNoRowsMessage(check.subject);
			} else if (check.kind == DecideScopeCheckKind::VALUE_IS_VALID) {
				// The bound-side window rewrite makes these over the source and its own windows, with no `rows`.
				D_ASSERT(!check.rows && CheckablePredicate(check.value.get()));
				row_check.invalid = check.value->Copy();
				ir.row_checks.push_back(std::move(row_check));
				continue;
			} else {
				// No rule groups rows by a `by` key or a `per K:` class yet.
				MarkNotProved(ir.status);
				continue;
			}
			// A `when` names no decision and is planned over the DECIDE input, so it reads source columns only.
			D_ASSERT(CheckablePredicate(row_check.rows.get()) && CheckablePredicate(row_check.filter.get()));
			ir.row_checks.push_back(std::move(row_check));
		}
	}

	void ReadDecisions(DirectIR &ir) const {
		auto count = decide.decide_variables.size();
		D_ASSERT(decide.is_boolean_var.size() == count && decide.variable_scopes.size() == count);
		for (idx_t i = 0; i < count; i++) {
			auto &variable = decide.decide_variables[i];
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
			} else {
				D_ASSERT(variable->return_type.IsIntegral());
				decision.domain = DirectDomain::INT;
			}
			ir.decisions.push_back(std::move(decision));
		}
	}

	void ReadEntityScopes(DirectIR &ir) const {
		for (auto &scope : decide.entity_scopes) {
			DirectIREntityScope ir_node;
			ir_node.relations = scope.source_table_indices;
			for (auto &binding : scope.entity_key_bindings) {
				auto found = std::find(source_bindings.begin(), source_bindings.end(), binding);
				// The binder keeps every entity key live in the source.
				D_ASSERT(found != source_bindings.end());
				ir_node.key_slots.push_back(NumericCast<idx_t>(found - source_bindings.begin()));
			}
			ir.entity_scopes.push_back(std::move(ir_node));
		}
	}

	//===--------------------------------------------------------------------===//
	// Parts: one additive term of a left side or the objective
	//===--------------------------------------------------------------------===//

	void ReadReducer(const BoundAggregateExpression &aggregate, DirectPart &part) const {
		D_ASSERT(!aggregate.IsDistinct() && !aggregate.order_bys && aggregate.children.size() == 1);
		string order;
		auto name = StringUtil::Lower(aggregate.function.name);
		if (ExtractDecideTagPayload(aggregate.GetAlias(), NORM_MARKER_TAG_PREFIX, order)) {
			// Canonicalization replaced every order but 0 with its definition. Order 0 needs indicator variables, so no
			// rule models it, and any other tag is not one this reader knows: both decline instead of throwing.
			MarkNotProved(part.status);
			return;
		} else if (name == "sum") {
			part.reducer = DirectReducer::SUM;
		} else if (name == "avg") {
			part.reducer = DirectReducer::AVG;
		} else if (name == "min") {
			part.reducer = DirectReducer::MIN;
		} else {
			// The binder admits no other reducer in a DECIDE clause.
			D_ASSERT(name == "max");
			part.reducer = DirectReducer::MAX;
		}
		idx_t group_key;
		if (TryParseGroupKeyTag(aggregate.GetAlias(), group_key)) {
			// No rule groups a reducer by its own `by` key yet.
			MarkNotProved(part.status);
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
				MarkNotProved(part.status);
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
		if (HasUnknownTerm(part.terms)) {
			MarkNotProved(part.status);
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
		if (HasUnknownTerm(part.terms)) {
			MarkNotProved(part.status);
		}
		return part;
	}

	void ReadScope(const Expression *when, DirectIRScope &scope) const {
		if (when) {
			scope.when = when->Copy();
		}
	}

	//===--------------------------------------------------------------------===//
	// Constraints
	//===--------------------------------------------------------------------===//

	DirectIRConstraint ReadConstraint(const Expression &leaf, const WrapperScope &wrappers) const {
		DirectIRConstraint ir_node;
		idx_t source_id;
		if (TryParseSourceClauseTag(leaf.GetAlias(), source_id)) {
			D_ASSERT(source_id < decide.constraint_sources.size());
			ir_node.source_clause_id = source_id;
		} else {
			MarkNotProved(ir_node.status);
		}
		ReadScope(wrappers.when, ir_node.scope);
		if (HasDecideTag(leaf.GetAlias(), VALUE_REWRITE_TAG)) {
			// No rule models the helper decision a reducer's value becomes.
			MarkNotProved(ir_node.status);
		}

		// The binder admits only comparisons and IN as a constraint leaf.
		if (leaf.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
			D_ASSERT(leaf.type == ExpressionType::COMPARE_IN);
			auto &membership = leaf.Cast<BoundOperatorExpression>();
			ir_node.comparison = ExpressionType::COMPARE_IN;
			ir_node.degree = 1;
			ir_node.lhs.push_back(ReadRowPart(*membership.children[0]));
		} else {
			D_ASSERT(leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON);
			auto &comparison = leaf.Cast<BoundComparisonExpression>();
			ir_node.comparison = comparison.type;
			auto shape = canonicalizer.ClassifyCanonicalComparison(comparison);
			// The canonicalizer rejected an invalid comparison when it bound the query.
			D_ASSERT(shape != CanonicalConstraintClass::INVALID);
			ir_node.aggregate = shape == CanonicalConstraintClass::AGGREGATE;
			if (ir_node.aggregate) {
				ReadParts(*comparison.left, ir_node.lhs);
			} else {
				ir_node.lhs.push_back(ReadRowPart(*comparison.left));
			}
			ir_node.rhs = comparison.right->Copy();
			ir_node.degree = DecideExpressionDegree(*comparison.left, decide.decide_index).degree;
			// Folding never reads a clause under a `when`: `when c: x <= 0` does not bound `x` everywhere. A folded
			// bound that is not plain (an affine spelling or an extremum) is one the rules do not model yet.
			bool plain = false;
			ir_node.variable_bound = !wrappers.when && DecideIsSingletonRowBound(context, decide, comparison, plain);
			if (ir_node.variable_bound && !plain) {
				MarkNotProved(ir_node.status);
			}
		}
		for (auto &part : ir_node.lhs) {
			if (part.status == DirectAnalysisStatus::NOT_PROVED) {
				MarkNotProved(ir_node.status);
			}
		}
		return ir_node;
	}

	void CollectConstraints(const Expression &expression, const WrapperScope &wrappers,
	                        vector<DirectIRConstraint> &out) const {
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			out.push_back(ReadConstraint(expression, wrappers));
			return;
		}
		auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		// The binder rejects a user-written OR between constraints; nothing else builds one before the IR.
		D_ASSERT(conjunction.type == ExpressionType::CONJUNCTION_AND);
		D_ASSERT(!conjunction.children.empty());
		if (IsWhenConstraintWrapper(conjunction)) {
			D_ASSERT(!wrappers.when && conjunction.children.size() == 2);
			WrapperScope nested = wrappers;
			nested.when = conjunction.children[1].get();
			CollectConstraints(*conjunction.children[0], nested, out);
			return;
		}
		if (IsPerClassWrapper(conjunction)) {
			// No rule writes one constraint per class of a `per K:` key yet.
			DirectIRConstraint ir_node;
			MarkNotProved(ir_node.status);
			out.push_back(std::move(ir_node));
			return;
		}
		for (auto &child : conjunction.children) {
			CollectConstraints(*child, wrappers, out);
		}
	}

	void ReadConstraints(DirectIR &ir) const {
		// The parser requires a SUCH THAT clause.
		D_ASSERT(decide.decide_constraints);
		CollectConstraints(*decide.decide_constraints, {}, ir.constraints);
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
		if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
		    IsPerConstraintWrapper(body->Cast<BoundConjunctionExpression>())) {
			// No rule models the two-level objective `outer(per k: inner(e) by (k))` yet.
			MarkNotProved(objective.status);
			return;
		}
		const Expression *when = nullptr;
		if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &wrapper = body->Cast<BoundConjunctionExpression>();
			D_ASSERT(IsWhenConstraintWrapper(wrapper) && wrapper.children.size() == 2);
			when = wrapper.children[1].get();
			body = UnwrapDecideCasts(*wrapper.children[0], index);
		}
		ReadScope(when, objective.scope);
		D_ASSERT(body->GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION);
		ReadParts(*body, objective.parts);
		for (auto &part : objective.parts) {
			if (part.status == DirectAnalysisStatus::NOT_PROVED) {
				MarkNotProved(objective.status);
			}
		}
	}
};

} // namespace

DirectIR DirectIR::Read(ClientContext &context, LogicalDecide &decide) {
	return DirectFrontEnd(context, decide).Read();
}

const DirectPart *DirectIRConstraint::PlainSum() const {
	if (!aggregate || lhs.size() != 1) {
		return nullptr;
	}
	auto &part = lhs[0];
	if (part.sign != 1 || part.scale || part.reducer != DirectReducer::SUM ||
	    part.qualifier != DConstants::INVALID_INDEX || part.inner) {
		return nullptr;
	}
	return &part;
}

const Expression *DirectIRConstraint::Membership(bool &ok) const {
	auto sum = PlainSum();
	ok = sum && !(sum->filter && scope.when);
	if (!ok) {
		return nullptr;
	}
	return sum->filter ? sum->filter.get() : scope.when.get();
}

} // namespace duckdb
