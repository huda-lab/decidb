//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/common/decide_source_info.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include <map>

namespace duckdb {

class Serializer;
class Deserializer;

//! What a span of the DECIDE tail is. The values are the parser's PGDecideSpanRole.
enum class DecideSpanRole : uint8_t { CMP_LHS = 0, CMP_RHS = 1, WHEN = 2, PER = 3, BY = 4, COLUMN = 5 };

//! The text of a DECIDE clause as the user wrote it (the Clang SourceManager / `CreateInfo::sql` pattern): the tail,
//! `SUCH THAT` through the objective, and where the parser found each construct in it. A construct is named by the
//! location of its own token, which the transformer also gives the parsed node as its query location, so a binder
//! cuts the text of a node it holds without the node carrying any. Not compared by `SelectNode::Equals`, as the query
//! location is not.
struct DecideSourceText {
	//! The tail, verbatim. Empty for a statement built without the parser's text.
	string tail;
	//! Where the tail starts in the string that was parsed; a location is relative to it.
	idx_t tail_start = 0;
	//! Four entries per construct: the anchor, its role, and its start and end, all relative to `tail_start`.
	vector<idx_t> spans;

	bool HasText() const {
		return !tail.empty();
	}
	//! The text written for the construct of `role` whose token is at `location` (a query location); false when the
	//! parser kept none.
	bool Cut(optional_idx location, DecideSpanRole role, string &text) const;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideSourceText Deserialize(Deserializer &deserializer);

private:
	//! (anchor, role) to the construct's entry in `spans`, built on first use.
	mutable std::map<std::pair<idx_t, idx_t>, idx_t> index;
	mutable bool indexed = false;
};

//! How diagnostics should render the right-hand side of a source constraint.
enum class ConstraintSourceRhsKind : uint8_t {
	//! No directly editable symbolic RHS. Diagnostics quote its evaluated value.
	NUMERIC_FALLBACK = 0,
	//! A user-visible data expression. Diagnostics render a symbolic offset.
	DATA_EXPRESSION = 1
};

//! Stable, solver-neutral display provenance for one canonical source comparison.
//! The vector that owns these entries is indexed by source_clause_id.
struct ConstraintSourceInfo {
	//! Canonical user-facing pieces after canonicalization.
	string canonical_lhs;
	string canonical_rhs;
	//! The comparison operator after canonicalization. Stored beside the two sides
	//! because canonicalization may flip it when it moves a term across the relation,
	//! and a plan that shows the canonical layer has to show the flipped operator too.
	string canonical_cmp;
	string qualifier;
	ConstraintSourceRhsKind rhs_kind = ConstraintSourceRhsKind::NUMERIC_FALLBACK;
	//! The clause as WRITTEN, captured before canonicalization moved anything.
	//!
	//! Canonicalization is free to move a decision-bearing term across the
	//! comparison, and usually there is nothing to see: `SUM(x) >= 100` is already
	//! canonical, so re-rendering the canonical tree reproduces what the user typed.
	//! But a bound that CONTAINS a decision genuinely has to move --
	//! `ship <= capacity * open` becomes `ship - capacity * open <= 0` -- and
	//! re-rendering then shows the user algebra they never wrote, against a clause
	//! they cannot find in their own query.
	//!
	//! These two fields keep the written spelling so a diagnosis can quote it. The
	//! offset a repair computes is valid against EITHER form: moving a term across
	//! the comparison does not change what adding a constant to the bound means, so
	//! `<canonical_rhs> + d` and `<source_rhs> + d` relax by the same amount.
	//! Empty when the source and canonical forms agree, which is the common case.
	string source_lhs;
	string source_rhs;

	//! The clause exactly as WRITTEN, recorded unconditionally for plan display.
	//!
	//! `source_lhs`/`source_rhs` above answer a narrower question -- "should a repair
	//! quote the written form instead of the canonical one?" -- so they are populated
	//! only for the one rewrite a user cannot recognize, and cleared again when the
	//! two forms agree. EXPLAIN needs the opposite: the written spelling for EVERY
	//! clause, so a plan can show what became of it. Rendered once in
	//! `InitializeConstraintSourceInfo` and assigned to both, since it is one
	//! spelling serving two readers rather than two concepts.
	string written_lhs;
	string written_rhs;
	//! Whether canonicalization changed the clause, so a plan shows the canonical layer under the written one. The
	//! written text is what the user typed (`SUM(x)`, `x::INT`), which DeciDB's rendering of the canonical clause
	//! spells differently, so the two cannot be compared as strings: FinalizeConstraintSourceInfo compares the
	//! renderings of the clause before and after canonicalization instead.
	bool canonical_differs = false;
	//! DeciDB's rendering of the two sides before canonicalization: scratch between InitializeConstraintSourceInfo and
	//! FinalizeConstraintSourceInfo, empty after it and not serialized.
	string rendered_lhs;
	string rendered_rhs;
	//! The comparison operator as written (`<=`, `>=`, `=`, `<>`). Kept beside the
	//! two sides because canonicalization may flip it along with them.
	string written_cmp;
	//! The other half of a BETWEEN (the Composite pattern: one written clause, two comparisons). The binder splits
	//! `x BETWEEN lo AND hi` into `x >= lo` and `x <= hi`. Each half keeps its own source id, because the canonical
	//! text, the bound kind and the source spelling above are written once per id and the halves differ in them.
	//! Both halves link to each other, `between_upper` tells them apart, and their `written_*` hold the whole clause
	//! (`x`, `BETWEEN`, `lo AND hi`), so a message that quotes either quotes what the user wrote. INVALID_INDEX for a
	//! clause that is not a BETWEEN half.
	idx_t between_partner = DConstants::INVALID_INDEX;
	bool between_upper = false;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static ConstraintSourceInfo Deserialize(Deserializer &deserializer);
};

//! The clause as the user wrote it (its canonical form where nothing was moved), `when c per K:` qualifier in front.
//! Every message that quotes a clause goes through this or QuoteEditedClause, so none can quote one without its
//! qualifier (Information Expert: the source holds the qualifier).
string QuoteWrittenClause(const ConstraintSourceInfo &source);

//! A clause DeciDB edited: `lhs cmp rhs` with the clause's qualifier in front. A null source (a row with no clause)
//! quotes bare.
string QuoteEditedClause(const ConstraintSourceInfo *source, const string &lhs, const string &cmp, const string &rhs);

} // namespace duckdb
