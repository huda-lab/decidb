#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/execution/operator/join/perfect_hash_join_executor.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/planner.hpp"

using namespace duckdb;

namespace {

//! The hash join of the physical plan of `sql`, or null.
optional_ptr<PhysicalHashJoin> FindHashJoin(PhysicalOperator &op) {
	if (op.type == PhysicalOperatorType::HASH_JOIN) {
		return op.Cast<PhysicalHashJoin>();
	}
	for (auto &child : op.children) {
		auto found = FindHashJoin(*child);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

//! Whether the executor takes the join of `sql` for a build side of the keys 1 to 5, with the statistics and nothing
//! built yet.
bool TakesPerfectHashJoin(Connection &con, const string &sql) {
	auto &context = *con.context;
	bool result = false;
	context.RunFunctionInTransaction([&]() {
		Parser parser;
		parser.ParseQuery(sql);
		Planner planner(context);
		planner.CreatePlan(std::move(parser.statements[0]));
		Optimizer optimizer(*planner.binder, context);
		auto logical = optimizer.Optimize(std::move(planner.plan));
		logical->ResolveOperatorTypes();
		PhysicalPlanGenerator generator(context);
		auto physical = generator.CreatePlan(std::move(logical));
		auto join = FindHashJoin(*physical);
		REQUIRE(join);
		auto hash_table = join->InitializeHashTable(context);
		PerfectHashJoinExecutor executor(*join, *hash_table);
		result = executor.CanDoPerfectHashJoin(*join, Value::INTEGER(1), Value::INTEGER(5));
	});
	return result;
}

} // namespace

// The perfect hash join takes the joins that keep every probe row as well as the inner join, on one integer equality
// condition; a join that pads the build side, one that filters, and any other key are for the hash table.
TEST_CASE("The perfect hash join takes the join types that keep a probe row", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE build (k INTEGER, v INTEGER, s VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE probe (id INTEGER, k INTEGER)"));
	// An empty table is replaced by an empty result before the physical plan.
	REQUIRE_NO_FAIL(con.Query("INSERT INTO build VALUES (1, 10, 'a'), (3, 30, 'c'), (5, 50, 'e')"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO probe SELECT i, i % 7 FROM range(100) t(i)"));
	auto join = [&](const string &type, const string &on) {
		return "SELECT * FROM probe " + type + " JOIN build ON " + on;
	};
	SECTION("the joins of one integer equality condition") {
		REQUIRE(TakesPerfectHashJoin(con, join("INNER", "probe.k = build.k")));
		REQUIRE(TakesPerfectHashJoin(con, join("LEFT", "probe.k = build.k")));
	}
	SECTION("the joins that pad the probe side or filter it") {
		REQUIRE(!TakesPerfectHashJoin(con, join("FULL", "probe.k = build.k")));
		REQUIRE(!TakesPerfectHashJoin(con, "SELECT * FROM probe WHERE probe.k IN (SELECT k FROM build)"));
	}
	SECTION("a condition that is not one integer equality") {
		REQUIRE(!TakesPerfectHashJoin(con, join("LEFT", "probe.k = build.k AND probe.id = build.v")));
		REQUIRE(!TakesPerfectHashJoin(con, join("LEFT", "probe.k IS NOT DISTINCT FROM build.k")));
		REQUIRE(!TakesPerfectHashJoin(con, join("LEFT", "CAST(probe.k AS VARCHAR) = build.s")));
	}
}
