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

//! User-facing name of a flat solver column: its provenance label (user variable
//! name or aux source expression), falling back to a positional name.
string ColLabel(const vector<ColumnProvenance> &cols, int col) {
	if (col >= 0 && static_cast<idx_t>(col) < cols.size() && !cols[col].label.empty()) {
		return cols[col].label;
	}
	return "col" + std::to_string(col);
}

//! Reconstruct the left-hand side of a constraint row as algebra over user-facing
//! column names (e.g. "x", "2*x + 3*y"). Coefficient ±1 is elided to keep the
//! rendering close to what the user wrote. Used to label the offending clause in
//! the elastic edit list without threading the source expression text.
string FormatLhs(const ModelConstraint &row, const vector<ColumnProvenance> &cols) {
	string out;
	for (idx_t i = 0; i < row.indices.size(); i++) {
		double c = row.coefficients[i];
		string var = ColLabel(cols, row.indices[i]);
		if (i == 0) {
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

const char *SenseStr(char sense) {
	return sense == '<' ? "<=" : (sense == '>' ? ">=" : "==");
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

namespace {

//! A slack column wired into one relaxable row. For `=` rows two slacks are used
//! (s⁺, s⁻); `neg_col` is INVALID for `<` / `>` rows (one-sided loosening).
struct SlackRef {
	idx_t row;
	idx_t pos_col;
	idx_t neg_col = DConstants::INVALID_INDEX;
	char sense;
};

} // namespace

DecideDiagnostic DiagnoseInfeasible(const InfeasibleDiagnosisInput &input) {
	// I1: build the elastic program — give every relaxable user constraint a
	// non-negative slack that lets its RHS stretch, minimize the total loosening,
	// and read which constraints to loosen (the positive-slack support) and by how
	// much. The minimal edit list is the least-change fix.
	if (!input.solve_model) {
		return DecideDiagnostic();
	}

	SolverModel elastic = input.model;

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

	// Append a REAL, non-negative, uncapped slack column with unit objective weight
	// (decisions 2+4: uniform wᵢ=1, sᵢ≥0 REAL even for integer RHS).
	auto add_slack = [&]() -> idx_t {
		idx_t c = elastic.num_vars;
		elastic.col_lower.push_back(0.0);
		elastic.col_upper.push_back(1e30);
		elastic.is_integer.push_back(false);
		elastic.is_binary.push_back(false);
		elastic.obj_coeffs.push_back(1.0);
		elastic.num_vars++;
		return c;
	};

	// Slack only the relaxable LINEAR rows (USER_PARAMETER). Quadratic rows and
	// STRUCTURAL / USER_MECHANISM rows stay rigid (quadratic-RHS slack is I2; ABS
	// pin rows are already USER_PARAMETER and are picked up here, their envelopes are
	// STRUCTURAL and stay rigid). One slack per row for `<` / `>`; two for `=`
	// (decision 3) so it can be violated in either direction while staying linear.
	vector<SlackRef> slacks;
	for (idx_t r = 0; r < elastic.constraints.size(); r++) {
		auto &row = elastic.constraints[r];
		if (!IsRelaxableForElastic(row.provenance.kind)) {
			continue;
		}
		if (row.sense == '<') {
			idx_t sc = add_slack(); // Ax − s ≤ b
			row.indices.push_back(static_cast<int>(sc));
			row.coefficients.push_back(-1.0);
			slacks.push_back({r, sc, DConstants::INVALID_INDEX, '<'});
		} else if (row.sense == '>') {
			idx_t sc = add_slack(); // Ax + s ≥ b
			row.indices.push_back(static_cast<int>(sc));
			row.coefficients.push_back(1.0);
			slacks.push_back({r, sc, DConstants::INVALID_INDEX, '>'});
		} else if (row.sense == '=') {
			idx_t sp = add_slack(); // Ax − s⁺ + s⁻ = b
			idx_t sn = add_slack();
			row.indices.push_back(static_cast<int>(sp));
			row.coefficients.push_back(-1.0);
			row.indices.push_back(static_cast<int>(sn));
			row.coefficients.push_back(1.0);
			slacks.push_back({r, sp, sn, '='});
		}
	}

	// Nothing loosenable: fall through to the static error (the user's only
	// constraints are rigid mechanism/structural rows we never edit).
	if (slacks.empty()) {
		return DecideDiagnostic();
	}

	SolverResult result = input.solve_model(elastic);

	if (result.status == SolverStatus::INFEASIBLE) {
		// The elastic program is itself infeasible: the conflict reaches rigid rows.
		// Only claim that when every user constraint was actually made relaxable — if
		// the operator punted a multi-instance bound (I2 scope), stay honest and fall
		// through to the static error rather than wrongly declaring it unfixable.
		if (input.has_unhandled_user_bounds) {
			return DecideDiagnostic();
		}
		return BuildElasticInfeasibleDiagnostic();
	}
	if (result.status != SolverStatus::OPTIMAL || result.solution.empty()) {
		return DecideDiagnostic();
	}

	// Read the slack support: every row with a positive slack is an edit, and the
	// slack value is the loosening amount. Label the clause from the ORIGINAL row
	// (input.model, before slacks were appended) over user-facing column names.
	vector<ColumnProvenance> columns =
	    BuildColumnProvenance(input.indexer, input.var_labels, input.var_is_aux);

	vector<ClauseEdit> edits;
	for (const auto &sl : slacks) {
		double amount;
		if (sl.sense == '=') {
			amount = result.solution[sl.pos_col] - result.solution[sl.neg_col];
		} else {
			amount = result.solution[sl.pos_col];
		}
		if (std::fabs(amount) <= DIAGNOSTIC_RAY_EPSILON) {
			continue;
		}
		const ModelConstraint &orig = input.model.constraints[sl.row];
		string lhs = FormatLhs(orig, columns);
		const char *sense = SenseStr(sl.sense);
		// `≥` loosens downward (b − s), `≤` / `=` upward (b + s).
		double new_rhs = (sl.sense == '>') ? orig.rhs - amount : orig.rhs + amount;
		ClauseEdit e;
		e.label = lhs + " " + sense + " " + FormatNum(orig.rhs);
		e.suggestion = lhs + " " + sense + " " + FormatNum(new_rhs);
		e.amount = FormatNum(std::fabs(amount));
		edits.push_back(std::move(e));
	}

	if (edits.empty()) {
		return DecideDiagnostic();
	}
	return BuildInfeasibleDiagnostic(edits);
}

} // namespace duckdb
