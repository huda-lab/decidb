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
		    !IsUnreachableBound(row.sense, row.rhs)) {
			continue;
		}
		record(row.provenance, SourceAwareLhs(model, row, columns), row.rhs, row.sense);
	}
	for (const auto &row : model.quadratic_constraints) {
		if (row.provenance.kind == ConstraintKind::STRUCTURAL ||
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

ElasticModel BuildElasticModel(const SolverModel &base, double removal_bigm,
                               const string &slack_scope) {
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

	// Append a REAL, non-negative, uncapped slack column (sᵢ≥0 REAL even for
	// integer RHS — decision 4). The repair objective coefficient is attached later
	// by the active lexicographic tier.
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
		if (IsRelaxableForElastic(row.provenance.kind) && !is_data_offset(row.provenance)) {
			consider_ref(lin_norm[r]);
		}
	}
	for (idx_t qr = 0; qr < elastic.quadratic_constraints.size(); qr++) {
		const auto &qc = elastic.quadratic_constraints[qr];
		assert_blamable_row(qc.provenance);
		qc_norm[qr] = rms_norm(qc.linear_coefficients);
		if (IsRelaxableForElastic(qc.provenance.kind)) {
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
		if (!IsRelaxableForElastic(row.provenance.kind) || !folds(row.provenance)) {
			continue;
		}
		blocks[block_key(row.provenance)].push_back(r);
	}

	// Emit in row order: a shared block is emitted once, at its first row (its rows
	// are collected in ascending order, so `front()` is the first we reach); every
	// other relaxable row gets its own size-1 block.
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		const auto &row = elastic.constraints[r];
		if (!IsRelaxableForElastic(row.provenance.kind)) {
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
		if (!IsRelaxableForElastic(qc.provenance.kind)) {
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

	// I4: the removal dial. A remove-only `<>` cannot be loosened — its two Big-M
	// disjunction rows (USER_MECHANISM, so the loosening passes above skipped them)
	// share one `indicator_col`. Group them by it and add ONE binary `w` per `<>`,
	// wired into each row with a ±M₂ coefficient (sign by sense, like a slack) so
	// w=1 makes both rows vacuous — the clause is dropped. The removal tier minimizes
	// Σw before any data/editable tier, so dropping is a last resort without a fixed
	// cross-tier weight. M₂ defaults to the clause's own disjunction Big-M (|row coeff
	// on indicator_col|, provably enough to neutralize either side), overridable.
	//
	// A `<>` the backend expressed natively has no Big-M rows at all — it is two
	// indicator constraints instead. Those still carry a ROW (that is why `<>` is
	// expressed as indicator constraints and not as a general constraint, which
	// carries none), so the same dial reaches them: `w` wired into an implied row makes
	// that row vacuous whenever the implication fires, which is exactly what dropping
	// the clause means. Indicator rows are addressed by `constraints.size() + i` so one
	// group can hold both encodings without the loop below caring which it has.
	// The two encodings store the same four things in different structs, so the dial
	// addresses a row through this view rather than knowing which list it came from.
	struct RemovableRow {
		vector<int> *indices;
		vector<double> *coefficients;
		char sense;
		double rhs;
		const vector<double> *col_lower;
		const vector<double> *col_upper;
	};
	auto row_at = [&elastic](idx_t r) -> RemovableRow {
		if (r < elastic.constraints.size()) {
			auto &row = elastic.constraints[r];
			return {&row.indices, &row.coefficients, row.sense, row.rhs, &elastic.col_lower,
			        &elastic.col_upper};
		}
		auto &row = elastic.indicator_constraints[r - elastic.constraints.size()];
		return {&row.indices, &row.coefficients, row.sense, row.rhs, &elastic.col_lower,
		        &elastic.col_upper};
	};
	std::map<idx_t, vector<idx_t>> rem_groups;
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		idx_t ind = elastic.constraints[r].provenance.indicator_col;
		if (ind != DConstants::INVALID_INDEX) {
			rem_groups[ind].push_back(r);
		}
	}
	for (idx_t i = 0; i < elastic.indicator_constraints.size(); i++) {
		idx_t ind = elastic.indicator_constraints[i].provenance.indicator_col;
		if (ind != DConstants::INVALID_INDEX) {
			rem_groups[ind].push_back(elastic.constraints.size() + i);
		}
	}
	for (auto &kv : rem_groups) {
		idx_t indicator_col = kv.first;
		const vector<idx_t> &grp = kv.second;
		// M₂: honor the pragma override, else derive from the disjunction Big-M.
		double m2 = removal_bigm;
		if (!(m2 > 0.0)) {
			for (idx_t r : grp) {
				auto row = row_at(r);
				for (idx_t k = 0; k < row.indices->size(); k++) {
					if (static_cast<idx_t>((*row.indices)[k]) == indicator_col) {
						m2 = std::max(m2, std::fabs((*row.coefficients)[k]));
					}
				}
			}
		}
		// A `<>` whose range collapsed to a plain inequality carries the same
		// provenance but holds no indicator term to read an M from, so derive one from
		// the row itself: enough to dominate the row's reachable span plus its bound is
		// enough to make it vacuous, which is all the removal dial needs. Without this
		// the group would get a coefficient of 0 and w=1 would neutralize nothing —
		// a removal offered in the diagnosis but inert in the model.
		if (!(m2 > 0.0)) {
			for (idx_t r : grp) {
				auto row = row_at(r);
				double span = std::fabs(row.rhs) + 1.0;
				for (idx_t k = 0; k < row.indices->size(); k++) {
					idx_t col = static_cast<idx_t>((*row.indices)[k]);
					span += std::fabs((*row.coefficients)[k]) *
					        std::max(std::fabs((*row.col_lower)[col]), std::fabs((*row.col_upper)[col]));
				}
				m2 = std::max(m2, span);
			}
		}
		// One binary removal indicator w ∈ {0,1}.
		idx_t w_col = elastic.num_vars;
		elastic.col_lower.push_back(0.0);
		elastic.col_upper.push_back(1.0);
		elastic.is_integer.push_back(true);
		elastic.is_binary.push_back(true);
		elastic.obj_coeffs.push_back(0.0);
		elastic.num_vars++;
		// Wire ±M₂·w into each disjunction row (same sign convention as a slack).
		for (idx_t r : grp) {
			auto row = row_at(r);
			row.indices->push_back(static_cast<int>(w_col));
			row.coefficients->push_back(row.sense == '>' ? m2 : -m2);
		}
		out.removals.push_back({grp, w_col, indicator_col});
	}

	return out;
}

//! Read the slack support of a solved elastic model into the user-facing edit list.
//! Every block whose slack exceeds DIAGNOSTIC_RAY_EPSILON is an edit; the amount is the
//! slack value (`=` reports the net s⁺ − s⁻). A data-RHS clause (`x <= col`) has no
//! single literal to loosen, so it reads back per the `slack_scope` policy (T3): "query"
//! folds it into one virtual offset `x <= col + delta`; "expanded" exposes each per-row
//! slack. Clauses are labelled from the ORIGINAL row (`orig`, before slacks were
//! appended). Pure in `solution`, so it runs against either the stage-1 or stage-2 solve.
static vector<ClauseEdit> ReadElasticEdits(const vector<BlockSlackRef> &slacks,
                                           const vector<RemovalRef> &removals,
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
		if (std::fabs(amount) <= DIAGNOSTIC_RAY_EPSILON) {
			continue;
		}
		// I3: snap the stage-2 read to absorb the budget cushion's sub-display noise; the
		// stage-1 read is exact (no budget constraint) and passes through untouched.
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

	// I4: a removal indicator at 1 means its remove-only `<>` was dropped. The clause
	// label comes from the indicator column's provenance ("(x <> 3)", recorded at
	// rewrite time via F6). The set {w = 1} is the minimum-cardinality removal set.
	// A single written `<>` that expands per row has one indicator (hence one removal)
	// per row, all sharing the same clause label; dedupe by label so the relation
	// carries one DROP per user clause, not one per row.
	std::set<string> dropped_labels;
	for (const auto &r : removals) {
		if (solution[r.w_col] > 0.5) {
			const string &label = columns[r.indicator_col].label;
			if (!dropped_labels.insert(label).second) {
				continue;
			}
			ClauseEdit e;
			e.kind = ClauseEditKind::DROP;
			e.label = label;
			e.edit_source = "remove_only";
			edits.push_back(std::move(e));
		}
	}
	return edits;
}

//! True iff `edits` contains at least one DROP (a dropped remove-only `<>`). Gates the
//! I3 stage-2 re-solve alongside HasLoosenEdit: a removal is an actionable fix, so the
//! achievable objective after dropping is worth reporting.
static bool HasRemoval(const vector<ClauseEdit> &edits) {
	for (const auto &e : edits) {
		if (e.kind == ClauseEditKind::DROP) {
			return true;
		}
	}
	return false;
}

//! True iff `edits` contains at least one actionable (editable) LOOSEN edit, as opposed
//! to only data-RHS conflict summaries. Gates the I3 stage-2 re-solve: with no editable
//! knob there is nothing to maximize the objective over, so an achievable-objective
//! number would be misleading.
static bool HasLoosenEdit(const vector<ClauseEdit> &edits) {
	for (const auto &e : edits) {
		if (e.kind == ClauseEditKind::LOOSEN) {
			return true;
		}
	}
	return false;
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
                            const vector<RemovalRef> &removals, ElasticRepairTier tier) {
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
	if (tier == ElasticRepairTier::REMOVAL) {
		for (const auto &r : removals) {
			row.indices.push_back(static_cast<int>(r.w_col));
			row.coefficients.push_back(1.0);
		}
	}
	return !row.indices.empty();
}

//! True iff `tier` owns at least one repair knob. An empty tier contributes nothing to the
//! lexicographic ladder, so its pass is skipped entirely (no solve, no budget row).
static bool TierHasTerms(const vector<BlockSlackRef> &slacks, const vector<RemovalRef> &removals,
                         ElasticRepairTier tier) {
	if (tier == ElasticRepairTier::REMOVAL) {
		return !removals.empty();
	}
	for (const auto &sl : slacks) {
		if (sl.tier == tier) {
			return true;
		}
	}
	return false;
}

static void SetTierObjective(SolverModel &model, const vector<BlockSlackRef> &slacks,
                             const vector<RemovalRef> &removals, ElasticRepairTier tier) {
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
	if (!AppendTierTerms(objective_terms, slacks, removals, tier)) {
		return;
	}
	for (idx_t i = 0; i < objective_terms.indices.size(); i++) {
		idx_t col = static_cast<idx_t>(objective_terms.indices[i]);
		model.obj_coeffs[col] = objective_terms.coefficients[i];
	}
}

static double ComputeTierValue(const vector<double> &solution, const vector<BlockSlackRef> &slacks,
                               const vector<RemovalRef> &removals, ElasticRepairTier tier) {
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
	if (tier == ElasticRepairTier::REMOVAL) {
		for (const auto &r : removals) {
			value += solution[r.w_col];
		}
	}
	return std::max(0.0, value);
}

static TierBudget MakeTierBudget(const vector<double> &solution, const vector<BlockSlackRef> &slacks,
                                 const vector<RemovalRef> &removals, ElasticRepairTier tier) {
	ModelConstraint terms;
	bool active = AppendTierTerms(terms, slacks, removals, tier);
	return {tier, active ? ComputeTierValue(solution, slacks, removals, tier) : 0.0, active};
}

static void AddTierBudgetRow(SolverModel &model, const vector<BlockSlackRef> &slacks,
                             const vector<RemovalRef> &removals, const TierBudget &budget) {
	if (!budget.active) {
		return;
	}
	ModelConstraint row;
	if (!AppendTierTerms(row, slacks, removals, budget.tier)) {
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
                                    const vector<RemovalRef> &removals,
                                    const SolverModel &original, const vector<TierBudget> &budgets) {
	SolverModel stage2 = elastic;

	// Freeze the lexicographic repair budgets, not individual repair variables.
	// For remove-only `<>` clauses this keeps R <= R* while letting stage 2 choose
	// the objective-best equally minimal DROP set.
	for (const auto &budget : budgets) {
		AddTierBudgetRow(stage2, slacks, removals, budget);
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
                               const vector<RemovalRef> &removals, bool freeze_objective,
                               double objective_value) {
	idx_t editable_knobs = 0;
	idx_t data_knobs = 0;
	for (const auto &sl : slacks) {
		(sl.tier == ElasticRepairTier::DATA_OFFSET ? data_knobs : editable_knobs)++;
	}
	if (removals.size() < 2 && editable_knobs < 2 && data_knobs < 2) {
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
	for (idx_t i = 0; i < removals.size(); i++) {
		tiebreak.obj_coeffs[removals[i].w_col] = static_cast<double>(i + 1);
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

DecideDiagnostic DiagnoseInfeasible(const InfeasibleDiagnosisInput &input) {
	// Build the elastic program — give every relaxable user constraint a non-negative
	// slack that lets its RHS stretch, and every remove-only `<>` a drop switch. Stage 1
	// then solves the repair objective lexicographically: minimize removals, then data
	// offsets, then editable loosening.
	if (!input.solve_model) {
		return DecideDiagnostic();
	}

	// Label the model's columns once: the unreachable-bound scan below and the elastic
	// readback further down both name clauses over the same user-facing column names.
	vector<ColumnProvenance> columns =
	    BuildColumnProvenance(input.indexer, input.var_labels, input.var_is_aux,
	                          input.global_variable_labels);

	// A bound no assignment can reach (`x >= inf`) is infeasible on its own, and the
	// elastic engine structurally cannot say so: `wire()` puts the slack on the LHS
	// (`Ax − s ≤ b`), so `b` is never touched and no finite `s` repairs the row. Left to
	// the solve it either saturates the slack at the 1e30 sentinel and hands back the
	// user's own text as the "fix", or returns elastic-infeasible and names nothing.
	// Recognize it here instead, before the elastic model exists — the finding is the
	// clause, and there is no edit to offer.
	auto unreachable = CollectUnreachableClauses(input.model, columns);
	if (!unreachable.empty()) {
		return BuildUnreachableBoundDiagnostic(unreachable);
	}

	ElasticModel elastic =
	    BuildElasticModel(input.model, input.params.removal_bigm, input.params.slack_scope);
	const vector<BlockSlackRef> &slacks = elastic.slacks;

	// Nothing actionable: fall through to the static error (the user's only constraints
	// are rigid structural rows we never edit). I4: a model with removable `<>` rows but
	// no loosenable slacks is still actionable — only bail when BOTH are empty.
	if (slacks.empty() && elastic.removals.empty()) {
		return DecideDiagnostic();
	}

	SolverModel stage1_model = elastic.model;
	vector<TierBudget> budgets;
	SolverResult stage1;
	const ElasticRepairTier tier_order[] = {
	    ElasticRepairTier::REMOVAL,
	    ElasticRepairTier::DATA_OFFSET,
	    ElasticRepairTier::EDITABLE_LOOSEN,
	};
	for (auto tier : tier_order) {
		// Skip a tier with no repair knobs: an empty pass would solve a zero-objective
		// feasibility problem whose result we discard. At least one tier is always
		// populated (we returned early when both slacks and removals are empty), so the
		// first surviving pass still detects an elastic-infeasible conflict.
		if (!TierHasTerms(slacks, elastic.removals, tier)) {
			continue;
		}
		SetTierObjective(stage1_model, slacks, elastic.removals, tier);
		SolverResult pass = input.solve_model(stage1_model);

		if (pass.status == SolverStatus::INFEASIBLE) {
			// The elastic program is itself infeasible: the conflict reaches rigid rows.
			// Only claim that when every user constraint was actually made relaxable — if
			// the operator punted a multi-instance bound (I2 scope), stay honest and fall
			// through to the static error rather than wrongly declaring it unfixable.
			if (input.has_unhandled_user_bounds) {
				return DecideDiagnostic();
			}
			return BuildElasticInfeasibleDiagnostic();
		}
		if (pass.status != SolverStatus::OPTIMAL || pass.solution.empty()) {
			return DecideDiagnostic();
		}

		TierBudget budget = MakeTierBudget(pass.solution, slacks, elastic.removals, tier);
		AddTierBudgetRow(stage1_model, slacks, elastic.removals, budget);
		budgets.push_back(budget);
		stage1 = std::move(pass);
	}

	// Read the final lexicographic pass's repair support. Label clauses over user-facing
	// column names (global_variable_labels names aggregate `<>` indicators so a dropped
	// one is named).
	vector<ClauseEdit> edits = ReadElasticEdits(slacks, elastic.removals, stage1.solution,
	                                            input.model, columns, /*snap=*/false,
	                                            input.params.slack_scope);

	if (edits.empty()) {
		return DecideDiagnostic();
	}

	// I3 — stage-2 freeze-budget re-solve. Only when there is a real objective and a
	// reported repair. Among ALL lexicographically minimal fixes (tier budgets frozen),
	// re-solve the user's objective and report the best one — the stage-2 slacks become
	// the edit so the edit and the objective agree.
	if (HasObjective(input.model) && (HasLoosenEdit(edits) || HasRemoval(edits))) {
		SolverModel stage2_model = BuildStage2Model(elastic.model, slacks, elastic.removals,
		                                            input.model, budgets);
		SolverResult stage2 = input.solve_model(stage2_model);

		if (stage2.status == SolverStatus::OPTIMAL && !stage2.solution.empty()) {
			// The objective-maximizing minimal fix and the objective it achieves.
			double achievable = stage2.objective_value;
			vector<double> repair_solution = std::move(stage2.solution);

			// Stage-2b: when the objective ties between equally-minimal repairs (DROP sets
			// or loosen edits), reselect the repair by source order so both backends name
			// the same clause. Keep the stage-2a achievable objective for reporting (the
			// tie-break's own objective is just the rank-weighted repair sum). Falls back
			// to the stage-2a repair on skip or non-optimal.
			SolverModel tiebreak_model = stage2_model;
			if (BuildTieBreakModel(tiebreak_model, slacks, elastic.removals,
			                       /*freeze_objective=*/true, achievable)) {
				SolverResult tiebreak = input.solve_model(tiebreak_model);
				if (tiebreak.status == SolverStatus::OPTIMAL && !tiebreak.solution.empty()) {
					repair_solution = std::move(tiebreak.solution);
				}
			}

			edits = ReadElasticEdits(slacks, elastic.removals, repair_solution, input.model, columns,
			                         /*snap=*/true, input.params.slack_scope);
			if (edits.empty()) {
				return DecideDiagnostic();
			}
			return BuildInfeasibleDiagnostic(edits, FormatNum(SnapDiagnosticValue(achievable)));
		}
		if (stage2.status == SolverStatus::UNBOUNDED || stage2.status == SolverStatus::INF_OR_UNBD) {
			// The minimal fix is valid but the relaxed problem has no finite optimum.
			// Keep the stage-1 edit and say so.
			return BuildInfeasibleDiagnostic(edits, /*achievable_objective=*/"",
			                                 /*unbounded_after_fix=*/true);
		}
		// Defensive: the stage-1 point satisfies the budget by construction, so stage 2
		// is feasible. On any other status fall back to the stage-1 edit, no objective.
	} else {
		// No stage 2 (no objective, or a data-only repair): a tie between equally-minimal
		// repairs is still solver-arbitrary, so run the same source-order tie-break
		// directly on the budget-frozen stage-1 model (every active tier's budget row was
		// appended as the lexicographic ladder ran). Falls back to the stage-1 edit on
		// skip or non-optimal.
		SolverModel tiebreak_model = stage1_model;
		if (BuildTieBreakModel(tiebreak_model, slacks, elastic.removals,
		                       /*freeze_objective=*/false, 0.0)) {
			SolverResult tiebreak = input.solve_model(tiebreak_model);
			if (tiebreak.status == SolverStatus::OPTIMAL && !tiebreak.solution.empty()) {
				vector<ClauseEdit> tie_edits =
				    ReadElasticEdits(slacks, elastic.removals, tiebreak.solution, input.model,
				                     columns, /*snap=*/false, input.params.slack_scope);
				if (!tie_edits.empty()) {
					edits = std::move(tie_edits);
				}
			}
		}
	}
	return BuildInfeasibleDiagnostic(edits);
}

} // namespace duckdb
