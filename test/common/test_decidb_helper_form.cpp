#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/planner.hpp"

using namespace duckdb;

namespace {

LogicalDecide *FindFormDecide(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE) {
		return &op.Cast<LogicalDecide>();
	}
	for (auto &child : op.children) {
		if (auto *found = FindFormDecide(*child)) {
			return found;
		}
	}
	return nullptr;
}

//! The optimized plan of a query over (id, g, h, p, w).
duckdb::unique_ptr<LogicalOperator> OptimizedPlan(Connection &con, const string &decide_clause) {
	if (!con.HasActiveTransaction()) {
		con.BeginTransaction();
	}
	Parser parser;
	parser.ParseQuery("SELECT id FROM (FROM (VALUES (1, 1, 1, 1, 2.0::DOUBLE), (2, 1, 2, 2, 1.0::DOUBLE),"
	                  " (3, 2, 1, 3, 4.0::DOUBLE), (4, 2, 2, 4, 3.0::DOUBLE)) t(id, g, h, p, w) DECIDE " +
	                  decide_clause + ") q");
	Planner planner(*con.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *con.context);
	return optimizer.Optimize(std::move(planner.plan));
}

//! The form the optimizer gives each reducer helper of the query, by the helper's label.
duckdb::vector<std::pair<string, DecideHelperForm>> HelperForms(Connection &con, const string &decide_clause) {
	auto plan = OptimizedPlan(con, decide_clause);
	auto *decide = FindFormDecide(*plan);
	REQUIRE(decide != nullptr);
	duckdb::vector<std::pair<string, DecideHelperForm>> forms;
	for (auto &definition : decide->helper_definitions) {
		if (definition->kind == DecideHelperKind::REDUCER_VALUE) {
			forms.emplace_back(definition->label, definition->Cast<DecideReducerValueDefinition>().form);
		}
	}
	return forms;
}

} // namespace

namespace {

//! The form of the helper labelled `label`.
DecideHelperForm FormOf(const duckdb::vector<std::pair<string, DecideHelperForm>> &forms, const string &label) {
	for (auto &form : forms) {
		if (form.first == label) {
			return form.second;
		}
	}
	FAIL("no reducer helper is labelled " + label);
	return DecideHelperForm::VARIABLE;
}

} // namespace

TEST_CASE("A SUM in a nest of sums is written into its reader, and so is the SUM that holds it", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
	auto sums = HelperForms(
	    con, "x(REAL) SUCH THAT x <= 5 AND per t.g: sum(sum(x) by (t.g, t.h)) by (t.g) + t.w <= 9 MAXIMIZE sum(x)");
	REQUIRE(FormOf(sums, "sum(x) by (t.g, t.h)") == DecideHelperForm::WRITTEN_IN);
	REQUIRE(FormOf(sums, "sum(sum(x) by (t.g, t.h)) by (t.g)") == DecideHelperForm::WRITTEN_IN);
	// A MAX in the nest keeps its column; the SUM that reads it is still written in (deck p. 115).
	auto mixed = HelperForms(
	    con, "x(REAL) SUCH THAT x <= 5 AND per t.g: sum(max(x) by (t.g, t.h)) by (t.g) + t.w <= 9 MAXIMIZE sum(x)");
	REQUIRE(FormOf(mixed, "max(x) by (t.g, t.h)") == DecideHelperForm::ONE_MEMBER_WRITTEN_IN);
	REQUIRE(FormOf(mixed, "sum(max(x) by (t.g, t.h)) by (t.g)") == DecideHelperForm::WRITTEN_IN);
}

TEST_CASE("A lone SUM is written into the one clause whose rows lie within its groups, and by the data's count of "
          "rows under any other one clause",
          "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
	// Data the class key does not hold makes a row per value, so the plan cannot say one row reads each group: the
	// data does, group by group. (With none, the facility-location link is an aggregate clause and has no helper.)
	auto per_value = HelperForms(con, "x(REAL), per t.g: b(BOOL) SUCH THAT x <= 5 AND "
	                                  "per t.g: sum(x) by (t.g) <= t.w * b MAXIMIZE sum(x)");
	REQUIRE(FormOf(per_value, "sum(x) by (t.g)") == DecideHelperForm::ONE_ROW_WRITTEN_IN);
	auto no_class =
	    HelperForms(con, "x(REAL), per (): y(REAL) SUCH THAT x <= 5 AND sum(x) by (t.g) <= t.w * y MAXIMIZE sum(x)");
	REQUIRE(FormOf(no_class, "sum(x) by (t.g)") == DecideHelperForm::ONE_ROW_WRITTEN_IN);
	// A decision read per row makes a row per input row.
	auto per_row = HelperForms(con, "x(REAL) SUCH THAT x <= 5 AND x <= 0.5 * sum(x) by (t.g) + 1 MAXIMIZE sum(x)");
	REQUIRE(FormOf(per_row, "sum(x) by (t.g)") == DecideHelperForm::ONE_ROW_WRITTEN_IN);
	// A row per group of `g` reads the query's sum from outside its one group, and its own group's from inside.
	auto coarser = HelperForms(con, "x(REAL) SUCH THAT x <= 5 AND sum(x) by (t.g) + sum(x) <= 9 MAXIMIZE sum(x)");
	REQUIRE(FormOf(coarser, "sum(x) by (t.g)") == DecideHelperForm::WRITTEN_IN);
	REQUIRE(FormOf(coarser, "sum(x)") == DecideHelperForm::ONE_ROW_WRITTEN_IN);
	// Neither key holds the other.
	auto crossed =
	    HelperForms(con, "x(REAL) SUCH THAT x <= 5 AND sum(x) by (t.g) + sum(x) by (t.h) <= 9 MAXIMIZE sum(x)");
	REQUIRE(FormOf(crossed, "sum(x) by (t.g)") == DecideHelperForm::ONE_ROW_WRITTEN_IN);
	REQUIRE(FormOf(crossed, "sum(x) by (t.h)") == DecideHelperForm::ONE_ROW_WRITTEN_IN);
	// A clause's classes finer than the groups read each group once per class.
	auto finer =
	    HelperForms(con, "x(REAL) SUCH THAT x <= 5 AND per t.id: sum(x) by (t.g) + sum(x) <= 9 MAXIMIZE sum(x)");
	REQUIRE(FormOf(finer, "sum(x) by (t.g)") == DecideHelperForm::ONE_ROW_WRITTEN_IN);
	// Two clauses read it: the column.
	auto twice = HelperForms(con, "x(REAL) SUCH THAT x <= 5 AND sum(x) by (t.g) + sum(x) <= 9 AND "
	                              "sum(x) by (t.g) + 2 * sum(x) <= 12 MAXIMIZE sum(x)");
	REQUIRE(FormOf(twice, "sum(x) by (t.g)") == DecideHelperForm::VARIABLE);
}

TEST_CASE("A SUM that is a member of a MIN or MAX is written in unless the MIN or MAX is pinned on both sides",
          "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
	auto one_side = HelperForms(con, "x(REAL) SUCH THAT x <= 5 MAXIMIZE sum(x) - max(sum(x) by (t.g))");
	REQUIRE(FormOf(one_side, "sum(x) by (t.g)") == DecideHelperForm::WRITTEN_IN);
	REQUIRE(FormOf(one_side, "max(sum(x) by (t.g))") == DecideHelperForm::ONE_MEMBER_WRITTEN_IN);
	// `=` pins the MAX on both sides, so each member stands in two rows.
	auto both_sides = HelperForms(
	    con, "x(REAL) SUCH THAT x <= 5 AND per t.g: max(sum(x) by (t.g, t.h)) by (t.g) = 4 MAXIMIZE sum(x)");
	REQUIRE(FormOf(both_sides, "sum(x) by (t.g, t.h)") == DecideHelperForm::VARIABLE);
}

TEST_CASE("A range is written into its one reader when its window moves with the row", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
	auto moving = HelperForms(con, "x(REAL) SUCH THAT x <= 5 AND x <= sum(from previous to previous else 0: x) "
	                               "over (t.p) + t.w MAXIMIZE sum(x)");
	REQUIRE(FormOf(moving, "sum(from previous to previous else 0: x) over (t.p)") == DecideHelperForm::WRITTEN_IN);
	// A whole timeline is one window every row of it reads: the data says whether that is one row.
	auto whole = HelperForms(
	    con, "x(REAL) SUCH THAT x <= 5 AND x <= sum(from first to last: x) over (t.p) - t.w MAXIMIZE sum(x)");
	REQUIRE(FormOf(whole, "sum(from first to last: x) over (t.p)") == DecideHelperForm::ONE_ROW_WRITTEN_IN);
	// A window a SUM reads is written into the SUM, which every row reads: the data says whether that is one row.
	auto summed = HelperForms(con, "x(REAL) SUCH THAT x <= 5 AND x <= sum(sum(from previous to previous else 0: x) "
	                               "over (t.p)) + 1 MAXIMIZE sum(x)");
	REQUIRE(FormOf(summed, "sum(from previous to previous else 0: x) over (t.p)") == DecideHelperForm::WRITTEN_IN);
	REQUIRE(FormOf(summed, "sum(sum(from previous to previous else 0: x) over (t.p))") ==
	        DecideHelperForm::ONE_ROW_WRITTEN_IN);
	// A one-sided ABS states its operand in two rows, so each window has two readers.
	auto twice = HelperForms(con, "x(REAL) SUCH THAT x <= 5 AND abs(x - sum(from previous to previous else 0: x) "
	                              "over (t.p)) <= 4 MAXIMIZE sum(x)");
	REQUIRE(FormOf(twice, "sum(from previous to previous else 0: x) over (t.p)") == DecideHelperForm::VARIABLE);
}

TEST_CASE("A helper a reader states by its column keeps the column", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
	// A product reads its factors' columns.
	auto product = HelperForms(con, "x(REAL), b(BOOL) SUCH THAT x <= 5 AND b * sum(x) by (t.g) <= 4 MAXIMIZE sum(x)");
	REQUIRE(FormOf(product, "sum(x) by (t.g)") == DecideHelperForm::VARIABLE);
	auto square = HelperForms(
	    con, "x(REAL) SUCH THAT x <= 5 AND per t.g: max(x) by (t.g) * max(x) by (t.h) <= 4 MAXIMIZE sum(x)");
	REQUIRE(FormOf(square, "max(x) by (t.g)") == DecideHelperForm::VARIABLE);
	// A bound on the helper alone may fold into its column's box.
	auto bound = HelperForms(
	    con, "x(REAL) SUCH THAT x <= 5 AND per t.g: max(sum(x) by (t.g, t.h)) by (t.g) <= 4 MAXIMIZE sum(x)");
	REQUIRE(FormOf(bound, "max(sum(x) by (t.g, t.h)) by (t.g)") == DecideHelperForm::VARIABLE);
	// An exact ABS states its sign rows from its operand's columns; a one-sided one states plain rows.
	auto exact = HelperForms(
	    con, "x(REAL) SUCH THAT x <= 5 AND per t.g: abs(max(x) by (t.g, t.h) - 3) + t.w >= 2 MAXIMIZE sum(x)");
	REQUIRE(FormOf(exact, "max(x) by (t.g, t.h)") == DecideHelperForm::VARIABLE);
	auto one_sided = HelperForms(
	    con, "x(REAL) SUCH THAT x <= 5 AND per t.g: abs(max(x) by (t.g, t.h) - 3) + t.w <= 9 MAXIMIZE sum(x)");
	REQUIRE(FormOf(one_sided, "max(x) by (t.g, t.h)") == DecideHelperForm::ONE_MEMBER_WRITTEN_IN);
}

// An account is of one helper, whatever it prints as: two helpers of one label, under two clauses, each say what they
// became (RewriteAccount::helper).
TEST_CASE("A helper's account follows its own form", "[decidb][helper_form]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto plan = OptimizedPlan(con, "x(REAL), b(BOOL) SUCH THAT x <= 5 AND when t.p > 1: b * sum(x) by (t.g) <= 4 AND "
	                               "sum(x) by (t.g) + sum(x) <= 9 MAXIMIZE sum(x)");
	auto *decide = FindFormDecide(*plan);
	REQUIRE(decide != nullptr);
	using Kind = LogicalDecide::RewriteAccount::Kind;
	duckdb::vector<Kind> kinds;
	for (auto &account : decide->rewrite_accounts) {
		if (account.subject == "sum(x) by (t.g)") {
			kinds.push_back(account.kind);
		}
	}
	// The product's factor keeps its column; the plain row's is written in.
	REQUIRE(kinds == duckdb::vector<Kind> {Kind::HELPER_PER_GROUP, Kind::VALUE_HELPER_INLINED});
}
