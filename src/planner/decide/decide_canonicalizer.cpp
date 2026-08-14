#include "duckdb/planner/decide/decide_canonicalizer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/decidb/decide_cast_policy.hpp"
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

static bool IsWhenConstraintWrapper(const BoundConjunctionExpression &conjunction) {
	return HasDecideTag(conjunction.GetAlias(), WHEN_CONSTRAINT_TAG);
}

static bool IsConstraintWrapper(const BoundConjunctionExpression &conjunction) {
	return IsPerConstraintTag(conjunction.GetAlias()) || IsWhenConstraintWrapper(conjunction);
}

static bool IsConstraintChild(const BoundConjunctionExpression &conjunction, idx_t child_index) {
	return !IsConstraintWrapper(conjunction) || child_index == 0;
}

//! Visit only nodes that belong to the constraint tree. WHEN conditions and PER
//! grouping columns are metadata children, so every read-only canonical pass shares
//! this boundary instead of reimplementing wrapper traversal.
template <class VISITOR>
static bool VisitCanonicalTree(const Expression &expression, VISITOR &visitor) {
	if (!visitor(expression)) {
		return false;
	}
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return true;
	}
	auto &conjunction = expression.Cast<BoundConjunctionExpression>();
	for (idx_t i = 0; i < conjunction.children.size(); i++) {
		if (IsConstraintChild(conjunction, i) && !VisitCanonicalTree(*conjunction.children[i], visitor)) {
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

	idx_t aggregate_child;
	if (is_div && UnwrapDecideCasts(*func.children[0], decide_index)->GetExpressionClass() ==
	                  ExpressionClass::BOUND_AGGREGATE) {
		aggregate_child = 0;
	} else if (is_mult && UnwrapDecideCasts(*func.children[1], decide_index)->GetExpressionClass() ==
	                         ExpressionClass::BOUND_AGGREGATE) {
		aggregate_child = 1;
	} else {
		return false;
	}
	if (UnwrapDecideCasts(*func.children[1 - aggregate_child], decide_index)->GetExpressionClass() ==
	    ExpressionClass::BOUND_AGGREGATE) {
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
//! typed, and neither is the `FILTER (WHERE w)` that an aggregate-local WHEN becomes.
static string UserFacingName(const Expression &expr) {
	const Expression *cur = StripCastsForIdentity(expr);
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
	const Expression *cur = StripCastsForIdentity(expr);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto idx = cur->Cast<BoundColumnRefExpression>().binding.table_index;
	return correlated_subquery_table_indexes.find(idx) != correlated_subquery_table_indexes.end();
}

bool DecideCanonicalizer::FindNonScalarDecideVar(
    const Expression &expr, idx_t &out_var_idx, const BoundColumnRefExpression *&out_ref) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			auto var_idx = colref.binding.column_index;
			if (var_idx >= variable_scopes.size() || !variable_scopes[var_idx].IsScalar()) {
				out_var_idx = var_idx;
				out_ref = &colref;
				return true;
			}
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = FindNonScalarDecideVar(child, out_var_idx, out_ref);
		}
	});
	return found;
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
			throw BinderException(
			    "%s: '%s' is a decision, so it cannot %s %s. "
			    "Only constants and query-wide values can scale SUM/AVG/MIN/MAX.",
			    clause_name, UserFacingName(*factor), verb, UserFacingName(*term));
		}
		if (!IsQueryWideConstant(*factor)) {
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

void DecideCanonicalizer::Decompose(const Expression &expr, int sign, vector<Atom> &out, Clause clause) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.function.name == "+" && func.children.size() == 2) {
			Decompose(*func.children[0], sign, out, clause);
			Decompose(*func.children[1], sign, out, clause);
			return;
		}
		if (func.function.name == "-" && func.children.size() == 2) {
			Decompose(*func.children[0], sign, out, clause);
			Decompose(*func.children[1], -sign, out, clause);
			return;
		}
		if (func.function.name == "-" && func.children.size() == 1) {
			Decompose(*func.children[0], -sign, out, clause);
			return;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		if (ReferencesDecideVar(expr)) {
			// The parsed boundary rejected every user-authored decision cast.
			// Anything left here was inserted by binding and is transparent in the
			// solver's single DOUBLE domain. Data-only casts stay atomic.
			Decompose(*cast.child, sign, out, clause);
			return;
		}
	}
	// Every other node is a term boundary -- but a factor sitting on a reducer is
	// peeled off it first, so the term the rest of the pipeline sees is the bare
	// reducer and the factor travels beside it.
	unique_ptr<Expression> scale;
	bool divides = false;
	auto &term = PeelScale(expr, scale, divides, clause);
	// Classify on the whole original term. The factors are decision-free and
	// reducer-free by construction, so this agrees with classifying `term` -- stated
	// rather than assumed, because Classify is the part that was wrong twice before.
	out.push_back(Atom {sign, &term, Classify(expr), std::move(scale), divides});
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

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeComparison(const Expression &expr) const {
	auto &cmp = expr.Cast<BoundComparisonExpression>();

	vector<Atom> left_atoms;
	vector<Atom> right_atoms;
	Decompose(*cmp.left, 1, left_atoms);
	Decompose(*cmp.right, 1, right_atoms);

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

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeObjective(const Expression &objective,
                                                                  double &out_constant_offset) const {
	// A WHEN/PER wrapper carries no algebra: recurse into the objective child and copy
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
		// fold and does not belong in an objective at all.
		double value = 0;
		if (!TryEvaluateFoldableDouble(context, *atom.expr, value)) {
			throw BinderException(
			    "MAXIMIZE/MINIMIZE: '%s' does not reference a decision, so it cannot be part of "
			    "the objective. Remove it, or move it to the SELECT list.",
			    UserFacingName(*atom.expr));
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
	return BuildAdditive(decision_atoms, false);
}

CanonicalConstraintClass DecideCanonicalizer::ClassifyCanonicalComparison(const Expression &expr) const {
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	if (ReferencesDecideVar(*cmp.right)) {
		return CanonicalConstraintClass::INVALID;
	}

	bool has_reducer = ContainsReducer(*cmp.left) || ContainsReducer(*cmp.right);
	if (!has_reducer) {
		return CanonicalConstraintClass::PER_ROW;
	}

	vector<Atom> lhs_atoms;
	Decompose(*cmp.left, 1, lhs_atoms);
	for (auto &atom : lhs_atoms) {
		if (atom.placement != Placement::LEFT) {
			return CanonicalConstraintClass::INVALID;
		}
		if (ContainsReducer(*atom.expr)) {
			// A legal reduced term is a reducer at the atom root. PeelScale already
			// removed the one supported outer factor representation before the atom
			// was recorded; any reducer still nested beneath another function is a
			// shape the physical aggregate extractor cannot consume.
			auto *root = UnwrapDecideCasts(*atom.expr, decide_index);
			if (root->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE ||
			    !ReferencesDecideVar(*root)) {
				return CanonicalConstraintClass::INVALID;
			}
			continue;
		}
		if (!IsQueryWideExpression(*atom.expr)) {
			return CanonicalConstraintClass::INVALID;
		}
	}
	return CanonicalConstraintClass::AGGREGATE;
}

CanonicalConstraintClass DecideCanonicalizer::ClassifyCanonicalTree(const Expression &constraints) const {
	CanonicalConstraintClass result = CanonicalConstraintClass::INVALID;
	bool first = true;
	auto classify = [&](const Expression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = expression.Cast<BoundConjunctionExpression>();
			return !IsConstraintWrapper(conjunction) || !conjunction.children.empty();
		}
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return false;
		}
		auto child_class = ClassifyCanonicalComparison(expression);
		if (child_class == CanonicalConstraintClass::INVALID || (!first && result != child_class)) {
			return false;
		}
		result = child_class;
		first = false;
		return true;
	};
	return VisitCanonicalTree(constraints, classify) && !first ? result : CanonicalConstraintClass::INVALID;
}

void DecideCanonicalizer::ValidateCanonicalComparison(const BoundComparisonExpression &comparison) const {
	auto classification = ClassifyCanonicalComparison(comparison);
	if (classification == CanonicalConstraintClass::INVALID) {
		vector<Atom> lhs_atoms;
		Decompose(*comparison.left, 1, lhs_atoms);
		for (auto &atom : lhs_atoms) {
			if (ContainsReducer(*atom.expr) || IsQueryWideExpression(*atom.expr)) {
				continue;
			}
			idx_t var_idx = DConstants::INVALID_INDEX;
			const BoundColumnRefExpression *var_ref = nullptr;
			if (FindNonScalarDecideVar(*atom.expr, var_idx, var_ref)) {
				bool is_entity = var_idx < variable_scopes.size() && variable_scopes[var_idx].IsEntity();
				throw BinderException(
				    "DECIDE constraint: %s decision '%s' cannot appear outside a reducer in an "
				    "aggregate constraint. Put it inside SUM/AVG/MIN/MAX, or use a scalar "
				    "decision when one query-wide value is intended.",
				    is_entity ? "entity-scoped" : "row-scoped", UserFacingName(*var_ref));
			}
			throw BinderException(
			    "DECIDE constraint: '%s' varies per row and cannot appear outside a reducer "
			    "in an aggregate constraint. Move the row-varying expression inside "
			    "SUM/AVG/MIN/MAX.",
			    UserFacingName(*atom.expr));
		}
		throw BinderException(
		    "DECIDE constraint mixes aggregate and per-row decision expressions. "
		    "Every row-varying decision term in an aggregate constraint must be inside "
		    "SUM/AVG/MIN/MAX.");
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
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = expression.Cast<BoundConjunctionExpression>();
			if (IsPerConstraintTag(conjunction.GetAlias()) &&
			    (conjunction.children.empty() ||
			     ClassifyCanonicalTree(*conjunction.children[0]) != CanonicalConstraintClass::AGGREGATE)) {
				throw BinderException(
				    "PER can only be applied to aggregate (SUM) constraints. Per-row constraints "
				    "already have one constraint per row.");
			}
		}
		return true;
	};
	VisitCanonicalTree(constraints, validate);
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
	Decompose(*comparison.left, 1, lhs_atoms);
	Decompose(*comparison.right, 1, rhs_atoms);
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
		if (IsPerConstraintTag(conjunction.GetAlias())) {
			if (conjunction.children.size() < 2) {
				CanonicalInvariantFailure("C0", "PER wrapper must contain a constraint and grouping column",
				                          expression);
			}
			if (ClassifyCanonicalTree(*conjunction.children[0]) != CanonicalConstraintClass::AGGREGATE) {
				CanonicalInvariantFailure("C5", "PER wrapper does not contain an aggregate constraint",
				                          expression);
			}
		}
		return true;
	};
	VisitCanonicalTree(constraints, verify);
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
}

void DecideCanonicalizer::VerifyCanonical(const Expression &constraints) const {
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
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTreeInternal(const Expression &constraints) const {
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = constraints.Cast<BoundConjunctionExpression>();

		auto result = make_uniq<BoundConjunctionExpression>(conj.type);
		result->SetAlias(conj.GetAlias());
		for (idx_t i = 0; i < conj.children.size(); i++) {
			// A WHEN/PER wrapper holds the constraint in child 0; the remaining
			// children are its condition / PER columns and are not constraints.
			result->children.push_back(IsConstraintChild(conj, i) ? CanonicalizeTreeInternal(*conj.children[i])
			                                                 : conj.children[i]->Copy());
		}
		return std::move(result);
	}
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		return CanonicalizeComparison(constraints);
	}
	return constraints.Copy();
}

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeTree(const Expression &constraints) const {
	auto result = CanonicalizeTreeInternal(constraints);
	ValidateCanonicalTree(*result);
	return result;
}

} // namespace duckdb
