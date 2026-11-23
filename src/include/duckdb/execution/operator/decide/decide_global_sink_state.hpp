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

	vector<EvaluatedConstraint> evaluated_constraints;
	//! The `<>` clauses, evaluated; moved into SolverInput::not_equals.
	vector<NotEqualClause> evaluated_not_equals;
	//! The clauses with terms of degree 2, evaluated; moved into SolverInput::quadratics.
	vector<QuadraticClause> evaluated_quadratics;
	//! The stage-06 records the emitter materializes from BOOLEAN_PRODUCT F nodes for the unchanged linearizer.
	vector<BilinearLinkSpec> evaluated_bilinear_links;
	//! One record per ABS F node, with its envelope rows evaluated; moved into SolverInput::abs_links.
	vector<AbsLinkSpec> evaluated_abs_links;
	//! `singleton_row_constraints`, evaluated under DIAGNOSE for the repair model alone.
	vector<EvaluatedConstraint> evaluated_singleton_row_constraints;
	//! The classes each aggregate-shape row's group stands for (D28), referenced from the
	//! rows; filled under DIAGNOSE only and moved into SolverInput::class_sets.
	vector<SolverInput::ClassSet> class_sets;
	//! The value rewrite's helper decisions, each with its reducer's members evaluated
	//!; moved into SolverInput::reducer_value_links.
	vector<ReducerValueLinkSpec> evaluated_value_links;
	//! Per helper, the instances written where they are read; moved into SolverInput::written_in.
	vector<vector<bool>> written_in;
	//! The objective's linear terms as they are read, a column over the input's rows each, with the decision each
	//! reads: the emitter's working state, which DecideModelEmitter::ObjectiveCoefficients sums and frees.
	vector<CoefficientColumn> evaluated_objective_coefficients; // [term_idx]
	vector<idx_t> objective_variable_indices;
	//! The objective's linear part per instance and its constant; moved into SolverInput::objective_coefficients and
	//! ::objective_constant.
	vector<vector<double>> objective_coefficients;
	double objective_constant = 0.0;

	//! The objective's product groups, evaluated; moved into SolverInput::objective_product_groups.
	vector<EvaluatedProductGroup> evaluated_objective_groups;

	vector<double> ilp_solution;
	VarIndexer var_indexer; // For mapping (var_idx, row) to solution indices

	//! The frame selectors' timelines and selections, one per
	//! frame, in LogicalDecide::frames order.
	vector<DecideFrameMap> frame_maps;
	//! The decisions a frame reads at another row (SolverInput::frame_reads): each pair of
	//! a decision and a frame once, the variable after the plan's decisions.
	vector<std::pair<idx_t, idx_t>> frame_reads;
	//! Per decision, the rows whose instance of it has no value beyond its keyed scope's absent class: an auxiliary
	//! whose definition reads an instance with none there (D88); empty where none.
	vector<vector<bool>> absent_reads;
	//! Per decision, the instances every row reads with no value; moved into SolverInput::absent_instances.
	vector<vector<bool>> absent_instances;
	//! The frame reads, each with its base decision's instance per row; moved into SolverInput::frame_reads.
	vector<SolverInput::FrameRead> evaluated_frame_reads;
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

	//! The variable a term reads at each row: its own, or -- for a frame's payload term --
	//! the read of it at the row the frame selects. A query-wide decision has one column,
	//! which every row reads, so it is its own read.
	idx_t ReadVariable(const DecideTerm &term) {
		return ReadVariable(term.variable_index, term.frame_read);
	}
	idx_t ReadVariable(idx_t variable, idx_t frame_read) {
		if (frame_read == DConstants::INVALID_INDEX) {
			return variable;
		}
		if (variable < op.tree->variable_scopes.size() && op.tree->variable_scopes[variable].IsGlobal()) {
			return variable;
		}
		auto read = std::make_pair(variable, frame_read);
		auto it = std::find(frame_reads.begin(), frame_reads.end(), read);
		if (it == frame_reads.end()) {
			frame_reads.push_back(read);
			it = frame_reads.end() - 1;
		}
		return op.tree->decide_variables.size() + static_cast<idx_t>(it - frame_reads.begin());
	}

	//! Set when the solve failed under DIAGNOSE: the findings were handed off instead of
	//! raised, so there is no solution to read back and this operator emits no rows. The
	//! DIAGNOSE operator above turns the handoff into the answer.
	bool diagnosis_only = false;
};

} // namespace duckdb
