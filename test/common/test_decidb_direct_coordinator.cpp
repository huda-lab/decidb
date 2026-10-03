#include "catch.hpp"
#include "test_helpers.hpp"

#include <functional>
#include <limits>

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

using namespace duckdb;

namespace {

struct StubMatch final : DirectRuleMatch {};
struct StubProof final : DirectRuleProof {};

//! How a stub plan reads its input before it releases rows.
enum class StubValidation : uint8_t {
	NONE,
	//! Checks the first source column for NULL in a projection that streams.
	STREAMING,
	//! The same check behind DirectValidationBarrier.
	BARRIER
};

//! A rule with fixed answers. Its plan assigns every row `value`.
class StubRule final : public DirectSolveRule {
public:
	StubRule(string name_p, bool proves_p, double cost_p, int32_t value_p,
	         StubValidation validation_p = StubValidation::NONE, idx_t *seen_rows_p = nullptr)
	    : name(std::move(name_p)), proves(proves_p), cost(cost_p), value(value_p), validation(validation_p),
	      seen_rows(seen_rows_p) {
	}

	bool wrong_output_slot = false;

	const char *Name() const override {
		return name.c_str();
	}
	duckdb::unique_ptr<DirectRuleMatch> Match(const DirectProblemFacts &, string &) const override {
		return make_uniq<StubMatch>();
	}
	duckdb::unique_ptr<DirectRuleProof> Prove(const DirectProblemFacts &, const DirectRuleMatch &, ClientContext &,
	                                  string &reason) const override {
		if (!proves) {
			reason = "stub_miss: " + name;
			return nullptr;
		}
		return make_uniq<StubProof>();
	}
	double Cost(const DirectRuleProof &, const DirectCostContext &context) const override {
		if (seen_rows) {
			*seen_rows = context.estimated_source_rows;
		}
		return cost;
	}
	void Explain(const DirectRuleProof &, DirectSolveDecisionRecord &record) const override {
		record.proof = "stub";
		record.guards = "none";
	}
	DirectRelationalProposal Rewrite(duckdb::unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                                 const DirectRuleProof &) const override {
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
		if (validation == StubValidation::BARRIER) {
			auto barrier = DirectValidationBarrier(optimizer.binder, std::move(plan), valid(bindings));
			plan = std::move(barrier.plan);
			input = barrier.input_bindings;
			check = DirectColumn(LogicalType::BIGINT, barrier.barrier);
		} else if (validation == StubValidation::STREAMING) {
			check = valid(bindings);
		}
		auto index = optimizer.binder.GenerateTableIndex();
		duckdb::vector<duckdb::unique_ptr<Expression>> expressions;
		for (idx_t i = 0; i < input.size(); i++) {
			expressions.push_back(DirectColumn(types[i], input[i]));
		}
		expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(value)));
		DirectRelationalProposal proposal;
		if (check) {
			proposal.validation_slots.push_back(expressions.size());
			expressions.push_back(std::move(check));
		}
		auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
		projection->children.push_back(std::move(plan));
		for (idx_t i = 0; i <= input.size(); i++) {
			proposal.output_slots.push_back(i);
		}
		if (wrong_output_slot) {
			proposal.output_slots.back() = 0;
		}
		proposal.prunable_decisions.push_back(1);
		proposal.child = std::move(projection);
		return proposal;
	}

private:
	string name;
	bool proves;
	double cost;
	int32_t value;
	StubValidation validation;
	idx_t *seen_rows;
};

using RuleList = duckdb::vector<duckdb::unique_ptr<DirectSolveRule>>;

void InstallRules(Connection &con, std::function<RuleList()> make_rules) {
	auto state = make_shared_ptr<DirectRuleOverride>();
	state->make_rules = std::move(make_rules);
	// Insert keeps an existing entry, so a test that swaps rules removes the old set first.
	con.context->registered_state->Remove(DIRECT_RULE_OVERRIDE_KEY);
	con.context->registered_state->Insert(DIRECT_RULE_OVERRIDE_KEY, std::move(state));
}

//! Three rows; the solver picks only v = 2.
const char *QUERY = "SELECT v, x FROM (FROM (SELECT i AS v FROM range(3) t(i))"
                    " DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(v*x)) q ORDER BY v";

string Explain(Connection &con, const string &sql) {
	auto result = con.Query("EXPLAIN " + sql);
	REQUIRE_NO_FAIL(*result);
	return result->ToString();
}

} // namespace

TEST_CASE("Direct coordinator commits a proved stub rule", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	InstallRules(con, [] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_ONE", true, 0.0, 1));
		return rules;
	});
	auto result = con.Query(QUERY);
	REQUIRE(CHECK_COLUMN(result, 1, {1, 1, 1}));
	auto plan = Explain(con, QUERY);
	REQUIRE(plan.find("STUB_ONE") != string::npos);
}

TEST_CASE("Direct coordinator falls back to the solver and lists every miss under require", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	InstallRules(con, [] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_A", false, 0.0, 1));
		rules.push_back(make_uniq<StubRule>("STUB_B", false, 0.0, 1));
		return rules;
	});
	auto result = con.Query(QUERY);
	REQUIRE(CHECK_COLUMN(result, 1, {0, 0, 1}));
	auto plan = Explain(con, QUERY);
	REQUIRE(plan.find("Direct solve") == string::npos);

	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='require'"));
	auto required = con.Query(QUERY);
	REQUIRE(required->HasError());
	auto &message = required->GetError();
	REQUIRE(message.find("STUB_A: stub_miss: STUB_A") != string::npos);
	REQUIRE(message.find("STUB_B: stub_miss: STUB_B") != string::npos);
	REQUIRE(message.find("solver skipped=true") != string::npos);
}

TEST_CASE("Direct coordinator picks the cheapest proved rule, then the first registered", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	SECTION("cheaper wins") {
		InstallRules(con, [] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_COSTLY", true, 2.0, 0));
			rules.push_back(make_uniq<StubRule>("STUB_CHEAP", true, 1.0, 1));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(CHECK_COLUMN(result, 1, {1, 1, 1}));
		REQUIRE(Explain(con, QUERY).find("STUB_CHEAP") != string::npos);
	}
	SECTION("tie goes to the first registered") {
		InstallRules(con, [] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_FIRST", true, 1.0, 0));
			rules.push_back(make_uniq<StubRule>("STUB_SECOND", true, 1.0, 1));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(CHECK_COLUMN(result, 1, {0, 0, 0}));
		REQUIRE(Explain(con, QUERY).find("STUB_FIRST") != string::npos);
	}
}

TEST_CASE("Direct coordinator gives Cost the estimated source row count", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	idx_t seen_rows = 0;
	InstallRules(con, [&seen_rows] {
		RuleList rules;
		rules.push_back(make_uniq<StubRule>("STUB_SEEN", true, 0.0, 1, StubValidation::NONE, &seen_rows));
		return rules;
	});
	REQUIRE_NO_FAIL(con.Query(QUERY));
	REQUIRE(seen_rows == 3);
}

TEST_CASE("Direct coordinator treats a broken rule as an internal error", "[decidb]") {
	SECTION("non-finite cost") {
		DuckDB db(nullptr);
		Connection con(db);
		InstallRules(con, [] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_NAN", true, std::numeric_limits<double>::quiet_NaN(), 1));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(result->HasError());
		REQUIRE(result->GetError().find("non-finite cost") != string::npos);
	}
	SECTION("output slot that does not match the DECIDE output") {
		DuckDB db(nullptr);
		Connection con(db);
		InstallRules(con, [] {
			RuleList rules;
			auto rule = make_uniq<StubRule>("STUB_WRONG_SLOT", true, 0.0, 1);
			rule->wrong_output_slot = true;
			rules.push_back(std::move(rule));
			return rules;
		});
		auto result = con.Query(QUERY);
		REQUIRE(result->HasError());
		REQUIRE(result->GetError().find("mismatched output slot") != string::npos);
	}
}

TEST_CASE("Direct validation barrier reads every row before an outer LIMIT or COUNT returns", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	// One thread, so a streaming plan under LIMIT 1 deterministically stops after its first chunk.
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	auto install = [&](StubValidation validation) {
		InstallRules(con, [validation] {
			RuleList rules;
			rules.push_back(make_uniq<StubRule>("STUB_VALIDATING", true, 0.0, 1, validation));
			return rules;
		});
	};
	const string decide = "FROM (SELECT CASE WHEN i = 4999 THEN NULL ELSE i END AS v FROM range(5000) t(i))"
	                      " DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(v*x)";

	// Without the barrier the late NULL is never read: the check is only as eager as the parent.
	install(StubValidation::STREAMING);
	auto streaming = con.Query("SELECT v FROM (" + decide + ") q LIMIT 1");
	REQUIRE_NO_FAIL(*streaming);

	install(StubValidation::BARRIER);
	for (auto &outer : {"SELECT v FROM (" + decide + ") q LIMIT 1", "SELECT COUNT(*) FROM (" + decide + ") q"}) {
		auto result = con.Query(outer);
		REQUIRE(result->HasError());
		REQUIRE(result->GetError().find("stub: NULL") != string::npos);
	}
	auto nothing = con.Query("SELECT v FROM (" + decide + ") q LIMIT 0");
	REQUIRE_NO_FAIL(*nothing);
	REQUIRE(nothing->RowCount() == 0);
}
