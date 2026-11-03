#include "duckdb/parser/expression/decide_frame_expression.hpp"

#include "duckdb/common/types/hash.hpp"

namespace duckdb {

DecideFrameExpression::DecideFrameExpression()
    : ParsedExpression(ExpressionType::DECIDE_FRAME, ExpressionClass::DECIDE_FRAME) {
}

string DecideFrameExpression::PositionToString(DecideFrameAnchor anchor, int64_t k) {
	switch (anchor) {
	case DecideFrameAnchor::FIRST:
		return "first";
	case DecideFrameAnchor::LAST:
		return "last";
	case DecideFrameAnchor::PREVIOUS:
		return k == 1 ? "previous" : to_string(k) + " previous";
	case DecideFrameAnchor::NEXT:
		return k == 1 ? "next" : to_string(k) + " next";
	default:
		throw InternalException("unknown DECIDE frame anchor");
	}
}

//! As written, the defaults omitted (asc, every 1, k = 1, one timeline).
string DecideFrameExpression::ToString() const {
	string result;
	if (IsRange()) {
		result = reducer + "(from " + PositionToString(from, from_k) + " to " + PositionToString(to, to_k);
		if (every != 1) {
			result += " every " + to_string(every);
		}
	} else {
		result = "at(" + PositionToString(from, from_k);
	}
	if (fallback) {
		result += " else " + fallback->ToString();
	} else if (all) {
		result += " all";
	}
	result += ": " + payload->ToString() + ") over (" + order->ToString();
	if (descending) {
		result += " desc";
	}
	if (cyclic) {
		result += " cyclic";
	}
	for (idx_t i = 0; i < within.size(); i++) {
		result += (i == 0 ? " within " : ", ") + within[i]->ToString();
	}
	return result + ")";
}

bool DecideFrameExpression::Equal(const DecideFrameExpression &a, const DecideFrameExpression &b) {
	if (a.reducer != b.reducer || a.from != b.from || a.from_k != b.from_k || a.every != b.every || a.all != b.all ||
	    a.descending != b.descending || a.cyclic != b.cyclic) {
		return false;
	}
	if (a.IsRange() && (a.to != b.to || a.to_k != b.to_k)) {
		return false;
	}
	if (!ParsedExpression::Equals(a.payload, b.payload) || !ParsedExpression::Equals(a.fallback, b.fallback) ||
	    !ParsedExpression::Equals(a.order, b.order)) {
		return false;
	}
	return ParsedExpression::ListEquals(a.within, b.within);
}

hash_t DecideFrameExpression::Hash() const {
	hash_t result = ParsedExpression::Hash();
	result = CombineHash(result, duckdb::Hash(reducer.c_str()));
	result = CombineHash(result, duckdb::Hash<uint8_t>(static_cast<uint8_t>(from)));
	result = CombineHash(result, duckdb::Hash<int64_t>(from_k));
	if (IsRange()) {
		// A point frame has no last end; Equal reads none either.
		result = CombineHash(result, duckdb::Hash<uint8_t>(static_cast<uint8_t>(to)));
		result = CombineHash(result, duckdb::Hash<int64_t>(to_k));
	}
	result = CombineHash(result, duckdb::Hash<int64_t>(every));
	result = CombineHash(result, duckdb::Hash<bool>(all));
	result = CombineHash(result, duckdb::Hash<bool>(descending));
	return CombineHash(result, duckdb::Hash<bool>(cyclic));
}

unique_ptr<ParsedExpression> DecideFrameExpression::Copy() const {
	auto copy = make_uniq<DecideFrameExpression>();
	copy->CopyProperties(*this);
	copy->reducer = reducer;
	copy->from = from;
	copy->from_k = from_k;
	copy->to = to;
	copy->to_k = to_k;
	copy->every = every;
	copy->all = all;
	copy->payload = payload->Copy();
	copy->fallback = fallback ? fallback->Copy() : nullptr;
	copy->order = order->Copy();
	copy->descending = descending;
	copy->cyclic = cyclic;
	for (auto &key : within) {
		copy->within.push_back(key->Copy());
	}
	return std::move(copy);
}

} // namespace duckdb
