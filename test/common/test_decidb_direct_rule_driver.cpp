#include "catch.hpp"
#include "test_helpers.hpp"

#include <functional>

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_factory.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

using namespace duckdb;

namespace {

struct StubCondition final : DirectApplicabilityCondition {};

//! How a stub plan reads its input before it releases rows.
enum class StubValidation : uint8_t {
	NONE,
	//! Checks the first source column for NULL in a pipelined projection.
	PIPELINED,
	//! The same check behind DirectValidationBreaker.
	BREAKER
};

//! A rule with fixed answers. Its plan assigns every row `value`.
class StubRule final : public DirectSolveRule {
public:
	StubRule(string name_p, bool applies_p, int32_t value_p, StubValidation validation_p = StubValidation::NONE)
	    : name(std::move(name_p)), applies(applies_p), value(value_p), validation(validation_p) {
	}

	bool wrong_slot_map = false;

	const char *Name() const override {
		return name.c_str();
	}
	duckdb::unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &, ClientContext &,
	                                                                string &reason) const override {
		if (!applies) {
			reason = "stub_not_applied: " + name;
			return nullptr;
		}
		return make_uniq<StubCondition>();
	}
	string Summary() const override {
		return "stub";
	}
	DirectSubstitute Apply(duckdb::unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &) const override {
		auto bindings = source->GetColumnBindings();
		auto types = source->types;
		auto valid = [&](const duckdb::vector<ColumnBinding> &input) {
			auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
			is_null->children.push_back(DirectColumn(types[0], input[0]));
			return make_uniq<BoundCaseExpression>(std::move(is_null), DirectErrorPredicate(optimizer, "stub: NULL"),
			                                      DirectConstantBool(true));
		};
		duckdb::vector<ColumnBinding> input = bindings;
		duckdb::unique_ptr<LogicalOperator> plan = std::move(source);
		duckdb::unique_ptr<Expression> check;
		if (validation == StubValidation::BREAKER) {
			auto breaker = DirectValidationBreaker(optimizer.binder, std::move(plan), valid(bindings));
			plan = std::move(breaker.plan);
			input = breaker.input_bindings;
			check = DirectColumn(LogicalType::BIGINT, breaker.breaker);
		} else if (validation == StubValidation::PIPELINED) {
			check = valid(bindings);
		}
		auto index = optimizer.binder.GenerateTableIndex();
		duckdb::vector<duckdb::unique_ptr<Expression>> expressions;
		for (idx_t i = 0; i < input.size(); i++) {
			expressions.push_back(DirectColumn(types[i], input[i]));
		}
		expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(value)));
		DirectSubstitute substitute;
		if (check) {
			substitute.validated_columns.push_back(expressions.size());
			expressions.push_back(std::move(check));
		}
		auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
		projection->children.push_back(std::move(plan));
		for (idx_t i = 0; i <= input.size(); i++) {
			substitute.slot_map.push_back(i);
		}
		if (wrong_slot_map) {
			substitute.slot_map.back() = 0;
		}
		substitute.prunable_decisions.push_back(1);
		substitute.child = std::move(projection);
		return substitute;
	}

private:
	string name;
	bool applies;
	int32_t value;
	StubValidation validation;
};

using RuleList = duckdb::vector<duckdb::unique_ptr<DirectSolveRule>>;

void InstallRules(Connection &con, std::function<RuleList()> make_rules) {
	auto state = make_shared_ptr<DirectRuleTestSeam>();
	state->make_rules = std::move(make_rules);
	// Insert keeps an existing entry, so a test that swaps rules removes the old set first.
	con.context->registered_state->Remove(DIRECT_RULE_TEST_SEAM_KEY);
	con.context->registered_state->Insert(DIRECT_RULE_TEST_SEAM_KEY, std::move(state));
}

//! Three rows; the solver picks only v = 2.
const char *QUERY = "SELECT v, x FROM (FROM (SELECT i AS v FROM range(3) t(i))"
                    " DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(v*x)) q ORDER BY v";

string ExplainText(Connection &con, const string &sql) {
	auto result = con.Query("EXPLAIN " + sql);
	REQUIRE_NO_FAIL(*result);
	return result->ToString();
}

} // namespace

TEST_CASE("Direct rule driver commits a stub rule that applies", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	InstallRules(con, [] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_ONE", true, 1));
		return rules;
	});
	auto result = con.Query(QUERY);
	REQUIRE(CHECK_COLUMN(result, 1, {1, 1, 1}));
	auto plan = ExplainText(con, QUERY);
	REQUIRE(plan.find("STUB_ONE") != string::npos);
}

TEST_CASE("Direct rule driver falls back to the solver and lists why each rule does not apply under require",
          "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	InstallRules(con, [] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_A", false, 1));
		rules.push_back(make_uniq<StubRule>("STUB_B", false, 1));
		return rules;
	});
	auto result = con.Query(QUERY);
	REQUIRE(CHECK_COLUMN(result, 1, {0, 0, 1}));
	auto plan = ExplainText(con, QUERY);
	REQUIRE(plan.find("Direct solve") == string::npos);

	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='require'"));
	auto required = con.Query(QUERY);
	REQUIRE(required->HasError());
	auto &message = required->GetError();
	REQUIRE(message.find("STUB_A: stub_not_applied: STUB_A") != string::npos);
	REQUIRE(message.find("STUB_B: stub_not_applied: STUB_B") != string::npos);
	REQUIRE(message.find("solver skipped=true") != string::npos);
}

TEST_CASE("Direct rule driver runs the rule that applies when another does not", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	SECTION("the missing rule is registered first") {
		InstallRules(con, [] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_NOT_APPLIED", false, 0));
			rules.push_back(make_uniq<StubRule>("STUB_APPLIES", true, 1));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(CHECK_COLUMN(result, 1, {1, 1, 1}));
		auto plan = ExplainText(con, QUERY);
		REQUIRE(plan.find("STUB_APPLIES") != string::npos);
		REQUIRE(plan.find("STUB_NOT_APPLIED") == string::npos);
	}
	SECTION("the missing rule is registered last") {
		InstallRules(con, [] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_APPLIES", true, 1));
			rules.push_back(make_uniq<StubRule>("STUB_NOT_APPLIED", false, 0));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(CHECK_COLUMN(result, 1, {1, 1, 1}));
		auto plan = ExplainText(con, QUERY);
		REQUIRE(plan.find("STUB_APPLIES") != string::npos);
		REQUIRE(plan.find("STUB_NOT_APPLIED") == string::npos);
	}
}

TEST_CASE("Direct rule driver treats two rules that apply to one query as an internal error", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	SECTION("two rules that apply are named") {
		InstallRules(con, [] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_FIRST", true, 0));
			rules.push_back(make_uniq<StubRule>("STUB_SECOND", true, 1));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(result->HasError());
		auto &message = result->GetError();
		REQUIRE(message.find("STUB_FIRST") != string::npos);
		REQUIRE(message.find("STUB_SECOND") != string::npos);
		REQUIRE(message.find("rules must be disjoint") != string::npos);
	}
	SECTION("a rule that does not apply is not named") {
		InstallRules(con, [] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_FIRST", true, 0));
			rules.push_back(make_uniq<StubRule>("STUB_NOT_APPLIED", false, 0));
			rules.push_back(make_uniq<StubRule>("STUB_SECOND", true, 1));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(result->HasError());
		auto &message = result->GetError();
		REQUIRE(message.find("STUB_FIRST") != string::npos);
		REQUIRE(message.find("STUB_SECOND") != string::npos);
		REQUIRE(message.find("STUB_NOT_APPLIED") == string::npos);
	}
	SECTION("every rule that applies is named when more than two apply") {
		InstallRules(con, [] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_FIRST", true, 0));
			rules.push_back(make_uniq<StubRule>("STUB_SECOND", true, 1));
			rules.push_back(make_uniq<StubRule>("STUB_THIRD", true, 1));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(result->HasError());
		auto &message = result->GetError();
		REQUIRE(message.find("STUB_FIRST") != string::npos);
		REQUIRE(message.find("STUB_SECOND") != string::npos);
		REQUIRE(message.find("STUB_THIRD") != string::npos);
	}
}

TEST_CASE("Direct rule driver treats a broken rule as an internal error", "[decidb]") {
	SECTION("slot map that does not match the DECIDE outputs") {
		DuckDB db(nullptr);
		Connection con(db);
		InstallRules(con, [] {
			RuleList rules;
			auto rule = make_uniq<StubRule>("STUB_WRONG_SLOT", true, 1);
			rule->wrong_slot_map = true;
			rules.push_back(std::move(rule));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(result->HasError());
		REQUIRE(result->GetError().find("slot map that does not match") != string::npos);
	}
}

TEST_CASE("Direct validation breaker reads every row before an outer LIMIT or COUNT returns", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// One thread, so a pipelined plan under LIMIT 1 deterministically stops after its first chunk.
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	auto install = [&](StubValidation validation) {
		InstallRules(con, [validation] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_VALIDATING", true, 1, validation));
			return rules;
		});
	};
	const string decide = "FROM (SELECT CASE WHEN i = 4999 THEN NULL ELSE i END AS v FROM range(5000) t(i))"
	                      " DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(v*x)";

	// Without the breaker the late NULL is never read: the check is only as eager as the parent.
	install(StubValidation::PIPELINED);
	auto pipelined = con.Query("SELECT v FROM (" + decide + ") q LIMIT 1");
	REQUIRE_NO_FAIL(*pipelined);

	install(StubValidation::BREAKER);
	for (auto &outer : {"SELECT v FROM (" + decide + ") q LIMIT 1", "SELECT COUNT(*) FROM (" + decide + ") q"}) {
		auto result = con.Query(outer);
		REQUIRE(result->HasError());
		REQUIRE(result->GetError().find("stub: NULL") != string::npos);
	}
	auto nothing = con.Query("SELECT v FROM (" + decide + ") q LIMIT 0");
	REQUIRE_NO_FAIL(*nothing);
	REQUIRE(nothing->RowCount() == 0);
}

TEST_CASE("With both real rules registered each query shape reaches the rule that applies to it", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE items AS SELECT * FROM (VALUES (1, 2.0, 5.0), (2, -1.0, 4.0), (3, 3.0, 6.0)) "
	              "v(id, score, cap)"));
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='require'"));
	auto plan_of = [&](const string &query) {
		auto result = con.Query("EXPLAIN " + query);
		REQUIRE_NO_FAIL(*result);
		return result->ToString();
	};
	// A count bound is S1's; per-row bounds on any decision type are A1's. Neither needs a line of engine code that
	// knows the other exists.
	auto s1_plan = plan_of("SELECT id, x FROM items DECIDE x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(score*x)");
	REQUIRE(s1_plan.find("S1_UNIFORM_MATROID") != string::npos);
	REQUIRE(s1_plan.find("A1_SEPARABLE_BOX") == string::npos);
	for (string declaration : {"x(REAL)", "x(INT)", "x(BOOL)"}) {
		auto a1_plan =
		    plan_of("SELECT id, x FROM items DECIDE " + declaration + " SUCH THAT x <= 1 MAXIMIZE SUM(score*x)");
		INFO(declaration);
		REQUIRE(a1_plan.find("A1_SEPARABLE_BOX") != string::npos);
		REQUIRE(a1_plan.find("S1_UNIFORM_MATROID") == string::npos);
	}
	// A query neither rule applies to lists both rules' reasons and leaves the solver to run.
	auto no_rule = con.Query("SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(x) <= 5 MAXIMIZE SUM(score*x)");
	REQUIRE(no_rule->HasError());
	REQUIRE(no_rule->GetError().find("S1_UNIFORM_MATROID: ") != string::npos);
	REQUIRE(no_rule->GetError().find("A1_SEPARABLE_BOX: ") != string::npos);
}

TEST_CASE("The late materialization happens only for a stored table and a column the rule does not use", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE sr AS SELECT i AS id, ((i * 37) % 100)::DOUBLE AS score, "
	                          "'payload-' || i::VARCHAR AS payload FROM range(100) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE sr_one AS SELECT 1 AS k"));
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='require'"));
	const string decide = " DECIDE x(BOOL) SUCH THAT SUM(x) <= 10 MAXIMIZE SUM(score * x)";
	auto scans = [&](const string &select, const string &relation) {
		auto plan = ExplainText(con, select + " FROM (FROM " + relation + decide + ") q");
		REQUIRE(plan.find("Direct solve rule") != string::npos);
		idx_t count = 0;
		for (auto position = plan.find("Table: sr"); position != string::npos;
		     position = plan.find("Table: sr", position + 1)) {
			// "sr_one" is a different table.
			if (plan.compare(position, 13, "Table: sr_one") != 0) {
				count++;
			}
		}
		return count;
	};
	// Nothing outside the rule is read, or only what the rule already needs: one scan.
	REQUIRE(scans("SELECT COUNT(*), SUM(x)", "sr") == 1);
	REQUIRE(scans("SELECT score, x", "sr") == 1);
	// A stored column the rule does not use is read from a second scan.
	REQUIRE(scans("SELECT payload, x", "sr") == 2);
	REQUIRE(scans("SELECT id, payload, x", "(SELECT * FROM sr WHERE id % 2 = 0) s") == 2);
	// A join, a volatile column, an aggregate and a computed column have no stable row key to read again by.
	REQUIRE(scans("SELECT payload, x", "(SELECT s.* FROM sr s JOIN sr_one o ON o.k = 1) s") == 1);
	REQUIRE(scans("SELECT payload, r, x", "(SELECT *, random() AS r FROM sr) s") == 1);
	REQUIRE(scans("SELECT payload, x",
	              "(SELECT id, MAX(score) AS score, MAX(payload) AS payload FROM sr GROUP BY id) s") == 1);
	REQUIRE(scans("SELECT payload, x", "(SELECT id, score, payload || '!' AS payload FROM sr) s") == 1);

	// The late materialization returns the rows the solver returns.
	const string query = "SELECT id, payload, x FROM (FROM sr" + decide + ") q ORDER BY id";
	auto late_materialization = con.Query(query);
	REQUIRE_NO_FAIL(*late_materialization);
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
	auto solver = con.Query(query);
	REQUIRE_NO_FAIL(*solver);
	REQUIRE(late_materialization->RowCount() == 100);
	REQUIRE(late_materialization->ToString() == solver->ToString());
}
