#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>

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
#include "duckdb/planner/operator/logical_cross_product.hpp"
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
	//! The position column the cost reads and its type. The key determines it: it is a column of the key, or the key
	//! holds a key of the position's table.
	ColumnBinding position;
	LogicalType position_type;
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
	if (auto absolute = CallOf(*factors.cores[0], "@", 1)) {
		return ReadDifference(*absolute->children[0], a, b);
	}
	auto square = CallOf(*factors.cores[0], "**", 2);
	return square && FoldsTo(context, *square->children[1], exponent) && exponent == 2.0 &&
	       ReadDifference(*square->children[0], a, b);
}

bool IsIn(const vector<ColumnBinding> &columns, ColumnBinding binding) {
	return std::find(columns.begin(), columns.end(), binding) != columns.end();
}

//! Whether the side's key fixes `position` on every row of a class: it is a column of the key, or the key holds a key
//! of the position's table.
bool Determines(const DirectIR &ir, const Side &side, ColumnBinding position) {
	if (IsIn(side.key, position)) {
		return true;
	}
	for (auto &table_key : ir.keys) {
		if (table_key[0].table_index == position.table_index &&
		    std::all_of(table_key.begin(), table_key.end(), [&](ColumnBinding at) { return IsIn(side.key, at); })) {
			return true;
		}
	}
	return false;
}

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}
	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.cross_product.left.empty() || !DirectIsOneRowDecision(ir, DirectDomain::BOOL) ||
		    ir.constraints.size() != 2) {
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
		// The source's rows are read once: by the pairs (each side reads its own rows).
		condition->source_reads = 1;
		return std::move(condition);
	}
	string Summary() const override {
		return "x in {0,1}, SUM(x) = 1 in each left and each right class, min SUM(ABS(a - b)*x) or SUM((a - b)^2*x), "
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
		if (element.grouping->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto binding = element.grouping->Cast<BoundColumnRefExpression>().binding;
		// A column of the key that may be NULL puts a row in no class, which the plan does not model.
		if (element.no_group && !IsIn(ir.not_null, binding)) {
			return false;
		}
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
	factors.sign = cost.sign * cost.terms[0].sign * SignOf(cost.factor.value);
	SplitFactors(context, *cost.terms[0].coefficient, factors);
	const BoundColumnRefExpression *a, *b;
	bool distance;
	if (!ReadCost(context, factors, a, b, distance)) {
		return false;
	}
	// The positions: one column of each side, in either order, which that side's key determines.
	auto place = [&](const BoundColumnRefExpression &of_left, const BoundColumnRefExpression &of_right) {
		auto &left = condition.left;
		auto &right = condition.right;
		left.position = of_left.binding;
		left.position_type = of_left.return_type;
		right.position = of_right.binding;
		right.position_type = of_right.return_type;
		return Determines(ir, left, left.position) && Determines(ir, right, right.position);
	};
	if (!place(*a, *b) && !place(*b, *a)) {
		return false;
	}
	// A NULL position costs nothing against any partner, which the order of the positions cannot place: the plan must
	// know that no position is NULL.
	if (!IsIn(ir.not_null, condition.left.position) || !IsIn(ir.not_null, condition.right.position)) {
		return false;
	}
	// The cost is minimized. A distance of negative weight is a maximized distance, which no ranking gives.
	if (distance && factors.sign < 0) {
		return false;
	}
	// Equal ranks minimize a distance and, by the rearrangement inequality, a product of negative weight.
	condition.opposite = !distance && factors.sign > 0;
	return true;
}

//! Builds the plan of one problem whose condition holds, the shape of a hand plan: each side ranks and chooses its copy
//! of every class on its own rows, and the pairs are formed once. The work whose keys come from one side runs below the
//! cross product (eager aggregation, applied to windows); a plan that ranks on the pairs would sort and group n^2 rows
//! for the n a side has.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, idx_t left_width_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), left_width(left_width_p), errors(errors_p) {
	}

	//! The verdict on every pair (DirectSourceReader::Gate): true when the written coefficient is a valid number on
	//! each of them, and otherwise the error of a pair that raises it. `left` and `right` are one read of each side;
	//! their pairs are the source's rows.
	unique_ptr<LogicalOperator> CheckPairs(DirectSourceReader::Read left, DirectSourceReader::Read right) const {
		// The source's columns as the two reads have them.
		vector<ColumnBinding> read_columns;
		vector<unique_ptr<Expression>> as_read;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			auto on_left = i < left_width;
			read_columns.emplace_back(on_left ? left.table_index : right.table_index, on_left ? i : i - left_width);
			as_read.push_back(calc.Out(calc.Col(source_types[i], read_columns.back())));
		}
		DirectProjectionStage p1(binder.GenerateTableIndex());
		p1.Forward(read_columns, source_types);
		// The written coefficient on every pair, as the solver reads it.
		auto &cost = condition.cost;
		auto &term = cost.terms[0];
		auto written = term.coefficient->Copy();
		DirectReplaceColumns(written, source_bindings, as_read);
		auto coefficient = p1.Add(DirectScaledTerm(optimizer, std::move(written), cost.sign * term.sign, cost.factor));
		vector<unique_ptr<Expression>> checks;
		if (auto valid = DirectCheckNumber(optimizer, coefficient, cost.names[0], DirectValuesOn(cost, 0),
		                                   source_bindings, p1.index, cost.factor)) {
			checks.push_back(std::move(valid));
		} else {
			// No value can make it past the DOUBLE range, but DuckDB's own evaluation of it can still raise, and only
			// a check that reads the number keeps it computed.
			checks.push_back(calc.Out(
			    calc.Coalesce({calc.Fn("isfinite", calc.Col(LogicalType::DOUBLE, coefficient)), calc.Bool(true)})));
		}
		auto pairs = LogicalCrossProduct::Create(std::move(left.plan), std::move(right.plan));
		return DirectAllRowsValid(optimizer, p1.Finish(std::move(pairs)), DirectAllHold(calc, std::move(checks)));
	}

	//! One side over its read `rows`, whose columns are `columns` of `types` (the side's own bindings, which the
	//! caller re-points at the read): the same columns, then the rank of the row's class by position (greatest first
	//! when `descending`), the number of classes, and whether the row is the one copy of its class that pairs. The
	//! columns are those of the projection `index`.
	unique_ptr<LogicalOperator> Rank(unique_ptr<LogicalOperator> rows, const Side &side, bool descending,
	                                 const vector<ColumnBinding> &columns, const vector<LogicalType> &types,
	                                 idx_t index) const {
		// Rows of one class are peers: the order names the position, then every other column of the key. The key
		// determines the position, so the rows that agree on the key agree on it.
		auto rank_index = binder.GenerateTableIndex();
		auto ranks = make_uniq<LogicalWindow>(rank_index);
		for (auto type :
		     {ExpressionType::WINDOW_ROW_NUMBER, ExpressionType::WINDOW_RANK, ExpressionType::WINDOW_RANK_DENSE}) {
			// Hand-built: a window is an operator-specific node, which the builder (scalar expressions) has no
			// operation for.
			auto window = make_uniq<BoundWindowExpression>(type, LogicalType::BIGINT, nullptr, nullptr);
			window->start = WindowBoundary::UNBOUNDED_PRECEDING;
			window->end = WindowBoundary::UNBOUNDED_FOLLOWING;
			auto order = [&](const LogicalType &column_type, ColumnBinding column, OrderType direction) {
				window->orders.emplace_back(direction, OrderByNullType::NULLS_LAST,
				                            calc.Out(calc.Col(column_type, column)));
			};
			order(side.position_type, side.position, descending ? OrderType::DESCENDING : OrderType::ASCENDING);
			for (idx_t i = 0; i < side.key.size(); i++) {
				if (side.key[i] != side.position) {
					order(side.class_key.types[i], side.key[i], OrderType::ASCENDING);
				}
			}
			ranks->expressions.push_back(std::move(window));
		}
		ranks->children.push_back(std::move(rows));
		// ROW_NUMBER, RANK and DENSE_RANK: the first copy of a class is where the first two agree, and the greatest
		// dense rank counts the classes. The count is a window over the one sorted order, not a second sort.
		auto copy_number = calc.Col(LogicalType::BIGINT, ColumnBinding(rank_index, 0));
		auto copy_rank = calc.Col(LogicalType::BIGINT, ColumnBinding(rank_index, 1));
		auto class_rank = calc.Col(LogicalType::BIGINT, ColumnBinding(rank_index, 2));
		auto count_index = binder.GenerateTableIndex();
		auto counted = make_uniq<LogicalWindow>(count_index);
		counted->expressions.push_back(DirectWindowExtremum(optimizer, "max", calc.Out(class_rank)));
		counted->children.push_back(std::move(ranks));
		DirectProjectionStage ranked(index);
		ranked.Forward(columns, types);
		ranked.Add(class_rank);
		ranked.Add(calc.Col(LogicalType::BIGINT, ColumnBinding(count_index, 0)));
		ranked.Add(calc.Cmp(ExpressionType::COMPARE_EQUAL, copy_number, copy_rank));
		return ranked.Finish(std::move(counted));
	}

	//! The substitute over the `pairs` of the two ranked sides (Rank, with the projections `left` and `right`): the
	//! source columns, then 1 on the one pair of each matched pair of classes, and the class-count check that keeps
	//! the sides' ranks live.
	DirectSubstitute Assign(unique_ptr<LogicalOperator> pairs, idx_t left, idx_t right) const {
		DirectProjectionStage result(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			auto on_left = i < left_width;
			result.Add(calc.Out(
			    calc.Col(source_types[i], ColumnBinding(on_left ? left : right, on_left ? i : i - left_width))));
		}
		// Each ranked side has its rank, class count and first-copy flag after its source columns.
		auto right_width = source_bindings.size() - left_width;
		auto left_rank = calc.Col(LogicalType::BIGINT, ColumnBinding(left, left_width));
		auto right_rank = calc.Col(LogicalType::BIGINT, ColumnBinding(right, right_width));
		auto left_count = calc.Col(LogicalType::BIGINT, ColumnBinding(left, left_width + 1));
		auto right_count = calc.Col(LogicalType::BIGINT, ColumnBinding(right, right_width + 1));
		auto left_first = calc.Col(LogicalType::BOOLEAN, ColumnBinding(left, left_width + 2));
		auto right_first = calc.Col(LogicalType::BOOLEAN, ColumnBinding(right, right_width + 2));
		auto assigned =
		    calc.And(calc.Cmp(ExpressionType::COMPARE_EQUAL, left_rank, right_rank), calc.And(left_first, right_first));
		result.Add(calc.If(assigned, calc.Constant(Value::INTEGER(1)), calc.Constant(Value::INTEGER(0))));
		// The two sides pair one to one only with as many classes on one side as on the other.
		result.Add(DirectRaiseUnless(calc, calc.Out(calc.Cmp(ExpressionType::COMPARE_EQUAL, left_count, right_count)),
		                             DirectInfeasibleErrorPredicate(optimizer, errors)));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		substitute.child = result.Finish(std::move(pairs));
		return substitute;
	}

private:
	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	//! The source's columns, the first `left_width` of them the left side's.
	const vector<ColumnBinding> &source_bindings;
	const vector<LogicalType> &source_types;
	idx_t left_width;
	const DirectStateErrors &errors;
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	// The source is read once, so the reader hands back DECIDE's own cross product. O1's shape has no bound table and
	// no row check, which would be reads of their own and a gate on it. The node stays in the plan as the cross
	// product of the ranked sides: the rule driver keeps a reference to it.
	auto pairs = reader.TakeRecomputedSource();
	D_ASSERT(pairs && pairs->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT && pairs->children.size() == 2);
	// One reader for each side: it reads the side's rows twice, once for the check of the coefficient and once to rank,
	// and makes the choice between a rescan and a copy of n rows for each side.
	DirectSourceReader left(optimizer, std::move(pairs->children[0]), 2, vector<idx_t>());
	DirectSourceReader right(optimizer, std::move(pairs->children[1]), 2, vector<idx_t>());
	auto left_width = left.SourceBindings().size();
	PlanBuilder plan(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), left_width, errors);
	// Verify, then stream: the left side ranks once every pair is known to be valid.
	left.Gate(plan.CheckPairs(left.ReadSource(), right.ReadSource()));
	auto rank = [&](DirectSourceReader &side_reader, const Side &side, bool descending, idx_t index) {
		return DirectBuildOverRead(side_reader, side_reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
			return plan.Rank(std::move(rows), side, descending, side_reader.SourceBindings(), side_reader.SourceTypes(),
			                 index);
		});
	};
	auto left_index = optimizer.binder.GenerateTableIndex();
	auto right_index = optimizer.binder.GenerateTableIndex();
	pairs->children[0] = rank(left, condition.left, false, left_index);
	pairs->children[1] = rank(right, condition.right, condition.opposite, right_index);
	pairs->ResolveOperatorTypes();
	auto substitute = plan.Assign(std::move(pairs), left_index, right_index);
	// The check reads the right side's copy, so that is outermost.
	substitute.child = right.Finish(left.Finish(std::move(substitute.child)));
	return substitute;
}

} // namespace direct_o1_monge_assignment

unique_ptr<DirectSolveRule> MakeO1MongeAssignmentRule() {
	return make_uniq<direct_o1_monge_assignment::Rule>();
}

} // namespace duckdb
