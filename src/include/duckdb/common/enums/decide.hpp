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

//! What a reducer does with the rows it reads. NORM is a norm no definition has replaced.
enum class DecideReducerKind : uint8_t { SUM = 0, AVG = 1, MIN = 2, MAX = 3, COUNT = 4, NORM = 5 };

inline bool IsDecideExtremum(DecideReducerKind kind) {
	return kind == DecideReducerKind::MIN || kind == DecideReducerKind::MAX;
}

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

//! What a helper definition is: the construct a rewrite kept and gave a helper decision.
enum class DecideHelperKind : uint8_t {
	//! `abs(e)` over a decision, kept where it is written (RewriteAbs). The helper stands for |body|; the tree states
	//! its two lower-envelope rows, `helper >= body` and `helper >= -body` (MakeAbsRow), and stage 06 its encoding.
	ABS,
	//! A written product of two forms over decisions, kept where it is written (FindAndReplaceBilinear). `body` is its
	//! expansion, which the term splitter and the tree read in its place: the product distributed over its factors'
	//! terms, each product of two decisions with a Boolean factor read as that BOOLEAN_PRODUCT's helper and every other
	//! one left as a term of degree 2. It has no helper of its own: the expansion is a sum over any number of them.
	PRODUCT,
	//! `bool_factor * other_factor` of two decisions, one of them Boolean: the helper a PRODUCT's expansion reads,
	//! linked to its factors by stage 06 (LinearizeBilinear). It has no body: the two decisions are the product.
	BOOLEAN_PRODUCT,
	//! The indicator of one counted row of `norm(e, 0)` (RewriteNorm): `links` are its three formulation rows, kept
	//! beside the clauses so the later rewrites give a nested ABS or product its helper; the first holds the counted
	//! expression and the rows it is read on.
	L0_INDICATOR,
	//! A reducer's nonlinear body, kept where it is written: the helper is `relation` its body on the rows `when`
	//! keeps, and the tree builds that row beneath the F node (MakeDefinitionRow).
	DEFINITION,
	//! A decision reducer of a scoped comparison in the VALUE shape (CanonicalConstraintClass::VALUE), or a range frame
	//! over decisions, replaced by the helper that holds the reducer (DecideHelperDefinition::Reducer) over the rows
	//! `when` keeps: one instance per group of its path key among those rows, or per window of its frame, defined once
	//! by stage 06 -- an equality row per group for SUM/AVG, an extremum link per group for MIN/MAX. One per reducer
	//! over one set of rows, shared by every row that reads it; its definition belongs to no clause (owner ruling
	//! B10), so a DROP of a reading clause leaves it and DIAGNOSE reads it as part of the fixed model.
	REDUCER_VALUE,
};

//! The definition a kept call names (BoundFunctionExpression::definition): its entry of
//! LogicalDecide::helper_definitions and that entry's kind, as a compiler's use node holds its declaration. The kind
//! is the entry's, written beside the index by the one writer (LogicalDecide::NameDefinition) for the readers that
//! hold a call and no table. Unset on every call no rewrite kept.
struct DecideHelperRef {
	DecideHelperKind kind = DecideHelperKind::ABS;
	idx_t index = DConstants::INVALID_INDEX;

	bool IsSet() const {
		return index != DConstants::INVALID_INDEX;
	}
	//! Whether it names a definition of `kind`.
	bool Is(DecideHelperKind kind_p) const {
		return IsSet() && kind == kind_p;
	}
	bool operator==(const DecideHelperRef &other) const {
		return index == other.index && kind == other.kind;
	}
};

//! The name prefixes a rewrite gives a helper decision it adds (AddAuxiliaryDecision). Each is the opening of a name
//! `<prefix><n>__`. They are display names: no reader finds a helper by its name.
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

} // namespace duckdb
