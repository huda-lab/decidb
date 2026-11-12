#pragma once

#include <limits>

#include "duckdb/common/common.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;

// The shared reader of what a direct rule takes from the IR. The file is named for its first half, the per-row bounds
// on a decision. It also reads the terms of an objective part or a budget: linear terms (DirectReadLinearPart),
// per-row constants (DirectReadConstantTerm), squares (DirectReadSquareTerm) and the rows a part reads
// (DirectObjectivePartKept), composed into DirectReadQuadraticPart. It reads the limit of an aggregate constraint too
// (DirectReadLimit). DirectNamesOf and DirectIsFiniteWhereKept, which name and test a value those readers produce,
// stand here too.

//! The solver's infinite ends of a box, written once for every rule that builds or reads one.
constexpr double POSITIVE_INFINITY = std::numeric_limits<double>::infinity();
constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();

//===--------------------------------------------------------------------===//
// Per-row bounds on one decision
//===--------------------------------------------------------------------===//
//
// A bound on one decision reads, on each row, as the lower and upper end of the values it admits: the solver path's
// own classifier (DecideReadSingletonRow) builds both ends. The reader has two halves that a rule composes. At plan
// time, DirectReadBounds separates what the solver folds into the decision's static box from the clauses the plan
// evaluates per row. In the plan, DirectClauseBounds reads one clause's two ends and DirectRowBox intersects them with
// the box.

//! A numeric expression over the source, read as DOUBLE and multiplied by `sign`.
struct DirectSignedValue {
	int sign = 1;
	unique_ptr<Expression> expr;
};

//! One bound the plan evaluates per row, under an optional `when`. A bound the solver folds into the variable's box is
//! not a clause, it is part of the decision's static box.
struct DirectBoundClause {
	idx_t decision = 0;
	//! The clause's coefficient and right side, which the plan checks as the solver path checks them.
	vector<DirectSignedValue> coefficient;
	unique_ptr<Expression> rhs;
	//! What a message calls them: the coefficient's values and its clause (DirectNamesOf), and the right side as the
	//! user wrote it (DirectIR::render).
	DirectNumberNames coefficient_names;
	string rhs_text;
	//! The values of the decision the clause admits (DecideSingletonRow): DOUBLE, ±infinity on an open side.
	unique_ptr<Expression> lower;
	unique_ptr<Expression> upper;
	unique_ptr<Expression> when;
	//! True when the clause replaces the default floor of 0 on the rows its `when` keeps (DecideSingletonRow::
	//! LiftsDefaultFloor): a constant lower bound under a `when`, on a decision with no lower bound written.
	//! The decision's static floor is then -infinity where any such `when` holds (DirectStaticFloor).
	bool lifts_floor = false;
};

//! A decision's box before any per-row bound: its default, replaced or tightened by the bounds the solver folds into
//! the variable. An end of magnitude 1e20 or more is infinite. A default lower end of 0 is lowered, on the rows a
//! clause with `lifts_floor` keeps, by DirectStaticFloor.
struct DirectDecisionBox {
	double lower = 0.0;
	double upper = std::numeric_limits<double>::infinity();
};

struct DirectBoundReading {
	//! The per-row clauses, in the order the constraints came.
	vector<DirectBoundClause> clauses;
	//! One static box per decision.
	vector<DirectDecisionBox> boxes;
};

//! Reads every per-row constraint of `ir` as a bound on one decision. An aggregate constraint is not a bound and is
//! skipped: the rule reads it, and A1, which has none, declines it. The rule's CheckCondition has checked each per-row
//! constraint as a bound on one decision (`singleton_row`). A bound the solver folds into the variable
//! (`variable_bound`) replaces the default lower bound of 0 and tightens the upper one; every other bound is a clause.
//! Returns false when a value cannot be proved or the box as written contradicts the decision's built-in domain, which
//! the solver reports.
bool DirectReadBounds(const DirectIR &ir, ClientContext &context, DirectBoundReading &reading);

//! True when the bounds the solver folds into a decision (`variable_bound`), as written, contradict its built-in domain
//! (DecideWrittenBox): the solver raises that error before it reads a row, so a rule that answers the query declines.
bool DirectContradictsDomain(const DirectIR &ir);

//! The columns of one clause's values, evaluated on every row.
struct DirectBoundColumns {
	ColumnBinding lower;
	ColumnBinding upper;
	bool has_when = false;
	ColumnBinding when;
};

//! The lower and upper bound one clause puts on its decision on one row: the admitted ends where the clause keeps the
//! row, and no bound elsewhere.
void DirectClauseBounds(const ExpressionBuilder &calc, const DirectBoundColumns &columns, ExprPtr &lower,
                        ExprPtr &upper);

//! A decision's static lower end on one row: `static_lower` where none of `lowering_whens` holds, and -infinity where
//! one does. `lowering_whens` are the `when` columns of the clauses with `lifts_floor`; a NULL `when` keeps no row, as
//! in DirectClauseBounds, so the static end stays.
ExprPtr DirectStaticFloor(const ExpressionBuilder &calc, double static_lower,
                          const vector<ColumnBinding> &lowering_whens);

//! A decision's box on one row: the static box intersected with the bounds of its clauses. `static_lower` is the
//! decision's static lower end on the row (DirectStaticFloor). A static end that is infinite is left out of the
//! LEAST/GREATEST, so a box with no finite static end clamps nothing it does not need to.
void DirectRowBox(const ExpressionBuilder &calc, const ExprPtr &static_lower, double static_upper,
                  const vector<ColumnBinding> &clause_lowers, const vector<ColumnBinding> &clause_uppers,
                  ExprPtr &lower, ExprPtr &upper);

//! True on a row where no value of the decision satisfies its box: the lower end is above the upper, the lower end is
//! +infinity, or the upper end is -infinity.
ExprPtr DirectBoxIsEmpty(const ExpressionBuilder &calc, const ExprPtr &lower, const ExprPtr &upper);

//===--------------------------------------------------------------------===//
// The limit of an aggregate constraint
//===--------------------------------------------------------------------===//

//! A limit read from the data: a source column or an expression over source columns, or the bound-side rewrite's
//! aggregates of the group (DecideGroupBound). The plan takes the group's tightest value of a `value`
//! (DirectTightestBound) and computes a `group`'s aggregates in its group aggregate. A NULL skips its instance: the
//! group's MIN or MAX passes over it.
struct DirectDataLimit {
	ExpressionType comparison = ExpressionType::INVALID;
	//! The limit, one value on every kept row of a group already. Null with a `group`.
	unique_ptr<Expression> value;
	unique_ptr<DecideGroupBound> group;

	//! The group bound the plan computes: the `group` the rewrite left, or else the tightest of the per-row `value`
	//! in the group (DirectTightestBound).
	unique_ptr<DecideGroupBound> GroupBound(Optimizer &optimizer) const;
};

//! The limit of one aggregate constraint: a constant, or a value of the data.
struct DirectLimit {
	//! True when the limit is the constant `number`; `data` is then empty.
	bool constant = false;
	double number = 0;
	DirectDataLimit data;
};

//! Reads the right side of an aggregate constraint: a constant the solver reads as a number, of magnitude below
//! DECIDE_EFFECTIVE_INFINITY (from there the solver stops reading a right side as a number, so those stay with it), or
//! a value of the data, which is read at run time instead. A value of the data is a numeric, deterministic expression
//! over source columns that mentions at least one and may raise (the plan evaluates it on the rows that read it, as the
//! solver does, so both raise on the same inputs), or the aggregates of its group that the bound-side rewrite left to
//! the plan (DirectGroupBoundIsReadable). False when it is neither.
bool DirectReadLimit(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &constraint,
                     DirectLimit &limit);

//! The limit of a count constraint `SUM(x) <comparison> B`, or `SUM(-x) <comparison> B`, read as a limit on the count
//! `SUM(x)` itself: `SUM(-x) <= B` is `SUM(x) >= -B`, so a negated body flips the comparison and negates the limit.
struct DirectCountLimit {
	//! The comparison on the count: the constraint's, flipped when `negated`.
	ExpressionType comparison = ExpressionType::INVALID;
	//! The count body is `-x`. A constant `limit.number` is already the limit on the count (`-B`); a limit of the data
	//! holds `B` as written, and the plan negates it.
	bool negated = false;
	DirectLimit limit;
};

//! Reads the right side of a count constraint (DirectReadLimit) as a limit on the count `SUM(x)`; `negated` says the
//! constraint's body is `SUM(-x)`. A constant is rounded to whole rows by DirectInclusiveRows, a limit of the data by
//! DirectInclusiveRowsExpr. False when DirectReadLimit reads no limit, which includes a constant of magnitude
//! DECIDE_EFFECTIVE_INFINITY or more.
bool DirectReadCountLimit(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &constraint,
                          bool negated, DirectCountLimit &count);

//===--------------------------------------------------------------------===//
// Linear terms of a part
//===--------------------------------------------------------------------===//

//! One linear term of a part: `sign * coefficient * x[decision]`, the coefficient a numeric value of the source row.
struct DirectLinearTerm {
	idx_t decision = 0;
	int sign = 1;
	unique_ptr<Expression> coefficient;
};

//! One part `sign * scale * SUM(...)` of an objective or of a constraint's left side, with the part's constant factor
//! (`2 * SUM(...)`, `SUM(...) / 2`) and its linear terms in the order they came. A term of any other kind is left to
//! the rule.
struct DirectLinearPart {
	int sign = 1;
	DirectFactor factor;
	vector<DirectLinearTerm> terms;
	//! For each decision, what a message names for the part's like terms on it (DirectNamesOf): the values of the
	//! solver path's one coefficient of the decision, and the part's clause.
	vector<DirectNumberNames> names;
};

//! `number` oriented as a minimum: times the part's sign and its factor (DirectFactor). The canonicalizer writes every
//! objective as a minimization, so a part's orientation is its sign times its factor and nothing else.
inline double DirectOriented(const DirectLinearPart &part, double number) {
	return part.sign * part.factor.ApplyTo(number);
}

//! The terms of `part` that name `decision`, each as a signed value.
vector<DirectSignedValue> DirectValuesOn(const DirectLinearPart &part, idx_t decision);

//! What a message names for a number computed from `values` (DirectNumberNames): each value a column holds, printed as
//! the user wrote it, in reading order, and `clause`. A literal is no column's value: a number made of literals names
//! the clause, as the solver path names it.
DirectNumberNames DirectNamesOf(const DirectIR &ir, const vector<DirectSignedValue> &values, const string &clause);

//! Whether a number computed from `values` (with the part's `factor`) needs no invalid-value check: the
//! sum of the magnitude bounds of the values (DirectMagnitudeBound), each read as a row its NULL guard keeps reads it
//! and times the factor, is below the range of a DOUBLE. A value of an exact numeric type is bounded by its type, so
//! only a FLOAT or DOUBLE column, or an expression of one, keeps its check. A row the guard skips reads 0, so the
//! number is finite on every row. A constant that is not finite keeps its check, which raises at run time as the solver
//! path does.
bool DirectIsFiniteWhereKept(Optimizer &optimizer, const vector<DirectSignedValue> &values,
                             const DirectFactor &factor = DirectFactor());

//! A bound on the magnitude of one term of a number DirectIsFiniteWhereKept judges: the bound of `value`
//! (DirectMagnitudeBound, through its NULL guard) times the part's factor. A rule whose number is made
//! of terms with factors of their own sums these and asks DirectSumIsFinite.
double DirectScaledMagnitude(Optimizer &optimizer, const Expression &value, const DirectFactor &factor);

//! Whether a number whose terms' magnitudes sum to at most `total` is below the range of a DOUBLE, whatever the
//! rounding of its sums.
bool DirectSumIsFinite(double total);

//! True when every term is linear: a coefficient times one decision. DirectReadLinearPart skips any other term and
//! leaves this check to the rule.
bool DirectOnlyLinearTerms(const vector<DecideSplitTerm> &terms);

//! Reads the sign and constant factor of one part and each of its linear terms. Returns false when the factor is not a
//! finite constant (or divides by zero) or when a linear term's coefficient is not a numeric value of the source row.
//! A term that is not linear is skipped, and so are the part's reducer, filter and qualifier: the rule checks those,
//! and which decision a term names. `clause` is the clause (or the objective) the part belongs to, which a message
//! names.
bool DirectReadLinearPart(const DirectIR &ir, ClientContext &context, const DirectPart &part, const string &clause,
                          DirectLinearPart &out);

//===--------------------------------------------------------------------===//
// Terms of a part that are not linear
//===--------------------------------------------------------------------===//

//! A per-row constant of a part, and what a message names for it (DirectNamesOf).
struct DirectConstantTerm {
	int sign = 1;
	unique_ptr<Expression> value;
	DirectNumberNames value_names;
};

//! A linear form in one decision, `SUM(linear) * x[decision] + SUM(constant)`: its slopes and its constants, each a
//! numeric value of the source row.
struct DirectLinearForm {
	idx_t decision = 0;
	vector<DirectSignedValue> linear;
	vector<DirectSignedValue> constant;
	//! What a message names for the sums of the two (DirectNamesOf).
	DirectNumberNames linear_names;
	DirectNumberNames constant_names;
};

//! `weight * f(inner)`, where `f` is set by the reader that fills it: DirectReadSquareTerm reads `f(x) = x^2` and
//! DirectReadAbsTerm reads `f(x) = ABS(x)`. The weight is the term's sign times its coefficient, a numeric value of the
//! source row (a constant, or read from the data).
struct DirectWeightedForm {
	//! The values whose sum is the weight: one, the term's sign and its coefficient (1 when it has none).
	vector<DirectSignedValue> weight;
	//! What a message names for the weight (DirectNamesOf).
	DirectNumberNames weight_names;
	DirectLinearForm inner;
};

//! Reads a CONSTANT term of a part. False when its value is not a numeric value of the source row.
bool DirectReadConstantTerm(const DirectIR &ir, const DecideSplitTerm &term, const string &clause,
                            DirectConstantTerm &out);

//! Reads the terms of a linear form (the factor of a square, or what an absolute value holds): linear terms of one
//! declared decision, and constants, each with a coefficient that is a numeric value of the source row. False when a
//! term is of another kind, the form names no decision or two, or a value cannot be proved.
bool DirectReadLinearForm(const DirectIR &ir, const vector<DecideSplitTerm> &form, const string &clause,
                          DirectLinearForm &out);

//! Reads a square of a part: a linear form squared under a coefficient. False when the form is not linear in one
//! decision, or the coefficient or a value in the form is not a numeric value of the source row. A weight read from the
//! data is read like any other; a rule that needs a constant one folds it (DirectFoldConstantSum).
bool DirectReadSquareTerm(const DirectIR &ir, const DecideSplitTerm &term, const string &clause,
                          DirectWeightedForm &out);

//! Reads an absolute value of a part, `ABS(inner)` under a coefficient, into the same form a square is read into.
//! DirectReadQuadraticPart does not call it, so a rule that does not take an absolute loss declines one without a check
//! of its own. `term` is an `ABS`. False when its form or coefficient cannot be read.
bool DirectReadAbsTerm(const DirectIR &ir, const DecideSplitTerm &term, const string &clause, DirectWeightedForm &out);

//! The signed sum of `values` as one constant, with a NULL guard read through (a row it skips reads 0, as coefficient
//! 0), so a term's weight `DirectWeightedForm::weight` or slope `DirectLinearForm::linear` folds to the number every
//! row shares. False when a value is not a finite constant.
bool DirectFoldConstantSum(ClientContext &context, const vector<DirectSignedValue> &values, double &total);

//! One part of an objective that holds linear terms, per-row constants and squares: its sign and factor with its linear
//! terms (`linear`), the rows it reads (`kept`), and its constants and squares in the order they came.
struct DirectQuadraticPart {
	DirectLinearPart linear;
	//! The rows the part reads (DirectObjectivePartKept), null for every row.
	unique_ptr<Expression> kept;
	vector<DirectConstantTerm> constants;
	//! The squares of the part. A2 lays its `ABS` here too.
	vector<DirectWeightedForm> squares;
};

//! Reads a part of an objective into its linear terms, constants and squares (DirectReadLinearPart,
//! DirectReadConstantTerm, DirectReadSquareTerm, DirectObjectivePartKept). False when any of them cannot be read, a
//! linear term names a decision the IR does not declare, or a term is of another kind. How many squares the rule
//! admits, and which signs, is the rule's to check on `out`.
bool DirectReadQuadraticPart(const DirectIR &ir, ClientContext &context, const DirectPart &part, const string &clause,
                             DirectQuadraticPart &out);

//! The rows an objective part reads: those the objective's `when` and the part's own both keep, over the source, or
//! null for every row. A row they drop (a false or NULL `when`) has no coefficient in the part, which a rule reads as
//! 0. False when a `when` is not a predicate of the source that cannot raise.
bool DirectObjectivePartKept(const DirectIR &ir, const DirectPart &part, unique_ptr<Expression> &kept);

//===--------------------------------------------------------------------===//
// Fixed values of a BOOL decision
//===--------------------------------------------------------------------===//

//! The signed sum of the terms' coefficients, as the solver's like-term collection sums them; false when one is not a
//! finite constant.
bool DirectConstantSlope(ClientContext &context, const vector<DecideSplitTerm> &terms, double &slope);

//! The decision's terms on a per-row left side, as in a fixed value `x = 1`; null for an aggregate constraint or a
//! left side with a term that is not linear. A membership constraint (`x IN (...)`) is the caller's to decline.
const vector<DecideSplitTerm> *DirectFixedTerms(const DirectIRConstraint &constraint);

//! A value from the row that fixes `x`: it must be neither NULL nor, for a floating point value, NaN; a coefficient
//! must also be finite.
struct DirectSourceFixedValue {
	unique_ptr<Expression> value;
	bool reject_nan = false;
	//! The clause's `when`; the value is read only on the rows it keeps. Null for every row.
	unique_ptr<Expression> when;
	bool coefficient = false;
	//! The value as the user wrote it, which the error names.
	string written;
};

//! What the per-row clauses on a BOOL decision fix, read as the set of {0, 1} each row allows: a condition per clause
//! under which a row is fixed to 1 and one under which it is fixed to 0 (a row that allows neither satisfies both: the
//! problem is infeasible), over the source, and the values the plan checks. All are over the source's bindings.
struct DirectFixedValues {
	vector<unique_ptr<Expression>> one_conditions;
	vector<unique_ptr<Expression>> zero_conditions;
	vector<DirectSourceFixedValue> source_values;

	bool Any() const {
		return !one_conditions.empty() || !zero_conditions.empty();
	}
};

//! Reads each clause of `clauses` (per-row constraints with DirectFixedTerms) as a fixed value: `x = 1`, `2 * x >= 1`,
//! `x <= flag`, `x <> c`, under an optional `when`. False when a clause is not one the plan can read, or when a bound
//! the solver folds contradicts the decision's built-in domain (DirectContradictsDomain: the solver raises that error
//! before it reads a row).
bool DirectReadFixedValues(const DirectIR &ir, ClientContext &context,
                           const vector<const DirectIRConstraint *> &clauses, DirectFixedValues &fixed);

//! True on a row where any of `conditions` is true; false for none. `Any` tests its first condition first and this
//! tests the last first, the solver's order.
unique_ptr<Expression> DirectAnyCondition(const ExpressionBuilder &calc,
                                          const vector<unique_ptr<Expression>> &conditions);

} // namespace duckdb
