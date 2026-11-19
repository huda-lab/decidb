#include "duckdb/optimizer/decide/decide_value_checks.hpp"

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"

namespace duckdb {

unique_ptr<Expression> DecideActiveWhen(const ExpressionBuilder &calc, const Expression *when) {
	return calc.Out(when ? calc.If(calc.Wrap(when->Copy()), calc.Bool(true), calc.Bool(false)) : calc.Bool(true));
}

ExprPtr DecideOnKeptRows(const ExpressionBuilder &calc, const ExprPtr &kept, const ExprPtr &value) {
	return kept ? calc.If(kept, value, calc.Constant(Value(value->return_type))) : value;
}

ExprPtr DecideCheckOnKeptRows(const ExpressionBuilder &calc, const ExprPtr &kept, const ExprPtr &check) {
	return kept ? calc.If(kept, check, calc.Bool(true)) : check;
}

void DecideNameValue(DecideNumberNames &names, const Expression &value, int sign, const string &text,
                     optional_idx place) {
	const bool constant = DecideUnguarded(value).IsFoldable();
	if (constant && !place.IsValid()) {
		return;
	}
	if (names.values == 0) {
		names.first_sign = sign;
		names.text = text;
		names.place = place;
	} else {
		const string front = names.values == 1 && names.first_sign < 0 ? "-" : "";
		names.text = front + names.text + (sign < 0 ? " - " : " + ") + text;
		names.place = optional_idx();
	}
	names.constant = names.constant && constant;
	names.values++;
}

//! The call that raises for a number that is not finite: it names the expression of the query the number stands for
//! and carries its place, and for a number the plan computes it names the clause (TOO_LARGE), at the clause's place.
static ExprPtr NotFiniteRaise(const ExpressionBuilder &calc, const DecideNumberNames &names) {
	if (names.values > 0) {
		return calc.RaiseInvalidValue(names.constant ? DecideValueRole::CONSTANT : DecideValueRole::COEFFICIENT,
		                              names.text, names.place);
	}
	return calc.RaiseInvalidValue(DecideValueRole::TOO_LARGE, names.clause.Noun(), names.clause.place);
}

ExprPtr DecideNumberCheck(const ExpressionBuilder &calc, const ExprPtr &number, const DecideNumberNames &names) {
	// The gate is one BOOLEAN per row, and the raise stands in the branch of the rows that fail alone, as a CASE
	// evaluates a branch only where it is chosen. A NULL number is skipped.
	return calc.If(calc.Fn("isfinite", number), calc.Bool(true),
	               calc.If(calc.IsNull(number), calc.Bool(true), NotFiniteRaise(calc, names)));
}

ExprPtr DecideNumberCheck(const ExpressionBuilder &calc, const ExprPtr &number, const DecideClauseRef &clause) {
	DecideNumberNames names;
	names.clause = clause;
	return DecideNumberCheck(calc, number, names);
}

} // namespace duckdb
