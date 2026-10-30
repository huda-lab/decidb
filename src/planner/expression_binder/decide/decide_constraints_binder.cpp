#include "duckdb/planner/expression_binder/decide/decide_constraints_binder.hpp"
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
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/main/client_context.hpp"

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
        case ExpressionClass::COLUMN_REF:
            // A plain data column, e.g. `stock` in `per depotID: sum(ship) by (depotID) <= stock`.
            // `ExpressionContainsDecideVariable` (checked by every caller of this
            // function) already excludes an actual decision variable by name on this
            // same parsed tree, so a bare column reaching here is always data.
            return true;
        case ExpressionClass::FUNCTION: {
            auto &func = expr.Cast<FunctionExpression>();
            if (func.is_operator) {
                // A qualifier or `by` marker's children past the first are relation
                // names or a key -- not values on this side -- so validating them as
                // bounds is a category error. Same rule K0 already states for the
                // canonicalizer: recurse into child 0 only.
                if (func.function_name == QUALIFIED_REDUCER_TAG || func.function_name == REDUCER_BY_TAG) {
                    return !func.children.empty() && IsAllowedDecisionFreeBoundExpression(*func.children[0], variables);
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
        case ExpressionClass::PARAMETER:
            // A prepared statement's `$1`: one value, supplied at EXECUTE.
            return true;
        case ExpressionClass::COMPARISON:
        case ExpressionClass::CONJUNCTION:
        case ExpressionClass::BETWEEN:
        case ExpressionClass::LAMBDA: {
            // Data as long as every part is: `sum(x) <= CAST(cap > 2 AS INT)`.
            bool allowed = true;
            ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
                allowed = allowed && IsAllowedDecisionFreeBoundExpression(child, variables);
            });
            return allowed;
        }
        default:
            return false;
    }
}

//! The refusal for a constraint shape DECIDE cannot read, quoting the part as written.
static BindResult UnsupportedConstraint(ParsedExpression &expr) {
	return BindResult(
	    BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT does not support '%s'", expr.ToString())));
}

BindResult DecideConstraintsBinder::BindComparison(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &comp = expr.Cast<ComparisonExpression>();

    if (!IsSupportedComparison(comp.type)) {
		return UnsupportedConstraint(expr);
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
		return UnsupportedConstraint(expr);
	}
}

//! `input BETWEEN lower AND upper` as the conjunction `input >= lower AND input <= upper`. Both halves take the
//! BETWEEN's location, where the parser kept the text of the clause, and a tag saying which half they are.
static unique_ptr<ConjunctionExpression> ExpandBetween(const BetweenExpression &between) {
	auto lower_bound = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	                                                   between.input->Copy(), between.lower->Copy());
	auto upper_bound = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, between.input->Copy(),
	                                                   between.upper->Copy());
	lower_bound->query_location = between.query_location;
	upper_bound->query_location = between.query_location;
	AddDecideTag(lower_bound->alias, BETWEEN_LOWER_TAG);
	AddDecideTag(upper_bound->alias, BETWEEN_UPPER_TAG);
	return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower_bound),
	                                        std::move(upper_bound));
}

BindResult DecideConstraintsBinder::BindBetween(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	expr_ptr = ExpandBetween(expr_ptr->Cast<BetweenExpression>());
	return BindConjunction(expr_ptr, depth);
}

BindResult DecideConstraintsBinder::BindConjunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &conj = expr.Cast<ConjunctionExpression>();
	// Constraints are joined with AND. An OR between them would need an indicator per
	// alternative, which no layer builds, and every later walk reads a conjunction as AND.
	if (conj.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
		return BindResult(BinderException::Unsupported(expr, "SUCH THAT: OR is not supported."));
	}
	// Only a constraint list reaches here (an operand conjunction is data or refused), so
	// each child is a constraint.
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

BindResult DecideConstraintsBinder::BindScopedConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	// `[when c] [per K]: body`: the per marker is the outer one, the when marker the
	// inner one. The markers stay where they are; their children bind in place. `per row`,
	// the default written out, adds no class key: one constraint per kept row.
	optional_ptr<FunctionExpression> per;
	optional_ptr<FunctionExpression> when;
	auto *body = &expr_ptr;
	if (IsDecideMarker(**body, SCOPE_PER_ROW_TAG)) {
		body = &(*body)->Cast<FunctionExpression>().children[0];
	} else if (IsDecideMarker(**body, SCOPE_PER_TAG)) {
		per = &(*body)->Cast<FunctionExpression>();
		body = &per->children[0];
	}
	if (IsDecideMarker(**body, SCOPE_WHEN_TAG)) {
		when = &(*body)->Cast<FunctionExpression>();
		body = &when->children[0];
	}

	ErrorData body_error;
	is_top_expression = true;
	BindChild(*body, depth, body_error);
	if (body_error.HasError()) {
		return BindResult(std::move(body_error));
	}

	// The condition takes data only (ValidateDecideScopeSpellings): plain SQL, no
	// parentheses needed around a compound one.
	auto bind_condition = [&](unique_ptr<ParsedExpression> &parsed, unique_ptr<Expression> &bound) {
		ErrorData error;
		is_top_expression = false;
		binding_when_condition = true;
		try {
			BindChild(parsed, depth, error);
		} catch (...) {
			binding_when_condition = false;
			throw;
		}
		binding_when_condition = false;
		if (!error.HasError()) {
			bound = BoundCastExpression::AddCastToType(context, std::move(BoundExpression::GetExpression(*parsed)),
			                                           LogicalType::BOOLEAN);
		}
		return error;
	};
	// A bound subquery cannot be copied, so a second constraint of the scope (a BETWEEN's
	// other half) binds the condition afresh from it as written.
	unique_ptr<ParsedExpression> condition_text;
	unique_ptr<Expression> condition;
	idx_t when_text = when && qualifier_context ? qualifier_context->WrittenFragment(*when, DecideSpanRole::WHEN)
	                                            : DConstants::INVALID_INDEX;
	if (when) {
		condition_text = when->children[1]->Copy();
		auto error = bind_condition(when->children[1], condition);
		if (error.HasError()) {
			return BindResult(std::move(error));
		}
	}
	idx_t key = DConstants::INVALID_INDEX;
	idx_t key_fragment = DConstants::INVALID_INDEX;
	if (per) {
		if (!qualifier_context || !qualifier_context->keys) {
			throw InternalException("DECIDE: a per wrapper needs the clause's keys");
		}
		// The key as written (`per D`), read before its elements bind. It cannot ride on
		// the marker's alias: the expression binder gives the bound wrapper that alias.
		auto key_text = DecideKeyToString(*per);
		if (qualifier_context->source_fragments) {
			// the key as the user wrote it, when the parser kept it
			key_fragment = qualifier_context->WrittenFragment(*per, DecideSpanRole::PER);
			if (key_fragment == DConstants::INVALID_INDEX) {
				key_fragment = qualifier_context->source_fragments->size();
				qualifier_context->source_fragments->push_back(key_text.empty() ? string("()") : key_text);
			}
		}
		case_insensitive_set_t decision_names;
		for (auto &variable : variables) {
			decision_names.insert(variable.first);
		}
		key = BindKey(context, binder.bind_context, *per, decision_names, key_text, *qualifier_context->keys);
	}

	// One scope per constraint: a BETWEEN bound to the conjunction of its two halves, and
	// each half gets the whole scope.
	auto scope = [&](unique_ptr<Expression> constraint) -> unique_ptr<Expression> {
		if (when) {
			unique_ptr<Expression> this_condition = std::move(condition);
			if (!this_condition) {
				auto parsed = condition_text->Copy();
				auto error = bind_condition(parsed, this_condition);
				if (error.HasError()) {
					error.Throw();
				}
			}
			auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			wrapper->children.push_back(std::move(constraint));
			wrapper->children.push_back(std::move(this_condition));
			wrapper->alias = WHEN_FILTER_TAG;
			if (when_text != DConstants::INVALID_INDEX) {
				AddDecideTag(wrapper->alias, MakeSourceFragmentTag(when_text));
			}
			constraint = std::move(wrapper);
		}
		if (per) {
			auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			wrapper->children.push_back(std::move(constraint));
			wrapper->alias = PER_CLASS_TAG;
			if (key != DConstants::INVALID_INDEX) {
				AddDecideTag(wrapper->alias, MakePerKeyTag(key));
			}
			if (key_fragment != DConstants::INVALID_INDEX) {
				AddDecideTag(wrapper->alias, MakeSourceFragmentTag(key_fragment));
			}
			constraint = std::move(wrapper);
		}
		return constraint;
	};
	auto bound = std::move(BoundExpression::GetExpression(**body));
	if (bound->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION && bound->alias.empty()) {
		auto &halves = bound->Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		for (auto &half : halves.children) {
			result->children.push_back(scope(std::move(half)));
		}
		return BindResult(std::move(result));
	}
	return BindResult(scope(std::move(bound)));
}

BindResult DecideConstraintsBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
    auto location = expr_ptr->GetQueryLocation();
    auto result = TagWrittenColumn(location, BindExpressionInternal(expr_ptr, depth, root_expression));
    if (!result.HasError() && result.expression) {
        PreserveDecideSourceFragment(*expr_ptr, *result.expression);
    }
    return ThrowClauseError(PreserveQueryLocation(location, std::move(result)), root_expression);
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
	// Below a constraint's top, an operand that reads no decision is data, whatever its
	// shape: `CAST(cap > 2 AS INT)`, a parameter `$1`, a lambda and its parameters.
	if (!is_top_expression && IsDataOperand(expr)) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	// A comparison over a decision below the top, as in `x <= (x > 1)`, is neither a
	// constraint nor data.
	if (!is_top_expression && (expr.GetExpressionClass() == ExpressionClass::COMPARISON ||
	                           expr.GetExpressionClass() == ExpressionClass::BETWEEN ||
	                           expr.GetExpressionClass() == ExpressionClass::CONJUNCTION)) {
		return UnsupportedConstraint(expr);
	}
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
		// A bare column at the top level of SUCH THAT is never a valid constraint.
		return BindResult(BinderException::Unsupported(
		    expr, StringUtil::Format("'%s' is not a valid SUCH THAT constraint on its own; compare it with a value",
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
		// `[when c] [per K]:` in front of a constraint.
		if (func.is_operator && (func.function_name == SCOPE_PER_TAG || func.function_name == SCOPE_WHEN_TAG ||
		                         func.function_name == SCOPE_PER_ROW_TAG)) {
			return BindScopedConstraint(expr_ptr, depth);
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
	return UnsupportedConstraint(expr);
}

DecideExpression DecideConstraintsBinder::GetExpressionType(ParsedExpression &expr_ptr, string& error_msg){
    // A relation qualifier changes which tuples a reducer sums over, not what shape it
    // is, so classification looks straight through it.
    auto &expr = const_cast<ParsedExpression &>(UnwrapReducerMarkers(expr_ptr));
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
		DecideExpression reducer_result;
		if (ClassifyReducerCall(func, reducer_result, error_msg)) {
			return reducer_result;
		}
		if (ContainsDecideAggregate(expr)) {
            return DecideExpression::SUM;
		} else if (ExpressionContainsDecideVariable(expr, variables)) {
            // Operator/function expressions containing DECIDE variables
            // are treated as per-row multi-variable constraints
            // (e.g., z_1 + z_2 + z_3 from IN rewrite, or d - x from ABS linearization)
            return DecideExpression::VARIABLE;
        } else {
			error_msg = StringUtil::Format("SUCH THAT clause does not support left-hand side function '%s', only SUM, "
			                               "AVG, MIN, or MAX is allowed.",
			                               DecideWrittenFunctionName(func));
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
