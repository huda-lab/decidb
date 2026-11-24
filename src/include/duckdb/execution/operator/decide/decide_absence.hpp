//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_absence.hpp
//
// The one "no value" rule of the bottom-up pass (⊥, literal plan §6.3; D67, D68, D84, D87, D88, D90).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/decide/decide_tree_terms.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"

namespace duckdb {

struct DecideBooleanProductDefinition;
struct DecideFrameMap;
struct DecideTree;
struct KeyedMapping;
struct TermFilterState;

//! What has no value, and what follows from it. Absence starts at a read and goes up, never down:
//!
//!   a keyed decision read at a row in no class of its scope       -> no value there (D84)
//!   a helper read at a row whose group holds no member            -> no value there (D68, D87)
//!   an auxiliary whose definition reads a decision with none      -> no value there (D88)
//!   a product where either factor has none                        -> none there
//!   a frame's term at an anchor whose frame selects nothing       -> none there (D84)
//!   a group in which some reducer of the clause reads no row      -> the group's instance is skipped (p. 56)
//!   an instance whose reads or data reach the comparison NULL     -> the instance is skipped (p. 50, D67)
//!
//! and a skipped instance is counted (D90). The bottom-up pass applies the first five where it reads a definition, in
//! the tree's order, so what defines a decision has its absence before what reads it (DecideModelEmitter): an
//! auxiliary's rows with none are `Rows`, which stage 06 reads by instance (SolverInput::absent_instances). A clause's
//! instances are skipped by the last two, where it is read. Data are not strict: DuckDB evaluates them, so
//! `coalesce(hole, 0)` consumes a NULL with no code here.
class DecideAbsence {
public:
	explicit DecideAbsence(const DecideTree &tree);

	//! What the rule reads: the keyed scopes' classes (a helper's from its G's groups), each auxiliary's rows with no
	//! value, complete for one once its definition is read, and the frames' maps once their O nodes are computed.
	void ReadClasses(const vector<DecideScopeRows> &classes, const vector<vector<bool>> &auxiliaries);
	void ReadFrames(const vector<DecideFrameMap> &frames);

	//! Whether decision `variable` has no value at `row`. None for no decision (INVALID_INDEX).
	bool At(idx_t variable, idx_t row) const;
	//! Whether `term` reads, at `row`, no value: its decision's (At), or a frame's selection (DecideFrameReadsAbsent).
	bool TermAt(const DecideTerm &term, idx_t row) const;
	//! Whether At can hold at some row: a keyed decision some row is in no class of (any, until the classes are read),
	//! an auxiliary some row of its definition leaves with none (so far), a product whose factor may have none. A
	//! decision that cannot is read at every row.
	bool MayBe(idx_t variable) const;
	//! Whether TermAt can hold at some row: a term of such a decision, or of a frame.
	bool MayBe(const DecideTerm &term) const;

	//! A reducer over no rows has no value, so in the aggregate shape a group in which some reducer reads no row has
	//! no instance (p. 56). Only a reducer's term carries a filter (its own `when`, its NULL-data and absent-decision
	//! tests, its de-duplication), so a term with none reads every row of its group. Applied before the group's bound
	//! and terms are read, so nothing is read for a group that has no instance.
	static void SkipEmptyReducers(const vector<const TermFilterState *> &filters, idx_t num_rows,
	                              vector<idx_t> &row_group_ids, idx_t &num_groups);
	//! A NULL that reaches a comparison unreduced makes that instance NULL, and a NULL instance is skipped (p. 50): in
	//! the aggregate shape the group the row is in (its bound and its terms beside the reducers are one value per
	//! group), else the row. `row_group_ids` empty is every row in one group.
	static void SkipNullInstances(const vector<bool> &null_rows, bool grouped, idx_t num_rows,
	                              vector<idx_t> &row_group_ids, idx_t &num_groups);

private:
	const DecideTree &tree;
	//! Per decision, the product it is the auxiliary of, or null.
	vector<const DecideBooleanProductDefinition *> products;
	const vector<DecideScopeRows> *classes = nullptr;
	const vector<vector<bool>> *auxiliaries = nullptr;
	const vector<DecideFrameMap> *frames = nullptr;
};

//! The one check of the rule: a term reading a decision with no instance at a row has no value there (D84), whether the
//! term stands in a row, beside a reducer or in a reducer, and whether the decision is a keyed one or a helper no row
//! holds a member of. Built over the terms a clause reads, once, from those that may read none; `At` is then asked per
//! row.
class DecideAbsentReads {
public:
	DecideAbsentReads(const DecideAbsence &absence, const vector<DecideTerm> &terms) : absence(absence) {
		for (auto &term : terms) {
			Add(term);
		}
	}
	explicit DecideAbsentReads(const DecideAbsence &absence) : absence(absence) {
	}
	void Add(const DecideTerm &term) {
		if (absence.MayBe(term)) {
			may_read_none.push_back(&term);
		}
	}
	//! Whether some term may read no value at some row.
	bool Any() const {
		return !may_read_none.empty();
	}
	//! Whether some term reads no value at `row`.
	bool At(idx_t row) const {
		for (auto term : may_read_none) {
			if (absence.TermAt(*term, row)) {
				return true;
			}
		}
		return false;
	}

private:
	const DecideAbsence &absence;
	vector<const DecideTerm *> may_read_none;
};

} // namespace duckdb
