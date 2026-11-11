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
#include "duckdb/planner/expression/bound_cast_expression.hpp"
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
	// Every constant the caller passes is one the engine built with `ExpressionBuilder::Num`.
	auto &constant = expr.Cast<BoundConstantExpression>().value;
	D_ASSERT(!constant.IsNull() && constant.type().id() == LogicalTypeId::DOUBLE);
	return DoubleValue::Get(constant) == value;
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

bool DirectReadLimit(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &constraint,
                     DirectLimit &limit) {
	if (TryEvaluateFoldableDouble(context, *constraint.rhs, limit.number, DIRECT_FINITE_CONSTANT)) {
		limit.constant = true;
		return std::fabs(limit.number) < DECIDE_EFFECTIVE_INFINITY;
	}
	limit.data.comparison = constraint.comparison;
	if (constraint.group_bound) {
		if (!DirectGroupBoundIsReadable(ir, *constraint.group_bound)) {
			return false;
		}
		limit.data.group = constraint.group_bound->Copy();
		return true;
	}
	if (!DirectIsSourceOnlyNumericMayRaise(*constraint.rhs, ir.decide_index, ir.source_bindings)) {
		return false;
	}
	limit.data.value = constraint.rhs->Copy();
	return true;
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

bool DirectIsFiniteWhereKept(Optimizer &optimizer, const vector<DirectSignedValue> &values,
                             const DirectFactor &factor) {
	if (values.empty()) {
		throw InternalException("DECIDE: a number check has no value to read; its caller promises at least one");
	}
	// The number is the sum of the values, each negated, then multiplied or divided by the part's factor
	// (DirectScaledTerm), so its magnitude is at most the sum of the values' bounds times the factor.
	double total = 0.0;
	for (auto &value : values) {
		total += DirectScaledMagnitude(optimizer, *value.expr, factor);
	}
	return DirectSumIsFinite(total);
}

double DirectScaledMagnitude(Optimizer &optimizer, const Expression &value, const DirectFactor &factor) {
	return std::fabs(factor.ApplyTo(DirectMagnitudeBound(optimizer.context, DecideUnguarded(value))));
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

bool DirectOnlyLinearTerms(const vector<DecideSplitTerm> &terms) {
	for (auto &term : terms) {
		if (!term.IsLinear()) {
			return false;
		}
	}
	return true;
}

bool DirectReadLinearPart(const DirectIR &ir, ClientContext &context, const DirectPart &part, const string &clause,
                          DirectLinearPart &out) {
	out.sign = part.sign;
	if (!DirectPartFactor(context, part, out.factor)) {
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

//===--------------------------------------------------------------------===//
// Fixed values of a BOOL decision
//===--------------------------------------------------------------------===//

bool DirectConstantSlope(ClientContext &context, const vector<DecideSplitTerm> &terms, double &slope) {
	slope = 0;
	for (auto &term : terms) {
		double value;
		if (!TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)) {
			return false;
		}
		slope += term.sign * value;
	}
	return true;
}

const vector<DecideSplitTerm> *DirectFixedTerms(const DirectIRConstraint &constraint) {
	if (constraint.aggregate || !DirectOnlyLinearTerms(constraint.lhs[0].terms)) {
		return nullptr;
	}
	return &constraint.lhs[0].terms;
}

//! True when every term's coefficient is a finite constant or a number of the source row (`w`, `w / 2`), which a plan
//! can read. A literal that is not a finite number is the solver's to report.
static bool SourceOnlyCoefficients(ClientContext &context, const DirectIR &ir, const vector<DecideSplitTerm> &terms) {
	for (auto &term : terms) {
		double value;
		if (term.coefficient->IsFoldable()
		        ? !TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)
		        : !DirectIsSourceOnlyNumeric(*term.coefficient, ir.decide_index, ir.source_bindings)) {
			return false;
		}
	}
	return true;
}

static const Expression *SourceBooleanFixedValue(const Expression &expr, idx_t decide_index,
                                                 const vector<ColumnBinding> &source_bindings) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return nullptr;
	}
	auto &cast = expr.Cast<BoundCastExpression>();
	if (cast.child->return_type != LogicalType::BOOLEAN || !expr.return_type.IsNumeric() ||
	    DirectMayThrow(*cast.child) || !DirectIsDecisionFreeDeterministic(*cast.child, decide_index) ||
	    !DirectReferencesOnlySource(*cast.child, source_bindings) || !DirectHasColumnReference(*cast.child)) {
		return nullptr;
	}
	return cast.child.get();
}

//! `value` on the rows a clause's `when` keeps, and false on the rest.
static unique_ptr<Expression> OnKeptRows(const ExpressionBuilder &calc, const Expression *when, const ExprPtr &value) {
	return calc.Out(when ? calc.If(calc.Of(*when), value, calc.Bool(false)) : value);
}

//! `x = flag`, `x >= flag` and `x <= flag` against a Boolean source fix the row to 1 where the source is true (`=`,
//! `>=`) and to 0 where it is false (`=`, `<=`).
static void FixFromBooleanSource(const ExpressionBuilder &calc, ExpressionType type, const Expression &source,
                                 const Expression *when, DirectFixedValues &fixed) {
	if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
		fixed.one_conditions.push_back(OnKeptRows(calc, when, calc.Of(source)));
	}
	if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
		fixed.zero_conditions.push_back(
		    OnKeptRows(calc, when, calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Of(source), calc.Bool(false))));
	}
	fixed.source_values.push_back({source.Copy(), false, when ? when->Copy() : nullptr, false, string()});
}

//! A clause read as the values of {0, 1} each row allows.
static bool FixFromAllowedValues(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &factor,
                                 bool constant_slope, DirectFixedValues &fixed) {
	ExpressionBuilder calc(context);
	auto when = factor.scope.when.get();
	auto &value = *factor.rhs;
	// A literal that is not a finite number is the solver's to report.
	double constant;
	bool constant_value = TryEvaluateFoldableDouble(context, value, constant, DIRECT_FINITE_CONSTANT);
	if (!constant_value &&
	    (value.IsFoldable() || !DirectIsSourceOnlyNumeric(value, ir.decide_index, ir.source_bindings))) {
		return false;
	}
	// Whether the row allows `x = v`. A singleton row allows the values between the ends the solver path's classifier
	// reads (DecideSingletonRow), as DuckDB evaluates the comparison: `2 * x >= 1` allows 1, `x < 1.5` allows 0 and 1.
	// A `<>` bounds nothing and allows the values that compare unequal.
	auto le = ExpressionType::COMPARE_LESSTHANOREQUALTO;
	auto allows = [&](double v) -> ExprPtr {
		if (factor.singleton_row) {
			return calc.If(calc.Cmp(le, calc.Wrap(factor.bound.RowLower()), calc.Num(v)),
			               calc.Cmp(le, calc.Num(v), calc.Wrap(factor.bound.RowUpper())), calc.Bool(false));
		}
		return calc.Cmp(factor.comparison, calc.Num(v), calc.Cast(calc.Of(value), LogicalType::DOUBLE));
	};
	if (constant_value && constant_slope) {
		double zero, one;
		auto as_number = [&](double v) {
			return calc.Out(calc.If(allows(v), calc.Num(1.0), calc.Num(0.0)));
		};
		if (!TryEvaluateFoldableDouble(context, *as_number(0.0), zero, FOLD_CATCH_ALL) ||
		    !TryEvaluateFoldableDouble(context, *as_number(1.0), one, FOLD_CATCH_ALL)) {
			return false;
		}
		// Both values allowed is no constraint. Neither allowed fixes the row to both, which the plan reports as
		// DECIDE's infeasible error.
		if (zero == 0.0) {
			fixed.one_conditions.push_back(OnKeptRows(calc, when, calc.Bool(true)));
		}
		if (one == 0.0) {
			fixed.zero_conditions.push_back(OnKeptRows(calc, when, calc.Bool(true)));
		}
		return true;
	}
	// A value from the row: the row is fixed to the one value it allows, and is infeasible when it allows neither.
	// The solver reads every coefficient of the row before its right side.
	auto disallows = [&](double v) {
		return calc.If(allows(v), calc.Bool(false), calc.Bool(true));
	};
	fixed.one_conditions.push_back(OnKeptRows(calc, when, disallows(0.0)));
	fixed.zero_conditions.push_back(OnKeptRows(calc, when, disallows(1.0)));
	for (auto &term : *DirectFixedTerms(factor)) {
		if (!term.coefficient->IsFoldable()) {
			fixed.source_values.push_back(
			    {term.coefficient->Copy(), true, when ? when->Copy() : nullptr, true, ir.render(*term.coefficient)});
		}
	}
	if (!constant_value) {
		auto reject_nan = value.return_type == LogicalType::FLOAT || value.return_type == LogicalType::DOUBLE;
		fixed.source_values.push_back(
		    {value.Copy(), reject_nan, when ? when->Copy() : nullptr, false, ir.render(value)});
	}
	return true;
}

//! One clause on `x` alone: the rows it fixes to 1 or to 0, and the values the plan checks on each row.
static bool ReadFixedValue(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &factor,
                           DirectFixedValues &fixed) {
	auto when = factor.scope.when.get();
	if (when && !DirectIsSourceOnlyPredicate(*when, ir.decide_index, ir.source_bindings)) {
		return false;
	}
	auto type = factor.comparison;
	auto &terms = *DirectFixedTerms(factor);
	// The coefficients add up to one constant, as the solver's like-term collection sums them, or, in a singleton
	// row, to a value of the row (`w * x >= 1`), whose ends the classifier reads per row. A `<>` and a Boolean source
	// compare one `x`: `x`, `1*x`, `(1+0)*x`.
	double slope;
	bool constant_slope = DirectConstantSlope(context, terms, slope);
	bool unit = constant_slope && slope == 1.0;
	if (!constant_slope && !(factor.singleton_row && SourceOnlyCoefficients(context, ir, terms))) {
		return false;
	}
	auto source_value = SourceBooleanFixedValue(*factor.rhs, ir.decide_index, ir.source_bindings);
	if (source_value && unit &&
	    (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	     type == ExpressionType::COMPARE_GREATERTHANOREQUALTO)) {
		FixFromBooleanSource(ExpressionBuilder(context), type, *source_value, when, fixed);
		return true;
	}
	if (!factor.singleton_row && !unit) {
		return false;
	}
	return FixFromAllowedValues(ir, context, factor, constant_slope, fixed);
}

bool DirectReadFixedValues(const DirectIR &ir, ClientContext &context,
                           const vector<const DirectIRConstraint *> &clauses, DirectFixedValues &fixed) {
	// A bound the solver folds that contradicts the Boolean's built-in domain makes it raise an error that quotes the
	// bound (`x >= 2 cannot hold for a BOOL variable.`) before it reads a row; that early error is not a rule's to
	// reproduce.
	if (DirectContradictsDomain(ir)) {
		return false;
	}
	for (auto clause : clauses) {
		if (!ReadFixedValue(ir, context, *clause, fixed)) {
			return false;
		}
	}
	return true;
}

unique_ptr<Expression> DirectAnyCondition(const ExpressionBuilder &calc,
                                          const vector<unique_ptr<Expression>> &conditions) {
	vector<ExprPtr> ordered;
	for (auto &condition : conditions) {
		ordered.push_back(calc.Of(*condition));
	}
	std::reverse(ordered.begin(), ordered.end());
	return calc.Out(calc.Any(ordered));
}

} // namespace duckdb
