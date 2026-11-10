#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>

#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_o1_monge_assignment {

constexpr const char *RULE_NAME = "O1_MONGE_ASSIGNMENT";

//! One side of the cross product: the key its constraint classes the pairs by, and the column of that key the cost
//! reads.
struct Side {
	//! The key's columns in key order, as the source has them. A relation key names every column of its relation, so
	//! its classes are the distinct rows of the side.
	vector<ColumnBinding> key;
	//! The key's column types, and how the join to the classes compares them (a NULL is a value of a relation).
	DirectClassKey class_key;
	//! The index in `key` of the position column the cost reads.
	idx_t position = DConstants::INVALID_INDEX;
};

//! What CheckCondition proved: a one-to-one matching of the classes of the two sides, costed by a distance or a
//! product of one position of each side. The optimum pairs the classes of equal rank, or of opposite rank, in the order
//! of their positions.
struct Condition final : DirectApplicabilityCondition {
	Side left;
	Side right;
	//! The right side ranks from its greatest position down, so that equal ranks match.
	bool opposite = false;
	//! The written coefficient `k * cost` of the decision and the part's own sign and factor, which the plan evaluates
	//! on every pair as the solver does.
	DirectLinearPart cost;
};

//! The constant `expression` folds to; false for a value a row can change or a constant that is not finite.
bool FoldsTo(ClientContext &context, const Expression &expression, double &value) {
	return TryEvaluateFoldableDouble(context, expression, value, DIRECT_FINITE_CONSTANT);
}

int SignOf(double value) {
	return (value > 0) - (value < 0);
}

//! Whether a cast from the numeric type `from` to `to` keeps every value: a DECIMAL widened in both digits, or any
//! number taken to the DOUBLE the solver reads it as.
bool PreservesValues(const LogicalType &from, const LogicalType &to) {
	if (to.id() == LogicalTypeId::DOUBLE) {
		return from.IsNumeric();
	}
	if (to.id() != LogicalTypeId::DECIMAL || from.id() != LogicalTypeId::DECIMAL) {
		return false;
	}
	auto from_whole = DecimalType::GetWidth(from) - DecimalType::GetScale(from);
	auto to_whole = DecimalType::GetWidth(to) - DecimalType::GetScale(to);
	return DecimalType::GetScale(to) >= DecimalType::GetScale(from) && to_whole >= from_whole;
}

//! Looks through the casts of `expression` that keep every value, or with `to_any_type` through any cast that cannot
//! fail to NULL.
const Expression &SkipCasts(const Expression &expression, bool to_any_type) {
	auto node = &expression;
	while (node->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = node->Cast<BoundCastExpression>();
		if (cast.try_cast || (!to_any_type && !PreservesValues(cast.child->return_type, cast.return_type))) {
			break;
		}
		node = cast.child.get();
	}
	return *node;
}

//! The numeric column `expression` names, looking through the casts the binder puts on a column to bring two columns
//! to a common type. A cast of a column maps its values in order, so the order of the column's values is the order of
//! the values the cost reads. Null for anything else.
const BoundColumnRefExpression *ColumnOf(const Expression &expression) {
	auto &node = SkipCasts(expression, true);
	if (node.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF || !node.return_type.IsNumeric()) {
		return nullptr;
	}
	return &node.Cast<BoundColumnRefExpression>();
}

//! The function `name` that `expression` applies to `arity` arguments, looking through the casts that keep every value;
//! null when it applies another.
const BoundFunctionExpression *CallOf(const Expression &expression, const char *name, idx_t arity) {
	auto &node = SkipCasts(expression, false);
	if (node.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &call = node.Cast<BoundFunctionExpression>();
	return call.function.name == name && call.children.size() == arity ? &call : nullptr;
}

//! A written coefficient `k * core`: the sign of the constant factors, and the factors that are not constant.
struct Factors {
	int sign = 1;
	vector<const Expression *> cores;
};

//! Splits a product into its constant factors and the others, in whatever order and grouping it was written.
void SplitFactors(ClientContext &context, const Expression &expression, Factors &factors) {
	auto &value = SkipCasts(DecideUnguarded(expression), false);
	double constant;
	if (FoldsTo(context, value, constant)) {
		factors.sign *= SignOf(constant);
	} else if (auto product = CallOf(value, "*", 2)) {
		SplitFactors(context, *product->children[0], factors);
		SplitFactors(context, *product->children[1], factors);
	} else if (auto negation = CallOf(value, "-", 1)) {
		factors.sign = -factors.sign;
		SplitFactors(context, *negation->children[0], factors);
	} else {
		factors.cores.push_back(&value);
	}
}

//! `a - b` of two numeric columns.
bool ReadDifference(const Expression &expression, const BoundColumnRefExpression *&a,
                    const BoundColumnRefExpression *&b) {
	auto difference = CallOf(expression, "-", 2);
	if (!difference) {
		return false;
	}
	a = ColumnOf(*difference->children[0]);
	b = ColumnOf(*difference->children[1]);
	return a && b;
}

//! The cost the factors make, as two numeric columns: `ABS(a - b)` or `POWER(a - b, 2)` (a distance), or `a * b`.
bool ReadCost(ClientContext &context, const Factors &factors, const BoundColumnRefExpression *&a,
              const BoundColumnRefExpression *&b, bool &distance) {
	distance = factors.cores.size() == 1;
	if (!distance) {
		if (factors.cores.size() != 2) {
			return false;
		}
		a = ColumnOf(*factors.cores[0]);
		b = ColumnOf(*factors.cores[1]);
		return a && b;
	}
	double exponent;
	if (auto absolute = CallOf(*factors.cores[0], "abs", 1)) {
		return ReadDifference(*absolute->children[0], a, b);
	}
	auto square = CallOf(*factors.cores[0], "power", 2);
	return square && FoldsTo(context, *square->children[1], exponent) && exponent == 2.0 &&
	       ReadDifference(*square->children[0], a, b);
}

idx_t IndexOf(const vector<ColumnBinding> &columns, ColumnBinding binding) {
	return NumericCast<idx_t>(std::find(columns.begin(), columns.end(), binding) - columns.begin());
}

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}
	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.cross_product.left.empty() || ir.decisions.size() != 1 || ir.decisions[0].domain != DirectDomain::BOOL ||
		    ir.decisions[0].scope != DecideVarScope::ROW || !ir.keyed_scopes.empty() || ir.constraints.size() != 2) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		Side first, second;
		bool first_on_left, second_on_left;
		if (!ProveClause(ir, context, ir.constraints[0], first, first_on_left) ||
		    !ProveClause(ir, context, ir.constraints[1], second, second_on_left) || first_on_left == second_on_left) {
			return nullptr;
		}
		condition->left = std::move(first_on_left ? first : second);
		condition->right = std::move(first_on_left ? second : first);
		if (!ProveCost(ir, context, *condition)) {
			return nullptr;
		}
		// The classes of each side, then the pairs.
		condition->source_reads = 3;
		return std::move(condition);
	}
	string Summary() const override {
		return "x in {0,1}, SUM(x) = 1 per left and per right class, min SUM(ABS(a - b)*x) or SUM((a - b)^2*x), "
		       "min/max SUM(a*b*x)";
	}
	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveClause(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &clause, Side &side,
	                 bool &on_left) const;
	bool ProveCost(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! A clause `per K: SUM(x) by (K) = 1`, where K is a relation key (a NULL is a value of its own) whose columns all come
//! from one side of the cross product. `on_left` says which.
bool Rule::ProveClause(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &clause, Side &side,
                       bool &on_left) const {
	double one;
	bool plain_sum;
	auto when = clause.Membership(plain_sum);
	if (!clause.per_class || clause.classes.empty() || clause.comparison != ExpressionType::COMPARE_EQUAL ||
	    !FoldsTo(context, *clause.rhs, one) || one != 1.0 || !plain_sum || when) {
		return false;
	}
	auto &sum = clause.lhs[0];
	double coefficient;
	if (!DirectSameKey(sum.by, clause.classes) || sum.terms.size() != 1 || !sum.terms[0].IsLinear() ||
	    sum.terms[0].sign != 1 || !FoldsTo(context, *sum.terms[0].coefficient, coefficient) || coefficient != 1.0) {
		return false;
	}
	for (idx_t i = 0; i < clause.classes.size(); i++) {
		auto &element = clause.classes[i];
		if (element.no_group || element.grouping->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto binding = element.grouping->Cast<BoundColumnRefExpression>().binding;
		auto in_left = std::find(ir.cross_product.left.begin(), ir.cross_product.left.end(), binding) !=
		               ir.cross_product.left.end();
		if (i > 0 && in_left != on_left) {
			return false;
		}
		on_left = in_left;
		side.key.push_back(binding);
	}
	side.class_key = DirectClassKeyOf(clause.classes, false);
	return true;
}

//! The objective: one `SUM(k * cost * x)`, with the cost a distance minimized or a product of one position of each
//! side. Records the position columns and the rank direction.
bool Rule::ProveCost(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	auto &objective = ir.objective;
	if (objective.parts.size() != 1 || objective.scope.when) {
		return false;
	}
	auto &part = objective.parts[0];
	if (!part.IsPlainSum() || part.filter || part.terms.size() != 1 || !part.terms[0].IsLinear() ||
	    !DirectReadLinearPart(ir, context, part, objective.written, condition.cost)) {
		return false;
	}
	auto &cost = condition.cost;
	Factors factors;
	factors.sign = cost.sign * cost.terms[0].sign * SignOf(cost.scale);
	SplitFactors(context, *cost.terms[0].coefficient, factors);
	const BoundColumnRefExpression *a, *b;
	bool distance;
	if (!ReadCost(context, factors, a, b, distance)) {
		return false;
	}
	// The positions: one column of each side's key, in either order.
	auto place = [&](const BoundColumnRefExpression &of_left, const BoundColumnRefExpression &of_right) {
		auto &left = condition.left;
		auto &right = condition.right;
		left.position = IndexOf(left.key, of_left.binding);
		right.position = IndexOf(right.key, of_right.binding);
		return left.position < left.key.size() && right.position < right.key.size();
	};
	if (!place(*a, *b) && !place(*b, *a)) {
		return false;
	}
	// A NULL position costs nothing against any partner, which the order of the positions cannot place: the plan must
	// know that no position is NULL.
	auto not_null = [&](const Side &side) {
		return std::find(ir.not_null.begin(), ir.not_null.end(), side.key[side.position]) != ir.not_null.end();
	};
	if (!not_null(condition.left) || !not_null(condition.right)) {
		return false;
	}
	bool maximize = objective.sense == DecideSense::MAXIMIZE;
	if (distance && (maximize || factors.sign < 0)) {
		return false;
	}
	// Equal ranks minimize a distance and, by the rearrangement inequality, maximize a product of positive weight.
	condition.opposite = !distance && (factors.sign > 0) != maximize;
	return true;
}

//! Builds the plan of one problem whose condition holds: the classes of each side ranked by position (a hash aggregate
//! of a read of the source on the key and a rank window), joined back to a third read of the pairs, which picks the
//! matched pairs.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
	}

	//! The class plan over a read `rows`: the side's key columns, then the rank of the class by position (greatest
	//! first when `descending`) and the number of classes, one row per class.
	unique_ptr<LogicalOperator> RankClasses(unique_ptr<LogicalOperator> rows, const Side &side, bool descending) const {
		DirectAggregateStage distinct(binder);
		vector<ColumnBinding> classes;
		for (idx_t i = 0; i < side.key.size(); i++) {
			classes.push_back(distinct.Group(calc.Out(calc.Col(side.class_key.types[i], side.key[i]))));
		}
		auto plan = distinct.Finish(std::move(rows));
		auto position = calc.Col(side.class_key.types[side.position], classes[side.position]);
		auto order = descending ? OrderType::DESCENDING : OrderType::ASCENDING;
		// Hand-built: a window is an operator-specific node, which the builder (scalar expressions) has no operation
		// for.
		auto rank =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		auto count =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
		                                     make_uniq<AggregateFunction>(CountStarFun::GetFunction()), nullptr);
		// The count shares the rank's order, so that the two share one sort.
		for (auto window : {rank.get(), count.get()}) {
			window->start = WindowBoundary::UNBOUNDED_PRECEDING;
			window->end = WindowBoundary::UNBOUNDED_FOLLOWING;
			window->orders.emplace_back(order, OrderByNullType::NULLS_LAST, calc.Out(position));
		}
		auto ranked = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		ranked->expressions.push_back(std::move(rank));
		ranked->expressions.push_back(std::move(count));
		ranked->children.push_back(std::move(plan));
		return std::move(ranked);
	}

	//! The substitute over the read `rows` of the pairs: its source columns, then 1 on one pair of each matched class
	//! pair, and the column that keeps the checks and the window live. `left` and `right` are the sides' class plans
	//! (RankClasses).
	DirectSubstitute Assign(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> left,
	                        unique_ptr<LogicalOperator> right) const {
		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto pairs = p1.Forward(source_bindings, source_types);
		// The written coefficient on every pair, as the solver reads it.
		auto &cost = condition.cost;
		auto &term = cost.terms[0];
		auto coefficient = p1.Add(DirectScaledTerm(optimizer, term.coefficient->Copy(), cost.sign * term.sign,
		                                           cost.scaled, cost.scale, cost.divides));
		vector<unique_ptr<Expression>> checks;
		if (auto valid = DirectCheckNumber(optimizer, coefficient, cost.names[0], DirectValuesOn(cost, 0),
		                                   source_bindings, p1.index, cost.scaled, cost.scale, cost.divides)) {
			checks.push_back(std::move(valid));
		}
		auto joined = Join(p1.Finish(std::move(rows)), pairs, condition.left, std::move(left));
		auto left_answers = joined.answers;
		joined = Join(std::move(joined.plan), pairs, condition.right, std::move(right));
		auto &right_answers = joined.answers;
		// The two sides pair one to one only with as many classes on one side as on the other.
		checks.push_back(DirectRaiseUnless(
		    calc,
		    calc.Out(calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Col(LogicalType::BIGINT, left_answers[1]),
		                      calc.Col(LogicalType::BIGINT, right_answers[1]))),
		    DirectInfeasibleErrorPredicate(optimizer, errors)));
		auto checked = make_uniq<LogicalFilter>(DirectAllHold(calc, std::move(checks)));
		checked->children.push_back(std::move(joined.plan));

		// Of the pairs of one matched pair of classes, the first by the window's order is the one assigned.
		auto left_rank = calc.Col(LogicalType::BIGINT, left_answers[0]);
		auto right_rank = calc.Col(LogicalType::BIGINT, right_answers[0]);
		auto first =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		first->start = WindowBoundary::UNBOUNDED_PRECEDING;
		first->end = WindowBoundary::UNBOUNDED_FOLLOWING;
		first->partitions.push_back(calc.Out(left_rank));
		first->partitions.push_back(calc.Out(right_rank));
		auto window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		window->expressions.push_back(std::move(first));
		window->children.push_back(std::move(checked));

		DirectProjectionStage result(binder.GenerateTableIndex());
		result.Forward(pairs, source_types);
		auto number = calc.Col(LogicalType::BIGINT, ColumnBinding(window_index, 0));
		auto assigned = calc.And(calc.Cmp(ExpressionType::COMPARE_EQUAL, left_rank, right_rank),
		                         calc.Cmp(ExpressionType::COMPARE_EQUAL, number, calc.Constant(Value::BIGINT(1))));
		result.Add(calc.If(assigned, calc.Constant(Value::INTEGER(1)), calc.Constant(Value::INTEGER(0))));
		// The otherwise unused number is a blocking dependency: it forces a complete read of the pairs, and so every
		// check, before an outer LIMIT, COUNT or column-pruning parent returns. The coefficient is carried for the same
		// reason: a value no check reads is still computed on every pair, and raises where the solver's would.
		result.Add(calc.Out(number));
		result.Add(calc.Out(calc.Col(LogicalType::DOUBLE, coefficient)));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		substitute.validated_columns.push_back(source_bindings.size() + 2);
		substitute.child = result.Finish(std::move(window));
		return substitute;
	}

private:
	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;

	//! Joins each pair to the rank and count of its class on `side`; `pairs` are the pair rows' source columns.
	DirectJoinedAnswers Join(unique_ptr<LogicalOperator> rows, const vector<ColumnBinding> &pairs, const Side &side,
	                         unique_ptr<LogicalOperator> classes) const {
		vector<ColumnBinding> key;
		for (auto &column : side.key) {
			key.push_back(pairs[IndexOf(source_bindings, column)]);
		}
		return DirectJoinClassAnswers(binder, std::move(rows), key, side.class_key, std::move(classes));
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	auto left = DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
		return plan.RankClasses(std::move(rows), condition.left, false);
	});
	auto right = DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
		return plan.RankClasses(std::move(rows), condition.right, condition.opposite);
	});
	return DirectBuildOverOneRead(reader, [&](unique_ptr<LogicalOperator> rows) {
		return plan.Assign(std::move(rows), std::move(left), std::move(right));
	});
}

} // namespace direct_o1_monge_assignment

unique_ptr<DirectSolveRule> MakeO1MongeAssignmentRule() {
	return make_uniq<direct_o1_monge_assignment::Rule>();
}

} // namespace duckdb
