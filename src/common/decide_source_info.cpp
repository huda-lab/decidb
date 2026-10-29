#include "duckdb/common/decide_source_info.hpp"

namespace duckdb {

//! A clause's text with its `when c per K:` qualifier in front of it, where DECIDE writes it.
static string AttachDecideQualifier(const string &clause, const string &qualifier) {
	return qualifier.empty() ? clause : qualifier + " " + clause;
}

//! The clause as written (its canonical form where nothing was moved), without its qualifier: `lhs cmp rhs`.
static string WrittenDecideComparison(const ConstraintSourceInfo &source) {
	const string &lhs = source.written_lhs.empty() ? source.canonical_lhs : source.written_lhs;
	const string &rhs = source.written_rhs.empty() ? source.canonical_rhs : source.written_rhs;
	const string &cmp = source.written_cmp.empty() ? source.canonical_cmp : source.written_cmp;
	return cmp.empty() || rhs.empty() ? lhs : lhs + " " + cmp + " " + rhs;
}

bool DecideSourceText::Cut(optional_idx location, DecideSpanRole role, string &text) const {
	if (!location.IsValid() || location.GetIndex() < tail_start) {
		return false;
	}
	if (!indexed) {
		for (idx_t i = 0; i + 3 < spans.size(); i += 4) {
			index.emplace(std::make_pair(spans[i], spans[i + 1]), i);
		}
		indexed = true;
	}
	auto entry = index.find(std::make_pair(location.GetIndex() - tail_start, static_cast<idx_t>(role)));
	if (entry == index.end()) {
		return false;
	}
	text = tail.substr(spans[entry->second + 2], spans[entry->second + 3] - spans[entry->second + 2]);
	return true;
}

string QuoteWrittenClause(const ConstraintSourceInfo &source) {
	return AttachDecideQualifier(WrittenDecideComparison(source), source.qualifier);
}

string QuoteEditedClause(const ConstraintSourceInfo *source, const string &lhs, const string &cmp, const string &rhs) {
	return AttachDecideQualifier(lhs + " " + cmp + " " + rhs, source ? source->qualifier : string());
}

} // namespace duckdb
