#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

using namespace duckdb;

namespace {

//! The text the parser kept for every node of `expr`, outermost first: the span the user wrote for each expression
//! of the DECIDE tail (DecideSourceText, role EXPR).
void CollectSpans(const ParsedExpression &expr, const DecideSourceText &text, duckdb::vector<string> &out) {
	string cut;
	if (text.Cut(expr.GetQueryLocation(), DecideSpanRole::EXPR, cut)) {
		out.push_back(cut);
	}
	ParsedExpressionIterator::EnumerateChildren(expr,
	                                            [&](const ParsedExpression &child) { CollectSpans(child, text, out); });
}

//! Every span the tail's constraints and objective have, as the user wrote them.
duckdb::vector<string> TailSpans(const string &sql) {
	Parser parser;
	parser.ParseQuery(sql);
	REQUIRE(parser.statements.size() == 1);
	auto &select = parser.statements[0]->Cast<SelectStatement>().node->Cast<SelectNode>();
	duckdb::vector<string> spans;
	CollectSpans(*select.decide_constraints, select.decide_text, spans);
	if (select.decide_objective) {
		CollectSpans(*select.decide_objective, select.decide_text, spans);
	}
	return spans;
}

bool Has(const duckdb::vector<string> &spans, const string &text) {
	return std::find(spans.begin(), spans.end(), text) != spans.end();
}

} // namespace

TEST_CASE("Every expression of a DECIDE tail has the span the user wrote", "[decidb]") {
	// The decorator reduction of a_expr and b_expr records each one; the transformer keys it by query location.
	auto spans = TailSpans("SELECT x FROM t DECIDE x(INT) SUCH THAT sum(x) <= 3 AND x BETWEEN 0 AND 1 "
	                       "MAXIMIZE (w + 1) * sum(x)");
	REQUIRE(Has(spans, "sum(x) <= 3"));
	REQUIRE(Has(spans, "sum(x)"));
	REQUIRE(Has(spans, "3"));
	REQUIRE(Has(spans, "x BETWEEN 0 AND 1"));
	REQUIRE(Has(spans, "0"));
	REQUIRE(Has(spans, "(w + 1) * sum(x)"));
}

TEST_CASE("A parenthesized expression's span leaves the parentheses out", "[decidb]") {
	// noteDecideParen: `(w + 1)` returns the node of `w + 1`, so the decorator over the parentheses is skipped.
	auto spans = TailSpans("SELECT x FROM t DECIDE x(INT) SUCH THAT sum(x) <= 3 MAXIMIZE (w + 1) * sum(x)");
	REQUIRE(Has(spans, "w + 1"));
	REQUIRE(!Has(spans, "(w + 1)"));
}

TEST_CASE("A flattened AND keeps the span of the whole conjunction", "[decidb]") {
	// makeAndExpr returns its left operand's node, so the node is decorated again with the wider span, which wins.
	auto spans = TailSpans("SELECT x FROM t DECIDE x(INT) SUCH THAT when (w > 1 AND w < 5) AND c > 0: sum(x) <= 3");
	REQUIRE(Has(spans, "(w > 1 AND w < 5) AND c > 0"));
	REQUIRE(Has(spans, "c > 0"));
}

TEST_CASE("A negated constant's span starts at the minus sign", "[decidb]") {
	// doNegate edits the constant in place and moves its location to the minus sign.
	auto spans = TailSpans("SELECT x FROM t DECIDE x(INT) SUCH THAT sum(x) >= -2");
	REQUIRE(Has(spans, "-2"));
}

TEST_CASE("A parameter and a comment inside an expression are kept as written", "[decidb]") {
	// A parameter has a location (TransformParamRef), and a cut keeps the comment between its tokens.
	auto spans = TailSpans("SELECT x FROM t DECIDE x(INT) SUCH THAT sum(x) <= $1 AND sum(x /* kg */ * w) >= 1");
	REQUIRE(Has(spans, "$1"));
	REQUIRE(Has(spans, "x /* kg */ * w"));
}
