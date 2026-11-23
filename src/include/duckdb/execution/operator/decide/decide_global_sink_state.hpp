//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_global_sink_state.hpp
//
// What PhysicalDecide's sink collects and its evaluation leaves for the model: every evaluator of a DECIDE query fills
// it (physical_decide.cpp, DecideModelEmitter).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/decide/decide_descent.hpp"
#include "duckdb/execution/operator/decide/decide_frame_map.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"

#include <map>

namespace duckdb {

class DecideGlobalSinkState : public GlobalSinkState {
public:
	explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
	    : data(context, op.children[0]->GetTypes()), op(op) {
	}

	mutex lock;
	//! The tree's descent (language/guide.md §6.1): Finalize routes the buffer below through it once, then releases it
	//! once the evaluation has returned.
	unique_ptr<DecideDescent> descent;
	//! Every row the child operator produced, each thread's rows in the order it received them, placed one thread after
	//! another as the threads combine; the descent numbers a row by its place here.
	ColumnDataCollection data;

	const PhysicalDecide &op;

	//===--------------------------------------------------------------------===//
	// Evaluated Coefficients (Phase 2)
	//===--------------------------------------------------------------------===//

	//! `singleton_row_constraints`, evaluated under DIAGNOSE for the repair model alone.
	vector<EvaluatedConstraint> evaluated_singleton_row_constraints;

	vector<double> ilp_solution;
	VarIndexer var_indexer; // For mapping (var_idx, row) to solution indices

	//! The frame selectors' timelines and selections, one per
	//! frame, in LogicalDecide::frames order.
	vector<DecideFrameMap> frame_maps;
	//! Per clause (its source clause id), the instances its rows hold once grouped, and those left once each with no
	//! value is skipped (deck pp. 50, 56): the most any of its rows holds, as a rewrite states the clause's instances
	//! in each of its rows (PhysicalDecide::ReportSkippedInstances).
	struct ClauseInstances {
		idx_t grouped = 0;
		idx_t kept = 0;
	};
	map<idx_t, ClauseInstances> clause_instances;
	void CountInstances(idx_t source_clause_id, idx_t grouped, idx_t kept) {
		if (source_clause_id == DConstants::INVALID_INDEX) {
			return;
		}
		auto &counts = clause_instances[source_clause_id];
		counts.grouped = MaxValue(counts.grouped, grouped);
		counts.kept = MaxValue(counts.kept, kept);
	}

	//! Set when the solve failed under DIAGNOSE: the findings were handed off instead of
	//! raised, so there is no solution to read back and this operator emits no rows. The
	//! DIAGNOSE operator above turns the handoff into the answer.
	bool diagnosis_only = false;
};

} // namespace duckdb
