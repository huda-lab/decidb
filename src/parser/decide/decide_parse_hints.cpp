#include "duckdb/parser/decide/decide_parse_hints.hpp"

#include <array>
#include <cctype>
#include <string>

namespace duckdb {

namespace {

// Case-insensitive substring test (ASCII; sufficient for keyword sniffing).
bool ContainsCI(const std::string &haystack, const std::string &needle) {
	if (needle.empty()) {
		return true;
	}
	if (haystack.size() < needle.size()) {
		return false;
	}
	auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
	for (size_t i = 0; i + needle.size() <= haystack.size(); i++) {
		size_t j = 0;
		for (; j < needle.size(); j++) {
			if (lower(static_cast<unsigned char>(haystack[i + j])) != lower(static_cast<unsigned char>(needle[j]))) {
				break;
			}
		}
		if (j == needle.size()) {
			return true;
		}
	}
	return false;
}

} // namespace

std::string MaybeAppendDecideWhenHint(const std::string &query, const std::string &error_message) {
	// Only touch genuine parser syntax errors.
	if (error_message.find("syntax error") == std::string::npos) {
		return error_message;
	}
	// Only DECIDE queries that actually use WHEN can hit the restricted condition grammar.
	if (!ContainsCI(query, "decide") || !ContainsCI(query, "when")) {
		return error_message;
	}
	// Objective WHEN accepts one atomic comparison. Constraint-local WHEN stays
	// narrow so it cannot steal the constraint bound; both paths still reject
	// unparenthesized NOT, arithmetic, and more complex shapes. Bison reports
	// those failures at one of these tokens. Gate on the token so we do not tack
	// the WHEN hint onto unrelated DECIDE syntax errors.
	static const std::array<const char *, 9> kBreakTokens = {
	    "\"NOT\"", "\"<=\"", "\">=\"", "\"<>\"", "\"<\"", "\">\"", "\"=\"", "\"+\"", "\"-\""};
	bool near_break_token = false;
	for (auto *tok : kBreakTokens) {
		if (error_message.find(tok) != std::string::npos) {
			near_break_token = true;
			break;
		}
	}
	if (!near_break_token) {
		return error_message;
	}
	return error_message +
	       "\nHint: wrap the WHEN condition in parentheses — e.g. WHEN (a = b), WHEN (NOT flag), or WHEN (a + b > 5).";
}

} // namespace duckdb
