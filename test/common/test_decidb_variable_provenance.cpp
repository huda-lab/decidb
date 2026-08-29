#include "catch.hpp"

#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/solver_input.hpp"

#include <utility>

using namespace duckdb;

// F6 variable provenance (column-side complement of F2). BuildColumnProvenance
// inverts the VarIndexer flat-column layout so a solver column resolves back to
// the user variable name (USER), the aux source expression (AUX), or an unnamed
// internal global auxiliary (GLOBAL_AUX). Built directly on a VarIndexer +
// pre-extracted labels, mirroring the F2 constraint-provenance unit test.

namespace {

// SolverInput whose VarIndexer is all row-scoped decide vars plus a global block.
SolverInput MakeRowScopedInput(idx_t num_rows, idx_t num_vars, idx_t num_global) {
	SolverInput input;
	input.num_rows = num_rows;
	input.num_decide_vars = num_vars;
	input.variable_types.assign(num_vars, LogicalType::DOUBLE);
	input.num_global_vars = num_global;
	input.global_variable_types.assign(num_global, LogicalType::DOUBLE);
	input.global_lower_bounds.assign(num_global, 0.0);
	input.global_upper_bounds.assign(num_global, 1e30);
	return input;
}

} // namespace

TEST_CASE("DeciDB F6 variable provenance", "[decidb][query_diagnostics][provenance]") {
	SECTION("row-scoped user + aux columns resolve to name / expression") {
		// 2 rows, 3 decide vars: x (user), y (user), aux (ABS(x + y)); 1 global var.
		SolverInput input = MakeRowScopedInput(2, 3, 1);
		VarIndexer indexer = VarIndexer::Build(input);

		duckdb::vector<string> labels = {"x", "y", "ABS(x + y)"};
		duckdb::vector<bool> is_aux = {false, false, true};
		auto prov = BuildColumnProvenance(indexer, labels, is_aux);

		// Layout: row block of num_rows * num_row_vars = 2 * 3 = 6, then 1 global.
		REQUIRE(prov.size() == 7);
		REQUIRE(indexer.total_vars == 7);

		// x at rows 0,1 -> Get(0,r) = r*3 + 0 = {0, 3}
		for (idx_t r = 0; r < 2; r++) {
			auto &p = prov[indexer.Get(0, r)];
			CHECK(p.kind == ColumnKind::USER);
			CHECK(p.label == "x");
			CHECK(p.decide_var_idx == 0);
			CHECK(p.instance == r);
		}
		// y at rows 0,1 -> Get(1,r) = r*3 + 1 = {1, 4}
		for (idx_t r = 0; r < 2; r++) {
			auto &p = prov[indexer.Get(1, r)];
			CHECK(p.kind == ColumnKind::USER);
			CHECK(p.label == "y");
		}
		// aux at rows 0,1 -> Get(2,r) = r*3 + 2 = {2, 5} resolves to the source expression
		for (idx_t r = 0; r < 2; r++) {
			auto &p = prov[indexer.Get(2, r)];
			CHECK(p.kind == ColumnKind::AUX);
			CHECK(p.label == "ABS(x + y)");
			CHECK(p.decide_var_idx == 2);
		}
		// Global-block column (index 6) stays an unnamed internal auxiliary.
		CHECK(prov[6].kind == ColumnKind::GLOBAL_AUX);
		CHECK(prov[6].label.empty());
		CHECK(prov[6].decide_var_idx == DConstants::INVALID_INDEX);
	}

	SECTION("each linearization aux kind resolves to its user expression") {
		// One column per aux kind: ABS / MAX / product / <>. 1 row keeps it flat.
		SolverInput input = MakeRowScopedInput(1, 4, 0);
		VarIndexer indexer = VarIndexer::Build(input);

		duckdb::vector<string> labels = {"ABS(x)", "MAX(q)", "(b * x)", "(a <> b)"};
		duckdb::vector<bool> is_aux = {true, true, true, true};
		auto prov = BuildColumnProvenance(indexer, labels, is_aux);

		REQUIRE(prov.size() == 4);
		for (idx_t v = 0; v < 4; v++) {
			auto &p = prov[indexer.Get(v, 0)];
			CHECK(p.kind == ColumnKind::AUX);
			CHECK(p.label == labels[v]);
			CHECK(p.decide_var_idx == v);
		}
	}

	SECTION("entity-scoped columns collapse rows onto one column per entity") {
		// 4 rows, 2 vars: x row-scoped (user), cap entity-scoped (user) over 2 entities.
		SolverInput input = MakeRowScopedInput(4, 2, 0);
		EntityMapping mapping;
		mapping.num_entities = 2;
		mapping.row_to_entity = {0, 0, 1, 1};
		input.entity_mappings.push_back(mapping);
		input.variable_scopes = {DecideVarScopeInfo::Row(), DecideVarScopeInfo::Entity(0)};

		VarIndexer indexer = VarIndexer::Build(input);
		duckdb::vector<string> labels = {"x", "cap"};
		duckdb::vector<bool> is_aux = {false, false};
		auto prov = BuildColumnProvenance(indexer, labels, is_aux);

		// Row block: num_rows * num_row_vars = 4 * 1 = 4 (only x is row-scoped),
		// then 2 entity columns for cap. No global block.
		REQUIRE(prov.size() == 6);
		REQUIRE(indexer.total_vars == 6);

		// x occupies the 4 row-block columns, one per row.
		for (idx_t r = 0; r < 4; r++) {
			auto &p = prov[indexer.Get(0, r)];
			CHECK(p.kind == ColumnKind::USER);
			CHECK(p.label == "x");
			CHECK(p.instance == r);
		}
		// cap collapses rows {0,1} -> entity 0 and {2,3} -> entity 1 onto 2 columns.
		idx_t cap_e0 = indexer.Get(1, 0);
		idx_t cap_e1 = indexer.Get(1, 2);
		CHECK(cap_e0 != cap_e1);
		CHECK(indexer.Get(1, 1) == cap_e0); // row 1 shares entity 0's column
		CHECK(indexer.Get(1, 3) == cap_e1); // row 3 shares entity 1's column
		CHECK(prov[cap_e0].kind == ColumnKind::USER);
		CHECK(prov[cap_e0].label == "cap");
		CHECK(prov[cap_e0].instance == 0); // entity id
		CHECK(prov[cap_e1].instance == 1);
	}
}
