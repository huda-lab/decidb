#include "catch.hpp"

#include "duckdb/decidb/decide_diagnostic_engines.hpp"
#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb.hpp"

using namespace duckdb;

namespace {

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
		c.provenance = {i, DConstants::INVALID_INDEX, rows[i].kind};
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
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, params, false, solve_highs};
		DecideDiagnostic diag = DiagnoseInfeasible(diag_input);

		REQUIRE(diag.valid);
		CHECK(diag.state == "infeasible");
		CHECK(FindRow(diag, "x <= 5", "suggested_change") == "x <= 10");
		CHECK(FindRow(diag, "x <= 5", "amount") == "5");
		CHECK(diag.summary.find("Loosen x <= 5 to x <= 10") != string::npos);
	}

	SECTION("equality row loosens via its two-sided slack") {
		// x = 5 (relaxable) conflicts with the rigid x >= 8: the equality must move
		// up to 8 (s⁺ = 3). The `=` two-slack form must report the net edit.
		SolverModel model = MakeSingleVarModel({
		    {1.0, '=', 5.0, ConstraintKind::USER_PARAMETER},
		    {1.0, '>', 8.0, ConstraintKind::STRUCTURAL},
		});
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, params, false, solve_highs};
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
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, params, false, solve_highs};
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
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, params,
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
		InfeasibleDiagnosisInput diag_input {model, indexer, labels, is_aux, params, false, solve_highs};
		CHECK(!DiagnoseInfeasible(diag_input).valid);
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
}
