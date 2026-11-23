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

//! DECIDE encodes the pipeline metadata of a frame and of a function in the `alias` of bound expressions. More than
//! one tag can apply to the same node, so tags are **concatenated** and matched by search, never by equality. Every
//! tag opens and closes with `__`, which is what makes concatenation unambiguous and lets a prefix tag's payload be
//! read back below.
inline void AddDecideTag(string &alias, const string &tag) {
	alias += tag;
}

//! True when `alias` carries `tag`, whatever else it also carries.
inline bool HasDecideTag(const string &alias, const string &tag) {
	return alias.find(tag) != string::npos;
}

//! Remove every DECIDE tag from an alias, leaving whatever ordinary alias it was appended
//! to: "cost__abs_function_0____null_guard__" yields "cost", and an alias that was
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
//! ("__abs_function_2____null_guard__", "__abs_function_") yields "2". The payload runs
//! from the end of the prefix to the tag's closing `__`, so payloads may contain single
//! underscores. Returns false when the tag is absent or has an empty payload.
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

//! On the COALESCE or CASE that reads a NULL coefficient as 0 (DecideOptimizer's DropNullObjectiveTerms, the direct
//! IR's GuardCoefficients): the query never wrote it, so a render prints the value it guards.
static constexpr const char *NULL_GUARD_TAG = "__null_guard__";

//! On a reference to a frame column (DecideFrameReads::columns): its name is the frame as written, which a
//! message prints as it stands, not as an identifier.
static constexpr const char *FRAME_COLUMN_TAG = "__frame_column__";

//! Tag on an ABS function in a clause: it names the auxiliary the ABS became.
static constexpr const char *ABS_FUNCTION_TAG_PREFIX = "__abs_function_";

//! Tag on a written Boolean product that the optimizer has described as an F-node group. The expression remains
//! intact in the plan; term extraction and tree lowering read the group's expansion from LogicalDecide metadata.
static constexpr const char *BILINEAR_FUNCTION_GROUP_TAG_PREFIX = "__bilinear_function_group_";
//! Tag on a nonlinear reducer body left intact in the clause tree. The F node owns its value and definition row.
static constexpr const char *DEFINITION_FUNCTION_TAG_PREFIX = "__definition_function_";

inline string MakeBilinearFunctionGroupTag(idx_t group_idx) {
	return string(BILINEAR_FUNCTION_GROUP_TAG_PREFIX) + to_string(group_idx) + "__";
}

inline string MakeDefinitionFunctionTag(idx_t var_idx) {
	return string(DEFINITION_FUNCTION_TAG_PREFIX) + to_string(var_idx) + "__";
}

inline string MakeAbsFunctionTag(idx_t var_idx) {
	return string(ABS_FUNCTION_TAG_PREFIX) + to_string(var_idx) + "__";
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
static constexpr const char *DECIDE_TAGS[] = {NULL_GUARD_TAG,
                                              FRAME_COLUMN_TAG,
                                              ABS_FUNCTION_TAG_PREFIX,
                                              BILINEAR_FUNCTION_GROUP_TAG_PREFIX,
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
