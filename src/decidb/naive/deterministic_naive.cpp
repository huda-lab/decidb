#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/solver_config.hpp"
#include "duckdb/decidb/solver_session.hpp"
#include "duckdb/decidb/diagnostic_constants.hpp"
#include "duckdb/common/exception.hpp"
#include "Highs.h"

#include <cmath>

namespace duckdb {

namespace {

//! Resumable HiGHS handle. Load() builds the model into the `highs` object once
//! (a member, so it survives across Continue()); RunAndReadback() sets the per-chunk
//! time_limit and (re-)runs. HiGHS resumes its MIP search on a repeat run() after a
//! time-limit stop, so Continue() is just another RunAndReadback() with a fresh chunk.
class HighsSession : public SolverSession {
public:
    SolverResult Solve(const SolverModel &model, double time_limit_seconds) override {
        Load(model);
        return RunAndReadback(time_limit_seconds);
    }

    SolverResult Continue(double time_limit_seconds) override {
        // Precondition: Solve() already loaded the model into `highs`.
        return RunAndReadback(time_limit_seconds);
    }

private:
    Highs highs;
    idx_t total_vars = 0;
    //! Whether the loaded model has any integer variable. HiGHS only populates
    //! `mip_dual_bound` / `mip_gap` for MIP solves; on an LP/QP timeout they hold
    //! defaults (0 / nan) that must not be read back as a proven bound.
    bool is_mip = false;

    void Load(const SolverModel &model);
    SolverResult RunAndReadback(double time_limit_seconds);
};

void HighsSession::Load(const SolverModel &model) {
    total_vars = model.num_vars;

    //===--------------------------------------------------------------------===//
    // 1. Create HiGHS model and set up variables
    //===--------------------------------------------------------------------===//

    highs.setOptionValue("log_to_console", false);
    // Note: the time_limit is NOT set here — it is a per-chunk budget applied in
    // RunAndReadback(), so a warm Continue() can extend it without a reload.

    vector<HighsVarType> var_types(total_vars);
    is_mip = false;
    for (idx_t i = 0; i < total_vars; i++) {
        var_types[i] = model.is_integer[i] ? HighsVarType::kInteger : HighsVarType::kContinuous;
        is_mip = is_mip || model.is_integer[i];
    }

    ObjSense sense = model.maximize ? ObjSense::kMaximize : ObjSense::kMinimize;

    //===--------------------------------------------------------------------===//
    // 2. Convert SolverModel constraints to HiGHS range format + COO matrix
    //===--------------------------------------------------------------------===//

    vector<int> a_rows;
    vector<int> a_cols;
    vector<double> a_vals;
    vector<double> row_lower;
    vector<double> row_upper;

    // Precompute total nnz to avoid repeated vector reallocation
    idx_t total_nnz = 0;
    for (auto &constr : model.constraints) {
        total_nnz += constr.indices.size();
    }
    a_rows.reserve(total_nnz);
    a_cols.reserve(total_nnz);
    a_vals.reserve(total_nnz);
    row_lower.reserve(model.constraints.size());
    row_upper.reserve(model.constraints.size());

    idx_t constraint_idx = 0;
    for (auto &constr : model.constraints) {
        for (idx_t j = 0; j < constr.indices.size(); j++) {
            a_rows.push_back(static_cast<int>(constraint_idx));
            a_cols.push_back(constr.indices[j]);
            a_vals.push_back(constr.coefficients[j]);
        }

        if (constr.sense == '>') {
            row_lower.push_back(constr.rhs);
            row_upper.push_back(1e30);
        } else if (constr.sense == '<') {
            row_lower.push_back(-1e30);
            row_upper.push_back(constr.rhs);
        } else {
            row_lower.push_back(constr.rhs);
            row_upper.push_back(constr.rhs);
        }

        constraint_idx++;
    }

    idx_t num_constraints = static_cast<idx_t>(row_lower.size());

    //===--------------------------------------------------------------------===//
    // 3. Build HighsLp and convert COO to CSR
    //===--------------------------------------------------------------------===//

    HighsLp lp;
    lp.num_col_ = total_vars;
    lp.num_row_ = num_constraints;
    lp.sense_ = sense;
    lp.offset_ = 0.0;
    lp.col_cost_ = model.obj_coeffs;
    lp.col_lower_ = model.col_lower;
    lp.col_upper_ = model.col_upper;
    lp.row_lower_ = row_lower;
    lp.row_upper_ = row_upper;

    lp.a_matrix_.format_ = MatrixFormat::kRowwise;
    vector<HighsInt> row_starts(num_constraints + 1, 0);

    for (idx_t i = 0; i < a_rows.size(); i++) {
        row_starts[a_rows[i] + 1]++;
    }
    for (idx_t i = 0; i < num_constraints; i++) {
        row_starts[i + 1] += row_starts[i];
    }

    vector<HighsInt> col_indices(a_vals.size());
    vector<double> values(a_vals.size());
    vector<HighsInt> current_pos = row_starts;

    for (idx_t i = 0; i < a_rows.size(); i++) {
        idx_t row = a_rows[i];
        idx_t pos = current_pos[row];
        col_indices[pos] = a_cols[i];
        values[pos] = a_vals[i];
        current_pos[row]++;
    }

    lp.a_matrix_.start_ = row_starts;
    lp.a_matrix_.index_ = col_indices;
    lp.a_matrix_.value_ = values;

    lp.integrality_.resize(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        lp.integrality_[i] = var_types[i];
    }

    HighsStatus status = highs.passModel(lp);
    if (status != HighsStatus::kOk) {
        throw InternalException("Failed to pass model to HiGHS: status %d", (int)status);
    }

    //===--------------------------------------------------------------------===//
    // 3b. Add quadratic objective (Hessian) if present
    //===--------------------------------------------------------------------===//

    // HiGHS does not support quadratic constraints
    if (!model.quadratic_constraints.empty()) {
        throw InvalidInputException(
            "Quadratic/bilinear constraints require Gurobi. "
            "HiGHS does not support quadratic constraints (QCQP). "
            "Either install Gurobi, or linearize the constraints.");
    }

    if (model.has_quadratic_obj && !model.q_vals.empty()) {
        // HiGHS does not support non-convex QP
        if (model.nonconvex_quadratic) {
            throw InvalidInputException(
                "Non-convex quadratic objectives require Gurobi. "
                "HiGHS only supports convex quadratic programs "
                "(MINIMIZE with positive-semidefinite Q, or MAXIMIZE with negative-semidefinite Q). "
                "Either install Gurobi, or reformulate the objective.");
        }
        // HiGHS does not support MIQP — reject if any variable is integer
        for (idx_t i = 0; i < total_vars; i++) {
            if (model.is_integer[i]) {
                throw InvalidInputException(
                    "Quadratic objectives with integer/boolean variables (MIQP) require Gurobi. "
                    "HiGHS only supports continuous quadratic programs (QP). "
                    "Either install Gurobi, or change all DECIDE variables to IS REAL.");
            }
        }

        // Convert COO lower-triangle Q to CSC format for HiGHS passHessian.
        // HiGHS expects the lower triangle in column-major compressed sparse column format.
        idx_t num_nz = model.q_vals.size();

        // Count entries per column
        vector<HighsInt> col_count(total_vars, 0);
        for (idx_t k = 0; k < num_nz; k++) {
            col_count[model.q_cols[k]]++;
        }

        // Build column start array
        vector<HighsInt> q_start(total_vars + 1, 0);
        for (idx_t c = 0; c < total_vars; c++) {
            q_start[c + 1] = q_start[c] + col_count[c];
        }

        // Fill CSC arrays
        vector<HighsInt> q_index(num_nz);
        vector<double> q_value(num_nz);
        vector<HighsInt> current_pos(q_start.begin(), q_start.begin() + total_vars);

        for (idx_t k = 0; k < num_nz; k++) {
            int col = model.q_cols[k];
            HighsInt pos = current_pos[col];
            q_index[pos] = model.q_rows[k];
            q_value[pos] = model.q_vals[k];
            current_pos[col]++;
        }

        status = highs.passHessian((HighsInt)total_vars, (HighsInt)num_nz,
                                   (HighsInt)HessianFormat::kTriangular,
                                   q_start.data(), q_index.data(), q_value.data());
        if (status != HighsStatus::kOk) {
            throw InternalException("Failed to pass quadratic objective (Hessian) to HiGHS: status %d",
                                    (int)status);
        }
    }
}

SolverResult HighsSession::RunAndReadback(double time_limit_seconds) {
    //===--------------------------------------------------------------------===//
    // 4. Solve
    //===--------------------------------------------------------------------===//

    // Apply the per-chunk wall-clock cap. Setting it here (rather than at Load) is
    // what lets a warm Continue() extend the budget without rebuilding: HiGHS
    // resumes its MIP search on a repeat run() after a time-limit stop.
    highs.setOptionValue("time_limit", time_limit_seconds);

    HighsStatus status = highs.run();
    // HiGHS returns kWarning (not kOk) at the time limit while still exposing a
    // valid model status + incumbent, so throwing on any non-kOk return would
    // crash a diagnosable timeout with INTERNAL. Throw only on a genuine kError;
    // let kWarning fall through to the model-status switch below.
    if (status == HighsStatus::kError) {
        HighsModelStatus model_status = highs.getModelStatus();
        throw InternalException("HiGHS solver failed: status %d, model_status %d", (int)status, (int)model_status);
    }

    //===--------------------------------------------------------------------===//
    // 5. Check status
    //===--------------------------------------------------------------------===//

    HighsModelStatus model_status = highs.getModelStatus();
    if (model_status != HighsModelStatus::kOptimal) {
        // Map the raw model status to a normalized SolverStatus and return it
        // (no solution). The operator surfaces the default error (manual-first)
        // or routes it to diagnosis when a diagnose pragma is active.
        SolverResult result;
        result.raw_status = (int)model_status;
        switch (model_status) {
        case HighsModelStatus::kInfeasible:
            result.status = SolverStatus::INFEASIBLE;
            break;
        case HighsModelStatus::kUnbounded:
            result.status = SolverStatus::UNBOUNDED;
            break;
        case HighsModelStatus::kUnboundedOrInfeasible:
            // HiGHS returns this on MILP-unbounded models; without an explicit
            // branch it would fall into OTHER (the old generic catch-all). U1
            // (obj=0 probe) later disambiguates it to UNBOUNDED / INFEASIBLE.
            result.status = SolverStatus::INF_OR_UNBD;
            break;
        case HighsModelStatus::kTimeLimit: {
            result.status = SolverStatus::TIME_LIMIT;
            const HighsInfo& info = highs.getInfo();
            // A proven bound only exists for MIP timeouts: on an LP/QP timeout
            // `mip_dual_bound` still holds its 0 default, which would read back
            // as a confident (and wrong) "best possible objective". Keep the NaN
            // "unavailable" default there and for non-finite values.
            if (is_mip && std::isfinite(info.mip_dual_bound) &&
                std::fabs(info.mip_dual_bound) < EFFECTIVE_INFINITY) {
                result.best_bound = info.mip_dual_bound;
            }
            // Incumbent reads (objective / gap / solution vector) are only valid
            // when HiGHS found a feasible solution; otherwise objective_function_value
            // is inf, mip_gap is nan, and col_value is garbage — so gate on the
            // primal-solution status. `mip_gap` additionally needs a MIP bound to
            // be meaningful (an LP feasible-at-timeout would read back nan).
            if (info.primal_solution_status == kSolutionStatusFeasible) {
                result.has_solution = true;
                result.objective_value = info.objective_function_value;
                if (is_mip && std::isfinite(info.mip_gap) && info.mip_gap < EFFECTIVE_INFINITY) {
                    result.gap = info.mip_gap;
                }
                const HighsSolution& incumbent = highs.getSolution();
                if (incumbent.col_value.size() >= total_vars) {
                    result.solution.assign(incumbent.col_value.begin(),
                                           incumbent.col_value.begin() + total_vars);
                }
            }
            break;
        }
        case HighsModelStatus::kIterationLimit:
            result.status = SolverStatus::ITERATION_LIMIT;
            break;
        default:
            result.status = SolverStatus::OTHER;
            break;
        }
        return result;
    }

    //===--------------------------------------------------------------------===//
    // 6. Extract solution
    //===--------------------------------------------------------------------===//

    const HighsSolution& solution = highs.getSolution();

    if (solution.col_value.size() < total_vars) {
        throw InternalException(
            "HiGHS returned incomplete solution: expected %llu variables, got %llu",
            total_vars, (idx_t)solution.col_value.size());
    }

    vector<double> result(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        double val = solution.col_value[i];
        if (!std::isfinite(val)) {
            throw InternalException(
                "HiGHS returned invalid solution value (NaN or Infinity) for variable %llu", i);
        }
        result[i] = val;
    }

    SolverResult solve_result;
    solve_result.status = SolverStatus::OPTIMAL;
    solve_result.solution = std::move(result);
    // Objective value at the optimum, in the model's own sense (the diagnostics
    // stage-2 re-solve reads this as the achievable objective).
    solve_result.objective_value = highs.getInfo().objective_function_value;
    return solve_result;
}

} // namespace

SolverResult DeterministicNaive::Solve(const SolverModel &model) {
    // Single-shot path: one Solve() on a throwaway session with the default limit.
    HighsSession session;
    return session.Solve(model, ResolveDecideTimeLimit());
}

unique_ptr<SolverSession> DeterministicNaive::CreateSession() {
    return make_uniq<HighsSession>();
}

} // namespace duckdb
