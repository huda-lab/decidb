#include "catch.hpp"
#include "test_helpers.hpp"

#include <functional>

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

using namespace duckdb;

namespace {

struct StubCondition final : DirectApplicabilityCondition {
	explicit StubCondition(idx_t source_reads_p) {
		source_reads = source_reads_p;
	}
};

//! How a stub plan reads its input before it releases rows.
enum class StubValidation : uint8_t {
	NONE,
	//! Checks the first source column for NULL in a pipelined projection.
	PIPELINED,
	//! The same check as a verdict the rule's second read waits for (DirectAllRowsValid, DirectSourceReader::Gate).
	VERDICT
};

//! A rule with fixed answers. Its plan assigns every row `value`.
class StubRule final : public DirectSolveRule {
public:
	StubRule(string name_p, bool applies_p, int32_t value_p, StubValidation validation_p = StubValidation::NONE)
	    : name(std::move(name_p)), applies(applies_p), value(value_p), validation(validation_p) {
	}

	bool wrong_slot_map = false;

	//! What a broken rule hands back to the adapter, one fault per variant.
	enum class Fault : uint8_t {
		NONE,
		NO_CHILD,
		NO_PRUNABLE_DECISION,
		SLOT_OUT_OF_RANGE,
		VALIDATED_COLUMN_OUT_OF_RANGE,
		//! The slot of the source row id, which the rule driver appends to a stored source, is out of range.
		ROW_ID_SLOT_OUT_OF_RANGE,
		//! The slot map covers the outputs but not the source row id, which the rule driver appended to a stored
		//! source.
		ROW_ID_NOT_PASSED,
		//! What the rule passes to a shared plan fragment is wrong, and the fragment raises while the rule builds.
		//! A value handed to DirectOverProjection reads a column of an outer query.
		OVER_PROJECTION_OUTER_COLUMN,
		//! A value handed to DirectOverProjection reads a column the source does not carry.
		OVER_PROJECTION_FOREIGN_COLUMN,
		//! The same, in the first operand of a sum whose second operand is fine.
		OVER_PROJECTION_FOREIGN_FIRST_OPERAND
	};
	Fault fault = Fault::NONE;

	const char *Name() const override {
		return name.c_str();
	}
	duckdb::unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &, ClientContext &) const override {
		if (!applies) {
			return nullptr;
		}
		// A verdict is one more read of the source.
		return make_uniq<StubCondition>(validation == StubValidation::VERDICT ? 2 : 1);
	}
	string Summary() const override {
		return "stub";
	}
	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &) const override {
		if (validation == StubValidation::VERDICT) {
			reader.Gate(DirectBuildOverRead(reader, reader.ReadSource(), [&](duckdb::unique_ptr<LogicalOperator> rows) {
				auto valid = Valid(optimizer, rows->types[0], rows->GetColumnBindings()[0]);
				return DirectAllRowsValid(optimizer, std::move(rows), std::move(valid));
			}));
		}
		return DirectBuildOverOneRead(
		    reader, [&](duckdb::unique_ptr<LogicalOperator> rows) { return Build(std::move(rows), optimizer); });
	}

private:
	//! True, or the row raises when the column is NULL.
	static duckdb::unique_ptr<Expression> Valid(Optimizer &optimizer, const LogicalType &type, ColumnBinding binding) {
		ExpressionBuilder build(optimizer.context);
		auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
		is_null->children.push_back(build.Out(build.Col(type, binding)));
		return make_uniq<BoundCaseExpression>(std::move(is_null), DirectErrorPredicate(optimizer, "stub: NULL"),
		                                      build.Out(build.Bool(true)));
	}

	DirectSubstitute Build(duckdb::unique_ptr<LogicalOperator> source, Optimizer &optimizer) const {
		ExpressionBuilder build(optimizer.context);
		auto bindings = source->GetColumnBindings();
		auto types = source->types;
		RaiseFromSharedFragment(bindings, types, optimizer);
		duckdb::vector<ColumnBinding> input = bindings;
		duckdb::unique_ptr<LogicalOperator> plan = std::move(source);
		duckdb::unique_ptr<Expression> check;
		if (validation == StubValidation::PIPELINED) {
			check = Valid(optimizer, types[0], bindings[0]);
		}
		auto index = optimizer.binder.GenerateTableIndex();
		duckdb::vector<duckdb::unique_ptr<Expression>> expressions;
		for (idx_t i = 0; i < input.size(); i++) {
			expressions.push_back(build.Out(build.Col(types[i], input[i])));
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
		switch (fault) {
		case Fault::NONE:
			break;
		case Fault::NO_CHILD:
			substitute.child.reset();
			break;
		case Fault::NO_PRUNABLE_DECISION:
			substitute.prunable_decisions.clear();
			break;
		case Fault::SLOT_OUT_OF_RANGE:
			substitute.slot_map.back() = 1000;
			break;
		case Fault::VALIDATED_COLUMN_OUT_OF_RANGE:
			substitute.validated_columns.push_back(1000);
			break;
		case Fault::ROW_ID_SLOT_OUT_OF_RANGE:
			// The rowid is the last source column, just before the decision.
			substitute.slot_map[input.size() - 1] = 1000;
			break;
		case Fault::ROW_ID_NOT_PASSED:
			substitute.slot_map.erase(substitute.slot_map.begin() + NumericCast<int64_t>(input.size() - 1));
			break;
		}
		return substitute;
	}

	//! Hands a shared plan fragment what the fault says is wrong with it. The fragment raises, so no plan results.
	void RaiseFromSharedFragment(const duckdb::vector<ColumnBinding> &bindings,
	                             const duckdb::vector<LogicalType> &types, Optimizer &optimizer) const {
		ExpressionBuilder build(optimizer.context);
		auto column = [&](ColumnBinding binding, idx_t depth = 0) {
			return duckdb::unique_ptr<Expression>(make_uniq<BoundColumnRefExpression>(types[0], binding, depth));
		};
		const ColumnBinding foreign(9999, 0);
		switch (fault) {
		case Fault::OVER_PROJECTION_OUTER_COLUMN:
			DirectOverProjection(*column(bindings[0], 1), bindings, 0);
			break;
		case Fault::OVER_PROJECTION_FOREIGN_COLUMN:
			DirectOverProjection(*column(foreign), bindings, 0);
			break;
		case Fault::OVER_PROJECTION_FOREIGN_FIRST_OPERAND: {
			auto sum = build.Add(build.Col(types[0], foreign), build.Col(types[0], bindings[0]));
			DirectOverProjection(*build.Out(sum), bindings, 0);
			break;
		}
		default:
			break;
		}
	}

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

TEST_CASE("Direct rule driver falls back to the solver and raises one fixed line under require",
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
	REQUIRE(message.find("decide_direct_solve=require") != string::npos);
	REQUIRE(message.find("STUB_A") == string::npos);
	REQUIRE(message.find("STUB_B") == string::npos);
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

TEST_CASE("Direct rule driver offers a rule no IR the front end could not read", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// The stub applies to whatever it is offered, so a plan with its name proves the IR was offered to it.
	InstallRules(con, [] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_OFFERED", true, 1));
		return rules;
	});
	auto query_of = [](const string &clauses) {
		return "SELECT v, x FROM (FROM (SELECT i AS v FROM range(3) t(i)) DECIDE x(BOOL) SUCH THAT " + clauses +
		       ") q ORDER BY v";
	};
	// The control: a readable IR is offered to the rule.
	REQUIRE(ExplainText(con, query_of("SUM(x) <= 1 MAXIMIZE SUM(v*x)")).find("STUB_OFFERED") != string::npos);

	// Each query leaves one part of the IR unread. The first is a single constraint the front end cannot model, the
	// second an objective it cannot model, the third a non-finite objective offset.
	for (string clauses : {"SUM(x) <= 1 AND norm(x, 0) <= 1 MAXIMIZE SUM(v*x)", "SUM(x) <= 1 MAXIMIZE norm(x, 0, 100)",
	                       "SUM(x) <= 1 MAXIMIZE SUM(v*x) + 'inf'::DOUBLE"}) {
		INFO(clauses);
		REQUIRE(ExplainText(con, query_of(clauses)).find("Direct solve") == string::npos);
		REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='require'"));
		auto required = con.Query(query_of(clauses));
		REQUIRE(required->HasError());
		REQUIRE(required->GetError().find("direct solve did not apply") != string::npos);
		REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='auto'"));
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

TEST_CASE("Direct rule driver rejects each way a rule can hand the adapter a substitute that does not fit",
          "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// A stored table, so the rule driver appends its row id and the late materialization is enabled.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE stub_src AS SELECT i AS v FROM range(3) t(i)"));
	const string stored_query = "SELECT v, x FROM (FROM stub_src DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE "
	                            "SUM(v*x)) q ORDER BY v";
	struct Case {
		const char *name;
		StubRule::Fault fault;
		bool stored;
		const char *error;
	};
	const Case cases[] = {
	    {"no child", StubRule::Fault::NO_CHILD, false, "incomplete result substitute"},
	    {"no prunable flag for the decision", StubRule::Fault::NO_PRUNABLE_DECISION, false,
	     "incomplete result substitute"},
	    {"a slot beyond the child", StubRule::Fault::SLOT_OUT_OF_RANGE, false, "slot map that does not match"},
	    {"a validated column beyond the child", StubRule::Fault::VALIDATED_COLUMN_OUT_OF_RANGE, false,
	     "lost a required validated column"},
	    {"a row id slot beyond the child", StubRule::Fault::ROW_ID_SLOT_OUT_OF_RANGE, true, "lost the source row id"},
	    {"a slot map without the source row id", StubRule::Fault::ROW_ID_NOT_PASSED, true,
	     "did not pass every column of its read through"}};
	for (auto &test_case : cases) {
		INFO(test_case.name);
		auto fault = test_case.fault;
		InstallRules(con, [fault] {
			RuleList rules;
			auto rule = make_uniq<StubRule>("STUB_BROKEN", true, 1);
			rule->fault = fault;
			rules.push_back(std::move(rule));
			return rules;
		});
		auto result = con.Query(test_case.stored ? stored_query : QUERY);
		REQUIRE(result->HasError());
		REQUIRE(result->GetError().find(test_case.error) != string::npos);
	}
	// The control: the same stub on a stored table, with no fault, answers.
	InstallRules(con, [] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_STORED", true, 1));
		return rules;
	});
	auto control = con.Query(stored_query);
	REQUIRE(CHECK_COLUMN(control, 1, {1, 1, 1}));
}

TEST_CASE("Direct rule driver rejects each way a rule can hand a shared plan fragment an input that does not fit",
          "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	struct Case {
		const char *name;
		StubRule::Fault fault;
		const char *error;
	};
	const Case cases[] = {{"a value that reads an outer query", StubRule::Fault::OVER_PROJECTION_OUTER_COLUMN,
	                       "could not re-point a value"},
	                      {"a value that reads a column the source lacks",
	                       StubRule::Fault::OVER_PROJECTION_FOREIGN_COLUMN, "could not re-point a value"},
	                      {"a sum whose first operand reads a column the source lacks",
	                       StubRule::Fault::OVER_PROJECTION_FOREIGN_FIRST_OPERAND, "could not re-point a value"}};
	for (auto &test_case : cases) {
		INFO(test_case.name);
		auto fault = test_case.fault;
		InstallRules(con, [fault] {
			RuleList rules;
			auto rule = make_uniq<StubRule>("STUB_BROKEN", true, 1);
			rule->fault = fault;
			rules.push_back(std::move(rule));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(result->HasError());
		REQUIRE(result->GetError().find(test_case.error) != string::npos);
	}
	// The control: the same stub with no fault answers.
	InstallRules(con, [] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_FINE", true, 1));
		return rules;
	});
	auto control = con.Query(QUERY);
	REQUIRE(CHECK_COLUMN(control, 1, {1, 1, 1}));
}

TEST_CASE("A rule's verdict reads every row before an outer LIMIT or COUNT returns", "[decidb]") {
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

	// Without the verdict the late NULL is never read: the check is only as eager as the parent.
	install(StubValidation::PIPELINED);
	auto pipelined = con.Query("SELECT v FROM (" + decide + ") q LIMIT 1");
	REQUIRE_NO_FAIL(*pipelined);

	install(StubValidation::VERDICT);
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
	// A query neither rule applies to raises the one fixed line, which names no rule.
	auto no_rule = con.Query("SELECT id, x FROM items DECIDE x(INT) SUCH THAT SUM(x) <= 5 MAXIMIZE SUM(score*x)");
	REQUIRE(no_rule->HasError());
	REQUIRE(no_rule->GetError().find("decide_direct_solve=require") != string::npos);
	REQUIRE(no_rule->GetError().find("S1_UNIFORM_MATROID") == string::npos);
	REQUIRE(no_rule->GetError().find("A1_SEPARABLE_BOX") == string::npos);
}

TEST_CASE("With all three real rules registered each query shape reaches the rule that applies to it", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE items AS SELECT * FROM (VALUES (1, 2.0, 5.0, 1.5), (2, -1.0, 4.0, 2.0), "
	                          "(3, 3.0, 6.0, 1.0)) v(id, score, cap, w)"));
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='require'"));
	auto rule_of = [&](const string &query) {
		auto result = con.Query("EXPLAIN " + query);
		REQUIRE_NO_FAIL(*result);
		auto plan = result->ToString();
		string rule;
		for (string name : {"S1_UNIFORM_MATROID", "A1_SEPARABLE_BOX", "R1_CONTINUOUS_KNAPSACK"}) {
			if (plan.find(name) != string::npos) {
				REQUIRE(rule.empty());
				rule = name;
			}
		}
		return rule;
	};
	// One REAL decision under a SUM budget is R1's, with or without a bound beside it; the same decision without the
	// budget is A1's; a BOOL count bound is S1's. The three ask for disjoint IR, so none needs to know the others.
	REQUIRE(rule_of("SELECT id, x FROM items DECIDE x(REAL) SUCH THAT x <= cap AND SUM(w*x) <= 7 "
	                "MAXIMIZE SUM(score*x)") == "R1_CONTINUOUS_KNAPSACK");
	REQUIRE(rule_of("SELECT id, x FROM items DECIDE x(REAL) SUCH THAT SUM(w*x) <= 7 MINIMIZE SUM(score*x)") ==
	        "R1_CONTINUOUS_KNAPSACK");
	REQUIRE(rule_of("SELECT id, x FROM items DECIDE x(REAL) SUCH THAT x <= cap MAXIMIZE SUM(score*x)") ==
	        "A1_SEPARABLE_BOX");
	REQUIRE(rule_of("SELECT id, x FROM items DECIDE x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE SUM(score*x)") ==
	        "S1_UNIFORM_MATROID");
	// A budget read from the data is R1's too: a column, and an aggregate of the source (a window column by then).
	REQUIRE(rule_of("SELECT id, x FROM items DECIDE x(REAL) SUCH THAT x <= cap AND SUM(w*x) <= cap "
	                "MAXIMIZE SUM(score*x)") == "R1_CONTINUOUS_KNAPSACK");
	REQUIRE(rule_of("SELECT id, x FROM items DECIDE x(REAL) SUCH THAT x <= cap AND SUM(w*x) <= 0.5 * SUM(cap) "
	                "MAXIMIZE SUM(score*x)") == "R1_CONTINUOUS_KNAPSACK");
	// A weighted budget on a BOOL or INT decision is nobody's: it needs a knapsack that is not continuous.
	for (string declaration : {"x(BOOL)", "x(INT)"}) {
		auto result = con.Query("SELECT id, x FROM items DECIDE " + declaration +
		                        " SUCH THAT SUM(w*x) <= 7 MAXIMIZE SUM(score*x)");
		INFO(declaration);
		REQUIRE(result->HasError());
		REQUIRE(result->GetError().find("decide_direct_solve=require") != string::npos);
	}
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

// The determination checks are made under a rule's plan, with the message the solver raises. Each case reaches a branch
// of DirectAddRowChecks that removing it would break: the least failing group by key value (not the first row), the
// least and greatest value (not the first two), NULL as a value after every other, a NULL key that is in no class
// (a column key) and one that is a class of its own (a relation key), the empty key, and the decision determination.
TEST_CASE("Direct rule driver raises a determination check as the solver does", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	InstallRules(con, [] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_ONE", true, 1));
		return rules;
	});
	struct Case {
		const char *source;
		const char *decide;
		const char *clause;
		const char *objective;
		//! The message both paths raise, or null when the check passes.
		const char *expected;
	};
	// The greater failing group and the greater value come first in the rows.
	const Case cases[] = {
	    // The least failing group, and its least and greatest value rather than the first two.
	    {"(VALUES (1,'b',20.0),(2,'b',10.0),(3,'a',7.0),(4,'a',5.0),(5,'a',6.0),(6,'c',1.0)) t(id,g,cap)", "x(BOOL)",
	     "SUM(per g: cap * x) <= 100", "SUM(x)", "g = 'a' has more than one value: 5.0, 7.0."},
	    // The same rows in the opposite order.
	    {"(VALUES (6,'c',1.0),(5,'a',6.0),(4,'a',5.0),(3,'a',7.0),(2,'b',10.0),(1,'b',20.0)) t(id,g,cap)", "x(BOOL)",
	     "SUM(per g: cap * x) <= 100", "SUM(x)", "g = 'a' has more than one value: 5.0, 7.0."},
	    // NULL is a value of its own, after every other.
	    {"(VALUES (1,'b',CAST(NULL AS DOUBLE)),(2,'b',20.0),(3,'a',5.0),(4,'a',5.0)) t(id,g,cap)", "x(BOOL)",
	     "SUM(per g: cap * x) <= 100", "SUM(x)", "g = 'b' has more than one value: 20.0, NULL."},
	    // A NULL in a column key is in no class.
	    {"(VALUES (1,CAST(NULL AS VARCHAR),1.0),(2,CAST(NULL AS VARCHAR),2.0),(3,'a',5.0)) t(id,g,cap)", "x(BOOL)",
	     "SUM(per g: cap * x) <= 100", "SUM(x)", nullptr},
	    // A NULL in a relation key is a class of its own.
	    {"(SELECT * FROM (VALUES (CAST(NULL AS INT)),(CAST(NULL AS INT))) t(g)) t, (VALUES (1),(2)) u(v)", "x(BOOL)",
	     "SUM(per t: v * x) <= 100", "SUM(x)", "t.g = NULL has more than one value: 1, 2."},
	    // The empty key is one class, which names no group.
	    {"(VALUES (1, 4.0, 3.0),(2, 4.0, 2.0)) t(id,a,b)", "per (): s(BOOL)", "s <= 1", "b * s",
	     "The kept rows have more than one value: 2.0, 3.0."},
	    // A row-scoped decision is held by one row of a class.
	    {"(VALUES (1,'b'),(2,'b'),(3,'a'),(4,'a')) t(id,g)", "x(BOOL)", "SUM(per g: x) <= 1", "SUM(x)",
	     "g = 'a' covers more than one x."},
	    {"(VALUES (1,'b'),(2,'a'),(3,'c')) t(id,g)", "x(BOOL)", "SUM(per g: x) <= 1", "SUM(x)", nullptr},
	    {"(VALUES (1,'b'),(2,'b')) t(id,g)", "x(BOOL)", "SUM(per (): x) <= 1", "SUM(x)",
	     "The kept rows cover more than one x."},
	};
	for (auto &test : cases) {
		string query = string("SELECT * FROM ") + test.source + " DECIDE " + test.decide + " SUCH THAT " + test.clause +
		               " MAXIMIZE " + test.objective;
		INFO(query);
		REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
		auto solver = con.Query(query);
		REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='require'"));
		auto direct = con.Query(query);
		if (!test.expected) {
			REQUIRE(!direct->HasError());
			REQUIRE(!solver->HasError());
			continue;
		}
		REQUIRE(solver->HasError());
		REQUIRE(direct->HasError());
		REQUIRE(solver->GetError().find(test.expected) != string::npos);
		REQUIRE(direct->GetError() == solver->GetError());
	}
}

// A source that could be re-scanned is copied once only when the plan reads it often enough for the copy to pay (R1's
// four reads do, two do not) and the copy fits in a share of the memory; with no rows, or no estimate of them, it is
// re-scanned.
TEST_CASE("The source reader copies a source only when reads and memory favor the copy", "[decidb]") {
	const idx_t memory = 8000;
	// Four reads, 100 rows of 10 bytes (1000 bytes, one eighth of the memory): the copy.
	REQUIRE(DirectSourceCopyIsCheaper(4, 100, 10, memory));
	// Two reads pay less than a copy costs.
	REQUIRE(!DirectSourceCopyIsCheaper(2, 100, 10, memory));
	REQUIRE(!DirectSourceCopyIsCheaper(1, 100, 10, memory));
	// A copy past a share of the memory.
	REQUIRE(!DirectSourceCopyIsCheaper(4, 101, 10, memory));
	// No rows, or no estimate.
	REQUIRE(!DirectSourceCopyIsCheaper(4, 0, 10, memory));
}

namespace {

//! A rule that applies to nothing and records how the bound of the query's one aggregate clause reaches it: `columns`
//! when the rule reads it as one value per group, else the functions of the aggregates of its group bound.
class BoundShapeRule final : public DirectSolveRule {
public:
	explicit BoundShapeRule(std::shared_ptr<string> shape_p) : shape(std::move(shape_p)) {
	}
	const char *Name() const override {
		return "STUB_BOUND_SHAPE";
	}
	duckdb::unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir,
	                                                                ClientContext &) const override {
		auto &group = ir.constraints[0].group_bound;
		if (!group) {
			*shape = "columns";
			return nullptr;
		}
		// The limits the bound gives read the placeholders of its aggregates, not columns of the source.
		REQUIRE(!DirectReferencesOnlySource(group->upper ? *group->upper : *group->lower, ir.source_bindings));
		shape->clear();
		for (auto &aggregate : group->aggregates) {
			*shape += (shape->empty() ? "" : ",") + aggregate.function->Cast<BoundAggregateExpression>().function.name;
			*shape += group->inputs[aggregate.input].filter ? "(when)" : "";
		}
		return nullptr;
	}
	string Summary() const override {
		return "stub";
	}
	DirectSubstitute Apply(DirectSourceReader &, Optimizer &, const DirectApplicabilityCondition &) const override {
		throw InternalException("never applies");
	}

private:
	std::shared_ptr<string> shape;
};

} // namespace

// The bound-side rewrite leaves a bound to the rule as the aggregates of its group: the reducers it is a function of,
// when each reads exactly the clause's group and the bound reads no data but through them, else the MIN and/or MAX of
// its raw value. A bound with one value per group already is a column, as is a bound whose sum has a `when` of its own
// (the windows are built). Each case below leaves one of those conditions out.
TEST_CASE("The bound-side rewrite hands a rule the aggregates of a bound's group", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto shape = std::make_shared<string>();
	InstallRules(con, [&] {
		RuleList rules;
		rules.push_back(make_uniq<BoundShapeRule>(shape));
		return rules;
	});
	struct Case {
		const char *clause;
		const char *expected;
		//! The source of the query: three rows, or two rows of a relation joined to the nation each belongs to.
		bool joined = false;
	};
	const Case cases[] = {
	    // Every reducer reads the clause's group: the key is the clause's, or neither has one.
	    {"SUM(x) by (g) <= SUM(cap) by (g)", "sum"},
	    {"SUM(x) by (g) <= SUM(cap) by (g) - COUNT(*) by (g) / 2", "sum,count_star"},
	    {"SUM(x) <= MIN(when flag: cap)", "min(when)"},
	    {"when flag: SUM(x) by (g) >= AVG(cap) by (g)", "avg"},
	    // A reducer over other rows than the group's: the windows give the bound per row and the rule takes its
	    // tightest
	    // value, or the bound is one value in the group already.
	    {"SUM(x) <= SUM(cap) by (g)", "min"},
	    {"SUM(x) by (g) <= SUM(cap)", "columns"},
	    // The bound reads a column beside its reducer.
	    {"SUM(x) by (g) <= SUM(cap) by (g) + cap", "min"},
	    // `<>` excludes each value its bound takes.
	    {"SUM(x) by (g) <> SUM(cap) by (g)", "columns"},
	    // The sum has a `when` of its own, so the rule sums fewer rows than the group the reducer reads.
	    {"SUM(when flag: x) by (g) <= SUM(cap) by (g)", "columns"},
	    // No reducer: a column, read at its tightest in each side the comparison reads.
	    {"SUM(x) by (g) <= cap", "min"},
	    {"SUM(x) by (g) >= cap", "max"},
	    {"SUM(x) by (g) = cap", "min,max"},
	    // A reducer that counts each nation once is no plain aggregate over the group.
	    {"SUM(x) <= SUM(n: n.ncap)", "columns", true},
	    {"SUM(x) <= SUM(n.ncap)", "sum", true},
	};
	for (auto &test : cases) {
		string source =
		    "(VALUES (1,'a',3.0::DOUBLE,TRUE,9.0),(2,'a',3.0::DOUBLE,FALSE,8.0),(3,'b',1.0::DOUBLE,TRUE,7.0)) "
		    "t(id,g,cap,flag,score)";
		if (test.joined) {
			source = "(VALUES (1,1,9.0),(2,1,8.0),(3,2,7.0)) c(id,k,score) JOIN (VALUES (1,2.0),(2,1.0)) n(k,ncap) "
			         "ON c.k = n.k";
		}
		string query = "SELECT id, x FROM (FROM " + source + " DECIDE x(BOOL) SUCH THAT " + test.clause +
		               " MAXIMIZE SUM(score*x)) q";
		INFO(query);
		shape->clear();
		ExplainText(con, query);
		REQUIRE(*shape == test.expected);
	}
}
