//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/planner/decide/decide_reducer_path.cpp
//
// The path rule (language/guide.md §5.5); see decide_reducer_path.hpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/planner/decide/decide_reducer_path.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

idx_t DecideReducerGroupKey(const BoundAggregateExpression &reducer) {
	idx_t group_key = DConstants::INVALID_INDEX;
	TryParseGroupKeyTag(reducer.GetAlias(), group_key);
	return group_key;
}

bool TryGetDecideFrame(const Expression &expr, idx_t &frame) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && TryParseFrameTag(expr.GetAlias(), frame);
}

bool ContainsDecideFrame(const Expression &expr) {
	idx_t frame;
	if (TryGetDecideFrame(expr, frame)) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { found = found || ContainsDecideFrame(child); });
	return found;
}

string DecideFrameAbsenceRemedy(const DecideFrameSpec &frame) {
	// A `count` that requests no position reads 0, so only `all` makes one read nothing.
	if (frame.range && frame.reducer != "count" && frame.MayBeEmpty()) {
		return "its first end may come after its last, which requests no position";
	}
	if (frame.all) {
		return "drop all, or make its timeline cyclic";
	}
	return "give it an else, or make its timeline cyclic";
}

DecideReducerPath DecideReducerPath::Below(const BoundAggregateExpression &reducer) const {
	auto below = *this;
	if (reducer.filter) {
		below.conditions.push_back(reducer.filter.get());
	}
	below.keys.push_back(DecideReducerGroupKey(reducer));
	return below;
}

unique_ptr<Expression> DecideReducerPath::Rows() const {
	if (conditions.empty()) {
		return nullptr;
	}
	if (conditions.size() == 1) {
		return conditions[0]->Copy();
	}
	auto all = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	for (auto *condition : conditions) {
		all->children.push_back(condition->Copy());
	}
	return std::move(all);
}

vector<idx_t> DecideReducerPath::KeysOf(idx_t own) const {
	auto result = keys;
	result.push_back(own);
	return result;
}

idx_t FindOrCreateDecidePathKey(const vector<idx_t> &path_keys, vector<DecideKey> &keys) {
	vector<DecideKeyElement> elements;
	vector<string> texts;
	for (auto key : path_keys) {
		if (key == DConstants::INVALID_INDEX) {
			continue;
		}
		for (auto &element : DecideKeyElements(keys, key)) {
			elements.push_back(element.Copy());
		}
		texts.push_back(DecideKeyText(keys, key));
	}
	return FindOrCreateDecideKey(std::move(elements), StringUtil::Join(texts, ", "), keys);
}

} // namespace duckdb
