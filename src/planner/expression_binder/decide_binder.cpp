#include "duckdb/planner/expression_binder/decide_binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include <unordered_set>

#include "duckdb/decidb/utility/debug.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

bool IsScalarValue(ParsedExpression &expr) {
    if (expr.GetExpressionClass() == ExpressionClass::CONSTANT) {
        auto &constant_expr = expr.Cast<ConstantExpression>();
        switch (constant_expr.value.type().id()) {
            case LogicalTypeId::LIST:
            case LogicalTypeId::STRUCT:
            case LogicalTypeId::MAP:
            case LogicalTypeId::ARRAY:
            case LogicalTypeId::UNION:
                return false;
            default:
                return true;
        }
    }
    if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
        auto &subquery_expr = expr.Cast<SubqueryExpression>();
        if (subquery_expr.subquery_type == SubqueryType::SCALAR) {
            return true;
        }
    }
    return false;
}

bool IsVariableExpression(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	const auto &colref = expr.Cast<const ColumnRefExpression>();
	if (colref.IsQualified()) {
		string qualified = colref.GetTableName() + "." + colref.GetColumnName();
		return variables.count(qualified) > 0;
	}
	return variables.count(colref.GetColumnName()) > 0;
}

static idx_t CountDecideVariableOccurrencesInternal(const ParsedExpression &expr,
                                                    const case_insensitive_map_t<idx_t> &variables) {
	idx_t count = 0;
	if (IsVariableExpression(expr, variables)) {
		count++;
	}
	// Descend into subquery QueryNode bodies (SELECT, WHERE, HAVING, etc.)
	// EnumerateChildren only visits SubqueryExpression.child, not the query body.
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = expr.Cast<const SubqueryExpression>();
		if (subquery_expr.subquery && subquery_expr.subquery->node) {
			// const_cast: EnumerateQueryNodeChildren requires non-const but we only read
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    const_cast<QueryNode &>(*subquery_expr.subquery->node),
			    [&](unique_ptr<ParsedExpression> &child) {
				    count += CountDecideVariableOccurrencesInternal(*child, variables);
			    },
			    [&](TableRef &ref) {});
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		count += CountDecideVariableOccurrencesInternal(child, variables);
	});
	return count;
}

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	return CountDecideVariableOccurrencesInternal(expr, variables) > 0;
}

bool IsDecideAggregateName(const string &name) {
	auto lname = StringUtil::Lower(name);
	return lname == "sum" || lname == "avg" || lname == "min" || lname == "max";
}

bool ContainsDecideAggregate(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (!func.is_operator && IsDecideAggregateName(func.function_name)) {
			return true;
		}
		if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG && !func.children.empty()) {
			return ContainsDecideAggregate(*func.children[0]);
		}
		for (auto &child : func.children) {
			if (ContainsDecideAggregate(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto &op = expr.Cast<const OperatorExpression>();
		for (auto &child : op.children) {
			if (ContainsDecideAggregate(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<const CastExpression>();
		return ContainsDecideAggregate(*cast.child);
	}
	return false;
}

bool ContainsWhenOperator(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
			return true;
		}
		for (auto &child : func.children) {
			if (ContainsWhenOperator(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto &op = expr.Cast<const OperatorExpression>();
		for (auto &child : op.children) {
			if (ContainsWhenOperator(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<const CastExpression>();
		return ContainsWhenOperator(*cast.child);
	}
	if (expr.GetExpressionClass() == ExpressionClass::COMPARISON) {
		auto &cmp = expr.Cast<const ComparisonExpression>();
		return ContainsWhenOperator(*cmp.left) || ContainsWhenOperator(*cmp.right);
	}
	return false;
}

// Names that can legally wrap a DECIDE variable at bind time. ABS is
// linearized by the optimizer; POWER/POW(expr, 2) feeds the QP objective path;
// SUM/AVG/MIN/MAX/COUNT are aggregates handled in BindAggregate; +, -, *, /,
// **, and unary tags fall through to the normal linear-extraction path.
// Including aggregate names prevents false-flagging the IN-rewrite's "+"
// nodes (synthesized as non-operator FunctionExpression) or nested aggregates.
static bool IsAllowedNameOverDecideVar(const string &name) {
	auto lname = StringUtil::Lower(name);
	if (lname == "abs" || lname == "power" || lname == "pow") return true;
	if (lname == "sum" || lname == "avg" || lname == "min" || lname == "max") return true;
	if (lname == "count" || lname == "count_star") return true;
	if (lname == "+" || lname == "-" || lname == "*" || lname == "/" || lname == "**") return true;
	// DecidB-internal tag operators (__when_constraint__, __per_constraint__, ...)
	// are synthesized as FunctionExpression and handled by dedicated binders.
	if (name.size() >= 4 && name.substr(0, 2) == "__" && name.substr(name.size() - 2) == "__") return true;
	return false;
}

// True when the function name resolves to an AGGREGATE in the catalog. These
// are rejected (with an aggregate-specific error) in BindAggregate, so we must
// not also false-flag them here as "non-linear scalars".
static bool IsAggregateFunctionName(ClientContext &context, const FunctionExpression &func) {
	auto entry = Catalog::GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY,
	                                func.catalog, func.schema, func.function_name,
	                                OnEntryNotFound::RETURN_NULL);
	return entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY;
}

// Reject any non-linear scalar function that wraps a DECIDE variable. Mirrors
// the COUNT(decide_var) guard in BindAggregate: catch the mis-use at bind time
// with a semantic-specific message instead of letting it slip through to
// symbolic normalization (which throws InternalException on unknown functions)
// or per-row execution (which would silently strip the scalar, producing
// wrong answers). Functions that only wrap table columns are fine — the
// wrapper folds to a per-row constant before the solver sees it.
// True if this function is POWER / POW / **. These share the same validation:
// exponent must be a constant numeric 2. All other exponents are non-linear
// (fractional → radicals, negative → reciprocal, variable → exponential),
// or unsupported higher-degree integer powers (3+ → cubic and above).
static bool IsPowerFunction(const FunctionExpression &func) {
	auto lname = StringUtil::Lower(func.function_name);
	return lname == "power" || lname == "pow" || lname == "**";
}

// Reject POWER(base, exp) when base contains a decide variable and exp is not
// a constant numeric 2. Mirrors ValidateQuadraticPower's error messages so
// test matchers and user-facing wording are consistent across SUM and non-SUM
// contexts. Must run before symbolic normalization — FromSymbolic would
// otherwise throw InternalException on non-integer exponents, exposing a C++
// stack trace to users.
static void ValidatePowerExponent(const FunctionExpression &func,
                                  const case_insensitive_map_t<idx_t> &variables) {
	if (func.children.size() != 2) {
		return; // Arity errors are surfaced elsewhere with clearer messages.
	}
	// Only gate POWER whose base transitively references a DECIDE variable.
	// POWER over pure data columns folds to a per-row constant and is fine.
	if (!ExpressionContainsDecideVariable(*func.children[0], variables)) {
		return;
	}
	auto &exponent = *func.children[1];
	if (exponent.GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw BinderException(
		    "POWER exponent in DECIDE expression must be a constant integer "
		    "(only 2 is supported)");
	}
	auto &exp_const = exponent.Cast<const ConstantExpression>();
	double exp_val;
	try {
		exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	} catch (...) {
		throw BinderException("POWER exponent must be numeric");
	}
	if (exp_val != 2.0) {
		throw BinderException(
		    "Only POWER(expr, 2) is supported for quadratic expressions. "
		    "Found exponent %g. Higher powers are not allowed.", exp_val);
	}
}

// True if `expr` contains an aggregate over table columns only (no DECIDE
// variable anywhere in its argument) — e.g. `AVG(price)`, `MIN(cost)`. Such a
// value needs the whole row set, so it is a scalar the query must pre-compute,
// not a per-row coefficient. Recurses so nested forms (`AVG(p) * 2`) are caught.
static bool ContainsDataOnlyAggregate(ClientContext &context, const ParsedExpression &expr,
                                      const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsAggregateFunctionName(context, func) &&
		    !ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (ContainsDataOnlyAggregate(context, child, variables)) {
			found = true;
		}
	});
	return found;
}

void ValidateDecideNoNonLinearScalar(ClientContext &context,
                                     const ParsedExpression &expr,
                                     const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsPowerFunction(func)) {
			ValidatePowerExponent(func, variables);
		} else if (func.is_operator && func.function_name == "*" &&
		           ExpressionContainsDecideVariable(expr, variables) &&
		           ContainsDataOnlyAggregate(context, expr, variables)) {
			// A data-only aggregate multiplied by a decision variable
			// (`SUM(avg(p) * x)`) is not a per-row coefficient: the aggregate
			// spans all rows. Symbolic normalization would silently distribute the
			// variable into it (`avg(p)*x` -> `avg(p*x)`, a different constraint) and
			// return a wrong optimum, so reject it here — before normalization —
			// consistent with the direct `SUM(sum(p) * x)` rejection.
			throw BinderException(
			    "An aggregate over table columns (e.g. AVG(col), MIN(col)) cannot multiply a "
			    "decision variable. Pre-compute it as a scalar in a subquery or CTE and reference "
			    "that value.");
		} else if (func.is_operator && func.function_name == "/") {
			// Division is only linear when the divisor contains no decide
			// variable. x / y (decide vars in divisor) is non-linear; catch
			// it here so it doesn't fall through to per-row extraction
			// (which would silently produce wrong results) or symbolic
			// normalization (which would throw InternalException).
			if (func.children.size() == 2 &&
			    ExpressionContainsDecideVariable(*func.children[1], variables)) {
				throw BinderException(
				    "Division by a DECIDE variable is not supported: "
				    "it would make the model non-linear. The divisor must "
				    "not reference a decision variable.");
			}
		} else if (!IsAllowedNameOverDecideVar(func.function_name) &&
		           !IsAggregateFunctionName(context, func)) {
			for (auto &child : func.children) {
				if (ExpressionContainsDecideVariable(*child, variables)) {
					throw BinderException(
					    "Scalar function '%s' over a DECIDE variable is not supported: "
					    "it would make the model non-linear. Only ABS() and POWER(..., 2) "
					    "can wrap a decision variable.",
					    func.function_name);
				}
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		ValidateDecideNoNonLinearScalar(context, child, variables);
	});
}

//! Collect the set of DECIDE variable indices referenced by an expression.
static void CollectDecideVariableIndices(const ParsedExpression &expr,
                                         const case_insensitive_map_t<idx_t> &variables,
                                         unordered_set<idx_t> &out) {
	if (IsVariableExpression(expr, variables)) {
		const auto &colref = expr.Cast<const ColumnRefExpression>();
		string key = colref.IsQualified()
		    ? (colref.GetTableName() + "." + colref.GetColumnName())
		    : colref.GetColumnName();
		auto it = variables.find(key);
		if (it != variables.end()) {
			out.insert(it->second);
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		CollectDecideVariableIndices(child, variables, out);
	});
}

// Forward declaration — needed because ValidateQuadraticPower calls ValidateSumArgumentInternal.
static bool ValidateSumArgumentInternal(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                        bool &has_decide_variable, string &error_msg, bool allow_quadratic = false,
                                        bool allow_bilinear = false);

// Shared validation for POWER(expr, 2) and expr ** 2 patterns.
// Returns true if the pattern is a valid quadratic form; sets has_decide_variable.
// The base expression is validated as strictly linear (allow_quadratic=false) to prevent nesting.
static bool ValidateQuadraticPower(vector<unique_ptr<ParsedExpression>> &children,
                                   const case_insensitive_map_t<idx_t> &variables,
                                   bool &has_decide_variable, string &error_msg, const string &label) {
	if (children.size() != 2) {
		error_msg = label + " requires exactly two arguments";
		return false;
	}
	auto &exponent = *children[1];
	if (exponent.GetExpressionClass() != ExpressionClass::CONSTANT) {
		error_msg = "POWER exponent in DECIDE expression must be a constant integer (only 2 is supported)";
		return false;
	}
	auto &exp_const = exponent.Cast<ConstantExpression>();
	double exp_val;
	try {
		exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	} catch (...) {
		error_msg = "POWER exponent must be numeric";
		return false;
	}
	if (exp_val != 2.0) {
		error_msg = StringUtil::Format(
		    "Only POWER(expr, 2) is supported for quadratic expressions. "
		    "Found exponent %g. Higher powers are not allowed.", exp_val);
		return false;
	}
	// Validate the base is a LINEAR expression (allow_quadratic=false prevents nesting like POWER(POWER(x,2),2))
	bool base_has_var = false;
	if (!ValidateSumArgumentInternal(*children[0], variables, base_has_var, error_msg, /*allow_quadratic=*/false)) {
		error_msg = "Inside " + label + ": " + error_msg;
		return false;
	}
	if (!base_has_var) {
		error_msg = label + " in DECIDE expression must reference at least one DECIDE variable";
		return false;
	}
	has_decide_variable = true;
	return true;
}

static bool ValidateSumArgumentInternal(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                        bool &has_decide_variable, string &error_msg,
                                        bool allow_quadratic, bool allow_bilinear) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
        if (IsVariableExpression(expr, variables)) {
            has_decide_variable = true;
        }
        return true;
    }
    case ExpressionClass::CONSTANT:
        return true;
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		string func_name_lower = StringUtil::Lower(func.function_name);
		if (!func.is_operator) {
			if (func_name_lower == "abs") {
				// ABS is allowed inside SUM — will be linearized by the optimizer.
				// Treat ABS as opaque: just verify it references a decide variable.
				if (func.children.size() != 1) {
					error_msg = "ABS requires exactly one argument";
					return false;
				}
				if (ExpressionContainsDecideVariable(*func.children[0], variables)) {
					has_decide_variable = true;
				}
				return true;
			}
			if (func_name_lower == "power" || func_name_lower == "pow") {
				if (!allow_quadratic) {
					error_msg = "SUM expression must remain linear in DECIDE variables — "
					            "POWER(expr, 2) is only allowed in objectives, not constraints";
					return false;
				}
				return ValidateQuadraticPower(func.children, variables, has_decide_variable, error_msg, "POWER(..., 2)");
			}
			if (func_name_lower == "min" || func_name_lower == "max" || func_name_lower == "sum" || func_name_lower == "avg") {
				// Nested aggregates (e.g., SUM(MAX(expr)) for PER objectives) are allowed.
				// The optimizer will detect and rewrite them.
				if (func.children.size() == 1 && ExpressionContainsDecideVariable(*func.children[0], variables)) {
					has_decide_variable = true;
					return true;
				}
				error_msg = StringUtil::Format("Nested %s() inside DECIDE expression must reference a DECIDE variable",
				                               StringUtil::Upper(func_name_lower));
				return false;
			}
			if (!ExpressionContainsDecideVariable(expr, variables)) {
				// Data-only named scalar function on table columns (e.g. mod(id, 97),
				// floor(price)). It folds into a per-row coefficient during
				// symbolization and contributes no DECIDE variable. Data-only
				// aggregates (SUM/AVG/MIN/MAX of columns only) were already rejected
				// above — they require a DECIDE variable — so they never reach here.
				return true;
			}
			error_msg = StringUtil::Format("Unsupported function '%s' inside DECIDE SUM expression", func.function_name);
			return false;
		}
		if (func_name_lower == "**") {
			if (!allow_quadratic) {
				error_msg = "SUM expression must remain linear in DECIDE variables — "
				            "expr ** 2 is only allowed in objectives, not constraints";
				return false;
			}
			return ValidateQuadraticPower(func.children, variables, has_decide_variable, error_msg, "expr ** 2");
		}
		if (func_name_lower == "-") {
			for (auto &child : func.children) {
				if (!ValidateSumArgumentInternal(*child, variables, has_decide_variable, error_msg, allow_quadratic, allow_bilinear)) {
					return false;
				}
			}
			return true;
		}
		if (func_name_lower == "/") {
			// Division is linear iff the divisor contains no decide variable
			// (numerator can reference decide vars; dividing by a data
			// expression is just a coefficient scale). x / y where both
			// sides are decide vars is non-linear and rejected here.
			if (func.children.size() != 2) {
				error_msg = "Division requires exactly two arguments";
				return false;
			}
			if (ExpressionContainsDecideVariable(*func.children[1], variables)) {
				error_msg = "Division by a DECIDE variable is not supported "
				            "(would make the model non-linear)";
				return false;
			}
			return ValidateSumArgumentInternal(*func.children[0], variables,
			                                   has_decide_variable, error_msg,
			                                   allow_quadratic, allow_bilinear);
		}
		if (func_name_lower == "*" || func_name_lower == "+") {
			for (auto &child : func.children) {
				if (!ValidateSumArgumentInternal(*child, variables, has_decide_variable, error_msg, allow_quadratic, allow_bilinear)) {
					return false;
				}
			}
			if (func_name_lower == "*") {
				idx_t decide_count = CountDecideVariableOccurrencesInternal(expr, variables);
				if (decide_count > 2) {
					error_msg = "Triple or higher-order products of DECIDE variables are not supported "
					            "(total degree > 2)";
					return false;
				}
				if (decide_count > 1) {
					if (func.children.size() == 2) {
						// Classify: quadratic (same vars in both factors) vs bilinear (disjoint vars)
						unordered_set<idx_t> vars_left, vars_right;
						CollectDecideVariableIndices(*func.children[0], variables, vars_left);
						CollectDecideVariableIndices(*func.children[1], variables, vars_right);
						bool has_common_var = false;
						for (auto idx : vars_left) {
							if (vars_right.count(idx)) {
								has_common_var = true;
								break;
							}
						}
						if (has_common_var) {
							// Quadratic: same DECIDE variable appears in both factors
							// (e.g., x*x, (a+x)*x) — only allowed in objectives
							if (!allow_quadratic) {
								error_msg = "SUM expression must remain linear in DECIDE variables — "
								            "quadratic terms (same variable in both factors) are only allowed in objectives, not constraints";
								return false;
							}
							has_decide_variable = true;
							return true;
						} else {
							// Bilinear: different DECIDE variables in each factor
							// (e.g., x*y) — allowed in constraints with bilinear support
							if (!allow_bilinear && !allow_quadratic) {
								error_msg = "SUM expression must remain linear in DECIDE variables — "
								            "products of different DECIDE variables are only allowed in objectives and bilinear constraints";
								return false;
							}
							has_decide_variable = true;
							return true;
						}
					}
					error_msg = "SUM expression must remain linear in DECIDE variables — "
					            "products of different DECIDE variables are only allowed in objectives and bilinear constraints";
					return false;
				}
			}
			return true;
		}
		// An operator the model doesn't understand (`%`, bitwise ops, …) is still
		// fine if this subexpression references no DECIDE variable: it's a per-row
		// constant coefficient. The symbolic layer folds it to a data placeholder
		// (see ToSymbolicRecursive) and the physical layer evaluates it as data.
		// Only genuinely variable-bearing uses of an unsupported operator are rejected.
		if (!ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
		error_msg = StringUtil::Format("Unsupported operator '%s' inside DECIDE SUM expression", func.function_name);
		return false;
	}
	case ExpressionClass::OPERATOR: {
		error_msg = StringUtil::Format("Unexpected operator expression inside DECIDE SUM expression: %s", expr.ToString());
		return false;
	}
	case ExpressionClass::CAST: {
		auto &cast = expr.Cast<CastExpression>();
		return ValidateSumArgumentInternal(*cast.child, variables, has_decide_variable, error_msg, allow_quadratic, allow_bilinear);
	}
    case ExpressionClass::SUBQUERY: {
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
            error_msg = "Subquery in DECIDE SUM expression must be scalar";
            return false;
        }
        if (ExpressionContainsDecideVariable(expr, variables)) {
            error_msg = "Subquery in DECIDE SUM expression cannot contain DECIDE variables";
            return false;
        }
        return true;
    }
    case ExpressionClass::CASE:
        error_msg = "CASE expressions are not supported inside DECIDE constraints or "
                    "objectives. Use postfix WHEN to gate on a row predicate "
                    "(e.g. `SUM(x) >= 1 WHEN category = 'A'`), PER to partition by "
                    "a column, or a CTE/subquery to pre-compute conditional values "
                    "before the DECIDE clause.";
        return false;
    default:
        error_msg = StringUtil::Format("Unsupported expression of type ExpressionClass::%s inside DECIDE SUM expression",
                                       EnumUtil::ToString(expr.GetExpressionClass()));
        return false;
    }
}

// Check if an expression contains any quadratic pattern (POWER(_, 2), **, or identical multiplication).
// Used to detect quadratic objectives for early MAXIMIZE rejection at bind time.
bool ContainsQuadraticPattern(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	string fname = StringUtil::Lower(func.function_name);
	if (fname == "power" || fname == "pow" || fname == "**") {
		if (func.children.size() == 2 &&
		    func.children[1]->GetExpressionClass() == ExpressionClass::CONSTANT) {
			auto &exp_const = func.children[1]->Cast<ConstantExpression>();
			try {
				double exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
				if (exp_val == 2.0 && ExpressionContainsDecideVariable(*func.children[0], variables)) {
					return true;
				}
			} catch (...) {}
		}
	}
	if (fname == "*" && func.children.size() == 2) {
		idx_t decide_count = CountDecideVariableOccurrencesInternal(expr, variables);
		if (decide_count > 1) {
			// Both identical multiplication (QP) and bilinear products (x*y) are non-linear
			return true;
		}
	}
	// Recurse into children
	for (auto &child : func.children) {
		if (ContainsQuadraticPattern(*child, variables)) {
			return true;
		}
	}
	return false;
}

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables, string &error_msg,
                         bool allow_quadratic, bool allow_bilinear) {
	bool has_decide_variable = false;
	if (!ValidateSumArgumentInternal(expr, variables, has_decide_variable, error_msg, allow_quadratic, allow_bilinear)) {
		return false;
	}
	if (!has_decide_variable) {
		error_msg = "SUM expression must reference at least one DECIDE variable";
		return false;
	}
	return true;
}

DecideBinder::DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables) : ExpressionBinder(binder, context), variables(variables) {
    is_top_expression = true;        
}

BindResult DecideBinder::BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) {
	ErrorData error;

	// DECIDE clauses only accept SUM, AVG, MIN, MAX, and COUNT as aggregates. Reject anything
	// else (STRING_AGG, BIT_AND, MEDIAN, HISTOGRAM, etc.) at the canonical DuckDB hook,
	// mirroring WhereBinder::UnsupportedAggregateMessage. `count_star` is permitted because
	// the symbolic phase synthesizes `count_star()` internally when rewriting SUM(constant).
	if (!IsDecideAggregateName(func.name) && func.name != "count" && func.name != "count_star") {
		return BindResult(BinderException::Unsupported(
		    aggr, StringUtil::Format("DECIDE clause does not support aggregate '%s', only SUM, AVG, MIN, MAX, or COUNT is allowed.",
		                             func.name)));
	}

	// COUNT over a DECIDE variable is degenerate: decision variables are never null, so
	// COUNT(x) is identically the row count and does not constrain x. Reject it with a
	// semantic-specific message rather than silently letting it bind to a no-op constraint.
	if (func.name == "count") {
		for (auto &child : aggr.children) {
			if (ExpressionContainsDecideVariable(*child, variables)) {
				return BindResult(BinderException::Unsupported(
				    aggr, "COUNT over a DECIDE variable is degenerate: decision variables are never null, "
				          "so COUNT(x) always equals the row count. Did you mean SUM(x)?"));
			}
		}
	}

	// No Filter/Distinc allowed for Aggregate
	if (aggr.filter || aggr.distinct) {
        return BindResult(BinderException::Unsupported(aggr, StringUtil::Format("DECIDE clause does not support '%s'", aggr.ToString())));
	}

	// Bind arguments
	vector<unique_ptr<Expression>> children;
	vector<LogicalType> child_types;
	for (auto &child_expr : aggr.children) {
        // Use this->BindExpression to ensure subqueries are handled (executed at bind time)
        auto result = BindExpression(child_expr, depth);
        if (result.HasError()) {
            return result;
        }
        auto &bound_child = result.expression;
		child_types.push_back(bound_child->return_type);
		children.push_back(std::move(bound_child));
	}

	// 2. Bind the aggregate function itself
	FunctionBinder function_binder(binder);
	auto best_function_idx = function_binder.BindFunction(func.name, func.functions, child_types, error);
	if (!best_function_idx.IsValid()) {
		error.AddQueryLocation(aggr);
		return BindResult(std::move(error));
	}
	auto bound_function = func.functions.GetFunctionByOffset(best_function_idx.GetIndex());

	// 3. Create the BoundAggregateExpression itself.
	auto bound_aggregate = function_binder.BindAggregateFunction(bound_function, std::move(children));
	// Note: We are NOT storing this aggregate in a BoundSelectNode. We are returning it directly.
	return BindResult(std::move(bound_aggregate));
}

static BoundAggregateExpression *GetBoundAggregate(Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return &expr.Cast<BoundAggregateExpression>();
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		return GetBoundAggregate(*cast.child);
	}
	return nullptr;
}

BindResult DecideBinder::BindLocalWhenAggregate(FunctionExpression &when_expr, idx_t depth) {
	if (when_expr.children.size() != 2) {
		return BindResult(BinderException::Unsupported(
		    when_expr, "Aggregate-local WHEN expects exactly two arguments: aggregate WHEN condition."));
	}
	if (ExpressionContainsDecideVariable(*when_expr.children[1], variables)) {
		return BindResult(BinderException::Unsupported(
		    when_expr,
		    "Aggregate-local WHEN conditions cannot reference DECIDE variables. "
		    "The WHEN condition must only reference table columns."));
	}

	auto aggregate_result = BindExpression(when_expr.children[0], depth);
	if (aggregate_result.HasError()) {
		return aggregate_result;
	}
	auto aggregate_expr = std::move(aggregate_result.expression);
	auto *aggregate = GetBoundAggregate(*aggregate_expr);
	if (!aggregate) {
		return BindResult(BinderException::Unsupported(
		    when_expr, "Aggregate-local WHEN can only be applied directly to SUM, AVG, MIN, or MAX aggregates."));
	}
	if (aggregate->filter) {
		return BindResult(BinderException::Unsupported(
		    when_expr, "DECIDE aggregate-local WHEN cannot be combined with SQL FILTER on the same aggregate."));
	}

	auto condition_result = ExpressionBinder::BindExpression(when_expr.children[1], depth);
	if (condition_result.HasError()) {
		return condition_result;
	}
	aggregate->filter = BoundCastExpression::AddCastToType(context, std::move(condition_result.expression),
	                                                       LogicalType::BOOLEAN);
	return BindResult(std::move(aggregate_expr));
}

BindResult DecideBinder::BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &function = expr.Cast<FunctionExpression>();
    if (function.is_operator && function.function_name == WHEN_CONSTRAINT_TAG) {
        return BindLocalWhenAggregate(function, depth);
    }
    // Check if this is an aggregate function
    QueryErrorContext error_context(expr_ptr->GetQueryLocation());
    auto &catalog_entry = *GetCatalogEntry(CatalogType::SCALAR_FUNCTION_ENTRY, function.catalog, function.schema, function.function_name, OnEntryNotFound::THROW_EXCEPTION, error_context);
    
    if (catalog_entry.type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
        // It's an aggregate function, bind it using our custom aggregate logic
        return BindAggregate(function, catalog_entry.Cast<AggregateFunctionCatalogEntry>(), depth);
    }
    // It's a scalar function, bind it normally
    return ExpressionBinder::BindExpression(expr_ptr, depth);
}

BindResult DecideBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
    if (depth > 0) {
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
    auto &expr = *expr_ptr;
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION:
        return BindFunction(expr_ptr, depth);
    case ExpressionClass::SUBQUERY: {
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
             return BindResult(BinderException::Unsupported(expr, "Only scalar subqueries are supported in DECIDE"));
        }
        if (ExpressionContainsDecideVariable(expr, variables)) {
            return BindResult(BinderException::Unsupported(expr,
                "Subqueries in DECIDE clauses cannot reference DECIDE variables."));
        }
        // Standard binding handles both correlated and uncorrelated scalar subqueries.
        // Uncorrelated: PlanSubqueries evaluates them as cross-joined scalars.
        // Correlated: PlanSubqueries decorrelates them into joins, producing
        //             per-row values that the DECIDE operator evaluates normally.
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
    default:
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
}



} // namespace duckdb
