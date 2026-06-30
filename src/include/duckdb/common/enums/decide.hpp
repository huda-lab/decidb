//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/common/enums/decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

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

inline bool IsRelaxableForElastic(ConstraintKind kind) {
	return kind == ConstraintKind::USER_PARAMETER;
}

//! How a relaxable row maps to a single user-editable knob, for elastic (infeasible)
//! diagnosis. Drives whether the rows a clause emits share ONE slack or get one each.
//!   PER_ROW_DATA   — the RHS is per-row data, or the rows are genuinely independent:
//!                    one slack per row, no single-knob edit (rolled into a conflict
//!                    summary). The safe default.
//!   SHARED_LITERAL — one user literal RHS fans into N rows (easy MIN/MAX `MAX(e)<=K`,
//!                    a per-row constraint with a constant RHS, a multi-instance bound):
//!                    the N rows of a (clause_id, group_key) block share ONE slack, so
//!                    the reported edit is the max overshoot, not the sum.
enum class ElasticShape : uint8_t { PER_ROW_DATA, SHARED_LITERAL };

//! Tag used to identify WHEN-conditional constraints throughout the pipeline
static constexpr const char *WHEN_CONSTRAINT_TAG = "__when_constraint__";

//! Tag used to identify PER-grouped constraints throughout the pipeline
static constexpr const char *PER_CONSTRAINT_TAG = "__per_constraint__";

//! Returns true if the alias is PER_CONSTRAINT_TAG
inline bool IsPerConstraintTag(const string &alias) {
	return alias == PER_CONSTRAINT_TAG;
}

//! Tag used to identify AVG→SUM rewritten aggregates (terms need coefficient scaling at execution)
static constexpr const char *AVG_REWRITE_TAG = "__avg_rewrite__";

//! Tag prefix for MIN/MAX hard-case indicator linking (on BoundAggregateExpression.alias)
//! Format: "__minmax_ind_<indicator_idx>_<min|max>__"
static constexpr const char *MINMAX_INDICATOR_TAG_PREFIX = "__minmax_ind_";

//! Tag prefix for not-equal indicator linking (on BoundComparisonExpression.alias)
//! Format: "__ne_ind_tag_<indicator_idx>__"
static constexpr const char *NE_INDICATOR_TAG_PREFIX = "__ne_ind_tag_";

//! Tag marking a comparison whose LHS was an easy-direction MIN/MAX aggregate
//! (MAX(e) <= K, MIN(e) >= K) that the optimizer stripped to a per-row form.
//! Preserves the empty-WHEN rejection guard: an empty row set for what the user
//! wrote as MIN/MAX must reject, even though the optimized constraint is now
//! per-row. Set on the BoundComparisonExpression.alias during RewriteMinMax.
static constexpr const char *MINMAX_EASY_REWRITE_TAG = "__minmax_easy__";

//! Tag marking optimizer-generated helper constraints that define auxiliaries or
//! link rewrite machinery. These rows are rigid and must not be elastic-relaxed.
static constexpr const char *STRUCTURAL_CONSTRAINT_TAG = "__decide_structural_constraint__";

//! Tag marking a per-row bound whose RHS was an UNCORRELATED scalar subquery
//! (e.g. `x <= (SELECT 5)`). PlanSubqueries flattens such a subquery into a
//! cross-joined column ref that is structurally indistinguishable from row data,
//! so foldability alone (IsFoldable) cannot tell it from a genuinely per-row RHS
//! (a correlated subquery / column). Detected before flattening and stamped on the
//! rewritten RHS column-ref alias (see plan_select_node.cpp), this tag tells the
//! elastic engine the RHS is one shared editable cap (ElasticShape::SHARED_LITERAL),
//! not per-row data — so infeasible diagnosis reports "Loosen x <= 5 to x <= 10"
//! instead of a data conflict. Correlated subqueries stay untagged (per-row).
static constexpr const char *SHARED_SCALAR_SUBQUERY_TAG = "__shared_scalar_subquery__";

//! Returns true if the alias is SHARED_SCALAR_SUBQUERY_TAG
inline bool IsSharedScalarSubqueryTag(const string &alias) {
	return alias == SHARED_SCALAR_SUBQUERY_TAG;
}

//! Tag prefix for ABS upper-bound constraint linking.
//! Format: "__abs_ub_pos_<y_idx>__" on C1 (aux >= inner)
//!         "__abs_ub_neg_<y_idx>__" on C2 (aux >= -inner)
//! Set on BoundComparisonExpression.alias by RewriteAbs when the aux needs the
//! Big-M upper envelope: either (a) sense==MAXIMIZE and ABS is in the objective,
//! or (b) ABS is in a constraint shape that does not naturally upper-bound aux
//! (e.g. ABS(...) >= K, ABS(...) = K). Both cases need the sign-indicator
//! binary y to pin aux = |inner| under solver pressure that would otherwise
//! let aux float free above |inner|.
static constexpr const char *ABS_UB_POS_TAG_PREFIX = "__abs_ub_pos_";
static constexpr const char *ABS_UB_NEG_TAG_PREFIX = "__abs_ub_neg_";

//! Tag set on BoundFunctionExpression.alias for ABS occurrences inside a
//! constraint shape that does not naturally upper-bound the auxiliary
//! (i.e. hard-direction ABS). Read by FindAndReplaceAbs, propagated to
//! AbsPairInfo::needs_bigm so RewriteAbs allocates a sign-indicator y and
//! emits the Big-M upper envelope at execution time. The tag is set by
//! TagAbsConstraintsForBigM (formerly ValidateAbsConstraintDirection).
static constexpr const char *ABS_NEEDS_BIGM_TAG = "__abs_needs_bigm__";

} // namespace duckdb
