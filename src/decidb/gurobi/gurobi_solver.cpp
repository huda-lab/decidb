#include "duckdb/decidb/gurobi/gurobi_solver.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/decidb/gurobi/gurobi_loader.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

namespace duckdb {

//! RAII wrapper for Gurobi C resources (uses function pointers from loader)
struct GurobiGuard {
    void *model = nullptr;
    void *env = nullptr;
    ~GurobiGuard() {
        auto &api = GurobiLoader::API();
        if (model) {
            api.freemodel(model);
        }
        if (env) {
            api.freeenv(env);
        }
    }
};

bool GurobiSolver::IsAvailable() {
    // Result is cached for the process lifetime. A Gurobi license that expires
    // mid-session will not be detected until the next fresh process start.
    static bool available = []() {
        if (!GurobiLoader::Load()) {
            return false;
        }
        // Trial: can we actually create and start an environment? (license check)
        auto &api = GurobiLoader::API();
        void *env = nullptr;
        bool ok = false;
        if (api.emptyenv_internal(&env, api.version_major, api.version_minor, api.version_tech) == 0 && env) {
            api.setintparam(env, "OutputFlag", 0);
            ok = (api.startenv(env) == 0);
        }
        if (env) {
            api.freeenv(env);
        }
        return ok;
    }();
    return available;
}

SolverResult GurobiSolver::Solve(const SolverModel &ilp) {
    auto &api = GurobiLoader::API();
    idx_t total_vars = ilp.num_vars;

    //===--------------------------------------------------------------------===//
    // 1. Create Gurobi environment
    //===--------------------------------------------------------------------===//

    GurobiGuard guard;
    int error = api.emptyenv_internal(&guard.env, api.version_major, api.version_minor, api.version_tech);
    if (error || !guard.env) {
        throw InternalException("Failed to create Gurobi environment (error %d). "
                                "Check that GUROBI_HOME is set and license is valid.",
                                error);
    }
    api.setintparam(guard.env, "OutputFlag", 0);
    // Cap solve time so a hard MIQP/QCQP doesn't hang the session indefinitely.
    // 300s is generous for typical workloads; overridable via DECIDB_TIME_LIMIT
    // (seconds, double). Truly hard problems return the best feasible solution
    // found so far (handled by the GRB_TIME_LIMIT branch below).
    double time_limit = 300.0;
    if (const char *env_limit = std::getenv("DECIDB_TIME_LIMIT")) {
        try {
            double parsed = std::stod(env_limit);
            if (parsed > 0.0) {
                time_limit = parsed;
            }
        } catch (...) {
            // Ignore unparseable values; keep the default.
        }
    }
    api.setdblparam(guard.env, "TimeLimit", time_limit);
    // Enable non-convex QP solving via spatial branching when needed
    if (ilp.nonconvex_quadratic) {
        api.setintparam(guard.env, "NonConvex", 2);
    }
    error = api.startenv(guard.env);
    if (error) {
        throw InternalException("Failed to start Gurobi environment (error %d). "
                                "Check that GUROBI_HOME is set and license is valid.",
                                error);
    }

    //===--------------------------------------------------------------------===//
    // 2. Create model with variables
    //===--------------------------------------------------------------------===//

    vector<char> var_types(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        if (ilp.is_binary[i]) {
            var_types[i] = GRB_BINARY;
        } else if (ilp.is_integer[i]) {
            var_types[i] = GRB_INTEGER;
        } else {
            var_types[i] = GRB_CONTINUOUS;
        }
    }

    error = api.newmodel(guard.env, &guard.model, "decidb_decide",
                         (int)total_vars,
                         const_cast<double *>(ilp.obj_coeffs.data()),
                         const_cast<double *>(ilp.col_lower.data()),
                         const_cast<double *>(ilp.col_upper.data()),
                         var_types.data(), nullptr);
    if (error) {
        throw InternalException("Failed to create Gurobi model: %s",
                                api.geterrormsg(guard.env));
    }

    int grb_sense = ilp.maximize ? GRB_MAXIMIZE : GRB_MINIMIZE;
    error = api.setintattr(guard.model, GRB_INT_ATTR_MODELSENSE, grb_sense);
    if (error) {
        throw InternalException("Failed to set Gurobi model sense: %s",
                                api.geterrormsg(guard.env));
    }

    //===--------------------------------------------------------------------===//
    // 3. Add constraints
    //===--------------------------------------------------------------------===//

    for (auto &constr : ilp.constraints) {
        error = api.addconstr(guard.model, (int)constr.indices.size(),
                             const_cast<int *>(constr.indices.data()),
                             const_cast<double *>(constr.coefficients.data()),
                             constr.sense, constr.rhs, nullptr);
        if (error) {
            // Use InvalidInputException so a malformed constraint rejects this query
            // without invalidating the DuckDB session for subsequent queries.
            throw InvalidInputException("Failed to add constraint to Gurobi: %s",
                                        api.geterrormsg(guard.env));
        }
    }

    // 3b. Add quadratic constraints (QCQP)
    for (auto &qc : ilp.quadratic_constraints) {
        if (!api.addqconstr) {
            throw InvalidInputException(
                "Quadratic constraints require Gurobi with GRBaddqconstr support. "
                "Your Gurobi version does not support this function.");
        }
        error = api.addqconstr(guard.model,
                               (int)qc.linear_indices.size(),
                               const_cast<int *>(qc.linear_indices.data()),
                               const_cast<double *>(qc.linear_coefficients.data()),
                               (int)qc.q_rows.size(),
                               const_cast<int *>(qc.q_rows.data()),
                               const_cast<int *>(qc.q_cols.data()),
                               const_cast<double *>(qc.q_coefficients.data()),
                               qc.sense, qc.rhs, nullptr);
        if (error) {
            throw InvalidInputException("Failed to add quadratic constraint to Gurobi: %s",
                                        api.geterrormsg(guard.env));
        }
    }

    //===--------------------------------------------------------------------===//
    // 3c. Add quadratic objective terms (QP/MIQP)
    //===--------------------------------------------------------------------===//

    if (ilp.has_quadratic_obj && !ilp.q_vals.empty()) {
        error = api.addqpterms(guard.model,
                               (int)ilp.q_vals.size(),
                               const_cast<int *>(ilp.q_rows.data()),
                               const_cast<int *>(ilp.q_cols.data()),
                               const_cast<double *>(ilp.q_vals.data()));
        if (error) {
            throw InvalidInputException("Failed to add quadratic objective terms to Gurobi: %s",
                                        api.geterrormsg(guard.env));
        }
    }

    //===--------------------------------------------------------------------===//
    // 4. Solve
    //===--------------------------------------------------------------------===//

    error = api.optimize(guard.model);
    if (error) {
        throw InvalidInputException("Gurobi optimization call failed: %s",
                                    api.geterrormsg(guard.env));
    }

    //===--------------------------------------------------------------------===//
    // 5. Check status
    //===--------------------------------------------------------------------===//

    int status;
    error = api.getintattr(guard.model, GRB_INT_ATTR_STATUS, &status);
    if (error) {
        throw InternalException("Failed to get Gurobi status: %s",
                                api.geterrormsg(guard.env));
    }

    // Presolve's dual reductions are only valid for feasible, bounded models;
    // when they fire on a model that is neither, Gurobi can only report the
    // ambiguous INF_OR_UNBD. Gurobi's documented recipe: disable dual
    // reductions and re-solve, which yields a definitive INFEASIBLE or
    // UNBOUNDED. The extra solve only happens in this rare ambiguous case.
    if (status == GRB_INF_OR_UNBD) {
        void *model_env = api.getenv_model(guard.model);
        if (model_env && api.setintparam(model_env, "DualReductions", 0) == 0 &&
            api.optimize(guard.model) == 0) {
            error = api.getintattr(guard.model, GRB_INT_ATTR_STATUS, &status);
            if (error) {
                throw InternalException("Failed to get Gurobi status: %s",
                                        api.geterrormsg(guard.env));
            }
        }
        // If disambiguation itself failed, fall through with the original
        // INF_OR_UNBD status; the branch below reports both possibilities.
    }

    // Soundness: never relabel a non-OPTIMAL termination to OPTIMAL. A
    // suboptimal-but-feasible incumbent at TIME_LIMIT / ITER_LIMIT / etc. is
    // not a proof of optimality; returning it as if it were would silently
    // mislead the caller. Instead we map the raw status to a SolverStatus and
    // return it (no solution); the operator surfaces the default error
    // (manual-first) or, with a diagnose pragma, routes it to diagnosis.
    if (status != GRB_OPTIMAL) {
        SolverResult result;
        result.raw_status = status;
        switch (status) {
        case GRB_INFEASIBLE:
            result.status = SolverStatus::INFEASIBLE;
            break;
        case GRB_UNBOUNDED:
            result.status = SolverStatus::UNBOUNDED;
            break;
        case GRB_INF_OR_UNBD:
            // Only reachable if the DualReductions=0 re-solve above failed to
            // disambiguate.
            result.status = SolverStatus::INF_OR_UNBD;
            break;
        case GRB_TIME_LIMIT:
            result.status = SolverStatus::TIME_LIMIT;
            break;
        case GRB_ITERATION_LIMIT:
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

    vector<double> result(total_vars);
    error = api.getdblattrarray(guard.model, GRB_DBL_ATTR_X, 0, (int)total_vars, result.data());
    if (error) {
        throw InternalException("Failed to extract Gurobi solution: %s",
                                api.geterrormsg(guard.env));
    }

    for (idx_t i = 0; i < total_vars; i++) {
        if (!std::isfinite(result[i])) {
            throw InternalException(
                "Gurobi returned invalid solution value (NaN or Infinity) for variable %llu", i);
        }
    }

    SolverResult solve_result;
    solve_result.status = SolverStatus::OPTIMAL;
    solve_result.solution = std::move(result);
    // Objective value at the optimum, in the model's own sense (the diagnostics
    // stage-2 re-solve reads this as the achievable objective). A failure to read it
    // is non-fatal — leave the 0.0 default.
    double obj_val = 0.0;
    if (api.getdblattr(guard.model, GRB_DBL_ATTR_OBJVAL, &obj_val) == 0) {
        solve_result.objective_value = obj_val;
    }
    return solve_result;
}

} // namespace duckdb
