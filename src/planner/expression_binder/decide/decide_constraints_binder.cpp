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
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

#include <functional>

namespace duckdb {

DecideConstraintsBinder::DecideConstraintsBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                                                 const case_insensitive_set_t &scalar_variables,
                                                 optional_ptr<DecideQualifierContext> qualifier_context)
    : DecideBinder(binder, context, variables, scalar_variables, qualifier_context) {
}

static bool IsAllowedDecisionFreeBoundExpression(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables);

//! True when a CASE (or DuckDB's `if(...)`, which parses as one) sits anywhere in `expr`.
static bool ContainsCaseExpression(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::CASE) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		found = found || ContainsCaseExpression(child);
	});
	return found;
}

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
            // A plain data column, e.g. `stock` in `SUM(ship) <= stock PER depotID`.
            // `ExpressionContainsDecideVariable` (checked by every caller of this
            // function) already excludes an actual decision variable by name on this
            // same parsed tree, so a bare column reaching here is always data.
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
                    func.function_name == QUALIFIED_REDUCER_TAG || func.function_name == REDUCER_BY_TAG ||
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
                // The reducer's own WHEN is a predicate over known rows, not a value on
                // this side; it is bound and checked as a filter by BindAggregate.
                if (func.filter && ExpressionContainsDecideVariable(*func.filter, variables)) {
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
            // A reducer's own WHEN (`COUNT(WHEN c: cap)`) is a predicate over known rows,
            // exactly as for SUM/AVG/MIN/MAX above, not a value on this side.
            if (func.filter && ExpressionContainsDecideVariable(*func.filter, variables)) {
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
        const ParsedExpression *bad_bound = nullptr;
        if (!IsDecideSide(left_type) && !IsValidBound(*comp.left)) {
            bad_bound = comp.left.get();
        } else if (!IsDecideSide(right_type) && !IsValidBound(*comp.right)) {
            bad_bound = comp.right.get();
        }
        if (bad_bound) {
            if (ContainsCaseExpression(*bad_bound)) {
                return BindResult(BinderException::Unsupported(expr, DecideCaseUnsupportedMessage()));
            }
            if (ExpressionContainsDecideVariable(*bad_bound, variables)) {
                return BindResult(BinderException::Unsupported(
                    expr, "The bound of a reduced constraint reads no decision, and this one does. Move every "
                          "decision term to the reducer's side (for example SUM(x) - SUM(y) <= 5); a subquery "
                          "in a bound cannot read a decision."));
            }
            return BindResult(BinderException::Unsupported(
                expr, "The bound of a reduced constraint is one value per instance: a constant, a column, a data "
                      "reducer such as MIN(cap) BY (k) or COUNT(*) BY (k), or a scalar subquery. Compute anything "
                      "else in a CTE before the DECIDE clause."));
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
            if (op.children[i]->GetExpressionClass() == ExpressionClass::CONSTANT &&
                op.children[i]->Cast<ConstantExpression>().value.IsNull()) {
                return BindResult(BinderException::Unsupported(expr,
                    "an IN list holds a NULL, which no value equals; remove it from the list."));
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
    // Every clause of SUCH THAT holds; OR does not connect constraints, and reading it
    // as AND would impose both sides. A choice between cases is a BOOL decision with
    // IF guards, or a domain (`x IN (...)`).
    if (conj.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
        return BindResult(BinderException::Unsupported(
            expr, "OR does not connect constraints: every SUCH THAT clause holds. To allow either case, "
                  "guard each on a BOOL decision (IF pick: x <= 1 AND IF NOT pick: x >= 5), or list the "
                  "allowed values (x IN (0, 1, 5, 6))."));
    }
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

//! Binds a decision-free condition (a WHEN filter, a PER key column) through the base
//! binder, with the DECIDE dispatch switched off for its duration.
BindResult DecideConstraintsBinder::BindKnownCondition(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	is_top_expression = false;
	binding_when_condition = true;
	ErrorData error;
	try {
		BindChild(expr_ptr, depth, error);
	} catch (...) {
		binding_when_condition = false;
		throw;
	}
	binding_when_condition = false;
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	return BindResult(std::move(BoundExpression::GetExpression(*expr_ptr)));
}

BindResult DecideConstraintsBinder::BindWhenConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &func = expr_ptr->Cast<FunctionExpression>();
	D_ASSERT(func.children.size() == 2);

	// WHEN filters known data before generation, so its condition reads no decision.
	if (ExpressionContainsDecideVariable(*func.children[1], variables)) {
		return BindResult(BinderException::Unsupported(*expr_ptr,
		    "A WHEN condition filters rows before the solve, so it cannot reference a decision; "
		    "to impose the constraint only when a decision holds, write IF <condition>: instead."));
	}
	// It reads each row's own known data; a frame reads another row's.
	if (ParsedExpressionContainsFrame(*func.children[1])) {
		return BindResult(BinderException::Unsupported(*expr_ptr,
		    "A WHEN condition reads a row's own known data; a frame (AT / FROM .. TO .. OVER) reads another "
		    "row's and cannot filter. Compare the frame in the constraint body instead."));
	}

	// Bind the constraint (child[0]) through normal DECIDE constraint dispatch
	is_top_expression = true;
	ErrorData constraint_error;
	BindChild(func.children[0], depth, constraint_error);
	if (constraint_error.HasError()) {
		return BindResult(std::move(constraint_error));
	}
	auto condition = BindKnownCondition(func.children[1], depth);
	if (condition.HasError()) {
		return condition;
	}

	// child[0] = bound constraint, child[1] = bound condition (cast to BOOLEAN)
	auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[0])));
	result->children.push_back(
	    BoundCastExpression::AddCastToType(context, std::move(condition.expression), LogicalType::BOOLEAN));
	result->alias = WHEN_CONSTRAINT_TAG;
	return BindResult(std::move(result));
}

BindResult DecideConstraintsBinder::BindIfConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &func = expr_ptr->Cast<FunctionExpression>();
	D_ASSERT(func.children.size() == 2);

	// IF guards the instance on a decision: a guard that reads none is a WHEN filter.
	if (!ExpressionContainsDecideVariable(*func.children[1], variables)) {
		return BindResult(BinderException::Unsupported(*expr_ptr,
		    "An IF guard must reference a decision; a condition over known data filters rows and is "
		    "written WHEN <condition>: instead."));
	}

	// A guarded row is stated as an implication over the row's own terms, which the
	// lowerings of ABS, products, squares and norms (auxiliaries with structural rows
	// of their own) do not carry. Say so before binding rather than after a Big-M
	// refusal names an auxiliary the user never wrote.
	{
		string offender;
		std::function<void(const ParsedExpression &)> scan = [&](const ParsedExpression &node) {
			if (!offender.empty()) {
				return;
			}
			if (node.GetExpressionClass() == ExpressionClass::FUNCTION) {
				auto &fn = node.Cast<FunctionExpression>();
				string lower = StringUtil::Lower(fn.function_name);
				if ((lower == "abs" || lower == "power" || lower == "pow" || lower == "norm" || lower == "**") &&
				    ExpressionContainsDecideVariable(node, variables)) {
					offender = StringUtil::Upper(lower) + "(...)";
					return;
				}
				if (lower == "*" && fn.children.size() == 2 &&
				    ExpressionContainsDecideVariable(*fn.children[0], variables) &&
				    ExpressionContainsDecideVariable(*fn.children[1], variables)) {
					offender = "a product of decisions";
					return;
				}
			}
			ParsedExpressionIterator::EnumerateChildren(node, [&](const ParsedExpression &child) { scan(child); });
		};
		scan(*func.children[0]);
		if (!offender.empty()) {
			return BindResult(BinderException::Unsupported(
			    *expr_ptr, StringUtil::Format("an IF guard is supported on linear constraints only; %s cannot be "
			                                  "guarded yet. State the guarded fact through a BOOL decision instead.",
			                                  offender)));
		}
	}

	is_top_expression = true;
	ErrorData constraint_error;
	BindChild(func.children[0], depth, constraint_error);
	if (constraint_error.HasError()) {
		return BindResult(std::move(constraint_error));
	}
	// `IF a AND b` over BOOL decisions is the linear guard `a + b >= 2`, and `IF a OR b`
	// is `a + b >= 1` (`NOT b` contributing `1 - b`). Rewritten here so the guard binds
	// as the comparison it is; a mix of AND and OR, or a comparison inside the
	// conjunction, has no single row and is refused by name.
	if (func.children[1]->GetExpressionClass() == ExpressionClass::CONJUNCTION) {
		auto &conjunction = func.children[1]->Cast<ConjunctionExpression>();
		vector<unique_ptr<ParsedExpression>> literals;
		bool simple = true;
		for (auto &child : conjunction.children) {
			auto *leaf = child.get();
			bool negated = false;
			if (leaf->GetExpressionClass() == ExpressionClass::OPERATOR &&
			    leaf->Cast<OperatorExpression>().type == ExpressionType::OPERATOR_NOT &&
			    leaf->Cast<OperatorExpression>().children.size() == 1) {
				negated = true;
				leaf = leaf->Cast<OperatorExpression>().children[0].get();
			}
			if (leaf->GetExpressionClass() != ExpressionClass::COLUMN_REF || !IsVariableExpression(*leaf, variables)) {
				// A leaf over known data belongs in the filter, not the guard.
				if (!ExpressionContainsDecideVariable(*child, variables)) {
					return BindResult(BinderException::Unsupported(
					    *expr_ptr, StringUtil::Format("'%s' reads known data, so it filters rows rather than guarding "
					                                  "the instance: write WHEN %s IF <decisions>: ... instead.",
					                                  child->ToString(), child->ToString())));
				}
				simple = false;
				break;
			}
			if (!negated) {
				literals.push_back(leaf->Copy());
				continue;
			}
			vector<unique_ptr<ParsedExpression>> minus;
			minus.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
			minus.push_back(leaf->Copy());
			auto complement = make_uniq<FunctionExpression>("-", std::move(minus));
			complement->is_operator = true;
			literals.push_back(std::move(complement));
		}
		if (!simple) {
			return BindResult(BinderException::Unsupported(
			    *expr_ptr, "An IF guard combines BOOL decisions with AND or OR (IF open AND NOT closed:); a "
			               "comparison inside the combination is not supported yet -- state it through a BOOL "
			               "decision, or guard a separate constraint with it."));
		}
		unique_ptr<ParsedExpression> sum = std::move(literals[0]);
		for (idx_t i = 1; i < literals.size(); i++) {
			vector<unique_ptr<ParsedExpression>> plus;
			plus.push_back(std::move(sum));
			plus.push_back(std::move(literals[i]));
			auto added = make_uniq<FunctionExpression>("+", std::move(plus));
			added->is_operator = true;
			sum = std::move(added);
		}
		const bool all = conjunction.type == ExpressionType::CONJUNCTION_AND;
		auto threshold = make_uniq<ConstantExpression>(Value::INTEGER(all ? NumericCast<int32_t>(literals.size()) : 1));
		func.children[1] = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(sum),
		                                                   std::move(threshold));
	}
	// The guard is a decision-bearing boolean. A comparison binds through the
	// constraint dispatch, so it is validated like the comparison it is; a bare BOOL
	// decision, or NOT of one, binds as the expression it is.
	auto &guard = *func.children[1];
	const bool guard_is_comparison = guard.GetExpressionClass() == ExpressionClass::COMPARISON ||
	                                 guard.GetExpressionClass() == ExpressionClass::BETWEEN;
	is_top_expression = guard_is_comparison;
	ErrorData guard_error;
	BindChild(func.children[1], depth, guard_error);
	if (guard_error.HasError()) {
		return BindResult(std::move(guard_error));
	}
	// Which shapes a guard may take (a BOOL decision, NOT of one, a comparison) is
	// settled where it is lowered: stage 05 reads the bound guard and rejects the rest.
	auto &bound_guard = BoundExpression::GetExpression(*func.children[1]);

	auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[0])));
	result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(bound_guard), LogicalType::BOOLEAN));
	result->alias = IF_CONSTRAINT_TAG;
	return BindResult(std::move(result));
}

BindResult DecideConstraintsBinder::BindPerConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &func = expr_ptr->Cast<FunctionExpression>();
	D_ASSERT(!func.children.empty());

	// Resolve the generation key once, here, into a shared entity scope. A key with no
	// elements is `PER ()`: one instance for the whole query.
	string gen_tag;
	if (func.children.size() == 1) {
		gen_tag = GEN_GLOBAL_TAG;
	} else {
		if (!qualifier_context) {
			return BindResult(BinderException::Unsupported(*expr_ptr, "PER is only allowed inside a DECIDE clause."));
		}
		vector<unique_ptr<ParsedExpression>> key;
		for (idx_t i = 1; i < func.children.size(); i++) {
			// A key partitions known rows, so it names columns or relations; a decision
			// has no value before the solve and cannot be generated over.
			if (ExpressionContainsDecideVariable(*func.children[i], variables)) {
				return BindResult(BinderException::Unsupported(
				    *expr_ptr, StringUtil::Format("PER key: '%s' is a decision; a key generates one instance per "
				                                  "value of known data, so it names columns or relations",
				                                  func.children[i]->ToString())));
			}
			key.push_back(func.children[i]->Copy());
		}
		string error;
		auto scope_idx = FindOrCreateKeyScope(binder.bind_context, key, *qualifier_context->entity_scopes,
		                                      *qualifier_context->table_scope_map, error);
		if (scope_idx == DConstants::INVALID_INDEX) {
			return BindResult(BinderException::Unsupported(*expr_ptr, "PER key: " + error));
		}
		gen_tag = MakeGenScopeTag(scope_idx);
	}

	// Whether the generated instances are well defined -- every value they read is a
	// function of the key -- is proved on the complete bound tree by
	// ValidateDecideGenerationTree, after the whole clause is bound.
	auto result = BindPerWrapper(func, depth);
	if (result.HasError()) {
		return result;
	}
	auto &wrapper = result.expression->Cast<BoundConjunctionExpression>();
	AddDecideTag(wrapper.alias, gen_tag);
	return result;
}

BindResult DecideConstraintsBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
    auto location = expr_ptr->GetQueryLocation();
    auto result = BindExpressionInternal(expr_ptr, depth, root_expression);
    if (!result.HasError() && result.expression) {
        PreserveDecideSourceFragment(*expr_ptr, *result.expression);
    }
    return PreserveQueryLocation(location, std::move(result));
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
                      "'%s' is not a valid SUCH THAT constraint on its own.",
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
        // DecidB: the WHEN and IF prefixes wrap a whole constraint.
        if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
            return BindWhenConstraint(expr_ptr, depth);
        }
        if (func.is_operator && func.function_name == IF_CONSTRAINT_TAG) {
            return BindIfConstraint(expr_ptr, depth);
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
    case ExpressionClass::CASE:
        // Same answer as a CASE inside a reducer, which ValidateSumArgument rejects
        // with this text: without this branch the generic message below leaks
        // `ExpressionClass::CASE`, which is not something a SQL user can act on.
        return BindResult(BinderException::Unsupported(expr, DecideCaseUnsupportedMessage()));
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
        } else if (ParsedExpressionContainsFrame(expr)) {
            error_msg = "a frame (AT / FROM .. TO .. OVER) reads known data at another row; on its own it decides "
                        "nothing. Compare a decision with it, e.g. x <= AT(PREVIOUS: cap) OVER (t).";
            return DecideExpression::INVALID;
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
        error_msg = StringUtil::Format("a constraint reads a decision; '%s' reads none. Compare a decision or a reducer over "
                                       "decisions (x <= cap, SUM(x) <= 10) with a bound.",
                                       expr.ToString());
    	return DecideExpression::INVALID;
    }
    }
}

} // namespace duckdb
