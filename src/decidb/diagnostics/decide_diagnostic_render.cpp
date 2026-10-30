//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/diagnostics/decide_diagnostic_render.cpp
//
// Clause rendering for DECIDE diagnostics. See decide_diagnostic_render.hpp.
//
//===----------------------------------------------------------------------===//

#include "duckdb/decidb/diagnostics/decide_diagnostic_render.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/decidb/diagnostics/diagnostic_constants.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace duckdb {
namespace decide_render {

//! Compact numeric formatting for user-facing constraint text (drops trailing
//! zeros: 12.5 not 12.500000, 10 not 10.0). Prints the 15 significant digits every
//! DOUBLE holds faithfully (DBL_DIG): fewer would round a moved bound to nearest and can
//! print it short of the amount it moved by (`x <= 100000` loosened by 0.333334 would read
//! `x <= 100000.3333`); more would print the binary noise of the arithmetic.
string FormatNum(double v) {
	if (v == 0.0) {
		return "0"; // normalize -0.0 (a signed-zero solver read) to a clean "0"
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%.15g", v);
	return string(buf);
}

//! Clean the sub-tolerance noise of a solver-read value that is no edit (the achievable
//! objective, a repair cost two candidates are compared by): a clean `10` would otherwise
//! print as `10.000001`. Snap to the nearest integer when within that noise: the feasibility
//! tolerance DeciDB sets, which a solved point may miss each row by, plus a relative part for
//! the backend's floating-point arithmetic at large magnitudes (`1234567889.0001` →
//! `1234567889`, which significant-figure rounding would have mangled to `1234570000`). That
//! arithmetic grows with the value and the number of terms summed into it, so no tolerance
//! DeciDB sets bounds it. Otherwise trim a genuinely fractional value to a fixed absolute
//! precision. An edit's amount never comes here (SnapToPrecision).
double SnapDiagnosticValue(double v) {
	if (v == 0.0 || !std::isfinite(v)) {
		return v;
	}
	double nearest_int = std::round(v);
	if (std::fabs(v - nearest_int) <= DECIDE_FEASIBILITY_TOLERANCE + 1e-9 * std::fabs(v)) {
		return nearest_int;
	}
	return std::round(v * 1e6) / 1e6;
}

//! Round a repair amount to the precision the elastic model can actually resolve, in
//! `direction`.
//!
//! Formulating against a widened box costs conditioning: a slack whose exact value is
//! `4.5` comes back as `4.499999`. Round to the last decimal that is meaningful at this
//! value's OWN magnitude. Relative, never absolute: an edit of 2 and an edit of 30000 in
//! the same model are each trustworthy to about six figures of themselves, and a grid
//! taken from the model's largest number would erase the small one entirely. Never
//! coarser than a whole number: `12345678.90123` → `12345679`, and a whole number keeps
//! every digit, which a user can read as data (`1234567889`).
//!
//! NEAREST is the tidy reading (`4.499999` → `4.5`, a backend's `7.00000028` → `7`), which
//! can fall short of what the solver found. OUTWARD never does (`4.3333333` → `4.33334`),
//! so it is a repair whenever the solver's amount was. Which one a diagnosis prints is
//! decided by a solve, not here (the certificate in decide_diagnostic_engines.cpp).
double SnapToPrecision(double v, RoundDirection direction) {
	if (v == 0.0 || !std::isfinite(v)) {
		return v;
	}
	double mag = std::fabs(v);
	double decimals = std::max(0.0, std::floor(-std::log10(1e-6 * mag)));
	if (decimals > 15.0) {
		return v;
	}
	double scale = std::pow(10.0, decimals);
	double ticks = std::round(mag * scale);
	if (direction == RoundDirection::OUTWARD && ticks / scale < mag) {
		ticks += 1.0;
	}
	double snapped = ticks / scale;
	return v < 0.0 ? -snapped : snapped;
}

//! User-facing name of a flat solver column: its provenance label (user variable
//! name or aux source expression), falling back to a positional name.
static string ColLabel(const vector<ColumnProvenance> &cols, int col) {
	if (col >= 0 && static_cast<idx_t>(col) < cols.size() && !cols[col].label.empty()) {
		return cols[col].label;
	}
	return "col" + std::to_string(col);
}

//! Reconstruct a linear combination as algebra over user-facing column names
//! (e.g. "x", "2*x + 3*y"). Coefficient ±1 is elided to keep the rendering close
//! to what the user wrote.
static string FormatTerms(const vector<int> &indices, const vector<double> &coeffs,
                   const vector<ColumnProvenance> &cols) {
	string out;
	for (idx_t i = 0; i < indices.size(); i++) {
		double c = coeffs[i];
		string var = ColLabel(cols, indices[i]);
		if (out.empty()) {
			if (c == 1.0) {
				out += var;
			} else if (c == -1.0) {
				out += "-" + var;
			} else {
				out += FormatNum(c) + "*" + var;
			}
		} else {
			double ac = std::fabs(c);
			out += (c < 0 ? " - " : " + ");
			out += (ac == 1.0) ? var : (FormatNum(ac) + "*" + var);
		}
	}
	return out.empty() ? "0" : out;
}

//! Terms of the same decide variable spread across several solver columns (an
//! aggregate fan-out, or AVG's pre-scaled per-row copies) collapsed to one entry each,
//! in first-seen order. `uniform[key]` is false when the stored coefficient differs
//! across that variable's terms — a data-varying coefficient neither caller can quote
//! as a single literal.
struct TermGrouping {
	vector<idx_t> order; // distinct variable keys, first-seen order
	std::map<idx_t, double> coeff_of;
	std::map<idx_t, idx_t> count_of;
	std::map<idx_t, bool> uniform;
	std::map<idx_t, string> label_of;
};

//! `merge_auxiliaries` decides what happens to a column with no decide variable:
//! `FormatAvgLhs` collapses them all onto one shared key (an AVG lhs has no fan-out to
//! preserve across them), `FormatSumLhs` gives each its own key so they stay separate
//! `SUM(...)` terms.
static TermGrouping GroupTermsByVariable(const vector<int> &indices, const vector<double> &coeffs,
                                  const vector<ColumnProvenance> &cols, bool merge_auxiliaries) {
	TermGrouping g;
	for (idx_t i = 0; i < indices.size(); i++) {
		int col = indices[i];
		idx_t var_key = (col >= 0 && static_cast<idx_t>(col) < cols.size())
		                    ? cols[col].decide_var_idx
		                    : DConstants::INVALID_INDEX;
		idx_t key = (merge_auxiliaries || var_key != DConstants::INVALID_INDEX)
		                ? var_key
		                : DConstants::INVALID_INDEX - 1 - i;
		double c = coeffs[i];
		auto it = g.coeff_of.find(key);
		if (it == g.coeff_of.end()) {
			g.coeff_of[key] = c;
			g.count_of[key] = 1;
			g.uniform[key] = true;
			g.label_of[key] = ColLabel(cols, col);
			g.order.push_back(key);
		} else {
			if (std::fabs(it->second - c) > 1e-12) {
				g.uniform[key] = false;
			}
			g.count_of[key]++;
		}
	}
	return g;
}

//! AVG rendering: an AVG→SUM-rewritten row stores coefficients pre-scaled by 1/N_g
//! (`provenance.avg_scaled`), so a plain reconstruction would read `0.5*x + 0.5*x`.
//! Recover `AVG(<inner>)` by collapsing the N equal-coefficient terms of each
//! variable back to one (inner coeff = stored coeff × term count = the user's
//! coefficient). Returns false (caller falls back to the raw reconstruction) for a
//! data-varying coefficient (`AVG(price * x)`), where no single literal coefficient
//! exists to re-quote.
static bool FormatAvgLhs(const vector<int> &indices, const vector<double> &coeffs,
                  const vector<ColumnProvenance> &cols, string &out) {
	TermGrouping g = GroupTermsByVariable(indices, coeffs, cols, /*merge_auxiliaries=*/true);
	const auto &order = g.order;
	auto &coeff_of = g.coeff_of;
	auto &count_of = g.count_of;
	auto &uniform = g.uniform;
	auto &label_of = g.label_of;
	vector<int> inner_idx;
	vector<double> inner_coeff;
	for (idx_t k = 0; k < order.size(); k++) {
		idx_t key = order[k];
		if (!uniform[key]) {
			return false; // data-varying coefficient: cannot render a clean AVG(...)
		}
		// Reuse FormatTerms by feeding it a synthetic single column per variable;
		// carry the label through a positional column index into a local table.
		inner_idx.push_back(static_cast<int>(k));
		inner_coeff.push_back(coeff_of[key] * static_cast<double>(count_of[key]));
	}
	vector<ColumnProvenance> inner_cols(order.size());
	for (idx_t k = 0; k < order.size(); k++) {
		inner_cols[k].label = label_of[order[k]];
	}
	out = "AVG(" + FormatTerms(inner_idx, inner_coeff, inner_cols) + ")";
	return true;
}

//! SUM rendering: an aggregate SUM over rows emits one solver column per row for the
//! same decide variable, so a plain reconstruction reads `x + x + x`. Collapse the
//! per-row fan-out back to `SUM(...)`: group terms by decide variable and render any
//! variable contributing more than one term as `SUM(c*var)` (a variable that appears
//! once — e.g. a global decide var added to the aggregate — stays as written). A summed
//! variable with data-varying coefficients (`SUM(buy * l_extendedprice)`: no single
//! literal to quote) renders symbolically as `SUM(var * weight_label)` when the clause
//! carried a coefficient label through `weight_labels`; without a label it returns false
//! (caller falls back to the raw reconstruction), as does a row with no aggregate fan.
static bool FormatSumLhs(const vector<int> &indices, const vector<double> &coeffs,
                  const vector<ColumnProvenance> &cols,
                  const vector<std::pair<idx_t, string>> &weight_labels, string &out,
                  const char *agg_name = "SUM") {
	// A column with no decide variable (an auxiliary column) never merges with another
	// — give it a unique key so each stays its own term.
	TermGrouping g = GroupTermsByVariable(indices, coeffs, cols, /*merge_auxiliaries=*/false);
	const auto &order = g.order;
	auto &coeff_of = g.coeff_of;
	auto &count_of = g.count_of;
	auto &uniform = g.uniform;
	auto &label_of = g.label_of;
	// A data-varying summed variable has no single literal to quote, but if the clause
	// carried a symbolic coefficient label (`buy → l_extendedprice`) we can still render
	// it as `SUM(var * label)`. Look up the label per decide variable.
	std::map<idx_t, string> label_for;
	for (const auto &wl : weight_labels) {
		label_for[wl.first] = wl.second;
	}
	bool any_fan = false;
	for (idx_t key : order) {
		if (count_of[key] > 1) {
			any_fan = true;
			if (!uniform[key] && label_for.find(key) == label_for.end()) {
				return false; // data-varying coefficient, no label: fall back to raw
			}
		}
	}
	if (!any_fan) {
		return false; // not an aggregate fan: render normally
	}
	string s;
	for (idx_t key : order) {
		double c = coeff_of[key];
		double ac = std::fabs(c);
		bool data_varying = count_of[key] > 1 && !uniform[key];
		string term;
		if (data_varying) {
			// `SUM(buy * l_extendedprice)` — quote the variable × its coefficient column.
			// `agg_name` is "AVG" for an AVG-rewritten row whose data-varying coefficient
			// blocked the clean FormatAvgLhs path — keep the aggregate the user wrote
			// instead of mislabeling it "SUM".
			term = string(agg_name) + "(" + label_of[key] + " * " + label_for[key] + ")";
		} else {
			term = (ac == 1.0) ? label_of[key] : (FormatNum(ac) + "*" + label_of[key]);
			if (count_of[key] > 1) {
				term = string(agg_name) + "(" + term + ")";
			}
		}
		// A data-varying weight carries its sign inside the column, so render it additively;
		// a uniform coefficient keeps its literal sign.
		bool neg = !data_varying && c < 0;
		if (s.empty()) {
			s += (neg ? "-" : "") + term;
		} else {
			s += (neg ? " - " : " + ") + term;
		}
	}
	out = s;
	return true;
}

//! Aggregate LHS for a row built on the *accumulating* path, where each matrix coefficient
//! is a sum over folded rows rather than the coefficient the user wrote. A keyed
//! variable collapses every joined row of a class onto one column, so `SUM(keepS)` over
//! two rows of a sensor reaches the matrix as `2*keepS` — there is no fan-out for
//! `FormatSumLhs` to key on, and the accumulated constant is not quotable. Render straight
//! from the per-term coefficients the builder recorded (`ConstraintProvenance::folded_terms`)
//! so the label reads as written: `SUM(keepS)`, `SUM(3*keepS)`, `SUM(keepS * price)`.
//! Returns false if any column of the row is not covered by a recorded term, or if a
//! data-varying term has no symbolic name — the caller then falls back to the raw
//! reconstruction, i.e. to today's behaviour.
static bool FormatFoldedSumLhs(const vector<int> &indices, const vector<FoldedAggTerm> &folded,
                        const vector<ColumnProvenance> &cols,
                        const vector<std::pair<idx_t, string>> &weight_labels, string &out,
                        const char *agg_name) {
	std::map<idx_t, const FoldedAggTerm *> term_of;
	for (const auto &t : folded) {
		term_of[t.decide_var_idx] = &t;
	}
	std::map<idx_t, string> label_for;
	for (const auto &wl : weight_labels) {
		label_for[wl.first] = wl.second;
	}

	vector<idx_t> order; // distinct decide variables, first-seen order
	std::map<idx_t, string> name_of;
	for (int col : indices) {
		// An auxiliary column carries no user-written term, so the row is not fully
		// reconstructible from `folded_terms` — bail rather than quote half of it.
		if (col < 0 || static_cast<idx_t>(col) >= cols.size() ||
		    cols[col].decide_var_idx == DConstants::INVALID_INDEX) {
			return false;
		}
		idx_t v = cols[col].decide_var_idx;
		if (term_of.find(v) == term_of.end()) {
			return false;
		}
		if (name_of.find(v) == name_of.end()) {
			name_of[v] = ColLabel(cols, col);
			order.push_back(v);
		}
	}
	if (order.empty()) {
		return false;
	}

	string s;
	for (idx_t v : order) {
		const auto &t = *term_of[v];
		string term;
		bool neg = false;
		if (t.has_unit) {
			double ac = std::fabs(t.unit);
			term = (ac == 1.0) ? name_of[v] : (FormatNum(ac) + "*" + name_of[v]);
			neg = t.unit < 0;
		} else {
			auto it = label_for.find(v);
			if (it == label_for.end()) {
				return false; // data-varying with no symbolic name to quote
			}
			// A data-varying weight carries its sign per row, so render it additively.
			term = name_of[v] + " * " + it->second;
		}
		term = string(agg_name) + "(" + term + ")";
		if (s.empty()) {
			s += (neg ? "-" : "") + term;
		} else {
			s += (neg ? " - " : " + ") + term;
		}
	}
	out = s;
	return true;
}

//! True if some decide variable contributes more than one term (an aggregate fan-out over
//! multiple solver columns). Lets a single-contribution aggregate group be wrapped in
//! SUM(...) (Facet B) without disturbing a multi-row data-varying fan, which stays as its
//! raw reconstruction.
static bool HasVarFan(const vector<int> &indices, const vector<ColumnProvenance> &cols) {
	std::map<idx_t, int> count;
	for (int col : indices) {
		if (col >= 0 && static_cast<idx_t>(col) < cols.size() &&
		    cols[col].decide_var_idx != DConstants::INVALID_INDEX) {
			if (++count[cols[col].decide_var_idx] > 1) {
				return true;
			}
		}
	}
	return false;
}

//! Aggregate-aware LHS for a linear constraint row: AVG(...) for an AVG-rewritten row,
//! SUM(...) for a SUM fan-out, else the raw linear reconstruction.
static string FormatLhs(const ModelConstraint &row, const vector<ColumnProvenance> &cols) {
	string agg;
	if (row.provenance.avg_scaled && FormatAvgLhs(row.indices, row.coefficients, cols, agg)) {
		return agg;
	}
	// An AVG-rewritten row whose coefficient is data-varying can't produce a clean
	// FormatAvgLhs, but it must still render as AVG(...) — otherwise the diagnosis and its
	// suggested edit mislabel the user's AVG constraint as SUM (the value is already in AVG
	// units, so the SUM label makes the suggestion a different, wrong constraint).
	const char *agg_name = row.provenance.avg_scaled ? "AVG" : "SUM";
	// Collapse a uniform SUM fan-out to SUM(c*var). Grouped aggregates fold too: they
	// stay distinguishable in the relation via the finding's `group` column + `when c per K:` qualifier
	// (Facet A), so the old group_key == INVALID gate is gone.
	// A row off the accumulating build path carries per-class *totals*, not the coefficients
	// the user wrote, so every matrix-inference path below misreads it — either as a literal
	// the user never typed (`SUM(2*keepS)`) or, when the totals differ across classes, as a
	// data-varying weight (`SUM(keepS * 1)`). Render from the recorded per-term coefficients
	// first. `folded_terms` is non-empty only for accumulated rows, so a row-scoped fan-out
	// never diverts here; and the call declines whenever it cannot reconstruct the whole row,
	// leaving the paths below to handle it exactly as before.
	if (!row.provenance.avg_scaled && !row.provenance.folded_terms.empty() &&
	    FormatFoldedSumLhs(row.indices, row.provenance.folded_terms, cols, row.provenance.weight_labels, agg,
	                       agg_name)) {
		return agg;
	}
	if (FormatSumLhs(row.indices, row.coefficients, cols, row.provenance.weight_labels, agg, agg_name)) {
		return agg;
	}
	string raw = FormatTerms(row.indices, row.coefficients, cols);
	// Facet B: a single-row / WHEN aggregate group has no per-row fan-out for FormatSumLhs
	// to fold, so wrap the reconstruction in SUM(...) explicitly. Only when there is no fan
	// — a multi-row data-varying SUM keeps its raw `2*x + 3*x` form (documented I2.d).
	if (row.provenance.is_aggregate && !HasVarFan(row.indices, cols)) {
		return string(agg_name) + "(" + raw + ")";
	}
	return raw;
}

//! LHS for a quadratic constraint: the linear part plus its Q terms, rendered as
//! `POWER(x, 2)` (diagonal) or `x*y` (off-diagonal). Best-effort labelling; the
//! slack only ever touches the linear RHS (I2.d).
static string FormatQuadraticLhs(const SolverModel::QuadraticConstraint &qc,
                          const vector<ColumnProvenance> &cols) {
	string out = FormatTerms(qc.linear_indices, qc.linear_coefficients, cols);
	if (out == "0") {
		out.clear();
	}
	for (idx_t i = 0; i < qc.q_coefficients.size(); i++) {
		double c = qc.q_coefficients[i];
		string term;
		if (qc.q_rows[i] == qc.q_cols[i]) {
			term = "POWER(" + ColLabel(cols, qc.q_rows[i]) + ", 2)";
		} else {
			term = ColLabel(cols, qc.q_rows[i]) + "*" + ColLabel(cols, qc.q_cols[i]);
		}
		double ac = std::fabs(c);
		if (out.empty()) {
			out += (c < 0 ? "-" : "") + ((ac == 1.0) ? term : (FormatNum(ac) + "*" + term));
		} else {
			out += (c < 0 ? " - " : " + ");
			out += (ac == 1.0) ? term : (FormatNum(ac) + "*" + term);
		}
	}
	return out.empty() ? "0" : out;
}

const ConstraintSourceInfo *FindConstraintSource(const SolverModel &model, const ConstraintProvenance &provenance) {
	auto source_id = provenance.source_clause_id;
	if (source_id == DConstants::INVALID_INDEX || source_id >= model.constraint_sources.size()) {
		return nullptr;
	}
	return &model.constraint_sources[source_id];
}

string SourceAwareLhs(const SolverModel &model, const ModelConstraint &row,
	                  const vector<ColumnProvenance> &columns) {
	auto source = FindConstraintSource(model, row.provenance);
	if (!source) {
		return FormatLhs(row, columns);
	}
	// The written spelling wins when canonicalization moved a term across the
	// comparison, so a diagnosis names a clause the user can find in their query
	// rather than the algebra stage 04 produced. Only set when the two forms differ.
	if (!source->source_lhs.empty()) {
		return source->source_lhs;
	}
	return !source->canonical_lhs.empty() ? source->canonical_lhs : FormatLhs(row, columns);
}

//! The bound to quote beside SourceAwareLhs. When the written spelling is in play the
//! two must come from the SAME form -- pairing a written LHS with a canonical bound
//! would read as a clause that is neither. Empty when there is no written spelling to
//! honour, which leaves every existing caller on its current path.
string SourceWrittenRhs(const SolverModel &model, const ConstraintProvenance &provenance) {
	auto source = FindConstraintSource(model, provenance);
	return source ? source->source_rhs : string();
}

string SourceAwareQuadraticLhs(const SolverModel &model, const SolverModel::QuadraticConstraint &row,
	                           const vector<ColumnProvenance> &columns) {
	auto source = FindConstraintSource(model, row.provenance);
	return source && !source->canonical_lhs.empty() ? source->canonical_lhs : FormatQuadraticLhs(row, columns);
}

string SourceAwareRhs(const SolverModel &model, const ConstraintProvenance &provenance, double fallback) {
	auto source = FindConstraintSource(model, provenance);
	if (source && source->rhs_kind == ConstraintSourceRhsKind::DATA_EXPRESSION &&
	    !source->canonical_rhs.empty()) {
		return source->canonical_rhs;
	}
	return provenance.rhs_label.empty() ? FormatNum(fallback) : provenance.rhs_label;
}

static const char *SenseStr(char sense) {
	return sense == '<' ? "<=" : (sense == '>' ? ">=" : "=");
}

//! The clause as the user wrote it: `lhs sense rhs`, with the `when c per K:` qualifier
//! in front (Facet C) so it stays recognizable. For a strict `<` / `>` the δ offset was
//! baked into `rhs` at build time, so quote the user's typed literal (`typed_k`) and
//! render `<` / `>` rather than the model's `<=` / `>=` (I2.d).
//! The number to quote for a row whose own RHS is not what the user typed. Two things
//! can shift it, and they never apply at once: the binder bakes a δ into a strict
//! `<` / `>` (re-quote `typed_k`), and a lowering adds a mechanism term to build a Big-M
//! row (subtract it back off). Everything the reader sees goes through here, so a clause
//! reads the same whichever rewrite the query happened to take.
static double DisplayRhs(const ConstraintProvenance &prov, double rhs, char sense) {
	if (prov.strict && sense != '=') {
		return prov.typed_k;
	}
	return rhs - prov.rhs_mechanism_offset;
}

static string MakeClauseLabel(const ConstraintProvenance &prov, const ConstraintSourceInfo *source, const string &lhs,
                              double rhs, char sense) {
	bool strict = prov.strict && sense != '=';
	string sense_str = strict ? (sense == '>' ? ">" : "<") : SenseStr(sense);
	double base_rhs = DisplayRhs(prov, rhs, sense);
	return QuoteEditedClause(source, lhs, sense_str, FormatNum(base_rhs));
}

//! The left side a half is edited and quoted with.
static const string &HalfLhs(const ConstraintSourceInfo &source) {
	return source.source_lhs.empty() ? source.canonical_lhs : source.source_lhs;
}

//! The bound of a half as the clause states it, for the end of a range an edit leaves where it is.
static const string &HalfBound(const ConstraintSourceInfo &source) {
	return source.source_rhs.empty() ? source.canonical_rhs : source.source_rhs;
}

bool IsRangeEnd(const SolverModel &model, idx_t source_clause_id, char sense) {
	if (source_clause_id >= model.constraint_sources.size()) {
		return false;
	}
	auto &source = model.constraint_sources[source_clause_id];
	if (source.between_partner == DConstants::INVALID_INDEX) {
		return source.written_cmp == "=" && sense != '=';
	}
	// The halves read as one range when they share a left side and each bounds the side its half names.
	return HalfLhs(source) == HalfLhs(model.constraint_sources[source.between_partner]) &&
	       sense == (source.between_upper ? '<' : '>');
}

vector<ClauseEdit> SpellRangeEdits(vector<ClauseEdit> edits, const SolverModel &model) {
	//! A range clause's edits so far: where the merged edit sits, the clause, and the text of each end.
	struct Range {
		idx_t at;
		idx_t root;
		string lhs;
		string lower;
		string upper;
	};
	vector<ClauseEdit> out;
	vector<Range> ranges;
	for (auto &edit : edits) {
		auto id = edit.moved.source_clause_id;
		if (edit.kind != ClauseEditKind::LOOSEN || id == DConstants::INVALID_INDEX ||
		    !IsRangeEnd(model, id, edit.moved.sense)) {
			out.push_back(std::move(edit));
			continue;
		}
		auto moved = edit.moved;
		auto &source = model.constraint_sources[id];
		// The clause is named by its lower half; an `=` is its own.
		idx_t root = source.between_upper ? source.between_partner : id;
		idx_t found = DConstants::INVALID_INDEX;
		for (idx_t r = 0; r < ranges.size(); r++) {
			if (ranges[r].root == root && out[ranges[r].at].group == edit.group && out[ranges[r].at].row == edit.row) {
				found = r;
			}
		}
		if (found == DConstants::INVALID_INDEX) {
			auto &lower = model.constraint_sources[root];
			auto &upper = source.between_partner == DConstants::INVALID_INDEX
			                  ? lower
			                  : model.constraint_sources[lower.between_partner];
			ranges.push_back({out.size(), root, edit.moved.lhs, HalfBound(lower), HalfBound(upper)});
			found = ranges.size() - 1;
			edit.label = QuoteWrittenClause(source);
			out.push_back(std::move(edit));
		} else {
			out[ranges[found].at].amount += edit.amount;
		}
		(moved.sense == '<' ? ranges[found].upper : ranges[found].lower) = moved.bound;
	}
	for (auto &range : ranges) {
		auto &edit = out[range.at];
		edit.suggestion = QuoteEditedClause(&model.constraint_sources[range.root], range.lhs, "BETWEEN",
		                                    range.lower + " AND " + range.upper);
	}
	return out;
}

//! Printable key of the group `name` names, empty for none: the per-key-column representative values joined with ", "
//! (composite key → `EU, 2024`). A column the user wrote renders its value, "NULL" for a NULL; a value of a column a
//! relation element stands for (`per D`) is an SQL literal, as a determination message writes it (`'EU'`, `NULL`),
//! because there NULL is a value a string `'NULL'` must be told from.
static string FormatPerGroupKey(const SolverModel &model, const GroupName &name) {
	string s;
	if (name.key_values == DConstants::INVALID_INDEX) {
		return s;
	}
	auto &key = model.key_values[name.key_values];
	for (idx_t c = 0; c < key.values.size(); c++) {
		const Value &v = key.values[c][name.group];
		if (!s.empty()) {
			s += ", ";
		}
		if (key.null_modes[c] == DecideKeyNullMode::VALUE) {
			s += DecideSqlLiteral(v);
		} else if (v.IsNull()) {
			s += "NULL";
		} else {
			string rendered = v.ToString();
			s += rendered.empty() ? "''" : rendered;
		}
	}
	return s;
}

//! Build a LOOSEN edit from a constraint's rendered LHS + sense + RHS and the
//! solved slack `amount` (signed; for `=` it is the net s⁺−s⁻). For a strict `<` /
//! `>` the δ offset was baked into `rhs` at build time, so re-quote the suggestion
//! against the user's typed literal (`typed_k`) and render `<` / `>` (I2.d).
ClauseEdit MakeLoosenEdit(const SolverModel &model, const ConstraintProvenance &prov,
                          const ConstraintSourceInfo *source, const string &lhs, double rhs, char sense,
                          double amount) {
	bool strict = prov.strict && sense != '=';
	string sense_str = strict ? (sense == '>' ? ">" : "<") : SenseStr(sense);
	double base_rhs = DisplayRhs(prov, rhs, sense);
	// `≥` loosens downward (b − s), `≤` / `=` upward (b + s).
	double new_rhs = (sense == '>') ? base_rhs - amount : base_rhs + amount;
	// Facet C: attach the `when c per K:` qualifier so the clause is recognizable;
	// Facet A: carry the group's printable key as its own field (a separate `group` EAV
	// row) so two folded `SUM(x)` groups stay distinguishable in the relation.
	ClauseEdit e;
	e.kind = ClauseEditKind::LOOSEN;
	e.label = MakeClauseLabel(prov, source, lhs, rhs, sense);
	e.suggestion = QuoteEditedClause(source, lhs, sense_str, FormatNum(new_rhs));
	e.has_amount = true;
	e.amount = std::fabs(amount);
	e.group = FormatPerGroupKey(model, prov.group_name);
	e.moved = {prov.source_clause_id, sense, lhs, FormatNum(new_rhs)};
	return e;
}

vector<ClauseEdit> FanOutByClass(ClauseEdit edit, const ConstraintProvenance &prov, const SolverModel &model,
                                 bool expanded) {
	using Kind = SolverInput::ClassSet::Kind;
	vector<ClauseEdit> out;
	if (prov.class_set >= model.class_sets.size()) {
		if (expanded && prov.per_kept_row) {
			edit.group.clear();
			edit.row = prov.data_row;
			edit.edit_source = "expanded_row";
		}
		out.push_back(std::move(edit));
		return out;
	}
	auto &set = model.class_sets[prov.class_set];
	if (set.kind == Kind::KEY_VALUES) {
		for (auto &name : set.classes) {
			ClauseEdit per_class = edit;
			per_class.group = FormatPerGroupKey(model, name);
			per_class.row = DConstants::INVALID_INDEX;
			if (expanded) {
				per_class.edit_source = "expanded_group";
			}
			out.push_back(std::move(per_class));
		}
		return out;
	}
	if (set.kind == Kind::KEPT_ROWS && expanded) {
		for (auto row : set.rows) {
			ClauseEdit per_row = edit;
			per_row.group.clear();
			per_row.row = row;
			per_row.edit_source = "expanded_row";
			out.push_back(std::move(per_row));
		}
		return out;
	}
	edit.group.clear();
	out.push_back(std::move(edit));
	return out;
}

//! Name every user clause whose bound is out of reach, before any solve. A clause fans
//! into one row per relation row, all rendering the same text, so entries are
//! de-duplicated on (label, group, row) — the user wrote one clause and reads one
//! finding, or one per class it names (FanOutByClass).
//!
//! The gate is "does this row trace back to a clause the user wrote", not "is it
//! elastically editable". They are different questions, and a hard MIN/MAX clause is
//! stamped USER_MECHANISM wholesale (it *will* fan into Big-M rows), so a relaxability
//! gate would miss `MIN(x) <= -inf` — the shape the report opened with. Only STRUCTURAL
//! rows are excluded, as internal definitions with no user text to quote.
//!
//! A USER_MECHANISM row is safe to read here because its bound is derived as `K ± M`
//! with both terms finite by construction: ClassifyMinMaxBound sends a group to the
//! Big-M rewrite only when `K` is finite, and DecideTightPerRowBigM refuses a non-finite
//! `M`. So an infinite bound on a mechanism row is never a helper's own bound — it is a
//! user bound re-emitted per row, which is exactly what we want to name.
vector<UnreachableClause> CollectUnreachableClauses(const SolverModel &model, const vector<ColumnProvenance> &columns,
                                                    bool expanded) {
	vector<UnreachableClause> found;
	std::set<std::tuple<string, string, idx_t>> seen;
	auto record = [&](const ConstraintProvenance &prov, const string &lhs, double rhs, char sense) {
		ClauseEdit base;
		base.label = IsRangeEnd(model, prov.source_clause_id, sense)
		                 ? QuoteWrittenClause(*FindConstraintSource(model, prov))
		                 : MakeClauseLabel(prov, FindConstraintSource(model, prov), lhs, rhs, sense);
		base.group = FormatPerGroupKey(model, prov.group_name);
		for (auto &finding : FanOutByClass(std::move(base), prov, model, expanded)) {
			UnreachableClause c;
			c.label = finding.label;
			c.group = finding.group;
			c.row = finding.row;
			c.too_low = rhs < 0.0;
			if (seen.insert(std::make_tuple(c.label, c.group, c.row)).second) {
				found.push_back(std::move(c));
			}
		}
	};
	for (const auto &row : model.constraints) {
		if (row.provenance.kind == ConstraintKind::STRUCTURAL ||
		    row.provenance.removal_group_id != DConstants::INVALID_INDEX ||
		    !IsUnreachableBound(row.sense, row.rhs)) {
			continue;
		}
		record(row.provenance, SourceAwareLhs(model, row, columns), row.rhs, row.sense);
	}
	for (const auto &row : model.quadratic_constraints) {
		if (row.provenance.kind == ConstraintKind::STRUCTURAL ||
		    row.provenance.removal_group_id != DConstants::INVALID_INDEX ||
		    !IsUnreachableBound(row.sense, row.rhs)) {
			continue;
		}
		record(row.provenance, SourceAwareQuadraticLhs(model, row, columns), row.rhs, row.sense);
	}
	return found;
}

//! Build a query-mode virtual-offset LOOSEN edit for a data-backed RHS clause
//! (`x <= col`): the clause's folded shared slack `delta` becomes one synthetic
//! query-level offset `x <= col + delta` (`>=` loosens downward → `col - delta`; `=`
//! moves by its signed net slack). A strict `<` / `>` keeps its operator: its row was
//! stepped onto the lattice, and the written bound moves by the same amount.
//! `rhs_text` is the RHS column name (`prov.rhs_label`) or a numeric fallback when no
//! name is available. `edit_source` is stamped by the caller.
ClauseEdit MakeVirtualOffsetEdit(const SolverModel &model, const ConstraintProvenance &prov,
                                 const ConstraintSourceInfo *source, const string &lhs, const string &rhs_text,
                                 char sense, double delta) {
	bool strict = prov.strict && sense != '=';
	string sense_str = strict ? (sense == '>' ? ">" : "<") : SenseStr(sense);
	bool down = sense == '=' ? delta < 0.0 : sense == '>';
	string op = down ? " - " : " + ";
	string mag = FormatNum(std::fabs(delta));
	ClauseEdit e;
	e.kind = ClauseEditKind::LOOSEN;
	e.label = QuoteEditedClause(source, lhs, sense_str, rhs_text);
	e.suggestion = QuoteEditedClause(source, lhs, sense_str, rhs_text + op + mag);
	e.has_amount = true;
	e.amount = std::fabs(delta);
	e.group = FormatPerGroupKey(model, prov.group_name);
	e.moved = {prov.source_clause_id, sense, lhs, rhs_text + op + mag};
	return e;
}

} // namespace decide_render
} // namespace duckdb
