#pragma once

#include <memory>

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundCastExpression;
class BoundColumnRefExpression;
class ClientContext;
class Optimizer;
enum class DirectDomain : uint8_t;
struct DirectIR;
struct DirectPart;

//! Rule-independent questions about a bound expression. A rule asks them while proving that an expression may be
//! evaluated per source row; none of them knows which problem class it is serving.

//! True when the expression is deterministic and independent of the decision variables: it mentions no decision
//! column, is consistent (not even constant only within a query, like `now()`), and has no subquery, parameter,
//! aggregate, window, or correlated column.
bool DirectIsDecisionFreeDeterministic(const Expression &expr, idx_t decide_index);

//! True when every column reference is an uncorrelated column of the source plan.
bool DirectReferencesOnlySource(const Expression &expr, const vector<ColumnBinding> &source_bindings);

//! True when the expression mentions at least one column.
bool DirectHasColumnReference(const Expression &expr);

//! Re-points every source column reference at the same position in a projection over the source. Returns false,
//! leaving the expression partly rewritten, when a reference is not a source column.
bool DirectRemapSourceReferences(Expression &expr, const vector<ColumnBinding> &source_bindings,
                                 idx_t projection_index);

//! True when evaluating the expression can raise at run time. TRY_CAST turns a failed conversion into NULL, so it
//! does not count.
bool DirectMayThrow(const Expression &expr);

//! True for a cast that can fail on some value: a narrowing cast that is not TRY_CAST. This node only, not its child.
bool DirectCastMayThrow(const BoundCastExpression &cast);

//! A deterministic, nonthrowing BOOLEAN expression over source columns only: a predicate that can be evaluated for
//! every row without changing which rows raise an error.
bool DirectIsSourceOnlyPredicate(const Expression &expr, idx_t decide_index,
                                 const vector<ColumnBinding> &source_bindings);

//! What a direct rule demands of a constant it reads with `TryEvaluateFoldableDouble` (`decide_cast_policy.hpp`): one
//! that no row, parameter or subquery can change, with an evaluation error read as "not a constant". The value may be
//! infinite or NaN; a rule that reads a Boolean adds `FOLD_ALLOW_BOOLEAN`.
constexpr uint32_t DIRECT_CONSTANT = FOLD_STABLE | FOLD_CATCH_DUCKDB_ERRORS;

//! The same, for a numeric constant that must also be finite and survive a round trip through DOUBLE.
constexpr uint32_t DIRECT_FINITE_CONSTANT = DIRECT_CONSTANT | FOLD_EXACT_FINITE;

//! The factor a query-wide `2 * SUM(...)` or `SUM(...) / 2` puts on a reducer: 1 without one. False unless it is a
//! finite foldable constant, and nonzero when it divides.
bool DirectPartScale(ClientContext &context, const DirectPart &part, double &scale);

//! A data-valued value a rule may read per row and check for NULL (DirectCheckRowBounds): numeric, nonthrowing,
//! deterministic, decision-free, over source columns only, and mentioning at least one.
bool DirectIsSourceOnlyNumeric(const Expression &expr, idx_t decide_index,
                               const vector<ColumnBinding> &source_bindings);

//! The same, for a plan that evaluates the value on every row as the solver does, so an expression that can raise (an
//! overflow, a failed cast) raises on both paths or on neither: numeric, deterministic, decision-free, over source
//! columns only, mentioning at least one. It may raise.
bool DirectIsSourceOnlyNumericMayRaise(const Expression &expr, idx_t decide_index,
                                       const vector<ColumnBinding> &source_bindings);

//! A numeric or Boolean value a rule may read per row: deterministic, independent of the decisions, over source columns
//! only, with or without a column. Unlike DirectIsSourceOnlyNumeric it may raise when evaluated, for a rule that
//! evaluates it on the rows that read it, where the solver raises too (nothing is computed on a row a `when` drops).
bool DirectIsSourceOnlyValue(const Expression &expr, idx_t decide_index, const vector<ColumnBinding> &source_bindings);

//! True for an INT or BOOL decision, whose bounds round to whole numbers.
bool DirectIsWholeNumbered(DirectDomain domain);

//! A coefficient or value the plan evaluates per row: numeric, deterministic, independent of the decisions, over the
//! source. It may raise when evaluated; the plan evaluates it on the rows that read it, as the solver does.
bool DirectProveValue(const DirectIR &ir, const Expression &expr);

//! `x >= v`, `x > v`, and `x = v` all bound a count from below; `x <= v`, `x < v`, and `x = v` from above.
bool DirectIsLowerBound(ExpressionType comparison);
bool DirectIsUpperBound(ExpressionType comparison);

//! Expressions as values, so the per-row algebra reads like the algebra. Each operation copies its operands.
using ExprPtr = std::shared_ptr<Expression>;

class ExpressionBuilder {
public:
	explicit ExpressionBuilder(ClientContext &context_p);

	ExprPtr Wrap(unique_ptr<Expression> expr) const;
	unique_ptr<Expression> Out(const ExprPtr &expr) const;
	//! A copy of an existing expression, to combine with others.
	ExprPtr Of(const Expression &expr) const;
	ExprPtr Num(double value) const;
	ExprPtr Bool(bool value) const;
	//! A constant of exactly this value and type (`Num` always makes a DOUBLE). A NULL of a type is `Value(type)`.
	ExprPtr Constant(const Value &value) const;
	ExprPtr Col(const LogicalType &type, ColumnBinding binding) const;
	ExprPtr Fn(const string &name, const ExprPtr &a) const;
	ExprPtr Fn(const string &name, const ExprPtr &a, const ExprPtr &b) const;
	//! A scalar function of any number of arguments.
	ExprPtr Fn(const string &name, const vector<ExprPtr> &args) const;
	//! `a IS NULL`.
	ExprPtr IsNull(const ExprPtr &a) const;
	//! `a` converted to `type`; `a` itself when it already has that type.
	ExprPtr Cast(const ExprPtr &a, const LogicalType &type) const;
	//! The first value that is not NULL, of the first value's type.
	ExprPtr Coalesce(const vector<ExprPtr> &values) const;
	ExprPtr Add(const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr Sub(const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr Mul(const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr Div(const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr Neg(const ExprPtr &a) const;
	ExprPtr Cmp(ExpressionType type, const ExprPtr &a, const ExprPtr &b) const;
	ExprPtr If(const ExprPtr &condition, const ExprPtr &then_value, const ExprPtr &else_value) const;
	ExprPtr Min(const vector<ExprPtr> &values) const;
	ExprPtr Max(const vector<ExprPtr> &values) const;
	//! True when any condition is true; false for none.
	ExprPtr Any(const vector<ExprPtr> &conditions) const;

private:
	ClientContext &context;

	ExprPtr Extremum(const char *name, const vector<ExprPtr> &values) const;
};

} // namespace duckdb
