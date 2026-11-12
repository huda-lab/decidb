#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/planner/column_binding.hpp"

namespace duckdb {

class Binder;
class LogicalWindow;
class Optimizer;

//! The columns a ranked-prefix plan (DirectRankedPrefix) carries from one stage to the next.
struct DirectRankedColumns {
	DirectCarried carried;
	//! Whether a clause fixes the row to 1 and to 0; set only with fixed rows.
	ColumnBinding fixed_one;
	ColumnBinding fixed_zero;
	//! The row's rank in its group (BIGINT), W1's column or P2's forward of it; unset in P1.
	ColumnBinding rank;
	//! The checks the rule put below the rank window (`checks_below`), true when they pass; unset when it has none.
	ColumnBinding row_valid;
	//! The checks the rule put above it (`checks_above`); set in the stage after the rank window, and unset when there
	//! are none.
	ColumnBinding valid;
	//! The size of the group's prefix, the column W2 gives; null without a W2 (PrefixSize).
	ExprPtr size;
};

//! A plan that ranks the rows of each group by one column, selects a prefix of the ranking and answers 1 inside it
//! (S3: the longest run of the lightest rows that fits a budget; S1: the best k rows by score). The builder fixes the
//! plan and a rule gives the pieces that differ, as the Template Method of GoF names it: `Build` is the template and
//! the protected virtual members are its steps.
//!
//!   S    the scope projection (DirectProjectScope): the eligible flag, the group key and the fixed-value flags
//!   P1   the rule's columns (LayoutRows), among them the ranking column; the checks of the fixed values; the checks
//!        below the rank window
//!   W1   per group, in order of the ranking column: the rank, what the rule adds (AddGroupWindows), and the group
//!        limits (AddGroupBound), over the one read
//!   P2   the rule's prefix columns (LayoutPrefix); the checks above W1 and what the group limits raise
//!   W2   per group, the size of the prefix (PrefixSize); only a rule whose prefix size needs a window of its own
//!   W3   over all the rows, whether every group can be met (GroupVerdict); only where a group can fail
//!   A    the source columns, x (Chosen), the column of the checks
//!
//! Every row is checked before the first row leaves (`architecture/rules.md` § What every rule must match). A window
//! holds every row before it gives one, then gives them group by group, so a check above it reaches a later group only
//! when the parent reads that far. A check stands below a window that holds every row, and what it reads says which
//! stage: a check over the scope projection's columns stands in P1, below the rank window (`checks_below`); a check
//! over P1's or W1's columns stands in P2, below W2 or W3 (`checks_above`). A rule that adds one of the second kind
//! must have a window over all the rows above P2 (PrefixSize or GroupVerdict), which the plan has where a group can
//! fail. The group's own checks, in W3, read every row, so every group has passed them when the first row leaves. The
//! builder adds the checks of the fixed values (each value a row fixes, a row fixed to both) to `checks_below`.
//!
//! Rows of equal ranking value are ordered as the sort gives them, and the prefix is right only if every window of W1
//! that reads the order (the rank, a running sum) sees the same one. They have one partition and one order, so DuckDB
//! sorts them once (PhysicalPlanGenerator::CreatePlan(LogicalWindow), which puts the windows whose
//! BoundWindowExpression::PartitionsAreEquivalent into one PhysicalWindow).
class DirectRankedPrefix {
public:
	virtual ~DirectRankedPrefix() = default;

	//! The substitute over the one read `rows`: the source columns, then x (INTEGER 0/1), then the column of the
	//! checks when the plan owes any (`validated_columns`).
	DirectSubstitute Build(unique_ptr<LogicalOperator> rows);

protected:
	//! `scope` and `fixed` are the rule's, read at Build: the rows and groups the ranking covers, and the rows its
	//! clauses fix to 1 or to 0, which leave the ranking and keep the value they are fixed to.
	DirectRankedPrefix(Optimizer &optimizer, const vector<ColumnBinding> &source_bindings,
	                   const vector<LogicalType> &source_types, const DirectStateErrors &errors,
	                   const DirectSharedScope &scope, const DirectFixedValues &fixed);

	//! The direction of the ranking: ascending ranks the smallest ranking value first.
	virtual OrderType RankOrder() const = 0;
	//! P1: adds the rule's columns over `state` (its source columns are `state.input_bindings`) and returns the ranking
	//! column, a DOUBLE. The builder ranks a row a clause fixes to 1 or to 0 last (RankKey). It appends the checks it
	//! owes below the rank window to `checks_below`, over `state`'s columns.
	virtual ColumnBinding LayoutRows(DirectProjectionStage &p1, const DirectScopeState &state) = 0;
	//! W1: adds to `w1` the windows of the rule over the group in order of the ranking column (a running sum of it, a
	//! count), over the rank's partition and order (OverRank). `rows` are the columns of P1, RankKey the ranking
	//! column's.
	virtual void AddGroupWindows(LogicalWindow &w1, const DirectRankedColumns &rows) = 0;
	//! P2: adds the rule's columns over `input`, the columns W1 gives (P1's columns and the rank), and the group limits
	//! (GroupValues). It appends the checks it owes above W1 to `checks_above`, over the same columns.
	virtual void LayoutPrefix(DirectProjectionStage &p2, const DirectRankedColumns &input) = 0;
	//! W2: the window that gives each group the size of its prefix, the `size` of DirectRankedColumns; null for a rule
	//! that reads the size off the group's own columns.
	virtual unique_ptr<BoundWindowExpression> PrefixSize(const DirectRankedColumns &rows) {
		return nullptr;
	}
	//! W3: whether the row's group can be met, a BOOLEAN over the columns of P2; null where no group can fail. The
	//! builder takes a row outside every group as met. A false raises the infeasible error before the first row leaves.
	virtual ExprPtr GroupVerdict(const DirectRankedColumns &rows) = 0;
	//! A: whether the row is selected, where no clause fixes it; the builder lets a fixed row keep its value.
	virtual ExprPtr Chosen(const DirectRankedColumns &rows) = 0;

	//! Registers a group bound whose aggregates W1 reads over the group, and returns its index into GroupValues. A rule
	//! registers its bounds when it is built.
	idx_t AddGroupBound(const DecideGroupBound &bound);
	//! What the registered bound gives, over the columns of W1; valid in LayoutPrefix and after.
	const DirectGroupValues &GroupValues(idx_t bound) const;

	//! Makes `window`, an aggregate over the whole group, take the rank's partition and order. DuckDB runs a window
	//! with no order, or no partition, on one thread and sorts it on its own; with the rank's order it shares the
	//! rank's sort and stays parallel, and its frame over the whole partition (ROWS BETWEEN UNBOUNDED PRECEDING AND
	//! UNBOUNDED FOLLOWING) leaves its value the same on every row. The group limits take it, and a rule's own windows
	//! over the group should too (AddGroupWindows).
	void OverRank(const DirectCarried &carried, BoundWindowExpression &window) const;
	//! The ranking column as W1 reads it, an expression over P1's columns: the rule's column, NULL on a row a clause
	//! fixes to 1 or to 0, which ranks last and which a sum skips.
	ExprPtr RankKey() const;
	//! Makes `window` run over the rows of `carried`'s group. A row outside every group has a NULL where the key is
	//! compared with `=`, so it is apart from every group.
	void PartitionByGroup(const DirectCarried &carried, BoundWindowExpression &window) const;
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

	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;
	const DirectSharedScope &scope;
	const DirectFixedValues &fixed;
	//! Whether the rows have a scope: a `by` key, a `when` or a `per` key.
	const bool scoped;
	//! The checks of the rule: `checks_below` read the scope projection's columns and stand in P1; `checks_above` read
	//! P1's or W1's columns and stand in P2.
	vector<unique_ptr<Expression>> checks_below;
	vector<unique_ptr<Expression>> checks_above;

private:
	//! A plan and the columns that stage carries.
	struct Stage {
		unique_ptr<LogicalOperator> plan;
		DirectRankedColumns columns;
	};

	Stage ProjectRows(unique_ptr<LogicalOperator> rows);
	void RankRows(Stage &rows);
	Stage ProjectPrefix(Stage ranked);
	//! Adds W2 and W3 over `prefix`; `feasible` is what W3 gives, null for a plan without one.
	void CloseGroups(Stage &prefix, ExprPtr &feasible);
	DirectSubstitute Answer(Stage prefix, const ExprPtr &feasible);

	vector<const DecideGroupBound *> bounds;
	//! Where P1 laid out what each registered bound reads, and what each gives once W1 has computed it.
	vector<vector<DirectGroupInputColumns>> bound_inputs;
	vector<DirectGroupValues> bound_values;
	//! The rule's ranking column in P1, and the columns of P1 that say a clause fixes the row (set with fixed rows).
	ColumnBinding rank_key;
	ColumnBinding key_fixed_one;
	ColumnBinding key_fixed_zero;
};

} // namespace duckdb
