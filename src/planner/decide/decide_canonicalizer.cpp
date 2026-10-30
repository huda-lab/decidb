#include "duckdb/common/decide_profile.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <cmath>

namespace duckdb {

static void CollectExpressionChildren(const Expression &expr, vector<const Expression *> &children) {
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { children.push_back(&child); });
}

//! Expression::Equals deliberately ignores aliases, while DECIDE stores structural
//! and provenance tags in aliases. Canonical fixed-point verification therefore needs
//! the normal semantic comparison plus an ordered, recursive alias comparison.
static bool CanonicalTreesEqual(const Expression &left, const Expression &right) {
	if (!Expression::Equals(left, right) || left.GetAlias() != right.GetAlias()) {
		return false;
	}
	vector<const Expression *> left_children;
	vector<const Expression *> right_children;
	CollectExpressionChildren(left, left_children);
	CollectExpressionChildren(right, right_children);
	if (left_children.size() != right_children.size()) {
		return false;
	}
	for (idx_t i = 0; i < left_children.size(); i++) {
		if (!CanonicalTreesEqual(*left_children[i], *right_children[i])) {
			return false;
		}
	}
	return true;
}

bool TryMatchScaledAggregate(const Expression &expr, idx_t decide_index, ScaledAggregateMatch &result) {
	result = {};
	auto current = UnwrapDecideCasts(expr, decide_index);
	if (current->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = current->Cast<BoundFunctionExpression>();
	bool is_mult = func.function.name == "*";
	bool is_div = func.function.name == "/";
	if ((!is_mult && !is_div) || func.children.size() != 2) {
		return false;
	}

	// A reducer, not a frame's marker: a frame is a tuple term, scaled as one.
	auto is_reducer = [&](const Expression &child) {
		auto *node = UnwrapDecideCasts(child, decide_index);
		idx_t frame;
		return node->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && !TryGetDecideFrame(*node, frame);
	};
	idx_t aggregate_child;
	if (is_div && is_reducer(*func.children[0])) {
		aggregate_child = 0;
	} else if (is_mult && is_reducer(*func.children[1])) {
		aggregate_child = 1;
	} else {
		return false;
	}
	if (is_reducer(*func.children[1 - aggregate_child])) {
		return false;
	}

	result.aggregate = &UnwrapDecideCasts(*func.children[aggregate_child], decide_index)
	                        ->Cast<BoundAggregateExpression>();
	result.scale = func.children[1 - aggregate_child].get();
	result.function = &func;
	result.divides = is_div;
	return true;
}

[[noreturn]] static void CanonicalInvariantFailure(const string &rule, const string &detail,
                                                   const Expression &expr) {
	throw InternalException("DECIDE canonical invariant %s violated: %s. Expression: %s", rule, detail,
	                        expr.ToString());
}

DecideCanonicalizer::DecideCanonicalizer(ClientContext &context, idx_t decide_index,
                                         vector<DecideVarScopeInfo> variable_scopes)
    : context(context), decide_index(decide_index), variable_scopes(std::move(variable_scopes)),
      judge_column_refs(false) {
}

DecideCanonicalizer::DecideCanonicalizer(ClientContext &context, idx_t decide_index,
                                         vector<DecideVarScopeInfo> variable_scopes,
                                         unordered_set<idx_t> query_wide_table_indexes,
                                         unordered_set<idx_t> correlated_subquery_table_indexes)
    : context(context), decide_index(decide_index), variable_scopes(std::move(variable_scopes)),
      query_wide_table_indexes(std::move(query_wide_table_indexes)),
      correlated_subquery_table_indexes(std::move(correlated_subquery_table_indexes)),
      judge_column_refs(true) {
}

bool DecideCanonicalizer::ReferencesDecideVar(const Expression &expr) const {
	return BoundExpressionReferencesDecide(expr, decide_index);
}

bool DecideCanonicalizer::ContainsReducer(const Expression &expr) {
	idx_t frame;
	if (TryGetDecideFrame(expr, frame)) {
		return false; // a tuple term read at another row, whose payload holds no reducer (D74)
	}
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
	// side has no aggregate evaluator -- SumFixedAggregateLhsOffset merely SUMS a term's
	// column over the group, which is why only SUM and AVG could ever be hoisted
	// there and MIN/MAX/COUNT were refused. The right side reduces per group by
	// kind (EvaluateRhsReducerPerGroup), so that is where a reducer belongs.
	return Placement::RIGHT;
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
	if (IsQueryWideValueTag(expr.GetAlias())) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		if (!judge_column_refs) {
			return false; // No evidence available: never promote an arbitrary column.
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
	if (expr.IsFoldable()) {
		return true;
	}
	// A factor that is identically zero before the query runs is one value for the
	// whole query -- 0 -- whatever its data factors hold: `0 * w` does not vary by row.
	if (PlanTimeSignOf(context, expr) == PlanTimeSign::ZERO) {
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

bool DecideCanonicalizer::IsQueryWideExpression(const Expression &expr) const {
	if (IsQueryWideValueTag(expr.GetAlias())) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			auto var_idx = colref.binding.column_index;
			return var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar();
		}
		return judge_column_refs &&
		       query_wide_table_indexes.find(colref.binding.table_index) != query_wide_table_indexes.end();
	}
	// Reducers are group-wide rather than query-wide: their legality is decided as
	// reducer terms by ClassifyCanonicalComparison, not recursively here.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subq = expr.Cast<BoundSubqueryExpression>();
		return subq.subquery_type == SubqueryType::SCALAR && !subq.IsCorrelated();
	}
	if (expr.IsFoldable()) {
		return true;
	}
	bool saw_child = false;
	bool all_query_wide = true;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		saw_child = true;
		if (all_query_wide && !IsQueryWideExpression(child)) {
			all_query_wide = false;
		}
	});
	return saw_child && all_query_wide;
}

unique_ptr<Expression>
DecideCanonicalizer::FinalizeBoundProvenance(unique_ptr<BoundComparisonExpression> comparison) const {
	// Root classification describes the complete canonical bound, so never trust a
	// copied value blindly: optimizer rewrites may have replaced children since the
	// previous canonicalization. Component-level QUERY_WIDE_VALUE_TAG facts remain and
	// are the semantic leaves from which the classification is recomputed.
	auto alias = comparison->right->GetAlias();
	RemoveDecideTag(alias, QUERY_WIDE_BOUND_TAG);
	comparison->right->SetAlias(std::move(alias));
	if (IsQueryWideConstant(*comparison->right)) {
		auto bound_alias = comparison->right->GetAlias();
		AddDecideTag(bound_alias, QUERY_WIDE_BOUND_TAG);
		comparison->right->SetAlias(std::move(bound_alias));
	}
	return std::move(comparison);
}

//! Name an expression the way the user wrote it, for an error message. Everything the
//! binder added is noise here: `CAST(weight AS DECIMAL(12,1))` is not what anyone
//! typed, and neither is the `FILTER (WHERE w)` that a reducer's own `when` becomes.
static string UserFacingName(const Expression &expr) {
	const Expression *cur = StripCastsForIdentity(expr);
	if (cur->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = cur->Cast<BoundAggregateExpression>();
		if (agg.children.size() == 1) {
			return StringUtil::Upper(agg.function.name) + "(" + UserFacingName(*agg.children[0]) + ")";
		}
		return StringUtil::Upper(agg.function.name) + "(...)";
	}
	auto name = StripDecideTags(cur->GetName());
	return name.empty() ? cur->ToString() : name;
}

bool DecideCanonicalizer::IsCorrelatedSubqueryRef(const Expression &expr) const {
	const Expression *cur = StripCastsForIdentity(expr);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto idx = cur->Cast<BoundColumnRefExpression>().binding.table_index;
	return correlated_subquery_table_indexes.find(idx) != correlated_subquery_table_indexes.end();
}

const Expression &DecideCanonicalizer::PeelScale(const Expression &expr, unique_ptr<Expression> &out_scale,
                                                 bool &out_divides, Clause clause) const {
	out_scale = nullptr;
	out_divides = false;
	const char *clause_name = clause == Clause::OBJECTIVE ? "DECIDE objective" : "DECIDE constraint";

	// Only a DECISION-BEARING reducer is scaled here: a data-only reducer
	// (`2 * AVG(price)` as a bound) is the RHS evaluator's business, and reaching into
	// it would reject shapes this pass has no stake in.
	auto is_scalable = [&](const Expression &e) {
		return ContainsReducer(e) && ReferencesDecideVar(e);
	};

	// Factors are gathered by role, not in encounter order, so that one composition
	// rule covers `M * AGG`, `AGG / D` and every nesting of the two.
	vector<const Expression *> multipliers;
	vector<const Expression *> divisors;
	const Expression *cur = &expr;

	while (true) {
		// Casts around decision-bearing algebra are binder-inserted type noise. A
		// data-only cast cannot reach this walk as `cur`, because `is_scalable`
		// requires the subtree to contain a decision-bearing reducer.
		cur = UnwrapDecideCasts(*cur, decide_index);
		if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			break;
		}
		auto &func = cur->Cast<BoundFunctionExpression>();
		bool is_mult = func.function.name == "*";
		bool is_div = func.function.name == "/";
		if ((!is_mult && !is_div) || func.children.size() != 2) {
			break;
		}

		const Expression *term = nullptr;
		const Expression *factor = nullptr;
		bool factor_divides = false;
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
			factor_divides = true;
		} else {
			// Two reducers multiplied, or no reducer at all (`price * x` is an ordinary
			// per-row coefficient). Not a scale; stop here.
			break;
		}

		// The single judgement. A reducer collapses many rows to one number, so anything
		// multiplying it must also be one number. Each nesting level is judged
		// separately, so the message names the factor the user actually has to fix.
		const char *verb = factor_divides ? "divide" : "multiply";
		if (ReferencesDecideVar(*factor)) {
			// A constraint takes any data factor (a row-varying one is a tuple term, below);
			// the objective, produced once, only one value.
			throw BinderException("%s: '%s' is a decision, so it cannot %s %s. %s", clause_name,
			                      UserFacingName(*factor), verb, UserFacingName(*term),
			                      clause == Clause::CONSTRAINT
			                          ? "Only data can scale SUM/AVG/MIN/MAX."
			                          : "Only constants and query-wide values can scale SUM/AVG/MIN/MAX.");
		}
		// A row-varying factor on a reducer of a constraint is a tuple term of the row's
		// constraint: the comparison takes the VALUE shape (ClassifyScoped), which
		// multiplies the reducer's helper decision by the factor's value on each row. The
		// objective, produced once, refuses it.
		bool row_varying_tuple_term = clause == Clause::CONSTRAINT;
		if (!IsQueryWideConstant(*factor) && !row_varying_tuple_term) {
			// A correlated subquery has no SQL identifier to quote -- flattening left it a
			// column ref named "SUBQUERY" -- so name it for what the user wrote instead of
			// echoing an internal name and suggesting `SUM(x * SUBQUERY)`, which is not
			// something anyone can type.
			if (IsCorrelatedSubqueryRef(*factor)) {
				throw BinderException(
				    "%s: this subquery returns a different value for each row, "
				    "so it cannot %s %s. Move it inside the aggregate, e.g. "
				    "SUM(x %s (SELECT ...)).",
				    clause_name, verb, UserFacingName(*term), factor_divides ? "/" : "*");
			}
			throw BinderException(
			    "%s: '%s' varies per row, so it cannot %s %s. "
			    "Move it inside the aggregate, e.g. SUM(x %s %s).",
			    clause_name, UserFacingName(*factor), verb, UserFacingName(*term),
			    factor_divides ? "/" : "*", UserFacingName(*factor));
		}

		(factor_divides ? divisors : multipliers).push_back(factor);
		cur = term;
	}

	if (multipliers.empty() && divisors.empty()) {
		return expr;
	}

	// Compose each role into one node, then combine the two. `M`, `D` and `M / D` are
	// the only three results, so the rebuild in BuildAdditive stays a single level and
	// the physical extractor's single-level match keeps working unchanged.
	auto product = [&](const vector<const Expression *> &factors) {
		unique_ptr<Expression> acc;
		for (auto *factor : factors) {
			acc = acc ? BindOp("*", std::move(acc), factor->Copy()) : factor->Copy();
		}
		return acc;
	};
	auto multiplier = product(multipliers);
	auto divisor = product(divisors);

	if (!divisor) {
		out_scale = std::move(multiplier);
	} else if (!multiplier) {
		out_scale = std::move(divisor);
		out_divides = true;
	} else {
		// Mixed nesting collapses to a single multiplication by `M / D` rather than
		// keeping a numerator and a denominator slot: one spelling downstream beats two.
		out_scale = BindOp("/", std::move(multiplier), std::move(divisor));
	}
	return *cur;
}

//! An additive spine over decisions: `+`, binary or unary `-` under the binder's casts.
static bool IsAdditiveSpine(const Expression &expr, idx_t decide_index) {
	auto *cur = UnwrapDecideCasts(expr, decide_index);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = cur->Cast<BoundFunctionExpression>();
	return (func.function.name == "+" && func.children.size() == 2) ||
	       (func.function.name == "-" && (func.children.size() == 1 || func.children.size() == 2));
}

unique_ptr<Expression> DecideCanonicalizer::ComposeScale(const Expression *outer, bool outer_divides,
                                                         unique_ptr<Expression> inner, bool inner_divides,
                                                         bool &out_divides) const {
	if (!outer) {
		out_divides = inner_divides;
		return inner;
	}
	if (!inner) {
		out_divides = outer_divides;
		return outer->Copy();
	}
	// The same three results PeelScale composes to: `M`, `D` or `M / D`.
	if (!outer_divides && !inner_divides) {
		out_divides = false;
		return BindOp("*", outer->Copy(), std::move(inner)); // c * (d * e) = (c * d) * e
	}
	if (outer_divides && inner_divides) {
		out_divides = true;
		return BindOp("*", std::move(inner), outer->Copy()); // (e / d) / c = e / (d * c)
	}
	out_divides = false;
	return outer_divides ? BindOp("/", std::move(inner), outer->Copy())   // (d * e) / c = (d / c) * e
	                     : BindOp("/", outer->Copy(), std::move(inner)); // c * (e / d) = (c / d) * e
}

void DecideCanonicalizer::Decompose(const Expression &expr, int sign, vector<Atom> &out, Clause clause,
                                    const Expression *factor, bool factor_divides) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.function.name == "+" && func.children.size() == 2) {
			Decompose(*func.children[0], sign, out, clause, factor, factor_divides);
			Decompose(*func.children[1], sign, out, clause, factor, factor_divides);
			return;
		}
		if (func.function.name == "-" && func.children.size() == 2) {
			Decompose(*func.children[0], sign, out, clause, factor, factor_divides);
			Decompose(*func.children[1], -sign, out, clause, factor, factor_divides);
			return;
		}
		if (func.function.name == "-" && func.children.size() == 1) {
			Decompose(*func.children[0], -sign, out, clause, factor, factor_divides);
			return;
		}
		// A factor known before the query runs distributes over an additive spine, as a
		// negation does above: `2 * (min(x) - 1)` is `2 * min(x) - 2 * 1`, the one spelling
		// a scaled reducer beside a constant has, so the MIN/MAX rewrite and bound
		// absorption read it as they read `2 * min(x) - 2`. A factor that varies by row,
		// or a subquery, stays on the spine as one atom: its value is the VALUE shape's.
		if ((func.function.name == "*" || func.function.name == "/") && func.children.size() == 2) {
			const bool divides = func.function.name == "/";
			for (idx_t side = divides ? 1 : 0; side < 2; side++) {
				auto &candidate = *func.children[side];
				auto &spine = *func.children[1 - side];
				if (candidate.IsFoldable() && !ReferencesDecideVar(candidate) && IsAdditiveSpine(spine, decide_index) &&
				    ReferencesDecideVar(spine)) {
					bool composed_divides;
					auto composed = ComposeScale(factor, factor_divides, candidate.Copy(), divides, composed_divides);
					Decompose(spine, sign, out, clause, composed.get(), composed_divides);
					return;
				}
			}
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		if (ReferencesDecideVar(expr)) {
			// The parsed boundary rejected every user-authored decision cast.
			// Anything left here was inserted by binding and is transparent in the
			// solver's single DOUBLE domain. Data-only casts stay atomic.
			Decompose(*cast.child, sign, out, clause, factor, factor_divides);
			return;
		}
	}
	// Every other node is a term boundary -- but a factor sitting on a reducer is
	// peeled off it first, so the term the rest of the pipeline sees is the bare
	// reducer and the factor travels beside it; a factor distributed from above
	// composes with it.
	unique_ptr<Expression> scale;
	bool divides = false;
	auto &term = PeelScale(expr, scale, divides, clause);
	if (factor) {
		scale = ComposeScale(factor, factor_divides, std::move(scale), divides, divides);
	}
	// Classify on the whole original term. The factors are decision-free and
	// reducer-free by construction, so this agrees with classifying `term` -- stated
	// rather than assumed, because Classify is the part that was wrong twice before.
	out.push_back(Atom {sign, &term, Classify(expr), std::move(scale), divides});
}

//! The bare decision `expr` is a constant multiple of, with that multiple folded into
//! `value` -- `2 * x`, `x * 2`, `-x`, `x / 2` -- or `expr` itself with `value` untouched
//! when it is not one. PeelScale peels factors off reducers only, so a plain decision
//! carries its factor inside the atom; the like-term merge needs it outside.
static const Expression *PeelBareDecisionFactors(ClientContext &context, const Expression &expr, idx_t decide_index,
                                                 double &value) {
	auto *cur = UnwrapDecideCasts(expr, decide_index);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return cur;
	}
	auto &func = cur->Cast<BoundFunctionExpression>();
	const auto &name = func.function.name;
	auto constant = [&](const Expression &candidate, double &c) {
		return TryEvaluateFoldableDoubleNoThrow(context, candidate, c) && std::isfinite(c) && c != 0.0;
	};
	double c;
	double inner = 1.0;
	const Expression *term = nullptr;
	if (name == "-" && func.children.size() == 1) {
		term = PeelBareDecisionFactors(context, *func.children[0], decide_index, inner);
		inner = -inner;
	} else if (name == "*" && func.children.size() == 2) {
		for (idx_t side = 0; side < 2 && !term; side++) {
			if (constant(*func.children[side], c)) {
				term = PeelBareDecisionFactors(context, *func.children[1 - side], decide_index, inner);
				inner *= c;
			}
		}
	} else if (name == "/" && func.children.size() == 2 && constant(*func.children[1], c)) {
		term = PeelBareDecisionFactors(context, *func.children[0], decide_index, inner);
		inner /= c;
	}
	if (!term || term->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return cur;
	}
	value *= inner;
	return term;
}

bool DecideCanonicalizer::MergeLikeAtoms(vector<Atom> &atoms) const {
	// The term an atom is a multiple of, and that multiple as a number known before the
	// query runs: the factor PeelScale peeled off a reducer (none is 1, a divisor its
	// reciprocal), with the constant factors a bare decision stands under folded in.
	// False when the factor is data or a subquery, which is not summable here.
	auto key_of = [&](const Atom &atom, const Expression *&term, double &value) {
		value = 1.0;
		if (atom.scale) {
			double scale;
			if (!TryEvaluateFoldableDouble(context, *atom.scale, scale) || !std::isfinite(scale) ||
			    (atom.scale_divides && scale == 0.0)) {
				return false;
			}
			value = atom.scale_divides ? 1.0 / scale : scale;
			term = atom.expr;
			return true;
		}
		term = PeelBareDecisionFactors(context, *atom.expr, decide_index, value);
		return true;
	};
	bool merged = false;
	for (idx_t i = 0; i < atoms.size(); i++) {
		const Expression *term;
		double total;
		if (atoms[i].placement != Placement::LEFT || !key_of(atoms[i], term, total)) {
			continue;
		}
		total *= atoms[i].sign;
		vector<idx_t> like;
		for (idx_t j = i + 1; j < atoms.size(); j++) {
			const Expression *other;
			double value;
			if (atoms[j].placement == Placement::LEFT && key_of(atoms[j], other, value) &&
			    CanonicalTreesEqual(*term, *other)) {
				total += atoms[j].sign * value;
				like.push_back(j);
			}
		}
		// A group whose factors cancel is left as written: dropping it could empty the
		// side, and `x - x` is the user's to resolve.
		if (like.empty() || total == 0.0) {
			continue;
		}
		const bool whole = total == std::floor(total) && std::fabs(total) < 2147483647.0;
		atoms[i].sign = 1;
		atoms[i].expr = term;
		atoms[i].scale = make_uniq_base<Expression, BoundConstantExpression>(
		    whole ? Value::INTEGER(static_cast<int32_t>(total)) : Value::DOUBLE(total));
		atoms[i].scale_divides = false;
		for (auto it = like.rbegin(); it != like.rend(); ++it) {
			atoms.erase(atoms.begin() + static_cast<int64_t>(*it));
		}
		merged = true;
	}
	return merged;
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

bool DecideCanonicalizer::IsLoneNegatedReducer(const vector<Atom> &atoms) const {
	if (atoms.size() != 1 || atoms[0].sign >= 0) {
		return false;
	}
	auto root = UnwrapDecideCasts(*atoms[0].expr, decide_index);
	idx_t frame;
	return root->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && !TryGetDecideFrame(*root, frame);
}

bool DecideCanonicalizer::IsSignedSpine(const Expression &side, const vector<Atom> &atoms) const {
	auto root = UnwrapDecideCasts(side, decide_index);
	if (atoms.size() != 1 || root->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &name = root->Cast<BoundFunctionExpression>().function.name;
	return name == "+" || name == "-";
}

void DecideCanonicalizer::FoldNegationIntoFactor(Atom &atom) const {
	atom.scale = atom.scale ? BindOp("-", std::move(atom.scale))
	                        : make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(-1));
	atom.sign = -atom.sign;
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeComparison(const Expression &expr) const {
	DecideProfileScope profile("canonicalize.comparison");
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	// Two spellings are fixed here. A lone negated reducer carries its sign in its factor,
	// the one spelling a factor on a reducer has, so `-max(e) by (g)` reads as
	// `-2 * max(e) by (g)` does: one MIN/MAX under a factor whose sign is known; and
	// `-(-max(e))` as `max(e)`. The MIN/MAX rewrite routes on the tree it is given, so the
	// tree must be the one its atoms rebuild. And a tuple term may stand under a
	// row-varying factor beside a reducer (PeelScale): the VALUE shape takes it.
	vector<Atom> left_atoms;
	vector<Atom> right_atoms;
	Decompose(*cmp.left, 1, left_atoms, Clause::CONSTRAINT);
	Decompose(*cmp.right, 1, right_atoms, Clause::CONSTRAINT);
	// Like terms under factors known before the query runs are one term: `x + x` is
	// `2 * x`, `min(x) + min(x)` is `2 * min(x)`, so a bound written as a repeated term
	// is read as the bound it is. Merged here so a side that is otherwise canonical does
	// not take the early return below with its twins intact (a merged atom carries a
	// scale, which forces the rebuild), and again on the partitioned decision side,
	// where a term that crossed the relation meets its twin (`3 * x >= x - 4`).
	MergeLikeAtoms(left_atoms);
	MergeLikeAtoms(right_atoms);

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
	    !HasScale(left_atoms) && !(IsLoneNegatedReducer(left_atoms) || IsSignedSpine(*cmp.left, left_atoms))) {
		auto copy = cmp.Copy();
		return FinalizeBoundProvenance(unique_ptr_cast<Expression, BoundComparisonExpression>(std::move(copy)));
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
	// Crossing the relation flips the sign and nothing else -- the peeled scale
	// travels with its term, since negating `2 * SUM(x)` negates the product, not
	// the factor. Each atom lands in exactly one destination, so it is moved rather
	// than copied; the scale it owns has no second home.
	auto cross = [](Atom &atom) {
		atom.sign = -atom.sign;
		return std::move(atom);
	};
	for (auto &atom : left_atoms) {
		if (atom.placement == Placement::RIGHT) {
			new_right.push_back(cross(atom));
		} else {
			new_left.push_back(std::move(atom));
		}
	}
	for (auto &atom : right_atoms) {
		if (atom.placement == Placement::LEFT) {
			new_left.push_back(cross(atom));
		} else {
			new_right.push_back(std::move(atom));
		}
	}

	MergeLikeAtoms(new_left);
	if (IsLoneNegatedReducer(new_left)) {
		FoldNegationIntoFactor(new_left[0]);
	}
	auto new_lhs = BuildAdditive(new_left, false);
	auto new_rhs = BuildAdditive(new_right, false);
	if (!new_rhs) {
		new_rhs = make_uniq<BoundConstantExpression>(Value::DOUBLE(0));
	}

	// K5: comparison tags are an out-of-band channel and must survive rebuilding.
	// Bound provenance is recomputed semantically from the complete canonical RHS;
	// it is never copied from whichever side happened to be the original RHS.
	auto result = make_uniq<BoundComparisonExpression>(cmp.type, std::move(new_lhs), std::move(new_rhs));
	result->SetAlias(cmp.GetAlias());
	return FinalizeBoundProvenance(std::move(result));
}

//! Tags every reducer over decisions in `expr` that holds another reducer -- the outermost
//! of each nest -- for the value rewrite: in the objective it is a helper decision, and so
//! is every reducer inside it (D75).
static void TagNestedObjectiveReducers(Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &reducer = expr.Cast<BoundAggregateExpression>();
		if (!reducer.children.empty() && BoundExpressionReferencesDecide(reducer, decide_index) &&
		    DecideCanonicalizer::ContainsReducer(*reducer.children[0]) &&
		    !HasDecideTag(reducer.GetAlias(), VALUE_REWRITE_TAG)) {
			auto alias = reducer.GetAlias();
			AddDecideTag(alias, VALUE_REWRITE_TAG);
			reducer.SetAlias(std::move(alias));
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](Expression &child) { TagNestedObjectiveReducers(child, decide_index); });
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeObjective(const Expression &objective,
                                                                  double &out_constant_offset) const {
	DecideProfileScope profile("canonicalize.objective");
	// A `when` or `per` wrapper carries no algebra: recurse into the objective child and copy
	// the condition or grouping columns unchanged. Identical rule to C0, and the same
	// wrapper predicate, so the two clauses cannot drift apart on what a wrapper is.
	if (objective.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = objective.Cast<BoundConjunctionExpression>();
		if (IsConstraintWrapper(conjunction) && !conjunction.children.empty()) {
			auto result = make_uniq<BoundConjunctionExpression>(conjunction.type);
			result->children.push_back(CanonicalizeObjective(*conjunction.children[0], out_constant_offset));
			for (idx_t i = 1; i < conjunction.children.size(); i++) {
				result->children.push_back(conjunction.children[i]->Copy());
			}
			result->SetAlias(conjunction.GetAlias());
			return std::move(result);
		}
	}

	// No decision content anywhere. Not a shape this pass owns -- and a legitimate
	// input rather than an error: RewriteComposedMinMaxObjectiveTop installs a
	// constant placeholder objective and supplies the coefficients from
	// composed_minmax_objective_terms instead.
	if (!ReferencesDecideVar(objective)) {
		return objective.Copy();
	}

	vector<Atom> atoms;
	Decompose(objective, 1, atoms, Clause::OBJECTIVE);
	MergeLikeAtoms(atoms);

	vector<Atom> decision_atoms;
	for (auto &atom : atoms) {
		if (atom.placement == Placement::LEFT) {
			decision_atoms.push_back(std::move(atom));
			continue;
		}
		// A decision-free additive term shifts the objective without moving its
		// argmax/argmin, so it is peeled here and the body downstream is pure decision
		// algebra. Peeling it requires knowing its value, which is exactly the
		// foldable case; a row-varying or query-executed term has no single value to
		// fold and does not belong in an objective at all. A factor distributed onto
		// it (`2 * (sum(x) - 1)`) is part of its value.
		double value = 0;
		unique_ptr<Expression> scaled;
		if (atom.scale) {
			scaled = atom.scale_divides ? BindOp("/", atom.expr->Copy(), atom.scale->Copy())
			                            : BindOp("*", atom.scale->Copy(), atom.expr->Copy());
		}
		if (!TryEvaluateFoldableDouble(context, scaled ? *scaled : *atom.expr, value)) {
			throw BinderException(
			    "MAXIMIZE/MINIMIZE: '%s' does not reference a decision, so it cannot be part of "
			    "the objective. Remove it, or move it to the SELECT list.",
			    QuoteAsWritten(*atom.expr));
		}
		out_constant_offset += atom.sign * value;
	}

	// Every term was decision-free despite the subtree containing a decision
	// reference -- only reachable if Classify and ReferencesDecideVar disagree, which
	// is an engine bug rather than a user error.
	if (decision_atoms.empty()) {
		throw InternalException(
		    "DECIDE canonicalizer: objective references a decision but decomposed into no "
		    "decision-bearing term");
	}
	auto result = BuildAdditive(decision_atoms, false);
	// A reducer holding a reducer takes the value rewrite, as in a constraint: the shape is
	// decided here, once, and the rewrite reads the tag.
	TagNestedObjectiveReducers(*result, decide_index);
	return result;
}

string DecideCanonicalizer::Quote(const Expression &expr) const {
	return message_names ? RenderDecideSource(expr, *message_names) : UserFacingName(expr);
}

string DecideCanonicalizer::QuoteAsWritten(const Expression &expr) const {
	// A node whose written text was recorded (a subquery or a cast) carries it in a tag,
	// and only that text names it in SQL; a reducer is spelled as the plan prints it,
	// with its `when` and `by`.
	idx_t fragment_id;
	auto *node = StripCastsForIdentity(expr);
	return TryParseSourceFragmentTag(node->GetAlias(), fragment_id) ||
	               node->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE
	           ? Quote(expr)
	           : UserFacingName(expr);
}

DecideCanonicalizer::ScopedShape DecideCanonicalizer::ClassifyScoped(const BoundComparisonExpression &cmp,
                                                                     idx_t &group_key, string &refusal) const {
	group_key = DConstants::INVALID_INDEX;
	vector<Atom> lhs_atoms;
	Decompose(*cmp.left, 1, lhs_atoms, Clause::CONSTRAINT);
	bool has_reducer = false;
	for (auto &atom : lhs_atoms) {
		has_reducer = has_reducer || ContainsReducer(*atom.expr);
	}
	if (!has_reducer) {
		// A data-only reducer sits on the right, as a bound.
		return ScopedShape::ROWS;
	}
	// The aggregate shape is today's grouped row: decision reducers sharing one `by` key,
	// under whole-query factors, with only query-wide terms beside them. Anything else on
	// the decision side takes the value rewrite (VALUE): a per-row or keyed decision, a
	// factor that varies by row, a second `by` key, a reducer inside another expression,
	// a product of decisions beside a reducer.
	bool value = false;
	bool first_reducer = true;
	// A query-wide decision beside the reducers counts once per group, in a linear,
	// bilinear or quadratic row alike.
	const Expression *query_wide_decision = nullptr;
	bool nonlinear_beside = false;
	// `<>` excludes an AVG's value by scaling the bound with the number of rows in the
	// group, which is exact only when every term is an AVG reading all of them.
	bool has_avg = false;
	bool only_plain_avgs = true;
	for (auto &atom : lhs_atoms) {
		if (!ContainsReducer(*atom.expr)) {
			only_plain_avgs = false;
			nonlinear_beside = nonlinear_beside || !DecideExpressionDegree(*atom.expr, decide_index).IsLinear();
			if (IsQueryWideExpression(*atom.expr)) {
				if (ReferencesDecideVar(*atom.expr)) {
					query_wide_decision = atom.expr;
				}
				continue;
			}
			value = true; // a per-row or keyed decision, or row data, beside a reducer
			continue;
		}
		auto *root = UnwrapDecideCasts(*atom.expr, decide_index);
		if (root->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE || !ReferencesDecideVar(*root)) {
			only_plain_avgs = false;
			value = true; // a reducer inside another expression: `abs(sum(e) by (g) - 40)`
			continue;
		}
		auto &reducer = root->Cast<BoundAggregateExpression>();
		if (!reducer.children.empty() && ContainsReducer(*reducer.children[0])) {
			value = true; // a reducer inside it: a helper at every level (syntax_reference.md §5.5)
		}
		idx_t qualifier;
		bool is_avg =
		    StringUtil::CIEquals(reducer.function.name, "avg") || HasDecideTag(reducer.GetAlias(), AVG_REWRITE_TAG);
		has_avg = has_avg || is_avg;
		only_plain_avgs = only_plain_avgs && is_avg && !reducer.filter &&
		                  !TryParseQualifiedReducerTag(reducer.GetAlias(), qualifier);
		if (atom.scale && !IsQueryWideConstant(*atom.scale)) {
			value = true; // a factor that varies by row: a tuple term of the row's constraint
		}
		idx_t key = DConstants::INVALID_INDEX;
		TryParseGroupKeyTag(root->GetAlias(), key);
		if (first_reducer) {
			group_key = key;
			first_reducer = false;
		} else if (key != group_key) {
			value = true; // reducers with different `by` keys
		}
	}
	if (query_wide_decision && nonlinear_beside) {
		value = true; // `sum(x) + b * s`: the product beside the reducer is a term of each row
	}
	if (cmp.type == ExpressionType::COMPARE_NOTEQUAL && has_avg && (!only_plain_avgs || nonlinear_beside)) {
		refusal = "DECIDE: AVG in a <> constraint is not supported yet beside another term or with its own when "
		          "or per";
		return ScopedShape::UNSUPPORTED;
	}
	// A MIN or MAX beside another term, or under a factor of unknown sign, takes the
	// composed formulation where that applies: one row over all of the clause's kept rows,
	// under one bound, with nothing but reducers in it. With a `by`, a bound that is not
	// a constant, or a decision beside it, the value rewrite pins the extremum per group.
	if (!value && TakesComposedMinMax(lhs_atoms) &&
	    (group_key != DConstants::INVALID_INDEX || !cmp.right->IsFoldable() || query_wide_decision)) {
		value = true;
	}
	return value ? ScopedShape::VALUE : ScopedShape::AGGREGATE;
}

bool DecideCanonicalizer::TakesComposedMinMax(const vector<Atom> &atoms) const {
	bool has_minmax = false;
	for (auto &atom : atoms) {
		auto *root = UnwrapDecideCasts(*atom.expr, decide_index);
		if (root->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && ReferencesDecideVar(*root)) {
			auto name = StringUtil::Lower(root->Cast<BoundAggregateExpression>().function.name);
			has_minmax = has_minmax || name == "min" || name == "max";
		}
	}
	return has_minmax &&
	       (atoms.size() > 1 || ScaleSignAtPlanTime(context, atoms[0].scale.get(), atoms[0].scale_divides) == 0);
}

CanonicalConstraintClass DecideCanonicalizer::ClassifyCanonicalComparison(const Expression &expr) const {
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	if (ReferencesDecideVar(*cmp.right)) {
		return CanonicalConstraintClass::INVALID;
	}
	idx_t group_key;
	string refusal;
	switch (ClassifyScoped(cmp, group_key, refusal)) {
	case ScopedShape::ROWS:
		return CanonicalConstraintClass::PER_ROW;
	case ScopedShape::AGGREGATE:
		return CanonicalConstraintClass::AGGREGATE;
	case ScopedShape::VALUE:
		return CanonicalConstraintClass::VALUE;
	default:
		return CanonicalConstraintClass::INVALID;
	}
}

void DecideCanonicalizer::ValidateCanonicalComparison(const BoundComparisonExpression &comparison) const {
	auto classification = ClassifyCanonicalComparison(comparison);
	if (classification == CanonicalConstraintClass::INVALID) {
		if (ReferencesDecideVar(*comparison.right)) {
			throw InternalException("DECIDE canonicalizer: a decision survived on the bound side of '%s'",
			                        comparison.ToString());
		}
		idx_t group_key;
		string refusal;
		ClassifyScoped(comparison, group_key, refusal);
		throw BinderException(refusal);
	}
	// A foldable bound is the one case whose NULL result is knowable at planning
	// time. Do not execute subqueries or speculate about nullable data columns here;
	// those retain the runtime COALESCE guidance.
	if (comparison.right->IsFoldable()) {
		Value value;
		if (ExpressionExecutor::TryEvaluateScalar(context, *comparison.right, value) && value.IsNull()) {
			throw BinderException(
			    "DECIDE constraint bound evaluates to NULL. Use COALESCE(...) to provide a numeric bound.");
		}
	}
}

void DecideCanonicalizer::ValidateCanonicalTree(const Expression &constraints) const {
	auto validate = [&](const Expression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			ValidateCanonicalComparison(expression.Cast<BoundComparisonExpression>());
			return true;
		}
		return true;
	};
	VisitConstraintTree(constraints, validate);
}

void DecideCanonicalizer::VerifyCanonicalComparison(const BoundComparisonExpression &comparison) const {
	if (!comparison.left || !comparison.right) {
		CanonicalInvariantFailure("C1", "comparison is missing an operand", comparison);
	}
	if (!ReferencesDecideVar(*comparison.left)) {
		CanonicalInvariantFailure("C1", "comparison has no decision-bearing left side", comparison);
	}
	if (ReferencesDecideVar(*comparison.right)) {
		CanonicalInvariantFailure("C2", "right side still references a DECIDE variable", comparison);
	}

	vector<Atom> lhs_atoms;
	vector<Atom> rhs_atoms;
	Decompose(*comparison.left, 1, lhs_atoms, Clause::CONSTRAINT);
	Decompose(*comparison.right, 1, rhs_atoms, Clause::CONSTRAINT);
	for (auto &atom : lhs_atoms) {
		if (atom.placement != Placement::LEFT) {
			CanonicalInvariantFailure("C3", "decision-free additive term remains on the left", comparison);
		}
	}
	for (auto &atom : rhs_atoms) {
		if (atom.placement != Placement::RIGHT) {
			CanonicalInvariantFailure("C3", "decision-bearing additive term remains on the right", comparison);
		}
	}
	if (ClassifyCanonicalComparison(comparison) == CanonicalConstraintClass::INVALID) {
		CanonicalInvariantFailure("C5", "comparison is neither a valid per-row nor aggregate constraint",
		                          comparison);
	}
}

void DecideCanonicalizer::VerifyCanonicalTree(const Expression &constraints) const {
	auto verify = [&](const Expression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			VerifyCanonicalComparison(expression.Cast<BoundComparisonExpression>());
			return true;
		}
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			// Optimizer-owned rewrites may replace a fully extracted constraint with a
			// Boolean placeholder. C0 deliberately preserves non-comparison leaves.
			return true;
		}

		auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		if (conjunction.type != ExpressionType::CONJUNCTION_AND) {
			CanonicalInvariantFailure("C0", "constraint conjunction is not AND", expression);
		}
		if (IsWhenConstraintWrapper(conjunction) && conjunction.children.size() != 2) {
			CanonicalInvariantFailure("C0", "WHEN wrapper must contain one constraint and one condition",
			                          expression);
		}
		if (IsPerClassWrapper(conjunction) && conjunction.children.empty()) {
			CanonicalInvariantFailure("C0", "per-class wrapper must contain a constraint", expression);
		}
		return true;
	};
	VisitConstraintTree(constraints, verify);
}

void DecideCanonicalizer::VerifyCanonicalObjectiveBody(const Expression &objective) const {
	if (objective.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = objective.Cast<BoundConjunctionExpression>();
		if (IsConstraintWrapper(conjunction)) {
			if (conjunction.children.empty()) {
				CanonicalInvariantFailure("O0", "objective wrapper has no objective child", objective);
			}
			VerifyCanonicalObjectiveBody(*conjunction.children[0]);
			return;
		}
	}
	// A decision-free objective is the composed MIN/MAX placeholder, which supplies
	// its coefficients from composed_minmax_objective_terms instead of from the tree.
	if (!ReferencesDecideVar(objective)) {
		return;
	}
	vector<Atom> atoms;
	Decompose(objective, 1, atoms, Clause::OBJECTIVE);
	for (auto &atom : atoms) {
		if (atom.placement != Placement::LEFT) {
			CanonicalInvariantFailure("O2", "canonical objective retains a decision-free additive term",
			                          *atom.expr);
		}
	}
}

void DecideCanonicalizer::VerifyCanonicalObjective(const Expression &objective) const {
#ifdef DEBUG
	try {
		VerifyCanonicalObjectiveBody(objective);
		if (!ReferencesDecideVar(objective)) {
			return;
		}
		// Re-canonicalizing a canonical objective must peel nothing: every additive
		// constant was already folded into objective_constant_offset, so a nonzero
		// residual means an offset would be counted twice.
		double residual_offset = 0.0;
		auto fixed_point = CanonicalizeObjective(objective, residual_offset);
		if (residual_offset != 0.0) {
			CanonicalInvariantFailure("O2", "canonical objective still yields a constant offset", objective);
		}
		if (!CanonicalTreesEqual(objective, *fixed_point)) {
			CanonicalInvariantFailure(
			    "O4/O7", "canonicalizing the objective again changes its structure, reducer scale, or tags",
			    objective);
		}
	} catch (const InternalException &) {
		throw;
	} catch (const std::exception &ex) {
		throw InternalException("DECIDE canonical objective verification failed for %s: %s",
		                        objective.ToString(), ex.what());
	}
#endif
}

void DecideCanonicalizer::VerifyCanonical(const Expression &constraints) const {
#ifdef DEBUG
	try {
		VerifyCanonicalTree(constraints);
		auto fixed_point = CanonicalizeTreeInternal(constraints);
		if (!CanonicalTreesEqual(constraints, *fixed_point)) {
			CanonicalInvariantFailure(
			    "C4/C6/C7",
			    "canonicalizing the tree again changes its structure, reducer scale, or provenance tags",
			    constraints);
		}
	} catch (const InternalException &) {
		throw;
	} catch (const std::exception &ex) {
		throw InternalException("DECIDE canonical invariant verification failed for %s: %s",
		                        constraints.ToString(), ex.what());
	}
#endif
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTreeInternal(const Expression &constraints) const {
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = constraints.Cast<BoundConjunctionExpression>();

		auto result = make_uniq<BoundConjunctionExpression>(conj.type);
		result->SetAlias(conj.GetAlias());
		for (idx_t i = 0; i < conj.children.size(); i++) {
			// A `when` or `per` wrapper holds the constraint in child 0; the remaining
			// children are its condition / key columns and are not constraints.
			result->children.push_back(IsConstraintChild(conj, i) ? CanonicalizeTreeInternal(*conj.children[i])
			                                                     : conj.children[i]->Copy());
		}
		// `per K:` over a clause in the aggregate shape adds no row to the model: its rows
		// are one per `by` group of the kept rows, and every instance in a group -- one per
		// class and variant (D64) -- differs only in its bound, which the group's tightest
		// bound covers (`=` refuses two). The wrapper stays all the same: it names the
		// classes each group's row stands for, which a diagnosis reports one by one
		// (syntax_reference.md §8.1, D28); the linear form reads it as the clause's class key.
		return std::move(result);
	}
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto result = CanonicalizeComparison(constraints);
		// A clause in the aggregate shape is grouped by the `by` key its decision reducers
		// share; the comparison carries it, as later rewrites rebuild the reducers.
		if (!ReferencesDecideVar(*result->Cast<BoundComparisonExpression>().right)) {
			idx_t group_key;
			string refusal;
			auto shape = ClassifyScoped(result->Cast<BoundComparisonExpression>(), group_key, refusal);
			idx_t stamped;
			if (shape == ScopedShape::AGGREGATE && group_key != DConstants::INVALID_INDEX &&
			    !TryParseGroupKeyTag(result->GetAlias(), stamped)) {
				auto alias = result->GetAlias();
				AddDecideTag(alias, MakeGroupKeyTag(group_key));
				result->SetAlias(std::move(alias));
			}
			// The VALUE shape is decided here, once: the value rewrite reads the tag.
			if (shape == ScopedShape::VALUE && !HasDecideTag(result->GetAlias(), VALUE_REWRITE_TAG)) {
				auto alias = result->GetAlias();
				AddDecideTag(alias, VALUE_REWRITE_TAG);
				result->SetAlias(std::move(alias));
			}
		}
		return result;
	}
	return constraints.Copy();
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTree(const Expression &constraints) const {
	auto result = CanonicalizeTreeInternal(constraints);
	ValidateCanonicalTree(*result);
	return result;
}

void DecideCanonicalizer::BuildConstraintScopeChecks(const Expression &constraints, DecideScopeChecks &scope) const {
	BuildConstraintScopeChecks(constraints, DecideEnclosingScope(), scope);
}

void DecideCanonicalizer::BuildConstraintScopeChecks(const Expression &constraints, DecideEnclosingScope enclosing,
                                                    DecideScopeChecks &scope) const {
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = constraints.Cast<BoundConjunctionExpression>();
		auto inner = enclosing;
		if (IsPerClassWrapper(conj)) {
			inner.per = &conj;
		} else if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			inner.when = conj.children[1].get();
		} else if (!IsConstraintWrapper(conj)) {
			inner = DecideEnclosingScope(); // a plain AND: each child is its own clause
		}
		for (idx_t i = 0; i < conj.children.size(); i++) {
			if (IsConstraintChild(conj, i)) {
				BuildConstraintScopeChecks(*conj.children[i], inner, scope);
			}
		}
		return;
	}
	BuildClauseScopeChecks(constraints, enclosing, scope);
}

//===--------------------------------------------------------------------===//
// Scope checks (syntax_reference.md §7)
//===--------------------------------------------------------------------===//

//! The reducers `expr` reads outside every reducer: a reducer's body is read per group, so
//! the reducers inside it are not the clause's.
static void CollectOuterReducers(const Expression &expr, vector<const BoundAggregateExpression *> &reducers) {
	idx_t frame;
	if (TryGetDecideFrame(expr, frame)) {
		return; // no reducer: a frame reads one row
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		reducers.push_back(&expr.Cast<BoundAggregateExpression>());
		return;
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { CollectOuterReducers(child, reducers); });
}

void DecideCanonicalizer::BuildReducerScopeChecks(const BoundAggregateExpression &reducer,
                                                  const DecideReducerPath &path, DecideScopeChecks &scope) const {
	// D8: every group of the path key among the rows the path keeps has a row the
	// reducer's own `when` keeps -- for a reducer at the top, its `by` groups of the kept
	// rows.
	if (reducer.filter) {
		DecideSourceNames names {scope.source_fragments, scope.entity_scopes, scope.group_keys, scope.relations};
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::READS_A_ROW;
		check.rows = path.Rows();
		check.key = DecidePathKeyExpressions(path.KeysOf(DecideReducerGroupKey(reducer)), scope.group_keys,
		                                     &check.key_names);
		check.filter = reducer.filter->Copy();
		check.subject = RenderDecideReducer(reducer, names);
		scope.checks.push_back(std::move(check));
	}
	// The reducers in its argument read along its path.
	if (reducer.children.empty()) {
		return;
	}
	vector<const BoundAggregateExpression *> nested;
	CollectOuterReducers(*reducer.children[0], nested);
	auto below = path.Below(reducer);
	for (auto *inner : nested) {
		BuildReducerScopeChecks(*inner, below, scope);
	}
}

void DecideCanonicalizer::BuildClauseScopeChecks(const Expression &leaf, DecideEnclosingScope enclosing,
                                                 DecideScopeChecks &scope) const {
	DecideSourceNames names {scope.source_fragments, scope.entity_scopes, scope.group_keys, scope.relations};
	// D4: the `when` keeps a row of a non-empty input.
	if (enclosing.when) {
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::KEEPS_A_ROW;
		check.rows = enclosing.when->Copy();
		check.subject = RenderDecideSource(*enclosing.when, names);
		scope.checks.push_back(std::move(check));
	}
	DecideReducerPath path;
	if (enclosing.when) {
		path.conditions.push_back(enclosing.when);
	}
	vector<const BoundAggregateExpression *> reducers;
	CollectOuterReducers(leaf, reducers);
	for (auto *reducer : reducers) {
		BuildReducerScopeChecks(*reducer, path, scope);
	}
}

void DecideCanonicalizer::BuildObjectiveScopeChecks(const Expression &objective, DecideScopeChecks &scope) const {
	// A `when` in front.
	const Expression *body = &objective;
	DecideEnclosingScope enclosing;
	if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = body->Cast<BoundConjunctionExpression>();
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			enclosing.when = conj.children[1].get();
			body = conj.children[0].get();
		}
	}
	DecideSourceNames names {scope.source_fragments, scope.entity_scopes, scope.group_keys, scope.relations};
	if (enclosing.when) {
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::KEEPS_A_ROW;
		check.rows = enclosing.when->Copy();
		check.subject = RenderDecideSource(*enclosing.when, names);
		scope.checks.push_back(std::move(check));
	}
	DecideReducerPath path;
	if (enclosing.when) {
		path.conditions.push_back(enclosing.when);
	}
	vector<const BoundAggregateExpression *> reducers;
	CollectOuterReducers(*body, reducers);
	for (auto *reducer : reducers) {
		BuildReducerScopeChecks(*reducer, path, scope);
	}
}

//===--------------------------------------------------------------------===//
// Variant signatures (syntax_reference.md §7.1)
//===--------------------------------------------------------------------===//

//! Whether `fields` holds `field` already, or `key` does, which splits nothing further.
static bool HasSignatureField(const vector<DecideSignatureField> &fields, const vector<unique_ptr<Expression>> &key,
                              const DecideSignatureField &field) {
	if (field.kind == DecideSignatureFieldKind::VALUE) {
		for (auto &column : key) {
			if (column->Equals(*field.value)) {
				return true;
			}
		}
	}
	for (auto &existing : fields) {
		if (existing.kind != field.kind) {
			continue;
		}
		switch (field.kind) {
		case DecideSignatureFieldKind::VALUE:
			if (existing.value->Equals(*field.value)) {
				return true;
			}
			break;
		case DecideSignatureFieldKind::DECISION:
			if (existing.decision == field.decision) {
				return true;
			}
			break;
		case DecideSignatureFieldKind::BY_KEY:
			if (existing.group_key == field.group_key) {
				return true;
			}
			break;
		case DecideSignatureFieldKind::FRAME:
			if (existing.frame == field.frame) {
				return true;
			}
			break;
		}
	}
	return false;
}

void DecideCanonicalizer::CollectSignatureFields(const Expression &expr, const DecideSignatures &out, Clause clause,
                                                 const vector<unique_ptr<Expression>> &key,
                                                 vector<DecideSignatureField> &fields) const {
	auto add = [&](DecideSignatureField field) {
		if (!HasSignatureField(fields, key, field)) {
			fields.push_back(std::move(field));
		}
	};
	// Whether the key holds `column` already: then it splits no class.
	auto in_key = [&](const Expression &column) {
		for (auto &part : key) {
			if (part->Equals(column)) {
				return true;
			}
		}
		return false;
	};
	idx_t frame;
	if (TryGetDecideFrame(expr, frame) && frame < out.frames.size()) {
		// A frame is compared by its keys, not by the value it reads (D76): its timeline, the
		// anchor's place on it when its position is relative to the anchor's, and its
		// fallback where the anchor reaches it. `first` and `last` read one value per
		// timeline, so a key that holds the timeline's splits nothing further.
		auto &spec = out.frames[frame];
		if (!spec.Relative()) {
			bool covered = true;
			for (auto &part : spec.within) {
				covered = covered && in_key(*part);
			}
			if (covered) {
				return;
			}
		}
		DecideSignatureField field;
		field.kind = DecideSignatureFieldKind::FRAME;
		field.frame = frame;
		field.text = spec.text;
		add(std::move(field));
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// A reducer reads a group, not the row: its `by` key is the field, and its body is
		// a scoped expression of its own.
		idx_t group_key;
		if (TryParseGroupKeyTag(expr.GetAlias(), group_key) && group_key < out.group_keys.size()) {
			bool covered = true;
			for (auto &part : out.group_keys[group_key].expressions) {
				covered = covered && in_key(*part);
			}
			if (covered) {
				return; // `per g: sum(x) by (g)`: the class is the group
			}
			DecideSignatureField field;
			field.kind = DecideSignatureFieldKind::BY_KEY;
			field.group_key = group_key;
			field.text = "by (" + out.group_keys[group_key].text + ")";
			add(std::move(field));
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.Cast<BoundColumnRefExpression>().binding.table_index == decide_index) {
		// A decision is compared by instance; a query-wide one has one everywhere.
		auto var_idx = expr.Cast<BoundColumnRefExpression>().binding.column_index;
		if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar()) {
			return;
		}
		if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsEntity() &&
		    variable_scopes[var_idx].entity_scope_idx < out.entity_scopes.size()) {
			// Keyed on columns the key holds: one instance per class already.
			auto &declared = out.entity_scopes[variable_scopes[var_idx].entity_scope_idx];
			bool covered = !declared.entity_key_bindings.empty();
			for (idx_t i = 0; i < declared.entity_key_bindings.size() && covered; i++) {
				BoundColumnRefExpression column(declared.entity_key_column_types[i],
				                                declared.entity_key_bindings[i]);
				covered = in_key(column);
			}
			if (covered) {
				return;
			}
		}
		DecideSignatureField field;
		field.kind = DecideSignatureFieldKind::DECISION;
		field.decision = var_idx;
		field.text = expr.GetName();
		add(std::move(field));
		return;
	}
	if (!ReferencesDecideVar(expr) && !ContainsReducer(expr) && !ContainsDecideFrame(expr)) {
		// A literal or a query-wide value has one value everywhere.
		if (!expr.IsFoldable() && !IsQueryWideConstant(expr)) {
			DecideSignatureField field;
			field.value = expr.Copy();
			field.text = Quote(expr);
			add(std::move(field));
		}
		return;
	}
	vector<Atom> atoms;
	Decompose(expr, 1, atoms, clause);
	if (atoms.size() == 1 && atoms[0].expr == &expr && !atoms[0].scale) {
		// Not an additive context: each child is one of its own.
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { CollectSignatureFields(child, out, clause, key, fields); });
		return;
	}
	// An additive context: its data terms, read through `+`, `-` and constant factors, are
	// one field (D77), so written associativity never changes the count.
	vector<Atom> data;
	for (auto &atom : atoms) {
		if (!ReferencesDecideVar(*atom.expr) && !ContainsReducer(*atom.expr) && !ContainsDecideFrame(*atom.expr)) {
			if (!atom.expr->IsFoldable() && !IsQueryWideConstant(*atom.expr)) {
				data.push_back(std::move(atom));
			}
			continue;
		}
		CollectSignatureFields(*atom.expr, out, clause, key, fields);
		if (atom.scale) {
			CollectSignatureFields(*atom.scale, out, clause, key, fields);
		}
	}
	if (!data.empty()) {
		DecideSignatureField field;
		field.value = BuildAdditive(data, false);
		field.text = Quote(*field.value);
		add(std::move(field));
	}
}

idx_t DecideCanonicalizer::AddSignature(DecideSignatures &out, const Expression *rows,
                                        vector<unique_ptr<Expression>> key, vector<DecideSignatureField> fields,
                                        string text, idx_t source_clause_id) const {
	DecideSignature signature;
	signature.rows = rows ? rows->Copy() : nullptr;
	signature.key = std::move(key);
	signature.fields = std::move(fields);
	signature.text = std::move(text);
	signature.source_clause_id = source_clause_id;
	out.signatures.push_back(std::move(signature));
	return out.signatures.size() - 1;
}

void DecideCanonicalizer::SignQualifiedReducers(Expression &expr, const DecideReducerPath &path, Clause clause,
                                                idx_t source_clause_id, DecideSignatures &out) const {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		ExpressionIterator::EnumerateChildren(
		    expr, [&](Expression &child) { SignQualifiedReducers(child, path, clause, source_clause_id, out); });
		return;
	}
	auto &reducer = expr.Cast<BoundAggregateExpression>();
	// The reducers in its argument first, along its path (syntax_reference.md §5.5).
	auto below = path.Below(reducer);
	for (auto &child : reducer.children) {
		SignQualifiedReducers(*child, below, clause, source_clause_id, out);
	}
	idx_t scope_idx;
	if (!TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx) || scope_idx >= out.entity_scopes.size() ||
	    reducer.children.empty()) {
		return;
	}
	// The reducer's own `per K'` -- or `D` of the colon form `SUM(D: e)` -- hands it one
	// value per class and signature of its body (D64, D65), over the rows its `when` keeps.
	vector<unique_ptr<Expression>> key;
	{
		auto &key_scope = out.entity_scopes[scope_idx];
		for (idx_t i = 0; i < key_scope.entity_key_bindings.size(); i++) {
			key.push_back(make_uniq<BoundColumnRefExpression>(
			    i < key_scope.key_names.size() ? key_scope.key_names[i] : string(),
			    key_scope.entity_key_column_types[i], key_scope.entity_key_bindings[i]));
		}
	}
	vector<DecideSignatureField> fields;
	CollectSignatureFields(*reducer.children[0], out, clause, key, fields);
	if (fields.empty()) {
		return; // the key's classes are its values already
	}
	// The rows it reads: its path's, then its own `when`'s.
	auto reducer_rows = below.Rows();
	auto text = message_names ? RenderDecideReducer(reducer, *message_names) : reducer.ToString();
	auto signature =
	    AddSignature(out, reducer_rows.get(), std::move(key), std::move(fields), std::move(text), source_clause_id);
	// A scope of its own, named as its key is: the classes it reads are the key's split by
	// the signature, which no other reducer's or declaration's sharing the key are.
	EntityScopeInfo variant_scope;
	variant_scope.table_alias = out.entity_scopes[scope_idx].table_alias;
	variant_scope.source_table_indices = out.entity_scopes[scope_idx].source_table_indices;
	variant_scope.key_names = out.entity_scopes[scope_idx].key_names;
	variant_scope.signature = signature;
	out.entity_scopes.push_back(std::move(variant_scope));
	auto alias = reducer.GetAlias();
	RemoveDecideTag(alias, MakeQualifiedReducerTag(scope_idx));
	AddDecideTag(alias, MakeQualifiedReducerTag(out.entity_scopes.size() - 1));
	reducer.SetAlias(std::move(alias));
}

void DecideCanonicalizer::BuildConstraintSignatures(Expression &constraints, DecideSignatures &out) const {
	BuildConstraintSignatures(constraints, nullptr, out);
}

void DecideCanonicalizer::BuildConstraintSignatures(Expression &expr, const Expression *when,
                                                    DecideSignatures &out) const {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(expr.GetAlias(), source_clause_id);
		DecideReducerPath path;
		if (when) {
			path.conditions.push_back(when);
		}
		SignQualifiedReducers(expr, path, Clause::CONSTRAINT, source_clause_id, out);
		return;
	}
	auto &conj = expr.Cast<BoundConjunctionExpression>();
	const Expression *inner_when = when;
	if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
		inner_when = conj.children[1].get();
	} else if (!IsConstraintWrapper(conj)) {
		inner_when = nullptr; // a plain AND: each child is its own clause
	}
	for (idx_t i = 0; i < conj.children.size(); i++) {
		if (IsConstraintChild(conj, i)) {
			BuildConstraintSignatures(*conj.children[i], inner_when, out);
		}
	}
	if (!IsPerClassWrapper(conj) || conj.children.empty()) {
		return;
	}
	// `per K: C` is one instance per class of K and distinct value of C's fields (D64): its
	// tuple terms and decisions outside reducers and its reducers' `by` keys, read on the
	// canonical clause (D77). The binder puts a `when` under the wrapper.
	const Expression *rows = when;
	if (!rows && conj.children[0]->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &child = conj.children[0]->Cast<BoundConjunctionExpression>();
		if (IsWhenConstraintWrapper(child) && child.children.size() == 2) {
			rows = child.children[1].get();
		}
	}
	vector<unique_ptr<Expression>> key;
	for (idx_t i = 1; i < conj.children.size(); i++) {
		key.push_back(conj.children[i]->Copy());
	}
	vector<DecideSignatureField> fields;
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	ForEachConstraintLeaf(*conj.children[0], [&](const Expression &leaf) {
		CollectSignatureFields(leaf, out, Clause::CONSTRAINT, key, fields);
		if (source_clause_id == DConstants::INVALID_INDEX) {
			TryParseSourceClauseTag(leaf.GetAlias(), source_clause_id);
		}
	});
	if (fields.empty()) {
		return; // one instance per class, as the key alone gives
	}
	// The key as written (`per D.depotID`), which the binder recorded as a fragment.
	idx_t fragment;
	string key_text = "()";
	if (message_names && TryParseSourceFragmentTag(conj.GetAlias(), fragment) &&
	    fragment < message_names->fragments.size()) {
		key_text = message_names->fragments[fragment];
	}
	auto signature =
	    AddSignature(out, rows, std::move(key), std::move(fields), "per " + key_text, source_clause_id);
	auto alias = conj.GetAlias();
	AddDecideTag(alias, MakeSignatureTag(signature));
	conj.SetAlias(std::move(alias));
}

void DecideCanonicalizer::BuildObjectiveSignatures(Expression &objective, DecideSignatures &out) const {
	Expression *body = &objective;
	const Expression *when = nullptr;
	if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = body->Cast<BoundConjunctionExpression>();
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			when = conj.children[1].get();
			body = conj.children[0].get();
		}
	}
	DecideReducerPath path;
	if (when) {
		path.conditions.push_back(when);
	}
	SignQualifiedReducers(*body, path, Clause::OBJECTIVE, DConstants::INVALID_INDEX, out);
	// Produced once (`per ()`, D66): a field with a second value over the kept rows would
	// be a second objective, which execution refuses.
	vector<DecideSignatureField> fields;
	CollectSignatureFields(*body, out, Clause::OBJECTIVE, {}, fields);
	if (!fields.empty()) {
		out.objective = AddSignature(out, when, {}, std::move(fields), "the objective", DConstants::INVALID_INDEX);
	}
}


//===--------------------------------------------------------------------===//
// Frame placement (syntax_reference.md §5.6)
//===--------------------------------------------------------------------===//

bool DecideCanonicalizer::ReducerPerKey(const BoundAggregateExpression &reducer,
                                        const vector<EntityScopeInfo> &entity_scopes, FramePerKey &out) const {
	idx_t scope_idx;
	if (!TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx) || scope_idx >= entity_scopes.size()) {
		return false;
	}
	auto &key_scope = entity_scopes[scope_idx];
	for (idx_t i = 0; i < key_scope.entity_key_bindings.size(); i++) {
		auto name = i < key_scope.key_names.size() ? key_scope.key_names[i] : string();
		out.key.push_back(make_uniq<BoundColumnRefExpression>(name, key_scope.entity_key_column_types[i],
		                                                      key_scope.entity_key_bindings[i]));
		out.names.push_back(std::move(name));
	}
	out.text = key_scope.table_alias;
	return true;
}

void DecideCanonicalizer::PlaceFramesIn(const Expression &expr, const DecideReducerPath &path,
                                        const BoundAggregateExpression *reducer, idx_t source_clause_id,
                                        const FramePerKey *per, DecideFramePlacement &placement) const {
	idx_t frame_idx;
	if (TryGetDecideFrame(expr, frame_idx)) {
		if (frame_idx >= placement.frames.size()) {
			throw InternalException("DECIDE: a frame marker names frame %llu of %llu", frame_idx,
			                        placement.frames.size());
		}
		if (placement.placed[frame_idx]) {
			throw InternalException("DECIDE: frame %llu stands in two places", frame_idx);
		}
		placement.placed[frame_idx] = true;
		auto &frame = placement.frames[frame_idx];
		frame.rows = path.Rows();
		frame.scope_key = FindOrCreateDecidePathKey(path.keys, placement.group_keys);
		frame.source_clause_id = source_clause_id;
		frame.reduced = reducer != nullptr;
		// Its timelines are split by the path's `by` keys, then by `within`, whose names the
		// binder recorded as written.
		vector<string> path_names;
		auto path_key = DecidePathKeyExpressions(path.keys, placement.group_keys, &path_names);
		frame.timeline_names.insert(frame.timeline_names.begin(), path_names.begin(), path_names.end());
		// A position missing inside a reducer over decisions drops the member, decisions and
		// all, and may leave the reducer with none: the ANR plan's phase 6.
		if (reducer && ReferencesDecideVar(*reducer) && frame.MaySelectNothing()) {
			throw BinderException("DECIDE: %s may select no position inside %s, a reducer over decisions, which is "
			                      "not supported yet; %s",
			                      frame.text, QuoteAsWritten(*reducer), DecideFrameAbsenceRemedy(frame));
		}
		if (!per) {
			return;
		}
		// D69: the classes of the `per` key read one timeline each. Each is inside one group
		// of the path's keys, so only `within` can split one -- unless the key holds it.
		auto determined = [&](const Expression &part) {
			for (auto &column : per->key) {
				if (column->Equals(part)) {
					return true;
				}
			}
			for (auto &column : path_key) {
				if (column->Equals(part)) {
					return true;
				}
			}
			return false;
		};
		bool spans = false;
		for (auto &part : frame.within) {
			spans = spans || !determined(*part);
		}
		if (!spans) {
			return;
		}
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::FRAME_PARTITION;
		check.rows = path.Rows();
		check.frame = frame_idx;
		check.key = std::move(path_key);
		check.key_names = std::move(path_names);
		for (idx_t i = 0; i < per->key.size(); i++) {
			check.key.push_back(per->key[i]->Copy());
			check.key_names.push_back(per->names[i]);
		}
		check.subject = "per " + per->text + " does not determine the timeline " +
		                StringUtil::Join(frame.timeline_names, ", ") + " of " + frame.text;
		placement.scope.checks.push_back(std::move(check));
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// A reducer's argument is a scoped expression of its own: its frames read the rows of
		// the reducer's members, along its path, under its own `per`.
		auto &inner = expr.Cast<BoundAggregateExpression>();
		auto below = path.Below(inner);
		FramePerKey own;
		bool has_per = ReducerPerKey(inner, placement.scope.entity_scopes, own);
		for (auto &child : inner.children) {
			PlaceFramesIn(*child, below, &inner, source_clause_id, has_per ? &own : nullptr, placement);
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		PlaceFramesIn(child, path, reducer, source_clause_id, per, placement);
	});
}

void DecideCanonicalizer::PlaceClauseFrames(const Expression &expr, DecideEnclosingScope enclosing,
                                            DecideFramePlacement &placement) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto inner = enclosing;
		if (IsPerClassWrapper(conj)) {
			inner.per = &conj;
		} else if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			inner.when = conj.children[1].get();
		} else if (!IsConstraintWrapper(conj)) {
			inner = DecideEnclosingScope(); // a plain AND: each child is its own clause
		}
		for (idx_t i = 0; i < conj.children.size(); i++) {
			if (IsConstraintChild(conj, i)) {
				PlaceClauseFrames(*conj.children[i], inner, placement);
			}
		}
		return;
	}
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	TryParseSourceClauseTag(expr.GetAlias(), source_clause_id);
	// A frame reached at no id would read as the objective's (DecideFrameSpec).
	if (source_clause_id == DConstants::INVALID_INDEX && ContainsDecideFrame(expr)) {
		throw InternalException("DECIDE: a constraint holding a frame selector has no clause id: %s",
		                        expr.ToString());
	}
	DecideReducerPath path;
	if (enclosing.when) {
		path.conditions.push_back(enclosing.when);
	}
	FramePerKey per;
	if (enclosing.per) {
		auto &wrapper = *enclosing.per;
		for (idx_t i = 1; i < wrapper.children.size(); i++) {
			per.key.push_back(wrapper.children[i]->Copy());
			per.names.push_back(StripDecideTags(wrapper.children[i]->GetName())); // as written
		}
		// The key as written (`per D.depotID`), which the binder recorded as a fragment.
		idx_t fragment;
		per.text = "()";
		if (message_names && TryParseSourceFragmentTag(wrapper.GetAlias(), fragment) &&
		    fragment < message_names->fragments.size()) {
			per.text = message_names->fragments[fragment];
		} else if (!per.names.empty()) {
			per.text = StringUtil::Join(per.names, ", ");
		}
	}
	PlaceFramesIn(expr, path, nullptr, source_clause_id, enclosing.per ? &per : nullptr, placement);
}

void DecideCanonicalizer::PlaceFrames(const Expression &constraints, DecideFramePlacement &placement) const {
	placement.placed.resize(placement.frames.size(), false);
	PlaceClauseFrames(constraints, DecideEnclosingScope(), placement);
}

void DecideCanonicalizer::PlaceObjectiveFrames(const Expression &objective, DecideFramePlacement &placement) const {
	placement.placed.resize(placement.frames.size(), false);
	const Expression *body = &objective;
	DecideReducerPath path;
	if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = body->Cast<BoundConjunctionExpression>();
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			path.conditions.push_back(conj.children[1].get());
			body = conj.children[0].get();
		}
	}
	PlaceFramesIn(*body, path, nullptr, DConstants::INVALID_INDEX, nullptr, placement);
}

} // namespace duckdb
