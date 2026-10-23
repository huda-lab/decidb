#pragma once

#include <limits>

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;

//! The solver's infinite ends of a box, written once for every rule that builds or reads one.
constexpr double POSITIVE_INFINITY = std::numeric_limits<double>::infinity();
constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();

//===--------------------------------------------------------------------===//
// Per-row bounds on one decision
//===--------------------------------------------------------------------===//
//
// A bound `coefficient * x  comparison  rhs` on one decision reads, on each row, as a lower and an upper bound on x.
// The reader has two halves that a rule composes. At plan time, DirectReadBounds proves the bounds and separates what
// the solver folds into the decision's static box from the clauses the plan evaluates per row. In the plan,
// DirectClauseBounds turns one clause's raw values into its two bounds and DirectRowBox intersects them with the box.

//! A numeric expression over the source, read as DOUBLE and multiplied by `sign`.
struct DirectSignedValue {
	int sign = 1;
	unique_ptr<Expression> expr;
};

//! One bound the plan evaluates per row: `coefficient * x  comparison  rhs`, under an optional WHEN. A bound the solver
//! folds into the variable's box is not a clause, it is part of the decision's static box.
struct DirectBoundClause {
	idx_t decision = 0;
	ExpressionType comparison = ExpressionType::INVALID;
	vector<DirectSignedValue> coefficient;
	unique_ptr<Expression> rhs;
	unique_ptr<Expression> when;

	//! A deep copy of every field, so a field added later cannot be missing from a clause split in two.
	DirectBoundClause Copy() const;
};

//! A decision's box before any per-row bound: its default, replaced or tightened by the bounds the solver folds into
//! the variable. An end of magnitude 1e20 or more is infinite.
struct DirectDecisionBox {
	double lower = 0.0;
	double upper = std::numeric_limits<double>::infinity();
};

struct DirectBoundReading {
	//! The per-row clauses, in the order the constraints came. An equality is split into its lower and its upper half.
	vector<DirectBoundClause> clauses;
	//! One static box per decision.
	vector<DirectDecisionBox> boxes;
};

//! Reads every per-row constraint of `ir` as a bound on one decision (a constraint of the form `a*x <comparison>
//! value`). An aggregate constraint is not a bound and is skipped: the rule reads it, and A1, which has none, declines
//! it. The rule's CheckCondition has checked each per-row constraint with DirectReadsOneDecision. A
//! bound the solver folds into the variable (`variable_bound`: an unconditional comparison of the bare decision
//! against a constant) replaces the default lower bound of 0 and tightens the upper one; every other bound is a
//! clause. Returns false when a bound cannot be proved, or when the folded box contradicts the decision's built-in
//! domain, which the solver reports.
bool DirectReadBounds(const DirectIR &ir, ClientContext &context, DirectBoundReading &reading);

//! One unfiltered per-row left side: only linear terms, in one decision of the `decision_count`, and no constant. The
//! shape a per-row bound must have for DirectReadBounds to read it.
bool DirectReadsOneDecision(const DirectIRConstraint &ir_node, idx_t decision_count);

//! The columns of one clause's raw values, evaluated on every row.
struct DirectBoundColumns {
	ColumnBinding coefficient;
	ColumnBinding rhs;
	bool has_when = false;
	ColumnBinding when;
};

//! The lower and upper bound one clause puts on its decision on one row, as the solver reads it: a clause holds
//! `coefficient * x  comparison  rhs`, so the coefficient's sign picks the side, a zero coefficient leaves the row
//! alone or infeasible, and a whole-numbered decision rounds to the integer DOUBLE arithmetic accepts.
void DirectClauseBounds(const ExpressionBuilder &calc, ExpressionType comparison, bool whole,
                        const DirectBoundColumns &columns, ExprPtr &lower, ExprPtr &upper);

//! A decision's box on one row: the static box intersected with the bounds of its clauses.
void DirectRowBox(const ExpressionBuilder &calc, double static_lower, double static_upper,
                  const vector<ColumnBinding> &clause_lowers, const vector<ColumnBinding> &clause_uppers,
                  ExprPtr &lower, ExprPtr &upper);

//! True on a row where no value of the decision satisfies its box: the lower end is above the upper, the lower end is
//! +infinity, or the upper end is -infinity.
ExprPtr DirectBoxIsEmpty(const ExpressionBuilder &calc, const ExprPtr &lower, const ExprPtr &upper);

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
	bool scaled = false;
	double scale = 1.0;
	bool divides = false;
	vector<DirectLinearTerm> terms;
};

//! Reads the sign and constant factor of one part and each of its linear terms. Returns false when the factor is not a
//! finite constant (or divides by zero) or when a linear term's coefficient is not a numeric value of the source row.
//! A term that is not linear is skipped, and so are the part's reducer, filter and qualifier: the rule checks those,
//! and which decision a term names.
bool DirectReadLinearPart(const DirectIR &ir, ClientContext &context, const DirectPart &part, DirectLinearPart &out);

} // namespace duckdb
