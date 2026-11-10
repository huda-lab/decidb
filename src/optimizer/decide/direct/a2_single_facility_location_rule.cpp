#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <cmath>
#include <limits>
#include <tuple>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

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
	//! The objective as a message names it: a number computed past a DOUBLE names it, as the solver path names one.
	const string &written;
};

//! The hash aggregate over the rows of a class: an aggregate added to it holds the class's value on the class's one
//! row (group-then-join, DirectJoinClassAnswers).
class ClassAggregate {
public:
	ClassAggregate(DirectAggregateStage &aggregate_p, Optimizer &optimizer_p, const ExpressionBuilder &calc_p)
	    : aggregate(aggregate_p), optimizer(optimizer_p), calc(calc_p) {
	}

	//! The aggregate `function` of a column over the class.
	ColumnBinding Add(const char *function, ColumnBinding column, const LogicalType &type) const {
		return aggregate.Add(DirectHashAggregate(optimizer, calc, function, calc.Col(type, column)));
	}

	//! The discrete median of a DOUBLE `value` over the class: the `ceil(n/2)`-th least of its `n` non-NULL values,
	//! NULL when there is none (`quantile_disc` at one half).
	ColumnBinding AddLowerMedian(const ExprPtr &value) const {
		return aggregate.Add(DirectHashAggregate(optimizer, calc, "quantile_disc", value, calc.Num(0.5)));
	}

private:
	DirectAggregateStage &aggregate;
	Optimizer &optimizer;
	const ExpressionBuilder &calc;
};

using OverSource = std::function<unique_ptr<Expression>(const Expression &)>;

//! The columns a loss keeps from stage to stage, by name: one subclass per loss. The skeleton makes one for the plan
//! (Loss::NewColumns) and hands it to every stage without reading it, as DuckDB hands a GlobalSinkState to a sink; a
//! loss casts it back to its own. A field holds the binding of the latest stage that wrote it.
struct LossColumns {
	virtual ~LossColumns() = default;

	template <class T>
	T &Cast() {
		DynamicCastCheck<T>(this);
		return reinterpret_cast<T &>(*this);
	}
};

//! One loss, a Strategy behind the plan's stage skeleton (PlanBuilder::Build, a Template Method). The skeleton owns
//! what every loss shares: the key, the bounds, the interval, the clamp, the answer of a row with no class, the
//! infeasible error and the too-large check of the answer. A loss owns its columns at each stage (LossColumns) and its
//! target: the point of the class it asks for before the clamp.
class Loss {
public:
	virtual ~Loss() = default;

	//! What a loss gives back from its last stage: its target (a point of the class, or NULL when every point is
	//! optimal, which the skeleton reads as 0), its checks, which raise, and its warnings. The skeleton runs the
	//! checks, then the too-large check of the clamped answer, then the warnings, so the first error to raise is the
	//! same whatever the loss.
	struct Answer {
		ExprPtr target;
		//! Where the clamped answer must be a number a DOUBLE holds, or null for no such check. A target past a DOUBLE
		//! clamps to a finite end of the interval, and with that end open the answer is past a DOUBLE.
		ExprPtr finite_where;
		vector<unique_ptr<Expression>> checks;
		vector<unique_ptr<Expression>> warnings;
	};

	//! The loss's columns, empty, for the stages below to fill.
	virtual unique_ptr<LossColumns> NewColumns() const = 0;
	//! P1: the loss's raw values on the rows it reads, each checked as the solver checks it (appended to `checks`).
	//! `over` re-points a value of the source at the plan.
	virtual void LayoutRaw(const PlanTools &tools, DirectProjectionStage &p1, const OverSource &over,
	                       LossColumns &columns, vector<unique_ptr<Expression>> &checks) const = 0;
	//! P2: the row's values the class aggregates read, from the raw values. Appends the row's checks.
	virtual void PerRow(const PlanTools &tools, DirectProjectionStage &p2, LossColumns &columns,
	                    vector<unique_ptr<Expression>> &checks) const = 0;
	//! W: the class's values, DOUBLE columns, from the aggregates the loss adds to the class aggregate.
	virtual void Aggregate(const PlanTools &tools, const ClassAggregate &classes, LossColumns &columns) const = 0;
	//! P3: forwards the class's values through the interval's projection, which is all P4 reads.
	virtual void CarryClasses(const PlanTools &tools, DirectProjectionStage &p3, LossColumns &columns) const = 0;
	//! P4: the target over the class's values, which the skeleton clamps into `[lower, upper]`. `has_class` is false on
	//! a row with no class.
	virtual Answer Finish(const PlanTools &tools, LossColumns &columns, const ExprPtr &lower, const ExprPtr &upper,
	                      const ExprPtr &has_class) const = 0;
};

//! Forwards a DOUBLE column of the class through P3, and names it by its binding there.
static void CarryDouble(const ExpressionBuilder &calc, DirectProjectionStage &p3, ColumnBinding &column) {
	column = p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, column)));
}

//! Appends `from` to `to`.
static void MoveInto(vector<unique_ptr<Expression>> &to, vector<unique_ptr<Expression>> &from) {
	for (auto &item : from) {
		to.push_back(std::move(item));
	}
}

//! The columns of the squared loss.
struct SquareColumns final : LossColumns {
	//! P1: the part as DirectLayoutQuadraticPart lays it out.
	DirectQuadraticColumns laid;
	//! P2: the row's `a` and `b`.
	ColumnBinding row_a;
	ColumnBinding row_b;
	//! W, then P3: the class's `a` and `b`.
	ColumnBinding a;
	ColumnBinding b;
};

//! `weight * (w*c + t)^2` summed over the rows: the objective is `a*c^2 + b*c` with `a` the sum of `weight*w^2` and `b`
//! the sum of `2*weight*w*t`. The weight is a constant, or a value read from the data of either sign. Where `a` is
//! positive the minimum over the interval is the turning point `-b / (2*a)`; otherwise the objective is concave or
//! linear and the minimum is at an end of the interval.
class SquareLoss final : public Loss {
public:
	explicit SquareLoss(DirectQuadraticPart part_p) {
		parts.push_back(std::move(part_p));
	}

	unique_ptr<LossColumns> NewColumns() const override {
		return make_uniq<SquareColumns>();
	}

	void LayoutRaw(const PlanTools &tools, DirectProjectionStage &p1, const OverSource &over, LossColumns &columns,
	               vector<unique_ptr<Expression>> &checks) const override {
		columns.Cast<SquareColumns>().laid = DirectLayoutQuadraticPart(tools.optimizer, tools.calc, p1, parts[0], 1,
		                                                               tools.source_bindings, over, checks);
	}

	void PerRow(const PlanTools &tools, DirectProjectionStage &p2, LossColumns &columns,
	            vector<unique_ptr<Expression>> &) const override {
		auto &square = columns.Cast<SquareColumns>();
		auto &calc = tools.calc;
		auto sum = DirectFoldQuadratic(tools.optimizer, calc, parts, {square.laid}, 0);
		square.row_a = p2.Add(calc.Out(sum.a));
		square.row_b = p2.Add(calc.Out(sum.b));
	}

	void Aggregate(const PlanTools &, const ClassAggregate &classes, LossColumns &columns) const override {
		auto &square = columns.Cast<SquareColumns>();
		square.a = classes.Add("sum", square.row_a, LogicalType::DOUBLE);
		square.b = classes.Add("sum", square.row_b, LogicalType::DOUBLE);
	}

	void CarryClasses(const PlanTools &tools, DirectProjectionStage &p3, LossColumns &columns) const override {
		auto &square = columns.Cast<SquareColumns>();
		CarryDouble(tools.calc, p3, square.a);
		CarryDouble(tools.calc, p3, square.b);
	}

	Answer Finish(const PlanTools &tools, LossColumns &columns, const ExprPtr &lower, const ExprPtr &upper,
	              const ExprPtr &has_class) const override {
		auto &calc = tools.calc;
		auto &square = columns.Cast<SquareColumns>();
		auto a = calc.Col(LogicalType::DOUBLE, square.a);
		auto b = calc.Col(LogicalType::DOUBLE, square.b);
		auto turning = DirectTurningPoint(calc, a, b, false);
		Answer answer;
		auto convex = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, calc.Num(0.0));
		auto end = EndOfInterval(calc, a, b, lower, upper);
		answer.target = calc.If(convex, turning.target, end);
		answer.finite_where = convex;
		answer.checks.push_back(DirectRaiseTooLarge(tools.optimizer, a, tools.written));
		answer.checks.push_back(DirectRaiseTooLarge(tools.optimizer, b, tools.written));
		// A concave or linear objective whose best end is infinite improves without limit. It follows the checks of the
		// sums, which can themselves be infinite.
		auto infinite = calc.Any({calc.Cmp(ExpressionType::COMPARE_EQUAL, end, calc.Num(POSITIVE_INFINITY)),
		                          calc.Cmp(ExpressionType::COMPARE_EQUAL, end, calc.Num(NEGATIVE_INFINITY))});
		answer.checks.push_back(DirectRaiseIf(calc, calc.Out(calc.And(has_class, calc.And(calc.Not(convex), infinite))),
		                                      DirectUnboundedErrorPredicate(tools.optimizer, tools.errors)));
		answer.warnings.push_back(DirectWarnIf(calc, calc.Out(turning.loses_precision)));
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
};

//! The columns of a loss over centers: a term's slope and constant (P1), then the row's center and weight (P2).
struct CenteredColumns : LossColumns {
	ColumnBinding slope;
	//! The term's constant `t`, negated.
	ColumnBinding minus_constant;
	ColumnBinding center;
	ColumnBinding weight;
};

//! The class's total weight and its lower and upper median (W, then P3).
struct MedianColumns final : CenteredColumns {
	ColumnBinding total;
	ColumnBinding lower;
	ColumnBinding upper;
};

//! The class's least and greatest center (W, then P3).
struct ExtremeColumns final : CenteredColumns {
	ColumnBinding least;
	ColumnBinding greatest;
};

//! `K * ABS(s*c + t)` on each row, the term of the absolute and the maximum absolute loss: a row has a center
//! `v = -t/s` and a weight `m = K*|s|`, with `K` the term's weight oriented as a minimum. A row the objective drops, or
//! whose NULL a guard skipped, has no center and no weight. The shared layout of a part lays the term out
//! (DirectLayoutQuadraticPart): its weight, slope and constant.
class CenteredLoss : public Loss {
public:
	CenteredLoss(DirectQuadraticPart term_p, double weight_p) : term(std::move(term_p)), weight(weight_p) {
	}

	//! The term's slope and constant.
	void LayoutRaw(const PlanTools &tools, DirectProjectionStage &p1, const OverSource &over, LossColumns &columns,
	               vector<unique_ptr<Expression>> &checks) const override {
		// The center is -t / w, so the constant is laid out negated and no column is negated again above it.
		auto laid = DirectLayoutQuadraticPart(tools.optimizer, tools.calc, p1, term, 1, tools.source_bindings, over,
		                                      checks, -1);
		auto &centered = columns.Cast<CenteredColumns>();
		centered.slope = laid.squares[0].w;
		centered.minus_constant = laid.squares[0].t;
	}

	//! The row's center and weight. The center divides by the slope, so it warns where the slope, or a nonzero constant
	//! beside it, is below the normal range (DirectTurningPoint's rule).
	void PerRow(const PlanTools &tools, DirectProjectionStage &p2, LossColumns &columns,
	            vector<unique_ptr<Expression>> &checks) const override {
		auto &calc = tools.calc;
		auto &centered = columns.Cast<CenteredColumns>();
		auto slope = calc.Col(LogicalType::DOUBLE, centered.slope);
		auto minus_constant = calc.Col(LogicalType::DOUBLE, centered.minus_constant);
		auto zero = calc.Num(0.0);
		auto present = calc.And(calc.Not(calc.IsNull(slope)), calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, slope, zero));
		auto center = calc.If(present, calc.Div(minus_constant, slope), calc.Constant(Value(LogicalType::DOUBLE)));
		auto row_weight = calc.If(present, calc.Mul(calc.Num(weight), calc.Fn("abs", slope)), zero);
		auto small_constant = calc.And(calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, minus_constant, zero),
		                               DirectBelowNormalRange(calc, minus_constant));
		checks.push_back(DirectWarnIf(
		    calc, calc.Out(calc.And(present, calc.Any({DirectBelowNormalRange(calc, slope), small_constant})))));
		centered.center = p2.Add(calc.Out(center));
		centered.weight = p2.Add(calc.Out(row_weight));
	}

private:
	DirectQuadraticPart term;
	//! `K`: the weight of the term oriented as a minimum, positive.
	double weight;
};

//! `SUM(K * ABS(s*c + t))`: the minimum is at a median of the centers `v = -t/s`. Every point between the lower and the
//! upper median is optimal; the answer is the one nearest 0.
//!
//! ProveAbsolute admits a constant `K` and a nonzero constant `s`, so every row that has a center has the same weight
//! `m = K*|s|`, and the weighted median is the plain one: the lower median is the `ceil(n/2)`-th least center
//! (`quantile_disc` at one half) and the upper median the `floor(n/2)+1`-th, which is the lower median of the negated
//! centers, negated. Both are hash aggregates over the class, with no sort and no window, so they run in parallel (the
//! standard order-statistic aggregate; a weighted median would need a running sum of the weights in order of the
//! centers). A rule that admits a weight read from the data needs that sum instead. The class's total weight stays,
//! since a sum past a DOUBLE is the solver's error.
class AbsoluteLoss final : public CenteredLoss {
public:
	using CenteredLoss::CenteredLoss;

	unique_ptr<LossColumns> NewColumns() const override {
		return make_uniq<MedianColumns>();
	}

	void Aggregate(const PlanTools &tools, const ClassAggregate &classes, LossColumns &columns) const override {
		auto &median = columns.Cast<MedianColumns>();
		auto &calc = tools.calc;
		auto center = calc.Col(LogicalType::DOUBLE, median.center);
		median.lower = classes.AddLowerMedian(center);
		median.upper = classes.AddLowerMedian(calc.Neg(center));
		median.total = classes.Add("sum", median.weight, LogicalType::DOUBLE);
	}

	void CarryClasses(const PlanTools &tools, DirectProjectionStage &p3, LossColumns &columns) const override {
		auto &median = columns.Cast<MedianColumns>();
		CarryDouble(tools.calc, p3, median.total);
		CarryDouble(tools.calc, p3, median.lower);
		CarryDouble(tools.calc, p3, median.upper);
	}

	Answer Finish(const PlanTools &tools, LossColumns &columns, const ExprPtr &, const ExprPtr &,
	              const ExprPtr &) const override {
		auto &calc = tools.calc;
		auto &median = columns.Cast<MedianColumns>();
		Answer answer;
		// The upper median is the negated lower median of the negated centers. The point of the optimal medians nearest
		// 0. With no center in the class, both medians are NULL, which the extrema skip: the target is 0.
		answer.target = calc.Min({calc.Max({calc.Num(0.0), calc.Col(LogicalType::DOUBLE, median.lower)}),
		                          calc.Neg(calc.Col(LogicalType::DOUBLE, median.upper))});
		answer.checks.push_back(
		    DirectRaiseTooLarge(tools.optimizer, calc.Col(LogicalType::DOUBLE, median.total), tools.written));
		return answer;
	}
};

//! `MAX(K * ABS(s*c + t))`: the minimum is at the midpoint of the least and the greatest center `v = -t/s`, the same
//! for every `K`. With no center in the class the target is NULL: every point is optimal.
class MaxAbsoluteLoss final : public CenteredLoss {
public:
	using CenteredLoss::CenteredLoss;

	unique_ptr<LossColumns> NewColumns() const override {
		return make_uniq<ExtremeColumns>();
	}

	void Aggregate(const PlanTools &, const ClassAggregate &classes, LossColumns &columns) const override {
		auto &extreme = columns.Cast<ExtremeColumns>();
		extreme.least = classes.Add("min", extreme.center, LogicalType::DOUBLE);
		extreme.greatest = classes.Add("max", extreme.center, LogicalType::DOUBLE);
	}

	void CarryClasses(const PlanTools &tools, DirectProjectionStage &p3, LossColumns &columns) const override {
		auto &extreme = columns.Cast<ExtremeColumns>();
		CarryDouble(tools.calc, p3, extreme.least);
		CarryDouble(tools.calc, p3, extreme.greatest);
	}

	Answer Finish(const PlanTools &tools, LossColumns &columns, const ExprPtr &, const ExprPtr &,
	              const ExprPtr &) const override {
		auto &calc = tools.calc;
		auto &extreme = columns.Cast<ExtremeColumns>();
		Answer answer;
		// Each half is taken before the sum: the sum of the ends can pass a DOUBLE.
		auto two = calc.Num(2.0);
		answer.target = calc.Add(calc.Div(calc.Col(LogicalType::DOUBLE, extreme.least), two),
		                         calc.Div(calc.Col(LogicalType::DOUBLE, extreme.greatest), two));
		return answer;
	}
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
	//! The objective as written, which a too-large message names (PlanTools::written).
	string written;
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
		condition->written = ir.objective.written;
		// The class plan reads the source, and the rows read it again to be given their class's answer.
		condition->source_reads = 2;
		if (ir.decisions[0].scope == DecideVarScope::KEYED) {
			// A key's columns are columns of the source (the IR reads them as a row check would), so a copy is all it
			// takes.
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
		return "min SUM((c - v)^2), SUM(ABS(c - v)) or MAX(ABS(c - v)), one shared c for each class, MAX(lo) <= c <= "
		       "MIN(hi)";
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
		if (!ir_node.singleton_row) {
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

//! The square is convex: its factor `K * w^2`, with the part's sign and its constant factor applied, is positive. A
//! weight read from the data has no sign before the rows are read, so the check is left to the plan, which reads the
//! sign of the total.
static bool SquareIsConvex(ClientContext &context, const DirectQuadraticPart &part, double slope) {
	double weight;
	if (!DirectFoldConstantSum(context, part.squares[0].weight, weight)) {
		return true;
	}
	double oriented = DirectOriented(part.linear, weight * (slope * slope));
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
	    !SquareIsConvex(context, part, slope)) {
		return nullptr;
	}
	return make_uniq<SquareLoss>(std::move(part));
}

//! The one `ABS` of a part, read into `part.squares` so the shared layout of a part lays it out: the weight, the slope
//! and the constant of `K * ABS(s*c + t)`. `weight` is `K` oriented as a minimum and `slope` is `s`. False when
//! `s` is not a nonzero constant, or `K` is not a constant that is positive once the part's sign is applied: a weight
//! read from the data is the solver's. The part holds the `ABS` and no other term (ProveObjective).
static bool ReadAbsolute(const DirectIR &ir, ClientContext &context, const DirectPart &part_ir,
                         DirectQuadraticPart &part, double &weight, double &slope) {
	if (!DirectReadLinearPart(ir, context, part_ir, ir.objective.written, part.linear) ||
	    !DirectObjectivePartKept(ir, part_ir, part.kept) ||
	    !DirectReadAbsTerm(ir, part_ir.terms[0], ir.objective.written, part.squares.emplace_back())) {
		return false;
	}
	auto &read = part.squares[0];
	double constant;
	if (!DirectFoldConstantSum(context, read.weight, constant) ||
	    !DirectFoldConstantSum(context, read.inner.linear, slope) || slope == 0.0) {
		return false;
	}
	weight = DirectOriented(part.linear, constant);
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
	return make_uniq<AbsoluteLoss>(std::move(part), weight);
}

//! One plain `MAX` of one `ABS`, minimized. The midpoint of the extreme centers is the same for every positive `K`.
static unique_ptr<Loss> ProveMaxAbsolute(const DirectIR &ir, ClientContext &context, const DirectPart &part_ir) {
	DirectQuadraticPart part;
	double weight, slope;
	// The reducer is a MAX (ProveObjective): a plain one is not qualified by a keyed scope or grouped by a key.
	bool plain = part_ir.qualifier == DConstants::INVALID_INDEX && part_ir.by.empty();
	if (!plain || !ReadAbsolute(ir, context, part_ir, part, weight, slope)) {
		return nullptr;
	}
	return make_uniq<MaxAbsoluteLoss>(std::move(part), weight);
}

//! The objective is one part holding one loss: a square, an `ABS` summed, or an `ABS` maximized. The part holds
//! nothing beside its loss, and its rows are the ones the objective's `when` and the part's own keep.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	if (ir.objective.parts.size() != 1) {
		return false;
	}
	auto &part = ir.objective.parts[0];
	if (part.terms.size() != 1 || !part.terms[0].IsAbs()) {
		condition.loss = ProveSquare(ir, context, part);
	} else if (part.reducer == DirectReducer::MAX) {
		condition.loss = ProveMaxAbsolute(ir, context, part);
	} else {
		condition.loss = ProveAbsolute(ir, context, part);
	}
	return condition.loss != nullptr;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! Builds the relational plan for one A2 problem whose condition holds, as group-then-join (DirectJoinClassAnswers)
//! over two reads of the source, as the skeleton of a Template Method; the loss (Loss) fills in its stages. The first
//! read computes the answer of each class once, on the class's one row; the second gives every row its class's answer.
//! Each stage of the first reads only the columns of the stage before it:
//!
//!   scope the row's class key (DirectProjectScope), for a keyed decision
//!   P1    raw values: the loss's values on the rows the objective keeps; each bound clause's ends on the rows it
//!         keeps; the check of every value that can fail
//!   P2    the loss's values for the row (a square's `a` and `b`, a center and its weight), each clause's lower and
//!         upper end on the row (no bound where its `when` drops the row), and the row's checks
//!   W     a hash aggregate on the class key, one row per class (one row for a
//!         query-wide decision): the loss's aggregates, the MAX of each clause's lower ends and the MIN of its upper
//!         ends, whether a floor-lifting clause keeps some row, and whether the checks of the rows held
//!   P3    the class's interval as columns: each clause's tightest ends, and the floor
//!   P4    the class's answer: the loss's target clamped into the interval (the box of DirectRowBox over the columns
//!         of P3), NULL for a key with no class (DirectClassPresent), and the column that keeps the checks live
//!
//! The second read is the scope (the class key) and the join with the class plan, on it. A query-wide
//! decision joins the class's one row to every row. No joined row exists before its class row has passed its checks:
//! the aggregate holds every row of the first read before it gives its row, so each check on a row (P2) has run on all
//! of them, and P4's checks on the class's row sit below the join (DirectJoinClassAnswers), as the solver reads every
//! row before it returns.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p),
	      source_types(source_types_p), tools {optimizer, calc, binder, source_bindings, errors_p, condition_p.written},
	      errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
	}

	//! The class plan over the first read `rows`: the key's columns, then the answer and the column that keeps the
	//! checks live, one row per class.
	unique_ptr<LogicalOperator> BuildClasses(unique_ptr<LogicalOperator> rows) const {
		auto state = DirectProjectScope(binder, std::move(rows), IsKeyed(), nullptr, condition.keys, {});
		auto from_scope = DirectCarriedOf(state, source_bindings.size());

		// The loss's columns travel through every stage; the skeleton never reads them.
		auto loss_columns = condition.loss->NewColumns();

		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto raw = LayoutRawValues(p1, from_scope, state.state_index, *loss_columns);
		auto plan = p1.Finish(std::move(state.plan));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto coefficients = ComputeRows(p2, raw, *loss_columns);
		plan = p2.Finish(std::move(plan));

		auto aggregates = AggregateClasses(coefficients, plan, *loss_columns);

		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto interval = ComputeInterval(p3, aggregates, *loss_columns);
		plan = p3.Finish(std::move(plan));

		DirectProjectionStage p4(binder.GenerateTableIndex());
		ComputeAnswer(p4, interval, *loss_columns);
		return p4.Finish(std::move(plan));
	}

	//! The substitute over the second read `rows`: its source columns, then each row's class's answer, and the column
	//! that keeps the class plan's checks live (P5).
	DirectSubstitute JoinAnswers(unique_ptr<LogicalOperator> rows, unique_ptr<LogicalOperator> classes) const {
		auto state = DirectProjectScope(binder, std::move(rows), IsKeyed(), nullptr, condition.keys, {});
		auto carried = DirectCarriedOf(state, source_bindings.size());
		auto joined =
		    DirectJoinClassAnswers(binder, std::move(state.plan), carried.keys, carried.key, std::move(classes));
		DirectProjectionStage p5(binder.GenerateTableIndex());
		p5.Forward(carried.source, source_types);
		D_ASSERT(joined.answers.size() == 2);
		p5.Add(calc.Out(calc.Col(LogicalType::DOUBLE, joined.answers[0])));
		p5.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, joined.answers[1])));
		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		// The source columns, then c. The class plan's checks are read by the column after them.
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		// The rows are probed against the class plan and held by nothing.
		substitute.pipelined = true;
		substitute.child = p5.Finish(std::move(joined.plan));
		return substitute;
	}

private:
	//! Columns of P1.
	struct Raw {
		DirectCarried carried;
		vector<DirectBoundColumns> clauses;
		vector<unique_ptr<Expression>> checks;
	};
	//! A clause's lower and upper end on the row (DirectClauseBounds), and the row's `when` for a clause that lifts the
	//! floor.
	struct ClauseEnds {
		ColumnBinding lower;
		ColumnBinding upper;
		ColumnBinding when;
	};
	//! Columns of P2: the class, which the aggregate reads, not the source's. A keyed row's class is its key; a row
	//! with a NULL in a key element that puts it in no class has none.
	struct Rows {
		vector<ColumnBinding> keys;
		DirectClassKey key;
		vector<ClauseEnds> ends;
		ColumnBinding valid;
	};
	//! Columns of W: the class, and the aggregates of the clauses over the results.
	struct Aggregates {
		vector<ColumnBinding> keys;
		DirectClassKey key;
		ColumnBinding valid;
		vector<DirectGroupValues> lowers;
		vector<DirectGroupValues> uppers;
		//! Whether some row of a floor-lifting clause is kept, one per such clause.
		vector<ColumnBinding> lowering_whens;
	};
	//! Columns of P3.
	struct Interval {
		vector<ColumnBinding> keys;
		DirectClassKey key;
		ColumnBinding valid;
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

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	Raw LayoutRawValues(DirectProjectionStage &p1, const DirectCarried &from_scope, idx_t state_index,
	                    LossColumns &loss_columns) const {
		Raw raw;
		raw.carried = p1.Forward(from_scope, source_types, IsKeyed());
		OverSource over_state = [&](const Expression &value) {
			return state_index == DConstants::INVALID_INDEX ? value.Copy()
			                                                : DirectOverProjection(value, source_bindings, state_index);
		};
		raw.clauses =
		    DirectLayoutBoundClauses(optimizer, calc, p1, condition.clauses, source_bindings, over_state, raw.checks);
		condition.loss->LayoutRaw(tools, p1, over_state, loss_columns, raw.checks);
		return raw;
	}

	//===------------------------------------------------------------------===//
	// P2: the loss's values for the row, each clause's ends, and the checks
	//===------------------------------------------------------------------===//

	Rows ComputeRows(DirectProjectionStage &p2, Raw &raw, LossColumns &loss_columns) const {
		Rows result;
		// A class has no source columns: P1 carried them for the checks, which are computed here.
		auto carried = raw.carried;
		carried.source.clear();
		carried = p2.Forward(carried, {}, IsKeyed());
		result.keys = carried.keys;
		result.key = carried.key;
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
		condition.loss->PerRow(tools, p2, loss_columns, raw.checks);
		// Every check is true or raises; a row passes when all of them do.
		result.valid = p2.Add(DirectAllHold(calc, std::move(raw.checks)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// W: the aggregates of a class
	//===------------------------------------------------------------------===//

	//! Puts one hash aggregate on the key over `plan`: the loss's aggregates, and for each clause the MAX of its lower
	//! ends and the MIN of its upper ends (DirectTightestBound), and whether some row a floor-lifting clause keeps
	//! exists. A row with no class sits in a key of its own, whose answer is NULL whatever the key holds.
	Aggregates AggregateClasses(const Rows &rows, unique_ptr<LogicalOperator> &plan, LossColumns &loss_columns) const {
		DirectAggregateStage stage(binder);
		Aggregates result;
		result.key = rows.key;
		for (idx_t i = 0; i < rows.keys.size(); i++) {
			result.keys.push_back(stage.Group(calc.Out(calc.Col(rows.key.types[i], rows.keys[i]))));
		}
		ClassAggregate classes(stage, optimizer, calc);
		condition.loss->Aggregate(tools, classes, loss_columns);
		DirectAggregateSink sink(stage);
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			auto &clause = condition.clauses[j];
			auto &ends = rows.ends[j];
			result.lowers.push_back(
			    TightestEnd(sink, ExpressionType::COMPARE_GREATERTHANOREQUALTO, *clause.lower, ends.lower));
			result.uppers.push_back(
			    TightestEnd(sink, ExpressionType::COMPARE_LESSTHANOREQUALTO, *clause.upper, ends.upper));
			if (clause.lifts_floor) {
				result.lowering_whens.push_back(classes.Add("bool_or", ends.when, LogicalType::BOOLEAN));
			}
		}
		// A query-wide decision has no key to group by, and an aggregate without a key gives its one row over no rows
		// too. That row has `valid` NULL (bool_and of no rows), and DirectAllHold puts `valid` first, so no check
		// runs on it: an empty source raises nothing and returns no rows.
		result.valid = classes.Add("bool_and", rows.valid, LogicalType::BOOLEAN);
		plan = stage.Finish(std::move(plan));
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
	Interval ComputeInterval(DirectProjectionStage &p3, const Aggregates &aggregates, LossColumns &loss_columns) const {
		Interval result;
		result.keys = p3.Forward(aggregates.keys, aggregates.key.types);
		result.key = aggregates.key;
		result.valid = p3.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, aggregates.valid)));
		condition.loss->CarryClasses(tools, p3, loss_columns);
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			result.clause_lowers.push_back(
			    p3.Add(calc.Out(calc.Cast(aggregates.lowers[j].lower, LogicalType::DOUBLE))));
			result.clause_uppers.push_back(
			    p3.Add(calc.Out(calc.Cast(aggregates.uppers[j].upper, LogicalType::DOUBLE))));
		}
		result.floor = p3.Add(calc.Out(DirectStaticFloor(calc, condition.lower, aggregates.lowering_whens)));
		return result;
	}

	//===------------------------------------------------------------------===//
	// P4: the answer
	//===------------------------------------------------------------------===//

	void ComputeAnswer(DirectProjectionStage &p4, const Interval &interval, LossColumns &loss_columns) const {
		p4.Forward(interval.keys, interval.key.types);
		ExprPtr lower, upper;
		DirectRowBox(calc, calc.Col(LogicalType::DOUBLE, interval.floor), condition.upper, interval.clause_lowers,
		             interval.clause_uppers, lower, upper);
		// A key with no class has no decision and shows NULL.
		vector<ExprPtr> key_columns;
		for (idx_t i = 0; i < interval.keys.size(); i++) {
			key_columns.push_back(calc.Col(interval.key.types[i], interval.keys[i]));
		}
		auto has_class = DirectClassPresent(calc, interval.key, key_columns);
		auto finish = condition.loss->Finish(tools, loss_columns, lower, upper, has_class);
		// The loss's target clamped into the interval. A NULL target leaves every point optimal, and the point of the
		// interval nearest 0 is the answer: the clamp of 0. Adding 0.0 turns -0.0 into 0.0.
		auto zero = calc.Num(0.0);
		auto answer = calc.Add(calc.Min({calc.Max({calc.Coalesce({finish.target, zero}), lower}), upper}), zero);
		p4.Add(calc.Out(calc.If(has_class, answer, calc.Constant(Value(LogicalType::DOUBLE)))));

		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, interval.valid)));
		// An empty interval raises on every key that has a class. A query whose only bounds are constants has two
		// constants here, which DuckDB folds.
		checks.push_back(DirectRaiseIf(calc, calc.Out(calc.And(has_class, DirectBoxIsEmpty(calc, lower, upper))),
		                               DirectInfeasibleErrorPredicate(optimizer, errors)));
		// The loss's checks, then the answer's, then the loss's warnings: a warning prints as it runs, so it follows
		// every check that can raise first.
		MoveInto(checks, finish.checks);
		if (finish.finite_where) {
			auto in_range =
			    calc.If(calc.And(has_class, finish.finite_where), answer, calc.Constant(Value(LogicalType::DOUBLE)));
			checks.push_back(DirectRaiseTooLarge(optimizer, in_range, condition.written));
		}
		MoveInto(checks, finish.warnings);
		p4.Add(DirectAllHold(calc, std::move(checks)));
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	auto classes = DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
		return plan_builder.BuildClasses(std::move(rows));
	});
	return DirectBuildOverOneRead(reader, [&](unique_ptr<LogicalOperator> rows) {
		return plan_builder.JoinAnswers(std::move(rows), std::move(classes));
	});
}

} // namespace direct_a2_single_facility_location

unique_ptr<DirectSolveRule> MakeA2SingleFacilityLocationRule() {
	return make_uniq<direct_a2_single_facility_location::Rule>();
}

} // namespace duckdb
