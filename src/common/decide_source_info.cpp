#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//! A clause's text with its `when c per K:` qualifier in front of it, where DECIDE writes it.
static string AttachDecideQualifier(const string &clause, const string &qualifier) {
	return qualifier.empty() ? clause : qualifier + " " + clause;
}

//! The clause as canonicalization spells it, without its qualifier: `lhs cmp rhs`.
static string CanonicalDecideComparison(const ConstraintSourceInfo &source) {
	return source.canonical_cmp.empty() || source.canonical_rhs.empty()
	           ? source.canonical_lhs
	           : source.canonical_lhs + " " + source.canonical_cmp + " " + source.canonical_rhs;
}

//! The text of the expression written at `location`; `fallback` where the query text holds none.
static string CutWrittenSide(optional_idx location, const DecideSourceText &text, const string &fallback) {
	string cut;
	int ignored;
	return text.CutExpr(location, cut, ignored) ? cut : fallback;
}

static constexpr idx_t SPAN_ENTRY_SIZE = 5;

bool DecideSourceText::Cut(optional_idx location, DecideSpanRole role, string &out) const {
	int ignored;
	return CutEntry(location, role, out, ignored);
}

bool DecideSourceText::CutExpr(optional_idx location, string &out, int &binding) const {
	return CutEntry(location, DecideSpanRole::EXPR, out, binding);
}

bool DecideSourceText::CutEntry(optional_idx location, DecideSpanRole role, string &out, int &binding) const {
	if (!location.IsValid() || location.GetIndex() < start) {
		return false;
	}
	if (!indexed) {
		for (idx_t i = 0; i + SPAN_ENTRY_SIZE <= spans.size(); i += SPAN_ENTRY_SIZE) {
			index.emplace(std::make_pair(spans[i], spans[i + 1]), i);
		}
		indexed = true;
	}
	auto entry = index.find(std::make_pair(location.GetIndex() - start, static_cast<idx_t>(role)));
	if (entry == index.end()) {
		return false;
	}
	out = text.substr(spans[entry->second + 2], spans[entry->second + 3] - spans[entry->second + 2]);
	binding = NumericCast<int>(spans[entry->second + 4]);
	return true;
}

string QuoteWrittenClause(const vector<ConstraintSourceInfo> &sources, idx_t source_clause_id,
                          const DecideSourceText &text) {
	auto &source = sources[source_clause_id];
	string clause;
	int ignored;
	if (!text.CutExpr(source.location, clause, ignored)) {
		clause = CanonicalDecideComparison(source);
		// Without the text a BETWEEN reads its sides off its two halves: `x >= 1` and `x <= 3` are `x BETWEEN 1 AND 3`.
		if (source.between_partner != DConstants::INVALID_INDEX) {
			auto &lower = source.between_upper ? sources[source.between_partner] : source;
			auto &upper = source.between_upper ? source : sources[source.between_partner];
			clause = lower.canonical_lhs + " BETWEEN " + lower.canonical_rhs + " AND " + upper.canonical_rhs;
		}
	}
	return AttachDecideQualifier(clause, source.qualifier);
}

string QuoteEditedClause(const ConstraintSourceInfo *source, const string &lhs, const string &cmp, const string &rhs) {
	return AttachDecideQualifier(lhs + " " + cmp + " " + rhs, source ? source->qualifier : string());
}

string QuoteCanonicalLhs(const ConstraintSourceInfo &source, const DecideSourceText &text) {
	return CutWrittenSide(source.lhs_location, text, source.canonical_lhs);
}

string QuoteClauseLhs(const ConstraintSourceInfo &source, const DecideSourceText &text) {
	return source.source_lhs.empty() ? QuoteCanonicalLhs(source, text) : source.source_lhs;
}

optional_idx DecideClauseLocation(const vector<ConstraintSourceInfo> &sources, idx_t source_clause_id) {
	return source_clause_id < sources.size() ? sources[source_clause_id].location : optional_idx();
}

unordered_map<string, string> DecideClauseErrorInfo(const vector<ConstraintSourceInfo> &sources,
                                                    idx_t source_clause_id) {
	auto location = DecideClauseLocation(sources, source_clause_id);
	return location.IsValid() ? Exception::InitializeExtraInfo(location) : unordered_map<string, string>();
}

} // namespace duckdb
