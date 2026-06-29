#include "duckdb/decidb/decide_diagnostic_engines.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/decidb/diagnostic_constants.hpp"

#include <cmath>
#include <cstdio>
#include <map>

namespace duckdb {

namespace {

struct VarAgg {
	string name;
	bool is_aux = false;
	string direction;
	idx_t vidx = DConstants::INVALID_INDEX;
	std::set<idx_t> instances;
};

//! Compact numeric formatting for user-facing constraint text (drops trailing
//! zeros: 12.5 not 12.500000, 10 not 10.0).
string FormatNum(double v) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%.10g", v);
	return string(buf);
}

//! Round `v` to `sig` significant figures. Used to clean the I3 stage-2 reads: the
//! budget freeze is enforced only to the backend feasibility tolerance, so a maximizing
//! re-solve rides that tolerance and a clean "10" would otherwise print as "10.000001".
//! Six figures sit safely below the feasibility tolerance (≈1e-6) for amounts of order
//! one and larger.
double RoundToSignificant(double v, int sig) {
	if (v == 0.0 || !std::isfinite(v)) {
		return v;
	}
	double mag = std::pow(10.0, sig - 1 - std::floor(std::log10(std::fabs(v))));
	return std::round(v * mag) / mag;
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

//! AVG rendering: an AVG→SUM-rewritten row stores coefficients pre-scaled by 1/N_g
//! (`provenance.avg_scaled`), so a plain reconstruction would read `0.5*x + 0.5*x`.
//! Recover `AVG(<inner>)` by collapsing the N equal-coefficient terms of each
//! variable back to one (inner coeff = stored coeff × term count = the user's
//! coefficient). Returns false (caller falls back to the raw reconstruction) for a
//! data-varying coefficient (`AVG(price * x)`), where no single literal coefficient
//! exists to re-quote.
bool FormatAvgLhs(const vector<int> &indices, const vector<double> &coeffs,
                  const vector<ColumnProvenance> &cols, string &out) {
	vector<idx_t> order; // distinct variable keys, first-seen order
	std::map<idx_t, double> coeff_of;
	std::map<idx_t, idx_t> count_of;
	std::map<idx_t, bool> uniform;
	std::map<idx_t, string> label_of;
	for (idx_t i = 0; i < indices.size(); i++) {
		int col = indices[i];
		idx_t key = (col >= 0 && static_cast<idx_t>(col) < cols.size())
		                ? cols[col].decide_var_idx
		                : DConstants::INVALID_INDEX;
		double c = coeffs[i];
		auto it = coeff_of.find(key);
		if (it == coeff_of.end()) {
			coeff_of[key] = c;
			count_of[key] = 1;
			uniform[key] = true;
			label_of[key] = ColLabel(cols, col);
			order.push_back(key);
		} else {
			if (std::fabs(it->second - c) > 1e-12) {
				uniform[key] = false;
			}
			count_of[key]++;
		}
	}
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

//! AVG-aware LHS for a linear constraint row.
string FormatLhs(const ModelConstraint &row, const vector<ColumnProvenance> &cols) {
	string avg;
	if (row.provenance.avg_scaled && FormatAvgLhs(row.indices, row.coefficients, cols, avg)) {
		return avg;
	}
	return FormatTerms(row.indices, row.coefficients, cols);
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

const char *SenseStr(char sense) {
	return sense == '<' ? "<=" : (sense == '>' ? ">=" : "==");
}

//! Build a LOOSEN edit from a constraint's rendered LHS + sense + RHS and the
//! solved slack `amount` (signed; for `=` it is the net s⁺−s⁻). For a strict `<` /
//! `>` the δ offset was baked into `rhs` at build time, so re-quote the suggestion
//! against the user's typed literal (`typed_k`) and render `<` / `>` (I2.d).
ClauseEdit MakeLoosenEdit(const ConstraintProvenance &prov, const string &lhs, double rhs,
                          char sense, double amount) {
	bool strict = prov.strict && sense != '=';
	string sense_str = strict ? (sense == '>' ? ">" : "<") : SenseStr(sense);
	double base_rhs = strict ? prov.typed_k : rhs;
	// `≥` loosens downward (b − s), `≤` / `=` upward (b + s).
	double new_rhs = (sense == '>') ? base_rhs - amount : base_rhs + amount;
	ClauseEdit e;
	e.kind = ClauseEditKind::LOOSEN;
	e.label = lhs + " " + sense_str + " " + FormatNum(base_rhs);
	e.suggestion = lhs + " " + sense_str + " " + FormatNum(new_rhs);
	e.amount = FormatNum(std::fabs(amount));
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
			agg.direction = rv > 0 ? "+inf" : "-inf";
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
		ve.is_entity_scoped = input.indexer.is_entity_scoped[agg.vidx];
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

ElasticModel BuildElasticModel(const SolverModel &base, double removal_bigm) {
	ElasticModel out;
	out.model = base;
	SolverModel &elastic = out.model;

	// Rebuild the objective as min Σ sᵢ: zero the user's objective over the existing
	// columns first (slack obj coeffs are appended as the slacks are added), and drop
	// any quadratic objective — stage 1 only minimizes loosening.
	for (auto &c : elastic.obj_coeffs) {
		c = 0.0;
	}
	elastic.q_rows.clear();
	elastic.q_cols.clear();
	elastic.q_vals.clear();
	elastic.has_quadratic_obj = false;
	elastic.nonconvex_quadratic = false;
	elastic.maximize = false;

	// Append a REAL, non-negative, uncapped slack column with the given objective
	// weight (sᵢ≥0 REAL even for integer RHS — decision 4). Editable knobs use
	// weight 1; data-RHS slacks are penalized so the solver prefers an actionable
	// edit (DIAGNOSTIC_DATA_SLACK_WEIGHT).
	auto add_slack = [&](double weight) -> idx_t {
		idx_t c = elastic.num_vars;
		elastic.col_lower.push_back(0.0);
		elastic.col_upper.push_back(1e30);
		elastic.is_integer.push_back(false);
		elastic.is_binary.push_back(false);
		elastic.obj_coeffs.push_back(weight);
		elastic.num_vars++;
		return c;
	};

	// A relaxable row whose RHS is per-row data (`x <= col`) cannot be edited by the
	// user; loosening it is a conflict, not a fix. Penalize its slack so editable
	// constraints loosen first (I2.c). Shared/aggregate/literal knobs are editable.
	auto slack_weight = [](const ConstraintProvenance &p) {
		return (p.shape == ElasticShape::PER_ROW_DATA && p.clause_id != DConstants::INVALID_INDEX)
		           ? DIAGNOSTIC_DATA_SLACK_WEIGHT
		           : 1.0;
	};

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
	// I2.a: SHARED_LITERAL rows of one (clause_id, group_key) share ONE slack — the
	// user knob fans into N rows, so the minimal loosening is the max overshoot (a
	// single `s` with `eᵢ − s ≤ K` is driven to the max), not the sum. PER_ROW_DATA
	// rows (and any without a clause id) stay independent, one slack each (I1).
	auto is_shared = [](const ConstraintProvenance &p) {
		return p.shape == ElasticShape::SHARED_LITERAL && p.clause_id != DConstants::INVALID_INDEX;
	};
	std::map<std::pair<idx_t, idx_t>, vector<idx_t>> blocks;
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		const auto &row = elastic.constraints[r];
		if (!IsRelaxableForElastic(row.provenance.kind) || !is_shared(row.provenance)) {
			continue;
		}
		blocks[{row.provenance.clause_id, row.provenance.group_key}].push_back(r);
	}

	// Emit in row order: a shared block is emitted once, at its first row (its rows
	// are collected in ascending order, so `front()` is the first we reach); every
	// other relaxable row gets its own size-1 block.
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		const auto &row = elastic.constraints[r];
		if (!IsRelaxableForElastic(row.provenance.kind)) {
			continue;
		}
		if (is_shared(row.provenance)) {
			const vector<idx_t> &grp = blocks[{row.provenance.clause_id, row.provenance.group_key}];
			if (grp.front() != r) {
				continue; // already emitted at the block's first row
			}
			// A shared block is one editable literal knob → weight 1.
			char sense = elastic.constraints[grp.front()].sense;
			idx_t pos_col = add_slack(1.0);
			idx_t neg_col = (sense == '=') ? add_slack(1.0) : DConstants::INVALID_INDEX;
			for (idx_t rr : grp) {
				wire(rr, pos_col, neg_col);
			}
			out.slacks.push_back({grp, pos_col, neg_col, sense});
		} else {
			char sense = row.sense;
			double w = slack_weight(row.provenance);
			idx_t pos_col = add_slack(w);
			idx_t neg_col = (sense == '=') ? add_slack(w) : DConstants::INVALID_INDEX;
			wire(r, pos_col, neg_col);
			out.slacks.push_back({{r}, pos_col, neg_col, sense});
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
		// (weight 1), not penalized as data.
		char sense = qc.sense;
		idx_t pos_col = add_slack(1.0);
		idx_t neg_col = (sense == '=') ? add_slack(1.0) : DConstants::INVALID_INDEX;
		qc.linear_indices.push_back(static_cast<int>(pos_col));
		qc.linear_coefficients.push_back(sense == '>' ? 1.0 : -1.0);
		if (sense == '=') {
			qc.linear_indices.push_back(static_cast<int>(neg_col));
			qc.linear_coefficients.push_back(1.0);
		}
		out.slacks.push_back({{qr}, pos_col, neg_col, sense, /*quadratic=*/true});
	}

	// I4: the removal dial. A remove-only `<>` cannot be loosened — its two Big-M
	// disjunction rows (USER_MECHANISM, so the loosening passes above skipped them)
	// share one `indicator_col`. Group them by it and add ONE binary `w` per `<>`,
	// wired into each row with a ±M₂ coefficient (sign by sense, like a slack) so
	// w=1 makes both rows vacuous — the clause is dropped. Penalize W·w
	// (DIAGNOSTIC_REMOVAL_WEIGHT, above the slack weights) so dropping is a last
	// resort. M₂ defaults to the clause's own disjunction Big-M (|row coeff on
	// indicator_col|, provably enough to neutralize either side), overridable.
	std::map<idx_t, vector<idx_t>> rem_groups;
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		idx_t ind = elastic.constraints[r].provenance.indicator_col;
		if (ind != DConstants::INVALID_INDEX) {
			rem_groups[ind].push_back(r);
		}
	}
	for (auto &kv : rem_groups) {
		idx_t indicator_col = kv.first;
		const vector<idx_t> &grp = kv.second;
		// M₂: honor the pragma override, else derive from the disjunction Big-M.
		double m2 = removal_bigm;
		if (!(m2 > 0.0)) {
			for (idx_t r : grp) {
				const auto &row = elastic.constraints[r];
				for (idx_t k = 0; k < row.indices.size(); k++) {
					if (static_cast<idx_t>(row.indices[k]) == indicator_col) {
						m2 = std::max(m2, std::fabs(row.coefficients[k]));
					}
				}
			}
		}
		// One binary removal indicator w ∈ {0,1}, penalized W.
		idx_t w_col = elastic.num_vars;
		elastic.col_lower.push_back(0.0);
		elastic.col_upper.push_back(1.0);
		elastic.is_integer.push_back(true);
		elastic.is_binary.push_back(true);
		elastic.obj_coeffs.push_back(DIAGNOSTIC_REMOVAL_WEIGHT);
		elastic.num_vars++;
		// Wire ±M₂·w into each disjunction row (same sign convention as a slack).
		for (idx_t r : grp) {
			auto &row = elastic.constraints[r];
			row.indices.push_back(static_cast<int>(w_col));
			row.coefficients.push_back(row.sense == '>' ? m2 : -m2);
		}
		out.removals.push_back({grp, w_col, indicator_col});
	}

	return out;
}

//! Read the slack support of a solved elastic model into the user-facing edit list.
//! Every block whose slack exceeds DIAGNOSTIC_RAY_EPSILON is an edit; the amount is the
//! slack value (`=` reports the net s⁺ − s⁻). A data-RHS clause (`x <= col`) has no
//! single literal to loosen, so its rows are collapsed into one CONFLICT_SUMMARY per
//! clause (I2.c). Clauses are labelled from the ORIGINAL row (`orig`, before slacks were
//! appended). Pure in `solution`, so it runs against either the stage-1 or stage-2 solve.
static vector<ClauseEdit> ReadElasticEdits(const vector<BlockSlackRef> &slacks,
                                           const vector<RemovalRef> &removals,
                                           const vector<double> &solution,
                                           const SolverModel &orig_model,
                                           const vector<ColumnProvenance> &columns, bool snap) {
	struct ConflictAccum {
		string lhs_sense_rhs; // the clause as written, from its first conflicting row
		idx_t conflicting = 0;
	};
	std::map<idx_t, ConflictAccum> conflicts; // clause_id → accumulator

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
			amount = RoundToSignificant(amount, 6);
		}
		// Label the clause from the block's representative ORIGINAL row (orig_model,
		// before slacks were appended). For a shared-slack block all rows render the
		// same LHS/RHS, so the first row is canonical.
		if (sl.quadratic) {
			const auto &orig = orig_model.quadratic_constraints[sl.rows.front()];
			edits.push_back(MakeLoosenEdit(orig.provenance, FormatQuadraticLhs(orig, columns),
			                               orig.rhs, sl.sense, amount));
			continue;
		}
		const ModelConstraint &orig = orig_model.constraints[sl.rows.front()];
		const ConstraintProvenance &prov = orig.provenance;
		bool data_rhs = prov.shape == ElasticShape::PER_ROW_DATA &&
		                prov.clause_id != DConstants::INVALID_INDEX;
		if (data_rhs) {
			auto &acc = conflicts[prov.clause_id];
			if (acc.conflicting == 0) {
				acc.lhs_sense_rhs =
				    FormatLhs(orig, columns) + " " + SenseStr(sl.sense) + " " + FormatNum(orig.rhs);
			}
			acc.conflicting++;
			continue;
		}
		edits.push_back(MakeLoosenEdit(prov, FormatLhs(orig, columns), orig.rhs, sl.sense, amount));
	}

	// Emit one conflict summary per data-RHS clause. N = total rows the clause
	// emitted (not just the conflicting ones), counted over the original model.
	for (auto &kv : conflicts) {
		idx_t clause_id = kv.first;
		idx_t total = 0;
		for (const auto &row : orig_model.constraints) {
			if (row.provenance.clause_id == clause_id) {
				total++;
			}
		}
		ClauseEdit e;
		e.kind = ClauseEditKind::CONFLICT_SUMMARY;
		e.label = kv.second.lhs_sense_rhs;
		e.detail = "conflicts in " + std::to_string(kv.second.conflicting) + " of " +
		           std::to_string(total) + " rows";
		edits.push_back(std::move(e));
	}

	// I4: a removal indicator at 1 means its remove-only `<>` was dropped. The clause
	// label comes from the indicator column's provenance ("(x <> 3)", recorded at
	// rewrite time via F6). The set {w = 1} is the minimum-cardinality removal set.
	for (const auto &r : removals) {
		if (solution[r.w_col] > 0.5) {
			ClauseEdit e;
			e.kind = ClauseEditKind::DROP;
			e.label = columns[r.indicator_col].label;
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

//! Stage-2 freeze-budget model (I3): start from the stage-1 elastic model (slacks
//! already wired), cap the total loosening at S*, and restore the user's original
//! objective so the re-solve maximizes it among all minimal-loosening fixes. The budget
//! row reuses the stage-1 slack weights — read back from the elastic model's objective
//! coefficients — so it freezes exactly the stage-1 objective ≤ S*.
static SolverModel BuildStage2Model(const SolverModel &elastic, const vector<BlockSlackRef> &slacks,
                                    const vector<RemovalRef> &removals,
                                    const vector<double> &stage1_solution, const SolverModel &original,
                                    double s_star) {
	SolverModel stage2 = elastic;

	// I4: freeze the removal set. Pin each removal binary to its stage-1 value (0/1)
	// so stage 2 cannot pick a different set of clauses to drop — the reported DROP set
	// is stable between stages. The fixed W·w terms then become a constant in the budget.
	for (const auto &r : removals) {
		double w = stage1_solution[r.w_col] > 0.5 ? 1.0 : 0.0;
		stage2.col_lower[r.w_col] = w;
		stage2.col_upper[r.w_col] = w;
	}

	// Budget row: Σ wᵢ sᵢ + Σ W·wⱼ ≤ S*. Build it while the elastic objective coefficients
	// still hold the slack/removal weights (editable 1, data DIAGNOSTIC_DATA_SLACK_WEIGHT,
	// removal DIAGNOSTIC_REMOVAL_WEIGHT). Including the (now-fixed) removal columns keeps the
	// cap exactly equal to the stage-1 objective S* = Σ wᵢ sᵢ + Σ W·wⱼ, so the loosening
	// budget is frozen even though the objective included the removal penalty. The cap is
	// exactly S*: the stage-1 optimum sits on it, and the backend feasibility tolerance
	// supplies the cushion that keeps the re-solve feasible. The reported amounts are
	// snapped past that tolerance — see RoundToSignificant.
	ModelConstraint budget;
	for (const auto &sl : slacks) {
		budget.indices.push_back(static_cast<int>(sl.pos_col));
		budget.coefficients.push_back(elastic.obj_coeffs[sl.pos_col]);
		if (sl.neg_col != DConstants::INVALID_INDEX) {
			budget.indices.push_back(static_cast<int>(sl.neg_col));
			budget.coefficients.push_back(elastic.obj_coeffs[sl.neg_col]);
		}
	}
	for (const auto &r : removals) {
		budget.indices.push_back(static_cast<int>(r.w_col));
		budget.coefficients.push_back(elastic.obj_coeffs[r.w_col]);
	}
	budget.sense = '<';
	budget.rhs = s_star;
	budget.provenance.kind = ConstraintKind::STRUCTURAL; // rigid: never slacked or edited

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

	stage2.constraints.push_back(std::move(budget));
	return stage2;
}

DecideDiagnostic DiagnoseInfeasible(const InfeasibleDiagnosisInput &input) {
	// Build the elastic program — give every relaxable user constraint a
	// non-negative slack that lets its RHS stretch, minimize the total loosening,
	// and read which constraints to loosen (the positive-slack support) and by how
	// much. The minimal edit list is the least-change fix.
	if (!input.solve_model) {
		return DecideDiagnostic();
	}

	ElasticModel elastic = BuildElasticModel(input.model, input.params.removal_bigm);
	const vector<BlockSlackRef> &slacks = elastic.slacks;

	// Nothing actionable: fall through to the static error (the user's only constraints
	// are rigid structural rows we never edit). I4: a model with removable `<>` rows but
	// no loosenable slacks is still actionable — only bail when BOTH are empty.
	if (slacks.empty() && elastic.removals.empty()) {
		return DecideDiagnostic();
	}

	SolverResult stage1 = input.solve_model(elastic.model);

	if (stage1.status == SolverStatus::INFEASIBLE) {
		// The elastic program is itself infeasible: the conflict reaches rigid rows.
		// Only claim that when every user constraint was actually made relaxable — if
		// the operator punted a multi-instance bound (I2 scope), stay honest and fall
		// through to the static error rather than wrongly declaring it unfixable.
		if (input.has_unhandled_user_bounds) {
			return DecideDiagnostic();
		}
		return BuildElasticInfeasibleDiagnostic();
	}
	if (stage1.status != SolverStatus::OPTIMAL || stage1.solution.empty()) {
		return DecideDiagnostic();
	}

	// Read the stage-1 slack support: every row with a positive slack is an edit, the
	// slack value is the loosening amount. Label clauses over user-facing column names
	// (global_variable_labels names aggregate `<>` indicators so a dropped one is named).
	vector<ColumnProvenance> columns =
	    BuildColumnProvenance(input.indexer, input.var_labels, input.var_is_aux,
	                          input.global_variable_labels);
	vector<ClauseEdit> edits = ReadElasticEdits(slacks, elastic.removals, stage1.solution,
	                                            input.model, columns, /*snap=*/false);

	if (edits.empty()) {
		return DecideDiagnostic();
	}

	// I3 — stage-2 freeze-budget re-solve. Only when there is a real objective AND an
	// editable knob to loosen: a constant objective has nothing to report, and a
	// data-RHS-only conflict has no actionable edit, so an achievable-objective number
	// would be misleading. Among ALL minimal-loosening fixes (budget frozen at S*),
	// re-solve the user's objective and report the best one — the stage-2 slacks become the
	// edit so the edit and the objective agree.
	if (HasObjective(input.model) && (HasLoosenEdit(edits) || HasRemoval(edits))) {
		// stage-1 ObjVal = Σ wᵢ sᵢ + Σ W·wⱼ; the budget freezes both loosening and the
		// (pinned) removal set, so the cap stays consistent with this S*.
		double s_star = stage1.objective_value;
		SolverModel stage2_model = BuildStage2Model(elastic.model, slacks, elastic.removals,
		                                            stage1.solution, input.model, s_star);
		SolverResult stage2 = input.solve_model(stage2_model);

		if (stage2.status == SolverStatus::OPTIMAL && !stage2.solution.empty()) {
			// Report the objective-maximizing minimal fix and the objective it achieves.
			edits = ReadElasticEdits(slacks, elastic.removals, stage2.solution, input.model, columns,
			                         /*snap=*/true);
			if (edits.empty()) {
				return DecideDiagnostic();
			}
			return BuildInfeasibleDiagnostic(
			    edits, FormatNum(RoundToSignificant(stage2.objective_value, 6)));
		}
		if (stage2.status == SolverStatus::UNBOUNDED || stage2.status == SolverStatus::INF_OR_UNBD) {
			// The minimal fix is valid but the relaxed problem has no finite optimum.
			// Keep the stage-1 edit and say so.
			return BuildInfeasibleDiagnostic(edits, /*achievable_objective=*/"",
			                                 /*unbounded_after_fix=*/true);
		}
		// Defensive: the stage-1 point satisfies the budget by construction, so stage 2
		// is feasible. On any other status fall back to the stage-1 edit, no objective.
	}
	return BuildInfeasibleDiagnostic(edits);
}

} // namespace duckdb
