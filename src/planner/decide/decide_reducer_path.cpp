//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/planner/decide/decide_reducer_path.cpp
//
// The path rule (syntax_reference.md §5.5); see decide_reducer_path.hpp.
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
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		found = found || ContainsDecideFrame(child);
	});
	return found;
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

vector<unique_ptr<Expression>>
DecidePathKeyExpressions(const vector<idx_t> &keys, const vector<DecideGroupKey> &group_keys, vector<string> *names) {
	vector<unique_ptr<Expression>> result;
	for (auto key : keys) {
		if (key == DConstants::INVALID_INDEX || key >= group_keys.size()) {
			continue;
		}
		auto &by = group_keys[key];
		for (idx_t i = 0; i < by.expressions.size(); i++) {
			bool seen = false;
			for (auto &kept : result) {
				seen = seen || kept->Equals(*by.expressions[i]);
			}
			if (seen) {
				continue;
			}
			result.push_back(by.expressions[i]->Copy());
			if (names) {
				names->push_back(i < by.names.size() ? by.names[i] : by.expressions[i]->ToString());
			}
		}
	}
	return result;
}

idx_t FindOrCreateDecidePathKey(const vector<idx_t> &keys, vector<DecideGroupKey> &group_keys) {
	vector<string> names;
	auto expressions = DecidePathKeyExpressions(keys, group_keys, &names);
	if (expressions.empty()) {
		return DConstants::INVALID_INDEX;
	}
	for (idx_t i = 0; i < group_keys.size(); i++) {
		auto &existing = group_keys[i].expressions;
		bool equal = existing.size() == expressions.size();
		for (idx_t j = 0; equal && j < expressions.size(); j++) {
			bool found = false;
			for (auto &part : existing) {
				found = found || part->Equals(*expressions[j]);
			}
			equal = found;
		}
		if (equal) {
			return i;
		}
	}
	DecideGroupKey path_key;
	path_key.expressions = std::move(expressions);
	path_key.text = StringUtil::Join(names, ", ");
	path_key.names = std::move(names);
	group_keys.push_back(std::move(path_key));
	return group_keys.size() - 1;
}

} // namespace duckdb
