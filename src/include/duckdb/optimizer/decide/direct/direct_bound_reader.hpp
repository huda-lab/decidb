#pragma once

#include <limits>

#include "duckdb/common/common.hpp"
#include "duckdb/common/decide_value_messages.hpp"
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
	//! What a message calls them, as the user wrote them (DirectCoefficientText, DirectIR::render). A coefficient with
	//! no column in it (a literal, like literals merged) is a number DeciDB computes rather than one it reads from a
	//! row: its message is TOO_LARGE and names the clause, as the solver path names it.
	DecideValueRole coefficient_role = DecideValueRole::COEFFICIENT;
	string coefficient_text;
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
//! constraint with DirectIsSingletonRow. A bound the solver folds into the variable (`variable_bound`) replaces
//! the default lower bound of 0 and tightens the upper one; every other bound is a clause. Returns false when a value
//! cannot be proved or the box as written contradicts the decision's built-in domain, which the solver reports.
bool DirectReadBounds(const DirectIR &ir, ClientContext &context, DirectBoundReading &reading);

//! True when the bounds the solver folds into a decision (`variable_bound`), as written, contradict its built-in domain
//! (DecideWrittenBox): the solver raises that error before it reads a row, so a rule that answers the query declines.
bool DirectContradictsDomain(const DirectIR &ir);

//! A per-row constraint the solver's classifier reads as a bound on one declared decision (DecideReadSingletonRow):
//! the shape a per-row bound must have for DirectReadBounds to read it.
bool DirectIsSingletonRow(const DirectIR &ir, const DirectIRConstraint &ir_node);

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
//! decision's static lower end on the row (DirectStaticFloor).
void DirectRowBox(const ExpressionBuilder &calc, const ExprPtr &static_lower, double static_upper,
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
	//! The coefficient as the solver path reads it, with the part's constant factor applied (`2 * w`, `w / 2`): what a
	//! message names.
	unique_ptr<Expression> scaled;
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
	//! For each decision, the coefficient a message names: the part's terms on it, merged as the solver merges like
	//! terms and scaled. Empty for a decision with no term.
	vector<string> text;
};

//! The coefficient a message names for `terms`, the like terms of one decision: merged as the solver path merges them
//! (DecideMergeLikeCoefficients, in reading order, the first term's sign kept), printed as the user wrote it. A single
//! term prints as itself.
string DirectCoefficientText(const DirectIR &ir, ClientContext &context, const vector<DirectSignedValue> &terms);

//! Reads the sign and constant factor of one part and each of its linear terms. Returns false when the factor is not a
//! finite constant (or divides by zero) or when a linear term's coefficient is not a numeric value of the source row.
//! A term that is not linear is skipped, and so are the part's reducer, filter and qualifier: the rule checks those,
//! and which decision a term names.
bool DirectReadLinearPart(const DirectIR &ir, ClientContext &context, const DirectPart &part, DirectLinearPart &out);

} // namespace duckdb
