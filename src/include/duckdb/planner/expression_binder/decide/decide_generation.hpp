//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/expression_binder/decide/decide_generation.hpp
//
// The well-definedness rule of DeciQL generation (spec §6.3): every value a
// generated constraint instance reads must be determined by the key that
// generates it. Stage 02 proves it from the schema and rejects what it cannot
// prove (the reject policy, OPEN-2).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"

namespace duckdb {

class BindContext;

//! A resolved generation key κ: per row, global, or the entity scope holding the key.
struct DecideGenerationScope {
	DecideScopeKind kind = DecideScopeKind::ROW;
	idx_t scope_idx = DConstants::INVALID_INDEX;

	static DecideGenerationScope Row() {
		return DecideGenerationScope();
	}
	static DecideGenerationScope Global() {
		DecideGenerationScope result;
		result.kind = DecideScopeKind::GLOBAL;
		return result;
	}
	static DecideGenerationScope Key(idx_t scope_idx) {
		DecideGenerationScope result;
		result.kind = DecideScopeKind::KEY;
		result.scope_idx = scope_idx;
		return result;
	}
};

//! Proves `Rθ ⊨ κ → e` for one bound expression: returns an empty string when every
//! column, decision, reducer group and nested key that `expr` reads is determined by
//! `scope`, else a message naming the first offender and how to repair it. The proof
//! uses only the schema -- a column is determined when it is in the key, when its
//! whole relation is in the key, or when a PRIMARY KEY / UNIQUE constraint of its
//! base table lies inside the key; a decision when its own key lies inside the key; a
//! reducer when its BY key does. A reducer's body is judged by that reducer's own
//! generation (its PER), not by the enclosing key, so the walk stops at it.
string CheckDeterminedByGeneration(const Expression &expr, DecideGenerationScope scope,
                                   const DecideQualifierContext &ctx, BindContext &bind_context);

//! Applies the rule to a whole bound SUCH THAT tree: each PER wrapper's body, guard
//! included, against its generation key. Throws BinderException on the first failure.
void ValidateDecideGenerationTree(const Expression &constraints, const DecideQualifierContext &ctx,
                                  BindContext &bind_context);

//! Tag every decision-free factor on a `BY (k)` reducer that the key `k` determines
//! (GROUP_WIDE_FACTOR_TAG), so the canonicalizer admits it as the reducer's scale.
void TagGroupWideReducerFactors(Expression &constraints, const DecideQualifierContext &ctx,
                                BindContext &bind_context);

//! Renders a generation key for messages: `()` for global, the written key otherwise.
string DescribeGenerationScope(DecideGenerationScope scope, const DecideQualifierContext &ctx);

} // namespace duckdb
