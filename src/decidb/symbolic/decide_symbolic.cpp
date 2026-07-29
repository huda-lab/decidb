//===----------------------------------------------------------------------===//
//                         DecidB
//
// decidb/symbolic/decide_symbolic.cpp
//
// Symbolic Translation Layer for DECIDE Expressions
//
//===----------------------------------------------------------------------===//
//
// ARCHITECTURE: Constraint LHS / objective body normalization
// -----------------------------------------------------------
// `NormalizeComparisonExpr` (constraints) and `NormalizeDecideObjective`
// (objectives) are the entry points that DuckDB's bind path calls before
// the DECIDE-specific binders run. Their job is to put the LHS / body into
// a canonical shape that the per-row aggregate-LHS extractor in
// `physical_decide.cpp` can walk: a tree of `+`/`-`/CAST/aggregate, with
// constants moved to the RHS, like-terms combined, and pure-data scalars
// hoisted out of SUM bodies as their own additive terms.
//
// The default normalizer uses SymEngine (`expand().simplify()`) for the
// arithmetic, which is correct for plain linear LHSes but DESTRUCTIVE for
// any expression carrying DECIDE-specific structural tags that SymEngine
// doesn't understand. SymEngine treats unknown nodes as opaque and freely
// reorders / flattens around them, scrambling structure the downstream
// pipeline needs.
//
// Result: the constraint normalizer has three mutually-exclusive structural paths,
// guarded by `if (...) return cmp.Copy()` early returns at the top of
// `NormalizeComparisonExpr`. They run in this order — first match wins:
//
//   1. Quadratic LHS bypass (`POWER(linear, 2)`, `(expr)*(expr)`)
//      Why: SymEngine `.expand()` would distribute the square and lose
//      the recognizable `POWER(linear, 2)` pattern that the QP extractor
//      pattern-matches on to populate the Q matrix.
//      Detector: `ComparisonLhsHasQuadratic`.
//      Downstream: QP path in `physical_decide.cpp::DetectQuadraticPattern`.
//
//   2. Composed MIN/MAX LHS bypass (`SUM(...) + MIN(...)` etc.)
//      Why: SymEngine doesn't know MIN/MAX semantics and would treat
//      MAX(x*v) as just another opaque function symbol, combining it
//      incorrectly with surrounding additive terms. The composed walker
//      needs the additive structure intact to emit per-aggregate
//      auxiliaries.
//      Detector: `ContainsTopLevelMinOrMax`.
//      Downstream: `RewriteComposedMinMaxInConstraint` in the optimizer.
//
//   3. Aggregate-local WHEN LHS path (`SUM(x) WHEN c`)
//      NOT a true bypass — does its own parsed-level rewrite.
//      Why: SymEngine doesn't know WHEN semantics and would absorb the
//      WHEN tag as opaque, then expand around it, scrambling which terms
//      the per-aggregate filter applies to.
//      What it does: (a) folds `K * (SUM(x) WHEN c)` and
//      `(SUM(x) WHEN c) / K` into `WHEN(SUM(K*x), c)` so the extractor
//      sees a bare WHEN-tagged aggregate; (b) decomposes the LHS
//      additively (recursing through `+`, binary `-`, unary `-`, CAST),
//      peels pure-numeric leaves into a single offset; (c) rebuilds the
//      LHS from the structural terms; (d) returns `LHS_struct OP (RHS - offset)`.
//      Helpers: `CopyAndFoldConstantsIntoWhenAggregates`,
//      `DecomposeAdditiveAtParsed`, `BuildAdditiveExpressionFromTerms`.
//
//   4. Default path: SymEngine `expand().simplify()` + term collection +
//      LHS/RHS partition by decide-var presence + rebuild.
//
// SAFETY INVARIANT (informal): each bypass is conservative — it preserves
// arbitrary leaf subtrees as opaque units when it doesn't recognize them.
// Cross-class shapes (e.g. quadratic + WHEN + additive offset) match the
// FIRST bypass in declaration order; the others would also have left them
// alone. Cross-product integration tests in
// `test/decide/tests/test_normalizer_path_interactions.py` pin this down
// for the practical combinations.
//
// REFACTOR TRIPWIRE: if a fifth bypass is needed, the right move is a
// dedicated refactor to a single classification-driven normalizer (see
// the prior-conversation discussion in branch "issue 3"). Five paths is
// too many to keep coherent by inspection.
//
//===----------------------------------------------------------------------===//

#include "duckdb/decidb/symbolic/decide_symbolic.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/expression_binder/decide_binder.hpp"
#include "symbolicc++.h"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/decidb/utility/debug.hpp"
#include <sstream>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <vector>
#include <limits>
#include <cstdlib>
#include <map>

namespace duckdb {

// Forward declare the recursive function
Symbolic ToSymbolicRecursive(const ParsedExpression &expr, SymbolicTranslationContext &ctx);
unique_ptr<ParsedExpression> FromSymbolic(const Symbolic &s, SymbolicTranslationContext &ctx);

static bool TryGetNonNegativeInteger(const Symbolic &value, int64_t &out_integer) {
    if (value.type() != typeid(Numeric)) {
        return false;
    }
    double numeric_value = double(value);
    double rounded = std::llround(numeric_value);
    if (fabs(numeric_value - rounded) > 1e-9) {
        return false;
    }
    if (rounded < 0) {
        return false;
    }
    out_integer = (int64_t)rounded;
    return true;
}

static Symbolic ApplySymbolicPower(const Symbolic &base, const Symbolic &exponent) {
    int64_t integer_exponent;
    if (TryGetNonNegativeInteger(exponent, integer_exponent)) {
        if (integer_exponent == 0) {
            return Symbolic(1.0);
        }
        Symbolic result = base;
        for (int64_t i = 1; i < integer_exponent; i++) {
            result = result * base;
        }
        return result;
    }
    return pow(base, exponent);
}

static bool SymbolicContainsDecideVariable(const Symbolic &s, const case_insensitive_map_t<idx_t> &decide_variables) {
    if (s.type() == typeid(Symbol)) {
        auto name = CastPtr<const Symbol>(s)->name;
        if (decide_variables.count(name) > 0) {
            return true;
        }
        // ABS placeholders contain decide variables by construction
        // (only created when the inner expression references a decide variable)
        if (name.rfind("__ABS_", 0) == 0) {
            return true;
        }
        return false;
    }
    if (s.type() == typeid(Numeric)) {
        return false;
    }
    if (s.type() == typeid(Product)) {
        CastPtr<const Product> prod(s);
        for (auto &factor : prod->factors) {
            if (SymbolicContainsDecideVariable(factor, decide_variables)) {
                return true;
            }
        }
        return false;
    }
    if (s.type() == typeid(Sum)) {
        CastPtr<const Sum> sum(s);
        for (auto &term : sum->summands) {
            if (SymbolicContainsDecideVariable(term, decide_variables)) {
                return true;
            }
        }
        return false;
    }
    if (s.type() == typeid(Power)) {
        CastPtr<const Power> power_node(s);
        return SymbolicContainsDecideVariable(power_node->parameters.front(), decide_variables) ||
               SymbolicContainsDecideVariable(power_node->parameters.back(), decide_variables);
    }
    // Fallback: inspect any Symbol-derived parameters
    try {
        CastPtr<const Symbol> sym(s);
        for (auto &param : sym->parameters) {
            if (SymbolicContainsDecideVariable(param, decide_variables)) {
                return true;
            }
        }
    } catch (...) {
        // Ignore if cast fails
    }
    return false;
}

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

bool IsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
    if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
        return false;
    }

    auto &colref = expr.Cast<ColumnRefExpression>();
    if (colref.IsQualified()) {
        // Check qualified form: Table.var (for table-scoped variables)
        string qualified = colref.GetTableName() + "." + colref.GetColumnName();
        return variables.count(qualified) > 0;
    }

    const auto &name = colref.GetColumnName();
    return variables.count(name) > 0;
}

//! Symbol name for a data column reference: the full dotted path, lowercased.
//! Lowercasing matches DuckDB's case-insensitive identifier resolution, so
//! `T1.W` and `t1.w` stay one symbol; the original reference is restored from
//! `ctx.column_map` by copy, so the folded case is never user-visible.
static string ColumnRefSymbolName(const ColumnRefExpression &colref) {
    return StringUtil::Lower(StringUtil::Join(colref.column_names, "."));
}

//===--------------------------------------------------------------------===//
// ToSymbolic Implementation
//===--------------------------------------------------------------------===//

Symbolic ToSymbolicRecursive(const ParsedExpression &expr, SymbolicTranslationContext &ctx) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::CONSTANT: {
            auto &const_expr = expr.Cast<ConstantExpression>();
            // Extract numeric value
            double value;
            switch (const_expr.value.type().id()) {
                case LogicalTypeId::INTEGER:
                    value = const_expr.value.GetValue<int32_t>();
                    break;
                case LogicalTypeId::BIGINT:
                    value = const_expr.value.GetValue<int64_t>();
                    break;
                case LogicalTypeId::DOUBLE:
                    value = const_expr.value.GetValue<double>();
                    break;
                case LogicalTypeId::FLOAT:
                    value = const_expr.value.GetValue<float>();
                    break;
                case LogicalTypeId::VARCHAR: {
                    auto s = const_expr.value.GetValue<string>();
                    return Symbolic(s.c_str());
                }
                case LogicalTypeId::SMALLINT:
                    value = const_expr.value.GetValue<int16_t>();
                    break;
                case LogicalTypeId::TINYINT:
                    value = const_expr.value.GetValue<int8_t>();
                    break;
                case LogicalTypeId::DECIMAL:
                    value = const_expr.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    break;
                case LogicalTypeId::HUGEINT: {
                    auto v = const_expr.value.DefaultCastAs(LogicalType::DOUBLE);
                    value = v.GetValue<double>();
                    break;
                }
                default:
                    throw InternalException("ToSymbolic: Unsupported constant type: %s", 
                        const_expr.value.type().ToString());
            }
            
            return Symbolic(value);
        }
        
        case ExpressionClass::COLUMN_REF: {
            auto &colref = expr.Cast<ColumnRefExpression>();
            if (IsDecideVariable(expr, ctx.decide_variables)) {
                // Canonicalize to the unqualified name: it is always registered as a
                // DECIDE variable (the qualified `T.x` form is an alias for the same
                // index), so `keepS` and `S.keepS` unify to a single symbol and the
                // `decide_variables.count(name)` classification below keeps working.
                return Symbolic(colref.GetColumnName());
            }
            // Data column: key the symbol by its full dotted path so two same-named
            // columns from different tables do not collapse into one symbol, and
            // stash the original reference for a lossless restore in FromSymbolic.
            auto path = ColumnRefSymbolName(colref);
            auto entry = ctx.column_map.find(path);
            if (entry == ctx.column_map.end()) {
                ctx.column_map[path] = expr.Copy();
            }
            return Symbolic(path);
        }
        
        case ExpressionClass::OPERATOR: {
            auto &op_expr = expr.Cast<OperatorExpression>();
            if (op_expr.type == ExpressionType::OPERATOR_NOT) {
                D_ASSERT(op_expr.children.size() == 1);
                auto child = ToSymbolicRecursive(*op_expr.children[0], ctx);
                std::stringstream cs;
                cs << child;
                return Symbolic("NOT(" + cs.str() + ")");
            }
            if (op_expr.type == ExpressionType::COMPARE_IN || op_expr.type == ExpressionType::COMPARE_NOT_IN) {
                // Represent IN as a special symbolic predicate: IN(left, rhs1, rhs2, ...)
                if (op_expr.children.size() < 2) {
                    throw InternalException("ToSymbolic: IN requires at least 2 children");
                }
                auto left = ToSymbolicRecursive(*op_expr.children[0], ctx);
                std::stringstream ls;
                ls << left;
                std::stringstream args_ss;
                for (idx_t i = 1; i < op_expr.children.size(); i++) {
                    if (i > 1) args_ss << ",";
                    auto right = ToSymbolicRecursive(*op_expr.children[i], ctx);
                    std::stringstream rs;
                    rs << right;
                    args_ss << rs.str();
                }
                string tag = (op_expr.type == ExpressionType::COMPARE_NOT_IN) ? "NOT_IN" : "IN";
                return Symbolic(tag + "(" + ls.str() + "," + args_ss.str() + ")");
            }
            throw InternalException("ToSymbolic: Unsupported operator type: %s", ExpressionTypeToString(op_expr.type));
        }

        case ExpressionClass::CAST: {
            auto &cast_expr = expr.Cast<CastExpression>();
            return ToSymbolicRecursive(*cast_expr.child, ctx);
        }

        case ExpressionClass::COMPARISON: {
            auto &cmp = expr.Cast<ComparisonExpression>();
            auto left = ToSymbolicRecursive(*cmp.left, ctx);
            auto right = ToSymbolicRecursive(*cmp.right, ctx);
            std::stringstream ls, rs;
            ls << left;
            rs << right;
            string tag;
            switch (cmp.type) {
                case ExpressionType::COMPARE_EQUAL:               tag = "EQ"; break;
                case ExpressionType::COMPARE_NOTEQUAL:            tag = "NE"; break;
                case ExpressionType::COMPARE_LESSTHAN:            tag = "LT"; break;
                case ExpressionType::COMPARE_LESSTHANOREQUALTO:   tag = "LE"; break;
                case ExpressionType::COMPARE_GREATERTHAN:         tag = "GT"; break;
                case ExpressionType::COMPARE_GREATERTHANOREQUALTO:tag = "GE"; break;
                case ExpressionType::COMPARE_BETWEEN:
                case ExpressionType::COMPARE_NOT_BETWEEN:
                case ExpressionType::COMPARE_IN:
                case ExpressionType::COMPARE_NOT_IN:
                case ExpressionType::COMPARE_DISTINCT_FROM:
                case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
                    throw InternalException("ToSymbolic: DISTINCT/IN/NOT IN handled elsewhere; BETWEEN has its own class");
                default:
                    throw InternalException("ToSymbolic: Unsupported comparison type: %s", ExpressionTypeToString(cmp.type));
            }
            return Symbolic(tag + "(" + ls.str() + "," + rs.str() + ")");
        }

        case ExpressionClass::BETWEEN: {
            auto &between = expr.Cast<BetweenExpression>();
            auto input = ToSymbolicRecursive(*between.input, ctx);
            auto lower = ToSymbolicRecursive(*between.lower, ctx);
            auto upper = ToSymbolicRecursive(*between.upper, ctx);
            std::stringstream is, ls, us;
            is << input; ls << lower; us << upper;
            return Symbolic("BETWEEN(" + is.str() + "," + ls.str() + "," + us.str() + ")");
        }

        case ExpressionClass::CONJUNCTION: {
            auto &conj = expr.Cast<ConjunctionExpression>();
            D_ASSERT(!conj.children.empty());
            string tag;
            if (conj.type == ExpressionType::CONJUNCTION_AND) tag = "AND";
            else if (conj.type == ExpressionType::CONJUNCTION_OR) tag = "OR";
            else throw InternalException("ToSymbolic: Unsupported conjunction type");
            std::stringstream ss;
            for (idx_t i = 0; i < conj.children.size(); i++) {
                if (i > 0) ss << ",";
                auto next = ToSymbolicRecursive(*conj.children[i], ctx);
                std::stringstream ns;
                ns << next;
                ss << ns.str();
            }
            return Symbolic(tag + "(" + ss.str() + ")");
        }
        
        case ExpressionClass::FUNCTION: {
            auto &func_expr = expr.Cast<FunctionExpression>();
            string func_name_lower = StringUtil::Lower(func_expr.function_name);
        
            // Handle built-in arithmetic operators rendered as functions
            if (func_expr.is_operator) {
                vector<Symbolic> args;
                args.reserve(func_expr.children.size());
                for (auto &child : func_expr.children) {
                    args.push_back(ToSymbolicRecursive(*child, ctx));
                }
        
                if (func_expr.function_name == "+") {
                    D_ASSERT(args.size() == 2);
                    return args[0] + args[1];
                } else if (func_expr.function_name == "-") {
                    if (args.size() == 1) {
                        return -args[0];
                    }
                    D_ASSERT(args.size() == 2);
                    return args[0] - args[1];
                } else if (func_expr.function_name == "*") {
                    D_ASSERT(args.size() == 2);
                    return args[0] * args[1];
                } else if (func_expr.function_name == "/") {
                    D_ASSERT(args.size() == 2);
                    return args[0] / args[1];
                } else if (func_expr.function_name == "^" || func_expr.function_name == "**") {
                    D_ASSERT(args.size() == 2);
                    return ApplySymbolicPower(args[0], args[1]);
                } else if (!ExpressionContainsDecideVariable(expr, ctx.decide_variables)) {
                    // The operator isn't modelled by the symbolic algebra, but this
                    // subexpression references no DECIDE variable — it's a per-row
                    // constant. Keep it opaque through normalization (like ABS) and
                    // let the physical layer evaluate it as ordinary data.
                    string placeholder = "__DATA_" + to_string(ctx.data_map.size()) + "__";
                    ctx.data_map[placeholder] = expr.Copy();
                    return Symbolic(placeholder);
                } else {
                    throw InvalidInputException(
                        "The '%s' operator is not supported inside a DECIDE clause. "
                        "Pre-compute it as a column in a subquery or CTE "
                        "(e.g. `SELECT (a %s b) AS c FROM ...`) and reference that "
                        "column in the DECIDE clause instead.",
                        func_expr.function_name, func_expr.function_name);
                }
            }

            if (func_name_lower == "sum") {
                if (func_expr.children.empty()) {
                    throw InternalException("ToSymbolic: SUM function with no arguments");
                }
                auto inner_symbolic = ToSymbolicRecursive(*func_expr.children[0], ctx);
                return Symbolic("__SUM__") * inner_symbolic;
            } else if (func_name_lower == "min") {
                if (func_expr.children.empty()) {
                    throw InternalException("ToSymbolic: MIN function with no arguments");
                }
                auto inner_symbolic = ToSymbolicRecursive(*func_expr.children[0], ctx);
                return Symbolic("__MIN__") * inner_symbolic;
            } else if (func_name_lower == "max") {
                if (func_expr.children.empty()) {
                    throw InternalException("ToSymbolic: MAX function with no arguments");
                }
                auto inner_symbolic = ToSymbolicRecursive(*func_expr.children[0], ctx);
                return Symbolic("__MAX__") * inner_symbolic;
            } else if (func_name_lower == "avg") {
                if (func_expr.children.empty()) {
                    throw InternalException("ToSymbolic: AVG function with no arguments");
                }
                auto inner_symbolic = ToSymbolicRecursive(*func_expr.children[0], ctx);
                return Symbolic("__AVG__") * inner_symbolic;
            } else if (func_name_lower == "pow" || func_name_lower == "power") {
                if (func_expr.children.size() != 2) {
                    throw InternalException("ToSymbolic: POW/POWER expects two arguments");
                }
                auto base = ToSymbolicRecursive(*func_expr.children[0], ctx);
                auto exponent = ToSymbolicRecursive(*func_expr.children[1], ctx);
                return ApplySymbolicPower(base, exponent);
            } else if (func_name_lower == "abs") {
                // ABS is nonlinear — treat as opaque placeholder (like subqueries).
                // Normalization sees it as a plain variable; the optimizer linearizes it later.
                if (func_expr.children.size() != 1) {
                    throw InternalException("ToSymbolic: ABS function requires one argument");
                }
                string placeholder = "__ABS_" + to_string(ctx.abs_map.size()) + "__";
                ctx.abs_map[placeholder] = expr.Copy();
                return Symbolic(placeholder);
            } else if (!ExpressionContainsDecideVariable(expr, ctx.decide_variables)) {
                // Named scalar function over table columns only (e.g. mod(id, 97),
                // floor(price)). Not modelled by the symbolic algebra, but data-only —
                // fold it to a per-row data placeholder, mirroring the is_operator
                // data-only path above and ABS. FromSymbolic restores the original
                // expression, and the physical layer evaluates it as ordinary data.
                string placeholder = "__DATA_" + to_string(ctx.data_map.size()) + "__";
                ctx.data_map[placeholder] = expr.Copy();
                return Symbolic(placeholder);
            } else {
                throw InternalException("ToSymbolic: Unsupported function: %s", func_expr.function_name);
            }
        }
        
        case ExpressionClass::SUBQUERY: {
            string placeholder = "__SUBQUERY_" + to_string(ctx.subquery_map.size()) + "__";
            ctx.subquery_map[placeholder] = expr.Copy();
            return Symbolic(placeholder);
        }

        case ExpressionClass::CASE:
            throw InvalidInputException(
                "CASE expressions are not supported inside DECIDE constraints or "
                "objectives. Use postfix WHEN to gate on a row predicate "
                "(e.g. `SUM(x) >= 1 WHEN category = 'A'`), PER to partition by "
                "a column, or a CTE/subquery to pre-compute conditional values "
                "before the DECIDE clause.");

        default:
            throw InternalException("ToSymbolic: Unsupported expression class: %s",
                EnumUtil::ToString(expr.GetExpressionClass()));
    }
}

//===--------------------------------------------------------------------===//
// FromSymbolic Implementation
//===--------------------------------------------------------------------===//

static unique_ptr<ParsedExpression> FromSymbolicNumber(double v) {
    return make_uniq_base<ParsedExpression, ConstantExpression>(Value::DOUBLE(v));
}

static unique_ptr<ParsedExpression> MakeOp(const string &op, unique_ptr<ParsedExpression> lhs, unique_ptr<ParsedExpression> rhs) {
    // Build arithmetic using operator FunctionExpression with is_operator=true
    vector<unique_ptr<ParsedExpression>> args;
    args.push_back(std::move(lhs));
    if (rhs) args.push_back(std::move(rhs));
    auto fe = make_uniq<FunctionExpression>(op, std::move(args), nullptr, nullptr, false, true);
    return std::move(fe);
}

static unique_ptr<ParsedExpression> BuildProductExpression(const vector<const Symbolic *> &factors,
                                                           SymbolicTranslationContext &ctx) {
    unique_ptr<ParsedExpression> acc;
    for (auto *factor : factors) {
        auto child = FromSymbolic(*factor, ctx);
        if (!acc) {
            acc = std::move(child);
        } else {
            acc = MakeOp("*", std::move(acc), std::move(child));
        }
    }
    return acc;
}

static void CollectAdditiveTerms(const Symbolic &expr, vector<Symbolic> &terms) {
    if (expr.type() == typeid(Sum)) {
        CastPtr<const Sum> sum(expr);
        for (auto &term : sum->summands) {
            CollectAdditiveTerms(term, terms);
        }
        return;
    }
    terms.push_back(expr);
}

static Symbolic SumSymbolicTerms(const vector<Symbolic> &terms) {
    if (terms.empty()) {
        return Symbolic(0.0);
    }
    Symbolic result = terms[0];
    for (idx_t i = 1; i < terms.size(); i++) {
        result = result + terms[i];
    }
    return result;
}

static Symbolic MultiplySymbolicFactors(const vector<Symbolic> &factors) {
    if (factors.empty()) {
        return Symbolic(1.0);
    }
    Symbolic result = factors[0];
    for (idx_t i = 1; i < factors.size(); i++) {
        result = result * factors[i];
    }
    return result;
}

static bool SymbolicIsApproxNumeric(const Symbolic &s, double target) {
    if (s.type() != typeid(Numeric)) {
        return false;
    }
    return fabs(double(s) - target) < 1e-12;
}

static bool SymbolicIsZero(const Symbolic &s) {
    return SymbolicIsApproxNumeric(s, 0.0);
}

static bool SymbolicIsOne(const Symbolic &s) {
    return SymbolicIsApproxNumeric(s, 1.0);
}

static bool SymbolicContainsDecideVariable(const Symbolic &s, const case_insensitive_map_t<idx_t> &decide_variables);
static bool IsSumMarker(const Symbolic &s);

static void CollectDecideFactors(const Symbolic &node, vector<Symbolic> &decide_factors,
                                 vector<Symbolic> &other_factors, const case_insensitive_map_t<idx_t> &decide_variables) {
    if (node.type() == typeid(Product)) {
        CastPtr<const Product> prod(node);
        for (auto &factor : prod->factors) {
            CollectDecideFactors(factor, decide_factors, other_factors, decide_variables);
        }
        return;
    }
    if (node.type() == typeid(Symbol)) {
        auto name = CastPtr<const Symbol>(node)->name;
        if (decide_variables.count(name) > 0 || name.rfind("__ABS_", 0) == 0) {
            decide_factors.push_back(node);
        } else {
            other_factors.push_back(node);
        }
        return;
    }
    if (node.type() == typeid(Numeric)) {
        other_factors.push_back(node);
        return;
    }
    if (SymbolicContainsDecideVariable(node, decide_variables)) {
        decide_factors.push_back(node);
    } else {
        other_factors.push_back(node);
    }
}

static pair<Symbolic, Symbolic> ExtractDecideFactors(const Symbolic &term, const case_insensitive_map_t<idx_t> &decide_variables) {
    vector<Symbolic> decide_factors;
    vector<Symbolic> other_factors;
    CollectDecideFactors(term, decide_factors, other_factors, decide_variables);
    Symbolic decide_part = MultiplySymbolicFactors(decide_factors);
    Symbolic other_part = MultiplySymbolicFactors(other_factors);
    return {decide_part, other_part};
}

struct FactoredTerm {
    Symbolic decide_part;
    Symbolic coefficient;
};

static bool ExtractSumInner(const Symbolic &term, Symbolic &inner_out) {
    if (term.type() == typeid(Product)) {
        CastPtr<const Product> prod(term);
        vector<Symbolic> remaining;
        bool has_sum = false;
        for (auto &factor : prod->factors) {
            if (IsSumMarker(factor)) {
                has_sum = true;
            } else {
                remaining.push_back(factor);
            }
        }
        if (has_sum) {
            inner_out = MultiplySymbolicFactors(remaining);
            return true;
        }
    }
    if (IsSumMarker(term)) {
        inner_out = Symbolic(1.0);
        return true;
    }
    return false;
}

static vector<FactoredTerm> CollectFactoredTerms(const Symbolic &expr, const case_insensitive_map_t<idx_t> &decide_variables) {
    vector<FactoredTerm> result;
    if (expr.type() != typeid(Sum)) {
        result.push_back(FactoredTerm{expr, Symbolic(1.0)});
        return result;
    }
    CastPtr<const Sum> sum(expr);
    std::map<string, FactoredTerm> grouped_terms;
    vector<string> order;
    for (auto &term : sum->summands) {
        auto parts = ExtractDecideFactors(term, decide_variables);
        Symbolic decide_part = parts.first.simplify();
        Symbolic coefficient = parts.second.simplify();
        std::stringstream ss;
        ss << decide_part;
        auto key = ss.str();
        auto it = grouped_terms.find(key);
        if (it == grouped_terms.end()) {
            grouped_terms.emplace(key, FactoredTerm{decide_part, coefficient});
            order.push_back(key);
        } else {
            it->second.coefficient = (it->second.coefficient + coefficient).simplify();
        }
    }
    for (auto &key : order) {
        auto &entry = grouped_terms[key];
        auto coeff = entry.coefficient.simplify();
        if (SymbolicIsZero(coeff)) {
            continue;
        }
        result.push_back(FactoredTerm{entry.decide_part.simplify(), coeff});
    }
    return result;
}

static unique_ptr<ParsedExpression> BuildFactoredTermExpression(const FactoredTerm &term, SymbolicTranslationContext &ctx) {
    bool decide_is_one = SymbolicIsOne(term.decide_part);
    bool coeff_is_one = SymbolicIsOne(term.coefficient);

    if (decide_is_one && coeff_is_one) {
        return FromSymbolicNumber(1.0);
    }
    if (decide_is_one) {
        return FromSymbolic(term.coefficient, ctx);
    }
    if (coeff_is_one) {
        return FromSymbolic(term.decide_part, ctx);
    }
    auto decide_expr = FromSymbolic(term.decide_part, ctx);
    auto coeff_expr = FromSymbolic(term.coefficient, ctx);
    return MakeOp("*", std::move(decide_expr), std::move(coeff_expr));
}

static unique_ptr<ParsedExpression> BuildFactoredSumExpression(const Symbolic &expr, SymbolicTranslationContext &ctx) {
    if (expr.type() != typeid(Sum)) {
        return FromSymbolic(expr, ctx);
    }
    auto terms = CollectFactoredTerms(expr, ctx.decide_variables);
    unique_ptr<ParsedExpression> aggregated;
    for (auto &term : terms) {
        auto term_expr = BuildFactoredTermExpression(term, ctx);
        if (!term_expr) {
            continue;
        }
        if (!aggregated) {
            aggregated = std::move(term_expr);
        } else {
            aggregated = MakeOp("+", std::move(aggregated), std::move(term_expr));
        }
    }
    if (!aggregated) {
        return FromSymbolicNumber(0.0);
    }
    return aggregated;
}

static unique_ptr<ParsedExpression> FromSymbolicProduct(const Product &prod, SymbolicTranslationContext &ctx) {
    vector<const Symbolic *> decide_factors;
    vector<const Symbolic *> other_factors;
    vector<const Symbolic *> all_factors;
    for (auto &factor : prod.factors) {
        all_factors.push_back(&factor);
        if (SymbolicContainsDecideVariable(factor, ctx.decide_variables)) {
            decide_factors.push_back(&factor);
        } else {
            other_factors.push_back(&factor);
        }
    }

    if (decide_factors.empty() || other_factors.empty()) {
        auto acc = BuildProductExpression(all_factors, ctx);
        if (!acc) {
            return FromSymbolicNumber(1.0);
        }
        return acc;
    }

    auto left_expr = BuildProductExpression(decide_factors, ctx);
    auto right_expr = BuildProductExpression(other_factors, ctx);
    if (!left_expr) {
        left_expr = FromSymbolicNumber(1.0);
    }
    if (!right_expr) {
        right_expr = FromSymbolicNumber(1.0);
    }
    return MakeOp("*", std::move(left_expr), std::move(right_expr));
}

static unique_ptr<ParsedExpression> FromSymbolicSum(const Sum &sum, SymbolicTranslationContext &ctx) {
    // Left fold using "+"
    unique_ptr<ParsedExpression> acc;
    for (auto &term : sum.summands) {
        auto child = FromSymbolic(term, ctx);
        if (!acc) acc = std::move(child);
        else acc = MakeOp("+", std::move(acc), std::move(child));
    }
    if (!acc) return FromSymbolicNumber(0.0);
    return acc;
}

static bool IsSumMarker(const Symbolic &s) {
    return s.type() == typeid(Symbol) && CastPtr<const Symbol>(s)->name == "__SUM__";
}

static bool IsMinMarker(const Symbolic &s) {
    return s.type() == typeid(Symbol) && CastPtr<const Symbol>(s)->name == "__MIN__";
}

static bool IsMaxMarker(const Symbolic &s) {
    return s.type() == typeid(Symbol) && CastPtr<const Symbol>(s)->name == "__MAX__";
}

static bool IsAvgMarker(const Symbolic &s) {
    return s.type() == typeid(Symbol) && CastPtr<const Symbol>(s)->name == "__AVG__";
}

static bool IsAggregateMarker(const Symbolic &s) {
    return IsSumMarker(s) || IsMinMarker(s) || IsMaxMarker(s) || IsAvgMarker(s);
}

static unique_ptr<ParsedExpression> FromSymbolicAggregateProduct(const Product &prod, SymbolicTranslationContext &ctx) {
    // Expect one aggregate marker (__SUM__, __MIN__, __MAX__, __AVG__) and remaining factors as inner expression
    list<Symbolic> non_markers;
    bool has_sum_marker = false;
    bool has_min_marker = false;
    bool has_max_marker = false;
    bool has_avg_marker = false;
    for (auto &f : prod.factors) {
        if (IsSumMarker(f)) has_sum_marker = true;
        else if (IsMinMarker(f)) has_min_marker = true;
        else if (IsMaxMarker(f)) has_max_marker = true;
        else if (IsAvgMarker(f)) has_avg_marker = true;
        else non_markers.push_back(f);
    }
    if (!(has_sum_marker || has_min_marker || has_max_marker || has_avg_marker) || non_markers.empty()) {
        // Fallback to regular product
        return FromSymbolicProduct(prod, ctx);
    }
    // Rebuild aggregate(inner)
    auto it = non_markers.begin();
    Symbolic inner = *it++;
    for (; it != non_markers.end(); ++it) inner = inner * (*it);
    vector<unique_ptr<ParsedExpression>> args;
    args.push_back(FromSymbolic(inner, ctx));
    string func_name = has_sum_marker ? "sum" : (has_min_marker ? "min" : (has_max_marker ? "max" : "avg"));
    return make_uniq_base<ParsedExpression, FunctionExpression>(func_name, std::move(args));
}

unique_ptr<ParsedExpression> FromSymbolic(const Symbolic &s, SymbolicTranslationContext &ctx) {
    if (s.type() == typeid(Numeric)) {
        return FromSymbolicNumber(double(s));
    }
    if (s.type() == typeid(Symbol)) {
        auto name = CastPtr<const Symbol>(s)->name;
        // Check if it's a subquery placeholder
        if (ctx.subquery_map.count(name)) {
            return ctx.subquery_map[name]->Copy();
        }
        // Check if it's an ABS placeholder
        if (ctx.abs_map.count(name)) {
            return ctx.abs_map[name]->Copy();
        }
        // Check if it's a folded data-only subexpression (e.g. `(id * 7) % 97`)
        if (ctx.data_map.count(name)) {
            return ctx.data_map[name]->Copy();
        }
        // Check if it's a qualified data column (`t1.w`) — restore the original
        // reference so the qualifier survives the round trip
        if (ctx.column_map.count(name)) {
            return ctx.column_map[name]->Copy();
        }
        // Treat any plain symbol as a column/variable reference
        return make_uniq_base<ParsedExpression, ColumnRefExpression>(name);
    }
    if (s.type() == typeid(Product)) {
        // Special-case aggregate marker
        CastPtr<const Product> prod(s);
        bool has_agg_marker = false;
        for (auto &f : prod->factors) if (IsAggregateMarker(f)) { has_agg_marker = true; break; }
        if (has_agg_marker) return FromSymbolicAggregateProduct(*prod, ctx);
        return FromSymbolicProduct(*prod, ctx);
    }
    if (s.type() == typeid(Sum)) {
        CastPtr<const Sum> sum(s);
        return FromSymbolicSum(*sum, ctx);
    }
    if (s.type() == typeid(Power)) {
        CastPtr<const Power> power_node(s);
        const auto &base = power_node->parameters.front();
        const auto &exponent = power_node->parameters.back();
        int64_t exponent_int;
        bool is_non_negative = TryGetNonNegativeInteger(exponent, exponent_int);
        // Negative integer exponent → reciprocal form (e.g. `x / w` → `x * w^-1`
        // symbolically). Emit `1.0 / base^|k|` so the downstream evaluator
        // sees a regular division. The binder has already rejected decide
        // variables in the denominator, so `base` here is pure data.
        if (!is_non_negative) {
            int64_t neg_exp;
            if (TryGetNonNegativeInteger(-exponent, neg_exp) && neg_exp > 0) {
                auto denom = FromSymbolic(base, ctx);
                for (int64_t i = 1; i < neg_exp; i++) {
                    denom = MakeOp("*", std::move(denom), FromSymbolic(base, ctx));
                }
                return MakeOp("/", FromSymbolicNumber(1.0), std::move(denom));
            }
            throw InternalException("FromSymbolic: Non-integer exponents are not supported in DECIDE normalization");
        }
        if (exponent_int == 0) {
            return FromSymbolicNumber(1.0);
        }
        auto result = FromSymbolic(base, ctx);
        for (int64_t i = 1; i < exponent_int; i++) {
            result = MakeOp("*", std::move(result), FromSymbolic(base, ctx));
        }
        return result;
    }
    // Fallback: stringify (debug) is not acceptable; throw
    throw InternalException("FromSymbolic: Unsupported symbolic node");
}

//===--------------------------------------------------------------------===//
// Normalization over ParsedExpression using Symbolic
//===--------------------------------------------------------------------===//

static bool IsNumericConstant(const ParsedExpression &expr, double &out) {
    if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) return false;
    auto &c = expr.Cast<ConstantExpression>();
    if (c.value.IsNull()) return false;
    switch (c.value.type().id()) {
        case LogicalTypeId::TINYINT: out = c.value.GetValue<int8_t>(); return true;
        case LogicalTypeId::SMALLINT: out = c.value.GetValue<int16_t>(); return true;
        case LogicalTypeId::INTEGER: out = c.value.GetValue<int32_t>(); return true;
        case LogicalTypeId::BIGINT: out = c.value.GetValue<int64_t>(); return true;
        case LogicalTypeId::FLOAT: out = c.value.GetValue<float>(); return true;
        case LogicalTypeId::DOUBLE: out = c.value.GetValue<double>(); return true;
        case LogicalTypeId::DECIMAL: out = c.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>(); return true;
        case LogicalTypeId::HUGEINT: out = c.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>(); return true;
        default: return false;
    }
}

static unique_ptr<ParsedExpression> MakeDoubleConstant(double v) {
    return make_uniq_base<ParsedExpression, ConstantExpression>(Value::DOUBLE(v));
}

// Check if an expression tree contains a SUM() function anywhere
static bool ContainsSumFunction(const ParsedExpression &expr) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::FUNCTION: {
            auto &func = expr.Cast<const FunctionExpression>();
            if (!func.is_operator && StringUtil::Lower(func.function_name) == "sum") {
                return true;
            }
            for (auto &child : func.children) {
                if (ContainsSumFunction(*child)) return true;
            }
            if (func.filter && ContainsSumFunction(*func.filter)) return true;
            return false;
        }
        case ExpressionClass::OPERATOR: {
            auto &op = expr.Cast<const OperatorExpression>();
            for (auto &child : op.children) {
                if (ContainsSumFunction(*child)) return true;
            }
            return false;
        }
        case ExpressionClass::CAST: {
            auto &cast = expr.Cast<const CastExpression>();
            return ContainsSumFunction(*cast.child);
        }
        case ExpressionClass::CONJUNCTION: {
            auto &conj = expr.Cast<const ConjunctionExpression>();
            for (auto &child : conj.children) {
                if (ContainsSumFunction(*child)) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

static bool ContainsAggregateLocalWhen(const ParsedExpression &expr) {
    if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = expr.Cast<const FunctionExpression>();
        if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
            return true;
        }
        for (auto &child : func.children) {
            if (ContainsAggregateLocalWhen(*child)) return true;
        }
        if (func.filter && ContainsAggregateLocalWhen(*func.filter)) return true;
        return false;
    }
    if (expr.GetExpressionClass() == ExpressionClass::OPERATOR) {
        auto &op = expr.Cast<const OperatorExpression>();
        for (auto &child : op.children) {
            if (ContainsAggregateLocalWhen(*child)) return true;
        }
        return false;
    }
    if (expr.GetExpressionClass() == ExpressionClass::CAST) {
        auto &cast = expr.Cast<const CastExpression>();
        return ContainsAggregateLocalWhen(*cast.child);
    }
    if (expr.GetExpressionClass() == ExpressionClass::COMPARISON) {
        auto &cmp = expr.Cast<const ComparisonExpression>();
        return ContainsAggregateLocalWhen(*cmp.left) || ContainsAggregateLocalWhen(*cmp.right);
    }
    return false;
}

// Does the top-level additive tree of `expr` contain a raw MIN() or MAX()
// aggregate as a sibling? This is the "composed MIN/MAX LHS" shape
// (e.g. `SUM(x*v) + MAX(x*v)`). We walk only through `+` / `-` / `CAST` — a
// MIN/MAX nested inside another function's argument (e.g. `SUM(x * MAX(v))`
// where `MAX(v)` is a scalar coefficient) is NOT a composed sibling.
static bool ContainsTopLevelMinOrMax(const ParsedExpression &expr) {
    if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = expr.Cast<const FunctionExpression>();
        if (!func.is_operator) {
            auto fname = StringUtil::Lower(func.function_name);
            return fname == "min" || fname == "max";
        }
        if (func.function_name == "+" || func.function_name == "-") {
            for (auto &child : func.children) {
                if (ContainsTopLevelMinOrMax(*child)) return true;
            }
        }
        return false;
    }
    if (expr.GetExpressionClass() == ExpressionClass::OPERATOR) {
        auto &op = expr.Cast<const OperatorExpression>();
        for (auto &child : op.children) {
            if (ContainsTopLevelMinOrMax(*child)) return true;
        }
        return false;
    }
    if (expr.GetExpressionClass() == ExpressionClass::CAST) {
        auto &cast = expr.Cast<const CastExpression>();
        return ContainsTopLevelMinOrMax(*cast.child);
    }
    return false;
}

// Normalize comparator between LHS and numeric RHS by isolating SUM terms containing DECIDE variables
// Forward declarations for quadratic detection (defined later in file).
static bool SumInnerIsQuadratic(const ParsedExpression &inner,
                                const case_insensitive_map_t<idx_t> &decide_variables);

// Check if a SUM inner expression contains ANY quadratic term (POWER/self-product),
// possibly as one additive term among several (e.g., SUM(POWER(x,2) + POWER(y,2) + linear_terms)).
// Recurses into + and - operators to find nested quadratic terms.
static bool SumInnerContainsQuadratic(const ParsedExpression &inner,
                                       const case_insensitive_map_t<idx_t> &decide_variables) {
    // Direct/negated/scaled quadratic (whole expression)
    if (SumInnerIsQuadratic(inner, decide_variables)) return true;
    if (inner.GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = inner.Cast<FunctionExpression>();
        // Recurse into additive/multiplicative terms
        if (func.is_operator && (func.function_name == "+" || func.function_name == "-" || func.function_name == "*")) {
            for (auto &child : func.children) {
                if (SumInnerContainsQuadratic(*child, decide_variables)) return true;
            }
        }
    }
    return false;
}

// Check if a comparison LHS contains a SUM whose inner has quadratic terms.
// If so, symbolic expansion would destroy the POWER/self-product structure.
static bool ComparisonLhsHasQuadratic(const ParsedExpression &lhs,
                                      const case_insensitive_map_t<idx_t> &decide_variables) {
    if (lhs.GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = lhs.Cast<FunctionExpression>();
        auto fname = StringUtil::Lower(func.function_name);
        if (!func.is_operator && fname == "sum" && !func.children.empty()) {
            if (SumInnerContainsQuadratic(*func.children[0], decide_variables)) return true;
        }
        // Recurse into operator children (for + / - combinations)
        for (auto &child : func.children) {
            if (ComparisonLhsHasQuadratic(*child, decide_variables)) return true;
        }
    }
    return false;
}

// Walk a parsed expression as an additive tree (`+`, binary `-`, unary `-`,
// CAST). Sign-tracks through subtraction. Stops at non-additive leaves.
//
// Each leaf is classified:
//   - pure numeric constant: contributes (sign * value) to `out_constant`,
//     does NOT appear in `out_terms`.
//   - everything else (aggregates, WHEN-tagged aggregates, columns,
//     multiplicative sub-expressions, etc.): pushed to `out_terms` as
//     (sign, leaf*) for the caller to reconstruct.
//
// Used to peel constant offsets from a constraint LHS or objective body
// without flattening WHEN-tagged aggregates (which the SymEngine-style
// symbolic library would do, losing the per-aggregate filter boundary).
static void DecomposeAdditiveAtParsed(
    const ParsedExpression &expr, int sign,
    vector<std::pair<int, const ParsedExpression *>> &out_terms,
    double &out_constant) {
    if (expr.GetExpressionClass() == ExpressionClass::CAST) {
        DecomposeAdditiveAtParsed(*expr.Cast<const CastExpression>().child,
                                  sign, out_terms, out_constant);
        return;
    }
    if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = expr.Cast<const FunctionExpression>();
        if (func.is_operator && func.function_name == "+") {
            for (auto &child : func.children) {
                DecomposeAdditiveAtParsed(*child, sign, out_terms, out_constant);
            }
            return;
        }
        if (func.is_operator && func.function_name == "-" && func.children.size() == 2) {
            DecomposeAdditiveAtParsed(*func.children[0], sign, out_terms, out_constant);
            DecomposeAdditiveAtParsed(*func.children[1], -sign, out_terms, out_constant);
            return;
        }
        if (func.is_operator && func.function_name == "-" && func.children.size() == 1) {
            DecomposeAdditiveAtParsed(*func.children[0], -sign, out_terms, out_constant);
            return;
        }
    }
    double cval;
    if (IsNumericConstant(expr, cval)) {
        out_constant += sign * cval;
        return;
    }
    out_terms.push_back({sign, &expr});
}

// Recursively rewrite `K * AGG(inner)` (and `AGG(inner) * K`,
// `AGG(inner) / K`) into `AGG(K * inner)`, where `AGG` is a DecidB aggregate
// (SUM/AVG/MIN/MAX) and optionally wrapped in WHEN_CONSTRAINT_TAG. Without
// this, constant scaling of an aggregate reaches the per-row extractor as
// `*(K, BoundAggregate)`, which it can't walk.
//
// Linearity invariant: K must be decide-var-free so distributing it into
// the aggregate's body preserves equivalence. A data column like `weight`
// satisfies this (the fold turns `weight * SUM(x)` into `SUM(weight * x)`,
// a standard per-row coefficient).
static unique_ptr<ParsedExpression> CopyAndFoldConstantsIntoAggregates(
    const ParsedExpression &expr,
    const case_insensitive_map_t<idx_t> &decide_variables);

// Return the inner aggregate FunctionExpression if `expr` is either a
// direct aggregate call (SUM/AVG/MIN/MAX) or a WHEN-tagged aggregate, and
// set `out_when` to the WHEN tag node if present (else nullptr). Unwraps
// CASTs.
static const FunctionExpression *AsFoldableAggregate(
    const ParsedExpression &expr, const FunctionExpression *&out_when) {
    out_when = nullptr;
    const ParsedExpression *cur = &expr;
    while (cur->GetExpressionClass() == ExpressionClass::CAST) {
        cur = cur->Cast<const CastExpression>().child.get();
    }
    if (cur->GetExpressionClass() != ExpressionClass::FUNCTION) return nullptr;
    auto &f = cur->Cast<const FunctionExpression>();
    if (f.is_operator && f.function_name == WHEN_CONSTRAINT_TAG) {
        if (f.children.size() != 2) return nullptr;
        const ParsedExpression *child = f.children[0].get();
        while (child->GetExpressionClass() == ExpressionClass::CAST) {
            child = child->Cast<const CastExpression>().child.get();
        }
        if (child->GetExpressionClass() != ExpressionClass::FUNCTION) return nullptr;
        auto &cf = child->Cast<const FunctionExpression>();
        if (cf.is_operator) return nullptr;
        auto name = StringUtil::Lower(cf.function_name);
        if (name != "sum" && name != "avg" && name != "min" && name != "max") return nullptr;
        out_when = &f;
        return &cf;
    }
    if (!f.is_operator) {
        auto name = StringUtil::Lower(f.function_name);
        if (name == "sum" || name == "avg" || name == "min" || name == "max") {
            return &f;
        }
    }
    return nullptr;
}

// Build a new aggregate (and optional WHEN wrapper) whose inner body is
// `K * inner` or `inner / K`. `factor` is moved into the new body.
static unique_ptr<ParsedExpression> MakeAggregateWithScaledInner(
    const FunctionExpression &agg,
    const FunctionExpression *when_tag,
    unique_ptr<ParsedExpression> factor,
    bool is_division,
    const case_insensitive_map_t<idx_t> &decide_variables) {
    if (agg.children.size() != 1) {
        return when_tag ? when_tag->Copy() : agg.Copy();
    }
    auto inner_copy = CopyAndFoldConstantsIntoAggregates(*agg.children[0], decide_variables);
    unique_ptr<ParsedExpression> new_inner;
    if (is_division) {
        new_inner = MakeOp("/", std::move(inner_copy), std::move(factor));
    } else {
        new_inner = MakeOp("*", std::move(factor), std::move(inner_copy));
    }
    vector<unique_ptr<ParsedExpression>> agg_args;
    agg_args.push_back(std::move(new_inner));
    auto new_agg = make_uniq<FunctionExpression>(agg.function_name, std::move(agg_args));
    if (!when_tag) {
        return std::move(new_agg);
    }
    vector<unique_ptr<ParsedExpression>> when_args;
    when_args.push_back(std::move(new_agg));
    when_args.push_back(when_tag->children[1]->Copy());
    auto result = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(when_args));
    result->is_operator = true;
    return std::move(result);
}

static unique_ptr<ParsedExpression> CopyAndFoldConstantsIntoAggregates(
    const ParsedExpression &expr,
    const case_insensitive_map_t<idx_t> &decide_variables) {
    if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = expr.Cast<const FunctionExpression>();
        if (func.is_operator && (func.function_name == "*" || func.function_name == "/") &&
            func.children.size() == 2) {
            const FunctionExpression *when_left = nullptr;
            const FunctionExpression *when_right = nullptr;
            const FunctionExpression *agg_left = AsFoldableAggregate(*func.children[0], when_left);
            const FunctionExpression *agg_right = AsFoldableAggregate(*func.children[1], when_right);
            if (func.function_name == "*") {
                // Either side may be the aggregate; the other side is the
                // scale factor. Reject only if both sides are aggregates
                // (that product is quadratic / bilinear — a separate concern
                // handled by its own pipeline).
                if (agg_left && !agg_right &&
                    !ExpressionContainsDecideVariable(*func.children[1], decide_variables)) {
                    auto factor = CopyAndFoldConstantsIntoAggregates(*func.children[1], decide_variables);
                    return MakeAggregateWithScaledInner(*agg_left, when_left, std::move(factor),
                                                       /*is_division=*/false, decide_variables);
                }
                if (agg_right && !agg_left &&
                    !ExpressionContainsDecideVariable(*func.children[0], decide_variables)) {
                    auto factor = CopyAndFoldConstantsIntoAggregates(*func.children[0], decide_variables);
                    return MakeAggregateWithScaledInner(*agg_right, when_right, std::move(factor),
                                                       /*is_division=*/false, decide_variables);
                }
            } else { // "/"
                // Only `AGG / K` is foldable (numerator must be the
                // aggregate; division by a decide variable was already
                // rejected upstream by the binder).
                if (agg_left &&
                    !ExpressionContainsDecideVariable(*func.children[1], decide_variables)) {
                    auto divisor = CopyAndFoldConstantsIntoAggregates(*func.children[1], decide_variables);
                    return MakeAggregateWithScaledInner(*agg_left, when_left, std::move(divisor),
                                                       /*is_division=*/true, decide_variables);
                }
            }
        }
        // Default: copy the function, recursively folding inside children.
        vector<unique_ptr<ParsedExpression>> new_children;
        new_children.reserve(func.children.size());
        for (auto &child : func.children) {
            new_children.push_back(CopyAndFoldConstantsIntoAggregates(*child, decide_variables));
        }
        auto result = make_uniq<FunctionExpression>(func.function_name, std::move(new_children));
        result->is_operator = func.is_operator;
        result->catalog = func.catalog;
        result->schema = func.schema;
        result->distinct = func.distinct;
        result->export_state = func.export_state;
        if (func.filter) {
            result->filter = func.filter->Copy();
        }
        return std::move(result);
    }
    // Non-function expressions: just copy.
    return expr.Copy();
}

// Reconstruct an additive expression from sign-tagged terms. Each entry is
// (sign, leaf*). The result is `term0 (+/-) term1 (+/-) ...`. Returns
// nullptr if `terms` is empty.
static unique_ptr<ParsedExpression> BuildAdditiveExpressionFromTerms(
    const vector<std::pair<int, const ParsedExpression *>> &terms) {
    if (terms.empty()) return nullptr;
    unique_ptr<ParsedExpression> acc;
    for (auto &entry : terms) {
        int sign = entry.first;
        auto term_copy = entry.second->Copy();
        if (!acc) {
            // First term: apply unary `-` if sign is negative.
            if (sign < 0) {
                vector<unique_ptr<ParsedExpression>> args;
                args.push_back(std::move(term_copy));
                auto neg = make_uniq<FunctionExpression>("-", std::move(args));
                neg->is_operator = true;
                acc = std::move(neg);
            } else {
                acc = std::move(term_copy);
            }
        } else {
            acc = MakeOp(sign >= 0 ? "+" : "-", std::move(acc), std::move(term_copy));
        }
    }
    return acc;
}

static unique_ptr<ParsedExpression> NormalizeComparisonExpr(const ComparisonExpression &cmp,
                                                            const case_insensitive_map_t<idx_t> &decide_variables) {
    // Only handle <=, <, >=, > with numeric RHS
    if (cmp.type != ExpressionType::COMPARE_LESSTHAN && cmp.type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
        cmp.type != ExpressionType::COMPARE_GREATERTHAN && cmp.type != ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
        return cmp.Copy();
    }
    double rhs_num;
    if (!IsNumericConstant(*cmp.right, rhs_num)) {
        return cmp.Copy();
    }

    // Only normalize if the LHS contains a SUM() somewhere in the tree.
    // For bare per-row constraints (e.g., "x < 6"), leave unchanged.
    if (!ContainsSumFunction(*cmp.left)) {
        return cmp.Copy();
    }

    // Aggregate-local WHEN needs to survive into binding/execution as a
    // per-aggregate filter. The full SymEngine-style normalization below
    // would flatten the aggregate tree and lose the filter boundary, so we
    // can't use it. Instead we do a smaller parsed-level rewrite that does
    // exactly what the user expects without touching the WHEN tag:
    //   1. Fold constant scalars into the WHEN-tagged aggregates' bodies
    //      (`K * (SUM(x) WHEN c)` → `WHEN(SUM(K*x), c)`,
    //      `(SUM(x) WHEN c) / K` → `WHEN(SUM(x/K), c)`). This makes the
    //      downstream extractor see a bare WHEN-tagged aggregate it can walk.
    //   2. Decompose the LHS additively, peel pure-numeric terms into a
    //      single offset, and rebuild the LHS from the structural terms.
    //   3. Move the offset to the RHS as `RHS - offset`, exactly mirroring
    //      what the symbolic library would do for the non-WHEN case.
    if (ContainsAggregateLocalWhen(*cmp.left)) {
        auto folded_lhs = CopyAndFoldConstantsIntoAggregates(*cmp.left, decide_variables);
        vector<std::pair<int, const ParsedExpression *>> structural;
        double lhs_offset = 0.0;
        DecomposeAdditiveAtParsed(*folded_lhs, +1, structural, lhs_offset);
        if (structural.empty()) {
            // LHS was entirely constants — pathological; let the caller see
            // the original (the SUM-presence guard above means this branch
            // should be unreachable, but be defensive).
            return cmp.Copy();
        }
        auto new_lhs = BuildAdditiveExpressionFromTerms(structural);
        auto new_rhs = MakeDoubleConstant(rhs_num - lhs_offset);
        return make_uniq_base<ParsedExpression, ComparisonExpression>(
            cmp.type, std::move(new_lhs), std::move(new_rhs));
    }

    // Skip normalization for quadratic constraints — symbolic expansion would
    // destroy the POWER(expr, 2) or self-product structure that the QP pipeline needs.
    if (ComparisonLhsHasQuadratic(*cmp.left, decide_variables)) {
        return cmp.Copy();
    }

    // Skip normalization when the LHS contains composed MIN/MAX (a MIN() or
    // MAX() aggregate appearing as an additive sibling to SUM, e.g.
    // `SUM(x*v) + MAX(x*v) <= K`). Symbolic normalization would wrap all
    // decide-variable-bearing terms in a single sum(), conflating the MIN/MAX
    // aggregate boundary with SUM-additive semantics and producing garbage
    // like `sum(x * (v + max(v)))`. The composed-MIN/MAX optimizer pass
    // handles this shape directly.
    if (ContainsTopLevelMinOrMax(*cmp.left)) {
        return cmp.Copy();
    }

    SymbolicTranslationContext ctx(decide_variables);
    Symbolic lhs_sym = ToSymbolicRecursive(*cmp.left, ctx).expand().simplify();

    vector<Symbolic> additive_terms;
    CollectAdditiveTerms(lhs_sym, additive_terms);

    vector<Symbolic> aggregate_inners;
    double lhs_constant = 0.0;
    bool has_nonaggregate_structural_term = false;

    aggregate_inners.reserve(additive_terms.size());

    for (auto &term : additive_terms) {
        if (term.type() == typeid(Numeric)) {
            lhs_constant += double(term);
            continue;
        }
        Symbolic inner;
        bool has_sum = ExtractSumInner(term, inner);
        if (has_sum) {
            // Keep every SUM(...) contribution on the aggregate LHS, including
            // data-only terms such as SUM(price + x). The physical/model-building
            // layer represents data-only aggregate terms as INVALID_INDEX
            // coefficients and subtracts their active-row contribution from RHS.
            aggregate_inners.push_back(inner);
        } else {
            // A non-aggregate structural sibling (e.g. SUM(x) + col <= K or
            // SUM(x) + y <= K) has per-row semantics and cannot be folded into
            // SUM(...). Leave the original tree intact so the normal constraint
            // extractor rejects it instead of silently changing semantics.
            has_nonaggregate_structural_term = true;
        }
    }

    if (aggregate_inners.empty() || has_nonaggregate_structural_term) {
        return cmp.Copy();
    }

    Symbolic aggregate_inner_sum = SumSymbolicTerms(aggregate_inners).simplify();
    auto lhs_inner_expr = BuildFactoredSumExpression(aggregate_inner_sum, ctx);
    vector<unique_ptr<ParsedExpression>> lhs_sum_args;
    lhs_sum_args.push_back(std::move(lhs_inner_expr));
    auto lhs_sum = make_uniq<FunctionExpression>("sum", std::move(lhs_sum_args));

    double rhs_constant = rhs_num - lhs_constant;
    unique_ptr<ParsedExpression> rhs_expr;
    if (fabs(rhs_constant) > 1e-12) {
        rhs_expr = MakeDoubleConstant(rhs_constant);
    }

    if (!rhs_expr) {
        rhs_expr = MakeDoubleConstant(0.0);
    }

    return make_uniq_base<ParsedExpression, ComparisonExpression>(cmp.type, std::move(lhs_sum), std::move(rhs_expr));
}

static unique_ptr<ParsedExpression> NormalizeConstraintsRecursive(const ParsedExpression &expr,
                                                                  const case_insensitive_map_t<idx_t> &decide_variables) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::CONJUNCTION: {
            auto &conj = expr.Cast<ConjunctionExpression>();
            vector<unique_ptr<ParsedExpression>> norm_children;
            norm_children.reserve(conj.children.size());
            for (auto &c : conj.children) {
                norm_children.push_back(NormalizeConstraintsRecursive(*c, decide_variables));
            }
            D_ASSERT(norm_children.size() >= 2);
            auto result = make_uniq<ConjunctionExpression>(conj.type, std::move(norm_children[0]), std::move(norm_children[1]));
            for (idx_t i = 2; i < norm_children.size(); i++) {
                result = make_uniq<ConjunctionExpression>(conj.type, std::move(result), std::move(norm_children[i]));
            }
            return std::move(result);
        }
        case ExpressionClass::COMPARISON: {
            auto &cmp = expr.Cast<ComparisonExpression>();
            return NormalizeComparisonExpr(cmp, decide_variables);
        }
        case ExpressionClass::FUNCTION: {
            // DecidB: Handle __when_constraint__(constraint, condition)
            auto &func = expr.Cast<FunctionExpression>();
            if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
                // Normalize the inner constraint (child[0]), pass through condition (child[1])
                auto normalized_constraint = NormalizeConstraintsRecursive(*func.children[0], decide_variables);

                // Fix grammar ambiguity: "A AND B WHEN C" parses as "(A AND B) WHEN C"
                // due to a_expr absorbing AND via shift/reduce. The user's intent is
                // "A AND (B WHEN C)" — WHEN binds to the rightmost constraint only.
                // Fix: pull all-but-last AND children out, wrap only the last with WHEN.
                if (normalized_constraint->GetExpressionClass() == ExpressionClass::CONJUNCTION) {
                    auto &conj = normalized_constraint->Cast<ConjunctionExpression>();
                    if (conj.children.size() >= 2) {
                        // Wrap only the last child with WHEN
                        auto cond_copy = func.children[1]->Copy();
                        vector<unique_ptr<ParsedExpression>> when_args;
                        when_args.push_back(std::move(conj.children.back()));
                        when_args.push_back(std::move(cond_copy));
                        auto when_expr = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(when_args));
                        when_expr->is_operator = true;

                        // Rebuild: unwrapped children AND WHEN(last, condition)
                        conj.children.pop_back();
                        conj.children.push_back(std::move(when_expr));
                        return std::move(normalized_constraint);
                    }
                }

                auto condition_copy = func.children[1]->Copy();
                vector<unique_ptr<ParsedExpression>> args;
                args.push_back(std::move(normalized_constraint));
                args.push_back(std::move(condition_copy));
                auto result = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(args));
                result->is_operator = true;
                return std::move(result);
            }
            if (func.is_operator && IsPerConstraintTag(func.function_name)) {
                // Normalize the inner constraint (child[0]), pass through PER columns (children[1..N])
                auto normalized_constraint = NormalizeConstraintsRecursive(*func.children[0], decide_variables);

                // Fix grammar ambiguity: "A AND B PER col" parses as "(A AND B) PER col"
                // due to a_expr absorbing AND via shift/reduce. The user's intent is
                // "A AND (B PER col)" — PER binds to the rightmost constraint only.
                // Fix: pull all-but-last AND children out, wrap only the last with PER.
                if (normalized_constraint->GetExpressionClass() == ExpressionClass::CONJUNCTION) {
                    auto &conj = normalized_constraint->Cast<ConjunctionExpression>();
                    if (conj.children.size() >= 2) {
                        // Wrap only the last child with PER
                        vector<unique_ptr<ParsedExpression>> per_args;
                        per_args.push_back(std::move(conj.children.back()));
                        for (idx_t i = 1; i < func.children.size(); i++) {
                            per_args.push_back(func.children[i]->Copy());
                        }
                        auto per_expr = make_uniq<FunctionExpression>(func.function_name, std::move(per_args));
                        per_expr->is_operator = true;

                        // Rebuild: unwrapped children AND PER(last, columns...)
                        conj.children.pop_back();
                        conj.children.push_back(std::move(per_expr));
                        return std::move(normalized_constraint);
                    }
                }

                vector<unique_ptr<ParsedExpression>> args;
                args.push_back(std::move(normalized_constraint));
                for (idx_t i = 1; i < func.children.size(); i++) {
                    args.push_back(func.children[i]->Copy());
                }
                auto result = make_uniq<FunctionExpression>(func.function_name, std::move(args));
                result->is_operator = true;
                return std::move(result);
            }
            return expr.Copy();
        }
        default:
            return expr.Copy();
    }
}

unique_ptr<ParsedExpression> NormalizeDecideConstraints(const ParsedExpression &expr,
                                                        const case_insensitive_map_t<idx_t> &decide_variables) {
    return NormalizeConstraintsRecursive(expr, decide_variables);
}

static bool ExprContainsDecideVar(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
    if (IsDecideVariable(expr, variables)) return true;
    bool found = false;
    ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
        if (!found && ExprContainsDecideVar(child, variables)) {
            found = true;
        }
    });
    return found;
}

static bool SumInnerIsQuadraticCore(const ParsedExpression &inner,
                                    const case_insensitive_map_t<idx_t> &decide_variables) {
    if (inner.GetExpressionClass() != ExpressionClass::FUNCTION) return false;
    auto &func = inner.Cast<FunctionExpression>();
    string name_lower = StringUtil::Lower(func.function_name);

    // POWER(expr, 2), POW(expr, 2)
    if (!func.is_operator && (name_lower == "power" || name_lower == "pow")) {
        if (func.children.size() == 2 &&
            func.children[1]->GetExpressionClass() == ExpressionClass::CONSTANT) {
            double val;
            if (IsNumericConstant(*func.children[1], val) && val == 2.0) {
                return ExprContainsDecideVar(*func.children[0], decide_variables);
            }
        }
        return false;
    }

    // expr ** 2
    if (func.is_operator && func.function_name == "**") {
        if (func.children.size() == 2 &&
            func.children[1]->GetExpressionClass() == ExpressionClass::CONSTANT) {
            double val;
            if (IsNumericConstant(*func.children[1], val) && val == 2.0) {
                return ExprContainsDecideVar(*func.children[0], decide_variables);
            }
        }
        return false;
    }

    // (expr) * (expr) where both sides are identical and contain a DECIDE variable
    if (func.is_operator && func.function_name == "*") {
        if (func.children.size() == 2 &&
            BaseExpression::Equals(*func.children[0], *func.children[1])) {
            return ExprContainsDecideVar(*func.children[0], decide_variables);
        }
    }

    return false;
}

static bool SumInnerIsQuadratic(const ParsedExpression &inner,
                                const case_insensitive_map_t<idx_t> &decide_variables) {
    // Direct quadratic pattern
    if (SumInnerIsQuadraticCore(inner, decide_variables)) {
        return true;
    }
    if (inner.GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = inner.Cast<FunctionExpression>();
        // Nested aggregate-of-quadratic: SUM/AVG/MIN/MAX(quadratic). Preserve
        // raw AST so the post-bind optimizer strip (decide_optimizer.cpp:634)
        // can flatten OUTER(INNER(expr)) without symbolic expansion destroying
        // the POWER node (which would leak a __SUM__ placeholder past the
        // nested-SUM validator in decide_binder.cpp:281).
        if (!func.is_operator && func.children.size() == 1) {
            string inner_name = StringUtil::Lower(func.function_name);
            if (inner_name == "sum" || inner_name == "avg" ||
                inner_name == "min" || inner_name == "max") {
                if (SumInnerIsQuadratic(*func.children[0], decide_variables)) {
                    return true;
                }
            }
        }
        // Scaled quadratic: -(POWER(expr, 2)) or K * POWER(expr, 2)
        // Unary negation: -(quadratic)
        if (func.is_operator && func.function_name == "-" && func.children.size() == 1) {
            return SumInnerIsQuadraticCore(*func.children[0], decide_variables);
        }
        // Multiplication by constant: K * quadratic or quadratic * K
        if (func.is_operator && func.function_name == "*" && func.children.size() == 2) {
            for (idx_t i = 0; i < 2; i++) {
                if (func.children[i]->GetExpressionClass() == ExpressionClass::CONSTANT) {
                    double val;
                    if (IsNumericConstant(*func.children[i], val) && val != 0.0) {
                        return SumInnerIsQuadraticCore(*func.children[1 - i], decide_variables);
                    }
                }
            }
        }
    }
    return false;
}

static bool IsDecideObjectiveAggregate(const ParsedExpression &expr) {
    if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = expr.Cast<const FunctionExpression>();
        if (!func.is_operator) {
            auto name_lower = StringUtil::Lower(func.function_name);
            return name_lower == "sum" || name_lower == "avg" || name_lower == "min" ||
                   name_lower == "max";
        }
    }
    if (expr.GetExpressionClass() == ExpressionClass::CAST) {
        auto &cast = expr.Cast<const CastExpression>();
        return IsDecideObjectiveAggregate(*cast.child);
    }
    return false;
}

static unique_ptr<ParsedExpression> ReassociateObjectiveWhenComparison(const ComparisonExpression &cmp) {
    if (cmp.left->GetExpressionClass() != ExpressionClass::FUNCTION) {
        return nullptr;
    }
    auto &left = cmp.left->Cast<const FunctionExpression>();
    if (!left.is_operator || left.function_name != WHEN_CONSTRAINT_TAG || left.children.size() != 2 ||
        !IsDecideObjectiveAggregate(*left.children[0])) {
        return nullptr;
    }

    auto condition = make_uniq<ComparisonExpression>(cmp.type, left.children[1]->Copy(), cmp.right->Copy());

    vector<unique_ptr<ParsedExpression>> args;
    args.push_back(left.children[0]->Copy());
    args.push_back(std::move(condition));
    auto result = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(args));
    result->is_operator = true;
    return std::move(result);
}

// Recursive worker for objective normalization. Handles the WHEN/PER/SUM
// dispatch and symbolic normalization of a single aggregate body. The top
// level (public NormalizeDecideObjective) peels additive constants and
// K*WHEN scalar factors before delegating into this worker, so `expr` here
// is guaranteed to be either a bare aggregate/WHEN/PER shape or a sum of
// such shapes — no constant offsets, no K*(WHEN-aggregate) products.
static unique_ptr<ParsedExpression> NormalizeObjectiveRecursive(
    const ParsedExpression &expr,
    const case_insensitive_map_t<idx_t> &decide_variables) {
    // Expect SUM(inner), possibly wrapped in WHEN or PER
    if (expr.GetExpressionClass() == ExpressionClass::COMPARISON) {
        auto &cmp = expr.Cast<const ComparisonExpression>();
        auto reassociated = ReassociateObjectiveWhenComparison(cmp);
        if (reassociated) {
            return NormalizeObjectiveRecursive(*reassociated, decide_variables);
        }
        return expr.Copy();
    }
    if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
        return expr.Copy();
    }
    auto &f = expr.Cast<FunctionExpression>();
    // Multi-aggregate additive body (`SUM(x) + SUM(y)`,
    // `SUM(x) WHEN c1 + SUM(y) WHEN c2`, etc.): normalize each term
    // independently and rebuild. The per-term normalizations preserve the
    // aggregate boundaries; the downstream extractor handles sum-of-aggregates
    // bodies natively.
    if (f.is_operator && (f.function_name == "+" || f.function_name == "-")) {
        vector<unique_ptr<ParsedExpression>> new_children;
        new_children.reserve(f.children.size());
        for (auto &child : f.children) {
            new_children.push_back(NormalizeObjectiveRecursive(*child, decide_variables));
        }
        auto result = make_uniq<FunctionExpression>(f.function_name, std::move(new_children));
        result->is_operator = true;
        return std::move(result);
    }
    // DecidB: Handle WHEN wrapper — normalize inner objective, pass through condition
    if (f.is_operator && f.function_name == WHEN_CONSTRAINT_TAG) {
        auto normalized = NormalizeObjectiveRecursive(*f.children[0], decide_variables);
        auto cond = f.children[1]->Copy();
        vector<unique_ptr<ParsedExpression>> args;
        args.push_back(std::move(normalized));
        args.push_back(std::move(cond));
        auto result = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(args));
        result->is_operator = true;
        return std::move(result);
    }
    // DecidB: Handle PER wrapper — normalize inner objective, pass through PER columns
    if (f.is_operator && IsPerConstraintTag(f.function_name)) {
        auto normalized = NormalizeObjectiveRecursive(*f.children[0], decide_variables);
        vector<unique_ptr<ParsedExpression>> args;
        args.push_back(std::move(normalized));
        for (idx_t i = 1; i < f.children.size(); i++) {
            args.push_back(f.children[i]->Copy());
        }
        auto result = make_uniq<FunctionExpression>(f.function_name, std::move(args));
        result->is_operator = true;
        return std::move(result);
    }
    if (StringUtil::Lower(f.function_name) != "sum" || f.children.empty()) {
        return expr.Copy();
    }

    // Skip normalization for quadratic objectives — symbolic expansion
    // would destroy the POWER(expr, 2) structure that the QP pipeline needs.
    if (SumInnerIsQuadratic(*f.children[0], decide_variables)) {
        return expr.Copy();
    }

    // Convert inner to Symbolic and simplify
    SymbolicTranslationContext ctx(decide_variables);
    Symbolic inner_sym = ToSymbolicRecursive(*f.children[0], ctx).expand().simplify();

    auto factored_terms = CollectFactoredTerms(inner_sym, ctx.decide_variables);
    vector<FactoredTerm> decide_terms;
    decide_terms.reserve(factored_terms.size());
    for (auto &term : factored_terms) {
        if (!SymbolicContainsDecideVariable(term.decide_part, ctx.decide_variables)) {
            continue;
        }
        decide_terms.push_back(term);
    }

    Symbolic combined_sym;
    bool has_terms = !decide_terms.empty();
    if (has_terms) {
        bool first = true;
        for (auto &term : decide_terms) {
            Symbolic combined(term.decide_part);
            combined = combined * term.coefficient;
            if (first) {
                combined_sym = combined;
                first = false;
            } else {
                combined_sym = combined_sym + combined;
            }
        }
    } else {
        combined_sym = Symbolic(0.0);
    }

    combined_sym = combined_sym.expand().simplify();
    auto new_inner = BuildFactoredSumExpression(combined_sym, ctx);
    vector<unique_ptr<ParsedExpression>> args;
    args.push_back(std::move(new_inner));
    return make_uniq_base<ParsedExpression, FunctionExpression>("sum", std::move(args));
}

unique_ptr<ParsedExpression> NormalizeDecideObjective(const ParsedExpression &expr,
                                                      const case_insensitive_map_t<idx_t> &decide_variables,
                                                      double &out_constant_offset) {
    out_constant_offset = 0.0;

    // Step 1: fold K * (SUM WHEN c) and (SUM WHEN c) / K so the downstream
    // extractor sees bare WHEN-tagged aggregates even when the user scaled
    // them with a constant factor. Mirrors the constraint-side treatment in
    // NormalizeComparisonExpr. No-op for non-WHEN bodies and for bodies
    // where WHEN-tagged aggregates aren't wrapped in */÷.
    auto folded_body = CopyAndFoldConstantsIntoAggregates(expr, decide_variables);

    // Step 2: peel additive constants. Walks `+`, binary/unary `-`, CAST.
    // Pure-numeric leaves go into the offset; structural terms are preserved
    // with their signs for rebuild. The offset doesn't affect argmax/argmin,
    // so dropping it from the objective body is mathematically free.
    vector<std::pair<int, const ParsedExpression *>> structural;
    double offset = 0.0;
    DecomposeAdditiveAtParsed(*folded_body, +1, structural, offset);

    unique_ptr<ParsedExpression> peeled_body;
    if (structural.empty()) {
        // Pure-constant objective (`MAXIMIZE 7`). Degenerate — argmax is any
        // feasible point. Hand the original expression to the downstream
        // binder so it produces whatever error/behavior is appropriate.
        peeled_body = folded_body->Copy();
    } else if (fabs(offset) < 1e-12 && structural.size() == 1 && structural[0].first > 0) {
        // Nothing peeled and no rebuild needed (single structural term with
        // positive sign): skip the rebuild to keep the expression shape
        // identical for the common case where no offset was present.
        peeled_body = folded_body->Copy();
    } else {
        peeled_body = BuildAdditiveExpressionFromTerms(structural);
        out_constant_offset = offset;
    }

    // Step 3: run the recursive normalizer on the cleaned-up body. After
    // peel + fold, the body is a bare aggregate, a WHEN/PER-wrapped
    // aggregate, or an additive sum of such terms — all shapes the
    // recursive worker and downstream extractor handle natively.
    return NormalizeObjectiveRecursive(*peeled_body, decide_variables);
}

//===--------------------------------------------------------------------===//
// DOT graph export for ParsedExpression
//===--------------------------------------------------------------------===//

static void DotEscape(string &s) {
    for (auto &ch : s) {
        if (ch == '"') ch = '\'';
    }
}

static void ExpressionToDotImpl(const ParsedExpression &expr, std::stringstream &ss, idx_t &next_id, idx_t parent_id) {
    idx_t my_id = next_id++;
    string label = EnumUtil::ToString(expr.GetExpressionClass());
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::FUNCTION: {
            auto &f = expr.Cast<FunctionExpression>();
            label = string("FUNCTION ") + f.function_name + (f.is_operator ? " (op)" : "");
            break;
        }
        case ExpressionClass::COLUMN_REF: {
            auto &c = expr.Cast<ColumnRefExpression>();
            label = string("COLUMN ") + c.GetColumnName();
            break;
        }
        case ExpressionClass::CONSTANT: {
            auto &c = expr.Cast<ConstantExpression>();
            label = string("CONST ") + c.value.ToString();
            break;
        }
        case ExpressionClass::COMPARISON: {
            auto &c = expr.Cast<ComparisonExpression>();
            label = string("COMP ") + ExpressionTypeToString(c.type);
            break;
        }
        case ExpressionClass::CONJUNCTION: {
            auto &c = expr.Cast<ConjunctionExpression>();
            label = string("CONJ ") + ExpressionTypeToString(c.type);
            break;
        }
        case ExpressionClass::OPERATOR: {
            auto &o = expr.Cast<OperatorExpression>();
            label = string("OPER ") + ExpressionTypeToString(o.type);
            break;
        }
        case ExpressionClass::CAST: {
            auto &c = expr.Cast<CastExpression>();
            label = string("CAST ") + c.cast_type.ToString();
            break;
        }
        case ExpressionClass::BETWEEN: {
            label = "BETWEEN";
            break;
        }
        default:
            break;
    }
    DotEscape(label);
    ss << "  n" << my_id << " [label=\"" << label << "\"];\n";
    if (parent_id != (idx_t)-1) {
        ss << "  n" << parent_id << " -> n" << my_id << ";\n";
    }

    switch (expr.GetExpressionClass()) {
        case ExpressionClass::FUNCTION: {
            auto &f = expr.Cast<FunctionExpression>();
            for (auto &ch : f.children) ExpressionToDotImpl(*ch, ss, next_id, my_id);
            if (f.filter) ExpressionToDotImpl(*f.filter, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::COMPARISON: {
            auto &c = expr.Cast<ComparisonExpression>();
            ExpressionToDotImpl(*c.left, ss, next_id, my_id);
            ExpressionToDotImpl(*c.right, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::CONJUNCTION: {
            auto &c = expr.Cast<ConjunctionExpression>();
            for (auto &ch : c.children) ExpressionToDotImpl(*ch, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::OPERATOR: {
            auto &o = expr.Cast<OperatorExpression>();
            for (auto &ch : o.children) ExpressionToDotImpl(*ch, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::CAST: {
            auto &c = expr.Cast<CastExpression>();
            ExpressionToDotImpl(*c.child, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::BETWEEN: {
            auto &b = expr.Cast<BetweenExpression>();
            ExpressionToDotImpl(*b.input, ss, next_id, my_id);
            ExpressionToDotImpl(*b.lower, ss, next_id, my_id);
            ExpressionToDotImpl(*b.upper, ss, next_id, my_id);
            break;
        }
        default:
            break;
    }
}

string ExpressionToDot(const ParsedExpression &expr) {
    std::stringstream ss;
    ss << "digraph ParsedExpression {\n";
    ss << "  node [shape=box, fontsize=10];\n";
    idx_t next_id = 0;
    ExpressionToDotImpl(expr, ss, next_id, (idx_t)-1);
    ss << "}\n";
    return ss.str();
}

} // namespace duckdb
