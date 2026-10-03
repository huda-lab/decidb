#include "duckdb/optimizer/decide/decide_linear_form.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
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
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
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

//! The solver path reads what survives lowering: constant, linear, product and square terms. An unknown term is
//! raised as the error the split recorded for it.
[[noreturn]] static void ThrowUnknownTerm(const DecideSplitTerm &term) {
	if (term.user_error) {
		throw InvalidInputException(term.reason);
	}
	throw InternalException(term.reason);
}

//! Terms of an expression that must be linear, as prepared terms.
static void AppendLinearTerms(vector<DecideSplitTerm> &split, vector<DecideTerm> &out) {
	for (auto &term : split) {
		if (term.kind == DecideTermKind::UNKNOWN) {
			ThrowUnknownTerm(term);
		}
		if (term.kind != DecideTermKind::CONSTANT && term.kind != DecideTermKind::LINEAR) {
			throw InternalException("DECIDE linear expression split into a non-linear term");
		}
		out.push_back(DecideTerm {term.var_a, std::move(term.coefficient), term.sign});
	}
}

//! Collect DECIDE variable references from a bound expression, tracking sign
//! through subtraction operators. Used for multi-variable per-row constraints.
struct ExprVarRef {
	idx_t var_idx;
	int sign; // +1 or -1
};

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
	      canonicalizer(context, op.decide_index, op.variable_scopes),
	      splitter(context, op.decide_index, op.decide_variables) {
	}

	void Build() {
		if (op.decide_constraints) {
			AnalyzeConstraint(op.decide_constraints);
		}
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

		// Group like terms, once, over every list that becomes a linear solver row.
		for (auto &constraint : op.prepared.constraints) {
			CollectLikeTerms(constraint->lhs_terms);
		}
		if (op.prepared.objective) {
			CollectLikeTerms(op.prepared.objective->terms);
		}
		for (auto &spec : op.composed_minmax_constraints) {
			for (auto &term : spec.terms) {
				CollectLikeTerms(term.inner_terms);
			}
		}
		for (auto &term : op.composed_minmax_objective_terms) {
			CollectLikeTerms(term.inner_terms);
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
		splitter.SplitLinear(expr, split);
		AppendLinearTerms(split, out);
	}

	void CollectDecideVarRefs(const Expression &expr, int sign, vector<ExprVarRef> &refs) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			idx_t var_idx = FindDecideVariable(expr);
			if (var_idx != DConstants::INVALID_INDEX) {
				refs.push_back({var_idx, sign});
			}
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();
			if (func.function.name == "-" && func.children.size() == 2) {
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], -sign, refs);
				return;
			}
			if (func.function.name == "+" && func.children.size() == 2) {
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], sign, refs);
				return;
			}
			if (func.function.name == "*" && func.children.size() == 2) {
				// Multiplication: descend into both children to find decide variables.
				// Sign propagates unchanged — * doesn't flip algebraic sign, it changes
				// the coefficient magnitude, which this walk does not measure.
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], sign, refs);
				return;
			}
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			CollectDecideVarRefs(*cast.child, sign, refs);
			return;
		}
		// Constants, data columns, etc.: no DECIDE vars
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
			if (!found && child && BoundExpressionContainsAggregate(*child)) {
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
	//! - `filter` is the aggregate-local `WHEN`. `SUM(x) WHEN a` and `SUM(x) WHEN b`
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
		if (a.variable_index == DConstants::INVALID_INDEX || a.variable_index != b.variable_index) {
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

	//! Sum like terms into one, in place, preserving first-occurrence order.
	//!
	//! `2*ship + 3*ship` used to reach the solver as two terms naming one column. The
	//! model builder folded the duplicate when writing the matrix row, so the emitted
	//! model was always right -- but every other consumer had to remember that an
	//! index can repeat, and one of them did not: the implied-bound derivation read a
	//! single term's coefficient instead of the sum (fixed 2026-08-15 by a defensive
	//! accumulate, which stays). Collecting here removes the trap at its source.
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
			vector<unique_ptr<Expression>> children;
			children.push_back(std::move(out[target].coefficient));
			children.push_back(std::move(term.coefficient));
			out[target].coefficient =
			    DecideRebindOperator(context, out[target].sign == term.sign ? "+" : "-", std::move(children));
		}
		terms = std::move(out);
	}

	//===------------------------------------------------------------------===//
	// Reducer metadata and peeled scales
	//===------------------------------------------------------------------===//

	//! Entity scope a reducer is qualified by (`sum(D: ...)`), read back from the tag the
	//! binder stamped on the aggregate; INVALID_INDEX when the reducer is unqualified.
	static idx_t QualifierScopeOf(const BoundAggregateExpression &agg) {
		idx_t scope_idx = DConstants::INVALID_INDEX;
		TryParseQualifiedReducerTag(agg.alias, scope_idx);
		return scope_idx;
	}

	static void ApplyAggregateMetadata(vector<DecideTerm> &terms, idx_t begin, const BoundAggregateExpression &agg) {
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);
		for (idx_t i = begin; i < terms.size(); i++) {
			if (agg.filter) {
				terms[i].filter = agg.filter->Copy();
			}
			terms[i].avg_scale = is_avg;
			terms[i].qualifier_scope_idx = qualifier_scope;
			terms[i].reduction = LinearTermReduction::SUM;
		}
	}

	//! Name an expression the way the user wrote it: strip the casts the binder added.
	static string ScaleUserName(const Expression &expr) {
		const Expression *cur = StripCastsForIdentity(expr);
		auto name = StripDecideTags(cur->GetName());
		return name.empty() ? cur->ToString() : name;
	}

	//! Multiply a coefficient by a factor that stayed outside a reducer, keeping the
	//! operand types the original `*` / `/` node was bound for and then binding the
	//! rebuilt node through `FunctionBinder`. Casting first preserves the division
	//! semantics the user's expression was bound with; binding after is what removes
	//! the need to reuse another node's `FunctionData`.
	unique_ptr<Expression> ScaleCoefficient(const BoundFunctionExpression &scale_func, const Expression &scale,
	                                        bool divides, unique_ptr<Expression> coef) const {
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
		}
		return DecideRebindOperator(context, divides ? "/" : "*", std::move(children));
	}

	//! Multiply everything the aggregate under a peeled scale just produced by that
	//! scale. Folding the factor into the reducer's body is what the canonicalizer
	//! refuses to do, because at the parsed level the aggregate may still be MIN/MAX
	//! and `MAX(-2x)` is `-2*MIN(x)`, not `-2*MAX(x)`. Here it is safe and exact: the
	//! optimizer has already rewritten every MIN/MAX to SUM (asserted below), and a
	//! sum distributes over any factor regardless of sign.
	void ApplyScaleToExtracted(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                           DecideConstraint &constraint, idx_t linear_before, idx_t bilinear_before,
	                           idx_t quadratic_before) {
		auto scaled = [&](unique_ptr<Expression> coef) {
			return ScaleCoefficient(scale_func, scale, divides, std::move(coef));
		};
		for (idx_t i = linear_before; i < constraint.lhs_terms.size(); i++) {
			constraint.lhs_terms[i].coefficient = scaled(std::move(constraint.lhs_terms[i].coefficient));
		}
		for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
			auto &bt = constraint.bilinear_terms[i];
			// A null coefficient means 1.0; the scale becomes the whole coefficient.
			bt.coefficient = bt.coefficient
			                     ? scaled(std::move(bt.coefficient))
			                     : scaled(make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1)));
		}
		if (quadratic_before == constraint.quadratic_groups.size()) {
			return;
		}
		// Fold a literal immediately. A query-wide subquery scale cannot be evaluated
		// until the relational input has run, so retain it on the quadratic group.
		if (!scale.IsFoldable()) {
			for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
				constraint.quadratic_groups[i].scale = scale.Copy();
				constraint.quadratic_groups[i].scale_divides = divides;
			}
			return;
		}
		double factor =
		    ExpressionExecutor::EvaluateScalar(context, scale).DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		if (divides && factor == 0.0) {
			throw InvalidInputException("DECIDE constraint: division by zero in a squared term.");
		}
		for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
			constraint.quadratic_groups[i].sign *= divides ? 1.0 / factor : factor;
		}
	}

	//! Objective twin of ApplyScaleToExtracted: multiply a factor that stayed outside a
	//! reducer into everything the reducer produced. `quadratic_sign` is a number
	//! rather than an expression, so a squared term needs the factor's value here.
	void ApplyScaleToObjective(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                           DecideObjective &obj, idx_t linear_before, idx_t bilinear_before, idx_t squared_before) {
		auto scaled = [&](unique_ptr<Expression> coef) {
			return ScaleCoefficient(scale_func, scale, divides, std::move(coef));
		};
		for (idx_t i = linear_before; i < obj.terms.size(); i++) {
			obj.terms[i].coefficient = scaled(std::move(obj.terms[i].coefficient));
		}
		for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
			auto &bt = obj.bilinear_terms[i];
			bt.coefficient = bt.coefficient
			                     ? scaled(std::move(bt.coefficient))
			                     : scaled(make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1)));
		}
		if (squared_before == obj.squared_terms.size()) {
			return;
		}
		double factor;
		if (!TryEvaluateFoldableDouble(context, scale, factor)) {
			throw InvalidInputException(
			    "DECIDE objective: a squared term cannot be multiplied by '%s', whose value is "
			    "not known until the query runs. Use a constant factor, or move it inside the "
			    "aggregate as SUM(%s * POWER(...)).",
			    scale.GetName(), scale.GetName());
		}
		if (divides && factor == 0.0) {
			throw InvalidInputException("DECIDE objective: division by zero in a squared term.");
		}
		obj.quadratic_sign *= divides ? 1.0 / factor : factor;
	}

	//! True when the decision referenced by `expr` is query-wide (`scalar`).
	//! Such a term is a complete objective contribution on its own: it maps to a
	//! single solver column, so there is no reducer to collapse it.
	bool IsScalarDecideTerm(const Expression &expr) const {
		idx_t var_idx = FindDecideVariable(expr);
		return var_idx != DConstants::INVALID_INDEX && var_idx < op.variable_scopes.size() &&
		       op.variable_scopes[var_idx].IsScalar();
	}

	//===------------------------------------------------------------------===//
	// Constraints
	//===------------------------------------------------------------------===//

	//! The left side of an aggregate constraint, one canonical atom at a time: a reducer, a reducer with the
	//! factor canonicalization left on it, a query-wide decision, or a data cast kept as a typed fixed term.
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
					throw InternalException(
					    "DECIDE constraint: '%s' is a decision, so it cannot multiply an "
					    "aggregate. Only constants and query-wide values can scale "
					    "SUM/AVG/MIN/MAX.",
					    ScaleUserName(*scale));
				}
				idx_t linear_before = constraint.lhs_terms.size();
				idx_t bilinear_before = constraint.bilinear_terms.size();
				idx_t quadratic_before = constraint.quadratic_groups.size();
				ExtractReducerConstraintTerms(*atom.scale.aggregate, constraint, atom.sign);
				ApplyScaleToExtracted(*atom.scale.function, *scale, atom.scale.divides, constraint, linear_before,
				                      bilinear_before, quadratic_before);
				continue;
			}
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST &&
			    FindDecideVariable(expr) == DConstants::INVALID_INDEX) {
				ExtractConstraintTerms(expr, constraint, atom.sign);
				continue;
			}
			// A query-wide (`scalar`) decision standing alone (not inside a reducer) is
			// row-invariant, so it is a complete term of an aggregate constraint on its own --
			// there is nothing for a reducer to collapse. This is K3's "reducer or
			// row-invariant" rule. The BOUND_AGGREGATE exclusion matters: a scalar may also
			// sit *inside* a reducer body (`SUM(cost * cap)`), and that case must reach the
			// SUM extraction below so the coefficient is evaluated per row instead of being
			// read as a bare `cap` with coefficient 1.
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE && IsScalarDecideTerm(expr)) {
				ExtractConstraintTerms(expr, constraint, atom.sign);
				continue;
			}
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
				throw InternalException("DECIDE aggregate constraint LHS contains a non-reducer term after canonical "
				                        "verification: %s",
				                        expr.ToString());
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
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);

		idx_t linear_before = constraint.lhs_terms.size();
		idx_t bilinear_before = constraint.bilinear_terms.size();
		idx_t quadratic_before = constraint.quadratic_groups.size();
		ExtractConstraintTerms(*agg.children[0], constraint, sign);
		ApplyAggregateMetadata(constraint.lhs_terms, linear_before, agg);
		for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
			if (agg.filter) {
				constraint.bilinear_terms[i].filter = agg.filter->Copy();
			}
			constraint.bilinear_terms[i].avg_scale = is_avg;
			constraint.bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
		}
		for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
			if (agg.filter) {
				constraint.quadratic_groups[i].filter = agg.filter->Copy();
			}
			constraint.quadratic_groups[i].avg_scale = is_avg;
			constraint.quadratic_groups[i].qualifier_scope_idx = qualifier_scope;
		}

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

	//! A reducer body or a per-row left side, split by the shared splitter into the constraint's linear terms,
	//! bilinear terms, and one quadratic group per square.
	void ExtractConstraintTerms(const Expression &expr, DecideConstraint &constr, int sign) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, sign, split);
		for (auto &term : split) {
			switch (term.kind) {
			case DecideTermKind::CONSTANT:
			case DecideTermKind::LINEAR:
				constr.lhs_terms.push_back(DecideTerm {term.var_a, std::move(term.coefficient), term.sign});
				break;
			case DecideTermKind::PRODUCT: {
				BilinearConstraintTerm bt;
				bt.var_a = term.var_a;
				bt.var_b = term.var_b;
				bt.coefficient = std::move(term.coefficient);
				bt.sign = term.sign;
				constr.bilinear_terms.push_back(std::move(bt));
				constr.has_bilinear = true;
				break;
			}
			case DecideTermKind::SQUARE: {
				DecideConstraint::QuadraticGroup qg;
				qg.sign = static_cast<double>(term.sign) * term.square_scale;
				AppendLinearTerms(term.inner, qg.inner_terms);
				constr.quadratic_groups.push_back(std::move(qg));
				constr.has_quadratic = true;
				break;
			}
			case DecideTermKind::ABS:
				throw InternalException("DECIDE ABS reached term extraction; RewriteAbs lowers it first");
			case DecideTermKind::UNKNOWN:
				ThrowUnknownTerm(term);
			}
		}
	}

	void AnalyzeConstraint(const unique_ptr<Expression> &expr_ptr, unique_ptr<Expression> when_condition = nullptr,
	                       vector<unique_ptr<Expression>> per_columns = {}) {
		auto &expr = *expr_ptr;
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_CONJUNCTION: {
			auto &conj = expr.Cast<BoundConjunctionExpression>();
			// DecidB: PER wrapper — outermost layer
			if (IsPerConstraintWrapper(conj) && conj.children.size() >= 2) {
				// child[0] = the constraint (possibly WHEN-wrapped)
				// children[1..N] = the PER column expressions
				vector<unique_ptr<Expression>> per_cols;
				for (idx_t i = 1; i < conj.children.size(); i++) {
					per_cols.push_back(conj.children[i]->Copy());
				}
				AnalyzeConstraint(conj.children[0], std::move(when_condition), std::move(per_cols));
				break;
			}
			// DecidB: Check if this is a WHEN constraint wrapper
			if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
				// child[0] = the actual constraint, child[1] = the WHEN condition
				AnalyzeConstraint(conj.children[0], conj.children[1]->Copy(), std::move(per_columns));
				break;
			}
			// Regular conjunction: recursively analyze each child
			for (auto &child : conj.children) {
				AnalyzeConstraint(child);
			}
			break;
		}

		case ExpressionClass::BOUND_COMPARISON: {
			auto &comp = expr.Cast<BoundComparisonExpression>();

			// Skip comparisons the optimizer already folded into the column box.
			// Emitting a DecideConstraint here would add num_rows redundant model
			// rows. The comparison is still in the tree so EXPLAIN renders it; the
			// tag is the decision, and it was made by AbsorbVariableBounds.
			if (HasDecideTag(comp.alias, ABSORBED_BOUND_TAG)) {
				break;
			}

			auto constraint = make_uniq<DecideConstraint>();
			constraint->comparison_type = comp.type;
			constraint->rhs_expr = comp.right->Copy();
			TryParseSourceClauseTag(comp.GetAlias(), constraint->source_clause_id);
			TryParseRemovalGroupTag(comp.GetAlias(), constraint->removal_group_id);

			// Parse not-equal indicator tag if present
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

			// DecidB: Store WHEN condition and PER columns if present
			if (when_condition) {
				constraint->when_condition = std::move(when_condition);
			}
			if (!per_columns.empty()) {
				constraint->per_columns = std::move(per_columns);
			}

			// Extract terms from LHS
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

			if (constraint_class == CanonicalConstraintClass::AGGREGATE) {
				// Aggregate constraint. Handles both legacy single aggregates and
				// additive aggregate expressions with aggregate-local WHEN filters. The
				// classification comes from the canonical boundary rather than aggregate
				// presence alone, so a data-only RHS reducer cannot change row semantics.
				constraint->lhs_is_aggregate = true;
				ExtractAggregateConstraintTerms(*lhs, *constraint);
			} else {
				// Per-row constraint (e.g., x <= 5, or multi-variable: d >= x - c)
				constraint->lhs_is_aggregate = false;

				// K1 guard. DecideCanonicalizer puts every decision-bearing term on
				// the left, so a decision variable reaching the RHS here means the
				// invariant was broken upstream -- by a new optimizer rewrite that
				// mutates a constraint in place instead of going through
				// LogicalDecide::AddConstraint, most likely. This used to be a second
				// implementation of the partition (the canonicalization refactor); it was
				// verified unreachable across the golden corpus and the full suite
				// before being replaced by the check, so a wrong answer here would
				// otherwise be silent.
				vector<ExprVarRef> rhs_refs;
				CollectDecideVarRefs(*comp.right, +1, rhs_refs);
				if (!rhs_refs.empty()) {
					throw InternalException(
					    "DECIDE constraint is not canonical: decision variable on the right-hand "
					    "side of '%s'. Constraints must be canonicalized by DecideCanonicalizer "
					    "before reaching term extraction.",
					    comp.right->ToString());
				}

				if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					// Simple single-variable constraint (e.g., x <= 5)
					idx_t var_idx = FindDecideVariable(*lhs);
					if (var_idx != DConstants::INVALID_INDEX) {
						constraint->lhs_terms.push_back(DecideTerm {
						    var_idx, make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
					}
				} else {
					// Multi-variable per-row constraint with complex LHS
					// (e.g., z_0 + z_1 = 1, or x + (-3)*z_0 + (-5)*z_1 = 0,
					//  or POWER(x - target, 2) <= K quadratic constraint)
					ExtractConstraintTerms(*lhs, *constraint, 1);
				}
			}

			op.prepared.constraints.push_back(std::move(constraint));
			break;
		}

		default:
			break;
		}
	}

	//===------------------------------------------------------------------===//
	// Objective
	//===------------------------------------------------------------------===//

	//! A SUM argument, split by the shared splitter into the objective's linear, bilinear and squared terms. The
	//! objective supports exactly one quadratic group (one Q matrix with one sign); a second is refused.
	void ExtractLinearAndBilinearTerms(const Expression &expr, DecideObjective &obj, int sign,
	                                   const Expression *filter = nullptr) {
		vector<DecideSplitTerm> split;
		splitter.Split(expr, sign, split);
		for (auto &term : split) {
			switch (term.kind) {
			case DecideTermKind::CONSTANT:
			case DecideTermKind::LINEAR: {
				DecideTerm linear {term.var_a, std::move(term.coefficient), term.sign};
				if (filter) {
					linear.filter = filter->Copy();
				}
				obj.terms.push_back(std::move(linear));
				break;
			}
			case DecideTermKind::PRODUCT: {
				DecideObjective::BilinearTerm bt;
				bt.var_a = term.var_a;
				bt.var_b = term.var_b;
				bt.coefficient = std::move(term.coefficient);
				bt.sign = term.sign;
				if (filter) {
					bt.filter = filter->Copy();
				}
				obj.bilinear_terms.push_back(std::move(bt));
				obj.has_bilinear = true;
				break;
			}
			case DecideTermKind::SQUARE: {
				if (obj.has_quadratic) {
					throw InvalidInputException(
					    "DECIDE objective contains multiple quadratic (POWER / (expr)*(expr)) "
					    "groups. Only a single quadratic group plus linear terms is supported; "
					    "combine them mathematically or rewrite the objective.");
				}
				obj.has_quadratic = true;
				obj.quadratic_sign = term.square_scale * static_cast<double>(term.sign);
				idx_t before = obj.squared_terms.size();
				AppendLinearTerms(term.inner, obj.squared_terms);
				if (filter) {
					for (idx_t i = before; i < obj.squared_terms.size(); i++) {
						obj.squared_terms[i].filter = filter->Copy();
					}
				}
				break;
			}
			case DecideTermKind::ABS:
				throw InternalException("DECIDE ABS reached term extraction; RewriteAbs lowers it first");
			case DecideTermKind::UNKNOWN:
				ThrowUnknownTerm(term);
			}
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
				// A decision on both sides of the `*` is a product of two decisions
				// (bilinear), not a scaled reducer; treating it as a coefficient reads a
				// decision column as data and crashes in evaluation.
				if (FindDecideVariable(*obj_scale) != DConstants::INVALID_INDEX) {
					throw InvalidInputException("DECIDE objective: '%s' is a decision, so it cannot multiply an "
					                            "aggregate. Only constants and query-wide values can scale "
					                            "SUM/AVG/MIN/MAX.",
					                            ScaleUserName(*obj_scale));
				}
				idx_t linear_before = obj.terms.size();
				idx_t bilinear_before = obj.bilinear_terms.size();
				idx_t squared_before = obj.squared_terms.size();
				ExtractReducerObjectiveTerms(*atom.scale.aggregate, obj, atom.sign);
				ApplyScaleToObjective(*atom.scale.function, *obj_scale, atom.scale.divides, obj, linear_before,
				                      bilinear_before, squared_before);
				continue;
			}
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST &&
			    FindDecideVariable(expr) == DConstants::INVALID_INDEX) {
				ExtractLinearAndBilinearTerms(expr, obj, atom.sign);
				continue;
			}
			// A query-wide decision standing alone (not inside a reducer) contributes without
			// one. Excludes BOUND_AGGREGATE for the same reason as the constraint side.
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE && IsScalarDecideTerm(expr)) {
				ExtractLinearAndBilinearTerms(expr, obj, atom.sign, nullptr);
				continue;
			}
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
				throw InvalidInputException(
				    "DECIDE objective contains a non-aggregate term: %s.\n"
				    "The objective must be a SUM/MIN/MAX/AVG of an expression in decision variables.\n"
				    "If you wrapped an aggregate inside another function (e.g. POWER(AVG(x), 2)), "
				    "use the supported shape SUM(POWER(x, 2)) instead.",
				    expr.ToString());
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
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);

		idx_t before = obj.terms.size();
		idx_t bilinear_before = obj.bilinear_terms.size();
		idx_t squared_before = obj.squared_terms.size();
		ExtractLinearAndBilinearTerms(*agg.children[0], obj, sign, agg.filter.get());
		for (idx_t i = before; i < obj.terms.size(); i++) {
			obj.terms[i].avg_scale = is_avg;
			obj.terms[i].qualifier_scope_idx = qualifier_scope;
			obj.terms[i].reduction = LinearTermReduction::SUM;
		}
		for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
			obj.bilinear_terms[i].avg_scale = is_avg;
			obj.bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
		}
		for (idx_t i = squared_before; i < obj.squared_terms.size(); i++) {
			obj.squared_terms[i].avg_scale = is_avg;
			obj.squared_terms[i].qualifier_scope_idx = qualifier_scope;
		}
	}

	void AnalyzeObjective(const unique_ptr<Expression> &expr_ptr) {
		auto *expr = UnwrapDecideCasts(*expr_ptr, decide_index);

		// DecidB: Check for PER wrapper on objective (outermost layer)
		vector<unique_ptr<Expression>> per_cols;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			if (IsPerConstraintWrapper(conj) && conj.children.size() >= 2) {
				for (idx_t i = 1; i < conj.children.size(); i++) {
					per_cols.push_back(conj.children[i]->Copy());
				}
				expr = UnwrapDecideCasts(*conj.children[0], decide_index);
			}
		}

		// DecidB: Check for WHEN wrapper on objective (inside PER, if present)
		unique_ptr<Expression> when_cond;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
				when_cond = conj.children[1]->Copy();
				// Unwrap to get the actual objective expression
				expr = UnwrapDecideCasts(*conj.children[0], decide_index);
			}
		}

		auto &objective = op.prepared.objective;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto &agg = expr->Cast<BoundAggregateExpression>();

			objective = make_uniq<DecideObjective>();

			// Walk the SUM argument. ExtractLinearAndBilinearTerms recognises
			// quadratic patterns (POWER/(expr)*(expr)/negated/K*POWER) at any
			// position in `+`/`-` trees and routes them into squared_terms, so
			// the same walker handles pure QP, pure linear+bilinear, and the
			// mixed forms (e.g. SUM(POWER(x-t, 2) + penalty*x)) uniformly.
			idx_t before = objective->terms.size();
			idx_t bilinear_before = objective->bilinear_terms.size();
			idx_t squared_before = objective->squared_terms.size();
			ExtractLinearAndBilinearTerms(*agg.children[0], *objective, 1, agg.filter.get());
			bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
			idx_t qualifier_scope = QualifierScopeOf(agg);
			for (idx_t i = before; i < objective->terms.size(); i++) {
				objective->terms[i].avg_scale = is_avg;
				objective->terms[i].qualifier_scope_idx = qualifier_scope;
				objective->terms[i].reduction = LinearTermReduction::SUM;
			}
			for (idx_t i = bilinear_before; i < objective->bilinear_terms.size(); i++) {
				objective->bilinear_terms[i].avg_scale = is_avg;
				objective->bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
			}
			for (idx_t i = squared_before; i < objective->squared_terms.size(); i++) {
				objective->squared_terms[i].avg_scale = is_avg;
				objective->squared_terms[i].qualifier_scope_idx = qualifier_scope;
			}

			objective->when_condition = std::move(when_cond);
			objective->per_columns = std::move(per_cols);
		} else if (BoundExpressionContainsAggregate(*expr) || IsScalarDecideTerm(*expr)) {
			// The second arm covers an objective made only of query-wide decisions
			// (e.g. `minimize max_shortfall`), which carries no aggregate at all.
			objective = make_uniq<DecideObjective>();
			ExtractAggregateObjectiveTerms(*expr, *objective);
			objective->when_condition = std::move(when_cond);
			objective->per_columns = std::move(per_cols);
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
