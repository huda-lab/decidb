#include "duckdb/decidb/decide_diagnostic_engines.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/decidb/diagnostic_constants.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace duckdb {

namespace {

struct VarAgg {
	string name;
	bool is_aux = false;
	EscapeDirection direction = EscapeDirection::POSITIVE;
	idx_t vidx = DConstants::INVALID_INDEX;
	std::set<idx_t> instances;
};

//! Compact numeric formatting for user-facing constraint text (drops trailing
//! zeros: 12.5 not 12.500000, 10 not 10.0).
string FormatNum(double v) {
	if (v == 0.0) {
		return "0"; // normalize -0.0 (a signed-zero solver read) to a clean "0"
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%.10g", v);
	return string(buf);
}

//! Clean the sub-tolerance noise an I3 stage-2 read carries: the budget freeze is enforced
//! only to the backend feasibility tolerance, so a maximizing re-solve rides it and a clean
//! `10` would otherwise print as `10.000001`. Snap to the nearest integer when within a
//! relative tolerance of one (recovering integer bounds at any magnitude — `1234567889.0001`
//! → `1234567889`, which significant-figure rounding would have mangled to `1234570000`);
//! otherwise trim a genuinely fractional value to a fixed absolute precision.
double SnapDiagnosticValue(double v) {
	if (v == 0.0 || !std::isfinite(v)) {
		return v;
	}
	double nearest_int = std::round(v);
	if (std::fabs(v - nearest_int) <= 1e-6 * std::max(1.0, std::fabs(v))) {
		return nearest_int;
	}
	return std::round(v * 1e6) / 1e6;
}

//! Round a repair amount to the precision the elastic model can actually resolve.
//!
//! Formulating against a widened box costs conditioning: a slack whose exact value is
//! `4.5` comes back as `4.499999`, and a repair reported one part in a million SHORT is
//! not a repair at all — the user pastes `ABS(x*0.5) >= 0.500001` and the query is still
//! infeasible. Round to the last decimal that is meaningful at this value's OWN
//! magnitude. Relative, never absolute: an edit of 2 and an edit of 30000 in the same
//! model are each trustworthy to about six figures of themselves, and a grid taken from
//! the model's largest number would erase the small one entirely.
static double SnapToPrecision(double v) {
	if (v == 0.0 || !std::isfinite(v)) {
		return v;
	}
	double mag = std::fabs(v);
	double grid = 1e-6 * mag;
	double decimals = std::floor(-std::log10(grid));
	if (!std::isfinite(decimals) || decimals < 0.0 || decimals > 15.0) {
		return v;
	}
	double scale = std::pow(10.0, decimals);
	if (!std::isfinite(scale) || scale <= 0.0) {
		return v;
	}
	double ticks = std::round(mag * scale);
	// Never round DOWN. Rounding is allowed to tidy `4.499999` into `4.5`, but it must
	// not turn `4.333333…` into `4.33333`: a loosening reported one tick short of what
	// the solver found is not a repair at all — the user pastes `x <= 3.33333`, the sum
	// lands on 9.99999 against a `>= 10` floor, and the query is still infeasible. When
	// the tidy value would fall below what was measured, take the next tick up instead.
	// Over-repairing by one part in a million is invisible; under-repairing is a bug.
	if (ticks / scale < mag) {
		ticks += 1.0;
	}
	double snapped = ticks / scale;
	return v < 0.0 ? -snapped : snapped;
}

//! User-facing name of a flat solver column: its provenance label (user variable
//! name or aux source expression), falling back to a positional name.
string ColLabel(const vector<ColumnProvenance> &cols, int col) {
	if (col >= 0 && static_cast<idx_t>(col) < cols.size() && !cols[col].label.empty()) {
		return cols[col].label;
	}
	return "col" + std::to_string(col);
}

//! Reconstruct a linear combination as algebra over user-facing column names
//! (e.g. "x", "2*x + 3*y"). Coefficient ±1 is elided to keep the rendering close
//! to what the user wrote.
string FormatTerms(const vector<int> &indices, const vector<double> &coeffs,
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
TermGrouping GroupTermsByVariable(const vector<int> &indices, const vector<double> &coeffs,
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
bool FormatAvgLhs(const vector<int> &indices, const vector<double> &coeffs,
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
bool FormatSumLhs(const vector<int> &indices, const vector<double> &coeffs,
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
//! is a sum over folded rows rather than the coefficient the user wrote. An entity-scoped
//! variable collapses every joined row of an entity onto one column, so `SUM(keepS)` over
//! two rows of a sensor reaches the matrix as `2*keepS` — there is no fan-out for
//! `FormatSumLhs` to key on, and the accumulated constant is not quotable. Render straight
//! from the per-term coefficients the builder recorded (`ConstraintProvenance::folded_terms`)
//! so the label reads as written: `SUM(keepS)`, `SUM(3*keepS)`, `SUM(keepS * price)`.
//! Returns false if any column of the row is not covered by a recorded term, or if a
//! data-varying term has no symbolic name — the caller then falls back to the raw
//! reconstruction, i.e. to today's behaviour.
bool FormatFoldedSumLhs(const vector<int> &indices, const vector<FoldedAggTerm> &folded,
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
string FormatLhs(const ModelConstraint &row, const vector<ColumnProvenance> &cols) {
	string agg;
	if (row.provenance.avg_scaled && FormatAvgLhs(row.indices, row.coefficients, cols, agg)) {
		return agg;
	}
	// An AVG-rewritten row whose coefficient is data-varying can't produce a clean
	// FormatAvgLhs, but it must still render as AVG(...) — otherwise the diagnosis and its
	// suggested edit mislabel the user's AVG constraint as SUM (the value is already in AVG
	// units, so the SUM label makes the suggestion a different, wrong constraint).
	const char *agg_name = row.provenance.avg_scaled ? "AVG" : "SUM";
	// Collapse a uniform SUM fan-out to SUM(c*var). PER-grouped aggregates fold too: they
	// stay distinguishable in the relation via the finding's `group` column + WHEN/PER qualifier
	// (Facet A), so the old group_key == INVALID gate is gone.
	// A row off the accumulating build path carries per-entity *totals*, not the coefficients
	// the user wrote, so every matrix-inference path below misreads it — either as a literal
	// the user never typed (`SUM(2*keepS)`) or, when the totals differ across entities, as a
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
string FormatQuadraticLhs(const SolverModel::QuadraticConstraint &qc,
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

const ConstraintSourceInfo *FindConstraintSource(const SolverModel &model,
	                                             const ConstraintProvenance &provenance) {
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

ConstraintProvenance SourceAwareProvenance(const SolverModel &model, const ConstraintProvenance &provenance) {
	auto result = provenance;
	auto source = FindConstraintSource(model, provenance);
	if (source && !source->qualifier.empty()) {
		result.qualifier = source->qualifier;
	}
	return result;
}

string SourceAwareRhs(const SolverModel &model, const ConstraintProvenance &provenance, double fallback) {
	auto source = FindConstraintSource(model, provenance);
	if (source && source->rhs_kind == ConstraintSourceRhsKind::DATA_EXPRESSION &&
	    !source->canonical_rhs.empty()) {
		return source->canonical_rhs;
	}
	return provenance.rhs_label.empty() ? FormatNum(fallback) : provenance.rhs_label;
}

const char *SenseStr(char sense) {
	return sense == '<' ? "<=" : (sense == '>' ? ">=" : "=");
}

//! The clause as the user wrote it: `lhs sense rhs`, with the WHEN/PER qualifier
//! appended (Facet C) so it stays recognizable. For a strict `<` / `>` the δ offset was
//! baked into `rhs` at build time, so quote the user's typed literal (`typed_k`) and
//! render `<` / `>` rather than the model's `<=` / `>=` (I2.d).
//! The number to quote for a row whose own RHS is not what the user typed. Two things
//! can shift it, and they never apply at once: the binder bakes a δ into a strict
//! `<` / `>` (re-quote `typed_k`), and a lowering adds a mechanism term to build a Big-M
//! row (subtract it back off). Everything the reader sees goes through here, so a clause
//! reads the same whichever rewrite the query happened to take.
double DisplayRhs(const ConstraintProvenance &prov, double rhs, char sense) {
	if (prov.strict && sense != '=') {
		return prov.typed_k;
	}
	return rhs - prov.rhs_mechanism_offset;
}

string MakeClauseLabel(const ConstraintProvenance &prov, const string &lhs, double rhs, char sense) {
	bool strict = prov.strict && sense != '=';
	string sense_str = strict ? (sense == '>' ? ">" : "<") : SenseStr(sense);
	double base_rhs = DisplayRhs(prov, rhs, sense);
	string suffix = prov.qualifier.empty() ? "" : (" " + prov.qualifier);
	return lhs + " " + sense_str + " " + FormatNum(base_rhs) + suffix;
}

//! Build a LOOSEN edit from a constraint's rendered LHS + sense + RHS and the
//! solved slack `amount` (signed; for `=` it is the net s⁺−s⁻). For a strict `<` /
//! `>` the δ offset was baked into `rhs` at build time, so re-quote the suggestion
//! against the user's typed literal (`typed_k`) and render `<` / `>` (I2.d).
ClauseEdit MakeLoosenEdit(const ConstraintProvenance &prov, const string &lhs, double rhs,
                          char sense, double amount) {
	bool strict = prov.strict && sense != '=';
	string sense_str = strict ? (sense == '>' ? ">" : "<") : SenseStr(sense);
	double base_rhs = DisplayRhs(prov, rhs, sense);
	// `≥` loosens downward (b − s), `≤` / `=` upward (b + s).
	double new_rhs = (sense == '>') ? base_rhs - amount : base_rhs + amount;
	// Facet C: append the WHEN/PER qualifier (`PER grp`) so the clause is recognizable;
	// Facet A: carry the group's printable key as its own field (a separate `group` EAV
	// row) so two folded `SUM(x)` groups stay distinguishable in the relation.
	string suffix = prov.qualifier.empty() ? "" : (" " + prov.qualifier);
	ClauseEdit e;
	e.kind = ClauseEditKind::LOOSEN;
	e.label = MakeClauseLabel(prov, lhs, rhs, sense);
	e.suggestion = lhs + " " + sense_str + " " + FormatNum(new_rhs) + suffix;
	e.has_amount = true;
	e.amount = std::fabs(amount);
	e.group = prov.group_label;
	return e;
}

//! Name every user clause whose bound is out of reach, before any solve. A clause fans
//! into one row per relation row, all rendering the same text, so entries are
//! de-duplicated on (label, group) — the user wrote one clause and reads one finding.
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
vector<UnreachableClause> CollectUnreachableClauses(const SolverModel &model,
                                                    const vector<ColumnProvenance> &columns) {
	vector<UnreachableClause> found;
	std::set<std::pair<string, string>> seen;
	auto record = [&](const ConstraintProvenance &prov, const string &lhs, double rhs, char sense) {
		auto display_provenance = SourceAwareProvenance(model, prov);
		UnreachableClause c;
		c.label = MakeClauseLabel(display_provenance, lhs, rhs, sense);
		c.group = display_provenance.group_label;
		if (!seen.insert({c.label, c.group}).second) {
			return;
		}
		found.push_back(std::move(c));
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
//! query-level offset `x <= col + delta` (`>=` loosens downward → `col - delta`).
//! `rhs_text` is the RHS column name (`prov.rhs_label`) or a numeric fallback when no
//! name is available. `edit_source` is stamped by the caller.
ClauseEdit MakeVirtualOffsetEdit(const ConstraintProvenance &prov, const string &lhs,
                                 const string &rhs_text, char sense, double delta) {
	string sense_str = SenseStr(sense);
	string suffix = prov.qualifier.empty() ? "" : (" " + prov.qualifier);
	string op = (sense == '>') ? " - " : " + ";
	string mag = FormatNum(std::fabs(delta));
	ClauseEdit e;
	e.kind = ClauseEditKind::LOOSEN;
	e.label = lhs + " " + sense_str + " " + rhs_text + suffix;
	e.suggestion = lhs + " " + sense_str + " " + rhs_text + op + mag + suffix;
	e.has_amount = true;
	e.amount = std::fabs(delta);
	e.group = prov.group_label;
	return e;
}

} // namespace

DecideDiagnostic DiagnoseUnbounded(const UnboundedDiagnosisInput &input) {
	if (input.result.ray.empty()) {
		return DecideDiagnostic();
	}

	vector<ColumnProvenance> columns =
	    BuildColumnProvenance(input.indexer, input.var_labels, input.var_is_aux);

	std::map<idx_t, VarAgg> by_var;
	for (idx_t col = 0; col < input.result.ray.size() && col < columns.size(); col++) {
		double rv = input.result.ray[col];
		if (std::fabs(rv) <= DIAGNOSTIC_RAY_EPSILON) {
			continue;
		}
		const ColumnProvenance &prov = columns[col];
		if (prov.kind == ColumnKind::GLOBAL_AUX || prov.label.empty() ||
		    prov.decide_var_idx == DConstants::INVALID_INDEX) {
			continue;
		}
		auto &agg = by_var[prov.decide_var_idx];
		if (agg.name.empty()) {
			agg.name = prov.label;
			agg.is_aux = prov.kind == ColumnKind::AUX;
			agg.direction = rv > 0 ? EscapeDirection::POSITIVE : EscapeDirection::NEGATIVE;
			agg.vidx = prov.decide_var_idx;
		}
		agg.instances.insert(prov.instance);
	}

	vector<VarEscape> escapes;
	for (auto &kv : by_var) {
		VarAgg &agg = kv.second;
		VarEscape ve;
		ve.name = agg.name;
		ve.direction = agg.direction;
		ve.is_aux = agg.is_aux;
		ve.is_entity_scoped = input.indexer.var_scope[agg.vidx] == DecideVarScope::ENTITY;
		ve.total = input.indexer.NumInstances(agg.vidx);
		ve.escaping = agg.instances.size();
		ve.all_escape = ve.escaping >= ve.total;
		if (!agg.is_aux && ve.escaping < ve.total && ve.total > 1 && input.get_candidates) {
			ve.rules = CharacterizeEscape(agg.instances, ve.total,
			                              input.get_candidates(agg.vidx, ve.total),
			                              input.params.escape_rate);
		}
		escapes.push_back(std::move(ve));
	}

	if (escapes.empty()) {
		return DecideDiagnostic();
	}
	return BuildUnboundedDiagnostic(escapes);
}

template <class T>
static void DropTaggedEntries(vector<T> &entries, const std::set<idx_t> &dropped) {
	vector<T> retained;
	retained.reserve(entries.size());
	for (auto &entry : entries) {
		auto group_id = entry.provenance.removal_group_id;
		if (group_id == DConstants::INVALID_INDEX || dropped.find(group_id) == dropped.end()) {
			retained.push_back(std::move(entry));
		}
	}
	entries = std::move(retained);
}

SolverModel DropRemovalGroups(const SolverModel &base, const std::set<idx_t> &removal_group_ids) {
	SolverModel result = base;
	DropTaggedEntries(result.constraints, removal_group_ids);
	DropTaggedEntries(result.quadratic_constraints, removal_group_ids);
	DropTaggedEntries(result.indicator_constraints, removal_group_ids);
	DropTaggedEntries(result.general_constraints, removal_group_ids);

	// Build-time infeasibility is cached on the whole model, so dropping the constant
	// row that proved it must clear the stale flag. Recompute from exactly the retained
	// coefficient-free linear rows using the builder's own tolerance and rules.
	result.build_proven_infeasible = false;
	constexpr double EPS = 1e-9;
	for (const auto &row : result.constraints) {
		if (!row.indices.empty()) {
			continue;
		}
		bool violated = row.sense == '<' ? row.rhs < -EPS
		                : row.sense == '>' ? row.rhs > EPS
		                                   : std::fabs(row.rhs) > EPS;
		if (violated) {
			result.build_proven_infeasible = true;
			break;
		}
	}
	return result;
}

ElasticModel BuildElasticModel(const SolverModel &base, const string &slack_scope) {
	ElasticModel out;
	out.model = base;
	SolverModel &elastic = out.model;
	// query mode (default) folds a data-backed clause's rows into one shared slack (one
	// virtual offset `x <= col + delta`); expanded mode keeps them independent so the
	// readback can expose the per-row profile. SHARED_SCALAR knobs fold in both modes.
	bool fold_data = slack_scope != "expanded";

	// Rebuild the objective as an empty repair objective: zero the user's objective over
	// the existing columns first (slack/removal columns are appended with 0 here), and
	// drop any quadratic objective. DiagnoseInfeasible sets one tier objective per
	// lexicographic pass.
	for (auto &c : elastic.obj_coeffs) {
		c = 0.0;
	}
	elastic.q_rows.clear();
	elastic.q_cols.clear();
	elastic.q_vals.clear();
	elastic.has_quadratic_obj = false;
	elastic.nonconvex_quadratic = false;
	elastic.maximize = false;

	// Append a REAL, non-negative, uncapped slack column (sᵢ≥0 REAL even for integer
	// RHS — decision 4). No ceiling is needed: what a repair can actually reach is
	// bounded by the widened COLUMN box, which is a hard constraint the solver cannot
	// exceed, so a slack has nothing to gain by running past it. The repair objective
	// coefficient is attached later by the active lexicographic tier.
	auto add_slack = [&]() -> idx_t {
		idx_t c = elastic.num_vars;
		elastic.col_lower.push_back(0.0);
		elastic.col_upper.push_back(1e30);
		elastic.is_integer.push_back(false);
		elastic.is_binary.push_back(false);
		elastic.obj_coeffs.push_back(0.0);
		elastic.num_vars++;
		return c;
	};

	// A relaxable row whose RHS is per-row data (`x <= col`) cannot be edited by the
	// user; loosening it is a conflict, not a source-literal edit. It gets its own
	// lexicographic tier. Shared/aggregate/literal knobs are editable.
	auto has_explicit_shape = [](const ConstraintProvenance &p) {
		return p.shape == ElasticShape::PER_ROW_DATA || p.shape == ElasticShape::SHARED_SCALAR;
	};
	// Everything a blamable row must carry. A relaxable row is one the diagnosis may
	// tell the user to edit, so besides an explicit shape it has to be able to name
	// WHICH clause of theirs it is: without a source_clause_id, SourceAwareLhs falls
	// back to FormatLhs and rebuilds the clause from the row's own coefficients, so the
	// user is shown a lowering's Big-M as though they had typed it. A row that encodes
	// a construct's mechanism rather than its bound is USER_MECHANISM and never gets
	// here -- which is also the honest answer when no editable form exists, since
	// "relax the definition of MIN" is not a change anyone can make to their SQL.
	auto assert_blamable_row = [&](const ConstraintProvenance &p) {
		if (IsRelaxableForElastic(p.kind) && p.repair_group_id != DConstants::INVALID_INDEX) {
			D_ASSERT(has_explicit_shape(p));
			D_ASSERT(p.source_clause_id != DConstants::INVALID_INDEX);
		}
	};
	auto is_data_offset = [](const ConstraintProvenance &p) {
		return p.shape == ElasticShape::PER_ROW_DATA && p.repair_group_id != DConstants::INVALID_INDEX;
	};

	// Scale-normalized editable slack weights (T1). The stage-1 objective sums slacks
	// across constraints in incomparable units (`SUM(buy) >= 30` count-units vs
	// `SUM(buy*price) <= 100` dollar-units); with uniform weights the small-magnitude
	// row is gutted rather than the genuinely-tight one ("require nothing" edit). Weight
	// each editable knob by `ref / rms(Aᵢ)`, where `rms(Aᵢ) = √(Σcⱼ²/nnz)` is the row's
	// root-mean-square coefficient — its *typical* magnitude, invariant to how many terms
	// it spans. (A plain ‖Aᵢ‖₂ would make a many-variable aggregate floor `x+y ≥ 10`
	// numerically cheaper to loosen than a single-variable cap purely because it has more
	// terms, gutting the floor to a degenerate objective-0 fix — RMS removes that
	// term-count bias while still capturing the data-magnitude mismatch that E needs.)
	// `ref` = the smallest editable RMS — a common factor (so it never changes *which* row
	// is chosen, only conditioning) that keeps every editable weight in (0,1], and leaves
	// all-equal-coefficient models (the usual case, RMS=1) at weight 1. Only the EDITABLE
	// tier is normalized; data offsets and removals are separate lexicographic tiers. RMS
	// is over the ORIGINAL row coefficients (before slack columns).
	auto rms_norm = [](const vector<double> &coeffs) {
		if (coeffs.empty()) {
			return 0.0;
		}
		double s = 0.0;
		for (double v : coeffs) {
			s += v * v;
		}
		return std::sqrt(s / static_cast<double>(coeffs.size()));
	};
	vector<double> lin_norm(elastic.constraints.size(), 0.0);
	vector<double> qc_norm(elastic.quadratic_constraints.size(), 0.0);
	double ref_norm = 0.0;
	bool have_ref = false;
	auto consider_ref = [&](double n) {
		if (n > 0.0 && (!have_ref || n < ref_norm)) {
			ref_norm = n;
			have_ref = true;
		}
	};
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		const auto &row = elastic.constraints[r];
		assert_blamable_row(row.provenance);
		lin_norm[r] = rms_norm(row.coefficients);
		if (row.provenance.removal_group_id == DConstants::INVALID_INDEX &&
		    IsRelaxableForElastic(row.provenance.kind) && !is_data_offset(row.provenance)) {
			consider_ref(lin_norm[r]);
		}
	}
	for (idx_t qr = 0; qr < elastic.quadratic_constraints.size(); qr++) {
		const auto &qc = elastic.quadratic_constraints[qr];
		assert_blamable_row(qc.provenance);
		qc_norm[qr] = rms_norm(qc.linear_coefficients);
		if (qc.provenance.removal_group_id == DConstants::INVALID_INDEX &&
		    IsRelaxableForElastic(qc.provenance.kind)) {
			consider_ref(qc_norm[qr]);
		}
	}
	if (!have_ref) {
		ref_norm = 1.0;
	}
	// ref / rms(Aᵢ), guarding a degenerate all-zero row.
	auto editable_weight = [&](double norm) { return norm > 0.0 ? ref_norm / norm : 1.0; };

	// Wire a (possibly shared) slack into one row by its sense. `<` loosens upward
	// (Ax − s ≤ b), `>` downward (Ax + s ≥ b), `=` both ways (Ax − s⁺ + s⁻ = b).
	auto wire = [&](idx_t r, idx_t pos_col, idx_t neg_col) {
		auto &row = elastic.constraints[r];
		if (row.sense == '>') {
			row.indices.push_back(static_cast<int>(pos_col));
			row.coefficients.push_back(1.0);
		} else { // '<' and '='
			row.indices.push_back(static_cast<int>(pos_col));
			row.coefficients.push_back(-1.0);
		}
		if (row.sense == '=') {
			row.indices.push_back(static_cast<int>(neg_col));
			row.coefficients.push_back(1.0);
		}
	};

	// Slack only the relaxable LINEAR rows (USER_PARAMETER). Quadratic rows and
	// STRUCTURAL / USER_MECHANISM rows stay rigid (quadratic-RHS slack is I2.d; ABS
	// pin rows are already USER_PARAMETER and are picked up here, their envelopes are
	// STRUCTURAL and stay rigid). One slack per row for `<` / `>`; two for `=`
	// (decision 3) so it can be violated in either direction while staying linear.
	//
	// T3 folding policy. A user knob fans into N rows; the minimal loosening is the max
	// overshoot (a single `s` with `eᵢ − s ≤ K` is driven to the max), not the sum.
	//   - SHARED_SCALAR (a query-wide cap, a PER/aggregate group row): always folds.
	//   - PER_ROW_DATA (a data-backed RHS `x <= col`): folds only in query mode, into one
	//     virtual offset `x <= col + delta`; in expanded mode its rows stay independent so
	//     the readback exposes the per-row profile.
	// Rows without a clause id always stay independent.
	auto folds = [&](const ConstraintProvenance &p) {
		if (p.repair_group_id == DConstants::INVALID_INDEX) {
			return false;
		}
		if (p.shape == ElasticShape::SHARED_SCALAR) {
			return true;
		}
		return fold_data && p.shape == ElasticShape::PER_ROW_DATA;
	};
	// The block key decides how far a knob folds. In query mode a PER clause is ONE SQL
	// literal the user edits, so all its groups fold into a single slack (key ignores
	// group_key). In expanded mode each PER group is its own slack (key includes it), so
	// the profile breaks out per group.
	auto block_key = [&](const ConstraintProvenance &p) -> std::pair<idx_t, idx_t> {
		return fold_data ? std::make_pair(p.repair_group_id, static_cast<idx_t>(0))
		                 : std::make_pair(p.repair_group_id, p.group_key);
	};
	std::map<std::pair<idx_t, idx_t>, vector<idx_t>> blocks;
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		const auto &row = elastic.constraints[r];
		if (row.provenance.removal_group_id != DConstants::INVALID_INDEX ||
		    !IsRelaxableForElastic(row.provenance.kind) || !folds(row.provenance)) {
			continue;
		}
		blocks[block_key(row.provenance)].push_back(r);
	}

	// Emit in row order: a shared block is emitted once, at its first row (its rows
	// are collected in ascending order, so `front()` is the first we reach); every
	// other relaxable row gets its own size-1 block.
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		const auto &row = elastic.constraints[r];
		if (row.provenance.removal_group_id != DConstants::INVALID_INDEX ||
		    !IsRelaxableForElastic(row.provenance.kind)) {
			continue;
		}
		if (folds(row.provenance)) {
			const vector<idx_t> &grp = blocks[block_key(row.provenance)];
			if (grp.front() != r) {
				continue; // already emitted at the block's first row
			}
			// A shared block is one editable literal knob, scale-normalized by its
			// representative row (members are same-clause instances → equal norms), or
			// one folded data offset in query mode.
			char sense = elastic.constraints[grp.front()].sense;
			bool data_offset = is_data_offset(row.provenance);
			ElasticRepairTier tier = data_offset ? ElasticRepairTier::DATA_OFFSET
			                                     : ElasticRepairTier::EDITABLE_LOOSEN;
			double w = data_offset ? 1.0 : editable_weight(lin_norm[grp.front()]);
			idx_t pos_col = add_slack();
			idx_t neg_col = (sense == '=') ? add_slack() : DConstants::INVALID_INDEX;
			for (idx_t rr : grp) {
				wire(rr, pos_col, neg_col);
			}
			out.slacks.push_back({grp, pos_col, neg_col, sense, tier, w});
		} else {
			char sense = row.sense;
			bool data_offset = is_data_offset(row.provenance);
			ElasticRepairTier tier = data_offset ? ElasticRepairTier::DATA_OFFSET
			                                     : ElasticRepairTier::EDITABLE_LOOSEN;
			double w = data_offset ? 1.0 : editable_weight(lin_norm[r]);
			idx_t pos_col = add_slack();
			idx_t neg_col = (sense == '=') ? add_slack() : DConstants::INVALID_INDEX;
			wire(r, pos_col, neg_col);
			out.slacks.push_back({{r}, pos_col, neg_col, sense, tier, w});
		}
	}

	// I2.d: relaxable QUADRATIC constraints (QCQP). Slack the LINEAR RHS only —
	// never the Q matrix — so `e(x) + xᵀQx ≤ K` loosens to `… ≤ K + s` while
	// staying a quadratic constraint. Each is its own size-1 block (shared-slack
	// quadratic shapes are out of scope); the `quadratic` flag marks that `rows`
	// indexes `quadratic_constraints`. Solver-gated to Gurobi (HiGHS skips QCQP).
	for (idx_t qr = 0; qr < elastic.quadratic_constraints.size(); qr++) {
		auto &qc = elastic.quadratic_constraints[qr];
		if (qc.provenance.removal_group_id != DConstants::INVALID_INDEX ||
		    !IsRelaxableForElastic(qc.provenance.kind)) {
			continue;
		}
		// A quadratic constraint is always reported as a LOOSEN edit (the conflict
		// summary covers linear data-RHS rows only), so its slack is an editable knob
		// (scale-normalized by its linear part), not penalized as data.
		char sense = qc.sense;
		double w = editable_weight(qc_norm[qr]);
		idx_t pos_col = add_slack();
		idx_t neg_col = (sense == '=') ? add_slack() : DConstants::INVALID_INDEX;
		qc.linear_indices.push_back(static_cast<int>(pos_col));
		qc.linear_coefficients.push_back(sense == '>' ? 1.0 : -1.0);
		if (sense == '=') {
			qc.linear_indices.push_back(static_cast<int>(neg_col));
			qc.linear_coefficients.push_back(1.0);
		}
		out.slacks.push_back({{qr}, pos_col, neg_col, sense, ElasticRepairTier::EDITABLE_LOOSEN, w,
		                      /*quadratic=*/true});
	}

	return out;
}

//! Read the slack support of a solved elastic model into the user-facing edit list.
//! Every block whose slack clears the noise floor is an edit; the amount is the
//! slack value (`=` reports the net s⁺ − s⁻). A data-RHS clause (`x <= col`) has no
//! single literal to loosen, so it reads back per the `slack_scope` policy (T3): "query"
//! folds it into one virtual offset `x <= col + delta`; "expanded" exposes each per-row
//! slack. Clauses are labelled from the ORIGINAL row (`orig`, before slacks were
//! appended). Pure in `solution`, so it runs against either the stage-1 or stage-2 solve.
static vector<ClauseEdit> ReadElasticEdits(const vector<BlockSlackRef> &slacks,
                                           const vector<double> &solution,
                                           const SolverModel &orig_model,
                                           const vector<ColumnProvenance> &columns, bool snap,
                                           const string &slack_scope) {
	// T3 slack-scope policy.
	//   query (default): each block is ONE SQL literal knob the user edits, folded across
	//     PER groups (BuildElasticModel). Editable literals report `source_literal`, a
	//     data RHS reports a `virtual_offset` (`x <= col + delta`); both are clause-level,
	//     so any per-group identity is dropped.
	//   expanded: PER/aggregate groups get one slack each (`expanded_group`, keeping the
	//     group key); a data RHS stays per-row (`expanded_row`, keeping the row id); a
	//     non-grouped literal has nothing to break out and reports `source_literal`.
	bool expanded = slack_scope == "expanded";

	vector<ClauseEdit> edits;
	for (const auto &sl : slacks) {
		double amount;
		if (sl.sense == '=') {
			amount = solution[sl.pos_col] - solution[sl.neg_col];
		} else {
			amount = solution[sl.pos_col];
		}
		// What counts as an edit rather than a backend's own rounding. A widened box is
		// worse conditioned than the model it came from, so a solver can return its
		// feasibility tolerance as a slack and turn it into a straight-faced
		// `x <= 1` → `x <= 1.000001`. The floor that removes it is ABSOLUTE and small — a
		// few multiples of the backend tolerance itself.
		//
		// It must not be relative to anything. A floor taken from the model's scale erases
		// every small edit in a model that happens to contain one large-coefficient row;
		// a floor taken from the largest edit in the same repair erases the `x <= 0.001`
		// half of a repair whose other half is `y <= 20000`. Both were measured, and both
		// are worse than the noise they remove: an extra hairline edit is cosmetic, while a
		// dropped one makes the reported repair not work when the user applies it. When in
		// doubt this reports the edit.
		double edit_floor = 1e-5;
		if (std::fabs(amount) <= edit_floor) {
			continue;
		}
		// Round away the conditioning noise a widened box costs. Applied to every read,
		// stage 1 included: this noise comes from the model's constants, not from the
		// budget freeze the I3 snap below cleans up.
		{
			amount = SnapToPrecision(amount);
			if (std::fabs(amount) <= edit_floor) {
				continue;
			}
		}
		// I3: snap the stage-2 read again to absorb the budget cushion's sub-display noise.
		// The precision cleanup above applies to both stages because it comes from the
		// widened formulation itself, not from the stage-2 budget row.
		if (snap) {
			amount = SnapDiagnosticValue(amount);
			if (std::fabs(amount) <= DIAGNOSTIC_RAY_EPSILON) {
				continue;
			}
		}
		// Label the clause from the block's representative ORIGINAL row (orig_model,
		// before slacks were appended). For a shared-slack block all rows render the
		// same LHS/RHS, so the first row is canonical.
		if (sl.quadratic) {
			const auto &orig = orig_model.quadratic_constraints[sl.rows.front()];
			auto display_provenance = SourceAwareProvenance(orig_model, orig.provenance);
			ClauseEdit e = MakeLoosenEdit(display_provenance, SourceAwareQuadraticLhs(orig_model, orig, columns),
			                              orig.rhs, sl.sense, amount);
			e.edit_source = "source_literal";
			edits.push_back(std::move(e));
			continue;
		}
		const ModelConstraint &orig = orig_model.constraints[sl.rows.front()];
		const ConstraintProvenance &prov = orig.provenance;
		auto display_provenance = SourceAwareProvenance(orig_model, prov);
		string display_lhs = SourceAwareLhs(orig_model, orig, columns);
		// Non-empty only when canonicalization moved a term across the comparison, in
		// which case it pairs with the written LHS that SourceAwareLhs just returned.
		string written_rhs = SourceWrittenRhs(orig_model, prov);
		bool data_rhs = prov.shape == ElasticShape::PER_ROW_DATA &&
		                prov.repair_group_id != DConstants::INVALID_INDEX;
		if (expanded) {
			// Only an unreduced per-row constraint expands row by row. A reduced
			// constraint emits one elastic row per *group*, so a data-derived bound on
			// one of those is a per-group knob, not a per-row profile entry — routing
			// it here would relabel every group as a row and drop the group key.
			if (data_rhs && !prov.is_aggregate) {
				// The independent per-row slack is that row's exact overshoot, one profile
				// entry. A debug view, not a directly pasteable edit.
				ClauseEdit e = MakeLoosenEdit(display_provenance, display_lhs, orig.rhs, sl.sense, amount);
				e.edit_source = "expanded_row";
				// The emitted row's own identity, so an `expanded` profile can be read
				// back row by row. `group_key` carries the row id for a per-row clause.
				e.row = prov.group_key;
				edits.push_back(std::move(e));
				continue;
			}
			// A knob to report on its own. A user literal re-quotes as a number; a
			// data-derived bound has no number in the query to edit, so it renders as
			// a symbolic offset over the column, exactly as query mode does.
			ClauseEdit e;
			if (!written_rhs.empty()) {
				// Canonicalization moved a term across this comparison, so the bound the
				// model carries is a literal the query does not contain. Offset the bound
				// the user actually wrote instead — the same relaxation, named where they
				// can apply it.
				e = MakeVirtualOffsetEdit(display_provenance, display_lhs, written_rhs, sl.sense, amount);
			} else if (data_rhs) {
				string rhs_text = SourceAwareRhs(orig_model, prov, orig.rhs);
				e = MakeVirtualOffsetEdit(display_provenance, display_lhs, rhs_text, sl.sense, amount);
			} else {
				e = MakeLoosenEdit(display_provenance, display_lhs, orig.rhs, sl.sense, amount);
			}
			// When PER-grouped this block is one group → break it out with its group
			// key; otherwise there is nothing per-group to expose.
			if (!prov.group_label.empty()) {
				e.edit_source = "expanded_group";
			} else {
				e.edit_source = data_rhs ? "virtual_offset" : "source_literal";
			}
			edits.push_back(std::move(e));
			continue;
		}
		// Query mode: the block folds a whole clause (all PER groups) into one edit, so it
		// is reported clause-level with no single-group identity.
		ClauseEdit e;
		if (!written_rhs.empty()) {
			// As above: offset the written bound, not the literal canonicalization left
			// behind. Reported as a virtual offset because that is what it is — there is
			// no number in the query to retype.
			e = MakeVirtualOffsetEdit(display_provenance, display_lhs, written_rhs, sl.sense, amount);
			e.edit_source = "virtual_offset";
		} else if (data_rhs) {
			// The folded shared slack `delta` is one virtual query-level offset over the
			// data column (`x <= col + delta`). rhs_label names the column; fall back to
			// the numeric representative RHS when it is unavailable.
			string rhs_text = SourceAwareRhs(orig_model, prov, orig.rhs);
			e = MakeVirtualOffsetEdit(display_provenance, display_lhs, rhs_text, sl.sense, amount);
			e.edit_source = "virtual_offset";
		} else {
			e = MakeLoosenEdit(display_provenance, display_lhs, orig.rhs, sl.sense, amount);
			e.edit_source = "source_literal";
		}
		e.group.clear(); // folded across groups → clause-level, no single group key
		edits.push_back(std::move(e));
	}

	return edits;
}

//! True iff the model carries a real objective to re-solve. With no objective there is
//! no "achievable objective" to report, so the I3 stage-2 re-solve is skipped (and the
//! stage-1 edit is reported unchanged). Also avoids perturbing objective-less models with
//! the budget-cushion re-solve.
static bool HasObjective(const SolverModel &model) {
	if (model.has_quadratic_obj) {
		return true;
	}
	for (double c : model.obj_coeffs) {
		if (c != 0.0) {
			return true;
		}
	}
	return false;
}

struct TierBudget {
	ElasticRepairTier tier;
	double value = 0.0;
	bool active = false;
};

static double SlackTierCoefficient(const BlockSlackRef &sl, ElasticRepairTier tier) {
	if (sl.tier != tier) {
		return 0.0;
	}
	if (tier == ElasticRepairTier::EDITABLE_LOOSEN) {
		return sl.weight;
	}
	return 1.0;
}

static bool AppendTierTerms(ModelConstraint &row, const vector<BlockSlackRef> &slacks,
                            ElasticRepairTier tier) {
	for (const auto &sl : slacks) {
		double coeff = SlackTierCoefficient(sl, tier);
		if (coeff == 0.0) {
			continue;
		}
		row.indices.push_back(static_cast<int>(sl.pos_col));
		row.coefficients.push_back(coeff);
		if (sl.neg_col != DConstants::INVALID_INDEX) {
			row.indices.push_back(static_cast<int>(sl.neg_col));
			row.coefficients.push_back(coeff);
		}
	}
	return !row.indices.empty();
}

//! True iff `tier` owns at least one repair knob. An empty tier contributes nothing to the
//! lexicographic ladder, so its pass is skipped entirely (no solve, no budget row).
static bool TierHasTerms(const vector<BlockSlackRef> &slacks, ElasticRepairTier tier) {
	for (const auto &sl : slacks) {
		if (sl.tier == tier) {
			return true;
		}
	}
	return false;
}

static void SetTierObjective(SolverModel &model, const vector<BlockSlackRef> &slacks,
                             ElasticRepairTier tier) {
	for (auto &c : model.obj_coeffs) {
		c = 0.0;
	}
	model.q_rows.clear();
	model.q_cols.clear();
	model.q_vals.clear();
	model.has_quadratic_obj = false;
	model.nonconvex_quadratic = false;
	model.maximize = false;

	ModelConstraint objective_terms;
	if (!AppendTierTerms(objective_terms, slacks, tier)) {
		return;
	}
	for (idx_t i = 0; i < objective_terms.indices.size(); i++) {
		idx_t col = static_cast<idx_t>(objective_terms.indices[i]);
		model.obj_coeffs[col] = objective_terms.coefficients[i];
	}
}

static double ComputeTierValue(const vector<double> &solution, const vector<BlockSlackRef> &slacks,
                               ElasticRepairTier tier) {
	double value = 0.0;
	for (const auto &sl : slacks) {
		double coeff = SlackTierCoefficient(sl, tier);
		if (coeff == 0.0) {
			continue;
		}
		value += coeff * solution[sl.pos_col];
		if (sl.neg_col != DConstants::INVALID_INDEX) {
			value += coeff * solution[sl.neg_col];
		}
	}
	return std::max(0.0, value);
}

static TierBudget MakeTierBudget(const vector<double> &solution, const vector<BlockSlackRef> &slacks,
                                 ElasticRepairTier tier) {
	ModelConstraint terms;
	bool active = AppendTierTerms(terms, slacks, tier);
	return {tier, active ? ComputeTierValue(solution, slacks, tier) : 0.0, active};
}

static void AddTierBudgetRow(SolverModel &model, const vector<BlockSlackRef> &slacks,
                             const TierBudget &budget) {
	if (!budget.active) {
		return;
	}
	ModelConstraint row;
	if (!AppendTierTerms(row, slacks, budget.tier)) {
		return;
	}
	row.sense = '<';
	row.rhs = budget.value;
	row.provenance.kind = ConstraintKind::STRUCTURAL; // rigid: never slacked or edited
	model.constraints.push_back(std::move(row));
}

//! Stage-2 freeze-budget model (I3): start from the elastic model (repair knobs already
//! wired), cap each solved lexicographic repair tier at its optimum, and restore the
//! user's original objective so the re-solve optimizes among all minimal repairs.
static SolverModel BuildStage2Model(const SolverModel &elastic, const vector<BlockSlackRef> &slacks,
                                    const SolverModel &original, const vector<TierBudget> &budgets) {
	SolverModel stage2 = elastic;

	// Freeze the lexicographic repair budgets, not individual repair variables.
	// For remove-only `<>` clauses this keeps R <= R* while letting stage 2 choose
	// the objective-best equally minimal DROP set.
	for (const auto &budget : budgets) {
		AddTierBudgetRow(stage2, slacks, budget);
	}

	// Restore the user's original objective. The slack columns (appended past the
	// original variables) get zero objective weight, so the achievable objective the
	// solver reports is exactly cᵀx over the user's variables.
	stage2.obj_coeffs = original.obj_coeffs;
	stage2.obj_coeffs.resize(stage2.num_vars, 0.0);
	stage2.q_rows = original.q_rows;
	stage2.q_cols = original.q_cols;
	stage2.q_vals = original.q_vals;
	stage2.has_quadratic_obj = original.has_quadratic_obj;
	stage2.nonconvex_quadratic = original.nonconvex_quadratic;
	stage2.maximize = original.maximize;

	return stage2;
}

//! Stage-2b source-order tie-break (solver-agnostic determinism). The lexicographic repair
//! objective can be *indifferent* between two equally-minimal repairs — two DROP sets, or
//! an LP that can put the same loosening on either of two clauses (or split it fractionally
//! across both where one edit suffices). The solver then picks arbitrarily, so Gurobi and
//! HiGHS can name different clauses. This pass removes that ambiguity: freeze the user
//! objective at its stage-2a optimum (when there is one), then, among all repairs that
//! still hit it under the frozen tier budgets, minimize a rank-weighted repair sum — every
//! knob keeps its tier coefficient (editable slacks: the T1 scale weight) scaled by its
//! 1-based rank in emission order (`removals` ascend by indicator column and `slacks` by
//! row: matrix rows in declaration order, then re-emitted absorbed bounds, whose original
//! declaration position is not recorded). Under a pinned tier budget, concentrating the
//! repair on the lowest-ranked clause is then the unique optimum, so both backends report
//! the same edit.
//!
//! Only meaningful when some tier owns >= 2 knobs (the budget rows pin each tier's total,
//! so cross-tier reshuffling is impossible), and the freeze only applies to linear
//! objectives — a quadratic objective would need a quadratic freeze row, and an exact tie
//! there is rarer still, so it keeps the stage-2a result. Returns false (skip the extra
//! solve) otherwise. `freeze_objective` is false on the no-objective path, where there is
//! nothing to preserve but a repair tie is just as solver-arbitrary.
static bool BuildTieBreakModel(SolverModel &tiebreak, const vector<BlockSlackRef> &slacks,
                               bool freeze_objective, double objective_value) {
	idx_t editable_knobs = 0;
	idx_t data_knobs = 0;
	for (const auto &sl : slacks) {
		(sl.tier == ElasticRepairTier::DATA_OFFSET ? data_knobs : editable_knobs)++;
	}
	if (editable_knobs < 2 && data_knobs < 2) {
		return false;
	}
	if (freeze_objective) {
		if (tiebreak.has_quadratic_obj) {
			return false;
		}
		// Freeze the objective at its stage-2a optimum with a one-sided row. The rhs is
		// EXACT — no tolerance cushion: any eps of objective room is monetized by the
		// rank objective, which would shave eps of repair onto a lower-ranked slack and
		// split the edit (`x <= 9.999989` plus a phantom second clause). The stage-2a
		// point attains the value read from the solver's own solution to machine
		// precision, well inside both backends' feasibility tolerance; if a rounding
		// pathology ever makes the row unattainable the pass just returns non-optimal
		// and the caller keeps the stage-2a repair.
		ModelConstraint freeze;
		for (idx_t j = 0; j < tiebreak.obj_coeffs.size(); j++) {
			if (tiebreak.obj_coeffs[j] != 0.0) {
				freeze.indices.push_back(static_cast<int>(j));
				freeze.coefficients.push_back(tiebreak.obj_coeffs[j]);
			}
		}
		if (freeze.indices.empty()) {
			return false; // constant objective: nothing to preserve, nothing to tie-break against
		}
		freeze.sense = tiebreak.maximize ? '>' : '<';
		freeze.rhs = objective_value;
		freeze.provenance.kind = ConstraintKind::STRUCTURAL; // rigid: never slacked or edited
		tiebreak.constraints.push_back(std::move(freeze));
	}

	// New objective: minimize the rank-weighted repair sum. Each tier's total is already
	// pinned at its stage-1 optimum by the budget rows carried into this model, so this
	// only reselects *which* clauses carry the repair — preferring the earliest.
	for (auto &c : tiebreak.obj_coeffs) {
		c = 0.0;
	}
	for (idx_t i = 0; i < slacks.size(); i++) {
		const auto &sl = slacks[i];
		double coeff = SlackTierCoefficient(sl, sl.tier) * static_cast<double>(i + 1);
		tiebreak.obj_coeffs[sl.pos_col] = coeff;
		if (sl.neg_col != DConstants::INVALID_INDEX) {
			tiebreak.obj_coeffs[sl.neg_col] = coeff;
		}
	}
	tiebreak.maximize = false;
	return true;
}

namespace {

struct RemovalGroupInfo {
	idx_t id = DConstants::INVALID_INDEX;
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	string qualifier;
};

static vector<RemovalGroupInfo> CollectRemovalGroups(const SolverModel &model) {
	std::map<idx_t, RemovalGroupInfo> by_group;
	auto record = [&](const ConstraintProvenance &prov) {
		if (prov.removal_group_id == DConstants::INVALID_INDEX) {
			return;
		}
		auto entry = by_group.emplace(
		    prov.removal_group_id,
		    RemovalGroupInfo {prov.removal_group_id, prov.source_clause_id, prov.qualifier});
		if (!entry.second) {
			if (entry.first->second.source_clause_id == DConstants::INVALID_INDEX) {
				entry.first->second.source_clause_id = prov.source_clause_id;
			}
			if (entry.first->second.qualifier.empty()) {
				entry.first->second.qualifier = prov.qualifier;
			}
		}
	};
	for (const auto &row : model.constraints) record(row.provenance);
	for (const auto &row : model.quadratic_constraints) record(row.provenance);
	for (const auto &row : model.indicator_constraints) record(row.provenance);
	for (const auto &row : model.general_constraints) record(row.provenance);
	vector<RemovalGroupInfo> result;
	for (const auto &entry : by_group) {
		result.push_back(entry.second);
	}
	return result;
}

static ClauseEdit MakeDropEdit(const SolverModel &model, const RemovalGroupInfo &group) {
	ClauseEdit edit;
	edit.kind = ClauseEditKind::DROP;
	edit.edit_source = "remove_only";
	if (group.source_clause_id < model.constraint_sources.size()) {
		const auto &source = model.constraint_sources[group.source_clause_id];
		const string &lhs = source.written_lhs.empty() ? source.canonical_lhs : source.written_lhs;
		const string &rhs = source.written_rhs.empty() ? source.canonical_rhs : source.written_rhs;
		const string &cmp = source.written_cmp.empty() ? source.canonical_cmp : source.written_cmp;
		edit.label = cmp.empty() || rhs.empty() ? lhs : lhs + " " + cmp + " " + rhs;
		const string &qualifier = source.qualifier.empty() ? group.qualifier : source.qualifier;
		if (!qualifier.empty()) {
			edit.label += " " + qualifier;
		}
	} else {
		edit.label = "constraint " + to_string(group.id);
	}
	return edit;
}

enum class CandidateStatus : uint8_t { FEASIBLE, INFEASIBLE, ABORT };

struct RemovalCandidate {
	CandidateStatus status = CandidateStatus::ABORT;
	vector<ClauseEdit> edits;
	double data_cost = 0.0;
	double editable_cost = 0.0;
	bool objective_unbounded = false;
	bool has_objective = false;
	double objective_value = 0.0;
	vector<idx_t> signature;
};

static double BudgetValue(const vector<TierBudget> &budgets, ElasticRepairTier tier) {
	for (const auto &budget : budgets) {
		if (budget.tier == tier) {
			return budget.value;
		}
	}
	return 0.0;
}

static RemovalCandidate SolveRemovalCandidate(const InfeasibleDiagnosisInput &input,
	                                            const vector<ColumnProvenance> &columns,
	                                            const vector<RemovalGroupInfo> &all_groups,
	                                            const vector<idx_t> &selected_indices) {
	RemovalCandidate result;
	std::set<idx_t> dropped_ids;
	for (idx_t index : selected_indices) {
		dropped_ids.insert(all_groups[index].id);
		result.signature.push_back(all_groups[index].id);
	}
	SolverModel candidate_model = DropRemovalGroups(input.model, dropped_ids);
	ElasticModel elastic = BuildElasticModel(candidate_model, input.params.slack_scope);
	const auto &slacks = elastic.slacks;

	SolverModel stage1_model = elastic.model;
	vector<TierBudget> budgets;
	SolverResult stage1;
	bool solved = false;
	const ElasticRepairTier tier_order[] = {
	    ElasticRepairTier::DATA_OFFSET,
	    ElasticRepairTier::EDITABLE_LOOSEN,
	};
	for (auto tier : tier_order) {
		if (!TierHasTerms(slacks, tier)) {
			continue;
		}
		SetTierObjective(stage1_model, slacks, tier);
		SolverResult pass = input.solve_model(stage1_model);
		if (pass.status == SolverStatus::INFEASIBLE) {
			result.status = input.has_unhandled_user_bounds ? CandidateStatus::ABORT
			                                                : CandidateStatus::INFEASIBLE;
			return result;
		}
		if (pass.status != SolverStatus::OPTIMAL || pass.solution.empty()) {
			return result;
		}
		TierBudget budget = MakeTierBudget(pass.solution, slacks, tier);
		AddTierBudgetRow(stage1_model, slacks, budget);
		budgets.push_back(budget);
		stage1 = std::move(pass);
		solved = true;
	}
	if (!solved) {
		SolverResult pass = input.solve_model(stage1_model);
		if (pass.status == SolverStatus::INFEASIBLE) {
			result.status = input.has_unhandled_user_bounds ? CandidateStatus::ABORT
			                                                : CandidateStatus::INFEASIBLE;
			return result;
		}
		if (pass.status != SolverStatus::OPTIMAL || pass.solution.empty()) {
			return result;
		}
		stage1 = std::move(pass);
	}

	result.data_cost = BudgetValue(budgets, ElasticRepairTier::DATA_OFFSET);
	result.editable_cost = BudgetValue(budgets, ElasticRepairTier::EDITABLE_LOOSEN);
	vector<double> repair_solution = stage1.solution;
	result.has_objective = HasObjective(candidate_model);
	if (result.has_objective) {
		SolverModel stage2_model = BuildStage2Model(elastic.model, slacks, candidate_model, budgets);
		SolverResult stage2 = input.solve_model(stage2_model);
		if (stage2.status == SolverStatus::OPTIMAL && !stage2.solution.empty()) {
			result.objective_value = stage2.objective_value;
			repair_solution = std::move(stage2.solution);
			SolverModel tiebreak_model = stage2_model;
			if (BuildTieBreakModel(tiebreak_model, slacks, /*freeze_objective=*/true,
			                       result.objective_value)) {
				SolverResult tiebreak = input.solve_model(tiebreak_model);
				if (tiebreak.status != SolverStatus::OPTIMAL || tiebreak.solution.empty()) {
					return result;
				}
				repair_solution = std::move(tiebreak.solution);
			}
		} else if (stage2.status == SolverStatus::UNBOUNDED) {
			result.objective_unbounded = true;
			SolverModel tiebreak_model = stage1_model;
			if (BuildTieBreakModel(tiebreak_model, slacks, /*freeze_objective=*/false, 0.0)) {
				SolverResult tiebreak = input.solve_model(tiebreak_model);
				if (tiebreak.status != SolverStatus::OPTIMAL || tiebreak.solution.empty()) {
					return result;
				}
				repair_solution = std::move(tiebreak.solution);
			}
		} else {
			// INF_OR_UNBD, timeout, suboptimal, and backend errors are not comparable
			// candidates. Never return a partial diagnosis from a partially searched layer.
			return result;
		}
	} else {
		SolverModel tiebreak_model = stage1_model;
		if (BuildTieBreakModel(tiebreak_model, slacks, /*freeze_objective=*/false, 0.0)) {
			SolverResult tiebreak = input.solve_model(tiebreak_model);
			if (tiebreak.status != SolverStatus::OPTIMAL || tiebreak.solution.empty()) {
				return result;
			}
			repair_solution = std::move(tiebreak.solution);
		}
	}

	result.edits = ReadElasticEdits(slacks, repair_solution, candidate_model, columns,
	                                /*snap=*/result.has_objective && !result.objective_unbounded,
	                                input.params.slack_scope);
	for (idx_t index : selected_indices) {
		result.edits.push_back(MakeDropEdit(input.model, all_groups[index]));
	}
	result.status = CandidateStatus::FEASIBLE;
	return result;
}

static int CompareDisplayed(double lhs, double rhs) {
	double a = SnapDiagnosticValue(lhs);
	double b = SnapDiagnosticValue(rhs);
	return a < b ? -1 : (a > b ? 1 : 0);
}

static bool BetterCandidate(const RemovalCandidate &lhs, const RemovalCandidate &rhs, bool maximize) {
	int cmp = CompareDisplayed(lhs.data_cost, rhs.data_cost);
	if (cmp != 0) return cmp < 0;
	cmp = CompareDisplayed(lhs.editable_cost, rhs.editable_cost);
	if (cmp != 0) return cmp < 0;
	if (lhs.objective_unbounded != rhs.objective_unbounded) return lhs.objective_unbounded;
	if (!lhs.objective_unbounded && lhs.has_objective && rhs.has_objective) {
		cmp = CompareDisplayed(lhs.objective_value, rhs.objective_value);
		if (cmp != 0) return maximize ? cmp > 0 : cmp < 0;
	}
	return std::lexicographical_compare(lhs.signature.begin(), lhs.signature.end(),
	                                    rhs.signature.begin(), rhs.signature.end());
}

static bool EnumerateCombinations(
    idx_t n, idx_t choose, idx_t start, vector<idx_t> &current,
    const std::function<bool(const vector<idx_t> &)> &visit) {
	if (current.size() == choose) {
		return visit(current);
	}
	idx_t needed = choose - current.size();
	for (idx_t i = start; i + needed <= n; i++) {
		current.push_back(i);
		if (!EnumerateCombinations(n, choose, i + 1, current, visit)) {
			return false;
		}
		current.pop_back();
	}
	return true;
}

} // namespace

DecideDiagnostic DiagnoseInfeasible(const InfeasibleDiagnosisInput &input) {
	if (!input.solve_model) {
		return DecideDiagnostic();
	}
	vector<ColumnProvenance> columns =
	    BuildColumnProvenance(input.indexer, input.var_labels, input.var_is_aux,
	                          input.global_variable_labels);
	auto unreachable = CollectUnreachableClauses(input.model, columns);
	if (!unreachable.empty()) {
		return BuildUnreachableBoundDiagnostic(unreachable);
	}

	vector<RemovalGroupInfo> groups = CollectRemovalGroups(input.model);
	if (groups.empty() && BuildElasticModel(input.model, input.params.slack_scope).slacks.empty()) {
		return DecideDiagnostic();
	}
	for (idx_t cardinality = 0; cardinality <= groups.size(); cardinality++) {
		vector<idx_t> current;
		bool found = false;
		bool aborted = false;
		RemovalCandidate best;
		EnumerateCombinations(groups.size(), cardinality, 0, current,
		                      [&](const vector<idx_t> &selection) {
			RemovalCandidate candidate = SolveRemovalCandidate(input, columns, groups, selection);
			if (candidate.status == CandidateStatus::ABORT) {
				aborted = true;
				return false;
			}
			if (candidate.status == CandidateStatus::INFEASIBLE) {
				return true;
			}
			if (!found || BetterCandidate(candidate, best, input.model.maximize)) {
				best = std::move(candidate);
				found = true;
			}
			return true;
		});
		if (aborted) {
			return DecideDiagnostic();
		}
		if (!found) {
			continue;
		}
		if (best.edits.empty()) {
			return DecideDiagnostic();
		}
		if (best.objective_unbounded) {
			return BuildInfeasibleDiagnostic(best.edits, "", /*unbounded_after_fix=*/true);
		}
		return BuildInfeasibleDiagnostic(
		    best.edits,
		    best.has_objective ? FormatNum(SnapDiagnosticValue(best.objective_value)) : string());
	}
	return input.has_unhandled_user_bounds ? DecideDiagnostic() : BuildElasticInfeasibleDiagnostic();
}

} // namespace duckdb
