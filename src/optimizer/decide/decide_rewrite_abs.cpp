//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_rewrite_abs.cpp
//
// DECIDE ABS handling: which encoding each ABS takes, and ABS linearization. See decide_optimizer.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/optimizer/decide/decide_value_checks.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"

#include <cstdlib>
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic_render.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

// ---------------------------------------------------------------------------
// ABS soundness handling
// ---------------------------------------------------------------------------
//
// DeciDB's ABS linearization replaces `ABS(e)` with an auxiliary `aux` and
// adds the lower-envelope constraints `aux >= e` and `aux >= -e`. That alone
// only forces `aux >= |e|`. Soundness then requires that aux be pinned to
// exactly |e|, not free above it. There are three pinning mechanisms:
//
//   1. Solver pressure: aux contributes to the objective with the sign the
//      sense pushes down -- positively under MINIMIZE, negatively under
//      MAXIMIZE -- and the solver pushes aux down to |e|. Sound — no Big-M
//      needed.
//   2. Constraint context that upper-bounds aux: ABS on the LHS of `<=`/`<`,
//      or on the RHS of `>=`/`>`. The constraint itself caps aux from
//      above, the lower envelope caps it from below at |e|. Sound — no
//      Big-M needed.
//   3. Big-M upper envelope with a sign-indicator binary y: emit
//      `aux <= e + 2M(1-y)` and `aux <= -e + 2M*y`. Combined with the
//      lower envelope this forces aux = |e| exactly, regardless of solver
//      pressure or constraint shape. Used for an objective ABS the sense
//      pushes up, and for any constraint shape outside category 2.
//
// Which one an ABS takes is an inherited attribute: the push of the body that holds it, read from the body's owner and
// passed down to the ABS. The owners are the clauses (the relation pushes the whole side: `<=` pushes a positive term
// down), the objective (minimized, which pushes a positive term down), a definition (`t >= q` pushes `q` down, `t <= q`
// up, `=` neither) and a value link (its sides, ValueLinkPush). A body several readers share, a helper's, is owned by
// the one definition that holds every side any reader needs, so the attribute is read only once the rewrites that
// join the readers have run (RewriteScopedReducers, RewriteFrameRanges, PrepareDefinitionFunctions), and every reader
// of the body gets the one encoding.
//
// The objective reads like a constraint: `MINIMIZE E` pins what `E <= K` pins and `MAXIMIZE E` what `E >= K` pins, so
// `MINIMIZE sum(abs(e))` and `MAXIMIZE -abs(e)` are held by pressure while `MAXIMIZE sum(abs(e))` and
// `MINIMIZE -abs(e)` are not. A term under a factor whose sign is unknown until the query runs (a data column weight)
// is not pinned either: `MINIMIZE sum(w * abs(e))` with one negative `w` was called unbounded.
//
// Aggregates over ABS compose under the same rule: when the push on the aggregate's value is the one the aggregate's
// members share, the auxes underneath are pinned transitively and Big-M is not needed. A MIN or MAX in a clause is a
// helper decision (a value link) but for the easy identity, which bounds each member; a helper pushed its hard way
// is encoded with a Big-M over its members, which needs every member's box, and an auxiliary held by pressure alone
// has none, so its members, and the members of every helper they read, always take the exact envelope, which boxes
// them (FindBoxedHelpers).
// ---------------------------------------------------------------------------

//! `abs(e)` with a decision in `e`.
static bool IsAbsOverDecideVar(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	return func.function.name == "@" && BoundExpressionReferencesDecide(*func.children[0], decide_index);
}

static bool ContainsAbsOverDecideVar(const Expression &expr, idx_t decide_index) {
	if (IsAbsOverDecideVar(expr, decide_index)) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsAbsOverDecideVar(child, decide_index);
		}
	});
	return found;
}

//! Marks, in `exact`, the ABS nodes of `expr` that its push does not pin. `pinning_sign` is the sign a term must carry
//! to be pushed down (+1 under `<=` and MINIMIZE, -1 under `>=`, 0 when nothing is pushed); the sign each ABS carries
//! is the one `expr` gives it once expanded additively (WalkPolarity).
//!
//! A factor whose sign is not known until execution -- a data column, as in `SUM(w * ABS(x - t))` -- yields sign 0,
//! "unknown", which never matches the pinning direction and so forces Big-M. That is conservative: for `w >= 0` the
//! aux would have been pinned without it. Deciding per row at execution time would recover those rows, but exactness
//! is one answer per ABS, given at plan time, so it cannot be chosen lazily as the code stands. A factor that is
//! exactly zero reads as positive, as for any other term. An ABS nested inside another sits under a function that is
//! not monotone in it, so no pressure and no shape pins it: it carries sign 0.
static void MarkExactAbs(ClientContext &context, Expression &expr, int sign, int pinning_sign, idx_t decide_index,
                         AbsExactSet &exact) {
	PolarityHolds holds = [&](const Expression &node) {
		return ContainsAbsOverDecideVar(node, decide_index);
	};
	PolarityVisit visit = [&](Expression &node, int node_sign) {
		if (!IsAbsOverDecideVar(node, decide_index)) {
			return false;
		}
		if (pinning_sign == 0 || node_sign != pinning_sign) {
			exact.insert(&node);
		}
		WalkPolarity(context, *node.Cast<BoundFunctionExpression>().children[0], 0, holds, visit);
		return true;
	};
	WalkPolarity(context, expr, sign, holds, visit);
}

//! A clause read as `E <op> 0` with `E = LHS - RHS`: every ABS is then a single signed term of one expression, and
//! "does this comparison push the aux down?" is a question about that term's sign rather than about which side of the
//! relation it happens to be written on.
static void MarkExactAbsInClause(ClientContext &context, Expression &clause, idx_t decide_index, AbsExactSet &exact) {
	// A `when` or `per` scope only affects which rows participate in the aggregate, and aux pinning is unconditional
	// per row, so a wrapper does not change classification -- the ordinary constraint-position descent is exactly
	// right here.
	ForEachConstraintLeaf(clause, [&](Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return; // a structural constant: BETWEEN and IN are comparisons by now
		}
		auto &comp = node.Cast<BoundComparisonExpression>();
		auto t = comp.type;
		int pinning_sign;
		if (t == ExpressionType::COMPARE_LESSTHAN || t == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
			pinning_sign = 1; // E bounded above: a positive term is pushed down
		} else if (t == ExpressionType::COMPARE_GREATERTHAN || t == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
			pinning_sign = -1; // E bounded below: a negative term is pushed down
		} else {
			pinning_sign = 0; // equality, <>: pin nothing
		}
		MarkExactAbs(context, *comp.left, 1, pinning_sign, decide_index, exact);
		MarkExactAbs(context, *comp.right, -1, pinning_sign, decide_index, exact);
	});
}

//! The sign a term of a value link's members must carry to be pushed down, from the sides the link states
//! (ReducerValueLink::need_envelope, need_closing), which join every reader's push (RewriteScopedReducers): `value >=
//! members` of a SUM, AVG or MAX holds the members down, `value <= members` of a MIN holds them up, and `value <=
//! members` of a SUM or AVG holds them up as well, the sign it reads them with the other way round. A MIN or MAX's
//! closing side is the hard direction, encoded with a Big-M over its members, and both sides read the members from
//! both directions: no sign is pinned, which is 0.
static int ValueLinkPush(const LogicalDecide::ReducerValueLink &link) {
	if (link.need_envelope == link.need_closing) {
		return 0;
	}
	const bool extremum = IsExtremumName(link.agg_name);
	if (link.need_envelope) {
		return link.agg_name == "min" ? -1 : 1;
	}
	return extremum ? 0 : -1;
}

//! The helpers whose box a hard extremum needs: the MIN or MAX links stating their closing side, which encode it with a
//! Big-M over their members, and every helper those members read, whose own members box it in turn. A helper held by
//! pressure alone has no box, so the ABS in the body of such a helper takes the exact envelope whatever pushes the
//! helper itself.
struct BoxedHelpers {
	vector<bool> links;
	vector<bool> definitions;
};

static BoxedHelpers FindBoxedHelpers(LogicalDecide &decide) {
	BoxedHelpers boxed {vector<bool>(decide.reducer_value_links.size(), false),
	                    vector<bool>(decide.definition_functions.size(), false)};
	auto binding_of = [&](idx_t var) {
		return decide.decide_variables[var]->Cast<BoundColumnRefExpression>().binding;
	};
	std::function<void(const Expression &)> demand = [&](const Expression &expr) {
		// A non-linear body stays where it was written, tagged as a read of its definition's helper.
		idx_t defined;
		if (TryParseDecideIndexTag(expr.GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, defined)) {
			for (idx_t i = 0; i < decide.definition_functions.size(); i++) {
				auto &definition = decide.definition_functions[i];
				if (!boxed.definitions[i] && definition.value_var == defined) {
					boxed.definitions[i] = true;
					if (definition.body) {
						demand(*definition.body);
					}
				}
			}
			return;
		}
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			ExpressionIterator::EnumerateChildren(expr, demand);
			return;
		}
		auto &binding = expr.Cast<BoundColumnRefExpression>().binding;
		for (idx_t i = 0; i < decide.reducer_value_links.size(); i++) {
			auto &link = decide.reducer_value_links[i];
			if (!boxed.links[i] && binding_of(link.value_var) == binding) {
				boxed.links[i] = true;
				demand(*link.inner_expr);
			}
		}
		for (idx_t i = 0; i < decide.definition_functions.size(); i++) {
			auto &definition = decide.definition_functions[i];
			if (!boxed.definitions[i] && binding_of(definition.value_var) == binding) {
				boxed.definitions[i] = true;
				if (definition.body) {
					demand(*definition.body);
				}
			}
		}
	};
	for (idx_t i = 0; i < decide.reducer_value_links.size(); i++) {
		auto &link = decide.reducer_value_links[i];
		if (!boxed.links[i] && IsExtremumName(link.agg_name) && link.need_closing) {
			boxed.links[i] = true;
			demand(*link.inner_expr);
		}
	}
	return boxed;
}

//! The ABS nodes, in every body of `decide`, that their owners do not pin (MarkExactAbs). The owners are read in the
//! form the rewrites leave them in: a reducer that became a helper is its link's, and a reducer's non-linear body its
//! definition's.
static AbsExactSet FindExactAbs(ClientContext &context, LogicalDecide &decide) {
	AbsExactSet exact;
	const idx_t decide_index = decide.decide_index;
	if (decide.decide_constraints) {
		MarkExactAbsInClause(context, *decide.decide_constraints, decide_index, exact);
	}
	// L0 indicator rows live as function metadata rather than in the main clause tree, but their reverse links still
	// need the same exactness classification as ordinary constraints.
	for (auto &indicator : decide.l0_indicator_functions) {
		for (auto &link : indicator.links) {
			MarkExactAbsInClause(context, *link, decide_index, exact);
		}
	}
	// `t >= q` holds the body down, `t <= q` up, `t = q` neither.
	const auto boxed = FindBoxedHelpers(decide);
	for (idx_t i = 0; i < decide.definition_functions.size(); i++) {
		auto &definition = decide.definition_functions[i];
		if (definition.body) {
			const bool down = definition.relation == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
			const bool up = definition.relation == ExpressionType::COMPARE_LESSTHANOREQUALTO;
			MarkExactAbs(context, *definition.body, 1,
			             boxed.definitions[i] ? 0
			             : down               ? 1
			             : up                 ? -1
			                                  : 0,
			             decide_index, exact);
		}
	}
	for (idx_t i = 0; i < decide.reducer_value_links.size(); i++) {
		auto &link = decide.reducer_value_links[i];
		MarkExactAbs(context, *link.inner_expr, 1, boxed.links[i] ? 0 : ValueLinkPush(link), decide_index, exact);
	}
	if (decide.decide_objective) {
		// A `when` or two-level `per` wrapper carries no sign: the body is its first child. The objective is
		// minimized, which pushes a positive term down.
		Expression *body = decide.decide_objective.get();
		while (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			body = body->Cast<BoundConjunctionExpression>().children[0].get();
		}
		MarkExactAbs(context, *body, 1, 1, decide_index, exact);
	}
	return exact;
}

//! The rows an ABS is read on: those the clause's `when` keeps that also pass the `when` of each reducer around the
//! ABS (`SUM(when f: ABS(x - v))`), as the reducer reads its own term. Null for every row.
static unique_ptr<Expression> AbsReadRows(const Expression *clause_when, const Expression *reducer_when) {
	if (!clause_when || !reducer_when) {
		const Expression *only = clause_when ? clause_when : reducer_when;
		return only ? only->Copy() : nullptr;
	}
	// A conjunction, as every scope condition is, so each conjunct keeps its tags (a reducer's NULL test,
	// SKIP_NULL_TAG).
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, clause_when->Copy(),
	                                             reducer_when->Copy());
}

//! The clause an ABS was written in, or the objective: what a message about a number computed from the ABS' argument
//! points at (DecideWrittenClause on the solver path). An ABS in a body that belongs to no one clause (a reducer value
//! helper's, shared by every clause that reads it; a definition's) is a constraint of no place.
static DecideClauseRef AbsWrittenClause(const LogicalDecide &decide, const string &clause_alias) {
	idx_t source_clause_id;
	if (TryParseSourceClauseTag(clause_alias, source_clause_id) &&
	    source_clause_id < decide.constraint_sources.size()) {
		return DecideClauseRefOf(decide.constraint_sources, decide.source_text, source_clause_id);
	}
	if (HasDecideTag(clause_alias, OBJECTIVE_ROW_TAG)) {
		return DecideClauseRefOf(decide.constraint_sources, decide.source_text, DConstants::INVALID_INDEX);
	}
	return DecideClauseRef();
}

//! Refuses an argument of an ABS whose data is NaN or infinite where the ABS is read, on the rows it is read on, before
//! the lowering turns the data into a bound on the auxiliary. The data is the sum of the argument's terms that hold no
//! decision (`-v` in `ABS(x - v)`). A row where it is not finite raises the error every other number the model reads
//! raises (DecideNumberCheck, as the direct-solve plans): the data, as the user wrote it, where it stands. A
//! coefficient of a decision is a coefficient of the lowered row, which the solver path refuses by name already.
static void AddAbsValueCheck(ClientContext &context, LogicalDecide &decide, const Expression &argument,
                             const Expression *rows, const DecideClauseRef &clause) {
	vector<DecideSplitTerm> terms;
	DecideTermSplitter(context, decide).Split(argument, 1, terms);
	ExpressionBuilder calc(context);
	// The tree evaluates the check, so a frame marker keeps its tag and is read at the row the frame selects (`Of`
	// strips it, which would read the marker as a reducer over the rows).
	DecideNumberNames names;
	names.clause = clause;
	ExprPtr total;
	for (auto &term : terms) {
		if (!term.IsConstant()) {
			continue;
		}
		ExprPtr value =
		    term.coefficient ? calc.Cast(calc.Wrap(term.coefficient->Copy()), LogicalType::DOUBLE) : calc.Num(1.0);
		value = term.sign < 0 ? calc.Neg(value) : value;
		total = total ? calc.Add(total, value) : value;
		if (term.coefficient) {
			DecideNameValue(
			    names, *term.coefficient, term.sign,
			    RenderDecideSource(*term.coefficient, decide.source_text, decide.keyed_scopes, &decide.keys),
			    DecideSpanLocation(decide.source_text, DecideUnguarded(*term.coefficient).GetQueryLocation()));
		}
	}
	if (!total) {
		return;
	}
	double constant;
	if (total->IsFoldable() &&
	    TryEvaluateFoldableDouble(context, *calc.Out(total), constant, FOLD_STABLE | FOLD_CATCH_DUCKDB_ERRORS) &&
	    std::isfinite(constant)) {
		return;
	}
	auto check = DecideNumberCheck(calc, total, names);
	ExprPtr kept = rows ? calc.Wrap(DecideActiveWhen(calc, rows)) : nullptr;
	AddValueCheck(decide, calc.Out(DecideCheckOnKeptRows(calc, kept, check)));
}

// ---------------------------------------------------------------------------
// ABS recording (allocate helpers, retain their function nodes, record each ABS for the tree and stage 06)
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAbs(LogicalDecide &decide) {
	// Find ABS(expr) nodes over decide vars and allocate their helper variables. Keep the function expressions in the
	// user's clauses; the tree owns their value through an F node, and term extraction resolves the tagged function to
	// the already allocated helper column. Each ABS is recorded (LogicalDecide::abs_functions) with its operand, the
	// clause it was written in (the objective's rows carry OBJECTIVE_ROW_TAG) and whether it needs the exact encoding.
	//
	// This is the whole of stage 05's part. The lower envelope
	//   aux >= inner  and  aux >= -inner
	// (which forces aux >= |inner|), the choice of how an exact ABS is pinned, and the indicator and Big-M rows of the
	// lowered form are all stage 06's: ABS is a pseudo-operation here, as in LLVM's legalization, which the lowering
	// either states natively or expands (EmitAbsConstraints), so no rewrite reads what the backend can state.
	//
	// An ABS is exact -- needs its upper side pinned as well -- when
	//   - the objective's sense pushes the auxiliary up: a positive ABS term under MAXIMIZE, a negative one under
	//     MINIMIZE, or one under a factor whose sign is not known until the query runs (FindExactAbs reads the
	//     objective by sign, as it reads a constraint).
	//   - ABS appears in a constraint shape that does not upper-bound aux (the hard direction: ABS(...) >= K,
	//     ABS = K, ABS <> K, BETWEEN, etc.).
	//   - ABS sits inside another ABS: the outer one is not monotone in it, so no pressure or shape pins it (sign 0 in
	//     FindExactAbs).
	//   - ABS sits in a helper's body that its readers push up, or that a hard MIN or MAX reads, whose Big-M needs
	//     every member boxed (FindBoxedHelpers).
	// A positive ABS under MINIMIZE, a negative one under MAXIMIZE, and a sound constraint shape (e.g. ABS <= K) are
	// not: solver pressure / direct upper-bounding handles pinning.
	const auto exact = FindExactAbs(optimizer.context, decide);
	DecideCanonicalizer canonicalizer(optimizer.context, decide.decide_index, decide.variable_scopes,
	                                  decide.keyed_scopes, decide.keys, decide.is_boolean_var);
	if (decide.decide_constraints) {
		FindAbsAndAllocateAux(decide.decide_constraints, decide, canonicalizer, exact, string(), DecideEnclosingScope(),
		                      nullptr);
	}
	// A definition body has left the ordinary clause list; its F node still needs the same ABS helpers and envelopes.
	for (auto &definition : decide.definition_functions) {
		if (!definition.body) {
			continue;
		}
		auto scoped = ScopeHelperRow(definition.body->Copy(), definition.condition.get());
		auto scope = EnterDecideScope(*scoped, DecideEnclosingScope());
		string alias = MakeDefinesTag(definition.value_var);
		AddDecideTag(alias, BODY_DEFINITION_TAG);
		FindAbsAndAllocateAux(definition.body, decide, canonicalizer, exact, alias, scope, nullptr);
	}
	if (decide.decide_objective) {
		auto objective = std::move(decide.decide_objective);
		FindAbsAndAllocateAux(objective, decide, canonicalizer, exact, OBJECTIVE_ROW_TAG, DecideEnclosingScope(),
		                      nullptr);
		decide.SetObjective(optimizer.context, std::move(objective));
	}
	// A reducer value helper's body belongs to no clause: it is read on the rows its path and its own `when` keep, so
	// its row and the check of its values are made for those.
	for (auto &link : decide.reducer_value_links) {
		auto link_rows = AbsReadRows(link.when.get(), link.filter.get());
		FindAbsAndAllocateAux(link.inner_expr, decide, canonicalizer, exact, string(), DecideEnclosingScope(),
		                      link_rows.get());
	}
	// `norm(e, 0)` owns its forward and reverse indicator links as tree metadata. The reverse link's `ABS(e)` still
	// needs the same plan-time helper allocation as an ABS written in a clause.
	for (auto &indicator : decide.l0_indicator_functions) {
		for (auto &link : indicator.links) {
			FindAbsAndAllocateAux(link, decide, canonicalizer, exact, string(), DecideEnclosingScope(), nullptr);
		}
	}
}

unique_ptr<Expression> decide_rewrite::MakeAbsRow(ClientContext &context, const LogicalDecide &decide,
                                                  const LogicalDecide::AbsFunction &function, bool negated) {
	// `aux >= inner` or `aux >= -inner`, in its natural form, which can leave decisions on both sides. The tree brings
	// each through the canonical boundary as it does any clause.
	auto &aux = decide.decide_variables[function.aux_idx]->Cast<BoundColumnRefExpression>();
	unique_ptr<Expression> bound = function.operand->Copy();
	if (negated) {
		// `0 - inner`
		vector<unique_ptr<Expression>> children;
		children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(0)));
		children.push_back(std::move(bound));
		FunctionBinder binder(context);
		ErrorData error;
		bound = binder.BindScalarFunction(DEFAULT_SCHEMA, "-", std::move(children), error);
		if (error.HasError()) {
			throw InternalException("DECIDE ABS: failed to bind the negated operand: %s", error.Message());
		}
	}
	auto row = make_uniq<BoundComparisonExpression>(
	    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	    make_uniq<BoundColumnRefExpression>(aux.alias, aux.return_type, aux.binding), std::move(bound));
	AddDecideTag(row->alias, negated ? MakeAbsFunctionNegTag(function.aux_idx) : MakeAbsFunctionTag(function.aux_idx));
	MarkFormulationConstraint(*row, function.source_alias, function.aux_idx);
	return ScopeRowPerInstance(decide, ScopeHelperRow(std::move(row), function.when.get()), function.scope);
}

void DecideOptimizer::FindAbsAndAllocateAux(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                            const DecideCanonicalizer &canonicalizer, const AbsExactSet &exact,
                                            const string &source_alias, DecideEnclosingScope scope,
                                            const Expression *reducer_when) {
	if (!expr) {
		return;
	}
	auto clause_alias = DescendSourceAlias(*expr, source_alias);
	scope = EnterDecideScope(*expr, scope);
	idx_t definition_aux;
	if (TryParseDecideIndexTag(expr->GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, definition_aux)) {
		return; // its body is visited through DefinitionFunction metadata below
	}

	// A term of a reducer with its own `when` reads only the rows that `when` keeps.
	unique_ptr<Expression> reducer_rows;
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &reducer = expr->Cast<BoundAggregateExpression>();
		if (reducer.filter) {
			reducer_rows = AbsReadRows(reducer_when, reducer.filter.get());
			reducer_when = reducer_rows.get();
		}
	}

	if (IsAbsOverDecideVar(*expr, decide.decide_index)) {
		auto &func = expr->Cast<BoundFunctionExpression>();
		// Declare the auxiliary as INTEGER when the inner expression is
		// integer-typed — |k| preserves integer-valuedness, so downstream
		// strict-inequality rewrites (`SUM(|...|) < K → <= K-1`) stay sound.
		// Without this, all ABS auxes are DOUBLE and the LHS-integer check
		// in ilp_model_builder would reject valid integer-valued strict cases.
		// Nested ABS functions will be tagged as helpers, but this function node remains in the source tree.
		const LogicalType inner_type = func.children[0]->return_type;
		bool inner_is_integer = inner_type.IsIntegral();
		LogicalType aux_type = inner_is_integer ? LogicalType::INTEGER : LogicalType::DOUBLE;

		// F6: a diagnosis and a plan name the helper by the user's ABS(inner), under the name it was written with
		// (`abs` or `@`), rendered before nested ABS nodes receive their helper tags.
		string label = DecideWrittenName(func.GetAlias(), func.function.name) + "(" +
		               RenderForPlan(*func.children[0], decide) + ")";

		// Innermost first: an ABS inside this one gets its helper first. The copied
		// operand retains that tagged function, which term extraction resolves to the
		// inner helper before building this function's envelope rows.
		FindAbsAndAllocateAux(func.children[0], decide, canonicalizer, exact, clause_alias, scope, reducer_when);

		// A column defined by its argument: one per instance of the scopes the argument reads.
		auto aux_scope = ScopeOf(decide, canonicalizer, *func.children[0]);
		idx_t aux_idx =
		    AddAuxiliaryDecision(decide, string(ABS_AUX_NAME_PREFIX) + to_string(decide.abs_functions.size()) + "__",
		                         aux_type, false, aux_scope, label);

		// FindExactAbs is the one place that decides exactness: for an ABS in a clause or the objective, in a helper's
		// body, and in another ABS (which it reads as unknown sign).
		const bool needs_exact = exact.count(&func) > 0;

		// The rows the ABS is read on: its row, and the check of its values, are made for those only.
		auto read_rows = AbsReadRows(scope.Condition(), reducer_when);
		AddAbsValueCheck(optimizer.context, decide, *func.children[0], read_rows.get(),
		                 AbsWrittenClause(decide, clause_alias));

		// Record the ABS: the F node reads its operand, and stage 06 its encoding.
		AddRewriteAccount(decide, clause_alias, LogicalDecide::RewriteAccount::Kind::HELPER, std::move(label));
		LogicalDecide::AbsFunction function;
		function.aux_idx = aux_idx;
		function.operand = func.children[0]->Copy();
		function.exact = needs_exact;
		function.when = std::move(read_rows);
		function.scope = aux_scope;
		function.source_alias = clause_alias;
		function.written = RenderDecideSource(func, decide.source_text, decide.keyed_scopes, &decide.keys);
		decide.abs_functions.push_back(std::move(function));

		// Preserve ABS(inner) in the clause tree and tag its value-helper mapping for
		// the F-aware term splitter.
		AddDecideTag(func.alias, MakeAbsFunctionTag(aux_idx));
		return;
	}

	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		FindAbsAndAllocateAux(child, decide, canonicalizer, exact, clause_alias, scope, reducer_when);
	});
}

} // namespace duckdb
