#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <cmath>
#include <limits>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_a2_single_facility_location {

constexpr const char *RULE_NAME = "A2_SINGLE_FACILITY_LOCATION";

//===--------------------------------------------------------------------===//
// Loss: how the rows of a class reduce to the point their objective is smallest at
//===--------------------------------------------------------------------===//

//! What a loss reads of the plan while it is built.
struct PlanTools {
	Optimizer &optimizer;
	const ExpressionBuilder &calc;
	Binder &binder;
	const vector<ColumnBinding> &source_bindings;
	const DirectStateErrors &errors;
};

//! The window over the rows of a row's class: an aggregate added to it holds the class's value on every row.
class ClassWindow {
public:
	ClassWindow(LogicalWindow &window_p, vector<ExprPtr> partitions_p, Optimizer &optimizer_p,
	            const ExpressionBuilder &calc_p)
	    : window(window_p), partitions(std::move(partitions_p)), optimizer(optimizer_p), calc(calc_p) {
	}

	//! The aggregate `function` of a column over the class, on the rows where `reads` holds (null: every row).
	ColumnBinding Add(const char *function, ColumnBinding column, const LogicalType &type,
	                  const ExprPtr &reads = nullptr) const {
		auto aggregate = DirectWindowExtremum(optimizer, function, calc.Out(calc.Col(type, column)));
		if (reads) {
			aggregate->filter_expr = calc.Out(reads);
		}
		Partition(*aggregate);
		window.expressions.push_back(std::move(aggregate));
		return ColumnBinding(window.window_index, window.expressions.size() - 1);
	}

	//! Makes `aggregate` run over the rows of the row's class.
	void Partition(BoundWindowExpression &aggregate) const {
		for (auto &partition : partitions) {
			aggregate.partitions.push_back(calc.Out(partition));
		}
	}

	//! The columns the class is partitioned by, none for a query-wide decision.
	const vector<ExprPtr> &Partitions() const {
		return partitions;
	}

	LogicalWindow &Window() const {
		return window;
	}

private:
	LogicalWindow &window;
	vector<ExprPtr> partitions;
	Optimizer &optimizer;
	const ExpressionBuilder &calc;
};

using OverSource = std::function<unique_ptr<Expression>(const Expression &)>;

//! One loss, a Strategy behind the plan's stage skeleton (PlanBuilder::Build, a Template Method). The skeleton owns
//! what every loss shares: the key, the bounds, the interval, the clamp, the answer of a row with no class and the
//! infeasible error. A loss owns its columns at each stage, given and returned as lists of bindings that only it reads
//! back, and its target: the point of the class it asks for before the clamp.
class Loss {
public:
	virtual ~Loss() = default;

	//! What a loss gives back from its last stage: its target (a point of the class, or NULL when every point is
	//! optimal, which the skeleton reads as 0) and its checks, which raise or warn.
	struct Answer {
		ExprPtr target;
		vector<unique_ptr<Expression>> checks;
	};

	//! P1: the loss's raw values on the rows it reads, each checked as the solver checks it (appended to `checks`).
	//! `over` re-points a value of the source at the plan.
	virtual vector<ColumnBinding> LayoutRaw(const PlanTools &tools, DirectProjectionStage &p1, const OverSource &over,
	                                        vector<unique_ptr<Expression>> &checks) const = 0;
	//! P2: the row's values the class aggregates read, from `raw`. Appends the row's checks.
	virtual vector<ColumnBinding> PerRow(const PlanTools &tools, DirectProjectionStage &p2,
	                                     const vector<ColumnBinding> &raw,
	                                     vector<unique_ptr<Expression>> &checks) const = 0;
	//! W: the class's values, DOUBLE columns, from the aggregates the loss adds to the class window. A loss that
	//! needs a window of its own first puts it over `plan`.
	virtual vector<ColumnBinding> Aggregate(const PlanTools &tools, const ClassWindow &window,
	                                        unique_ptr<LogicalOperator> &plan,
	                                        const vector<ColumnBinding> &row) const = 0;
	//! P4: the target over the class's values, which the skeleton clamps into `[lower, upper]`. `has_class` is false on
	//! a row with no class.
	virtual Answer Finish(const PlanTools &tools, const vector<ExprPtr> &classes, const ExprPtr &lower,
	                      const ExprPtr &upper, const ExprPtr &has_class) const = 0;
};

//! The factor that turns a part's value into a minimum: the sense, the part's sign and its constant factor.
static double Orientation(const DirectIR &ir, const DirectLinearPart &linear) {
	double part_factor = linear.scaled ? (linear.divides ? 1.0 / linear.scale : linear.scale) : 1.0;
	return (ir.objective.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0) * linear.sign * part_factor;
}

//! `weight * (w*c + t)^2` summed over the rows: the objective is `a*c^2 + b*c` with `a` the sum of `weight*w^2` and `b`
//! the sum of `2*weight*w*t`. The weight is a constant, or a value read from the data of either sign. Where `a` is
//! positive the minimum over the interval is the turning point `-b / (2*a)`; otherwise the objective is concave or
//! linear and the minimum is at an end of the interval.
class SquareLoss final : public Loss {
public:
	SquareLoss(DirectQuadraticPart part_p, double sense_p, string written_p)
	    : sense(sense_p), written(std::move(written_p)) {
		parts.push_back(std::move(part_p));
	}

	vector<ColumnBinding> LayoutRaw(const PlanTools &tools, DirectProjectionStage &p1, const OverSource &over,
	                                vector<unique_ptr<Expression>> &checks) const override {
		auto columns = DirectLayoutQuadraticPart(tools.optimizer, tools.calc, p1, parts[0], 1, tools.source_bindings,
		                                         over, checks);
		vector<ColumnBinding> raw;
		if (columns.has_mask) {
			raw.push_back(columns.mask);
		}
		auto &square = columns.squares[0];
		raw.push_back(square.k);
		raw.push_back(square.w);
		raw.push_back(square.t);
		return raw;
	}

	vector<ColumnBinding> PerRow(const PlanTools &tools, DirectProjectionStage &p2, const vector<ColumnBinding> &raw,
	                             vector<unique_ptr<Expression>> &) const override {
		DirectQuadraticColumns laid;
		idx_t at = 0;
		laid.has_mask = parts[0].kept != nullptr;
		if (laid.has_mask) {
			laid.mask = raw[at++];
		}
		DirectQuadraticColumns::Square square;
		square.k = raw[at++];
		square.w = raw[at++];
		square.t = raw[at++];
		laid.squares.push_back(square);
		auto &calc = tools.calc;
		auto sum = DirectFoldQuadratic(tools.optimizer, calc, parts, {laid}, 0);
		return {p2.Add(calc.Out(calc.Mul(calc.Num(sense), sum.a))), p2.Add(calc.Out(calc.Mul(calc.Num(sense), sum.b)))};
	}

	vector<ColumnBinding> Aggregate(const PlanTools &, const ClassWindow &window, unique_ptr<LogicalOperator> &,
	                                const vector<ColumnBinding> &row) const override {
		return {window.Add("sum", row[0], LogicalType::DOUBLE), window.Add("sum", row[1], LogicalType::DOUBLE)};
	}

	Answer Finish(const PlanTools &tools, const vector<ExprPtr> &classes, const ExprPtr &lower, const ExprPtr &upper,
	              const ExprPtr &has_class) const override {
		auto &calc = tools.calc;
		auto &a = classes[0];
		auto &b = classes[1];
		auto turning = DirectTurningPoint(calc, a, b, false);
		Answer answer;
		answer.target = calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, calc.Num(0.0)), turning.target,
		                        EndOfInterval(calc, a, b, lower, upper));
		answer.checks.push_back(DirectRaiseTooLarge(tools.optimizer, a, written));
		answer.checks.push_back(DirectRaiseTooLarge(tools.optimizer, b, written));
		// An infinite target is a best end that is infinite: the objective improves without limit. It follows the
		// checks of the sums, which can themselves be infinite.
		auto infinite = calc.Any({calc.Cmp(ExpressionType::COMPARE_EQUAL, answer.target, calc.Num(POSITIVE_INFINITY)),
		                          calc.Cmp(ExpressionType::COMPARE_EQUAL, answer.target, calc.Num(NEGATIVE_INFINITY))});
		answer.checks.push_back(DirectRaiseIf(calc, calc.Out(calc.And(has_class, infinite)),
		                                      DirectUnboundedErrorPredicate(tools.optimizer, tools.errors)));
		answer.checks.push_back(DirectWarnIf(calc, calc.Out(turning.loses_precision)));
		return answer;
	}

private:
	//! The minimum of `a*c^2 + b*c` over `[lower, upper]` for `a <= 0`, where the objective is concave or linear and
	//! the minimum is at an end: the upper one when `f(U) - f(L) = (U - L) * (a*(L + U) + b)` is negative, the lower
	//! one when it is positive (a NaN counts as positive: it comes from two infinite ends, where either is unbounded),
	//! and on a tie the end nearest 0. With `a = 0` the factor is `b` alone, since `0 * infinity` is not a number, and
	//! a tie
	//! (`b = 0`) leaves every point optimal, so the target is 0 for the skeleton's clamp to bring nearest.
	static ExprPtr EndOfInterval(const ExpressionBuilder &calc, const ExprPtr &a, const ExprPtr &b,
	                             const ExprPtr &lower, const ExprPtr &upper) {
		auto zero = calc.Num(0.0);
		auto flat = calc.Cmp(ExpressionType::COMPARE_EQUAL, a, zero);
		auto upper_gain = calc.If(flat, b, calc.Add(calc.Mul(a, calc.Add(lower, upper)), b));
		auto nearer_lower =
		    calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, calc.Fn("abs", lower), calc.Fn("abs", upper));
		auto tie = calc.If(flat, zero, calc.If(nearer_lower, lower, upper));
		return calc.If(calc.Cmp(ExpressionType::COMPARE_LESSTHAN, upper_gain, zero), upper,
		               calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, upper_gain, zero), lower, tie));
	}

	//! The one part of the objective, in a vector because DirectFoldQuadratic reads a list of parts.
	vector<DirectQuadraticPart> parts;
	//! The sense as a factor: -1 for a maximum.
	double sense;
	//! The objective as a message names it: a number computed past a DOUBLE names it, as the solver path names one.
	string written;
};

//! `K * ABS(s*c + t)` on each row, the term of the absolute and the maximum absolute loss: a row has a center
//! `v = -t/s` and a weight `m = K*|s|`, with `K` the term's weight oriented as a minimum. A row the objective drops, or
//! whose NULL a guard skipped, has no center and no weight. The term is laid out as a square is (the same weight,
//! slope and constant), which the shared layout does.
class CenteredLoss : public Loss {
public:
	CenteredLoss(DirectQuadraticPart term_p, double weight_p) : term(std::move(term_p)), weight(weight_p) {
	}

	//! The columns of a square: the mask when the part has one, then the slope and the constant.
	vector<ColumnBinding> LayoutRaw(const PlanTools &tools, DirectProjectionStage &p1, const OverSource &over,
	                                vector<unique_ptr<Expression>> &checks) const override {
		auto columns =
		    DirectLayoutQuadraticPart(tools.optimizer, tools.calc, p1, term, 1, tools.source_bindings, over, checks);
		vector<ColumnBinding> raw;
		if (columns.has_mask) {
			raw.push_back(columns.mask);
		}
		raw.push_back(columns.squares[0].w);
		raw.push_back(columns.squares[0].t);
		return raw;
	}

	//! The row's center and weight. The center divides by the slope, so it warns where the slope, or a nonzero constant
	//! beside it, is below the normal range (DirectTurningPoint's rule).
	vector<ColumnBinding> PerRow(const PlanTools &tools, DirectProjectionStage &p2, const vector<ColumnBinding> &raw,
	                             vector<unique_ptr<Expression>> &checks) const override {
		auto &calc = tools.calc;
		idx_t at = term.kept ? 1 : 0;
		auto slope = calc.Col(LogicalType::DOUBLE, raw[at]);
		auto constant = calc.Col(LogicalType::DOUBLE, raw[at + 1]);
		auto zero = calc.Num(0.0);
		auto present = calc.And(calc.Not(calc.IsNull(slope)), calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, slope, zero));
		auto center = calc.If(present, calc.Div(calc.Neg(constant), slope), calc.Constant(Value(LogicalType::DOUBLE)));
		auto row_weight = calc.If(present, calc.Mul(calc.Num(weight), calc.Fn("abs", slope)), zero);
		auto small_constant = calc.And(calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, constant, zero),
		                               DirectBelowNormalRange(calc, constant));
		checks.push_back(DirectWarnIf(
		    calc, calc.Out(calc.And(present, calc.Any({DirectBelowNormalRange(calc, slope), small_constant})))));
		return {p2.Add(calc.Out(center)), p2.Add(calc.Out(row_weight))};
	}

private:
	DirectQuadraticPart term;
	//! `K`: the weight of the term oriented as a minimum, positive.
	double weight;
};

//! `SUM(K * ABS(s*c + t))`: the minimum is at a median of the centers `v = -t/s`, each weighted by `m = K*|s|`. Every
//! point between the lower and the upper weighted median is optimal; the answer is the one nearest 0.
//!
//! A running sum `R` of the weights in order of the centers sits beside the class's total `M`, on one sort. The lower
//! median is the least center where `2R >= M`: the weight up to it is at least half. The upper median is the greatest
//! center where the weight from it on, `M - R + m`, is at least half.
class AbsoluteLoss final : public CenteredLoss {
public:
	AbsoluteLoss(DirectQuadraticPart term_p, double weight_p, string written_p)
	    : CenteredLoss(std::move(term_p), weight_p), written(std::move(written_p)) {
	}

	vector<ColumnBinding> Aggregate(const PlanTools &tools, const ClassWindow &window,
	                                unique_ptr<LogicalOperator> &plan,
	                                const vector<ColumnBinding> &row) const override {
		auto &calc = tools.calc;
		auto center = calc.Col(LogicalType::DOUBLE, row[0]);
		auto weight = calc.Col(LogicalType::DOUBLE, row[1]);
		// A window of its own under the class window: both aggregates share the class and its order of centers. A row
		// with no center sorts last, and its weight is 0.
		auto sorted = make_uniq<LogicalWindow>(tools.binder.GenerateTableIndex());
		auto total = DirectWindowExtremum(tools.optimizer, "sum", calc.Out(weight));
		window.Partition(*total);
		total->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, calc.Out(center));
		sorted->expressions.push_back(std::move(total));
		sorted->expressions.push_back(
		    DirectRunningSum(tools.optimizer, calc, weight, center, OrderType::ASCENDING, window.Partitions()));
		ColumnBinding class_weight(sorted->window_index, 0);
		auto up_to = calc.Col(LogicalType::DOUBLE, ColumnBinding(sorted->window_index, 1));
		sorted->children.push_back(std::move(plan));
		plan = std::move(sorted);

		auto total_weight = calc.Col(LogicalType::DOUBLE, class_weight);
		auto two = calc.Num(2.0);
		auto from_here = calc.Add(calc.Sub(total_weight, up_to), weight);
		auto lower =
		    window.Add("min", row[0], LogicalType::DOUBLE,
		               calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, calc.Mul(two, up_to), total_weight));
		auto upper =
		    window.Add("max", row[0], LogicalType::DOUBLE,
		               calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, calc.Mul(two, from_here), total_weight));
		return {class_weight, lower, upper};
	}

	Answer Finish(const PlanTools &tools, const vector<ExprPtr> &classes, const ExprPtr &, const ExprPtr &,
	              const ExprPtr &) const override {
		auto &calc = tools.calc;
		Answer answer;
		// The point of the optimal medians nearest 0. With no weight on any row of the class, both medians are NULL,
		// which the extrema skip: the target is 0.
		answer.target = calc.Min({calc.Max({calc.Num(0.0), classes[1]}), classes[2]});
		answer.checks.push_back(DirectRaiseTooLarge(tools.optimizer, classes[0], written));
		return answer;
	}

private:
	string written;
};

//===--------------------------------------------------------------------===//
// Condition
//===--------------------------------------------------------------------===//

//! What CheckCondition proved: one query-wide or keyed REAL decision `c`, bounded by constants and by per-row bounds
//! that each class of the key intersects, and an objective that is one loss of the distance between `c` and a source
//! value on each row the objective keeps: a square, an absolute value or a maximum absolute value (Loss). A keyed
//! decision has one such problem per class of its key.
struct Condition final : DirectApplicabilityCondition {
	//! The box before any per-row bound (DirectDecisionBox): the default floor of 0 or the written lower bound, and the
	//! written upper bound.
	double lower = 0.0;
	double upper = POSITIVE_INFINITY;
	//! The bounds the plan evaluates per row: read from the data, or under a `when`. A class's interval is the box
	//! tightened by the tightest end of each clause over the rows of the class.
	vector<DirectBoundClause> clauses;
	//! The decision's key, empty for a query-wide decision (one class). A row with a NULL in a `no_group` element is in
	//! no class: it has no decision, and the front end already leaves it out of the objective's `kept` rows.
	vector<DirectKeyElement> keys;
	unique_ptr<Loss> loss;
};

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.decisions.size() != 1 || ir.decisions[0].domain != DirectDomain::REAL ||
		    ir.decisions[0].scope == DecideVarScope::ROW) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		if (ir.decisions[0].scope == DecideVarScope::KEYED) {
			// A key's columns are columns of the source (the IR reads them as a row check would), so a copy is all it takes.
			for (auto &element : ir.keyed_scopes[ir.decisions[0].keyed_scope].key) {
				condition->keys.push_back(element.Copy());
			}
		}
		if (!ProveInterval(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}

	string Summary() const override {
		return "min SUM((c - v)^2), MAX(lo) <= c <= MIN(hi), one shared c for each class";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveInterval(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! Every constraint is a bound on `c`. The solver folds the ones known before the query runs into the decision's box,
//! and the rest, read from the data or under a `when`, bind the same decision on each row they keep, so a class's
//! interval is their intersection. An empty interval is not declined: the plan raises the solver's infeasible error.
bool Rule::ProveInterval(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	for (auto &ir_node : ir.constraints) {
		if (!DirectIsSingletonRow(ir, ir_node)) {
			return false;
		}
	}
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading)) {
		return false;
	}
	condition.lower = reading.boxes[0].lower;
	condition.upper = reading.boxes[0].upper;
	condition.clauses = std::move(reading.clauses);
	return true;
}

//! The square is convex in the sense: its factor `K * w^2`, with the sense, the part's sign and its constant factor
//! applied, is positive. A weight read from the data has no sign before the rows are read, so the check is left to the
//! plan, which reads the sign of the total.
static bool SquareIsConvex(ClientContext &context, const DirectIR &ir, const DirectQuadraticPart &part, double slope) {
	double weight;
	if (!DirectFoldConstantSum(context, part.squares[0].weight, weight)) {
		return true;
	}
	double oriented = Orientation(ir, part.linear) * (weight * (slope * slope));
	return std::isfinite(oriented) && oriented > 0.0;
}

//! One plain `SUM` of one square, and nothing else: no linear term, no per-row constant. The square's slope folds to a
//! nonzero constant, and its weight is a constant that makes it convex (SquareIsConvex) or a value read from the data.
//! The rows it reads (the objective's `when` and the part's own) are the part's `kept`.
static unique_ptr<Loss> ProveSquare(const DirectIR &ir, ClientContext &context, const DirectPart &part_ir) {
	DirectQuadraticPart part;
	if (!part_ir.IsPlainSum() || !DirectReadQuadraticPart(ir, context, part_ir, ir.objective.written, part)) {
		return nullptr;
	}
	if (part.squares.size() != 1 || !part.linear.terms.empty() || !part.constants.empty()) {
		return nullptr;
	}
	double slope;
	if (!DirectFoldConstantSum(context, part.squares[0].inner.linear, slope) || slope == 0.0 ||
	    !SquareIsConvex(context, ir, part, slope)) {
		return nullptr;
	}
	double sense = ir.objective.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0;
	return make_uniq<SquareLoss>(std::move(part), sense, ir.objective.written);
}

//! The one `ABS` of a part, read like a square so the shared layout of a square's columns lays it out: the weight, the
//! slope and the constant of `K * ABS(s*c + t)`. `weight` is `K` oriented as a minimum and `slope` is `s`. False when
//! `s` is not a nonzero constant, or `K` is not a constant that is positive once the sense is applied: a weight read
//! from the data is the solver's. The part holds the `ABS` and no other term (ProveObjective).
static bool ReadAbsolute(const DirectIR &ir, ClientContext &context, const DirectPart &part_ir,
                         DirectQuadraticPart &part, double &weight, double &slope) {
	DirectAbsTerm term;
	if (!DirectReadLinearPart(ir, context, part_ir, ir.objective.written, part.linear) ||
	    !DirectObjectivePartKept(ir, part_ir, part.kept) ||
	    !DirectReadAbsTerm(ir, part_ir.terms[0], ir.objective.written, term)) {
		return false;
	}
	DirectSquareTerm as_square;
	as_square.weight = std::move(term.weight);
	as_square.weight_names = std::move(term.weight_names);
	as_square.inner = std::move(term.inner);
	part.squares.push_back(std::move(as_square));
	auto &read = part.squares[0];
	double constant;
	if (!DirectFoldConstantSum(context, read.weight, constant) ||
	    !DirectFoldConstantSum(context, read.inner.linear, slope) || slope == 0.0) {
		return false;
	}
	weight = Orientation(ir, part.linear) * constant;
	return std::isfinite(weight) && weight > 0.0;
}

//! One plain `SUM` of one `ABS`. The weight of a center, `K * |s|`, is a normal DOUBLE: a smaller one has lost the bits
//! that order the medians.
static unique_ptr<Loss> ProveAbsolute(const DirectIR &ir, ClientContext &context, const DirectPart &part_ir) {
	DirectQuadraticPart part;
	double weight, slope;
	if (!part_ir.IsPlainSum() || !ReadAbsolute(ir, context, part_ir, part, weight, slope)) {
		return nullptr;
	}
	double center_weight = weight * std::fabs(slope);
	if (!std::isfinite(center_weight) || center_weight < std::numeric_limits<double>::min()) {
		return nullptr;
	}
	return make_uniq<AbsoluteLoss>(std::move(part), weight, ir.objective.written);
}

//! The objective is one part holding one loss: a square or an `ABS` summed. The part holds nothing beside its loss, and
//! its rows are the ones the objective's `when` and the part's own keep.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	if (ir.objective.parts.size() != 1) {
		return false;
	}
	auto &part = ir.objective.parts[0];
	if (part.terms.size() != 1 || !part.terms[0].IsAbs()) {
		condition.loss = ProveSquare(ir, context, part);
	} else {
		condition.loss = ProveAbsolute(ir, context, part);
	}
	return condition.loss != nullptr;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! Builds the relational plan for one A2 problem whose condition holds, over one read of the source, as the skeleton of
//! a Template Method; the loss (Loss) fills in its stages. Each stage reads only the columns of the stage before it:
//!
//!   scope the row's class: whether it has one, and its key (DirectProjectScope), for a keyed decision
//!   P1    raw values: the loss's values on the rows the objective keeps; each bound clause's ends on the rows it
//!         keeps; the check of every value that can fail
//!   P2    the loss's values for the row (a square's `a` and `b`, a center and its weight), each clause's lower and
//!         upper end on the row (no bound where its `when` drops the row), and the row's checks
//!   W     over the rows of the row's class (one window partitioned by the key): the loss's aggregates, the MAX of
//!         each clause's lower ends and the MIN of its upper ends, and whether a floor-lifting clause keeps some row
//!   P3    the class's interval as columns: each clause's tightest ends, and the floor
//!   P4    the answer: the loss's target clamped into the interval (the box of DirectRowBox over the columns of P3),
//!         repeated on every row of the class, and the column that keeps the checks live
//!
//! Aggregate-then-repeat: the window is the aggregate and the row's own columns are what it repeats on, so the source
//! is read once. A query-wide decision is the partition with no key. The window holds every row before it releases one,
//! so each check on a row (P2) has run on all of them first, as the solver reads every row before it returns.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p),
	      source_types(source_types_p), tools {optimizer, calc, binder, source_bindings, errors_p}, errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> rows) const {
		auto state = DirectProjectScope(binder, std::move(rows), IsKeyed(), nullptr, condition.keys, {});
		Carried from_scope;
		from_scope.source = state.input_bindings;
		from_scope.eligible = state.eligible;
		from_scope.keys = state.keys;
		from_scope.key_types = state.key_types;

		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto raw = LayoutRawValues(p1, from_scope, state.state_index);
		auto plan = p1.Finish(std::move(state.plan));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto coefficients = ComputeRows(p2, raw);
		plan = p2.Finish(std::move(plan));

		auto aggregates = AggregateClasses(coefficients, plan);

		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto interval = ComputeInterval(p3, aggregates);
		plan = p3.Finish(std::move(plan));

		DirectProjectionStage p4(binder.GenerateTableIndex());
		ComputeAnswer(p4, interval);
		plan = p4.Finish(std::move(plan));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		// The source columns, then c. The window holds every row, and the validation column reads it.
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		substitute.child = std::move(plan);
		return substitute;
	}

private:
	//! The columns every stage forwards.
	struct Carried {
		vector<ColumnBinding> source;
		//! Set only for a keyed decision.
		ColumnBinding eligible;
		vector<ColumnBinding> keys;
		//! The types of `keys`.
		vector<LogicalType> key_types;
	};
	//! Columns of P1.
	struct Raw {
		Carried carried;
		vector<DirectBoundColumns> clauses;
		//! The loss's raw columns.
		vector<ColumnBinding> loss;
		vector<unique_ptr<Expression>> checks;
	};
	//! A clause's lower and upper end on the row (DirectClauseBounds), and the row's `when` for a clause that lifts the
	//! floor.
	struct ClauseEnds {
		ColumnBinding lower;
		ColumnBinding upper;
		ColumnBinding when;
	};
	//! Columns of P2.
	struct Rows {
		Carried carried;
		vector<ClauseEnds> ends;
		//! The loss's columns for the row.
		vector<ColumnBinding> loss;
		ColumnBinding valid;
	};
	//! Columns of W: the class's values and the aggregates of its clauses, over the results.
	struct Aggregates {
		Carried carried;
		ColumnBinding valid;
		//! The loss's class values, DOUBLE.
		vector<ColumnBinding> classes;
		vector<DirectGroupValues> lowers;
		vector<DirectGroupValues> uppers;
		//! Whether some row of a floor-lifting clause is kept, one per such clause.
		vector<ColumnBinding> lowering_whens;
	};
	//! Columns of P3.
	struct Interval {
		Carried carried;
		ColumnBinding valid;
		vector<ColumnBinding> classes;
		vector<ColumnBinding> clause_lowers;
		vector<ColumnBinding> clause_uppers;
		ColumnBinding floor;
	};

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	PlanTools tools;
	const DirectStateErrors &errors;

	bool IsKeyed() const {
		return !condition.keys.empty();
	}

	Carried Forward(DirectProjectionStage &stage, const Carried &from) const {
		Carried to;
		for (idx_t i = 0; i < from.source.size(); i++) {
			to.source.push_back(stage.Add(calc.Out(calc.Col(source_types[i], from.source[i]))));
		}
		if (IsKeyed()) {
			to.eligible = stage.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, from.eligible)));
		}
		for (idx_t i = 0; i < from.keys.size(); i++) {
			to.keys.push_back(stage.Add(calc.Out(calc.Col(from.key_types[i], from.keys[i]))));
		}
		to.key_types = from.key_types;
		return to;
	}

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	Raw LayoutRawValues(DirectProjectionStage &p1, const Carried &from_scope, idx_t state_index) const {
		Raw raw;
		raw.carried = Forward(p1, from_scope);
		OverSource over_state = [&](const Expression &value) {
			return state_index == DConstants::INVALID_INDEX ? value.Copy()
			                                                : DirectOverProjection(value, source_bindings, state_index);
		};
		raw.clauses = DirectLayoutBoundClauses(optimizer, calc, p1, condition.clauses, source_bindings, over_state,
		                                       raw.checks);
		raw.loss = condition.loss->LayoutRaw(tools, p1, over_state, raw.checks);
		return raw;
	}

	//===------------------------------------------------------------------===//
	// P2: the loss's values for the row, each clause's ends, and the checks
	//===------------------------------------------------------------------===//

	Rows ComputeRows(DirectProjectionStage &p2, Raw &raw) const {
		Rows result;
		result.carried = Forward(p2, raw.carried);
		for (auto &columns : raw.clauses) {
			ExprPtr lower, upper;
			DirectClauseBounds(calc, columns, lower, upper);
			ClauseEnds ends;
			ends.lower = p2.Add(calc.Out(lower));
			ends.upper = p2.Add(calc.Out(upper));
			if (columns.has_when) {
				ends.when = p2.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, columns.when)));
			}
			result.ends.push_back(ends);
		}
		result.loss = condition.loss->PerRow(tools, p2, raw.loss, raw.checks);
		// Every check is true or raises; a row passes when all of them do.
		result.valid = p2.Add(DirectAllHold(calc, std::move(raw.checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// W: the aggregates of a class
	//===------------------------------------------------------------------===//

	//! Puts one window over `plan` (the loss may put its own under it first): the loss's aggregates, and for each
	//! clause the MAX of its lower ends and the MIN of its upper ends (DirectTightestBound), and whether some row a
	//! floor-lifting clause keeps exists. A row with no class sits in a partition of its own, and its answer is NULL
	//! whatever the partition holds.
	Aggregates AggregateClasses(const Rows &rows, unique_ptr<LogicalOperator> &plan) const {
		auto window = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		auto &carried = rows.carried;
		vector<ExprPtr> partitions;
		for (idx_t i = 0; i < carried.keys.size(); i++) {
			partitions.push_back(calc.Col(carried.key_types[i], carried.keys[i]));
		}
		ClassWindow class_window(*window, std::move(partitions), optimizer, calc);
		Aggregates result;
		result.carried = carried;
		result.valid = rows.valid;
		result.classes = condition.loss->Aggregate(tools, class_window, plan, rows.loss);
		DirectWindowSink sink(*window, [&](BoundWindowExpression &aggregate) { class_window.Partition(aggregate); });
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			auto &ends = rows.ends[j];
			result.lowers.push_back(
			    TightestEnd(sink, ExpressionType::COMPARE_GREATERTHANOREQUALTO, *clause.lower, ends.lower));
			result.uppers.push_back(
			    TightestEnd(sink, ExpressionType::COMPARE_LESSTHANOREQUALTO, *clause.upper, ends.upper));
			if (clause.lifts_floor) {
				result.lowering_whens.push_back(class_window.Add("bool_or", ends.when, LogicalType::BOOLEAN));
			}
		}
		window->children.push_back(std::move(plan));
		plan = std::move(window);
		return result;
	}

	//! The tightest value of a clause's end `column` (of the admitted end `end`) over the class: the MAX of lower ends
	//! for `>=`, the MIN of upper ends for `<=`.
	DirectGroupValues TightestEnd(DirectGroupSink &sink, ExpressionType comparison, const Expression &end,
	                              ColumnBinding column) const {
		auto bound = DirectTightestBound(optimizer, comparison, end.Copy(), string(), nullptr);
		vector<DirectGroupInputColumns> input(1);
		input[0].argument = column;
		return DirectAddGroupBound(calc, *bound, input, sink, nullptr);
	}

	//===------------------------------------------------------------------===//
	// P3: the class's interval as columns
	//===------------------------------------------------------------------===//

	//! The loss's class values, each clause's tightest ends, and the static floor, lowered when a row a floor-lifting
	//! clause keeps exists.
	Interval ComputeInterval(DirectProjectionStage &p3, const Aggregates &aggregates) const {
		Interval result;
		result.carried = Forward(p3, aggregates.carried);
		result.valid = p3.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, aggregates.valid)));
		for (auto &value : aggregates.classes) {
			result.classes.push_back(p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, value))));
		}
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			result.clause_lowers.push_back(p3.Add(calc.Out(calc.Cast(aggregates.lowers[j].lower, LogicalType::DOUBLE))));
			result.clause_uppers.push_back(p3.Add(calc.Out(calc.Cast(aggregates.uppers[j].upper, LogicalType::DOUBLE))));
		}
		result.floor = p3.Add(calc.Out(DirectStaticFloor(calc, condition.lower, aggregates.lowering_whens)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P4: the answer
	//===------------------------------------------------------------------===//

	void ComputeAnswer(DirectProjectionStage &p4, const Interval &interval) const {
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p4.Add(calc.Out(calc.Col(source_types[i], interval.carried.source[i])));
		}
		vector<ExprPtr> classes;
		for (auto &binding : interval.classes) {
			classes.push_back(calc.Col(LogicalType::DOUBLE, binding));
		}
		ExprPtr lower, upper;
		DirectRowBox(calc, calc.Col(LogicalType::DOUBLE, interval.floor), condition.upper, interval.clause_lowers,
		             interval.clause_uppers, lower, upper);
		// A row with no class has no decision and shows NULL.
		auto has_class = IsKeyed() ? calc.Col(LogicalType::BOOLEAN, interval.carried.eligible) : calc.Bool(true);
		auto finish = condition.loss->Finish(tools, classes, lower, upper, has_class);
		// The loss's target clamped into the interval. A NULL target leaves every point optimal, and the point of the
		// interval nearest 0 is the answer: the clamp of 0. Adding 0.0 turns -0.0 into 0.0.
		auto zero = calc.Num(0.0);
		auto answer = calc.Add(calc.Min({calc.Max({calc.Coalesce({finish.target, zero}), lower}), upper}), zero);
		p4.Add(calc.Out(calc.If(has_class, answer, calc.Constant(Value(LogicalType::DOUBLE)))));

		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, interval.valid)));
		// An empty interval raises on every row that has a class. The interval of a class is the same on every row of
		// it, so a query whose only bounds are constants has two constants here, which DuckDB folds.
		checks.push_back(DirectRaiseIf(calc, calc.Out(calc.And(has_class, DirectBoxIsEmpty(calc, lower, upper))),
		                               DirectInfeasibleErrorPredicate(optimizer, errors)));
		for (auto &check : finish.checks) {
			checks.push_back(std::move(check));
		}
		p4.Add(DirectAllHold(calc, std::move(checks)));
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_a2_single_facility_location

unique_ptr<DirectSolveRule> MakeA2SingleFacilityLocationRule() {
	return make_uniq<direct_a2_single_facility_location::Rule>();
}

} // namespace duckdb
