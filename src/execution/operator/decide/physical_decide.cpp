#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "duckdb/common/profiler.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/ilp_linearization.hpp"
#include "duckdb/decidb/decide_diagnostic.hpp"
#include "duckdb/decidb/decide_cast_policy.hpp"
#include "duckdb/decidb/decide_diagnostic_engines.hpp"
#include "duckdb/decidb/decide_router.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/decidb/solver_config.hpp"
#include <sys/resource.h>
#include <unistd.h>  // isatty / STDIN_FILENO — interactive-terminal check for the slow-solve prompt
#include <iostream>  // std::cin / std::getline — read the continuation decision at a time-limit stop

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Transform Helpers
//===--------------------------------------------------------------------===//
// These static functions replace BoundColumnRefExpression nodes with
// BoundReferenceExpression nodes so that DuckDB's ExpressionExecutor can
// evaluate expressions against data chunks (which use positional indices).

//! Collect every reducer in an expression. Descent stops at a match: a reducer inside a
//! reducer is not legal SQL, so there is nothing below one to find.
static void CollectReducers(const Expression &expr, vector<const Expression *> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		out.push_back(&expr);
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		CollectReducers(child, out);
	});
}

//! Transform a coefficient/value expression for ExpressionExecutor.
//! Handles: BOUND_COLUMN_REF, BOUND_FUNCTION, BOUND_CAST, BOUND_AGGREGATE,
//! BOUND_COMPARISON, BOUND_CONJUNCTION, BOUND_OPERATOR. Falls back to Copy() for others.
//!
//! `agg_substitutions` maps a reducer node to an extra chunk column carrying that
//! reducer's already-computed value, broadcast over the rows of its group. It is how a
//! right-hand side like `MIN(cap) + demand * 2` becomes evaluable per row: the reducer
//! collapses to a column reference and the rest of the tree is untouched. A tree built
//! this way is only valid against the augmented chunk layout, so it must never be
//! memoized by CachedTransformToChunkExpression.
static unique_ptr<Expression> TransformToChunkExpression(
    const Expression &expr, ClientContext &context,
    const std::unordered_map<const Expression *, idx_t> *agg_substitutions = nullptr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		vector<unique_ptr<Expression>> new_children;
		for (auto &child : func.children) {
			new_children.push_back(TransformToChunkExpression(*child, context, agg_substitutions));
		}
		unique_ptr<FunctionData> new_bind_info;
		if (func.bind_info) {
			new_bind_info = func.bind_info->Copy();
		}
		return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
		                                                           std::move(new_children), std::move(new_bind_info));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		auto transformed_child = TransformToChunkExpression(*cast.child, context, agg_substitutions);
		return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		if (agg_substitutions) {
			auto it = agg_substitutions->find(&expr);
			if (it != agg_substitutions->end()) {
				// The extra column is DOUBLE, but the parent node was bound against this
				// reducer's own type. Handing it a DOUBLE would not fail — it would
				// reinterpret the physical representation and compute a wrong value
				// (a DOUBLE 4.0 read as INTEGER is 0), so cast back explicitly.
				auto ref = make_uniq<BoundReferenceExpression>(LogicalType::DOUBLE, it->second);
				return BoundCastExpression::AddCastToType(context, std::move(ref),
				                                          expr.return_type);
			}
		}
		// Reached only where no substitution was prepared — an aggregate outside the
		// right-hand side, e.g. in a WHEN condition or a coefficient, where a reducer has
		// no meaning.
		throw InvalidInputException(
		    "DECIDE: an aggregate cannot be used here. Reducers are allowed on either side "
		    "of a constraint and in the objective, not inside a WHEN condition or a "
		    "coefficient.");
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		auto left = TransformToChunkExpression(*comp.left, context, agg_substitutions);
		auto right = TransformToChunkExpression(*comp.right, context, agg_substitutions);
		return make_uniq_base<Expression, BoundComparisonExpression>(comp.type, std::move(left), std::move(right));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
		for (auto &child : conj.children) {
			result->children.push_back(TransformToChunkExpression(*child, context, agg_substitutions));
		}
		return std::move(result);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		auto result = make_uniq<BoundOperatorExpression>(op_expr.type, op_expr.return_type);
		for (auto &child : op_expr.children) {
			result->children.push_back(TransformToChunkExpression(*child, context, agg_substitutions));
		}
		return std::move(result);
	} else {
		return expr.Copy();
	}
}

//! ChunkExprCache is declared in physical_decide.hpp (Finalize's private phase methods
//! need it in their signatures). Keyed on input Expression pointer identity — addresses
//! are stable for the duration of one Finalize. The cache owns lifetime; callers that
//! pass the returned reference to ExpressionExecutor must keep the cache alive until
//! the executor is no longer used.
//!
//! Only ever holds substitution-free trees. A tree built with `agg_substitutions` is
//! valid solely against the augmented chunk it was built for, so it is transformed
//! directly and never cached.
static const Expression &CachedTransformToChunkExpression(ChunkExprCache &cache,
                                                          const Expression &expr,
                                                          ClientContext &context) {
	auto it = cache.find(&expr);
	if (it != cache.end()) {
		return *it->second;
	}
	auto transformed = TransformToChunkExpression(expr, context);
	auto *raw = transformed.get();
	cache.emplace(&expr, std::move(transformed));
	return *raw;
}

//! Vectorized extraction of a chunk-result column into a vector<double>, multiplied by `sign`.
//! Throws InvalidInputException on NULL or non-finite values, citing `err_context`.
//! Fast path for DOUBLE; otherwise casts via VectorOperations::DefaultCast.
//!
//! `allow_infinite` relaxes the check to NaN-only, and is used exclusively for values
//! on the path to a model row's `rhs` — the row's own bound, and the reducer input
//! that a per-group bound is folded from. An infinite bound is not an error there:
//! it is the absence of a constraint (`<= +inf`) or a constraint nothing satisfies
//! (`>= +inf`), both of which the solver contract already expresses — the model
//! validator accepts a non-finite rhs and rejects only NaN, and the constant and
//! absorbed-bound RHS paths have always passed infinities straight through. A
//! coefficient has no such reading, so every other caller stays strict.
static void ExtractDoubleColumn(Vector &result_vec, idx_t count, double sign,
                                vector<double> &out, const char *err_context,
                                bool allow_infinite = false) {
	if (count == 0) {
		return;
	}
	UnifiedVectorFormat format;
	Vector cast_tmp(LogicalType::DOUBLE);
	Vector *src;
	if (result_vec.GetType().id() == LogicalTypeId::DOUBLE) {
		src = &result_vec;
	} else {
		VectorOperations::DefaultCast(result_vec, cast_tmp, count, false);
		src = &cast_tmp;
	}
	src->ToUnifiedFormat(count, format);
	auto data = UnifiedVectorFormat::GetData<double>(format);
	for (idx_t i = 0; i < count; i++) {
		idx_t idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			throw InvalidInputException(
				"DECIDE %s returned NULL at row %llu. "
				"NULL values are not allowed in optimization expressions. "
				"Use COALESCE() to handle NULLs or filter them with WHERE clause.",
				err_context, out.size());
		}
		double dv = data[idx];
		if (allow_infinite ? std::isnan(dv) : !std::isfinite(dv)) {
			throw InvalidInputException(
				"DECIDE %s contains invalid value (%s) at row %llu. "
				"Common causes:\n"
				"  • Division by zero in the expression\n"
				"  • Arithmetic overflow in calculations\n"
				"  • NULL values that propagated through math operations\n"
				"Check your expressions and input data.",
				err_context, allow_infinite ? "NaN" : "NaN or Infinity", out.size());
		}
		out.push_back(dv * sign);
	}
}

//! Streaming typed-hash grouping. Assigns group IDs during the chunk scan
//! instead of materializing all key Values up front. Representative key tuples
//! are stored once per group (size O(num_groups)), not once per row, and only
//! materialized on the (rare) hash-collision fallback path.
//!
//!   key_exprs       — pre-transformed expressions for chunk evaluation, one per key column
//!   row_filter      — return true to include row in grouping (empty function = include all)
//!   null_excludes   — true = any NULL in a key column maps the row to INVALID_INDEX (PER semantics);
//!                     false = NULL is part of the composite key (entity semantics)
//!   out_row_to_group — sized num_rows; INVALID_INDEX for excluded rows, [0..K) otherwise
//!   out_num_groups   — K, count of distinct groups in input order
static void BuildGroupIds(const vector<const Expression *> &key_exprs,
                          ClientContext &context,
                          ColumnDataCollection &data,
                          idx_t num_rows,
                          const std::function<bool(idx_t)> &row_filter,
                          bool null_excludes,
                          vector<idx_t> &out_row_to_group,
                          idx_t &out_num_groups,
                          vector<vector<Value>> *out_rep_keys = nullptr) {
	out_row_to_group.assign(num_rows, DConstants::INVALID_INDEX);
	out_num_groups = 0;
	if (num_rows == 0 || key_exprs.empty()) {
		return;
	}

	const idx_t num_key_cols = key_exprs.size();

	// Build one executor for all key columns; produces a multi-column result chunk per scan.
	ExpressionExecutor key_executor(context);
	vector<LogicalType> key_types;
	key_types.reserve(num_key_cols);
	for (auto *e : key_exprs) {
		key_executor.AddExpression(*e);
		key_types.push_back(e->return_type);
	}

	// rep_keys[c][gid] — key values for the representative row of group `gid`.
	// Size grows with num_groups, not num_rows.
	vector<vector<Value>> rep_keys(num_key_cols);
	std::unordered_multimap<hash_t, idx_t> hash_to_rep_group;

	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	DataChunk key_results;
	key_results.Initialize(context, key_types);
	Vector chunk_hashes(LogicalType::HASH);
	vector<UnifiedVectorFormat> key_udatas(num_key_cols);
	idx_t next_group = 0;
	idx_t row_offset = 0;
	while (data.Scan(scan, chunk)) {
		idx_t count = chunk.size();
		if (count == 0) {
			continue;
		}
		key_results.Reset();
		key_executor.Execute(chunk, key_results);

		VectorOperations::Hash(key_results.data[0], chunk_hashes, count);
		for (idx_t c = 1; c < num_key_cols; c++) {
			VectorOperations::CombineHash(chunk_hashes, key_results.data[c], count);
		}
		auto hashes_data = FlatVector::GetData<hash_t>(chunk_hashes);

		// UnifiedVectorFormat lets us check NULLness without materializing a Value.
		for (idx_t c = 0; c < num_key_cols; c++) {
			key_results.data[c].ToUnifiedFormat(count, key_udatas[c]);
		}

		for (idx_t i = 0; i < count; i++) {
			idx_t row = row_offset + i;
			if (row_filter && !row_filter(row)) {
				continue;
			}
			if (null_excludes) {
				bool has_null = false;
				for (idx_t c = 0; c < num_key_cols; c++) {
					const auto &u = key_udatas[c];
					if (!u.validity.RowIsValid(u.sel->get_index(i))) {
						has_null = true;
						break;
					}
				}
				if (has_null) {
					continue;
				}
			}
			hash_t h = hashes_data[i];
			auto range = hash_to_rep_group.equal_range(h);
			bool matched = false;
			for (auto it = range.first; it != range.second; ++it) {
				idx_t gid = it->second;
				bool eq = true;
				for (idx_t c = 0; c < num_key_cols; c++) {
					Value v = key_results.data[c].GetValue(i);
					const Value &rv = rep_keys[c][gid];
					if (v.IsNull() != rv.IsNull()) {
						eq = false;
						break;
					}
					if (v.IsNull()) {
						continue;
					}
					if (!(v == rv)) {
						eq = false;
						break;
					}
				}
				if (eq) {
					out_row_to_group[row] = gid;
					matched = true;
					break;
				}
			}
			if (!matched) {
				idx_t gid = next_group++;
				for (idx_t c = 0; c < num_key_cols; c++) {
					rep_keys[c].push_back(key_results.data[c].GetValue(i));
				}
				hash_to_rep_group.emplace(h, gid);
				out_row_to_group[row] = gid;
			}
		}
		row_offset += count;
	}

	if (row_offset != num_rows) {
		throw InternalException(
			"DECIDE BuildGroupIds: chunk scan produced %llu rows, expected %llu",
			row_offset, num_rows);
	}

	out_num_groups = next_group;
	if (out_rep_keys) {
		// Expose the per-group representative key values (group g's value for each key
		// column) — used by the unbounded diagnosis to label escaping categorical groups.
		*out_rep_keys = std::move(rep_keys);
	}
}

//! PerGroupCacheEntry/PerGroupCache are declared in physical_decide.hpp (Finalize's
//! private phase methods need PerGroupCache in their signatures). Cache for PER group
//! assignments: shares one full-data scan + group-map build across constraints/
//! objectives that use the same PER expression set. The cached value is the
//! *unfiltered* row→group mapping (BuildGroupIds run with row_filter=nullptr); each
//! call site then applies its own WHEN/local filter and remaps the surviving group IDs
//! to consecutive 0..K' to preserve today's "encounter-order, no holes" semantics.
//! Lifetime: one PerGroupCache per Finalize invocation; cleared at end.

//! Printable key for an (unfiltered) group: the per-key-column representative values
//! joined with ", " (composite PER key → `EU, 2024`); a NULL value renders "NULL".
static string FormatPerGroupKey(const vector<vector<Value>> &rep_keys, idx_t gid) {
	string s;
	for (idx_t c = 0; c < rep_keys.size(); c++) {
		if (gid >= rep_keys[c].size()) {
			continue;
		}
		const Value &v = rep_keys[c][gid];
		if (!s.empty()) {
			s += ", ";
		}
		if (v.IsNull()) {
			s += "NULL";
		} else {
			string rendered = v.ToString();
			s += rendered.empty() ? "''" : rendered;
		}
	}
	return s;
}

//! Hash a PER expression set + null_excludes flag into a size_t for the cache.
static size_t HashPerKey(const vector<unique_ptr<Expression>> &per_columns, bool null_excludes) {
	size_t h = std::hash<bool>{}(null_excludes);
	for (auto &e : per_columns) {
		size_t eh = static_cast<size_t>(e->Hash());
		h ^= eh + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
	}
	return h;
}

//! Returns true if the cache entry's expression set + null_excludes flag matches
//! the constraint/objective being evaluated.
static bool PerKeyMatches(const PerGroupCacheEntry &entry,
                          const vector<unique_ptr<Expression>> &per_columns,
                          bool null_excludes) {
	if (entry.null_excludes != null_excludes) return false;
	if (entry.exprs.size() != per_columns.size()) return false;
	for (idx_t i = 0; i < per_columns.size(); i++) {
		if (!entry.exprs[i]->Equals(*per_columns[i])) return false;
	}
	return true;
}

//! Look up (or build + cache) the unfiltered group ids for `per_columns`,
//! then materialize a filter-aware view in `out_row_group_ids` / `out_num_groups`.
//!
//! Filter semantics (preserved from BuildGroupIds): rows where row_filter(r) is
//! false (or NULL keys when null_excludes=true) get INVALID_INDEX in the output.
//! Surviving group IDs are remapped to consecutive 0..K' in their first-seen
//! row order so downstream code sees a dense [0..K') range exactly as today.
static void LookupOrBuildPerGroupIds(PerGroupCache &cache,
                                     const vector<unique_ptr<Expression>> &per_columns,
                                     ChunkExprCache &chunk_expr_cache,
                                     ClientContext &context,
                                     ColumnDataCollection &data,
                                     idx_t num_rows,
                                     bool null_excludes,
                                     const std::function<bool(idx_t)> &row_filter,
                                     vector<idx_t> &out_row_group_ids,
                                     idx_t &out_num_groups,
                                     vector<string> &out_group_labels,
                                     // Optional second map over the SAME group numbering but a
                                     // looser row filter. The right-hand side needs this: its
                                     // reducers must not inherit the LHS's aggregate-local WHEN,
                                     // which `row_filter` folds in. Groups that no strict row
                                     // reached emit no constraint, so their rows stay excluded.
                                     const std::function<bool(idx_t)> *loose_row_filter = nullptr,
                                     vector<idx_t> *out_loose_row_group_ids = nullptr) {
	size_t key = HashPerKey(per_columns, null_excludes);
	auto &bucket = cache[key];
	PerGroupCacheEntry *entry = nullptr;
	for (auto &e : bucket) {
		if (PerKeyMatches(e, per_columns, null_excludes)) {
			entry = &e;
			break;
		}
	}
	if (entry == nullptr) {
		bucket.emplace_back();
		entry = &bucket.back();
		entry->null_excludes = null_excludes;
		entry->exprs.reserve(per_columns.size());
		for (auto &e : per_columns) {
			entry->exprs.push_back(e.get());
		}
		// Build unfiltered group ids; BuildGroupIds already excludes NULL keys
		// when null_excludes=true.
		vector<const Expression *> key_exprs;
		key_exprs.reserve(per_columns.size());
		for (auto &col : per_columns) {
			key_exprs.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, *col, context));
		}
		std::function<bool(idx_t)> no_filter; // empty = include all
		BuildGroupIds(key_exprs, context, data, num_rows, no_filter, null_excludes,
		              entry->unfiltered_row_group_ids, entry->unfiltered_num_groups,
		              &entry->unfiltered_rep_keys);
	}

	// Apply per-call filter and remap surviving group IDs to consecutive 0..K'
	// in encounter order, matching the legacy BuildGroupIds output exactly.
	out_row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
	out_group_labels.clear();
	if (out_loose_row_group_ids) {
		out_loose_row_group_ids->assign(num_rows, DConstants::INVALID_INDEX);
	}
	if (entry->unfiltered_num_groups == 0 || num_rows == 0) {
		out_num_groups = 0;
		return;
	}
	vector<idx_t> remap(entry->unfiltered_num_groups, DConstants::INVALID_INDEX);
	idx_t next_remap = 0;
	for (idx_t r = 0; r < num_rows; r++) {
		idx_t unf_gid = entry->unfiltered_row_group_ids[r];
		if (unf_gid == DConstants::INVALID_INDEX) continue;
		if (row_filter && !row_filter(r)) continue;
		idx_t mapped = remap[unf_gid];
		if (mapped == DConstants::INVALID_INDEX) {
			mapped = next_remap++;
			remap[unf_gid] = mapped;
			// Reindex the printable key in lockstep with the dense 0..K' renumber:
			// mapped == out_group_labels.size() exactly here, so labels stay aligned.
			out_group_labels.push_back(FormatPerGroupKey(entry->unfiltered_rep_keys, unf_gid));
		}
		out_row_group_ids[r] = mapped;
	}
	out_num_groups = next_remap;

	if (out_loose_row_group_ids) {
		// Same `remap`, so a row lands in the group the model builder will emit for.
		for (idx_t r = 0; r < num_rows; r++) {
			idx_t unf_gid = entry->unfiltered_row_group_ids[r];
			if (unf_gid == DConstants::INVALID_INDEX) continue;
			if (loose_row_filter && *loose_row_filter && !(*loose_row_filter)(r)) continue;
			idx_t mapped = remap[unf_gid];
			if (mapped == DConstants::INVALID_INDEX) continue; // group emits no constraint
			(*out_loose_row_group_ids)[r] = mapped;
		}
	}
}


//! True when a bound contains a flattened scalar-subquery value. Its bound alias is
//! internal planning metadata (`SUBQUERY` plus a DECIDE tag), not SQL the user can edit.
//! Mixed data/subquery bounds therefore use the evaluated numeric fallback instead of
//! leaking an internal placeholder into a diagnostic suggestion.
static bool ContainsScalarSubqueryProvenance(const Expression &expr) {
	if (IsQueryWideValueTag(expr.GetAlias()) || IsRowVaryingSubqueryTag(expr.GetAlias())) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsScalarSubqueryProvenance(child);
		}
	});
	return found;
}





//! Entity scope a reducer is qualified by (`sum(D: ...)`), read back from the tag the
//! binder stamped on the aggregate; INVALID_INDEX when the reducer is unqualified.
//! The prepared terms already carry this on `qualifier_scope_idx`; a data-side reducer
//! on the RHS is evaluated here from the tree, so it reads the tag itself.
static idx_t QualifierScopeOf(const BoundAggregateExpression &agg) {
	idx_t scope_idx = DConstants::INVALID_INDEX;
	TryParseQualifiedReducerTag(agg.alias, scope_idx);
	return scope_idx;
}

// DecidB: reject an aggregate whose effective row set is empty (after WHEN
// filtering). An empty aggregate has no well-defined value — MIN(∅)=+∞ and
// MAX(∅)=-∞ cannot be represented in the MILP encoding, SUM(∅)=0 and AVG(∅)
// is undefined. Without this guard the hard-direction MIN/MAX z_k auxiliary
// floats free and silently vacates the outer constraint or objective.
static void RejectEmptyAggregate(idx_t effective_row_count, const char *what, const char *ctx) {
    if (effective_row_count == 0) {
        throw InvalidInputException(
            "DECIDE empty row set for %s in %s. "
            "An empty aggregate has no well-defined value; check your WHEN clause.",
            what, ctx);
    }
}

// Reduce an aggregate constraint's right-hand side to one value per group.
//
// A reduced constraint emits ONE row per group, but the RHS is still a column, so a
// bound that varies row to row has to collapse. Paper §3.2.1 fixes the semantics:
// without PER, the clause generates one instance per tuple, each pairing the reducer
// (evaluated over the whole selection) with that tuple's non-reduced values. The
// conjunction `LHS <= r_i` over every row is exactly `LHS <= min r_i`, so MIN for
// `<=` and MAX for `>=` is a derivation, not a policy choice. With PER we apply the
// same rule per partition.
//
// `=` and `<>` have no such collapse — differing values are contradictory (or
// unrelated) constraints, not a tighter one — so they are refused. Only when the
// values genuinely differ, though: that is a fact about the data, and rejecting on
// the query's shape alone would refuse a bound that happens to be constant.
//
// The reduced value is broadcast back over the group's rows, so every reader
// downstream keeps seeing a plain `rhs_values` column and needs no new field. Rows in
// no group keep their raw value; each reader either skips them or skips the whole
// constraint. When nothing varies the column is left untouched, which keeps the
// uniform-storage fast path (and the built model) exactly as it was.
//
// `group_ids` is the RHS's own map — the constraint's WHEN and PER, deliberately
// without the LHS's aggregate-local filters, the same map the RHS reducers are
// evaluated over. An aggregate-local WHEN scopes its own reducer, not the per-tuple
// fan-out, so `SUM(x) WHEN a <= cap` takes the tightest `cap` over every selected row
// rather than over the a-rows. Reducing over the looser map is safe for the broadcast
// too: it is a superset of the strict map, so every row a reader reaches is written.
static void ReduceAggregateRhsPerGroup(EvaluatedConstraint &ec,
                                       const vector<idx_t> &group_ids, idx_t num_rows,
                                       const string &rhs_text) {
    if (ec.rhs_values.IsUniform() || ec.rhs_values.Size() == 0) {
        return; // one value for every row already
    }
    const bool has_groups = !group_ids.empty();
    const idx_t num_groups = has_groups ? ec.num_groups : 1;
    if (num_groups == 0) {
        return;
    }
    const idx_t rows = MinValue<idx_t>(num_rows, ec.rhs_values.Size());

    const auto cmp = ec.comparison_type;
    const bool take_min = cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
                          cmp == ExpressionType::COMPARE_LESSTHAN;
    const bool take_max = cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
                          cmp == ExpressionType::COMPARE_GREATERTHAN;

    auto group_of = [&](idx_t row) -> idx_t {
        if (!has_groups) {
            return 0;
        }
        idx_t g = group_ids[row];
        return g >= num_groups ? DConstants::INVALID_INDEX : g;
    };

    vector<double> reduced(num_groups, 0.0);
    vector<bool> seen(num_groups, false);
    bool varies = false;

    for (idx_t row = 0; row < rows; row++) {
        idx_t g = group_of(row);
        if (g == DConstants::INVALID_INDEX) {
            continue;
        }
        double v = ec.rhs_values.Get(row);
        if (!seen[g]) {
            reduced[g] = v;
            seen[g] = true;
            continue;
        }
        if (v == reduced[g]) {
            continue;
        }
        if (!take_min && !take_max) {
            throw InvalidInputException(
                "%s takes more than one value here (%g and %g), so `%s` has no single "
                "bound. Use <= or >=, or compare against one value such as a scalar "
                "subquery.",
                rhs_text.c_str(), reduced[g], v,
                cmp == ExpressionType::COMPARE_EQUAL ? "=" : "<>");
        }
        varies = true;
        if (take_min ? (v < reduced[g]) : (v > reduced[g])) {
            reduced[g] = v;
        }
    }

    if (!varies) {
        return; // every group already agreed — leave the column alone
    }
    auto &col = ec.rhs_values.MutableDense();
    for (idx_t row = 0; row < rows; row++) {
        idx_t g = group_of(row);
        if (g == DConstants::INVALID_INDEX || !seen[g]) {
            continue;
        }
        col[row] = reduced[g];
    }
    ec.rhs_values.SyncSize();
}

//===--------------------------------------------------------------------===//
// Phase 2 shared evaluation helpers
//===--------------------------------------------------------------------===//

// TermFilterState is declared in physical_decide.hpp (EvaluateObjective's
// ObjectiveEvalState return value carries it across Finalize's phase methods).

// Which rows a relation-qualified reducer (`sum(D: ...)`) actually contributes.
// The join repeats each D tuple once per matching row; §3.2.2 asks for one term per
// tuple identity, and the binder has already guaranteed that every row sharing an
// identity carries the same value — so keeping the first row of each identity and
// dropping the rest yields exactly one contribution per identity, whichever row is
// kept. De-duplication runs inside the PER partition, after `when` selection and
// `per` partitioning, which is the construction order the paper pins.
static vector<bool> BuildQualifierKeepMask(const vector<EntityMapping> &mappings, idx_t scope_idx,
                                           const vector<idx_t> &row_group_ids, idx_t num_rows) {
    auto &mapping = mappings[scope_idx];
    vector<bool> keep(num_rows, false);
    // Group ids are dense in [0, num_groups) and entity ids in [0, num_entities),
    // so (group, entity) flattens into one dense index with no hashing.
    idx_t num_groups = 1;
    if (!row_group_ids.empty()) {
        for (idx_t gid : row_group_ids) {
            if (gid != DConstants::INVALID_INDEX && gid + 1 > num_groups) {
                num_groups = gid + 1;
            }
        }
    }
    vector<bool> seen(num_groups * mapping.num_entities, false);
    for (idx_t row = 0; row < num_rows; row++) {
        idx_t group = row_group_ids.empty() ? 0 : row_group_ids[row];
        if (group == DConstants::INVALID_INDEX) {
            continue; // excluded by WHEN or a NULL PER key — never a surviving identity
        }
        idx_t slot = group * mapping.num_entities + mapping.row_to_entity[row];
        if (!seen[slot]) {
            seen[slot] = true;
            keep[row] = true;
        }
    }
    return keep;
}

// Fold that mask into a term's filter state, so everything downstream — coefficient
// zeroing, AVG's denominator (which counts surviving rows and therefore becomes the
// distinct-identity count), the empty-aggregate guard — treats a duplicate row
// exactly as it treats a WHEN-excluded one.
static void ApplyQualifierToFilter(const vector<EntityMapping> &mappings, idx_t scope_idx,
                                   const vector<idx_t> &row_group_ids, idx_t num_rows,
                                   TermFilterState &state) {
    auto keep = BuildQualifierKeepMask(mappings, scope_idx, row_group_ids, num_rows);
    if (!state.has_filter) {
        state.mask = std::move(keep);
        state.has_filter = true;
        return;
    }
    for (idx_t row = 0; row < num_rows; row++) {
        state.mask[row] = state.mask[row] && keep[row];
    }
}

// Zero every row the (now possibly de-duplicated) mask drops. The WHEN pass already
// did this for its own mask; re-applying the combined mask is idempotent.
static void MaskCoefficientColumn(CoefficientColumn &column, const vector<bool> &mask) {
    auto &values = column.MutableDense();
    for (idx_t row = 0; row < values.size(); row++) {
        if (!mask[row]) {
            values[row] = 0.0;
        }
    }
}

// Evaluate N boolean filter expressions in a single scan over `data`.
static vector<vector<bool>> EvaluateBooleanMasks(const vector<const Expression *> &conditions,
                                                  ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                                  ColumnDataCollection &data, idx_t num_rows) {
    if (conditions.empty()) return {};

    ExpressionExecutor cond_executor(context);
    for (auto *cond : conditions) {
        cond_executor.AddExpression(
            CachedTransformToChunkExpression(chunk_expr_cache, *cond, context));
    }

    vector<vector<bool>> masks(conditions.size());
    for (auto &m : masks) m.reserve(num_rows);

    vector<LogicalType> result_types(conditions.size(), LogicalType::BOOLEAN);

    ColumnDataScanState cond_scan_state;
    data.InitializeScan(cond_scan_state);
    DataChunk cond_chunk;
    cond_chunk.Initialize(context, data.Types());

    while (data.Scan(cond_scan_state, cond_chunk)) {
        DataChunk cond_result;
        cond_result.Initialize(context, result_types);
        cond_executor.Execute(cond_chunk, cond_result);

        for (idx_t col = 0; col < conditions.size(); col++) {
            auto &vec = cond_result.data[col];
            for (idx_t row = 0; row < cond_chunk.size(); row++) {
                Value val = vec.GetValue(row);
                masks[col].push_back(val.IsNull() ? false : val.GetValue<bool>());
            }
        }
    }
    return masks;
}

static vector<bool> EvaluateBooleanMask(const Expression &condition, ChunkExprCache &chunk_expr_cache,
                                        ClientContext &context, ColumnDataCollection &data, idx_t num_rows) {
    return EvaluateBooleanMasks({&condition}, chunk_expr_cache, context, data, num_rows)[0];
}

// Reduce one data-only reducer on the right-hand side to a value per group.
//
// Implements the construction order paper §3.2.2 fixes for every reducer:
//
//     when selection -> per partitioning -> qualifier de-duplication -> aggregation
//
// The first two arrive as `group_ids` (built from the constraint's WHEN and PER,
// deliberately without the LHS's aggregate-local filters). This adds the reducer's
// own WHEN, then the relation-qualifier de-duplication, then folds.
//
// Unlike the left-hand side, nothing here can be expressed as a coefficient: the
// LHS reduces by *summing a column*, which is why only SUM and AVG could ever be
// moved there and MIN/MAX/COUNT were refused. Folding per kind is what removes
// that asymmetry.
static vector<double> EvaluateRhsReducerPerGroup(const BoundAggregateExpression &agg,
                                                 const vector<idx_t> &group_ids, idx_t num_groups,
                                                 const vector<EntityMapping> &entity_mappings,
                                                 ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                                 ColumnDataCollection &data, idx_t num_rows) {
    const bool has_groups = !group_ids.empty();
    const idx_t groups = has_groups ? num_groups : 1;
    auto group_of = [&](idx_t row) -> idx_t {
        return has_groups ? group_ids[row] : 0;
    };

    // Stage 1: the reducer's own WHEN (`SUM(b) WHEN w`), which scopes this reducer
    // and nothing else.
    vector<bool> keep;
    if (agg.filter) {
        keep = EvaluateBooleanMask(*agg.filter, chunk_expr_cache, context, data, num_rows);
    }
    // Stage 2: relation-qualified reducers (`sum(D: cost)`) contribute once per
    // distinct entity, not once per joined row.
    idx_t scope_idx = QualifierScopeOf(agg);
    if (scope_idx != DConstants::INVALID_INDEX) {
        auto dedup = BuildQualifierKeepMask(entity_mappings, scope_idx, group_ids, num_rows);
        if (keep.empty()) {
            keep = std::move(dedup);
        } else {
            for (idx_t row = 0; row < num_rows && row < keep.size(); row++) {
                keep[row] = keep[row] && dedup[row];
            }
        }
    }

    const string name = StringUtil::Lower(agg.function.name);
    const bool is_count = name == "count" || name == "count_star";
    const bool is_avg = name == "avg" || HasDecideTag(agg.alias, AVG_REWRITE_TAG);
    const bool is_min = name == "min";
    const bool is_max = name == "max";
    const bool is_sum = name == "sum";
    if (!is_count && !is_avg && !is_min && !is_max && !is_sum) {
        throw InvalidInputException(
            "DECIDE constraint right-hand side: %s is not supported as a bound. "
            "Use SUM, AVG, MIN, MAX or COUNT, or pre-compute the value in a scalar "
            "subquery.",
            agg.function.name);
    }
    // A data-only AVG reaches here as a genuine AVG node: DecideOptimizer's
    // AVG->SUM rewrite deliberately skips decision-free aggregates, because
    // rebinding one as SUM redeclares it with SUM's integral type while its value
    // stays fractional — and this is the one place a reducer's value is handed
    // back to a surrounding expression that was bound against that declared type.
    // Left as AVG, the round trip is DOUBLE->DOUBLE and the fold below is exact.

    // COUNT(*) has no argument to evaluate; every other kind reduces one column.
    vector<double> values;
    if (!agg.children.empty()) {
        ExpressionExecutor arg_executor(context);
        const Expression &arg =
            CachedTransformToChunkExpression(chunk_expr_cache, *agg.children[0], context);
        arg_executor.AddExpression(arg);
        values.reserve(num_rows);
        ColumnDataScanState scan;
        data.InitializeScan(scan);
        DataChunk in_chunk;
        in_chunk.Initialize(context, data.Types());
        DataChunk out_chunk;
        out_chunk.Initialize(context, vector<LogicalType>{arg.return_type});
        while (data.Scan(scan, in_chunk)) {
            out_chunk.Reset();
            arg_executor.Execute(in_chunk, out_chunk);
            // A reducer's input feeds a bound, so it follows the same rule as
            // every other RHS: ±inf is a value, not an error. A group whose
            // MAX(cap) folds to +inf is a group with no upper bound, and one
            // whose bound points out of reach is an infeasibility the solver
            // reports naming the clause — neither is ours to refuse here.
            // Reading it as an error also made the outcome depend on spelling:
            // `MIN(x) <= 1e1000::DOUBLE PER g` was accepted and classified per
            // group while `MIN(x) <= MAX(cap) PER g` was rejected wholesale.
            // NaN stays refused, here and in the per-row extraction that reads
            // this value back, so `MAX(cap) - MIN(cap)` over infinities is
            // still caught.
            ExtractDoubleColumn(out_chunk.data[0], in_chunk.size(), 1.0, values,
                                "constraint right-hand side aggregate",
                                /*allow_infinite=*/true);
        }
    }

    // Stage 3: fold.
    vector<double> acc(groups, 0.0);
    vector<idx_t> counts(groups, 0);
    for (idx_t row = 0; row < num_rows; row++) {
        idx_t g = group_of(row);
        if (g == DConstants::INVALID_INDEX || g >= groups) {
            continue;
        }
        if (!keep.empty() && !keep[row]) {
            continue;
        }
        double v = values.empty() ? 0.0 : (row < values.size() ? values[row] : 0.0);
        if (counts[g] == 0) {
            acc[g] = is_count ? 1.0 : v;
        } else if (is_min) {
            acc[g] = MinValue<double>(acc[g], v);
        } else if (is_max) {
            acc[g] = MaxValue<double>(acc[g], v);
        } else if (is_count) {
            acc[g] += 1.0;
        } else {
            acc[g] += v; // SUM, and AVG's numerator
        }
        counts[g]++;
    }
    for (idx_t g = 0; g < groups; g++) {
        // An empty reducer has no value — MIN(∅) and MAX(∅) are not representable
        // and AVG(∅) is undefined. Same rule the left-hand side already enforces.
        RejectEmptyAggregate(counts[g], "aggregate", "constraint right-hand side");
        if (is_avg) {
            acc[g] /= static_cast<double>(counts[g]);
        }
    }
    return acc;
}

// Evaluate every bilinear term's coefficient expression (constraint or objective side:
// the two are the same shape, differing only in which prepared-model term type feeds
// in and which struct the caller stages results into — both of which are field-
// identical, so this returns the one shared EvaluatedConstraint::BilinearTerm either
// way). Batches every term's coefficient expression into a single ExpressionExecutor
// and scans `data` once, then zeroes rows `filters[i]` excludes and, if given, rows
// `extra_mask` excludes (the objective side's query-level WHEN; the constraint side
// has no equivalent second mask here since its WHEN is already folded upstream).
// AVG scaling is deliberately left to the caller: the constraint side applies it
// immediately per-constraint, the objective side defers it until its PER-grouping is
// resolved in Phase 3 — a real difference in control flow, not something to collapse.
template <class BilinearTermSource>
static vector<EvaluatedConstraint::BilinearTerm> EvaluateBilinearTerms(
    const vector<BilinearTermSource> &terms, const vector<TermFilterState> &filters,
    ChunkExprCache &chunk_expr_cache, ClientContext &context, ColumnDataCollection &data,
    idx_t num_rows, const char *coefficient_err_label, const vector<bool> *extra_mask = nullptr) {
    const idx_t num_bl = terms.size();
    vector<EvaluatedConstraint::BilinearTerm> ebts(num_bl);
    for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
        auto &bt = terms[term_idx];
        ebts[term_idx].var_a = bt.var_a;
        ebts[term_idx].var_b = bt.var_b;
    }

    vector<idx_t> bl_route; // result column index → term index
    vector<LogicalType> bl_types;
    ExpressionExecutor bl_executor(context);
    for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
        auto &bt = terms[term_idx];
        if (!bt.coefficient) {
            ebts[term_idx].row_coefficients.AssignScalar(num_rows, static_cast<double>(bt.sign));
            continue;
        }
        const Expression &cached =
            CachedTransformToChunkExpression(chunk_expr_cache, *bt.coefficient, context);
        bl_types.push_back(cached.return_type);
        bl_executor.AddExpression(cached);
        bl_route.push_back(term_idx);
        ebts[term_idx].row_coefficients.Reserve(num_rows);
    }

    if (!bl_route.empty()) {
        ColumnDataScanState bl_scan;
        data.InitializeScan(bl_scan);
        DataChunk bl_chunk;
        bl_chunk.Initialize(context, data.Types());
        DataChunk bl_results;
        bl_results.Initialize(context, bl_types);
        while (data.Scan(bl_scan, bl_chunk)) {
            bl_results.Reset();
            bl_executor.Execute(bl_chunk, bl_results);
            for (idx_t j = 0; j < bl_route.size(); j++) {
                idx_t term_idx = bl_route[j];
                auto &col = ebts[term_idx].row_coefficients.MutableDense();
                ExtractDoubleColumn(bl_results.data[j], bl_chunk.size(), terms[term_idx].sign, col,
                                    coefficient_err_label);
                ebts[term_idx].row_coefficients.SyncSize();
            }
        }
    }

    for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
        auto &ebt = ebts[term_idx];
        if (filters[term_idx].has_filter) {
            auto &mask = filters[term_idx].mask;
            auto &col = ebt.row_coefficients.MutableDense();
            for (idx_t row = 0; row < col.size(); row++) {
                if (!mask[row]) {
                    col[row] = 0.0;
                }
            }
        }
        if (extra_mask) {
            auto &col = ebt.row_coefficients.MutableDense();
            for (idx_t row = 0; row < col.size(); row++) {
                if (!(*extra_mask)[row]) {
                    col[row] = 0.0;
                }
            }
        }
    }
    return ebts;
}

// AVG denominator scaling, shared by the constraint and objective sides and by the
// linear/bilinear (single-column) and quadratic-inner (sqrt-scaled) cases. `when_mask`
// is only meaningful in the ungrouped branch — the constraint side never passes one
// because its WHEN is already folded into `row_group_ids` upstream; the objective side
// passes its query-level WHEN mask because `row_group_ids` can be empty (PER is
// optional) while WHEN still needs to gate the denominator.
static void ScaleAvgRows(CoefficientColumn &col, bool has_filter, const vector<bool> &filter_mask,
                         bool quadratic_inner, idx_t num_rows, const vector<idx_t> &row_group_ids,
                         idx_t num_groups, const vector<bool> *when_mask = nullptr) {
    auto &coefficients = col.MutableDense();
    if (row_group_ids.empty()) {
        idx_t denominator = 0;
        for (idx_t row = 0; row < num_rows; row++) {
            if (when_mask && !(*when_mask)[row]) {
                continue;
            }
            if (!has_filter || filter_mask[row]) {
                denominator++;
            }
        }
        if (denominator == 0) {
            std::fill(coefficients.begin(), coefficients.end(), 0.0);
            return;
        }
        double scale = quadratic_inner ? 1.0 / std::sqrt(static_cast<double>(denominator))
                                       : 1.0 / static_cast<double>(denominator);
        for (auto &coefficient : coefficients) {
            coefficient *= scale;
        }
        return;
    }

    vector<idx_t> group_counts(num_groups, 0);
    for (idx_t row = 0; row < num_rows; row++) {
        idx_t gid = row_group_ids[row];
        if (gid == DConstants::INVALID_INDEX) {
            continue;
        }
        if (!has_filter || filter_mask[row]) {
            group_counts[gid]++;
        }
    }
    for (idx_t row = 0; row < coefficients.size(); row++) {
        idx_t gid = row_group_ids[row];
        if (gid == DConstants::INVALID_INDEX || group_counts[gid] == 0) {
            coefficients[row] = 0.0;
            continue;
        }
        double scale = quadratic_inner ? 1.0 / std::sqrt(static_cast<double>(group_counts[gid]))
                                       : 1.0 / static_cast<double>(group_counts[gid]);
        coefficients[row] *= scale;
    }
}

//===--------------------------------------------------------------------===//
// Constructor
//===--------------------------------------------------------------------===//

PhysicalDecide::PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality, 
                    unique_ptr<PhysicalOperator> child, idx_t decide_index, 
                    vector<unique_ptr<Expression>> decide_variables,
                    unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                    unique_ptr<Expression> decide_objective)
    : PhysicalOperator(PhysicalOperatorType::DECIDE, std::move(types), estimated_cardinality)
    , decide_index(decide_index)
    , decide_variables(std::move(decide_variables))
    , decide_constraints(std::move(decide_constraints))
    , decide_sense(decide_sense)
    , decide_objective(std::move(decide_objective)) {
    children.push_back(std::move(child));
    for (idx_t i = 0; i < this->decide_variables.size(); i++) {
        auto &var = this->decide_variables[i]->Cast<BoundColumnRefExpression>();
        decide_variable_map[var.binding] = i;
    }
}

SolverBackend PhysicalDecide::PlannedSolverBackend() const {
	SolverBackend backend = SolverRegistry::Find(solver_backend_name);
	if (!backend.IsValid()) {
		// Physical planning always records a name (ChooseDecideSolver runs there when the
		// DECIDE optimizer did not), so an unresolvable one means the plan and the
		// registry disagree — an internal defect, never a user error.
		throw InternalException("DECIDE reached the solver with no backend named on the plan");
	}
	return backend;
}

//===--------------------------------------------------------------------===//
// EXPLAIN Support
//===--------------------------------------------------------------------===//

string PhysicalDecide::GetName() const {
	return "DECIDE";
}

InsertionOrderPreservingMap<string> PhysicalDecide::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;

	string vars_info;
	idx_t user_var_count = decide_variables.size() - num_auxiliary_vars;
	for (idx_t i = 0; i < user_var_count; i++) {
		if (i > 0) {
			vars_info += "\n";
		}
		vars_info += decide_variables[i]->GetName();
	}
	result["Variables"] = vars_info;

	if (decide_objective) {
		// Render through the same tagged-expression walker as constraints so
		// WHEN/PER wrappers print as postfix suffixes (e.g. "SUM(x) WHEN c")
		// rather than the raw internal tag or a generic conjunction form.
		string obj_info = (decide_sense == DecideSense::MAXIMIZE) ? "MAXIMIZE " : "MINIMIZE ";
		vector<string> objective_strs;
		CollectDecideExpressionStrings(*decide_objective, source_fragments, entity_scopes, objective_strs);
		for (idx_t i = 0; i < objective_strs.size(); i++) {
			if (i > 0) {
				obj_info += "\n";
			}
			obj_info += objective_strs[i];
		}
		result["Objective"] = obj_info;
	} else {
		result["Objective"] = "FEASIBILITY";
	}

	if (decide_constraints) {
		vector<string> constraint_strs;
		CollectDecideExpressionStrings(*decide_constraints, source_fragments, entity_scopes, constraint_strs);
		string constraints_info;
		for (idx_t i = 0; i < constraint_strs.size(); i++) {
			if (i > 0) {
				constraints_info += "\n";
			}
			constraints_info += constraint_strs[i];
		}
		result["Constraints"] = constraints_info;
	}

	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

//===--------------------------------------------------------------------===//
// Multi-variable per-row constraint helpers
//===--------------------------------------------------------------------===//


//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//

//! Bound absorption is decided at layer 05 (DecideOptimizer::AbsorbVariableBounds).
//! `UserBoundSpec` and the `ABSORBED_LOWER_UNSET` sentinel live on LogicalDecide with
//! it; execution reads the resulting box rather than deriving one.
using UserBoundSpec = LogicalDecide::UserBoundSpec;
static constexpr double ABSORBED_LOWER_UNSET = LogicalDecide::ABSORBED_LOWER_UNSET;

class DecideGlobalSinkState : public GlobalSinkState {
public:
	explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
	    : data(context, op.children[0]->GetTypes()), context(context), op(op),
	      canonicalizer(context, op.decide_index, op.variable_scopes) {
        // The decision column box was resolved by DecideOptimizer::AbsorbVariableBounds.
        // A variable still at ABSORBED_LOWER_UNSET is one the query never lowered, and
        // Finalize resolves it to the default 0 floor.
        idx_t num_decide_vars = op.decide_variables.size();
        absorbed_lower_bounds = op.absorbed_lower_bounds;
        absorbed_upper_bounds = op.absorbed_upper_bounds;
        user_absorbed_bounds = op.user_absorbed_bounds;

        // A user bound that contradicts the variable's INTRINSIC domain (a non-negative
        // REAL/INTEGER's `>= 0` floor, or a BOOLEAN's `0/1` box) is a deterministic semantic
        // error, not a constraint conflict to diagnose — loosening can't help, the type can.
        // Raise a precise static error here (the main pipeline) so it never reaches the
        // elastic engine, and so it supersedes Build's generic "conflicting bounds" throw.
        //   - `x <= -1` (REAL): upper below the non-negativity floor → infeasible. But
        //     `x = -1` or `x <= -1 AND x >= -5` explicitly lower the floor below 0, so the
        //     guard is `U < 0 AND L >= 0` (floor still at/above the intrinsic 0).
        //   - `x >= 2` / `x = 2` (BOOLEAN): lower above the 0/1 ceiling → infeasible.
        // A purely user-vs-user inverted box (e.g. `x >= 5 AND x <= 1`, both >= 0) does NOT
        // match and proceeds to the elastic engine, which reports a least-change loosen.
        for (idx_t var = 0; var < num_decide_vars; var++) {
            bool is_bool = var < op.is_boolean_var.size() && op.is_boolean_var[var];
            double L = (absorbed_lower_bounds[var] <= ABSORBED_LOWER_UNSET)
                           ? 0.0
                           : absorbed_lower_bounds[var];
            double U = absorbed_upper_bounds[var];
            const string vname = op.decide_variables[var]->GetName();
            const char *domain = is_bool ? "BOOLEAN (0 or 1)" : "non-negative (>= 0)";
            if (U < 0.0 && L >= 0.0) {
                throw InvalidInputException(StringUtil::Format(
                    "DECIDE optimization is infeasible: %s <= %g cannot hold because %s is %s. "
                    "%s", vname, U, vname, domain,
                    is_bool ? "Drop the bound or change the variable's type."
                            : "Add an explicit lower bound if it may be negative."));
            }
            if (is_bool && L > 1.0) {
                throw InvalidInputException(StringUtil::Format(
                    "DECIDE optimization is infeasible: %s >= %g cannot hold because %s is "
                    "BOOLEAN (0 or 1). Drop the bound or change the variable's type.",
                    vname, L, vname));
            }
        }

        // Minimal: keep constructor lean; detailed solver output comes from HiGHS
    }

    mutex lock;
    // This collection will hold all the data from the child operator
    ColumnDataCollection data;

    //! Needed to evaluate coefficient expressions against the materialized rows.
    ClientContext &context;

    const PhysicalDecide &op;
    DecideCanonicalizer canonicalizer;

    //! The flattened constraints and objective, decided by BuildDecidePreparedModel
    //! (stage 05). Aliases rather than copies: this state evaluates their
    //! coefficients against the data, it does not derive or alter their shape.
    const vector<unique_ptr<DecideConstraint>> &constraints = op.prepared.constraints;
    const unique_ptr<DecideObjective> &objective = op.prepared.objective;

    //! Local copies of the decision column box and the absorbed-bound records that
    //! DecideOptimizer::AbsorbVariableBounds resolved. Finalize copies the box into
    //! solver_input; the infeasible diagnosis re-emits the records as slackable rows.
    vector<double> absorbed_lower_bounds;
    vector<double> absorbed_upper_bounds;
    vector<UserBoundSpec> user_absorbed_bounds;

    //===--------------------------------------------------------------------===//
    // Evaluated Coefficients (Phase 2)
    //===--------------------------------------------------------------------===//

    vector<EvaluatedConstraint> evaluated_constraints;
    vector<CoefficientColumn> evaluated_objective_coefficients;  // [term_idx]
    vector<idx_t> objective_variable_indices;

    // Quadratic objective: evaluated inner linear expression coefficients
    vector<CoefficientColumn> evaluated_quadratic_coefficients;  // [term_idx]
    vector<idx_t> quadratic_variable_indices;
    bool has_quadratic_objective = false;
    double quadratic_sign = 1.0;

    // Bilinear objective: pairs of different decide variables with data coefficients.
    // Reuses EvaluatedConstraint::BilinearTerm (solver_input.hpp) rather than declaring
    // a field-identical duplicate — the two are populated by the same shared evaluator.
    vector<EvaluatedConstraint::BilinearTerm> evaluated_bilinear_terms;

    vector<double> ilp_solution;
    VarIndexer var_indexer;  // For mapping (var_idx, row) to solution indices
};

class DecideLocalSinkState : public LocalSinkState {
public:
    explicit DecideLocalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()) {
        data.InitializeAppend(append_state);
    }

    // A local collection to buffer chunks before merging into the global state
    ColumnDataCollection data;
    ColumnDataAppendState append_state;
};

unique_ptr<GlobalSinkState> PhysicalDecide::GetGlobalSinkState(ClientContext &context) const {
    return make_uniq_base<GlobalSinkState, DecideGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalDecide::GetLocalSinkState(ExecutionContext &context) const {
    return make_uniq_base<LocalSinkState, DecideLocalSinkState>(context.client, *this);
}

SinkResultType PhysicalDecide::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
    auto &lstate = input.local_state.Cast<DecideLocalSinkState>();
    lstate.data.Append(lstate.append_state, chunk);
    return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDecide::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    auto &lstate = input.local_state.Cast<DecideLocalSinkState>();

    lock_guard<mutex> guard(gstate.lock);
    gstate.data.Combine(lstate.data);

    return SinkCombineResultType::FINISHED;
}

//! Builds the categorical-candidate groupings the unbounded engine consumes to
//! characterize *which* instances of an escaping variable run away. Operator-bound
//! (it reads the executor chunks, entity scopes, and BuildGroupIds), so it lives
//! here rather than in the pure router/engine units; Finalize just invokes it.
//!
//! A stateful functor (not a set of lambdas) so its lazily-built caches — the
//! row-scoped candidates and the per-scope entity candidates — survive across the
//! many calls the engine makes, one per escaping decision variable. The stored
//! references point at Finalize-locals that outlive the engine call.
namespace {
struct UnboundedCandidateProvider {
	ClientContext &context;
	ColumnDataCollection &data;
	vector<LogicalType> types;
	const DecideDiagParams &params;
	const vector<string> &input_column_names;
	const VarIndexer &var_indexer;
	const vector<EntityScopeInfo> &entity_scopes;
	const vector<EntityMapping> &entity_mappings;
	idx_t num_rows;

	//! Row-scoped candidate columns are variable-independent — built once.
	vector<ColumnGrouping> row_candidates;
	bool row_candidates_built = false;
	//! Entity-scoped candidates cached per scope.
	std::map<idx_t, vector<ColumnGrouping>> entity_candidates;

	bool ColumnHasHarvestedName(idx_t ci) const {
		return ci < input_column_names.size() && !input_column_names[ci].empty();
	}

	//! Build a single-column row grouping, keeping it only if it is categorical
	//! (2 ≤ distinct ≤ max(min_categories, ratio × denom)).
	bool BuildRowGrouping(idx_t ci, idx_t denom, ColumnGrouping &out) {
		// Suppress rules over columns whose source name we never resolved (e.g.
		// referenced only in the outer SELECT): naming them with a positional
		// `colN` the user never wrote is worse than staying silent.
		if (!ColumnHasHarvestedName(ci)) {
			return false;
		}
		BoundReferenceExpression ref(types[ci], ci);
		vector<const Expression *> keys {&ref};
		vector<idx_t> row_to_group;
		idx_t num_groups = 0;
		vector<vector<Value>> rep_keys;
		BuildGroupIds(keys, context, data, num_rows, std::function<bool(idx_t)> {},
		              /*null_excludes=*/false, row_to_group, num_groups, &rep_keys);
		idx_t cap = std::max(params.min_categories, (idx_t)(params.categorical_ratio * (double)denom));
		if (num_groups < 2 || num_groups > cap || rep_keys.empty()) {
			return false;
		}
		out.column = input_column_names[ci];
		out.instance_to_group = std::move(row_to_group); // row-indexed
		out.group_value.resize(num_groups);
		for (idx_t g = 0; g < num_groups && g < rep_keys[0].size(); g++) {
			out.group_value[g] = rep_keys[0][g].IsNull() ? "NULL" : rep_keys[0][g].ToString();
		}
		return true;
	}

	vector<ColumnGrouping> &GetRowCandidates() {
		if (!row_candidates_built) {
			for (idx_t ci = 0; ci < types.size(); ci++) {
				ColumnGrouping cg;
				if (BuildRowGrouping(ci, num_rows, cg)) {
					row_candidates.push_back(std::move(cg));
				}
			}
			row_candidates_built = true;
		}
		return row_candidates;
	}

	//! Lift a row-indexed grouping to entity granularity. The lift is accepted only
	//! when every joined row for an entity maps to the same categorical group; this
	//! lets dimension-table labels characterize entity escapes without inventing a
	//! single value for genuinely one-to-many columns.
	bool LiftRowGroupingToEntities(const ColumnGrouping &rowcg, const EntityMapping &mapping, idx_t num_entities,
	                               ColumnGrouping &out) {
		out.column = rowcg.column;
		out.group_value = rowcg.group_value;
		out.instance_to_group.assign(num_entities, DConstants::INVALID_INDEX);
		vector<bool> seen(num_entities, false);
		for (idx_t r = 0; r < num_rows && r < mapping.row_to_entity.size() && r < rowcg.instance_to_group.size(); r++) {
			idx_t e = mapping.row_to_entity[r];
			if (e >= num_entities) {
				continue;
			}
			idx_t g = rowcg.instance_to_group[r];
			if (!seen[e]) {
				out.instance_to_group[e] = g;
				seen[e] = true;
			} else if (out.instance_to_group[e] != g) {
				return false;
			}
		}
		return true;
	}

	//! Entity-scoped candidate columns are any named DECIDE-clause input columns
	//! whose value is constant within each entity; grouping is lifted to entity
	//! granularity after the constancy check.
	vector<ColumnGrouping> &GetEntityCandidates(idx_t scope_idx) {
		auto found = entity_candidates.find(scope_idx);
		if (found != entity_candidates.end()) {
			return found->second;
		}
		vector<ColumnGrouping> cands;
		auto &mapping = entity_mappings[scope_idx];
		idx_t num_entities = mapping.num_entities;
		for (idx_t ci = 0; ci < types.size(); ci++) {
			ColumnGrouping rowcg;
			if (!BuildRowGrouping(ci, num_entities, rowcg)) {
				continue;
			}
			ColumnGrouping ecg;
			if (!LiftRowGroupingToEntities(rowcg, mapping, num_entities, ecg)) {
				continue;
			}
			cands.push_back(std::move(ecg));
		}
		return entity_candidates.emplace(scope_idx, std::move(cands)).first->second;
	}

	vector<ColumnGrouping> operator()(idx_t decide_var_idx, idx_t total_instances) {
		(void)total_instances;
		auto scope = decide_var_idx < var_indexer.var_scope.size()
		                 ? var_indexer.var_scope[decide_var_idx]
		                 : DecideVarScope::ROW;
		if (scope == DecideVarScope::SCALAR) {
			// One column for the whole query: there is no subset of rows or
			// entities to characterize, so offer no grouping candidates.
			return {};
		}
		if (scope == DecideVarScope::ROW) {
			return GetRowCandidates();
		}
		idx_t scope_idx = decide_var_idx < var_indexer.var_entity_mapping_idx.size()
		                      ? var_indexer.var_entity_mapping_idx[decide_var_idx]
		                      : DConstants::INVALID_INDEX;
		if (scope_idx == DConstants::INVALID_INDEX || scope_idx >= entity_scopes.size() ||
		    scope_idx >= entity_mappings.size()) {
			return {};
		}
		return GetEntityCandidates(scope_idx);
	}
};
} // namespace

//! Assemble the unbounded engine's `get_candidates` callback. Resolves the live
//! entity mappings (the Finalize-local mappings have been moved into the solver
//! input by now, so var_indexer owns/references them) and returns a stateful
//! provider wrapped as the engine's std::function input.
static std::function<vector<ColumnGrouping>(idx_t, idx_t)>
BuildUnboundedCandidateProvider(ClientContext &context, ColumnDataCollection &data, vector<LogicalType> types,
                                const DecideDiagParams &params, const vector<string> &input_column_names,
                                const VarIndexer &var_indexer, const vector<EntityScopeInfo> &entity_scopes,
                                idx_t num_rows) {
	const vector<EntityMapping> &live_entity_mappings =
	    var_indexer.entity_mappings_ref ? *var_indexer.entity_mappings_ref : var_indexer.entity_mappings_owned;
	return UnboundedCandidateProvider {context,    data,           std::move(types), params, input_column_names,
	                                   var_indexer, entity_scopes,  live_entity_mappings, num_rows};
}

//===--------------------------------------------------------------------===//
// Slow-solve checkpoint report (S2)
//===--------------------------------------------------------------------===//
// On a time-limit termination we tell the SQL user what the solver already found
// and how far it can still improve, in plain language (no "incumbent"/"gap"/"bound"
// solver words per the user-output rule). S2 prints the informational block only;
// the interactive "keep going?" prompt and warm re-solve are S3.

//! Peak resident set size of the whole process, in bytes. Whole-process, not
//! solve-only — it answers "will continuing risk running out of RAM." ru_maxrss is
//! bytes on macOS but KiB on Linux, so normalize.
static double PeakProcessMemoryBytes() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
#ifdef __APPLE__
    return (double)usage.ru_maxrss;
#else
    return (double)usage.ru_maxrss * 1024.0;
#endif
}

//! Compact byte count, e.g. "1.2 GB" / "340 MB".
static string FormatMemory(double bytes) {
    const char *unit = "B";
    double value = bytes;
    if (value >= 1024.0 * 1024.0 * 1024.0) {
        value /= 1024.0 * 1024.0 * 1024.0;
        unit = "GB";
    } else if (value >= 1024.0 * 1024.0) {
        value /= 1024.0 * 1024.0;
        unit = "MB";
    } else if (value >= 1024.0) {
        value /= 1024.0;
        unit = "KB";
    }
    return StringUtil::Format(value >= 100.0 ? "%.0f %s" : "%.1f %s", value, unit);
}

//! Seconds, integer when whole (300s), one decimal for sub-limit probe values (1.5s).
//! Sub-second values keep their significant digits (0.05s) — one decimal would round a
//! small time limit up to a misleading 0.1s (or down to 0.0s).
static string FormatDuration(double seconds) {
    if (seconds != 0.0 && std::fabs(seconds) < 1.0) {
        return StringUtil::Format("%.2gs", seconds);
    }
    if (std::fabs(seconds - std::round(seconds)) < 0.05) {
        return StringUtil::Format("%.0fs", std::round(seconds));
    }
    return StringUtil::Format("%.1fs", seconds);
}

//! Print the path-1 (solution found) / path-2 (no solution) status block to stderr.
//! Info only — the "keep improving it?" prompt lands with the S3 continuation loop.
static void PrintDecideTimeoutReport(const SolverResult &result, double elapsed_seconds,
                                     double time_limit) {
    string limit_str = FormatDuration(time_limit);
    string tail = StringUtil::Format("  elapsed %s · peak memory %s\n",
                                     FormatDuration(elapsed_seconds).c_str(),
                                     FormatMemory(PeakProcessMemoryBytes()).c_str());
    // A user Ctrl-C and a wall-clock timeout share the same best-so-far readback, but
    // must not read the same to the user: "you stopped it" vs "the clock ran out".
    if (result.has_solution) {
        // gap is a fraction on both backends; render as a percentage. It is NaN when
        // no proven bound exists (LP/QP timeouts, failed reads) — claim nothing then.
        string closeness = std::isfinite(result.gap)
                               ? StringUtil::Format("  best objective so far: %g  (within %.2f%% of the best possible)\n",
                                                    result.objective_value, result.gap * 100.0)
                               : StringUtil::Format("  best objective so far: %g\n", result.objective_value);
        if (result.user_interrupted) {
            fprintf(stderr, "DECIDE stopped at your request with a usable solution (not proven best).\n%s%s",
                    closeness.c_str(), tail.c_str());
        } else {
            fprintf(stderr, "DECIDE hit the %s time limit with a usable solution (not proven best).\n%s%s",
                    limit_str.c_str(), closeness.c_str(), tail.c_str());
        }
    } else {
        if (result.user_interrupted) {
            fprintf(stderr, "DECIDE stopped at your request before finding a solution yet.\n%s", tail.c_str());
        } else {
            fprintf(stderr, "DECIDE hit the %s time limit without finding a solution yet.\n%s",
                    limit_str.c_str(), tail.c_str());
        }
    }
}

//! Structured mirror of the checkpoint report for decide_diagnostics(): the same facts
//! (solution quality, best-possible objective, elapsed, peak memory) as one model-level
//! EAV block, so a timed-out solve populates the relation and points to it exactly like
//! the unbounded / infeasible terminals do. User-facing voice only — no solver jargon.
static DecideDiagnostic BuildTimeoutDiagnostic(const SolverResult &result, double elapsed_seconds,
                                               bool has_objective) {
    DecideDiagnostic diag;
    diag.valid = true;
    diag.state = "slow";
    auto add = [&](const char *attr, const string &val) {
        DiagnosticRow row;
        row.subject_kind = "model";
        row.attribute = attr;
        row.value = val;
        diag.rows.push_back(std::move(row));
    };
    // Summaries read after the "DECIDE optimization is slow:" headline, so the interrupt
    // variants are phrased to stay coherent with that prefix (the stderr report already
    // carries the primary "stopped at your request" line).
    add("stopped_by", result.user_interrupted ? "user_interrupt" : "time_limit");
    if (result.has_solution) {
        diag.summary = result.user_interrupted
                           ? "the solve was still improving when you stopped it — keep solving "
                             "with SET decide_on_timeout='continue', or reduce the input size to "
                             "prove the best"
                           : "the solve hit the time limit with a usable but unproven solution — "
                             "reduce the input size to prove it, or keep solving with "
                             "SET decide_on_timeout='continue'";
        add("status", "solution_found");
        add("best_objective", StringUtil::Format("%g", result.objective_value));
        // NaN gap = no proven bound (LP/QP timeouts, failed reads) — omit the row
        // rather than print "nan%" or overstate an unproven incumbent as proven.
        if (std::isfinite(result.gap)) {
            add("within_percent_of_best", StringUtil::Format("%.2f%%", result.gap * 100.0));
        }
    } else {
        diag.summary = result.user_interrupted
                           ? "you stopped the solve before it found a solution — keep searching "
                             "with SET decide_on_timeout='continue', or reduce the input size / "
                             "loosen the constraints"
                           : "the solve hit the time limit before finding a solution — reduce the "
                             "input size or loosen the constraints, or keep searching with "
                             "SET decide_on_timeout='continue'";
        add("status", "no_solution");
    }
    // The best objective still achievable (the solver's proven bound) — only meaningful
    // when the query actually optimizes something. A pure-feasibility DECIDE (no
    // MAXIMIZE / MINIMIZE) has no objective, so the "bound" is a trivial 0; skip it there.
    // Finite guard also keeps an open-relaxation sentinel out of the relation.
    if (has_objective && std::isfinite(result.best_bound)) {
        add("best_possible_objective", StringUtil::Format("%g", result.best_bound));
    }
    add("elapsed", FormatDuration(elapsed_seconds));
    add("peak_memory", FormatMemory(PeakProcessMemoryBytes()));
    return diag;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 1.5 — Build Entity Mappings for Table-Scoped Variables
//===--------------------------------------------------------------------===//

vector<EntityMapping> PhysicalDecide::BuildEntityMappings(ClientContext &context, DecideGlobalSinkState &gstate,
                                                           idx_t num_rows) const {
    vector<EntityMapping> entity_mappings;
    auto data_types = gstate.data.Types();
    for (idx_t scope_idx = 0; scope_idx < entity_scopes.size(); scope_idx++) {
        auto &scope = entity_scopes[scope_idx];
        EntityMapping mapping;

        // Build BoundReferenceExpression for each entity-key physical column index
        // so BuildGroupIds can run them through one ExpressionExecutor.
        vector<unique_ptr<Expression>> key_exprs_owned;
        key_exprs_owned.reserve(scope.entity_key_physical_indices.size());
        vector<const Expression *> key_exprs;
        key_exprs.reserve(scope.entity_key_physical_indices.size());
        for (idx_t col_idx = 0; col_idx < scope.entity_key_physical_indices.size(); col_idx++) {
            idx_t phys_idx = scope.entity_key_physical_indices[col_idx];
            key_exprs_owned.push_back(make_uniq_base<Expression, BoundReferenceExpression>(
                data_types[phys_idx], phys_idx));
            key_exprs.push_back(key_exprs_owned.back().get());
        }

        BuildGroupIds(key_exprs, context, gstate.data, num_rows,
                      std::function<bool(idx_t)>{}, /*null_excludes=*/false,
                      mapping.row_to_entity, mapping.num_entities);
        entity_mappings.push_back(std::move(mapping));
    }
    return entity_mappings;
}

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
    bool bench = std::getenv("DECIDB_BENCH") != nullptr;
    Profiler model_timer;
    Profiler solver_timer;

    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    idx_t num_rows = gstate.data.Count();

    if (bench) {
        model_timer.Start();
    }

    // Empty input → return empty result (mirrors standard SQL behavior)
    if (num_rows == 0) {
        return SinkFinalizeType::READY;
    }

    idx_t num_decide_vars = decide_variables.size();
    if (num_decide_vars == 0) {
        throw InternalException(
            "DECIDE operator has no decision variables "
            "(should have been caught during binding)");
    }

    // Evaluate coefficients and build the model (solver provides verbose output)

    // Per-Finalize cache of TransformToChunkExpression results. Outlives every
    // ExpressionExecutor created below, so it's safe for executors to retain
    // references into cached entries.
    ChunkExprCache chunk_expr_cache;

    // Per-Finalize cache of unfiltered PER row→group assignments, shared across
    // every constraint and the objective so a PER spec evaluated once is reused
    // by every other call site that asks for the same expression set.
    PerGroupCache per_group_cache;

    //===--------------------------------------------------------------------===//
    // PHASE 1.5: Build Entity Mappings for Table-Scoped Variables
    //===--------------------------------------------------------------------===//

    vector<EntityMapping> entity_mappings = BuildEntityMappings(context, gstate, num_rows);

    //===--------------------------------------------------------------------===//
    // PHASE 2: Evaluate Coefficient Expressions
    //===--------------------------------------------------------------------===//

    // 1. Evaluate constraints
    EvaluateConstraints(context, gstate, num_rows, chunk_expr_cache, per_group_cache, entity_mappings);

    // 2. Evaluate objective
    ObjectiveEvalState obj_state =
        EvaluateObjective(context, gstate, num_rows, chunk_expr_cache, per_group_cache, entity_mappings);

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP
    //===--------------------------------------------------------------------===//

    VarIndexer var_indexer;
    SolverInput solver_input = BuildSolverInput(context, gstate, num_rows, chunk_expr_cache, per_group_cache,
                                                std::move(entity_mappings), std::move(obj_state), var_indexer,
                                                bench, model_timer);

    return FinalizeSolveResult(context, gstate, solver_input, var_indexer, bench, model_timer, solver_timer);
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 2, sub-phase 1 -- Evaluate Constraints
//===--------------------------------------------------------------------===//

void PhysicalDecide::EvaluateConstraints(ClientContext &context, DecideGlobalSinkState &gstate,
                                         idx_t num_rows, ChunkExprCache &chunk_expr_cache,
                                         PerGroupCache &per_group_cache,
                                         const vector<EntityMapping> &entity_mappings) const {
    for (idx_t c = 0; c < gstate.constraints.size(); c++) {
        auto &constraint = gstate.constraints[c];

        EvaluatedConstraint eval_const;
        eval_const.comparison_type = constraint->comparison_type;
        eval_const.source_clause_id = constraint->source_clause_id;
        eval_const.repair_group_id = c;
        // Preserve whether the original LHS was an aggregate (e.g., SUM(...))
        eval_const.lhs_is_aggregate = constraint->lhs_is_aggregate;
        eval_const.minmax_indicator_idx = constraint->minmax_indicator_idx;
        eval_const.minmax_agg_type = constraint->minmax_agg_type;
        eval_const.ne_indicator_idx = constraint->ne_indicator_idx;
        eval_const.abs_aux_idx = constraint->abs_aux_idx;
        eval_const.abs_is_pos_bound = constraint->abs_is_pos_bound;
        eval_const.kind = constraint->kind;

        // Initialize result storage
        eval_const.row_coefficients.resize(constraint->lhs_terms.size());

        // Scan data and evaluate LHS coefficients
        ColumnDataScanState scan_state;
        gstate.data.InitializeScan(scan_state);

        DataChunk chunk;
        chunk.Initialize(context, gstate.data.Types());

        // Store variable indices for all terms (before scanning data). Capture each term's
        // symbolic coefficient label too (`l_extendedprice` for `buy * l_extendedprice`) so
        // infeasible diagnosis can render a data-weighted SUM clause symbolically instead of
        // dumping the per-row numeric fan-out.
        for (auto &term : constraint->lhs_terms) {
            eval_const.variable_indices.push_back(term.variable_index);
            eval_const.linear_term_reductions.push_back(term.reduction);
            eval_const.coefficient_labels.push_back(term.coefficient ? term.coefficient->GetName()
                                                                      : string());
        }

        vector<TermFilterState> term_filters(constraint->lhs_terms.size());
        vector<TermFilterState> bilinear_filters(constraint->bilinear_terms.size());
        vector<TermFilterState> quadratic_filters(constraint->quadratic_groups.size());
        vector<bool> local_row_active(num_rows, false);
        bool has_local_filters = false;
        bool has_unfiltered_aggregate_part = false;

        if (constraint->lhs_is_aggregate) {
            // Collect all per-term filter expressions and their target states, then
            // batch-evaluate them in a single scan instead of one scan per filter.
            struct FilterSlot { const Expression *cond; TermFilterState *state; };
            vector<FilterSlot> filter_slots;

            for (idx_t i = 0; i < constraint->lhs_terms.size(); i++) {
                auto &term = constraint->lhs_terms[i];
                term_filters[i].avg_scale = term.avg_scale;
                if (term.filter) {
                    filter_slots.push_back({term.filter.get(), &term_filters[i]});
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }
            for (idx_t i = 0; i < constraint->bilinear_terms.size(); i++) {
                auto &term = constraint->bilinear_terms[i];
                bilinear_filters[i].avg_scale = term.avg_scale;
                if (term.filter) {
                    filter_slots.push_back({term.filter.get(), &bilinear_filters[i]});
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }
            for (idx_t i = 0; i < constraint->quadratic_groups.size(); i++) {
                auto &group = constraint->quadratic_groups[i];
                quadratic_filters[i].avg_scale = group.avg_scale;
                if (group.filter) {
                    filter_slots.push_back({group.filter.get(), &quadratic_filters[i]});
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }

            if (!filter_slots.empty()) {
                vector<const Expression *> cond_ptrs;
                cond_ptrs.reserve(filter_slots.size());
                for (auto &s : filter_slots) cond_ptrs.push_back(s.cond);

                auto masks = EvaluateBooleanMasks(cond_ptrs, chunk_expr_cache, context, gstate.data, num_rows);

                has_local_filters = true;
                for (idx_t i = 0; i < filter_slots.size(); i++) {
                    if (masks[i].size() != num_rows) {
                        throw InternalException(
                            "DECIDE aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                            num_rows, masks[i].size());
                    }
                    filter_slots[i].state->mask = std::move(masks[i]);
                    filter_slots[i].state->has_filter = true;
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (filter_slots[i].state->mask[row]) {
                            local_row_active[row] = true;
                        }
                    }
                }
            }

            if (has_unfiltered_aggregate_part) {
                std::fill(local_row_active.begin(), local_row_active.end(), true);
            }
        }

        // Batch all linear coefficient expressions into one ExpressionExecutor and
        // produce a multi-column result chunk per scan iteration.
        vector<LogicalType> coef_result_types;
        coef_result_types.reserve(constraint->lhs_terms.size());
        ExpressionExecutor coef_executor(context);
        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            auto &term = constraint->lhs_terms[term_idx];
            const Expression &cached =
                CachedTransformToChunkExpression(chunk_expr_cache, *term.coefficient, context);
            coef_result_types.push_back(cached.return_type);
            try {
                coef_executor.AddExpression(cached);
            } catch (const std::exception &e) {
                throw InternalException("Failed to add expression for term %llu: %s\nOriginal: %s\nTransformed: %s",
                    term_idx, e.what(), term.coefficient->ToString(), cached.ToString());
            }
            eval_const.row_coefficients[term_idx].Reserve(num_rows);
        }

        DataChunk coef_results;
        if (!constraint->lhs_terms.empty()) {
            coef_results.Initialize(context, coef_result_types);
        }

        idx_t coefficient_row_base = 0;
        while (gstate.data.Scan(scan_state, chunk)) {
            if (constraint->lhs_terms.empty()) {
                continue;
            }
            coef_results.Reset();
            coef_executor.Execute(chunk, coef_results);
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                auto &col = eval_const.row_coefficients[term_idx].MutableDense();
                ExtractDoubleColumn(coef_results.data[term_idx], chunk.size(),
                                    constraint->lhs_terms[term_idx].sign,
                                    col,
                                    "constraint coefficient");
                eval_const.row_coefficients[term_idx].SyncSize();
            }
        }

        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (!term_filters[term_idx].has_filter) {
                continue;
            }
            auto &coefficients = eval_const.row_coefficients[term_idx].MutableDense();
            auto &mask = term_filters[term_idx].mask;
            for (idx_t row = 0; row < coefficients.size(); row++) {
                if (!mask[row]) {
                    coefficients[row] = 0.0;
                }
            }
        }

        // DecidB: Unified WHEN+PER row→group assignment
        // Produces row_group_ids and num_groups for the evaluated constraint.
        // - No WHEN, no PER: row_group_ids stays empty, num_groups = 0 (fast path)
        // - WHEN only: row_group_ids[row] = 0 (matching) or INVALID_INDEX (excluded), num_groups = 1
        // - PER only: row_group_ids[row] = 0..K-1 (group id), INVALID_INDEX for NULL PER values, num_groups = K
        // - WHEN+PER: WHEN filters first, then PER groups the remaining rows
        bool has_when = (constraint->when_condition != nullptr);
        bool has_per = (!constraint->per_columns.empty());

        // Group map for the right-hand side's own reducers: the constraint's WHEN and
        // PER, without the LHS's aggregate-local filters. Empty means "every row, one
        // group", the same convention `row_group_ids` uses.
        vector<idx_t> rhs_row_group_ids;

        // Facet C: render the WHEN/PER qualifier for the clause label through
        // RenderDecideSource, the same renderer EXPLAIN uses, so a clause reads
        // identically wherever it is quoted back. Order mirrors the postfix syntax
        // (`... WHEN <cond> PER <cols>`). Stamped onto provenance at the aggregate
        // emission sites; the diagnosis appends it to the reconstructed label.
        {
            string &q = eval_const.qualifier;
            if (has_when) {
                q = "WHEN " + RenderDecideSource(*constraint->when_condition, source_fragments, entity_scopes);
            }
            if (has_per) {
                if (!q.empty()) {
                    q += " ";
                }
                q += "PER ";
                bool parenthesize = constraint->per_columns.size() > 1;
                if (parenthesize) {
                    q += "(";
                }
                for (idx_t i = 0; i < constraint->per_columns.size(); i++) {
                    if (i > 0) {
                        q += ", ";
                    }
                    q += RenderDecideSource(*constraint->per_columns[i], source_fragments, entity_scopes);
                }
                if (parenthesize) {
                    q += ")";
                }
            }
        }

        if (has_when || has_per || has_local_filters) {
            vector<bool> when_mask;
            if (has_when) {
                when_mask = EvaluateBooleanMask(*constraint->when_condition, chunk_expr_cache, context,
                                                gstate.data, num_rows);
            }

            auto row_is_included = [&](idx_t row) {
                if (has_when && !when_mask[row]) {
                    return false;
                }
                if (has_local_filters && !local_row_active[row]) {
                    return false;
                }
                return true;
            };
            // The RHS's own reducers are scoped by the constraint's WHEN and PER, but
            // NOT by the LHS's aggregate-local WHENs — those scope their own reducer
            // only. `SUM(x) WHEN a <= MIN(b)` must take MIN over every row, not the
            // a-rows. Kept in the same numbering so group g means the same thing on
            // both sides.
            std::function<bool(idx_t)> rhs_row_is_included = [&](idx_t row) {
                return !has_when || when_mask[row];
            };

            if (has_per) {
                LookupOrBuildPerGroupIds(per_group_cache, constraint->per_columns,
                                         chunk_expr_cache, context, gstate.data, num_rows,
                                         /*null_excludes=*/true, row_is_included,
                                         eval_const.row_group_ids, eval_const.num_groups,
                                         eval_const.group_labels,
                                         &rhs_row_is_included, &rhs_row_group_ids);
                // PER: individual empty groups are skipped silently, but the
                // aggregate as a whole must see at least one group. Per-row
                // constraints are exempt — a per-row WHEN matching zero rows is
                // a valid no-op. Easy-direction MIN/MAX have been rewritten to
                // per-row form by the optimizer but still count as aggregates
                // for rejection. Spec: when/done.md → "Empty Row Sets" — reject
                // when *every* group is empty.
                if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
                    RejectEmptyAggregate(eval_const.num_groups, "aggregate", "constraint");
                }
            } else {
                eval_const.row_group_ids.resize(num_rows);
                rhs_row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
                // WHEN and/or aggregate-local WHEN (no PER): one group (group 0) for matching rows
                idx_t included_rows = 0;
                for (idx_t row = 0; row < num_rows; row++) {
                    bool inc = row_is_included(row);
                    eval_const.row_group_ids[row] = inc ? 0 : DConstants::INVALID_INDEX;
                    if (inc) included_rows++;
                    if (rhs_row_is_included(row)) {
                        rhs_row_group_ids[row] = 0;
                    }
                }
                eval_const.num_groups = 1;
                if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
                    RejectEmptyAggregate(included_rows, "aggregate", "constraint");
                }
            }
        }

        // Relation-qualified reducers: drop the duplicate rows the join introduced, now
        // that WHEN selection and PER partitioning have fixed the groups to de-duplicate
        // within. Folding this into the term filter keeps AVG's denominator honest.
        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            idx_t scope_idx = constraint->lhs_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(entity_mappings, scope_idx, eval_const.row_group_ids, num_rows,
                                   term_filters[term_idx]);
            MaskCoefficientColumn(eval_const.row_coefficients[term_idx], term_filters[term_idx].mask);
        }
        for (idx_t term_idx = 0; term_idx < constraint->bilinear_terms.size(); term_idx++) {
            idx_t scope_idx = constraint->bilinear_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(entity_mappings, scope_idx, eval_const.row_group_ids, num_rows,
                                   bilinear_filters[term_idx]);
        }
        for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
            idx_t scope_idx = constraint->quadratic_groups[group_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(entity_mappings, scope_idx, eval_const.row_group_ids, num_rows,
                                   quadratic_filters[group_idx]);
        }

        // Per-term aggregate-local WHEN: reject any term whose own filter mask
        // matches zero rows. Without this the term contributes nothing to the
        // constraint (its coefficients are all zero-masked at line 1829-1840);
        // for a MIN/MAX term routed via the z_k pathway, that would leave z_k
        // unpinned and silently vacuous. This guards composed-like LHS shapes
        // that flow through the lhs_terms path.
        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (!term_filters[term_idx].has_filter) continue;
            idx_t cnt = 0;
            auto &mask = term_filters[term_idx].mask;
            for (bool m : mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, "aggregate term", "constraint");
        }
        for (idx_t term_idx = 0; term_idx < constraint->bilinear_terms.size(); term_idx++) {
            if (!bilinear_filters[term_idx].has_filter) continue;
            idx_t cnt = 0;
            auto &mask = bilinear_filters[term_idx].mask;
            for (bool m : mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, "bilinear aggregate term", "constraint");
        }
        for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
            if (!quadratic_filters[group_idx].has_filter) continue;
            idx_t cnt = 0;
            auto &mask = quadratic_filters[group_idx].mask;
            for (bool m : mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, "quadratic aggregate term", "constraint");
        }

        // Evaluate RHS
        // RHS can be a constant, an aggregate (scalar), or a row-varying expression (for row-wise constraints)

        if (constraint->rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
            auto &const_expr = constraint->rhs_expr->Cast<BoundConstantExpression>();
            double rhs_constant = const_expr.value.GetValue<double>();
            eval_const.rhs_values.AssignScalar(num_rows, rhs_constant);
            // A constant RHS is one editable scalar shared across every row this
            // clause emits, so the elastic engine collapses them to one shared slack.
            eval_const.rhs_is_shared_scalar = true;
        } else {
            // RHS is a complex expression. It might be row-varying (e.g., column ref) or scalar (aggregate).
            // We evaluate it against the data chunks.
            // Canonicalization classifies the complete rebuilt bound. This is what
            // keeps forward/reversed subquery spellings and arithmetic over query-wide
            // values equivalent without re-deciding shape in the physical layer.
            eval_const.rhs_is_shared_scalar =
                IsQueryWideBoundTag(constraint->rhs_expr->GetAlias());
            // Capture the RHS symbolic name (e.g. the data column `cap_col`) so query-mode
            // infeasible diagnosis can render `x <= cap_col + delta`. Only meaningful for a
            // genuine data RHS; a shared-scalar RHS reports the numeric knob instead.
            if (!eval_const.rhs_is_shared_scalar &&
                !ContainsScalarSubqueryProvenance(*constraint->rhs_expr)) {
                eval_const.rhs_label = RenderDecideSource(*constraint->rhs_expr, source_fragments, entity_scopes);
            }
            eval_const.rhs_values.Reserve(num_rows);

            // Reducers on this side are evaluated first, to one value per group, and
            // replaced by an extra chunk column carrying that value broadcast over the
            // group's rows. What remains is an ordinary per-row expression, so a mixed
            // bound like `MIN(cap) + demand * 2` needs no special case: the reducer part
            // is constant within the group and the rest still varies, which is exactly
            // what the reduction after this block expects.
            vector<const Expression *> rhs_reducers;
            CollectReducers(*constraint->rhs_expr, rhs_reducers);

            std::unordered_map<const Expression *, idx_t> agg_substitutions;
            vector<vector<double>> reducer_values;
            const idx_t data_columns = gstate.data.ColumnCount();
            for (idx_t i = 0; i < rhs_reducers.size(); i++) {
                auto &agg = rhs_reducers[i]->Cast<BoundAggregateExpression>();
                reducer_values.push_back(EvaluateRhsReducerPerGroup(
                    agg, rhs_row_group_ids, eval_const.num_groups, entity_mappings,
                    chunk_expr_cache, context, gstate.data, num_rows));
                agg_substitutions.emplace(rhs_reducers[i], data_columns + i);
            }

            // A substituted tree is only valid against the augmented chunk below, so it
            // is transformed directly rather than through the shared cache.
            unique_ptr<Expression> owned_rhs;
            const Expression *transformed_rhs = nullptr;
            if (agg_substitutions.empty()) {
                transformed_rhs = &CachedTransformToChunkExpression(
                    chunk_expr_cache, *constraint->rhs_expr, context);
            } else {
                owned_rhs = TransformToChunkExpression(*constraint->rhs_expr, context,
                                                       &agg_substitutions);
                transformed_rhs = owned_rhs.get();
            }

            // Prepare executor
            ExpressionExecutor rhs_executor(context);
            rhs_executor.AddExpression(*transformed_rhs);

            // Scan data and evaluate
            ColumnDataScanState rhs_scan_state;
            gstate.data.InitializeScan(rhs_scan_state);
            vector<LogicalType> chunk_types = gstate.data.Types();
            for (idx_t i = 0; i < rhs_reducers.size(); i++) {
                chunk_types.push_back(LogicalType::DOUBLE);
            }
            DataChunk rhs_chunk;
            rhs_chunk.Initialize(context, chunk_types);
            DataChunk scan_chunk;
            scan_chunk.Initialize(context, gstate.data.Types());

            DataChunk rhs_result;
            vector<LogicalType> result_types = {transformed_rhs->return_type};
            rhs_result.Initialize(context, result_types);
            idx_t scanned = 0;
            while (gstate.data.Scan(rhs_scan_state, scan_chunk)) {
                DataChunk *eval_chunk = &scan_chunk;
                if (!rhs_reducers.empty()) {
                    rhs_chunk.Reset();
                    for (idx_t col = 0; col < scan_chunk.ColumnCount(); col++) {
                        rhs_chunk.data[col].Reference(scan_chunk.data[col]);
                    }
                    for (idx_t i = 0; i < rhs_reducers.size(); i++) {
                        auto &vals = reducer_values[i];
                        auto out = FlatVector::GetData<double>(rhs_chunk.data[data_columns + i]);
                        for (idx_t row = 0; row < scan_chunk.size(); row++) {
                            idx_t abs_row = scanned + row;
                            idx_t g = rhs_row_group_ids.empty()
                                          ? 0
                                          : rhs_row_group_ids[abs_row];
                            // A row in no group contributes to no constraint; its value
                            // is never read, so any finite filler will do.
                            out[row] = (g == DConstants::INVALID_INDEX || g >= vals.size())
                                           ? (vals.empty() ? 0.0 : vals[0])
                                           : vals[g];
                        }
                    }
                    rhs_chunk.SetCardinality(scan_chunk.size());
                    eval_chunk = &rhs_chunk;
                }
                rhs_result.Reset();
                rhs_executor.Execute(*eval_chunk, rhs_result);
                auto &rhs_col = eval_const.rhs_values.MutableDense();
                // Infinities are admitted here and nowhere else: this column becomes
                // the model row's `rhs`, where ±inf is a meaningful bound. Canonical-
                // ization rebuilds `x + v <= K` as `x <= K - v`, so an infinite K that
                // was absorbed as a column bound in its bare spelling must stay legal
                // in its rebuilt one. `inf - inf` still yields NaN and is still refused.
                ExtractDoubleColumn(rhs_result.data[0], scan_chunk.size(), 1.0, rhs_col,
                                    "constraint right-hand side", /*allow_infinite=*/true);
                eval_const.rhs_values.SyncSize();
                scanned += scan_chunk.size();
            }
        }

        // A reduced constraint emits one row per group, so its bound must be one value
        // per group. `was_minmax_easy` is included: the optimizer emitted it per row,
        // but it is still one reducer compared against every tuple's bound, so the
        // tightest bound is what pins it.
        if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
            string rhs_text = eval_const.rhs_label.empty()
                                  ? RenderDecideSource(*constraint->rhs_expr, source_fragments, entity_scopes)
                                  : eval_const.rhs_label;
            // A decorrelated scalar subquery flattens to a column literally named
            // SUBQUERY. Quoting that back at the user names nothing they wrote, so
            // fall back to describing the side instead.
            if (rhs_text.empty() || StringUtil::CIEquals(rhs_text, "SUBQUERY")) {
                rhs_text = "The right-hand side";
            } else {
                rhs_text = "`" + rhs_text + "`";
            }
            ReduceAggregateRhsPerGroup(eval_const, rhs_row_group_ids, num_rows, rhs_text);

        }


        // AVG(x) <> K special case: dividing LHS coefficients by the AVG denominator
        // produces fractional coefficients, which the NE integer-step guard rejects.
        // For pure linear LHS where every term is AVG-scaled, hoist the denominator to
        // the RHS instead — keep LHS as SUM and multiply per-group RHS by group size
        // in the deferred NE expansion. Mixed AVG/non-AVG terms or bilinear/quadratic
        // LHS fall through to the existing path (which may still reject).
        bool ne_avg_hoist = false;
        if (constraint->ne_indicator_idx != DConstants::INVALID_INDEX &&
            constraint->lhs_is_aggregate && !constraint->has_bilinear && !constraint->has_quadratic &&
            !constraint->lhs_terms.empty()) {
            ne_avg_hoist = true;
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                if (!term_filters[term_idx].avg_scale) {
                    ne_avg_hoist = false;
                    break;
                }
            }
        }
        if (ne_avg_hoist) {
            eval_const.ne_avg_rhs_scale = true;
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                term_filters[term_idx].avg_scale = false;
            }
        }

        // Track whether EVERY linear term was AVG-scaled (a pure AVG LHS), so the
        // model builder can tag the row provenance and infeasible diagnosis renders
        // it as `AVG(...)`. Mixed AVG/non-AVG or bilinear/quadratic LHS stays false
        // (it has no clean AVG re-quote). I2.d.
        bool all_avg = !constraint->lhs_terms.empty() && !constraint->has_bilinear &&
                       !constraint->has_quadratic;
        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (term_filters[term_idx].avg_scale) {
                ScaleAvgRows(eval_const.row_coefficients[term_idx], term_filters[term_idx].has_filter,
                            term_filters[term_idx].mask, /*quadratic_inner=*/false, num_rows,
                            eval_const.row_group_ids, eval_const.num_groups);
            } else {
                all_avg = false;
            }
        }
        eval_const.avg_scaled = all_avg;

        // Evaluate bilinear terms in constraint (if any).
        if (constraint->has_bilinear) {
            auto ebts = EvaluateBilinearTerms(constraint->bilinear_terms, bilinear_filters, chunk_expr_cache,
                                              context, gstate.data, num_rows, "bilinear constraint coefficient");
            for (idx_t term_idx = 0; term_idx < ebts.size(); term_idx++) {
                auto &ebt = ebts[term_idx];
                if (bilinear_filters[term_idx].avg_scale) {
                    ScaleAvgRows(ebt.row_coefficients, bilinear_filters[term_idx].has_filter,
                                bilinear_filters[term_idx].mask, /*quadratic_inner=*/false, num_rows,
                                eval_const.row_group_ids, eval_const.num_groups);
                }
                eval_const.bilinear_terms.push_back(std::move(ebt));
            }
        }

        // Evaluate quadratic groups in constraint (POWER(expr, 2) / self-products).
        // Per group, batch all inner_terms into a single ExpressionExecutor.
        if (constraint->has_quadratic) {
            eval_const.has_quadratic = true;
            for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
                auto &qg = constraint->quadratic_groups[group_idx];
                EvaluatedConstraint::QuadraticGroup eqg;
                eqg.sign = qg.sign;

                if (qg.scale) {
                    const Expression &transformed =
                        CachedTransformToChunkExpression(chunk_expr_cache, *qg.scale, context);
                    ExpressionExecutor scale_executor(context);
                    scale_executor.AddExpression(transformed);
                    ColumnDataScanState scale_scan;
                    gstate.data.InitializeScan(scale_scan);
                    DataChunk scale_chunk;
                    scale_chunk.Initialize(context, gstate.data.Types());
                    bool got_scale = false;
                    while (!got_scale && gstate.data.Scan(scale_scan, scale_chunk)) {
                        if (scale_chunk.size() == 0) {
                            continue;
                        }
                        DataChunk scale_result;
                        scale_result.Initialize(context, {transformed.return_type});
                        scale_executor.Execute(scale_chunk, scale_result);
                        Value value = scale_result.data[0].GetValue(0);
                        if (value.IsNull()) {
                            throw InvalidInputException(
                                "DECIDE constraint: the factor on a squared aggregate evaluated to NULL.");
                        }
                        double factor = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                        if (!std::isfinite(factor)) {
                            throw InvalidInputException(
                                "DECIDE constraint: the factor on a squared aggregate is not finite (NaN/Inf).");
                        }
                        if (qg.scale_divides && factor == 0.0) {
                            throw InvalidInputException(
                                "DECIDE constraint: division by zero -- the factor on a squared aggregate evaluated to 0.");
                        }
                        eqg.sign *= qg.scale_divides ? 1.0 / factor : factor;
                        got_scale = true;
                    }
                }

                vector<LogicalType> q_types;
                ExpressionExecutor q_executor(context);
                eqg.row_coefficients.resize(qg.inner_terms.size());
                for (idx_t inner_idx = 0; inner_idx < qg.inner_terms.size(); inner_idx++) {
                    auto &term = qg.inner_terms[inner_idx];
                    eqg.variable_indices.push_back(term.variable_index);
                    const Expression &cached =
                        CachedTransformToChunkExpression(chunk_expr_cache, *term.coefficient, context);
                    q_types.push_back(cached.return_type);
                    q_executor.AddExpression(cached);
                    eqg.row_coefficients[inner_idx].Reserve(num_rows);
                }

                if (!qg.inner_terms.empty()) {
                    ColumnDataScanState qscan;
                    gstate.data.InitializeScan(qscan);
                    DataChunk qchunk;
                    qchunk.Initialize(context, gstate.data.Types());
                    DataChunk q_results;
                    q_results.Initialize(context, q_types);
                    while (gstate.data.Scan(qscan, qchunk)) {
                        q_results.Reset();
                        q_executor.Execute(qchunk, q_results);
                        for (idx_t inner_idx = 0; inner_idx < qg.inner_terms.size(); inner_idx++) {
                            auto &col = eqg.row_coefficients[inner_idx].MutableDense();
                            ExtractDoubleColumn(q_results.data[inner_idx], qchunk.size(),
                                                qg.inner_terms[inner_idx].sign,
                                                col,
                                                "quadratic constraint coefficient");
                            eqg.row_coefficients[inner_idx].SyncSize();
                        }
                    }
                }
                if (quadratic_filters[group_idx].has_filter) {
                    auto &mask = quadratic_filters[group_idx].mask;
                    for (auto &qcol : eqg.row_coefficients) {
                        auto &coefficients = qcol.MutableDense();
                        for (idx_t row = 0; row < coefficients.size(); row++) {
                            if (!mask[row]) {
                                coefficients[row] = 0.0;
                            }
                        }
                    }
                }
                if (quadratic_filters[group_idx].avg_scale) {
                    for (auto &qcol : eqg.row_coefficients) {
                        ScaleAvgRows(qcol, quadratic_filters[group_idx].has_filter,
                                    quadratic_filters[group_idx].mask, /*quadratic_inner=*/true, num_rows,
                                    eval_const.row_group_ids, eval_const.num_groups);
                    }
                }
                eval_const.quadratic_groups.push_back(std::move(eqg));
            }
        }

        gstate.evaluated_constraints.push_back(std::move(eval_const));
    }
}

PhysicalDecide::ObjectiveEvalState PhysicalDecide::EvaluateObjective(ClientContext &context,
                                                                     DecideGlobalSinkState &gstate,
                                                                     idx_t num_rows,
                                                                     ChunkExprCache &chunk_expr_cache,
                                                                     PerGroupCache &per_group_cache,
                                                                     const vector<EntityMapping> &entity_mappings) const {
    // 2. Evaluate objective
    vector<TermFilterState> obj_linear_term_filters;
    vector<TermFilterState> obj_quadratic_term_filters;
    vector<TermFilterState> obj_bilinear_filters;
    vector<bool> objective_when_mask;
    bool objective_has_when = false;

    if (gstate.objective) {
        if (gstate.objective->has_quadratic) {
            gstate.has_quadratic_objective = true;
            gstate.quadratic_sign = gstate.objective->quadratic_sign;
        }

        objective_has_when = (gstate.objective->when_condition != nullptr);
        if (objective_has_when) {
            objective_when_mask = EvaluateBooleanMask(*gstate.objective->when_condition, chunk_expr_cache,
                                                       context, gstate.data, num_rows);
            if (objective_when_mask.size() != num_rows) {
                throw InternalException("DECIDE objective WHEN mask size mismatch: expected %llu rows, got %llu",
                                        num_rows, objective_when_mask.size());
            }
        }

        // DecidB: evaluate both the linear and quadratic-inner term lists so that
        // mixed objectives (e.g. SUM(POWER(x-t, 2) + penalty * x)) emit coefficients
        // into both solver-input arrays. The two buckets are processed together
        // inside a single scan over gstate.data — doubling coefficient evaluators
        // was cheap, but doubling the ColumnDataCollection scan was not.
        struct ObjBucket {
            vector<DecideTerm> *src_terms;
            vector<CoefficientColumn> *out_coeffs;
            vector<idx_t> *out_var_indices;
            vector<TermFilterState> *out_term_filters;
        };
        vector<ObjBucket> buckets;
        if (!gstate.objective->terms.empty()) {
            buckets.push_back({&gstate.objective->terms,
                               &gstate.evaluated_objective_coefficients,
                               &gstate.objective_variable_indices,
                               &obj_linear_term_filters});
        }
        if (gstate.objective->has_quadratic) {
            buckets.push_back({&gstate.objective->squared_terms,
                               &gstate.evaluated_quadratic_coefficients,
                               &gstate.quadratic_variable_indices,
                               &obj_quadratic_term_filters});
        }

        if (!buckets.empty()) {
            // Pre-size filters + out_coeffs, snapshot variable indices, and collect
            // all filter expressions so they can be batch-evaluated in one scan.
            struct ObjFilterSlot { const Expression *cond; TermFilterState *state; };
            vector<ObjFilterSlot> obj_filter_slots;

            for (auto &b : buckets) {
                b.out_term_filters->resize(b.src_terms->size());
                b.out_coeffs->resize(b.src_terms->size());
                for (idx_t term_idx = 0; term_idx < b.src_terms->size(); term_idx++) {
                    auto &term = (*b.src_terms)[term_idx];
                    (*b.out_term_filters)[term_idx].avg_scale = term.avg_scale;
                    if (term.filter) {
                        (*b.out_term_filters)[term_idx].has_filter = true;
                        obj_filter_slots.push_back({term.filter.get(), &(*b.out_term_filters)[term_idx]});
                    }
                    b.out_var_indices->push_back(term.variable_index);
                }
            }

            if (!obj_filter_slots.empty()) {
                vector<const Expression *> cond_ptrs;
                cond_ptrs.reserve(obj_filter_slots.size());
                for (auto &s : obj_filter_slots) cond_ptrs.push_back(s.cond);
                auto masks = EvaluateBooleanMasks(cond_ptrs, chunk_expr_cache, context, gstate.data, num_rows);
                for (idx_t i = 0; i < obj_filter_slots.size(); i++) {
                    if (masks[i].size() != num_rows) {
                        throw InternalException(
                            "DECIDE objective aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                            num_rows, masks[i].size());
                    }
                    obj_filter_slots[i].state->mask = std::move(masks[i]);
                }
            }

            // Flatten all coefficient expressions across buckets. `route[i]` names
            // the bucket and the position within that bucket that flat term `i`
            // writes to — so the scan loop can route each evaluated value back
            // without knowing which bucket it came from.
            vector<pair<idx_t, idx_t>> route; // (bucket_idx, term_idx)
            vector<LogicalType> obj_result_types;
            ExpressionExecutor obj_executor(context);
            for (idx_t b_idx = 0; b_idx < buckets.size(); b_idx++) {
                auto &b = buckets[b_idx];
                for (idx_t term_idx = 0; term_idx < b.src_terms->size(); term_idx++) {
                    const Expression &cached = CachedTransformToChunkExpression(
                        chunk_expr_cache, *(*b.src_terms)[term_idx].coefficient, context);
                    obj_result_types.push_back(cached.return_type);
                    obj_executor.AddExpression(cached);
                    route.emplace_back(b_idx, term_idx);
                    (*b.out_coeffs)[term_idx].Reserve(num_rows);
                }
            }

            ColumnDataScanState obj_scan_state;
            gstate.data.InitializeScan(obj_scan_state);
            DataChunk obj_chunk;
            obj_chunk.Initialize(context, gstate.data.Types());
            DataChunk obj_results;
            if (!route.empty()) {
                obj_results.Initialize(context, obj_result_types);
            }

            while (gstate.data.Scan(obj_scan_state, obj_chunk)) {
                if (route.empty()) {
                    continue;
                }
                obj_results.Reset();
                obj_executor.Execute(obj_chunk, obj_results);
                for (idx_t j = 0; j < route.size(); j++) {
                    auto &b = buckets[route[j].first];
                    idx_t term_idx = route[j].second;
                    auto &out = (*b.out_coeffs)[term_idx].MutableDense();
                    int term_sign = (*b.src_terms)[term_idx].sign;
                    ExtractDoubleColumn(obj_results.data[j], obj_chunk.size(),
                                        static_cast<double>(term_sign), out,
                                        "objective coefficient");
                    (*b.out_coeffs)[term_idx].SyncSize();
                }
            }

            // Apply per-term aggregate-local WHEN filters and the expression-level
            // WHEN mask once per bucket. Both masks are shared between linear and
            // quadratic lists for a mixed objective.
            for (auto &b : buckets) {
                for (idx_t term_idx = 0; term_idx < b.out_coeffs->size(); term_idx++) {
                    if ((*b.out_term_filters)[term_idx].has_filter) {
                        auto &mask = (*b.out_term_filters)[term_idx].mask;
                        auto &out = (*b.out_coeffs)[term_idx].MutableDense();
                        for (idx_t row = 0; row < out.size(); row++) {
                            if (!mask[row]) {
                                out[row] = 0.0;
                            }
                        }
                    }
                }
                if (objective_has_when) {
                    for (idx_t term_idx = 0; term_idx < b.out_coeffs->size(); term_idx++) {
                        auto &out = (*b.out_coeffs)[term_idx].MutableDense();
                        for (idx_t row = 0; row < num_rows; row++) {
                            if (objective_when_mask[row]) continue;
                            out[row] = 0.0;
                        }
                    }
                }
            }

            for (auto &b : buckets) {
                for (idx_t term_idx = 0; term_idx < b.out_term_filters->size(); term_idx++) {
                    if (!(*b.out_term_filters)[term_idx].has_filter) continue;
                    idx_t cnt = 0;
                    auto &mask = (*b.out_term_filters)[term_idx].mask;
                    for (bool m : mask) if (m) cnt++;
                    RejectEmptyAggregate(cnt, "aggregate term", "objective");
                }
            }
        }

        // Reject an objective-level WHEN that matches zero rows. Hoisted out
        // of the `if (!buckets.empty())` block so it also covers bilinear-only
        // objectives (where `terms` is empty but a bilinear WHEN filter still
        // needs to be guarded). Without this, flat MIN/MAX objectives build a
        // z aux + per-row linking over all num_rows, with every linking
        // constraint vacuously satisfied — the solver drives z to whatever
        // extreme the objective sense prefers.
        if (objective_has_when) {
            idx_t cnt = 0;
            for (bool m : objective_when_mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, "aggregate", "objective");
        }

        obj_bilinear_filters.resize(gstate.objective->bilinear_terms.size());
        {
            struct BilFilterSlot { const Expression *cond; idx_t term_idx; };
            vector<BilFilterSlot> bil_slots;
            for (idx_t i = 0; i < gstate.objective->bilinear_terms.size(); i++) {
                auto &term = gstate.objective->bilinear_terms[i];
                obj_bilinear_filters[i].avg_scale = term.avg_scale;
                if (term.filter) {
                    obj_bilinear_filters[i].has_filter = true;
                    bil_slots.push_back({term.filter.get(), i});
                }
            }
            if (!bil_slots.empty()) {
                vector<const Expression *> cond_ptrs;
                cond_ptrs.reserve(bil_slots.size());
                for (auto &s : bil_slots) cond_ptrs.push_back(s.cond);
                auto masks = EvaluateBooleanMasks(cond_ptrs, chunk_expr_cache, context, gstate.data, num_rows);
                for (idx_t i = 0; i < bil_slots.size(); i++) {
                    idx_t tidx = bil_slots[i].term_idx;
                    if (masks[i].size() != num_rows) {
                        throw InternalException(
                            "DECIDE objective aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                            num_rows, masks[i].size());
                    }
                    obj_bilinear_filters[tidx].mask = std::move(masks[i]);
                    idx_t cnt = 0;
                    for (bool m : obj_bilinear_filters[tidx].mask) if (m) cnt++;
                    RejectEmptyAggregate(cnt, "bilinear aggregate term", "objective");
                }
            }
        }

        // No extra debug here; solver output will show timings/objective

        // Evaluate bilinear term coefficients (non-Boolean pairs left by optimizer).
        if (gstate.objective->has_bilinear) {
            gstate.evaluated_bilinear_terms = EvaluateBilinearTerms(
                gstate.objective->bilinear_terms, obj_bilinear_filters, chunk_expr_cache, context, gstate.data,
                num_rows, "bilinear objective coefficient", objective_has_when ? &objective_when_mask : nullptr);
        }
    }

    ObjectiveEvalState result;
    result.linear_filters = std::move(obj_linear_term_filters);
    result.quadratic_filters = std::move(obj_quadratic_term_filters);
    result.bilinear_filters = std::move(obj_bilinear_filters);
    result.when_mask = std::move(objective_when_mask);
    result.has_when = objective_has_when;
    return result;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 3, model build -- BuildSolverInput
//===--------------------------------------------------------------------===//

SolverInput PhysicalDecide::BuildSolverInput(ClientContext &context, DecideGlobalSinkState &gstate,
                                             idx_t num_rows, ChunkExprCache &chunk_expr_cache,
                                             PerGroupCache &per_group_cache, vector<EntityMapping> entity_mappings,
                                             ObjectiveEvalState obj_state, VarIndexer &out_var_indexer,
                                             bool bench, Profiler &model_timer) const {
    idx_t num_decide_vars = decide_variables.size();
    auto &obj_linear_term_filters = obj_state.linear_filters;
    auto &obj_quadratic_term_filters = obj_state.quadratic_filters;
    auto &obj_bilinear_filters = obj_state.bilinear_filters;
    auto &objective_when_mask = obj_state.when_mask;
    bool objective_has_when = obj_state.has_when;

    // Construct SolverInput
    SolverInput solver_input;
    solver_input.num_rows = num_rows;
    solver_input.num_decide_vars = num_decide_vars;
    solver_input.entity_mappings = std::move(entity_mappings);
    solver_input.variable_scopes = variable_scopes;
    
    // Variable types and bounds
    solver_input.variable_types.resize(num_decide_vars);
    for (idx_t var = 0; var < num_decide_vars; var++) {
        auto &decide_var = decide_variables[var]->Cast<BoundColumnRefExpression>();
        // A BOOLEAN-domain variable (declared `x(BOOL)`, or a boolean-valued
        // auxiliary such as an IN-domain/L0 indicator) is bound with an INTEGER
        // DuckDB type so it can appear in arithmetic (`M * z`, `x - v1*z1 - ...`),
        // but its solver-facing domain is BOOLEAN: `is_boolean_var` is the
        // authoritative signal, not `return_type`. Reporting it as BOOLEAN here
        // makes SolverModel::Build (ilp_model_builder.cpp) apply the `[0,1]` box
        // and mark the column binary directly from the type — the same path
        // optimizer-created BOOLEAN auxiliaries (MIN/MAX, NE indicators) already
        // use — with no constraint-tree representation of the domain at all.
        bool bool_domain = var < is_boolean_var.size() && is_boolean_var[var];
        solver_input.variable_types[var] = bool_domain ? LogicalType::BOOLEAN : decide_var.return_type;
    }

    // Bounds were absorbed in the gstate constructor from simple
    // `x OP const` / BETWEEN constraints; those comparisons were skipped in
    // AnalyzeConstraint so we don't re-emit them as per-row model rows.
    solver_input.lower_bounds = gstate.absorbed_lower_bounds;
    solver_input.constraint_sources = constraint_sources;
    // Resolve the "unset" sentinel to the default lower bound 0: a variable the
    // query never explicitly lowered stays non-negative. Variables with an
    // explicit (possibly negative) lower-bound constraint keep their absorbed
    // value. Must run before any consumer of lower_bounds (implied-bound
    // propagation, Big-M, McCormick, model builder).
    for (auto &lb : solver_input.lower_bounds) {
        if (lb <= ABSORBED_LOWER_UNSET) {
            lb = 0.0;
        }
    }
    solver_input.upper_bounds = gstate.absorbed_upper_bounds;

    // Everything below this point is stage 06 working on the evaluated model, so
    // hand the constraints and the formulation links over before the first pass.
    solver_input.constraints = std::move(gstate.evaluated_constraints);
    for (auto &link : bilinear_links) {
        solver_input.bilinear_links.push_back(
            BilinearLinkSpec {link.aux_idx, link.bool_var_idx, link.other_var_idx});
    }
    for (auto &link : abs_maximize_links) {
        solver_input.abs_maximize_links.push_back(AbsMaximizeLinkSpec {link.aux_idx, link.y_idx});
    }
    // Plain declared names, for the refusals the linearization can raise. `alias`
    // lives on BaseExpression, so no cast is needed to read it.
    vector<string> decide_var_names;
    decide_var_names.reserve(decide_variables.size());
    for (auto &v : decide_variables) {
        decide_var_names.push_back(v->alias);
    }

    // The rigid box, captured before any propagation runs and with every user-absorbed
    // direction re-opened. What is left is the intrinsic domain alone — the only part of
    // the column box that survives infeasibility diagnosis unchanged, and therefore the
    // only part a structural rewrite may rely on. The re-opening mirrors the
    // `user_absorbed_bounds` re-emission below, which relaxes the same directions for the
    // same reason.
    solver_input.rigid_lower_bounds = solver_input.lower_bounds;
    solver_input.rigid_upper_bounds = solver_input.upper_bounds;
    for (const auto &b : gstate.user_absorbed_bounds) {
        idx_t v = b.decide_var_idx;
        if (v >= solver_input.rigid_lower_bounds.size()) {
            continue;
        }
        bool bound_is_bool = v < is_boolean_var.size() && is_boolean_var[v];
        if (b.sense == '<' || b.sense == '=') {
            solver_input.rigid_upper_bounds[v] = bound_is_bool ? 1.0 : NumericLimits<double>::Maximum();
        }
        if (b.sense == '>' || b.sense == '=') {
            solver_input.rigid_lower_bounds[v] = bound_is_bool ? 0.0 : -NumericLimits<double>::Maximum();
        }
    }

    // Data-driven implied-bound propagation: derive finite upper bounds for
    // otherwise-unbounded variables from non-negative `<=`/`=` constraints (the
    // knapsack/budget pattern), so the downstream Big-M can be finite and tight.
    // Only provably-implied bounds are applied; the feasible region is unchanged.
    DecidePropagateImpliedBounds(solver_input.constraints, solver_input.lower_bounds,
                                 solver_input.upper_bounds, num_rows);

    // Auto-M for L0 `norm(e, 0)`: the binder emitted the raw links `e <= M*z` and
    // `-e <= M*z` with a placeholder coefficient on each `__l0auto_ind_*` indicator.
    // Now that implied bounds are known, fill a tight, data-driven Big-M (mirrors
    // the <> path): zero the indicator's own coefficient, compute M from the
    // remaining (inner-expression) terms + RHS, then set the indicator coefficient
    // to -M. The links carry a negative indicator coefficient, so implied-bound
    // propagation above already skipped them (no contamination from the placeholder).
    {
        unordered_set<idx_t> l0_auto;
        for (idx_t i = 0; i < decide_variables.size(); i++) {
            if (StringUtil::StartsWith(decide_variables[i]->GetName(), "__l0auto_ind_")) {
                l0_auto.insert(i);
            }
        }
        if (!l0_auto.empty()) {
            for (auto &ec : solver_input.constraints) {
                // Locate the auto-L0 indicator term, and confirm this is a LINK
                // (it also carries the inner expression's variable) rather than the
                // indicator's own 0<=z<=1 bound row — which we must not rewrite.
                idx_t z_term = DConstants::INVALID_INDEX;
                bool has_inner_var = false;
                for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
                    idx_t v = ec.variable_indices[t];
                    if (v == DConstants::INVALID_INDEX) {
                        continue;
                    }
                    if (l0_auto.count(v)) {
                        z_term = t;
                    } else if (!StringUtil::StartsWith(decide_variables[v]->GetName(), "__abs_aux_")) {
                        // The REVERSE link `ABS(inner) >= TOL*z` also references the L0
                        // indicator, but its only other variable is the ABS aux and its
                        // z-coefficient is the fixed tolerance — it must NOT be refilled.
                        // Only the FORWARD links carry the inner decision variable, so an
                        // ABS-aux companion marks the reverse link to skip.
                        has_inner_var = true;
                    }
                }
                if (z_term == DConstants::INVALID_INDEX || !has_inner_var) {
                    continue;
                }
                // Zero the indicator's own contribution, compute the tight Big-M from
                // the remaining (inner) terms + RHS, then set the indicator coeff = +M.
                // The link is normalized to `M*z - inner >= rhs`, so the indicator
                // coefficient is positive.
                ec.row_coefficients[z_term].AssignScalar(num_rows, 0.0);
                double M = DecideTightPerRowBigM(ec, solver_input.lower_bounds,
                                                 solver_input.upper_bounds, num_rows,
                                                 decide_var_names);
                ec.row_coefficients[z_term].AssignScalar(num_rows, M);
            }
        }
    }

    // THE ROUTING, as stage 05 decided it. Whether ABS is stated natively or lowered is
    // a FORMULATION choice, and formulation belongs to stage 05 — so this reads the
    // decision off the plan rather than asking a backend. Native means no Big-M and
    // therefore no bound requirement, so the answer changes what is refused, not only
    // what is fast; that is exactly why it has to be the same answer the rewrites above
    // were selected against. Both arms below only translate.
    const bool native_abs = use_native_constructs.abs;
    // MIN/MAX arrives as a POLICY rather than an answer. Stage 05 decided both halves of
    // it — whether the backend can state the construct, and whether a declared construct
    // is used everywhere or only as a fallback — and neither is re-decided here. What is
    // left is a question about the data: whether THIS clause has a Big-M, which only a
    // stage that has seen the evaluated coefficients can answer. Applying a decision to
    // data is this layer's job; choosing the decision was not.
    const NativeConstructPolicy native_min_max {use_native_constructs.min_max,
                                                force_native_constructs};

    // The flat column space, built ONCE and handed to every construct site below. A
    // general or indicator constraint names flat columns, so a site that may state one
    // natively needs the index before it can emit — which is why this is built here,
    // ahead of linearization, rather than after it. Nothing below adds a decide
    // variable, so the row / entity / scalar blocks are final from this point; only
    // `solver_input.num_global_vars` keeps growing as auxiliary globals are appended,
    // and `total_vars` is refreshed just before the solve.
    //
    // It is reused for: (1) the flat indices every construct site emits against,
    // (2) the SolverModel::Build() call inside SolveModel(), and (3) gstate.var_indexer
    // for solution readback after the solve. The owning form, so it survives past
    // `solver_input` once that is moved onto gstate.
    VarIndexer var_indexer = VarIndexer::Build(solver_input);

    // ABS FIRST, and the order is load-bearing. Deriving an ABS auxiliary's range is
    // also what boxes its column, and every linearizer below computes its Big-M from
    // column boxes — run them first and an outer MIN/MAX or `<>` over ABS(...) sees an
    // unbounded column and refuses a query whose bound was there to be computed. Only
    // the LOWERING path refuses an underivable range; the native path leaves the
    // auxiliary open and answers.
    DeriveAbsAuxiliaryBounds(solver_input, decide_var_names, !native_abs);
    if (native_abs) {
        EmitNativeAbs(solver_input, var_indexer);
    } else {
        LinearizeAbsMaximize(solver_input);
    }

    // Encode every hard MIN/MAX constraint stage 05 tagged. Native states `z = MAX(t..)`
    // directly and needs no Big-M, so it also needs no bound on the contributing
    // variables; the lowering arm's indicator family does. Both read the same tag and
    // make the same bound classification first.
    if (!minmax_indicator_links.empty()) {
        LinearizeMinMaxConstraints(solver_input, var_indexer, decide_var_names, native_min_max);
    }

    // Encode `<>` as its disjunction: a Big-M pair, or a pair of implications on the
    // backend that states those. Both spellings — per-row against the row-scoped
    // indicator, aggregate against a global binary per group — are finished here.
    if (!ne_indicator_indices.empty()) {
        LinearizeNotEqual(solver_input, var_indexer, aux_var_expressions, decide_var_names,
                          use_native_constructs.not_equal);
    }

    // Emit the McCormick envelope for every bilinear w = b * x auxiliary.
    LinearizeBilinear(solver_input, decide_var_names);

    // Objective (linear part)
    solver_input.objective_coefficients = std::move(gstate.evaluated_objective_coefficients);
    solver_input.objective_variable_indices = std::move(gstate.objective_variable_indices);
    solver_input.sense = decide_sense;

    // Quadratic objective (if present)
    if (gstate.has_quadratic_objective) {
        solver_input.has_quadratic_objective = true;
        solver_input.quadratic_sign = gstate.quadratic_sign;
        solver_input.quadratic_inner_coefficients = std::move(gstate.evaluated_quadratic_coefficients);
        solver_input.quadratic_inner_variable_indices.resize(gstate.quadratic_variable_indices.size());
        for (idx_t i = 0; i < gstate.quadratic_variable_indices.size(); i++) {
            solver_input.quadratic_inner_variable_indices[i] = gstate.quadratic_variable_indices[i];
        }
    }

    // Bilinear objective terms (non-Boolean pairs, for Q matrix off-diagonal entries)
    if (!gstate.evaluated_bilinear_terms.empty()) {
        for (auto &ebt : gstate.evaluated_bilinear_terms) {
            SolverInput::BilinearObjectiveTerm bot;
            bot.var_a = ebt.var_a;
            bot.var_b = ebt.var_b;
            bot.row_coefficients = std::move(ebt.row_coefficients);
            solver_input.bilinear_objective_terms.push_back(std::move(bot));
        }
    }

    // Evaluate PER column for objective grouping (must happen after solver_input is constructed)
    if (gstate.objective && !gstate.objective->per_columns.empty()) {
        bool objective_has_local_filters = false;
        bool objective_has_unfiltered_part = false;
        for (auto &f : obj_linear_term_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }
        for (auto &f : obj_quadratic_term_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }
        for (auto &f : obj_bilinear_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }

        auto objective_row_has_local_term = [&](idx_t row) {
            if (!objective_has_local_filters || objective_has_unfiltered_part) {
                return true;
            }
            for (auto &f : obj_linear_term_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            for (auto &f : obj_quadratic_term_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            for (auto &f : obj_bilinear_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            return false;
        };

        auto obj_row_is_included = [&](idx_t row) -> bool {
            if (objective_has_when && !objective_when_mask[row]) return false;
            if (!objective_row_has_local_term(row)) return false;
            return true;
        };

        vector<string> obj_group_labels; // unused: objective groups are not diagnosed by clause key
        LookupOrBuildPerGroupIds(per_group_cache, gstate.objective->per_columns,
                                 chunk_expr_cache, context, gstate.data, num_rows,
                                 /*null_excludes=*/true, obj_row_is_included,
                                 solver_input.objective_row_group_ids,
                                 solver_input.objective_num_groups, obj_group_labels);
    }

    // Relation-qualified reducers in the objective: same de-duplication as on the
    // constraint side, applied once the objective's PER groups are settled and before
    // AVG scaling reads the surviving-row counts.
    if (gstate.objective) {
        for (idx_t term_idx = 0; term_idx < gstate.objective->terms.size() &&
                                 term_idx < obj_linear_term_filters.size();
             term_idx++) {
            idx_t scope_idx = gstate.objective->terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.entity_mappings, scope_idx,
                                   solver_input.objective_row_group_ids, num_rows,
                                   obj_linear_term_filters[term_idx]);
            MaskCoefficientColumn(solver_input.objective_coefficients[term_idx],
                                  obj_linear_term_filters[term_idx].mask);
        }
        for (idx_t term_idx = 0; term_idx < gstate.objective->squared_terms.size() &&
                                 term_idx < obj_quadratic_term_filters.size();
             term_idx++) {
            idx_t scope_idx = gstate.objective->squared_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.entity_mappings, scope_idx,
                                   solver_input.objective_row_group_ids, num_rows,
                                   obj_quadratic_term_filters[term_idx]);
            MaskCoefficientColumn(solver_input.quadratic_inner_coefficients[term_idx],
                                  obj_quadratic_term_filters[term_idx].mask);
        }
        for (idx_t term_idx = 0; term_idx < gstate.objective->bilinear_terms.size() &&
                                 term_idx < obj_bilinear_filters.size();
             term_idx++) {
            idx_t scope_idx = gstate.objective->bilinear_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.entity_mappings, scope_idx,
                                   solver_input.objective_row_group_ids, num_rows,
                                   obj_bilinear_filters[term_idx]);
            MaskCoefficientColumn(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
                                  obj_bilinear_filters[term_idx].mask);
        }
    }

    for (idx_t term_idx = 0; term_idx < obj_linear_term_filters.size(); term_idx++) {
        if (!obj_linear_term_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleAvgRows(solver_input.objective_coefficients[term_idx],
                    obj_linear_term_filters[term_idx].has_filter,
                    obj_linear_term_filters[term_idx].mask, /*quadratic_inner=*/false, num_rows,
                    solver_input.objective_row_group_ids, solver_input.objective_num_groups,
                    objective_has_when ? &objective_when_mask : nullptr);
    }
    for (idx_t term_idx = 0; term_idx < obj_quadratic_term_filters.size(); term_idx++) {
        if (!obj_quadratic_term_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleAvgRows(solver_input.quadratic_inner_coefficients[term_idx],
                    obj_quadratic_term_filters[term_idx].has_filter,
                    obj_quadratic_term_filters[term_idx].mask, /*quadratic_inner=*/true, num_rows,
                    solver_input.objective_row_group_ids, solver_input.objective_num_groups,
                    objective_has_when ? &objective_when_mask : nullptr);
    }

    for (idx_t term_idx = 0; term_idx < obj_bilinear_filters.size(); term_idx++) {
        if (!obj_bilinear_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleAvgRows(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
                    obj_bilinear_filters[term_idx].has_filter,
                    obj_bilinear_filters[term_idx].mask, /*quadratic_inner=*/false, num_rows,
                    solver_input.objective_row_group_ids, solver_input.objective_num_groups,
                    objective_has_when ? &objective_when_mask : nullptr);
    }

    // Handle MIN/MAX objective: create global auxiliary variable z and linking constraints.
    // Two paths: (A) non-PER flat MIN/MAX, (B) PER with nested OUTER(INNER(expr)).
    //
    // For PER objectives, two-level auxiliary formulation:
    //   Phase A (inner): per-group auxiliary z_g for inner MIN/MAX aggregate
    //   Phase B (outer): global auxiliary w for outer MIN/MAX aggregate
    //
    // Easy/hard classification at each level:
    //   Easy (no indicators): MINIMIZE+MAX or MAXIMIZE+MIN
    //   Hard (Big-M indicators): MINIMIZE+MIN or MAXIMIZE+MAX

    // Global variables are appended at var_indexer.global_block_start.
    // As we add more global vars, their indices are global_block_start + g
    // where g is the position in the global vars array.

    // Encode a MIN/MAX objective (flat `MIN(expr)`/`MAX(expr)` or the nested
    // `OUTER(INNER(expr)) PER key` spelling) into global auxiliaries and their
    // envelope/indicator rows. Pure stage-06 work: it reads the coefficients PHASE 2
    // evaluated plus the flat column space, and needs no row of its own.
    MinMaxObjectiveSpec minmax_objective_spec;
    minmax_objective_spec.flat_agg = flat_objective_agg;
    minmax_objective_spec.flat_is_easy = flat_objective_is_easy;
    minmax_objective_spec.per_inner_agg = per_inner_agg;
    minmax_objective_spec.per_outer_agg = per_outer_agg;
    minmax_objective_spec.per_inner_is_easy = per_inner_is_easy;
    minmax_objective_spec.per_outer_is_easy = per_outer_is_easy;
    minmax_objective_spec.per_inner_was_avg = per_inner_was_avg;
    minmax_objective_spec.has_when = objective_has_when;
    minmax_objective_spec.when_mask = objective_when_mask;
    LinearizeMinMaxObjective(solver_input, var_indexer, minmax_objective_spec, decide_var_names,
                             native_min_max);

    // ================================================================
    // Composed MIN/MAX: additive clauses mixing SUM/AVG/MIN/MAX, in a constraint
    // (`SUM(a) + MAX(b) <= K`) or in the objective. This layer evaluates the parts
    // that need a row — each inner term's per-row coefficient, the query-wide factor
    // on a reducer, the term's WHEN mask, the constant RHS — and hands the result to
    // stage 06, which owns the auxiliary, envelope and indicator emission.
    // ================================================================
    if (!composed_minmax_constraints.empty() || !composed_minmax_objective_terms.empty()) {
        // Evaluate a DecideTerm's per-row coefficient (scaled by term.sign). `what` is
        // "constraint" or "objective", naming the clause in any error.
        auto EvaluateTermCoefs = [&](const DecideTerm &term, const char *what) -> vector<double> {
            vector<double> coefs;
            coefs.reserve(num_rows);
            const Expression &transformed =
                CachedTransformToChunkExpression(chunk_expr_cache, *term.coefficient, context);
            ExpressionExecutor exec(context);
            exec.AddExpression(transformed);
            ColumnDataScanState scan;
            gstate.data.InitializeScan(scan);
            DataChunk chunk;
            chunk.Initialize(context, gstate.data.Types());
            while (gstate.data.Scan(scan, chunk)) {
                DataChunk result;
                result.Initialize(context, {transformed.return_type});
                exec.Execute(chunk, result);
                for (idx_t r = 0; r < chunk.size(); r++) {
                    Value val = result.data[0].GetValue(r);
                    if (val.IsNull()) {
                        throw InvalidInputException(
                            "Composed MIN/MAX %s: coefficient expression returned NULL.", what);
                    }
                    double d = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    if (!std::isfinite(d)) {
                        throw InvalidInputException(
                            "Composed MIN/MAX %s: coefficient is not finite (NaN/Inf).", what);
                    }
                    coefs.push_back(d * term.sign);
                }
            }
            return coefs;
        };

        //! A factor on a reducer is one value for the whole query by construction (the
        //! canonicalizer rejects anything row-varying), so evaluate it once and read
        //! row 0. It goes through the same chunk-expression transform every other
        //! coefficient does -- a flattened scalar subquery is a column reference, and
        //! reading it without that transform would index the chunk by a logical column
        //! index.
        //!
        //! `scale` must be the expression OWNED by the term, never a copy:
        //! CachedTransformToChunkExpression keys its cache on the Expression's address,
        //! so a temporary would leave a dangling key that a later allocation at the
        //! same address silently inherits.
        auto EvaluateQueryWideScale = [&](const Expression &scale, bool divides,
                                          const char *what) -> double {
            const Expression &transformed =
                CachedTransformToChunkExpression(chunk_expr_cache, scale, context);
            ExpressionExecutor exec(context);
            exec.AddExpression(transformed);
            ColumnDataScanState scan;
            gstate.data.InitializeScan(scan);
            DataChunk chunk;
            chunk.Initialize(context, gstate.data.Types());
            double v = 1.0;
            bool got = false;
            while (!got && gstate.data.Scan(scan, chunk)) {
                if (chunk.size() == 0) {
                    continue;
                }
                DataChunk result;
                result.Initialize(context, {transformed.return_type});
                exec.Execute(chunk, result);
                Value val = result.data[0].GetValue(0);
                if (val.IsNull()) {
                    throw InvalidInputException(
                        "DECIDE: the factor on an aggregate evaluated to NULL.");
                }
                v = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                if (!std::isfinite(v)) {
                    throw InvalidInputException(
                        "DECIDE: the factor on an aggregate is not finite (NaN/Inf).");
                }
                got = true;
            }
            if (!got) {
                return 1.0;
            }
            if (divides) {
                if (v == 0.0) {
                    throw InvalidInputException(
                        "DECIDE %s: division by zero — the factor on an aggregate "
                        "evaluated to 0.", what);
                }
                return 1.0 / v;
            }
            return v;
        };

        // Evaluate one composed clause's terms into the stage-06 input. `what` is
        // "constraint" or "objective"; it names the clause in every error raised here.
        auto EvaluateComposedTerms =
            [&](const vector<LogicalDecide::ComposedMinMaxTerm> &terms,
                const char *what) -> vector<ComposedMinMaxTermData> {
            // Collect filter expressions for batch evaluation (one scan for all terms).
            vector<const Expression *> cond_ptrs;
            for (auto &term : terms) {
                if (term.filter) cond_ptrs.push_back(term.filter.get());
            }
            auto masks = EvaluateBooleanMasks(cond_ptrs, chunk_expr_cache, context, gstate.data, num_rows);

            vector<ComposedMinMaxTermData> evaluated;
            idx_t mask_slot = 0;
            for (auto &term : terms) {
                ComposedMinMaxTermData ta;
                ta.is_minmax = (term.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND);
                ta.agg_name = term.agg_name;
                ta.sign = term.sign;
                ta.is_easy = term.is_easy;
                if (term.scale) {
                    ta.scale = EvaluateQueryWideScale(*term.scale, term.scale_divides, what);
                }
                if (ta.is_minmax) {
                    ta.label = StringUtil::Upper(term.agg_name) + "(" + term.inner_expr->ToString() + ")";
                }
                ta.inner_terms = &term.inner_terms;
                for (auto &inner_t : term.inner_terms) {
                    ta.per_term_coefs.push_back(EvaluateTermCoefs(inner_t, what));
                }
                if (term.filter) {
                    ta.filter_mask = std::move(masks[mask_slot++]);
                } else {
                    ta.filter_mask.assign(num_rows, true);
                }
                // Fold in the relation qualifier's de-duplication mask, exactly as the
                // non-composed reducer paths do. Composed v1 has no outer PER, so every
                // row is in one group and `row_group_ids` is empty. Applied uniformly:
                // for MIN/MAX it is provably a no-op (every row of an identity carries the
                // same value, so dropping repeats cannot move an extremum), which keeps
                // one code path instead of a special case that has to stay in sync.
                if (term.qualifier_scope_idx != DConstants::INVALID_INDEX) {
                    auto keep = BuildQualifierKeepMask(solver_input.entity_mappings,
                                                       term.qualifier_scope_idx, {}, num_rows);
                    for (idx_t row = 0; row < num_rows; row++) {
                        ta.filter_mask[row] = ta.filter_mask[row] && keep[row];
                    }
                }
                evaluated.push_back(std::move(ta));
            }

            // Reject an empty row set before stage 06 sees the term. For MIN/MAX the
            // auxiliary would float free (no per-row pinning), silently vacating the
            // clause; for SUM/AVG an empty SUM contributes a vacuous 0 and an empty AVG
            // would divide by zero. MIN/MAX first, so a clause with both reports the
            // MIN/MAX term.
            string ctx = "composed " + string(what);
            for (auto &ta : evaluated) {
                if (!ta.is_minmax) continue;
                idx_t cnt = 0;
                for (bool m : ta.filter_mask) if (m) cnt++;
                RejectEmptyAggregate(cnt, ta.agg_name.c_str(), ctx.c_str());
            }
            for (auto &ta : evaluated) {
                if (ta.is_minmax) continue;
                idx_t cnt = 0;
                for (bool m : ta.filter_mask) if (m) cnt++;
                RejectEmptyAggregate(cnt, ta.agg_name.c_str(), ctx.c_str());
            }
            return evaluated;
        };

        for (auto &spec : composed_minmax_constraints) {
            // RHS must be row-invariant in v1. Decide that by foldability rather
            // than by matching a literal node: canonicalization rebuilds the bound
            // side as an additive chain, so a constant arrives as `(0 - 3) + 0`
            // just as legitimately as `-3`. IsFoldable is the same test the
            // per-row constraint path historically used for a shared scalar RHS.
            if (!spec.rhs_expr->IsFoldable()) {
                throw BinderException(
                    "Composed MIN/MAX in DECIDE v1 requires a constant RHS; got '%s'.",
                    spec.rhs_expr->ToString());
            }
            double rhs_val = ExpressionExecutor::EvaluateScalar(context, *spec.rhs_expr)
                                 .DefaultCastAs(LogicalType::DOUBLE)
                                 .GetValue<double>();

            auto evaluated = EvaluateComposedTerms(spec.terms, "constraint");
            LinearizeComposedMinMaxConstraint(solver_input, var_indexer, evaluated, rhs_val,
                                              spec.outer_cmp, spec.source_clause_id, decide_var_names,
                                              native_min_max);
        }

        if (!composed_minmax_objective_terms.empty()) {
            auto evaluated = EvaluateComposedTerms(composed_minmax_objective_terms, "objective");
            LinearizeComposedMinMaxObjective(solver_input, var_indexer, evaluated, decide_var_names,
                                             native_min_max);
        }
    }

    // Refresh total_vars: the row/entity blocks were finalized at construction,
    // but global aux vars were appended throughout deferred-NE and MIN/MAX expansion.
    var_indexer.total_vars = var_indexer.global_block_start + solver_input.num_global_vars;

    out_var_indexer = std::move(var_indexer);
    return solver_input;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 3, solve + readback -- FinalizeSolveResult
//===--------------------------------------------------------------------===//

SinkFinalizeType PhysicalDecide::FinalizeSolveResult(ClientContext &context, DecideGlobalSinkState &gstate,
                                                     SolverInput &solver_input, VarIndexer &var_indexer,
                                                     bool bench, Profiler &model_timer,
                                                     Profiler &solver_timer) const {
    idx_t num_rows = solver_input.num_rows;

    // Capture model size before solve (solver may move data)
    size_t bench_total_vars = var_indexer.total_vars;
    // Clause-level model size: the number of constraint *specs* the operator hands the
    // builder. A per-row spec still expands to one row per data row and a PER spec to one
    // row per group, so this is not a matrix row count — the row count is read back off
    // the built model (`solve_result.model_constraint_rows`) after the solve below.
    size_t bench_constraint_specs = solver_input.constraints.size() + solver_input.global_constraints.size();

    if (bench) {
        model_timer.End();
        solver_timer.Start();
    }

    // F4: read the diagnose_decide setting (auto by default; off suppresses). Under
    // auto, arm diagnosis prep: pre-extract the unbounded ray so it is ready if the
    // solve turns out unbounded, and retain the built model so the INFEASIBLE terminal
    // can hand it to the elastic engine. off pays for neither, and both failure
    // terminals are auto-only anyway (RouteSolveResult).
    string diagnose_mode = GetDiagnoseDecideMode(context);
    bool diagnosis_armed = DiagnoseModeArmsDiagnosis(diagnose_mode);
    SolveModelOptions solve_options;
    solve_options.extract_unbounded_ray = diagnosis_armed;
    // Keep an inverted column box (col_lower > col_upper) alive through Build under
    // diagnosis so the INFEASIBLE terminal can reset it to the intrinsic domain and
    // diagnose it (Bug 1, all-column-bound conflicts). Off mode keeps the fast throw.
    solver_input.tolerate_infeasible_bounds = diagnosis_armed;

    // S3/S4: slow-solve continuation. `decide_on_timeout` governs what a time-limit
    // stop does *under diagnose auto* (off is a master mute — RouteSolveResult never
    // routes TIME_LIMIT to its terminal when diagnosis isn't armed). `ask` needs a
    // human, so it falls back to `error` when stdin is not a terminal (tests,
    // benchmarks, -c pipes, C-API) — never prompt where no one can answer. Only the
    // continue-capable modes need the warm solver retained across the timeout.
    string on_timeout = GetDecideOnTimeoutMode(context);
    bool stdin_is_tty = isatty(STDIN_FILENO) != 0;
    string eff_on_timeout = (on_timeout == "ask" && !stdin_is_tty) ? "error" : on_timeout;
    bool want_session = diagnosis_armed && eff_on_timeout != "error";
    // The first solve chunk uses the same per-chunk budget every Continue() chunk will,
    // so elapsed climbs in even multiples (300s, 600s, ...) across the loop.
    solve_options.time_limit_seconds = ResolveDecideTimeLimit();
    // Ctrl-C is a peer trigger into the slow branch: arm the interrupt poll on the
    // *first* solve (decoupled from the timeout mode / retained session), so a user
    // interrupt on any armed solve cuts it short and routes into the TIME_LIMIT
    // terminal with the best-so-far — instead of aborting the query. `off` stays plain
    // (no poll). Gurobi honors it mid-solve; HiGHS stays boundary-only.
    if (diagnosis_armed) {
        solve_options.interrupt_poll = [&context]() { return context.interrupted.load(); };
    }
    SolveModelOptions diagnostic_solve_options;
    diagnostic_solve_options.time_limit_seconds =
        ResolveDecideDiagnosticTimeLimit(solve_options.time_limit_seconds);
    diagnostic_solve_options.interrupt_poll = solve_options.interrupt_poll;

    // Reconcile the `<>` label channel with the final global-var count. Only the
    // aggregate-`<>` site labels its global z (for the infeasible removal dial),
    // and those z's are allocated first (the deferred-NE loop runs before all
    // MIN/MAX linking), so the labels form a contiguous prefix; pad the trailing
    // unlabeled MIN/MAX globals with "" to keep the vector parallel with
    // global_variable_types. (Invariant: never shrinks — only labeled globals
    // precede the resize point.)
    D_ASSERT(solver_input.global_variable_labels.size() <= solver_input.num_global_vars);
    solver_input.global_variable_labels.resize(solver_input.num_global_vars);

    // Retained only when diagnosis is armed; the move is trivial and the model is
    // freed when Finalize returns. SolveModel otherwise discards the built model.
    SolverModel retained_model;
    // Retained only when a continue-capable timeout mode is active: the live warm
    // solver, so the TIME_LIMIT terminal can Continue() it for more wall-clock.
    unique_ptr<SolverSession> retained_session;
    // Always-on wall-clock around the solve (independent of DECIDB_BENCH): the slow
    // checkpoint report (S2) needs the elapsed solve time to show the user.
    Profiler solve_wall_timer;
    solve_wall_timer.Start();
    SolverResult solve_result =
        SolveModel(solver_input, var_indexer, PlannedSolverBackend(), solve_options,
                   diagnosis_armed ? &retained_model : nullptr,
                   want_session ? &retained_session : nullptr);
    solve_wall_timer.End();

    // Per-decide-variable labels + is-aux flags for column provenance. Both failure
    // terminals need them (UNBOUNDED names the runaway variable; INFEASIBLE carries
    // them into the elastic engine), so build them once here; invoked lazily inside a
    // failing arm so the SOLVED path pays nothing.
    auto build_var_labels = [&](vector<string> &var_labels, vector<bool> &var_is_aux) {
        idx_t nvars = decide_variables.size();
        var_labels.assign(nvars, string());
        var_is_aux.assign(nvars, false);
        for (auto &ae : aux_var_expressions) {
            if (ae.first < nvars) {
                var_labels[ae.first] = ae.second;
                var_is_aux[ae.first] = true;
            }
        }
        for (idx_t i = 0; i < nvars; i++) {
            if (!var_is_aux[i]) {
                var_labels[i] = decide_variables[i]->GetName();
            }
        }
    };

    // Route the solve outcome to its diagnosis terminal. RouteSolveResult is a pure
    // classifier (status + mode → terminal); the operator owns the engine call,
    // stash, and throw for each terminal.
    // Set when a caveated success (an unproven incumbent returned as rows) has stashed a
    // `state='slow'` diagnosis it wants to survive: the success epilogue's blanket
    // ClearDecideDiagnostic must then spare it, so the quality stays queryable.
    bool keep_slow_diagnosis = false;
    switch (RouteSolveResult(solve_result, diagnose_mode)) {
    case DiagnosisTerminal::SOLVED:
        if (solve_result.status == SolverStatus::SUBOPTIMAL) {
            // A feasible incumbent returned without a proof of optimality (a numerically
            // hard QCP where the barrier stopped before satisfying tolerances). Deliver
            // the rows, but say plainly they are not proven best — the same caveat as an
            // early time-limit stop. There is no live SQL NOTICE channel, so it rides on
            // stderr with the results.
            fprintf(stderr,
                    "DECIDE is returning a feasible solution — the solver could not prove it is the best possible.\n");
            // Machine-readable twin of the caveat above, for the test harness only. The
            // caveat's wording is owned by the user-facing-output principle and will keep
            // moving; a suite that greps its prose silently stops matching (it already did
            // once, turning the QCQP retry into a flake). Tests key on this instead, and
            // `decidb_cli.py` strips DECIDB_STATUS lines before classifying stderr. Gated
            // on the env var so user-facing output is unchanged — same pattern as
            // DECIDB_BENCH instrumentation.
            if (std::getenv("DECIDB_STATUS_MARKERS")) {
                fprintf(stderr, "DECIDB_STATUS: SUBOPTIMAL\n");
            }
        }
        break; // fall through to the success stores below
    case DiagnosisTerminal::UNBOUNDED: {
        vector<string> var_labels;
        vector<bool> var_is_aux;
        build_var_labels(var_labels, var_is_aux);

        auto diag_params = GetDecideDiagnosticParams(context);
        auto get_candidates =
            BuildUnboundedCandidateProvider(context, gstate.data, gstate.data.Types(), diag_params,
                                            input_column_names, var_indexer, entity_scopes, num_rows);

        UnboundedDiagnosisInput diag_input {
            solve_result, var_indexer, var_labels, var_is_aux, diag_params, get_candidates,
        };
        DecideDiagnostic diag = DiagnoseUnbounded(diag_input);
        if (diag.valid && !diag.rows.empty()) {
            StashDecideDiagnostic(context, diag);
            string extra_message;
            if (solve_result.status == SolverStatus::INF_OR_UNBD) {
                extra_message = "It may instead be infeasible.";
            }
            ThrowDecideDiagnosisReady(diag, extra_message);
        }
        // Diagnosis was requested but produced no per-variable content. Say why it
        // is unavailable rather than throwing the generic "enable diagnosis and
        // re-run" advert — that advert is misleading here, since the mode is already
        // on and re-running cannot help. An empty ray is a non-linear limitation only
        // when the retained model is actually non-linear; otherwise use a neutral
        // "could not identify" reason. A present ray that named nothing means only
        // internal auxiliaries escaped.
        bool has_nonlinear_terms = retained_model.has_quadratic_obj ||
                                   !retained_model.quadratic_constraints.empty() ||
                                   !retained_model.general_constraints.empty() ||
                                   !retained_model.indicator_constraints.empty();
        string reason = BuildUnboundedDiagnosisUnavailableReason(
            solve_result.diagnostic_timed_out, solve_result.ray.empty(), has_nonlinear_terms);
        // This failure produced no diagnosis of its own; clear any stash left by an
        // earlier failed solve so decide_diagnostics() cannot be misread as being
        // about this query (A4).
        ClearDecideDiagnostic(context);
        ThrowUnboundedDiagnosisUnavailable(reason);
    }
    case DiagnosisTerminal::INFEASIBLE: {
        // A residual INF_OR_UNBD is routed here only with an empty ray; normalize it
        // so the message (and any diagnosis) reads as infeasible.
        SolverResult terminal_result = solve_result;
        if (terminal_result.status == SolverStatus::INF_OR_UNBD) {
            terminal_result.status = SolverStatus::INFEASIBLE;
        }
        // No model was retained: SolveModel returned INFEASIBLE from a Build that threw
        // before it finished (conflicting column bounds with diagnosis off), so
        // `retained_model` is still default-constructed — no columns, no rows. Every
        // step below indexes it by column, so bail to the static error instead. Diagnosis
        // has nothing to work from; saying so plainly beats walking an empty model.
        if (retained_model.num_vars == 0) {
            ClearDecideDiagnostic(context);
            ThrowDecideSolveError(terminal_result);
        }
        vector<string> var_labels;
        vector<bool> var_is_aux;
        build_var_labels(var_labels, var_is_aux);

        auto diag_params = GetDecideDiagnosticParams(context);
        // The backend the primary solve ran on, named at plan time. Asking
        // SelectSolverBackend() again here would be a second, independent answer —
        // harmless while selection reads only the environment, but wrong the moment it
        // depends on the model, because the elastic re-solves would then run on a
        // different solver than the one that produced the failure being diagnosed.
        SolverBackend backend = PlannedSolverBackend();

        // Decision 1a: a user constraint like `x <= 10` / `x BETWEEN a AND b` was
        // absorbed into the column-bound arrays, not emitted as a matrix row, so it
        // is invisible to the elastic engine. Re-emit each absorbed user bound as a
        // USER_PARAMETER slackable row on the retained model and relax the rigid
        // column bound it produced, so the bound is enforced only by the (loosenable)
        // row. The bound `x <= k` is ONE editable knob; on a multi-instance variable
        // it fans into one row per instance under a single (synthetic) clause id and
        // shape SHARED_SCALAR, so the elastic engine collapses them to one shared
        // slack and reports the max overshoot (I2.a). Single-instance is the N=1 case
        // (a size-1 block). Only genuine user bounds reach here — a variable's
        // intrinsic domain (BOOLEAN 0/1, default non-negativity) is never recorded in
        // user_absorbed_bounds (see DecideOptimizer::AbsorbVariableBounds), so it stays
        // rigid.
        // A BOOLEAN pin (`x <= 0`, `x >= 1`, `x = 1`) IS recorded; its column is
        // opened only back to the intrinsic [0,1] — never past it — so the pin
        // becomes loosenable while the 0/1 domain itself stays rigid.
        // C3: if a recorded bound cannot be re-emitted (its column is missing from
        // the retained model), the elastic model is missing a user constraint —
        // flag it so the engine won't claim an elastic-infeasible verdict.
        bool has_unhandled_user_bounds = false;
        // The synthetic ids must be FRESH: the elastic engine groups rows into one
        // shared slack by `repair_group_id`, so an id already in use silently welds an
        // absorbed bound onto an unrelated clause and reports one edit for both. They
        // are read off the model rather than derived from a count, because no count is
        // the id space. A regular constraint takes its index in `solver_input.constraints`
        // and a GLOBAL one takes `constraints.size() + its own index`, so the two ranges
        // are adjacent: starting at `constraints.size()` lands on top of the first global
        // constraint. That collision is invisible whenever the linear specs outnumber the
        // ids actually in use, and appears as soon as a construct is stated natively —
        // fewer linear specs, same ids — which is how it was found. Taking the max in use
        // cannot drift with either allocation rule.
        idx_t synthetic_clause_id = 0;
        for (const auto &row : retained_model.constraints) {
            if (row.provenance.repair_group_id != DConstants::INVALID_INDEX &&
                row.provenance.repair_group_id + 1 > synthetic_clause_id) {
                synthetic_clause_id = row.provenance.repair_group_id + 1;
            }
        }
        for (const auto &b : gstate.user_absorbed_bounds) {
            idx_t num_instances = var_indexer.NumInstances(b.decide_var_idx);
            idx_t bound_clause_id = synthetic_clause_id++;
            bool bound_is_bool = b.decide_var_idx < is_boolean_var.size() &&
                                 is_boolean_var[b.decide_var_idx];
            for (idx_t inst = 0; inst < num_instances; inst++) {
                idx_t col = var_indexer.Get(b.decide_var_idx, inst);
                if (col >= retained_model.num_vars) {
                    has_unhandled_user_bounds = true;
                    continue;
                }
                // Relax the rigid column bound for the direction this spec enforces,
                // so the bound is enforced only by the loosenable row: to the model's
                // ±infinity (1e30 / -1e30), or for a BOOLEAN to its intrinsic [0,1].
                if (b.sense == '<' || b.sense == '=') {
                    retained_model.col_upper[col] = bound_is_bool ? 1.0 : 1e30;
                }
                if (b.sense == '>' || b.sense == '=') {
                    retained_model.col_lower[col] = bound_is_bool ? 0.0 : -1e30;
                }
                ModelConstraint row;
                row.indices.push_back(static_cast<int>(col));
                row.coefficients.push_back(1.0);
                row.sense = b.sense;
                row.rhs = b.k;
                row.provenance.source_clause_id = b.source_clause_id;
                row.provenance.repair_group_id = bound_clause_id;
                row.provenance.group_key = DConstants::INVALID_INDEX;
                row.provenance.kind = ConstraintKind::USER_PARAMETER;
                row.provenance.shape = ElasticShape::SHARED_SCALAR;
                // Mirror the strict re-quote stamped by ApplyComparisonSense on the
                // non-absorbed path, so `x < 10` reports `< 10` → `< 16`, not `<= 9`.
                row.provenance.strict = b.strict;
                row.provenance.typed_k = b.typed_k;
                retained_model.constraints.push_back(std::move(row));
            }
        }

        // Bug 3: a single-variable user row like `x <= 2+3` is ALSO copied into the rigid
        // column box by DecidePropagateImpliedBounds (a presolve tightening that keeps no
        // provenance, so it is not in user_absorbed_bounds). The slackable row stays pinned
        // behind that rigid bound. Reset every non-binary DECIDE column back to its intrinsic
        // domain so the (loosenable) row is the sole enforcer. Implied tightenings only ever
        // RAISE the lower above 0 or LOWER the upper below +inf (propagation never loosens),
        // so clamping `lower>0 → 0` and `upper → +inf` reverses exactly the implied part:
        //   - a user-bounded direction was already opened to ±1e30 by the loop above (kept);
        //   - the intrinsic default (lower 0 / upper +inf) is unchanged;
        //   - an implied tightening (lower>0 / upper<+inf) is reverted — its backing row
        //     (USER_PARAMETER slackable, or STRUCTURAL still-rigid) continues to enforce it.
        // BOOLEAN columns reset only within [0,1] so the intrinsic box stays rigid. NOTE:
        // `is_binary[col]` is also true here (the DomainSpec fix reports BOOLEAN-domain
        // variables as `LogicalType::BOOLEAN` to the model builder, same as `is_boolean_var`),
        // but this checks `is_boolean_var[var]` explicitly and FIRST, ahead of the generic
        // `is_binary` branch below. The generic branch only *skips* — correct for an
        // optimizer-created BOOLEAN indicator (MIN/MAX z, NE), which DecidePropagateImpliedBounds
        // never tightens — but a domain-BOOL variable (declared `x(BOOL)`, or an IN/L0
        // indicator) CAN be tightened by it (the budget example below), and skipping would
        // leave that stale sub-1 pin in place. Using the generic is_binary skip here for
        // BOOLEAN-domain variables would silently reintroduce that bug.
        for (idx_t var = 0; var < decide_variables.size(); var++) {
            bool is_bool = var < is_boolean_var.size() && is_boolean_var[var];
            idx_t num_instances = var_indexer.NumInstances(var);
            for (idx_t inst = 0; inst < num_instances; inst++) {
                idx_t col = var_indexer.Get(var, inst);
                if (col >= retained_model.num_vars) {
                    continue;
                }
                if (is_bool) {
                    // A BOOLEAN's intrinsic domain is [0,1], but DecidePropagateImpliedBounds
                    // can tighten it by absorbing a user row — e.g. `buy_i ≤ K/priceᵢ` from a
                    // budget `SUM(priceᵢ·buyᵢ) ≤ K`. With a fractional upper (<1) an INTEGER
                    // buy is silently pinned to 0, so the relaxable budget can never be
                    // exercised and the only "fix" is gutting the other constraint. Revert to
                    // [0,1] so the (now-slackable) row is the sole enforcer — but never open
                    // past 1, which would unbound the variable.
                    if (retained_model.col_lower[col] > 0.0) {
                        retained_model.col_lower[col] = 0.0;
                    }
                    if (retained_model.col_upper[col] < 1.0) {
                        retained_model.col_upper[col] = 1.0;
                    }
                    continue;
                }
                if (retained_model.is_binary[col]) {
                    continue;
                }
                if (retained_model.col_lower[col] > 0.0) {
                    retained_model.col_lower[col] = 0.0;
                }
                if (retained_model.col_upper[col] < 1e30) {
                    retained_model.col_upper[col] = 1e30;
                }
            }
        }

        bool diagnostic_solve_timed_out = terminal_result.diagnostic_timed_out;
        InfeasibleDiagnosisInput diag_input {
            retained_model, var_indexer, var_labels, var_is_aux,
            solver_input.global_variable_labels, diag_params,
            has_unhandled_user_bounds,
            [backend, diagnostic_solve_options, &diagnostic_solve_timed_out](const SolverModel &m) {
                SolverResult result = SolvePreparedModel(m, backend, diagnostic_solve_options);
                if (result.status == SolverStatus::TIME_LIMIT) {
                    diagnostic_solve_timed_out = true;
                }
                return result;
            },
        };
        DecideDiagnostic diag = DiagnoseInfeasible(diag_input);
        if (diag.valid && !diag.rows.empty()) {
            StashDecideDiagnostic(context, diag);
            ThrowDecideDiagnosisReady(diag);
        }
        // The elastic engine can still decline to report when no actionable relaxation
        // exists; keep the plain static infeasible error as the fallback. No new
        // diagnosis was stashed, so clear any stale one from an earlier failure (A4).
        ClearDecideDiagnostic(context);
        if (diagnostic_solve_timed_out) {
            string state = solve_result.status == SolverStatus::INF_OR_UNBD ? "infeasible or unbounded"
                                                                            : "infeasible";
            throw InvalidInputException(
                "DECIDE optimization is " + state +
                ": diagnosis ran out of time before it could find a least-change repair.");
        }
        ThrowDecideSolveError(terminal_result);
    }
    case DiagnosisTerminal::TIME_LIMIT: {
        // S2/S3/S4: at each chunk boundary print the checkpoint report (what was found
        // + how far it can still improve + elapsed/memory), then act per
        // `decide_on_timeout`: error → stop; ask → prompt the user; continue → keep
        // resuming automatically. "Continue" re-runs the SAME warm solver
        // (retained_session) for another fresh chunk — the MIP search resumes, elapsed
        // accumulates. On stop, a usable incumbent is returned as a SUCCESSFUL result
        // (with a plain stderr caveat that it is not proven best); no incumbent falls
        // to the existing timeout error.
        double chunk = ResolveDecideTimeLimit();
        double cum_elapsed = solve_wall_timer.Elapsed();
        double cum_budget = chunk;

        bool has_objective = decide_objective != nullptr || !composed_minmax_objective_terms.empty();

        if (eff_on_timeout == "error") {
            // Print the checkpoint once, then error — `error` never returns the
            // incumbent (that is the ask/continue stop behavior below). This is also
            // the non-TTY fallback for `ask`, so it preserves today's report-then-error
            // behavior for tests / benchmarks / pipes. Stash the structured mirror + point
            // to decide_diagnostics(), like the unbounded / infeasible terminals.
            PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget);
            DecideDiagnostic diag = BuildTimeoutDiagnostic(solve_result, cum_elapsed, has_objective);
            StashDecideDiagnostic(context, diag);
            ThrowDecideDiagnosisReady(diag);
        }

        // The interrupt poll installed on the first solve persists on the retained session
        // into every Continue() chunk, so `continue` keeps its mid-chunk Ctrl-C for free.
        // `ask`, though, stops the user at each prompt already, and a watcher thread
        // contending with the interactive getline destabilizes the prompt (found
        // empirically) — so reset it to boundary-only here. The entry interrupt already
        // fired *before* this loop, so first-solve Ctrl-C still routed us in; only within
        // the ask loop is it boundary-only. HiGHS ignores the poll either way.
        if (retained_session && eff_on_timeout == "ask") {
            retained_session->SetInterruptPoll({});
        }

        // ask / continue: report at each boundary, then continue or stop.
        for (;;) {
            PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget);

            bool go;
            if (eff_on_timeout == "continue") {
                // Auto-continue until the solver finishes on its own. Ctrl-C (the
                // query interrupt) breaks out at this checkpoint boundary — v1 has no
                // mid-chunk interrupt (see slow/todo.md).
                go = !context.interrupted;
            } else {
                // "ask": stdin is a terminal (eff_on_timeout guaranteed this), so it is
                // safe to block reading the user's decision. The CLI's line editor is
                // inactive mid-execution, so a plain getline does not fight it.
                if (solve_result.has_solution) {
                    fprintf(stderr, "Keep improving it?  [Enter] continue +%s  ·  s + Enter to stop and take this solution: ",
                            FormatDuration(chunk).c_str());
                } else {
                    fprintf(stderr, "Keep searching?  [Enter] continue +%s  ·  s + Enter to give up: ",
                            FormatDuration(chunk).c_str());
                }
                fflush(stderr);
                string line;
                if (!std::getline(std::cin, line)) {
                    go = false; // EOF (Ctrl-D / closed input) → stop
                } else {
                    StringUtil::Trim(line);
                    go = StringUtil::Lower(line) != "s";
                }
            }
            if (!go) {
                break;
            }

            // Resume the warm solver for another chunk (warm start is automatic — the
            // solver never left scope).
            Profiler chunk_timer;
            chunk_timer.Start();
            solve_result = retained_session->Continue(chunk);
            chunk_timer.End();
            cum_elapsed += chunk_timer.Elapsed();
            cum_budget += chunk;

            if (solve_result.status == SolverStatus::OPTIMAL) {
                break; // proven optimum — fall through to the shared success stores
            }
            if (solve_result.status != SolverStatus::TIME_LIMIT) {
                // A resume can only reach a definitive INFEASIBLE/UNBOUNDED when no
                // incumbent ever existed (an incumbent proves feasibility); surface it
                // as the plain solver error. No new diagnosis was stashed for this
                // resume, so clear any stale one from an earlier failure (A4).
                ClearDecideDiagnostic(context);
                ThrowDecideSolveError(solve_result);
            }
            // else: another time-limit stop — loop and re-report with fresh numbers.
        }

        // Loop exited on a stop decision or an OPTIMAL break.
        if (solve_result.status == SolverStatus::OPTIMAL || solve_result.has_solution) {
            if (solve_result.status != SolverStatus::OPTIMAL) {
                // Stopped early with a usable-but-unproven incumbent: succeed and
                // return it, but say plainly it is not proven best. There is no live
                // SQL NOTICE channel, so this rides on stderr with the report.
                fprintf(stderr,
                        "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n");
                // Also stash the quality as a queryable `state='slow'` diagnosis and keep it
                // past the success epilogue, so `SELECT * FROM decide_diagnostics()` answers
                // "how good is the solution I got" after a caveated stop (the stderr caveat is
                // one-shot). A proven-OPTIMAL stop skips this — nothing to caveat. A caveated
                // success has an incumbent, so bucket-B does not run (feasibility is proven).
                StashDecideDiagnostic(context,
                                      BuildTimeoutDiagnostic(solve_result, cum_elapsed, has_objective));
                keep_slow_diagnosis = true;
            }
            // If a Ctrl-C in continue mode broke the loop, the interrupt is now handled
            // (we stopped solving and have a result to return); clear it so the rows
            // flow to the client instead of the executor aborting the query. Covers both
            // the incumbent stop and the rare "Ctrl-C landed as the optimum was proven".
            context.interrupted = false;
            break; // fall through to the shared success stores below
        }
        // Stopped with nothing found yet: stash the structured diagnosis and point to it
        // (mirrors the error-mode exit above and the unbounded / infeasible terminals).
        DecideDiagnostic no_incumbent_diag =
            BuildTimeoutDiagnostic(solve_result, cum_elapsed, has_objective);
        StashDecideDiagnostic(context, no_incumbent_diag);
        ThrowDecideDiagnosisReady(no_incumbent_diag);
    }
    case DiagnosisTerminal::UNDIAGNOSED:
        // Mode off, or a status no engine covers yet: the plain static solver error.
        // Clear any stash left by an earlier failed solve so decide_diagnostics()
        // cannot be misread as being about this query (A4).
        ClearDecideDiagnostic(context);
        ThrowDecideSolveError(solve_result);
    }

    // We are past the switch, so we are delivering rows (every failure terminal threw).
    // With the interrupt poll armed, the rare race where a user Ctrl-C landed just as the
    // solver proved OPTIMAL leaves `context.interrupted` set — which would make the
    // executor abort a query that actually succeeded. Clear it so the rows flow. (The
    // TIME_LIMIT terminal already clears it on its incumbent-stop path; this covers the
    // SOLVED path. Guarded on the poll being armed, so nothing else changes.)
    if (solve_options.interrupt_poll) {
        context.interrupted = false;
    }

    // Success: invalidate any diagnosis stashed by an earlier failed solve on this
    // connection, so decide_diagnostics() no longer reports a now-resolved failure —
    // unless this very solve is a caveated success that just stashed its own quality
    // diagnosis (keep_slow_diagnosis), which the user should still be able to query.
    if (!keep_slow_diagnosis) {
        ClearDecideDiagnostic(context);
    }
    gstate.ilp_solution = std::move(solve_result.solution);
    // Move the indexer onto gstate now that solve is complete; readback in
    // Execute() needs it to outlive solver_input.
    gstate.var_indexer = std::move(var_indexer);

    if (bench) {
        solver_timer.End();
        fprintf(stderr, "DECIDB_BENCH: model_construction_ms=%.2f\n", model_timer.Elapsed() * 1000.0);
        fprintf(stderr, "DECIDB_BENCH: solver_ms=%.2f\n", solver_timer.Elapsed() * 1000.0);
        fprintf(stderr, "DECIDB_BENCH: total_variables=%zu\n", bench_total_vars);
        // Two separate units, never summed: `specs` is the clause-level view (which DECIDE
        // clause to look at), `rows` is what the solver actually received (where the matrix
        // pressure is). The old `total_constraints` added the two and reported neither.
        fprintf(stderr, "DECIDB_BENCH: total_constraint_specs=%zu\n", bench_constraint_specs);
        fprintf(stderr, "DECIDB_BENCH: total_constraint_rows=%zu\n",
                (size_t)solve_result.model_constraint_rows);
        fprintf(stderr, "DECIDB_BENCH: num_rows=%zu\n", (size_t)num_rows);
    }

    return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source (Producing Output)
//===--------------------------------------------------------------------===//
class DecideGlobalSourceState : public GlobalSourceState {
public:
    explicit DecideGlobalSourceState(const PhysicalDecide &op, DecideGlobalSinkState &sink) {
        sink.data.InitializeScan(scan_state);
        sink.data.InitializeScanChunk(scan_chunk);
        current_row_offset = 0;
    }

    ColumnDataScanState scan_state;
    DataChunk scan_chunk; // Sized to the buffered input relation, not the wider output chunk
    idx_t current_row_offset; // Track which row we're at in the solution vector

    idx_t MaxThreads() override {
        return 1; // For simplicity, we'll make the source single-threaded.
    }
};

unique_ptr<GlobalSourceState> PhysicalDecide::GetGlobalSourceState(ClientContext &context) const {
    auto &sink = sink_state->Cast<DecideGlobalSinkState>();
    return make_uniq_base<GlobalSourceState, DecideGlobalSourceState>(*this, sink);
}

SourceResultType PhysicalDecide::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
    auto &gstate = sink_state->Cast<DecideGlobalSinkState>();
    auto &source_state = input.global_state.Cast<DecideGlobalSourceState>();

    // Scan the original buffered data into a chunk sized to match the collection;
    // `chunk` is wider (it has the appended DECIDE variable columns), so scanning
    // directly into it would violate ColumnDataCollectionSegment::ReadChunk's
    // column-count assertion.
    gstate.data.Scan(source_state.scan_state, source_state.scan_chunk);
    if (source_state.scan_chunk.size() == 0) {
        return SourceResultType::FINISHED;
    }

    // Reference the scanned columns into the leading columns of the wide output chunk.
    for (idx_t col = 0; col < source_state.scan_chunk.ColumnCount(); col++) {
        chunk.data[col].Reference(source_state.scan_chunk.data[col]);
    }
    chunk.SetCardinality(source_state.scan_chunk.size());

    // All DECIDE vars (user + auxiliary) are in the output; projection above prunes aux vars
    idx_t total_decide_vars = decide_variables.size();
    idx_t chunk_size = chunk.size();

    // Fill in ALL DECIDE variable columns with solution values from ILP solver
    for (idx_t decide_var_idx = 0; decide_var_idx < total_decide_vars; decide_var_idx++) {
        // The DECIDE columns are appended at the end of the output
        idx_t column_idx = types.size() - total_decide_vars + decide_var_idx;

        auto &output_vector = chunk.data[column_idx];

        // Set vector to flat (each row has its own value)
        output_vector.SetVectorType(VectorType::FLAT_VECTOR);

        // Get the logical type for this DECIDE variable
        auto &decide_var = decide_variables[decide_var_idx]->Cast<BoundColumnRefExpression>();
        auto var_type = decide_var.return_type;

        // Get data pointer once based on type
        if (var_type == LogicalType::INTEGER || var_type == LogicalType::BIGINT) {
            // Use int32_t for INTEGER, int64_t for BIGINT
            if (var_type == LogicalType::INTEGER) {
                auto output_data = FlatVector::GetData<int32_t>(output_vector);

                for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                    idx_t global_row = source_state.current_row_offset + row_in_chunk;
                    idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                    double solution_value = 0.0;
                    if (solution_idx < gstate.ilp_solution.size()) {
                        solution_value = gstate.ilp_solution[solution_idx];
                    }
                    int32_t int_value = static_cast<int32_t>(std::round(solution_value));
                    output_data[row_in_chunk] = int_value;
                }
            } else { // BIGINT
                auto output_data = FlatVector::GetData<int64_t>(output_vector);

                for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                    idx_t global_row = source_state.current_row_offset + row_in_chunk;
                    idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                    double solution_value = 0.0;
                    if (solution_idx < gstate.ilp_solution.size()) {
                        solution_value = gstate.ilp_solution[solution_idx];
                    }
                    int64_t int_value = static_cast<int64_t>(std::round(solution_value));
                    output_data[row_in_chunk] = int_value;
                }
            }

        } else if (var_type == LogicalType::BOOLEAN) {
            auto output_data = FlatVector::GetData<bool>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = (solution_value >= 0.5);
            }

        } else if (var_type == LogicalType::DOUBLE) {
            auto output_data = FlatVector::GetData<double>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = solution_value;
            }

        } else {
            // Default to INTEGER
            auto output_data = FlatVector::GetData<int64_t>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = static_cast<int64_t>(std::round(solution_value));
            }
        }
    }

    // Update row offset for next chunk
    source_state.current_row_offset += chunk_size;

    return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
