#include "catch.hpp"
#include "test_helpers.hpp"

#include <functional>
#include <limits>

#include "duckdb/common/string_util.hpp"
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
// over a second decision, an output type that is not INTEGER) and for grouping keys. Every case names the branch it
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
constexpr idx_t FLAG_SLOT = 3;
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
	DirectSubstitute Apply(DirectSourceReader &, Optimizer &, const DirectApplicabilityCondition &,
	                       const DirectStateErrors &) const override {
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
	copy.sign = term.sign;
	if (term.coefficient) {
		copy.coefficient = term.coefficient->Copy();
	}
	copy.degree = term.degree;
	copy.var_a = term.var_a;
	for (auto &factor : term.factors) {
		DecideSplitFactor read;
		read.power = factor.power;
		for (auto &part : factor.form) {
			read.form.push_back(CopyTerm(part));
		}
		copy.factors.push_back(std::move(read));
	}
	if (term.opaque) {
		copy.opaque = term.opaque->Copy();
	}
	for (auto &inner : term.inner) {
		copy.inner.push_back(CopyTerm(inner));
	}
	return copy;
}

//! The linear form a square term squares.
duckdb::vector<DecideSplitTerm> &SquareForm(DecideSplitTerm &term) {
	return term.factors[0].form;
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
	shapes.push_back(S1("an objective WHEN", "SUM(x) <= 2", nullptr, "MAXIMIZE when flag: SUM(w*x)"));
	shapes.push_back(S1("an objective part with a WHEN", "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(when flag: w*x)"));
	shapes.push_back(S1("a WHEN on the objective and on its part", "SUM(x) <= 2", nullptr,
	                    "MAXIMIZE when flag: SUM(when cap > 1: w*x)"));
	shapes.push_back(S1("a count bound below zero", "SUM(x) <= -1"));
	shapes.push_back(S1("a feasibility problem", "SUM(x) <= 2", nullptr, ""));
	shapes.push_back(S1("an aggregate-local WHEN", "SUM(when flag: x) <= 2 AND SUM(when flag: x) >= 1"));
	shapes.push_back(S1("a negated body against every comparison",
	                    "SUM(-x) <= -1 AND SUM(-x) < 5 AND SUM(-x) > -3 AND SUM(-x) >= -3"));
	shapes.push_back(S1("a negated body against an equality", "SUM(-x) = -2"));
	shapes.push_back(S1("a computed source-valued count bound", "SUM(x) <= cap + cap"));
	shapes.push_back(S1("a scaled objective that divides", "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(w*x) / 4"));
	shapes.push_back(
	    S1("fixed values by a strict or not-equal comparison", "SUM(x) <= 2 AND x > 0 AND x < 1 AND x <> 1"));
	shapes.push_back(S1("fixed values from a Boolean column", "SUM(x) <= 2 AND x = flag AND x <= flag AND x >= flag"));
	shapes.push_back(S1("fixed values from a Boolean column under a WHEN",
	                    "SUM(x) <= 2 AND when flag: x <= flag AND when flag: x = 1"));
	shapes.push_back(S1("a Boolean column compared strictly", "SUM(x) <= 2 AND x < flag"));
	shapes.push_back(S1("a fixed value from a Boolean expression", "SUM(x) <= 2 AND x <= CAST(NOT flag AS INTEGER)"));
	shapes.push_back(S1("a fixed value from a DOUBLE column", "SUM(x) <= 2 AND x <= w"));
	shapes.push_back(S1("a fixed value from a FLOAT column", "SUM(x) <= 2 AND x <= f"));
	shapes.push_back(S1("a fixed value with a column coefficient", "SUM(x) <= 2 AND w * x = 1"));
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
	shapes.push_back(S1("a query-wide decision", "s <= 1", nullptr, "MAXIMIZE s", "per (): s(BOOL)"));
	shapes.push_back(S1("a decision whose output is not INTEGER", "SUM(x) <= 2",
	                    [](DirectIR &ir) { ir.decisions[0].output_type = LogicalType::BIGINT; }));
	shapes.push_back(S1("a keyed scope", "SUM(x) <= 2", [](DirectIR &ir) { ir.keyed_scopes.emplace_back(); }));
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
	shapes.push_back(S1("an objective with a by key", "SUM(x) <= 2", nullptr, "MAXIMIZE SUM(w*x) by (flag)"));
	shapes.push_back(
	    S1("a sense with no objective part", "SUM(x) <= 2", [](DirectIR &ir) { ir.objective.parts.clear(); }));
	// ProveScope.
	shapes.push_back(S1("a count with a negative sign", "0 - SUM(x) >= -2"));
	// The canonicalizer reads `0 - SUM(x)` as a scale of -1, so only an edit sets the part's own sign.
	shapes.push_back(
	    S1("a count whose part has a negative sign", "SUM(x) <= 2", [](DirectIR &ir) { LeftPart(ir).sign = -1; }));
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
	shapes.push_back(S1("bounds with different keys", "SUM(x) by (flag) >= 1 AND SUM(x) by (name) <= 2"));
	shapes.push_back(S1("a bound with a key beside one without", "SUM(x) by (flag) >= 1 AND SUM(x) <= 2"));
	shapes.push_back(S1("a key that reads a decision", "SUM(x) by (flag) <= 2", [](DirectIR &ir) {
		LeftPart(ir).by[0].grouping =
		    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ColumnBinding(ir.decide_index, 0));
	}));
	shapes.push_back(S1("a second bound without the WHEN the first has", "when flag: SUM(x) >= 1 AND SUM(x) <= 2"));
	shapes.push_back(
	    S1("bounds with different WHEN conditions", "when flag: SUM(x) >= 1 AND when NOT flag: SUM(x) <= 2"));
	shapes.push_back(S1("a WHEN that may raise", "when CAST(cap AS TINYINT) > 0: SUM(x) <= 2"));
	shapes.push_back(S1("a WHEN that is not Boolean", "SUM(x) <= 2", [](DirectIR &ir) {
		ir.constraints[0].scope.when = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	}));
	// ProveFixedVariables.
	shapes.push_back(
	    S1("a fixed value with a coefficient that may raise", "SUM(x) <= 2 AND CAST(cap AS TINYINT) * x <= 1"));
	shapes.push_back(
	    S1("a fixed value with an infinite coefficient", "SUM(x) <= 2 AND CAST('inf' AS DOUBLE) * x <= 1"));
	shapes.push_back(S1("a not-equal value with a column coefficient", "SUM(x) <= 2 AND cap * x <> 1"));
	shapes.push_back(S1("a not-equal value with a coefficient other than 1", "SUM(x) <= 2 AND 2 * x <> 2"));
	shapes.push_back(S1("a fixed value with no coefficient", "SUM(x) <= 2 AND x = 1",
	                    [](DirectIR &ir) { LeftPart(ir, 1).terms[0].coefficient.reset(); }));
	shapes.push_back(S1("a fixed value under a WHEN that reads more than the source",
	                    "SUM(x) <= 2 AND when txid_current() >= 0: x = 1"));
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
	shapes.push_back(
	    S1("a count bound that is a cast to a type that is not a number", "SUM(x) <= cap", [](DirectIR &ir) {
		    // `cap` varies in its group, so the plan reads the limit its group bound gives.
		    auto &bound = ir.constraints[0];
		    (bound.group_bound ? *bound.group_bound->upper : *bound.rhs).return_type = LogicalType::BOOLEAN;
	    }));
	// ProveObjective.
	shapes.push_back(S1("an average objective", "SUM(x) <= 2", nullptr, "MAXIMIZE AVG(w*x)"));
	shapes.push_back(S1("an objective WHEN that reads more than the source", "SUM(x) <= 2", nullptr,
	                    "MAXIMIZE when txid_current() >= 0: SUM(w*x)"));
	shapes.push_back(S1("an objective part WHEN that reads more than the source", "SUM(x) <= 2", nullptr,
	                    "MAXIMIZE SUM(when txid_current() >= 0: w*x)"));
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
	shapes.push_back(S1("an objective coefficient fixed only within the query", "SUM(x) <= 2", nullptr,
	                    "MAXIMIZE SUM(txid_current()*x)"));
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
	shapes.push_back(A1("an equality and a bound under a WHEN", "x = cap AND when flag: x >= -3"));
	shapes.push_back(A1("a Boolean decision", "x <= flag", nullptr, A1_OBJECTIVE, "x(BOOL)"));
	shapes.push_back(A1("a square with a constant", "x <= cap", nullptr, "MINIMIZE SUM(POWER(x - w, 2) + 5)"));
	shapes.push_back(
	    A1("an objective part with a WHEN", "x <= cap", nullptr, "MINIMIZE when flag: SUM(POWER(w*x - 3, 2))"));
	shapes.push_back(A1("no objective", "x <= cap", nullptr, ""));
	shapes.push_back(A1("an objective constant that reads a column", "x <= 5", nullptr, "MAXIMIZE SUM(2*x + w)"));
	shapes.push_back(
	    A1("a whole-numbered decision with strict folded bounds", "x > 0 AND x < 5", nullptr, A1_OBJECTIVE, "x(INT)"));
	shapes.push_back(
	    A1("an objective part with a filter", "x <= cap", [](DirectIR &ir) { ir.objective.parts[0].filter = True(); }));
	shapes.push_back(A1(
	    "a square over two terms of one decision", "x <= cap",
	    [](DirectIR &ir) {
		    auto &inner = SquareForm(ir.objective.parts[0].terms[0]);
		    inner.push_back(CopyTerm(inner[0]));
	    },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	return shapes;
}

duckdb::vector<Shape> A1DeclineShapes() {
	duckdb::vector<Shape> shapes;
	// CheckCondition: the decisions.
	shapes.push_back(A1("no decision", "x <= cap", [](DirectIR &ir) { ir.decisions.clear(); }));
	shapes.push_back(A1("a keyed scope", "x <= cap", [](DirectIR &ir) { ir.keyed_scopes.emplace_back(); }));
	shapes.push_back(A1("a query-wide decision", "s <= 3", nullptr, "MAXIMIZE s", "per (): s(REAL)"));
	// CheckCondition: the constraints.
	shapes.push_back(A1("an aggregate constraint", "SUM(x) <= 5"));
	shapes.push_back(A1("a membership constraint", "x IN (1, 2)"));
	shapes.push_back(A1("a not-equal constraint", "x <> 2", nullptr, A1_OBJECTIVE, "x(INT)"));
	// DirectIsSingletonRow.
	shapes.push_back(A1("a product of two decisions", "x * y <= 4", nullptr, A1_OBJECTIVE, "x(REAL), y(REAL)"));
	shapes.push_back(A1("a left part over two decisions", "x + y <= 3", nullptr, A1_OBJECTIVE, "x(REAL), y(REAL)"));
	shapes.push_back(A1("a bound on a decision that is not declared", "x <= cap",
	                    [](DirectIR &ir) { ir.constraints[0].bound.var_idx = 1; }));
	// CheckCondition: the objective.
	shapes.push_back(A1("an objective with a by key", "x <= cap", nullptr, "MAXIMIZE SUM(w*x) by (flag)"));
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
	shapes.push_back(A1("a bound under a WHEN that may raise", "when CAST(cap AS TINYINT) > 0: x <= cap"));
	shapes.push_back(A1("a bound under a WHEN that is not Boolean", "x <= cap", [](DirectIR &ir) {
		ir.constraints[0].scope.when = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	}));
	shapes.push_back(A1("a bound with a coefficient that is a string", "x <= cap", [](DirectIR &ir) {
		LeftPart(ir).terms[0].coefficient = make_uniq<BoundConstantExpression>(Value("a"));
	}));
	shapes.push_back(A1("a bound with a coefficient that reads a column outside the source", "x <= cap",
	                    [](DirectIR &ir) { LeftPart(ir).terms[0].coefficient = ForeignColumn(LogicalType::DOUBLE); }));
	shapes.push_back(A1("a Boolean bound below the domain", "x <= -1", nullptr, A1_OBJECTIVE, "x(BOOL)"));
	// ProveObjective.
	shapes.push_back(A1("an objective WHEN that reads more than the source", "x <= cap", nullptr,
	                    "MAXIMIZE when txid_current() >= 0: SUM(w*x)"));
	shapes.push_back(A1("an objective coefficient fixed only within the query", "x <= cap", nullptr,
	                    "MAXIMIZE SUM(txid_current()*x)"));
	shapes.push_back(A1("an objective divided by zero", "x <= cap", nullptr, "MAXIMIZE SUM(w*x) / 0"));
	shapes.push_back(A1("an objective scaled by infinity", "x <= cap", nullptr, "MAXIMIZE 'inf'::DOUBLE * SUM(w*x)"));
	shapes.push_back(A1("an objective term over a decision that is not declared", "x <= cap",
	                    [](DirectIR &ir) { ir.objective.parts[0].terms[0].var_a = 5; }));
	shapes.push_back(A1("an objective part with a filter that reads more than the source", "x <= cap",
	                    [](DirectIR &ir) { ir.objective.parts[0].filter = ForeignBoolean(); }));
	shapes.push_back(A1("an objective constant fixed only within the query", "x <= cap", nullptr,
	                    "MAXIMIZE SUM(w*x + txid_current())"));
	shapes.push_back(A1("an objective term with no coefficient", "x <= cap",
	                    [](DirectIR &ir) { ir.objective.parts[0].terms[0].coefficient.reset(); }));
	shapes.push_back(A1(
	    "an objective constant with no value", "x <= cap",
	    [](DirectIR &ir) {
		    for (auto &term : ir.objective.parts[0].terms) {
			    if (term.IsConstant()) {
				    term.coefficient.reset();
			    }
		    }
	    },
	    "MAXIMIZE SUM(w*x + 1)"));
	shapes.push_back(A1(
	    "a square scaled by zero", "x <= cap",
	    [](DirectIR &ir) {
		    ir.objective.parts[0].terms[0].coefficient = make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
	    },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(A1(
	    "a square scaled by a number that is not finite", "x <= cap",
	    [](DirectIR &ir) {
		    ir.objective.parts[0].terms[0].coefficient =
		        make_uniq<BoundConstantExpression>(Value::DOUBLE(std::numeric_limits<double>::quiet_NaN()));
	    },
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
	    [](DirectIR &ir) { SquareForm(ir.objective.parts[0].terms[0])[0].var_a = 5; },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(
	    A1("a square over two decisions", "x <= cap", nullptr, "MINIMIZE SUM(POWER(x + y, 2))", "x(REAL), y(REAL)"));
	shapes.push_back(A1(
	    "a square over a term that is neither linear nor constant", "x <= cap",
	    [](DirectIR &ir) {
		    SquareForm(ir.objective.parts[0].terms[0])[1].opaque =
		        make_uniq<BoundConstantExpression>(Value::INTEGER(0));
	    },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(A1(
	    "a square over no decision", "x <= cap",
	    [](DirectIR &ir) {
		    auto &inner = SquareForm(ir.objective.parts[0].terms[0]);
		    inner.erase(inner.begin());
	    },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(A1("a square with a constant fixed only within the query", "x <= cap", nullptr,
	                    "MINIMIZE SUM(POWER(x - txid_current(), 2))"));
	shapes.push_back(A1(
	    "a square term with no coefficient", "x <= cap",
	    [](DirectIR &ir) { SquareForm(ir.objective.parts[0].terms[0])[1].coefficient.reset(); },
	    "MINIMIZE SUM(POWER(x - w, 2))"));
	return shapes;
}

//===--------------------------------------------------------------------===//
// R1
//===--------------------------------------------------------------------===//

const char *R1_OBJECTIVE = "MAXIMIZE SUM(w*x)";

//! The budget is the second constraint: `x <= cap` comes first, so an edit of constraint 0 reaches a bound and an edit
//! of constraint 1 reaches the budget.
Shape R1(const string &branch, const string &constraints, Edit edit = nullptr, const string &objective = R1_OBJECTIVE,
         const string &declaration = "x(REAL)") {
	return {branch, declaration, constraints, objective, std::move(edit)};
}

duckdb::vector<Shape> R1AppliesShapes() {
	duckdb::vector<Shape> shapes;
	shapes.push_back(R1("plain form: a bound, a budget and a linear objective", "x <= cap AND SUM(cap*x) <= 5"));
	shapes.push_back(R1("a budget with no per-row bound", "SUM(cap*x) <= 5"));
	shapes.push_back(R1("an equality and a bound under a WHEN, a folded lower bound",
	                    "when flag: x = cap AND x >= -3 AND when NOT flag: x <= cap AND SUM(cap*x) <= 5"));
	shapes.push_back(R1("no objective", "x <= cap AND SUM(cap*x) <= 5", nullptr, ""));
	shapes.push_back(R1("a minimizing objective", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MINIMIZE SUM(w*x)"));
	shapes.push_back(R1("an objective WHEN", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MAXIMIZE when flag: SUM(w*x)"));
	shapes.push_back(
	    R1("an objective part with a WHEN", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MAXIMIZE SUM(when flag: w*x)"));
	shapes.push_back(R1("a WHEN on the objective and on its part", "x <= cap AND SUM(cap*x) <= 5", nullptr,
	                    "MAXIMIZE when flag: SUM(when cap > 1: w*x)"));
	shapes.push_back(R1("an objective of two scaled parts", "x <= cap AND SUM(cap*x) <= 5", nullptr,
	                    "MAXIMIZE 2 * SUM(w*x) - SUM(f*x) / 4"));
	shapes.push_back(R1("a budget of two terms", "x <= cap AND SUM(cap*x + w*x) <= 5"));
	shapes.push_back(R1("a budget limit that is an expression", "x <= cap AND SUM(cap*x) <= 2 + 3.5::DECIMAL(5,1)"));
	shapes.push_back(R1("a negative budget limit", "x <= cap AND SUM(cap*x) <= -3"));
	// A limit read from the data: a column (an integer one, and a floating point one, which can hold NaN), an
	// expression, an aggregate of the source (a column by the time the rule reads it), on either side, with other
	// limits, under a scope.
	shapes.push_back(R1("a budget limit read from an integer column", "x <= cap AND SUM(w*x) <= cap"));
	shapes.push_back(R1("a budget limit read from a floating point column", "x <= cap AND SUM(w*x) <= f"));
	shapes.push_back(R1("a budget limit that is an expression of columns", "x <= cap AND SUM(w*x) <= cap * 2 + w"));
	shapes.push_back(
	    R1("a budget limit that is an aggregate of the source", "x <= cap AND SUM(w*x) <= 0.5 * SUM(cap)"));
	shapes.push_back(R1("a minimum read from a column", "x <= cap AND SUM(w*x) >= cap"));
	shapes.push_back(R1("an exact fill read from a column", "x <= cap AND SUM(w*x) = cap"));
	shapes.push_back(R1("a range read from the data", "SUM(w*x) >= cap AND SUM(w*x) <= cap + 10"));
	shapes.push_back(R1("a column limit beside a constant one", "SUM(w*x) >= 1 AND SUM(w*x) <= cap"));
	shapes.push_back(R1("a column limit and a constant one on the same side", "SUM(w*x) <= 7 AND SUM(w*x) <= cap"));
	shapes.push_back(R1("a column limit under a WHEN of the sum", "SUM(when flag: w*x) <= cap"));
	shapes.push_back(R1("a column limit on two decisions", "x <= cap AND y <= cap AND SUM(w*x + w*y) <= cap", nullptr,
	                    "MAXIMIZE SUM(w*x + f*y)", "x(REAL), y(REAL)"));
	// The budget's direction: a lower limit, both limits, a range, one sum compared twice.
	shapes.push_back(R1("a budget that is a minimum", "x <= cap AND SUM(cap*x) >= 1"));
	shapes.push_back(R1("a budget that is an exact fill", "x <= cap AND SUM(cap*x) = 3"));
	shapes.push_back(R1("a budget that is a range", "x <= cap AND SUM(cap*x) BETWEEN 1 AND 5"));
	shapes.push_back(R1("a budget that is a range written as two comparisons", "SUM(cap*x) >= 1 AND SUM(cap*x) <= 5"));
	shapes.push_back(R1("a budget compared twice from above, the sum of two terms",
	                    "SUM(cap*x + w*x) <= 5 AND SUM(cap*x + w*x) <= 3"));
	shapes.push_back(R1("a budget whose limits cross", "SUM(cap*x) >= 5 AND SUM(cap*x) <= 3"));
	// The budget's scope: a PER key, a WHEN written on the clause, on the aggregate or on both, and the same scope on
	// every clause of one budget, written either way.
	shapes.push_back(R1("a budget under a WHEN", "x <= cap AND when flag: SUM(cap*x) <= 5"));
	shapes.push_back(R1("a budget with an aggregate WHEN", "x <= cap AND SUM(when flag: cap*x) <= 5"));
	shapes.push_back(R1("a range under a WHEN written on both clauses",
	                    "when flag: SUM(cap*x) >= 1 AND when flag: SUM(cap*x) <= 5"));
	shapes.push_back(R1("a range with the WHEN on the clause of one and the aggregate of the other",
	                    "when flag: SUM(cap*x) >= 1 AND SUM(when flag: cap*x) <= 5"));
	shapes.push_back(R1("a range with the WHEN on the aggregate of both",
	                    "SUM(when flag: cap*x) >= 1 AND SUM(when flag: cap*x) <= 5"));
	shapes.push_back(R1("a WHEN on the clause and on the aggregate", "SUM(when flag: cap*x) <= 5",
	                    [](DirectIR &ir) { ir.constraints[0].scope.when = True(); }));
	// Several decisions in one budget: the pieces of a benefit. Each decision has its own weight, value and bounds.
	shapes.push_back(R1("two decisions in one budget, each with its own bound and its own weight",
	                    "x <= cap AND y <= cap AND SUM(cap*x + w*y) <= 5", nullptr, "MAXIMIZE SUM(w*x + cap*y)",
	                    "x(REAL), y(REAL)"));
	shapes.push_back(R1("three decisions in one budget",
	                    "x <= cap AND y <= cap AND z <= cap AND SUM(w*x + w*y + w*z) <= 5", nullptr,
	                    "MAXIMIZE SUM(cap*x + cap*y + cap*z)", "x(REAL), y(REAL), z(REAL)"));
	shapes.push_back(R1("a decision the budget does not name", "SUM(cap*x) <= 5 AND y <= 1", nullptr, R1_OBJECTIVE,
	                    "x(REAL), y(REAL)"));
	shapes.push_back(R1("a decision the objective does not name", "SUM(cap*x + cap*y) <= 5", nullptr,
	                    "MAXIMIZE SUM(w*x)", "x(REAL), y(REAL)"));
	shapes.push_back(R1("a decision with a bound of its own under a WHEN",
	                    "x <= cap AND when flag: y <= cap AND SUM(cap*x + cap*y) <= 5", nullptr,
	                    "MAXIMIZE SUM(w*x + w*y)", "x(REAL), y(REAL)"));
	return shapes;
}

duckdb::vector<Shape> R1DeclineShapes() {
	duckdb::vector<Shape> shapes;
	// CheckCondition: the decision.
	shapes.push_back(R1("no decision", "x <= cap AND SUM(cap*x) <= 5", [](DirectIR &ir) { ir.decisions.clear(); }));
	// ProveDecision reads every decision, not the first alone.
	shapes.push_back(
	    R1("an INT second decision", "SUM(cap*x + cap*y) <= 5", nullptr, "MAXIMIZE SUM(w*x + w*y)", "x(REAL), y(INT)"));
	shapes.push_back(R1(
	    "a second decision whose output is not DOUBLE", "SUM(cap*x + cap*y) <= 5",
	    [](DirectIR &ir) { ir.decisions[1].output_type = LogicalType::BIGINT; }, "MAXIMIZE SUM(w*x + w*y)",
	    "x(REAL), y(REAL)"));
	shapes.push_back(R1("a query-wide second decision", "SUM(cap*x + cap*s) <= 5", nullptr, "MAXIMIZE SUM(w*x) + s",
	                    "x(REAL), per (): s(REAL)"));
	shapes.push_back(R1("an INT decision", "SUM(cap*x) <= 5", nullptr, R1_OBJECTIVE, "x(INT)"));
	shapes.push_back(R1("a BOOL decision", "SUM(cap*x) <= 5", nullptr, R1_OBJECTIVE, "x(BOOL)"));
	shapes.push_back(R1("a query-wide decision", "SUM(cap*s) <= 5", nullptr, "MAXIMIZE s", "per (): s(REAL)"));
	shapes.push_back(R1("a decision whose output is not DOUBLE", "SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.decisions[0].output_type = LogicalType::BIGINT; }));
	shapes.push_back(R1("a keyed scope", "SUM(cap*x) <= 5", [](DirectIR &ir) { ir.keyed_scopes.emplace_back(); }));
	// CheckCondition: the constraints.
	shapes.push_back(R1("no budget, only a bound", "x <= cap"));
	// Aggregate constraints are one budget only when their sums have the same terms (ProveBudgetClause).
	shapes.push_back(R1("two budgets with different weights", "SUM(cap*x) <= 5 AND SUM(w*x) <= 7"));
	shapes.push_back(R1("two budgets with different numbers of terms", "SUM(cap*x) >= 1 AND SUM(cap*x + w*x) <= 5"));
	shapes.push_back(
	    R1("two budgets with a term of the other sign", "SUM(cap*x + w*x) >= 1 AND SUM(cap*x - w*x) <= 5"));
	// Two decisions share the budget; the clauses of one budget name the same decisions (SameBudgetTerms).
	shapes.push_back(R1("two budgets with the same weights over different decisions",
	                    "SUM(cap*x) >= 1 AND SUM(cap*y) <= 5", nullptr, "MAXIMIZE SUM(w*x + w*y)", "x(REAL), y(REAL)"));
	// A per-row constraint that reads two decisions links them (DirectIsSingletonRow).
	shapes.push_back(R1("a per-row constraint that links two decisions",
	                    "x <= cap AND y <= cap AND x + y <= 4 AND SUM(cap*x + cap*y) <= 5", nullptr,
	                    "MAXIMIZE SUM(w*x + w*y)", "x(REAL), y(REAL)"));
	shapes.push_back(R1("a membership constraint", "x IN (1, 2) AND SUM(cap*x) <= 5"));
	// ProveBudgetScope: every clause of the budget covers the same rows.
	shapes.push_back(R1("a key on one clause of the budget only", "SUM(cap*x) by (name) >= 1 AND SUM(cap*x) <= 5"));
	shapes.push_back(R1("another key on the second clause", "SUM(cap*x) by (name) >= 1 AND SUM(cap*x) by (flag) <= 5"));
	shapes.push_back(
	    R1("a key more on the second clause", "SUM(cap*x) by (name) >= 1 AND SUM(cap*x) by (name, flag) <= 5"));
	shapes.push_back(R1("a WHEN on one clause of the budget only", "when flag: SUM(cap*x) >= 1 AND SUM(cap*x) <= 5"));
	shapes.push_back(
	    R1("another WHEN on the second clause", "when flag: SUM(cap*x) >= 1 AND when NOT flag: SUM(cap*x) <= 5"));
	shapes.push_back(R1("another WHEN on the aggregate of the second clause",
	                    "when flag: SUM(cap*x) >= 1 AND SUM(when cap > 1: cap*x) <= 5"));
	shapes.push_back(R1("a WHEN on the clause and on the aggregate of the second clause",
	                    "when flag: SUM(cap*x) >= 1 AND SUM(when flag: cap*x) <= 5",
	                    [](DirectIR &ir) { ir.constraints[1].scope.when = True(); }));
	shapes.push_back(
	    R1("a second clause of the budget that is not a plain sum", "SUM(cap*x) >= 1 AND 2 * SUM(cap*x) <= 5"));
	shapes.push_back(R1("a WHEN that reads more than the source", "when txid_current() >= 0: SUM(cap*x) <= 5"));
	shapes.push_back(
	    R1("an aggregate WHEN that reads more than the source", "SUM(when txid_current() >= 0: cap*x) <= 5"));
	// ProveBudget.
	shapes.push_back(R1("a strict budget", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.constraints[1].comparison = ExpressionType::COMPARE_LESSTHAN; }));
	shapes.push_back(R1("a scaled budget", "x <= cap AND 2 * SUM(cap*x) <= 5"));
	shapes.push_back(R1("a negated budget", "x <= cap AND 0 - SUM(cap*x) >= -5"));
	shapes.push_back(R1("a budget of two sums", "x <= cap AND SUM(cap*x) + SUM(w*x) <= 5"));
	shapes.push_back(R1("an average budget", "x <= cap AND AVG(cap*x) <= 5"));
	shapes.push_back(R1("a nested budget", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { LeftPart(ir, 1).inner = make_uniq<DirectPart>(); }));
	shapes.push_back(R1("a budget with a constant term", "x <= cap AND SUM(cap*x + 1) <= 5"));
	shapes.push_back(R1("a squared budget", "x <= cap AND SUM(POWER(x, 2)) <= 5"));
	shapes.push_back(R1("a budget with no term", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { LeftPart(ir, 1).terms.clear(); }));
	shapes.push_back(R1("a budget term over another decision", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { LeftPart(ir, 1).terms[0].var_a = 1; }));
	shapes.push_back(R1("a budget term with no coefficient", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { LeftPart(ir, 1).terms[0].coefficient.reset(); }));
	shapes.push_back(
	    R1("a budget with a weight fixed only within the query", "x <= cap AND SUM(txid_current()*x) <= 5"));
	shapes.push_back(R1("a budget with a weight outside the source", "x <= cap AND SUM(cap*x) <= 5", [](DirectIR &ir) {
		LeftPart(ir, 1).terms[0].coefficient = ForeignColumn(LogicalType::DOUBLE);
	}));
	// A limit of the data must be a numeric value of the source row (ProveDataLimit); a constant is read as a number.
	// The bound-side window rewrite reduces a limit that varies by row to a column, so this one has one value already.
	shapes.push_back(
	    R1("a budget limit fixed only within the query", "x <= cap AND SUM(w*x) <= SUM(cap) * txid_current()"));
	shapes.push_back(
	    R1("a budget limit that is an aggregate the rewrite left", "x <= cap AND SUM(w*x) <= 5", [](DirectIR &ir) {
		    ir.constraints[1].rhs = make_uniq<BoundAggregateExpression>(
		        CountStarFun::GetFunction(), duckdb::vector<duckdb::unique_ptr<Expression>>(), nullptr, nullptr,
		        AggregateType::NON_DISTINCT);
	    }));
	shapes.push_back(R1("a budget limit that is infinite", "x <= cap AND SUM(cap*x) <= 'inf'::DOUBLE"));
	shapes.push_back(R1("a budget limit of 1e20", "x <= cap AND SUM(cap*x) <= 1e20"));
	shapes.push_back(R1("a budget limit of minus 1e20", "x <= cap AND SUM(cap*x) <= -1e20"));
	shapes.push_back(R1("a minimum of 1e20", "x <= cap AND SUM(cap*x) >= 1e20"));
	shapes.push_back(R1("an exact fill that is infinite", "x <= cap AND SUM(cap*x) = 'inf'::DOUBLE"));
	// Every comparison of the budget is checked, not only the first.
	shapes.push_back(
	    R1("a second comparison of the budget with a limit of 1e20", "SUM(cap*x) >= 1 AND SUM(cap*x) <= 1e20"));
	// ProveBounds.
	shapes.push_back(R1("a bound whose value is a subquery", "x <= cap AND SUM(cap*x) <= 5", [](DirectIR &ir) {
		ir.constraints[0].rhs = make_uniq<BoundSubqueryExpression>(LogicalType::DOUBLE);
	}));
	shapes.push_back(R1("a bound below the built-in domain", "x <= -1 AND SUM(cap*x) <= 5"));
	// CheckCondition and ProveObjective: the objective.
	shapes.push_back(
	    R1("an objective with a by key", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MAXIMIZE SUM(w*x) by (flag)"));
	shapes.push_back(R1("a budget key that reads a decision", "SUM(cap*x) by (name) <= 5", [](DirectIR &ir) {
		LeftPart(ir).by[0].grouping =
		    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ColumnBinding(ir.decide_index, 0));
	}));
	shapes.push_back(R1("an objective WHEN that reads more than the source", "x <= cap AND SUM(cap*x) <= 5", nullptr,
	                    "MAXIMIZE when txid_current() >= 0: SUM(w*x)"));
	shapes.push_back(R1("an objective part WHEN that reads more than the source", "x <= cap AND SUM(cap*x) <= 5",
	                    nullptr, "MAXIMIZE SUM(when txid_current() >= 0: w*x)"));
	shapes.push_back(R1("an average objective", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MAXIMIZE AVG(w*x)"));
	shapes.push_back(R1("a nested objective part", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.objective.parts[0].inner = make_uniq<DirectPart>(); }));
	shapes.push_back(R1("a qualified objective part", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.objective.parts[0].qualifier = 0; }));
	// A term that multiplies two decisions is not a value per decision (OnlyLinearTerms).
	shapes.push_back(R1("an objective product of two decisions", "x <= cap AND y <= cap AND SUM(cap*x + cap*y) <= 5",
	                    nullptr, "MAXIMIZE SUM(x*y)", "x(REAL), y(REAL)"));
	shapes.push_back(R1("an objective constant", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MAXIMIZE SUM(w*x + 1)"));
	shapes.push_back(
	    R1("an objective square", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(R1("an objective term over another decision", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.objective.parts[0].terms[0].var_a = 1; }));
	shapes.push_back(R1("an objective term with no coefficient", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.objective.parts[0].terms[0].coefficient.reset(); }));
	shapes.push_back(
	    R1("an objective divided by zero", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MAXIMIZE SUM(w*x) / 0"));
	shapes.push_back(R1("an objective coefficient fixed only within the query", "x <= cap AND SUM(cap*x) <= 5", nullptr,
	                    "MAXIMIZE SUM(txid_current()*x)"));
	return shapes;
}

//! The shapes that carry a `by` key, which a rule that reads the key applies to.
duckdb::vector<Shape> S1KeyedShapes() {
	duckdb::vector<Shape> shapes;
	shapes.push_back(S1("counts per a column", "SUM(x) by (flag) <= 2"));
	shapes.push_back(S1("a count per class of a column", "per flag: SUM(x) <= cap"));
	shapes.push_back(S1("counts per two columns", "SUM(x) by (name, flag) <= 1"));
	// A key is one key in whichever order its elements are written.
	shapes.push_back(S1("bounds whose keys list the same columns in another order",
	                    "SUM(x) by (flag, name) >= 1 AND SUM(x) by (name, flag) <= 2"));
	shapes.push_back(S1("counts per an expression", "SUM(x) by (cap + 1) <= 2"));
	// A relation's columns compare NULL as a value, so its rows are all in a group.
	shapes.push_back(S1("counts per a relation", "SUM(x) by (t) <= 1"));
	shapes.push_back(S1("counts per a column under a WHEN", "when cap > 1: SUM(x) by (flag) <= 2"));
	shapes.push_back(S1("a range per a column", "SUM(x) by (flag) >= 1 AND SUM(x) by (flag) <= 2"));
	shapes.push_back(S1("a source-valued bound per a column", "SUM(x) by (flag) <= cap"));
	shapes.push_back(S1("a fixed value beside a count per a column", "SUM(x) by (flag) <= 2 AND x >= flag"));
	return shapes;
}

duckdb::vector<Shape> R1KeyedShapes() {
	duckdb::vector<Shape> shapes;
	shapes.push_back(R1("a budget per a column", "SUM(cap*x) by (name) <= 5"));
	shapes.push_back(R1("a budget per class of a column", "x <= cap AND per flag: SUM(cap*x) <= 5"));
	shapes.push_back(R1("a budget per two columns", "SUM(cap*x) by (name, flag) <= 5"));
	shapes.push_back(R1("a budget per an expression", "SUM(cap*x) by (cap + 1) <= 5"));
	shapes.push_back(R1("a budget per a relation", "SUM(cap*x) by (t) <= 5"));
	shapes.push_back(R1("a budget per a column under a WHEN", "when flag: SUM(cap*x) by (name) <= 5"));
	shapes.push_back(R1("a column limit per a column and under a WHEN", "when flag: SUM(w*x) by (name) <= cap"));
	shapes.push_back(
	    R1("a range with the same key on both clauses", "SUM(cap*x) by (name) >= 1 AND SUM(cap*x) by (name) <= 5"));
	shapes.push_back(R1("a range whose keys list the same columns in another order",
	                    "SUM(cap*x) by (name, flag) >= 1 AND SUM(cap*x) by (flag, name) <= 5"));
	shapes.push_back(R1("two decisions in a budget per a column", "SUM(cap*x + cap*y) by (name) <= 5", nullptr,
	                    "MAXIMIZE SUM(w*x + w*y)", "x(REAL), y(REAL)"));
	return shapes;
}

TEST_CASE("S1 applies to the forms of its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectApplies(MakeS1UniformMatroidRule, con, S1AppliesShapes());
	ExpectApplies(MakeS1UniformMatroidRule, con, S1KeyedShapes());
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

TEST_CASE("R1 applies to the forms of its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectApplies(MakeR1ContinuousKnapsackRule, con, R1AppliesShapes());
	ExpectApplies(MakeR1ContinuousKnapsackRule, con, R1KeyedShapes());
}

TEST_CASE("R1 declines each shape that is not exactly its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectDeclines(MakeR1ContinuousKnapsackRule, con, R1DeclineShapes());
}

//===--------------------------------------------------------------------===//
// N1
//===--------------------------------------------------------------------===//

const char *N1_LIMIT = "SUM(POWER(x, 2)) <= 25";
const char *N1_OBJECTIVE = "MAXIMIZE SUM(w*x)";

Shape N1(const string &branch, const string &constraints, Edit edit = nullptr, const string &objective = N1_OBJECTIVE,
         const string &declaration = "x(REAL)") {
	return {branch, declaration, constraints, objective, std::move(edit)};
}

//! A second square, which the front end refuses to build from SQL: `x^2` on the IR's first decision.
void AddSquare(duckdb::vector<DecideSplitTerm> &terms) {
	DecideSplitTerm inner;
	inner.degree = 1;
	inner.var_a = 0;
	inner.coefficient = make_uniq<BoundConstantExpression>(Value::DOUBLE(1.0));
	DecideSplitTerm square;
	square.degree = 2;
	square.factors.resize(1);
	square.factors[0].power = 2;
	square.factors[0].form.push_back(std::move(inner));
	terms.push_back(std::move(square));
}

//! A shape whose limit is the plain one and whose objective is the one the case is about.
Shape N1Objective(const string &branch, const string &objective, Edit edit = nullptr) {
	return N1(branch, N1_LIMIT, std::move(edit), objective);
}

duckdb::vector<Shape> N1AppliesShapes() {
	duckdb::vector<Shape> shapes;
	// Every spelling of the limit reads as one square.
	shapes.push_back(N1("the plain form", N1_LIMIT));
	shapes.push_back(N1("a product of the decision with itself", "SUM(x * x) <= 25"));
	shapes.push_back(N1("a power written with stars", "SUM(x ** 2) <= 25"));
	shapes.push_back(N1("a norm", "norm(x, 2) <= 25"));
	shapes.push_back(N1("a factor on the square", "SUM(2 * POWER(x, 2)) <= 50"));
	shapes.push_back(N1("a slope in the square", "SUM(POWER(2 * x, 2)) <= 100"));
	// The box.
	shapes.push_back(N1("the default box written out", "x >= 0 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a free domain", "x >= -1e30 AND SUM(POWER(x, 2)) <= 25"));
	// The limit.
	shapes.push_back(N1("a negative limit", "SUM(POWER(x, 2)) <= -1"));
	shapes.push_back(
	    N1("a limit over a small factor that passes a DOUBLE as B / q", "SUM(1e-300 * POWER(x, 2)) <= 1e19"));
	shapes.push_back(N1("a column limit", "SUM(POWER(x, 2)) <= cap"));
	shapes.push_back(N1("a column limit per a column", "SUM(POWER(x, 2)) by (name) <= cap"));
	shapes.push_back(N1("a limit left as an aggregate of the rows", "SUM(POWER(x, 2)) <= 0.5 * SUM(w)"));
	shapes.push_back(N1("a limit left as an aggregate of the group", "SUM(POWER(x, 2)) by (name) <= SUM(w) by (name)"));
	// Scope.
	shapes.push_back(N1("a key", "SUM(POWER(x, 2)) by (name) <= 25"));
	shapes.push_back(N1("a class", "per flag: SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a class and a key", "per flag: SUM(POWER(x, 2)) by (flag) <= 25"));
	shapes.push_back(N1("a WHEN on the clause", "when flag: SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a WHEN inside the sum", "SUM(when flag: POWER(x, 2)) <= 25"));
	// The objective.
	shapes.push_back(N1Objective("no objective", ""));
	shapes.push_back(N1Objective("a minimum", "MINIMIZE SUM(w*x)"));
	shapes.push_back(N1Objective("a constant term", "MAXIMIZE SUM(w*x + 5)"));
	shapes.push_back(N1Objective("several parts", "MAXIMIZE SUM(w*x) - SUM(f*x)"));
	shapes.push_back(N1Objective("a square", "MINIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(N1Objective("a square with a slope", "MINIMIZE SUM(POWER(2 * x - w, 2))"));
	shapes.push_back(N1Objective("a negated square, maximized", "MAXIMIZE -SUM(POWER(x - w, 2))"));
	shapes.push_back(N1Objective("a square and a linear term", "MINIMIZE SUM(POWER(x - w, 2) + f*x)"));
	shapes.push_back(
	    N1Objective("a square of a slope alone, and a linear part", "MINIMIZE SUM(POWER(x, 2)) + SUM(w*x)"));
	shapes.push_back(N1Objective("a square under a WHEN, with no linear term elsewhere",
	                             "MINIMIZE SUM(when flag: POWER(x - w, 2))"));
	shapes.push_back(N1Objective("a square under a WHEN, with its linear term in its part",
	                             "MINIMIZE SUM(when flag: POWER(x - w, 2) + f*x)"));
	return shapes;
}

duckdb::vector<Shape> N1DeclineShapes() {
	duckdb::vector<Shape> shapes;
	// CheckCondition: the decision.
	shapes.push_back(N1("no decision", N1_LIMIT, [](DirectIR &ir) { ir.decisions.clear(); }));
	shapes.push_back(
	    N1("two decisions", "SUM(POWER(x, 2)) <= 25 AND y <= 1", nullptr, N1_OBJECTIVE, "x(REAL), y(REAL)"));
	shapes.push_back(N1("an INT decision", N1_LIMIT, nullptr, N1_OBJECTIVE, "x(INT)"));
	shapes.push_back(N1("a BOOL decision", N1_LIMIT, nullptr, N1_OBJECTIVE, "x(BOOL)"));
	shapes.push_back(
	    N1("a query-wide decision", N1_LIMIT, [](DirectIR &ir) { ir.decisions[0].scope = DecideVarScope::GLOBAL; }));
	shapes.push_back(N1("a keyed scope", N1_LIMIT, [](DirectIR &ir) { ir.keyed_scopes.emplace_back(); }));
	// ProveLimitClause: the one aggregate clause.
	shapes.push_back(N1("no aggregate clause", "x >= 0"));
	shapes.push_back(N1("a per-row square", "POWER(x, 2) <= 9"));
	shapes.push_back(N1("a per-row clause that is not one row's bound", "x IN (1, 2) AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("two limit clauses", "SUM(POWER(x, 2)) <= 25 AND SUM(POWER(x, 2)) <= 9"));
	shapes.push_back(N1("a limit from below as well", "SUM(POWER(x, 2)) >= 1 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a BETWEEN limit", "SUM(POWER(x, 2)) BETWEEN 1 AND 25"));
	shapes.push_back(N1("an equal limit", "SUM(POWER(x, 2)) = 25"));
	shapes.push_back(N1("a limit from below", "SUM(POWER(x, 2)) >= 25"));
	shapes.push_back(N1("a strict limit", N1_LIMIT,
	                    [](DirectIR &ir) { ir.constraints[0].comparison = ExpressionType::COMPARE_LESSTHAN; }));
	shapes.push_back(N1("a scaled sum", "2 * SUM(POWER(x, 2)) <= 50"));
	shapes.push_back(N1("a negated sum", "-SUM(POWER(x, 2)) >= -25"));
	shapes.push_back(N1("an average", "AVG(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a WHEN that reads more than the source", "when txid_current() >= 0: SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a linear limit", N1_LIMIT, [](DirectIR &ir) {
		auto &term = LeftPart(ir).terms[0];
		term = CopyTerm(SquareForm(term)[0]);
	}));
	shapes.push_back(N1("a square and a linear term in the limit", "SUM(POWER(x, 2) + w * x) <= 25"));
	shapes.push_back(N1("a square and a constant term in the limit", "SUM(POWER(x, 2) + 1) <= 25"));
	shapes.push_back(N1("a ball off center by a constant", "SUM(POWER(x - 1, 2)) <= 25"));
	shapes.push_back(N1("a ball off center by a column", "SUM(POWER(x - w, 2)) <= 25"));
	shapes.push_back(N1("a row-valued slope in the limit", "SUM(POWER(w * x, 2)) <= 25"));
	shapes.push_back(N1("a row-valued weight on the square in the limit", "SUM(w * POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a slope in the limit fixed only within the query", "SUM(POWER(txid_current() * x, 2)) <= 25"));
	// The read of the square stops at this term, with the slope already read: only the read's own answer declines it.
	shapes.push_back(
	    N1("a center in the limit fixed only within the query", "SUM(POWER(x + txid_current(), 2)) <= 25"));
	shapes.push_back(N1("a square of slope zero in the limit", N1_LIMIT, [](DirectIR &ir) {
		SquareForm(LeftPart(ir).terms[0])[0].coefficient = make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
	}));
	shapes.push_back(N1("a square past a DOUBLE", "SUM(POWER(1e200 * x, 2)) <= 25"));
	shapes.push_back(N1("a negative square", "SUM(-POWER(x, 2)) <= 25"));
	// ProveLimitValue.
	shapes.push_back(N1("a limit of 1e20", "SUM(POWER(x, 2)) <= 1e20"));
	shapes.push_back(N1("a limit of minus 1e20", "SUM(POWER(x, 2)) <= -1e20"));
	shapes.push_back(N1("an infinite limit", "SUM(POWER(x, 2)) <= 'inf'::DOUBLE"));
	shapes.push_back(N1("a limit fixed only within the query", "SUM(POWER(x, 2)) <= txid_current()"));
	shapes.push_back(
	    N1("a limit left as an aggregate that is not a number", "SUM(POWER(x, 2)) <= 0.5 * SUM(w)",
	       [](DirectIR &ir) { ir.constraints[0].group_bound->upper->return_type = LogicalType::BOOLEAN; }));
	// ProveBox.
	shapes.push_back(N1("an upper bound", "x <= 2 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("an upper bound that would not bind", "x <= 50 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a large upper bound", "x <= 1e19 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a column upper bound", "x <= cap AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a lower bound below zero", "x >= -2 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a lower bound above zero", "x >= 1 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a large lower bound", "x >= -1e19 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(N1("a bound under a WHEN", "when flag: x >= 0 AND SUM(POWER(x, 2)) <= 25"));
	shapes.push_back(
	    N1("a bound that DirectReadBounds cannot read", "x <= 2 AND SUM(POWER(x, 2)) <= 25",
	       [](DirectIR &ir) { ir.constraints[0].rhs = make_uniq<BoundSubqueryExpression>(LogicalType::DOUBLE); }));
	// ProveObjective: the part.
	shapes.push_back(N1Objective("an average objective", "MAXIMIZE AVG(w*x)"));
	shapes.push_back(N1Objective("an objective with a by key", "MAXIMIZE SUM(w*x) by (flag)"));
	shapes.push_back(N1Objective("a nested objective part", N1_OBJECTIVE,
	                             [](DirectIR &ir) { ir.objective.parts[0].inner = make_uniq<DirectPart>(); }));
	shapes.push_back(N1Objective("a qualified objective part", N1_OBJECTIVE,
	                             [](DirectIR &ir) { ir.objective.parts[0].qualifier = 0; }));
	shapes.push_back(
	    N1Objective("an objective coefficient fixed only within the query", "MAXIMIZE SUM(txid_current()*x)"));
	shapes.push_back(N1Objective("an objective divided by zero", "MAXIMIZE SUM(w*x) / 0"));
	shapes.push_back(N1Objective("an objective WHEN that reads more than the source",
	                             "MAXIMIZE when txid_current() >= 0: SUM(w*x)"));
	// ProveObjective: the terms.
	shapes.push_back(
	    N1Objective("an objective constant fixed only within the query", "MAXIMIZE SUM(w*x + txid_current())"));
	shapes.push_back(
	    N1Objective("a term that is neither linear, constant nor a square", N1_OBJECTIVE, [](DirectIR &ir) {
		    auto &term = ir.objective.parts[0].terms[0];
		    DecideSplitFactor factor;
		    factor.form.push_back(CopyTerm(term));
		    term.degree = 2;
		    term.factors.push_back(std::move(factor));
		    term.factors.push_back(DecideSplitFactor {});
		    term.factors[1].form.push_back(CopyTerm(term.factors[0].form[0]));
	    }));
	shapes.push_back(N1Objective("two squares in one part", "MINIMIZE SUM(POWER(x - w, 2))",
	                             [](DirectIR &ir) { AddSquare(ir.objective.parts[0].terms); }));
	shapes.push_back(N1Objective("two squares in two parts", "MINIMIZE SUM(POWER(x, 2)) + SUM(w*x)",
	                             [](DirectIR &ir) { AddSquare(ir.objective.parts[1].terms); }));
	shapes.push_back(N1Objective("a row-valued slope in the objective", "MINIMIZE SUM(POWER(w * x - 1, 2))"));
	shapes.push_back(N1Objective("a slope in the objective fixed only within the query",
	                             "MINIMIZE SUM(POWER(txid_current() * x, 2))"));
	shapes.push_back(N1Objective("a center in the objective fixed only within the query",
	                             "MINIMIZE SUM(POWER(x - txid_current(), 2))"));
	shapes.push_back(
	    N1Objective("a square of slope zero in the objective", "MINIMIZE SUM(POWER(x, 2))", [](DirectIR &ir) {
		    SquareForm(ir.objective.parts[0].terms[0])[0].coefficient =
		        make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
	    }));
	shapes.push_back(N1Objective("a square past a DOUBLE", "MINIMIZE SUM(POWER(1e200 * x, 2))"));
	shapes.push_back(
	    N1Objective("a square past a DOUBLE with its part's factor", "MINIMIZE 1e308 * SUM(1e10 * POWER(x - w, 2))"));
	shapes.push_back(N1Objective("a maximized convex square", "MAXIMIZE SUM(POWER(x - w, 2))"));
	shapes.push_back(N1Objective("a minimized concave square", "MINIMIZE SUM(-POWER(x - w, 2))"));
	shapes.push_back(N1Objective("a minimized negated part", "MINIMIZE -SUM(POWER(x - w, 2))"));
	// ProveObjective: K is one constant on every row with a linear term.
	shapes.push_back(N1Objective("a square under a part WHEN with a linear term elsewhere",
	                             "MINIMIZE SUM(when flag: POWER(x, 2)) + SUM(f*x)"));
	shapes.push_back(N1Objective("a square under a NULL guard with a linear term elsewhere",
	                             "MINIMIZE SUM(POWER(x - w, 2)) + SUM(f*x)"));
	return shapes;
}

TEST_CASE("N1 applies to the forms of its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectApplies(MakeN1EuclideanBallRule, con, N1AppliesShapes());
}

TEST_CASE("N1 declines each shape that is not exactly its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectDeclines(MakeN1EuclideanBallRule, con, N1DeclineShapes());
}

// The rules partition the problems they apply to: no two rules apply to one IR.
TEST_CASE("N1 and every other rule never apply to the same query", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectDeclines(MakeS1UniformMatroidRule, con, N1AppliesShapes());
	ExpectDeclines(MakeA1SeparableBoxRule, con, N1AppliesShapes());
	ExpectDeclines(MakeR1ContinuousKnapsackRule, con, N1AppliesShapes());
	ExpectDeclines(MakeN1EuclideanBallRule, con, S1AppliesShapes());
	ExpectDeclines(MakeN1EuclideanBallRule, con, A1AppliesShapes());
	ExpectDeclines(MakeN1EuclideanBallRule, con, R1AppliesShapes());
}

//===--------------------------------------------------------------------===//
// A2
//===--------------------------------------------------------------------===//

const char *A2_OBJECTIVE = "MINIMIZE SUM(POWER(c - w, 2))";

Shape A2(const string &branch, const string &constraints, Edit edit = nullptr, const string &objective = A2_OBJECTIVE,
         const string &declaration = "per (): c(REAL)") {
	return {branch, declaration, constraints, objective, std::move(edit)};
}

duckdb::vector<Shape> A2AppliesShapes() {
	duckdb::vector<Shape> shapes;
	shapes.push_back(
	    A2("plain form: constant bounds and the square of the decision minus a column", "c BETWEEN 0 AND 10"));
	shapes.push_back(A2("one bound, the other the default", "c <= 10"));
	shapes.push_back(A2("an interval with an open lower end", "c >= -1e30 AND c <= 4"));
	shapes.push_back(A2("an empty interval", "c >= 5 AND c <= 3"));
	shapes.push_back(
	    A2("the column minus the decision", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(POWER(w - c, 2))"));
	shapes.push_back(
	    A2("a factor on the square", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE 3 * SUM(POWER(c - w, 2)) / 2"));
	shapes.push_back(
	    A2("a maximized negative square", "c BETWEEN 0 AND 10", nullptr, "MAXIMIZE -SUM(POWER(c - w, 2))"));
	shapes.push_back(
	    A2("an objective WHEN", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE when flag: SUM(POWER(c - w, 2))"));
	shapes.push_back(
	    A2("a WHEN on the sum", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(when flag: POWER(c - w, 2))"));
	shapes.push_back(A2("an objective constant", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(POWER(c - w, 2)) + 5"));
	shapes.push_back(A2("a slope other than one", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(POWER(2 * c - w, 2))"));
	// Bounds read from the data, or under a WHEN, are clauses the plan intersects per class.
	shapes.push_back(A2("a bound read from the data", "c <= cap"));
	shapes.push_back(A2("a lower bound read from the data", "c >= w - 4"));
	shapes.push_back(A2("a bound under a WHEN", "when flag: c BETWEEN 1 AND w"));
	shapes.push_back(A2("a floor lowered under a WHEN", "when flag: c >= -4"));
	// A keyed decision: a column element has no class on a NULL, and so does each column of a key of two.
	shapes.push_back(
	    A2("a decision keyed by a column", "c BETWEEN 0 AND 10", nullptr, A2_OBJECTIVE, "per name: c(REAL)"));
	shapes.push_back(
	    A2("a decision keyed by two columns", "c BETWEEN 0 AND 10", nullptr, A2_OBJECTIVE, "per name, flag: c(REAL)"));
	// A relation element takes a NULL as a value of its own.
	shapes.push_back(
	    A2("a decision keyed by a relation", "c BETWEEN 0 AND 10", nullptr, A2_OBJECTIVE, "per t: c(REAL)"));
	return shapes;
}

duckdb::vector<Shape> A2DeclineShapes() {
	duckdb::vector<Shape> shapes;
	// CheckCondition: the decision.
	shapes.push_back(A2("two decisions", "c <= 5 AND d <= 5", nullptr, A2_OBJECTIVE, "per (): c(REAL), d(REAL)"));
	shapes.push_back(A2("a whole-numbered decision", "c BETWEEN 0 AND 10", nullptr, A2_OBJECTIVE, "per (): c(INT)"));
	shapes.push_back(A2("a decision of each row", "c BETWEEN 0 AND 10", nullptr, A2_OBJECTIVE, "c(REAL)"));
	// ProveInterval.
	shapes.push_back(A2("a constraint that is not a bound", "c BETWEEN 0 AND 10",
	                    [](DirectIR &ir) { ir.constraints[0].singleton_row = false; }));
	shapes.push_back(A2("a bound below the built-in domain", "c <= -1"));
	// ProveObjective.
	shapes.push_back(A2("no objective", "c BETWEEN 0 AND 10", nullptr, ""));
	shapes.push_back(A2("an average objective", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE AVG(POWER(c - w, 2))"));
	shapes.push_back(A2("a square fixed only within the query", "c BETWEEN 0 AND 10", nullptr,
	                    "MINIMIZE SUM(POWER(c - w * txid_current(), 2))"));
	shapes.push_back(A2("a linear objective", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(w * c)"));
	shapes.push_back(
	    A2("a second square", "c BETWEEN 0 AND 10", [](DirectIR &ir) { AddSquare(ir.objective.parts[0].terms); }));
	shapes.push_back(
	    A2("a linear term beside the square", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(POWER(c - w, 2) + w * c)"));
	shapes.push_back(A2("a per-row constant beside the square", "c BETWEEN 0 AND 10", nullptr,
	                    "MINIMIZE SUM(POWER(c - w, 2) + cap)"));
	shapes.push_back(
	    A2("a square weighted by a column", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(w * POWER(c - f, 2))"));
	shapes.push_back(
	    A2("a slope read from the data", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(POWER(w * c - f, 2))"));
	shapes.push_back(A2("a square of slope zero", "c BETWEEN 0 AND 10", [](DirectIR &ir) {
		SquareForm(ir.objective.parts[0].terms[0])[0].coefficient =
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
	}));
	shapes.push_back(
	    A2("a square past a DOUBLE", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE SUM(POWER(1e200 * c - w, 2))"));
	shapes.push_back(A2("a minimized negated part", "c BETWEEN 0 AND 10", nullptr, "MINIMIZE -SUM(POWER(c - w, 2))"));
	shapes.push_back(A2("a maximized convex square", "c BETWEEN 0 AND 10", nullptr, "MAXIMIZE SUM(POWER(c - w, 2))"));
	return shapes;
}

TEST_CASE("A2 applies to the forms of its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectApplies(MakeA2SingleFacilityLocationRule, con, A2AppliesShapes());
}

TEST_CASE("A2 declines each shape that is not exactly its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectDeclines(MakeA2SingleFacilityLocationRule, con, A2DeclineShapes());
}

// The rules partition the problems they apply to: no two rules apply to one IR.
TEST_CASE("A2 and every other rule never apply to the same query", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectDeclines(MakeS1UniformMatroidRule, con, A2AppliesShapes());
	ExpectDeclines(MakeA1SeparableBoxRule, con, A2AppliesShapes());
	ExpectDeclines(MakeR1ContinuousKnapsackRule, con, A2AppliesShapes());
	ExpectDeclines(MakeN1EuclideanBallRule, con, A2AppliesShapes());
	ExpectDeclines(MakeA2SingleFacilityLocationRule, con, S1AppliesShapes());
	ExpectDeclines(MakeA2SingleFacilityLocationRule, con, A1AppliesShapes());
	ExpectDeclines(MakeA2SingleFacilityLocationRule, con, R1AppliesShapes());
	ExpectDeclines(MakeA2SingleFacilityLocationRule, con, N1AppliesShapes());
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

//===--------------------------------------------------------------------===//
// Keyed plans
//===--------------------------------------------------------------------===//

// A reducer's `by` key is read into the IR (DirectPart::by). S1 and R1 group by it, and their answer must equal the
// solver's on the same query.

//! Source columns: id, g (VARCHAR; rows 8 and 9 have a NULL g, which is in no group, so the keyed plans and the solver
//! both leave them ungrouped), w (DOUBLE), cap (INTEGER), flag (BOOLEAN). Within a group no two rows tie on w or on
//! w / cap, so each optimum is unique.
const char *KEYED_ROWS = "(VALUES (1, 'a', 5.0::DOUBLE, 2, TRUE), (2, 'a', 4.0::DOUBLE, 1, FALSE),"
                         " (3, 'a', 1.5::DOUBLE, 3, TRUE), (4, 'b', 3.0::DOUBLE, 2, FALSE),"
                         " (5, 'b', 6.0::DOUBLE, 1, TRUE), (6, 'c', 2.5::DOUBLE, 1, TRUE),"
                         " (7, 'c', 7.0::DOUBLE, 2, FALSE), (8, NULL, 4.5::DOUBLE, 2, FALSE),"
                         " (9, NULL, 3.5::DOUBLE, 1, FALSE)) t(id, g, w, cap, flag)";

string KeyedQuery(const string &declaration, const string &constraints, const string &objective,
                  const string &selected) {
	return "SELECT id, " + selected + " FROM (FROM " + string(KEYED_ROWS) + " DECIDE " + declaration + " SUCH THAT " +
	       constraints + " " + objective + ") q ORDER BY id";
}

duckdb::unique_ptr<MaterializedQueryResult> RunWithMode(Connection &con, const string &mode, const string &query) {
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='" + mode + "'"));
	return con.Query(query);
}

struct KeyedCase {
	string branch;
	//! The rule that reads the key (S1 or R1).
	string rule;
	string declaration;
	//! The constraints, with the key written where `{by}` stands.
	string constraints;
	string objective;
	string selected;
	string written_key;
};

string WithKey(const string &constraints, const string &key) {
	return StringUtil::Replace(constraints, "{by}", key);
}

TEST_CASE("S1 and R1 keyed plans answer as the solver does on the same `by` key", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::vector<KeyedCase> cases;
	cases.push_back({"S1 counts per group", "S1_UNIFORM_MATROID", "x(BOOL)", "SUM(x){by} <= 1", "MAXIMIZE SUM(w*x)",
	                 "x", " by (g)"});
	// Fixed values and no lower bound: the plan counts the fixed rows per group but has no free count to keep.
	cases.push_back({"S1 per group with a fixed value and no lower bound", "S1_UNIFORM_MATROID", "x(BOOL)",
	                 "SUM(x){by} <= 2 AND x >= flag", "MAXIMIZE SUM(w*x)", "x", " by (g)"});
	cases.push_back({"R1 fills each group's budget by value per weight", "R1_CONTINUOUS_KNAPSACK", "x(REAL)",
	                 "x <= cap AND SUM(cap*x){by} <= 3", "MAXIMIZE SUM(w*x)", "ROUND(x, 9)", " by (g)"});
	cases.push_back({"R1 groups by two key columns", "R1_CONTINUOUS_KNAPSACK", "x(REAL)",
	                 "x <= cap AND SUM(cap*x){by} <= 2", "MAXIMIZE SUM(w*x)", "ROUND(x, 9)", " by (g, flag)"});
	// The second decision has no weight in the budget, so its rows spend nothing in any group.
	cases.push_back({"R1 per group beside a decision outside the budget", "R1_CONTINUOUS_KNAPSACK", "x(REAL), y(REAL)",
	                 "x <= cap AND y <= cap AND SUM(cap*x){by} <= 3", "MAXIMIZE SUM(w*x) + SUM(w*y)",
	                 "ROUND(x, 9), ROUND(y, 9)", " by (g)"});
	for (auto &keyed : cases) {
		INFO(keyed.branch);
		auto query = KeyedQuery(keyed.declaration, WithKey(keyed.constraints, keyed.written_key), keyed.objective,
		                        keyed.selected);
		auto solver = RunWithMode(con, "off", query);
		REQUIRE_NO_FAIL(*solver);
		auto direct = RunWithMode(con, "require", query);
		REQUIRE_NO_FAIL(*direct);
		CHECK(direct->ToString() == solver->ToString());
		auto plan = RunWithMode(con, "require", "EXPLAIN " + query);
		REQUIRE_NO_FAIL(*plan);
		CHECK(plan->ToString().find(keyed.rule) != string::npos);
	}
}

// The columns of a relation compare NULL as a value (D5), so a row whose column is NULL is still in its relation's
// group: each of the two rows of `t` below, joined with two rows of `u`, is a group of two rows with one pick.
TEST_CASE("S1 groups by a relation whose column is NULL as the solver does", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	const string query =
	    "SELECT t.id, u.v, x FROM (VALUES (1, CAST(NULL AS VARCHAR), 5.0), (2, CAST(NULL AS VARCHAR), 4.0)) t(id, g, "
	    "w),"
	    " (VALUES (1), (2)) u(v) DECIDE x(BOOL) SUCH THAT SUM(x) by (t) <= 1 MAXIMIZE SUM(w * x + v * x) ORDER BY 1, 2";
	auto solver = RunWithMode(con, "off", query);
	REQUIRE_NO_FAIL(*solver);
	auto direct = RunWithMode(con, "require", query);
	REQUIRE_NO_FAIL(*direct);
	CHECK(direct->ToString() == solver->ToString());
	// Each group picks one row: two picks, not four.
	int32_t picks = 0;
	for (idx_t row = 0; row < direct->RowCount(); row++) {
		picks += direct->GetValue(2, row).GetValue<int32_t>();
	}
	CHECK(picks == 2);
}

} // namespace direct_decline_test
