#include "duckdb/planner/expression/bound_decide_frame_expression.hpp"

#include "duckdb/common/types/hash.hpp"

namespace duckdb {

BoundDecideFrameExpression::BoundDecideFrameExpression(LogicalType return_type, idx_t frame_p,
                                                       vector<unique_ptr<Expression>> children_p)
    : Expression(ExpressionType::BOUND_DECIDE_FRAME, ExpressionClass::BOUND_DECIDE_FRAME, std::move(return_type)),
      frame(frame_p), children(std::move(children_p)) {
}

string BoundDecideFrameExpression::ToString() const {
	string result = "frame" + std::to_string(frame) + "(";
	for (idx_t i = 0; i < children.size(); i++) {
		result += (i ? ", " : "") + children[i]->ToString();
	}
	return result + ")";
}

hash_t BoundDecideFrameExpression::Hash() const {
	auto hash = CombineHash(Expression::Hash(), duckdb::Hash<idx_t>(frame));
	for (auto &child : children) {
		hash = CombineHash(hash, child->Hash());
	}
	return hash;
}

bool BoundDecideFrameExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundDecideFrameExpression>();
	return frame == other.frame && Expression::ListEquals(children, other.children);
}

unique_ptr<Expression> BoundDecideFrameExpression::Copy() const {
	vector<unique_ptr<Expression>> new_children;
	for (auto &child : children) {
		new_children.push_back(child->Copy());
	}
	auto copy = make_uniq<BoundDecideFrameExpression>(return_type, frame, std::move(new_children));
	copy->CopyProperties(*this);
	return std::move(copy);
}

} // namespace duckdb
