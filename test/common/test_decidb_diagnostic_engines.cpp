#include "catch.hpp"

#include "duckdb/decidb/decide_diagnostic_engines.hpp"
#include "duckdb/decidb/diagnostic_constants.hpp"
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
		// These model literal-RHS bounds (`x <= 5`), so they are SHARED_LITERAL — a
		// single editable knob — not per-row data (which routes to a conflict summary).
		c.provenance = {i, DConstants::INVALID_INDEX, rows[i].kind, ElasticShape::SHARED_LITERAL};
		m.constraints.push_back(std::move(c));
	}
	return m;
}

//! One linear constraint `coeff * x[var] <sense> rhs` over a multi-variable model,
//! with explicit provenance for exercising shared-slack blocks (I2.a): rows sharing
//! a `clause_id` and tagged SHARED_LITERAL collapse to one slack.
struct MultiVarRow {
	int var;
	double coeff;
	char sense;
	double rhs;
	ConstraintKind kind;
	idx_t clause_id = DConstants::INVALID_INDEX;
	ElasticShape shape = ElasticShape::PER_ROW_DATA;
	idx_t group_key = DConstants::INVALID_INDEX;
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
	for (const auto &r : rows) {
		ModelConstraint c;
		c.indices = {r.var};
		c.coefficients = {r.coeff};
		c.sense = r.sense;
		c.rhs = r.rhs;
		c.provenance.clause_id = r.clause_id;
		c.provenance.kind = r.kind;
		c.provenance.shape = r.shape;
		c.provenance.group_key = r.group_key;
		m.constraints.push_back(std::move(c));
	}
	return m;
}

//! First value of the EAV diagnosis row matching subject+attribute, or "".
string FindRow(const DecideDiagnostic &diag, const string &subject, const string &attribute) {
	for (const auto &r : diag.rows) {
		if (r.subject == subject && r.attribute == attribute) {
			return r.value;
		}
	}
	return string();
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
		VarIndexer indexer = VarIndexer::BuildRef(input);

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
		REQUIRE(diag.rows.size() == 2);
		CHECK(diag.rows[0].subject_kind == "variable");
		CHECK(diag.rows[0].subject == "x");
		CHECK(diag.rows[0].attribute == "grows_toward");
		CHECK(diag.rows[0].value == "+inf");
		CHECK(diag.rows[1].subject_kind == "variable");
		CHECK(diag.rows[1].subject == "x");
		CHECK(diag.rows[1].attribute == "affected_rows");
		CHECK(diag.rows[1].value == "2 of 2 rows where channel = 'export'");
	}

	SECTION("unbounded engine returns invalid diagnosis when ray names no variable") {
		SolverInput input = MakeRowScopedInput(1);
		input.num_global_vars = 1;
		input.global_variable_types = {LogicalType::DOUBLE};
		input.global_lower_bounds = {0.0};
		input.global_upper_bounds = {1e30};
		input.global_obj_coeffs = {0.0};
		VarIndexer indexer = VarIndexer::BuildRef(input);
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
	// the least-change fix. `input` outlives `indexer` (BuildRef references it).
	SolverInput input = MakeRowScopedInput(1);
	VarIndexer indexer = VarIndexer::BuildRef(input);
	duckdb::vector<string> labels {"x"};
	duckdb::vector<bool> is_aux {false};
	DecideDiagParams params;
	auto solve_highs = [](const SolverModel &m) { return SolvePreparedModel(m, SolverBackend::HIGHS); };

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
		CHECK(FindRow(diag, "x <= 5", "suggested_change") == "x <= 10");
		CHECK(FindRow(diag, "x <= 5", "amount") == "5");
		// The summary points to the problem clause; the structured rows above carry
		// the concrete edit. edit_kind is uniform.
		CHECK(FindRow(diag, "x <= 5", "edit_kind") == "loosen");
		CHECK(diag.summary.find("diagnosis points to clause `x <= 5`") != string::npos);
		CHECK(diag.summary.find("loosen `x <= 5` to `x <= 10`") == string::npos);
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
		CHECK(FindRow(diag, "x == 5", "suggested_change") == "x == 8");
		CHECK(FindRow(diag, "x == 5", "amount") == "3");
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
		REQUIRE(diag.rows.size() == 1);
		CHECK(diag.rows[0].subject_kind == "model");
		CHECK(diag.rows[0].attribute == "elastic_infeasible");
		CHECK(diag.rows[0].value == "true");
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
		// Objective is min Σ sᵢ: the user var carries weight 0, each slack weight 1.
		CHECK(elastic.model.obj_coeffs[0] == 0.0);
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

	SECTION("BuildElasticModel shares ONE slack across a SHARED_LITERAL block") {
		// I2.a structure: three rows of one clause (clause_id 0), tagged SHARED_LITERAL,
		// must collapse to a single shared slack column wired into all three rows — not
		// three independent slacks. A second clause stays its own block.
		SolverModel model = MakeModel(4, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL},
		    {2, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL},
		    {3, 1.0, '<', 9.0, ConstraintKind::USER_PARAMETER, 1, ElasticShape::SHARED_LITERAL},
		});
		ElasticModel elastic = BuildElasticModel(model);

		// 4 user vars + 1 shared slack (clause 0) + 1 slack (clause 1) = 6 columns.
		CHECK(elastic.model.num_vars == 6);
		REQUIRE(elastic.slacks.size() == 2);
		CHECK(elastic.slacks[0].rows == duckdb::vector<idx_t> {0, 1, 2});
		CHECK(elastic.slacks[1].rows == duckdb::vector<idx_t> {3});
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
		VarIndexer row_indexer = VarIndexer::BuildRef(row_input);
		duckdb::vector<string> row_labels {"x"};
		duckdb::vector<bool> row_aux {false};
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL},
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
		idx_t change_rows = 0;
		for (const auto &r : diag.rows) {
			if (r.attribute == "suggested_change") {
				change_rows++;
			}
		}
		CHECK(change_rows == 1);
		CHECK(FindRow(diag, "x <= 5", "amount") == "7");
		CHECK(FindRow(diag, "x <= 5", "suggested_change") == "x <= 12");
	}

	SECTION("PER: one shared slack per group, not per row and not one across all groups") {
		// I2.b: an easy-MAX-style clause under PER fans into N_g rows per group, all
		// tagged SHARED_LITERAL with the same clause_id but distinct group_key. Each
		// group is its own shared block: group 0 (rows 0,1) → one slack, group 1
		// (rows 2,3) → another. The runnable edit is one move per group.
		SolverModel model = MakeModel(4, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL, 0},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL, 0},
		    {2, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL, 1},
		    {3, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL, 1},
		});
		ElasticModel elastic = BuildElasticModel(model);

		// 4 user vars + one shared slack per group = 6 columns.
		CHECK(elastic.model.num_vars == 6);
		REQUIRE(elastic.slacks.size() == 2);
		CHECK(elastic.slacks[0].rows == duckdb::vector<idx_t> {0, 1});
		CHECK(elastic.slacks[1].rows == duckdb::vector<idx_t> {2, 3});
	}

	SECTION("data-RHS rows stay independent size-1 blocks") {
		// PER_ROW_DATA rows (a data RHS like `x <= col`) have no shared-literal collapse
		// and must remain independent size-1 blocks — pin that I2.a grouping did not
		// absorb them. (Aggregate PER rows are SHARED_LITERAL; this is the data path.)
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA, 0},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA, 1},
		});
		ElasticModel elastic = BuildElasticModel(model);

		REQUIRE(elastic.slacks.size() == 2);
		CHECK(elastic.slacks[0].rows == duckdb::vector<idx_t> {0});
		CHECK(elastic.slacks[1].rows == duckdb::vector<idx_t> {1});
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

	SECTION("data-RHS slack is penalized so an editable knob loosens first") {
		// I2.c: an editable cap (SHARED_LITERAL) and a data floor (PER_ROW_DATA) both
		// conflict with a rigid pin; the data slack carries a higher weight, so the
		// solver loosens the editable cap and leaves the data row alone.
		SolverModel model = MakeModel(1, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL},
		    {0, 1.0, '>', 3.0, ConstraintKind::USER_PARAMETER, 1, ElasticShape::PER_ROW_DATA},
		    {0, 1.0, '>', 10.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, kNoGlobalLabels, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		// The cap loosens to 10 (amount 5); the data floor is not touched.
		CHECK(FindRow(diag, "x <= 5", "suggested_change") == "x <= 10");
		CHECK(FindRow(diag, "x >= 3", "conflict").empty());
	}

	SECTION("data-RHS conflict reports a per-clause summary, not a suggestion") {
		// I2.c: when only data-RHS rows can move (rigid floors force them), report ONE
		// conflict summary per clause (M of N rows), with no suggested_change/amount.
		SolverInput row_input = MakeRowScopedInput(2);
		VarIndexer row_indexer = VarIndexer::BuildRef(row_input);
		duckdb::vector<string> row_labels {"x"};
		duckdb::vector<bool> row_aux {false};
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA},
		    {1, 1.0, '<', 5.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::PER_ROW_DATA},
		    {0, 1.0, '>', 8.0, ConstraintKind::STRUCTURAL},
		    {1, 1.0, '>', 12.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model,   row_indexer, row_labels,
		                                     row_aux, kNoGlobalLabels, params, false,
		                                     solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(FindRow(diag, "x <= 5", "conflict") == "conflicts in 2 of 2 rows");
		CHECK(FindRow(diag, "x <= 5", "suggested_change").empty());
		CHECK(FindRow(diag, "x <= 5", "amount").empty());
		// I5: a data-RHS conflict carries edit_kind='conflict' (uniform vocabulary), and
		// the summary points to the data-backed problem clause.
		CHECK(FindRow(diag, "x <= 5", "edit_kind") == "conflict");
		CHECK(diag.summary.find("diagnosis points to per-row data in clause `x <= 5`") != string::npos);
		CHECK(diag.summary.find("a possible edit was found") == string::npos);
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
		CHECK(FindRow(diag, "x < 5", "suggested_change") == "x < 11");
		CHECK(FindRow(diag, "x < 5", "amount") == "6");
	}

	SECTION("AVG row renders an AVG(...) label and reports the raw slack") {
		// I2.d: a pure AVG aggregate stores coefficients pre-scaled by 1/N (avg_scaled),
		// so the label collapses back to `AVG(x)` and the slack is reported in AVG units.
		SolverInput row_input = MakeRowScopedInput(2);
		VarIndexer row_indexer = VarIndexer::BuildRef(row_input);
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
		avg_row.provenance.clause_id = 0;
		avg_row.provenance.kind = ConstraintKind::USER_PARAMETER;
		avg_row.provenance.shape = ElasticShape::SHARED_LITERAL;
		avg_row.provenance.avg_scaled = true;
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
		CHECK(FindRow(diag, "AVG(x) <= 5", "suggested_change") == "AVG(x) <= 10");
		CHECK(FindRow(diag, "AVG(x) <= 5", "amount") == "5");
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
		qc.provenance.clause_id = 0;
		qc.provenance.kind = ConstraintKind::USER_PARAMETER;
		qc.provenance.shape = ElasticShape::SHARED_LITERAL;
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
	VarIndexer indexer2 = VarIndexer::BuildRef(input2);
	duckdb::vector<string> labels2 {"x", "y"};
	duckdb::vector<bool> is_aux2 {false, false};

	SECTION("stage 2 reports the objective-maximizing minimal fix among ties") {
		// Two editable caps x <= 0, y <= 0 conflict with the rigid floor x + y >= 10.
		// Total loosening S* = 10 with MANY minimizers (loosen x, loosen y, or split).
		// Freezing arbitrary stage-1 amounts could loosen y (giving objective 0);
		// stage 2 maximizes x, so it must loosen x's cap to 10 and report objective 10.
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 0.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL},
		    {1, 1.0, '<', 0.0, ConstraintKind::USER_PARAMETER, 1, ElasticShape::SHARED_LITERAL},
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
		CHECK(FindRow(diag, "x <= 0", "suggested_change") == "x <= 10");
		CHECK(FindRow(diag, "x <= 0", "amount") == "10");
		CHECK(FindRow(diag, "y <= 0", "suggested_change").empty());
		// The achievable objective is reported as a model-level fact, not in the summary.
		CHECK(FindRow(diag, "", "achievable_objective") == "10");
		CHECK(diag.summary.find("best achievable objective is 10") == string::npos);
	}

	SECTION("stage 2 reports an unbounded objective after the fix") {
		// x <= 0 (editable) conflicts with the rigid x >= 5: the unique fix loosens the
		// cap to 5. But y is free above and absent from every constraint, so MAXIMIZE y
		// has no finite optimum once feasible; the table reports that model-level fact.
		SolverModel model = MakeModel(2, {
		    {0, 1.0, '<', 0.0, ConstraintKind::USER_PARAMETER, 0, ElasticShape::SHARED_LITERAL},
		    {0, 1.0, '>', 5.0, ConstraintKind::STRUCTURAL},
		});
		model.obj_coeffs = {0.0, 1.0}; // MAXIMIZE y (unbounded)
		model.maximize = true;

		InfeasibleDiagnosisInput diag_input {model,    indexer2, labels2, is_aux2, kNoGlobalLabels,
		                                     params,   false,    solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(FindRow(diag, "x <= 0", "suggested_change") == "x <= 5");
		CHECK(FindRow(diag, "x <= 0", "amount") == "5");
		CHECK(FindRow(diag, "", "achievable_objective") == "unbounded");
		CHECK(diag.summary.find("objective is unbounded") == string::npos);
	}

	SECTION("diagnostics relation carries clause-shaped infeasible attributes") {
		DuckDB db(nullptr);
		Connection con(db);

		DecideDiagnostic diag;
		diag.valid = true;
		diag.status = SolverStatus::INFEASIBLE;
		diag.state = "infeasible";
		diag.summary = "test infeasible summary";
		DiagnosticRow row;
		row.subject_kind = "clause";
		row.subject = "0";
		row.attribute = "relaxation";
		row.value = "rhs + 1";
		diag.rows.push_back(std::move(row));
		StashDecideDiagnostic(*con.context, std::move(diag));

		auto result = con.Query("SELECT * FROM decide_diagnostics()");
		REQUIRE(!result->HasError());
		REQUIRE(result->RowCount() == 1);
		CHECK(result->GetValue(0, 0).ToString() == "1");
		CHECK(result->GetValue(1, 0).ToString() == "infeasible");
		CHECK(result->GetValue(2, 0).ToString() == "clause");
		CHECK(result->GetValue(3, 0).ToString() == "0");
		CHECK(result->GetValue(4, 0).ToString() == "relaxation");
		CHECK(result->GetValue(5, 0).ToString() == "rhs + 1");
	}

	SECTION("inf-or-unbounded caveat is query-message only") {
		DecideDiagnostic diag;
		diag.valid = true;
		diag.status = SolverStatus::UNBOUNDED;
		diag.state = "unbounded";
		diag.summary = "The objective is unbounded because x can grow without bound.";

		try {
			ThrowDecideDiagnosisReady(diag, "the problem may still be infeasible.");
			FAIL("expected ThrowDecideDiagnosisReady to throw");
		} catch (const InvalidInputException &ex) {
			string message = ex.what();
			CHECK(message.find("The objective is unbounded because x can grow without bound.") != string::npos);
			CHECK(message.find("the problem may still be infeasible.") != string::npos);
			CHECK(message.find("SELECT * FROM decide_diagnostics()") != string::npos);
		}

		CHECK(diag.summary == "The objective is unbounded because x can grow without bound.");
		CHECK(diag.summary.find("the problem may still be infeasible.") == string::npos);
	}

	// I4 — the L0 / removal dial. A remove-only `<>` cannot be loosened; instead the
	// elastic transform gives its disjunction pair one binary `w` (penalized above the
	// slack weights) that, set to 1, drops the clause.
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
		// One binary column appended (w = col 2), penalized DIAGNOSTIC_REMOVAL_WEIGHT.
		CHECK(elastic.model.num_vars == 3);
		CHECK(rem.w_col == 2);
		CHECK(elastic.model.is_binary[2]);
		CHECK(elastic.model.is_integer[2]);
		CHECK(elastic.model.col_lower[2] == 0.0);
		CHECK(elastic.model.col_upper[2] == 1.0);
		CHECK(elastic.model.obj_coeffs[2] == DIAGNOSTIC_REMOVAL_WEIGHT);
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
	VarIndexer ne_indexer = VarIndexer::BuildRef(ne_input);
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
		CHECK(FindRow(diag, "(x <> 3)", "edit_kind") == "drop");
		CHECK(diag.summary.find("diagnosis points to clause `(x <> 3)`") != string::npos);
		CHECK(diag.summary.find("remove `(x <> 3)`") == string::npos);
	}

	SECTION("prefer-loosen: a loosenable knob is chosen over dropping a `<>`") {
		// x is pinned to 3 by a rigid floor and an EDITABLE cap, with x <> 3 forbidding
		// it. Loosening the cap (cost 1) or dropping the `<>` (cost W) both work, so the
		// engine loosens and never drops (verifies removal weight > slack weight).
		SolverModel model = MakeNotEqualModel();
		ModelConstraint cap; // editable x <= 3
		cap.indices = {0};
		cap.coefficients = {1.0};
		cap.sense = '<';
		cap.rhs = 3.0;
		cap.provenance.kind = ConstraintKind::USER_PARAMETER;
		cap.provenance.shape = ElasticShape::SHARED_LITERAL;
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
		CHECK(FindRow(diag, "x <= 3", "suggested_change") == "x <= 4");
		CHECK(FindRow(diag, "(x <> 3)", "edit_kind").empty()); // not dropped
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
		VarIndexer agg_indexer = VarIndexer::BuildRef(agg_input);
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
		CHECK(FindRow(diag, "(SUM(x) <> 3)", "edit_kind") == "drop");
	}
}
