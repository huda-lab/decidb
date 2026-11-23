#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_value_messages.hpp"
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

static bool DirectProveValues(const LogicalDecide &decide, const vector<DecideSplitTerm> &terms,
                              vector<DirectSignedValue> &out) {
	for (auto &term : terms) {
		if (!DirectProveValue(decide, *term.coefficient)) {
			return false;
		}
		DirectSignedValue value;
		value.sign = term.sign;
		value.expr = term.coefficient->Copy();
		out.push_back(std::move(value));
	}
	return true;
}

bool DirectContradictsDomain(const LogicalDecide &decide, const vector<DirectClause> &clauses) {
	// Every bound here is the user's (Add): direct solve runs before RewriteInDomain, so a rule reads an IN as written
	// and never the bounds its lowering adds (AddLowered).
	vector<DecideWrittenBox> written(decide.bound.variables.size());
	for (auto &ir_node : clauses) {
		if (ir_node.variable_bound) {
			written[ir_node.bound.var_idx].Add(ir_node.bound.sense, ir_node.bound.written_k);
		} else if (ir_node.When() && ir_node.singleton_row && ir_node.bound.LiftsDefaultFloor()) {
			// A floor under a `when` replaces the default on the rows it keeps (AddLowered counts it only when
			// negative); the solve says whether the clauses on the other rows leave a value.
			written[ir_node.bound.var_idx].AddLowered('>', ir_node.bound.written_k);
		}
	}
	for (idx_t decision = 0; decision < written.size(); decision++) {
		if (written[decision].ContradictsDomain(DirectDomainOf(decide, decision) == DirectDomain::BOOL)) {
			return true;
		}
	}
	// A bound written as a constant that no box takes (under a `when`) is checked by the solver on each row an instance
	// keeps (DecideModelEmitter::CheckDomain): against the box above, whose floor is the lowest any clause lowers the
	// decision to, with the bound's own ends put in. Which rows an instance keeps is known only from the data, so a
	// rule declines every such bound; the solver then raises the error or, where no row is kept, solves.
	for (auto &ir_node : clauses) {
		if (ir_node.Aggregate() || !ir_node.singleton_row || ir_node.variable_bound ||
		    !ir_node.bound.constant_on_read_rows) {
			continue;
		}
		auto box = written[ir_node.bound.var_idx].Box();
		if (std::isfinite(ir_node.bound.upper_value)) {
			DecideFoldSingletonRow(box, '<', ir_node.bound.upper_value);
		}
		if (std::isfinite(ir_node.bound.lower_value)) {
			DecideFoldSingletonRow(box, '>', ir_node.bound.lower_value);
		}
		if (DecideBoxContradictsDomain(box, DirectDomainOf(decide, ir_node.bound.var_idx) == DirectDomain::BOOL)) {
			return true;
		}
	}
	return false;
}

bool DirectReadBounds(const LogicalDecide &decide, const vector<DirectClause> &clauses, ClientContext &context,
                      DirectBoundReading &reading) {
	// The solver rejects, before it reads any row, a box that contradicts the variable's built-in domain. It says so on
	// an empty source too, so the query is left to the solver, which reports it, rather than proved here and answered
	// with no rows.
	if (DirectContradictsDomain(decide, clauses)) {
		return false;
	}
	idx_t decision_count = decide.bound.variables.size();
	// The values the bounds the solver folds admit, folded by the function the solver's own folding pass calls.
	vector<DecideBoundBox> admitted(decision_count);
	for (auto &ir_node : clauses) {
		if (ir_node.Aggregate()) {
			continue;
		}
		auto &bound = ir_node.bound;
		idx_t decision = bound.var_idx;
		// The classifier and the IR read the comparison with the same term splitter: one part of linear terms in the
		// decision.
		D_ASSERT(ir_node.singleton_row && ir_node.lhs.size() == 1);
		if (!DirectIsSourceOnlyValue(ir_node.Rhs(), decide.decide_index, decide.SourceBindings())) {
			return false;
		}
		if (ir_node.When() &&
		    !DirectIsSourceOnlyPredicate(*ir_node.When(), decide.decide_index, decide.SourceBindings())) {
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
		if (!DirectProveValues(decide, ir_node.lhs[0].terms, clause.coefficient)) {
			return false;
		}
		clause.rhs = ir_node.Rhs().Copy();
		clause.coefficient_names = DirectNamesOf(decide, clause.coefficient, ir_node.written);
		clause.rhs_text = DirectRender(decide, *clause.rhs);
		clause.rhs_place = DirectPlace(decide, *clause.rhs);
		clause.lower = bound.RowLower();
		clause.upper = bound.RowUpper();
		if (ir_node.When()) {
			clause.when = ir_node.When()->Copy();
			clause.lifts_floor = bound.LiftsDefaultFloor();
		}
		reading.clauses.push_back(std::move(clause));
	}
	// A floor under a `when` replaces the default floor of 0 only on a decision with no lower bound written, which a
	// BOOL always has: its floor stays 0.
	for (auto &clause : reading.clauses) {
		clause.lifts_floor = clause.lifts_floor && !admitted[clause.decision].lower_written &&
		                     DirectDomainOf(decide, clause.decision) != DirectDomain::BOOL;
	}
	reading.boxes.resize(decision_count);
	for (idx_t decision = 0; decision < decision_count; decision++) {
		// A written lower bound replaces the default of 0, except that a BOOL never leaves {0, 1}. An end of magnitude
		// 1e20 or more (a written -1e30 included) is no bound.
		bool boolean = DirectDomainOf(decide, decision) == DirectDomain::BOOL;
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
	// A box inverted by rounding alone is one point, as the model builder reads it: the lower end closes onto the top
	// (LEAST leaves a lower end at or below the top as it is). NaN, from an infinite difference, closes nothing.
	auto reach =
	    calc.Mul(calc.Num(DECIDE_ROUNDED_BOX_RELATIVE_TOLERANCE), calc.Max({calc.Num(1.0), calc.Fn("abs", upper)}));
	auto rounded = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, calc.Sub(lower, upper), reach);
	lower = calc.If(rounded, calc.Min({lower, upper}), lower);
}

ExprPtr DirectBoxIsEmpty(const ExpressionBuilder &calc, const ExprPtr &lower, const ExprPtr &upper) {
	return calc.Any({calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, lower, upper),
	                 calc.Cmp(ExpressionType::COMPARE_EQUAL, lower, calc.Num(POSITIVE_INFINITY)),
	                 calc.Cmp(ExpressionType::COMPARE_EQUAL, upper, calc.Num(NEGATIVE_INFINITY))});
}

bool DirectReadLimit(const LogicalDecide &decide, ClientContext &context, const DirectClause &constraint,
                     DirectLimit &limit) {
	if (TryEvaluateFoldableDouble(context, constraint.Rhs(), limit.number, DIRECT_FINITE_CONSTANT)) {
		limit.constant = true;
		return std::fabs(limit.number) < DECIDE_EFFECTIVE_INFINITY;
	}
	limit.data.comparison = constraint.Comparison();
	if (!DirectIsSourceOnlyNumericMayRaise(constraint.Rhs(), decide.decide_index, decide.SourceBindings())) {
		return false;
	}
	limit.data.value = constraint.Rhs().Copy();
	return true;
}

bool DirectReadCountLimit(const LogicalDecide &decide, ClientContext &context, const DirectClause &constraint,
                          bool negated, DirectCountLimit &count) {
	count.negated = negated;
	count.comparison = negated ? FlipComparisonExpression(constraint.Comparison()) : constraint.Comparison();
	if (!DirectReadLimit(decide, context, constraint, count.limit)) {
		return false;
	}
	if (negated && count.limit.constant) {
		count.limit.number = -count.limit.number;
	}
	return true;
}

DecideNumberNames DirectNamesOf(const LogicalDecide &decide, const vector<DirectSignedValue> &values,
                                const DecideClauseRef &clause) {
	DecideNumberNames names;
	names.clause = clause;
	for (auto &value : values) {
		DecideNameValue(names, *value.expr, value.sign, DirectRender(decide, *value.expr),
		                DirectPlace(decide, *value.expr));
	}
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

bool DirectReadLinearPart(const LogicalDecide &decide, ClientContext &context, const DirectPart &part,
                          const DecideClauseRef &clause, DirectLinearPart &out) {
	out.sign = part.sign;
	if (!DirectPartFactor(context, part, out.factor)) {
		return false;
	}
	for (auto &term : part.terms) {
		if (!term.IsLinear()) {
			continue;
		}
		if (!DirectProveValue(decide, *term.coefficient)) {
			return false;
		}
		out.terms.push_back({term.var_a, term.sign, term.coefficient->Copy()});
	}
	for (idx_t decision = 0; decision < decide.bound.variables.size(); decision++) {
		out.names.push_back(DirectNamesOf(decide, DirectValuesOn(out, decision), clause));
	}
	return true;
}

bool DirectReadConstantTerm(const LogicalDecide &decide, const DecideSplitTerm &term, const DecideClauseRef &clause,
                            DirectConstantTerm &out) {
	if (!DirectProveValue(decide, *term.coefficient)) {
		return false;
	}
	out.sign = term.sign;
	out.value = term.coefficient->Copy();
	vector<DirectSignedValue> value;
	value.push_back({term.sign, term.coefficient->Copy()});
	out.value_names = DirectNamesOf(decide, value, clause);
	return true;
}

//! The weight of a square or an absolute value: the term's sign and its coefficient (1 when it has none), which a rule
//! that needs it constant folds (DirectFoldConstantSum).
static bool DirectReadWeight(const LogicalDecide &decide, const DecideSplitTerm &term, const DecideClauseRef &clause,
                             vector<DirectSignedValue> &weight, DecideNumberNames &names) {
	if (term.coefficient && !DirectProveValue(decide, *term.coefficient)) {
		return false;
	}
	DirectSignedValue value;
	value.sign = term.sign;
	value.expr = term.coefficient ? term.coefficient->Copy() : make_uniq<BoundConstantExpression>(Value::DOUBLE(1.0));
	weight.push_back(std::move(value));
	names = DirectNamesOf(decide, weight, clause);
	return true;
}

//! The one decision a form's linear terms name; false when they name two. A form is what a square or an ABS of
//! decisions holds, so it names one, and the clause reader has refused a term in it that is neither linear nor
//! constant.
static bool DirectReadFormDecision(const vector<DecideSplitTerm> &form, idx_t &decision) {
	decision = DConstants::INVALID_INDEX;
	for (auto &inner : form) {
		if (!inner.IsLinear()) {
			continue;
		}
		if (decision != DConstants::INVALID_INDEX && decision != inner.var_a) {
			return false;
		}
		decision = inner.var_a;
	}
	return true;
}

bool DirectReadLinearForm(const LogicalDecide &decide, const vector<DecideSplitTerm> &form,
                          const DecideClauseRef &clause, DirectLinearForm &out) {
	if (!DirectReadFormDecision(form, out.decision)) {
		return false;
	}
	for (auto &inner : form) {
		if (!DirectProveValue(decide, *inner.coefficient)) {
			return false;
		}
		DirectSignedValue value;
		value.sign = inner.sign;
		value.expr = inner.coefficient->Copy();
		(inner.IsLinear() ? out.linear : out.constant).push_back(std::move(value));
	}
	out.linear_names = DirectNamesOf(decide, out.linear, clause);
	out.constant_names = DirectNamesOf(decide, out.constant, clause);
	return true;
}

bool DirectReadSquareTerm(const LogicalDecide &decide, const DecideSplitTerm &term, const DecideClauseRef &clause,
                          DirectWeightedForm &out) {
	return term.IsSquare() && DirectReadWeight(decide, term, clause, out.weight, out.weight_names) &&
	       DirectReadLinearForm(decide, term.factors[0].form, clause, out.inner);
}

bool DirectReadAbsTerm(const LogicalDecide &decide, const DecideSplitTerm &term, const DecideClauseRef &clause,
                       DirectWeightedForm &out) {
	if (!term.IsAbs()) {
		throw InternalException("DECIDE: an absolute value is expected here; the A2 rule promises an ABS term");
	}
	return DirectReadWeight(decide, term, clause, out.weight, out.weight_names) &&
	       DirectReadLinearForm(decide, term.inner, clause, out.inner);
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

bool DirectObjectivePartKept(const LogicalDecide &decide, const DirectPart &part, unique_ptr<Expression> &kept) {
	for (auto when : {decide.bound.objective.scope.when.get(), part.filter.get()}) {
		if (!when) {
			continue;
		}
		if (!DirectIsSourceOnlyPredicate(*when, decide.decide_index, decide.SourceBindings())) {
			return false;
		}
		kept =
		    kept ? make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(kept), when->Copy())
		         : when->Copy();
	}
	return true;
}

bool DirectReadQuadraticPart(const LogicalDecide &decide, ClientContext &context, const DirectPart &part,
                             const DecideClauseRef &clause, DirectQuadraticPart &out) {
	if (!DirectReadLinearPart(decide, context, part, clause, out.linear) ||
	    !DirectObjectivePartKept(decide, part, out.kept)) {
		return false;
	}
	for (auto &term : part.terms) {
		if (term.IsLinear()) {
			continue;
		}
		if (term.IsConstant()) {
			out.constants.emplace_back();
			if (!DirectReadConstantTerm(decide, term, clause, out.constants.back())) {
				return false;
			}
		} else {
			out.squares.emplace_back();
			if (!DirectReadSquareTerm(decide, term, clause, out.squares.back())) {
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

bool DirectReadCountBody(ClientContext &context, const vector<DecideSplitTerm> &terms, bool &negated) {
	double slope;
	if (!DirectOnlyLinearTerms(terms) || !DirectConstantSlope(context, terms, slope) ||
	    (slope != 1.0 && slope != -1.0)) {
		return false;
	}
	negated = slope < 0;
	return true;
}

const vector<DecideSplitTerm> *DirectFixedTerms(const DirectClause &constraint) {
	if (constraint.Aggregate() || !DirectOnlyLinearTerms(constraint.lhs[0].terms)) {
		return nullptr;
	}
	return &constraint.lhs[0].terms;
}

bool DirectIsOneRowDecision(const LogicalDecide &decide, DirectDomain domain) {
	return decide.bound.variables.size() == 1 && DirectDomainOf(decide, 0) == domain &&
	       decide.bound.variable_scopes[0].scope == DecideVarScope::ROW && decide.bound.keyed_scopes.empty();
}

bool DirectSplitFixes(const vector<DirectClause> &clauses, vector<const DirectClause *> &fixes,
                      vector<const DirectClause *> &others) {
	for (auto &constraint : clauses) {
		if (constraint.Comparison() == ExpressionType::COMPARE_IN) {
			return false;
		}
		(DirectFixedTerms(constraint) ? fixes : others).push_back(&constraint);
	}
	return true;
}

//! True when every term's coefficient is a finite constant or a number of the source row (`w`, `w / 2`, `w - 2`), which
//! a plan can read on the rows its clause keeps, where a coefficient that can raise raises as the solver does. A
//! literal that is not a finite number is the solver's to report.
static bool SourceOnlyCoefficients(ClientContext &context, const LogicalDecide &decide,
                                   const vector<DecideSplitTerm> &terms) {
	for (auto &term : terms) {
		double value;
		if (term.coefficient->IsFoldable()
		        ? !TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)
		        : !DirectIsSourceOnlyNumericMayRaise(*term.coefficient, decide.decide_index, decide.SourceBindings())) {
			return false;
		}
	}
	return true;
}

//! The Boolean a fixed value casts, when it cannot raise. A clause here fixes one side (`x >= flag` fixes to 1 only),
//! so another clause that is true first can skip evaluating it on a row where the solver raises; a Boolean that may
//! raise is read as a number instead (FixFromAllowedValues), which both sides evaluate.
static const Expression *SourceBooleanFixedValue(const Expression &expr, idx_t decide_index,
                                                 const vector<ColumnBinding> &source_bindings) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return nullptr;
	}
	auto &cast = expr.Cast<BoundCastExpression>();
	if (cast.child->return_type != LogicalType::BOOLEAN || DirectMayThrow(*cast.child) ||
	    !DirectIsDecisionFreeDeterministic(*cast.child, decide_index) ||
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
	fixed.source_values.push_back(
	    {source.Copy(), false, when ? when->Copy() : nullptr, false, string(), optional_idx()});
}

//! A clause read as the values of {0, 1} each row allows.
static bool FixFromAllowedValues(const LogicalDecide &decide, ClientContext &context, const DirectClause &factor,
                                 bool constant_slope, DirectFixedValues &fixed) {
	ExpressionBuilder calc(context);
	auto when = factor.When();
	auto &value = factor.Rhs();
	// A literal that is not a finite number is the solver's to report. A value of the row may raise: the plan reads it
	// on the rows the clause keeps (OnKeptRows), where the solver reads it too.
	double constant;
	bool constant_value = TryEvaluateFoldableDouble(context, value, constant, DIRECT_FINITE_CONSTANT);
	if (!constant_value && (value.IsFoldable() ||
	                        !DirectIsSourceOnlyNumericMayRaise(value, decide.decide_index, decide.SourceBindings()))) {
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
		return calc.Cmp(factor.Comparison(), calc.Num(v), calc.Cast(calc.Of(value), LogicalType::DOUBLE));
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
			fixed.source_values.push_back({term.coefficient->Copy(), true, when ? when->Copy() : nullptr, true,
			                               DirectRender(decide, *term.coefficient),
			                               DirectPlace(decide, *term.coefficient)});
		}
	}
	if (!constant_value) {
		auto reject_nan = value.return_type == LogicalType::FLOAT || value.return_type == LogicalType::DOUBLE;
		fixed.source_values.push_back({value.Copy(), reject_nan, when ? when->Copy() : nullptr, false,
		                               DirectRender(decide, value), DirectPlace(decide, value)});
	}
	return true;
}

//! One clause on `x` alone: the rows it fixes to 1 or to 0, and the values the plan checks on each row.
static bool ReadFixedValue(const LogicalDecide &decide, ClientContext &context, const DirectClause &factor,
                           DirectFixedValues &fixed) {
	auto when = factor.When();
	if (when && !DirectIsSourceOnlyPredicate(*when, decide.decide_index, decide.SourceBindings())) {
		return false;
	}
	auto type = factor.Comparison();
	auto &terms = *DirectFixedTerms(factor);
	// The coefficients add up to one constant, as the solver's like-term collection sums them, or, in a singleton
	// row, to a value of the row (`w * x >= 1`), whose ends the classifier reads per row. A `<>` and a Boolean source
	// compare one `x`: `x`, `1*x`, `(1+0)*x`.
	double slope;
	bool constant_slope = DirectConstantSlope(context, terms, slope);
	bool unit = constant_slope && slope == 1.0;
	if (!constant_slope && !(factor.singleton_row && SourceOnlyCoefficients(context, decide, terms))) {
		return false;
	}
	auto source_value = SourceBooleanFixedValue(factor.Rhs(), decide.decide_index, decide.SourceBindings());
	if (source_value && unit &&
	    (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	     type == ExpressionType::COMPARE_GREATERTHANOREQUALTO)) {
		FixFromBooleanSource(ExpressionBuilder(context), type, *source_value, when, fixed);
		return true;
	}
	if (!factor.singleton_row && !unit) {
		return false;
	}
	return FixFromAllowedValues(decide, context, factor, constant_slope, fixed);
}

bool DirectReadFixedValues(const LogicalDecide &decide, const vector<DirectClause> &clauses, ClientContext &context,
                           const vector<const DirectClause *> &fixes, DirectFixedValues &fixed) {
	// A bound the solver folds that contradicts the Boolean's built-in domain makes it raise an error that quotes the
	// bound (`x >= 2 cannot hold for a BOOL variable.`) before it reads a row; that early error is not a rule's to
	// reproduce.
	if (DirectContradictsDomain(decide, clauses)) {
		return false;
	}
	for (auto clause : fixes) {
		if (!ReadFixedValue(decide, context, *clause, fixed)) {
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
