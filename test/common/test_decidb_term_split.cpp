#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

using namespace duckdb;

namespace {

LogicalDecide *FindTermSplitDecide(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE) {
		return &op.Cast<LogicalDecide>();
	}
	for (auto &child : op.children) {
		if (auto *found = FindTermSplitDecide(*child)) {
			return found;
		}
	}
	return nullptr;
}

//! The canonical objective of a DECIDE query over (x, y) decisions and (w, t) data, before any DECIDE rewrite.
struct CanonicalObjective {
	CanonicalObjective(Connection &con, const string &objective) {
		REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
		plan = con.ExtractPlan("SELECT x FROM (FROM (VALUES (1, 2.0::DOUBLE, 3.0::DOUBLE)) s(id, w, t)"
		                       " DECIDE x(INT), y(INT) SUCH THAT x <= 5 AND y <= 5 MINIMIZE " +
		                       objective + ") q");
		decide = FindTermSplitDecide(*plan);
		REQUIRE(decide != nullptr);
	}
	duckdb::unique_ptr<LogicalOperator> plan;
	LogicalDecide *decide;
};

//! The terms of the objective's single reducer body.
duckdb::vector<DecideSplitTerm> SplitObjectiveBody(Connection &con, CanonicalObjective &objective) {
	auto atoms = ReadCanonicalAtoms(*objective.decide->decide_objective, objective.decide->decide_index);
	REQUIRE(atoms.size() == 1);
	REQUIRE(atoms[0].term->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
	auto &aggregate = atoms[0].term->Cast<BoundAggregateExpression>();
	DecideTermSplitter splitter(*con.context, objective.decide->decide_index, objective.decide->decide_variables);
	duckdb::vector<DecideSplitTerm> terms;
	splitter.Split(*aggregate.children[0], atoms[0].sign, terms);
	return terms;
}

} // namespace

TEST_CASE("Canonicalization replaces norm with its definition and keeps the written spelling", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	struct Case {
		const char *objective;
		const char *reducer;
	};
	for (auto &test : {Case {"norm(x - t, 1)", "sum"}, Case {"norm(x - t, 2)", "sum"}, Case {"norm(x, 'inf')", "max"}}) {
		CanonicalObjective objective(con, test.objective);
		auto atoms = ReadCanonicalAtoms(*objective.decide->decide_objective, objective.decide->decide_index);
		REQUIRE(atoms.size() == 1);
		auto &aggregate = atoms[0].term->Cast<BoundAggregateExpression>();
		REQUIRE(StringUtil::Lower(aggregate.function.name) == test.reducer);
		REQUIRE(!HasDecideTag(aggregate.GetAlias(), NORM_MARKER_TAG_PREFIX));
		REQUIRE(HasDecideTag(aggregate.GetAlias(), WRITTEN_NORM_TAG_PREFIX));
	}
	// L0 needs indicator variables, so its marker stays for the optimizer.
	CanonicalObjective l0(con, "norm(x, 0)");
	auto atoms = ReadCanonicalAtoms(*l0.decide->decide_objective, l0.decide->decide_index);
	REQUIRE(atoms.size() == 1);
	REQUIRE(HasDecideTag(atoms[0].term->GetAlias(), NORM_MARKER_TAG_PREFIX));
}

TEST_CASE("Canonical atoms carry the sign and the factor canonicalization left on a reducer", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	CanonicalObjective objective(con, "SUM(w * x) - 2 * SUM(y)");
	auto atoms = ReadCanonicalAtoms(*objective.decide->decide_objective, objective.decide->decide_index);
	REQUIRE(atoms.size() == 2);
	REQUIRE(atoms[0].sign == 1);
	REQUIRE(!atoms[0].scaled);
	REQUIRE(atoms[1].sign == -1);
	REQUIRE(atoms[1].scaled);
	REQUIRE(atoms[1].scale.aggregate != nullptr);
}

TEST_CASE("The term splitter names every term kind and never reads ABS(x) as x", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	SECTION("linear and constant") {
		CanonicalObjective objective(con, "SUM(w * x + t)");
		auto terms = SplitObjectiveBody(con, objective);
		REQUIRE(terms.size() == 2);
		REQUIRE(terms[0].kind == DecideTermKind::LINEAR);
		REQUIRE(terms[0].var_a == 0);
		REQUIRE(terms[1].kind == DecideTermKind::CONSTANT);
	}
	SECTION("product of two decisions") {
		CanonicalObjective objective(con, "SUM(w * x * y)");
		auto terms = SplitObjectiveBody(con, objective);
		REQUIRE(terms.size() == 1);
		REQUIRE(terms[0].kind == DecideTermKind::PRODUCT);
		REQUIRE(terms[0].var_a != terms[0].var_b);
	}
	SECTION("square") {
		CanonicalObjective objective(con, "SUM(POWER(x - t, 2))");
		auto terms = SplitObjectiveBody(con, objective);
		REQUIRE(terms.size() == 1);
		REQUIRE(terms[0].kind == DecideTermKind::SQUARE);
		REQUIRE(terms[0].inner.size() == 2);
		REQUIRE(terms[0].inner[0].kind == DecideTermKind::LINEAR);
		REQUIRE(terms[0].inner[1].kind == DecideTermKind::CONSTANT);
		REQUIRE(terms[0].inner[1].sign == -1);
	}
	SECTION("absolute value, alone and with a data coefficient") {
		for (auto &body : {"ABS(x - t)", "w * ABS(x - t)"}) {
			CanonicalObjective objective(con, string("SUM(") + body + ")");
			auto terms = SplitObjectiveBody(con, objective);
			REQUIRE(terms.size() == 1);
			REQUIRE(terms[0].kind == DecideTermKind::ABS);
			REQUIRE(terms[0].inner.size() == 2);
			REQUIRE(terms[0].inner[0].kind == DecideTermKind::LINEAR);
		}
	}
	SECTION("a decision under ABS inside a linear expression is unknown") {
		CanonicalObjective objective(con, "SUM(POWER(ABS(x), 2))");
		auto terms = SplitObjectiveBody(con, objective);
		REQUIRE(terms.size() == 1);
		REQUIRE(terms[0].kind == DecideTermKind::SQUARE);
		REQUIRE(terms[0].inner.size() == 1);
		REQUIRE(terms[0].inner[0].kind == DecideTermKind::UNKNOWN);
		REQUIRE(!terms[0].inner[0].user_error);
	}
}
