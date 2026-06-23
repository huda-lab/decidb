#include "catch.hpp"

#include "duckdb/decidb/decide_diagnostic_engines.hpp"
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
		CHECK(diag.rows[0].attribute == "direction");
		CHECK(diag.rows[0].value == "+inf");
		CHECK(diag.rows[1].subject_kind == "variable");
		CHECK(diag.rows[1].subject == "x");
		CHECK(diag.rows[1].attribute == "escaping_instances");
		CHECK(diag.rows[1].value == "channel=export (2/2)");
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
