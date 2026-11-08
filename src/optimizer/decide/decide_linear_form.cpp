#include "duckdb/optimizer/decide/decide_linear_form.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

#include <functional>
#include <unordered_map>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Structural helpers
//===--------------------------------------------------------------------===//

// ExpressionIterator::EnumerateChildren has no const overload; this wrapper
// isolates the const_cast so no call site needs to mention it.
static void EnumerateChildrenConst(const Expression &expr,
                                   const std::function<void(unique_ptr<Expression> &)> &callback) {
	ExpressionIterator::EnumerateChildren(const_cast<Expression &>(expr), callback);
}

//! The solver path reads what survives lowering: terms of degree 0, 1 and 2. The shape check refused every other
//! node over a decision and RewriteAbs lowered every ABS, so an opaque term here was built by a rewrite.
[[noreturn]] static void ThrowOpaqueTerm(const DecideSplitTerm &term) {
	throw InternalException("DECIDE term '%s' reached term extraction unopened", term.opaque->ToString());
}

//! Takes what a coefficient is computed from out of its term (DecideTerm::inputs): the values recorded when DeciDB
//! computed it, else the coefficient itself, as written.
static vector<unique_ptr<Expression>> TakeInputs(vector<unique_ptr<Expression>> &inputs,
                                                 const Expression &coefficient) {
	vector<unique_ptr<Expression>> taken;
	if (inputs.empty()) {
		taken.push_back(coefficient.Copy());
	}
	for (auto &input : inputs) {
		taken.push_back(std::move(input));
	}
	inputs.clear();
	return taken;
}

//! Terms of an expression that must be linear, as prepared terms.
static void AppendLinearTerms(vector<DecideSplitTerm> &split, vector<DecideTerm> &out) {
	for (auto &term : split) {
		if (term.opaque) {
			ThrowOpaqueTerm(term);
		}
		if (term.degree > 1) {
			throw InternalException("DECIDE linear expression split into a non-linear term");
		}
		out.push_back(DecideTerm {term.var_a, std::move(term.coefficient), term.sign});
	}
}

//! A term of degree 2 as the group stages 06 and 08 read: its coefficient is the group's weight, and its one squared
//! factor, or its two factors, are the group's forms.
static DecideProductGroup ReadProductGroup(DecideSplitTerm &term) {
	D_ASSERT(term.IsSquare() || term.IsProduct());
	DecideProductGroup group;
	group.weight = std::move(term.coefficient);
	group.sign = term.sign;
	group.square = term.IsSquare();
	AppendLinearTerms(term.factors[0].form, group.left);
	if (!group.square) {
		AppendLinearTerms(term.factors[1].form, group.right);
	}
	return group;
}

//===--------------------------------------------------------------------===//
// DecideLinearFormBuilder
//===--------------------------------------------------------------------===//

//! Flattens the canonical constraint tree and the objective into the prepared
//! linear form. Everything it needs is a type or a structure: the decision
//! variable list, their scopes, and the tags earlier passes stamped on the tree.
//! It never reads a data row.
class DecideLinearFormBuilder {
public:
	DecideLinearFormBuilder(ClientContext &context, LogicalDecide &op)
	    : context(context), op(op), decide_index(op.decide_index),
	      canonicalizer(context, op.decide_index, op.variable_scopes, op.keyed_scopes, op.is_boolean_var),
	      splitter(context, op.decide_index, op.decide_variables) {
	}

	void Build() {
		ForEachScopedConstraintLeaf(
		    *op.decide_constraints,
		    [&](const Expression &leaf, const DecideEnclosingScope &scope) { AnalyzeConstraint(leaf, scope); });
		if (op.decide_objective) {
			AnalyzeObjective(op.decide_objective);
		}
		// The composed MIN/MAX terms are emitted by RewriteComposedMinMax as bare
		// inner expressions. Flatten them here too, so stage 08 reads prepared terms
		// for every construct rather than for all but one.
		for (auto &spec : op.composed_minmax_constraints) {
			for (auto &term : spec.terms) {
				ExtractLinearTerms(*term.inner_expr, term.inner_terms);
			}
		}
		for (auto &term : op.composed_minmax_objective_terms) {
			ExtractLinearTerms(*term.inner_expr, term.inner_terms);
		}
		// The reducers the value rewrite lifted into helper decisions, the same way.
		for (auto &link : op.reducer_value_links) {
			ExtractLinearTerms(*link.inner_expr, link.inner_terms);
		}

		// Group like terms, once, over every list that becomes a linear solver row.
		for (auto &constraint : op.prepared.constraints) {
			CollectLikeTerms(constraint->lhs_terms);
			SettleWeightSigns(constraint->product_groups);
		}
		if (op.prepared.objective) {
			DropNullObjectiveTerms(*op.prepared.objective);
			CollectLikeTerms(op.prepared.objective->terms);
			SettleWeightSigns(op.prepared.objective->product_groups);
		}
		for (auto &spec : op.composed_minmax_constraints) {
			for (auto &term : spec.terms) {
				CollectLikeTerms(term.inner_terms);
			}
		}
		for (auto &term : op.composed_minmax_objective_terms) {
			CollectLikeTerms(term.inner_terms);
		}
		for (auto &link : op.reducer_value_links) {
			CollectLikeTerms(link.inner_terms);
		}

		// A recorded bound's repair rows come from its clause, so the repair model would
		// open its column with nothing enforcing it were its clause not flattened above.
		if (op.user_singleton_row_bounds.size() != op.prepared.singleton_row_constraints.size()) {
			throw InternalException("DECIDE: %llu singleton-row bounds take their repair rows from their clause, but "
			                        "%llu such clauses were flattened",
			                        op.user_singleton_row_bounds.size(), op.prepared.singleton_row_constraints.size());
		}
	}

private:
	//===------------------------------------------------------------------===//
	// Variable lookup and degree
	//===------------------------------------------------------------------===//

	idx_t FindDecideVariable(const Expression &expr) const {
		return splitter.FindVariable(expr);
	}

	void ExtractLinearTerms(const Expression &expr, vector<DecideTerm> &out) const {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, 1, split);
		AppendLinearTerms(split, out);
	}

	static bool BoundExpressionContainsAggregate(const Expression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			return true;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			return BoundExpressionContainsAggregate(*cast.child);
		}
		bool found = false;
		EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
			if (!found && BoundExpressionContainsAggregate(*child)) {
				found = true;
			}
		});
		return found;
	}

	//===------------------------------------------------------------------===//
	// Like-term collection
	//===------------------------------------------------------------------===//

	//! Whether two terms describe the same contribution and may be summed into one.
	//!
	//! Naming the same variable is not enough. A term also carries *which rows it
	//! applies to* and *which reducer produced it*, and two terms that disagree on
	//! any of that are different contributions that happen to share a column:
	//!
	//! - `reduction` separates a reducer term from a row-invariant one. `SUM(x)` and
	//!   a query-wide `x` in the same clause are summed differently downstream.
	//! - `filter` is the reducer's own `when`. `sum(when a: x)` and `sum(when b: x)`
	//!   name one column over two row sets; merging them would apply one mask to both.
	//! - `avg_scale` divides by the group's row count, so an AVG term and a SUM term
	//!   are not summable before that scaling happens.
	//! - `qualifier_scope_idx` selects a de-duplication mask (`sum(D: ...)`), which is
	//!   again a statement about which rows contribute.
	//!
	//! Constants (`INVALID_INDEX`) are deliberately left alone: they are a fixed
	//! offset folded into the RHS, not a repeated column, and they are not what any
	//! consumer iterating `variable_indices` can trip over.
	static bool TermsAreLike(const DecideTerm &a, const DecideTerm &b) {
		if (a.variable_index == DConstants::INVALID_INDEX) {
			return false;
		}
		if (a.reduction != b.reduction || a.avg_scale != b.avg_scale ||
		    a.qualifier_scope_idx != b.qualifier_scope_idx) {
			return false;
		}
		if ((a.filter == nullptr) != (b.filter == nullptr)) {
			return false;
		}
		return !a.filter || a.filter->Equals(*b.filter);
	}

	//! A NULL term of the objective is dropped (B1): its coefficient reads 0. This is
	//! what keeps a NULL from reaching the objective's solver row, wherever it comes
	//! from -- a factor on a reducer known only at run time (`(SELECT k) * SUM(p*x)`) or
	//! a term read once outside every reducer (`c * u`); a NULL inside a reducer is
	//! already out of its filter (DecideCanonicalizer::SkipNullData). It runs before like
	//! terms merge, which would let one NULL term drop another on the same decision. A
	//! product group's weight is guarded the same way; a form the group multiplies is not a
	//! sum of terms, so a NULL inside it is not read as 0.
	void DropNullObjectiveTerms(DecideObjective &objective) const {
		auto zero_if_null = [&](unique_ptr<Expression> &coefficient) {
			if (!coefficient || coefficient->IsFoldable()) {
				return;
			}
			auto type = coefficient->return_type;
			auto guarded = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, type);
			guarded->children.push_back(std::move(coefficient));
			guarded->children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(0).DefaultCastAs(type)));
			guarded->SetAlias(NULL_GUARD_TAG);
			coefficient = std::move(guarded);
		};
		for (auto &term : objective.terms) {
			zero_if_null(term.coefficient);
		}
		for (auto &group : objective.product_groups) {
			zero_if_null(group.weight);
		}
	}

	//! Sum like terms into one, in place, preserving first-occurrence order.
	//!
	//! `2*ship + 3*ship` used to reach the solver as two terms naming one column. The
	//! model builder folded the duplicate when writing the matrix row, so the emitted
	//! model was always right -- but every other consumer had to remember that an
	//! index can repeat, and one of them did not: the implied-bound derivation read a
	//! single term's coefficient instead of the sum (fixed 2026-08-15 by a defensive
	//! accumulate, which stays). Collecting here removes the trap at its source.
	//!
	//! A NULL coefficient merges to NULL, so a NULL objective term is made 0 first
	//! (DropNullObjectiveTerms); in a constraint the NULL is the instance's and skips it.
	//!
	//! A term contributes `sign * coefficient`, so a group merges as
	//! `sign_first * (coef_first ± coef_next ± ...)`, taking `-` exactly when a term's
	//! sign differs from the group's. The result is bound through `FunctionBinder`
	//! like every other rebuilt coefficient.
	void CollectLikeTerms(vector<DecideTerm> &terms) const {
		if (terms.size() < 2) {
			return;
		}
		vector<DecideTerm> out;
		out.reserve(terms.size());
		// Bucket by variable so the scan for a match stays short on a wide clause.
		unordered_map<idx_t, vector<idx_t>> candidates;
		for (auto &term : terms) {
			idx_t target = DConstants::INVALID_INDEX;
			auto bucket = candidates.find(term.variable_index);
			if (bucket != candidates.end()) {
				for (auto slot : bucket->second) {
					if (TermsAreLike(out[slot], term)) {
						target = slot;
						break;
					}
				}
			}
			if (target == DConstants::INVALID_INDEX) {
				candidates[term.variable_index].push_back(out.size());
				out.push_back(std::move(term));
				continue;
			}
			auto &merged = out[target];
			auto inputs = TakeInputs(merged.inputs, *merged.coefficient);
			for (auto &input : TakeInputs(term.inputs, *term.coefficient)) {
				inputs.push_back(std::move(input));
			}
			merged.coefficient = DecideMergeLikeCoefficients(context, merged.sign, std::move(merged.coefficient),
			                                                 term.sign, std::move(term.coefficient));
			merged.inputs = std::move(inputs);
		}
		terms = std::move(out);
	}

	//===------------------------------------------------------------------===//
	// Reducer metadata and peeled scales
	//===------------------------------------------------------------------===//

	//! Keyed scope a reducer is qualified by (`sum(D: ...)`), read back from the tag the
	//! binder stamped on the aggregate; INVALID_INDEX when the reducer is unqualified.
	static idx_t QualifierScopeOf(const BoundAggregateExpression &agg) {
		idx_t scope_idx = DConstants::INVALID_INDEX;
		TryParseQualifiedReducerTag(agg.alias, scope_idx);
		return scope_idx;
	}

	//! A reducer's AVG tag, qualifier and reduction on every term or product group extracted from its body since
	//! `begin`.
	template <class TERM>
	static void ApplyAggregateMetadata(vector<TERM> &items, idx_t begin, const BoundAggregateExpression &agg) {
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);
		for (idx_t i = begin; i < items.size(); i++) {
			items[i].avg_scale = is_avg;
			items[i].qualifier_scope_idx = qualifier_scope;
			items[i].reduction = LinearTermReduction::SUM;
		}
	}

	//! Attach a reducer's own `when` to every term extracted from its body since the given
	//! marks: linear terms and product groups alike. It is done here, once,
	//! after extraction, rather than threaded through every extraction branch: a branch that
	//! forgot it -- the decision-free cast arm once did -- left one term unfiltered, and the
	//! executor then read every kept row as a member of an easy MIN/MAX, so the rows the
	//! reducer dropped reached the model with the body's constant as their whole left side.
	static void AttachReducerFilter(DecideConstraint &constraint, idx_t linear_before, idx_t group_before,
	                                const Expression *filter) {
		if (!filter) {
			return;
		}
		for (idx_t i = linear_before; i < constraint.lhs_terms.size(); i++) {
			constraint.lhs_terms[i].filter = filter->Copy();
		}
		for (idx_t i = group_before; i < constraint.product_groups.size(); i++) {
			constraint.product_groups[i].filter = filter->Copy();
		}
	}

	//! What the plan knows of each group's weight sign, read once its reducer's factor is in (IsNonconvexProductGroup).
	void SettleWeightSigns(vector<DecideProductGroup> &groups) const {
		for (auto &group : groups) {
			group.weight_sign = group.sign * ScaleSignAtPlanTime(context, group.weight.get(), false);
		}
	}

	//! Multiply a coefficient by a factor that stayed outside a reducer, keeping the
	//! operand types the original `*` / `/` node was bound for and then binding the
	//! rebuilt node through `FunctionBinder`. Casting first preserves the division
	//! semantics the user's expression was bound with; binding after is what removes
	//! the need to reuse another node's `FunctionData`. A missing coefficient is 1.
	//!
	//! The product is a number DeciDB computes: `inputs` (DecideTerm::inputs) records the factor and what the
	//! coefficient is computed from, in the order the operands stand.
	void ScaleCoefficient(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                      unique_ptr<Expression> &coef, vector<unique_ptr<Expression>> &inputs) const {
		if (!coef) {
			coef = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		}
		auto written = TakeInputs(inputs, *coef);
		const auto &coef_type = scale_func.function.arguments[divides ? 0 : 1];
		const auto &scale_type = scale_func.function.arguments[divides ? 1 : 0];
		vector<unique_ptr<Expression>> children;
		// `scale * coef` keeps the factor on the left, matching the canonical
		// spelling; `coef / scale` has to keep the operand order division needs.
		if (divides) {
			children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), coef_type));
			children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), scale_type));
		} else {
			children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), scale_type));
			children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), coef_type));
			inputs.push_back(scale.Copy());
		}
		for (auto &input : written) {
			inputs.push_back(std::move(input));
		}
		if (divides) {
			inputs.push_back(scale.Copy());
		}
		coef = DecideRebindOperator(context, divides ? "/" : "*", std::move(children));
	}

	//! Multiply everything the aggregate under a peeled scale just produced by that
	//! scale. Folding the factor into the reducer's body is what the canonicalizer
	//! refuses to do, because at the parsed level the aggregate may still be MIN/MAX
	//! and `MAX(-2x)` is `-2*MIN(x)`, not `-2*MAX(x)`. Here it is safe and exact: the
	//! optimizer has already rewritten every MIN/MAX to SUM (asserted below), and a
	//! sum distributes over any factor regardless of sign.
	void ApplyScaleToExtracted(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                           vector<DecideTerm> &terms, vector<DecideProductGroup> &groups, idx_t linear_before,
	                           idx_t group_before) {
		for (idx_t i = linear_before; i < terms.size(); i++) {
			ScaleCoefficient(scale_func, scale, divides, terms[i].coefficient, terms[i].inputs);
		}
		for (idx_t i = group_before; i < groups.size(); i++) {
			ScaleCoefficient(scale_func, scale, divides, groups[i].weight, groups[i].inputs);
		}
	}

	//! Every decision `expr` reads is query-wide: one column each, whichever row reads it.
	bool ReadsOnlyQueryWideDecisions(const Expression &expr) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			idx_t var_idx = FindDecideVariable(expr);
			// A data factor of a product outside a reducer is peeled as the atom's scale (ReadCanonicalAtoms), so the
			// columns left here are decisions.
			if (var_idx == DConstants::INVALID_INDEX) {
				throw InternalException("DECIDE objective term '%s' holds a data column beside a product of decisions",
				                        expr.ToString());
			}
			return op.variable_scopes[var_idx].IsGlobal();
		}
		bool all = true;
		EnumerateChildrenConst(
		    expr, [&](unique_ptr<Expression> &child) { all = all && ReadsOnlyQueryWideDecisions(*child); });
		return all;
	}

	//===------------------------------------------------------------------===//
	// Constraints
	//===------------------------------------------------------------------===//

	//! The left side of an aggregate constraint, one canonical atom at a time: a reducer, a reducer with the
	//! factor canonicalization left on it, or a tuple term beside the reducers.
	void ExtractAggregateConstraintTerms(const Expression &lhs, DecideConstraint &constraint) {
		for (auto &atom : ReadCanonicalAtoms(lhs, decide_index)) {
			auto &expr = *atom.term;
			if (atom.scaled) {
				auto scale = atom.scale.scale;
				// Defensive invariant: both user-written and optimizer-generated
				// constraints pass canonical validation. Keep this guard so an
				// in-place optimizer mutation cannot turn a decision into a coefficient
				// and crash during evaluation.
				if (FindDecideVariable(*scale) != DConstants::INVALID_INDEX) {
					throw InternalException("DECIDE constraint: '%s' is a decision, so it cannot multiply an "
					                        "aggregate. Only data can scale SUM/AVG/MIN/MAX.",
					                        scale->ToString());
				}
				idx_t linear_before = constraint.lhs_terms.size();
				idx_t group_before = constraint.product_groups.size();
				ExtractReducerConstraintTerms(*atom.scale.aggregate, constraint, atom.sign);
				ApplyScaleToExtracted(*atom.scale.function, *scale, atom.scale.divides, constraint.lhs_terms,
				                      constraint.product_groups, linear_before, group_before);
				continue;
			}
			// A tuple term beside the reducers: the canonicalizer admitted it as one value per group
			// (ClassifyScoped), and execution counts it once in each group's row (CountOnce). A decision inside a
			// reducer body (`SUM(cost * cap)`) is the reducer's, read below with the coefficient of each row.
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
				ExtractConstraintTerms(expr, constraint, atom.sign);
				continue;
			}
			ExtractReducerConstraintTerms(expr.Cast<BoundAggregateExpression>(), constraint, atom.sign);
		}
	}

	void ExtractReducerConstraintTerms(const BoundAggregateExpression &agg, DecideConstraint &constraint, int sign) {
		auto agg_name = StringUtil::Lower(agg.function.name);
		if (agg_name != "sum") {
			throw InternalException("DECIDE optimizer did not rewrite aggregate '%s' to SUM before execution",
			                        agg.function.name);
		}
		idx_t linear_before = constraint.lhs_terms.size();
		idx_t group_before = constraint.product_groups.size();
		ExtractConstraintTerms(*agg.children[0], constraint, sign);
		AttachReducerFilter(constraint, linear_before, group_before, agg.filter.get());
		ApplyAggregateMetadata(constraint.lhs_terms, linear_before, agg);
		ApplyAggregateMetadata(constraint.product_groups, group_before, agg);

		string minmax_payload;
		if (ExtractDecideTagPayload(agg.alias, MINMAX_CLAUSE_TAG_PREFIX, minmax_payload)) {
			// "<clause_idx>_<agg>", always both parts: the clause is registered where it is
			// tagged, before either formulation is chosen, so the index is there whichever
			// arm follows. Neither "min" nor "max" contains an underscore, so the first one
			// separates the two.
			auto sep = minmax_payload.find('_');
			D_ASSERT(sep != string::npos);
			constraint.minmax_clause_idx = std::stoull(minmax_payload.substr(0, sep));
			constraint.minmax_agg_type = minmax_payload.substr(sep + 1);
			constraint.kind = ConstraintKind::USER_MECHANISM;
		}
	}

	//! The per-row terms of a scoped easy MIN/MAX, `[s *] max(when c: e)`: the terms of `e`,
	//! each filtered by `c` and scaled by `s`.
	void ExtractEasyExtremumTerms(const Expression &lhs, DecideConstraint &constraint) {
		ScaledAggregateMatch scale_match;
		const BoundAggregateExpression *agg = nullptr;
		if (TryMatchScaledAggregate(lhs, decide_index, scale_match)) {
			agg = scale_match.aggregate;
		} else if (lhs.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			agg = &lhs.Cast<BoundAggregateExpression>();
		}
		if (!agg || agg->children.size() != 1) {
			throw InternalException("DECIDE scoped MIN/MAX reached term extraction without its reducer: %s",
			                        lhs.ToString());
		}
		D_ASSERT(constraint.lhs_terms.empty() && constraint.product_groups.empty());
		ExtractConstraintTerms(*agg->children[0], constraint, 1);
		AttachReducerFilter(constraint, 0, 0, agg->filter.get());
		if (scale_match.aggregate) {
			ApplyScaleToExtracted(*scale_match.function, *scale_match.scale, scale_match.divides, constraint.lhs_terms,
			                      constraint.product_groups, 0, 0);
		}
	}

	//! A reducer body or a per-row left side, split by the shared splitter into the constraint's linear terms and one
	//! product group per term of degree 2.
	void ExtractConstraintTerms(const Expression &expr, DecideConstraint &constr, int sign) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, sign, split);
		for (auto &term : split) {
			if (term.opaque) {
				ThrowOpaqueTerm(term);
			}
			if (term.degree <= 1) {
				constr.lhs_terms.push_back(DecideTerm {term.var_a, std::move(term.coefficient), term.sign});
				continue;
			}
			constr.product_groups.push_back(ReadProductGroup(term));
		}
	}

	//! One clause of the constraint tree, under the `when` and `per K:` that enclose it.
	void AnalyzeConstraint(const Expression &expr, const DecideEnclosingScope &scope) {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_COMPARISON: {
			auto &comp = expr.Cast<BoundComparisonExpression>();

			// Skip comparisons the optimizer already folded into the column box.
			// Emitting a DecideConstraint here would add num_rows redundant model
			// rows. The comparison is still in the tree so EXPLAIN renders it; the
			// tag is the decision, and it was made by FoldSingletonRows. One whose
			// repair rows come from its clause is flattened aside, for the repair model
			// alone (singleton_row_constraints).
			const bool folded = HasDecideTag(comp.alias, SINGLETON_ROW_BOUND_TAG);
			if (folded && !HasDecideTag(comp.alias, SINGLETON_ROW_CLAUSE_ROWS_TAG)) {
				break;
			}

			auto constraint = make_uniq<DecideConstraint>();
			constraint->comparison_type = comp.type;
			constraint->rhs_expr = comp.right->Copy();
			TryParseSourceClauseTag(comp.GetAlias(), constraint->source_clause_id);
			TryParseRemovalGroupTag(comp.GetAlias(), constraint->removal_group_id);

			string payload;
			if (ExtractDecideTagPayload(comp.alias, NE_CLAUSE_TAG_PREFIX, payload)) {
				constraint->ne_clause_idx = std::stoull(payload);
				constraint->kind = ConstraintKind::USER_MECHANISM;
			}

			// Parse ABS MAXIMIZE upper-bound tag: marks a lower-bound ABS constraint
			// (aux >= inner or aux >= -inner) that needs Big-M upper bounds at finalization.
			if (HasDecideTag(comp.alias, STRUCTURAL_CONSTRAINT_TAG)) {
				constraint->kind = ConstraintKind::STRUCTURAL;
			}
			if (ExtractDecideTagPayload(comp.alias, ABS_UB_POS_TAG_PREFIX, payload)) {
				constraint->abs_aux_idx = std::stoull(payload);
				constraint->abs_is_pos_bound = true;
				constraint->kind = ConstraintKind::STRUCTURAL;
			} else if (ExtractDecideTagPayload(comp.alias, ABS_UB_NEG_TAG_PREFIX, payload)) {
				constraint->abs_aux_idx = std::stoull(payload);
				constraint->abs_is_pos_bound = false;
				constraint->kind = ConstraintKind::STRUCTURAL;
			}

			// Detect easy-direction MIN/MAX optimizer rewrite (see decide.hpp).
			if (HasDecideTag(comp.alias, MINMAX_EASY_REWRITE_TAG)) {
				constraint->was_minmax_easy = true;
			}

			constraint->scope = DecideClauseScope(scope);
			TryParseGroupKeyTag(comp.alias, constraint->group_key);

			// Only binder-generated wrappers over decision algebra are transparent.
			// A data cast is a SQL computation and UnwrapDecideCasts stops at it.
			Expression *lhs = UnwrapDecideCasts(*comp.left, decide_index);

			auto constraint_class = canonicalizer.ClassifyCanonicalComparison(comp);
			if (constraint_class == CanonicalConstraintClass::INVALID) {
				throw InternalException(
				    "DECIDE constraint reached term extraction with invalid aggregate/per-row "
				    "homogeneity: '%s'. Canonical validation must reject this during planning.",
				    comp.ToString());
			}
			if (constraint_class == CanonicalConstraintClass::VALUE) {
				throw InternalException("DECIDE constraint in the VALUE shape reached term extraction: the value "
				                        "rewrite did not replace its reducers in '%s'.",
				                        comp.ToString());
			}
			// `per K:` makes a per-row clause one row per class. Over the aggregate shape, and
			// over the easy MIN/MAX whose rows are its members, the key only names the
			// classes a group's row stands for.
			if (constraint->scope.per != DecideClauseScope::PerRole::NONE && !constraint->was_minmax_easy &&
			    constraint_class != CanonicalConstraintClass::AGGREGATE) {
				constraint->scope.per = DecideClauseScope::PerRole::ONE_ROW_PER_CLASS;
			}

			if (constraint->was_minmax_easy) {
				// The easy direction of a MIN/MAX (`max(when c: e) by (g) <= cap`)
				// holds when every member of its reducer does: one row per member, the
				// reducer's `when` choosing the members, while the bound is reduced over
				// each `by` group of every kept row. RewriteMinMax kept the reducer so its
				// `when` and `by` reach this point; a factor on it distributes over the rows.
				constraint->lhs_is_aggregate = false;
				ExtractEasyExtremumTerms(*lhs, *constraint);
			} else if (constraint_class == CanonicalConstraintClass::AGGREGATE) {
				// Aggregate constraint. Handles both legacy single aggregates and
				// additive aggregate expressions whose reducers carry their own `when`. The
				// classification comes from the canonical boundary rather than aggregate
				// presence alone, so a data-only RHS reducer cannot change row semantics.
				constraint->lhs_is_aggregate = true;
				ExtractAggregateConstraintTerms(*lhs, *constraint);
			} else {
				// Per-row constraint (e.g., x <= 5, or multi-variable: d >= x - c)
				constraint->lhs_is_aggregate = false;

				if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					// Simple single-variable constraint (e.g., x <= 5)
					constraint->lhs_terms.push_back(
					    DecideTerm {FindDecideVariable(*lhs),
					                make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
				} else {
					// Multi-variable per-row constraint with complex LHS
					// (e.g., z_0 + z_1 = 1, or x + (-3)*z_0 + (-5)*z_1 = 0,
					//  or POWER(x - target, 2) <= K quadratic constraint)
					ExtractConstraintTerms(*lhs, *constraint, 1);
				}
			}
			// A bound on one decision, a per-row one or an easy MIN/MAX on each member it reads, folds into the
			// decision's column on each row it emits (stage 06).
			DecideSingletonRow bound;
			if (!folded && !constraint->lhs_is_aggregate && DecideReadSingletonRow(context, op, comp, bound)) {
				constraint->admitted_lower = std::move(bound.lower);
				constraint->admitted_upper = std::move(bound.upper);
				constraint->admitted_a = std::move(bound.a);
				constraint->admitted_r = std::move(bound.r);
				constraint->admitted_member = std::move(bound.member);
				// A bound the folding pass did not enter (a `when`, a reducer filter) that lowers the default floor
				// signs its decision on the rows the clause keeps (stage 06, ResolveLiftedFloors).
				if (bound.LiftsDefaultFloor()) {
					constraint->lifted_floor = bound.var_idx;
					constraint->lifted_floor_k = bound.written_k;
				}
			}

			(folded ? op.prepared.singleton_row_constraints : op.prepared.constraints).push_back(std::move(constraint));
			break;
		}

		default:
			break;
		}
	}

	//===------------------------------------------------------------------===//
	// Objective
	//===------------------------------------------------------------------===//

	//! A SUM argument, split by the shared splitter into the objective's linear terms and one product group per term of
	//! degree 2.
	void ExtractObjectiveBodyTerms(const Expression &expr, DecideObjective &obj, int sign,
	                               const Expression *filter = nullptr) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, sign, split);
		for (auto &term : split) {
			if (term.opaque) {
				ThrowOpaqueTerm(term);
			}
			if (term.degree > 1) {
				obj.product_groups.push_back(ReadProductGroup(term));
				if (filter) {
					obj.product_groups.back().filter = filter->Copy();
				}
				continue;
			}
			DecideTerm linear {term.var_a, std::move(term.coefficient), term.sign};
			if (filter) {
				linear.filter = filter->Copy();
			}
			obj.terms.push_back(std::move(linear));
		}
	}

	//! The objective, one canonical atom at a time, as for an aggregate constraint's left side.
	void ExtractAggregateObjectiveTerms(const Expression &objective_expr, DecideObjective &obj) {
		for (auto &atom : ReadCanonicalAtoms(objective_expr, decide_index)) {
			auto &expr = *atom.term;
			// A factor left outside a reducer (`2 * SUM(x*p)`): extract the reducer, then
			// multiply the factor into everything it produced.
			if (atom.scaled) {
				auto obj_scale = atom.scale.scale;
				idx_t linear_before = obj.terms.size();
				idx_t group_before = obj.product_groups.size();
				ExtractReducerObjectiveTerms(*atom.scale.aggregate, obj, atom.sign);
				ApplyScaleToExtracted(*atom.scale.function, *obj_scale, atom.scale.divides, obj.terms,
				                      obj.product_groups, linear_before, group_before);
				continue;
			}
			// The objective reads a decision outside its reducers once, on the first kept row
			// (a scope check made sure it has one instance there), a query-wide one included.
			// A product of decisions has no once-only reading in the model -- except a product
			// of two query-wide decisions (`s * u`), one `NONE` term execution counts once
			// (CountOnce in physical_decide.cpp).
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
				idx_t group_before = obj.product_groups.size();
				ExtractObjectiveBodyTerms(expr, obj, atom.sign, nullptr);
				bool square = false;
				for (idx_t i = group_before; i < obj.product_groups.size(); i++) {
					square = square || obj.product_groups[i].square;
				}
				if (square || (obj.product_groups.size() != group_before && !ReadsOnlyQueryWideDecisions(expr))) {
					throw InvalidInputException(
					    "DECIDE objective: '%s' multiplies decisions outside a reducer, which is not supported yet",
					    RenderDecideSource(expr, op.source_fragments, op.keyed_scopes, &op.keys));
				}
				continue;
			}
			ExtractReducerObjectiveTerms(expr.Cast<BoundAggregateExpression>(), obj, atom.sign);
		}
	}

	void ExtractReducerObjectiveTerms(const BoundAggregateExpression &agg, DecideObjective &obj, int sign) {
		auto agg_name = StringUtil::Lower(agg.function.name);
		if (agg_name != "sum") {
			throw InvalidInputException(
			    "DECIDE optimizer should rewrite objective aggregate '%s' to SUM before execution",
			    agg.function.name);
		}
		idx_t before = obj.terms.size();
		idx_t group_before = obj.product_groups.size();
		ExtractObjectiveBodyTerms(*agg.children[0], obj, sign, agg.filter.get());
		ApplyAggregateMetadata(obj.terms, before, agg);
		ApplyAggregateMetadata(obj.product_groups, group_before, agg);
	}

	void AnalyzeObjective(const unique_ptr<Expression> &expr_ptr) {
		auto *expr = UnwrapDecideCasts(*expr_ptr, decide_index);

		// DeciDB: Check for PER wrapper on objective (outermost layer)
		idx_t two_level_key = DConstants::INVALID_INDEX;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			if (IsTwoLevelObjectiveWrapper(conj)) {
				// `per ()` groups nothing: the objective is read once.
				two_level_key = PerWrapperKey(conj);
				expr = UnwrapDecideCasts(*conj.children[0], decide_index);
			}
		}

		// DeciDB: Check for WHEN wrapper on objective (inside PER, if present)
		unique_ptr<Expression> when_cond;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			when_cond = conj.children[1]->Copy();
			expr = UnwrapDecideCasts(*conj.children[0], decide_index);
		}

		auto &objective = op.prepared.objective;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto &agg = expr->Cast<BoundAggregateExpression>();

			objective = make_uniq<DecideObjective>();

			// Walk the SUM argument: its linear terms and its product groups, a mixed body
			// (SUM(POWER(x-t, 2) + penalty*x)) included.
			ExtractObjectiveBodyTerms(*agg.children[0], *objective, 1, agg.filter.get());
			ApplyAggregateMetadata(objective->terms, 0, agg);
			ApplyAggregateMetadata(objective->product_groups, 0, agg);

			objective->when_condition = std::move(when_cond);
			objective->two_level_key = two_level_key;
		} else if (BoundExpressionContainsAggregate(*expr) || FindDecideVariable(*expr) != DConstants::INVALID_INDEX) {
			// The second arm covers an objective with no aggregate at all: decisions
			// outside every reducer (e.g. `minimize max_shortfall`), read once.
			objective = make_uniq<DecideObjective>();
			ExtractAggregateObjectiveTerms(*expr, *objective);
			objective->when_condition = std::move(when_cond);
			objective->two_level_key = two_level_key;
		}
	}

private:
	ClientContext &context;
	LogicalDecide &op;
	idx_t decide_index;
	DecideTermSplitter splitter;
	DecideCanonicalizer canonicalizer;
};

void BuildDecidePreparedModel(ClientContext &context, LogicalDecide &decide) {
	DecideLinearFormBuilder builder(context, decide);
	builder.Build();
}

} // namespace duckdb
