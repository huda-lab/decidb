//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/optimizer/decide/decide_singleton_row_folding.hpp
//
// A bound on one decision: the values it admits, and which bounds fold into a variable's box.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundComparisonExpression;
class ClientContext;
class LogicalDecide;

//! A comparison that bounds one decision (`x <= 10`, `2 * x >= cap`, `0.1 * x <= c`, an easy `min(x) >= K`), read as
//! the interval of values it admits. The solver path folds a bound known before the query runs into the column box
//! here, the linear form hands a data bound's interval to stage 08, and direct solve projects the same interval, so
//! the paths cannot disagree about which values a bound admits.
struct DecideSingletonRow {
	//! The decision, an index into decide_variables.
	idx_t var_idx = DConstants::INVALID_INDEX;
	//! The admitted interval as DOUBLE expressions over the source row: ±infinity on a side the clause leaves open (on
	//! both sides for a row an easy extremum's reducer filter leaves out), and `lower > upper` when no value holds. A
	//! whole-numbered decision admits exactly the whole numbers for which the written comparison holds as DuckDB
	//! evaluates it.
	unique_ptr<Expression> lower;
	unique_ptr<Expression> upper;
	//! True when the bound folds into the decision's box (D41): the interval is known before the query runs, the
	//! coefficient is not 0, and the side it binds is within the solvers' infinity. A folded bound replaces the default
	//! floor of 0, while one that stays a model row only intersects with it. The fields below are set only then.
	bool folds = false;
	//! The side the clause binds: '<' above, '>' below, '=' both.
	char sense = '<';
	double lower_value = 0.0;
	double upper_value = 0.0;
	//! The bound as written, moved onto the decision (`r / a`, a strict bound stepped by DecideStrictBound). The domain
	//! check and the diagnosis read it; the box holds the admitted ends.
	double written_k = 0.0;
};

//! Reads `comparison` as a bound on one decision. False when it is not one: its decision side is not linear terms of
//! one decision over coefficients that hold no decision (nor an easy `min`/`max` over such terms), or the decision
//! is not numeric. The caller owns the `when` question: a clause that carries one bounds only the rows it keeps.
bool DecideReadSingletonRow(ClientContext &context, const LogicalDecide &decide,
                                const BoundComparisonExpression &comparison, DecideSingletonRow &out);

//! The bounds on one decision as written (`DecideSingletonRow::written_k`, not the values they admit), and the
//! check the solver makes on them before it reads any row, so on an empty table too. Stage 08 raises the error this
//! check finds, and a direct rule that answers a query itself declines when it finds one, so the solver raises it.
class DecideWrittenBox {
public:
	//! Folds a bound the user wrote.
	void Add(char sense, double k);
	//! Folds a bound an IN lowering wrote. Only a negative floor counts: an IN with a negative value signs the decision
	//! down, so `x IN (-5, 0, 5) AND x <= -3` is no bound below the default floor of 0. The rest is not the user's
	//! bound, and exact diagnosis may omit its whole clause; the solve still reads the lowering's rows.
	void AddLowered(char sense, double k);
	//! True when the box contradicts the decision's built-in domain. A box with no lower bound written has the default
	//! floor of 0. It contradicts the domain when the upper bound is below 0 while the floor is still at or above 0 (a
	//! BOOL's floor is always 0, so for it any upper bound below 0), or when a BOOL's floor is above 1. A purely
	//! inverted box such as `x >= 5 AND x <= 1` is not a contradiction here; the elastic engine reports it.
	bool ContradictsDomain(bool is_bool) const;
	const DecideBoundBox &Box() const {
		return box;
	}

private:
	DecideBoundBox box;
};

} // namespace duckdb
