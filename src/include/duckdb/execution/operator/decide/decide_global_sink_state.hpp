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
#include "duckdb/planner/decide/decide_canonicalizer.hpp"

#include <map>

namespace duckdb {

//! Singleton-row folding is decided at layer 05 (DecideOptimizer::FoldSingletonRows).
//! `UserBoundSpec` lives on LogicalDecide with it; execution reads the resulting box
//! rather than deriving one.
using UserBoundSpec = LogicalDecide::UserBoundSpec;

class DecideGlobalSinkState : public GlobalSinkState {
public:
	explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
	    : data(context, op.children[0]->GetTypes()), context(context), op(op),
	      canonicalizer(context, op.decide_index, op.variable_scopes, op.keyed_scopes, op.keys, op.is_boolean_var) {
        // The decision column box was resolved by DecideOptimizer::FoldSingletonRows.
        // A variable whose box never had a lower bound written is one the query never
        // lowered, and Finalize resolves it to the default 0 floor.
        idx_t num_decide_vars = op.decide_variables.size();
        singleton_row_boxes = op.singleton_row_boxes;
        user_singleton_row_bounds = op.user_singleton_row_bounds;

        // A user bound that contradicts the variable's INTRINSIC domain (a non-negative
        // REAL/INTEGER's `>= 0` floor, or a BOOLEAN's `0/1` box) is a deterministic semantic
        // error, not a constraint conflict to diagnose — loosening can't help, the type can.
        // It is raised here (the main pipeline), before any row is read, so it never reaches the
        // elastic engine and supersedes the generic infeasible error.
        //   - `x <= -1` (REAL): upper below the non-negativity floor → infeasible. But
        //     `x = -1` or `x <= -1 AND x >= -5` explicitly lower the floor below 0, so the
        //     guard is `U < 0 AND L >= 0` (floor still at/above the intrinsic 0).
        //   - `x >= 2` / `x <= -1` (BOOLEAN): outside the 0/1 box. This is Boolean arithmetic, not
        //     a wider variable, so the box stays [0,1] and the query is infeasible; the error quotes
        //     the bound.
        // Both are raised under DIAGNOSE too. A purely user-vs-user inverted box (e.g. `x >= 5 AND
        // x <= 1`, both >= 0) does NOT match and proceeds to the elastic engine, which reports a
        // least-change loosen.
        // A helper decision of the value rewrite has no intrinsic domain: it holds a
        // reducer's value, `sum(w * x)` negative wherever `w` is, so no bound on it
        // contradicts a type (its floor is free, DecideOptimizer::FoldSingletonRows).
        vector<bool> is_helper(num_decide_vars, false);
        for (auto &link : op.reducer_value_links) {
            if (link.value_var < num_decide_vars) {
                is_helper[link.value_var] = true;
            }
        }
		// An auxiliary whose definition may read no value -- a data value (NULL), a frame, a keyed decision (a row in
		// no class), or another such auxiliary -- has none on those rows, where no bound on it binds (D88): the data
		// says whether one row is left that the bound contradicts, so the solve reports it.
		std::function<bool(idx_t, idx_t)> may_lack_value = [&](idx_t var, idx_t depth) {
			if (depth > num_decide_vars) {
				return true;
			}
			auto reads_none = [&](const DecideTerm &term) {
				if (term.coefficient && !term.coefficient->IsFoldable()) {
					return true;
				}
				auto read = term.variable_index;
				if (read == DConstants::INVALID_INDEX || read == var) {
					return false;
				}
				return term.frame_read != DConstants::INVALID_INDEX ||
				       (read < op.variable_scopes.size() && op.variable_scopes[read].IsKeyed()) ||
				       may_lack_value(read, depth + 1);
			};
			for (const auto &constraint : op.prepared.constraints) {
				if (constraint->defined_var != var) {
					continue;
				}
				if (constraint->rhs_expr && !constraint->rhs_expr->IsFoldable()) {
					return true;
				}
				for (auto &term : constraint->lhs_terms) {
					if (reads_none(term)) {
						return true;
					}
				}
				for (auto &group : constraint->quadratic_groups) {
					for (auto &term : group.inner_terms) {
						if (reads_none(term)) {
							return true;
						}
					}
				}
			}
			return false;
		};
        for (idx_t var = 0; var < num_decide_vars; var++) {
            if (is_helper[var] || may_lack_value(var, 0)) {
                continue;
            }
            bool is_bool = var < op.is_boolean_var.size() && op.is_boolean_var[var];
            // The box of the bounds as written. The folded box holds the admitted ends instead
            // (`x = -0.5` admits no INT, which the solve reports as infeasible), so it is not the
            // box the user wrote.
            DecideWrittenBox written;
            for (const auto &bound : op.member_singleton_row_bounds) {
                if (bound.decide_var_idx == var) {
                    written.Add(bound.sense, bound.k);
                }
            }
            for (const auto &bound : user_singleton_row_bounds) {
                if (bound.decide_var_idx != var) {
                    continue;
                }
                if (bound.removal_group_id == DConstants::INVALID_INDEX) {
                    written.Add(bound.sense, bound.k);
                } else {
                    written.AddLowered(bound.sense, bound.k);
                }
            }
            // A floor under a `when` replaces the default on the rows it keeps (AddLowered counts it only when
            // negative). The solve says whether the clauses on the other rows leave a value.
            for (const auto &constraint : op.prepared.constraints) {
                if (constraint->lifted_floor == var) {
                    written.AddLowered('>', constraint->lifted_floor_k);
                }
            }
            if (!written.ContradictsDomain(is_bool)) {
                continue;
            }
            const auto &box = written.Box();
			// An auxiliary decision (an ABS, a norm) is named by what it stands for.
			const bool is_aux = var >= num_decide_vars - op.num_auxiliary_vars;
			string vname = op.decide_variables[var]->GetName();
			for (auto &label : op.aux_var_expressions) {
				if (is_aux && label.first == var) {
					vname = label.second;
				}
			}
			// The clause that set the bound `k`, as written (`max(x) <= -5` as much as `x <= -5`, a strict
			// `x < 0` as typed rather than the folded `x <= -1`).
			auto written_bound = [&](bool upper, double k) {
				const vector<UserBoundSpec> *lists[] = {&user_singleton_row_bounds, &op.member_singleton_row_bounds};
				for (auto *bounds : lists) {
					for (const auto &bound : *bounds) {
						if (bound.decide_var_idx == var && bound.sense != (upper ? '>' : '<') && bound.k == k &&
						    bound.source_clause_id < op.constraint_sources.size()) {
							return QuoteWrittenClause(op.constraint_sources[bound.source_clause_id]);
						}
					}
				}
				return StringUtil::Format("%s %s %g", vname, upper ? "<=" : ">=", k);
			};
			if (is_bool) {
				// Both sides can contradict at once (`x <= -1 AND x >= 2`); the upper bound is reported.
				bool upper = box.upper < 0.0;
				throw InvalidInputException(
				    StringUtil::Format("DECIDE optimization is infeasible: %s cannot hold for a BOOL variable.",
				                       written_bound(upper, upper ? box.upper : box.lower)));
			}
			if (is_aux) {
				throw InvalidInputException(StringUtil::Format(
				    "DECIDE optimization is infeasible: %s cannot hold because %s is never negative.",
				    written_bound(true, box.upper), vname));
			}
			throw InvalidInputException(StringUtil::Format(
			    "DECIDE optimization is infeasible: %s cannot hold because %s is non-negative (>= 0). "
			    "Add an explicit lower bound if it may be negative.",
			    written_bound(true, box.upper), vname));
		}

		// Minimal: keep constructor lean; detailed solver output comes from HiGHS
	}

    mutex lock;
	//! The tree's descent, the threads' merged (language/guide.md §6.1): the rows are read once, as they arrive, and
	//! the buffer below is never scanned to evaluate them.
	unique_ptr<DecideDescent> descent;
	idx_t descent_rows = 0;
    // This collection will hold all the data from the child operator, each thread's rows in the order its descent read
    // them, placed as the descents merge
    ColumnDataCollection data;

    //! Needed to evaluate coefficient expressions against the materialized rows.
    ClientContext &context;

    const PhysicalDecide &op;
    DecideCanonicalizer canonicalizer;

    //! The flattened constraints and objective, decided by BuildDecidePreparedModel
    //! (stage 05). Aliases rather than copies: this state evaluates their
    //! coefficients against the data, it does not derive or alter their shape.
    const vector<unique_ptr<DecideConstraint>> &constraints = op.prepared.constraints;
    const vector<unique_ptr<DecideConstraint>> &singleton_row_constraints = op.prepared.singleton_row_constraints;
    const unique_ptr<DecideObjective> &objective = op.prepared.objective;

    //! Local copies of the decision column box and the singleton-row-bound records that
    //! DecideOptimizer::FoldSingletonRows resolved. Finalize copies the box into
    //! solver_input; the records quote a bound that contradicts its domain and re-open its
    //! direction of the rigid box, while its clause reaches the repair model through
    //! `singleton_row_constraints`.
    vector<DecideBoundBox> singleton_row_boxes;
    vector<UserBoundSpec> user_singleton_row_bounds;

    //===--------------------------------------------------------------------===//
    // Evaluated Coefficients (Phase 2)
    //===--------------------------------------------------------------------===//

    vector<EvaluatedConstraint> evaluated_constraints;
    //! `singleton_row_constraints`, evaluated under DIAGNOSE for the repair model alone.
    vector<EvaluatedConstraint> evaluated_singleton_row_constraints;
    //! The classes each aggregate-shape row's group stands for (D28), referenced from the
    //! rows; filled under DIAGNOSE only and moved into SolverInput::class_sets.
    vector<SolverInput::ClassSet> class_sets;
    //! The value rewrite's helper decisions, each with its reducer's members evaluated
    //!; moved into SolverInput::reducer_value_links.
    vector<ReducerValueLinkSpec> evaluated_value_links;
    vector<CoefficientColumn> evaluated_objective_coefficients; // [term_idx]
    vector<idx_t> objective_variable_indices;

    // Quadratic objective: evaluated inner linear expression coefficients
    vector<CoefficientColumn> evaluated_quadratic_coefficients;  // [term_idx]
    vector<idx_t> quadratic_variable_indices;
    bool has_quadratic_objective = false;
    double quadratic_sign = 1.0;

    // Bilinear objective: pairs of different decide variables with data coefficients.
    // Reuses EvaluatedConstraint::BilinearTerm (solver_input.hpp) rather than declaring
    // a field-identical duplicate — the two are populated by the same shared evaluator.
    vector<EvaluatedConstraint::BilinearTerm> evaluated_bilinear_terms;

    vector<double> ilp_solution;
    VarIndexer var_indexer;  // For mapping (var_idx, row) to solution indices

	//! The frame selectors' timelines and selections, one per
	//! frame, in LogicalDecide::frames order.
	vector<DecideFrameMap> frame_maps;
	//! The decisions a frame reads at another row (SolverInput::frame_reads): each pair of
	//! a decision and a frame once, the variable after the plan's decisions.
	vector<std::pair<idx_t, idx_t>> frame_reads;
	//! A reference to each column the frames computed (DecidePreparedModel::frame_columns), held for the
	//! query so the chunk expression cache, keyed by node, never meets a freed one.
	vector<unique_ptr<Expression>> frame_column_refs;
	//! Per decision, the rows whose instance of it has no value beyond its keyed scope's absent class: an auxiliary
	//! whose definition reads an instance with none there (D88); empty where none.
	vector<vector<bool>> absent_reads;
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
		if (term.frame_read == DConstants::INVALID_INDEX) {
			return term.variable_index;
		}
		auto var = term.variable_index;
		if (var < op.variable_scopes.size() && op.variable_scopes[var].IsGlobal()) {
			return var;
		}
		auto read = std::make_pair(var, term.frame_read);
		auto it = std::find(frame_reads.begin(), frame_reads.end(), read);
		if (it == frame_reads.end()) {
			frame_reads.push_back(read);
			it = frame_reads.end() - 1;
		}
		return op.decide_variables.size() + static_cast<idx_t>(it - frame_reads.begin());
	}

	//! Set when the solve failed under DIAGNOSE: the findings were handed off instead of
    //! raised, so there is no solution to read back and this operator emits no rows. The
    //! DIAGNOSE operator above turns the handoff into the answer.
    bool diagnosis_only = false;
};

} // namespace duckdb
