//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/common/enums/decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"

#include <cstring>

namespace duckdb {

class Serializer;
class Deserializer;

enum class DecideSense : uint8_t { MAXIMIZE = 0, MINIMIZE = 1, FEASIBILITY = 2 };

//! How one DECIDE declaration maps onto solver columns.
//!   ROW        — `x(INT)`, `per row: x(INT)` one column per result row
//!   KEYED      — `per K: x(INT)`             one column per distinct value of the key
//!   GLOBAL     — `per (): x(INT)`            one column for the whole query
//!   FRAME_READ — never declared: a decision read at the row a frame selector selects for
//!                each row (SolverInput::frame_reads). It owns no column; at a row it is
//!                its base decision's column at the selected row. Execution creates it.
//!   INLINED    — never declared: a helper decision stage 06 wrote where it is read
//!                (SolverInput::inlined_helpers, D89). It owns no column; a row reading it
//!                reads its members. `keyed_scope_idx` stays the scope that names its
//!                instances.
//! The values are serialized, so a stored plan reads under any rename.
enum class DecideVarScope : uint8_t { ROW = 0, KEYED = 1, GLOBAL = 2, FRAME_READ = 3, INLINED = 4 };

//! Per-variable scope assignment. Kept as one struct rather than parallel
//! arrays so the optimizer's auxiliary-variable appends cannot desync the
//! scope from its keyed scope index.
struct DecideVarScopeInfo {
	DecideVarScope scope = DecideVarScope::ROW;
	//! Index into keyed_scopes / keyed_mappings. Meaningful when scope == KEYED, and for an
	//! INLINED helper that was keyed (the scope naming its instances); INVALID_INDEX otherwise.
	idx_t keyed_scope_idx = DConstants::INVALID_INDEX;
	//! The key written after `per`, as written (`D.depotID, S.customerID`), so a message
	//! can name the decision the way it was declared. Empty for every other spelling.
	string declared_key;
	//! Where the decision is declared in the query, which an error about the decision points at. Invalid for a
	//! decision the optimizer adds.
	optional_idx declaration;

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

//! What one field of a variant signature (LogicalDecide::signatures) compares, at a row:
//!   VALUE    — a data expression, by value (NULL a value)
//!   DECISION — a decision, by instance: a per-row one by its row, a keyed one by its class
//!   BY_KEY   — a reducer's `by` key, by the value of each of its elements
//!   FRAME    — a frame selector, by its keys (D82): its timeline, the anchor's order key
//!              when its position is relative to the anchor, and its fallback where the
//!              anchor reaches it
enum class DecideSignatureFieldKind : uint8_t { VALUE = 0, DECISION = 1, BY_KEY = 2, FRAME = 3 };

//! How one column of a key (LogicalDecide::keys) compares NULL.
//!   NO_GROUP — a column the user wrote: rows group as SQL's `=` compares them, so a NULL
//!              value is in no group (deck p. 30)
//!   VALUE    — a column a relation element stands for (`per D`, `by (D)`): NULL is a value,
//!              so every row of D is one class whatever its NULLs
enum class DecideKeyNullMode : uint8_t { NO_GROUP = 0, VALUE = 1 };
//! How a `per` is written.
enum class DecidePer : uint8_t {
	//! No `per`
	NONE = 0,
	//! `per row`, the default written out
	ROW = 1,
	//! `per ()`: one for the whole query
	ALL = 2,
	//! `per K`: one per distinct key
	KEY = 3
};

//! The type a declaration gives its decision.
enum class DecideVariableType : uint8_t { REAL = 0, INT = 1, BOOL = 2 };

//! A frame selector's position on the anchor row's timeline (language/guide.md §5.6):
//! the first or last peer, or k peers before or after the anchor's.
enum class DecideFrameAnchor : uint8_t { FIRST = 0, LAST = 1, PREVIOUS = 2, NEXT = 3 };

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

//! The index a tag "<prefix><i>__" carries; false (leaving `value` untouched) when the alias carries no such tag.
inline bool TryParseDecideIndexTag(const string &alias, const string &prefix, idx_t &value) {
	string digits;
	if (!ExtractDecideTagPayload(alias, prefix, digits) || digits.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	value = static_cast<idx_t>(std::stoull(digits));
	return true;
}

//! The bound form of a `when c:` in front of a constraint or the objective: a conjunction whose child 0 is the body
//! and child 1 the condition. The body reads only the rows where the condition is true. A nested objective is a nested
//! reducer (D81).
static constexpr const char *WHEN_FILTER_TAG = "__when_filter__";

//! Tag prefix recording a reducer's own key, `sum(per K: e)` (on
//! BoundAggregateExpression.alias). Format: "__qualified_by_<keyed_scope_idx>__" — the
//! index is into LogicalDecide::keyed_scopes, whose entry supplies the classes
//! the reducer takes one value from. The scope may have no variable declared on it.
static constexpr const char *QUALIFIED_REDUCER_SCOPE_TAG_PREFIX = "__qualified_by_";

//! Builds the alias tag naming the keyed scope a reducer is qualified by ("__qualified_by_<i>__").
inline string MakeQualifiedReducerTag(idx_t keyed_scope_idx) {
	return string(QUALIFIED_REDUCER_SCOPE_TAG_PREFIX) + to_string(keyed_scope_idx) + "__";
}

//! Reads back the keyed scope index from an alias written by MakeQualifiedReducerTag.
//! Returns false (leaving `scope_idx` untouched) when the alias carries no such tag.
inline bool TryParseQualifiedReducerTag(const string &alias, idx_t &scope_idx) {
	return TryParseDecideIndexTag(alias, QUALIFIED_REDUCER_SCOPE_TAG_PREFIX, scope_idx);
}

//! The bound form of a constraint's `per key:`: a conjunction whose child 0 is the
//! constraint and whose children[1..] are the key's columns, none for `per ()`. The
//! constraint is generated once per class of the key among the kept rows; it groups no
//! reducer, and NULL is a class.
static constexpr const char *PER_CLASS_TAG = "__per_class__";

//! Tag prefix naming the variant signature of a `per K:` clause (on its PER_CLASS_TAG wrapper):
//! "__signature_<i>__" indexes LogicalDecide::signatures. The clause is one instance per class of K and value of the
//! signature's fields (D70); no tag means it has no field, so one instance per class.
static constexpr const char *SIGNATURE_TAG_PREFIX = "__signature_";

inline string MakeSignatureTag(idx_t signature_idx) {
	return string(SIGNATURE_TAG_PREFIX) + to_string(signature_idx) + "__";
}

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

inline string MakeGroupKeyTag(idx_t group_key_idx) {
	return string(GROUP_KEY_TAG_PREFIX) + to_string(group_key_idx) + "__";
}

//! Tag prefix naming the key of a `per K:` wrapper (PER_CLASS_TAG):
//! "__per_key_<i>__" indexes LogicalDecide::keys. No tag means `per ()`: one class.
static constexpr const char *PER_KEY_TAG_PREFIX = "__per_key_";

inline string MakePerKeyTag(idx_t key_idx) {
	return string(PER_KEY_TAG_PREFIX) + to_string(key_idx) + "__";
}

//! Tag prefix naming a frame selector: "__frame_<i>__" indexes LogicalDecide::frames. It
//! stands on the aggregate a frame binds to -- `first(e)` for a point frame, its reducer
//! for a range -- whose rows at an anchor row are the ones the frame selects there
//! (language/guide.md §5.6).
static constexpr const char *FRAME_TAG_PREFIX = "__frame_";

inline string MakeFrameTag(idx_t frame_idx) {
	return string(FRAME_TAG_PREFIX) + to_string(frame_idx) + "__";
}

//! Tag used to identify AVG→SUM rewritten aggregates (terms need coefficient scaling at execution)
static constexpr const char *AVG_REWRITE_TAG = "__avg_rewrite__";

//! Metadata for the executable tree when a flat MIN/MAX carrier deliberately
//! drops its qualifier. Term extraction and rendering do not read these.
static constexpr const char *TREE_RETAINED_EXPRESSION_TAG_PREFIX = "__tree_retained_expression_";
static constexpr const char *TREE_REDUCER_SCOPE_TAG_PREFIX = "__tree_reducer_scope_";
static constexpr const char *TREE_REDUCER_KEY_TAG_PREFIX = "__tree_reducer_key_";

//! Tag marking a comparison whose LHS is an easy-direction MIN/MAX reducer
//! (MAX(e) <= K, MIN(e) >= K). The reducer stays in the tree: the linear form expands it
//! to one row per member, the bound reduced over each `by` group, and bound absorption
//! folds a bare one into the column box (D41). Set on the
//! BoundComparisonExpression.alias during RewriteMinMax.
static constexpr const char *MINMAX_EASY_REWRITE_TAG = "__minmax_easy__";

//! Tag marking a reducer with a nonlinear body held by a tree-owned definition F node. The source body remains at its
//! written site as a DEFINITION_FUNCTION_TAG_PREFIX read, while this aggregate marker tells MIN/MAX planning that its
//! members are helper columns. A helper has no box, so stage 08 states a MIN/MAX over it natively.
//! Set on the BoundAggregateExpression.alias and on the BoundComparisonExpression.alias of the row defining the
//! helper, which EXPLAIN leaves out: it restates the body, and the clause's account line says how it is held.
static constexpr const char *BODY_DEFINITION_TAG = "__body_definition__";

//! Tag marking optimizer-generated helper constraints that define auxiliaries or
//! link rewrite machinery. These rows are rigid and must not be elastic-relaxed.
static constexpr const char *STRUCTURAL_CONSTRAINT_TAG = "__decide_structural_constraint__";

//! On a row that defines an auxiliary decision standing for a value its readers read -- an ABS's envelope, a body
//! helper's definition, an L0 indicator's links -- the auxiliary's index (DecideConstraint::defined_var). The
//! auxiliary has no value where what its definition reads has none (D88).
static constexpr const char *DEFINES_TAG_PREFIX = "__defines_";

inline string MakeDefinesTag(idx_t var_idx) {
	return string(DEFINES_TAG_PREFIX) + to_string(var_idx) + "__";
}

//! Tag marking a row a rewrite added for the objective (an ABS envelope, a product's
//! corner rows, an L0 link), which no clause owns: the rewrites start the objective's
//! walk with it as the source alias, and EXPLAIN prints such rows under the objective.
static constexpr const char *OBJECTIVE_ROW_TAG = "__objective_row__";

//! Tag on the placeholder coefficient of an automatic L0 link (`norm(e, 0)` with no
//! bound written): stage 06 replaces it with a bound derived from the column box, so a
//! plan prints it as `M` rather than as the 1.0 it holds until then.
static constexpr const char *L0_AUTO_BOUND_TAG = "__l0_auto_bound__";

//! Tags on the other two constants an L0 link holds, which the query did not write as a coefficient: the bound M the
//! query wrote in `norm(e, 0, M)`, and the tolerance of the link `abs(e) >= tolerance * indicator`
//! (`decide_l0_tolerance`). A plan prints M as the number written and the tolerance by the name it is set with, and a
//! coefficient of 1 here is still printed.
static constexpr const char *L0_WRITTEN_BOUND_TAG = "__l0_written_bound__";
static constexpr const char *L0_TOLERANCE_TAG = "__l0_tolerance__";

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
//! clause's own rows, one per kept row or class (its folded root's clause, DecideNode::folded,
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

//! On a call whose name was rewritten to its primary's: `abs` for `@`, `pow`, `power` and `^` for `**`, `mean` for
//! `avg`. DuckDB registers each alias with its primary's implementation, so the binder renames every such call in a
//! DECIDE clause's algebra (RespellDecideAliases) and every check matches the primary alone. The tag holds the name
//! the user wrote, which a plan and a message print. Display only: two calls that differ in it are equal.
static constexpr const char *WRITTEN_NAME_TAG_PREFIX = "__written_name_";

//! The name a call was written under, from its tags, else `name`.
inline string DecideWrittenName(const string &tags, const string &name) {
	string written;
	return ExtractDecideTagPayload(tags, WRITTEN_NAME_TAG_PREFIX, written) ? written : name;
}

//! The tags without the written-name tag, for comparing structure.
inline string WithoutWrittenName(const string &tags) {
	string written;
	string result = tags;
	if (ExtractDecideTagPayload(tags, WRITTEN_NAME_TAG_PREFIX, written)) {
		RemoveDecideTag(result, string(WRITTEN_NAME_TAG_PREFIX) + written + "__");
	}
	return result;
}

//! Semantic provenance stamped on the flattened value produced by an UNCORRELATED
//! scalar subquery. Shape alone cannot distinguish that query-wide column ref from
//! ordinary row data after PlanSubqueries.
static constexpr const char *QUERY_WIDE_VALUE_TAG = "__query_wide_value__";

//! Semantic provenance stamped on a flattened CORRELATED scalar subquery. It remains
//! row-varying; the tag exists so downstream diagnostics never quote DuckDB's internal
//! `SUBQUERY` placeholder as if it were user SQL.
static constexpr const char *ROW_VARYING_SUBQUERY_TAG = "__row_varying_subquery__";

//! Semantic provenance stamped on what stands for a reducer the bound-side rewrite recorded (its placeholder column, or
//! the guard over one that is NULL outside the reducer's groups): a computed value, read whole. A signature counts it
//! as the reducer's key alone (DirectBoundTables), and the tree carries it as a value no decision reads. Messages quote
//! it by its query location (DecideReplaces), not by this tag.
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

inline string MakeSourceClauseTag(idx_t source_clause_id) {
	return string(SOURCE_CLAUSE_TAG_PREFIX) + to_string(source_clause_id) + "__";
}

inline string MakeRemovalGroupTag(idx_t removal_group_id) {
	return string(REMOVAL_GROUP_TAG_PREFIX) + to_string(removal_group_id) + "__";
}

inline bool TryParseSourceClauseTag(const string &alias, idx_t &source_clause_id) {
	return TryParseDecideIndexTag(alias, SOURCE_CLAUSE_TAG_PREFIX, source_clause_id);
}

inline bool TryParseRemovalGroupTag(const string &alias, idx_t &removal_group_id) {
	return TryParseDecideIndexTag(alias, REMOVAL_GROUP_TAG_PREFIX, removal_group_id);
}

inline bool TryParseGroupKeyTag(const string &alias, idx_t &group_key_idx) {
	return TryParseDecideIndexTag(alias, GROUP_KEY_TAG_PREFIX, group_key_idx);
}

inline bool TryParsePerKeyTag(const string &alias, idx_t &key_idx) {
	return TryParseDecideIndexTag(alias, PER_KEY_TAG_PREFIX, key_idx);
}

//! Reads back the signature index from an alias written by MakeSignatureTag.
inline bool TryParseSignatureTag(const string &alias, idx_t &signature_idx) {
	return TryParseDecideIndexTag(alias, SIGNATURE_TAG_PREFIX, signature_idx);
}

inline bool TryParseFrameTag(const string &alias, idx_t &frame_idx) {
	return TryParseDecideIndexTag(alias, FRAME_TAG_PREFIX, frame_idx);
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

//! On the comparison of a clause over groups: its bound reads a frame and may differ within a group, so it takes the
//! group's tightest value among the rows where it has one (D85). No side table can read a frame, so execution takes it.
//! The comparison holds the tag because a comparison's tags survive a rebuild (K5), which a bound's node does not.
static constexpr const char *FRAME_TIGHTEST_BOUND_TAG = "__frame_tightest_bound__";

//! On a reference to a frame column (DecideFrameReads::columns): its name is the frame as written, which a
//! message prints as it stands, not as an identifier.
static constexpr const char *FRAME_COLUMN_TAG = "__frame_column__";

//! On a reducer over data whose argument reads a frame: "__frame_aggregate_<i>__" indexes
//! LogicalDecide::frame_aggregates. No side table can read a frame, so execution computes it after the frames.
static constexpr const char *FRAME_AGGREGATE_TAG_PREFIX = "__frame_aggregate_";

inline string MakeFrameAggregateTag(idx_t index) {
	return string(FRAME_AGGREGATE_TAG_PREFIX) + to_string(index) + "__";
}

inline bool TryParseFrameAggregateTag(const string &alias, idx_t &index) {
	string payload;
	if (!ExtractDecideTagPayload(alias, FRAME_AGGREGATE_TAG_PREFIX, payload) || payload.empty() ||
	    payload.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	index = static_cast<idx_t>(std::stoull(payload));
	return true;
}

//! Tag on an ABS function in a clause, and on the positive lower-envelope row the tree builds for its F node (`aux >=
//! inner`): both name the auxiliary the ABS became.
static constexpr const char *ABS_FUNCTION_TAG_PREFIX = "__abs_function_";
//! Matching marker on the negative lower-envelope row (`aux >= -inner`).
static constexpr const char *ABS_FUNCTION_NEG_TAG_PREFIX = "__abs_function_neg_";

//! Tag on a written Boolean product that the optimizer has described as an F-node group. The expression remains
//! intact in the plan; term extraction and tree lowering read the group's expansion from LogicalDecide metadata.
static constexpr const char *BILINEAR_FUNCTION_GROUP_TAG_PREFIX = "__bilinear_function_group_";
//! Tag on an optimizer-generated L0 indicator link, identifying the L0 F node that owns the row.
static constexpr const char *L0_INDICATOR_FUNCTION_TAG_PREFIX = "__l0_indicator_function_";
//! Tag on a nonlinear reducer body left intact in the clause tree. The F node owns its value and definition row.
static constexpr const char *DEFINITION_FUNCTION_TAG_PREFIX = "__definition_function_";

inline string MakeBilinearFunctionGroupTag(idx_t group_idx) {
	return string(BILINEAR_FUNCTION_GROUP_TAG_PREFIX) + to_string(group_idx) + "__";
}

inline string MakeL0IndicatorFunctionTag(idx_t var_idx) {
	return string(L0_INDICATOR_FUNCTION_TAG_PREFIX) + to_string(var_idx) + "__";
}

inline string MakeDefinitionFunctionTag(idx_t var_idx) {
	return string(DEFINITION_FUNCTION_TAG_PREFIX) + to_string(var_idx) + "__";
}

inline string MakeAbsFunctionTag(idx_t var_idx) {
	return string(ABS_FUNCTION_TAG_PREFIX) + to_string(var_idx) + "__";
}

inline string MakeAbsFunctionNegTag(idx_t var_idx) {
	return string(ABS_FUNCTION_NEG_TAG_PREFIX) + to_string(var_idx) + "__";
}

//! The ABS lower-envelope rows are built by the tree from the optimizer's record, not written by the user; each is
//! marked with one of these tags, and this returns the helper they define.
inline bool TryParseAbsFunctionEnvelopeTag(const string &alias, idx_t &var_idx) {
	return TryParseDecideIndexTag(alias, ABS_FUNCTION_TAG_PREFIX, var_idx) ||
	       TryParseDecideIndexTag(alias, ABS_FUNCTION_NEG_TAG_PREFIX, var_idx);
}

//! The name prefixes a rewrite gives a helper decision it adds (AddAuxiliaryDecision). Each is the opening of a name
//! `<prefix><n>__`.
//! They are tags in a decision's or a column's name, so DecideNameContainsTag refuses a user name that holds one.
//! A helper's origin is read from them until the helper carries it as a field (R20).
//! The 0/1 indicator of an L0 count: the one whose big-M the optimizer picks, and the one whose big-M is written.
static constexpr const char *L0_AUTO_INDICATOR_NAME_PREFIX = "__l0auto_ind_";
static constexpr const char *L0_INDICATOR_NAME_PREFIX = "__l0_ind_";
//! The 0/1 indicator of one value of an `x IN (...)` over a decision; its name continues with the decision's own.
static constexpr const char *IN_INDICATOR_NAME_PREFIX = "__in_ind_";
//! The auxiliary of an ABS.
static constexpr const char *ABS_AUX_NAME_PREFIX = "__abs_aux_";
//! The auxiliary of a bilinear product.
static constexpr const char *BILINEAR_AUX_NAME_PREFIX = "__bilinear_aux_";
//! The helper standing for a nonlinear reducer body.
static constexpr const char *BODY_NAME_PREFIX = "__body_";
//! The helper standing for a reducer's value, one per link of a scoped rewrite.
static constexpr const char *VALUE_LINK_NAME_PREFIX = "__value_";

//! Every DECIDE tag above, the one list DecideNameContainsTag reads. A constant added above is added here.
static constexpr const char *DECIDE_TAGS[] = {WHEN_FILTER_TAG,
                                              QUALIFIED_REDUCER_SCOPE_TAG_PREFIX,
                                              PER_CLASS_TAG,
                                              SIGNATURE_TAG_PREFIX,
                                              VALUE_REWRITE_TAG,
                                              GROUP_KEY_TAG_PREFIX,
                                              PER_KEY_TAG_PREFIX,
                                              FRAME_TAG_PREFIX,
                                              AVG_REWRITE_TAG,
                                              TREE_RETAINED_EXPRESSION_TAG_PREFIX,
                                              TREE_REDUCER_SCOPE_TAG_PREFIX,
                                              TREE_REDUCER_KEY_TAG_PREFIX,
                                              MINMAX_EASY_REWRITE_TAG,
                                              BODY_DEFINITION_TAG,
                                              STRUCTURAL_CONSTRAINT_TAG,
                                              DEFINES_TAG_PREFIX,
                                              OBJECTIVE_ROW_TAG,
                                              L0_AUTO_BOUND_TAG,
                                              L0_WRITTEN_BOUND_TAG,
                                              L0_TOLERANCE_TAG,
                                              SINGLETON_ROW_BOUND_TAG,
                                              SINGLETON_ROW_CLAUSE_ROWS_TAG,
                                              NORM_MARKER_TAG_PREFIX,
                                              WRITTEN_NORM_TAG_PREFIX,
                                              WRITTEN_NAME_TAG_PREFIX,
                                              QUERY_WIDE_VALUE_TAG,
                                              ROW_VARYING_SUBQUERY_TAG,
                                              BOUND_REDUCER_TAG,
                                              SKIP_NULL_TAG,
                                              NULL_GUARD_TAG,
                                              SOURCE_CLAUSE_TAG_PREFIX,
                                              BETWEEN_LOWER_TAG,
                                              BETWEEN_UPPER_TAG,
                                              REMOVAL_GROUP_TAG_PREFIX,
                                              QUERY_WIDE_BOUND_TAG,
                                              FRAME_TIGHTEST_BOUND_TAG,
                                              FRAME_COLUMN_TAG,
                                              FRAME_AGGREGATE_TAG_PREFIX,
                                              ABS_FUNCTION_TAG_PREFIX,
                                              ABS_FUNCTION_NEG_TAG_PREFIX,
                                              BILINEAR_FUNCTION_GROUP_TAG_PREFIX,
                                              L0_INDICATOR_FUNCTION_TAG_PREFIX,
                                              DEFINITION_FUNCTION_TAG_PREFIX,
                                              L0_AUTO_INDICATOR_NAME_PREFIX,
                                              L0_INDICATOR_NAME_PREFIX,
                                              IN_INDICATOR_NAME_PREFIX,
                                              ABS_AUX_NAME_PREFIX,
                                              BILINEAR_AUX_NAME_PREFIX,
                                              BODY_NAME_PREFIX,
                                              VALUE_LINK_NAME_PREFIX};

//! True when a name a user wrote contains a DECIDE tag, in any letter case. The tags travel in expression aliases and
//! are found by search (HasDecideTag), so such a name is read as the tag. Until the tags are gone (R20) DECIDE refuses
//! it, as a decision's name and as a column a clause reads.
inline bool DecideNameContainsTag(const string &name) {
	auto lower = StringUtil::Lower(name);
	for (auto tag : DECIDE_TAGS) {
		if (lower.find(tag) != string::npos) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
