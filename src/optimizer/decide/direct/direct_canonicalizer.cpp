#include "duckdb/optimizer/decide/direct/direct_canonicalizer.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

namespace {

//! The columns of an operator's output that hold the same constant on every row.
using ConstantColumns = column_binding_map_t<Value>;

//! Replaces a reference to a column that holds a constant by the constant.
class ConstantPropagator final : public LogicalOperatorVisitor {
public:
	explicit ConstantPropagator(const ConstantColumns &constants_p) : constants(constants_p) {
	}

	//! The expressions of `op` that evaluate a value per row. The keys of a join and of an aggregate are not among
	//! them: a hash join and a hash aggregate are built on columns.
	void Propagate(LogicalOperator &op) {
		if (constants.empty()) {
			return;
		}
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_WINDOW:
			VisitOperatorExpressions(op);
			break;
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
			for (auto &aggregate : op.expressions) {
				VisitExpression(&aggregate);
			}
			break;
		default:
			break;
		}
	}

protected:
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *) override {
		auto found = constants.find(expr.binding);
		if (found == constants.end()) {
			return nullptr;
		}
		return make_uniq<BoundConstantExpression>(found->second);
	}

private:
	const ConstantColumns &constants;
};

//! The constant columns a join passes on: the side that is not NULL-padded.
void PassOn(ConstantColumns &to, const ConstantColumns &from) {
	for (auto &column : from) {
		to.insert(column);
	}
}

class Canonicalizer {
public:
	Canonicalizer(Optimizer &optimizer_p, const LogicalOperator &source_p) : optimizer(optimizer_p), source(source_p) {
	}

	//! Canonicalizes the plan below and including `op`, children first, and returns the constant columns of its output.
	ConstantColumns Visit(LogicalOperator &op) {
		if (&op == &source) {
			return ConstantColumns();
		}
		vector<ConstantColumns> children;
		ConstantColumns visible;
		for (auto &child : op.children) {
			children.push_back(Visit(*child));
			PassOn(visible, children.back());
		}
		ConstantPropagator(visible).Propagate(op);
		optimizer.rewriter.RewriteOperatorExpressions(op);
		return OutputOf(op, children);
	}

private:
	static ConstantColumns OutputOf(LogicalOperator &op, const vector<ConstantColumns> &children) {
		ConstantColumns output;
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			auto &projection = op.Cast<LogicalProjection>();
			for (idx_t i = 0; i < projection.expressions.size(); i++) {
				auto &expression = *projection.expressions[i];
				if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
					output.emplace(ColumnBinding(projection.table_index, i),
					               expression.Cast<BoundConstantExpression>().value);
				}
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_WINDOW:
			// They pass their child's columns on.
			PassOn(output, children[0]);
			break;
		case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
			PassOn(output, children[0]);
			PassOn(output, children[1]);
			break;
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
			// A column of the side an outer join pads is NULL for a row with no partner, so only an inner join passes
			// both sides on. The joins a rule builds that keep every row of their left side (LEFT, SINGLE) pass the
			// left on; any other join passes nothing.
			auto join_type = op.Cast<LogicalComparisonJoin>().join_type;
			if (join_type == JoinType::INNER) {
				PassOn(output, children[1]);
			}
			if (join_type == JoinType::INNER || join_type == JoinType::LEFT || join_type == JoinType::SINGLE) {
				PassOn(output, children[0]);
			}
			break;
		}
		default:
			break;
		}
		return output;
	}

	Optimizer &optimizer;
	const LogicalOperator &source;
};

} // namespace

void DirectCanonicalize(Optimizer &optimizer, LogicalOperator &plan, const LogicalOperator &source) {
	Canonicalizer(optimizer, source).Visit(plan);
}

} // namespace duckdb
