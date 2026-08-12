#include "duckdb/planner/decide/decide_canonicalizer.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

DecideCanonicalizer::DecideCanonicalizer(ClientContext &context, idx_t decide_index)
    : context(context), decide_index(decide_index), judge_column_refs(false) {
}

DecideCanonicalizer::DecideCanonicalizer(ClientContext &context, idx_t decide_index,
                                         unordered_set<idx_t> query_wide_table_indexes,
                                         unordered_set<idx_t> correlated_subquery_table_indexes)
    : context(context), decide_index(decide_index),
      query_wide_table_indexes(std::move(query_wide_table_indexes)),
      correlated_subquery_table_indexes(std::move(correlated_subquery_table_indexes)),
      judge_column_refs(true) {
}

bool DecideCanonicalizer::ReferencesDecideVar(const Expression &expr) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return colref.binding.table_index == decide_index;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ReferencesDecideVar(child);
		}
	});
	return found;
}

bool DecideCanonicalizer::ContainsReducer(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsReducer(child);
		}
	});
	return found;
}

DecideCanonicalizer::Placement DecideCanonicalizer::Classify(const Expression &expr) const {
	if (ReferencesDecideVar(expr)) {
		return Placement::LEFT;
	}
	// Everything decision-free is a bound, including a data-only reducer. The left
	// side has no aggregate evaluator -- FixedLinearLhsOffset merely SUMS a term's
	// column over the group, which is why only SUM and AVG could ever be hoisted
	// there and MIN/MAX/COUNT were refused. The right side reduces per group by
	// kind (EvaluateRhsReducerPerGroup), so that is where a reducer belongs.
	return Placement::RIGHT;
}

bool DecideCanonicalizer::IsWideningNumericCast(const LogicalType &from, const LogicalType &to) {
	if (!from.IsNumeric() || !to.IsNumeric()) {
		return false;
	}
	if (from == to) {
		return true;
	}

	struct IntegralRange {
		uint8_t bits;
		uint8_t decimal_digits;
		bool is_signed;
	};
	auto get_integral_range = [](const LogicalType &type, IntegralRange &range) {
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
			range = {8, 3, true};
			return true;
		case LogicalTypeId::SMALLINT:
			range = {16, 5, true};
			return true;
		case LogicalTypeId::INTEGER:
			range = {32, 10, true};
			return true;
		case LogicalTypeId::BIGINT:
			range = {64, 19, true};
			return true;
		case LogicalTypeId::HUGEINT:
			range = {128, 39, true};
			return true;
		case LogicalTypeId::UTINYINT:
			range = {8, 3, false};
			return true;
		case LogicalTypeId::USMALLINT:
			range = {16, 5, false};
			return true;
		case LogicalTypeId::UINTEGER:
			range = {32, 10, false};
			return true;
		case LogicalTypeId::UBIGINT:
			range = {64, 20, false};
			return true;
		case LogicalTypeId::UHUGEINT:
			range = {128, 39, false};
			return true;
		default:
			return false;
		}
	};

	IntegralRange from_range {0, 0, false};
	if (get_integral_range(from, from_range)) {
		IntegralRange to_range {0, 0, false};
		if (get_integral_range(to, to_range)) {
			// The target must contain the source's complete range. A signed
			// source never fits an unsigned target; an unsigned source needs one
			// extra bit when crossing to a signed representation.
			if (from_range.is_signed) {
				return to_range.is_signed && to_range.bits >= from_range.bits;
			}
			return to_range.is_signed ? to_range.bits > from_range.bits
			                          : to_range.bits >= from_range.bits;
		}
		if (to.id() == LogicalTypeId::DECIMAL) {
			auto integral_capacity = DecimalType::GetWidth(to) - DecimalType::GetScale(to);
			return integral_capacity >= from_range.decimal_digits;
		}
		// Integer-to-binary-float casts are intentionally not inferred from
		// DuckDB's implicit-cast table. Some small integer types would be exact,
		// but keeping this allow-list conservative makes any unproved case an
		// atomic term rather than risking a changed feasible set.
		return false;
	}

	// IEEE binary32 values are exactly representable in binary64.
	if (from.id() == LogicalTypeId::FLOAT && to.id() == LogicalTypeId::DOUBLE) {
		return true;
	}

	// DECIMAL widening is exact only when the target preserves both the
	// fractional digits and the integral range.
	if (from.id() == LogicalTypeId::DECIMAL && to.id() == LogicalTypeId::DECIMAL) {
		auto from_scale = DecimalType::GetScale(from);
		auto to_scale = DecimalType::GetScale(to);
		auto from_width = DecimalType::GetWidth(from);
		auto to_width = DecimalType::GetWidth(to);
		return to_scale >= from_scale && (to_width - to_scale) >= (from_width - from_scale);
	}
	return false;
}

bool DecideCanonicalizer::IsQueryWideConstant(const Expression &expr) const {
	// A factor on a reducer has to answer "are you one value for the whole query?", and
	// for a column ref that cannot be answered from shape: flattening leaves
	// `(SELECT max(w) FROM p)` looking exactly like `weight`, down to being a
	// BoundColumnRefExpression on a table index of its own.
	//
	// So the answer comes from evidence collected before flattening, and the test is
	// an ALLOW-list: query-wide iff positively marked. Framing it the other way --
	// "not one of the reduced relation's own bindings" -- reads identically for
	// `weight` but silently admits a CORRELATED subquery, which also lands on a fresh
	// table index yet yields a different value per row.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		if (!judge_column_refs) {
			return true; // No evidence available; decline to judge rather than guess.
		}
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return query_wide_table_indexes.find(colref.binding.table_index) !=
		       query_wide_table_indexes.end();
	}
	// An unflattened uncorrelated scalar subquery is one value by definition. A
	// correlated one is genuinely per-row and must not qualify.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subq = expr.Cast<BoundSubqueryExpression>();
		return subq.subquery_type == SubqueryType::SCALAR && !subq.IsCorrelated();
	}
	if (IsQueryWideConstantTag(expr.GetAlias())) {
		return true;
	}
	if (expr.IsFoldable()) {
		return true;
	}
	// Arithmetic over query-wide values is itself query-wide, which is what makes
	// `2 * (SELECT k FROM p) * SUM(x)` work: neither factor folds on its own.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.children.empty()) {
			return false;
		}
		for (auto &child : func.children) {
			if (!IsQueryWideConstant(*child)) {
				return false;
			}
		}
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return IsQueryWideConstant(*expr.Cast<BoundCastExpression>().child);
	}
	return false;
}

//! Name an expression the way the user wrote it, for an error message. Everything the
//! binder added is noise here: `CAST(weight AS DECIMAL(12,1))` is not what anyone
//! typed, and neither is the `FILTER (WHERE w)` that an aggregate-local WHEN becomes.
static string UserFacingName(const Expression &expr) {
	const Expression *cur = &expr;
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<BoundCastExpression>().child.get();
	}
	if (cur->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = cur->Cast<BoundAggregateExpression>();
		if (agg.children.size() == 1) {
			return StringUtil::Upper(agg.function.name) + "(" + UserFacingName(*agg.children[0]) + ")";
		}
		return StringUtil::Upper(agg.function.name) + "(...)";
	}
	auto name = cur->GetName();
	return name.empty() ? cur->ToString() : name;
}

bool DecideCanonicalizer::IsCorrelatedSubqueryRef(const Expression &expr) const {
	const Expression *cur = &expr;
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<BoundCastExpression>().child.get();
	}
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto idx = cur->Cast<BoundColumnRefExpression>().binding.table_index;
	return correlated_subquery_table_indexes.find(idx) != correlated_subquery_table_indexes.end();
}

const Expression &DecideCanonicalizer::PeelScale(const Expression &expr, const Expression *&out_scale,
                                                 bool &out_divides) const {
	out_scale = nullptr;
	out_divides = false;
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return expr;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	bool is_mult = func.function.name == "*";
	bool is_div = func.function.name == "/";
	if ((!is_mult && !is_div) || func.children.size() != 2) {
		return expr;
	}

	// Identify the reducer side. Only a DECISION-BEARING reducer is scaled here: a
	// data-only reducer (`2 * AVG(price)` as a bound) is the RHS evaluator's business,
	// and reaching into it would reject shapes this pass has no stake in.
	auto is_scalable = [&](const Expression &e) {
		return ContainsReducer(e) && ReferencesDecideVar(e);
	};
	const Expression *term = nullptr;
	const Expression *factor = nullptr;
	if (is_mult && is_scalable(*func.children[0]) && !ContainsReducer(*func.children[1])) {
		term = func.children[0].get();
		factor = func.children[1].get();
	} else if (is_mult && is_scalable(*func.children[1]) && !ContainsReducer(*func.children[0])) {
		term = func.children[1].get();
		factor = func.children[0].get();
	} else if (is_div && is_scalable(*func.children[0]) && !ContainsReducer(*func.children[1])) {
		// Only `AGG / K` divides. `K / AGG` is not a scaled reducer at all.
		term = func.children[0].get();
		factor = func.children[1].get();
		out_divides = true;
	} else {
		// Two reducers multiplied, or no reducer at all (`price * x` is an ordinary
		// per-row coefficient). Not a scale; leave it as one term.
		return expr;
	}

	// The single judgement. A reducer collapses many rows to one number, so anything
	// multiplying it must also be one number.
	const char *verb = out_divides ? "divide" : "multiply";
	if (ReferencesDecideVar(*factor)) {
		throw BinderException(
		    "DECIDE constraint: '%s' is a decision, so it cannot %s %s. "
		    "Only constants and query-wide values can scale SUM/AVG/MIN/MAX.",
		    UserFacingName(*factor), verb, UserFacingName(*term));
	}
	if (!IsQueryWideConstant(*factor)) {
		// A correlated subquery has no SQL identifier to quote -- flattening left it a
		// column ref named "SUBQUERY" -- so name it for what the user wrote instead of
		// echoing an internal name and suggesting `SUM(x * SUBQUERY)`, which is not
		// something anyone can type.
		if (IsCorrelatedSubqueryRef(*factor)) {
			throw BinderException(
			    "DECIDE constraint: this subquery returns a different value for each row, "
			    "so it cannot %s %s. Move it inside the aggregate, e.g. "
			    "SUM(x %s (SELECT ...)).",
			    verb, UserFacingName(*term), out_divides ? "/" : "*");
		}
		throw BinderException(
		    "DECIDE constraint: '%s' varies per row, so it cannot %s %s. "
		    "Move it inside the aggregate, e.g. SUM(x %s %s).",
		    UserFacingName(*factor), verb, UserFacingName(*term), out_divides ? "/" : "*",
		    UserFacingName(*factor));
	}
	out_scale = factor;
	return *term;
}

void DecideCanonicalizer::Decompose(const Expression &expr, int sign, vector<Atom> &out,
                                    const LogicalType *cast_type) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.function.name == "+" && func.children.size() == 2) {
			Decompose(*func.children[0], sign, out, cast_type);
			Decompose(*func.children[1], sign, out, cast_type);
			return;
		}
		if (func.function.name == "-" && func.children.size() == 2) {
			Decompose(*func.children[0], sign, out, cast_type);
			Decompose(*func.children[1], -sign, out, cast_type);
			return;
		}
		if (func.function.name == "-" && func.children.size() == 1) {
			Decompose(*func.children[0], -sign, out, cast_type);
			return;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		// A widening cast distributes over addition, so it may be pushed onto each
		// term. Keep the OUTERMOST pending type when casts nest: applying it
		// directly skips the intermediate hop, and widening composes, so the value
		// is the same. A narrowing cast is left whole -- it becomes an ordinary
		// term, and any outer widening cast still applies on top of it.
		if (IsWideningNumericCast(cast.child->return_type, cast.return_type)) {
			auto *target = cast_type ? cast_type : &cast.return_type;
			vector<Atom> inner;
			Decompose(*cast.child, sign, inner, target);
			// Descend only if it bought a split; a cast over a single term rebuilds
			// to exactly what it already was, so leaving it alone avoids the churn.
			// And only if every term really does widen to the target: the terms of a
			// sum are not guaranteed to be no wider than the sum itself (decimal
			// arithmetic grows the result type), and casting one of those down would
			// lose the precision the original expression kept.
			bool worth_it = inner.size() > 1;
			for (auto &atom : inner) {
				worth_it = worth_it && IsWideningNumericCast(atom.expr->return_type, *target);
			}
			if (worth_it) {
				for (auto &atom : inner) {
					out.push_back(atom);
				}
				return;
			}
		}
	}
	// Every other node is a term boundary -- but a factor sitting on a reducer is
	// peeled off it first, so the term the rest of the pipeline sees is the bare
	// reducer and the factor travels beside it.
	const Expression *scale = nullptr;
	bool divides = false;
	auto &term = PeelScale(expr, scale, divides);
	// Classify on the whole original term. The factor is decision-free and
	// reducer-free by construction, so this agrees with classifying `term` -- stated
	// rather than assumed, because Classify is the part that was wrong twice before.
	out.push_back(Atom {sign, &term, Classify(expr), cast_type, scale, divides});
}

unique_ptr<Expression> DecideCanonicalizer::BindOp(const string &name, unique_ptr<Expression> left,
                                                   unique_ptr<Expression> right) const {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	return BindOp(name, std::move(children));
}

unique_ptr<Expression> DecideCanonicalizer::BindOp(const string &name, unique_ptr<Expression> operand) const {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(operand));
	return BindOp(name, std::move(children));
}

unique_ptr<Expression> DecideCanonicalizer::BindOp(const string &name,
                                                   vector<unique_ptr<Expression>> children) const {
	FunctionBinder function_binder(context);
	ErrorData error;
	auto result = function_binder.BindScalarFunction(DEFAULT_SCHEMA, name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("DECIDE canonicalizer failed to bind '%s': %s", name, error.Message());
	}
	return result;
}

unique_ptr<Expression> DecideCanonicalizer::BuildAdditive(const vector<Atom> &atoms, bool negate) const {
	unique_ptr<Expression> acc;
	for (auto &atom : atoms) {
		int sign = negate ? -atom.sign : atom.sign;
		auto term = atom.expr->Copy();
		if (atom.scale) {
			// Re-attach the peeled factor in the one canonical spelling: multiplication
			// with the factor on the LEFT. `SUM(x) * 2`, `2 * SUM(x)` and the cast-laden
			// variants all converge here, which is what lets each downstream consumer
			// match a single shape instead of four.
			term = atom.scale_divides
			           ? BindOp("/", std::move(term), atom.scale->Copy())
			           : BindOp("*", atom.scale->Copy(), std::move(term));
		}
		if (atom.cast_type) {
			// Re-apply the cast the spine descended through. Splitting a side is
			// what makes this necessary: the cast used to cover the whole side, and
			// once the side is several expressions the only equivalent placement is
			// on each term. Sound because the descent only accepts widening casts.
			term = BoundCastExpression::AddCastToType(context, std::move(term), *atom.cast_type);
		}
		if (!acc) {
			if (sign >= 0) {
				acc = std::move(term);
			} else {
				// Leading negative term: unary minus, NOT `0 - term`. The spelling
				// matters because the additive spine this pass emits is what downstream
				// walkers decompose, and a synthesized `0` is a term to them. It is not
				// a term to K3, which admits only reducers and row-invariant values, so
				// `price - SUM(x) <= cap` rebuilt as `(0 - SUM(x)) - cap` was rejected by
				// the aggregate extractor for a constant this pass had invented.
				// Negating in place introduces no node the spine has to explain away.
				acc = BindOp("-", std::move(term));
			}
			continue;
		}
		acc = BindOp(sign >= 0 ? "+" : "-", std::move(acc), std::move(term));
	}
	return acc;
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeComparison(const Expression &expr) const {
	auto &cmp = expr.Cast<BoundComparisonExpression>();

	vector<Atom> left_atoms;
	vector<Atom> right_atoms;
	Decompose(*cmp.left, 1, left_atoms, nullptr);
	Decompose(*cmp.right, 1, right_atoms, nullptr);

	auto Has = [](const vector<Atom> &atoms, Placement placement) {
		for (auto &atom : atoms) {
			if (atom.placement == placement) {
				return true;
			}
		}
		return false;
	};

	bool left_has_decision = Has(left_atoms, Placement::LEFT);
	bool right_has_decision = Has(right_atoms, Placement::LEFT);

	auto HasScale = [](const vector<Atom> &atoms) {
		for (auto &atom : atoms) {
			if (atom.scale) {
				return true;
			}
		}
		return false;
	};

	// Already canonical: decision terms on the left, none on the right, and no
	// plain data stranded on the left. A peeled scale still forces a rebuild -- the
	// side may be correctly partitioned and yet spell the factor on the wrong side
	// (`SUM(x) * 2`), and the whole point of peeling is that consumers see one
	// spelling. Rebuilding an already-canonical `2 * SUM(x)` reproduces it.
	if (left_has_decision && !right_has_decision && !Has(left_atoms, Placement::RIGHT) &&
	    !HasScale(left_atoms)) {
		return cmp.Copy();
	}

	// No decision content anywhere. Not a shape this pass owns; leave it exactly as
	// it is and let the existing validation speak.
	if (!left_has_decision && !right_has_decision) {
		return cmp.Copy();
	}

	// Mirror image: every decision term sits on the right. Swap the sides and flip
	// the relation rather than negating both sides. Signs are preserved, which is
	// what keeps MIN/MAX easy-vs-hard classification in RewriteMinMaxInConstraint
	// stable -- negating a reducer would silently turn a cheap per-row rewrite into
	// a Big-M encoding.
	if (!left_has_decision) {
		auto swapped = make_uniq<BoundComparisonExpression>(FlipComparisonExpression(cmp.type),
		                                                    cmp.right->Copy(), cmp.left->Copy());
		swapped->SetAlias(cmp.GetAlias());
		return CanonicalizeComparison(*swapped);
	}

	// Decision terms on BOTH sides. Migrating the right-hand ones left negates
	// them, which is only sound because the two downstream analyses that used to
	// classify by syntactic position now consult sign: ABS Big-M pinning
	// (ClassifyAbsConstraints) and composed MIN/MAX easy-vs-hard (WalkComposedLhs).
	// Both were made sign-aware before this branch was allowed to fall through.

	// General partition: decision terms migrate left, everything else migrates
	// right. A term that crosses the relation flips sign.
	vector<Atom> new_left;
	vector<Atom> new_right;
	auto crossed = [](const Atom &atom) {
		// Crossing the relation flips the sign and nothing else -- the peeled scale
		// travels with its term, since negating `2 * SUM(x)` negates the product, not
		// the factor.
		return Atom {-atom.sign, atom.expr, atom.placement, atom.cast_type, atom.scale, atom.scale_divides};
	};
	for (auto &atom : left_atoms) {
		if (atom.placement == Placement::RIGHT) {
			new_right.push_back(crossed(atom));
		} else {
			new_left.push_back(atom);
		}
	}
	for (auto &atom : right_atoms) {
		if (atom.placement == Placement::LEFT) {
			new_left.push_back(crossed(atom));
		} else {
			new_right.push_back(atom);
		}
	}

	auto new_lhs = BuildAdditive(new_left, false);
	auto new_rhs = BuildAdditive(new_right, false);
	if (!new_rhs) {
		new_rhs = make_uniq<BoundConstantExpression>(Value::DOUBLE(0));
	}

	// K5: tags are an out-of-band channel and several of them are read off the top
	// node. Carry the comparison's own tag across, and re-stamp the right-hand
	// side's tag when the right-hand side was rebuilt -- physical_decide reads
	// SHARED_SCALAR_SUBQUERY_TAG off `rhs_expr` itself, so burying it one level
	// down inside a subtraction would silently lose it.
	auto result = make_uniq<BoundComparisonExpression>(cmp.type, std::move(new_lhs), std::move(new_rhs));
	result->SetAlias(cmp.GetAlias());
	if (!cmp.right->GetAlias().empty() && result->right->GetAlias().empty()) {
		result->right->SetAlias(cmp.right->GetAlias());
	}
	return std::move(result);
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTree(const Expression &constraints) const {
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = constraints.Cast<BoundConjunctionExpression>();
		bool is_wrapper = IsPerConstraintTag(conj.GetAlias()) || conj.GetAlias() == WHEN_CONSTRAINT_TAG;

		auto result = make_uniq<BoundConjunctionExpression>(conj.type);
		result->SetAlias(conj.GetAlias());
		for (idx_t i = 0; i < conj.children.size(); i++) {
			// A WHEN/PER wrapper holds the constraint in child 0; the remaining
			// children are its condition / PER columns and are not constraints.
			bool is_constraint_child = !is_wrapper || i == 0;
			result->children.push_back(is_constraint_child ? CanonicalizeTree(*conj.children[i])
			                                               : conj.children[i]->Copy());
		}
		return std::move(result);
	}
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		return CanonicalizeComparison(constraints);
	}
	return constraints.Copy();
}

} // namespace duckdb
