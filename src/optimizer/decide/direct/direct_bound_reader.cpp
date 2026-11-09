#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

static bool DirectProveValues(const DirectIR &ir, const vector<DecideSplitTerm> &terms,
                              vector<DirectSignedValue> &out) {
	for (auto &term : terms) {
		if (!DirectProveValue(ir, *term.coefficient)) {
			return false;
		}
		DirectSignedValue value;
		value.sign = term.sign;
		value.expr = term.coefficient->Copy();
		out.push_back(std::move(value));
	}
	return true;
}

bool DirectContradictsDomain(const DirectIR &ir) {
	// Every bound here is the user's (Add): direct solve runs before RewriteInDomain, so a rule reads an IN as written
	// and never the bounds its lowering adds (AddLowered).
	vector<DecideWrittenBox> written(ir.decisions.size());
	for (auto &ir_node : ir.constraints) {
		if (ir_node.variable_bound) {
			written[ir_node.bound.var_idx].Add(ir_node.bound.sense, ir_node.bound.written_k);
		} else if (ir_node.scope.when && ir_node.singleton_row && ir_node.bound.LiftsDefaultFloor()) {
			// A floor under a `when` replaces the default on the rows it keeps (AddLowered counts it only when
			// negative); the solve says whether the clauses on the other rows leave a value.
			written[ir_node.bound.var_idx].AddLowered('>', ir_node.bound.written_k);
		}
	}
	for (idx_t decision = 0; decision < written.size(); decision++) {
		if (written[decision].ContradictsDomain(ir.decisions[decision].domain == DirectDomain::BOOL)) {
			return true;
		}
	}
	return false;
}

bool DirectReadBounds(const DirectIR &ir, ClientContext &context, DirectBoundReading &reading) {
	// The solver rejects, before it reads any row, a box that contradicts the variable's built-in domain. It says so on
	// an empty source too, so the query is left to the solver, which reports it, rather than proved here and answered
	// with no rows.
	if (DirectContradictsDomain(ir)) {
		return false;
	}
	idx_t decision_count = ir.decisions.size();
	// The values the bounds the solver folds admit, folded by the function the solver's own folding pass calls.
	vector<DecideBoundBox> admitted(decision_count);
	for (auto &ir_node : ir.constraints) {
		if (ir_node.aggregate) {
			continue;
		}
		auto &bound = ir_node.bound;
		idx_t decision = bound.var_idx;
		// The classifier and the IR read the comparison with the same term splitter: one part of linear terms in the
		// decision.
		D_ASSERT(ir_node.singleton_row && ir_node.lhs.size() == 1);
		if (!DirectIsSourceOnlyValue(*ir_node.rhs, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		if (ir_node.scope.when &&
		    !DirectIsSourceOnlyPredicate(*ir_node.scope.when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		if (ir_node.variable_bound) {
			if (bound.sense != '>') {
				DecideFoldSingletonRow(admitted[decision], '<', bound.upper_value);
			}
			if (bound.sense != '<') {
				DecideFoldSingletonRow(admitted[decision], '>', bound.lower_value);
			}
			continue;
		}
		DirectBoundClause clause;
		clause.decision = decision;
		if (!DirectProveValues(ir, ir_node.lhs[0].terms, clause.coefficient)) {
			return false;
		}
		clause.rhs = ir_node.rhs->Copy();
		clause.coefficient_names = DirectNamesOf(ir, clause.coefficient, ir_node.written);
		clause.rhs_text = ir.render(*clause.rhs);
		clause.lower = bound.RowLower();
		clause.upper = bound.RowUpper();
		if (ir_node.scope.when) {
			clause.when = ir_node.scope.when->Copy();
			clause.lifts_floor = bound.LiftsDefaultFloor();
		}
		reading.clauses.push_back(std::move(clause));
	}
	// A floor under a `when` replaces the default floor of 0 only on a decision with no lower bound written, which a
	// BOOL always has: its floor stays 0.
	for (auto &clause : reading.clauses) {
		clause.lifts_floor = clause.lifts_floor && !admitted[clause.decision].lower_written &&
		                     ir.decisions[clause.decision].domain != DirectDomain::BOOL;
	}
	reading.boxes.resize(decision_count);
	for (idx_t decision = 0; decision < decision_count; decision++) {
		// A written lower bound replaces the default of 0, except that a BOOL never leaves {0, 1}. An end of magnitude
		// 1e20 or more (a written -1e30 included) is no bound.
		bool boolean = ir.decisions[decision].domain == DirectDomain::BOOL;
		auto &box = admitted[decision];
		double lower = DecideIsOpenBelow(box.lower) ? NEGATIVE_INFINITY : box.lower;
		double upper = DecideIsOpenAbove(box.upper) ? POSITIVE_INFINITY : box.upper;
		reading.boxes[decision].lower = box.lower_written ? (boolean ? std::max(0.0, lower) : lower) : 0.0;
		reading.boxes[decision].upper = boolean ? std::min(1.0, upper) : upper;
	}
	return true;
}

void DirectClauseBounds(const ExpressionBuilder &calc, const DirectBoundColumns &columns, ExprPtr &lower,
                        ExprPtr &upper) {
	lower = calc.Col(LogicalType::DOUBLE, columns.lower);
	upper = calc.Col(LogicalType::DOUBLE, columns.upper);
	if (columns.has_when) {
		auto active = calc.Col(LogicalType::BOOLEAN, columns.when);
		lower = calc.If(active, lower, calc.Num(NEGATIVE_INFINITY));
		upper = calc.If(active, upper, calc.Num(POSITIVE_INFINITY));
	}
}

ExprPtr DirectStaticFloor(const ExpressionBuilder &calc, double static_lower,
                          const vector<ColumnBinding> &lowering_whens) {
	if (lowering_whens.empty()) {
		return calc.Num(static_lower);
	}
	vector<ExprPtr> whens;
	for (auto &binding : lowering_whens) {
		whens.push_back(calc.Col(LogicalType::BOOLEAN, binding));
	}
	return calc.If(calc.Any(whens), calc.Num(NEGATIVE_INFINITY), calc.Num(static_lower));
}

//! Whether `expr` is the DOUBLE constant `value`.
static bool DirectIsDoubleConstant(const Expression &expr, double value) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &constant = expr.Cast<BoundConstantExpression>().value;
	return !constant.IsNull() && constant.type().id() == LogicalTypeId::DOUBLE && DoubleValue::Get(constant) == value;
}

void DirectRowBox(const ExpressionBuilder &calc, const ExprPtr &static_lower, double static_upper,
                  const vector<ColumnBinding> &clause_lowers, const vector<ColumnBinding> &clause_uppers,
                  ExprPtr &lower, ExprPtr &upper) {
	// An infinite static end clamps nothing: +infinity is LEAST's identity and -infinity GREATEST's for every value but
	// NaN, and a NaN bound has raised before the box is read. With no other end the box is open on that side.
	vector<ExprPtr> lowers, uppers;
	if (!DirectIsDoubleConstant(*static_lower, NEGATIVE_INFINITY)) {
		lowers.push_back(static_lower);
	}
	if (static_upper != POSITIVE_INFINITY) {
		uppers.push_back(calc.Num(static_upper));
	}
	for (auto &binding : clause_lowers) {
		lowers.push_back(calc.Col(LogicalType::DOUBLE, binding));
	}
	for (auto &binding : clause_uppers) {
		uppers.push_back(calc.Col(LogicalType::DOUBLE, binding));
	}
	lower = lowers.empty() ? calc.Num(NEGATIVE_INFINITY) : calc.Max(lowers);
	upper = uppers.empty() ? calc.Num(POSITIVE_INFINITY) : calc.Min(uppers);
}

ExprPtr DirectBoxIsEmpty(const ExpressionBuilder &calc, const ExprPtr &lower, const ExprPtr &upper) {
	return calc.Any({calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, lower, upper),
	                 calc.Cmp(ExpressionType::COMPARE_EQUAL, lower, calc.Num(POSITIVE_INFINITY)),
	                 calc.Cmp(ExpressionType::COMPARE_EQUAL, upper, calc.Num(NEGATIVE_INFINITY))});
}

DirectNumberNames DirectNamesOf(const DirectIR &ir, const vector<DirectSignedValue> &values, const string &clause) {
	DirectNumberNames names;
	for (auto &value : values) {
		// A literal under a NULL guard (it reads 0 on a skipped row) is still a literal.
		if (!DecideUnguarded(*value.expr).IsFoldable()) {
			names.inputs.push_back(value.expr->Copy());
			names.texts.push_back(ir.render(*value.expr));
		} else if (DecideUnguarded(*value.expr).GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			// A literal past the DOUBLE range is a number too large, not a constant SQL computed as infinite.
			names.constants.push_back(DecideUnguarded(*value.expr).Copy());
			names.constant_texts.push_back(ir.render(*value.expr));
		}
	}
	names.clause = clause;
	return names;
}

bool DirectIsFiniteWhereKept(Optimizer &optimizer, const vector<DirectSignedValue> &values, bool scaled, double scale,
                             bool divides) {
	if (values.empty()) {
		throw InternalException("DECIDE: a number check has no value to read; its caller promises at least one");
	}
	// The number is the sum of the values, each negated, then multiplied or divided by the part's factor
	// (DirectScaledTerm), so its magnitude is at most the sum of the values' bounds times the factor.
	double total = 0.0;
	for (auto &value : values) {
		total += DirectScaledMagnitude(optimizer, *value.expr, scaled, scale, divides);
	}
	return DirectSumIsFinite(total);
}

double DirectScaledMagnitude(Optimizer &optimizer, const Expression &value, bool scaled, double scale, bool divides) {
	double bound = DirectMagnitudeBound(optimizer.context, DecideUnguarded(value));
	if (scaled) {
		bound = divides ? bound / std::fabs(scale) : bound * std::fabs(scale);
	}
	return bound;
}

bool DirectSumIsFinite(double total) {
	// Half the range leaves room for the rounding of the products and the sum themselves.
	return total < std::numeric_limits<double>::max() / 2;
}

vector<DirectSignedValue> DirectValuesOn(const DirectLinearPart &part, idx_t decision) {
	vector<DirectSignedValue> values;
	for (auto &term : part.terms) {
		if (term.decision == decision) {
			values.push_back({term.sign, term.coefficient->Copy()});
		}
	}
	return values;
}

bool DirectReadLinearPart(const DirectIR &ir, ClientContext &context, const DirectPart &part, const string &clause,
                          DirectLinearPart &out) {
	out.sign = part.sign;
	out.scaled = part.scale != nullptr;
	out.divides = part.scale_divides;
	if (!DirectPartScale(context, part, out.scale)) {
		return false;
	}
	for (auto &term : part.terms) {
		if (!term.IsLinear()) {
			continue;
		}
		if (!DirectProveValue(ir, *term.coefficient)) {
			return false;
		}
		out.terms.push_back({term.var_a, term.sign, term.coefficient->Copy()});
	}
	// The part's factor is a literal, which no column holds: a message names the terms' own values.
	for (idx_t decision = 0; decision < ir.decisions.size(); decision++) {
		out.names.push_back(DirectNamesOf(ir, DirectValuesOn(out, decision), clause));
	}
	return true;
}

bool DirectReadConstantTerm(const DirectIR &ir, const DecideSplitTerm &term, const string &clause,
                            DirectConstantTerm &out) {
	if (!DirectProveValue(ir, *term.coefficient)) {
		return false;
	}
	out.sign = term.sign;
	out.value = term.coefficient->Copy();
	vector<DirectSignedValue> value;
	value.push_back({term.sign, term.coefficient->Copy()});
	out.value_names = DirectNamesOf(ir, value, clause);
	return true;
}

//! The weight of a square or an absolute value: the term's sign and its coefficient (1 when it has none), which a rule
//! that needs it constant folds (DirectFoldConstantSum).
static bool DirectReadWeight(const DirectIR &ir, const DecideSplitTerm &term, const string &clause,
                             vector<DirectSignedValue> &weight, DirectNumberNames &names) {
	if (term.coefficient && !DirectProveValue(ir, *term.coefficient)) {
		return false;
	}
	DirectSignedValue value;
	value.sign = term.sign;
	value.expr = term.coefficient ? term.coefficient->Copy() : make_uniq<BoundConstantExpression>(Value::DOUBLE(1.0));
	weight.push_back(std::move(value));
	names = DirectNamesOf(ir, weight, clause);
	return true;
}

//! The one decision a form's linear terms name; false when they name two, none, or a term is neither linear nor
//! constant.
static bool DirectReadFormDecision(const DirectIR &ir, const vector<DecideSplitTerm> &form, idx_t &decision) {
	bool has_decision = false;
	for (auto &inner : form) {
		if (inner.IsLinear()) {
			if (has_decision && decision != inner.var_a) {
				return false;
			}
			decision = inner.var_a;
			has_decision = true;
		} else if (!inner.IsConstant()) {
			return false;
		}
	}
	return has_decision;
}

bool DirectReadLinearForm(const DirectIR &ir, const vector<DecideSplitTerm> &form, const string &clause,
                          DirectLinearForm &out) {
	if (!DirectReadFormDecision(ir, form, out.decision)) {
		return false;
	}
	for (auto &inner : form) {
		if (!DirectProveValue(ir, *inner.coefficient)) {
			return false;
		}
		DirectSignedValue value;
		value.sign = inner.sign;
		value.expr = inner.coefficient->Copy();
		(inner.IsLinear() ? out.linear : out.constant).push_back(std::move(value));
	}
	out.linear_names = DirectNamesOf(ir, out.linear, clause);
	out.constant_names = DirectNamesOf(ir, out.constant, clause);
	return true;
}

bool DirectReadSquareTerm(const DirectIR &ir, const DecideSplitTerm &term, const string &clause,
                          DirectWeightedForm &out) {
	return term.IsSquare() && DirectReadWeight(ir, term, clause, out.weight, out.weight_names) &&
	       DirectReadLinearForm(ir, term.factors[0].form, clause, out.inner);
}

bool DirectReadAbsTerm(const DirectIR &ir, const DecideSplitTerm &term, const string &clause, DirectWeightedForm &out) {
	if (!term.IsAbs()) {
		throw InternalException("DECIDE: an absolute value is expected here; the A2 rule promises an ABS term");
	}
	return DirectReadWeight(ir, term, clause, out.weight, out.weight_names) &&
	       DirectReadLinearForm(ir, term.inner, clause, out.inner);
}

bool DirectFoldConstantSum(ClientContext &context, const vector<DirectSignedValue> &values, double &total) {
	total = 0.0;
	for (auto &value : values) {
		double constant;
		if (!TryEvaluateFoldableDouble(context, DecideUnguarded(*value.expr), constant, DIRECT_FINITE_CONSTANT)) {
			return false;
		}
		total += value.sign * constant;
	}
	return true;
}

bool DirectObjectivePartKept(const DirectIR &ir, const DirectPart &part, unique_ptr<Expression> &kept) {
	for (auto when : {ir.objective.scope.when.get(), part.filter.get()}) {
		if (!when) {
			continue;
		}
		if (!DirectIsSourceOnlyPredicate(*when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		kept =
		    kept ? make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(kept), when->Copy())
		         : when->Copy();
	}
	return true;
}

bool DirectReadQuadraticPart(const DirectIR &ir, ClientContext &context, const DirectPart &part, const string &clause,
                             DirectQuadraticPart &out) {
	if (!DirectReadLinearPart(ir, context, part, clause, out.linear) || !DirectObjectivePartKept(ir, part, out.kept)) {
		return false;
	}
	for (auto &term : part.terms) {
		if (term.IsLinear()) {
			continue;
		}
		if (term.IsConstant()) {
			out.constants.emplace_back();
			if (!DirectReadConstantTerm(ir, term, clause, out.constants.back())) {
				return false;
			}
		} else {
			out.squares.emplace_back();
			if (!DirectReadSquareTerm(ir, term, clause, out.squares.back())) {
				return false;
			}
		}
	}
	return true;
}

} // namespace duckdb
