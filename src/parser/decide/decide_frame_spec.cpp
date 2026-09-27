#include "duckdb/parser/decide/decide_frame_spec.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

string DecideFrameSelector::ToString() const {
	switch (kind) {
	case DecideFrameSelectorKind::FIRST:
		return "FIRST";
	case DecideFrameSelectorKind::LAST:
		return "LAST";
	case DecideFrameSelectorKind::PREVIOUS:
		return distance == 1 ? "PREVIOUS" : to_string(distance) + " PREVIOUS";
	case DecideFrameSelectorKind::NEXT:
		return distance == 1 ? "NEXT" : to_string(distance) + " NEXT";
	}
	return "PREVIOUS";
}

static string EncodeSelector(const DecideFrameSelector &selector) {
	return to_string(static_cast<int>(selector.kind)) + "/" + to_string(selector.distance);
}

static DecideFrameSelector DecodeSelector(const string &text) {
	auto parts = StringUtil::Split(text, "/");
	if (parts.size() != 2) {
		throw InternalException("DECIDE frame spec: malformed selector '%s'", text);
	}
	DecideFrameSelector result;
	result.kind = static_cast<DecideFrameSelectorKind>(std::stoi(parts[0]));
	result.distance = static_cast<idx_t>(std::stoull(parts[1]));
	return result;
}

string DecideFrameSpec::Encode() const {
	// kind;agg;from;to;every;policy;dir;cyclic;within
	vector<string> parts;
	parts.push_back(is_range ? "range" : "at");
	parts.push_back(aggregate);
	parts.push_back(EncodeSelector(from_selector));
	parts.push_back(EncodeSelector(to_selector));
	parts.push_back(to_string(every));
	parts.push_back(to_string(static_cast<int>(policy)));
	parts.push_back(descending ? "desc" : "asc");
	parts.push_back(cyclic ? "cyclic" : "linear");
	parts.push_back(has_within ? "within" : "nowithin");
	return StringUtil::Join(parts, ";");
}

DecideFrameSpec DecideFrameSpec::Decode(const string &encoded) {
	// Split by hand: an `AT` frame has an empty aggregate field, which
	// StringUtil::Split would drop.
	vector<string> parts;
	string current;
	for (auto ch : encoded) {
		if (ch == ';') {
			parts.push_back(current);
			current.clear();
		} else {
			current += ch;
		}
	}
	parts.push_back(current);
	if (parts.size() != 9) {
		throw InternalException("DECIDE frame spec: malformed '%s'", encoded);
	}
	DecideFrameSpec spec;
	spec.is_range = parts[0] == "range";
	spec.aggregate = parts[1];
	spec.from_selector = DecodeSelector(parts[2]);
	spec.to_selector = DecodeSelector(parts[3]);
	spec.every = static_cast<idx_t>(std::stoull(parts[4]));
	spec.policy = static_cast<DecideFramePolicy>(std::stoi(parts[5]));
	spec.descending = parts[6] == "desc";
	spec.cyclic = parts[7] == "cyclic";
	spec.has_within = parts[8] == "within";
	return spec;
}

bool DecideFrameSpec::operator==(const DecideFrameSpec &other) const {
	return is_range == other.is_range && aggregate == other.aggregate && from_selector == other.from_selector &&
	       to_selector == other.to_selector && every == other.every && policy == other.policy &&
	       descending == other.descending && cyclic == other.cyclic && has_within == other.has_within;
}

} // namespace duckdb
