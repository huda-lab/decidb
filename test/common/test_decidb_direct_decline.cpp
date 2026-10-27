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
	DirectSubstitute Apply(duckdb::unique_ptr<LogicalOperator>, Optimizer &, const DirectApplicabilityCondition &,
	                       const duckdb::vector<string> &) const override {
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

//! Grouping keys on the constraints, one list of source slots per constraint in order. No SQL produces one until
//! direct solve reads a reducer's `by` key, so every grouping case is an edit.
Edit PerKeys(duckdb::vector<duckdb::vector<idx_t>> slots) {
	return [slots](DirectIR &ir) {
		for (idx_t i = 0; i < slots.size(); i++) {
			for (auto slot : slots[i]) {
				ir.constraints[i].scope.per_key_slots.push_back(slot);
			}
		}
	};
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
	shapes.push_back(S1("counts PER a column", "SUM(x) <= 2", PerKeys({{FLAG_SLOT}})));
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
	shapes.push_back(S1("an objective WHEN", "SUM(x) <= 2", nullptr, "MAXIMIZE when flag: SUM(w*x)"));
	shapes.push_back(
	    S1("an objective PER key", "SUM(x) <= 2", [](DirectIR &ir) { ir.objective.scope.per_key_slots.push_back(0); }));
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
	shapes.push_back(S1("bounds with different PER keys", "SUM(x) >= 1 AND SUM(x) <= 2", PerKeyOnConstraint(1)));
	shapes.push_back(S1("a second bound without the WHEN the first has", "when flag: SUM(x) >= 1 AND SUM(x) <= 2"));
	shapes.push_back(
	    S1("bounds with different WHEN conditions", "when flag: SUM(x) >= 1 AND when NOT flag: SUM(x) <= 2"));
	shapes.push_back(S1("a WHEN that may raise", "when CAST(cap AS TINYINT) > 0: SUM(x) <= 2"));
	shapes.push_back(S1("a WHEN that is not Boolean", "SUM(x) <= 2", [](DirectIR &ir) {
		ir.constraints[0].scope.when = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	}));
	// ProveFixedVariables.
	shapes.push_back(S1("a fixed value under PER", "SUM(x) <= 2 AND x = 1", PerKeyOnConstraint(1)));
	shapes.push_back(S1("a fixed value with a column coefficient", "SUM(x) <= 2 AND w * x = 1"));
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
	shapes.push_back(A1("a query-wide decision", "s <= 3", nullptr, "MAXIMIZE s", "per (): s(REAL)"));
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
	shapes.push_back(A1("a bound under a WHEN that may raise", "when CAST(cap AS TINYINT) > 0: x <= cap"));
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
	shapes.push_back(A1("a square with a constant fixed only within the query", "x <= cap", nullptr,
	                    "MINIMIZE SUM(POWER(x - txid_current(), 2))"));
	shapes.push_back(A1(
	    "a square term with no coefficient", "x <= cap",
	    [](DirectIR &ir) { ir.objective.parts[0].terms[0].inner[1].coefficient.reset(); },
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
	shapes.push_back(
	    R1("a column limit with a PER key and a WHEN", "when flag: SUM(w*x) <= cap", PerKeys({{NAME_SLOT}})));
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
	shapes.push_back(
	    R1("a budget with a PER key edited into the IR", "x <= cap AND SUM(cap*x) <= 5", PerKeyOnConstraint(1)));
	shapes.push_back(R1("a budget with two PER keys", "SUM(cap*x) <= 5", PerKeys({{NAME_SLOT, FLAG_SLOT}})));
	shapes.push_back(R1("a budget under a WHEN", "x <= cap AND when flag: SUM(cap*x) <= 5"));
	shapes.push_back(R1("a budget with an aggregate WHEN", "x <= cap AND SUM(when flag: cap*x) <= 5"));
	shapes.push_back(R1("a budget with a PER key and a WHEN", "when flag: SUM(cap*x) <= 5", PerKeys({{NAME_SLOT}})));
	shapes.push_back(R1("a range with the same PER key on both clauses", "SUM(cap*x) >= 1 AND SUM(cap*x) <= 5",
	                    PerKeys({{NAME_SLOT}, {NAME_SLOT}})));
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
	shapes.push_back(R1("two decisions in a budget with a PER key", "SUM(cap*x + cap*y) <= 5", PerKeys({{NAME_SLOT}}),
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
	shapes.push_back(R1("an entity scope", "SUM(cap*x) <= 5", [](DirectIR &ir) { ir.entity_scopes.emplace_back(); }));
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
	// A per-row constraint that reads two decisions links them (DirectReadsOneDecision).
	shapes.push_back(R1("a per-row constraint that links two decisions",
	                    "x <= cap AND y <= cap AND x + y <= 4 AND SUM(cap*x + cap*y) <= 5", nullptr,
	                    "MAXIMIZE SUM(w*x + w*y)", "x(REAL), y(REAL)"));
	shapes.push_back(R1("a membership constraint", "x IN (1, 2) AND SUM(cap*x) <= 5"));
	// The binder rejects `x <> 2` on a REAL decision, so the comparison is edited.
	shapes.push_back(R1("a not-equal constraint", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.constraints[0].comparison = ExpressionType::COMPARE_NOTEQUAL; }));
	shapes.push_back(R1("a comparison that is not a bound", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.constraints[0].comparison = ExpressionType::COMPARE_DISTINCT_FROM; }));
	shapes.push_back(R1("a bound with a PER key", "x <= cap AND SUM(cap*x) <= 5", PerKeyOnConstraint(0)));
	shapes.push_back(R1("a bound with a negative sign", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { LeftPart(ir, 0).sign = -1; }));
	// ProveBudgetScope: every clause of the budget covers the same rows.
	shapes.push_back(R1("a PER key on one clause of the budget only", "SUM(cap*x) >= 1 AND SUM(cap*x) <= 5",
	                    PerKeys({{NAME_SLOT}, {}})));
	shapes.push_back(R1("another PER key on the second clause", "SUM(cap*x) >= 1 AND SUM(cap*x) <= 5",
	                    PerKeys({{NAME_SLOT}, {FLAG_SLOT}})));
	shapes.push_back(R1("the PER keys in another order on the second clause", "SUM(cap*x) >= 1 AND SUM(cap*x) <= 5",
	                    PerKeys({{NAME_SLOT, FLAG_SLOT}, {FLAG_SLOT, NAME_SLOT}})));
	shapes.push_back(R1("a PER key more on the second clause", "SUM(cap*x) >= 1 AND SUM(cap*x) <= 5",
	                    PerKeys({{NAME_SLOT}, {NAME_SLOT, FLAG_SLOT}})));
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
	// The binder rejects a strict bound on a REAL decision, so the comparison is edited.
	shapes.push_back(R1("a strict folded bound on a real decision", "x <= 3 AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.constraints[0].comparison = ExpressionType::COMPARE_LESSTHAN; }));
	// CheckCondition and ProveObjective: the objective.
	shapes.push_back(R1("an objective PER key", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.objective.scope.per_key_slots.push_back(0); }));
	shapes.push_back(R1("an objective WHEN", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MAXIMIZE when flag: SUM(w*x)"));
	shapes.push_back(R1("an average objective", "x <= cap AND SUM(cap*x) <= 5", nullptr, "MAXIMIZE AVG(w*x)"));
	shapes.push_back(R1("an objective part with a filter", "x <= cap AND SUM(cap*x) <= 5",
	                    [](DirectIR &ir) { ir.objective.parts[0].filter = True(); }));
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

TEST_CASE("R1 applies to the forms of its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectApplies(MakeR1ContinuousKnapsackRule, con, R1AppliesShapes());
}

TEST_CASE("R1 declines each shape that is not exactly its class", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	ExpectDeclines(MakeR1ContinuousKnapsackRule, con, R1DeclineShapes());
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
// Keyed plans, run from an edited IR
//===--------------------------------------------------------------------===//

// The front end declines a reducer's `by` key until direct solve reads one (framework todo H-04, H-05), yet S1's and
// R1's keyed plans stay for that work. These cases run them: the rule checks and applies the IR after an edit adds the
// key, and its answer must equal the solver's on the query that writes the same key as `by`.

//! Edits the IR the driver offers, then lets `target` check and apply it, so the rule's real plan runs.
class EditedRule final : public DirectSolveRule {
public:
	EditedRule(duckdb::unique_ptr<DirectSolveRule> target_p, Edit edit_p)
	    : target(std::move(target_p)), edit(std::move(edit_p)) {
	}
	const char *Name() const override {
		return target->Name();
	}
	duckdb::unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir,
	                                                                ClientContext &context) const override {
		edit(const_cast<DirectIR &>(ir));
		return target->CheckCondition(ir, context);
	}
	string Summary() const override {
		return target->Summary();
	}
	DirectSubstitute Apply(duckdb::unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &condition,
	                       const duckdb::vector<string> &source_fragments) const override {
		return target->Apply(std::move(source), optimizer, condition, source_fragments);
	}

private:
	duckdb::unique_ptr<DirectSolveRule> target;
	Edit edit;
};

//! Source columns: id, g (VARCHAR, never NULL: the keyed plans leave a NULL-key row ungrouped, framework todo H-05),
//! w (DOUBLE), cap (INTEGER), flag (BOOLEAN). Within a group no two rows tie on w or on w / cap, so each optimum is
//! unique.
const char *KEYED_ROWS = "(VALUES (1, 'a', 5.0::DOUBLE, 2, TRUE), (2, 'a', 4.0::DOUBLE, 1, FALSE),"
                         " (3, 'a', 1.5::DOUBLE, 3, TRUE), (4, 'b', 3.0::DOUBLE, 2, FALSE),"
                         " (5, 'b', 6.0::DOUBLE, 1, TRUE), (6, 'c', 2.5::DOUBLE, 1, TRUE),"
                         " (7, 'c', 7.0::DOUBLE, 2, FALSE)) t(id, g, w, cap, flag)";
constexpr idx_t KEYED_ID_SLOT = 0;
constexpr idx_t KEYED_G_SLOT = 1;
constexpr idx_t KEYED_FLAG_SLOT = 4;

string KeyedQuery(const string &declaration, const string &constraints, const string &objective,
                  const string &selected) {
	return "SELECT id, " + selected + " FROM (FROM " + string(KEYED_ROWS) + " DECIDE " + declaration + " SUCH THAT " +
	       constraints + " " + objective + ") q ORDER BY id";
}

//! The answer of `query` under direct solve with only `rule` (after `edit`) registered; `require` fails the query when
//! the rule declines.
duckdb::unique_ptr<MaterializedQueryResult> RunEdited(Connection &con, MakeRule make_rule, Edit edit,
                                                      const string &query) {
	auto state = make_shared_ptr<DirectRuleTestSeam>();
	state->make_rules = [make_rule, edit] {
		duckdb::vector<duckdb::unique_ptr<DirectSolveRule>> rules;
		rules.push_back(make_uniq<EditedRule>(make_rule(), edit));
		return rules;
	};
	con.context->registered_state->Remove(DIRECT_RULE_TEST_SEAM_KEY);
	con.context->registered_state->Insert(DIRECT_RULE_TEST_SEAM_KEY, std::move(state));
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='require'"));
	auto result = con.Query(query);
	con.context->registered_state->Remove(DIRECT_RULE_TEST_SEAM_KEY);
	return result;
}

duckdb::unique_ptr<MaterializedQueryResult> RunSolver(Connection &con, const string &query) {
	REQUIRE_NO_FAIL(con.Query("SET decide_direct_solve='off'"));
	return con.Query(query);
}

struct KeyedCase {
	string branch;
	MakeRule make_rule;
	string declaration;
	//! The constraints direct solve reads; `{by}` is empty there and the written key for the solver.
	string constraints;
	string objective;
	string selected;
	//! The key slots of each constraint, in order.
	duckdb::vector<duckdb::vector<idx_t>> keys;
	string written_key;
};

string WithKey(const string &constraints, const string &key) {
	return StringUtil::Replace(constraints, "{by}", key);
}

TEST_CASE("S1 and R1 keyed plans answer as the solver does on the same `by` key", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::vector<KeyedCase> cases;
	cases.push_back({"S1 counts per group",
	                 MakeS1UniformMatroidRule,
	                 "x(BOOL)",
	                 "SUM(x){by} <= 1",
	                 "MAXIMIZE SUM(w*x)",
	                 "x",
	                 {{KEYED_G_SLOT}},
	                 " by (g)"});
	// Fixed values and no lower bound: the plan counts the fixed rows per group but has no free count to keep.
	cases.push_back({"S1 per group with a fixed value and no lower bound",
	                 MakeS1UniformMatroidRule,
	                 "x(BOOL)",
	                 "SUM(x){by} <= 2 AND x >= flag",
	                 "MAXIMIZE SUM(w*x)",
	                 "x",
	                 {{KEYED_G_SLOT}},
	                 " by (g)"});
	cases.push_back({"R1 fills each group's budget by value per weight",
	                 MakeR1ContinuousKnapsackRule,
	                 "x(REAL)",
	                 "x <= cap AND SUM(cap*x){by} <= 3",
	                 "MAXIMIZE SUM(w*x)",
	                 "ROUND(x, 9)",
	                 {{}, {KEYED_G_SLOT}},
	                 " by (g)"});
	cases.push_back({"R1 groups by two key columns",
	                 MakeR1ContinuousKnapsackRule,
	                 "x(REAL)",
	                 "x <= cap AND SUM(cap*x){by} <= 2",
	                 "MAXIMIZE SUM(w*x)",
	                 "ROUND(x, 9)",
	                 {{}, {KEYED_G_SLOT, KEYED_FLAG_SLOT}},
	                 " by (g, flag)"});
	// The second decision has no weight in the budget, so its rows spend nothing in any group.
	cases.push_back({"R1 per group beside a decision outside the budget",
	                 MakeR1ContinuousKnapsackRule,
	                 "x(REAL), y(REAL)",
	                 "x <= cap AND y <= cap AND SUM(cap*x){by} <= 3",
	                 "MAXIMIZE SUM(w*x) + SUM(w*y)",
	                 "ROUND(x, 9), ROUND(y, 9)",
	                 {{}, {}, {KEYED_G_SLOT}},
	                 " by (g)"});
	for (auto &keyed : cases) {
		INFO(keyed.branch);
		auto solver = RunSolver(con, KeyedQuery(keyed.declaration, WithKey(keyed.constraints, keyed.written_key),
		                                        keyed.objective, keyed.selected));
		REQUIRE_NO_FAIL(*solver);
		auto direct =
		    RunEdited(con, keyed.make_rule, PerKeys(keyed.keys),
		              KeyedQuery(keyed.declaration, WithKey(keyed.constraints, ""), keyed.objective, keyed.selected));
		REQUIRE_NO_FAIL(*direct);
		CHECK(direct->ToString() == solver->ToString());
	}
}

//! A reducer row check that looks only at the rows `rows` keeps (`when flag: SUM(when id > bar: x)` in SQL, which no
//! rule reads yet): the plan raises its message when none of those rows passes the filter.
Edit RowCheckOverFlaggedRows(int32_t bar) {
	return [bar](DirectIR &ir) {
		DirectRowCheck check;
		check.rows = make_uniq<BoundColumnRefExpression>(LogicalType::BOOLEAN, ir.source_bindings[KEYED_FLAG_SLOT]);
		check.filter = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHAN,
		    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ir.source_bindings[KEYED_ID_SLOT]),
		    make_uniq<BoundConstantExpression>(Value::INTEGER(bar)));
		check.message = "probe: the reducer reads no flagged row";
		ir.row_checks.push_back(std::move(check));
	};
}

TEST_CASE("A reducer row check reads only the rows its clause keeps", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto query = KeyedQuery("x(BOOL)", "SUM(x) <= 1", "MAXIMIZE SUM(w*x)", "x");
	// Row 6 is flagged and has id > 5.
	REQUIRE_NO_FAIL(*RunEdited(con, MakeS1UniformMatroidRule, RowCheckOverFlaggedRows(5), query));
	// Row 7 has id > 6 but is not flagged, so the check must not count it.
	auto failed = RunEdited(con, MakeS1UniformMatroidRule, RowCheckOverFlaggedRows(6), query);
	REQUIRE(failed->HasError());
	CHECK(failed->GetError().find("probe: the reducer reads no flagged row") != string::npos);
}

} // namespace direct_decline_test
