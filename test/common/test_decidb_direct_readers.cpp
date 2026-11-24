#include "catch.hpp"
#include "duckdb/planner/decide/decide_tree_nodes.hpp"
#include "test_helpers.hpp"

#include "duckdb/optimizer/decide/decide_optimizer.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include <algorithm>

using namespace duckdb;

namespace {

LogicalDecide *FindReadDecide(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE) {
		return &op.Cast<LogicalDecide>();
	}
	for (auto &child : op.children) {
		if (auto *found = FindReadDecide(*child)) {
			return found;
		}
	}
	return nullptr;
}

//! The optimized plan of `sql` with the DECIDE optimizer off, as a rule reads it: bound
//! columns not yet resolved to chunk positions, which `ExtractPlan` also does.
duckdb::unique_ptr<LogicalOperator> ReadPlan(Connection &con, const string &sql) {
	if (!con.HasActiveTransaction()) {
		con.BeginTransaction();
	}
	Parser parser;
	parser.ParseQuery(sql);
	Planner planner(*con.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *con.context);
	return optimizer.Optimize(std::move(planner.plan));
}

//! What the rules' readers read of one DECIDE query, before any DECIDE rewrite.
struct QueryReading {
	QueryReading(Connection &con, const string &sql) {
		REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
		// A reader can rebind a coefficient, which reads the catalog; the optimizer always has a transaction.
		plan = ReadPlan(con, sql);
		decide = FindReadDecide(*plan);
		REQUIRE(decide != nullptr);
		client = con.context.get();
		Read();
	}
	void Read() {
		clauses.clear();
		objective = DirectObjective();
		clauses_read = DirectReadClauses(*client, *decide, clauses);
		objective_read = DirectReadObjective(*client, *decide, objective);
	}
	//! Gives every norm marker in the constraints the order `order` and reads again.
	void ReorderNorms(DecideNorm::Order order) {
		for (auto &clause : decide->bound.clauses) {
			ExpressionIterator::EnumerateExpression(clause.body, [&](Expression &expression) {
				if (IsDecideNormMarker(expression)) {
					expression.Cast<BoundDecideReducerExpression>().norm.order = order;
				}
			});
		}
		Read();
	}
	duckdb::unique_ptr<LogicalOperator> plan;
	LogicalDecide *decide = nullptr;
	ClientContext *client = nullptr;
	//! The clauses read, up to and including the first one no rule's plan reads when `clauses_read` is false.
	duckdb::vector<DirectClause> clauses;
	bool clauses_read = false;
	DirectObjective objective;
	bool objective_read = false;
};

//! A query over three rows of (id, w, cap, flag) with the given declaration, constraints and objective.
string ReadQuery(const string &declaration, const string &constraints, const string &objective) {
	return "SELECT id FROM (FROM (VALUES (1, 2.0::DOUBLE, 3, TRUE), (2, 1.0::DOUBLE, 1, FALSE),"
	       " (3, 4.0::DOUBLE, 2, TRUE)) t(id, w, cap, flag) DECIDE " +
	       declaration + " SUCH THAT " + constraints + " " + objective + ") q";
}

DirectSolveRule *FindRuleByName(duckdb::vector<duckdb::unique_ptr<DirectSolveRule>> &rules, const string &name) {
	for (auto &rule : rules) {
		if (string(rule->Name()) == name) {
			return rule.get();
		}
	}
	return nullptr;
}

DirectSolveRule *FindS1Rule(duckdb::vector<duckdb::unique_ptr<DirectSolveRule>> &rules) {
	return FindRuleByName(rules, "S1_UNIFORM_MATROID");
}

//! Whether `rule` applies to the query.
bool RuleApplies(DirectSolveRule &rule, Connection &con, const string &declaration, const string &constraints,
                 const string &objective, string *condition_text = nullptr) {
	QueryReading query(con, ReadQuery(declaration, constraints, objective));
	auto condition = rule.CheckCondition(*query.decide, *con.context);
	if (!condition) {
		return false;
	}
	if (condition_text) {
		*condition_text = rule.Summary();
	}
	return true;
}

} // namespace

TEST_CASE("A clause the query text holds no place for is read by no rule", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryReading query(con, "SELECT id, x FROM ("
	                        " FROM (VALUES (1, 9.0::DOUBLE), (2, -1.0::DOUBLE)) s(id,score)"
	                        " DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)"
	                        ") q");
	REQUIRE(DirectDomainOf(*query.decide, 0) == DirectDomain::BOOL);
	REQUIRE(query.clauses_read);
	REQUIRE(query.clauses.size() == 1);
	REQUIRE(query.objective_read);
	REQUIRE(query.objective.parts.size() == 1);
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	REQUIRE(s1->CheckCondition(*query.decide, *con.context) != nullptr);
	REQUIRE(s1->Summary() == "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi");

	// A clause with no provenance has no name in a message: its reader declines it, and so does the rule.
	query.decide->bound.clauses[0].origin = DecideRowOrigin();
	query.Read();
	REQUIRE(!query.clauses_read);
	REQUIRE(s1->CheckCondition(*query.decide, *con.context) == nullptr);
}

TEST_CASE("The source facts hold the source columns declared NOT NULL", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (a INTEGER NOT NULL, b INTEGER, k INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 2, 3)"));
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();

	// The source columns of `t`, in order, are a, b and k.
	auto read = [&](const string &source) {
		auto plan =
		    ReadPlan(con, "SELECT * FROM (FROM " + source + " DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(x)) q");
		auto *decide = FindReadDecide(*plan);
		REQUIRE(decide != nullptr);
		return DirectSourceFacts(*decide).not_null;
	};
	// A declared column is in it, and so is a primary key; an undeclared one is not.
	auto scanned = read("t");
	REQUIRE(scanned.size() == 2);
	REQUIRE(scanned[0].table_index == scanned[1].table_index);
	REQUIRE(scanned[0].column_index == 0);
	REQUIRE(scanned[1].column_index == 2);
	// A filter passes the scan's bindings through.
	REQUIRE(read("t WHERE b > 0").size() == 2);
	// A projection makes bindings of its own.
	REQUIRE(read("(SELECT a + 0 AS a FROM t) p").empty());
	// A table with the same columns, undeclared, is not in it.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE u AS SELECT * FROM t"));
	REQUIRE(read("u").empty());
}

TEST_CASE("S1 reads both interval bounds and exact equality", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	for (auto &constraint : {"SUM(x)>=1 AND SUM(x)<=2", "SUM(x)=2", "SUM(x)>=1"}) {
		auto plan = ReadPlan(con, "SELECT id,x FROM ("
		                          " FROM (VALUES (1, 9.0::DOUBLE), (2, -1.0::DOUBLE)) s(id,score)"
		                          " DECIDE x(BOOL) SUCH THAT " +
		                              string(constraint) + " MAXIMIZE SUM(score*x)) q");
		auto *decide = FindReadDecide(*plan);
		REQUIRE(decide != nullptr);
		auto condition = s1->CheckCondition(*decide, *con.context);
		REQUIRE(condition != nullptr);
		REQUIRE(s1->Summary() == "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi");
	}
}

TEST_CASE("S1 condition normalizes finite count bounds", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	for (auto &constraint : {"SUM(x)<=1.5", "SUM(x)<2", "SUM(x)=1.5"}) {
		auto plan = ReadPlan(con, "SELECT id, x FROM ("
		                          " FROM (VALUES (1, 9.0::DOUBLE)) s(id,score)"
		                          " DECIDE x(BOOL) SUCH THAT " +
		                              string(constraint) + " MAXIMIZE SUM(score*x)) q");
		auto *decide = FindReadDecide(*plan);
		REQUIRE(decide != nullptr);
		auto condition = s1->CheckCondition(*decide, *con.context);
		REQUIRE(condition != nullptr);
		REQUIRE(s1->Summary() == "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi");
	}
}

TEST_CASE("S1 condition rejects an unrepresentable bound and volatile coefficient", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='decide_optimizer'"));
	con.BeginTransaction();
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	REQUIRE(s1 != nullptr);
	auto applies = [&](const string &constraint, const string &objective) {
		auto plan = ReadPlan(con, "SELECT id, x FROM ("
		                          " FROM (VALUES (1, 9.0::DOUBLE)) s(id,score)"
		                          " DECIDE x(BOOL) SUCH THAT " +
		                              constraint + " MAXIMIZE " + objective + ") q");
		auto *decide = FindReadDecide(*plan);
		REQUIRE(decide != nullptr);
		return s1->CheckCondition(*decide, *con.context) != nullptr;
	};
	// The plain form of each case applies, so the case declines for the one thing it changes.
	REQUIRE(applies("SUM(x)<=1", "SUM(score*x)"));
	REQUIRE(!applies("SUM(x)<=9007199254740993", "SUM(score*x)"));
	REQUIRE(!applies("SUM(x)<=1", "SUM((score+txid_current())*x)"));
}

TEST_CASE("A reducer's by key is read into its part", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);

	QueryReading plain(con, "SELECT id, x FROM ("
	                        " FROM (VALUES (1, 9.0::DOUBLE, TRUE)) s(id,score,active)"
	                        " DECIDE x(BOOL) SUCH THAT when active: SUM(x)<=1"
	                        " MAXIMIZE SUM(score*x)"
	                        ") q");
	REQUIRE(plain.clauses_read);
	REQUIRE(plain.clauses[0].When() != nullptr);
	REQUIRE(plain.clauses[0].lhs[0].by.empty());

	QueryReading keyed(con, "SELECT id, x FROM ("
	                        " FROM (VALUES (1, 9.0::DOUBLE, 'A'::VARCHAR),"
	                        " (2, -1.0::DOUBLE, 'B'::VARCHAR)) s(id,score,dept)"
	                        " DECIDE x(BOOL) SUCH THAT SUM(x) by (dept)<=1"
	                        " MAXIMIZE SUM(score*x)"
	                        ") q");
	// The key lives on the reducer's part, where `by` is written: the column the rows group on, the name a message
	// gives it, and a NULL in it puts the row in no group (D5).
	REQUIRE(keyed.clauses_read);
	auto &key = keyed.clauses[0].lhs[0].by;
	REQUIRE(key.size() == 1);
	REQUIRE(key[0].name == "dept");
	REQUIRE(key[0].no_group);
	REQUIRE(key[0].grouping->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);

	// A key of two elements keeps both; a relation's columns compare NULL as a value.
	QueryReading both(con, "SELECT id, x FROM ("
	                       " FROM (VALUES (1, 9.0::DOUBLE, 'A'::VARCHAR, TRUE)) s(id,score,dept,flag)"
	                       " DECIDE x(BOOL) SUCH THAT SUM(x) by (dept, flag)<=1 AND SUM(x) by (s)<=1"
	                       " MAXIMIZE SUM(score*x)"
	                       ") q");
	REQUIRE(both.clauses_read);
	REQUIRE(both.clauses.size() == 2);
	auto &pair = both.clauses[0].lhs[0].by;
	REQUIRE(pair.size() == 2);
	REQUIRE((pair[0].name == "dept" && pair[1].name == "flag"));
	auto &relation = both.clauses[1].lhs[0].by;
	REQUIRE(!relation.empty());
	for (auto &element : relation) {
		REQUIRE(!element.no_group);
	}

	// An objective under a `when` reads as its parts; the `when` stays the operator's.
	QueryReading objective(con, "SELECT id, x FROM ("
	                            " FROM (VALUES (1, 9.0::DOUBLE, TRUE)) s(id,score,active)"
	                            " DECIDE x(BOOL) SUCH THAT SUM(x)<=1"
	                            " MAXIMIZE when active: SUM(score*x)"
	                            ") q");
	REQUIRE(objective.objective_read);
	REQUIRE(objective.objective.parts.size() == 1);
}

TEST_CASE("The readers give every decision's domain and a relation key's columns", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryReading query(con, ReadQuery("x(BOOL), n(INT), r(REAL), per (): s(INT), per t: e(BOOL)",
	                                  "x <= 1 AND n <= 3 AND r <= 2 AND s <= 4 AND e <= 1",
	                                  "MAXIMIZE SUM(w*x) + SUM(n) + SUM(r) + s + SUM(e)"));
	auto &decide = *query.decide;
	REQUIRE(DirectDomainOf(decide, 0) == DirectDomain::BOOL);
	REQUIRE(DirectDomainOf(decide, 1) == DirectDomain::INT);
	REQUIRE(DirectDomainOf(decide, 2) == DirectDomain::REAL);
	// `per t` is a relation element: it stands for every column t stores, and a NULL in one is a value of its own.
	REQUIRE(decide.bound.keyed_scopes.size() == 1);
	duckdb::vector<DirectKeyElement> relation_key;
	REQUIRE(DirectReadKey(decide, decide.bound.keyed_scopes[0].key, relation_key));
	REQUIRE(relation_key.size() == 4);
	for (auto &element : relation_key) {
		REQUIRE(!element.no_group);
		REQUIRE(element.grouping);
	}
	// The query-wide decision is a row-level part beside the reducers.
	REQUIRE(query.objective_read);
	REQUIRE(query.objective.parts.size() == 5);
	REQUIRE(!query.objective.parts[3].reduced);
}

TEST_CASE("A keyed decision's key is read as the plan reads it", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// A column element is in no class on a NULL (D5), and a key of two columns keeps both.
	QueryReading query(con, ReadQuery("per cap, flag: e(BOOL)", "e <= 1", "MAXIMIZE SUM(w*e)"));
	REQUIRE(query.decide->bound.keyed_scopes.size() == 1);
	duckdb::vector<DirectKeyElement> key;
	REQUIRE(DirectReadKey(*query.decide, query.decide->bound.keyed_scopes[0].key, key));
	REQUIRE(key.size() == 2);
	for (auto &element : key) {
		REQUIRE(element.no_group);
		REQUIRE(element.label);
	}
	// The binder keeps a key in one order, whichever order wrote it.
	REQUIRE((key[0].name == "cap") != (key[1].name == "cap"));
	REQUIRE((key[0].name == "flag") != (key[1].name == "flag"));
}

TEST_CASE("The clause reader reads comparisons and right-hand sides, and declines a membership", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryReading query(con, ReadQuery("x(INT), y(INT)",
	                                  "x + y <= 3 AND x <> 1 AND SUM(x) <= cap AND "
	                                  "SUM(y) <= (SELECT 5) AND SUM(x) BETWEEN 1 AND 2",
	                                  "MAXIMIZE SUM(w*x)"));
	REQUIRE(query.clauses_read);
	auto &constraints = query.clauses;
	REQUIRE(constraints.size() == 6);
	// One constraint per row, holding both decisions.
	REQUIRE(!constraints[0].Aggregate());
	REQUIRE(constraints[0].lhs.size() == 1);
	REQUIRE(!constraints[0].lhs[0].reduced);
	REQUIRE(constraints[0].lhs[0].terms.size() == 2);
	REQUIRE(constraints[1].Comparison() == ExpressionType::COMPARE_NOTEQUAL);
	REQUIRE(constraints[2].Aggregate());
	// BETWEEN is two written comparisons.
	REQUIRE(constraints[4].Comparison() == ExpressionType::COMPARE_GREATERTHANOREQUALTO);
	REQUIRE(constraints[5].Comparison() == ExpressionType::COMPARE_LESSTHANOREQUALTO);
	// No rule models a membership, so the reader declines the clauses that hold one.
	QueryReading membership(con, ReadQuery("x(INT)", "x <= 3 AND x IN (0, 2)", "MAXIMIZE SUM(w*x)"));
	REQUIRE(!membership.clauses_read);
}

TEST_CASE("The clause reader reads reducers, filters, qualifiers, scales and norms", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryReading query(con, ReadQuery("per t: x(INT), y(INT)",
	                                  "AVG(y) <= 2 AND MIN(y) >= 0 AND MAX(y) <= 3 AND SUM(when flag: y) <= 2 AND "
	                                  "SUM(per t: x) <= 4 AND 2 * SUM(y) <= 9 AND norm(y - cap, 1) <= 5",
	                                  "MINIMIZE SUM(w*y)"));
	REQUIRE(query.clauses_read);
	auto &constraints = query.clauses;
	REQUIRE(constraints.size() == 7);
	for (auto &constraint : constraints) {
		REQUIRE(constraint.lhs.size() == 1);
	}
	REQUIRE(constraints[0].lhs[0].reducer == DecideReducerKind::AVG);
	REQUIRE(constraints[1].lhs[0].reducer == DecideReducerKind::MIN);
	REQUIRE(constraints[2].lhs[0].reducer == DecideReducerKind::MAX);
	REQUIRE(constraints[3].lhs[0].filter != nullptr);
	REQUIRE(constraints[4].lhs[0].qualifier == 0);
	REQUIRE(constraints[5].lhs[0].scale != nullptr);
	// L1 reads as its definition: SUM of an absolute value.
	REQUIRE(constraints[6].lhs[0].reducer == DecideReducerKind::SUM);
	REQUIRE(constraints[6].lhs[0].terms.size() == 1);
	REQUIRE(constraints[6].lhs[0].terms[0].IsAbs());
}

TEST_CASE("The clause reader declines a norm marker of any order, never raising", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// `norm(y, 0)` needs indicator variables: no rule models it.
	QueryReading query(con, ReadQuery("y(INT)", "norm(y, 0) <= 2", "MINIMIZE SUM(w*y)"));
	REQUIRE(!query.clauses_read);
	// An order canonicalization would have expanded.
	REQUIRE_NOTHROW(query.ReorderNorms(DecideNorm::Order::L2));
	REQUIRE(!query.clauses_read);
}

TEST_CASE("The readers read products and squares, and decline a nested objective", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryReading products(
	    con, ReadQuery("x(INT), y(INT)", "SUM(x*y) <= 4 AND SUM(POWER(x - cap, 2)) <= 9", "MAXIMIZE SUM(w*x)"));
	REQUIRE(products.clauses_read);
	auto &constraints = products.clauses;
	REQUIRE(constraints.size() == 2);
	REQUIRE(constraints[0].lhs[0].terms[0].IsProduct());
	REQUIRE(constraints[1].lhs[0].terms[0].IsSquare());

	// A product inside an absolute value is not algebra any rule reads.
	QueryReading product_in_abs(con, ReadQuery("x(INT), y(INT)", "x <= 3", "MINIMIZE SUM(ABS(x*y - 1))"));
	REQUIRE(!product_in_abs.objective_read);

	// No rule models the helper decision a nested reducer becomes (D81), `outer(per k: inner(e) by (k))` included.
	QueryReading nested(con, ReadQuery("x(INT)", "x <= 3", "MINIMIZE SUM(per id: MAX(w*x) by (id))"));
	REQUIRE(!nested.objective_read);
	REQUIRE(nested.objective.parts.empty());
}

TEST_CASE("The clause reader reads an aggregate clause per class and declines a per-row clause per class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// `per K: C` is one constraint per class and variant (D70); an aggregate clause bounds the total of each group, and
	// its instances differ only in their bound, which the group's tightest bound covers: it reads as the clause.
	QueryReading aggregate(con, ReadQuery("x(BOOL)", "per flag: SUM(x) <= cap", "MAXIMIZE SUM(w*x)"));
	REQUIRE(aggregate.clauses_read);
	REQUIRE(aggregate.clauses.size() == 1);
	REQUIRE(aggregate.clauses[0].Aggregate());
	REQUIRE(aggregate.clauses[0].PerClass());
	REQUIRE(aggregate.clauses[0].classes.size() == 1);

	// A clause per row would hold in a class alone: no rule models it.
	QueryReading per_row(con, ReadQuery("x(BOOL)", "SUM(x) <= 2 AND per flag: x <= 1", "MAXIMIZE SUM(w*x)"));
	REQUIRE(!per_row.clauses_read);
	REQUIRE(per_row.clauses.size() == 2);

	// A reducer's own `per` over a body with variants reads the key's classes split by the body's values (D71), which
	// the operator numbers and no rule's plan reads.
	QueryReading variants(con, ReadQuery("x(BOOL)", "sum(per flag: x) <= 1", "MAXIMIZE SUM(w*x)"));
	REQUIRE(!variants.clauses_read);
	// An objective with a field is produced once (D72), which the operator checks and no rule's plan does.
	QueryReading once(con, ReadQuery("x(BOOL), per (): s(INT)", "x <= 1 AND s <= 3", "MAXIMIZE SUM(w*x) + cap * s"));
	REQUIRE(!once.objective_read);
}

TEST_CASE("The clause reader declines at the first clause it cannot read", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// A decision under ABS inside a square is not linear algebra the splitter can read.
	QueryReading query(con, ReadQuery("x(INT)", "x <= 3 AND SUM(POWER(ABS(x - cap), 2)) <= 9", "MAXIMIZE SUM(w*x)"));
	REQUIRE(!query.clauses_read);
	REQUIRE(query.clauses.size() == 2);
}

TEST_CASE("No rule reads a frame selector", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// A frame reads the row its position selects on a timeline, which the operator computes and no rule's plan reads.
	// One over data is no value of the source row, so every rule declines the bound that holds it.
	QueryReading data(con, ReadQuery("x(INT)", "x <= at(previous else 3: cap) over (id)", "MAXIMIZE SUM(x)"));
	for (auto &rule : RegisteredDirectRules()) {
		INFO(rule->Name());
		REQUIRE(rule->CheckCondition(*data.decide, *con.context) == nullptr);
	}
	// One over a decision is no term a rule reads. Its marker, an aggregate `first`, is never taken for a reducer.
	QueryReading reduced(con, ReadQuery("x(INT)", "x <= 3 AND SUM(at(first: x) over (id)) <= 4", "MAXIMIZE SUM(x)"));
	REQUIRE(!reduced.clauses_read);
	REQUIRE(reduced.clauses.size() == 2);
}

TEST_CASE("The clause reader says which bounds the solver folds into a variable's box", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);

	// One decision, constraints that each read as one clause in order; `expected` is the solver's folding of each.
	auto check = [&](const string &declaration, const string &constraints, duckdb::vector<bool> expected) {
		QueryReading query(con, ReadQuery(declaration, constraints, "MINIMIZE SUM(x)"));
		INFO(declaration << " | " << constraints);
		REQUIRE(query.clauses_read);
		REQUIRE(query.clauses.size() == expected.size());
		for (idx_t i = 0; i < expected.size(); i++) {
			REQUIRE(query.clauses[i].variable_bound == expected[i]);
		}
	};

	// A bare decision against a finite constant is folded into the box, whatever the side or the spelling of the
	// constant. BETWEEN arrives as two comparisons.
	check("x(REAL)", "x >= -3", {true});
	check("x(REAL)", "-3 <= x", {true});
	check("x(REAL)", "x <= 5", {true});
	check("x(REAL)", "x = 2", {true});
	check("x(REAL)", "x >= CAST(-3 AS DOUBLE)", {true});
	check("x(REAL)", "x >= -(3)", {true});
	check("x(REAL)", "x BETWEEN -3 AND 5", {true, true});
	check("x(INT)", "x < 3 AND x > -2", {true, true});
	check("x(BOOL)", "x <= 1", {true});

	// A bound is read by value (D41): a coefficient or offset on the decision and an arithmetic right side fold too,
	// into the box the classifier reads for both paths.
	for (auto affine : {"2*x >= -6", "x + 1 >= -2", "-x <= 3", "x >= 0 - 3"}) {
		check("x(REAL)", affine, {true});
		QueryReading query(con, ReadQuery("x(REAL)", affine, "MINIMIZE SUM(x)"));
		auto &bound = query.clauses[0];
		REQUIRE(bound.bound.sense == '>');
		REQUIRE(bound.bound.lower_value == -3.0);
	}

	// Anything else only intersects with the default box: a column, a `when`, and a comparison that is not a bound.
	check("x(REAL)", "x >= cap", {false});
	check("x(REAL)", "x >= w", {false});
	check("x(REAL)", "when flag: x >= -3", {false});
	check("x(INT)", "x <> 3", {false});
	check("x(INT)", "x < cap", {false});

	// A bound mixed with ones that are not folded keeps each clause's own answer.
	check("x(REAL)", "x >= -3 AND x <= cap AND when flag: x >= -1", {true, false, false});
}

TEST_CASE("The objective reader reads each spelling the solver accepts", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);

	// A square carries its coefficient (none is 1) and the linear form it squares: x - w is [x, w with sign -1].
	QueryReading square(con, ReadQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2))"));
	REQUIRE(square.objective_read);
	REQUIRE(square.objective.parts.size() == 1);
	auto &terms = square.objective.parts[0].terms;
	REQUIRE(terms.size() == 1);
	REQUIRE(terms[0].IsSquare());
	REQUIRE(terms[0].coefficient == nullptr);
	auto &form = terms[0].factors[0].form;
	REQUIRE(form.size() == 2);
	REQUIRE(form[0].IsLinear());
	REQUIRE(form[1].IsConstant());
	REQUIRE(form[1].sign == -1);

	// A constant factor inside the SUM is the square's coefficient; one outside stays on the part.
	QueryReading inside(con, ReadQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(3*POWER(x - w, 2))"));
	auto &inside_square = inside.objective.parts[0].terms[0];
	REQUIRE(inside_square.IsSquare());
	REQUIRE(inside_square.coefficient != nullptr);
	// A NULL `w` skips the row, so the coefficient is guarded as every other term's is
	// (BoundDecideNullGuardExpression); the guard reads 0 on that row and 3 on every other.
	REQUIRE(DecideIsGuarded(*inside_square.coefficient));
	REQUIRE(ExpressionExecutor::EvaluateScalar(*con.context, DecideUnguarded(*inside_square.coefficient))
	            .GetValue<double>() == 3.0);
	QueryReading outside(con, ReadQuery("x(REAL)", "x <= cap", "MINIMIZE 2*SUM(POWER(x - w, 2))"));
	REQUIRE(outside.objective.parts[0].scale != nullptr);
	REQUIRE(!outside.objective.parts[0].scale_divides);
	QueryReading divided(con, ReadQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2))/2"));
	REQUIRE(divided.objective.parts[0].scale_divides);
	QueryReading negated(con, ReadQuery("x(REAL)", "x <= cap", "MINIMIZE -SUM(POWER(x - w, 2))"));
	REQUIRE(negated.objective.parts[0].sign == -1);
	// The objective is a minimization: MAXIMIZE writes its flip as the sign of each part.
	QueryReading maximized(con, ReadQuery("x(REAL)", "x <= cap", "MAXIMIZE SUM(POWER(x - w, 2))"));
	REQUIRE(maximized.objective.parts[0].sign == -1);
	QueryReading maximized_negation(con, ReadQuery("x(REAL)", "x <= cap", "MAXIMIZE -SUM(POWER(x - w, 2))"));
	REQUIRE(maximized_negation.objective.parts[0].sign == 1);

	// x*x - 2*w*x is a square plus a negative linear term; a constant stays beside the square.
	QueryReading expanded(con, ReadQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(x*x - 2*w*x + 5)"));
	auto &expanded_terms = expanded.objective.parts[0].terms;
	REQUIRE(expanded_terms.size() == 3);
	REQUIRE(expanded_terms[0].IsSquare());
	REQUIRE(expanded_terms[1].IsLinear());
	REQUIRE(expanded_terms[1].sign == -1);
	REQUIRE(expanded_terms[2].IsConstant());

	// A row-valued coefficient inside the square is in its form; one outside it, written either way, is the square's
	// coefficient, which a rule's square reader takes as the square's weight (DirectReadSquareTerm).
	QueryReading inner_weight(con, ReadQuery("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(w*x - 3, 2))"));
	REQUIRE(inner_weight.objective_read);
	for (auto objective : {"MINIMIZE SUM(w*POWER(x - 1, 2))", "MINIMIZE SUM(w*x*x)"}) {
		QueryReading outer_weight(con, ReadQuery("x(REAL)", "x <= cap", objective));
		REQUIRE(outer_weight.objective_read);
		auto &weighted = outer_weight.objective.parts[0].terms[0];
		REQUIRE(weighted.IsSquare());
		REQUIRE(weighted.coefficient != nullptr);
		REQUIRE(!weighted.coefficient->IsFoldable());
	}

	// No objective is no part at all.
	QueryReading none(con, ReadQuery("x(REAL)", "x <= cap", ""));
	REQUIRE(none.objective_read);
	REQUIRE(none.objective.parts.empty());

	// A per-row bound with a coefficient keeps the coefficient in the term; the constant moves to the right side.
	QueryReading affine(con, ReadQuery("x(REAL)", "2*x + 1 <= cap", "MAXIMIZE SUM(x)"));
	REQUIRE(affine.clauses[0].lhs[0].terms.size() == 1);
	REQUIRE(affine.clauses[0].lhs[0].terms[0].IsLinear());
}

TEST_CASE("A1 accepts a separable box-constrained problem and declines anything else", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto rules = RegisteredDirectRules();
	auto *a1 = FindRuleByName(rules, "A1_SEPARABLE_BOX");
	REQUIRE(a1 != nullptr);
	string condition;

	// What it accepts.
	REQUIRE(RuleApplies(*a1, con, "x(REAL)", "x BETWEEN 0 AND cap", "MINIMIZE SUM(POWER(x - w, 2))", &condition));
	REQUIRE(condition == "min/max SUM(c*x), l <= x <= u, each row independent");
	REQUIRE(RuleApplies(*a1, con, "x(INT)", "x <= cap AND x >= 0", "MAXIMIZE SUM(w*x)"));
	REQUIRE(RuleApplies(*a1, con, "x(BOOL)", "x <= flag", "MAXIMIZE SUM(w*x)"));
	REQUIRE(RuleApplies(*a1, con, "x(REAL), y(INT)", "x <= cap AND y <= 5", "MINIMIZE SUM(x) + SUM(w*y)"));
	REQUIRE(RuleApplies(*a1, con, "x(REAL)", "x <= cap", ""));
	REQUIRE(RuleApplies(*a1, con, "x(REAL)", "2*x <= cap AND w*x >= -1 AND when flag: x >= -3", "MAXIMIZE SUM(x)"));
	REQUIRE(RuleApplies(*a1, con, "x(REAL)", "x <= cap", "MINIMIZE when flag: SUM(POWER(w*x - 3, 2))"));
	// A cast that can throw is read per row and raises as the solver does, so it is admitted.
	REQUIRE(RuleApplies(*a1, con, "x(REAL)", "x <= CAST(cap AS TINYINT)", "MAXIMIZE SUM(x)"));

	// What it declines.
	auto declines = [&](const string &declaration, const string &constraints, const string &objective) {
		INFO(declaration << " | " << constraints << " | " << objective);
		REQUIRE(!RuleApplies(*a1, con, declaration, constraints, objective));
	};
	declines("x(REAL)", "SUM(x) <= 5", "MAXIMIZE SUM(w*x)");
	declines("x(REAL), y(REAL)", "x + y <= 3", "MAXIMIZE SUM(x)");
	declines("x(REAL), y(REAL)", "x <= cap AND y <= cap", "MAXIMIZE SUM(x*y)");
	declines("x(INT)", "x <= cap AND x <> 2", "MAXIMIZE SUM(w*x)");
	declines("x(INT)", "x IN (1, 2, 3)", "MAXIMIZE SUM(w*x)");
	declines("x(REAL)", "x <= cap", "MAXIMIZE SUM(per id: SUM(w*x) by (id))");
	declines("x(REAL)", "x <= cap", "MAXIMIZE AVG(w*x)");
	declines("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2)) + SUM(POWER(x - cap, 2))");
	declines("x(REAL)", "x <= cap", "MINIMIZE SUM(w*POWER(x - 1, 2))");
	declines("x(REAL)", "x <= cap", "MAXIMIZE SUM(txid_current()*x)");
	declines("x(REAL)", "when txid_current() >= 0: x <= cap", "MAXIMIZE SUM(x)");
	declines("x(REAL)", "POWER(x, 2) <= 9", "MAXIMIZE SUM(x)");
	declines("x(REAL)", "ABS(x) <= 3", "MAXIMIZE SUM(x)");
	declines("x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(w*x - 1, 2)) + SUM(POWER(x - 1, 2))");
}

TEST_CASE("S1 and A1 never both apply to one query", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	auto *a1 = FindRuleByName(rules, "A1_SEPARABLE_BOX");
	REQUIRE(s1 != nullptr);
	REQUIRE(a1 != nullptr);
	struct Shape {
		string declaration, constraints, objective;
		bool s1, a1;
	};
	duckdb::vector<Shape> shapes = {
	    // S1 needs one count bound on SUM(x); A1 forbids any aggregate constraint.
	    {"x(BOOL)", "SUM(x) <= 1", "MAXIMIZE SUM(w*x)", true, false},
	    {"x(BOOL)", "SUM(x) >= 1 AND SUM(x) <= 2", "MAXIMIZE SUM(w*x)", true, false},
	    {"x(BOOL)", "SUM(x) <= 1 AND x <= 1", "MAXIMIZE SUM(w*x)", true, false},
	    {"x(BOOL)", "x <= flag", "MAXIMIZE SUM(w*x)", false, true},
	    {"x(BOOL)", "x <= 1", "MAXIMIZE SUM(w*x)", false, true},
	    {"x(BOOL)", "x = 1", "MINIMIZE SUM(w*x)", false, true},
	    {"x(BOOL)", "SUM(x) <= 1", "MAXIMIZE SUM(POWER(x - w, 2))", false, false},
	    {"x(INT)", "SUM(x) <= 5", "MAXIMIZE SUM(w*x)", false, false},
	    {"x(INT)", "x <= 5", "MAXIMIZE SUM(w*x)", false, true},
	    {"x(REAL)", "x <= cap", "MINIMIZE SUM(POWER(x - w, 2))", false, true},
	    {"x(BOOL), y(BOOL)", "SUM(x) <= 1 AND y <= 1", "MAXIMIZE SUM(w*x)", false, false},
	};
	for (auto &shape : shapes) {
		INFO(shape.declaration << " | " << shape.constraints << " | " << shape.objective);
		bool applies_s1 = RuleApplies(*s1, con, shape.declaration, shape.constraints, shape.objective);
		bool applies_a1 = RuleApplies(*a1, con, shape.declaration, shape.constraints, shape.objective);
		REQUIRE(!(applies_s1 && applies_a1));
		REQUIRE(applies_s1 == shape.s1);
		REQUIRE(applies_a1 == shape.a1);
	}
}

TEST_CASE("R1 never applies to a query S1 or A1 applies to", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto rules = RegisteredDirectRules();
	auto *s1 = FindS1Rule(rules);
	auto *a1 = FindRuleByName(rules, "A1_SEPARABLE_BOX");
	auto *r1 = FindRuleByName(rules, "R1_CONTINUOUS_KNAPSACK");
	REQUIRE(s1 != nullptr);
	REQUIRE(a1 != nullptr);
	REQUIRE(r1 != nullptr);
	struct Shape {
		string declaration, constraints, objective;
		bool s1, a1, r1;
	};
	duckdb::vector<Shape> shapes = {
	    // R1 needs a REAL decision under exactly one SUM budget; S1 needs a BOOL count bound; A1 forbids any aggregate.
	    {"x(REAL)", "SUM(w*x) <= 5", "MAXIMIZE SUM(w*x)", false, false, true},
	    {"x(REAL)", "x <= cap AND SUM(w*x) <= 5", "MINIMIZE SUM(w*x)", false, false, true},
	    {"x(REAL)", "SUM(x) <= 5", "MAXIMIZE SUM(w*x)", false, false, true},
	    // The budget's direction does not change whose it is.
	    {"x(REAL)", "SUM(w*x) >= 5", "MINIMIZE SUM(w*x)", false, false, true},
	    {"x(REAL)", "SUM(w*x) BETWEEN 1 AND 5", "MAXIMIZE SUM(w*x)", false, false, true},
	    {"x(BOOL)", "SUM(x) >= 1", "MAXIMIZE SUM(w*x)", true, false, false},
	    {"x(REAL)", "x <= cap", "MAXIMIZE SUM(w*x)", false, true, false},
	    {"x(REAL)", "x <= cap AND SUM(w*x) <= 5", "MINIMIZE SUM(POWER(x - w, 2))", false, false, false},
	    {"x(BOOL)", "SUM(x) <= 2", "MAXIMIZE SUM(w*x)", true, false, false},
	    {"x(BOOL)", "SUM(w*x) <= 2", "MAXIMIZE SUM(w*x)", false, false, false},
	    {"x(INT)", "SUM(x) <= 5", "MAXIMIZE SUM(w*x)", false, false, false},
	    {"x(INT)", "x <= 5", "MAXIMIZE SUM(w*x)", false, true, false},
	};
	for (auto &shape : shapes) {
		INFO(shape.declaration << " | " << shape.constraints << " | " << shape.objective);
		bool applies_s1 = RuleApplies(*s1, con, shape.declaration, shape.constraints, shape.objective);
		bool applies_a1 = RuleApplies(*a1, con, shape.declaration, shape.constraints, shape.objective);
		bool applies_r1 = RuleApplies(*r1, con, shape.declaration, shape.constraints, shape.objective);
		REQUIRE(applies_s1 + applies_a1 + applies_r1 <= 1);
		REQUIRE(applies_s1 == shape.s1);
		REQUIRE(applies_a1 == shape.a1);
		REQUIRE(applies_r1 == shape.r1);
	}
}

TEST_CASE("The clause reader declines a per-row term that is neither linear nor an ABS", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);

	// A per-row side with a decision under ABS inside a square has a term the splitter cannot read.
	QueryReading unknown_row(con, ReadQuery("x(INT)", "POWER(ABS(x - cap), 2) <= 9", "MAXIMIZE SUM(w*x)"));
	REQUIRE(!unknown_row.clauses_read);
	QueryReading known_row(con, ReadQuery("x(INT)", "ABS(x - cap) <= 9", "MAXIMIZE SUM(w*x)"));
	REQUIRE(known_row.clauses_read);
}

TEST_CASE("The source facts hold the two sides of a cross product", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE l AS SELECT * FROM (VALUES (1, 5), (2, -3), (3, 8)) t(id, pos)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE r AS SELECT * FROM (VALUES (1, 4), (2, 6)) t(id, pos)"));

	SECTION("A one-side filter sits inside its side, so the cross product is complete") {
		QueryReading query(
		    con, "SELECT sel FROM l CROSS JOIN r WHERE l.pos > 0 DECIDE sel(BOOL) SUCH THAT per l: "
		         "SUM(sel) BY (l) = 1 AND per r: SUM(sel) BY (r) = 1 MINIMIZE SUM(POWER(l.pos - r.pos, 2) * sel)");
		DirectSourceFacts sides(*query.decide);
		auto source_bindings = query.decide->SourceBindings();
		// l stores (id, pos) and r stores (id, pos): each side holds exactly its own columns, left first.
		REQUIRE(sides.left.size() == 2);
		REQUIRE(sides.right.size() == 2);
		REQUIRE(source_bindings.size() == 4);
		for (idx_t i = 0; i < 2; i++) {
			REQUIRE(sides.left[i] == source_bindings[i]);
			REQUIRE(sides.right[i] == source_bindings[2 + i]);
		}
	}

	SECTION("A filter across both sides makes a join, which leaves both empty") {
		QueryReading query(
		    con, "SELECT sel FROM l CROSS JOIN r WHERE l.pos < r.pos DECIDE sel(BOOL) SUCH THAT per l: "
		         "SUM(sel) BY (l) = 1 AND per r: SUM(sel) BY (r) = 1 MINIMIZE SUM(POWER(l.pos - r.pos, 2) * sel)");
		DirectSourceFacts sides(*query.decide);
		REQUIRE(sides.left.empty());
		REQUIRE(sides.right.empty());
	}

	SECTION("A single table has no sides") {
		QueryReading query(
		    con, "SELECT sel FROM l DECIDE sel(BOOL) SUCH THAT per l: SUM(sel) BY (l) = 1 MINIMIZE SUM(pos * sel)");
		DirectSourceFacts sides(*query.decide);
		REQUIRE(sides.left.empty());
		REQUIRE(sides.right.empty());
	}
}

TEST_CASE("The source facts hold the keys of the tables the source scans", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// The source columns of k, in order, are a, b, c, d, e, f. The keys are (a, b), the primary key; d; and (d, f).
	// (b, c) and (f, e) are UNIQUE over a nullable column, and e alone is UNIQUE and nullable: UNIQUE lets NULLs
	// repeat, so none of the three is a key.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE k (a INTEGER, b INTEGER, c INTEGER, d INTEGER NOT NULL UNIQUE, "
	                          "e INTEGER UNIQUE, f INTEGER NOT NULL, PRIMARY KEY (a, b), UNIQUE (b, c), UNIQUE (f, e), "
	                          "UNIQUE (d, f))"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE plain (a INTEGER NOT NULL, b INTEGER)"));
	auto columns = [](const duckdb::vector<ColumnBinding> &key) {
		duckdb::vector<idx_t> result;
		for (auto &binding : key) {
			result.push_back(binding.column_index);
		}
		std::sort(result.begin(), result.end());
		return result;
	};
	auto read = [&](const string &source) {
		QueryReading query(con, "SELECT * FROM (FROM " + source +
		                            " DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(x)) q");
		return DirectSourceFacts(*query.decide).keys;
	};

	SECTION("A key is a UNIQUE or PRIMARY KEY constraint whose columns are all NOT NULL") {
		auto keys = read("k");
		REQUIRE(keys.size() == 3);
		duckdb::vector<duckdb::vector<idx_t>> found;
		for (auto &key : keys) {
			REQUIRE(key[0].table_index == keys[0][0].table_index);
			found.push_back(columns(key));
		}
		std::sort(found.begin(), found.end());
		REQUIRE(found == duckdb::vector<duckdb::vector<idx_t>>({{0, 1}, {3}, {3, 5}}));
	}
	SECTION("A filter passes the scan's bindings through") {
		REQUIRE(read("k WHERE c > 0").size() == 3);
	}
	SECTION("A table without a UNIQUE constraint has no key") {
		REQUIRE(read("plain").empty());
	}
	SECTION("Each scanned table keeps its own keys") {
		auto keys = read("k k1 CROSS JOIN k k2");
		REQUIRE(keys.size() == 6);
		REQUIRE(keys[0][0].table_index != keys[3][0].table_index);
	}
	SECTION("A projection makes bindings of its own") {
		REQUIRE(read("(SELECT d + 0 AS d FROM k) p").empty());
	}
}

TEST_CASE("A clause keeps the shape and the direction bound reduction read", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	QueryReading query(con, ReadQuery("x(INT)", "sum(x) <= 5", "MAXIMIZE sum(x)"));
	auto &clause = query.decide->bound.clauses[0];
	REQUIRE(clause.shape == CanonicalConstraintClass::AGGREGATE);
	REQUIRE(clause.body->type == ExpressionType::COMPARE_LESSTHANOREQUALTO);
	SECTION("a clause no bound reduction recorded is not checked") {
		REQUIRE_NOTHROW(DecideOptimizer::VerifyBoundReads(*query.decide));
	}
	SECTION("the shape and the direction it read hold") {
		clause.bound_read_shape = CanonicalConstraintClass::AGGREGATE;
		clause.bound_read_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
		REQUIRE_NOTHROW(DecideOptimizer::VerifyBoundReads(*query.decide));
	}
	SECTION("a clause read in the value shape is in the aggregate shape") {
		clause.bound_read_shape = CanonicalConstraintClass::VALUE;
		clause.bound_read_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
		REQUIRE_THROWS_AS(DecideOptimizer::VerifyBoundReads(*query.decide), InternalException);
	}
	SECTION("a reduced bound's comparison has another direction") {
		clause.bound_read_shape = CanonicalConstraintClass::AGGREGATE;
		clause.bound_read_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		REQUIRE_THROWS_AS(DecideOptimizer::VerifyBoundReads(*query.decide), InternalException);
	}
	SECTION("an equality's bound is the tightest of both directions") {
		clause.bound_read_shape = CanonicalConstraintClass::AGGREGATE;
		clause.bound_read_type = ExpressionType::COMPARE_EQUAL;
		REQUIRE_NOTHROW(DecideOptimizer::VerifyBoundReads(*query.decide));
	}
}

TEST_CASE("A clause whose every reducer becomes a helper ends what bound reduction read", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
	// Both bounds differ between the rows of a group, so bound reduction reads each clause as one row per group.
	// The hard MAX becomes a helper (RewriteScopedReducers) and its clause a row per row; the sum keeps its row.
	auto plan = ReadPlan(con, ReadQuery("x(INT)", "x <= 5 AND max(x) >= cap AND sum(x) <= cap", "MINIMIZE sum(x)"));
	auto *decide = FindReadDecide(*plan);
	REQUIRE(decide != nullptr);
	REQUIRE_NOTHROW(DecideOptimizer::VerifyBoundReads(*decide));
	idx_t kept = 0;
	for (auto &clause : decide->bound.clauses) {
		if (clause.bound_read_shape == CanonicalConstraintClass::INVALID) {
			continue;
		}
		REQUIRE(clause.shape == CanonicalConstraintClass::AGGREGATE);
		kept++;
	}
	REQUIRE(kept == 1);
}
