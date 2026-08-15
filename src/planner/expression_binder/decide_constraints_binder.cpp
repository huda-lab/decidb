#include "duckdb/planner/expression_binder/decide_constraints_binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

DecideConstraintsBinder::DecideConstraintsBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                                                 const case_insensitive_set_t &scalar_variables,
                                                 optional_ptr<DecideQualifierContext> qualifier_context)
    : DecideBinder(binder, context, variables, scalar_variables, qualifier_context) {
}

static bool IsAllowedDecisionFreeBoundExpression(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables);

static bool IsSupportedComparison(ExpressionType type) {
    switch (type) {
    case ExpressionType::COMPARE_EQUAL:
    case ExpressionType::COMPARE_LESSTHAN:
    case ExpressionType::COMPARE_GREATERTHAN:
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
    case ExpressionType::COMPARE_NOTEQUAL:
        return true;
    default:
        return false;
    }
}

//! Does this side of a comparison constrain a decision at all?
//!
//! This used to be `IsDecideConstraintLHS`, and the name was the whole problem: it
//! gated a *position*, so the binder required the DECIDE expression on the left and
//! flipped the comparison when it was not (the canonicalization refactor). Which side a term
//! belongs on is DecideCanonicalizer's decision, and it makes the same flip itself on
//! the bound tree -- so the question here is side-agnostic, and a comparison is a
//! constraint when EITHER side answers yes.
static bool IsDecideSide(DecideExpression type) {
    return type != DecideExpression::INVALID;
}

static bool IsAllowedOperatorChildren(const vector<unique_ptr<ParsedExpression>> &children,
                                      const case_insensitive_map_t<idx_t> &variables) {
    for (auto &child : children) {
        if (!IsAllowedDecisionFreeBoundExpression(*child, variables)) {
            return false;
        }
    }
    return true;
}

static bool IsAllowedDecisionFreeBoundExpression(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::CONSTANT:
            return true;
        case ExpressionClass::FUNCTION: {
            auto &func = expr.Cast<FunctionExpression>();
            if (func.is_operator) {
                // A WHEN / PER / qualifier wrapper's children past the first are a
                // predicate, PER key columns, or a relation alias -- not values on this
                // side -- so validating them as bounds is a category error. A bare `w`
                // is a legal WHEN condition and an illegal bound, and checking it as
                // the latter rejected the whole wrapper. Same rule K0 already states
                // for the canonicalizer: recurse into child 0 only.
                //
                // Reachable since the bind-time hoist was deleted by the
                // canonicalization refactor; before that, `<= SUM(b) WHEN w` was
                // rewritten away before this
                // check ever saw it. The physical layer has always had the matching
                // stages -- EvaluateRhsReducerPerGroup applies the reducer's own filter
                // and BuildQualifierKeepMask its de-duplication -- so this opens paths
                // that were built and unreachable, not new ones.
                if (func.function_name == WHEN_CONSTRAINT_TAG ||
                    func.function_name == QUALIFIED_REDUCER_TAG ||
                    IsPerConstraintTag(func.function_name)) {
                    return !func.children.empty() &&
                           IsAllowedDecisionFreeBoundExpression(*func.children[0], variables);
                }
                // `-` used to be refused outright while `+` was allowed. Nothing
                // downstream needs that asymmetry: a bound is evaluated as an
                // expression over the row, so subtraction and negation compose from
                // allowed operands exactly like addition does. The ban dated from the
                // parsed-level symbolic layer, which moved terms across the comparison
                // and is gone. It also refused `-5.0::DOUBLE`, where the minus is the
                // literal's own sign, so a negative bound could not be written with a
                // cast at all.
                if (!IsAllowedOperatorChildren(func.children, variables)) {
                    return false;
                }
                if (func.filter && !IsAllowedDecisionFreeBoundExpression(*func.filter, variables)) {
                    return false;
                }
                return true;
            }
            auto fn = StringUtil::Lower(func.function_name);
            if (fn == "sum" || fn == "avg" || fn == "min" || fn == "max") {
                if (func.children.empty()) {
                    return false;
                }
                if (func.children.size() != 1) {
                    return false;
                }
                if (func.filter && !IsAllowedDecisionFreeBoundExpression(*func.filter, variables)) {
                    return false;
                }
                if (ExpressionContainsDecideVariable(*func.children[0], variables)) {
                    return false;
                }
                return true;
            }
            for (auto &child : func.children) {
                if (!IsAllowedDecisionFreeBoundExpression(*child, variables)) {
                    return false;
                }
            }
            if (func.filter && !IsAllowedDecisionFreeBoundExpression(*func.filter, variables)) {
                return false;
            }
            return true;
        }
        case ExpressionClass::OPERATOR: {
            auto &op = expr.Cast<OperatorExpression>();
            return IsAllowedOperatorChildren(op.children, variables);
        }
        case ExpressionClass::CAST: {
            auto &cast = expr.Cast<CastExpression>();
            return IsAllowedDecisionFreeBoundExpression(*cast.child, variables);
        }
        case ExpressionClass::SUBQUERY: {
            auto &subquery = expr.Cast<SubqueryExpression>();
            if (subquery.subquery_type != SubqueryType::SCALAR) {
                return false;
            }
            return true;
        }
        default:
            return false;
    }
}

BindResult DecideConstraintsBinder::BindComparison(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &comp = expr.Cast<ComparisonExpression>();

    if (!IsSupportedComparison(comp.type)) {
        return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT constraint clause does not support '%s'(ExpressionType::%s)", expr.ToString(), EnumUtil::ToString(comp.type))));
    }

    // This function no longer rewrites the parsed tree at all. It used to do two
    // things beyond validating: flip the sides (the canonicalization refactor), and strip
    // an `expr + 0` residue from the right side that the parsed-level symbolic
    // layer left behind. That layer was deleted for constraints at C.4, so nothing
    // rewrites a constraint before binding any more; a probe confirmed the strip
    // never fired across the golden corpus or the suite, and it went at C.2.

    // Classify BOTH sides. The comparison is a constraint when either one is a
    // DECIDE expression -- `SUM(x) <= cap` and `cap >= SUM(x)` are the same
    // constraint, and so are `x <= 5` and `5 >= x`. Nothing here rewrites the
    // comparison to make that true: DecideCanonicalizer swaps the sides on the
    // BOUND tree when every decision term sits on the right (the canonicalization refactor),
    // so a second, earlier, parsed-level flip was the last of the five duplicate
    // shape decisions the canonicalization plan exists to remove.
    string left_error, right_error;
    auto left_type = GetExpressionType(*comp.left, left_error);
    auto right_type = GetExpressionType(*comp.right, right_error);

    if (!IsDecideSide(left_type) && !IsDecideSide(right_type)) {
        // Neither side decides anything. Report the left-hand diagnosis: it is the
        // one the user reads first, and for the common `col <= 5` it is the accurate
        // one -- classification only ever fails on a side, never on the relation.
        return BindResult(BinderException::Unsupported(expr, left_error));
    }

    // A reduced constraint collapses many rows to one number, so a side of it that
    // carries no decision has to reduce to one value too. That is a property of the
    // BOUND, not of a position, so it is checked on whichever side is the bound;
    // when both sides bear decisions there is no bound and nothing to check.
    if (left_type == DecideExpression::SUM || right_type == DecideExpression::SUM) {
        if (!ContainsDecideAggregate(*comp.left) && !ContainsDecideAggregate(*comp.right)) {
            return BindResult(BinderException::Unsupported(expr, "DECIDE constraint must contain SUM(...), AVG(...), MIN(...), or MAX(...)"));
        }
        auto IsValidBound = [&](const ParsedExpression &side) {
            return IsAllowedDecisionFreeBoundExpression(side, variables) &&
                   !ExpressionContainsDecideVariable(side, variables);
        };
        if ((!IsDecideSide(left_type) && !IsValidBound(*comp.left)) ||
            (!IsDecideSide(right_type) && !IsValidBound(*comp.right))) {
            return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUM cannot be compared to an expression that is not a scalar or aggregate without DECIDE variables, found '%s'", expr.ToString())));
        }
    }
    is_top_expression = false;
    return ExpressionBinder::BindExpression(expr_ptr, depth);
}

BindResult DecideConstraintsBinder::BindOperator(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &op = expr.Cast<OperatorExpression>();
    switch (op.type) {
    case ExpressionType::COMPARE_IN:{
        if (op.children.size() < 2 || !IsVariableExpression(*op.children.front(), variables)) {
            return BindResult(BinderException::Unsupported(expr, StringUtil::Format(
                "SUCH THAT does not support IN on '%s'. Only simple DECIDE variables are allowed as the IN target",
                op.children.front()->ToString())));
        }
        for (idx_t i = 1; i < op.children.size(); i++) {
            if (ExpressionContainsDecideVariable(*op.children[i], variables)) {
                return BindResult(BinderException::Unsupported(expr,
                    "IN domain constraints on DECIDE variables are not yet supported. "
                    "The values in the IN list must be constants or table columns, not DECIDE variables."));
            }
        }
        // Keep the native bound operator as an optimizer marker. DuckDB binds its
        // normal coercions here; DecideOptimizer expands it into the exact existing
        // indicator/cardinality/linking formulation.
        auto was_top_expression = is_top_expression;
        is_top_expression = false;
        auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
        is_top_expression = was_top_expression;
        return result;
    }
    default:
        return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT constraint clause does not support '%s'(ExpressionType::%s)", expr.ToString(), EnumUtil::ToString(op.type))));
    }
}

BindResult DecideConstraintsBinder::BindBetween(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &between = expr.Cast<BetweenExpression>();

    // Transform BETWEEN into (input >= lower) AND (input <= upper)
    auto input_copy = between.input->Copy();
    
    auto lower_comp = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(between.input), std::move(between.lower));
    auto upper_comp = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(input_copy), std::move(between.upper));

    auto conjunction = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower_comp), std::move(upper_comp));
    
    // Bind the new conjunction
    // We need to replace the current expression pointer with the new conjunction
    expr_ptr = std::move(conjunction);
    return BindConjunction(expr_ptr, depth);
}

BindResult DecideConstraintsBinder::BindConjunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &conj = expr.Cast<ConjunctionExpression>();
    // first try to bind the children of the case expression
    ErrorData error;
    for (idx_t i = 0; i < conj.children.size(); i++) {
        is_top_expression = true;
        BindChild(conj.children[i], depth, error);
    }
    if (error.HasError()) {
        return BindResult(std::move(error));
    }
    // the children have been successfully resolved
    // cast the input types to boolean (if necessary)
    // and construct the bound conjunction expression
    auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
    for (auto &child_expr : conj.children) {
        auto &child = BoundExpression::GetExpression(*child_expr);
        result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(child), LogicalType::BOOLEAN));
    }
    // now create the bound conjunction expression
    return BindResult(std::move(result));
}

BindResult DecideConstraintsBinder::BindWhenConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &func = expr_ptr->Cast<FunctionExpression>();
    D_ASSERT(func.children.size() == 2);

    // Validate: WHEN condition (child[1]) cannot reference DECIDE variables
    if (ExpressionContainsDecideVariable(*func.children[1], variables)) {
        return BindResult(BinderException::Unsupported(*expr_ptr,
            "WHEN conditions cannot reference DECIDE variables. "
            "The WHEN condition must only reference table columns."));
    }
    if (ContainsWhenOperator(*func.children[0])) {
        return BindResult(BinderException::Unsupported(*expr_ptr,
            "Cannot combine expression-level WHEN with aggregate-local WHEN in the same DECIDE constraint. "
            "Move the shared condition into each aggregate-local WHEN, or keep a single expression-level WHEN."));
    }

    // A relation-qualified reducer (`sum(D: ...)`) is not a `func_application`, so WHEN
    // written right after it always parses as this (whole-constraint) form rather than
    // aggregate-local WHEN, taking everything after it — including a trailing comparison
    // like `<= bound` — as the condition. What is left in child[0] is then the bare
    // reducer, which can never be a constraint on its own. Catch that shape here with a
    // message that names the fix, instead of falling through to the generic dispatch
    // below, which would report the reducer's internal tag.
    if (func.children[0]->GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &candidate = func.children[0]->Cast<FunctionExpression>();
        if (candidate.is_operator && candidate.function_name == QUALIFIED_REDUCER_TAG &&
            candidate.children.size() == 2 &&
            candidate.children[0]->GetExpressionClass() == ExpressionClass::FUNCTION &&
            candidate.children[1]->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
            auto agg_name = candidate.children[0]->Cast<FunctionExpression>().function_name;
            auto relation = candidate.children[1]->Cast<ColumnRefExpression>().GetColumnName();
            return BindResult(BinderException::Unsupported(*expr_ptr,
                StringUtil::Format(
                    "A relation-qualified reducer's WHEN must follow the comparison, not precede it. "
                    "Write %s(%s: ...) <= bound WHEN cond.",
                    StringUtil::Upper(agg_name), relation)));
        }
    }

    // Bind the constraint (child[0]) through normal DECIDE constraint dispatch
    is_top_expression = true;
    ErrorData constraint_error;
    BindChild(func.children[0], depth, constraint_error);
    if (constraint_error.HasError()) {
        return BindResult(std::move(constraint_error));
    }

    // Bind the condition (child[1]) using the base ExpressionBinder (not DECIDE-specific)
    // RAII guard ensures flag is reset even if BindChild throws
    is_top_expression = false;
    binding_when_condition = true;
    ErrorData condition_error;
    try {
        BindChild(func.children[1], depth, condition_error);
    } catch (...) {
        binding_when_condition = false;
        throw;
    }
    binding_when_condition = false;
    if (condition_error.HasError()) {
        return BindResult(std::move(condition_error));
    }

    // Construct bound result: tagged BoundConjunctionExpression
    // child[0] = bound constraint, child[1] = bound condition (cast to BOOLEAN)
    auto &bound_constraint = BoundExpression::GetExpression(*func.children[0]);
    auto &bound_condition = BoundExpression::GetExpression(*func.children[1]);

    auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
    result->children.push_back(std::move(bound_constraint));
    result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(bound_condition), LogicalType::BOOLEAN));
    result->alias = WHEN_CONSTRAINT_TAG;
    return BindResult(std::move(result));
}

BindResult DecideConstraintsBinder::BindPerConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &func = expr_ptr->Cast<FunctionExpression>();
    D_ASSERT(func.children.size() >= 2);

    auto &constraint_child = func.children[0];  // constraint (possibly WHEN-wrapped)

    // Validate each PER column (children[1..N])
    for (idx_t i = 1; i < func.children.size(); i++) {
        auto &column_child = func.children[i];

        // Validate: PER column must not reference a DECIDE variable
        if (ExpressionContainsDecideVariable(*column_child, variables)) {
            return BindResult(BinderException::Unsupported(*expr_ptr,
                "PER column cannot be a DECIDE variable. "
                "PER must group by a table column."));
        }

        // Validate: PER column must be a simple column reference
        if (column_child->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
            return BindResult(BinderException::Unsupported(*expr_ptr,
                "PER columns must be simple column references "
                "(e.g., PER empID or PER (empID, dept)). Expressions are not supported."));
        }
    }

    // Aggregate eligibility is deliberately deferred until the bound comparison has
    // been canonicalized. Parsed shape cannot distinguish `SUM(p) + x <= 10` (a
    // per-row decision beside a data-only reducer) from a homogeneous aggregate row.
    // DecideCanonicalizer validates the completed shape and owns the PER error.

    // Bind the constraint child through normal dispatch (handles WHEN recursively)
    is_top_expression = true;
    ErrorData constraint_error;
    BindChild(func.children[0], depth, constraint_error);
    if (constraint_error.HasError()) {
        return BindResult(std::move(constraint_error));
    }

    // Bind each PER column using the base ExpressionBinder
    // (reuse binding_when_condition flag to bypass DECIDE-specific dispatch)
    is_top_expression = false;
    binding_when_condition = true;
    for (idx_t i = 1; i < func.children.size(); i++) {
        ErrorData column_error;
        try {
            BindChild(func.children[i], depth, column_error);
        } catch (...) {
            binding_when_condition = false;
            throw;
        }
        if (column_error.HasError()) {
            binding_when_condition = false;
            return BindResult(std::move(column_error));
        }
    }
    binding_when_condition = false;

    // Construct tagged bound result:
    // child[0] = bound constraint (possibly WHEN-wrapped)
    // children[1..N] = bound PER columns (BoundColumnRefExpression)
    auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
    result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[0])));
    for (idx_t i = 1; i < func.children.size(); i++) {
        result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[i])));
    }
    result->alias = func.function_name;  // preserves PER_CONSTRAINT_TAG
    return BindResult(std::move(result));
}

BindResult DecideConstraintsBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
    auto result = BindExpressionInternal(expr_ptr, depth, root_expression);
    if (!result.HasError() && result.expression) {
        PreserveDecideSourceFragment(*expr_ptr, *result.expression);
    }
    return result;
}

BindResult DecideConstraintsBinder::BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                            bool root_expression) {
    if (binding_when_condition) {
        return ExpressionBinder::BindExpression(expr_ptr, depth);
    }
    if (depth > 0) {
        return ExpressionBinder::BindExpression(expr_ptr, depth);
    }
    auto &expr = *expr_ptr;
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        // A bare column at the top level of SUCH THAT is never a valid
        // constraint. Top-level commas are rejected by the parser, but keep
        // the PER hint here for malformed single-column inputs and older
        // prepared parse trees that surface a bare grouping key.
        return BindResult(BinderException::Unsupported(
            expr, StringUtil::Format(
                      "'%s' is not a valid SUCH THAT constraint on its own. "
                      "To group a constraint by multiple columns, parenthesize them: PER (col1, col2).",
                      expr.ToString())));
    }
    case ExpressionClass::CONSTANT: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        break;
    }
    case ExpressionClass::CAST: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        break;
    }
    case ExpressionClass::FUNCTION: {
        auto &func = expr.Cast<FunctionExpression>();
        // DecidB: PER constraint wrapper (outermost, wraps optional WHEN)
        if (func.is_operator && IsPerConstraintTag(func.function_name)) {
            return BindPerConstraint(expr_ptr, depth);
        }
        // DecidB: top-level WHEN wraps a whole constraint. Nested WHEN is the
        // aggregate-local form and binds through DecideBinder::BindFunction.
        if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
            if (!is_top_expression) {
                return BindFunction(expr_ptr, depth);
            }
            return BindWhenConstraint(expr_ptr, depth);
        }
        if (!is_top_expression) {
            return BindFunction(expr_ptr, depth);
        }
        break;
    }
    case ExpressionClass::COMPARISON:
        return BindComparison(expr_ptr, depth);
    case ExpressionClass::OPERATOR: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        return BindOperator(expr_ptr, depth);
    }
    case ExpressionClass::BETWEEN:
        return BindBetween(expr_ptr, depth);
    case ExpressionClass::CONJUNCTION:
        return BindConjunction(expr_ptr, depth);
    case ExpressionClass::SUBQUERY:
        return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
    default:
        break;
    }
    return BindResult(BinderException::Unsupported(
        expr, StringUtil::Format("SUCH THAT clause does not support '%s'(ExpressionClass::%s)", expr.ToString(),
                                EnumUtil::ToString(expr.GetExpressionClass()))));
}

DecideExpression DecideConstraintsBinder::GetExpressionType(ParsedExpression &expr_ptr, string& error_msg){
    // A relation qualifier changes which tuples a reducer sums over, not what shape it
    // is, so classification looks straight through it.
    auto &expr = const_cast<ParsedExpression &>(UnwrapQualifiedReducer(expr_ptr));
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF: {
        if (!IsVariableExpression(expr, variables)) {
            error_msg = StringUtil::Format("SUCH THAT clause: Column '%s' must be one of the DECIDE variables", expr.ToString());
            return DecideExpression::INVALID;
        }
        return DecideExpression::VARIABLE;
    }
    case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		auto fname = StringUtil::Lower(func.function_name);
		if (fname == "norm") {
            if (func.children.empty() || !ValidateSumArgument(*func.children.front(), variables, error_msg,
                                                               /*allow_quadratic=*/true, /*allow_bilinear=*/true)) {
                error_msg += ", found '" + expr.ToString() + "'";
                return DecideExpression::INVALID;
            }
            return DecideExpression::SUM;
        }
		if (fname == "sum" || fname == "avg" || fname == "min" || fname == "max") {
            auto scalar_name = FindScalarDecideVariable(*func.children.front());
            if (!scalar_name.empty()) {
                error_msg = StringUtil::Format(
                    "'%s' is a query-wide decision, so %s(%s) has nothing to aggregate over; "
                    "use %s on its own",
                    scalar_name, StringUtil::Upper(fname), scalar_name, scalar_name);
                return DecideExpression::INVALID;
            }
            if (!ValidateSumArgument(*func.children.front(), variables, error_msg, /*allow_quadratic=*/true, /*allow_bilinear=*/true)) {
                error_msg += ", found '" + expr.ToString() + "'";
                return DecideExpression::INVALID;
            }
            return DecideExpression::SUM;
		} else if (ContainsDecideAggregate(expr)) {
            return DecideExpression::SUM;
		} else if (ExpressionContainsDecideVariable(expr, variables)) {
            // Operator/function expressions containing DECIDE variables
            // are treated as per-row multi-variable constraints
            // (e.g., z_1 + z_2 + z_3 from IN rewrite, or d - x from ABS linearization)
            return DecideExpression::VARIABLE;
        } else {
            error_msg = StringUtil::Format("SUCH THAT clause does not support left-hand side function '%s', only SUM, AVG, MIN, or MAX is allowed.", func.function_name);
            return DecideExpression::INVALID;
        }
    }
    case ExpressionClass::CAST: {
        // A cast changes the value semantics of a DECIDE side, not its
        // row/aggregate shape. Keep it in the parsed and bound trees so the
        // exact-cast classifier or atomic preimage lowering can consume it.
        auto &cast = expr.Cast<CastExpression>();
        return GetExpressionType(*cast.child, error_msg);
    }
    default: {
        error_msg = StringUtil::Format("The left-hand side of a SUCH THAT constraint must be a DECIDE variable or a SUM expression over a DECIDE variable (e.g., SUM(x * a) / SUM(x)). Found '%s' instead.", expr.ToString());
    	return DecideExpression::INVALID;
    }
    }
}

} // namespace duckdb
