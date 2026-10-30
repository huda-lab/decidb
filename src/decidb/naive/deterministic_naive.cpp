#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_profile.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/solver/solver_session.hpp"
#include "duckdb/decidb/diagnostics/diagnostic_constants.hpp"
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

//! Render a model row as the user wrote it (`per g: sum(x) by (g) >= 100`) from the provenance
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
    return QuoteEditedClause(&source, source.canonical_lhs, sense, rhs_text);
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
// side of a row or column is an error rather than a bound. Scaling carries the bound along
// with the row, so this is what caps how far a row may be scaled up. A linear objective
// coefficient has the same ceiling (`infinite_cost`, also 1e20): HiGHS reads one at or
// past it as an infinite cost and fails the solve.
constexpr double HIGHS_INFINITE_BOUND = 1e20;
//! Why a row or the objective could not be placed inside the window, so the refusal can say which.
enum class HighsScaleResult { OK, SPREAD_TOO_WIDE, BOUND_TOO_LARGE };

//! Choose a power of two to multiply one row through by so its coefficients land inside
//! HiGHS's window. The objective and the columns are placed by the same rule (see
//! ScaleObjectiveIntoHighsWindow and ChooseHighsColumnScales); a set with no coefficients
//! (`hi` = 0) is placed by its bound alone, moved only as far as the bound needs.
//! Scaling a row by a positive constant is the one rewrite that leaves a
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
HighsScaleResult ChooseHighsScale(double lo, double hi, double bound, int &exponent) {
	exponent = 0;
	if (lo > HIGHS_SMALL_MATRIX_VALUE && hi < HIGHS_LARGE_MATRIX_VALUE && bound < HIGHS_INFINITE_BOUND) {
		return HighsScaleResult::OK;
	}
	int k_min = std::numeric_limits<int>::min();
	int k_max = std::numeric_limits<int>::max();
	int k = 0;
	if (hi > 0.0) {
		// Smallest k with 2^k·lo > small; largest k with 2^k·hi < large. Both thresholds are
		// exclusive on the side HiGHS rejects (it drops at <= small and errors at >= large).
		k_min = static_cast<int>(std::floor(std::log2(HIGHS_SMALL_MATRIX_VALUE / lo))) + 1;
		k_max = static_cast<int>(std::ceil(std::log2(HIGHS_LARGE_MATRIX_VALUE / hi))) - 1;
		if (k_min > k_max) {
			// The row's own coefficients span more than the window's ~24 orders of magnitude.
			// No single factor can hold both ends, and rescaling is all we are allowed to do.
			return HighsScaleResult::SPREAD_TOO_WIDE;
		}
		// Centre the row in the window rather than shifting it just far enough to clear the
		// edge: a coefficient sitting a hair above the drop threshold is inside the window but
		// still badly conditioned.
		double window_centre = 0.5 * (std::log2(HIGHS_SMALL_MATRIX_VALUE) + std::log2(HIGHS_LARGE_MATRIX_VALUE));
		double row_centre = 0.5 * (std::log2(lo) + std::log2(hi));
		k = static_cast<int>(std::lround(window_centre - row_centre));
	}
	if (bound > 0.0) {
		int k_bound = static_cast<int>(std::ceil(std::log2(HIGHS_INFINITE_BOUND / bound))) - 1;
		if (k_min > k_bound) {
			// The coefficients fit, but only by scaling the row up so far that its bound
			// crosses into HiGHS's infinity and stops being a bound at all.
			return HighsScaleResult::BOUND_TOO_LARGE;
		}
		k_max = std::min(k_max, k_bound);
	}
	exponent = std::min(std::max(k, k_min), k_max);
	return HighsScaleResult::OK;
}

//! Refuse a row or the objective that fits the window as written and fails only once a column is scaled: a
//! decision's limit at or past `infinite_bound` is the cause (ChooseHighsColumnScales), and the clause already shows
//! the user which decisions it holds.
[[noreturn]] void ThrowDecisionLimitRefusal(const string &where) {
	throw InvalidInputException("%s cannot be loaded by HiGHS together with a decision limit of %g or more. Use "
	                            "smaller limits on the decisions in it, or install Gurobi, which loads this range.",
	                            where, HIGHS_INFINITE_BOUND);
}

//! Refuse a row no power of two places. The cause is judged on the row as the user wrote it, before any column
//! scale: if that row cannot be placed, the row is the cause and the refusal quotes its own coefficients; if it can,
//! a scaled column's limit is (ThrowDecisionLimitRefusal). A row holding no scaled column is the same either way.
[[noreturn]] void ThrowUnscalableRow(const SolverModel &model, const ModelConstraint &constr, double bound) {
	double lo = std::numeric_limits<double>::infinity();
	double hi = 0.0;
	for (auto c : constr.coefficients) {
		double v = std::fabs(c);
		if (v != 0.0) {
			lo = std::min(lo, v);
			hi = std::max(hi, v);
		}
	}
	int exponent;
	HighsScaleResult why = ChooseHighsScale(lo, hi, bound, exponent);
	string clause = DescribeRowClause(model, constr, StringUtil::Format("%g", constr.rhs));
	string where =
	    clause.empty() ? string("A constraint in this query") : StringUtil::Format("Constraint `%s`", clause);
	if (why == HighsScaleResult::OK) {
		ThrowDecisionLimitRefusal(where);
	}
	if (why == HighsScaleResult::BOUND_TOO_LARGE) {
		throw InvalidInputException("%s sets a limit of %g that HiGHS cannot hold alongside coefficients as small "
		                            "as %g. Use a smaller limit, or express the column and its limit in the same "
		                            "unit; Gurobi loads this range as written.",
		                            where, constr.rhs, lo);
	}
	throw InvalidInputException("%s mixes numbers too far apart in size for HiGHS to load: coefficients run from %g "
	                            "to %g in one constraint. Express the columns in the same unit, or install Gurobi, "
	                            "which loads this range.",
	                            where, lo, hi);
}

//! Multiply each out-of-window row of the constraint matrix through by its own power of
//! two, bounds included. Applied to the row-wise matrix `passModel` receives (`start`, `value`).
void ScaleRowsIntoHighsWindow(const SolverModel &model, const std::vector<HighsInt> &start, std::vector<double> &value,
                              std::vector<double> &row_lower, std::vector<double> &row_upper) {
	idx_t num_rows = row_lower.size();
    vector<double> lo(num_rows, std::numeric_limits<double>::infinity());
    vector<double> hi(num_rows, 0.0);
	for (idx_t r = 0; r < num_rows; r++) {
		for (auto i = start[r]; i < start[r + 1]; i++) {
			double v = std::fabs(value[i]);
			if (v == 0.0) {
				// The model builder strips exact zeros; one that slips through carries no
				// information, so HiGHS dropping it silently is correct and it must not drag
				// the row's `lo` to zero.
				continue;
			}
			lo[r] = std::min(lo[r], v);
			hi[r] = std::max(hi[r], v);
		}
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
        // vacuous infinity (SolverModel::ProvesInfeasible answers the other direction before
        // HiGHS sees the model) — HiGHS reads it as no bound at all, and it is not a number
        // to scale.
        const auto &constr = model.constraints[r];
        double bound = std::isfinite(constr.rhs) ? std::fabs(constr.rhs) : 0.0;
        if (ChooseHighsScale(lo[r], hi[r], bound, exponent[r]) != HighsScaleResult::OK) {
            ThrowUnscalableRow(model, constr, bound);
        }
        any = any || exponent[r] != 0;
    }
    if (!any) {
        return;
    }

    for (idx_t r = 0; r < num_rows; r++) {
		if (exponent[r] == 0) {
			continue;
		}
		for (auto i = start[r]; i < start[r + 1]; i++) {
			value[i] = std::ldexp(value[i], exponent[r]);
		}
		const auto &constr = model.constraints[r];
		if (!std::isfinite(constr.rhs)) {
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

//! The power of two `e` each column is scaled by, x = 2^e·x', so its bounds land inside
//! HiGHS's window: the bounds are divided by 2^e, and the column's matrix entries, cost and
//! Hessian entries multiplied. This is LP column scaling, applied only to a column HiGHS
//! cannot hold as written, so every other column keeps e = 0 and loads exactly as before.
//! That column has a bound past `infinite_bound` on the side HiGHS cannot read as "no
//! bound": `x >= 1e25` is a floor (Gurobi answers x = 1e25) that `passModel` refuses. The
//! same magnitude on the other side (`x <= 1e25`) is open in DeciDB's convention
//! (DecideIsOpenAbove) and in HiGHS's alike, and stays open. The column moves only as far
//! as its bound needs; its entries grow by that factor, and the row scaling that runs
//! after this places the rows they land in. An infinite bound never arrives: a box holding
//! one is answered infeasible before any backend sees the model.
vector<int> ChooseHighsColumnScales(const SolverModel &model) {
	vector<int> exponent(model.num_vars, 0);
	for (idx_t i = 0; i < model.num_vars; i++) {
		double lower = model.col_lower[i];
		double upper = model.col_upper[i];
		double bound =
		    std::max(lower >= HIGHS_INFINITE_BOUND ? lower : 0.0, upper <= -HIGHS_INFINITE_BOUND ? -upper : 0.0);
		if (bound == 0.0) {
			continue;
		}
		// Dividing a bound by 2^e is multiplying it by 2^-e, so the row rule's choice for
		// a set with no coefficients, negated, is the column's.
		ChooseHighsScale(std::numeric_limits<double>::infinity(), 0.0, bound, exponent[i]);
		exponent[i] = -exponent[i];
	}
	return exponent;
}

//! The value HiGHS's Hessian holds for the objective's k-th quadratic term before the
//! objective's own scale: the diagonal doubled (see the Hessian conversion in Load) and
//! both columns' scales applied.
double HighsHessianValue(const SolverModel &model, idx_t k, const vector<int> &column_exponent) {
	int row = model.q_rows[k];
	int col = model.q_cols[k];
	double value = row == col ? 2.0 * model.q_vals[k] : model.q_vals[k];
	return std::ldexp(value, column_exponent[row] + column_exponent[col]);
}

//! The row rule for the objective with each column scaled by `column_exponent` and linear coefficients `cost`: the
//! Hessian entries are the coefficients and the largest linear coefficient is the bound.
HighsScaleResult ChooseObjectiveScale(const SolverModel &model, const vector<int> &column_exponent,
                                      const std::vector<double> &cost, int &exponent) {
	double lo = std::numeric_limits<double>::infinity();
	double hi = 0.0;
	if (model.has_quadratic_obj) {
		// The model builder strips exact zeros from Q, and a column scale only multiplies.
		for (idx_t k = 0; k < model.q_vals.size(); k++) {
			double v = std::fabs(HighsHessianValue(model, k, column_exponent));
			lo = std::min(lo, v);
			hi = std::max(hi, v);
		}
	}
	double largest_cost = 0.0;
	for (auto c : cost) {
		largest_cost = std::max(largest_cost, std::fabs(c));
	}
	return ChooseHighsScale(lo, hi, largest_cost, exponent);
}

//! Choose the power of two the objective is multiplied by so its Hessian entries land inside
//! the matrix window and its linear coefficients under `infinite_cost`, and apply it to
//! `cost`. Multiplying an objective by a positive constant moves no optimum, and readback
//! divides the objective value back. An objective already inside both windows gets 0 and
//! loads as before; one that cannot be placed is refused, its cause judged as a row's is
//! (ThrowUnscalableRow): on the objective as written, before any column scale.
int ScaleObjectiveIntoHighsWindow(const SolverModel &model, const vector<int> &column_exponent,
                                  std::vector<double> &cost) {
	int exponent;
	if (ChooseObjectiveScale(model, column_exponent, cost, exponent) != HighsScaleResult::OK) {
		if (ChooseObjectiveScale(model, vector<int>(model.num_vars, 0), model.obj_coeffs, exponent) ==
		    HighsScaleResult::OK) {
			ThrowDecisionLimitRefusal("The objective");
		}
		throw InvalidInputException("The objective mixes numbers too far apart in size for HiGHS to load. Express "
		                            "the columns in the same unit, or install Gurobi, which loads this range.");
	}
	if (exponent != 0) {
		for (auto &c : cost) {
			c = std::ldexp(c, exponent);
		}
	}
	return exponent;
}

//! Resumable HiGHS handle. Load() builds the model into the `highs` object once
//! (a member, so it survives across Continue()); RunAndReadback() sets the per-chunk
//! time_limit and (re-)runs. HiGHS resumes its MIP search on a repeat run() after a
//! time-limit stop, so Continue() is just another RunAndReadback() with a fresh chunk.
class HighsSession : public SolverSession {
public:
	//! HiGHS asks its interrupt callbacks beside its `time_limit` checks in the MIP search, simplex and IPM. One
	//! callback answers all three from the poll, so a Ctrl-C stops the solve at the next check with status kInterrupt. The poll is read on every call, so an empty one (set at a continuation
	//! prompt) leaves the solve to run to its limit.
	void SetInterruptPoll(std::function<bool()> poll) override {
		should_interrupt = std::move(poll);
		highs.setCallback([this](int, const std::string &, const HighsCallbackOutput *, HighsCallbackInput *data_in,
		                         void *) { data_in->user_interrupt = should_interrupt && should_interrupt(); });
		highs.startCallback(kCallbackMipInterrupt);
		highs.startCallback(kCallbackSimplexInterrupt);
		highs.startCallback(kCallbackIpmInterrupt);
	}

protected:
    SolverResult SolveChunk(const SolverModel &model, double time_limit_seconds) override {
		SolverResult not_loaded;
		if (!Load(model, not_loaded)) {
			return not_loaded;
		}
		return RunAndReadback(time_limit_seconds);
    }

    SolverResult ContinueChunk(double time_limit_seconds) override {
        // Precondition: SolveChunk() already loaded the model into `highs`.
        return RunAndReadback(time_limit_seconds);
    }

private:
    Highs highs;
	//! Polled by the interrupt callback during run(); true stops the solve.
	std::function<bool()> should_interrupt;
    idx_t total_vars = 0;
    //! Whether the loaded model has any integer variable. HiGHS only populates
    //! `mip_dual_bound` / `mip_gap` for MIP solves; on an LP/QP timeout they hold
    //! defaults (0 / nan) that must not be read back as a proven bound.
    bool is_mip = false;
	//! The powers of two Load placed the model in HiGHS's window with (ChooseHighsColumnScales,
	//! ScaleObjectiveIntoHighsWindow), so readback can return the model's own values.
	vector<int> column_exponent;
	int objective_exponent = 0;

	//! Load `model` into `highs`. False when the model cannot be run, with `not_loaded` holding the solve's result:
	//! INFEASIBLE when an integer column's bounds hold no whole number, OTHER when HiGHS refuses the model.
	bool Load(const SolverModel &model, SolverResult &not_loaded);
	SolverResult RunAndReadback(double time_limit_seconds);
	//! The model's objective value from HiGHS's: the objective's scale divided out.
	double UnscaleObjective(double value) const {
		return std::ldexp(value, -objective_exponent);
	}
	//! The model's values of the first `total_vars` columns from HiGHS's: x = 2^e·x'.
	vector<double> UnscaleColumns(const std::vector<double> &values) {
		vector<double> result(total_vars);
		for (idx_t i = 0; i < total_vars; i++) {
			result[i] = std::ldexp(values[i], column_exponent[i]);
		}
		return result;
	}
};

bool HighsSession::Load(const SolverModel &model, SolverResult &not_loaded) {
    DecideProfileScope profile("highs.load");
    DecideProfileScope phase("highs.variable_types");
    total_vars = model.num_vars;

    //===--------------------------------------------------------------------===//
    // 1. Create HiGHS model and set up variables
    //===--------------------------------------------------------------------===//

    highs.setOptionValue("log_to_console", std::getenv("DECIDB_PROFILE_SOLVER_LOG") != nullptr);
	// The gap the user accepts (`decide_mip_gap`, default 0: the optimum, not HiGHS's default incumbent within 1e-4);
	// Gurobi's `MIPGap` is set the same way.
	highs.setOptionValue("mip_rel_gap", mip_gap);
	// The tolerances DeciDB defines (decide_numeric_policy.hpp), which the large-number warning reads too. A bound
	// within the integrality tolerance of a whole number is read as it (see the integer columns below).
	highs.setOptionValue("mip_feasibility_tolerance", DECIDE_INTEGRALITY_TOLERANCE);
	highs.setOptionValue("primal_feasibility_tolerance", DECIDE_FEASIBILITY_TOLERANCE);
	// Note: the time_limit is NOT set here — it is a per-chunk budget applied in
    // RunAndReadback(), so a warm Continue() can extend it without a reload.

    vector<HighsVarType> var_types(total_vars);
    is_mip = false;
    for (idx_t i = 0; i < total_vars; i++) {
        var_types[i] = model.is_integer[i] ? HighsVarType::kInteger : HighsVarType::kContinuous;
        is_mip = is_mip || model.is_integer[i];
    }

    ObjSense sense = model.maximize ? ObjSense::kMaximize : ObjSense::kMinimize;
    // Columns first: a column's scale multiplies its matrix entries, cost and Hessian
    // entries, which the row and objective scaling below then place.
    column_exponent = ChooseHighsColumnScales(model);

	//===--------------------------------------------------------------------===//
	// 2. The rows, straight into the row-wise matrix HiGHS takes
	//===--------------------------------------------------------------------===//

	// The model's rows are already row-wise, so they pack into HiGHS's compressed rows with no
	// coordinate list between, and `lp` moves into `passModel`: the matrix is held once.
	phase.Next("highs.pack_rows");
	HighsLp lp;
	auto &matrix = lp.a_matrix_;
	matrix.format_ = MatrixFormat::kRowwise;
	idx_t total_nnz = 0;
    for (auto &constr : model.constraints) {
        total_nnz += constr.indices.size();
    }
	matrix.start_.reserve(model.constraints.size() + 1);
	matrix.index_.reserve(total_nnz);
	matrix.value_.reserve(total_nnz);
	lp.row_lower_.reserve(model.constraints.size());
	lp.row_upper_.reserve(model.constraints.size());
	matrix.start_.assign(1, 0);
	for (auto &constr : model.constraints) {
        for (idx_t j = 0; j < constr.indices.size(); j++) {
			matrix.index_.push_back(constr.indices[j]);
			matrix.value_.push_back(std::ldexp(constr.coefficients[j], column_exponent[constr.indices[j]]));
		}
		matrix.start_.push_back(static_cast<HighsInt>(matrix.index_.size()));
		lp.row_lower_.push_back(constr.sense == '<' ? -DECIDE_SOLVER_INFINITY : constr.rhs);
		lp.row_upper_.push_back(constr.sense == '>' ? DECIDE_SOLVER_INFINITY : constr.rhs);
	}

	// Bring every row inside HiGHS's coefficient window. Each out-of-window row is multiplied
	// through by its own power of two, bounds included, which leaves the constraint — and
	// therefore the answer — exactly intact.
	phase.Next("highs.scale_rows");
	ScaleRowsIntoHighsWindow(model, matrix.start_, matrix.value_, lp.row_lower_, lp.row_upper_);

	//===--------------------------------------------------------------------===//
	// 3. The columns
	//===--------------------------------------------------------------------===//

	phase.Next("highs.pack_columns");
	lp.num_col_ = total_vars;
	lp.num_row_ = model.constraints.size();
	lp.sense_ = sense;
    lp.offset_ = 0.0;
    lp.col_lower_ = model.col_lower;
    lp.col_upper_ = model.col_upper;
    lp.col_cost_.resize(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        lp.col_cost_[i] = std::ldexp(model.obj_coeffs[i], column_exponent[i]);
        if (column_exponent[i] == 0) {
            continue;
        }
        // A scaled column's bound past its window is divided into it. Its other side is
        // open (a floor past `infinite_bound` leaves no finite ceiling above it), and stays
        // open: divided, the 1e30 sentinel could land inside the window as a real bound.
        lp.col_lower_[i] = DecideIsOpenBelow(model.col_lower[i]) ? -kHighsInf
                                                                 : std::ldexp(model.col_lower[i], -column_exponent[i]);
        lp.col_upper_[i] = DecideIsOpenAbove(model.col_upper[i]) ? kHighsInf
                                                                 : std::ldexp(model.col_upper[i], -column_exponent[i]);
    }
    objective_exponent = ScaleObjectiveIntoHighsWindow(model, column_exponent, lp.col_cost_);
    // HiGHS can answer below a fractional bound of an integer column whose other side is
    // open (`x >= -2.5` minimized to -3), so an integer column takes the whole bounds its
    // bounds imply. One left with no whole number cannot be loaded: HiGHS refuses an
    // inverted box.
    for (idx_t i = 0; i < total_vars; i++) {
        if (var_types[i] != HighsVarType::kInteger) {
            continue;
        }
        lp.col_lower_[i] = std::ceil(lp.col_lower_[i] - DECIDE_INTEGRALITY_TOLERANCE);
        lp.col_upper_[i] = std::floor(lp.col_upper_[i] + DECIDE_INTEGRALITY_TOLERANCE);
        if (lp.col_lower_[i] > lp.col_upper_[i]) {
			not_loaded.status = SolverStatus::INFEASIBLE;
			return false;
        }
    }
	lp.integrality_ = std::move(var_types);

	phase.Next("highs.pass_model");
	HighsStatus status = highs.passModel(std::move(lp));
	// `passModel` returns kWarning for conditions it expects the caller to proceed
	// through — a matrix entry dropped for being sub-tolerance, an inconsistent bound
	// pair left in place so the solve can deduce infeasibility from it. Treating those as
	// fatal turned an ordinary query into an internal error with a stack trace. Only a
	// kError stops here, and it is a solve HiGHS could not run, reported as OTHER exactly
	// as `RunAndReadback` reports a kError from `run()`, with no status text: HiGHS never
	// reached a model status.
	//
	// Neither window edge should arrive here at all: every row, column and the objective
	// is scaled into the window above, and the model builder strips exact zeros, so there
	// is nothing left below the threshold to drop or past the ceiling to refuse.
	// Proceeding is still the right response if one appears.
	if (status == HighsStatus::kError) {
		not_loaded.status = SolverStatus::OTHER;
		return false;
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
    phase.Next("highs.hessian");
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
        // nothing cancels its 1/2. HighsHessianValue doubles the diagonal on the way in and
        // leaves the rest.
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
            q_value[pos] = std::ldexp(HighsHessianValue(model, k, column_exponent), objective_exponent);
            current_pos[col]++;
        }

        status = highs.passHessian((HighsInt)total_vars, (HighsInt)num_nz,
                                   (HighsInt)HessianFormat::kTriangular,
                                   q_start.data(), q_index.data(), q_value.data());
		// Same kWarning/kError split as `passModel` above: `assessHessian` drops
		// sub-tolerance entries of the normalised Hessian and warns, which is a report
		// rather than a failure, and an entry past HiGHS's ceiling is a kError, reported as
		// `passModel`'s is. The objective scale chosen above keeps every entry inside both
		// edges, so neither should arrive here.
		if (status == HighsStatus::kError) {
			not_loaded.status = SolverStatus::OTHER;
			return false;
		}
    }
    return true;
}

SolverResult HighsSession::RunAndReadback(double time_limit_seconds) {
    DecideProfileScope profile("highs.run_and_readback");
    DecideProfileScope phase("highs.optimize");
    //===--------------------------------------------------------------------===//
    // 4. Solve
    //===--------------------------------------------------------------------===//

    // Apply the per-chunk wall-clock cap. Setting it here (rather than at Load) is
    // what lets a warm Continue() extend the budget without rebuilding: HiGHS
    // resumes its MIP search on a repeat run() after a time-limit stop.
    highs.setOptionValue("time_limit", time_limit_seconds);

    HighsStatus status = highs.run();
    phase.Next("highs.status_and_readback");
    if (DecideProfileScope::Enabled()) {
        const auto &info = highs.getInfo();
        DecideProfileScope::Counter("highs.simplex_iterations", info.simplex_iteration_count);
        DecideProfileScope::Counter("highs.ipm_iterations", info.ipm_iteration_count);
        DecideProfileScope::Counter("highs.qp_iterations", info.qp_iteration_count);
        DecideProfileScope::Counter("highs.mip_nodes", info.mip_node_count);
        if (std::isfinite(info.objective_function_value)) {
            DecideProfileScope::Counter("highs.objective", info.objective_function_value);
        }
    }
    // HiGHS returns kWarning (not kOk) at the time limit while still exposing a
    // valid model status + incumbent, so kWarning falls through to the
    // model-status switch below. A kError is a solve HiGHS could not finish (its
    // QP solver can fail to converge on a large model), not a DeciDB fault: it is
    // reported as OTHER, so the operator raises its ordinary solve-failed error,
    // or DIAGNOSE takes it, instead of an INTERNAL error that ends the session.
    if (status == HighsStatus::kError) {
        SolverResult result;
		result.status_text = highs.modelStatusToString(highs.getModelStatus());
		result.status = SolverStatus::OTHER;
        return result;
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
		result.status_text = highs.modelStatusToString(model_status);
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
		case HighsModelStatus::kInterrupt: // Ctrl-C: a stop-with-best-so-far like a time-limit stop, flagged as the
		                                   // user's, the same mapping as Gurobi's GRB_INTERRUPTED.
			result.user_interrupted = true;
			// fall through
        case HighsModelStatus::kTimeLimit: {
            result.status = SolverStatus::TIME_LIMIT;
            const HighsInfo& info = highs.getInfo();
            // A proven bound only exists for MIP timeouts: on an LP/QP timeout
            // `mip_dual_bound` still holds its 0 default, which would read back
            // as a confident (and wrong) "best possible objective". Keep the NaN
            // "unavailable" default there and for non-finite values.
            if (is_mip && std::isfinite(info.mip_dual_bound) &&
                std::fabs(info.mip_dual_bound) < DECIDE_EFFECTIVE_INFINITY) {
                result.best_bound = UnscaleObjective(info.mip_dual_bound);
            }
            // Incumbent reads (objective / gap / solution vector) are only valid
            // when HiGHS found a feasible solution; otherwise objective_function_value
            // is inf, mip_gap is nan, and col_value is garbage — so gate on the
            // primal-solution status. `mip_gap` additionally needs a MIP bound to
            // be meaningful (an LP feasible-at-timeout would read back nan).
            if (info.primal_solution_status == kSolutionStatusFeasible) {
                result.has_solution = true;
                result.objective_value = UnscaleObjective(info.objective_function_value);
                if (is_mip && std::isfinite(info.mip_gap) && info.mip_gap < DECIDE_EFFECTIVE_INFINITY) {
                    result.gap = info.mip_gap;
                }
                const HighsSolution& incumbent = highs.getSolution();
                if (incumbent.col_value.size() >= total_vars) {
                    result.solution = UnscaleColumns(incumbent.col_value);
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

    for (idx_t i = 0; i < total_vars; i++) {
        if (!std::isfinite(solution.col_value[i])) {
            throw InternalException(
                "HiGHS returned invalid solution value (NaN or Infinity) for variable %llu", i);
        }
    }

    SolverResult solve_result;
    solve_result.status = SolverStatus::OPTIMAL;
    solve_result.solution = UnscaleColumns(solution.col_value);
    // Objective value at the optimum, in the model's own sense (the diagnostics
    // stage-2 re-solve reads this as the achievable objective).
    solve_result.objective_value = UnscaleObjective(highs.getInfo().objective_function_value);
    return solve_result;
}

} // namespace

unique_ptr<SolverSession> DeterministicNaive::CreateSession() {
    return make_uniq<HighsSession>();
}

} // namespace duckdb
