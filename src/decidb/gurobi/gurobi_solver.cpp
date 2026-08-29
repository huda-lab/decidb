#include "duckdb/decidb/gurobi/gurobi_solver.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/solver/solver_session.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/decidb/gurobi/gurobi_loader.hpp"
#include "duckdb/decidb/diagnostics/diagnostic_constants.hpp"

#include <atomic>
#include <cstdlib>
#include <string>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

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

const SolverCapabilities &GurobiSolver::Capabilities() {
    // Cached alongside IsAvailable(): both are answers about the library that was
    // loaded into THIS process, and neither can change without a fresh start.
    //
    // This reports what Gurobi CAN do. The test-only DECIDB_NATIVE_CONSTRUCTS switch is
    // not applied here — SolverBackend::Capabilities() masks constructs centrally, for
    // every backend, so no backend carries its own copy of the switch.
    static const SolverCapabilities capabilities = []() {
        SolverCapabilities caps;
        // Model classes Gurobi accepts directly. These are gates: nothing lowers a
        // non-convex objective into linear rows, so a backend that says false here
        // makes the query unrunnable rather than slow.
        caps.model_classes.quadratic_constraints = true;
        caps.model_classes.nonconvex_quadratic = true;
        caps.model_classes.miqp = true;
        // Gurobi's QP solver handles a rank-deficient Q — the flat valley one squared
        // expression over several decisions produces — and reaches the true optimum on
        // it. Verified against the sweep behind the HiGHS refusal.
        caps.model_classes.singular_quadratic = true;
        // Construct flags. Each is true only when the loaded library actually exported
        // the symbol behind it — a capability is partly a runtime fact — and each is
        // A/B-verifiable: DECIDB_NATIVE_CONSTRUCTS=off forces every one back down its
        // lowering path, which must reach the same optimum. A flag that cannot be
        // tested that way does not belong in the table.
        //
        // Load() FIRST, and note this answer is cached for the process: reading the
        // symbol table before the library is open would report every construct as
        // unexported, and nothing later could undo that false negative. Load() is itself
        // call_once, so asking here costs nothing on the paths that already ran it.
        //
        // The remaining construct (bilinear) stays false until the loader binds its
        // symbols and stage 08 knows how to emit it: a capability may not be declared
        // ahead of the code that honors it.
        if (GurobiLoader::Load()) {
            auto &api = GurobiLoader::API();
            caps.constructs.abs = api.addgenconstrAbs != nullptr;
            caps.constructs.min_max = api.addgenconstrMin != nullptr && api.addgenconstrMax != nullptr;
            caps.constructs.not_equal = api.addgenconstrIndicator != nullptr;
        }
        return caps;
    }();
    return capabilities;
}

namespace {

//! Resumable Gurobi handle. Load() builds the env+model once (its C resources
//! live in `guard` as members, so they survive across Continue()); RunAndReadback()
//! sets the per-chunk TimeLimit on the live model and (re-)optimizes. Gurobi resumes
//! the MIP search in place on a repeat optimize() after a time-limit stop, so
//! Continue() is just another RunAndReadback() with a fresh chunk budget.
class GurobiSession : public SolverSession {
public:
    SolverResult Solve(const SolverModel &ilp, double time_limit_seconds) override {
        Load(ilp);
        return RunAndReadback(time_limit_seconds);
    }

    SolverResult Continue(double time_limit_seconds) override {
        // Precondition: Solve() already loaded the model into `guard`.
        return RunAndReadback(time_limit_seconds);
    }

    void SetInterruptPoll(std::function<bool()> poll) override {
        should_interrupt = std::move(poll);
    }

private:
    GurobiGuard guard;
    idx_t total_vars = 0;
    //! When set (continuation only), a watcher thread polls this during optimize() and
    //! calls GRBterminate() to cut the chunk short; empty = boundary-only (the default).
    std::function<bool()> should_interrupt;

    void Load(const SolverModel &ilp);
    SolverResult RunAndReadback(double time_limit_seconds);
};

void GurobiSession::Load(const SolverModel &ilp) {
    auto &api = GurobiLoader::API();
    total_vars = ilp.num_vars;

    //===--------------------------------------------------------------------===//
    // 1. Create Gurobi environment
    //===--------------------------------------------------------------------===//

    int error = api.emptyenv_internal(&guard.env, api.version_major, api.version_minor, api.version_tech);
    if (error || !guard.env) {
        throw InternalException("Failed to create Gurobi environment (error %d). "
                                "Check that GUROBI_HOME is set and license is valid.",
                                error);
    }
    api.setintparam(guard.env, "OutputFlag", 0);
    // Note: the TimeLimit is NOT set here — it is a per-chunk budget applied in
    // RunAndReadback() (on the live model's env), so a warm Continue() can raise it
    // without a reload.
    // Enable non-convex QP solving via spatial branching when needed
    if (ilp.nonconvex_quadratic) {
        api.setintparam(guard.env, "NonConvex", 2);
    }
    // A quadratic constraint switches Gurobi from its fast QP solver to the barrier
    // over a second-order-cone reformulation of BOTH the objective and the constraint.
    // At scale the barrier reaches the optimum early but then oscillates in the dual
    // for many extra iterations, often stopping at "sub-optimal termination". Raising
    // NumericFocus makes the barrier careful enough to converge to a true optimum —
    // empirically several times faster than the default's stalled solve, and it removes
    // the sub-optimal stop. Gated on quadratic constraints so the fast pure-QP-objective
    // path (no such reformulation) is left untouched. Gurobi-only: HiGHS rejects
    // quadratic constraints upstream, so nothing reaches this code on that backend.
    if (!ilp.quadratic_constraints.empty()) {
        api.setintparam(guard.env, "NumericFocus", 2);
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

    // An open column box has to arrive as Gurobi's OWN infinity, not ours. The model
    // builder writes 1e30 for "no bound" and every DeciDB-side check reads it that way
    // (EFFECTIVE_INFINITY is 1e20), but GRB_INFINITY is 1e100 — so 1e30 lands as a
    // perfectly finite bound. Gurobi then solves an unbounded query to "optimality"
    // against that ceiling, and an INTEGER column comes back at its integer limit
    // (2147483647) instead of the query being reported as unbounded. Translating here
    // keeps the sentinel local to each backend rather than forcing one on the model.
    vector<double> col_lower(ilp.col_lower.begin(), ilp.col_lower.end());
    vector<double> col_upper(ilp.col_upper.begin(), ilp.col_upper.end());
    for (idx_t i = 0; i < total_vars; i++) {
        if (col_lower[i] <= -EFFECTIVE_INFINITY) {
            col_lower[i] = -GRB_INFINITY_VALUE;
        }
        if (col_upper[i] >= EFFECTIVE_INFINITY) {
            col_upper[i] = GRB_INFINITY_VALUE;
        }
    }

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
                         col_lower.data(),
                         col_upper.data(),
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

    // 3b'. General constraints — constructs DeciDB left native rather than lowering.
    // Pure translation: the model only ever holds a kind this backend declared in
    // GurobiSolver::Capabilities(), and that declaration is itself gated on the loader
    // having found the symbol. So an unknown kind or a null pointer here is a bug in
    // the gate, not a user error.
    for (auto &gc : ilp.general_constraints) {
        switch (gc.kind) {
        case GeneralConstraintKind::ABS: {
            D_ASSERT(api.addgenconstrAbs && gc.argument_columns.size() == 1);
            error = api.addgenconstrAbs(guard.model, nullptr, gc.result_column,
                                        gc.argument_columns[0]);
            break;
        }
        case GeneralConstraintKind::MIN: {
            D_ASSERT(api.addgenconstrMin && !gc.argument_columns.empty());
            // The trailing constant is a neutral element, not a participant: +inf can
            // never be the minimum of the listed columns.
            error = api.addgenconstrMin(guard.model, nullptr, gc.result_column,
                                        static_cast<int>(gc.argument_columns.size()),
                                        gc.argument_columns.data(), GRB_INFINITY_VALUE);
            break;
        }
        case GeneralConstraintKind::MAX: {
            D_ASSERT(api.addgenconstrMax && !gc.argument_columns.empty());
            error = api.addgenconstrMax(guard.model, nullptr, gc.result_column,
                                        static_cast<int>(gc.argument_columns.size()),
                                        gc.argument_columns.data(), -GRB_INFINITY_VALUE);
            break;
        }
        default:
            throw InternalException("Gurobi was handed a general constraint kind it did not declare");
        }
        if (error) {
            throw InvalidInputException("Failed to add general constraint to Gurobi: %s",
                                        api.geterrormsg(guard.env));
        }
    }

    // 3b''. Indicator constraints — a row conditioned on a binary, rather than the
    // Big-M that would otherwise stand in for the implication.
    for (auto &ic : ilp.indicator_constraints) {
        D_ASSERT(api.addgenconstrIndicator);
        error = api.addgenconstrIndicator(guard.model, nullptr, ic.binary_column, ic.binary_value,
                                          static_cast<int>(ic.indices.size()), ic.indices.data(),
                                          ic.coefficients.data(), ic.sense, ic.rhs);
        if (error) {
            throw InvalidInputException("Failed to add indicator constraint to Gurobi: %s",
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
}

SolverResult GurobiSession::RunAndReadback(double time_limit_seconds) {
    auto &api = GurobiLoader::API();

    //===--------------------------------------------------------------------===//
    // 4. Solve
    //===--------------------------------------------------------------------===//

    // Apply the per-chunk wall-clock cap on the live model's env. Setting it here
    // (rather than on the base env at Load) is what lets a warm Continue() extend
    // the budget without rebuilding — the same live-model param path the
    // DualReductions disambiguation below uses.
    void *model_env = api.getenv_model(guard.model);
    if (model_env) {
        api.setdblparam(model_env, "TimeLimit", time_limit_seconds);
    }

    // Mid-solve interrupt: while optimize() blocks, a watcher thread polls `should_interrupt`
    // and calls GRBterminate() (thread-safe) to end the chunk early on Ctrl-C — otherwise the
    // interrupt would only be seen at the next chunk boundary. Gated on the poll being set
    // (continuation only) AND the optional terminate symbol being present; absent either, this
    // is a no-op and the solve runs exactly as before. optimize() then returns status
    // GRB_INTERRUPTED, mapped to TIME_LIMIT below (a stop-with-best-so-far, like a real limit).
    std::atomic<bool> solve_finished {false};
    std::thread interrupt_watcher;
    if (should_interrupt && api.terminate) {
        interrupt_watcher = std::thread([this, &api, &solve_finished]() {
            while (!solve_finished.load(std::memory_order_relaxed)) {
                if (should_interrupt()) {
                    api.terminate(guard.model);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
    }
    int error = api.optimize(guard.model);
    solve_finished.store(true, std::memory_order_relaxed);
    if (interrupt_watcher.joinable()) {
        interrupt_watcher.join();
    }
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
    // return it (no solution); the operator either surfaces the default error for an
    // unprefixed statement or routes it to diagnosis under DIAGNOSE.
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
        case GRB_INTERRUPTED: // mid-solve Ctrl-C (terminate): a stop-with-best-so-far, exactly
                              // like a time-limit stop — read the incumbent the same way, but
                              // flag it so the report says "you stopped it", not "hit the limit".
            result.user_interrupted = true;
            // fall through
        case GRB_TIME_LIMIT: {
            result.status = SolverStatus::TIME_LIMIT;
            // A proven bound only exists for MIP timeouts; on an LP/QP timeout
            // ObjBound reads back the ±1e100 infinity sentinel (which is finite,
            // so it would sail through downstream isfinite guards). Keep the NaN
            // "unavailable" default unless the read succeeds with a real value.
            double obj_bound = 0.0;
            if (api.getdblattr(guard.model, GRB_DBL_ATTR_OBJBOUND, &obj_bound) == 0 &&
                std::isfinite(obj_bound) && std::fabs(obj_bound) < EFFECTIVE_INFINITY) {
                result.best_bound = obj_bound;
            }
            // Incumbent reads (objective / gap / solution vector) are only valid
            // when Gurobi found at least one feasible solution; with SolCount == 0
            // ObjVal/MIPGap succeed but return the -1e100 / 1e100 sentinels, so
            // gate every incumbent read on SolCount.
            int sol_count = 0;
            if (api.getintattr(guard.model, GRB_INT_ATTR_SOLCOUNT, &sol_count) == 0 && sol_count > 0) {
                result.has_solution = true;
                double obj_val = 0.0;
                if (api.getdblattr(guard.model, GRB_DBL_ATTR_OBJVAL, &obj_val) == 0) {
                    result.objective_value = obj_val;
                }
                // MIPGap is unavailable on LP/QP models (the read fails) and is
                // the 1e100 sentinel when no bound exists yet; keep NaN in both
                // cases so the report omits the closeness claim instead of
                // asserting "within 0.00%" on an unproven incumbent.
                double mip_gap = 0.0;
                if (api.getdblattr(guard.model, GRB_DBL_ATTR_MIPGAP, &mip_gap) == 0 &&
                    std::isfinite(mip_gap) && mip_gap < EFFECTIVE_INFINITY) {
                    result.gap = mip_gap;
                }
                vector<double> incumbent(total_vars);
                if (api.getdblattrarray(guard.model, GRB_DBL_ATTR_X, 0, (int)total_vars,
                                        incumbent.data()) == 0) {
                    result.solution = std::move(incumbent);
                }
            }
            break;
        }
        case GRB_ITERATION_LIMIT:
            result.status = SolverStatus::ITERATION_LIMIT;
            break;
        case GRB_SUBOPTIMAL: {
            // Gurobi could not satisfy optimality tolerances (typically a numerically
            // hard barrier on a QCP/SOC model) but a feasible solution IS available.
            // Read it back and return it as a feasible-but-unproven result rather than
            // discarding it; the operator delivers the rows with a "not proven best"
            // caveat. Guard on SolCount and a successful vector read: if no usable
            // incumbent exists (should not happen for SUBOPTIMAL), fall back to the
            // generic error path so we never route an empty solution to success.
            int sol_count = 0;
            vector<double> incumbent(total_vars);
            if (api.getintattr(guard.model, GRB_INT_ATTR_SOLCOUNT, &sol_count) == 0 && sol_count > 0 &&
                api.getdblattrarray(guard.model, GRB_DBL_ATTR_X, 0, (int)total_vars,
                                    incumbent.data()) == 0) {
                result.status = SolverStatus::SUBOPTIMAL;
                result.has_solution = true;
                result.solution = std::move(incumbent);
                double obj_val = 0.0;
                if (api.getdblattr(guard.model, GRB_DBL_ATTR_OBJVAL, &obj_val) == 0) {
                    result.objective_value = obj_val;
                }
            } else {
                result.status = SolverStatus::OTHER;
            }
            break;
        }
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

} // namespace

unique_ptr<SolverSession> GurobiSolver::CreateSession() {
    return make_uniq<GurobiSession>();
}

} // namespace duckdb
