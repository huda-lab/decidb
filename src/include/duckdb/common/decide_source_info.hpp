//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/common/decide_source_info.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/unordered_map.hpp"
#include <map>

namespace duckdb {

class Serializer;
class Deserializer;

//! What a span of the DECIDE text is. The values are the parser's PGDecideSpanRole.
enum class DecideSpanRole : uint8_t {
	//! A `per` key, anchored at the `per` (a reducer's own, at the location the reducer binds at); a reducer's `by`
	//! key, anchored at the `by`.
	PER = 0,
	BY = 1,
	//! The objective, anchored at the start of the tail (`tail_start`).
	OBJECTIVE = 2,
	//! An expression of the tail, anchored at the query location its parsed expression took.
	EXPR = 3
};

//! How tightly the text of an EXPR span binds, which decides whether it needs parentheses as an operand: the level of
//! its outermost operator as the user wrote it (the parsed node's, not the bound node's, which a macro, a fold or an
//! `if` that binds as a CASE can differ from). A span quoted under an operator that binds tighter is parenthesized.
enum class DecideBinding : uint8_t {
	//! A call, a column, a literal, a cast, a CASE (it ends in END), a parenthesized or subquery expression: whole in
	//! itself.
	WHOLE = 0,
	//! A comparison, AND, OR, NOT, IS, IN, LIKE or BETWEEN: looser than every arithmetic operator.
	LOOSE = 5,
	ADDITIVE = 10,
	MULTIPLICATIVE = 20,
	POWER = 30
};

//! The text of a DECIDE clause as the user wrote it (the Clang SourceManager pattern): one buffer from `DECIDE`
//! through the objective, and where the parser found each construct in it. In the split clause order the buffer holds
//! the FROM and WHERE text between the declarations and `SUCH THAT`; no span lies in it and nothing cuts it. A
//! construct is named by the query location of its parsed node, which a bound node keeps (a copy and a serialization
//! carry it), so a message cuts the text of a node it holds without the node carrying any. Every expression of the
//! clause has an EXPR span; the other roles are the constructs that are no expression (a key, a BETWEEN's sides, the
//! objective with its scope). Not compared by `SelectNode::Equals`, as the query location is not.
struct DecideSourceText {
	//! The clause, verbatim, from `DECIDE` through the objective. Empty for a statement built without the parser's
	//! text.
	string text;
	//! Where the tail (`SUCH THAT` through the objective) starts in the string that was parsed. It anchors the
	//! objective's span, and `Tail()` starts there.
	idx_t tail_start = 0;
	//! Five entries per construct: the anchor, its role, its start and end (all relative to `start`), and, for an
	//! EXPR span, its DecideBinding (0 for any other role).
	vector<idx_t> spans;
	//! Where `text` starts in the string that was parsed (the `DECIDE`); a location is relative to it.
	idx_t start = 0;

	bool HasText() const {
		return !text.empty();
	}
	//! The text from `SUCH THAT` through the objective.
	string Tail() const {
		return text.substr(tail_start - start);
	}
	//! The serialized fields; the serializer compares a member with its default to leave a default out.
	bool operator==(const DecideSourceText &other) const {
		return text == other.text && tail_start == other.tail_start && spans == other.spans && start == other.start;
	}
	//! The text written for the construct of `role` whose token is at `location` (a query location); false when the
	//! parser kept none.
	bool Cut(optional_idx location, DecideSpanRole role, string &out) const;
	//! The text of the expression at `location` and how tightly it binds (a DecideBinding).
	bool CutExpr(optional_idx location, string &out, int &binding) const;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideSourceText Deserialize(Deserializer &deserializer);

private:
	bool CutEntry(optional_idx location, DecideSpanRole role, string &out, int &binding) const;
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

//! One clause of the DECIDE tail, indexed by source_clause_id: where the user wrote it and what canonicalization made
//! of it. The clause's text is not kept: it is cut from the query by its span (`DecideSourceText`), as a compiler cuts
//! a diagnostic's source line, and a clause's location is the one a later stage points an error at.
struct ConstraintSourceInfo {
	//! The clause's span: the query location of its comparison (an IN's own; a BETWEEN's, which its two halves share),
	//! as `Expression::query_location`. `DecideSourceText::CutExpr` cuts the clause as written from it. Invalid for a
	//! clause the parser gave no location.
	optional_idx location;
	//! The span of the canonical left side: where the user wrote the term canonicalization left there, or respelled
	//! (`x * w` canonicalizes to `w * x` and keeps the span of `x * w`), so a diagnosis edits text the user can find.
	//! Invalid for a side canonicalization built.
	optional_idx lhs_location;
	//! The clause after canonicalization, as a plan shows the canonical layer: every product in the one spelling
	//! (`w * x`) and the decision structure reprinted. The right side is data, so a located node is cut as written.
	string canonical_lhs;
	string canonical_rhs;
	//! The comparison operator after canonicalization, which may flip when a term moves across the relation.
	string canonical_cmp;
	string qualifier;
	ConstraintSourceRhsKind rhs_kind = ConstraintSourceRhsKind::NUMERIC_FALLBACK;
	//! Whether canonicalization changed the clause, as the canonicalizer reports it (DecideCanonicalizer::
	//! CanonicalizeTree), so a plan shows the canonical layer under the written one.
	bool canonical_differs = false;
	//! The two sides of a clause whose sides BOTH read a decision (`ship <= capacity * open`), as DeciDB renders them
	//! before canonicalization; empty for any other clause. Canonicalization has to merge such sides
	//! (`ship - capacity * open <= 0`), a clause the query does not contain, against a bound that is not in it either,
	//! so a diagnosis quotes these. They are DeciDB's rendering, not the query text: an edit pastes back as SQL meaning
	//! the same clause, so `sum(x) by ()` quotes as `sum(x)`.
	string source_lhs;
	string source_rhs;
	//! Whether `source_rhs` reads data. What canonicalization leaves of such a bound can be a literal
	//! (`ship - capacity * open <= 0`), and the repair tier follows the bound the user wrote, not that literal.
	bool source_rhs_reads_data = false;
	//! The other half of a BETWEEN (the Composite pattern: one written clause, two comparisons). The binder splits
	//! `x BETWEEN lo AND hi` into `x >= lo` and `x <= hi`. Each half keeps its own source id, because the canonical
	//! text and the bound kind are written once per id and the halves differ in them. Both halves link to each other
	//! and share the BETWEEN's `location`, so a message that quotes either quotes what the user wrote;
	//! `between_upper` tells them apart. INVALID_INDEX for a clause that is not a BETWEEN half.
	idx_t between_partner = DConstants::INVALID_INDEX;
	bool between_upper = false;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static ConstraintSourceInfo Deserialize(Deserializer &deserializer);
};

//! The clause the user wrote that `source_clause_id` is a part of, named by its first source id: a BETWEEN is one
//! clause of two halves, and its lower half names it. Any other clause names itself. `source_clause_id` is one a clause
//! owns.
idx_t WrittenClauseId(const vector<ConstraintSourceInfo> &sources, idx_t source_clause_id);

//! The clause `source_clause_id` as the user wrote it, `when c per K:` qualifier in front. Every message that quotes a
//! clause goes through this or QuoteEditedClause, so none can quote one without its qualifier (Information Expert: the
//! source holds the qualifier). A clause the query text holds no span for (a statement built without it) quotes its
//! canonical rendering, a BETWEEN its two halves' as one clause.
string QuoteWrittenClause(const vector<ConstraintSourceInfo> &sources, idx_t source_clause_id,
                          const DecideSourceText &text);

//! A clause DeciDB edited: `lhs cmp rhs` with the clause's qualifier in front. A null source (a row with no clause)
//! quotes bare.
string QuoteEditedClause(const ConstraintSourceInfo *source, const string &lhs, const string &cmp, const string &rhs);

//! The left side of a clause after canonicalization, as a message quotes it: as written where canonicalization left or
//! respelled it, as it spells it where it built it.
string QuoteCanonicalLhs(const ConstraintSourceInfo &source, const DecideSourceText &text);

//! The left side of a clause as an edit quotes it: QuoteCanonicalLhs, except that a clause whose sides canonicalization
//! merged quotes the left side it was written with (`source_lhs`).
string QuoteClauseLhs(const ConstraintSourceInfo &source, const DecideSourceText &text);

//! Where a later stage points an error about the clause `source_clause_id`: its span, invalid for an id no clause owns
//! (the objective's, a row no clause claims) or a clause with no location.
optional_idx DecideClauseLocation(const vector<ConstraintSourceInfo> &sources, idx_t source_clause_id);

//! Where an error about the expression at `location` points: `location` when the query text holds a span for it, and
//! invalid otherwise (a statement built without the text, a clause a table macro's tail expanded, whose locations are
//! relative to the macro body), so no caret lands on text the user did not write there.
optional_idx DecideSpanLocation(const DecideSourceText &text, optional_idx location);

//! Where an error about the objective points: the start of the objective's text. Invalid for a statement built without
//! the query text.
optional_idx DecideObjectiveLocation(const DecideSourceText &text);

//! Where a SUCH THAT clause, or the objective, stands in the query: what a message about a number the model computes
//! across the clause's rows names (TOO_LARGE), and where its error points.
struct DecideClauseRef {
	//! The objective, not a clause.
	bool objective = false;
	//! The clause's span, or the start of the objective's text; invalid where the query text holds none (a clause a
	//! table macro's tail expanded, a statement built without the text).
	optional_idx place;

	//! What a TOO_LARGE message calls it.
	const char *Noun() const {
		return objective ? "objective" : "constraint";
	}
};

//! Where a row of the model came from: the three ids every kind of row carries, and the objective's mark. A Value
//! Object: a row holds it by composition (`row.origin.source_clause_id`) and copies it whole, as LLVM copies a
//! `DebugLoc`, so a site that copies a row's origin cannot drop one of the ids.
struct DecideRowOrigin {
	//! The user's clause, an index into `constraint_sources`; stable display provenance.
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	//! Atomic DROP identity: every row and native construct sharing it is omitted together.
	idx_t removal_group_id = DConstants::INVALID_INDEX;
	//! Elastic grouping identity, kept apart because one source comparison can emit several independently repairable
	//! rows or directions.
	idx_t repair_group_id = DConstants::INVALID_INDEX;
	//! A row a rewrite made for the objective (an ABS envelope, a product's corner rows, an L0 link): no clause owns
	//! it, so it has no source clause, and EXPLAIN prints it under the objective.
	bool objective = false;

	//! The origin of a row made of the clause `source_clause_id` alone.
	static DecideRowOrigin OfClause(idx_t source_clause_id) {
		DecideRowOrigin origin;
		origin.source_clause_id = source_clause_id;
		return origin;
	}
	//! The origin of a row a rewrite makes for the objective.
	static DecideRowOrigin OfObjective() {
		DecideRowOrigin origin;
		origin.objective = true;
		return origin;
	}
};

//! The clause `source_clause_id` as a message refers to it: its span, or for an id no clause owns (the objective's, a
//! row no clause claims) the objective.
DecideClauseRef DecideClauseRefOf(const vector<ConstraintSourceInfo> &sources, const DecideSourceText &text,
                                  idx_t source_clause_id);

//! The extra info of an exception about the clause `source_clause_id`, so DuckDB prints the caret under its span
//! (`Exception::InitializeExtraInfo`; `ClientContext::ProcessError` adds the `LINE n:` block). Empty where the clause
//! has no span.
unordered_map<string, string> DecideClauseErrorInfo(const vector<ConstraintSourceInfo> &sources,
                                                    idx_t source_clause_id);

} // namespace duckdb
