//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/common/enums/decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

enum class DecideSense : uint8_t {
    MAXIMIZE = 0,
    MINIMIZE = 1,
    FEASIBILITY = 2
};

//! The domain of a DECIDE declaration, as written: `x(INT)`, `x(TEXT in [...])`.
//!   INT / REAL / BOOL     — the classic domains
//!   SEMIREAL / SEMIINT    — {0} ∪ [lo, hi]; the bounds are mandatory
//!   TEXT                  — one of a finite set of strings (one-hot at the solver)
enum class DecideDomain : uint8_t { INT = 0, REAL = 1, BOOL = 2, SEMIREAL = 3, SEMIINT = 4, TEXT = 5 };

//! How many instances a declaration, a constraint or a reducer term generates
//! (DeciQL "Key", spec §5). ROW is the default (one per surviving row), GLOBAL is
//! `PER ()` (exactly one), KEY is `PER k1, k2, ...` (one per distinct key value).
enum class DecideScopeKind : uint8_t { ROW = 0, GLOBAL = 1, KEY = 2 };

enum class DecideExpression : uint8_t {
    INVALID = 0,
    VARIABLE,
    SUM
};

//! How one DECIDE declaration maps onto solver columns.
//!   ROW    — `x(INT)`        one column per result row
//!   ENTITY — `T.x(INT)`      one column per distinct entity of table T
//!   SCALAR — `scalar x(INT)` one column for the whole query
enum class DecideVarScope : uint8_t {
    ROW = 0,
    ENTITY = 1,
    SCALAR = 2
};

//! Per-variable scope assignment. Kept as one struct rather than parallel
//! arrays so the optimizer's auxiliary-variable appends cannot desync the
//! scope from its entity index.
//! A `TEXT IN [...]` decision's one-hot encoding: `values[i]` is chosen exactly when
//! the hidden BOOL decision `indicator_indices[i]` is 1. The binder declares the
//! indicators beside the decision (same scope) and states the sum-to-one row; the
//! readback maps the chosen indicator back to its string.
struct DecideTextDomain {
    idx_t variable_index = DConstants::INVALID_INDEX;
    vector<string> values;
    vector<idx_t> indicator_indices;

    void Serialize(Serializer &serializer) const;
    static DecideTextDomain Deserialize(Deserializer &deserializer);
};

struct DecideVarScopeInfo {
    DecideVarScope scope = DecideVarScope::ROW;
    //! Index into entity_scopes / entity_mappings. Meaningful only when
    //! scope == ENTITY; INVALID_INDEX otherwise.
    idx_t entity_scope_idx = DConstants::INVALID_INDEX;

    DecideVarScopeInfo() = default;
    DecideVarScopeInfo(DecideVarScope scope_p, idx_t entity_scope_idx_p)
        : scope(scope_p), entity_scope_idx(entity_scope_idx_p) {
    }

    static DecideVarScopeInfo Row() {
        return DecideVarScopeInfo();
    }
    static DecideVarScopeInfo Entity(idx_t entity_scope_idx_p) {
        return DecideVarScopeInfo(DecideVarScope::ENTITY, entity_scope_idx_p);
    }
    static DecideVarScopeInfo Scalar() {
        return DecideVarScopeInfo(DecideVarScope::SCALAR, DConstants::INVALID_INDEX);
    }

    bool IsEntity() const {
        return scope == DecideVarScope::ENTITY;
    }
    bool IsScalar() const {
        return scope == DecideVarScope::SCALAR;
    }

    //! Generated from `storage/serialization/nodes.json`.
    void Serialize(Serializer &serializer) const;
    static DecideVarScopeInfo Deserialize(Deserializer &deserializer);
};

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
//! to: "cost__source_clause_0____when_constraint__" yields "cost", and an alias that was
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

//! Tag used to identify WHEN-conditional constraints throughout the pipeline
static constexpr const char *WHEN_CONSTRAINT_TAG = "__when_constraint__";

//! Tag used to identify PER-generated constraints throughout the pipeline. Child 0 is
//! the constraint; children 1..N are the generation key's columns, and a wrapper with
//! NO key children is `PER ()`: one instance for the whole query. PER decides how many
//! constraint instances exist and nothing else -- which rows a reducer inside the
//! constraint sums is that reducer's own BY (REDUCER_BY_TAG).
static constexpr const char *PER_CONSTRAINT_TAG = "__per_constraint__";

//! Tags recording a PER wrapper's resolved generation key: `__gen_scope_<idx>__` names
//! the entity scope (LogicalDecide::entity_scopes) whose distinct values are the
//! constraint instances; `__gen_global__` is `PER ()`, exactly one instance. A PER
//! wrapper carries exactly one of the two beside PER_CONSTRAINT_TAG.
static constexpr const char *GEN_SCOPE_TAG_PREFIX = "__gen_scope_";
static constexpr const char *GEN_GLOBAL_TAG = "__gen_global__";

inline string MakeGenScopeTag(idx_t entity_scope_idx) {
	return string(GEN_SCOPE_TAG_PREFIX) + to_string(entity_scope_idx) + "__";
}

//! Tag used to identify a guarded constraint, `IF b: body`. Child 0 is the constraint,
//! child 1 the guard, which references at least one decision: the instance is imposed
//! only when the guard holds (an implication the optimizer lowers).
static constexpr const char *IF_CONSTRAINT_TAG = "__if_constraint__";

//! Tag on a parsed reducer carrying an aggregation key, `agg(...) BY (k1, ...)`. Child 0
//! is the reducer (possibly wrapped by QUALIFIED_REDUCER_TAG), children 1..N the key
//! expressions. An omitted or empty BY is the global group and carries no wrapper.
static constexpr const char *REDUCER_BY_TAG = "__reducer_by__";

//! Tag on a parsed frame expression, `AT(sel: e) OVER (...)` / `agg(FROM .. TO ..: e)
//! OVER (...)`. Children: [0] the navigated expression, [1] a VARCHAR constant holding
//! the DecideFrameSpec, [2] the order key, [3] the fill value (a NULL constant when there
//! is none), [4..] the WITHIN partition columns.
static constexpr const char *FRAME_TAG = "__decide_frame__";

//! Returns true if the alias is PER_CONSTRAINT_TAG
inline bool IsPerConstraintTag(const string &alias) {
	return HasDecideTag(alias, PER_CONSTRAINT_TAG);
}

//! Tag used by the parser to mark a reducer with a generation key inside it,
//! `agg(PER K: expr)`: one term per distinct value of K within the reducer's group,
//! which is how a relation's tuple identity (`PER D`) is counted once however many
//! rows the join repeated it on. children[0] is the aggregate, children[1..] the key's
//! columns or relations, and no key children is `PER ()`. The binder consumes and
//! discards it, folding the resolved key into the tag below.
static constexpr const char *QUALIFIED_REDUCER_TAG = "__qualified_reducer__";

//! Tag prefix recording a bound reducer's generation key (on BoundAggregateExpression
//! .alias). Format: "__qualified_by_<entity_scope_idx>__" — the index is into
//! LogicalDecide::entity_scopes, whose entry supplies the key the reducer
//! de-duplicates by (one term per distinct key value). A key is an entity scope that
//! may have no variable declared on it, so the two share one table of keys and one
//! mapping.
static constexpr const char *QUALIFIED_REDUCER_SCOPE_TAG_PREFIX = "__qualified_by_";

//! Tag prefix recording a bound reducer's aggregation key (on BoundAggregateExpression
//! .alias). Format: "__reduce_by_<entity_scope_idx>__". The reducer sums the rows of
//! the key's group that contains the generated instance; absent, it sums the whole
//! enclosing relation (`BY ()`).
static constexpr const char *REDUCE_BY_SCOPE_TAG_PREFIX = "__reduce_by_";

inline string MakeReduceByTag(idx_t entity_scope_idx) {
	return string(REDUCE_BY_SCOPE_TAG_PREFIX) + to_string(entity_scope_idx) + "__";
}

//! Builds the alias tag naming the entity scope a reducer is qualified by.
inline string MakeQualifiedReducerTag(idx_t entity_scope_idx) {
	return string(QUALIFIED_REDUCER_SCOPE_TAG_PREFIX) + to_string(entity_scope_idx) + "__";
}

//! Reads back the entity scope index from an alias written by MakeQualifiedReducerTag.
//! Returns false (leaving `scope_idx` untouched) when the alias carries no such tag.
inline bool TryParseQualifiedReducerTag(const string &alias, idx_t &scope_idx) {
	string digits;
	if (!ExtractDecideTagPayload(alias, QUALIFIED_REDUCER_SCOPE_TAG_PREFIX, digits)) {
		return false;
	}
	if (digits.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	scope_idx = static_cast<idx_t>(std::stoull(digits));
	return true;
}

//! Reads back the generation scope from a PER wrapper's alias. Returns false when the
//! alias carries neither tag (which a bound PER wrapper never does).
inline bool TryParseGenScopeTag(const string &alias, DecideScopeKind &kind, idx_t &scope_idx) {
	if (HasDecideTag(alias, GEN_GLOBAL_TAG)) {
		kind = DecideScopeKind::GLOBAL;
		scope_idx = DConstants::INVALID_INDEX;
		return true;
	}
	string digits;
	if (!ExtractDecideTagPayload(alias, GEN_SCOPE_TAG_PREFIX, digits) ||
	    digits.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	kind = DecideScopeKind::KEY;
	scope_idx = static_cast<idx_t>(std::stoull(digits));
	return true;
}

//! Reads back the entity scope index from an alias written by MakeReduceByTag.
inline bool TryParseReduceByTag(const string &alias, idx_t &scope_idx) {
	string digits;
	if (!ExtractDecideTagPayload(alias, REDUCE_BY_SCOPE_TAG_PREFIX, digits)) {
		return false;
	}
	if (digits.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	scope_idx = static_cast<idx_t>(std::stoull(digits));
	return true;
}

//! Alias tag naming the frame descriptor (`LogicalDecide::frames[i]`) a bound aggregate
//! navigates by: `AT(sel: e) OVER (...)` and `agg(FROM .. TO ..: e) OVER (...)` bind as
//! a SUM/AVG aggregate whose rows are the navigated positions rather than a group.
static constexpr const char *FRAME_REF_TAG_PREFIX = "__frame_ref_";

inline string MakeFrameRefTag(idx_t frame_idx) {
	return string(FRAME_REF_TAG_PREFIX) + to_string(frame_idx) + "__";
}

inline bool TryParseFrameRefTag(const string &alias, idx_t &frame_idx) {
	string digits;
	if (!ExtractDecideTagPayload(alias, FRAME_REF_TAG_PREFIX, digits)) {
		return false;
	}
	if (digits.find_first_not_of("0123456789") != string::npos) {
		return false;
	}
	frame_idx = static_cast<idx_t>(std::stoull(digits));
	return true;
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
//! Format: "__ne_clause_tag_<clause_idx>__", indexing `LogicalDecide::ne_clause_labels`.
static constexpr const char *NE_CLAUSE_TAG_PREFIX = "__ne_clause_tag_";

//! Tag marking a comparison whose LHS was an easy-direction MIN/MAX aggregate
//! (MAX(e) <= K, MIN(e) >= K) that the optimizer stripped to a per-row form.
//! Preserves the empty-WHEN rejection guard: an empty row set for what the user
//! wrote as MIN/MAX must reject, even though the optimized constraint is now
//! per-row. Set on the BoundComparisonExpression.alias during RewriteMinMax.
static constexpr const char *MINMAX_EASY_REWRITE_TAG = "__minmax_easy__";

//! Tag marking optimizer-generated helper constraints that define auxiliaries or
//! link rewrite machinery. These rows are rigid and must not be elastic-relaxed.
static constexpr const char *STRUCTURAL_CONSTRAINT_TAG = "__decide_structural_constraint__";

//! Tag marking a simple `x OP const` comparison whose entire meaning the optimizer
//! folded into the decision column's box (`LogicalDecide::absorbed_*_bounds`), so it
//! must not also be emitted as a model row. Set on the BoundComparisonExpression.alias
//! by DecideOptimizer::AbsorbVariableBounds; read by physical term extraction.
//!
//! The comparison deliberately stays in the tree rather than being replaced by a TRUE
//! placeholder: EXPLAIN renders the constraint list from this tree, and the user wrote
//! this bound, so it keeps rendering. The tag moves the *decision* upstream without
//! moving the rendering.
static constexpr const char *ABSORBED_BOUND_TAG = "__absorbed_bound__";

//! A private aggregate marker emitted by the DECIDE binder for norm(expr, p).
//! The marker is only a transport representation: DecideOptimizer must lower it
//! before physical planning. Its payload is one of `1`, `2`, `inf`, `0_auto`,
//! or `0_<positive-double>`.
static constexpr const char *NORM_MARKER_TAG_PREFIX = "__decide_norm_";

//! Semantic provenance stamped on the flattened value produced by an UNCORRELATED
//! scalar subquery. Shape alone cannot distinguish that query-wide column ref from
//! ordinary row data after PlanSubqueries.
static constexpr const char *QUERY_WIDE_VALUE_TAG = "__query_wide_value__";

//! Semantic provenance stamped on a flattened CORRELATED scalar subquery. It remains
//! row-varying; the tag exists so downstream diagnostics never quote DuckDB's internal
//! `SUBQUERY` placeholder as if it were user SQL.
static constexpr const char *ROW_VARYING_SUBQUERY_TAG = "__row_varying_subquery__";

//! Stable source-comparison identity. The payload indexes the per-DECIDE
//! ConstraintSourceInfo registry; it survives canonical rebuilding and optimizer
//! one-to-many rewrites but is never used to group elastic slacks.
static constexpr const char *SOURCE_CLAUSE_TAG_PREFIX = "__source_clause_";

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

inline bool TryParseSourceFragmentTag(const string &alias, idx_t &fragment_id) {
	return TryParseDecideIndexTag(alias, SOURCE_FRAGMENT_TAG_PREFIX, fragment_id);
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
//! TagAbsConstraintsForBigM (formerly ValidateAbsConstraintDirection).
static constexpr const char *ABS_NEEDS_BIGM_TAG = "__abs_needs_bigm__";

} // namespace duckdb
