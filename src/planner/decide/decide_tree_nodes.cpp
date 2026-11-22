//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/planner/decide/decide_tree_nodes.cpp
//
// The facts of a reducer and of a computed value read whole; see decide_tree_nodes.hpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/planner/decide/decide_tree_nodes.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

static void SetTag(Expression &expr, const string &tag, bool present) {
	auto alias = expr.GetAlias();
	if (HasDecideTag(alias, tag) == present) {
		return;
	}
	if (present) {
		AddDecideTag(alias, tag);
	} else {
		RemoveDecideTag(alias, tag);
	}
	expr.SetAlias(std::move(alias));
}

//! Replaces the tag "<prefix><payload>__" of `expr`, whatever its payload, by one holding `payload`; an empty
//! `payload` leaves none.
static void SetPayloadTag(BaseExpression &expr, const string &prefix, const string &payload) {
	auto alias = expr.GetAlias();
	string old;
	if (ExtractDecideTagPayload(alias, prefix, old)) {
		if (old == payload) {
			return;
		}
		RemoveDecideTag(alias, prefix + old + "__");
	}
	if (!payload.empty()) {
		AddDecideTag(alias, prefix + payload + "__");
	}
	expr.SetAlias(std::move(alias));
}

static void SetIndexTag(Expression &expr, const string &prefix, idx_t index) {
	SetPayloadTag(expr, prefix, index == DConstants::INVALID_INDEX ? string() : to_string(index));
}

//===--------------------------------------------------------------------===//
// Norm
//===--------------------------------------------------------------------===//
static string NormPayload(const DecideNorm &norm) {
	switch (norm.order) {
	case DecideNorm::Order::NONE:
		return string();
	case DecideNorm::Order::L1:
		return "1";
	case DecideNorm::Order::L2:
		return "2";
	case DecideNorm::Order::INF:
		return "inf";
	default:
		return norm.bound == 0 ? "0_auto" : "0_" + StringUtil::Format("%.17g", norm.bound);
	}
}

static DecideNorm NormOf(const string &payload) {
	DecideNorm norm;
	if (payload == "1") {
		norm.order = DecideNorm::Order::L1;
	} else if (payload == "2") {
		norm.order = DecideNorm::Order::L2;
	} else if (payload == "inf") {
		norm.order = DecideNorm::Order::INF;
	} else if (!payload.empty()) {
		norm.order = DecideNorm::Order::L0;
		norm.bound = payload == "0_auto" ? 0 : std::stod(payload.substr(2));
	}
	return norm;
}

string DecideNorm::Written() const {
	switch (order) {
	case Order::L0:
		return bound == 0 ? "0" : "0, " + StringUtil::Format("%.17g", bound);
	case Order::INF:
		return "'inf'";
	default:
		return NormPayload(*this);
	}
}

bool IsDecideNormMarker(const Expression &expr, DecideNorm &norm) {
	string payload;
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE ||
	    !ExtractDecideTagPayload(expr.GetAlias(), NORM_MARKER_TAG_PREFIX, payload)) {
		return false;
	}
	norm = NormOf(payload);
	return true;
}

bool IsDecideNormMarker(const Expression &expr) {
	DecideNorm norm;
	return IsDecideNormMarker(expr, norm);
}

void SetDecideNormMarker(BoundAggregateExpression &reducer, const DecideNorm &norm) {
	SetPayloadTag(reducer, NORM_MARKER_TAG_PREFIX, NormPayload(norm));
}

void ClearDecideNormMarker(BoundAggregateExpression &reducer) {
	SetPayloadTag(reducer, NORM_MARKER_TAG_PREFIX, string());
}

DecideNorm DecideWrittenNorm(const BoundAggregateExpression &reducer) {
	string payload;
	ExtractDecideTagPayload(reducer.GetAlias(), WRITTEN_NORM_TAG_PREFIX, payload);
	return NormOf(payload);
}

void SetDecideWrittenNorm(BoundAggregateExpression &reducer, const DecideNorm &norm) {
	SetPayloadTag(reducer, WRITTEN_NORM_TAG_PREFIX, NormPayload(norm));
}

//===--------------------------------------------------------------------===//
// A reducer
//===--------------------------------------------------------------------===//
idx_t DecideReducerGroupKey(const BoundAggregateExpression &reducer) {
	idx_t key = DConstants::INVALID_INDEX;
	TryParseDecideIndexTag(reducer.GetAlias(), GROUP_KEY_TAG_PREFIX, key);
	return key;
}

void SetDecideReducerGroupKey(BoundAggregateExpression &reducer, idx_t key) {
	SetIndexTag(reducer, GROUP_KEY_TAG_PREFIX, key);
}

bool DecideReducerPer(const BoundAggregateExpression &reducer, idx_t &keyed_scope) {
	return TryParseDecideIndexTag(reducer.GetAlias(), QUALIFIED_REDUCER_SCOPE_TAG_PREFIX, keyed_scope);
}

bool DecideReducerHasPer(const BoundAggregateExpression &reducer) {
	idx_t keyed_scope;
	return DecideReducerPer(reducer, keyed_scope);
}

void SetDecideReducerPer(BoundAggregateExpression &reducer, idx_t keyed_scope) {
	SetIndexTag(reducer, QUALIFIED_REDUCER_SCOPE_TAG_PREFIX, keyed_scope);
}

bool DecideReducerCountScale(const BoundAggregateExpression &reducer) {
	return HasDecideTag(reducer.GetAlias(), AVG_REWRITE_TAG);
}

void SetDecideReducerCountScale(BoundAggregateExpression &reducer) {
	SetTag(reducer, AVG_REWRITE_TAG, true);
}

bool DecideReducerHoldsReducer(const Expression &reducer) {
	return HasDecideTag(reducer.GetAlias(), VALUE_REWRITE_TAG);
}

void SetDecideReducerHoldsReducer(Expression &reducer, bool holds) {
	SetTag(reducer, VALUE_REWRITE_TAG, holds);
}

bool DecideRetainedExpression(const Expression &expr, idx_t &retained) {
	return TryParseDecideIndexTag(expr.GetAlias(), TREE_RETAINED_EXPRESSION_TAG_PREFIX, retained);
}

void SetDecideRetainedExpression(Expression &expr, idx_t retained) {
	SetIndexTag(expr, TREE_RETAINED_EXPRESSION_TAG_PREFIX, retained);
}

bool DecideTreeReducerPer(const BoundAggregateExpression &reducer, idx_t &keyed_scope) {
	return TryParseDecideIndexTag(reducer.GetAlias(), TREE_REDUCER_SCOPE_TAG_PREFIX, keyed_scope);
}

void SetDecideTreeReducerPer(BoundAggregateExpression &reducer, idx_t keyed_scope) {
	SetIndexTag(reducer, TREE_REDUCER_SCOPE_TAG_PREFIX, keyed_scope);
}

//! The tags without the written-name tag.
static string StructuralTags(const Expression &expr) {
	auto tags = expr.GetAlias();
	string written;
	if (ExtractDecideTagPayload(tags, WRITTEN_NAME_TAG_PREFIX, written)) {
		RemoveDecideTag(tags, string(WRITTEN_NAME_TAG_PREFIX) + written + "__");
	}
	return tags;
}

bool DecideFactsEqual(const Expression &a, const Expression &b) {
	return StructuralTags(a) == StructuralTags(b);
}

//===--------------------------------------------------------------------===//
// A reducer's NULL tests
//===--------------------------------------------------------------------===//
unique_ptr<Expression> MakeDecideNullTest(unique_ptr<Expression> value) {
	auto test = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
	test->children.push_back(std::move(value));
	test->SetAlias(SKIP_NULL_TAG);
	return std::move(test);
}

bool IsDecideNullTest(const Expression &condition) {
	return HasDecideTag(condition.GetAlias(), SKIP_NULL_TAG);
}

//===--------------------------------------------------------------------===//
// The name a call was written under
//===--------------------------------------------------------------------===//
void SetDecideWrittenName(BaseExpression &call, const string &name) {
	SetPayloadTag(call, WRITTEN_NAME_TAG_PREFIX, name);
}

void KeepDecideWrittenName(BaseExpression &to, const BaseExpression &from) {
	string name;
	if (ExtractDecideTagPayload(from.GetAlias(), WRITTEN_NAME_TAG_PREFIX, name) &&
	    !HasDecideTag(to.GetAlias(), WRITTEN_NAME_TAG_PREFIX)) {
		SetDecideWrittenName(to, name);
	}
}

static string WrittenName(const Expression &call, const string &name) {
	string written;
	return ExtractDecideTagPayload(call.GetAlias(), WRITTEN_NAME_TAG_PREFIX, written) ? written : name;
}

string DecideWrittenName(const BoundFunctionExpression &call) {
	return WrittenName(call, call.function.name);
}

string DecideWrittenName(const BoundAggregateExpression &call) {
	return WrittenName(call, call.function.name);
}

//===--------------------------------------------------------------------===//
// A computed value read whole
//===--------------------------------------------------------------------===//
static const char *ValueTag(DecideValueKind kind) {
	switch (kind) {
	case DecideValueKind::UNCORRELATED_SUBQUERY:
		return QUERY_WIDE_VALUE_TAG;
	case DecideValueKind::CORRELATED_SUBQUERY:
		return ROW_VARYING_SUBQUERY_TAG;
	default:
		return BOUND_REDUCER_TAG;
	}
}

void MarkDecideValue(Expression &value, DecideValueKind kind) {
	SetTag(value, ValueTag(kind), true);
}

bool IsDecideValue(const Expression &expr, DecideValueKind kind) {
	return HasDecideTag(expr.GetAlias(), ValueTag(kind));
}

bool IsDecideSubqueryValue(const Expression &expr) {
	return IsDecideValue(expr, DecideValueKind::UNCORRELATED_SUBQUERY) ||
	       IsDecideValue(expr, DecideValueKind::CORRELATED_SUBQUERY);
}

void MarkDecideQueryWideValue(Expression &value) {
	SetTag(value, QUERY_WIDE_VALUE_TAG, true);
}

bool IsDecideQueryWideValue(const Expression &expr) {
	return HasDecideTag(expr.GetAlias(), QUERY_WIDE_VALUE_TAG);
}

bool IsDecideQueryWideBound(const Expression &bound) {
	return HasDecideTag(bound.GetAlias(), QUERY_WIDE_BOUND_TAG);
}

void SetDecideQueryWideBound(Expression &bound, bool query_wide) {
	SetTag(bound, QUERY_WIDE_BOUND_TAG, query_wide);
}

} // namespace duckdb
