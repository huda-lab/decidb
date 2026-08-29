#include "catch.hpp"

#include "duckdb/decidb/decide_diagnostic_engines.hpp"
#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb.hpp"

using namespace duckdb;

namespace {

//! No labeled global-block columns (only aggregate `<>` indicators carry labels).
//! Passed where the model has no global vars to name.
const duckdb::vector<duckdb::string> kNoGlobalLabels;

SolverInput MakeRowScopedInput(idx_t num_rows) {
	SolverInput input;
	input.num_rows = num_rows;
	input.num_decide_vars = 1;
	input.variable_types = {LogicalType::DOUBLE};
	return input;
}

//! One linear constraint over the single variable x: `coeff * x <sense> rhs`.
struct SingleVarRow {
	double coeff;
	char sense;
	double rhs;
	ConstraintKind kind;
};

//! A one-variable REAL model (x in [0, +inf)) with the given constraints. Used to
//! exercise the elastic engine end-to-end against the bundled HiGHS backend.
SolverModel MakeSingleVarModel(const duckdb::vector<SingleVarRow> &rows) {
	SolverModel m;
	m.num_vars = 1;
	m.col_lower = {0.0};
	m.col_upper = {1e30};
	m.is_integer = {false};
	m.is_binary = {false};
	m.obj_coeffs = {0.0};
	m.maximize = false;
	for (idx_t i = 0; i < rows.size(); i++) {
		ModelConstraint c;
		c.indices = {0};
		c.coefficients = {rows[i].coeff};
		c.sense = rows[i].sense;
		c.rhs = rows[i].rhs;
		// These model query-wide RHS bounds (`x <= 5`), so they are SHARED_SCALAR — a
		// single editable knob — not per-row data (which routes to a conflict summary).
		c.provenance.repair_group_id = i;
		c.provenance.kind = rows[i].kind;
		c.provenance.shape = ElasticShape::SHARED_SCALAR;
		if (IsRelaxableForElastic(rows[i].kind)) {
			// Test-built rows must satisfy the same provenance contract as rows emitted
			// by the physical model builder. Source identity is a separate namespace
			// from repair grouping even when both happen to be one-to-one here.
			c.provenance.source_clause_id = m.constraint_sources.size();
			m.constraint_sources.push_back(ConstraintSourceInfo());
		}
		m.constraints.push_back(std::move(c));
	}
	return m;
}

//! One linear constraint `coeff * x[var] <sense> rhs` over a multi-variable model,
//! with explicit provenance for exercising shared-slack blocks (I2.a): rows sharing
//! a `repair_group_id` and tagged SHARED_SCALAR collapse to one slack.
struct MultiVarRow {
	int var;
	double coeff;
	char sense;
	double rhs;
	ConstraintKind kind;
	idx_t repair_group_id = DConstants::INVALID_INDEX;
	ElasticShape shape = ElasticShape::PER_ROW_DATA;
	idx_t group_key = DConstants::INVALID_INDEX;
	string rhs_label = ""; //!< data RHS column name (query-mode virtual offset)
};

//! A REAL model over `num_vars` variables (each in [0, +inf)) with the given rows.
SolverModel MakeModel(idx_t num_vars, const duckdb::vector<MultiVarRow> &rows) {
	SolverModel m;
	m.num_vars = num_vars;
	m.col_lower.assign(num_vars, 0.0);
	m.col_upper.assign(num_vars, 1e30);
	m.is_integer.assign(num_vars, false);
	m.is_binary.assign(num_vars, false);
	m.obj_coeffs.assign(num_vars, 0.0);
	m.maximize = false;
	std::map<idx_t, idx_t> source_by_repair_group;
	for (const auto &r : rows) {
		ModelConstraint c;
		c.indices = {r.var};
		c.coefficients = {r.coeff};
		c.sense = r.sense;
		c.rhs = r.rhs;
		c.provenance.repair_group_id = r.repair_group_id;
		c.provenance.kind = r.kind;
		c.provenance.shape = r.shape;
		c.provenance.group_key = r.group_key;
		c.provenance.rhs_label = r.rhs_label;
		if (IsRelaxableForElastic(r.kind) && r.repair_group_id != DConstants::INVALID_INDEX) {
			auto source = source_by_repair_group.find(r.repair_group_id);
			if (source == source_by_repair_group.end()) {
				idx_t source_id = m.constraint_sources.size();
				m.constraint_sources.push_back(ConstraintSourceInfo());
				source = source_by_repair_group.emplace(r.repair_group_id, source_id).first;
			}
			c.provenance.source_clause_id = source->second;
		}
		m.constraints.push_back(std::move(c));
	}
	return m;
}

//! The first finding whose `clause` column matches, or a default-constructed one.
DiagnosticFinding Find(const DecideDiagnostic &diag, const string &clause) {
	for (const auto &f : diag.findings) {
		if (f.clause == clause) {
			return f;
		}
	}
	return DiagnosticFinding();
}

//! `amount` rendered the way the relation would, or "" for a NULL amount. Lets the
//! assertions below stay written in the user's units instead of in doubles.
string AmountOf(const DecideDiagnostic &diag, const string &clause) {
	auto f = Find(diag, clause);
	if (!f.has_amount) {
		return string();
	}
	return StringUtil::Format("%g", f.amount);
}

//! How many findings carry a suggested change (one per editable clause).
idx_t CountSuggestions(const DecideDiagnostic &diag) {
	idx_t n = 0;
	for (const auto &f : diag.findings) {
		if (!f.suggested_change.empty()) {
			n++;
		}
	}
	return n;
}

//! The coefficient on column `col` in `row` (0 if the column is absent).
double CoeffOn(const ModelConstraint &row, idx_t col) {
	for (idx_t k = 0; k < row.indices.size(); k++) {
		if (static_cast<idx_t>(row.indices[k]) == col) {
			return row.coefficients[k];
		}
	}
	return 0.0;
}

//! A 2-column model (x = col 0; the `<>` indicator z = col 1, binary) holding the two
//! rigid USER_MECHANISM Big-M disjunction rows for `x <> 3` (M = 14): `x − 14z ≤ 2`
//! (x ≤ 2 when z=0) and `x − 14z ≥ −10` (x ≥ 4 when z=1). Both rows carry
//! indicator_col = 1, so the I4 removal dial groups them into one droppable clause.
SolverModel MakeNotEqualModel() {
	SolverModel m;
	m.num_vars = 2;
	m.col_lower = {0.0, 0.0};
	m.col_upper = {10.0, 1.0};
	m.is_integer = {false, true};
	m.is_binary = {false, true};
	m.obj_coeffs = {0.0, 0.0};
	m.maximize = false;
	for (char sense : {'<', '>'}) {
		ModelConstraint c;
		c.indices = {0, 1};
		c.coefficients = {1.0, -14.0};
		c.sense = sense;
		c.rhs = sense == '<' ? 2.0 : -10.0;
		c.provenance.kind = ConstraintKind::USER_MECHANISM;
		c.provenance.indicator_col = 1;
		m.constraints.push_back(std::move(c));
	}
	return m;
}

} // namespace

TEST_CASE("DeciDB diagnosis engines", "[decidb][query_diagnostics][engines]") {
	SECTION("unbounded engine names variable and uses injected grouping") {
		SolverInput input = MakeRowScopedInput(3);
		VarIndexer indexer = VarIndexer::Build(input);

		SolverResult result;
		result.status = SolverStatus::UNBOUNDED;
		result.ray = {1.0, 1.0, 0.0};

		duckdb::vector<string> labels {"x"};
		duckdb::vector<bool> is_aux {false};
		DecideDiagParams params;
		params.escape_rate = 0.8;

		auto get_candidates = [](idx_t decide_var_idx, idx_t total_instances) {
			ColumnGrouping grouping;
			grouping.column = "channel";
			grouping.instance_to_group = {0, 0, 1};
			grouping.group_value = {"export", "domestic"};
			return duckdb::vector<ColumnGrouping> {std::move(grouping)};
		};

		UnboundedDiagnosisInput diag_input {
		    result,
		    indexer,
		    labels,
		    is_aux,
		    params,
		    get_candidates,
		};
		DecideDiagnostic diag = DiagnoseUnbounded(diag_input);

		REQUIRE(diag.valid);
		CHECK(diag.state == "unbounded");
		// One finding per characterized slice: x runs to +inf on the 2 export rows.
		REQUIRE(diag.findings.size() == 1);
		CHECK(diag.findings[0].clause == "x");
		CHECK(diag.findings[0].edit_source == "runaway_+inf");
		// The one rule accounts for every escaping instance and its whole group escapes,
		// so the cap is scoped to those rows rather than capping the domestic row too.
		CHECK(diag.findings[0].suggested_change == "x <= <cap> WHEN channel = 'export'");
		CHECK(diag.findings[0].group == "channel = 'export'");
		REQUIRE(diag.findings[0].has_amount);
		CHECK(diag.findings[0].amount == 2.0);
		REQUIRE(diag.findings[0].has_total);
		CHECK(diag.findings[0].total == 2);
		CHECK(diag.findings[0].scope == "row");
	}

	SECTION("unbounded engine preserves negative ray direction") {
		SolverInput input = MakeRowScopedInput(1);
		VarIndexer indexer = VarIndexer::Build(input);

		SolverResult result;
		result.status = SolverStatus::UNBOUNDED;
		result.ray = {-1.0};

		duckdb::vector<string> labels {"x"};
		duckdb::vector<bool> is_aux {false};
		DecideDiagParams params;
		UnboundedDiagnosisInput diag_input {
		    result,
		    indexer,
		    labels,
		    is_aux,
		    params,
		    [](idx_t, idx_t) { return duckdb::vector<ColumnGrouping>(); },
		};
		DecideDiagnostic diag = DiagnoseUnbounded(diag_input);

		REQUIRE(diag.valid);
		REQUIRE(diag.findings.size() == 1);
		CHECK(diag.findings[0].edit_source == "runaway_-inf");
	}

	SECTION("unbounded engine returns invalid diagnosis when ray names no variable") {
		SolverInput input = MakeRowScopedInput(1);
		input.num_global_vars = 1;
		input.global_variable_types = {LogicalType::DOUBLE};
		input.global_lower_bounds = {0.0};
		input.global_upper_bounds = {1e30};
		input.global_obj_coeffs = {0.0};
		VarIndexer indexer = VarIndexer::Build(input);
		indexer.total_vars = indexer.global_block_start + input.num_global_vars;

		SolverResult result;
		result.status = SolverStatus::UNBOUNDED;
		result.ray = {0.0, 1.0};
		duckdb::vector<string> labels {"x"};
		duckdb::vector<bool> is_aux {false};
		DecideDiagParams params;

		UnboundedDiagnosisInput diag_input {
		    result,
		    indexer,
		    labels,
		    is_aux,
		    params,
		    [](idx_t, idx_t) { return duckdb::vector<ColumnGrouping>(); },
		};
		CHECK(!DiagnoseUnbounded(diag_input).valid);
	}

	// I1: the elastic engine runs a real stage-1 solve (bundled HiGHS) and reports
	// the least-change fix.
	SolverInput input = MakeRowScopedInput(1);
	VarIndexer indexer = VarIndexer::Build(input);
	duckdb::vector<string> labels {"x"};
	duckdb::vector<bool> is_aux {false};
	DecideDiagParams params;
	auto solve_highs = [](const SolverModel &m) { return SolvePreparedModel(m, SolverRegistry::Find("highs")); };

	SECTION("elastic engine reports the minimal loosening of a relaxable row") {
		// x <= 5 (relaxable) conflicts with the rigid x >= 10. The only fix is to
		// loosen the cap to 10 — a unique minimizer, so the amount is deterministic.
		SolverModel model = MakeSingleVarModel({
		    {1.0, '<', 5.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(diag.state == "infeasible");
		CHECK(Find(diag, "x <= 5").suggested_change == "x <= 10");
		CHECK(AmountOf(diag, "x <= 5") == "5");
		// The summary points to the problem clause; the structured rows above carry
		// the concrete edit. edit_kind is uniform.
		CHECK(Find(diag, "x <= 5").edit_source == "source_literal");
	}

	SECTION("equality row loosens via its two-sided slack") {
		// x = 5 (relaxable) conflicts with the rigid x >= 8: the equality must move
		// up to 8 (s⁺ = 3). The `=` two-slack form must report the net edit.
		SolverModel model = MakeSingleVarModel({
		    {1.0, '=', 5.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '>', 8.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(Find(diag, "x = 5").suggested_change == "x = 8");
		CHECK(AmountOf(diag, "x = 5") == "3");
	}

	SECTION("elastic-infeasible when loosening cannot fix a rigid conflict") {
		// The conflict is between two rigid rows (x <= 5, x >= 10); the lone
		// relaxable row (x <= 100) cannot help, so the elastic program is itself
		// infeasible — a distinct, honest outcome.
		SolverModel model = MakeSingleVarModel({
		    {1.0, '<', 100.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '<', 5.0, ConstraintKind::STRUCTURAL},
		    {1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		REQUIRE(diag.findings.size() == 1);
		CHECK(diag.findings[0].edit_source == "rigid_conflict");
		CHECK(diag.findings[0].clause.empty());
	}

	SECTION("punted multi-instance bound suppresses the elastic-infeasible claim") {
		// Same rigid conflict, but the operator flagged an absorbed bound it could
		// not re-emit (I2 scope). The engine must NOT declare it unfixable — it falls
		// through to the static error (invalid diagnosis) instead.
		SolverModel model = MakeSingleVarModel({
		    {1.0, '<', 100.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '<', 5.0, ConstraintKind::STRUCTURAL},
		    {1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params,
		                                     /*has_unhandled_user_bounds=*/true, solve_highs};
		CHECK(!DiagnoseInfeasible(diag_input).valid);
	}

	SECTION("no relaxable rows falls through to the static error") {
		// Every constraint is rigid: there is nothing to loosen, so the engine
		// returns an invalid diagnosis (caller uses the static infeasible error).
		SolverModel model = MakeSingleVarModel({
		    {1.0, '<', 5.0, ConstraintKind::STRUCTURAL},
		    {1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params, false, solve_highs};
		CHECK(!DiagnoseInfeasible(diag_input).valid);
	}

	SECTION("BuildElasticModel adds one size-1 block per relaxable row, rigid rows untouched") {
		// Structural seam (I2.0): the elastic transform is a pure function of the base
		// model. A relaxable `<` row gets one slack column; the `=` row gets two; the
		// STRUCTURAL row gets none. Each block spans exactly its own row (size 1 == I1).
		SolverModel model = MakeSingleVarModel({
		    {1.0, '<', 5.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '=', 7.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		ElasticModel elastic = BuildElasticModel(model);

		// 1 original var + 1 slack (`<`) + 2 slacks (`=`) = 4 columns.
		CHECK(elastic.model.num_vars == 4);
		CHECK(elastic.model.col_lower.size() == 4);
		CHECK(elastic.model.obj_coeffs.size() == 4);
		// The transform only wires repair knobs; lexicographic pass objectives are set later.
		CHECK(elastic.model.obj_coeffs[0] == 0.0);
		CHECK(elastic.model.obj_coeffs[1] == 0.0);
		CHECK(elastic.model.obj_coeffs[2] == 0.0);
		CHECK(elastic.model.obj_coeffs[3] == 0.0);
		CHECK(!elastic.model.maximize);

		REQUIRE(elastic.slacks.size() == 2);
		const auto &b0 = elastic.slacks[0];
		CHECK(b0.rows == duckdb::vector<idx_t> {0});
		CHECK(b0.sense == '<');
		CHECK(b0.neg_col == DConstants::INVALID_INDEX);
		const auto &b1 = elastic.slacks[1];
		CHECK(b1.rows == duckdb::vector<idx_t> {1});
		CHECK(b1.sense == '=');
		CHECK(b1.neg_col != DConstants::INVALID_INDEX);
	}

	SECTION("BuildElasticModel scale-normalizes editable slack weights (T1)") {
		// Two editable single-term rows in incomparable units: a count floor (coeff 1,
		// RMS = 1) and a "budget" (coeff 1000, RMS = 1000). Uniform weights would sum
		// their slacks as one currency and gut the small-magnitude floor; normalization
		// weights each knob by ref/rms(A) — the root-mean-square coefficient, so the
		// weight tracks the typical coefficient magnitude, not the term count. ref = min
		// editable RMS = 1, so w_floor = 1/1 = 1 and w_budget = 1/1000. These are now
		// per-tier coefficients recorded on the slack refs, not flat model objective
		// coefficients. The end-to-end "loosen the budget, not the floor" flip is pinned by
		// the Python TPC-H degenerate-edit test on real multi-variable data.
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '>', 30.0, ConstraintKind::USER_PARAMETER},
		    {1, 1000.0, '<', 100.0, ConstraintKind::USER_PARAMETER},
		});
		ElasticModel elastic = BuildElasticModel(model);

		REQUIRE(elastic.slacks.size() == 2);
		const auto &floor_block = elastic.slacks[0];
		const auto &budget_block = elastic.slacks[1];
		CHECK(floor_block.rows == duckdb::vector<idx_t> {0});
		CHECK(budget_block.rows == duckdb::vector<idx_t> {1});
		CHECK(floor_block.tier == ElasticRepairTier::EDITABLE_LOOSEN);
		CHECK(budget_block.tier == ElasticRepairTier::EDITABLE_LOOSEN);
		double w_floor = floor_block.weight;
		double w_budget = budget_block.weight;
		CHECK(w_floor == Approx(1.0));
		CHECK(w_budget == Approx(0.001));
		// The large-coefficient row is cheaper to loosen per native unit; the built model's
		// objective is still empty because tier objectives are installed per pass.
		CHECK(w_budget < w_floor);
		CHECK(elastic.model.obj_coeffs[floor_block.pos_col] == 0.0);
		CHECK(elastic.model.obj_coeffs[budget_block.pos_col] == 0.0);
	}

	SECTION("BuildElasticModel shares ONE slack across a SHARED_SCALAR block") {
		// I2.a structure: three rows of one repair group, tagged SHARED_SCALAR,
		// must collapse to a single shared slack column wired into all three rows — not
		// three independent slacks. A second clause stays its own block.
		SolverModel model = MakeModel(4, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR},
		    {2, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR},
		    {3, 1.0, '<', 9.0, ConstraintKind::USER_PARAMETER, 1, ElasticShape::SHARED_SCALAR},
		});
		ElasticModel elastic = BuildElasticModel(model);

		// 4 user vars + 1 shared slack (clause 0) + 1 slack (clause 1) = 6 columns.
		CHECK(elastic.model.num_vars == 6);
		REQUIRE(elastic.slacks.size() == 2);
		CHECK(elastic.slacks[0].rows == duckdb::vector<idx_t> {0, 1, 2});
		CHECK(elastic.slacks[1].rows == duckdb::vector<idx_t> {3});
		CHECK(elastic.slacks[0].tier == ElasticRepairTier::EDITABLE_LOOSEN);
		CHECK(elastic.slacks[1].tier == ElasticRepairTier::EDITABLE_LOOSEN);
		// The shared slack column appears in all three rows of clause 0.
		idx_t shared_col = elastic.slacks[0].pos_col;
		for (idx_t r : {0u, 1u, 2u}) {
			const auto &row = elastic.model.constraints[r];
			bool found = false;
			for (idx_t k = 0; k < row.indices.size(); k++) {
				if (static_cast<idx_t>(row.indices[k]) == shared_col) {
					found = true;
					CHECK(row.coefficients[k] == -1.0); // `<` loosens upward (Ax − s ≤ b)
				}
			}
			CHECK(found);
		}
	}

	SECTION("shared-slack block reports the max overshoot, not the sum") {
		// One clause `x <= 5` over two rows (instances of the row-scoped var x);
		// structural floors force x@row0 ≥ 8 (overshoot 3) and x@row1 ≥ 12 (overshoot
		// 7). One shared knob → the fix is the MAX overshoot (7), a single edit.
		// Independent slacks would report 3 and 7 (sum 10) and two edits.
		SolverInput row_input = MakeRowScopedInput(2);
		VarIndexer row_indexer = VarIndexer::Build(row_input);
		duckdb::vector<string> row_labels {"x"};
		duckdb::vector<bool> row_aux {false};
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR},
		    {0, 1.0, '>', 8.0, ConstraintKind::STRUCTURAL},
		    {1, 1.0, '>', 12.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model,  row_indexer, row_labels,
		                                     row_aux, kNoGlobalLabels, params, false,
		                                     solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(diag.state == "infeasible");
		// Exactly one edit (one clause), amount = max overshoot = 7.
		CHECK(CountSuggestions(diag) == 1);
		CHECK(AmountOf(diag, "x <= 5") == "7");
		CHECK(Find(diag, "x <= 5").suggested_change == "x <= 12");
	}

	SECTION("PER SHARED_SCALAR folds by mode: one edit in query, per-group in expanded") {
		// T3: a PER clause fans into N_g rows per group, all SHARED_SCALAR with the same
		// repair_group_id but distinct group_key. query mode is the single SQL literal the user
		// edits → ONE slack across every group; expanded mode breaks it out per group.
		SolverModel model = MakeModel(4, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR, 0},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR, 0},
		    {2, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR, 1},
		    {3, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR, 1},
		});
		// query (default): one slack spans all four rows (both groups). 4 vars + 1 slack.
		ElasticModel q = BuildElasticModel(model);
		CHECK(q.model.num_vars == 5);
		REQUIRE(q.slacks.size() == 1);
		CHECK(q.slacks[0].rows == duckdb::vector<idx_t> {0, 1, 2, 3});
		CHECK(q.slacks[0].tier == ElasticRepairTier::EDITABLE_LOOSEN);
		// expanded: one slack per group. 4 vars + 2 slacks.
		ElasticModel e = BuildElasticModel(model, 0.0, "expanded");
		CHECK(e.model.num_vars == 6);
		REQUIRE(e.slacks.size() == 2);
		CHECK(e.slacks[0].rows == duckdb::vector<idx_t> {0, 1});
		CHECK(e.slacks[1].rows == duckdb::vector<idx_t> {2, 3});
		CHECK(e.slacks[0].tier == ElasticRepairTier::EDITABLE_LOOSEN);
		CHECK(e.slacks[1].tier == ElasticRepairTier::EDITABLE_LOOSEN);
	}

	SECTION("data-RHS folds by mode: one offset in query, per-row in expanded") {
		// T3: a data RHS (PER_ROW_DATA, `x <= col`) folds into ONE shared slack (the single
		// virtual offset) in query mode; in expanded mode its rows stay independent so each
		// row's overshoot surfaces as its own profile entry.
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA, 0},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA, 1},
		});
		// query (default): one shared slack across both data rows.
		ElasticModel q = BuildElasticModel(model);
		REQUIRE(q.slacks.size() == 1);
		CHECK(q.slacks[0].rows == duckdb::vector<idx_t> {0, 1});
		CHECK(q.slacks[0].tier == ElasticRepairTier::DATA_OFFSET);
		CHECK(q.slacks[0].weight == 1.0);
		CHECK(q.model.obj_coeffs[q.slacks[0].pos_col] == 0.0);
		// expanded: independent size-1 blocks.
		ElasticModel e = BuildElasticModel(model, 0.0, "expanded");
		REQUIRE(e.slacks.size() == 2);
		CHECK(e.slacks[0].rows == duckdb::vector<idx_t> {0});
		CHECK(e.slacks[1].rows == duckdb::vector<idx_t> {1});
		CHECK(e.slacks[0].tier == ElasticRepairTier::DATA_OFFSET);
		CHECK(e.slacks[1].tier == ElasticRepairTier::DATA_OFFSET);
	}

	SECTION("USER_MECHANISM and STRUCTURAL rows are never slackened") {
		// I2.e: `<>` indicator rows (USER_MECHANISM) and McCormick links (STRUCTURAL)
		// are rigid — the elastic transform attaches no slack to them.
		SolverModel model = MakeSingleVarModel({
		    {1.0, '<', 5.0, ConstraintKind::USER_MECHANISM},
		    {1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		ElasticModel elastic = BuildElasticModel(model);
		CHECK(elastic.slacks.empty());
		CHECK(elastic.model.num_vars == 1); // no slack columns added
	}

	SECTION("data-RHS slack is a separate tier so an editable knob loosens first") {
		// I2.c: an editable cap (SHARED_SCALAR) and a data floor (PER_ROW_DATA) both
		// conflict with a rigid pin; the lexicographic data tier is frozen at zero before
		// editable loosening, so the solver loosens the editable cap and leaves data alone.
		SolverModel model = MakeModel(1, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR},
		    {0, 1.0, '>', 3.0, ConstraintKind::USER_PARAMETER, 1, ElasticShape::PER_ROW_DATA},
		    {0, 1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		// The cap loosens to 10 (amount 5); the data floor is not touched.
		CHECK(Find(diag, "x <= 5").suggested_change == "x <= 10");
		CHECK(Find(diag, "x >= 3").suggested_change.empty());
	}

	SECTION("query mode folds a data-RHS clause into one virtual offset") {
		// T3 query mode (default): a data-backed RHS (`x <= cap`) has no literal to loosen,
		// so its rows fold into one shared slack = a single virtual query offset
		// `x <= cap + delta` (delta = max overshoot). The rhs_label names the column.
		SolverInput row_input = MakeRowScopedInput(2);
		VarIndexer row_indexer = VarIndexer::Build(row_input);
		duckdb::vector<string> row_labels {"x"};
		duckdb::vector<bool> row_aux {false};
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA,
		     DConstants::INVALID_INDEX, "cap"},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA,
		     DConstants::INVALID_INDEX, "cap"},
		    {0, 1.0, '>', 8.0, ConstraintKind::STRUCTURAL},
		    {1, 1.0, '>', 12.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model,   row_indexer, row_labels,
		                                     row_aux, kNoGlobalLabels, params, false,
		                                     solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		// One folded edit: max overshoot is 7 (row 1 needs x >= 12 vs cap 5).
		CHECK(Find(diag, "x <= cap").suggested_change == "x <= cap + 7");
		CHECK(AmountOf(diag, "x <= cap") == "7");
		CHECK(Find(diag, "x <= cap").edit_source == "virtual_offset");
		// Clause-level: query scope names no group and no row.
		CHECK(Find(diag, "x <= cap").group.empty());
		CHECK(!Find(diag, "x <= cap").has_row);
	}

	SECTION("expanded mode exposes each data-RHS row as its own profile entry") {
		// T3 expanded mode: the data rows stay per-row, so each conflicting row is a
		// separate `expanded_row` edit carrying its exact overshoot (3 and 7).
		SolverInput row_input = MakeRowScopedInput(2);
		VarIndexer row_indexer = VarIndexer::Build(row_input);
		duckdb::vector<string> row_labels {"x"};
		duckdb::vector<bool> row_aux {false};
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA},
		    {0, 1.0, '>', 8.0, ConstraintKind::STRUCTURAL},
		    {1, 1.0, '>', 12.0, ConstraintKind::STRUCTURAL},
		});
		DecideDiagParams expanded_params = params;
		expanded_params.slack_scope = "expanded";
		InfeasibleDiagnosisInput diag_input {model,   row_indexer,     row_labels,
		                                     row_aux, kNoGlobalLabels, expanded_params,
		                                     false,   solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		// Two independent per-row edits (subjects differ by their numeric rhs — same 5 here,
		// so both render `x <= 5`), each tagged expanded_row/row. Assert the tags exist.
		CHECK(Find(diag, "x <= 5").edit_source == "expanded_row");
		// Both rows conflict: the max overshoot 7 appears as an amount somewhere.
		bool saw_amount_7 = false;
		for (const auto &f : diag.findings) {
			if (f.has_amount && f.amount == 7.0) {
				saw_amount_7 = true;
			}
		}
		CHECK(saw_amount_7);
	}

	SECTION("strict < re-quotes the suggestion against the user's typed literal") {
		// I2.d: `x < 5` (integer) is built as `x <= 4` with strict provenance + typed_k=5.
		// Loosening the δ-row by 6 (to <= 10) re-quotes as `x < 5` → `x < 11`, rendered `<`.
		SolverModel model = MakeSingleVarModel({
		    {1.0, '<', 4.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		model.constraints[0].provenance.strict = true;
		model.constraints[0].provenance.typed_k = 5.0;
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(Find(diag, "x < 5").suggested_change == "x < 11");
		CHECK(AmountOf(diag, "x < 5") == "6");
	}

	SECTION("source registry supplies canonical names, casts, RHS, and qualifiers") {
		SolverModel model = MakeSingleVarModel({
		    {1.0, '<', 5.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '>', 8.0, ConstraintKind::STRUCTURAL},
		});
		model.constraints[0].provenance.source_clause_id = 0;
		model.constraints[0].provenance.shape = ElasticShape::PER_ROW_DATA;
		ConstraintSourceInfo source;
		source.canonical_lhs = "CAST(real_x AS DOUBLE)";
		source.canonical_rhs = "capacity";
		source.qualifier = "WHEN enabled PER region";
		source.rhs_kind = ConstraintSourceRhsKind::DATA_EXPRESSION;
		model.constraint_sources[0] = std::move(source);
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		string clause = "CAST(real_x AS DOUBLE) <= capacity WHEN enabled PER region";
		CHECK(Find(diag, clause).suggested_change ==
		      "CAST(real_x AS DOUBLE) <= capacity + 3 WHEN enabled PER region");
	}

	SECTION("AVG row renders an AVG(...) label and reports the raw slack") {
		// I2.d: a pure AVG aggregate stores coefficients pre-scaled by 1/N (avg_scaled),
		// so the label collapses back to `AVG(x)` and the slack is reported in AVG units.
		SolverInput row_input = MakeRowScopedInput(2);
		VarIndexer row_indexer = VarIndexer::Build(row_input);
		duckdb::vector<string> row_labels {"x"};
		duckdb::vector<bool> row_aux {false};
		SolverModel m;
		m.num_vars = 2;
		m.col_lower = {0.0, 0.0};
		m.col_upper = {1e30, 1e30};
		m.is_integer = {false, false};
		m.is_binary = {false, false};
		m.obj_coeffs = {0.0, 0.0};
		m.maximize = false;
		ModelConstraint avg_row;
		avg_row.indices = {0, 1};
		avg_row.coefficients = {0.5, 0.5}; // (1/2)x0 + (1/2)x1 = AVG(x)
		avg_row.sense = '<';
		avg_row.rhs = 5.0;
		avg_row.provenance.repair_group_id = 0;
		avg_row.provenance.source_clause_id = 0;
		avg_row.provenance.kind = ConstraintKind::USER_PARAMETER;
		avg_row.provenance.shape = ElasticShape::SHARED_SCALAR;
		avg_row.provenance.avg_scaled = true;
		m.constraint_sources.push_back(ConstraintSourceInfo());
		m.constraints.push_back(std::move(avg_row));
		for (int v : {0, 1}) {
			ModelConstraint floor;
			floor.indices = {v};
			floor.coefficients = {1.0};
			floor.sense = '>';
			floor.rhs = 10.0;
			floor.provenance.kind = ConstraintKind::STRUCTURAL;
			m.constraints.push_back(std::move(floor));
		}
		InfeasibleDiagnosisInput diag_input {m,       row_indexer, row_labels,
		                                     row_aux, kNoGlobalLabels, params, false,
		                                     solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(Find(diag, "AVG(x) <= 5").suggested_change == "AVG(x) <= 10");
		CHECK(AmountOf(diag, "AVG(x) <= 5") == "5");
	}

	SECTION("BuildElasticModel slacks a quadratic constraint's linear RHS only, never Q") {
		// I2.d: a relaxable quadratic constraint `x² <= 4` gets a slack on its LINEAR
		// part; the Q matrix is untouched, and the block is marked quadratic.
		SolverModel model;
		model.num_vars = 1;
		model.col_lower = {0.0};
		model.col_upper = {1e30};
		model.is_integer = {false};
		model.is_binary = {false};
		model.obj_coeffs = {0.0};
		model.maximize = false;
		SolverModel::QuadraticConstraint qc;
		qc.q_rows = {0};
		qc.q_cols = {0};
		qc.q_coefficients = {1.0}; // x²
		qc.sense = '<';
		qc.rhs = 4.0;
		qc.provenance.repair_group_id = 0;
		qc.provenance.source_clause_id = 0;
		qc.provenance.kind = ConstraintKind::USER_PARAMETER;
		qc.provenance.shape = ElasticShape::SHARED_SCALAR;
		model.constraint_sources.push_back(ConstraintSourceInfo());
		model.quadratic_constraints.push_back(std::move(qc));

		ElasticModel elastic = BuildElasticModel(model);

		CHECK(elastic.model.num_vars == 2); // one slack column
		REQUIRE(elastic.slacks.size() == 1);
		CHECK(elastic.slacks[0].quadratic);
		CHECK(elastic.slacks[0].rows == duckdb::vector<idx_t> {0});
		CHECK(elastic.slacks[0].sense == '<');
		const auto &eqc = elastic.model.quadratic_constraints[0];
		// Q matrix untouched.
		CHECK(eqc.q_coefficients == duckdb::vector<double> {1.0});
		CHECK(eqc.q_rows.size() == 1);
		// Slack landed on the linear part with the loosening sign.
		REQUIRE(eqc.linear_indices.size() == 1);
		CHECK(static_cast<idx_t>(eqc.linear_indices[0]) == elastic.slacks[0].pos_col);
		CHECK(eqc.linear_coefficients[0] == -1.0);
	}

	// I3: a 2-decide-var, 1-row input so columns name distinct variables x and y.
	SolverInput input2;
	input2.num_rows = 1;
	input2.num_decide_vars = 2;
	input2.variable_types = {LogicalType::DOUBLE, LogicalType::DOUBLE};
	VarIndexer indexer2 = VarIndexer::Build(input2);
	duckdb::vector<string> labels2 {"x", "y"};
	duckdb::vector<bool> is_aux2 {false, false};

	SECTION("stage 2 reports the objective-maximizing minimal fix among ties") {
		// Two editable caps x <= 0, y <= 0 conflict with the rigid floor x + y >= 10.
		// Total loosening S* = 10 with MANY minimizers (loosen x, loosen y, or split).
		// Freezing arbitrary stage-1 amounts could loosen y (giving objective 0);
		// stage 2 maximizes x, so it must loosen x's cap to 10 and report objective 10.
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 0.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR},
		    {1, 1.0, '<', 0.0, ConstraintKind::USER_PARAMETER, 1, ElasticShape::SHARED_SCALAR},
		});
		ModelConstraint floor; // rigid x + y >= 10
		floor.indices = {0, 1};
		floor.coefficients = {1.0, 1.0};
		floor.sense = '>';
		floor.rhs = 10.0;
		floor.provenance.kind = ConstraintKind::STRUCTURAL;
		model.constraints.push_back(std::move(floor));
		model.obj_coeffs = {1.0, 0.0}; // MAXIMIZE x
		model.maximize = true;

		InfeasibleDiagnosisInput diag_input {model,    indexer2, labels2, is_aux2, kNoGlobalLabels,
		                                     params,   false,    solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(diag.state == "infeasible");
		// The fix loosens x's cap to 10 (amount 10), not y's.
		CHECK(Find(diag, "x <= 0").suggested_change == "x <= 10");
		CHECK(AmountOf(diag, "x <= 0") == "10");
		CHECK(Find(diag, "y <= 0").suggested_change.empty());
		// The achievable objective is reported as a model-level fact, not in the summary.
		CHECK(Find(diag, "").edit_source == "achievable_objective");
		CHECK(AmountOf(diag, "") == "10");
	}

	SECTION("stage 2 reports an unbounded objective after the fix") {
		// x <= 0 (editable) conflicts with the rigid x >= 5: the unique fix loosens the
		// cap to 5. But y is free above and absent from every constraint, so MAXIMIZE y
		// has no finite optimum once feasible; the table reports that model-level fact.
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 0.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_SCALAR},
		    {0, 1.0, '>', 5.0, ConstraintKind::STRUCTURAL},
		});
		model.obj_coeffs = {0.0, 1.0}; // MAXIMIZE y (unbounded)
		model.maximize = true;

		InfeasibleDiagnosisInput diag_input {model,    indexer2, labels2, is_aux2, kNoGlobalLabels,
		                                     params,   false,    solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(Find(diag, "x <= 0").suggested_change == "x <= 5");
		CHECK(AmountOf(diag, "x <= 0") == "5");
		CHECK(Find(diag, "").edit_source == "unbounded_after_fix");
		CHECK(!Find(diag, "").has_amount);
		// The remedy has to say what to bound. "Bound the objective" is not actionable —
		// an objective is an expression, not a knob — so it points at the terms in it.
		// Deliberately no solver vocabulary ("no finite optimum", "unbounded ray").
		string after_fix = Find(diag, "").suggested_change;
		CHECK(after_fix.find("terms") != string::npos);
		CHECK(after_fix.find("optimum") == string::npos);
	}

	SECTION("the stash crosses one statement and is consumed once") {
		DuckDB db(nullptr);
		Connection con(db);

		DecideDiagnostic diag;
		diag.valid = true;
		diag.state = "infeasible";
		DiagnosticFinding f;
		f.clause = "x <= 5";
		f.suggested_change = "x <= 12";
		f.has_amount = true;
		f.amount = 7.0;
		f.edit_source = "source_literal";
		diag.findings.push_back(std::move(f));
		StashDecideDiagnostic(*con.context, std::move(diag));

		DecideDiagnostic taken = TakeDecideDiagnostic(*con.context);
		REQUIRE(taken.valid);
		CHECK(taken.state == "infeasible");
		REQUIRE(taken.findings.size() == 1);
		CHECK(taken.findings[0].clause == "x <= 5");
		CHECK(taken.findings[0].amount == 7.0);
		// Consumed: the stash exists only to cross from DECIDE to the DIAGNOSE operator
		// above it within one statement, so a second read finds nothing.
		CHECK(!TakeDecideDiagnostic(*con.context).valid);
	}

	SECTION("a diagnosis renders into the flat relation's columns") {
		duckdb::vector<string> names;
		duckdb::vector<LogicalType> types;
		GetDecideDiagnoseSchema(names, types);
		REQUIRE(names.size() == 9);
		CHECK(names[0] == "state");
		CHECK(names[3] == "amount");
		CHECK(types[3] == LogicalType::DOUBLE);
		CHECK(names[4] == "total");
		CHECK(types[4] == LogicalType::BIGINT);
		CHECK(names[5] == "scope");
		CHECK(types[5] == LogicalType::VARCHAR);
		CHECK(names[8] == "row");
		CHECK(types[8] == LogicalType::BIGINT);

		DecideDiagnostic diag;
		diag.valid = true;
		diag.state = "infeasible";
		DiagnosticFinding f;
		f.clause = "x <= 5";
		f.suggested_change = "x <= 12";
		f.has_amount = true;
		f.amount = 7.0;
		f.edit_source = "source_literal";
		diag.findings.push_back(std::move(f));

		DataChunk chunk;
		chunk.Initialize(Allocator::DefaultAllocator(), types);
		idx_t offset = 0;
		RenderDecideDiagnostic(diag, offset, chunk);
		REQUIRE(chunk.size() == 1);
		CHECK(chunk.GetValue(0, 0).ToString() == "infeasible");
		CHECK(chunk.GetValue(1, 0).ToString() == "x <= 5");
		CHECK(chunk.GetValue(2, 0).ToString() == "x <= 12");
		CHECK(chunk.GetValue(3, 0).GetValue<double>() == 7.0);
		CHECK(chunk.GetValue(4, 0).IsNull());
		CHECK(chunk.GetValue(5, 0).IsNull());
		CHECK(chunk.GetValue(6, 0).ToString() == "source_literal");
		// total, scope, group and row are NULL for a non-counted finding outside
		// the expanded slack scope.
		CHECK(chunk.GetValue(7, 0).IsNull());
		CHECK(chunk.GetValue(8, 0).IsNull());
	}

	SECTION("a feasible query still reports one finding") {
		DecideDiagnostic diag = BuildFeasibleDiagnostic();
		REQUIRE(diag.valid);
		CHECK(diag.state == "feasible");
		REQUIRE(diag.findings.size() == 1);
		CHECK(diag.findings[0].clause.empty());
		CHECK(diag.findings[0].suggested_change.empty());
		CHECK(!diag.findings[0].has_amount);
	}

	SECTION("unbounded unavailable reason distinguishes empty linear rays") {
		CHECK(BuildUnboundedDiagnosisUnavailableReason(true, true, true) ==
		      "diagnosis ran out of time before it could identify the runaway variable.");
		CHECK(BuildUnboundedDiagnosisUnavailableReason(false, true, true) ==
		      "a non-linear term prevents naming the variable.");

		string linear_reason = BuildUnboundedDiagnosisUnavailableReason(false, true, false);
		CHECK(linear_reason == "the runaway variable could not be identified.");
		CHECK(linear_reason.find("non-linear") == string::npos);

		CHECK(BuildUnboundedDiagnosisUnavailableReason(false, false, false) ==
		      "the runaway is an internal helper variable.");
	}

	// I4 — the L0 / removal dial. A remove-only `<>` cannot be loosened; instead the
	// elastic transform gives its disjunction pair one binary `w` that, set to 1, drops
	// the clause. The removal tier minimizes Σw before data/editable tiers.
	SECTION("BuildElasticModel adds a binary removal indicator for a `<>` clause") {
		// The two USER_MECHANISM rows share indicator_col 1; they get no slack but a
		// single binary w wired ±M₂ by sense (auto M₂ = the disjunction Big-M = 14).
		SolverModel model = MakeNotEqualModel();
		ElasticModel elastic = BuildElasticModel(model);

		CHECK(elastic.slacks.empty()); // nothing loosenable
		REQUIRE(elastic.removals.size() == 1);
		const auto &rem = elastic.removals[0];
		CHECK(rem.rows == duckdb::vector<idx_t> {0, 1});
		CHECK(rem.indicator_col == 1);
		// One binary column appended (w = col 2); its objective coefficient is set only
		// during the removal-tier pass.
		CHECK(elastic.model.num_vars == 3);
		CHECK(rem.w_col == 2);
		CHECK(elastic.model.is_binary[2]);
		CHECK(elastic.model.is_integer[2]);
		CHECK(elastic.model.col_lower[2] == 0.0);
		CHECK(elastic.model.col_upper[2] == 1.0);
		CHECK(elastic.model.obj_coeffs[2] == 0.0);
		// w neutralizes each row with the disjunction Big-M, sign by sense.
		CHECK(CoeffOn(elastic.model.constraints[0], 2) == -14.0); // `<` row
		CHECK(CoeffOn(elastic.model.constraints[1], 2) == 14.0);  // `>` row
	}

	SECTION("removal Big-M honors the pragma override") {
		// diagnose_decide_removal_bigm replaces the auto M₂ with a flat value.
		SolverModel model = MakeNotEqualModel();
		ElasticModel elastic = BuildElasticModel(model, /*removal_bigm=*/99.0);
		REQUIRE(elastic.removals.size() == 1);
		idx_t w = elastic.removals[0].w_col;
		CHECK(CoeffOn(elastic.model.constraints[0], w) == -99.0);
		CHECK(CoeffOn(elastic.model.constraints[1], w) == 99.0);
	}

	// A labeled 2-var (x DOUBLE, `<>` indicator BOOLEAN) layout so the dropped clause
	// renders its user-facing name.
	SolverInput ne_input;
	ne_input.num_rows = 1;
	ne_input.num_decide_vars = 2;
	ne_input.variable_types = {LogicalType::DOUBLE, LogicalType::BOOLEAN};
	VarIndexer ne_indexer = VarIndexer::Build(ne_input);
	duckdb::vector<string> ne_labels {"x", "(x <> 3)"};
	duckdb::vector<bool> ne_aux {false, true};

	SECTION("must-drop: a `<>` pinned onto its forbidden value is reported as a drop") {
		// Rigid bounds pin x to exactly 3, which x <> 3 forbids. Nothing is loosenable,
		// so the only fix is to drop the `<>` — reported as a DROP edit.
		SolverModel model = MakeNotEqualModel();
		for (char sense : {'<', '>'}) { // rigid x <= 3 AND x >= 3  → x == 3
			ModelConstraint pin;
			pin.indices = {0};
			pin.coefficients = {1.0};
			pin.sense = sense;
			pin.rhs = 3.0;
			pin.provenance.kind = ConstraintKind::STRUCTURAL;
			model.constraints.push_back(std::move(pin));
		}
		InfeasibleDiagnosisInput diag_input {model,  ne_indexer, ne_labels, ne_aux, kNoGlobalLabels,
		                                     params, false,      solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(diag.state == "infeasible");
		CHECK(Find(diag, "(x <> 3)").edit_source == "remove_only");
		CHECK(Find(diag, "(x <> 3)").suggested_change == "remove this clause");
		CHECK(!Find(diag, "(x <> 3)").has_amount);
	}

	SECTION("prefer-loosen: a loosenable knob is chosen over dropping a `<>`") {
		// x is pinned to 3 by a rigid floor and an EDITABLE cap, with x <> 3 forbidding
		// it. Loosening the cap or dropping the `<>` both work, so the engine loosens
		// and never drops because the minimum removal count is zero.
		SolverModel model = MakeNotEqualModel();
		ModelConstraint cap; // editable x <= 3
		cap.indices = {0};
		cap.coefficients = {1.0};
		cap.sense = '<';
		cap.rhs = 3.0;
		cap.provenance.kind = ConstraintKind::USER_PARAMETER;
		cap.provenance.shape = ElasticShape::SHARED_SCALAR;
		model.constraints.push_back(std::move(cap));
		ModelConstraint floor; // rigid x >= 3
		floor.indices = {0};
		floor.coefficients = {1.0};
		floor.sense = '>';
		floor.rhs = 3.0;
		floor.provenance.kind = ConstraintKind::STRUCTURAL;
		model.constraints.push_back(std::move(floor));

		InfeasibleDiagnosisInput diag_input {model,  ne_indexer, ne_labels, ne_aux, kNoGlobalLabels,
		                                     params, false,      solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(Find(diag, "x <= 3").suggested_change == "x <= 4");
		CHECK(Find(diag, "(x <> 3)").edit_source.empty()); // not dropped
	}

	// I4 follow-up — aggregate `<>` (`SUM(x) <> K`). The disjunction binary is a
	// GLOBAL-block column, so its name arrives via the global_variable_labels channel
	// (not var_labels). Same removal dial, named drop. The flat layout matches
	// MakeNotEqualModel (x = col 0, indicator = col 1); only the label source differs.
	SECTION("aggregate `<>`: a dropped global-indicator clause is named via global labels") {
		SolverInput agg_input;
		agg_input.num_rows = 1;
		agg_input.num_decide_vars = 1;
		agg_input.variable_types = {LogicalType::DOUBLE};
		agg_input.num_global_vars = 1; // the `<>` disjunction binary lives in the global block
		agg_input.global_variable_types = {LogicalType::BOOLEAN};
		agg_input.global_lower_bounds = {0.0};
		agg_input.global_upper_bounds = {1.0};
		agg_input.global_obj_coeffs = {0.0};
		agg_input.global_variable_labels = {"(SUM(x) <> 3)"}; // names global col 1
		VarIndexer agg_indexer = VarIndexer::Build(agg_input);
		agg_indexer.total_vars = agg_indexer.global_block_start + agg_input.num_global_vars;
		duckdb::vector<string> agg_labels {"x"};
		duckdb::vector<bool> agg_aux {false};

		SolverModel model = MakeNotEqualModel(); // col 1 = disjunction binary, indicator_col = 1
		for (char sense : {'<', '>'}) {          // pin x == 3 (the forbidden value)
			ModelConstraint pin;
			pin.indices = {0};
			pin.coefficients = {1.0};
			pin.sense = sense;
			pin.rhs = 3.0;
			pin.provenance.kind = ConstraintKind::STRUCTURAL;
			model.constraints.push_back(std::move(pin));
		}
		InfeasibleDiagnosisInput diag_input {model,      agg_indexer, agg_labels, agg_aux,
		                                     agg_input.global_variable_labels, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(diag.state == "infeasible");
		// Without the label channel this subject would be empty (nameless drop).
		CHECK(Find(diag, "(SUM(x) <> 3)").edit_source == "remove_only");
	}
}
