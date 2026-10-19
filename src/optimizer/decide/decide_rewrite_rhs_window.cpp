//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_rhs_window.cpp
//
// Right-hand-side aggregates become window columns. See decide_optimizer.cpp.
//
// `SUM(x) <= SUM(cap)`: the right side is a reducer over data. The solver used to fold it row by row in DOUBLE
// (PhysicalDecide::EvaluateRhsReducerPerGroup), which loses digits DuckDB keeps. This pass replaces each such
// aggregate with a column of the DECIDE source: the same DuckDB aggregate, as a window over the clause's group. The
// solver and the direct-solve rules then both read one number, and the surrounding arithmetic (`COUNT(*) * k`) is
// evaluated in DuckDB's own types.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_factory.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"

namespace duckdb {

namespace {

//! The windows one DECIDE node needs, collected while the constraint tree is walked and installed as one
//! LogicalWindow directly above DECIDE's child. A window keeps its child's bindings and appends its own, so no
//! binding the DECIDE node (its entity scopes, source column names, clauses) holds changes.
class RhsWindowRewriter {
public:
	RhsWindowRewriter(Optimizer &optimizer_p, LogicalDecide &decide_p) : optimizer(optimizer_p), decide(decide_p) {
		decide.children[0]->ResolveOperatorTypes();
		child_bindings = decide.children[0]->GetColumnBindings();
	}

	void Run() {
		if (!decide.decide_constraints) {
			return;
		}
		Walk(*decide.decide_constraints);
		Install();
	}

private:
	//! A reducer replaced by a window column, remembered so an aggregate written twice shares one window.
	struct Placed {
		unique_ptr<Expression> aggregate;
		vector<unique_ptr<Expression>> keys;
		unique_ptr<Expression> when;
		unique_ptr<Expression> lhs_active;
		idx_t slot;
	};

	void Walk(Expression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = expr.Cast<BoundConjunctionExpression>();
			bool wrapper = IsConstraintWrapper(conjunction);
			auto saved_per = per_keys;
			auto saved_when = when;
			if (IsPerConstraintWrapper(conjunction)) {
				for (idx_t i = 1; i < conjunction.children.size(); i++) {
					per_keys.push_back(conjunction.children[i].get());
				}
			} else if (IsWhenConstraintWrapper(conjunction) && conjunction.children.size() == 2) {
				when = conjunction.children[1].get();
			}
			for (idx_t i = 0; i < conjunction.children.size(); i++) {
				if (!wrapper || i == 0) {
					Walk(*conjunction.children[i]);
				}
			}
			per_keys = std::move(saved_per);
			when = saved_when;
			return;
		}
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto &comparison = expr.Cast<BoundComparisonExpression>();
		// Only a clause whose left side holds a decision reducer is rewritten. A scalar decision on the left
		// (`s <= SUM(cap)`) is classified as a reduced clause because of the right-hand reducer; without it the clause
		// would read as one constraint per row, so it keeps the reducer.
		if (!HoldsDecisionReducer(*comparison.left)) {
			return;
		}
		ReadLeftActivity(*comparison.left);
		if (lhs_readable) {
			ReplaceReducers(comparison.right);
		}
	}

	//! The solver evaluates the right side only for groups with a row the left side covers: a row of the clause on
	//! which at least one left-hand reducer's own WHEN holds. A reducer with no WHEN of its own covers every row.
	void ReadLeftActivity(const Expression &left) {
		lhs_active.reset();
		lhs_readable = true;
		vector<const Expression *> filters;
		bool unfiltered = false;
		CollectLeftFilters(left, filters, unfiltered);
		if (unfiltered || filters.empty()) {
			return;
		}
		unique_ptr<Expression> any = DirectConstantBool(false);
		for (auto filter : filters) {
			if (!ReadableOverSource(*filter)) {
				lhs_readable = false;
				return;
			}
			any = make_uniq<BoundCaseExpression>(filter->Copy(), DirectConstantBool(true), std::move(any));
		}
		lhs_active = std::move(any);
	}

	void CollectLeftFilters(const Expression &expr, vector<const Expression *> &filters, bool &unfiltered) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			if (!BoundExpressionReferencesDecide(expr, decide.decide_index)) {
				return;
			}
			auto &aggregate = expr.Cast<BoundAggregateExpression>();
			if (aggregate.filter) {
				filters.push_back(aggregate.filter.get());
			} else {
				unfiltered = true;
			}
			return;
		}
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { CollectLeftFilters(child, filters, unfiltered); });
	}

	bool HoldsDecisionReducer(const Expression &expr) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			return BoundExpressionReferencesDecide(expr, decide.decide_index);
		}
		bool found = false;
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { found = found || HoldsDecisionReducer(child); });
		return found;
	}

	void ReplaceReducers(unique_ptr<Expression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			if (IsRewritable(expr->Cast<BoundAggregateExpression>())) {
				expr = MakeWindowColumn(std::move(expr));
			}
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { ReplaceReducers(child); });
	}

	bool ReadableOverSource(const Expression &expr) const {
		return DirectIsDecisionFreeDeterministic(expr, decide.decide_index) &&
		       DirectReferencesOnlySource(expr, child_bindings);
	}

	//! Whether the clause covers a row: its WHEN holds (a NULL WHEN does not) and no PER key is NULL. Null when the
	//! clause has neither, because every row is then covered.
	unique_ptr<Expression> ClauseCovers() const {
		if (per_keys.empty() && !when) {
			return nullptr;
		}
		unique_ptr<Expression> eligible = DirectActiveWhen(when);
		for (auto key : per_keys) {
			eligible = make_uniq<BoundCaseExpression>(DirectIsNull(key->Copy()), DirectConstantBool(false),
			                                          std::move(eligible));
		}
		return eligible;
	}

	//! Whether the group a row sits in is evaluated at all: the clause covers the row's group and the left side is
	//! active on some row of it. Null when every row qualifies. `covers` is the clause's own coverage.
	unique_ptr<Expression> Eligible(idx_t &coverage_slot) {
		auto covers = ClauseCovers();
		if (!lhs_active) {
			return covers;
		}
		if (coverage_index == DConstants::INVALID_INDEX) {
			coverage_index = optimizer.binder.GenerateTableIndex();
		}
		auto active = DirectWindowMatchingCount(
		    covers ? make_uniq<BoundCaseExpression>(covers->Copy(), lhs_active->Copy(), DirectConstantBool(false))
		           : lhs_active->Copy());
		if (covers) {
			active->partitions.push_back(covers->Copy());
		}
		for (auto key : per_keys) {
			active->partitions.push_back(key->Copy());
		}
		coverage_slot = coverage_windows.size();
		coverage_windows.push_back(std::move(active));
		auto positive = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHAN,
		    DirectColumn(LogicalType::BIGINT, ColumnBinding(coverage_index, coverage_slot)),
		    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
		return make_uniq<BoundCaseExpression>(
		    std::move(positive), covers ? std::move(covers) : DirectConstantBool(true), DirectConstantBool(false));
	}

	//! The reducers whose window equals the solver's reading: COUNT, MIN, MAX, SUM and AVG of one numeric expression
	//! over the source, with no WHEN, relation qualifier, DISTINCT, ORDER BY or norm of its own. Every other reducer
	//! stays on the solver's fold.
	bool IsRewritable(const BoundAggregateExpression &aggregate) const {
		// The clause's own PER keys and WHEN define the group, so they must be values the window can read.
		for (auto key : per_keys) {
			if (!ReadableOverSource(*key)) {
				return false;
			}
		}
		if (when && !ReadableOverSource(*when)) {
			return false;
		}
		auto name = StringUtil::Lower(aggregate.function.name);
		if (name != "count" && name != "count_star" && name != "min" && name != "max" && name != "sum" &&
		    name != "avg") {
			return false;
		}
		string payload;
		idx_t scope;
		if (aggregate.filter || aggregate.IsDistinct() || aggregate.order_bys ||
		    TryParseQualifiedReducerTag(aggregate.GetAlias(), scope) ||
		    ExtractDecideTagPayload(aggregate.GetAlias(), NORM_MARKER_TAG_PREFIX, payload) ||
		    ExtractDecideTagPayload(aggregate.GetAlias(), WRITTEN_NORM_TAG_PREFIX, payload) ||
		    HasDecideTag(aggregate.GetAlias(), AVG_REWRITE_TAG)) {
			return false;
		}
		if (name == "count_star") {
			return aggregate.children.empty();
		}
		if (aggregate.children.size() != 1) {
			return false;
		}
		auto &argument = *aggregate.children[0];
		return argument.return_type.IsNumeric() && !argument.IsAggregate() && !argument.HasSubquery() &&
		       DirectIsDecisionFreeDeterministic(argument, decide.decide_index) &&
		       DirectReferencesOnlySource(argument, child_bindings);
	}

	//! The aggregate's argument, checked on every row as the solver checks it (a NULL or NaN raises even where the
	//! clause covers no row), and read as 0 where the group does not cover the row. The NULL message names the columns
	//! that are NULL on the row; the NaN message is the solver's without its row position.
	unique_ptr<Expression> GuardedArgument(const Expression &argument, unique_ptr<Expression> eligible) {
		auto type = argument.return_type;
		auto raise = [&](unique_ptr<Expression> message) {
			return BoundCastExpression::AddCastToType(optimizer.context,
			                                          optimizer.BindScalarFunction("error", std::move(message)), type);
		};
		unique_ptr<Expression> guarded = argument.Copy();
		if (eligible) {
			auto zero = BoundCastExpression::AddCastToType(optimizer.context,
			                                               make_uniq<BoundConstantExpression>(Value::INTEGER(0)), type);
			guarded = make_uniq<BoundCaseExpression>(std::move(eligible), std::move(guarded), std::move(zero));
		}
		if (type == LogicalType::FLOAT || type == LogicalType::DOUBLE) {
			auto as_double =
			    BoundCastExpression::AddCastToType(optimizer.context, argument.Copy(), LogicalType::DOUBLE);
			auto nan_message = make_uniq<BoundConstantExpression>(
			    Value(DecideInvalidValueMessage("constraint right-hand side aggregate", "NaN", string())));
			guarded = make_uniq<BoundCaseExpression>(optimizer.BindScalarFunction("isnan", std::move(as_double)),
			                                         raise(std::move(nan_message)), std::move(guarded));
		}
		return make_uniq<BoundCaseExpression>(DirectIsNull(argument.Copy()),
		                                      raise(DirectNullMessageOf(optimizer, argument)), std::move(guarded));
	}

	bool SameScope(const Placed &placed) const {
		if (placed.keys.size() != per_keys.size() || static_cast<bool>(placed.when) != (when != nullptr)) {
			return false;
		}
		for (idx_t i = 0; i < per_keys.size(); i++) {
			if (!placed.keys[i]->Equals(*per_keys[i])) {
				return false;
			}
		}
		if (static_cast<bool>(placed.lhs_active) != static_cast<bool>(lhs_active) ||
		    (lhs_active && !placed.lhs_active->Equals(*lhs_active))) {
			return false;
		}
		return !when || placed.when->Equals(*when);
	}

	unique_ptr<Expression> MakeWindowColumn(unique_ptr<Expression> aggregate_p) {
		auto &aggregate = aggregate_p->Cast<BoundAggregateExpression>();
		if (window_index == DConstants::INVALID_INDEX) {
			window_index = optimizer.binder.GenerateTableIndex();
		}
		idx_t slot = DConstants::INVALID_INDEX;
		for (auto &placed : placements) {
			if (placed.aggregate->Equals(aggregate) && SameScope(placed)) {
				slot = placed.slot;
				break;
			}
		}
		if (slot == DConstants::INVALID_INDEX) {
			auto window = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, aggregate.return_type,
			                                               make_uniq<AggregateFunction>(aggregate.function),
			                                               aggregate.bind_info ? aggregate.bind_info->Copy() : nullptr);
			window->start = WindowBoundary::UNBOUNDED_PRECEDING;
			window->end = WindowBoundary::UNBOUNDED_FOLLOWING;
			// The group is the clause's covered rows by PER key. Rows it does not cover form partitions of their own
			// whose argument is 0, so they read 0; a count reads 0 there because its FILTER keeps no row.
			idx_t coverage_slot = DConstants::INVALID_INDEX;
			auto eligible = Eligible(coverage_slot);
			if (eligible) {
				window->partitions.push_back(eligible->Copy());
				for (auto key : per_keys) {
					window->partitions.push_back(key->Copy());
				}
				if (aggregate.function.name == "count" || aggregate.function.name == "count_star") {
					window->filter_expr = eligible->Copy();
				}
			}
			if (!aggregate.children.empty()) {
				window->children.push_back(
				    GuardedArgument(*aggregate.children[0], eligible ? eligible->Copy() : nullptr));
			}
			slot = windows.size();
			windows.push_back(std::move(window));
			Placed placed;
			placed.aggregate = aggregate.Copy();
			for (auto key : per_keys) {
				placed.keys.push_back(key->Copy());
			}
			if (when) {
				placed.when = when->Copy();
			}
			if (lhs_active) {
				placed.lhs_active = lhs_active->Copy();
			}
			placed.slot = slot;
			placements.push_back(std::move(placed));
		}
		// The column carries the written text of the aggregate as its name, so every place that quotes a bound
		// (the canonical EXPLAIN layer, the infeasible diagnosis) still reads `SUM(cap)`.
		auto text = RenderDecideSource(aggregate, decide.source_fragments, decide.entity_scopes);
		auto column =
		    make_uniq<BoundColumnRefExpression>(text, aggregate.return_type, ColumnBinding(window_index, slot));
		auto alias = aggregate.GetAlias();
		if (!alias.empty()) {
			column->SetAlias(text + alias);
		}
		return std::move(column);
	}

	void Install() {
		if (windows.empty()) {
			return;
		}
		if (!coverage_windows.empty()) {
			auto coverage = make_uniq<LogicalWindow>(coverage_index);
			coverage->expressions = std::move(coverage_windows);
			coverage->estimated_cardinality = decide.children[0]->estimated_cardinality;
			coverage->children.push_back(std::move(decide.children[0]));
			decide.children[0] = std::move(coverage);
		}
		auto window = make_uniq<LogicalWindow>(window_index);
		window->expressions = std::move(windows);
		window->estimated_cardinality = decide.children[0]->estimated_cardinality;
		window->children.push_back(std::move(decide.children[0]));
		decide.children[0] = std::move(window);
	}

	Optimizer &optimizer;
	LogicalDecide &decide;
	vector<ColumnBinding> child_bindings;
	vector<const Expression *> per_keys;
	const Expression *when = nullptr;
	//! Rows on which the clause's left side has something to reduce: the union of its aggregate-local WHENs, or null
	//! when some left-hand reducer has none (every row). A group where no row is active is never evaluated.
	unique_ptr<Expression> lhs_active;
	//! False when a left-hand filter cannot be read over the source, so the clause keeps the solver's fold.
	bool lhs_readable = true;
	idx_t window_index = DConstants::INVALID_INDEX;
	vector<unique_ptr<Expression>> windows;
	//! The windows that count, per group, the rows the left side is active on. The aggregate windows read them.
	idx_t coverage_index = DConstants::INVALID_INDEX;
	vector<unique_ptr<Expression>> coverage_windows;
	vector<Placed> placements;
};

} // namespace

void DecideOptimizer::RewriteRhsAggregates(LogicalDecide &decide) {
	if (decide.children.size() != 1) {
		return;
	}
	RhsWindowRewriter(optimizer, decide).Run();
}

} // namespace duckdb
