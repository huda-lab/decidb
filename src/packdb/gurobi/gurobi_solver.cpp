#include "duckdb/packdb/gurobi/gurobi_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/common/exception.hpp"
#include "gurobi_loader.hpp"

#include <cmath>

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

vector<double> GurobiSolver::Solve(const SolverModel &ilp) {
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
    // 300s is generous for typical workloads; truly hard problems return the best
    // feasible solution found so far (handled by the GRB_TIME_LIMIT branch below).
    api.setdblparam(guard.env, "TimeLimit", 300.0);
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

    error = api.newmodel(guard.env, &guard.model, "packdb_decide",
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

    // Soundness: never relabel a non-OPTIMAL termination to OPTIMAL. A
    // suboptimal-but-feasible incumbent at TIME_LIMIT / ITER_LIMIT / etc. is
    // not a proof of optimality; returning it as if it were would silently
    // mislead the caller. We throw a clear error instead. The TimeLimit set
    // above keeps the session from hanging; surfacing it as a query error
    // (rather than relabeling to OPTIMAL) preserves correctness.
    if (status != GRB_OPTIMAL) {
        if (status == GRB_INFEASIBLE) {
            throw InvalidInputException(
                "DECIDE optimization is infeasible: No valid solution exists that satisfies all constraints.\n\n"
                "This means the SUCH THAT conditions cannot all be met simultaneously.\n\n"
                "Common causes:\n"
                "  • Contradictory bounds (e.g., x >= 10 AND x <= 5)\n"
                "  • SUM constraints impossible to satisfy with available data\n"
                "  • Variable types too restrictive (BOOLEAN when INTEGER needed)\n\n"
                "Suggestion: Try relaxing constraints or verify input data.");
        } else if (status == GRB_UNBOUNDED || status == GRB_INF_OR_UNBD) {
            throw InvalidInputException(
                "DECIDE optimization is unbounded: The objective can grow infinitely.\n\n"
                "This means the MAXIMIZE/MINIMIZE goal has no finite optimal value.\n"
                "You must add constraints to bound the decision variables.\n\n"
                "Examples:\n"
                "  • Add upper bounds: SUCH THAT x <= 100\n"
                "  • Add budget limits: SUCH THAT SUM(x * cost) <= budget\n"
                "  • Use BOOLEAN instead of INTEGER for selection problems");
        } else if (status == GRB_TIME_LIMIT) {
            throw InvalidInputException(
                "DECIDE optimization exceeded time limit.\n"
                "The problem may be too complex to solve in reasonable time.\n"
                "Try simplifying constraints or reducing data size.");
        } else if (status == GRB_ITERATION_LIMIT) {
            throw InvalidInputException(
                "DECIDE optimization exceeded iteration limit.\n"
                "The problem may be too complex. Try simplifying constraints.");
        } else {
            throw InvalidInputException(
                "DECIDE optimization failed with Gurobi status %d.\n"
                "The optimization could not find a solution.\n"
                "This may indicate a problem with the constraints or objective.",
                status);
        }
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

    return result;
}

} // namespace duckdb
