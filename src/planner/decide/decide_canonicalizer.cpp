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

bool DecideCanonicalizer::IsLoneNegatedReducer(const vector<Atom> &atoms) const {
	return atoms.size() == 1 && atoms[0].sign < 0 &&
	       UnwrapDecideCasts(*atoms[0].expr, decide_index)->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE;
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
	// A scoped clause spells a lone negated reducer with its sign in its factor, the one
	// spelling a factor on a reducer has, so `-max(e) by (g)` reads as `-2 * max(e) by (g)`
	// does: one MIN/MAX under a factor whose sign is known; and `-(-max(e))` as `max(e)`.
	// The MIN/MAX rewrite routes on the tree it is given, so the tree must be the one its
	// atoms rebuild. The old spellings keep `-MAX(e)` (D17), which the composed MIN/MAX
	// route answers.
	const bool fold_negation = HasDecideTag(cmp.GetAlias(), SCOPED_CLAUSE_TAG);

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
	    !HasScale(left_atoms) &&
	    !(fold_negation && (IsLoneNegatedReducer(left_atoms) || IsSignedSpine(*cmp.left, left_atoms)))) {
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

	if (fold_negation && IsLoneNegatedReducer(new_left)) {
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

unique_ptr<Expression> DecideCanonicalizer::CanonicalizeObjective(const Expression &objective,
                                                                  double &out_constant_offset) const {
	DecideProfileScope profile("canonicalize.objective");
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

string DecideCanonicalizer::Quote(const Expression &expr) const {
	return message_names ? RenderDecideSource(expr, *message_names, /*scoped=*/true) : UserFacingName(expr);
}

DecideCanonicalizer::ScopedShape DecideCanonicalizer::ClassifyScoped(const BoundComparisonExpression &cmp,
                                                                     idx_t &group_key, string &refusal) const {
	group_key = DConstants::INVALID_INDEX;
	vector<Atom> lhs_atoms;
	Decompose(*cmp.left, 1, lhs_atoms);
	bool has_reducer = false;
	for (auto &atom : lhs_atoms) {
		has_reducer = has_reducer || ContainsReducer(*atom.expr);
	}
	if (!has_reducer) {
		// A data-only reducer sits on the right, as a bound.
		return ScopedShape::ROWS;
	}
	bool first_reducer = true;
	// A query-wide decision beside the reducers counts once per group -- except in a
	// quadratic or bilinear row, whose builder sums every term over the group's rows.
	const Expression *query_wide_decision = nullptr;
	bool nonlinear = false;
	// `<>` excludes an AVG's value by scaling the bound with the number of rows in the
	// group, which is exact only when every term is an AVG reading all of them.
	bool has_avg = false;
	bool only_plain_avgs = true;
	for (auto &atom : lhs_atoms) {
		nonlinear = nonlinear || !DecideExpressionDegree(*atom.expr, decide_index).IsLinear();
		if (!ContainsReducer(*atom.expr)) {
			only_plain_avgs = false;
			if (IsQueryWideExpression(*atom.expr)) {
				if (ReferencesDecideVar(*atom.expr)) {
					query_wide_decision = atom.expr;
				}
				continue;
			}
			idx_t var_idx;
			const BoundColumnRefExpression *var_ref = nullptr;
			refusal = FindNonScalarDecideVar(*atom.expr, var_idx, var_ref)
			              ? StringUtil::Format("DECIDE: the decision %s outside a reducer, in a constraint with "
			                                   "SUM, AVG, MIN or MAX, is not supported yet",
			                                   UserFacingName(*var_ref))
			              : StringUtil::Format("DECIDE: '%s' outside a reducer, in a constraint with SUM, AVG, "
			                                   "MIN or MAX, is not supported yet",
			                                   Quote(*atom.expr));
			return ScopedShape::UNSUPPORTED;
		}
		auto *root = UnwrapDecideCasts(*atom.expr, decide_index);
		if (root->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE || !ReferencesDecideVar(*root)) {
			refusal = StringUtil::Format("DECIDE: '%s' holds a reducer inside another expression, which is not "
			                             "supported yet",
			                             Quote(*atom.expr));
			return ScopedShape::UNSUPPORTED;
		}
		auto &reducer = root->Cast<BoundAggregateExpression>();
		idx_t qualifier;
		bool is_avg =
		    StringUtil::CIEquals(reducer.function.name, "avg") || HasDecideTag(reducer.GetAlias(), AVG_REWRITE_TAG);
		has_avg = has_avg || is_avg;
		only_plain_avgs = only_plain_avgs && is_avg && !reducer.filter &&
		                  !TryParseQualifiedReducerTag(reducer.GetAlias(), qualifier);
		idx_t key = DConstants::INVALID_INDEX;
		TryParseGroupKeyTag(root->GetAlias(), key);
		if (first_reducer) {
			group_key = key;
			first_reducer = false;
		} else if (key != group_key) {
			refusal = "DECIDE: reducers with different by keys in one constraint are not supported yet";
			return ScopedShape::UNSUPPORTED;
		}
	}
	if (query_wide_decision && nonlinear) {
		refusal = StringUtil::Format("DECIDE: '%s' beside a reducer, in a constraint that multiplies decisions, is "
		                             "not supported yet",
		                             Quote(*query_wide_decision));
		return ScopedShape::UNSUPPORTED;
	}
	if (cmp.type == ExpressionType::COMPARE_NOTEQUAL && has_avg && (!only_plain_avgs || nonlinear)) {
		refusal = "DECIDE: AVG in a <> constraint is not supported yet beside another term, over a product of "
		          "decisions, or with its own when or per";
		return ScopedShape::UNSUPPORTED;
	}
	// The composed formulation is one row over all of the clause's kept rows, and its
	// bound is one number.
	if (TakesComposedMinMax(lhs_atoms)) {
		static constexpr const char *COMPOSED =
		    "a MIN or MAX beside another term, or under a factor whose sign is not known before the query runs";
		if (group_key != DConstants::INVALID_INDEX) {
			refusal = StringUtil::Format("DECIDE: '%s' is not supported yet: by (...) cannot apply to %s",
			                             Quote(*cmp.left), COMPOSED);
			return ScopedShape::UNSUPPORTED;
		}
		if (!cmp.right->IsFoldable()) {
			refusal = StringUtil::Format(
			    "DECIDE: '%s' is not supported yet as the bound of %s; the bound must be a constant",
			    Quote(*cmp.right), COMPOSED);
			return ScopedShape::UNSUPPORTED;
		}
	}
	return ScopedShape::AGGREGATE;
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
	if (HasDecideTag(cmp.GetAlias(), SCOPED_CLAUSE_TAG)) {
		idx_t group_key;
		string refusal;
		switch (ClassifyScoped(cmp, group_key, refusal)) {
		case ScopedShape::ROWS:
			return CanonicalConstraintClass::PER_ROW;
		case ScopedShape::AGGREGATE:
			return CanonicalConstraintClass::AGGREGATE;
		default:
			return CanonicalConstraintClass::INVALID;
		}
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
	return VisitConstraintTree(constraints, classify) && !first ? result : CanonicalConstraintClass::INVALID;
}

void DecideCanonicalizer::ValidateCanonicalComparison(const BoundComparisonExpression &comparison) const {
	auto classification = ClassifyCanonicalComparison(comparison);
	if (classification == CanonicalConstraintClass::INVALID && HasDecideTag(comparison.GetAlias(), SCOPED_CLAUSE_TAG) &&
	    !ReferencesDecideVar(*comparison.right)) {
		idx_t group_key;
		string refusal;
		ClassifyScoped(comparison, group_key, refusal);
		throw BinderException(refusal);
	}
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
				if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsKeyed()) {
					throw BinderException(
					    "DECIDE constraint: decision '%s' (declared per %s) cannot appear outside a reducer in an "
					    "aggregate constraint. Put it inside SUM/AVG/MIN/MAX, or declare it per () when one "
					    "query-wide value is intended.",
					    UserFacingName(*var_ref), variable_scopes[var_idx].declared_key);
				}
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
			if (IsPerConstraintWrapper(conjunction) &&
			    (conjunction.children.empty() ||
			     ClassifyCanonicalTree(*conjunction.children[0]) != CanonicalConstraintClass::AGGREGATE)) {
				throw BinderException(
				    "PER can only be applied to aggregate (SUM) constraints. Per-row constraints "
				    "already have one constraint per row.");
			}
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
		if (IsPerClassWrapper(conjunction) && conjunction.children.empty()) {
			CanonicalInvariantFailure("C0", "per-class wrapper must contain a constraint", expression);
		}
		if (IsPerConstraintWrapper(conjunction)) {
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

//! The constraint under a clause's WHEN wrapper, or the clause itself.
static const Expression &ClauseLeaf(const Expression &clause) {
	if (clause.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = clause.Cast<BoundConjunctionExpression>();
		if (IsWhenConstraintWrapper(conj) && !conj.children.empty()) {
			return ClauseLeaf(*conj.children[0]);
		}
	}
	return clause;
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
		// `per K:` over a clause in the aggregate shape adds nothing to the model: its rows
		// are one per `by` group of the kept rows, and every per-K constraint in a group
		// differs only in its bound, which the group's tightest bound already covers once
		// K determines it (a scope check). The wrapper goes; the checks keep what K requires.
		if (IsPerClassWrapper(conj) && !result->children.empty()) {
			auto &leaf = ClauseLeaf(*result->children[0]);
			if (leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON &&
			    HasDecideTag(leaf.GetAlias(), SCOPED_CLAUSE_TAG) &&
			    ClassifyCanonicalComparison(leaf) == CanonicalConstraintClass::AGGREGATE) {
				return std::move(result->children[0]);
			}
		}
		return std::move(result);
	}
	if (constraints.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto result = CanonicalizeComparison(constraints);
		// A clause in the aggregate shape is grouped by the `by` key its decision reducers
		// share; the comparison carries it, as later rewrites rebuild the reducers.
		if (HasDecideTag(result->GetAlias(), SCOPED_CLAUSE_TAG) &&
		    !ReferencesDecideVar(*result->Cast<BoundComparisonExpression>().right)) {
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
	if (HasDecideTag(constraints.GetAlias(), SCOPED_CLAUSE_TAG)) {
		BuildClauseScopeChecks(constraints, enclosing, scope);
	}
}

//===--------------------------------------------------------------------===//
// Scope checks (anr_language_extension.md §7, §8)
//===--------------------------------------------------------------------===//

namespace {
//! What a clause reads once per class: its tuple terms -- data values and decisions
//! outside reducers -- and its reducers, whose bodies are read per group instead.
struct ClauseParts {
	vector<const Expression *> values;
	vector<const BoundColumnRefExpression *> decisions;
	vector<const BoundAggregateExpression *> reducers;
};
} // namespace

static bool HoldsReducer(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { found = found || HoldsReducer(child); });
	return found;
}

//! Fills `parts` from `expr`. A value is a maximal subtree with neither a decision nor a
//! reducer; a literal or a query-wide value has one value everywhere and is left out.
static void CollectClauseParts(const Expression &expr, idx_t decide_index,
                               const std::function<bool(const Expression &)> &query_wide, ClauseParts &parts) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		parts.reducers.push_back(&expr.Cast<BoundAggregateExpression>());
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.Cast<BoundColumnRefExpression>().binding.table_index == decide_index) {
		parts.decisions.push_back(&expr.Cast<BoundColumnRefExpression>());
		return;
	}
	if (!BoundExpressionReferencesDecide(expr, decide_index) && !HoldsReducer(expr)) {
		if (!expr.IsFoldable() && !query_wide(expr)) {
			parts.values.push_back(&expr);
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { CollectClauseParts(child, decide_index, query_wide, parts); });
}

//! A key column as a message names it: the name binding gave it (as written, or
//! relation-qualified for a relation element), else the expression rendered.
static string KeyColumnName(const Expression &column, const DecideSourceNames &names) {
	if (column.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return column.Cast<BoundColumnRefExpression>().GetName();
	}
	return RenderDecideSource(column, names);
}

//! How a message describes a decision's own key: `declared per row`, `declared per
//! D.depotID`, `declared as D.open`.
static string DeclaredScope(const DecideVarScopeInfo &scope, const vector<EntityScopeInfo> &entity_scopes,
                            const string &name) {
	if (scope.IsKeyed()) {
		return "declared per " + scope.declared_key;
	}
	if (scope.IsEntity() && scope.entity_scope_idx < entity_scopes.size()) {
		return "declared as " + entity_scopes[scope.entity_scope_idx].table_alias + "." + name;
	}
	return "declared per row";
}

void DecideCanonicalizer::BuildReducerScopeChecks(const BoundAggregateExpression &reducer, const Expression *rows,
                                                  DecideScopeChecks &scope) const {
	DecideSourceNames names {scope.source_fragments, scope.entity_scopes, scope.group_keys, scope.relations};
	auto reducer_text = RenderDecideReducer(reducer, names);
	idx_t group_key;
	const DecideGroupKey *by = TryParseGroupKeyTag(reducer.GetAlias(), group_key) && group_key < scope.group_keys.size()
	                               ? &scope.group_keys[group_key]
	                               : nullptr;
	auto by_key = [&](vector<unique_ptr<Expression>> &key, vector<string> &key_names) {
		if (!by) {
			return;
		}
		for (idx_t i = 0; i < by->expressions.size(); i++) {
			key.push_back(by->expressions[i]->Copy());
			key_names.push_back(i < by->names.size() ? by->names[i] : by->expressions[i]->ToString());
		}
	};
	// D8: every `by` group of the kept rows has a row the reducer's own `when` keeps.
	if (reducer.filter) {
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::READS_A_ROW;
		check.rows = rows ? rows->Copy() : nullptr;
		by_key(check.key, check.key_names);
		check.filter = reducer.filter->Copy();
		check.subject = reducer_text;
		scope.checks.push_back(std::move(check));
	}
	// The reducer's own `per K'` determines its body within each `by` group, over the rows
	// its `when` keeps. The colon form `SUM(D: e)` keeps its bind-time rule instead (D13).
	idx_t scope_idx;
	if (!TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx) || scope_idx >= scope.entity_scopes.size() ||
	    !scope.entity_scopes[scope_idx].exact_key || reducer.children.empty()) {
		return;
	}
	auto &key_scope = scope.entity_scopes[scope_idx];
	ClauseParts body;
	CollectClauseParts(*reducer.children[0], decide_index,
	                   [&](const Expression &expr) { return IsQueryWideConstant(expr); }, body);
	auto make_check = [&]() {
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::DETERMINES;
		check.rows = rows ? rows->Copy() : nullptr;
		for (idx_t i = 0; i < key_scope.entity_key_bindings.size(); i++) {
			check.key.push_back(make_uniq<BoundColumnRefExpression>(
			    i < key_scope.key_names.size() ? key_scope.key_names[i] : string(),
			    key_scope.entity_key_column_types[i], key_scope.entity_key_bindings[i]));
			check.key_names.push_back(i < key_scope.key_names.size() ? key_scope.key_names[i] : string());
		}
		by_key(check.key, check.key_names);
		check.filter = reducer.filter ? reducer.filter->Copy() : nullptr;
		check.subject = key_scope.table_alias;
		check.context = "in " + reducer_text;
		return check;
	};
	for (auto *value : body.values) {
		auto check = make_check();
		check.value = value->Copy();
		check.object = RenderDecideSource(*value, names);
		scope.checks.push_back(std::move(check));
	}
	for (auto *decision : body.decisions) {
		auto var_idx = decision->binding.column_index;
		if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar()) {
			continue;
		}
		auto check = make_check();
		check.decision = var_idx;
		check.object = decision->GetName();
		scope.checks.push_back(std::move(check));
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
	ClauseParts parts;
	CollectClauseParts(leaf, decide_index, [&](const Expression &expr) { return IsQueryWideConstant(expr); }, parts);
	// The `per` key determines every tuple term, decision and `by` key of the clause.
	if (enclosing.per) {
		auto &per = *enclosing.per;
		vector<string> key_names;
		for (idx_t i = 1; i < per.children.size(); i++) {
			key_names.push_back(KeyColumnName(*per.children[i], names));
		}
		idx_t fragment;
		auto key_text = TryParseSourceFragmentTag(per.GetAlias(), fragment) && fragment < scope.source_fragments.size()
		                    ? scope.source_fragments[fragment]
		                    : (key_names.empty() ? string("()") : StringUtil::Join(key_names, ", "));
		auto make_check = [&]() {
			DecideScopeCheck check;
			check.kind = DecideScopeCheckKind::DETERMINES;
			check.rows = enclosing.when ? enclosing.when->Copy() : nullptr;
			for (idx_t i = 1; i < per.children.size(); i++) {
				check.key.push_back(per.children[i]->Copy());
			}
			check.key_names = key_names;
			check.subject = key_text;
			return check;
		};
		for (auto *value : parts.values) {
			auto check = make_check();
			check.value = value->Copy();
			check.object = RenderDecideSource(*value, names);
			scope.checks.push_back(std::move(check));
		}
		for (auto *decision : parts.decisions) {
			auto var_idx = decision->binding.column_index;
			if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar()) {
				continue;
			}
			auto check = make_check();
			check.decision = var_idx;
			check.object = decision->GetName();
			check.context = var_idx < variable_scopes.size()
			                    ? DeclaredScope(variable_scopes[var_idx], scope.entity_scopes, decision->GetName())
			                    : string("declared per row");
			scope.checks.push_back(std::move(check));
		}
		for (auto *reducer : parts.reducers) {
			idx_t group_key;
			if (!TryParseGroupKeyTag(reducer->GetAlias(), group_key) || group_key >= scope.group_keys.size()) {
				continue;
			}
			auto &by = scope.group_keys[group_key];
			for (idx_t i = 0; i < by.expressions.size(); i++) {
				auto check = make_check();
				check.value = by.expressions[i]->Copy();
				check.object = i < by.names.size() ? by.names[i] : by.expressions[i]->ToString();
				check.context = "in by (" + by.text + ")";
				scope.checks.push_back(std::move(check));
			}
		}
	}
	for (auto *reducer : parts.reducers) {
		BuildReducerScopeChecks(*reducer, enclosing.when, scope);
	}
}

void DecideCanonicalizer::BuildObjectiveScopeChecks(const Expression &objective, DecideScopeChecks &scope) const {
	// The two-level form's PER wrapper and a `when` in front; the body is produced once.
	const Expression *body = &objective;
	DecideEnclosingScope enclosing;
	bool two_level = false;
	while (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = body->Cast<BoundConjunctionExpression>();
		if (IsPerConstraintWrapper(conj) && !conj.children.empty()) {
			two_level = true;
		} else if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			enclosing.when = conj.children[1].get();
		} else {
			break;
		}
		body = conj.children[0].get();
	}
	DecideSourceNames names {scope.source_fragments, scope.entity_scopes, scope.group_keys, scope.relations};
	if (enclosing.when) {
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::KEEPS_A_ROW;
		check.rows = enclosing.when->Copy();
		check.subject = RenderDecideSource(*enclosing.when, names);
		scope.checks.push_back(std::move(check));
	}
	if (two_level) {
		return; // its classes are the inner reducer's own `by` groups, its key's
	}
	ClauseParts parts;
	CollectClauseParts(*body, decide_index, [&](const Expression &expr) { return IsQueryWideConstant(expr); }, parts);
	auto make_check = [&]() {
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::DETERMINES;
		check.rows = enclosing.when ? enclosing.when->Copy() : nullptr;
		check.subject = "()";
		return check;
	};
	// Produced once (`per ()`): every value and decision outside a reducer has one value or
	// instance, and every top-level reducer one `by` group, over the kept rows.
	for (auto *value : parts.values) {
		auto check = make_check();
		check.value = value->Copy();
		check.object = RenderDecideSource(*value, names);
		scope.checks.push_back(std::move(check));
	}
	for (auto *decision : parts.decisions) {
		auto var_idx = decision->binding.column_index;
		if (var_idx < variable_scopes.size() && variable_scopes[var_idx].IsScalar()) {
			continue;
		}
		auto check = make_check();
		check.decision = var_idx;
		check.object = decision->GetName();
		check.context = var_idx < variable_scopes.size()
		                    ? DeclaredScope(variable_scopes[var_idx], scope.entity_scopes, decision->GetName())
		                    : string("declared per row");
		scope.checks.push_back(std::move(check));
	}
	for (auto *reducer : parts.reducers) {
		idx_t group_key;
		if (TryParseGroupKeyTag(reducer->GetAlias(), group_key) && group_key < scope.group_keys.size()) {
			auto &by = scope.group_keys[group_key];
			for (idx_t i = 0; i < by.expressions.size(); i++) {
				auto check = make_check();
				check.value = by.expressions[i]->Copy();
				check.object = i < by.names.size() ? by.names[i] : by.expressions[i]->ToString();
				check.context = "in by (" + by.text + ")";
				scope.checks.push_back(std::move(check));
			}
		}
		BuildReducerScopeChecks(*reducer, enclosing.when, scope);
	}
}

} // namespace duckdb
