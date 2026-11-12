#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/planner/column_binding.hpp"

namespace duckdb {

class Binder;
class ColumnBindingReplacer;
class LogicalWindow;
class Optimizer;

//! What a ranked-prefix plan carries from one stage to the next for one level: the columns of its scope, and the row's
//! rank in its group.
struct DirectLevelColumns : DirectScopeColumns {
	explicit DirectLevelColumns(DirectScopeColumns columns) : DirectScopeColumns(std::move(columns)) {
	}
	//! The row's rank among the rows the level ranks (BIGINT), the level's window's column or P2's forward of it; unset
	//! in P1.
	ColumnBinding rank;
};

//! The columns a ranked-prefix plan (DirectRankedPrefix) carries from one stage to the next.
struct DirectRankedColumns {
	vector<ColumnBinding> source;
	//! One per level, finest first.
	vector<DirectLevelColumns> levels;
	//! Whether a clause fixes the row to 1 and to 0; set only with fixed rows.
	ColumnBinding fixed_one;
	ColumnBinding fixed_zero;
	//! The checks the rule put below the rank window (DirectRowLayout::checks_below, and the builder's), true when
	//! they pass; unset when there are none.
	ColumnBinding row_valid;
	//! The checks above it (the rule's `checks_above` and LayoutPrefix's, and the builder's); set in the stage after
	//! the rank window, and unset when there are none.
	ColumnBinding valid;
	//! The size of the group's prefix, the column W2 gives; null without a W2 (PrefixSize).
	ExprPtr size;
};

//! What a rule lays out over the rows (LayoutRows): its ranking column, and the checks it owes over what it laid out.
struct DirectRowLayout {
	//! The ranking column, a DOUBLE.
	ColumnBinding rank_key;
	//! The checks over the scope projection's columns, which stand below the rank window.
	vector<unique_ptr<Expression>> checks_below;
	//! The checks over P1's columns, which stand above it.
	vector<unique_ptr<Expression>> checks_above;
};

//! A plan that ranks the rows of each group by one column, selects a prefix of the ranking and answers 1 inside it
//! (S3: the longest run of the lightest rows that fits a budget; S1: the best k rows by score). The builder fixes the
//! plan and a rule gives the pieces that differ, as the Template Method of GoF names it: `Build` is the template and
//! the protected virtual members are its steps.
//!
//! The rows are ranked over a chain of levels, finest first: each level has a scope (a `by` key, a `when`, a `per`
//! key), group bounds and a rank. The first level ranks every row not fixed. A level after it ranks, in its own
//! groups and in the rule's order (RankKey), only the rows the level before it keeps, and a row is kept by a level
//! when the rule says it is within the level's limit (WithinLimit) and the levels before keep it (Keeps). S1 and S3
//! have one level.
//!
//!   S    the scope projection (DirectProjectScopes): for each level the eligible flag and the group key, and the
//!        fixed-value flags
//!   P1   the rule's columns (LayoutRows), among them the ranking column; the checks of the fixed values; the checks
//!        below the rank window
//!   W1   for each level, per group, in order of the ranking column: the rank, what the rule adds (AddGroupWindows),
//!        and the group limits (AddGroupBound), over the one read. A level's window stands over the last one's, which
//!        gives it the keep flag of the level before it as the expression the rule gives (WithinLimit)
//!   PL   what the group limits give, once per row, as columns (only with a group limit)
//!   P2   the rule's prefix columns (LayoutPrefix); the checks above W1 and what the group limits raise
//!   W2   per group, the size of the prefix (PrefixSize); only a rule whose prefix size needs a window of its own
//!   H    the rows held in a materialized CTE (DirectHeldRows) over everything above, and the verdict, whether every
//!        group can be met (GroupVerdict), raised in the hold's naming projection with the checks the plan owes; only
//!        where a check stands above the last window
//!   A    the source columns, x (Chosen), the column of the checks, over a reference to the held rows
//!
//! Every row is checked before the first row leaves (`architecture/rules.md` § What every rule must match). A window
//! holds every row before it gives one, then gives them group by group, so a check above it reaches a later group only
//! when the parent reads that far. A check stands below a window that holds every row, and what it reads says which
//! stage: a check over the scope projection's columns stands in P1, below the rank window (`checks_below` of
//! LayoutRows); a check over P1's or W1's columns stands in P2, below W2 when the rule has one (`checks_above` of
//! LayoutRows, and what LayoutPrefix returns). Where no window stands above P2, or a verdict stands above W2, the plan
//! holds the rows in H instead of in a window over all the rows (which DuckDB runs on one thread, and which keeps the
//! rows above it on one thread): the CTE's definition runs to its end before A reads a row, and the verdict, raised in
//! its naming projection, is read on every row, so every group has passed its checks when the first row leaves. A
//! never raises the verdict itself: A runs in the scan's pipeline, which a LIMIT stops. The builder adds the checks of
//! the fixed values (each value a row fixes, a row fixed to both) to the checks below.
//!
//! Rows of equal ranking value are ordered as the sort gives them, and the prefix is right only if every window of W1
//! that reads the order (the rank, a running sum) sees the same one. They have one partition and one order, so DuckDB
//! sorts them once (PhysicalPlanGenerator::CreatePlan(LogicalWindow), which puts the windows whose
//! BoundWindowExpression::PartitionsAreEquivalent into one PhysicalWindow). A level has its own partition, so it has
//! its own sort, and ties may break differently at each level.
class DirectRankedPrefix {
public:
	virtual ~DirectRankedPrefix() = default;

	//! The substitute over the one read `rows`: the source columns, then x (INTEGER 0/1), then the column of the
	//! checks when the plan owes any (`validated_columns`).
	DirectSubstitute Build(unique_ptr<LogicalOperator> rows);

protected:
	//! `scopes` and `fixed` are the rule's, read at Build: the rows and groups each level ranks, finest first, and the
	//! rows its clauses fix to 1 or to 0, which leave the ranking and keep the value they are fixed to.
	DirectRankedPrefix(Optimizer &optimizer, const vector<ColumnBinding> &source_bindings,
	                   const vector<LogicalType> &source_types, const DirectStateErrors &errors,
	                   const vector<reference<const DirectSharedScope>> &scopes, const DirectFixedValues &fixed);

	//! The direction of the ranking: ascending ranks the smallest ranking value first.
	virtual OrderType RankOrder() const = 0;
	//! P1: adds the rule's columns over `state` (its source columns are `state.input_bindings`) and returns the ranking
	//! column, a DOUBLE, and the checks it owes. The builder ranks a row a clause fixes to 1 or to 0 last (RankKey).
	virtual DirectRowLayout LayoutRows(DirectProjectionStage &p1, const DirectScopeState &state) = 0;
	//! W1: adds to `w1` the windows of the rule over the group of `level` in order of the ranking column (a running sum
	//! of it, a count), over the rank's partition and order (OverRank). `rows` are the columns of P1 and of the
	//! levels' windows before this one, RankKey the ranking column's.
	virtual void AddGroupWindows(idx_t level, LogicalWindow &w1, const DirectRankedColumns &rows) = 0;
	//! Whether the row is within the limit of `level` (a BOOLEAN, never NULL): a row outside the level's groups is,
	//! and so is a row ranked within the group's room for rows. Over the columns of the level's window and of those
	//! before it (`rows`), and the group values of the level (GroupValues). Called only for a rule with several
	//! levels.
	virtual ExprPtr WithinLimit(idx_t level, const DirectRankedColumns &rows) const {
		throw InternalException("DirectRankedPrefix: a rule with several levels gives WithinLimit");
	}
	//! P2: adds the rule's columns over `input`, the columns W1 gives (P1's columns and the ranks), and the group
	//! limits (GroupValues), and returns the checks it owes above W1, over the same columns.
	virtual vector<unique_ptr<Expression>> LayoutPrefix(DirectProjectionStage &p2,
	                                                    const DirectRankedColumns &input) = 0;
	//! W2: the window, over the group it sizes (PartitionByGroup), that gives each group the size of its prefix, the
	//! `size` of DirectRankedColumns; null for a rule that reads the size off the group's own columns.
	virtual unique_ptr<BoundWindowExpression> PrefixSize(const DirectRankedColumns &rows) {
		return nullptr;
	}
	//! H: whether the row's group in `level` can be met, a BOOLEAN over the columns of P2 and W2; null where no group
	//! of the level can fail. The builder takes a row outside the level's groups as met and a row as met when every
	//! level meets it. A false raises the infeasible error before the first row leaves.
	virtual ExprPtr GroupVerdict(idx_t level, const DirectRankedColumns &rows) = 0;
	//! A: whether the row is selected, where no clause fixes it; the builder lets a fixed row keep its value.
	virtual ExprPtr Chosen(const DirectRankedColumns &rows) = 0;

	//! Registers a group bound of `level` whose aggregates W1 reads over the level's group, and returns its index into
	//! GroupValues of the level. A rule registers its bounds when it is built.
	idx_t AddGroupBound(idx_t level, const DecideGroupBound &bound);
	//! What the registered bound of `level` gives, over the columns of the level's window; valid in LayoutPrefix and
	//! after (and in WithinLimit of the level and the levels after it).
	const DirectGroupValues &GroupValues(idx_t level, idx_t bound) const;

	//! Makes `window`, an aggregate over the whole group, take the rank's partition and order. DuckDB runs a window
	//! with no order, or no partition, on one thread and sorts it on its own; with the rank's order it shares the
	//! rank's sort and stays parallel, and its frame over the whole partition (ROWS BETWEEN UNBOUNDED PRECEDING AND
	//! UNBOUNDED FOLLOWING) leaves its value the same on every row. The group limits take it, and a rule's own windows
	//! over the group should too (AddGroupWindows). `level` is the level's columns.
	void OverRank(const DirectScopeColumns &level, BoundWindowExpression &window) const;
	//! The ranking column as W1 reads it, an expression over P1's columns: the rule's column, NULL on a row a clause
	//! fixes to 1 or to 0, which ranks last and which a sum skips.
	ExprPtr RankKey() const;
	//! Makes `window` run over the rows of `level`'s group. A row outside every group has a NULL where the key is
	//! compared with `=`, so it is apart from every group.
	void PartitionByGroup(const DirectScopeColumns &level, BoundWindowExpression &window) const;
	//! Whether the levels up to and including `level` keep the row: each says it is within its limit (WithinLimit).
	ExprPtr Keeps(idx_t level, const DirectRankedColumns &rows) const;
	//! Whether a clause fixes the row to 1 and to 0, over `state`.
	ExprPtr FixedOne(const DirectScopeState &state) const;
	ExprPtr FixedZero(const DirectScopeState &state) const;
	//! A copy of a proved expression whose source references follow the state projection, when there is one.
	unique_ptr<Expression> OverState(const DirectScopeState &state, const Expression &value) const;
	bool HasFixes() const {
		return fixed.Any();
	}
	ExprPtr Column(const LogicalType &type, ColumnBinding binding) const {
		return calc.Col(type, binding);
	}

	//! A level of the chain: the rows and groups it ranks and its group bounds, which a rule reads only through
	//! `scope` and `scoped`.
	struct Level {
		explicit Level(const DirectSharedScope &scope_p) : scope(scope_p), scoped(scope_p.Scoped()) {
		}
		const DirectSharedScope &scope;
		//! Whether the rows have a scope: a `by` key, a `when` or a `per` key.
		const bool scoped;
		//! The bounds a rule registered (AddGroupBound), where P1 laid out what each reads, and what each gives once
		//! the level's window has computed it.
		vector<const DecideGroupBound *> bounds;
		vector<vector<DirectGroupInputColumns>> bound_inputs;
		vector<DirectGroupValues> bound_values;
	};

	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;
	//! The levels, finest first.
	vector<Level> levels;
	const DirectFixedValues &fixed;

private:
	//! The rows of a stage held in a materialized CTE, set by CloseGroups; `rows` is null when they are not.
	struct Hold {
		unique_ptr<DirectHeldRows> rows;
		//! The plan that is held, and the reference the stage reads in its place; `reference.plan` is moved into the
		//! stage's plan.
		unique_ptr<LogicalOperator> definition;
		DirectHeldRows::Reference reference;
		//! The column of the checks the rows are held with: every check the stage owes, and the verdict.
		ColumnBinding checks;
	};
	//! A plan and the columns that stage carries.
	struct Stage {
		unique_ptr<LogicalOperator> plan;
		DirectRankedColumns columns;
		Hold hold;
		//! The checks the stage above this one owes over its columns (P1's, which stand in P2).
		vector<unique_ptr<Expression>> checks;
	};

	//! S: the scope projection over the one read.
	DirectScopeState ProjectScopes(unique_ptr<LogicalOperator> rows);
	//! Lays out in `p1` what each registered group bound reads, and appends the checks of what it reads to `checks`.
	void LayoutGroupInputs(DirectProjectionStage &p1, const DirectScopeState &state,
	                       vector<unique_ptr<Expression>> &checks);
	//! Appends the check of each message the group bounds raise to `checks`.
	void AppendGroupMessages(vector<unique_ptr<Expression>> &checks) const;
	bool HasGroupBounds() const;
	Stage ProjectRows(unique_ptr<LogicalOperator> rows);
	//! The rank of the rows of `level`: ROW_NUMBER in the first level, which ranks every row not fixed (a fixed row
	//! has a NULL ranking value and ranks last), and a running count of the rows the level before keeps in the others.
	unique_ptr<BoundWindowExpression> RankWindow(idx_t level, const DirectRankedColumns &rows) const;
	void RankRows(Stage &rows);
	//! Puts what the group bounds give into columns of a projection over W1 (PL), and records in `forwarded` where each
	//! column of W1 goes, for the stage above to be re-pointed.
	void HoistGroupValues(Stage &ranked, ColumnBindingReplacer &forwarded);
	Stage ProjectPrefix(Stage ranked);
	//! Adds W2 over `prefix`, and holds its rows (DirectHeldRows) where a check stands above the last window.
	void CloseGroups(Stage &prefix);
	//! The checks the stage owes, folded in the order they run (`row_valid`, `valid`, then `verdict`, when there is
	//! one): true when every one holds, and otherwise the error of the first that raises. Null when there are none.
	unique_ptr<Expression> OwedChecks(const DirectRankedColumns &columns, unique_ptr<Expression> verdict) const;
	//! Whether every level meets the row's group, from the rule's verdict of each (GroupVerdict); null when none can
	//! fail.
	ExprPtr AllLevelsMet(const DirectRankedColumns &columns);
	DirectSubstitute Answer(Stage prefix);

	//! The rule's ranking column in P1, and the columns of P1 that say a clause fixes the row (set with fixed rows).
	ColumnBinding rank_key;
	ColumnBinding key_fixed_one;
	ColumnBinding key_fixed_zero;
};

} // namespace duckdb
