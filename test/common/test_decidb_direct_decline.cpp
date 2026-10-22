#include "catch.hpp"
#include "test_helpers.hpp"

#include <functional>
#include <limits>

#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_rules.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

// The decline branches of a rule's CheckCondition, tested on the IR. A rule's input is a DirectIR, so each case
// hands the rule the IR of a small query and asserts that it declines. The IR is the one the rule driver builds: a
// probe rule, installed through the rule driver's test seam, receives the IR the driver offers, optionally edits it,
// calls the rule under test on it and records whether it applied. The query is planned (EXPLAIN), never run, and
// the probe declines, so the solver path stays in place.
//
// The edit is for the shapes the front end never produces from SQL (a per-row clause that carries a reducer, a term
// over a second decision, an output type that is not INTEGER) and for PER keys. Every case names the branch it
// targets, and reaches an arm of the rule's decision code that no other case of the rule reaches (the per-arm
// check is `make decide-coverage`). Each rule also has forms that apply, so a decline is about the one thing a
// case changes.
//
// `DirectIRIsReadable` is the rule driver's own check; its cases build the IR struct from scratch.

using namespace duckdb;

namespace direct_decline_test {

using Edit = std::function<void(DirectIR &)>;

//! One query shape, the branch it targets, and the optional edit applied to the IR the query reads as.
struct Shape {
	string branch;
	string declaration;
	string constraints;
	string objective;
	Edit edit;
};

//! Source columns: id, w (DOUBLE), cap (INTEGER), flag (BOOLEAN), name (VARCHAR), f (FLOAT).
const char *SOURCE_ROWS =
    "(VALUES (1, 2.0::DOUBLE, 3, TRUE, 'a', 1.5::FLOAT), (2, 1.0::DOUBLE, 1, FALSE, 'b', 2.5::FLOAT),"
    " (3, 4.0::DOUBLE, 2, TRUE, 'c', 0.5::FLOAT)) t(id, w, cap, flag, name, f)";
constexpr idx_t NAME_SLOT = 4;

//! Offers the IR to `target`, after the edit, and records the answer. It always declines, so the query keeps its
//! solver plan and EXPLAIN has nothing to run.
class ProbeRule final : public DirectSolveRule {
public:
	ProbeRule(duckdb::unique_ptr<DirectSolveRule> target_p, Edit edit_p, bool &offered_p, bool &applied_p)
	    : target(std::move(target_p)), edit(std::move(edit_p)), offered(offered_p), applied(applied_p) {
	}
	const char *Name() const override {
		return "PROBE";
	}
	duckdb::unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir,
	                                                                ClientContext &context) const override {
		offered = true;
		if (edit) {
			edit(const_cast<DirectIR &>(ir));
		}
		applied = target->CheckCondition(ir, context) != nullptr;
		return nullptr;
	}
	string Summary() const override {
		return "probe";
	}
	DirectSubstitute Apply(duckdb::unique_ptr<LogicalOperator>, Optimizer &,
	                       const DirectApplicabilityCondition &) const override {
		throw InternalException("The probe rule never applies");
	}

private:
	duckdb::unique_ptr<DirectSolveRule> target;
	Edit edit;
	bool &offered;
	bool &applied;
};

using MakeRule = duckdb::unique_ptr<DirectSolveRule> (*)();

//! Whether the rule applies to the shape. The shape must be one the rule driver offers the rule (the IR is read in
//! full), so a case cannot decline because the front end failed to read the query.
bool Applies(MakeRule make_rule, Connection &con, const Shape &shape) {
	INFO(shape.branch << " | " << shape.declaration << " | " << shape.constraints << " | " << shape.objective);
	bool offered = false;
	bool applied = false;
	auto state = make_shared_ptr<DirectRuleTestSeam>();
	state->make_rules = [&] {
		duckdb::vector<duckdb::unique_ptr<DirectSolveRule>> rules;
		rules.push_back(make_uniq<ProbeRule>(make_rule(), shape.edit, offered, applied));
		return rules;
	};
	con.context->registered_state->Remove(DIRECT_RULE_TEST_SEAM_KEY);
	con.context->registered_state->Insert(DIRECT_RULE_TEST_SEAM_KEY, std::move(state));
	auto sql = "EXPLAIN SELECT id FROM (FROM " + string(SOURCE_ROWS) + " DECIDE " + shape.declaration + " SUCH THAT " +
	           shape.constraints + " " + shape.objective + ") q";
	auto result = con.Query(sql);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(offered);
	return applied;
}

void ExpectDeclines(MakeRule make_rule, Connection &con, const duckdb::vector<Shape> &shapes) {
	for (auto &shape : shapes) {
		INFO("declines: " << shape.branch);
		CHECK(!Applies(make_rule, con, shape));
	}
}

void ExpectApplies(MakeRule make_rule, Connection &con, const duckdb::vector<Shape> &shapes) {
	for (auto &shape : shapes) {
		INFO("applies: " << shape.branch);
		CHECK(Applies(make_rule, con, shape));
	}
}

//===--------------------------------------------------------------------===//
// Edits: shapes the front end does not produce from this SQL
//===--------------------------------------------------------------------===//

duckdb::unique_ptr<Expression> True() {
	return make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
}

//! A boolean over a column that is not one of the query's source columns.
duckdb::unique_ptr<Expression> ForeignBoolean() {
	return make_uniq<BoundColumnRefExpression>(LogicalType::BOOLEAN, ColumnBinding(9999, 0));
}

DirectPart &LeftPart(DirectIR &ir, idx_t constraint = 0) {
	return ir.constraints[constraint].lhs[0];
}

DecideSplitTerm CopyTerm(const DecideSplitTerm &term) {
	DecideSplitTerm copy;
	copy.kind = term.kind;
	copy.sign = term.sign;
	if (term.coefficient) {
		copy.coefficient = term.coefficient->Copy();
	}
	copy.square_scale = term.square_scale;
	copy.var_a = term.var_a;
	copy.var_b = term.var_b;
	for (auto &inner : term.inner) {
		copy.inner.push_back(CopyTerm(inner));
	}
	return copy;
}

//! A column of the given type that is not one of the query's source columns.
duckdb::unique_ptr<Expression> ForeignColumn(const LogicalType &type) {
	return make_uniq<BoundColumnRefExpression>(type, ColumnBinding(9999, 0));
}

//! A comparison used as a value with two column children: the first is `first`, the second is source column `slot`.
duckdb::unique_ptr<Expression> PairWithSourceColumn(DirectIR &ir, duckdb::unique_ptr<Expression> first, idx_t slot) {
	return make_uniq<BoundComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, std::move(first),
	    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ir.source_bindings[slot]));
}

Edit PerKeyOnConstraint(idx_t constraint) {
	return [constraint](DirectIR &ir) {
		ir.constraints[constraint].scope.per_key_slots.push_back(0);
	};
}

//===--------------------------------------------------------------------===//
// S1
//===--------------------------------------------------------------------===//

const char *S1_OBJECTIVE = "MAXIMIZE SUM(w*x)";

Shape S1Plain() {
	return {"plain form: one count bound and a linear objective", "x(BOOL)", "SUM(x) <= 2", S1_OBJECTIVE, nullptr};
}

//! A shape over one Boolean `x` with the given constraints and the plain objective.
Shape S1(const string &branch, const string &constraints, Edit edit = nullptr, const string &objective = S1_OBJECTIVE,
         const string &declaration = "x(BOOL)") {
	return {branch, declaration, constraints, objective, std::move(edit)};
}

duckdb::vector<Shape> S1AppliesShapes() {
	duckdb::vector<Shape> shapes;
	shapes.push_back(S1Plain());
	shapes.push_back(S1("a source-valued count bound", "SUM(x) <= cap"));
	shapes.push_back(S1("fixed values against a constant, a Boolean column and a number column",
	                    "SUM(x) <= 2 AND x = 1 AND x >= flag AND x <= cap AND x >= -3"));
	shapes.push_back(S1("a scaled objective part", "SUM(x) <= 2", nullptr, "MINIMIZE 2 * SUM(w*x) / 4"));
	shapes.push_back(S1("a count bound below zero", "SUM(x) <= -1"));
	shapes.push_back(S1("a feasibility problem", "SUM(x) <= 2", nullptr, ""));
	shapes.push_back(S1("counts PER a column", "SUM(x) <= 2 PER flag"));
	shapes.push_back(S1("an aggregate-local WHEN", "SUM(x) WHEN flag <= 2 AND SUM(x) WHEN flag >= 1"));
	shapes.push_back(S1("a negated body against every comparison",
	                    "SUM(-x) <= -1 AND SUM(-x) < 5 AND SUM(-x) > -3 AND SUM(-x) >= -3"));
	shapes.push_back(S1("a negated body against an equality", "SUM(-x) = -2"));
	shapes.push_back(S1("a computed source-valued count bound", "SUM(x) <= cap + cap"));
	shapes.push_back(S1("a scaled objective that divides", "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(w*x) / 4"));
	shapes.push_back(
	    S1("fixed values by a strict or not-equal comparison", "SUM(x) <= 2 AND x > 0 AND x < 1 AND x <> 1"));
	shapes.push_back(S1("fixed values from a Boolean column", "SUM(x) <= 2 AND x = flag AND x <= flag AND x >= flag"));
	shapes.push_back(S1("fixed values from a Boolean column under a WHEN",
	                    "SUM(x) <= 2 AND x <= flag WHEN flag AND x = 1 WHEN flag"));
	shapes.push_back(S1("a Boolean column compared strictly", "SUM(x) <= 2 AND x < flag"));
	shapes.push_back(S1("a fixed value from a Boolean expression", "SUM(x) <= 2 AND x <= CAST(NOT flag AS INTEGER)"));
	shapes.push_back(S1("a fixed value from a DOUBLE column", "SUM(x) <= 2 AND x <= w"));
	shapes.push_back(S1("a fixed value from a FLOAT column", "SUM(x) <= 2 AND x <= f"));
	shapes.push_back(S1("a fixed value through TRY_CAST", "SUM(x) <= 2 AND x <= TRY_CAST(cap AS TINYINT)"));
	shapes.push_back(S1("a fixed value through a cast to the same type",
	                    "SUM(x) <= 2 AND x <= CAST(CAST(cap AS DECIMAL(5,2)) AS DECIMAL(9,2))"));
	return shapes;
}

duckdb::vector<Shape> S1DeclineShapes() {
	duckdb::vector<Shape> shapes;
	// CheckCondition: the decision.
	shapes.push_back(S1("two decisions", "SUM(x) <= 2", nullptr, S1_OBJECTIVE, "x(BOOL), y(BOOL)"));
	shapes.push_back(S1("a REAL decision", "SUM(x) <= 2", nullptr, S1_OBJECTIVE, "x(REAL)"));
	shapes.push_back(S1("a query-wide decision", "s <= 1", nullptr, "MAXIMIZE s", "scalar s(BOOL)"));
	shapes.push_back(S1("a decision whose output is not INTEGER", "SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.decisions[0].output_type = LogicalType::BIGINT; }));
	shapes.push_back(S1("an entity scope", "SUM(x) <= 2", [](DirectIR &ir) { ir.entity_scopes.emplace_back(); }));
	// CheckCondition: the constraints.
	shapes.push_back(S1("no constraint", "SUM(x) <= 2", [](DirectIR &ir) {
		ir.constraints.clear();
		ir.source_clause_count = 0;
	}));
	shapes.push_back(S1("a clause count that differs from the constraint count", "SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.source_clause_count++; }));
	shapes.push_back(S1("a source clause id out of range", "SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.constraints[0].source_clause_id = DConstants::INVALID_INDEX; }));
	shapes.push_back(S1("one source clause read twice", "SUM(x) >= 1 AND SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.constraints[1].source_clause_id = ir.constraints[0].source_clause_id; }));
	shapes.push_back(S1("a membership constraint", "SUM(x) <= 2 AND x IN (0, 1)"));
	shapes.push_back(S1("a count that is not a bound", "SUM(x) <> 1"));
	shapes.push_back(S1("no count bound, only a fixed value", "x = 1"));
	// FixedTerms and OnlyDecisionTerms: a per-row clause that is not a fix is read as a count bound, which declines.
	shapes.push_back(S1("a per-row clause with a negative sign", "SUM(x) <= 2 AND x <= 1",
	                    [](DirectIR &ir) { LeftPart(ir, 1).sign = -1; }));
	shapes.push_back(S1("a per-row clause with a reducer", "SUM(x) <= 2 AND x <= 1",
	                    [](DirectIR &ir) { LeftPart(ir, 1).reducer = DirectReducer::SUM; }));
	shapes.push_back(S1("a per-row clause with a scale", "SUM(x) <= 2 AND x <= 1", [](DirectIR &ir) {
		LeftPart(ir, 1).scale = make_uniq<BoundConstantExpression>(Value::DOUBLE(2.0));
	}));
	shapes.push_back(S1("a per-row clause with two parts", "SUM(x) <= 2 AND x <= 1",
	                    [](DirectIR &ir) { ir.constraints[1].lhs.emplace_back(); }));
	shapes.push_back(S1("a per-row clause that is a square", "SUM(x) <= 2 AND POWER(x, 2) <= 1"));
	shapes.push_back(S1("a per-row clause over another decision", "SUM(x) <= 2 AND x <= 1",
	                    [](DirectIR &ir) { LeftPart(ir, 1).terms[0].var_a = 1; }));
	// CheckCondition: the objective.
	shapes.push_back(S1("an objective WHEN", "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(w*x) WHEN flag"));
	shapes.push_back(
	    S1("an objective PER key", "SUM(x) <= 2", [](DirectIR &ir) { ir.objective.scope.per_key_slots.push_back(0); }));
	shapes.push_back(
	    S1("a sense with no objective part", "SUM(x) <= 2", [](DirectIR &ir) { ir.objective.parts.clear(); }));
	// ProveScope.
	shapes.push_back(S1("a count with a negative sign", "0 - SUM(x) >= -2"));
	shapes.push_back(S1("an average where a count is", "AVG(x) <= 1"));
	shapes.push_back(S1("a qualified count", "SUM(x) <= 2", [](DirectIR &ir) { LeftPart(ir).qualifier = 0; }));
	shapes.push_back(
	    S1("a nested count", "SUM(x) <= 2", [](DirectIR &ir) { LeftPart(ir).inner = make_uniq<DirectPart>(); }));
	shapes.push_back(S1("a sum of two aggregates", "SUM(x) + SUM(x) <= 2"));
	shapes.push_back(S1("a later bound that is not a plain count", "SUM(x) >= 1 AND 2 * SUM(x) <= 4"));
	shapes.push_back(S1("a later bound with another aggregate WHEN and clause WHEN", "SUM(x) >= 1 AND SUM(x) <= 2",
	                    [](DirectIR &ir) {
		                    LeftPart(ir, 1).filter = True();
		                    ir.constraints[1].scope.when = True();
	                    }));
	shapes.push_back(S1("bounds with different PER keys", "SUM(x) >= 1 AND SUM(x) <= 2", PerKeyOnConstraint(1)));
	shapes.push_back(S1("a second bound without the WHEN the first has", "SUM(x) >= 1 WHEN flag AND SUM(x) <= 2"));
	shapes.push_back(
	    S1("bounds with different WHEN conditions", "SUM(x) >= 1 WHEN flag AND SUM(x) <= 2 WHEN (NOT flag)"));
	shapes.push_back(S1("a WHEN that may raise", "SUM(x) <= 2 WHEN (CAST(cap AS TINYINT) > 0)"));
	shapes.push_back(S1("a WHEN that is not Boolean", "SUM(x) <= 2", [](DirectIR &ir) {
		ir.constraints[0].scope.when = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	}));
	// ProveFixedVariables.
	shapes.push_back(S1("a fixed value under PER", "SUM(x) <= 2 AND x = 1", PerKeyOnConstraint(1)));
	shapes.push_back(S1("a fixed value with a column coefficient", "SUM(x) <= 2 AND w * x = 1"));
	shapes.push_back(S1("a fixed value with no coefficient", "SUM(x) <= 2 AND x = 1",
	                    [](DirectIR &ir) { LeftPart(ir, 1).terms[0].coefficient.reset(); }));
	shapes.push_back(
	    S1("a fixed value under a WHEN that reads more than the source", "SUM(x) <= 2 AND x = 1 WHEN (random() < 2)"));
	shapes.push_back(S1("a fixed value below the Boolean domain", "SUM(x) <= 2 AND x <= -1"));
	shapes.push_back(
	    S1("a fixed value that is a constant but not a finite number", "SUM(x) <= 2 AND x = 'inf'::DOUBLE"));
	// SourceBooleanFixedValue: the value is a Boolean column or expression cast to a number.
	shapes.push_back(S1("a fixed value from a Boolean expression that may raise",
	                    "SUM(x) <= 2 AND x <= CAST(CAST(cap AS TINYINT) AS BOOLEAN)"));
	shapes.push_back(
	    S1("a fixed value from a Boolean expression over a column outside the source", "SUM(x) <= 2 AND x <= flag",
	       [](DirectIR &ir) { ir.constraints[1].rhs->Cast<BoundCastExpression>().child = ForeignBoolean(); }));
	shapes.push_back(S1("a fixed value from a Boolean expression that reads no column", "SUM(x) <= 2 AND x <= flag",
	                    [](DirectIR &ir) {
		                    ir.constraints[1].rhs->Cast<BoundCastExpression>().child =
		                        make_uniq<BoundReferenceExpression>(LogicalType::BOOLEAN, 0);
	                    }));
	shapes.push_back(S1("a fixed value from a number that is not Boolean", "SUM(x) <= 2 AND x <= flag",
	                    [](DirectIR &ir) { ir.constraints[1].rhs->return_type = LogicalType::VARCHAR; }));
	shapes.push_back(S1("a fixed value from a Boolean expression that is a parameter", "SUM(x) <= 2 AND x <= flag",
	                    [](DirectIR &ir) {
		                    auto parameter = make_uniq<BoundParameterExpression>("1");
		                    parameter->return_type = LogicalType::BOOLEAN;
		                    ir.constraints[1].rhs->Cast<BoundCastExpression>().child = std::move(parameter);
	                    }));
	// ProveBounds.
	shapes.push_back(S1("a count body with a column coefficient", "SUM(w*x) <= 2"));
	shapes.push_back(S1("a count body that adds up to two", "SUM(2*x) <= 2"));
	shapes.push_back(S1("a count body that is a square", "SUM(POWER(x, 2)) <= 2"));
	shapes.push_back(
	    S1("a count body over another decision", "SUM(x) <= 2", [](DirectIR &ir) { LeftPart(ir).terms[0].var_a = 1; }));
	shapes.push_back(S1("a count body with no coefficient", "SUM(x) <= 2",
	                    [](DirectIR &ir) { LeftPart(ir).terms[0].coefficient.reset(); }));
	// The right-hand-side rewrite turns an aggregate on the right into a source column, so the IR holds one only when
	// the rewrite leaves it.
	shapes.push_back(S1("a count bound that is an aggregate", "SUM(x) <= 2", [](DirectIR &ir) {
		ir.constraints[0].rhs = make_uniq<BoundAggregateExpression>(CountStarFun::GetFunction(),
		                                                            duckdb::vector<duckdb::unique_ptr<Expression>>(),
		                                                            nullptr, nullptr, AggregateType::NON_DISTINCT);
	}));
	shapes.push_back(S1("a strict lower count bound at the exact range", "SUM(x) > 9007199254740992"));
	shapes.push_back(S1("a lower count bound above the exact range", "SUM(x) >= 1e30"));
	shapes.push_back(S1("a count bound that is a column of a correlated query", "SUM(x) <= 2", [](DirectIR &ir) {
		ir.constraints[0].rhs = make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ir.source_bindings[2], 1);
	}));
	shapes.push_back(S1("a count bound that is a column outside the source", "SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.constraints[0].rhs = ForeignColumn(LogicalType::INTEGER); }));
	shapes.push_back(S1("a count bound that is a cast to a type that is not a number", "SUM(x) <= cap",
	                    [](DirectIR &ir) { ir.constraints[0].rhs->return_type = LogicalType::BOOLEAN; }));
	// ProveObjective.
	shapes.push_back(S1("an average objective", "SUM(x) <= 2", nullptr, "MAXIMIZE AVG(w*x)"));
	shapes.push_back(S1("an objective part with a WHEN", "SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.objective.parts[0].filter = True(); }));
	shapes.push_back(S1("a nested objective part", "SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.objective.parts[0].inner = make_uniq<DirectPart>(); }));
	shapes.push_back(
	    S1("a qualified objective part", "SUM(x) <= 2", [](DirectIR &ir) { ir.objective.parts[0].qualifier = 0; }));
	shapes.push_back(S1("an objective constant", "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(w*x + 1)"));
	shapes.push_back(S1("an objective term over another decision", "SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.objective.parts[0].terms[0].var_a = 1; }));
	shapes.push_back(S1("an objective divided by zero", "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(w*x) / 0"));
	shapes.push_back(
	    S1("an objective scaled by infinity", "SUM(x) <= 2", nullptr, "MAXIMIZE 'inf'::DOUBLE * SUM(w*x)"));
	shapes.push_back(S1("an objective coefficient that is a string", "SUM(x) <= 2", [](DirectIR &ir) {
		ir.objective.parts[0].terms[0].coefficient = make_uniq<BoundConstantExpression>(Value("a"));
	}));
	shapes.push_back(
	    S1("an objective coefficient that reads a column outside the source", "SUM(x) <= 2",
	       [](DirectIR &ir) { ir.objective.parts[0].terms[0].coefficient = ForeignColumn(LogicalType::DOUBLE); }));
	shapes.push_back(S1("a random objective coefficient", "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(random()*x)"));
	shapes.push_back(S1("several objective terms where a coefficient has a child that may raise before another child",
	                    "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(w*x) + SUM(least(cap*cap, 5)*x)"));
	return shapes;
}

//===--------------------------------------------------------------------===//
// A1
//===--------------------------------------------------------------------===//

const char *A1_OBJECTIVE = "MAXIMIZE SUM(w*x)";

Shape A1(const string &branch, const string &constraints, Edit edit = nullptr, const string &objective = A1_OBJECTIVE,
         const string &declaration = "x(REAL)") {
	return {branch, declaration, constraints, objective, std::move(edit)};
}

duckdb::vector<Shape> A1AppliesShapes() {
	duckdb::vector<Shape> shapes;
	shapes.push_back(A1("plain form: a per-row bound and a linear objective", "x <= cap"));
	shapes.push_back(A1("an equality and a bound under a WHEN", "x = cap AND x >= -3 WHEN flag"));
	shapes.push_back(A1("a Boolean decision", "x <= flag", nullptr, A1_OBJECTIVE, "x(BOOL)"));
	shapes.push_back(A1("a square with a constant", "x <= cap", nullptr, "MINIMIZE SUM(POWER(x - w, 2) + 5)"));
	shapes.push_back(
	    A1("an objective part with a WHEN", "x <= cap", nullptr, "MINIMIZE SUM(POWER(w*x - 3, 2)) WHEN flag"));
	shapes.push_back(A1("no objective", "x <= cap", nullptr, ""));
	shapes.push_back(A1("an objective constant that reads a column", "x <= 5", nullptr, "MAXIMIZE SUM(2*x + w)"));
	shapes.push_back(
	    A1("a whole-numbered decision with strict folded bounds", "x > 0 AND x < 5", nullptr, A1_OBJECTIVE, "x(INT)"));
	shapes.push_back(
	    A1("an objective part with a filter", "x <= cap", [](DirectIR &ir) { ir.objective.parts[0].filter = True(); }));
	shapes.push_back(A1(
	    "a square over two terms of one decision", "x <= cap",
	    [](DirectIR &ir) {
		    auto &inner = ir.objective.parts[0].terms[0].inner;
		    inner.push_back(CopyTerm(inner[0]));
	    },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	return shapes;
}

duckdb::vector<Shape> A1DeclineShapes() {
	duckdb::vector<Shape> shapes;
	// CheckCondition: the decisions.
	shapes.push_back(A1("no decision", "x <= cap", [](DirectIR &ir) { ir.decisions.clear(); }));
	shapes.push_back(A1("an entity scope", "x <= cap", [](DirectIR &ir) { ir.entity_scopes.emplace_back(); }));
	shapes.push_back(A1("a query-wide decision", "s <= 3", nullptr, "MAXIMIZE s", "scalar s(REAL)"));
	// CheckCondition: the constraints.
	shapes.push_back(A1("an aggregate constraint", "SUM(x) <= 5"));
	shapes.push_back(A1("a membership constraint", "x IN (1, 2)"));
	shapes.push_back(A1("a not-equal constraint", "x <> 2", nullptr, A1_OBJECTIVE, "x(INT)"));
	shapes.push_back(A1("a comparison that is not a bound", "x <= cap",
	                    [](DirectIR &ir) { ir.constraints[0].comparison = ExpressionType::COMPARE_DISTINCT_FROM; }));
	shapes.push_back(A1("a PER key", "x <= cap", PerKeyOnConstraint(0)));
	// DirectReadsOneDecision.
	shapes.push_back(
	    A1("a constraint with two left parts", "x <= cap", [](DirectIR &ir) { ir.constraints[0].lhs.emplace_back(); }));
	shapes.push_back(A1("a product of two decisions", "x * y <= 4", nullptr, A1_OBJECTIVE, "x(REAL), y(REAL)"));
	shapes.push_back(A1("a left part with a negative sign", "x <= cap", [](DirectIR &ir) { LeftPart(ir).sign = -1; }));
	shapes.push_back(
	    A1("a left part with a reducer", "x <= cap", [](DirectIR &ir) { LeftPart(ir).reducer = DirectReducer::SUM; }));
	shapes.push_back(A1("a left part with a scale", "x <= cap", [](DirectIR &ir) {
		LeftPart(ir).scale = make_uniq<BoundConstantExpression>(Value::DOUBLE(2.0));
	}));
	shapes.push_back(A1("a left part with a filter", "x <= cap", [](DirectIR &ir) { LeftPart(ir).filter = True(); }));
	shapes.push_back(
	    A1("a nested left part", "x <= cap", [](DirectIR &ir) { LeftPart(ir).inner = make_uniq<DirectPart>(); }));
	shapes.push_back(A1("a left part with no term", "x <= cap", [](DirectIR &ir) { LeftPart(ir).terms.clear(); }));
	shapes.push_back(A1("a left part with a term that is not linear", "x <= cap",
	                    [](DirectIR &ir) { LeftPart(ir).terms[0].kind = DecideTermKind::CONSTANT; }));
	shapes.push_back(A1("a left part over two decisions", "x + y <= 3", nullptr, A1_OBJECTIVE, "x(REAL), y(REAL)"));
	shapes.push_back(A1("a left part with a term that has no coefficient", "x <= cap",
	                    [](DirectIR &ir) { LeftPart(ir).terms[0].coefficient.reset(); }));
	// CheckCondition: the objective.
	shapes.push_back(
	    A1("an objective PER key", "x <= cap", [](DirectIR &ir) { ir.objective.scope.per_key_slots.push_back(0); }));
	shapes.push_back(A1("an average objective", "x <= cap", nullptr, "MAXIMIZE AVG(w*x)"));
	shapes.push_back(A1("a nested objective part", "x <= cap",
	                    [](DirectIR &ir) { ir.objective.parts[0].inner = make_uniq<DirectPart>(); }));
	shapes.push_back(
	    A1("a qualified objective part", "x <= cap", [](DirectIR &ir) { ir.objective.parts[0].qualifier = 0; }));
	shapes.push_back(A1("an objective term that is an absolute value", "x <= cap", nullptr, "MAXIMIZE SUM(ABS(x))"));
	// DirectReadBounds.
	shapes.push_back(A1("a bound whose value is a subquery", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].rhs = make_uniq<BoundSubqueryExpression>(LogicalType::DOUBLE);
	}));
	shapes.push_back(A1("a bound whose value is a parameter", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].rhs = make_uniq<BoundParameterExpression>("1");
		ir.constraints[0].rhs->return_type = LogicalType::DOUBLE;
	}));
	shapes.push_back(A1("a bound whose value is an aggregate", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].rhs = make_uniq<BoundAggregateExpression>(CountStarFun::GetFunction(),
		                                                            duckdb::vector<duckdb::unique_ptr<Expression>>(),
		                                                            nullptr, nullptr, AggregateType::NON_DISTINCT);
	}));
	shapes.push_back(A1("a bound whose value reads the decision", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].rhs =
		    make_uniq<BoundColumnRefExpression>(LogicalType::DOUBLE, ColumnBinding(ir.decide_index, 0));
	}));
	shapes.push_back(A1("a bound whose value has a correlated child", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].rhs = PairWithSourceColumn(
		    ir, make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ir.source_bindings[2], 1), 2);
	}));
	shapes.push_back(A1("a bound whose value has a child outside the source", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].rhs = PairWithSourceColumn(ir, ForeignColumn(LogicalType::INTEGER), 2);
	}));
	shapes.push_back(A1("a bound whose value is a window", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].rhs =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
	}));
	shapes.push_back(A1("a bound whose value is a string", "x <= cap",
	                    [](DirectIR &ir) { ir.constraints[0].rhs = make_uniq<BoundConstantExpression>(Value("a")); }));
	shapes.push_back(A1("a bound under a WHEN that may raise", "x <= cap WHEN (CAST(cap AS TINYINT) > 0)"));
	shapes.push_back(A1("a bound under a WHEN that is not Boolean", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].scope.when = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	}));
	shapes.push_back(A1("a bound with a coefficient that is a string", "x <= cap", [](DirectIR &ir) {
		LeftPart(ir).terms[0].coefficient = make_uniq<BoundConstantExpression>(Value("a"));
	}));
	shapes.push_back(A1("a bound with a coefficient that reads a column outside the source", "x <= cap",
	                    [](DirectIR &ir) { LeftPart(ir).terms[0].coefficient = ForeignColumn(LogicalType::DOUBLE); }));
	shapes.push_back(A1("a folded bound that is not a constant", "x <= cap",
	                    [](DirectIR &ir) { ir.constraints[0].variable_bound = true; }));
	// The binder rejects a strict bound on a REAL decision, so the comparison is edited.
	shapes.push_back(A1("a strict folded bound on a real decision", "x <= 3",
	                    [](DirectIR &ir) { ir.constraints[0].comparison = ExpressionType::COMPARE_LESSTHAN; }));
	shapes.push_back(A1("a Boolean bound below the domain", "x <= -1", nullptr, A1_OBJECTIVE, "x(BOOL)"));
	// ProveObjective.
	shapes.push_back(A1("an objective WHEN that reads more than the source", "x <= cap", nullptr,
	                    "MAXIMIZE SUM(w*x) WHEN (random() < 2)"));
	shapes.push_back(A1("a random objective coefficient", "x <= cap", nullptr, "MAXIMIZE SUM(random()*x)"));
	shapes.push_back(A1("an objective divided by zero", "x <= cap", nullptr, "MAXIMIZE SUM(w*x) / 0"));
	shapes.push_back(A1("an objective scaled by infinity", "x <= cap", nullptr, "MAXIMIZE 'inf'::DOUBLE * SUM(w*x)"));
	shapes.push_back(A1("an objective term over a decision that is not declared", "x <= cap",
	                    [](DirectIR &ir) { ir.objective.parts[0].terms[0].var_a = 5; }));
	shapes.push_back(A1("an objective part with a filter that reads more than the source", "x <= cap",
	                    [](DirectIR &ir) { ir.objective.parts[0].filter = ForeignBoolean(); }));
	shapes.push_back(A1("an objective constant that is random", "x <= cap", nullptr, "MAXIMIZE SUM(w*x + random())"));
	shapes.push_back(A1("an objective term with no coefficient", "x <= cap",
	                    [](DirectIR &ir) { ir.objective.parts[0].terms[0].coefficient.reset(); }));
	shapes.push_back(A1(
	    "an objective constant with no value", "x <= cap",
	    [](DirectIR &ir) {
		    for (auto &term : ir.objective.parts[0].terms) {
			    if (term.kind == DecideTermKind::CONSTANT) {
				    term.coefficient.reset();
			    }
		    }
	    },
	    "MAXIMIZE SUM(w*x + 1)"));
	shapes.push_back(A1(
	    "a square scaled by zero", "x <= cap", [](DirectIR &ir) { ir.objective.parts[0].terms[0].square_scale = 0.0; },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(A1(
	    "a square scaled by a number that is not finite", "x <= cap",
	    [](DirectIR &ir) { ir.objective.parts[0].terms[0].square_scale = std::numeric_limits<double>::quiet_NaN(); },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	// The binder rejects an objective with two quadratic groups, so the second square is added to the IR.
	shapes.push_back(A1(
	    "two squares", "x <= cap",
	    [](DirectIR &ir) {
		    auto &terms = ir.objective.parts[0].terms;
		    terms.push_back(CopyTerm(terms[0]));
	    },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(A1(
	    "a square over a decision that is not declared", "x <= cap",
	    [](DirectIR &ir) { ir.objective.parts[0].terms[0].inner[0].var_a = 5; }, "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(
	    A1("a square over two decisions", "x <= cap", nullptr, "MINIMIZE SUM(POWER(x + y, 2))", "x(REAL), y(REAL)"));
	shapes.push_back(A1(
	    "a square over a term that is neither linear nor constant", "x <= cap",
	    [](DirectIR &ir) { ir.objective.parts[0].terms[0].inner[1].kind = DecideTermKind::ABS; },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(A1(
	    "a square over no decision", "x <= cap",
	    [](DirectIR &ir) {
		    auto &inner = ir.objective.parts[0].terms[0].inner;
		    inner.erase(inner.begin());
	    },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(
	    A1("a square with a random constant", "x <= cap", nullptr, "MINIMIZE SUM(POWER(x - random(), 2))"));
	shapes.push_back(A1(
	    "a square term with no coefficient", "x <= cap",
	    [](DirectIR &ir) { ir.objective.parts[0].terms[0].inner[1].coefficient.reset(); },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	return shapes;
}

TEST_CASE("S1 applies to the forms of its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectApplies(MakeS1UniformMatroidRule, con, S1AppliesShapes());
}

TEST_CASE("S1 declines each shape that is not exactly its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectDeclines(MakeS1UniformMatroidRule, con, S1DeclineShapes());
}

TEST_CASE("A1 applies to the forms of its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectApplies(MakeA1SeparableBoxRule, con, A1AppliesShapes());
}

TEST_CASE("A1 declines each shape that is not exactly its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectDeclines(MakeA1SeparableBoxRule, con, A1DeclineShapes());
}

//===--------------------------------------------------------------------===//
// The rule driver's check that the front end read the whole IR
//===--------------------------------------------------------------------===//

//! An IR the front end read in full, built by hand: one proved constraint, a finite offset and a known sense.
DirectIR ReadableIR() {
	DirectIR ir;
	ir.constraints.emplace_back();
	ir.objective.status = DirectAnalysisStatus::PROVED;
	ir.objective.sense = DecideSense::MAXIMIZE;
	ir.objective.offset = 0;
	return ir;
}

TEST_CASE("The rule driver offers a rule only an IR the front end read in full", "[decidb]") {
	REQUIRE(DirectIRIsReadable(ReadableIR()));
	// Each sense the rules know is readable; the objective sense is a branch of its own.
	for (auto sense : {DecideSense::MAXIMIZE, DecideSense::MINIMIZE, DecideSense::FEASIBILITY}) {
		auto ir = ReadableIR();
		ir.objective.sense = sense;
		CHECK(DirectIRIsReadable(ir));
	}

	// Each case breaks the one thing it names.
	auto objective = ReadableIR();
	objective.objective.status = DirectAnalysisStatus::NOT_PROVED;
	CHECK(!DirectIRIsReadable(objective));
	auto offset = ReadableIR();
	offset.objective.offset = std::numeric_limits<double>::infinity();
	CHECK(!DirectIRIsReadable(offset));
	auto sense = ReadableIR();
	sense.objective.sense = static_cast<DecideSense>(7);
	CHECK(!DirectIRIsReadable(sense));
	auto constraint = ReadableIR();
	constraint.constraints.emplace_back();
	constraint.constraints[1].status = DirectAnalysisStatus::NOT_PROVED;
	CHECK(!DirectIRIsReadable(constraint));
}

} // namespace direct_decline_test
