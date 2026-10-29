//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/common/enums/decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

enum class DecideSense : uint8_t {
    MAXIMIZE = 0,
    MINIMIZE = 1,
    FEASIBILITY = 2
};

enum class DecideExpression : uint8_t {
    INVALID = 0,
    VARIABLE,
    SUM
};

//! How one DECIDE declaration maps onto solver columns.
//!   ROW    — `x(INT)`, `per row: x(INT)` one column per result row
//!   KEYED  — `per K: x(INT)`             one column per distinct value of the key
//!   GLOBAL — `per (): x(INT)`            one column for the whole query
//! The values are serialized, so a stored plan reads under any rename.
enum class DecideVarScope : uint8_t {
    ROW = 0,
    KEYED = 1,
    GLOBAL = 2
};

//! Per-variable scope assignment. Kept as one struct rather than parallel
//! arrays so the optimizer's auxiliary-variable appends cannot desync the
//! scope from its keyed scope index.
struct DecideVarScopeInfo {
    DecideVarScope scope = DecideVarScope::ROW;
    //! Index into keyed_scopes / keyed_mappings. Meaningful only when
    //! scope == KEYED; INVALID_INDEX otherwise.
    idx_t keyed_scope_idx = DConstants::INVALID_INDEX;
	//! The key written after `per`, as written (`D.depotID, S.customerID`), so a message
	//! can name the decision the way it was declared. Empty for every other spelling.
	string declared_key;

	DecideVarScopeInfo() = default;
	DecideVarScopeInfo(DecideVarScope scope_p, idx_t keyed_scope_idx_p)
	    : scope(scope_p), keyed_scope_idx(keyed_scope_idx_p) {
	}

	static DecideVarScopeInfo Row() {
        return DecideVarScopeInfo();
    }
	//! One instance per class of the key of keyed_scopes[keyed_scope_idx_p]. `declared_key_p` is the key as written
	//! for `per K: x(TYPE)`, and empty for a decision the optimizer adds.
	static DecideVarScopeInfo Keyed(idx_t keyed_scope_idx_p, string declared_key_p = string()) {
		DecideVarScopeInfo result(DecideVarScope::KEYED, keyed_scope_idx_p);
		result.declared_key = std::move(declared_key_p);
		return result;
	}
	static DecideVarScopeInfo Global() {
		return DecideVarScopeInfo(DecideVarScope::GLOBAL, DConstants::INVALID_INDEX);
	}

	bool IsKeyed() const {
		return scope == DecideVarScope::KEYED;
	}
	bool IsGlobal() const {
		return scope == DecideVarScope::GLOBAL;
	}
	//! Declared `per K: x(TYPE)` with a non-empty key.
	bool HasDeclaredKey() const {
		return !declared_key.empty();
	}

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideVarScopeInfo Deserialize(Deserializer &deserializer);
};

//! A scope as a key of LogicalDecide::keys, ordered by how many values it has: one for the whole query (the empty
//! key, INVALID_INDEX), one per class of a key, one per row (DECIDE_ROW_KEY).
static constexpr idx_t DECIDE_ROW_KEY = DConstants::INVALID_INDEX - 1;

//! The scope of a value computed from two others: their least upper bound, as DuckDB promotes the type of a binary
//! operator. A query-wide scope joins to the other, a key to itself, and two different keys to one per row (a key that
//! neither names is not looked for).
inline idx_t JoinDecideKeys(idx_t a, idx_t b) {
	if (a == DConstants::INVALID_INDEX) {
		return b;
	}
	if (b == DConstants::INVALID_INDEX) {
		return a;
	}
	return a == b ? a : DECIDE_ROW_KEY;
}

//! What a run-time scope check (LogicalDecide::scope_checks) asks of the rows.
//!   DETERMINES     — every class of a key shows one value, or one instance of a decision
//!   VALUE_IS_VALID — every row holds a value the model can read: a computed message, NULL on a valid row
//! An empty group is no check: it skips its constraint (deck p. 56).
enum class DecideScopeCheckKind : uint8_t { DETERMINES = 1, VALUE_IS_VALID = 3 };

//! How one column of a key (LogicalDecide::keys) compares NULL.
//!   NO_GROUP — a column the user wrote: rows group as SQL's `=` compares them, so a NULL
//!              value is in no group (deck p. 30)
//!   VALUE    — a column a relation element stands for (`per D`, `by (D)`): NULL is a value,
//!              so every row of D is one class whatever its NULLs
enum class DecideKeyNullMode : uint8_t { NO_GROUP = 0, VALUE = 1 };

//! Type of aggregate used in MIN/MAX objective linearization
enum class ObjectiveAggregateType : uint8_t {
    NONE = 0,   //! No MIN/MAX objective (pure SUM or no objective)
    SUM,        //! SUM aggregate
    MIN_AGG,    //! MIN aggregate (suffixed to avoid MIN/MAX macro collision)
    MAX_AGG     //! MAX aggregate
};

//! Origin/relaxability role of an emitted DECIDE matrix row.
//!   USER_PARAMETER — carries a user-editable parameter/RHS; elastic may slacken.
//!   USER_MECHANISM — helper row attached to a user clause; rigid.
//!   STRUCTURAL     — synthesized definition/linking row; rigid.
enum class ConstraintKind : uint8_t { USER_PARAMETER, USER_MECHANISM, STRUCTURAL };

//! Reduction that owns a linear term in a canonical constraint LHS. Fixed
//! (decision-free) terms may only reach the model builder through a SUM reducer;
//! a NONE fixed term there means an earlier canonical-shape invariant was broken.
enum class LinearTermReduction : uint8_t { NONE, SUM };

inline bool IsRelaxableForElastic(ConstraintKind kind) {
	return kind == ConstraintKind::USER_PARAMETER;
}

//! How a relaxable row maps to a single user-editable knob, for elastic (infeasible)
//! diagnosis. Drives whether the rows a clause emits share ONE slack or get one each.
//!   UNSET          — invalid for a relaxable user-clause row; builder sites must stamp
//!                    one of the explicit shapes below.
//!   PER_ROW_DATA   — the RHS is per-row data, or the rows are genuinely independent:
//!                    one slack per row, no single-knob edit (rolled into a conflict
//!                    summary).
//!   SHARED_SCALAR  — one query-wide RHS value fans into N rows (easy MIN/MAX
//!                    `MAX(e)<=K`, a per-row constraint with a constant or
//!                    uncorrelated-subquery RHS, a multi-instance bound):
//!                    the N rows of a (repair_group_id, group_key) block share ONE slack, so
//!                    the reported edit is the max overshoot, not the sum.
enum class ElasticShape : uint8_t { UNSET, PER_ROW_DATA, SHARED_SCALAR };

//! DECIDE encodes pipeline metadata in the `alias` of bound expressions. More than one
//! tag can apply to the same node — a relation-qualified AVG is tagged by the binder
//! (which relation it reduces over) and again by the AVG->SUM rewrite (that its
//! coefficients need the 1/N denominator) — so tags are **concatenated** and matched by
//! search, never by equality. Every tag opens and closes with `__`, which is what makes
//! concatenation unambiguous and lets a prefix tag's payload be read back below.
inline void AddDecideTag(string &alias, const string &tag) {
	alias += tag;
}

//! True when `alias` carries `tag`, whatever else it also carries.
inline bool HasDecideTag(const string &alias, const string &tag) {
	return alias.find(tag) != string::npos;
}

//! Remove every occurrence of an exact DECIDE tag while preserving ordinary aliases
//! and any other concatenated tags.
inline void RemoveDecideTag(string &alias, const string &tag) {
	idx_t pos;
	while ((pos = alias.find(tag)) != string::npos) {
		alias.erase(pos, tag.size());
	}
}

//! Remove every DECIDE tag from an alias, leaving whatever ordinary alias it was appended
//! to: "cost__source_clause_0____when_filter__" yields "cost", and an alias that was
//! nothing but tags yields "".
//!
//! Tags are only ever appended (`AddDecideTag`), so they are always a suffix, and each one
//! opens and closes with `__` without containing `__` internally. Peeling closing-delimited
//! runs off the end therefore strips exactly the tags: a user alias is only touched if it
//! itself ends in a `__`-delimited run.
//!
//! Use this at every user-facing render site. `GetName()` returns the alias whenever one is
//! set, so a tagged expression prints its internal tag instead of its SQL unless the tags
//! are stripped first.
inline string StripDecideTags(const string &alias) {
	string result = alias;
	while (result.size() > 2 && result.compare(result.size() - 2, 2, "__") == 0) {
		auto open = result.rfind("__", result.size() - 3);
		if (open == string::npos) {
			break;
		}
		result.erase(open);
	}
	return result;
}

//! Reads the payload of a prefix-shaped tag out of a possibly multi-tag alias:
//! ("__qualified_by_2____avg_rewrite__", "__qualified_by_") yields "2". The payload runs
//! from the end of the prefix to the tag's closing `__`, so payloads may contain single
//! underscores ("0_min"). Returns false when the tag is absent or has an empty payload.
inline bool ExtractDecideTagPayload(const string &alias, const string &prefix, string &payload) {
	auto start = alias.find(prefix);
	if (start == string::npos) {
		return false;
	}
	start += prefix.size();
	auto end = alias.find("__", start);
	if (end == string::npos) {
		return false;
	}
	payload = alias.substr(start, end - start);
	return !payload.empty();
}

//! The parser's markers for the removed postfix spellings, `C WHEN c` and `C PER k`: children[0] is the clause,
//! children[1] the condition or children[1..] the key. RefuseRemovedDecideSpellings refuses both (language/errors.md
//! §3); a parse tree stored before they were removed (a view) still prints its own SQL from them. A parser marker is
//! named for the spelling it marks and a bound wrapper for what it means, so no check reads one as the other.
static constexpr const char *POSTFIX_WHEN_TAG = "__when_constraint__";
static constexpr const char *POSTFIX_PER_TAG = "__per_constraint__";

//! The bound form of a `when c:` in front of a constraint or the objective: a conjunction whose child 0 is the body
//! and child 1 the condition. The body reads only the rows where the condition is true.
static constexpr const char *WHEN_FILTER_TAG = "__when_filter__";

//! The bound form of the two-level objective, `outer(per k: inner(e) by (k))`: a conjunction whose child 0 is the
//! objective `outer(inner(e))`, keyed on k (PER_KEY_TAG_PREFIX). A constraint's `per K:` is PER_CLASS_TAG.
static constexpr const char *TWO_LEVEL_OBJECTIVE_TAG = "__two_level_objective__";

inline bool IsTwoLevelObjectiveTag(const string &alias) {
	return HasDecideTag(alias, TWO_LEVEL_OBJECTIVE_TAG);
}

//! Tag used by the parser to mark the colon form, `sum(D: expr)`: children[0] is the
//! aggregate, children[1..] the relation names before the colon. The binder consumes
//! and discards it, folding the resolved relations into the tag below.
static constexpr const char *QUALIFIED_REDUCER_TAG = "__qualified_reducer__";

//! Tag prefix recording a reducer's own key, `sum(per K: e)` or `SUM(D: e)` (on
//! BoundAggregateExpression.alias). Format: "__qualified_by_<keyed_scope_idx>__" — the
//! index is into LogicalDecide::keyed_scopes, whose entry supplies the classes
//! the reducer takes one value from. The scope may have no variable declared on it.
static constexpr const char *QUALIFIED_REDUCER_SCOPE_TAG_PREFIX = "__qualified_by_";

//! On a bound reducer written in the colon form `SUM(D: e)`, which means `sum(per D: e)`
//! and shares its scope: the tag keeps only the spelling, for the echo (D38).
static constexpr const char *COLON_FORM_TAG = "__colon_form__";

//! Builds the alias tag naming the keyed scope a reducer is qualified by.
//! `text_fragment`, when given, is the fragment holding the key as this reducer wrote it
//! ("__qualified_by_<i>_<fragment>__").
inline string MakeQualifiedReducerTag(idx_t keyed_scope_idx, idx_t text_fragment = DConstants::INVALID_INDEX) {
	return string(QUALIFIED_REDUCER_SCOPE_TAG_PREFIX) + to_string(keyed_scope_idx) +
	       (text_fragment == DConstants::INVALID_INDEX ? string() : "_" + to_string(text_fragment)) + "__";
}

//! Reads back the keyed scope index from an alias written by MakeQualifiedReducerTag.
//! Returns false (leaving `scope_idx` untouched) when the alias carries no such tag.
inline bool TryParseQualifiedReducerTag(const string &alias, idx_t &scope_idx) {
	string digits;
	if (!ExtractDecideTagPayload(alias, QUALIFIED_REDUCER_SCOPE_TAG_PREFIX, digits)) {
		return false;
	}
	digits = digits.substr(0, digits.find('_'));
	if (digits.empty() || digits.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	scope_idx = static_cast<idx_t>(std::stoull(digits));
	return true;
}

//! The fragment holding the key as a qualified reducer wrote it, when its tag carries one.
inline bool TryParseQualifiedReducerTextTag(const string &alias, idx_t &text_fragment) {
	string payload;
	if (!ExtractDecideTagPayload(alias, QUALIFIED_REDUCER_SCOPE_TAG_PREFIX, payload) ||
	    payload.find('_') == string::npos) {
		return false;
	}
	text_fragment = static_cast<idx_t>(std::stoull(payload.substr(payload.find('_') + 1)));
	return true;
}

//! Tags the parser uses for the scope written in front of a constraint, an objective, a
//! declared variable or a reducer's argument: `[when condition] [per key]: body`.
//! children[0] is the body. SCOPE_WHEN_TAG holds the condition in children[1];
//! SCOPE_PER_TAG holds the key in children[1..], none for `per ()`. With both,
//! SCOPE_PER_TAG is the outer tag. `per row`, the default written out, is
//! SCOPE_PER_ROW_TAG over the body alone: it names no key (D19).
static constexpr const char *SCOPE_WHEN_TAG = "__scope_when__";
static constexpr const char *SCOPE_PER_TAG = "__scope_per__";
static constexpr const char *SCOPE_PER_ROW_TAG = "__scope_per_row__";

//! Tag the parser uses for a reducer's `BY (key)`: children[0] is the reducer call,
//! children[1..] the key, none for `BY ()`.
static constexpr const char *REDUCER_BY_TAG = "__reducer_by__";

//! The bound form of a constraint's `per key:`: a conjunction whose child 0 is the
//! constraint and whose children[1..] are the key's columns, none for `per ()`. The
//! constraint is generated once per class of the key among the kept rows; it groups no
//! reducer, and NULL is a class.
static constexpr const char *PER_CLASS_TAG = "__per_class__";

//! Tag on a scoped comparison in the VALUE shape (CanonicalConstraintClass::VALUE): the
//! canonicalizer stamps it once, and DecideOptimizer::RewriteScopedReducers replaces each
//! decision reducer under it by a helper decision (LogicalDecide::reducer_value_links),
//! after which the comparison is one row per class like any other.
static constexpr const char *VALUE_REWRITE_TAG = "__value_rewrite__";

//! Tag prefix naming a `BY (key)`: "__group_key_<i>__" indexes LogicalDecide::keys.
//! On a reducer, the rows it reads at a class are the kept rows sharing the class's key
//! value; on a scoped aggregate comparison, the key its decision reducers share, which
//! is the comparison's grouping. No tag means `BY ()`: every kept row.
static constexpr const char *GROUP_KEY_TAG_PREFIX = "__group_key_";

//! `text_fragment`, when given, is the fragment holding this reducer's own `by` text as the user wrote it
//! ("__group_key_<i>_<fragment>__"): two reducers over one key keep their own spelling of it.
inline string MakeGroupKeyTag(idx_t group_key_idx, idx_t text_fragment = DConstants::INVALID_INDEX) {
	return string(GROUP_KEY_TAG_PREFIX) + to_string(group_key_idx) +
	       (text_fragment == DConstants::INVALID_INDEX ? string() : "_" + to_string(text_fragment)) + "__";
}

//! Tag prefix naming the key of a `per K:` wrapper (PER_CLASS_TAG, TWO_LEVEL_OBJECTIVE_TAG):
//! "__per_key_<i>__" indexes LogicalDecide::keys. No tag means `per ()`: one class.
static constexpr const char *PER_KEY_TAG_PREFIX = "__per_key_";

inline string MakePerKeyTag(idx_t key_idx) {
	return string(PER_KEY_TAG_PREFIX) + to_string(key_idx) + "__";
}

//! Tag used to identify AVG→SUM rewritten aggregates (terms need coefficient scaling at execution)
static constexpr const char *AVG_REWRITE_TAG = "__avg_rewrite__";

//! Tag prefix marking a SUM(...) that really means a hard MIN/MAX (on
//! BoundAggregateExpression.alias).
//! Format: "__minmax_clause_<clause_idx>_<min|max>__", on both arms.
//! The clause index points into `LogicalDecide::minmax_clause_labels`, which holds the
//! text a diagnosis renders for this clause. Both parts are always present: the clause
//! is registered where it is tagged, before either formulation is chosen, so the index
//! does not depend on whether a Big-M disjunction or a native extremum follows.
static constexpr const char *MINMAX_CLAUSE_TAG_PREFIX = "__minmax_clause_";

//! Tag prefix linking a `<>` comparison to its clause (on BoundComparisonExpression.alias).
//! Format: "__ne_clause_tag_<clause_idx>__", indexing `LogicalDecide::ne_clause_sources`.
static constexpr const char *NE_CLAUSE_TAG_PREFIX = "__ne_clause_tag_";

//! Tag marking a comparison whose LHS is an easy-direction MIN/MAX reducer
//! (MAX(e) <= K, MIN(e) >= K). The reducer stays in the tree: the linear form expands it
//! to one row per member, the bound reduced over each `by` group, and bound absorption
//! folds a bare one into the column box (D41). Set on the
//! BoundComparisonExpression.alias during RewriteMinMax.
static constexpr const char *MINMAX_EASY_REWRITE_TAG = "__minmax_easy__";

//! Tag marking a reducer whose members DecideOptimizer::DefineNonlinearReducerBodies replaced by helper decisions. A
//! helper has no box, so stage 08 states a MIN/MAX over it natively, and the MIN/MAX rewrites say so in the plan.
//! Set on the BoundAggregateExpression.alias, and on the BoundComparisonExpression.alias of the row defining the
//! helper, which EXPLAIN leaves out: it restates the body, and the clause's account line says how it is held.
static constexpr const char *BODY_DEFINITION_TAG = "__body_definition__";

//! Tag marking optimizer-generated helper constraints that define auxiliaries or
//! link rewrite machinery. These rows are rigid and must not be elastic-relaxed.
static constexpr const char *STRUCTURAL_CONSTRAINT_TAG = "__decide_structural_constraint__";

//! Tag marking a row a rewrite added for the objective (an ABS envelope, a product's
//! corner rows, an L0 link), which no clause owns: the rewrites start the objective's
//! walk with it as the source alias, and EXPLAIN prints such rows under the objective.
static constexpr const char *OBJECTIVE_ROW_TAG = "__objective_row__";

//! Tag on the placeholder coefficient of an automatic L0 link (`norm(e, 0)` with no
//! bound written): stage 06 replaces it with a bound derived from the column box, so a
//! plan prints it as `M` rather than as the 1.0 it holds until then.
static constexpr const char *L0_AUTO_BOUND_TAG = "__l0_auto_bound__";

//! Tag marking a simple `x OP const` comparison whose entire meaning the optimizer
//! folded into the decision column's box (`LogicalDecide::singleton_row_boxes`), so it
//! must not also be emitted as a model row. Set on the BoundComparisonExpression.alias
//! by DecideOptimizer::FoldSingletonRows; read by physical term extraction.
//!
//! The comparison deliberately stays in the tree rather than being replaced by a TRUE
//! placeholder: EXPLAIN renders the constraint list from this tree, and the user wrote
//! this bound, so it keeps rendering. The tag moves the *decision* upstream without
//! moving the rendering.
static constexpr const char *SINGLETON_ROW_BOUND_TAG = "__singleton_row_bound__";

//! Beside SINGLETON_ROW_BOUND_TAG on a recorded bound: a repair model loosens it through the
//! clause's own rows, one per kept row or class (DecidePreparedModel::singleton_row_constraints,
//! D28), not one knob per column. Set by DecideOptimizer::FoldSingletonRows.
static constexpr const char *SINGLETON_ROW_CLAUSE_ROWS_TAG = "__singleton_row_clause_rows__";

//! A private aggregate marker emitted by the DECIDE binder for norm(expr, p).
//! The marker is only a transport representation: canonicalization replaces the
//! orders `1`, `2` and `inf` with their definitions, and DecideOptimizer lowers the
//! L0 orders `0_auto` and `0_<positive-double>` before physical planning.
static constexpr const char *NORM_MARKER_TAG_PREFIX = "__decide_norm_";

//! Display-only spelling on an aggregate canonicalization built from `norm(e, p)`:
//! `SUM(ABS(e))` for `1`, `SUM(POWER(e, 2))` for `2`, `MAX(ABS(e))` for `inf`. The
//! tree means what it says; the tag only lets the canonical text read as the user
//! wrote it. RewriteNorm strips it before the optimizer formulates the clause. Its
//! prefix must not contain NORM_MARKER_TAG_PREFIX.
static constexpr const char *WRITTEN_NORM_TAG_PREFIX = "__written_norm_";

//! Semantic provenance stamped on the flattened value produced by an UNCORRELATED
//! scalar subquery. Shape alone cannot distinguish that query-wide column ref from
//! ordinary row data after PlanSubqueries.
static constexpr const char *QUERY_WIDE_VALUE_TAG = "__query_wide_value__";

//! Semantic provenance stamped on a flattened CORRELATED scalar subquery. It remains
//! row-varying; the tag exists so downstream diagnostics never quote DuckDB's internal
//! `SUBQUERY` placeholder as if it were user SQL.
static constexpr const char *ROW_VARYING_SUBQUERY_TAG = "__row_varying_subquery__";

//! Provenance stamped on the column that stands for a reducer on a bound (the bound-side window rewrite): its alias
//! is the reducer as written, so a render still prints `sum(cap) by (g)`, and the tag says that text names a
//! computed value, not a column of the source.
static constexpr const char *BOUND_REDUCER_TAG = "__bound_reducer__";

//! On the `IS NOT NULL` test a reducer's own `when` gains for a value its body reads
//! (DecideCanonicalizer::SkipNullData): the query never wrote it, so no render prints it.
static constexpr const char *SKIP_NULL_TAG = "__skip_null__";

//! On the COALESCE or CASE that reads a NULL coefficient as 0 (DecideOptimizer's DropNullObjectiveTerms, the direct
//! IR's GuardCoefficients): the query never wrote it, so a render prints the value it guards.
static constexpr const char *NULL_GUARD_TAG = "__null_guard__";

//! Stable source-comparison identity. The payload indexes the per-DECIDE
//! ConstraintSourceInfo registry; it survives canonical rebuilding and optimizer
//! one-to-many rewrites but is never used to group elastic slacks.
static constexpr const char *SOURCE_CLAUSE_TAG_PREFIX = "__source_clause_";

//! Tags on the two comparisons `x BETWEEN lo AND hi` binds to (`DecideConstraintsBinder::BindBetween`), `>=` first.
//! InitializeConstraintSourceInfo reads them to link the halves into the one clause the user wrote.
static constexpr const char *BETWEEN_LOWER_TAG = "__between_lower__";
static constexpr const char *BETWEEN_UPPER_TAG = "__between_upper__";

//! Source-level identity for clauses that diagnostics may only remove atomically.
//! Unlike source_clause_id (display provenance) and repair_group_id (elastic slack
//! grouping), this id means every generated row carrying it is one indivisible DROP.
static constexpr const char *REMOVAL_GROUP_TAG_PREFIX = "__removal_group_";

//! Parsed-source fragment identity for casts and scalar subqueries whose spelling
//! cannot be reconstructed after binding/PlanSubqueries.
static constexpr const char *SOURCE_FRAGMENT_TAG_PREFIX = "__source_fragment_";

inline string MakeSourceClauseTag(idx_t source_clause_id) {
	return string(SOURCE_CLAUSE_TAG_PREFIX) + to_string(source_clause_id) + "__";
}

inline string MakeRemovalGroupTag(idx_t removal_group_id) {
	return string(REMOVAL_GROUP_TAG_PREFIX) + to_string(removal_group_id) + "__";
}

inline string MakeSourceFragmentTag(idx_t fragment_id) {
	return string(SOURCE_FRAGMENT_TAG_PREFIX) + to_string(fragment_id) + "__";
}

inline bool TryParseDecideIndexTag(const string &alias, const string &prefix, idx_t &value) {
	string digits;
	if (!ExtractDecideTagPayload(alias, prefix, digits) || digits.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	value = static_cast<idx_t>(std::stoull(digits));
	return true;
}

inline bool TryParseSourceClauseTag(const string &alias, idx_t &source_clause_id) {
	return TryParseDecideIndexTag(alias, SOURCE_CLAUSE_TAG_PREFIX, source_clause_id);
}

inline bool TryParseRemovalGroupTag(const string &alias, idx_t &removal_group_id) {
	return TryParseDecideIndexTag(alias, REMOVAL_GROUP_TAG_PREFIX, removal_group_id);
}

//! `alias` without its source fragment tags: a column's spelling as written is metadata for a message, not part of the
//! name a plan displays. The other tags are kept; DECIDE reads them off the same alias.
inline string StripSourceFragmentTags(string alias) {
	idx_t pos;
	while ((pos = alias.find(SOURCE_FRAGMENT_TAG_PREFIX)) != string::npos) {
		auto end = alias.find("__", pos + strlen(SOURCE_FRAGMENT_TAG_PREFIX));
		alias.erase(pos, end == string::npos ? string::npos : end + 2 - pos);
	}
	return alias;
}

inline bool TryParseSourceFragmentTag(const string &alias, idx_t &fragment_id) {
	return TryParseDecideIndexTag(alias, SOURCE_FRAGMENT_TAG_PREFIX, fragment_id);
}

inline bool TryParseGroupKeyTag(const string &alias, idx_t &group_key_idx) {
	string payload;
	if (!ExtractDecideTagPayload(alias, GROUP_KEY_TAG_PREFIX, payload)) {
		return false;
	}
	auto digits = payload.substr(0, payload.find('_'));
	if (digits.empty() || digits.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	group_key_idx = static_cast<idx_t>(std::stoull(digits));
	return true;
}

//! The fragment holding a reducer's own `by` text, when its group key tag carries one.
inline bool TryParseGroupKeyTextTag(const string &alias, idx_t &text_fragment) {
	string payload;
	if (!ExtractDecideTagPayload(alias, GROUP_KEY_TAG_PREFIX, payload) || payload.find('_') == string::npos) {
		return false;
	}
	text_fragment = static_cast<idx_t>(std::stoull(payload.substr(payload.find('_') + 1)));
	return true;
}

inline bool TryParsePerKeyTag(const string &alias, idx_t &key_idx) {
	return TryParseDecideIndexTag(alias, PER_KEY_TAG_PREFIX, key_idx);
}

//! Classification stamped by DecideCanonicalizer on the complete canonical RHS when
//! every component is query-wide. Physical evaluation consumes this fact and does not
//! re-decide the RHS shape.
static constexpr const char *QUERY_WIDE_BOUND_TAG = "__query_wide_bound__";

inline bool IsQueryWideValueTag(const string &alias) {
	return HasDecideTag(alias, QUERY_WIDE_VALUE_TAG);
}

inline bool IsRowVaryingSubqueryTag(const string &alias) {
	return HasDecideTag(alias, ROW_VARYING_SUBQUERY_TAG);
}

inline bool IsQueryWideBoundTag(const string &alias) {
	return HasDecideTag(alias, QUERY_WIDE_BOUND_TAG);
}

//! Tag prefix for ABS envelope constraint linking.
//! Format: "__abs_ub_pos_<aux_idx>__" on C1 (aux >= inner)
//!         "__abs_ub_neg_<aux_idx>__" on C2 (aux >= -inner)
//! Set on BoundComparisonExpression.alias by RewriteAbs when the aux needs its upper
//! side pinned as well: either (a) sense==MAXIMIZE and ABS is in the objective, or
//! (b) ABS is in a constraint shape that does not naturally upper-bound aux
//! (e.g. ABS(...) >= K, ABS(...) = K). Without that, solver pressure lets aux float
//! free above |inner|.
//!
//! The payload is the AUXILIARY's index, because the two arms that pin it need
//! different things: the lowering arm adds a Big-M envelope switched by a binary sign
//! indicator, while the native arm states `aux = |t|` and allocates no binary at all.
//! The auxiliary is the only index both arms are guaranteed to have.
static constexpr const char *ABS_UB_POS_TAG_PREFIX = "__abs_ub_pos_";
static constexpr const char *ABS_UB_NEG_TAG_PREFIX = "__abs_ub_neg_";

//! Tag set on BoundFunctionExpression.alias for ABS occurrences inside a
//! constraint shape that does not naturally upper-bound the auxiliary
//! (i.e. hard-direction ABS). Read by FindAndReplaceAbs, propagated to
//! AbsPairInfo::needs_bigm so RewriteAbs pins the auxiliary's upper side too --
//! with a sign-indicator y and a Big-M envelope on the lowering arm, or with a
//! native `aux = |t|` on a backend that states ABS itself. The tag is set by
//! TagAbsForBigM (formerly ValidateAbsConstraintDirection).
static constexpr const char *ABS_NEEDS_BIGM_TAG = "__abs_needs_bigm__";

} // namespace duckdb
