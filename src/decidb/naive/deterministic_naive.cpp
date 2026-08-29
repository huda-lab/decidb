#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/solver_session.hpp"
#include "duckdb/decidb/diagnostic_constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "Highs.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace duckdb {

bool DeterministicNaive::IsAvailable() {
    // HiGHS is vendored and statically linked: it is the backend that makes
    // selection total, so it can never be missing.
    return true;
}

const SolverCapabilities &DeterministicNaive::Capabilities() {
    // The floor of the registry, and the reason every field can be left at its default:
    // HiGHS declares nothing. Plain linear objectives, and convex quadratic objectives
    // whose Q is diagonal. Every model class is false, so a query needing one is refused
    // with a message about the host; every construct is false, so each arrives fully
    // lowered — the path DeciDB has always taken.
    //
    // `singular_quadratic` is false for a different reason than the other three. HiGHS
    // LOADS a rank-deficient Q without complaint; it just answers it wrong, stopping
    // partway along the flat valley of optima or failing outright, on roughly half of
    // them. Refusing is the only way to keep a wrong answer from being returned as an
    // optimal one. Revisit as HiGHS's QP solver improves.
    //
    // Nothing here reads DECIDB_NATIVE_CONSTRUCTS: SolverBackend::Capabilities() masks
    // constructs for every backend, so the switch reaches HiGHS whether or not HiGHS
    // ever declares one. It is a no-op today precisely because this table is empty.
    static const SolverCapabilities capabilities;
    return capabilities;
}

namespace {

//! Render a model row as the user wrote it (`SUM(x) >= 100 PER g`) from the provenance
//! the model already carries, so a refusal below reads in SQL terms rather than as a
//! matrix row. `rhs_text` is the bound as the caller wants it spelled. Empty when the row
//! has no source clause to quote; the caller then falls back to a clause-free message.
string DescribeRowClause(const SolverModel &model, const ModelConstraint &constr, const string &rhs_text) {
    auto source_id = constr.provenance.source_clause_id;
    if (source_id == DConstants::INVALID_INDEX || source_id >= model.constraint_sources.size()) {
        return string();
    }
    const auto &source = model.constraint_sources[source_id];
    if (source.canonical_lhs.empty()) {
        return string();
    }
    const char *sense = constr.sense == '<' ? "<=" : (constr.sense == '>' ? ">=" : "=");
    string label = source.canonical_lhs + " " + sense + " " + rhs_text;
    if (!source.qualifier.empty()) {
        label += " " + source.qualifier;
    }
    return label;
}

//! The `DescribeRowClause` spelling for the unreachable-bound refusal below. The bound is
//! always ±inf there — that is what makes the row unreachable — so there is no number to
//! format, and the strict-`<` δ offset that `ConstraintProvenance::typed_k` exists for
//! cannot apply.
string DescribeUnreachableRow(const SolverModel &model, const ModelConstraint &constr) {
    return DescribeRowClause(model, constr, constr.rhs > 0.0 ? "inf" : "-inf");
}

//===--------------------------------------------------------------------===//
// HiGHS's coefficient window, and the row scaling that keeps our models inside it
//===--------------------------------------------------------------------===//

// `passModel` enforces a magnitude window on every constraint-matrix entry, at both ends
// and with different consequences:
//   * |v| <= `small_matrix_value` (1e-9): the entry is DELETED from the matrix and
//     `passModel` returns kWarning. The model HiGHS then solves is not the one we built.
//   * |v| >= `large_matrix_value` (1e15): `passModel` returns kError.
// Both edges are reachable from ordinary SQL, and neither is a malformed query. A
// per-unit rate of 1e-9 is data. The heavy end is reached by coefficients DeciDB itself
// generates — the Big-M closing `<>` / MIN / MAX is the decision's own span, so
// `x(REAL) SUCH THAT x <= 4e15 AND MAX(x) >= 3` puts a 4e15 entry in the matrix while
// every number the user typed is in range. Gurobi loads both ends without complaint, so
// left alone this decides whether a query works by which solver happens to be installed.
constexpr double HIGHS_SMALL_MATRIX_VALUE = 1e-9;
constexpr double HIGHS_LARGE_MATRIX_VALUE = 1e15;
// |bound| >= `infinite_bound` (1e20) is read by HiGHS as ±infinity, which on the finite
// side of a row is an error rather than a bound. Scaling carries the bound along with the
// row, so this is what caps how far a row may be scaled up.
constexpr double HIGHS_INFINITE_BOUND = 1e20;
//! Why a row could not be placed inside the window, so the refusal can say which.
enum class RowScaleResult { OK, SPREAD_TOO_WIDE, BOUND_TOO_LARGE };

//! Choose a power of two to multiply one row through by so its coefficients land inside
//! HiGHS's window. Scaling a row by a positive constant is the one rewrite that leaves a
//! constraint's meaning exactly intact — `1e-9x₁ + 1e-9x₂ >= 1` and `x₁ + x₂ >= 1e9` have
//! identical solution sets — so unlike rounding a sub-tolerance entry to zero it cannot
//! change an answer. (Rounding can: drop both coefficients of that same row and it becomes
//! `0 >= 1`, turning a feasible query infeasible.) A POWER OF TWO specifically, because
//! `ldexp` is then exact in binary floating point and introduces no drift, so both
//! backends still solve bit-identical constraints. HiGHS's own internal scaling picks
//! powers of two for the same reason.
//!
//! `lo` / `hi` are the row's smallest and largest |coefficient|; `bound` is the largest
//! |finite row bound|. A row is scaled when either its coefficients or that bound is out
//! of range — a limit at or above `infinite_bound` needs the row scaled DOWN even though
//! its coefficients are fine, which is how `SUM(x) >= 1e25` becomes a row HiGHS can load
//! and answer instead of dying at `passModel`. Rows inside both
//! windows return exponent 0 and are left untouched rather than re-centred, so this
//! changes nothing about the models that load today.
RowScaleResult ChooseHighsRowScale(double lo, double hi, double bound, int &exponent) {
    exponent = 0;
    if (lo > HIGHS_SMALL_MATRIX_VALUE && hi < HIGHS_LARGE_MATRIX_VALUE && bound < HIGHS_INFINITE_BOUND) {
        return RowScaleResult::OK;
    }
    // Smallest k with 2^k·lo > small; largest k with 2^k·hi < large. Both thresholds are
    // exclusive on the side HiGHS rejects (it drops at <= small and errors at >= large).
    int k_min = static_cast<int>(std::floor(std::log2(HIGHS_SMALL_MATRIX_VALUE / lo))) + 1;
    int k_max = static_cast<int>(std::ceil(std::log2(HIGHS_LARGE_MATRIX_VALUE / hi))) - 1;
    if (k_min > k_max) {
        // The row's own coefficients span more than the window's ~24 orders of magnitude.
        // No single factor can hold both ends, and rescaling is all we are allowed to do.
        return RowScaleResult::SPREAD_TOO_WIDE;
    }
    if (bound > 0.0) {
        int k_bound = static_cast<int>(std::ceil(std::log2(HIGHS_INFINITE_BOUND / bound))) - 1;
        if (k_min > k_bound) {
            // The coefficients fit, but only by scaling the row up so far that its bound
            // crosses into HiGHS's infinity and stops being a bound at all.
            return RowScaleResult::BOUND_TOO_LARGE;
        }
        k_max = std::min(k_max, k_bound);
    }
    // Centre the row in the window rather than shifting it just far enough to clear the
    // edge: a coefficient sitting a hair above the drop threshold is inside the window but
    // still badly conditioned.
    double window_centre = 0.5 * (std::log2(HIGHS_SMALL_MATRIX_VALUE) + std::log2(HIGHS_LARGE_MATRIX_VALUE));
    double row_centre = 0.5 * (std::log2(lo) + std::log2(hi));
    int k = static_cast<int>(std::lround(window_centre - row_centre));
    exponent = std::min(std::max(k, k_min), k_max);
    return RowScaleResult::OK;
}

[[noreturn]] void ThrowUnscalableRow(const SolverModel &model, const ModelConstraint &constr, RowScaleResult why,
                                     double lo, double hi) {
    string clause = DescribeRowClause(model, constr, StringUtil::Format("%g", constr.rhs));
    string where = clause.empty() ? string("A constraint in this query")
                                  : StringUtil::Format("Constraint `%s`", clause);
    if (why == RowScaleResult::BOUND_TOO_LARGE) {
        throw InvalidInputException(
            "%s sets a limit of %g that HiGHS cannot hold alongside coefficients as small "
            "as %g. Use a smaller limit, or express the column and its limit in the same "
            "unit; Gurobi loads this range as written.",
            where, constr.rhs, lo);
    }
    throw InvalidInputException(
        "%s mixes numbers too far apart in size for HiGHS to load: coefficients run from %g "
        "to %g in one constraint. Express the columns in the same unit, or install Gurobi, "
        "which loads this range.",
        where, lo, hi);
}

//! Multiply each out-of-window row of the constraint matrix through by its own power of
//! two, bounds included. Applied to the COO matrix before it is packed into CSR, so the
//! scaled values are what reaches `passModel`.
void ScaleRowsIntoHighsWindow(const SolverModel &model, const vector<int> &a_rows, vector<double> &a_vals,
                              vector<double> &row_lower, vector<double> &row_upper) {
    idx_t num_rows = row_lower.size();
    vector<double> lo(num_rows, std::numeric_limits<double>::infinity());
    vector<double> hi(num_rows, 0.0);
    for (idx_t i = 0; i < a_vals.size(); i++) {
        double v = std::fabs(a_vals[i]);
        if (v == 0.0) {
            // The model builder strips exact zeros; one that slips through carries no
            // information, so HiGHS dropping it silently is correct and it must not drag
            // the row's `lo` to zero.
            continue;
        }
        idx_t r = static_cast<idx_t>(a_rows[i]);
        lo[r] = std::min(lo[r], v);
        hi[r] = std::max(hi[r], v);
    }

    vector<int> exponent(num_rows, 0);
    bool any = false;
    for (idx_t r = 0; r < num_rows; r++) {
        if (hi[r] == 0.0) {
            continue; // empty row: nothing to scale
        }
        // The row's bound moves with the row, so it both constrains how far the row may
        // be scaled and, when it is already unrepresentable, forces a scaling of its own.
        // The requirement is the same in either direction: a finite bound must end up with
        // |b| < `infinite_bound`. HiGHS reads anything at or above that as ±infinity, which
        // on one side of a row is an error and on the other silently drops the constraint
        // — a rescue that deletes a row is the model change this exists to prevent.
        //
        // The bound is read from the CLAUSE, not from the packed range row: the row's open
        // side holds HiGHS's ±1e30 "no bound here" sentinel, and a user limit of 1e40 is
        // indistinguishable from that sentinel by magnitude alone. The sense says which
        // side is which, exactly as the loop that packed them did. A non-finite rhs is the
        // vacuous infinity `IsUnreachableBound` deliberately lets through — HiGHS reads it
        // as no bound at all, and it is not a number to scale.
        const auto &constr = model.constraints[r];
        double bound = std::isfinite(constr.rhs) ? std::fabs(constr.rhs) : 0.0;
        RowScaleResult why = ChooseHighsRowScale(lo[r], hi[r], bound, exponent[r]);
        if (why != RowScaleResult::OK) {
            ThrowUnscalableRow(model, constr, why, lo[r], hi[r]);
        }
        any = any || exponent[r] != 0;
    }
    if (!any) {
        return;
    }

    for (idx_t i = 0; i < a_vals.size(); i++) {
        int k = exponent[static_cast<idx_t>(a_rows[i])];
        if (k != 0) {
            a_vals[i] = std::ldexp(a_vals[i], k);
        }
    }
    for (idx_t r = 0; r < num_rows; r++) {
        const auto &constr = model.constraints[r];
        if (exponent[r] == 0 || !std::isfinite(constr.rhs)) {
            continue;
        }
        double scaled = std::ldexp(constr.rhs, exponent[r]);
        if (constr.sense == '>') {
            row_lower[r] = scaled;
        } else if (constr.sense == '<') {
            row_upper[r] = scaled;
        } else {
            row_lower[r] = scaled;
            row_upper[r] = scaled;
        }
    }
}

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

    // A bound no assignment can reach (`SUM(x) >= inf`) cannot be handed to HiGHS at all.
    // HiGHS spells a one-sided row bound by pairing the user's bound with its own ±1e30
    // infinity sentinel, so `Ax >= +inf` becomes `lower = +inf, upper = 1e30` — an
    // inverted pair that `passModel` rejects outright, taking the connection down with an
    // internal error. Refuse it here in SQL terms instead. Only the unreachable direction
    // is affected: `Ax <= +inf` is vacuous, pairs cleanly with the sentinel, and solves.
    for (auto &constr : model.constraints) {
        if (!IsUnreachableBound(constr.sense, constr.rhs)) {
            continue;
        }
        string clause = DescribeUnreachableRow(model, constr);
        if (clause.empty()) {
            throw InvalidInputException(
                "A constraint in this query sets a bound no value can reach (an infinite "
                "limit), and HiGHS cannot load it. Replace the infinite bound with a finite "
                "value, or install Gurobi, which reports this as an infeasible query.");
        }
        throw InvalidInputException(
            "Constraint `%s` sets a bound no value can reach, and HiGHS cannot load it. "
            "Replace the infinite bound with a finite value, or install Gurobi, which "
            "reports this as an infeasible query.",
            clause);
    }

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

    // Bring every row inside HiGHS's coefficient window before the matrix is packed.
    // Each out-of-window row is multiplied through by its own power of two, bounds
    // included, which leaves the constraint — and therefore the answer — exactly intact.
    ScaleRowsIntoHighsWindow(model, a_rows, a_vals, row_lower, row_upper);

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
    // `passModel` returns kWarning for conditions it expects the caller to proceed
    // through — a matrix entry dropped for being sub-tolerance, an inconsistent bound
    // pair left in place so the solve can deduce infeasibility from it. Treating those as
    // fatal turned an ordinary query into an internal error with a stack trace. Throw only
    // on a genuine kError, the same split `RunAndReadback` already makes at `run()`.
    //
    // The dropped-coefficient case should no longer arrive here at all: every row is
    // scaled into the window above, and the model builder strips exact zeros, so there is
    // nothing left below the threshold to drop. Proceeding is still the right response if
    // one appears.
    if (status == HighsStatus::kError) {
        throw InternalException("Failed to pass model to HiGHS: status %d", (int)status);
    }

    //===--------------------------------------------------------------------===//
    // 3b. Add quadratic objective (Hessian) if present
    //===--------------------------------------------------------------------===//

    // The model classes HiGHS cannot take — quadratic constraints, a non-convex
    // objective, MIQP, and a rank-deficient Q — are all declared false in its
    // SolverModelClass and refused at plan time (stage 05's RequireDecideSolverSupport),
    // before this query reads a row. `SolveModel` re-checks the built model against the
    // same table before loading it, so no such model reaches this function.
    //
    // The last of those is why every Q arriving here is diagonal, and why the conversion
    // below has no off-diagonal case left to exercise in practice. It is still written
    // for the general matrix: the refusal is a solver-quality judgement that should be
    // lifted when HiGHS improves, and the conversion must be correct when it is.
    if (model.has_quadratic_obj && !model.q_vals.empty()) {
        // Convert COO lower-triangle Q to CSC format for HiGHS passHessian.
        // HiGHS expects the lower triangle in column-major compressed sparse column format.
        //
        // The two solvers spell the same objective differently, and SolverModel commits to
        // Gurobi's spelling (see ilp_model.hpp): a stored value is the plain coefficient of
        // its monomial. HiGHS's passHessian instead takes the Q of `(1/2) x^T Q x`, and it
        // applies that 1/2 to the triangle it is given -- HighsHessian::objectiveValue sums
        // `0.5*q_ii*x_i^2` over the diagonal but a full `q_ij*x_i*x_j` off it. The 1/2 is
        // there to undo the double-counting of an off-diagonal pair, which appears at both
        // (i,j) and (j,i) of the symmetric matrix; a diagonal entry is never mirrored, so
        // nothing cancels its 1/2. Double the diagonal on the way in and leave the rest.
        //
        // Doubling every entry instead would not merely rescale the objective: it inflates
        // the cross terms relative to the squares, which turns the PSD Q of a multi-variable
        // POWER group into an indefinite one and puts the model outside what HiGHS solves.
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
            q_value[pos] = (model.q_rows[k] == col) ? 2.0 * model.q_vals[k] : model.q_vals[k];
            current_pos[col]++;
        }

        status = highs.passHessian((HighsInt)total_vars, (HighsInt)num_nz,
                                   (HighsInt)HessianFormat::kTriangular,
                                   q_start.data(), q_index.data(), q_value.data());
        // Same kWarning/kError split as `passModel` above: `assessHessian` drops
        // sub-tolerance entries of the normalised Hessian and warns, which is a report
        // rather than a failure. Row scaling does not reach here — it rewrites constraint
        // rows, and the Hessian is the objective.
        if (status == HighsStatus::kError) {
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
        // (no solution). The operator either surfaces the default error for an
        // unprefixed statement or routes it to diagnosis under DIAGNOSE.
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

unique_ptr<SolverSession> DeterministicNaive::CreateSession() {
    return make_uniq<HighsSession>();
}

} // namespace duckdb
