//===----------------------------------------------------------------------===//
//                         DecidB
//
// decidb/parsed/decide_grammar_repair.cpp
//
// Post-parse repair of DECIDE clause association. See the header for why the
// mis-association exists and why the fix is not (yet) in the grammar itself.
//===----------------------------------------------------------------------===//

#include "duckdb/decidb/parsed/decide_grammar_repair.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

#include <sstream>

namespace duckdb {

//===--------------------------------------------------------------------===//
// SUCH THAT: `A AND B WHEN c` binds WHEN to B, not to `A AND B`
//===--------------------------------------------------------------------===//

static unique_ptr<ParsedExpression> RepairConstraintRecursive(const ParsedExpression &expr) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::CONJUNCTION: {
            auto &conj = expr.Cast<ConjunctionExpression>();
            vector<unique_ptr<ParsedExpression>> repaired_children;
            repaired_children.reserve(conj.children.size());
            for (auto &c : conj.children) {
                repaired_children.push_back(RepairConstraintRecursive(*c));
            }
            D_ASSERT(repaired_children.size() >= 2);
            auto result = make_uniq<ConjunctionExpression>(conj.type, std::move(repaired_children[0]), std::move(repaired_children[1]));
            for (idx_t i = 2; i < repaired_children.size(); i++) {
                result = make_uniq<ConjunctionExpression>(conj.type, std::move(result), std::move(repaired_children[i]));
            }
            return std::move(result);
        }
        case ExpressionClass::COMPARISON: {
            // Constraint shape is decided once, by DecideCanonicalizer, on the
            // bound tree. Nothing here touches it.
            return expr.Copy();
        }
        case ExpressionClass::FUNCTION: {
            // DecidB: Handle __when_constraint__(constraint, condition)
            auto &func = expr.Cast<FunctionExpression>();
            if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
                // Normalize the inner constraint (child[0]), pass through condition (child[1])
                auto repaired = RepairConstraintRecursive(*func.children[0]);

                // Fix grammar ambiguity: "A AND B WHEN C" parses as "(A AND B) WHEN C"
                // due to a_expr absorbing AND via shift/reduce. The user's intent is
                // "A AND (B WHEN C)" — WHEN binds to the rightmost constraint only.
                // Fix: pull all-but-last AND children out, wrap only the last with WHEN.
                if (repaired->GetExpressionClass() == ExpressionClass::CONJUNCTION) {
                    auto &conj = repaired->Cast<ConjunctionExpression>();
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
                        return std::move(repaired);
                    }
                }

                auto condition_copy = func.children[1]->Copy();
                vector<unique_ptr<ParsedExpression>> args;
                args.push_back(std::move(repaired));
                args.push_back(std::move(condition_copy));
                auto result = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(args));
                result->is_operator = true;
                return std::move(result);
            }
            if (func.is_operator && IsPerConstraintTag(func.function_name)) {
                // Normalize the inner constraint (child[0]), pass through PER columns (children[1..N])
                auto repaired = RepairConstraintRecursive(*func.children[0]);

                // Fix grammar ambiguity: "A AND B PER col" parses as "(A AND B) PER col"
                // due to a_expr absorbing AND via shift/reduce. The user's intent is
                // "A AND (B PER col)" — PER binds to the rightmost constraint only.
                // Fix: pull all-but-last AND children out, wrap only the last with PER.
                if (repaired->GetExpressionClass() == ExpressionClass::CONJUNCTION) {
                    auto &conj = repaired->Cast<ConjunctionExpression>();
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
                        return std::move(repaired);
                    }
                }

                vector<unique_ptr<ParsedExpression>> args;
                args.push_back(std::move(repaired));
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


unique_ptr<ParsedExpression> RepairDecideConstraintGrammar(const ParsedExpression &expr) {
    return RepairConstraintRecursive(expr);
}

//===--------------------------------------------------------------------===//
// MAXIMIZE/MINIMIZE: `SUM(x) WHEN a > b` puts the comparison outside the WHEN
//===--------------------------------------------------------------------===//

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
// Walk the objective's additive spine and its WHEN/PER wrappers looking for the
// mis-parsed comparison. Only association is repaired; every other node is copied.
static unique_ptr<ParsedExpression> RepairObjectiveRecursive(const ParsedExpression &expr) {
    if (expr.GetExpressionClass() == ExpressionClass::COMPARISON) {
        auto &cmp = expr.Cast<const ComparisonExpression>();
        auto reassociated = ReassociateObjectiveWhenComparison(cmp);
        if (reassociated) {
            return RepairObjectiveRecursive(*reassociated);
        }
        return expr.Copy();
    }
    if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
        return expr.Copy();
    }
    auto &f = expr.Cast<FunctionExpression>();
    // Additive spine: a mis-parsed comparison can sit under any term.
    if (f.is_operator && (f.function_name == "+" || f.function_name == "-")) {
        vector<unique_ptr<ParsedExpression>> repaired_children;
        repaired_children.reserve(f.children.size());
        for (auto &child : f.children) {
            repaired_children.push_back(RepairObjectiveRecursive(*child));
        }
        auto result = make_uniq<FunctionExpression>(f.function_name, std::move(repaired_children));
        result->is_operator = true;
        return std::move(result);
    }
    // WHEN wrapper: repair the objective child, copy the condition unchanged.
    if (f.is_operator && f.function_name == WHEN_CONSTRAINT_TAG && f.children.size() == 2) {
        vector<unique_ptr<ParsedExpression>> args;
        args.push_back(RepairObjectiveRecursive(*f.children[0]));
        args.push_back(f.children[1]->Copy());
        auto result = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(args));
        result->is_operator = true;
        return std::move(result);
    }
    // PER wrapper: repair the objective child, copy the grouping columns unchanged.
    if (f.is_operator && IsPerConstraintTag(f.function_name) && !f.children.empty()) {
        vector<unique_ptr<ParsedExpression>> args;
        args.push_back(RepairObjectiveRecursive(*f.children[0]));
        for (idx_t i = 1; i < f.children.size(); i++) {
            args.push_back(f.children[i]->Copy());
        }
        auto result = make_uniq<FunctionExpression>(f.function_name, std::move(args));
        result->is_operator = true;
        return std::move(result);
    }
    return expr.Copy();
}

unique_ptr<ParsedExpression> RepairDecideObjectiveGrammar(const ParsedExpression &expr) {
    return RepairObjectiveRecursive(expr);
}

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
