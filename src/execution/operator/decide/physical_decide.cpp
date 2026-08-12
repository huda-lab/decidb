#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
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

#include "duckdb/decidb/utility/debug.hpp"
#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/decidb/decide_diagnostic.hpp"
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

//! Per-Finalize cache for TransformToChunkExpression. Keyed on input Expression
//! pointer identity — addresses are stable for the duration of one Finalize. The cache
//! owns lifetime; callers that pass the returned reference to ExpressionExecutor
//! must keep the cache alive until the executor is no longer used.
//!
//! Only ever holds substitution-free trees. A tree built with `agg_substitutions` is
//! valid solely against the augmented chunk it was built for, so it is transformed
//! directly and never cached.
using ChunkExprCache = std::unordered_map<const Expression*, unique_ptr<Expression>>;

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
static void ExtractDoubleColumn(Vector &result_vec, idx_t count, double sign,
                                vector<double> &out, const char *err_context) {
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
		if (!std::isfinite(dv)) {
			throw InvalidInputException(
				"DECIDE %s contains invalid value (NaN or Infinity) at row %llu. "
				"Common causes:\n"
				"  • Division by zero in the expression\n"
				"  • Arithmetic overflow in calculations\n"
				"  • NULL values that propagated through math operations\n"
				"Check your expressions and input data.",
				err_context, out.size());
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

//! Cache for PER group assignments: shares one full-data scan + group-map build
//! across constraints/objectives that use the same PER expression set.
//! The cached value is the *unfiltered* row→group mapping (BuildGroupIds run
//! with row_filter=nullptr); each call site then applies its own WHEN/local
//! filter and remaps the surviving group IDs to consecutive 0..K' to preserve
//! today's "encounter-order, no holes" semantics.
//! Lifetime: one PerGroupCache per Finalize invocation; cleared at end.
struct PerGroupCacheEntry {
	vector<const Expression *> exprs;
	bool null_excludes;
	vector<idx_t> unfiltered_row_group_ids;
	idx_t unfiltered_num_groups;
	//! Representative key values per unfiltered group ([key_col][gid]); used to label
	//! each PER group with its printable key for infeasible diagnosis.
	vector<vector<Value>> unfiltered_rep_keys;
};

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

using PerGroupCache = std::unordered_map<size_t, vector<PerGroupCacheEntry>>;

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

struct NormalizedProductTerm {
	const BoundFunctionExpression *mul_func = nullptr;
	vector<const Expression *> coefficient_factors;
	vector<idx_t> decide_factors;
};

static const Expression *UnwrapBoundCasts(const Expression &expr) {
	const Expression *cur = &expr;
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<BoundCastExpression>().child.get();
	}
	return cur;
}

//! User-facing rendering of a WHEN predicate for diagnosis labels: unwrap the implicit
//! CASTs the binder inserts around literals (so `grp = 'a'` reads cleanly instead of
//! `(grp = CAST('a' AS VARCHAR))`) and drop the redundant outer parens GetName() adds.
//! Handles comparisons and AND/OR conjunctions; anything else falls back to the unwrapped
//! expression's ToString.
static string RenderWhenPredicate(const Expression &expr) {
	const Expression *cur = UnwrapBoundCasts(expr);
	if (cur->GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = cur->Cast<BoundComparisonExpression>();
		return RenderWhenPredicate(*comp.left) + " " + ExpressionTypeToOperator(comp.type) + " " +
		       RenderWhenPredicate(*comp.right);
	}
	if (cur->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = cur->Cast<BoundConjunctionExpression>();
		string op = conj.type == ExpressionType::CONJUNCTION_AND ? " AND " : " OR ";
		string s;
		for (auto &child : conj.children) {
			if (!s.empty()) {
				s += op;
			}
			s += RenderWhenPredicate(*child);
		}
		return s;
	}
	return cur->ToString();
}

//! User-facing rendering of a data-backed RHS for diagnosis labels: unwrap top-level
//! implicit casts from binding so `x >= lo` does not report `x >= CAST(lo AS DOUBLE)`.
static string RenderDiagnosticRhsLabel(const Expression &expr) {
	return UnwrapBoundCasts(expr)->ToString();
}

static bool IsBoundMultiply(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	return func.function.name == "*";
}

static void CollectMultiplicativeFactors(const Expression &expr, vector<const Expression *> &factors) {
	const Expression *cur = UnwrapBoundCasts(expr);
	if (IsBoundMultiply(*cur)) {
		auto &func = cur->Cast<BoundFunctionExpression>();
		for (auto &child : func.children) {
			CollectMultiplicativeFactors(*child, factors);
		}
		return;
	}
	factors.push_back(cur);
}

//! Re-resolve a binary operator against the children it is actually being given.
//!
//! Rebuilding a `BoundFunctionExpression` by hand — reusing another node's
//! `function` / `return_type` / `bind_info` over different children — does not
//! fail when the types disagree. It reinterprets the children's *physical*
//! representation, which silently yields a wrong number and can read past the
//! end of a narrower vector. DECIDE has now hit that failure mode three times
//! (see `07_issues/bugs/done.md`), always where a subtree was rebuilt after
//! terms were dropped or distributed. Binding through `FunctionBinder` is the
//! only rebuild that stays correct for arbitrary children: it picks the
//! implementation for these argument types, computes the matching return type
//! and bind data, and inserts whatever casts the signature needs.
static unique_ptr<Expression> RebindOperator(ClientContext &context, const string &name,
                                             vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(context);
	ErrorData error;
	auto result = function_binder.BindScalarFunction(DEFAULT_SCHEMA, name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("DECIDE failed to rebind '%s' while rebuilding a coefficient: %s", name,
		                       error.Message());
	}
	return result;
}

static unique_ptr<Expression> RebindMultiply(ClientContext &context, unique_ptr<Expression> lhs,
                                             unique_ptr<Expression> rhs) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(lhs));
	children.push_back(std::move(rhs));
	return RebindOperator(context, "*", std::move(children));
}

//! Fold a flattened factor list back into a product. Each binary node is bound
//! for its own operands rather than inheriting the original `*`'s signature:
//! `CollectMultiplicativeFactors` both unwraps casts and (via its callers) drops
//! factors, so neither the operand types nor the arity survive the round trip.
static unique_ptr<Expression> BuildCoefficientFromFactors(ClientContext &context,
                                                          const vector<const Expression *> &factors) {
	if (factors.empty()) {
		return nullptr;
	}
	if (factors.size() == 1) {
		return factors[0]->Copy();
	}

	auto result = factors[0]->Copy();
	for (idx_t i = 1; i < factors.size(); i++) {
		result = RebindMultiply(context, std::move(result), factors[i]->Copy());
	}
	return result;
}

// Distribute multiplication over addition/subtraction: when a `*` chain has
// an additive (`+` / `-` / unary-`-`) factor, expand into a vector of
// (sign, product) pairs, each a pure `*` chain with the additive factor
// replaced by one of its addends. The caller recurses into each pair with
// its sign applied, so `K * (a - b*x)` becomes `(+1, K*a)` and `(-1, K*b*x)`.
//
// Without this expansion, `ClassifyNormalizedProduct` rejects the `(a - b*x)`
// factor as "unexpanded nonlinear product" because it isn't a bare decide-var
// reference, even though the algebraic form is linear in decision vars.
//
// Returns empty when no additive factor is present (caller falls through to
// the existing classification logic).
static vector<pair<int, unique_ptr<Expression>>>
TryDistributeMultiplyOverAdd(ClientContext &context, const BoundFunctionExpression &mul_expr) {
	vector<pair<int, unique_ptr<Expression>>> out;
	vector<const Expression *> factors;
	CollectMultiplicativeFactors(mul_expr, factors);

	int additive_idx = -1;
	for (idx_t i = 0; i < factors.size(); i++) {
		const Expression *f = factors[i];
		if (f->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) continue;
		auto &ff = f->Cast<BoundFunctionExpression>();
		if (ff.function.name == "+" && ff.children.size() >= 1) {
			additive_idx = (int)i; break;
		}
		if (ff.function.name == "-" &&
		    (ff.children.size() == 1 || ff.children.size() == 2)) {
			additive_idx = (int)i; break;
		}
	}
	if (additive_idx < 0) return out;

	auto &add_func = factors[additive_idx]->Cast<BoundFunctionExpression>();
	vector<pair<int, const Expression *>> addends;
	if (add_func.function.name == "+") {
		for (auto &c : add_func.children) addends.push_back({1, c.get()});
	} else { // "-"
		if (add_func.children.size() == 2) {
			addends.push_back({1, add_func.children[0].get()});
			addends.push_back({-1, add_func.children[1].get()});
		} else {
			addends.push_back({-1, add_func.children[0].get()});
		}
	}

	for (auto &kv : addends) {
		int s = kv.first;
		const Expression *ad = kv.second;
		vector<const Expression *> new_factors;
		for (idx_t j = 0; j < factors.size(); j++) {
			if ((int)j == additive_idx) continue;
			new_factors.push_back(factors[j]);
		}
		new_factors.push_back(ad);
		auto prod = BuildCoefficientFromFactors(context, new_factors);
		out.push_back({s, std::move(prod)});
	}
	return out;
}

static bool TryGetBareDecideFactor(const Expression &expr, const PhysicalDecide &op, idx_t &var_idx) {
	const Expression *cur = UnwrapBoundCasts(expr);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	var_idx = op.FindDecideVariable(*cur);
	return var_idx != DConstants::INVALID_INDEX;
}

static bool ClassifyNormalizedProduct(const Expression &expr, const PhysicalDecide &op,
                                      NormalizedProductTerm &result) {
	const Expression *root = UnwrapBoundCasts(expr);
	if (!IsBoundMultiply(*root)) {
		return false;
	}

	result = NormalizedProductTerm();
	result.mul_func = &root->Cast<BoundFunctionExpression>();

	vector<const Expression *> factors;
	CollectMultiplicativeFactors(*root, factors);
	for (auto *factor : factors) {
		idx_t var_idx = DConstants::INVALID_INDEX;
		if (TryGetBareDecideFactor(*factor, op, var_idx)) {
			result.decide_factors.push_back(var_idx);
			continue;
		}
		if (op.FindDecideVariable(*factor) != DConstants::INVALID_INDEX) {
			throw InvalidInputException(
			    "DECIDE expression contains an unsupported product factor that still "
			    "references decision variables after normalization (total degree > 2 "
			    "or unexpanded nonlinear product). Products must be data factors times "
			    "one DECIDE variable, or data factors times two different DECIDE variables.");
		}
		result.coefficient_factors.push_back(factor);
	}

	if (result.decide_factors.size() > 2) {
		throw InvalidInputException(
		    "DECIDE expression contains a product of decision variables with total degree > 2. "
		    "Only linear products and bilinear products of two different DECIDE variables are supported.");
	}
	if (result.decide_factors.size() == 2 && result.decide_factors[0] == result.decide_factors[1]) {
		throw InvalidInputException(
		    "DECIDE expression contains a same-variable product that is not in a supported "
		    "quadratic form. Use POWER(linear_expr, 2) or (linear_expr) * (linear_expr) "
		    "for quadratic terms.");
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Expression Analysis Helper Functions
//===--------------------------------------------------------------------===//

// ExpressionIterator::EnumerateChildren has no const overload; this wrapper
// isolates the const_cast so no call site needs to mention it.
static void EnumerateChildrenConst(const Expression &expr,
                                   const std::function<void(unique_ptr<Expression> &)> &callback) {
	ExpressionIterator::EnumerateChildren(const_cast<Expression &>(expr), callback);
}

idx_t PhysicalDecide::FindDecideVariable(const Expression &expr) const {
    // Base case: check if this is a column reference to a DECIDE variable
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto it = decide_variable_map.find(colref.binding);
        if (it != decide_variable_map.end()) {
            return it->second;
        }
    }

    // Recursive case: search in children
    idx_t result = DConstants::INVALID_INDEX;
    EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
        if (result == DConstants::INVALID_INDEX && child) {
            result = FindDecideVariable(*child);
        }
    });
    return result;
}

bool PhysicalDecide::ContainsVariable(const Expression &expr, idx_t var_idx) const {
    // Check if this expression is the variable we're looking for
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
        return colref.binding == decide_var.binding;
    }

    // Recursively check children
    bool found = false;
    EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
        if (!found && child && ContainsVariable(*child, var_idx)) {
            found = true;
        }
    });
    return found;
}

bool PhysicalDecide::IsLinearInDecideVars(const Expression &expr) const {
    // Column refs and constants: a decide-var col-ref contributes degree 1;
    // non-decide col-refs and constants contribute degree 0. Both are linear.
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
        expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT ||
        expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
        return true;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        return IsLinearInDecideVars(*expr.Cast<BoundCastExpression>().child);
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        string fname = StringUtil::Lower(func.function.name);

        // Additive operators preserve linearity iff every child is linear.
        if (fname == "+" || fname == "-") {
            for (auto &child : func.children) {
                if (!IsLinearInDecideVars(*child)) {
                    return false;
                }
            }
            return true;
        }

        // Multiplication is linear iff at most one factor contains a decide
        // variable and every factor is itself linear. Two var-carrying factors
        // (e.g. x * y, x * POWER(y,2)) push the product to degree ≥ 2.
        if (fname == "*") {
            idx_t factors_with_vars = 0;
            for (auto &child : func.children) {
                if (!IsLinearInDecideVars(*child)) {
                    return false;
                }
                if (FindDecideVariable(*child) != DConstants::INVALID_INDEX) {
                    factors_with_vars++;
                }
            }
            return factors_with_vars <= 1;
        }

        // Division is linear iff the divisor is decide-var-free and the
        // numerator is linear. `x / 2` is a coefficient scale (linear);
        // `x / y` is non-linear (already rejected upstream by the bind-time
        // validator, but we guard here anyway for defence-in-depth).
        if (fname == "/" && func.children.size() == 2) {
            if (FindDecideVariable(*func.children[1]) != DConstants::INVALID_INDEX) {
                return false;
            }
            return IsLinearInDecideVars(*func.children[0]);
        }

        // Any other function (POWER, SIN, ABS, ...) is linear only when none
        // of its arguments reference a decide variable (it is a pure data
        // expression evaluated at runtime into a coefficient).
        return FindDecideVariable(expr) == DConstants::INVALID_INDEX;
    }

    // Unknown expression classes: linear only if they contain no decide var.
    return FindDecideVariable(expr) == DConstants::INVALID_INDEX;
}

unique_ptr<Expression> PhysicalDecide::ExtractCoefficientWithoutVariable(ClientContext &context, const Expression &expr,
                                                                        idx_t var_idx) const {
    // If this IS the variable itself, return constant 1
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
        if (colref.binding == decide_var.binding) {
            return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
        }
    }

    // If it's a multiplication, filter out children containing the variable
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        if (func.function.name == "*") {
            vector<unique_ptr<Expression>> filtered_children;
            for (auto &child : func.children) {
                if (!ContainsVariable(*child, var_idx)) {
                    filtered_children.push_back(child->Copy());
                    continue;
                }
                // Child contains the variable: recurse to keep its non-variable
                // scalar/data factors — `(2*x)` yields `2`, a bare `x` yields `1`.
                // Dropping the whole child (the previous behavior) silently lost
                // nested coefficients like the `2` in `(2*x)*v`, which reaches here
                // un-normalized on the composed MIN/MAX path. The already-normalized
                // `x*(2*v)` form is unchanged (its variable child is the bare `x`).
                auto sub = ExtractCoefficientWithoutVariable(context, *child, var_idx);
                if (sub->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                    auto &cv = sub->Cast<BoundConstantExpression>().value;
                    if (!cv.IsNull() && cv.type().IsNumeric() &&
                        cv.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 1.0) {
                        continue; // bare variable contributes no scalar factor
                    }
                }
                filtered_children.push_back(std::move(sub));
            }

            if (filtered_children.empty()) {
                return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
            }
            if (filtered_children.size() == 1) {
                return std::move(filtered_children[0]);
            }

            // Rebuild the multiplication by re-binding it for the children that
            // actually remain. Dropping the variable also drops the casts above it,
            // so a child can come back narrower than the original signature expects
            // (`CAST(x * price AS DECIMAL(38,2))` yields a bare DECIMAL(15,2)
            // `price`), and dropping a child shifts the rest out of alignment with
            // `function.arguments`. Reusing the original bound function through
            // either of those does not fail — it reinterprets the physical
            // representation and silently computes a wrong coefficient. See
            // `RebindOperator`.
            auto result = std::move(filtered_children[0]);
            for (idx_t i = 1; i < filtered_children.size(); i++) {
                result = RebindMultiply(context, std::move(result), std::move(filtered_children[i]));
            }
            return result;
        }
    }

    // If it's a cast, recurse into child
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        return ExtractCoefficientWithoutVariable(context, *cast.child, var_idx);
    }

    // Otherwise, return a copy of the entire expression (no variable in it)
    return expr.Copy();
}

//! Result of DetectQuadraticPattern. `inner_linear_expr` is a non-owning
//! pointer into the tree rooted at the caller's expression (valid only
//! while that tree is alive). `sign` carries the scalar multiplier from
//! negation and constant-times-quadratic patterns (e.g. `-POWER(x,2)` → -1,
//! `(-2)*POWER` → -2). When `inner_linear_expr == nullptr`, no pattern
//! matched. Defined here (not in the header) so the lifetime contract stays
//! internal to this translation unit.
struct PhysicalDecide::QuadraticPattern {
    const Expression *inner_linear_expr = nullptr;
    double sign = 1.0;
};

PhysicalDecide::QuadraticPattern PhysicalDecide::DetectQuadraticPattern(const Expression &expr) const {
    // Unwrap any cast wrappers on the incoming expression.
    const Expression *cur = &expr;
    while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        cur = cur->Cast<BoundCastExpression>().child.get();
    }
    if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
        return {};
    }
    auto &func = cur->Cast<BoundFunctionExpression>();
    string fname = StringUtil::Lower(func.function.name);

    // Fast path: nothing below this point can match on names outside this set.
    // Without the gate every recursive additive (`+`) node in the objective
    // tree would pay for the self-product `ToString() == ToString()` compare,
    // which is O(subtree-size) — turning the walker into O(n^2) on deep sums.
    if (fname != "-" && fname != "*" && fname != "power" && fname != "pow" && fname != "**") {
        return {};
    }

    // -(quadratic)
    if (fname == "-" && func.children.size() == 1) {
        auto inner = DetectQuadraticPattern(*func.children[0]);
        if (inner.inner_linear_expr) {
            return {inner.inner_linear_expr, -inner.sign};
        }
    }

    // K * quadratic or quadratic * K (constant on either side)
    if (fname == "*" && func.children.size() == 2) {
        for (idx_t side = 0; side < 2; side++) {
            const Expression *maybe_const = func.children[side].get();
            while (maybe_const->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                maybe_const = maybe_const->Cast<BoundCastExpression>().child.get();
            }
            if (maybe_const->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                double cval = maybe_const->Cast<BoundConstantExpression>()
                                  .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                if (cval != 0.0) {
                    auto inner = DetectQuadraticPattern(*func.children[1 - side]);
                    if (inner.inner_linear_expr) {
                        return {inner.inner_linear_expr, cval * inner.sign};
                    }
                }
            }
        }
    }

    // POWER / POW / **  with literal exponent 2
    if ((fname == "power" || fname == "pow" || fname == "**") && func.children.size() == 2) {
        const Expression *exp_expr = func.children[1].get();
        while (exp_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            exp_expr = exp_expr->Cast<BoundCastExpression>().child.get();
        }
        if (exp_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
            double exponent = exp_expr->Cast<BoundConstantExpression>()
                                  .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
            if (exponent == 2.0) {
                const Expression *inner = func.children[0].get();
                while (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    inner = inner->Cast<BoundCastExpression>().child.get();
                }
                if (FindDecideVariable(*inner) != DConstants::INVALID_INDEX) {
                    // Shape matches POWER(expr, 2); reject expr that is itself
                    // degree > 1 in decide vars (e.g. POWER(POWER(x,2), 2) =
                    // x^4, POWER(x*y, 2) = x^2 y^2) rather than silently
                    // emitting an x^2-shaped Q term.
                    if (!IsLinearInDecideVars(*inner)) {
                        throw InvalidInputException(
                            "DECIDE objective/constraint contains a non-linear expression "
                            "inside POWER(..., 2) (total degree > 2 in decision variables). "
                            "Only POWER(linear_expr, 2) is supported; rewrite the expression "
                            "or combine it into a single quadratic group.");
                    }
                    return {inner, 1.0};
                }
            }
        }
    }

    // (expr) * (expr) with identical children containing a DECIDE variable
    if (fname == "*" && func.children.size() == 2 &&
        Expression::Equals(*func.children[0], *func.children[1]) &&
        FindDecideVariable(*func.children[0]) != DConstants::INVALID_INDEX) {
        const Expression *inner = func.children[0].get();
        while (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            inner = inner->Cast<BoundCastExpression>().child.get();
        }
        // Identical-child self-product matches `(expr)*(expr)`; the inner
        // must be linear in decide vars or the product is degree > 2 (e.g.
        // POWER(x,2) * POWER(x,2) = x^4, (x*y) * (x*y) = x^2 y^2).
        if (!IsLinearInDecideVars(*inner)) {
            throw InvalidInputException(
                "DECIDE objective/constraint contains a self-product of a non-linear "
                "expression (e.g. POWER(x, 2) * POWER(x, 2) or (x*y) * (x*y)), total "
                "degree > 2 in decision variables. Only (linear_expr) * (linear_expr) "
                "is supported as a quadratic pattern.");
        }
        return {inner, 1.0};
    }

    return {};
}

void PhysicalDecide::ExtractTerms(ClientContext &context, const Expression &expr, vector<Term> &out_terms) const {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();

        // Addition: recursively process all children
        if (func.function.name == "+") {
            for (auto &child : func.children) {
                ExtractTerms(context, *child, out_terms);
            }
            return;
        }

        // Subtraction: first child positive, second child negated
        if (func.function.name == "-" && func.children.size() == 2) {
            ExtractTerms(context, *func.children[0], out_terms);
            idx_t before = out_terms.size();
            ExtractTerms(context, *func.children[1], out_terms);
            for (idx_t i = before; i < out_terms.size(); i++) {
                out_terms[i].sign *= -1;
            }
            return;
        }

        // Unary minus: recurse and flip sign of every produced term.
        if (func.function.name == "-" && func.children.size() == 1) {
            idx_t before = out_terms.size();
            ExtractTerms(context, *func.children[0], out_terms);
            for (idx_t i = before; i < out_terms.size(); i++) {
                out_terms[i].sign *= -1;
            }
            return;
        }

        // Multiplication: extract variable and coefficient
        if (func.function.name == "*") {
            // If the `*` chain has an additive factor (e.g. `K * (1 - pick)`),
            // distribute first so each resulting product is `coef * var`-shaped.
            // Without this, ExtractCoefficientWithoutVariable would silently
            // drop the additive structure and produce a wrong coefficient.
            auto distributed = TryDistributeMultiplyOverAdd(context, func);
            if (!distributed.empty()) {
                for (auto &kv : distributed) {
                    idx_t before = out_terms.size();
                    ExtractTerms(context, *kv.second, out_terms);
                    if (kv.first == -1) {
                        for (idx_t i = before; i < out_terms.size(); i++) {
                            out_terms[i].sign *= -1;
                        }
                    }
                }
                return;
            }

            idx_t var_idx = FindDecideVariable(func);

            if (var_idx == DConstants::INVALID_INDEX) {
                // No variable found - this is a constant term
                out_terms.push_back(Term{DConstants::INVALID_INDEX, func.Copy()});
            } else {
                // Variable found - extract coefficient
                auto coef = ExtractCoefficientWithoutVariable(context, func, var_idx);
                out_terms.push_back(Term{var_idx, std::move(coef)});
            }
            return;
        }

        // Division by a DECIDE-variable-free expression: recurse into the
        // numerator and wrap every produced term's coefficient in `coef / divisor`.
        // Division where the divisor itself contains a decide variable is
        // non-linear and is already rejected upstream by the bind-time validator.
        // Cast both sides to the `/` function's expected argument types so
        // an extracted integer coefficient doesn't silently turn into
        // integer-division truncation (e.g., `x/2` gave 0 when coef was INT 1).
        if (func.function.name == "/" && func.children.size() == 2 &&
            FindDecideVariable(*func.children[1]) == DConstants::INVALID_INDEX) {
            idx_t before = out_terms.size();
            ExtractTerms(context, *func.children[0], out_terms);
            D_ASSERT(func.function.arguments.size() == 2);
            const auto &num_type = func.function.arguments[0];
            const auto &denom_type = func.function.arguments[1];
            for (idx_t i = before; i < out_terms.size(); i++) {
                auto coef = BoundCastExpression::AddDefaultCastToType(
                    std::move(out_terms[i].coefficient), num_type);
                auto divisor = BoundCastExpression::AddDefaultCastToType(
                    func.children[1]->Copy(), denom_type);
                vector<unique_ptr<Expression>> div_children;
                div_children.push_back(std::move(coef));
                div_children.push_back(std::move(divisor));
                out_terms[i].coefficient = make_uniq_base<Expression, BoundFunctionExpression>(
                    func.return_type, func.function, std::move(div_children), nullptr);
            }
            return;
        }
    }

    // Handle casts
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        ExtractTerms(context, *cast.child, out_terms);
        return;
    }

    // Base case: constant or simple column reference
    idx_t var_idx = FindDecideVariable(expr);
    if (var_idx == DConstants::INVALID_INDEX) {
        // Constant term
        out_terms.push_back(Term{DConstants::INVALID_INDEX, expr.Copy()});
    } else {
        // Just a variable (coefficient = 1)
        out_terms.push_back(Term{var_idx,
            make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
    }
}

static bool BoundExpressionContainsAggregate(const Expression &expr) {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
        return true;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        return BoundExpressionContainsAggregate(*cast.child);
    }
    bool found = false;
    EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
        if (!found && child && BoundExpressionContainsAggregate(*child)) {
            found = true;
        }
    });
    return found;
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

//===--------------------------------------------------------------------===//
// EXPLAIN Support
//===--------------------------------------------------------------------===//

string PhysicalDecide::GetName() const {
	return "DECIDE";
}

static void CollectTaggedExpressionStringsPhysical(const Expression &expr, vector<string> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
			string per_suffix = " PER ";
			bool parenthesize = conj.children.size() > 2;
			if (parenthesize) {
				per_suffix += "(";
			}
			for (idx_t i = 1; i < conj.children.size(); i++) {
				if (i > 1) {
					per_suffix += ", ";
				}
				per_suffix += conj.children[i]->GetName();
			}
			if (parenthesize) {
				per_suffix += ")";
			}
			vector<string> inner;
			CollectTaggedExpressionStringsPhysical(*conj.children[0], inner);
			for (auto &s : inner) {
				out.push_back(s + per_suffix);
			}
			return;
		}
		if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
			string when_suffix = " WHEN " + conj.children[1]->GetName();
			vector<string> inner;
			CollectTaggedExpressionStringsPhysical(*conj.children[0], inner);
			for (auto &s : inner) {
				out.push_back(s + when_suffix);
			}
			return;
		}
		for (auto &child : conj.children) {
			CollectTaggedExpressionStringsPhysical(*child, out);
		}
		return;
	}
	out.push_back(expr.GetName());
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
		CollectTaggedExpressionStringsPhysical(*decide_objective, objective_strs);
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
		CollectTaggedExpressionStringsPhysical(*decide_constraints, constraint_strs);
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

//! Collect DECIDE variable references from a bound expression, tracking sign
//! through subtraction operators. Used for multi-variable per-row constraints.
struct ExprVarRef {
    idx_t var_idx;
    int sign; // +1 or -1
};

static void CollectDecideVarRefs(const Expression &expr, int sign,
                                  vector<ExprVarRef> &refs,
                                  const PhysicalDecide &op) {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        idx_t var_idx = op.FindDecideVariable(expr);
        if (var_idx != DConstants::INVALID_INDEX) {
            refs.push_back({var_idx, sign});
        }
        return;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        if (func.function.name == "-" && func.children.size() == 2) {
            CollectDecideVarRefs(*func.children[0], sign, refs, op);
            CollectDecideVarRefs(*func.children[1], -sign, refs, op);
            return;
        }
        if (func.function.name == "+" && func.children.size() == 2) {
            CollectDecideVarRefs(*func.children[0], sign, refs, op);
            CollectDecideVarRefs(*func.children[1], sign, refs, op);
            return;
        }
        if (func.function.name == "*" && func.children.size() == 2) {
            // Multiplication: descend into both children to find decide variables.
            // Sign propagates unchanged — * doesn't flip algebraic sign, it changes
            // the coefficient magnitude, which this walk does not measure.
            CollectDecideVarRefs(*func.children[0], sign, refs, op);
            CollectDecideVarRefs(*func.children[1], sign, refs, op);
            return;
        }
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        CollectDecideVarRefs(*cast.child, sign, refs, op);
        return;
    }
    // Constants, data columns, etc.: no DECIDE vars
}

//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//

//! Sentinel marking "no explicit lower-bound constraint" during bound absorption.
//! Resolved to the default 0 in Finalize for any variable the query never lowered.
//! Distinct from a real bound: signed variables use finite negative bounds only
//! (no -inf domain), so no legitimate lower bound is anywhere near this value.
static constexpr double ABSORBED_LOWER_UNSET = -1e30;

//! A user-written simple bound (`x OP const` / `x BETWEEN a AND b`) that was
//! absorbed into the column-bound arrays instead of emitted as a matrix row, so it
//! carries no provenance and is invisible to the elastic engine. The infeasible
//! diagnosis re-emits these as USER_PARAMETER slackable rows (I1, decision 1a). One
//! spec per user-written direction: BETWEEN yields two (lower + upper). Intrinsic
//! domains are NOT recorded — default non-negativity never is, and a bound on a
//! BOOLEAN is skipped only when it merely restates the 0/1 box (`>= 0` / `<= 1`);
//! a genuine BOOLEAN pin (`x <= 0`, `x >= 1`, `x = 1`) IS recorded.
struct UserBoundSpec {
    idx_t decide_var_idx; //!< index into op.decide_variables
    char sense;           //!< '<' (<= K), '>' (>= K), '=' (== K)
    double k;             //!< the (integer-strict-normalized) bound value
    //! True when the user wrote a strict `<` / `>` that was integer-normalized into
    //! `k` (e.g. `x < 10` → k=9). `typed_k` carries the user's original literal so the
    //! re-emitted row mirrors `ConstraintProvenance::strict` / `typed_k` and the
    //! infeasible diagnosis re-quotes the suggestion as `<` / `>` against it.
    bool strict = false;
    double typed_k = 0.0;
};

class DecideGlobalSinkState : public GlobalSinkState {
public:
    explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()), context(context), op(op) {
        // Pre-absorb simple variable bounds (x OP const / BETWEEN) into column-bound
        // arrays so AnalyzeConstraint can skip emitting one DecideConstraint per row
        // for constraints that are fully captured by column bounds.
        idx_t num_decide_vars = op.decide_variables.size();
        // Initialize lower bounds to an "unset" sentinel rather than 0 so that an
        // explicit negative lower-bound constraint (e.g. `x >= -5`, `BETWEEN -10
        // AND 10`) is honored instead of clamped to 0. The std::max combiners in
        // TraverseBoundsConstraints still pick the tightest of multiple `>=`
        // constraints (max(-1e30, k) == k), and Finalize resolves any variable
        // still at the sentinel to the default 0 (non-negative unless the query
        // explicitly lowers the bound). See ABSORBED_LOWER_UNSET.
        absorbed_lower_bounds.assign(num_decide_vars, ABSORBED_LOWER_UNSET);
        absorbed_upper_bounds.assign(num_decide_vars, 1e30);
        if (op.decide_constraints) {
            TraverseBoundsConstraints(*op.decide_constraints, absorbed_lower_bounds,
                                      absorbed_upper_bounds);
        }

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

        // Analyze constraints and objective using new visitor-based approach
        AnalyzeConstraint(op.decide_constraints);
        if (op.decide_objective) {
            AnalyzeObjective(op.decide_objective);
        }

        // Minimal: keep constructor lean; detailed solver output comes from HiGHS
    }

    //! Entity scope a reducer is qualified by (`sum(D: ...)`), read back from the tag the
    //! binder stamped on the aggregate; INVALID_INDEX when the reducer is unqualified.
    static idx_t QualifierScopeOf(const BoundAggregateExpression &agg) {
        idx_t scope_idx = DConstants::INVALID_INDEX;
        TryParseQualifiedReducerTag(agg.alias, scope_idx);
        return scope_idx;
    }

    static void ApplyAggregateMetadata(vector<Term> &terms, idx_t begin, const BoundAggregateExpression &agg) {
        bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
        idx_t qualifier_scope = QualifierScopeOf(agg);
        for (idx_t i = begin; i < terms.size(); i++) {
            if (agg.filter) {
                terms[i].filter = agg.filter->Copy();
            }
            terms[i].avg_scale = is_avg;
            terms[i].qualifier_scope_idx = qualifier_scope;
        }
    }

    //! Multiply everything the aggregate under a peeled scale just produced by that
    //! scale. Folding the factor into the reducer's body is what the canonicalizer
    //! refuses to do, because at the parsed level the aggregate may still be MIN/MAX
    //! and `MAX(-2x)` is `-2*MIN(x)`, not `-2*MAX(x)`. Here it is safe and exact: the
    //! optimizer has already rewritten every MIN/MAX to SUM (asserted below), and a
    //! sum distributes over any factor regardless of sign.
    //!
    //! `scale_func` is the bound `*` or `/` node the factor came from; reusing its
    //! FunctionData is how the coefficient gets rebuilt without a binder here, the
    //! same way ExtractTerms handles `expr / K`.
    void ApplyScaleToExtracted(const BoundFunctionExpression &scale_func, const Expression &scale,
                               bool divides, DecideConstraint &constraint, idx_t linear_before,
                               idx_t bilinear_before, idx_t quadratic_before) {
        auto scaled = [&](unique_ptr<Expression> coef) {
            const auto &lhs_type = scale_func.function.arguments[divides ? 0 : 1];
            const auto &rhs_type = scale_func.function.arguments[divides ? 1 : 0];
            vector<unique_ptr<Expression>> children;
            // `scale * coef` keeps the factor on the left, matching the canonical
            // spelling; `coef / scale` has to keep the operand order division needs.
            if (divides) {
                children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), lhs_type));
                children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), rhs_type));
            } else {
                children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), rhs_type));
                children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), lhs_type));
            }
            return make_uniq_base<Expression, BoundFunctionExpression>(
                scale_func.return_type, scale_func.function, std::move(children), nullptr);
        };
        for (idx_t i = linear_before; i < constraint.lhs_terms.size(); i++) {
            constraint.lhs_terms[i].coefficient = scaled(std::move(constraint.lhs_terms[i].coefficient));
        }
        for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
            auto &bt = constraint.bilinear_terms[i];
            // A null coefficient means 1.0; the scale becomes the whole coefficient.
            bt.coefficient = bt.coefficient
                                 ? scaled(std::move(bt.coefficient))
                                 : scaled(make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1)));
        }
        if (quadratic_before == constraint.quadratic_groups.size()) {
            return;
        }
        // A quadratic group carries a numeric multiplier rather than an expression, so
        // scaling one needs the factor's value here. Read it straight off the node, the
        // way the scaled-quadratic detection below already does -- there is no
        // ClientContext at analysis time to evaluate anything richer.
        const Expression *literal = &scale;
        while (literal->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            literal = literal->Cast<BoundCastExpression>().child.get();
        }
        if (literal->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
            throw InvalidInputException(
                "DECIDE constraint: a squared term cannot be multiplied by '%s', whose value "
                "is not known until the query runs. Use a constant factor, or move it inside "
                "the aggregate as SUM(%s * POWER(...)).",
                scale.GetName(), scale.GetName());
        }
        double factor = literal->Cast<BoundConstantExpression>()
                            .value.DefaultCastAs(LogicalType::DOUBLE)
                            .GetValue<double>();
        if (divides && factor == 0.0) {
            throw InvalidInputException("DECIDE constraint: division by zero in a squared term.");
        }
        for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
            constraint.quadratic_groups[i].sign *= divides ? 1.0 / factor : factor;
        }
    }

    //! If `expr` is a reducer with a factor peeled onto it by the canonicalizer
    //! (`2 * SUM(x)`, `SUM(x) / 2`), return the bare reducer and report the factor.
    //! Only ONE spelling has to be recognized here -- factor on the left for `*` --
    //! because canonicalization converged `SUM(x) * 2` and friends onto it.
    //! Name an expression the way the user wrote it: strip the casts the binder added.
    static string ScaleUserName(const Expression &expr) {
        const Expression *cur = &expr;
        while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            cur = cur->Cast<BoundCastExpression>().child.get();
        }
        auto name = cur->GetName();
        return name.empty() ? cur->ToString() : name;
    }

    static const Expression *AsScaledAggregate(const Expression &expr, const Expression *&out_scale,
                                               bool &out_divides,
                                               const BoundFunctionExpression *&out_func) {
        out_scale = nullptr;
        out_divides = false;
        out_func = nullptr;
        if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
            return nullptr;
        }
        auto &func = expr.Cast<BoundFunctionExpression>();
        bool is_mult = func.function.name == "*";
        bool is_div = func.function.name == "/";
        if ((!is_mult && !is_div) || func.children.size() != 2) {
            return nullptr;
        }
        // Both orders are matched for `*`. Canonicalization puts the factor on the
        // left, but it only runs on CONSTRAINTS -- an objective reaches here spelled
        // however the user wrote it. `/` is not commutative: only `AGG / K` qualifies.
        auto unwrap = [](const Expression &e) -> const Expression * {
            const Expression *cur = &e;
            while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                cur = cur->Cast<BoundCastExpression>().child.get();
            }
            return cur;
        };
        idx_t agg_child;
        if (unwrap(*func.children[0])->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
            agg_child = 0;
        } else if (is_mult && unwrap(*func.children[1])->GetExpressionClass() ==
                                  ExpressionClass::BOUND_AGGREGATE) {
            agg_child = 1;
        } else {
            return nullptr;
        }
        // Two reducers multiplied is a product of aggregates, not a scaled one.
        if (unwrap(*func.children[1 - agg_child])->GetExpressionClass() ==
            ExpressionClass::BOUND_AGGREGATE) {
            return nullptr;
        }
        out_scale = func.children[1 - agg_child].get();
        out_divides = is_div;
        out_func = &func;
        return unwrap(*func.children[agg_child]);
    }

    void ExtractAggregateConstraintTerms(const Expression &expr, DecideConstraint &constraint, int sign) {
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            auto &cast = expr.Cast<BoundCastExpression>();
            ExtractAggregateConstraintTerms(*cast.child, constraint, sign);
            return;
        }
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto &func = expr.Cast<BoundFunctionExpression>();
            if (func.function.name == "+") {
                for (auto &child : func.children) {
                    ExtractAggregateConstraintTerms(*child, constraint, sign);
                }
                return;
            }
            if (func.function.name == "-" && func.children.size() == 2) {
                ExtractAggregateConstraintTerms(*func.children[0], constraint, sign);
                ExtractAggregateConstraintTerms(*func.children[1], constraint, -sign);
                return;
            }
            if (func.function.name == "-" && func.children.size() == 1) {
                ExtractAggregateConstraintTerms(*func.children[0], constraint, -sign);
                return;
            }
        }
        // A scaled reducer: unwrap to the reducer, extract it, then apply the factor to
        // everything that came out.
        {
            const Expression *scale = nullptr;
            bool divides = false;
            const BoundFunctionExpression *scale_func = nullptr;
            if (const Expression *agg_expr = AsScaledAggregate(expr, scale, divides, scale_func)) {
                // Belt and braces: the canonicalizer rejects a decision-bearing factor
                // before a user-written constraint reaches here, but constraints the
                // OPTIMIZER emits are canonicalized by the permissive path, which does
                // not judge factors. Reading a decision column as a coefficient crashes
                // in evaluation rather than failing cleanly, so check here too.
                if (op.FindDecideVariable(*scale) != DConstants::INVALID_INDEX) {
                    throw InvalidInputException(
                        "DECIDE constraint: '%s' is a decision, so it cannot multiply an "
                        "aggregate. Only constants and query-wide values can scale "
                        "SUM/AVG/MIN/MAX.",
                        ScaleUserName(*scale));
                }
                idx_t linear_before = constraint.lhs_terms.size();
                idx_t bilinear_before = constraint.bilinear_terms.size();
                idx_t quadratic_before = constraint.quadratic_groups.size();
                ExtractAggregateConstraintTerms(*agg_expr, constraint, sign);
                ApplyScaleToExtracted(*scale_func, *scale, divides, constraint, linear_before,
                                      bilinear_before, quadratic_before);
                return;
            }
        }
        // A query-wide (`scalar`) decision is row-invariant, so it is a complete term of
        // an aggregate constraint on its own -- there is nothing for a reducer to collapse.
        // This is K3's "reducer or row-invariant" rule; the objective path already reads
        // the same way (see ExtractAggregateObjectiveTerms).
        if (IsScalarDecideTerm(expr)) {
            ExtractConstraintTerms(expr, constraint, sign);
            return;
        }
        if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
            throw InvalidInputException("DECIDE aggregate constraint LHS contains a non-aggregate term: %s.\n"
                                        "Use SUM/MIN/MAX/AVG of an expression in decision variables.",
                                        expr.ToString());
        }

        auto &agg = expr.Cast<BoundAggregateExpression>();
        auto agg_name = StringUtil::Lower(agg.function.name);
        if (agg_name != "sum") {
            throw InvalidInputException("DECIDE optimizer should rewrite aggregate '%s' to SUM before execution",
                                        agg.function.name);
        }
        bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
        idx_t qualifier_scope = QualifierScopeOf(agg);

        idx_t linear_before = constraint.lhs_terms.size();
        idx_t bilinear_before = constraint.bilinear_terms.size();
        idx_t quadratic_before = constraint.quadratic_groups.size();
        ExtractConstraintTerms(*agg.children[0], constraint, sign);
        ApplyAggregateMetadata(constraint.lhs_terms, linear_before, agg);
        for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
            if (agg.filter) {
                constraint.bilinear_terms[i].filter = agg.filter->Copy();
            }
            constraint.bilinear_terms[i].avg_scale = is_avg;
            constraint.bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
        }
        for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
            if (agg.filter) {
                constraint.quadratic_groups[i].filter = agg.filter->Copy();
            }
            constraint.quadratic_groups[i].avg_scale = is_avg;
            constraint.quadratic_groups[i].qualifier_scope_idx = qualifier_scope;
        }

        string minmax_payload;
        if (ExtractDecideTagPayload(agg.alias, MINMAX_INDICATOR_TAG_PREFIX, minmax_payload)) {
            auto sep = minmax_payload.find('_');
            constraint.minmax_indicator_idx = std::stoull(minmax_payload.substr(0, sep));
            constraint.minmax_agg_type = minmax_payload.substr(sep + 1);
            constraint.kind = ConstraintKind::USER_MECHANISM;
        }
    }

    void AnalyzeConstraint(const unique_ptr<Expression>& expr_ptr,
                           unique_ptr<Expression> when_condition = nullptr,
                           vector<unique_ptr<Expression>> per_columns = {}) {
        auto &expr = *expr_ptr;
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // DecidB: PER wrapper — outermost layer
                if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
                    // child[0] = the constraint (possibly WHEN-wrapped)
                    // children[1..N] = the PER column expressions
                    vector<unique_ptr<Expression>> per_cols;
                    for (idx_t i = 1; i < conj.children.size(); i++) {
                        per_cols.push_back(conj.children[i]->Copy());
                    }
                    AnalyzeConstraint(conj.children[0], std::move(when_condition),
                                      std::move(per_cols));
                    break;
                }
                // DecidB: Check if this is a WHEN constraint wrapper
                if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                    // child[0] = the actual constraint, child[1] = the WHEN condition
                    AnalyzeConstraint(conj.children[0], conj.children[1]->Copy(),
                                      std::move(per_columns));
                    break;
                }
                // Regular conjunction: recursively analyze each child
                for (auto &child : conj.children) {
                    AnalyzeConstraint(child);
                }
                break;
            }

            case ExpressionClass::BOUND_COMPARISON: {
                auto &comp = expr.Cast<BoundComparisonExpression>();

                // Skip comparisons whose entire semantics are already captured in
                // column bounds by the constructor's absorption pass. Emitting a
                // DecideConstraint here would add num_rows redundant model rows.
                // WHEN-wrapped comparisons are never absorbed (see
                // TraverseBoundsConstraints WHEN_CONSTRAINT_TAG branch), so
                // skipping here is safe.
                if (absorbed_bound_exprs.count(&expr)) {
                    break;
                }

                auto constraint = make_uniq<DecideConstraint>();
                constraint->comparison_type = comp.type;
                constraint->rhs_expr = comp.right->Copy();

                // Parse not-equal indicator tag if present
                if (comp.alias.size() > strlen(NE_INDICATOR_TAG_PREFIX) + 2 &&
                    comp.alias.substr(0, strlen(NE_INDICATOR_TAG_PREFIX)) == NE_INDICATOR_TAG_PREFIX) {
                    auto payload = comp.alias.substr(strlen(NE_INDICATOR_TAG_PREFIX));
                    payload = payload.substr(0, payload.size() - 2);  // strip trailing "__"
                    constraint->ne_indicator_idx = std::stoull(payload);
                    constraint->kind = ConstraintKind::USER_MECHANISM;
                }

                // Parse ABS MAXIMIZE upper-bound tag: marks a lower-bound ABS constraint
                // (aux >= inner or aux >= -inner) that needs Big-M upper bounds at finalization.
                if (comp.alias == STRUCTURAL_CONSTRAINT_TAG) {
                    constraint->kind = ConstraintKind::STRUCTURAL;
                }
                if (comp.alias.size() > strlen(ABS_UB_POS_TAG_PREFIX) + 2 &&
                    comp.alias.substr(0, strlen(ABS_UB_POS_TAG_PREFIX)) == ABS_UB_POS_TAG_PREFIX) {
                    auto payload = comp.alias.substr(strlen(ABS_UB_POS_TAG_PREFIX));
                    constraint->abs_y_idx = std::stoull(payload.substr(0, payload.size() - 2));
                    constraint->abs_is_pos_bound = true;
                    constraint->kind = ConstraintKind::STRUCTURAL;
                } else if (comp.alias.size() > strlen(ABS_UB_NEG_TAG_PREFIX) + 2 &&
                           comp.alias.substr(0, strlen(ABS_UB_NEG_TAG_PREFIX)) == ABS_UB_NEG_TAG_PREFIX) {
                    auto payload = comp.alias.substr(strlen(ABS_UB_NEG_TAG_PREFIX));
                    constraint->abs_y_idx = std::stoull(payload.substr(0, payload.size() - 2));
                    constraint->abs_is_pos_bound = false;
                    constraint->kind = ConstraintKind::STRUCTURAL;
                }

                // Detect easy-direction MIN/MAX optimizer rewrite (see decide.hpp).
                if (comp.alias == MINMAX_EASY_REWRITE_TAG) {
                    constraint->was_minmax_easy = true;
                }

                // DecidB: Store WHEN condition and PER columns if present
                if (when_condition) {
                    constraint->when_condition = std::move(when_condition);
                }
                if (!per_columns.empty()) {
                    constraint->per_columns = std::move(per_columns);
                }

                // Extract terms from LHS
                Expression *lhs = comp.left.get();
                while (lhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    lhs = lhs->Cast<BoundCastExpression>().child.get();
                }

                if (BoundExpressionContainsAggregate(*lhs)) {
                    // Aggregate constraint. Handles both legacy single aggregates and
                    // additive aggregate expressions with aggregate-local WHEN filters.
                    constraint->lhs_is_aggregate = true;
                    ExtractAggregateConstraintTerms(*lhs, *constraint, 1);
                } else {
                    // Per-row constraint (e.g., x <= 5, or multi-variable: d >= x - c)
                    constraint->lhs_is_aggregate = false;

                    // K1 guard. DecideCanonicalizer puts every decision-bearing term on
                    // the left, so a decision variable reaching the RHS here means the
                    // invariant was broken upstream -- by a new optimizer rewrite that
                    // mutates a constraint in place instead of going through
                    // LogicalDecide::AddConstraint, most likely. This used to be a second
                    // implementation of the partition (canonicalize.md sites 4/5); it was
                    // verified unreachable across the golden corpus and the full suite
                    // before being replaced by the check, so a wrong answer here would
                    // otherwise be silent.
                    vector<ExprVarRef> rhs_refs;
                    CollectDecideVarRefs(*comp.right, +1, rhs_refs, op);
                    if (!rhs_refs.empty()) {
                        throw InternalException(
                            "DECIDE constraint is not canonical: decision variable on the right-hand "
                            "side of '%s'. Constraints must be canonicalized by DecideCanonicalizer "
                            "before reaching the physical operator.",
                            comp.right->ToString());
                    }

                    if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                        // Simple single-variable constraint (e.g., x <= 5)
                        idx_t var_idx = op.FindDecideVariable(*lhs);
                        if (var_idx != DConstants::INVALID_INDEX) {
                            constraint->lhs_terms.push_back(Term{
                                var_idx,
                                make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))
                            });
                        }
                    } else {
                        // Multi-variable per-row constraint with complex LHS
                        // (e.g., z_0 + z_1 = 1, or x + (-3)*z_0 + (-5)*z_1 = 0,
                        //  or POWER(x - target, 2) <= K quadratic constraint)
                        ExtractConstraintTerms(*lhs, *constraint, 1);
                    }
                }

                constraints.push_back(std::move(constraint));
                break;
            }

            default:
                break;
        }
    }

    //! Walk a SUM argument expression tree and split into linear terms and bilinear terms.
    //! Bilinear terms (x * y where both are decide variables) go to objective->bilinear_terms.
    //! Linear terms (c * x, constants) go to objective->terms via ExtractTerms.
    void ExtractLinearAndBilinearTerms(const Expression &expr, Objective &obj, int sign,
                                       const Expression *filter = nullptr) {
        // DecidB: detect quadratic patterns (POWER / x*x / negated / const * POWER)
        // *before* any linear-structure traversal. This allows mixed shapes like
        // SUM(POWER(x-t, 2) + penalty*x) to route the POWER leaf into squared_terms
        // while the `+` recursion below sends the linear sibling into terms.
        //
        // The objective currently supports exactly one quadratic group per
        // objective (the inner expression of a single SUM(POWER(...))), with a
        // single scalar quadratic_sign. Additional quadratic groups (e.g.
        // `SUM(POWER(x,2)) + SUM(POWER(y,2))`) would need per-group Q matrices
        // downstream and are explicitly rejected.
        auto quad_pattern = op.DetectQuadraticPattern(expr);
        if (quad_pattern.inner_linear_expr) {
            double effective_sign = quad_pattern.sign * static_cast<double>(sign);
            if (obj.has_quadratic) {
                throw InvalidInputException(
                    "DECIDE objective contains multiple quadratic (POWER / (expr)*(expr)) "
                    "groups. Only a single quadratic group plus linear terms is supported; "
                    "combine them mathematically or rewrite the objective."
                );
            }
            obj.has_quadratic = true;
            obj.quadratic_sign = effective_sign;
            idx_t before = obj.squared_terms.size();
            op.ExtractTerms(context, *quad_pattern.inner_linear_expr, obj.squared_terms);
            if (filter) {
                for (idx_t i = before; i < obj.squared_terms.size(); i++) {
                    obj.squared_terms[i].filter = filter->Copy();
                }
            }
            return;
        }

        if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto &func = expr.Cast<BoundFunctionExpression>();
            string fname = func.function.name;

            // Addition: recurse on all children
            if (fname == "+") {
                for (auto &child : func.children) {
                    ExtractLinearAndBilinearTerms(*child, obj, sign, filter);
                }
                return;
            }

            // Subtraction: first child same sign, second negated
            if (fname == "-" && func.children.size() == 2) {
                ExtractLinearAndBilinearTerms(*func.children[0], obj, sign, filter);
                ExtractLinearAndBilinearTerms(*func.children[1], obj, -sign, filter);
                return;
            }

            // Unary negation
            if (fname == "-" && func.children.size() == 1) {
                ExtractLinearAndBilinearTerms(*func.children[0], obj, -sign, filter);
                return;
            }

            // Multiplication: the parsed normalizer is responsible for algebraic
            // expansion; at physical planning we only flatten already-normalized
            // product factors for classification.
            if (fname == "*") {
                // Distribute before ClassifyNormalizedProduct (which throws on
                // additive factors). See ExtractConstraintTerms for rationale.
                {
                    auto distributed = TryDistributeMultiplyOverAdd(context, func);
                    if (!distributed.empty()) {
                        for (auto &kv : distributed) {
                            ExtractLinearAndBilinearTerms(*kv.second, obj, sign * kv.first, filter);
                        }
                        return;
                    }
                }
                NormalizedProductTerm product;
                if (ClassifyNormalizedProduct(func, op, product)) {
                    if (product.decide_factors.size() == 2) {
                        Objective::BilinearTerm bt;
                        bt.var_a = product.decide_factors[0];
                        bt.var_b = product.decide_factors[1];
                        bt.coefficient = BuildCoefficientFromFactors(context, product.coefficient_factors);
                        bt.sign = sign;
                        if (filter) {
                            bt.filter = filter->Copy();
                        }
                        obj.bilinear_terms.push_back(std::move(bt));
                        obj.has_bilinear = true;
                        return;
                    }
                }
            }
        }

        // Handle casts
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            auto &cast = expr.Cast<BoundCastExpression>();
            ExtractLinearAndBilinearTerms(*cast.child, obj, sign, filter);
            return;
        }

        // Not bilinear — delegate to linear extraction
        idx_t before = obj.terms.size();
        op.ExtractTerms(context, expr, obj.terms);
        // Apply sign to newly added terms
        if (sign == -1) {
            for (idx_t i = before; i < obj.terms.size(); i++) {
                obj.terms[i].sign *= -1;
            }
        }
        if (filter) {
            for (idx_t i = before; i < obj.terms.size(); i++) {
                obj.terms[i].filter = filter->Copy();
            }
        }
    }

    //! Extract linear and bilinear terms from a SUM argument in a constraint.
    //! Similar to ExtractLinearAndBilinearTerms but outputs to DecideConstraint fields.
    void ExtractConstraintTerms(const Expression &expr, DecideConstraint &constr, int sign,
                                const Expression *filter = nullptr) {
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto &func = expr.Cast<BoundFunctionExpression>();
            string fname = func.function.name;

            if (fname == "+") {
                for (auto &child : func.children) {
                    ExtractConstraintTerms(*child, constr, sign, filter);
                }
                return;
            }
            if (fname == "-" && func.children.size() == 2) {
                ExtractConstraintTerms(*func.children[0], constr, sign, filter);
                ExtractConstraintTerms(*func.children[1], constr, -sign, filter);
                return;
            }
            if (fname == "-" && func.children.size() == 1) {
                ExtractConstraintTerms(*func.children[0], constr, -sign, filter);
                return;
            }
            // Helper: try to detect POWER(expr, 2), POW(expr, 2), expr ** 2,
            // or (expr)*(expr) self-product. Returns the inner expression on success.
            auto TryDetectConstraintQuadratic = [&](const Expression *test_expr) -> const Expression * {
                while (test_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    test_expr = test_expr->Cast<BoundCastExpression>().child.get();
                }
                if (test_expr->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return nullptr;
                auto &qf = test_expr->Cast<BoundFunctionExpression>();
                string qname = StringUtil::Lower(qf.function.name);
                // POWER/POW/** with exponent 2
                if ((qname == "power" || qname == "pow" || qname == "**") && qf.children.size() == 2) {
                    const Expression *exp_expr = qf.children[1].get();
                    while (exp_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                        exp_expr = exp_expr->Cast<BoundCastExpression>().child.get();
                    }
                    if (exp_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                        double exponent = exp_expr->Cast<BoundConstantExpression>()
                                              .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                        if (exponent == 2.0) {
                            const Expression *inner = qf.children[0].get();
                            while (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                                inner = inner->Cast<BoundCastExpression>().child.get();
                            }
                            if (op.FindDecideVariable(*inner) != DConstants::INVALID_INDEX) {
                                if (!op.IsLinearInDecideVars(*inner)) {
                                    throw InvalidInputException(
                                        "DECIDE constraint contains a non-linear expression "
                                        "inside POWER(..., 2) (total degree > 2 in decision "
                                        "variables). Only POWER(linear_expr, 2) is supported.");
                                }
                                return inner;
                            }
                        }
                    }
                }
                // Self-product: (expr)*(expr) with identical sides
                if (qname == "*" && qf.children.size() == 2 &&
                    Expression::Equals(*qf.children[0], *qf.children[1]) &&
                    op.FindDecideVariable(*qf.children[0]) != DConstants::INVALID_INDEX) {
                    const Expression *inner = qf.children[0].get();
                    while (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                        inner = inner->Cast<BoundCastExpression>().child.get();
                    }
                    if (!op.IsLinearInDecideVars(*inner)) {
                        throw InvalidInputException(
                            "DECIDE constraint contains a self-product of a non-linear "
                            "expression (e.g. POWER(x, 2) * POWER(x, 2)), total degree > 2 "
                            "in decision variables. Only (linear_expr) * (linear_expr) is "
                            "supported as a quadratic pattern.");
                    }
                    return inner;
                }
                return nullptr;
            };

            // Direct POWER/self-product detection
            {
                const Expression *inner = TryDetectConstraintQuadratic(&func);
                if (inner) {
                    DecideConstraint::QuadraticGroup qg;
                    qg.sign = static_cast<double>(sign);
                    if (filter) {
                        qg.filter = filter->Copy();
                    }
                    op.ExtractTerms(context, *inner, qg.inner_terms);
                    constr.quadratic_groups.push_back(std::move(qg));
                    constr.has_quadratic = true;
                    return;
                }
            }
            if (fname == "*") {
                // Scaled quadratic: const * POWER(expr, 2) or POWER(expr, 2) * const
                if (func.children.size() == 2) {
                    for (idx_t side = 0; side < 2; side++) {
                        const Expression *maybe_const = func.children[side].get();
                        while (maybe_const->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                            maybe_const = maybe_const->Cast<BoundCastExpression>().child.get();
                        }
                        if (maybe_const->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                            double cval = maybe_const->Cast<BoundConstantExpression>()
                                              .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                            if (cval != 0.0) {
                                const Expression *inner = TryDetectConstraintQuadratic(func.children[1 - side].get());
                                if (inner) {
                                    DecideConstraint::QuadraticGroup qg;
                                    qg.sign = static_cast<double>(sign) * cval;
                                    if (filter) {
                                        qg.filter = filter->Copy();
                                    }
                                    op.ExtractTerms(context, *inner, qg.inner_terms);
                                    constr.quadratic_groups.push_back(std::move(qg));
                                    constr.has_quadratic = true;
                                    return;
                                }
                            }
                        }
                    }
                }
                // Distribution must come BEFORE ClassifyNormalizedProduct, since
                // the classifier throws on additive factors instead of returning
                // false. Shapes like `K * (1 - pick)` reach here from MIN/MAX
                // hard-direction rewrites and other paths the symbolic normalizer
                // didn't fully expand.
                {
                    auto distributed = TryDistributeMultiplyOverAdd(context, func);
                    if (!distributed.empty()) {
                        for (auto &kv : distributed) {
                            ExtractConstraintTerms(*kv.second, constr, sign * kv.first, filter);
                        }
                        return;
                    }
                }
                NormalizedProductTerm product;
                if (ClassifyNormalizedProduct(func, op, product)) {
                    if (product.decide_factors.size() == 2) {
                        BilinearConstraintTerm bt;
                        bt.var_a = product.decide_factors[0];
                        bt.var_b = product.decide_factors[1];
                        bt.coefficient = BuildCoefficientFromFactors(context, product.coefficient_factors);
                        bt.sign = sign;
                        if (filter) {
                            bt.filter = filter->Copy();
                        }
                        constr.bilinear_terms.push_back(std::move(bt));
                        constr.has_bilinear = true;
                        return;
                    }
                }
            }
        }
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            auto &cast = expr.Cast<BoundCastExpression>();
            ExtractConstraintTerms(*cast.child, constr, sign, filter);
            return;
        }
        // Linear — delegate to ExtractTerms
        idx_t before = constr.lhs_terms.size();
        op.ExtractTerms(context, expr, constr.lhs_terms);
        if (sign == -1) {
            for (idx_t i = before; i < constr.lhs_terms.size(); i++) {
                constr.lhs_terms[i].sign *= -1;
            }
        }
        if (filter) {
            for (idx_t i = before; i < constr.lhs_terms.size(); i++) {
                constr.lhs_terms[i].filter = filter->Copy();
            }
        }
    }

    //! True when the decision referenced by `expr` is query-wide (`scalar`).
    //! Such a term is a complete objective contribution on its own: it maps to a
    //! single solver column, so there is no reducer to collapse it.
    bool IsScalarDecideTerm(const Expression &expr) const {
        idx_t var_idx = op.FindDecideVariable(expr);
        return var_idx != DConstants::INVALID_INDEX && var_idx < op.variable_scopes.size() &&
               op.variable_scopes[var_idx].IsScalar();
    }

    //! Objective twin of ApplyScaleToExtracted: multiply a factor that stayed outside a
    //! reducer into everything the reducer produced. `quadratic_sign` is a number
    //! rather than an expression, so a squared term needs the factor's value here.
    void ApplyScaleToObjective(const BoundFunctionExpression &scale_func, const Expression &scale,
                               bool divides, Objective &obj, idx_t linear_before,
                               idx_t bilinear_before, idx_t squared_before) {
        auto scaled = [&](unique_ptr<Expression> coef) {
            const auto &lhs_type = scale_func.function.arguments[divides ? 0 : 1];
            const auto &rhs_type = scale_func.function.arguments[divides ? 1 : 0];
            vector<unique_ptr<Expression>> children;
            if (divides) {
                children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), lhs_type));
                children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), rhs_type));
            } else {
                children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), rhs_type));
                children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), lhs_type));
            }
            return make_uniq_base<Expression, BoundFunctionExpression>(
                scale_func.return_type, scale_func.function, std::move(children), nullptr);
        };
        for (idx_t i = linear_before; i < obj.terms.size(); i++) {
            obj.terms[i].coefficient = scaled(std::move(obj.terms[i].coefficient));
        }
        for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
            auto &bt = obj.bilinear_terms[i];
            bt.coefficient = bt.coefficient
                                 ? scaled(std::move(bt.coefficient))
                                 : scaled(make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1)));
        }
        if (squared_before == obj.squared_terms.size()) {
            return;
        }
        const Expression *literal = &scale;
        while (literal->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            literal = literal->Cast<BoundCastExpression>().child.get();
        }
        if (literal->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
            throw InvalidInputException(
                "DECIDE objective: a squared term cannot be multiplied by '%s', whose value is "
                "not known until the query runs. Use a constant factor, or move it inside the "
                "aggregate as SUM(%s * POWER(...)).",
                scale.GetName(), scale.GetName());
        }
        double factor = literal->Cast<BoundConstantExpression>()
                            .value.DefaultCastAs(LogicalType::DOUBLE)
                            .GetValue<double>();
        if (divides && factor == 0.0) {
            throw InvalidInputException("DECIDE objective: division by zero in a squared term.");
        }
        obj.quadratic_sign *= divides ? 1.0 / factor : factor;
    }

    void ExtractAggregateObjectiveTerms(const Expression &expr, Objective &obj, int sign) {
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            auto &cast = expr.Cast<BoundCastExpression>();
            ExtractAggregateObjectiveTerms(*cast.child, obj, sign);
            return;
        }
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto &func = expr.Cast<BoundFunctionExpression>();
            if (func.function.name == "+") {
                for (auto &child : func.children) {
                    ExtractAggregateObjectiveTerms(*child, obj, sign);
                }
                return;
            }
            if (func.function.name == "-" && func.children.size() == 2) {
                ExtractAggregateObjectiveTerms(*func.children[0], obj, sign);
                ExtractAggregateObjectiveTerms(*func.children[1], obj, -sign);
                return;
            }
            if (func.function.name == "-" && func.children.size() == 1) {
                ExtractAggregateObjectiveTerms(*func.children[0], obj, -sign);
                return;
            }
        }
        // A factor left outside a reducer (`2 * SUM(x*p)`): extract the reducer, then
        // multiply the factor into everything it produced. Mirrors the constraint
        // side; objectives are not canonicalized, so both spellings arrive as written.
        {
            const Expression *obj_scale = nullptr;
            bool obj_divides = false;
            const BoundFunctionExpression *obj_scale_func = nullptr;
            if (const Expression *agg_expr = AsScaledAggregate(expr, obj_scale, obj_divides, obj_scale_func)) {
                // The canonicalizer vets a factor before it gets here -- but it only runs
                // on CONSTRAINTS, so an objective arrives unvetted and this is the first
                // place that can say no. A decision on both sides of the `*` is a product
                // of two decisions (bilinear), not a scaled reducer; treating it as a
                // coefficient reads a decision column as data and crashes in evaluation.
                if (op.FindDecideVariable(*obj_scale) != DConstants::INVALID_INDEX) {
                    throw InvalidInputException(
                        "DECIDE objective: '%s' is a decision, so it cannot multiply an "
                        "aggregate. Only constants and query-wide values can scale "
                        "SUM/AVG/MIN/MAX.",
                        ScaleUserName(*obj_scale));
                }
                idx_t linear_before = obj.terms.size();
                idx_t bilinear_before = obj.bilinear_terms.size();
                idx_t squared_before = obj.squared_terms.size();
                ExtractAggregateObjectiveTerms(*agg_expr, obj, sign);
                ApplyScaleToObjective(*obj_scale_func, *obj_scale, obj_divides, obj, linear_before,
                                      bilinear_before, squared_before);
                return;
            }
        }
        // A query-wide decision contributes without a reducer.
        if (IsScalarDecideTerm(expr)) {
            ExtractLinearAndBilinearTerms(expr, obj, sign, nullptr);
            return;
        }
        if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
            throw InvalidInputException(
                "DECIDE objective contains a non-aggregate term: %s.\n"
                "The objective must be a SUM/MIN/MAX/AVG of an expression in decision variables.\n"
                "If you wrapped an aggregate inside another function (e.g. POWER(AVG(x), 2)), "
                "use the supported shape SUM(POWER(x, 2)) instead.",
                expr.ToString());
        }

        auto &agg = expr.Cast<BoundAggregateExpression>();
        auto agg_name = StringUtil::Lower(agg.function.name);
        if (agg_name != "sum") {
            throw InvalidInputException("DECIDE optimizer should rewrite objective aggregate '%s' to SUM before execution",
                                        agg.function.name);
        }
        bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
        idx_t qualifier_scope = QualifierScopeOf(agg);

        idx_t before = obj.terms.size();
        idx_t bilinear_before = obj.bilinear_terms.size();
        idx_t squared_before = obj.squared_terms.size();
        ExtractLinearAndBilinearTerms(*agg.children[0], obj, sign, agg.filter.get());
        for (idx_t i = before; i < obj.terms.size(); i++) {
            obj.terms[i].avg_scale = is_avg;
            obj.terms[i].qualifier_scope_idx = qualifier_scope;
        }
        for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
            obj.bilinear_terms[i].avg_scale = is_avg;
            obj.bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
        }
        for (idx_t i = squared_before; i < obj.squared_terms.size(); i++) {
            obj.squared_terms[i].avg_scale = is_avg;
            obj.squared_terms[i].qualifier_scope_idx = qualifier_scope;
        }
    }

    void AnalyzeObjective(const unique_ptr<Expression>& expr_ptr) {
        auto *expr = expr_ptr.get();
        while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            expr = expr->Cast<BoundCastExpression>().child.get();
        }

        // DecidB: Check for PER wrapper on objective (outermost layer)
        vector<unique_ptr<Expression>> per_cols;
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
            auto &conj = expr->Cast<BoundConjunctionExpression>();
            if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
                for (idx_t i = 1; i < conj.children.size(); i++) {
                    per_cols.push_back(conj.children[i]->Copy());
                }
                expr = conj.children[0].get();
                while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    expr = expr->Cast<BoundCastExpression>().child.get();
                }
            }
        }

        // DecidB: Check for WHEN wrapper on objective (inside PER, if present)
        unique_ptr<Expression> when_cond;
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
            auto &conj = expr->Cast<BoundConjunctionExpression>();
            if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                when_cond = conj.children[1]->Copy();
                // Unwrap to get the actual objective expression
                expr = conj.children[0].get();
                while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    expr = expr->Cast<BoundCastExpression>().child.get();
                }
            }
        }

        if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
            auto &agg = expr->Cast<BoundAggregateExpression>();

            objective = make_uniq<Objective>();

            // Walk the SUM argument. ExtractLinearAndBilinearTerms recognises
            // quadratic patterns (POWER/(expr)*(expr)/negated/K*POWER) at any
            // position in `+`/`-` trees and routes them into squared_terms, so
            // the same walker handles pure QP, pure linear+bilinear, and the
            // mixed forms (e.g. SUM(POWER(x-t, 2) + penalty*x)) uniformly.
            idx_t before = objective->terms.size();
            idx_t bilinear_before = objective->bilinear_terms.size();
            idx_t squared_before = objective->squared_terms.size();
            ExtractLinearAndBilinearTerms(*agg.children[0], *objective, 1, agg.filter.get());
            bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
            idx_t qualifier_scope = QualifierScopeOf(agg);
            for (idx_t i = before; i < objective->terms.size(); i++) {
                objective->terms[i].avg_scale = is_avg;
                objective->terms[i].qualifier_scope_idx = qualifier_scope;
            }
            for (idx_t i = bilinear_before; i < objective->bilinear_terms.size(); i++) {
                objective->bilinear_terms[i].avg_scale = is_avg;
                objective->bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
            }
            for (idx_t i = squared_before; i < objective->squared_terms.size(); i++) {
                objective->squared_terms[i].avg_scale = is_avg;
                objective->squared_terms[i].qualifier_scope_idx = qualifier_scope;
            }

            objective->when_condition = std::move(when_cond);
            objective->per_columns = std::move(per_cols);
        } else if (BoundExpressionContainsAggregate(*expr) || IsScalarDecideTerm(*expr)) {
            // The second arm covers an objective made only of query-wide decisions
            // (e.g. `minimize max_shortfall`), which carries no aggregate at all.
            objective = make_uniq<Objective>();
            ExtractAggregateObjectiveTerms(*expr, *objective, 1);
            objective->when_condition = std::move(when_cond);
            objective->per_columns = std::move(per_cols);
        }
    }

    //===--------------------------------------------------------------------===//
    // Variable Bounds Extraction (Part 3)
    //===--------------------------------------------------------------------===//

    // Absorbs literal `x OP const` / BETWEEN constraints the query actually wrote
    // into the column-bound arrays. A variable's intrinsic domain (BOOLEAN 0/1,
    // default non-negativity) is never represented here — it comes from
    // `is_boolean_var` directly (see PhysicalDecide::Finalize) — so this only ever
    // sees genuine user constraints.
    void TraverseBoundsConstraints(const Expression &expr,
                                   vector<double> &lower_bounds,
                                   vector<double> &upper_bounds) {
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // DecidB PER: only recurse into the constraint (child[0]), skip the columns
                if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
                    TraverseBoundsConstraints(*conj.children[0], lower_bounds, upper_bounds);
                    break;
                }
                // DecidB WHEN: skip entirely — WHEN constraints are conditional (per-row),
                // so they must NOT contribute to global variable bounds.
                // E.g., "x <= 0 WHEN condition" should NOT set upper_bounds[x] = 0 globally.
                if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                    break;
                }
                // AND expression - recurse on all children
                for (auto &child : conj.children) {
                    TraverseBoundsConstraints(*child, lower_bounds, upper_bounds);
                }
                break;
            }

            case ExpressionClass::BOUND_COMPARISON: {
                auto &comp = expr.Cast<BoundComparisonExpression>();

                // Only extract global bounds from simple "x OP constant" constraints,
                // where x is a bare DECIDE variable (possibly cast-wrapped).
                // Multi-variable expressions (e.g., x - 3*z_1 - 5*z_2 = 0 from IN rewrite)
                // must NOT be treated as single-variable bounds.
                auto *lhs = comp.left.get();
                while (lhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    lhs = lhs->Cast<BoundCastExpression>().child.get();
                }

                if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    // Simple single-variable LHS — check if it's a DECIDE variable
                    auto &colref = lhs->Cast<BoundColumnRefExpression>();
                    idx_t var_idx = DConstants::INVALID_INDEX;
                    for (idx_t i = 0; i < op.decide_variables.size(); i++) {
                        auto &decide_var = op.decide_variables[i]->Cast<BoundColumnRefExpression>();
                        if (colref.binding == decide_var.binding) {
                            var_idx = i;
                            break;
                        }
                    }

                    if (var_idx != DConstants::INVALID_INDEX) {
                        // Extract bound value from RHS, unwrapping CASTs
                        // (DuckDB may insert implicit casts like CAST(5 AS INTEGER))
                        auto *rhs_ptr = comp.right.get();
                        while (rhs_ptr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                            rhs_ptr = rhs_ptr->Cast<BoundCastExpression>().child.get();
                        }
                        if (rhs_ptr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                            auto &rhs = rhs_ptr->Cast<BoundConstantExpression>();

                            // Cast to double - handle both INTEGER and DOUBLE types
                            double bound_value;
                            if (rhs.value.type().id() == LogicalTypeId::INTEGER ||
                                rhs.value.type().id() == LogicalTypeId::BIGINT) {
                                bound_value = static_cast<double>(rhs.value.GetValue<int64_t>());
                            } else if (rhs.value.type().id() == LogicalTypeId::DOUBLE ||
                                       rhs.value.type().id() == LogicalTypeId::FLOAT) {
                                bound_value = rhs.value.GetValue<double>();
                            } else {
                                // Try default cast
                                bound_value = rhs.value.GetValue<double>();
                            }

                            bool is_integer_var =
                                (op.decide_variables[var_idx]->return_type.id() == LogicalTypeId::INTEGER ||
                                 op.decide_variables[var_idx]->return_type.id() == LogicalTypeId::BIGINT);
                            // The intrinsic `[0,1]` domain of a BOOLEAN-domain variable
                            // (`op.is_boolean_var`) is applied directly to the solver
                            // column in PhysicalDecide::Finalize, never synthesized as a
                            // constraint — so a bare `x >= 0` / `x <= 1` only reaches here
                            // if the user wrote it themselves, redundantly restating the
                            // domain. That restatement is never a loosenable parameter:
                            // apply it to the column bounds (a no-op, since the intrinsic
                            // domain already enforces it) but do NOT record it for elastic
                            // re-emission. A genuine user PIN on a BOOLEAN (`x <= 0`,
                            // `x >= 1`, `x = 1`) tightens the box below/above the domain
                            // and IS recorded — erasing it made the elastic model diverge
                            // from the user's query (wrong or missing infeasible diagnoses).
                            bool is_bool_var =
                                var_idx < op.is_boolean_var.size() && op.is_boolean_var[var_idx];
                            auto RecordBound = [&](char sense, double k) {
                                if (is_bool_var) {
                                    // Skip only domain restatements: an upper at/above 1
                                    // or a lower at/below 0 does not tighten [0,1].
                                    if (sense == '<' && k >= 1.0) return false;
                                    if (sense == '>' && k <= 0.0) return false;
                                }
                                return true;
                            };
                            bool absorbed = true;
                            // Apply bound based on comparison type. Each absorbed
                            // direction is also recorded as a UserBoundSpec so the
                            // infeasible diagnosis can re-emit it as a slackable row;
                            // the spec carries the same integer-strict normalization.
                            if (comp.type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value);
                                if (RecordBound('<', bound_value)) user_absorbed_bounds.push_back({var_idx, '<', bound_value});
                            } else if (comp.type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value);
                                if (RecordBound('>', bound_value)) user_absorbed_bounds.push_back({var_idx, '>', bound_value});
                            } else if (comp.type == ExpressionType::COMPARE_EQUAL) {
                                // Intersect (like `<=`/`>=`), never overwrite: two equalities
                                // on one variable (`x = 5 AND x = 10`) must invert the box
                                // (lower 10 > upper 5) so the conflict is caught, not silently
                                // resolved to the last value written.
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value);
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value);
                                if (RecordBound('=', bound_value)) user_absorbed_bounds.push_back({var_idx, '=', bound_value});
                            } else if (comp.type == ExpressionType::COMPARE_LESSTHAN && is_integer_var) {
                                // x < bound → x <= bound-1 for integers. REAL strict
                                // inequality has no valid absorption — leave it for
                                // the constraint path which rejects with a clear error.
                                // Carry strict + typed_k so the diagnosis re-quotes `< bound`.
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value - 1.0);
                                if (RecordBound('<', bound_value - 1.0)) user_absorbed_bounds.push_back({var_idx, '<', bound_value - 1.0, true, bound_value});
                            } else if (comp.type == ExpressionType::COMPARE_GREATERTHAN && is_integer_var) {
                                // x > bound → x >= bound+1 for integers.
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value + 1.0);
                                if (RecordBound('>', bound_value + 1.0)) user_absorbed_bounds.push_back({var_idx, '>', bound_value + 1.0, true, bound_value});
                            } else {
                                absorbed = false;
                            }
                            if (absorbed) {
                                absorbed_bound_exprs.insert(&expr);
                            }
                        }
                    }
                }
                break;
            }

            case ExpressionClass::BOUND_BETWEEN: {
                auto &between = expr.Cast<BoundBetweenExpression>();

                auto *input = between.input.get();
                while (input->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    input = input->Cast<BoundCastExpression>().child.get();
                }

                if (input->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto &colref = input->Cast<BoundColumnRefExpression>();
                    idx_t var_idx = DConstants::INVALID_INDEX;
                    for (idx_t i = 0; i < op.decide_variables.size(); i++) {
                        auto &decide_var = op.decide_variables[i]->Cast<BoundColumnRefExpression>();
                        if (colref.binding == decide_var.binding) {
                            var_idx = i;
                            break;
                        }
                    }

                    if (var_idx != DConstants::INVALID_INDEX) {
                        bool is_integer_var =
                            (op.decide_variables[var_idx]->return_type.id() == LogicalTypeId::INTEGER ||
                             op.decide_variables[var_idx]->return_type.id() == LogicalTypeId::BIGINT);
                        // Same BOOLEAN rule as the COMPARISON arm: record a side only
                        // when it tightens the intrinsic [0,1] box (a genuine user pin),
                        // never when it merely restates the domain.
                        bool is_bool_var =
                            var_idx < op.is_boolean_var.size() && op.is_boolean_var[var_idx];
                        auto RecordBound = [&](char sense, double k) {
                            if (is_bool_var) {
                                if (sense == '<' && k >= 1.0) return false;
                                if (sense == '>' && k <= 0.0) return false;
                            }
                            return true;
                        };

                        auto ExtractBound = [](const Expression *e) -> double {
                            while (e->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                                e = e->Cast<BoundCastExpression>().child.get();
                            }
                            if (e->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
                                return std::numeric_limits<double>::quiet_NaN();
                            }
                            auto &c = e->Cast<BoundConstantExpression>();
                            if (c.value.type().id() == LogicalTypeId::INTEGER ||
                                c.value.type().id() == LogicalTypeId::BIGINT) {
                                return static_cast<double>(c.value.GetValue<int64_t>());
                            }
                            return c.value.GetValue<double>();
                        };

                        double lo = ExtractBound(between.lower.get());
                        double hi = ExtractBound(between.upper.get());

                        // BETWEEN is absorbed but (unlike COMPARISON) never tracked
                        // for re-emission until now. Record each finite side as its
                        // own UserBoundSpec so the infeasible diagnosis loosens
                        // BETWEEN uniformly with the other simple bounds (I1).
                        if (!std::isnan(lo)) {
                            // A strict lower side (`a <` …) is integer-normalized to lo+1;
                            // carry strict + the user's typed literal so the diagnosis
                            // re-quotes `> a` rather than the normalized `>= a+1`.
                            bool lo_strict = !between.lower_inclusive && is_integer_var;
                            double lo_typed = lo;
                            if (lo_strict) lo += 1.0;
                            lower_bounds[var_idx] = std::max(lower_bounds[var_idx], lo);
                            if (RecordBound('>', lo)) user_absorbed_bounds.push_back({var_idx, '>', lo, lo_strict, lo_typed});
                        }
                        if (!std::isnan(hi)) {
                            bool hi_strict = !between.upper_inclusive && is_integer_var;
                            double hi_typed = hi;
                            if (hi_strict) hi -= 1.0;
                            upper_bounds[var_idx] = std::min(upper_bounds[var_idx], hi);
                            if (RecordBound('<', hi)) user_absorbed_bounds.push_back({var_idx, '<', hi, hi_strict, hi_typed});
                        }
                    }
                }
                break;
            }

            case ExpressionClass::BOUND_CONSTANT: {
                // Type declarations return dummy constants - skip them
                break;
            }

            default:
                break;
        }
    }

    mutex lock;
    // This collection will hold all the data from the child operator
    ColumnDataCollection data;

    //! Kept so the term extractors can re-bind a rebuilt product through
    //! `FunctionBinder` instead of reusing another node's signature — see
    //! `RebindOperator`. Analysis runs entirely inside this state's constructor,
    //! so the reference stays valid for every use.
    ClientContext &context;

    const PhysicalDecide &op;

    vector<unique_ptr<DecideConstraint>> constraints;
    unique_ptr<Objective> objective;

    //! Variable bounds populated once in the constructor from simple
    //! `x OP const` / `x BETWEEN a AND b` constraints. Finalize copies these
    //! into solver_input instead of re-walking the expression tree.
    vector<double> absorbed_lower_bounds;
    vector<double> absorbed_upper_bounds;
    //! BOUND_COMPARISON expression pointers that were fully absorbed into
    //! column bounds — AnalyzeConstraint skips these to avoid emitting
    //! `num_rows` redundant per-row model rows per absorbed bound.
    std::unordered_set<const Expression *> absorbed_bound_exprs;
    //! Structured record of every user-written simple bound absorbed above, kept so
    //! the infeasible diagnosis can re-emit them as slackable rows (I1, decision 1a).
    //! Recorded for both COMPARISON and BETWEEN (the latter as two specs).
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

    // Bilinear objective: pairs of different decide variables with data coefficients
    struct EvaluatedBilinearTerm {
        idx_t var_a;
        idx_t var_b;
        CoefficientColumn row_coefficients;
    };
    vector<EvaluatedBilinearTerm> evaluated_bilinear_terms;

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

// --- Data-driven Big-M support ---------------------------------------------
// A Big-M linearization toggles a constraint on/off via a binary indicator. The
// constant M must be at least the reachable magnitude of the constraint's
// left-hand expression: too small silently distorts the feasible region (wrong
// answer), too large degrades numerical stability. We therefore derive M from
// the actual variable bounds and per-row coefficient data instead of a fixed
// constant, mirroring the long-standing ABS-maximize path.

//! Worst-case absolute contribution of row `r`'s decision variables:
//! sum over terms of |coef[t][r]| * max(|lb|,|ub|). Constant terms
//! (var == INVALID_INDEX, folded into the RHS by callers) are skipped. If any
//! contributing variable lacks a finite bound, `has_unbounded` is set and that
//! term is omitted so the caller can apply a conservative fallback.
static double DecideRowTermRange(const vector<idx_t> &variable_indices,
                                 const vector<CoefficientColumn> &row_coefficients,
                                 idx_t row, const vector<double> &lower_bounds,
                                 const vector<double> &upper_bounds,
                                 bool &has_unbounded,
                                 idx_t skip_idx = DConstants::INVALID_INDEX) {
    double sum = 0.0;
    for (idx_t t = 0; t < variable_indices.size(); t++) {
        idx_t v = variable_indices[t];
        if (v == DConstants::INVALID_INDEX || v == skip_idx) {
            continue;
        }
        double coef = std::abs(row_coefficients[t].Get(row));
        if (coef < 1e-15) {
            continue;
        }
        double lb = lower_bounds[v];
        double ub = upper_bounds[v];
        if (ub >= 1e20 || lb <= -1e20) {
            has_unbounded = true;
            continue;
        }
        sum += coef * std::max(std::abs(lb), std::abs(ub));
    }
    return sum;
}

static double DecideRowFixedLhsOffset(const vector<idx_t> &variable_indices,
                                      const vector<CoefficientColumn> &row_coefficients,
                                      idx_t row) {
    double offset = 0.0;
    for (idx_t t = 0; t < variable_indices.size(); t++) {
        if (variable_indices[t] == DConstants::INVALID_INDEX) {
            offset += row_coefficients[t].Get(row);
        }
    }
    return offset;
}

//! Legacy fixed Big-M, retained only as a non-strict fallback for genuinely
//! unbounded variables (no query is rejected; behavior matches the prior code).
static constexpr double DECIDE_BIGM_FALLBACK = 1e6;

//! Tight scalar Big-M for a per-row indicator constraint: the maximum over
//! active rows of |rhs[r]| + (worst-case row contribution), plus a 1-unit margin
//! that covers the integer-step band of the `<>` rewrite (harmless slack for the
//! MIN/MAX rewrites). When every contributing variable is bounded this is exact
//! and typically far below 1e6; otherwise we keep the 1e6 floor.
static double DecideTightPerRowBigM(const EvaluatedConstraint &ec,
                                    const vector<double> &lower_bounds,
                                    const vector<double> &upper_bounds,
                                    idx_t num_rows) {
    bool has_unbounded = false;
    double M = 0.0;
    bool uniform_rhs = ec.rhs_values.IsUniform();
    for (idx_t r = 0; r < num_rows; r++) {
        if (!ec.row_group_ids.empty() &&
            ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
            continue;
        }
        double rhs = uniform_rhs ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(r);
        rhs -= DecideRowFixedLhsOffset(ec.variable_indices, ec.row_coefficients, r);
        double rhs_mag = std::abs(rhs);
        double range = rhs_mag + DecideRowTermRange(ec.variable_indices, ec.row_coefficients,
                                                    r, lower_bounds, upper_bounds, has_unbounded);
        M = std::max(M, range);
    }
    M += 1.0;
    if (has_unbounded) {
        M = std::max(M, DECIDE_BIGM_FALLBACK);
    }
    return M;
}

//! Data-driven implied-bound propagation. For a non-negative `<=`/`=` constraint
//! Sum_t a_t x_t (<=|=) K with a_t >= 0 and x_t >= 0, each variable instance
//! satisfies x <= K / a (the other non-negative terms only help), so a sound
//! shared upper bound is max over the variable's positive-coefficient rows of
//! K_r / a_r. This converts many declared-unbounded variables into bounded ones,
//! enabling a finite, correct, tighter Big-M. Only provably-implied bounds are
//! applied, so the feasible region — and the optimum — are unchanged.
//!
//! Single pass (not a fixpoint): a bound derived for one variable is not fed back
//! to tighten others in the same sweep. This is sound — it only leaves some
//! tightness on the table for chained implications — and avoids iterating to
//! convergence over potentially many constraints.
static void DecidePropagateImpliedBounds(const vector<EvaluatedConstraint> &constraints,
                                         vector<double> &lower_bounds,
                                         vector<double> &upper_bounds, idx_t num_rows) {
    for (auto &ec : constraints) {
        if (ec.comparison_type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
            ec.comparison_type != ExpressionType::COMPARE_EQUAL) {
            continue;
        }
        if (!ec.bilinear_terms.empty() || ec.has_quadratic) {
            continue;
        }
        if (ec.minmax_indicator_idx != DConstants::INVALID_INDEX ||
            ec.ne_indicator_idx != DConstants::INVALID_INDEX ||
            ec.abs_y_idx != DConstants::INVALID_INDEX) {
            continue;
        }
        // WHEN-conditional constraints apply to only a subset of rows, but the
        // bound we derive is shared across ALL of a variable's rows. Deriving it
        // from the active subset would wrongly bound the excluded (WHEN-false)
        // rows, which carry no such constraint. Skip any constraint that has
        // excluded rows.
        bool has_excluded = false;
        for (idx_t gid : ec.row_group_ids) {
            if (gid == DConstants::INVALID_INDEX) {
                has_excluded = true;
                break;
            }
        }
        if (has_excluded) {
            continue;
        }
        // Soundness requires every term to be non-negative — both the variable
        // lower bounds (x >= 0) AND the coefficients (a >= 0) — so that dropping
        // the other terms to derive x_t <= K/a_t is valid. Constraints with any
        // negative coefficient (e.g. the IN/ABS rewrites such as x - z1 - 3*z2 = 0)
        // must be skipped: dropping a negative term would wrongly tighten the
        // bound and cut feasible points (or make the model infeasible).
        bool nonneg = true;
        for (idx_t v : ec.variable_indices) {
            if (v != DConstants::INVALID_INDEX && lower_bounds[v] < 0.0) {
                nonneg = false;
                break;
            }
        }
        for (idx_t t = 0; nonneg && t < ec.row_coefficients.size(); t++) {
            const auto &col = ec.row_coefficients[t];
            if (col.IsUniform()) {
                // Scalar column: one value covers every row.
                if (col.UniformValue() < -1e-15) {
                    nonneg = false;
                }
                continue;
            }
            for (idx_t r = 0; r < num_rows; r++) {
                if (!ec.row_group_ids.empty() &&
                    ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                    continue;
                }
                if (col.Get(r) < -1e-15) {
                    nonneg = false;
                    break;
                }
            }
        }
        if (!nonneg) {
            continue;
        }
        bool uniform_rhs = ec.rhs_values.IsUniform();
        for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
            idx_t v = ec.variable_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                continue;
            }
            const auto &col = ec.row_coefficients[t];
            double bound = 0.0;
            // The shared bound ub_x applies to EVERY row instance of x. A row
            // bounds x only if it is active (not WHEN-excluded) AND x has a
            // non-zero coefficient there. WHEN-excluded rows show up as a zero
            // coefficient (not always as a row_group_ids marker), so if ANY row
            // leaves x_r unconstrained we cannot use this constraint to bound the
            // shared variable — doing so would cap the unconstrained rows.
            // Two benign degenerate edges, both sound and intentionally left as-is:
            //  - num_rows == 0: the loop never runs, every_row_constrained stays
            //    true and bound stays 0, so ub may be pinned to 0 — harmless,
            //    because with no rows there are no instances of x to bind.
            //  - a tiny positive coefficient (a just above 1e-15): K/a is a huge
            //    but still-VALID upper bound; it merely re-inflates M and loosens
            //    the relaxation. Degenerate input, sound, not worth special-casing.
            bool every_row_constrained = true;
            for (idx_t r = 0; r < num_rows; r++) {
                bool excluded = !ec.row_group_ids.empty() &&
                                ec.row_group_ids[r] == DConstants::INVALID_INDEX;
                double a = col.Get(r);
                if (excluded || a <= 1e-15) {
                    every_row_constrained = false;
                    break;
                }
                double k = uniform_rhs ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(r);
                if (k < 0.0) {
                    every_row_constrained = false;
                    break;
                }
                bound = std::max(bound, k / a);
            }
            if (every_row_constrained && bound < upper_bounds[v] && bound >= lower_bounds[v]) {
                upper_bounds[v] = bound;
            }
        }
    }
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

    //===--------------------------------------------------------------------===//
    // PHASE 2: Evaluate Coefficient Expressions
    //===--------------------------------------------------------------------===//

    //! Per-term filter state for aggregate-local WHEN.
    struct TermFilterState {
        vector<bool> mask;
        bool has_filter = false;
        bool avg_scale = false;
    };

    // Which rows a relation-qualified reducer (`sum(D: ...)`) actually contributes.
    // The join repeats each D tuple once per matching row; §3.2.2 asks for one term per
    // tuple identity, and the binder has already guaranteed that every row sharing an
    // identity carries the same value — so keeping the first row of each identity and
    // dropping the rest yields exactly one contribution per identity, whichever row is
    // kept. De-duplication runs inside the PER partition, after `when` selection and
    // `per` partitioning, which is the construction order the paper pins.
    // `mappings` is passed rather than captured: the local entity_mappings is moved onto
    // solver_input partway down Finalize, so the constraint side reads the local and the
    // objective side reads solver_input's copy.
    auto BuildQualifierKeepMask = [&](const vector<EntityMapping> &mappings, idx_t scope_idx,
                                      const vector<idx_t> &row_group_ids) {
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
    };

    // Fold that mask into a term's filter state, so everything downstream — coefficient
    // zeroing, AVG's denominator (which counts surviving rows and therefore becomes the
    // distinct-identity count), the empty-aggregate guard — treats a duplicate row
    // exactly as it treats a WHEN-excluded one.
    auto ApplyQualifierToFilter = [&](const vector<EntityMapping> &mappings, idx_t scope_idx,
                                      const vector<idx_t> &row_group_ids, TermFilterState &state) {
        auto keep = BuildQualifierKeepMask(mappings, scope_idx, row_group_ids);
        if (!state.has_filter) {
            state.mask = std::move(keep);
            state.has_filter = true;
            return;
        }
        for (idx_t row = 0; row < num_rows; row++) {
            state.mask[row] = state.mask[row] && keep[row];
        }
    };

    // Zero every row the (now possibly de-duplicated) mask drops. The WHEN pass already
    // did this for its own mask; re-applying the combined mask is idempotent.
    auto MaskCoefficientColumn = [&](CoefficientColumn &column, const vector<bool> &mask) {
        auto &values = column.MutableDense();
        for (idx_t row = 0; row < values.size(); row++) {
            if (!mask[row]) {
                values[row] = 0.0;
            }
        }
    };

    // Evaluate N boolean filter expressions in a single scan over gstate.data.
    auto EvaluateBooleanMasks = [&](const vector<const Expression*> &conditions) -> vector<vector<bool>> {
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
        gstate.data.InitializeScan(cond_scan_state);
        DataChunk cond_chunk;
        cond_chunk.Initialize(context, gstate.data.Types());

        while (gstate.data.Scan(cond_scan_state, cond_chunk)) {
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
    };

    auto EvaluateBooleanMask = [&](const Expression &condition) -> vector<bool> {
        return EvaluateBooleanMasks({&condition})[0];
    };

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
    auto EvaluateRhsReducerPerGroup = [&](const BoundAggregateExpression &agg,
                                          const vector<idx_t> &group_ids,
                                          idx_t num_groups) -> vector<double> {
        const bool has_groups = !group_ids.empty();
        const idx_t groups = has_groups ? num_groups : 1;
        auto group_of = [&](idx_t row) -> idx_t {
            return has_groups ? group_ids[row] : 0;
        };

        // Stage 1: the reducer's own WHEN (`SUM(b) WHEN w`), which scopes this reducer
        // and nothing else.
        vector<bool> keep;
        if (agg.filter) {
            keep = EvaluateBooleanMask(*agg.filter);
        }
        // Stage 2: relation-qualified reducers (`sum(D: cost)`) contribute once per
        // distinct entity, not once per joined row.
        idx_t scope_idx = DecideGlobalSinkState::QualifierScopeOf(agg);
        if (scope_idx != DConstants::INVALID_INDEX) {
            auto dedup = BuildQualifierKeepMask(entity_mappings, scope_idx, group_ids);
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
            gstate.data.InitializeScan(scan);
            DataChunk in_chunk;
            in_chunk.Initialize(context, gstate.data.Types());
            DataChunk out_chunk;
            out_chunk.Initialize(context, vector<LogicalType>{arg.return_type});
            while (gstate.data.Scan(scan, in_chunk)) {
                out_chunk.Reset();
                arg_executor.Execute(in_chunk, out_chunk);
                ExtractDoubleColumn(out_chunk.data[0], in_chunk.size(), 1.0, values,
                                    "constraint right-hand side aggregate");
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
    };

    // 1. Evaluate constraints
    for (idx_t c = 0; c < gstate.constraints.size(); c++) {
        auto &constraint = gstate.constraints[c];

        EvaluatedConstraint eval_const;
        eval_const.comparison_type = constraint->comparison_type;
        // Preserve whether the original LHS was an aggregate (e.g., SUM(...))
        eval_const.lhs_is_aggregate = constraint->lhs_is_aggregate;
        eval_const.minmax_indicator_idx = constraint->minmax_indicator_idx;
        eval_const.minmax_agg_type = constraint->minmax_agg_type;
        eval_const.ne_indicator_idx = constraint->ne_indicator_idx;
        eval_const.abs_y_idx = constraint->abs_y_idx;
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

                auto masks = EvaluateBooleanMasks(cond_ptrs);

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

        // Facet C: render the WHEN/PER qualifier for the clause label, reusing the same
        // expression GetName() the EXPLAIN/ParamsToString path uses. Order mirrors the
        // postfix syntax (`... WHEN <cond> PER <cols>`). Stamped onto provenance at the
        // aggregate emission sites; the diagnosis appends it to the reconstructed label.
        {
            string &q = eval_const.qualifier;
            if (has_when) {
                q = "WHEN " + RenderWhenPredicate(*constraint->when_condition);
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
                    q += constraint->per_columns[i]->GetName();
                }
                if (parenthesize) {
                    q += ")";
                }
            }
        }

        if (has_when || has_per || has_local_filters) {
            vector<bool> when_mask;
            if (has_when) {
                when_mask = EvaluateBooleanMask(*constraint->when_condition);
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
            ApplyQualifierToFilter(entity_mappings, scope_idx, eval_const.row_group_ids, term_filters[term_idx]);
            MaskCoefficientColumn(eval_const.row_coefficients[term_idx], term_filters[term_idx].mask);
        }
        for (idx_t term_idx = 0; term_idx < constraint->bilinear_terms.size(); term_idx++) {
            idx_t scope_idx = constraint->bilinear_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(entity_mappings, scope_idx, eval_const.row_group_ids, bilinear_filters[term_idx]);
        }
        for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
            idx_t scope_idx = constraint->quadratic_groups[group_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(entity_mappings, scope_idx, eval_const.row_group_ids, quadratic_filters[group_idx]);
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
            // The RHS is one editable literal shared across every row this clause
            // emits → the elastic engine collapses those rows to one shared slack.
            eval_const.rhs_is_shared_literal = true;
        } else {
            // RHS is a complex expression. It might be row-varying (e.g., column ref) or scalar (aggregate).
            // We evaluate it against the data chunks.
            // A *foldable* RHS (e.g. `2 + 3`, `5 * 2`, `ABS(-4)`) is one compile-time
            // scalar shared by every row this clause emits, exactly like a bare literal
            // — collapse those rows to one shared slack so the infeasible diagnosis
            // treats it as a single editable cap, not per-row data. A row-varying RHS
            // (column ref, correlated subquery) is not foldable and stays PER_ROW_DATA.
            // An uncorrelated scalar subquery (`x <= (SELECT 5)`) flattens to a column
            // ref that is not foldable but IS one shared cap; the binder marked it with
            // SHARED_SCALAR_SUBQUERY_TAG before flattening so we recognize it here.
            eval_const.rhs_is_shared_literal =
                constraint->rhs_expr->IsFoldable() ||
                IsSharedScalarSubqueryTag(constraint->rhs_expr->GetAlias());
            // Capture the RHS symbolic name (e.g. the data column `cap_col`) so query-mode
            // infeasible diagnosis can render `x <= cap_col + delta`. Only meaningful for a
            // genuine data RHS; a shared-literal RHS reports the numeric knob instead.
            if (!eval_const.rhs_is_shared_literal) {
                eval_const.rhs_label = RenderDiagnosticRhsLabel(*constraint->rhs_expr);
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
                    agg, rhs_row_group_ids, eval_const.num_groups));
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
                ExtractDoubleColumn(rhs_result.data[0], scan_chunk.size(), 1.0,
                                    rhs_col,
                                    "constraint right-hand side");
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
                                  ? RenderDiagnosticRhsLabel(*constraint->rhs_expr)
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

        auto ScaleAvgRowCoefficients = [&](CoefficientColumn &col, bool has_filter,
                                           const vector<bool> &filter_mask) {
            auto &coefficients = col.MutableDense();
            if (eval_const.row_group_ids.empty()) {
                idx_t denominator = 0;
                for (idx_t row = 0; row < num_rows; row++) {
                    if (!has_filter || filter_mask[row]) {
                        denominator++;
                    }
                }
                if (denominator == 0) {
                    std::fill(coefficients.begin(), coefficients.end(), 0.0);
                    return;
                }
                double scale = 1.0 / static_cast<double>(denominator);
                for (auto &coefficient : coefficients) {
                    coefficient *= scale;
                }
                return;
            }

            vector<idx_t> group_counts(eval_const.num_groups, 0);
            for (idx_t row = 0; row < num_rows; row++) {
                idx_t gid = eval_const.row_group_ids[row];
                if (gid == DConstants::INVALID_INDEX) {
                    continue;
                }
                if (!has_filter || filter_mask[row]) {
                    group_counts[gid]++;
                }
            }
            for (idx_t row = 0; row < coefficients.size(); row++) {
                idx_t gid = eval_const.row_group_ids[row];
                if (gid == DConstants::INVALID_INDEX || group_counts[gid] == 0) {
                    coefficients[row] = 0.0;
                    continue;
                }
                coefficients[row] /= static_cast<double>(group_counts[gid]);
            }
        };

        auto ScaleAvgQuadraticCoefficients = [&](vector<CoefficientColumn> &row_coefficients, bool has_filter,
                                                 const vector<bool> &filter_mask) {
            if (eval_const.row_group_ids.empty()) {
                idx_t denominator = 0;
                for (idx_t row = 0; row < num_rows; row++) {
                    if (!has_filter || filter_mask[row]) {
                        denominator++;
                    }
                }
                if (denominator == 0) {
                    for (auto &col : row_coefficients) {
                        auto &coefficients = col.MutableDense();
                        std::fill(coefficients.begin(), coefficients.end(), 0.0);
                    }
                    return;
                }
                double scale = 1.0 / std::sqrt(static_cast<double>(denominator));
                for (auto &col : row_coefficients) {
                    auto &coefficients = col.MutableDense();
                    for (auto &coefficient : coefficients) {
                        coefficient *= scale;
                    }
                }
                return;
            }

            vector<idx_t> group_counts(eval_const.num_groups, 0);
            for (idx_t row = 0; row < num_rows; row++) {
                idx_t gid = eval_const.row_group_ids[row];
                if (gid == DConstants::INVALID_INDEX) {
                    continue;
                }
                if (!has_filter || filter_mask[row]) {
                    group_counts[gid]++;
                }
            }
            for (auto &col : row_coefficients) {
                auto &coefficients = col.MutableDense();
                for (idx_t row = 0; row < coefficients.size(); row++) {
                    idx_t gid = eval_const.row_group_ids[row];
                    if (gid == DConstants::INVALID_INDEX || group_counts[gid] == 0) {
                        coefficients[row] = 0.0;
                        continue;
                    }
                    coefficients[row] /= std::sqrt(static_cast<double>(group_counts[gid]));
                }
            }
        };

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
                ScaleAvgRowCoefficients(eval_const.row_coefficients[term_idx], term_filters[term_idx].has_filter,
                                        term_filters[term_idx].mask);
            } else {
                all_avg = false;
            }
        }
        eval_const.avg_scaled = all_avg;

        // Evaluate bilinear terms in constraint (if any).
        // Batch terms with coefficient expressions into a single ExpressionExecutor.
        if (constraint->has_bilinear) {
            const idx_t num_bl = constraint->bilinear_terms.size();
            vector<EvaluatedConstraint::BilinearTerm> ebts(num_bl);
            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &bt = constraint->bilinear_terms[term_idx];
                ebts[term_idx].var_a = bt.var_a;
                ebts[term_idx].var_b = bt.var_b;
            }

            vector<idx_t> bl_route;       // result column index → bilinear_terms index
            vector<LogicalType> bl_types;
            ExpressionExecutor bl_executor(context);
            idx_t bl_added = 0;
            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &bt = constraint->bilinear_terms[term_idx];
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
                bl_added++;
            }

            if (bl_added > 0) {
                ColumnDataScanState bl_scan;
                gstate.data.InitializeScan(bl_scan);
                DataChunk bl_chunk;
                bl_chunk.Initialize(context, gstate.data.Types());
                DataChunk bl_results;
                bl_results.Initialize(context, bl_types);
                while (gstate.data.Scan(bl_scan, bl_chunk)) {
                    bl_results.Reset();
                    bl_executor.Execute(bl_chunk, bl_results);
                    for (idx_t j = 0; j < bl_route.size(); j++) {
                        idx_t term_idx = bl_route[j];
                        auto &col = ebts[term_idx].row_coefficients.MutableDense();
                        ExtractDoubleColumn(bl_results.data[j], bl_chunk.size(),
                                            constraint->bilinear_terms[term_idx].sign,
                                            col,
                                            "bilinear constraint coefficient");
                        ebts[term_idx].row_coefficients.SyncSize();
                    }
                }
            }

            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &ebt = ebts[term_idx];
                if (bilinear_filters[term_idx].has_filter) {
                    auto &mask = bilinear_filters[term_idx].mask;
                    auto &col = ebt.row_coefficients.MutableDense();
                    for (idx_t row = 0; row < col.size(); row++) {
                        if (!mask[row]) {
                            col[row] = 0.0;
                        }
                    }
                }
                if (bilinear_filters[term_idx].avg_scale) {
                    ScaleAvgRowCoefficients(ebt.row_coefficients, bilinear_filters[term_idx].has_filter,
                                            bilinear_filters[term_idx].mask);
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
                    ScaleAvgQuadraticCoefficients(eqg.row_coefficients, quadratic_filters[group_idx].has_filter,
                                                  quadratic_filters[group_idx].mask);
                }
                eval_const.quadratic_groups.push_back(std::move(eqg));
            }
        }

        gstate.evaluated_constraints.push_back(std::move(eval_const));
    }

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
            objective_when_mask = EvaluateBooleanMask(*gstate.objective->when_condition);
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
            vector<Term> *src_terms;
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
                auto masks = EvaluateBooleanMasks(cond_ptrs);
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
                auto masks = EvaluateBooleanMasks(cond_ptrs);
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
        // Batch terms with coefficient expressions into a single ExpressionExecutor.
        if (gstate.objective->has_bilinear) {
            const idx_t num_bl = gstate.objective->bilinear_terms.size();
            vector<DecideGlobalSinkState::EvaluatedBilinearTerm> ebts(num_bl);
            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &bt = gstate.objective->bilinear_terms[term_idx];
                ebts[term_idx].var_a = bt.var_a;
                ebts[term_idx].var_b = bt.var_b;
            }

            vector<idx_t> bl_route;
            vector<LogicalType> bl_types;
            ExpressionExecutor bl_executor(context);
            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &bt = gstate.objective->bilinear_terms[term_idx];
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
                gstate.data.InitializeScan(bl_scan);
                DataChunk bl_chunk;
                bl_chunk.Initialize(context, gstate.data.Types());
                DataChunk bl_results;
                bl_results.Initialize(context, bl_types);
                while (gstate.data.Scan(bl_scan, bl_chunk)) {
                    bl_results.Reset();
                    bl_executor.Execute(bl_chunk, bl_results);
                    for (idx_t j = 0; j < bl_route.size(); j++) {
                        idx_t term_idx = bl_route[j];
                        auto &col = ebts[term_idx].row_coefficients.MutableDense();
                        ExtractDoubleColumn(bl_results.data[j], bl_chunk.size(),
                                            gstate.objective->bilinear_terms[term_idx].sign,
                                            col,
                                            "bilinear objective coefficient");
                        ebts[term_idx].row_coefficients.SyncSize();
                    }
                }
            }

            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &ebt = ebts[term_idx];
                if (obj_bilinear_filters[term_idx].has_filter) {
                    auto &mask = obj_bilinear_filters[term_idx].mask;
                    auto &col = ebt.row_coefficients.MutableDense();
                    for (idx_t row = 0; row < col.size(); row++) {
                        if (!mask[row]) {
                            col[row] = 0.0;
                        }
                    }
                }
                if (objective_has_when) {
                    auto &col = ebt.row_coefficients.MutableDense();
                    for (idx_t row = 0; row < col.size(); row++) {
                        if (!objective_when_mask[row]) {
                            col[row] = 0.0;
                        }
                    }
                }
                gstate.evaluated_bilinear_terms.push_back(std::move(ebt));
            }
        }
    }

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP
    //===--------------------------------------------------------------------===//

    // Construct SolverInput (num_decide_vars already declared above)
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

    // Data-driven implied-bound propagation: derive finite upper bounds for
    // otherwise-unbounded variables from non-negative `<=`/`=` constraints (the
    // knapsack/budget pattern), so the downstream Big-M can be finite and tight.
    // Only provably-implied bounds are applied; the feasible region is unchanged.
    DecidePropagateImpliedBounds(gstate.evaluated_constraints, solver_input.lower_bounds,
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
            for (auto &ec : gstate.evaluated_constraints) {
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
                                                 solver_input.upper_bounds, num_rows);
                ec.row_coefficients[z_term].AssignScalar(num_rows, M);
            }
        }
    }

    // Generate Big-M constraints for MIN/MAX indicator variables
    // For hard cases where MIN/MAX was rewritten to SUM by the optimizer:
    //   MAX(expr) >= K: for each row i, expr_i - M*y_i >= K - M, and SUM(y) >= 1
    //   MIN(expr) <= K: for each row i, expr_i + M*y_i <= K + M, and SUM(y) >= 1
    // Constraints are matched to their indicator variables via explicit tags (not positional).
    if (!minmax_indicator_links.empty()) {
        vector<EvaluatedConstraint> new_constraints;
        for (auto &ec : gstate.evaluated_constraints) {
            // Skip constraints without a minmax indicator tag
            if (ec.minmax_indicator_idx == DConstants::INVALID_INDEX) {
                new_constraints.push_back(std::move(ec));
                continue;
            }

            idx_t indicator_idx = ec.minmax_indicator_idx;
            bool is_max_agg = (ec.minmax_agg_type == "max");

            // Compute Big-M from variable bounds. Skip constant LHS terms
            // (var_idx == INVALID_INDEX) — they have no associated variable
            // bound; their contribution will be folded into the RHS by the
            // per-row constraint emitter.
            double M = DecideTightPerRowBigM(ec, solver_input.lower_bounds,
                                             solver_input.upper_bounds, num_rows);

            auto BuildShiftedRhs = [&](double shift) {
                if (ec.rhs_values.IsUniform()) {
                    return CoefficientColumn::MakeScalar(ec.rhs_values.UniformValue() + shift, num_rows);
                }
                auto col = CoefficientColumn::MakeDense(num_rows, 0.0);
                for (idx_t r = 0; r < num_rows; r++) {
                    col.Set(r, ec.rhs_values.Get(r) + shift);
                }
                return col;
            };

            if (is_max_agg) {
                // Hard MAX(expr) >= K: for each row i, expr_i - M*y_i >= K - M
                // This is a per-row constraint (not aggregate)
                EvaluatedConstraint ec_row;
                ec_row.variable_indices = ec.variable_indices;
                ec_row.row_coefficients = ec.row_coefficients;
                // Add indicator variable: -M * y_i (broadcast)
                ec_row.variable_indices.push_back(indicator_idx);
                ec_row.row_coefficients.push_back(CoefficientColumn::MakeScalar(-M, num_rows));
                ec_row.rhs_values = BuildShiftedRhs(-M);
                ec_row.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
                ec_row.lhs_is_aggregate = false; // per-row!
                ec_row.row_group_ids = ec.row_group_ids;
                ec_row.num_groups = ec.num_groups;
                ec_row.group_labels = ec.group_labels;
                ec_row.qualifier = ec.qualifier;
                ec_row.kind = ConstraintKind::USER_MECHANISM;
                new_constraints.push_back(std::move(ec_row));

                // SUM(y) >= 1 (at least one row must satisfy)
                EvaluatedConstraint ec_sum;
                ec_sum.variable_indices = {indicator_idx};
                ec_sum.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
                ec_sum.rhs_values.AssignScalar(num_rows, 1.0);
                ec_sum.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
                ec_sum.lhs_is_aggregate = true;
                ec_sum.row_group_ids = ec.row_group_ids;
                ec_sum.num_groups = ec.num_groups;
                ec_sum.group_labels = ec.group_labels;
                ec_sum.qualifier = ec.qualifier;
                ec_sum.kind = ConstraintKind::USER_MECHANISM;
                new_constraints.push_back(std::move(ec_sum));
            } else {
                // MIN(expr) <= K: for each row i, expr_i + M*y_i <= K + M
                EvaluatedConstraint ec_row;
                ec_row.variable_indices = ec.variable_indices;
                ec_row.row_coefficients = ec.row_coefficients;
                // Add indicator variable: +M * y_i (broadcast)
                ec_row.variable_indices.push_back(indicator_idx);
                ec_row.row_coefficients.push_back(CoefficientColumn::MakeScalar(M, num_rows));
                ec_row.rhs_values = BuildShiftedRhs(M);
                ec_row.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
                ec_row.lhs_is_aggregate = false;
                ec_row.row_group_ids = ec.row_group_ids;
                ec_row.num_groups = ec.num_groups;
                ec_row.group_labels = ec.group_labels;
                ec_row.qualifier = ec.qualifier;
                ec_row.kind = ConstraintKind::USER_MECHANISM;
                new_constraints.push_back(std::move(ec_row));

                // SUM(y) >= 1
                EvaluatedConstraint ec_sum;
                ec_sum.variable_indices = {indicator_idx};
                ec_sum.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
                ec_sum.rhs_values.AssignScalar(num_rows, 1.0);
                ec_sum.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
                ec_sum.lhs_is_aggregate = true;
                ec_sum.row_group_ids = ec.row_group_ids;
                ec_sum.num_groups = ec.num_groups;
                ec_sum.group_labels = ec.group_labels;
                ec_sum.qualifier = ec.qualifier;
                ec_sum.kind = ConstraintKind::USER_MECHANISM;
                new_constraints.push_back(std::move(ec_sum));
            }
        }
        gstate.evaluated_constraints = std::move(new_constraints);
    }

    // Generate Big-M constraints for not-equal (<>) indicators.
    // For each COMPARE_NOTEQUAL constraint, replace it with two disjunctive constraints:
    //   x - M*z ≤ K-1        (z=0 → x ≤ K-1; z=1 → trivially true)
    //   x - M*z ≥ K+1-M      (z=0 → trivially true; z=1 → x ≥ K+1)
    //
    // Per-row NE: expanded inline with row-scoped indicator variables (one z per row).
    // Aggregate NE: deferred — expanded after the VarIndexer is built, using a single
    //   global binary z per group. This avoids the per-row z interaction with the
    //   aggregate constraint building path (unified path with row_group_ids).
    struct DeferredAggregateNE {
        EvaluatedConstraint original;
    };
    vector<DeferredAggregateNE> deferred_ne_aggregate;

    // The ±1 band above is only semantically exact when the LHS is integer-valued.
    // For REAL variables or non-integer coefficients the band (K-1, K+1) wrongly
    // excludes feasible continuous points. Mirror the strict-inequality guard in
    // ilp_model_builder.cpp::IsEvalConstraintLhsIntegerValued.
    auto NEIsRealType = [](const LogicalType &t) {
        return t == LogicalType::DOUBLE || t == LogicalType::FLOAT;
    };
    auto NELhsIsIntegerValued = [&](const EvaluatedConstraint &ec) -> bool {
        for (idx_t i = 0; i < ec.variable_indices.size(); i++) {
            idx_t vi = ec.variable_indices[i];
            if (vi == DConstants::INVALID_INDEX) continue;
            if (NEIsRealType(solver_input.variable_types[vi])) return false;
            if (!ec.row_coefficients[i].AllIntegral()) return false;
        }
        return true;
    };
    // Companion check on the RHS. With integer-valued LHS and a non-integer K,
    // `LHS <> K` is a tautology (no integer can equal K). The ±1 Big-M rewrite
    // would emit `LHS <= K-1 ∨ LHS >= K+1`, which on the integer lattice
    // wrongly excludes floor(K) and ceil(K) — both of which the original
    // predicate accepted. Treat such RHS values as a silent drop.
    auto NEIsIntegerValuedRhs = [](double k) {
        return std::abs(k - std::round(k)) < 1e-9;
    };

    if (!ne_indicator_indices.empty()) {
        vector<EvaluatedConstraint> new_constraints;
        for (auto &ec : gstate.evaluated_constraints) {
            if (ec.ne_indicator_idx != DConstants::INVALID_INDEX) {
                if (!NELhsIsIntegerValued(ec)) {
                    throw InvalidInputException(
                        "Inequality '<>' is not supported when the left-hand side "
                        "involves a REAL variable or a non-integer coefficient. "
                        "The integer-step rewrite (x <> K → x <= K-1 OR x >= K+1) "
                        "would cut continuous feasible points in the band (K-1, K+1).");
                }
                if (ec.lhs_is_aggregate) {
                    // Aggregate NE: defer to after var_indexer is built.
                    // Will be expanded with a single global z per group, and the
                    // per-group Big-M is computed there from each group's SUMMED
                    // range (a single per-row bound would be far too small).
                    // The per-group integer-RHS check (for AVG <> where the
                    // rescaled K*N_g may or may not be integer) lives in the
                    // deferred expansion below; we don't filter here.
                    DeferredAggregateNE deferred;
                    deferred.original = ec; // copy before the loop moves on
                    deferred_ne_aggregate.push_back(std::move(deferred));
                    // Don't add to new_constraints — handled via global_constraints later
                } else {
                    // Tight data-driven per-row Big-M for the inline NE expansion.
                    double M = DecideTightPerRowBigM(ec, solver_input.lower_bounds,
                                                     solver_input.upper_bounds, num_rows);
                    // Per-row NE: expand inline with row-scoped indicator variable.
                    //
                    // Integer-valued RHS guard. If RHS is uniform and non-integer,
                    // every row's `LHS <> K` is a tautology — drop the whole
                    // constraint. If RHS varies per row (e.g. correlated subquery),
                    // mask out only the non-integer rows by adding them to
                    // row_group_ids as INVALID_INDEX so the model builder skips
                    // them. The remaining rows still get the real Big-M pair.
                    if (ec.rhs_values.IsUniform()) {
                        if (!NEIsIntegerValuedRhs(ec.rhs_values.UniformValue())) {
                            continue; // drop ec entirely (tautology)
                        }
                    } else {
                        // Build/extend a mask. row_group_ids may be empty (no WHEN/PER);
                        // in that case materialize one initialised to group 0 so we can
                        // exclude individual rows by setting INVALID_INDEX.
                        if (ec.row_group_ids.empty()) {
                            ec.row_group_ids.assign(num_rows, 0);
                            ec.num_groups = 1;
                        }
                        idx_t dropped = 0;
                        for (idx_t r = 0; r < num_rows; r++) {
                            if (ec.row_group_ids[r] == DConstants::INVALID_INDEX) continue;
                            if (!NEIsIntegerValuedRhs(ec.rhs_values.Get(r))) {
                                ec.row_group_ids[r] = DConstants::INVALID_INDEX;
                                dropped++;
                            }
                        }
                        if (dropped == num_rows) {
                            continue; // every row is a tautology — drop the constraint
                        }
                    }
                    idx_t indicator_var_idx = ec.ne_indicator_idx;

                    // Build indicator coefficient column. If no WHEN/PER filter, every
                    // row gets -M (broadcast scalar). Otherwise, only the active rows
                    // hold -M and the rest are 0 — store as SparseMasked instead of
                    // Dense to skip the per-excluded-row 0 allocation. ec.row_group_ids
                    // is iterated in row order, so the resulting sparse_indices list is
                    // already sorted ascending (the SparseMasked invariant).
                    CoefficientColumn indicator_coeffs;
                    if (ec.row_group_ids.empty()) {
                        indicator_coeffs = CoefficientColumn::MakeScalar(-M, num_rows);
                    } else {
                        vector<idx_t> active_indices;
                        active_indices.reserve(num_rows / 8);
                        for (idx_t r = 0; r < num_rows; r++) {
                            if (ec.row_group_ids[r] != DConstants::INVALID_INDEX) {
                                active_indices.push_back(r);
                            }
                        }
                        indicator_coeffs = CoefficientColumn::MakeSparseMasked(
                            num_rows, std::move(active_indices), -M);
                    }

                    auto BuildShiftedRhs = [&](double shift) {
                        if (ec.rhs_values.IsUniform()) {
                            return CoefficientColumn::MakeScalar(ec.rhs_values.UniformValue() + shift, num_rows);
                        }
                        auto col = CoefficientColumn::MakeDense(num_rows, 0.0);
                        for (idx_t r = 0; r < num_rows; r++) {
                            col.Set(r, ec.rhs_values.Get(r) + shift);
                        }
                        return col;
                    };

                    // Constraint 1: x - M*z ≤ K - 1
                    EvaluatedConstraint ec1;
                    ec1.variable_indices = ec.variable_indices;
                    ec1.row_coefficients = ec.row_coefficients;
                    ec1.variable_indices.push_back(indicator_var_idx);
                    ec1.row_coefficients.push_back(indicator_coeffs);
                    ec1.rhs_values = BuildShiftedRhs(-1.0);
                    ec1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
                    ec1.lhs_is_aggregate = false; // per-row
                    ec1.row_group_ids = ec.row_group_ids;
                    ec1.num_groups = ec.num_groups;
                    ec1.group_labels = ec.group_labels;
                    ec1.qualifier = ec.qualifier;
                    ec1.kind = ConstraintKind::USER_MECHANISM;
                    // I4: tag this disjunction row with its indicator so the elastic
                    // engine can group the pair and offer removal (remove-only `<>`).
                    ec1.ne_indicator_idx = indicator_var_idx;
                    new_constraints.push_back(std::move(ec1));

                    // Constraint 2: x - M*z ≥ K + 1 - M
                    EvaluatedConstraint ec2;
                    ec2.variable_indices = ec.variable_indices;
                    ec2.row_coefficients = ec.row_coefficients;
                    ec2.variable_indices.push_back(indicator_var_idx);
                    ec2.row_coefficients.push_back(std::move(indicator_coeffs));
                    ec2.rhs_values = BuildShiftedRhs(1.0 - M);
                    ec2.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
                    ec2.lhs_is_aggregate = false; // per-row
                    ec2.row_group_ids = ec.row_group_ids;
                    ec2.num_groups = ec.num_groups;
                    ec2.group_labels = ec.group_labels;
                    ec2.qualifier = ec.qualifier;
                    ec2.kind = ConstraintKind::USER_MECHANISM;
                    // I4: same indicator as ec1 — both rows form one removable `<>`.
                    ec2.ne_indicator_idx = indicator_var_idx;
                    new_constraints.push_back(std::move(ec2));
                }
            } else {
                new_constraints.push_back(std::move(ec));
            }
        }
        gstate.evaluated_constraints = std::move(new_constraints);
    }

    // Generate the McCormick constraints for bilinear auxiliary variables
    // (w = b * x) where b is Boolean and x ∈ [L, U]. The exact linearization is:
    //   w <= U*b                  (ec1)
    //   w >= x - U*(1-b)          (ec2)
    //   w <= x - L*(1-b)          (ec3, upper corner)
    //   w >= L*b                  (ec4, lower corner)
    // For L >= 0 the lower corner is implied by w's own non-negative bound, and
    // ec3 simplifies to the plain structural `w <= x` (w=0 at b=0 is enforced by
    // ec1). We emit exactly those two-plus-one constraints in that case, identical
    // to the prior behavior — the optimizer no longer emits the structural `w <= x`
    // (it lives here now). For L < 0 we emit the full four corners and widen w's
    // own lower bound so the product can take the negative value of x when b=1.
    for (auto &link : bilinear_links) {
        double U = solver_input.upper_bounds[link.other_var_idx];
        double L = solver_input.lower_bounds[link.other_var_idx];
        if (U >= 1e20) {
            throw InvalidInputException(
                "Bilinear term requires a finite upper bound on variable '%s'. "
                "Add a constraint like '%s <= <bound>' to provide one.",
                decide_variables[link.other_var_idx]->Cast<BoundColumnRefExpression>().alias,
                decide_variables[link.other_var_idx]->Cast<BoundColumnRefExpression>().alias);
        }

        // ec1: w <= U * b  (i.e., w - U*b <= 0)
        EvaluatedConstraint ec1;
        ec1.variable_indices = {link.aux_idx, link.bool_var_idx};
        ec1.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
        ec1.row_coefficients.push_back(CoefficientColumn::MakeScalar(-U, num_rows));
        ec1.rhs_values.AssignScalar(num_rows, 0.0);
        ec1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec1.lhs_is_aggregate = false;
        ec1.kind = ConstraintKind::STRUCTURAL;
        gstate.evaluated_constraints.push_back(std::move(ec1));

        // ec2: w >= x - U*(1-b) = x - U + U*b
        // Rearranged: w - x + U*b >= -U  →  1*w + (-1)*x + (-U)*b >= -U
        EvaluatedConstraint ec2;
        ec2.variable_indices = {link.aux_idx, link.other_var_idx, link.bool_var_idx};
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(-1.0, num_rows));  // -x
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(-U, num_rows));    // -U*b
        ec2.rhs_values.AssignScalar(num_rows, -U);
        ec2.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
        ec2.lhs_is_aggregate = false;
        ec2.kind = ConstraintKind::STRUCTURAL;
        gstate.evaluated_constraints.push_back(std::move(ec2));

        // ec3: upper corner. L >= 0 → plain `w <= x`; L < 0 → `w <= x - L*(1-b)`,
        // i.e. w - x - L*b <= -L.
        EvaluatedConstraint ec3;
        if (L < 0.0) {
            ec3.variable_indices = {link.aux_idx, link.other_var_idx, link.bool_var_idx};
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(-1.0, num_rows));  // -x
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(-L, num_rows));    // -L*b
            ec3.rhs_values.AssignScalar(num_rows, -L);
        } else {
            ec3.variable_indices = {link.aux_idx, link.other_var_idx};
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
            ec3.row_coefficients.push_back(CoefficientColumn::MakeScalar(-1.0, num_rows));  // -x
            ec3.rhs_values.AssignScalar(num_rows, 0.0);
        }
        ec3.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec3.lhs_is_aggregate = false;
        ec3.kind = ConstraintKind::STRUCTURAL;
        gstate.evaluated_constraints.push_back(std::move(ec3));

        // ec4: lower corner `w >= L*b`, only needed when x can be negative.
        // Also widen the aux's own lower bound so w may equal the negative x at b=1.
        if (L < 0.0) {
            solver_input.lower_bounds[link.aux_idx] =
                std::min(solver_input.lower_bounds[link.aux_idx], L);
            EvaluatedConstraint ec4;
            ec4.variable_indices = {link.aux_idx, link.bool_var_idx};
            ec4.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
            ec4.row_coefficients.push_back(CoefficientColumn::MakeScalar(-L, num_rows));    // -L*b
            ec4.rhs_values.AssignScalar(num_rows, 0.0);
            ec4.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
            ec4.lhs_is_aggregate = false;
            ec4.kind = ConstraintKind::STRUCTURAL;
            gstate.evaluated_constraints.push_back(std::move(ec4));
        }
    }

    // Generate Big-M upper-bound constraints for MAXIMIZE + ABS auxiliary variables.
    // For each AbsMaximizeLink, find the two tagged lower-bound EvaluatedConstraints
    // (C1: aux >= inner tagged ABS_UB_POS, C2: aux >= -inner tagged ABS_UB_NEG) and emit:
    //   C_ub1: derived from C1, add y with coeff +2M, comparison <=, rhs[r] += 2M
    //   C_ub2: derived from C2, add y with coeff -2M, comparison <=, rhs unchanged
    // Together with C1/C2 these force aux = |inner| under MAXIMIZE.
    if (!abs_maximize_links.empty()) {
        struct AbsConstraintPair {
            idx_t c1 = DConstants::INVALID_INDEX;
            idx_t c2 = DConstants::INVALID_INDEX;
        };
        unordered_map<idx_t, AbsConstraintPair> abs_tag_map;
        for (idx_t ci = 0; ci < gstate.evaluated_constraints.size(); ci++) {
            auto &ec = gstate.evaluated_constraints[ci];
            if (ec.abs_y_idx == DConstants::INVALID_INDEX) {
                continue;
            }
            if (ec.abs_is_pos_bound) {
                abs_tag_map[ec.abs_y_idx].c1 = ci;
            } else {
                abs_tag_map[ec.abs_y_idx].c2 = ci;
            }
        }

        // Reserve up-front so the two push_back calls per link cannot reallocate
        // gstate.evaluated_constraints. With capacity guaranteed, references to
        // existing C1/C2 stay valid across appends and we don't need defensive
        // copies of their fields.
        gstate.evaluated_constraints.reserve(
            gstate.evaluated_constraints.size() + 2 * abs_maximize_links.size());

        for (auto &link : abs_maximize_links) {
            auto it = abs_tag_map.find(link.y_idx);
            D_ASSERT(it != abs_tag_map.end() &&
                     it->second.c1 != DConstants::INVALID_INDEX &&
                     it->second.c2 != DConstants::INVALID_INDEX);

            const auto &c1 = gstate.evaluated_constraints[it->second.c1];
            const auto &c2 = gstate.evaluated_constraints[it->second.c2];

            // Compute M = max over rows of |rhs[r]| + sum_{t: var != aux} |coeff[t][r]| * max(|lb|, |ub|),
            // reusing the shared per-row range helper (skipping the aux term). This
            // upper-bounds |inner| across all rows and variable values. Unlike the
            // indicator sites, ABS-maximize is STRICT: if no finite bound can be
            // derived for a contributing variable, there is no valid M, so we throw
            // rather than fall back. (Implied-bound propagation may have already
            // supplied a bound, in which case this succeeds.)
            bool abs_unbounded = false;
            double M = 0.0;
            for (idx_t r = 0; r < num_rows; r++) {
                double row_bound = std::abs(c1.rhs_values.Get(r)) +
                                   DecideRowTermRange(c1.variable_indices, c1.row_coefficients, r,
                                                      solver_input.lower_bounds, solver_input.upper_bounds,
                                                      abs_unbounded, link.aux_idx);
                M = std::max(M, row_bound);
            }
            if (abs_unbounded) {
                // Locate an offending variable to name in the error.
                idx_t bad = DConstants::INVALID_INDEX;
                for (idx_t t = 0; t < c1.variable_indices.size(); t++) {
                    idx_t v = c1.variable_indices[t];
                    if (v == DConstants::INVALID_INDEX || v == link.aux_idx) continue;
                    if (solver_input.upper_bounds[v] >= 1e20 || solver_input.lower_bounds[v] <= -1e20) {
                        bad = v;
                        break;
                    }
                }
                const char *name = decide_variables[bad]->Cast<BoundColumnRefExpression>().alias.c_str();
                throw InvalidInputException(
                    "ABS over decision variable requires a finite bound on '%s' "
                    "for the Big-M sign-indicator linearization. Add constraints "
                    "'%s >= <lower>' and '%s <= <upper>'. (Triggered by "
                    "MAXIMIZE SUM(ABS(...)) or by a hard-direction ABS constraint "
                    "such as ABS(...) >= K or ABS(...) = K.)",
                    name, name, name);
            }
            double two_M = 2.0 * M;

            auto ShiftRhs = [&](const CoefficientColumn &src, double delta) {
                if (src.IsUniform()) {
                    return CoefficientColumn::MakeScalar(src.UniformValue() + delta, num_rows);
                }
                auto out = CoefficientColumn::MakeDense(num_rows, 0.0);
                for (idx_t r = 0; r < num_rows; r++) {
                    out.Set(r, src.Get(r) + delta);
                }
                return out;
            };

            // C_ub1: same as C1 but add y_idx with coeff +2M, flip to <=, rhs[r] += 2M
            EvaluatedConstraint ec_ub1;
            ec_ub1.variable_indices = c1.variable_indices;
            ec_ub1.row_coefficients = c1.row_coefficients;
            ec_ub1.variable_indices.push_back(link.y_idx);
            ec_ub1.row_coefficients.push_back(CoefficientColumn::MakeScalar(two_M, num_rows));
            ec_ub1.rhs_values = ShiftRhs(c1.rhs_values, two_M);
            ec_ub1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
            ec_ub1.lhs_is_aggregate = false;
            ec_ub1.row_group_ids = c1.row_group_ids;
            ec_ub1.num_groups = c1.num_groups;
            ec_ub1.group_labels = c1.group_labels;
            ec_ub1.qualifier = c1.qualifier;
            ec_ub1.kind = ConstraintKind::STRUCTURAL;
            gstate.evaluated_constraints.push_back(std::move(ec_ub1));

            // C_ub2: same as C2 but add y_idx with coeff -2M, flip to <=, rhs unchanged
            EvaluatedConstraint ec_ub2;
            ec_ub2.variable_indices = c2.variable_indices;
            ec_ub2.row_coefficients = c2.row_coefficients;
            ec_ub2.variable_indices.push_back(link.y_idx);
            ec_ub2.row_coefficients.push_back(CoefficientColumn::MakeScalar(-two_M, num_rows));
            ec_ub2.rhs_values = c2.rhs_values;
            ec_ub2.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
            ec_ub2.lhs_is_aggregate = false;
            ec_ub2.row_group_ids = c2.row_group_ids;
            ec_ub2.num_groups = c2.num_groups;
            ec_ub2.group_labels = c2.group_labels;
            ec_ub2.qualifier = c2.qualifier;
            ec_ub2.kind = ConstraintKind::STRUCTURAL;
            gstate.evaluated_constraints.push_back(std::move(ec_ub2));
        }
    }

    // Constraints
    solver_input.constraints = std::move(gstate.evaluated_constraints);

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

    auto ScaleObjectiveAvgRows = [&](CoefficientColumn &col, bool has_filter, const vector<bool> &filter_mask,
                                     bool quadratic_inner) {
        auto &coefficients = col.MutableDense();
        if (solver_input.objective_row_group_ids.empty()) {
            idx_t denominator = 0;
            for (idx_t row = 0; row < num_rows; row++) {
                if (objective_has_when && !objective_when_mask[row]) {
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

        vector<idx_t> group_counts(solver_input.objective_num_groups, 0);
        for (idx_t row = 0; row < num_rows; row++) {
            idx_t gid = solver_input.objective_row_group_ids[row];
            if (gid == DConstants::INVALID_INDEX) {
                continue;
            }
            if (!has_filter || filter_mask[row]) {
                group_counts[gid]++;
            }
        }
        for (idx_t row = 0; row < coefficients.size(); row++) {
            idx_t gid = solver_input.objective_row_group_ids[row];
            if (gid == DConstants::INVALID_INDEX || group_counts[gid] == 0) {
                coefficients[row] = 0.0;
                continue;
            }
            double scale = quadratic_inner ? 1.0 / std::sqrt(static_cast<double>(group_counts[gid]))
                                           : 1.0 / static_cast<double>(group_counts[gid]);
            coefficients[row] *= scale;
        }
    };

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
                                   solver_input.objective_row_group_ids,
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
                                   solver_input.objective_row_group_ids,
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
                                   solver_input.objective_row_group_ids,
                                   obj_bilinear_filters[term_idx]);
            MaskCoefficientColumn(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
                                  obj_bilinear_filters[term_idx].mask);
        }
    }

    for (idx_t term_idx = 0; term_idx < obj_linear_term_filters.size(); term_idx++) {
        if (!obj_linear_term_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleObjectiveAvgRows(solver_input.objective_coefficients[term_idx],
                              obj_linear_term_filters[term_idx].has_filter,
                              obj_linear_term_filters[term_idx].mask, false);
    }
    for (idx_t term_idx = 0; term_idx < obj_quadratic_term_filters.size(); term_idx++) {
        if (!obj_quadratic_term_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleObjectiveAvgRows(solver_input.quadratic_inner_coefficients[term_idx],
                              obj_quadratic_term_filters[term_idx].has_filter,
                              obj_quadratic_term_filters[term_idx].mask, true);
    }

    for (idx_t term_idx = 0; term_idx < obj_bilinear_filters.size(); term_idx++) {
        if (!obj_bilinear_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleObjectiveAvgRows(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
                              obj_bilinear_filters[term_idx].has_filter,
                              obj_bilinear_filters[term_idx].mask, false);
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

    // Build the VarIndexer once and reuse it for: (1) computing absolute variable
    // indices during deferred-NE / MIN/MAX objective construction below, (2) the
    // SolverModel::Build() call inside SolveModel(), and (3) gstate.var_indexer
    // for solution readback after the solve. Use the owning form so it survives
    // past `solver_input` once moved onto gstate.
    //
    // The row+entity portions of the index never change after this point;
    // however `solver_input.num_global_vars` keeps growing as auxiliary globals
    // are added below. We refresh `total_vars` just before SolveModel is called.
    VarIndexer var_indexer = VarIndexer::Build(solver_input);

    // Global variables are appended at var_indexer.global_block_start.
    // As we add more global vars, their indices are global_block_start + g
    // where g is the position in the global vars array.

    // Expand deferred aggregate NE constraints using global binary indicator variables.
    // Each group gets a single z (binary), yielding two raw constraints:
    //   SUM(coeffs) - M*z <= K-1       (z=0 → SUM ≤ K-1; z=1 → trivially true)
    //   SUM(coeffs) - M*z >= K+1-M     (z=0 → trivially true; z=1 → SUM ≥ K+1)
    //
    // Reusable scratch for per-group LHS accumulation (replaces a per-group
    // unordered_map<int,double>). The decide-variable flat indices are bounded
    // by var_indexer.global_block_start, so we size the dense accumulator to
    // that — tighter than total_vars and unaffected by the new globals appended
    // below as the loop runs.
    SparseCoeffAccumulator accum;
    {
        constexpr idx_t DENSE_CAP = 1u << 20;
        idx_t decide_var_index_span = var_indexer.global_block_start;
        if (decide_var_index_span <= DENSE_CAP) {
            accum.BeginDense(decide_var_index_span);
        } else {
            accum.BeginSparse(num_rows); // hint; per-group merging keeps actual size small
        }
    }

    for (auto &deferred : deferred_ne_aggregate) {
        auto &ec = deferred.original;
        bool has_groups = !ec.row_group_ids.empty();

        // I4 (aggregate `<>`): clause text used to name a dropped aggregate `<>`.
        // The optimizer recorded "(SUM(x) <> K)" in aux_var_expressions keyed by
        // the indicator decide-var; carry it onto every global z this `ec`
        // allocates so the infeasible removal dial can label the DROP edit.
        string ne_label;
        for (auto &ae : aux_var_expressions) {
            if (ae.first == ec.ne_indicator_idx) {
                ne_label = ae.second;
                break;
            }
        }

        // Build group → rows mapping. For grouped constraints reuse the CSR
        // index already attached to ec; for ungrouped, materialize the trivial
        // single-group CSR locally.
        idx_t num_groups_to_process = 1;
        vector<idx_t> ungrouped_offsets;
        vector<idx_t> ungrouped_flat;
        const vector<idx_t> *offsets_ptr = nullptr;
        const vector<idx_t> *flat_ptr = nullptr;
        if (has_groups) {
            BuildGroupCSR(ec.row_group_ids, ec.num_groups,
                          ec.group_offsets, ec.group_row_ids);
            num_groups_to_process = ec.num_groups;
            offsets_ptr = &ec.group_offsets;
            flat_ptr = &ec.group_row_ids;
        } else {
            ungrouped_offsets = {0, num_rows};
            ungrouped_flat.resize(num_rows);
            for (idx_t r = 0; r < num_rows; r++) ungrouped_flat[r] = r;
            offsets_ptr = &ungrouped_offsets;
            flat_ptr = &ungrouped_flat;
        }
        const auto &offsets = *offsets_ptr;
        const auto &flat_rows = *flat_ptr;

        for (idx_t g = 0; g < num_groups_to_process; g++) {
            idx_t g_begin = offsets[g];
            idx_t g_end = offsets[g + 1];
            if (g_begin == g_end) {
                continue;
            }
            idx_t g_size = g_end - g_begin;

            // Base (unscaled) RHS, read from a row that actually belongs to this group.
            // For AVG(x) <> K we store the original K in rhs_values and multiply by the
            // group size below. Reading row 0 once for every group predates the RHS
            // carrying a per-group value; it also silently used a WHEN-excluded row.
            double rhs = ec.rhs_values.Get(flat_rows[g_begin]);
            if (ec.ne_avg_rhs_scale) {
                rhs *= static_cast<double>(g_size);
            }
            double fixed_offset = 0.0;
            for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
                if (ec.variable_indices[term_idx] != DConstants::INVALID_INDEX) {
                    continue;
                }
                auto &col = ec.row_coefficients[term_idx];
                for (idx_t k = g_begin; k < g_end; k++) {
                    fixed_offset += col.Get(flat_rows[k]);
                }
            }
            rhs -= fixed_offset;

            // Integer-RHS guard: with integer LHS (already enforced by
            // NELhsIsIntegerValued at deferral time) and a non-integer K,
            // `LHS <> K` is a tautology — every integer LHS satisfies it.
            // The ±1 Big-M rewrite would wrongly cut floor(K) and ceil(K).
            // Skip the group entirely. For AVG <> with mixed group sizes,
            // some groups may have integer K*N_g and others not — each is
            // handled independently. No global z is allocated for skipped
            // groups, so the model stays clean.
            if (std::abs(rhs - std::round(rhs)) >= 1e-9) {
                continue;
            }

            // Tight per-group Big-M: the aggregate LHS ranges over the SUM of
            // this group's rows, so M must cover the summed magnitude. A single
            // per-row bound (the legacy behavior) is far too small at scale and
            // would silently cap the aggregate.
            bool grp_unbounded = false;
            double grp_range = 0.0;
            for (idx_t k = g_begin; k < g_end; k++) {
                grp_range += DecideRowTermRange(ec.variable_indices, ec.row_coefficients,
                                                flat_rows[k], solver_input.lower_bounds,
                                                solver_input.upper_bounds, grp_unbounded);
            }
            double M = grp_range + std::abs(rhs) + 1.0;
            if (grp_unbounded) {
                M = std::max(M, DECIDE_BIGM_FALLBACK);
            }

            // Allocate one global binary z for this group
            idx_t z_idx = var_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars++;
            solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
            solver_input.global_lower_bounds.push_back(0.0);
            solver_input.global_upper_bounds.push_back(1.0);
            solver_input.global_obj_coeffs.push_back(0.0);
            solver_input.global_variable_labels.push_back(ne_label);

            // Accumulate LHS coefficients for active rows in this group.
            for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
                idx_t decide_var_idx = ec.variable_indices[term_idx];
                if (decide_var_idx == DConstants::INVALID_INDEX) {
                    continue;
                }
                auto &col = ec.row_coefficients[term_idx];
                for (idx_t k = g_begin; k < g_end; k++) {
                    idx_t row = flat_rows[k];
                    double coeff = col.Get(row);
                    if (std::abs(coeff) < 1e-15) {
                        continue;
                    }
                    int var_idx = static_cast<int>(var_indexer.Get(decide_var_idx, row));
                    accum.Add(var_idx, coeff);
                }
            }

            // Flush once into a deduped (idx, coeff) snapshot reused for both rc1 and rc2.
            vector<int> common_indices;
            vector<double> common_coefs;
            accum.Flush(common_indices, common_coefs);

            // ec1: SUM(coeffs) - M*z <= K - 1
            SolverInput::RawConstraint rc1;
            rc1.sense = '<';
            rc1.rhs = rhs - 1.0;
            rc1.indices = common_indices;
            rc1.coefficients = common_coefs;
            rc1.indices.push_back(static_cast<int>(z_idx));
            rc1.coefficients.push_back(-M);
            rc1.kind = ConstraintKind::USER_MECHANISM;
            rc1.indicator_col = z_idx;
            solver_input.global_constraints.push_back(std::move(rc1));

            // ec2: SUM(coeffs) - M*z >= K + 1 - M
            SolverInput::RawConstraint rc2;
            rc2.sense = '>';
            rc2.rhs = rhs + 1.0 - M;
            rc2.indices = std::move(common_indices);
            rc2.coefficients = std::move(common_coefs);
            rc2.indices.push_back(static_cast<int>(z_idx));
            rc2.coefficients.push_back(-M);
            rc2.kind = ConstraintKind::USER_MECHANISM;
            rc2.indicator_col = z_idx;
            solver_input.global_constraints.push_back(std::move(rc2));
        }
    }

    // Save objective data (needed for constraint generation in the PER MIN/MAX
    // and flat aggregate paths). Defer the deep copy of objective_coefficients
    // — which is a vector<vector<double>> sized num_terms * num_rows — until
    // we know we'll take one of those paths.
    auto saved_obj_var_indices = solver_input.objective_variable_indices;
    bool need_saved_obj =
        !saved_obj_var_indices.empty() &&
        ((per_inner_agg != ObjectiveAggregateType::NONE && solver_input.objective_num_groups > 0) ||
         flat_objective_agg != ObjectiveAggregateType::NONE);
    vector<CoefficientColumn> saved_obj_coefficients;
    if (need_saved_obj) {
        saved_obj_coefficients = solver_input.objective_coefficients;
    }

    // Big-M for the MIN/MAX objective auxiliaries. Unlike the per-row constraint
    // sites (where M bounds an expression against a fixed RHS), these link an
    // auxiliary variable z/z_g/w to the objective expression via
    // (aux - expr) +/- M*y (>=|<=) +/- M. The deactivated branch must stay slack
    // across the GLOBAL spread of (aux - expr): with the aux pinned inside the
    // expression's reachable range, that worst case is
    //   max_r exprmax_r  -  min_r exprmin_r
    // where each row's exprmax/exprmin take the SIGN of every coefficient against
    // the variable's [lb, ub]. This is the tight, data-driven value (the per-row
    // range used elsewhere can under-estimate it when coefficient signs differ
    // across rows). Falls back to the 1e6 floor only when a variable is unbounded.
    auto compute_big_m = [&]() -> double {
        bool unbounded = false;
        double global_max = 0.0; // x = lb (>= 0) is always reachable, giving expr-contribution 0
        double global_min = 0.0;
        for (idx_t r = 0; r < num_rows; r++) {
            double row_max = 0.0;
            double row_min = 0.0;
            for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                idx_t v = saved_obj_var_indices[t];
                if (v == DConstants::INVALID_INDEX) {
                    continue;
                }
                double c = saved_obj_coefficients[t].Get(r);
                if (std::abs(c) < 1e-15) {
                    continue;
                }
                double lb = solver_input.lower_bounds[v];
                double ub = solver_input.upper_bounds[v];
                if (ub >= 1e20 || lb <= -1e20) {
                    unbounded = true;
                    continue;
                }
                if (c > 0.0) {
                    row_max += c * ub;
                    row_min += c * lb;
                } else {
                    row_max += c * lb;
                    row_min += c * ub;
                }
            }
            global_max = std::max(global_max, row_max);
            global_min = std::min(global_min, row_min);
        }
        double M = global_max - global_min;
        if (unbounded) {
            M = std::max(M, DECIDE_BIGM_FALLBACK);
        }
        return M;
    };

    // Accumulator for a MIN/MAX linking row (`z - expr op bound`).
    //
    // Term arrays are indexed by term, not by variable, so the same solver column
    // reaches one row more than once in two situations: `(c + 1) * x` distributes
    // into `c*x + 1*x`, and an entity-scoped or scalar variable resolves to a
    // single column across every row it spans. A repeated column index is rejected
    // outright by both Gurobi and HiGHS, so coefficients are summed per column here
    // rather than pushed per term. Constant terms carry no column at all
    // (`variable_index == INVALID_INDEX`) and collect into `constant`, which the
    // caller folds into the bound: `z <= expr + k` is `z - expr <= k`.
    //
    // Columns keep first-appearance order — emitting straight from the hash map
    // would hand the solver a different matrix ordering run to run.
    struct MinMaxLinkRow {
        vector<int> indices;
        vector<double> coefficients;
        double constant = 0.0;
        std::unordered_map<int, idx_t> column_slot;

        void AddColumn(int column, double coefficient) {
            auto entry = column_slot.find(column);
            if (entry == column_slot.end()) {
                column_slot.emplace(column, indices.size());
                indices.push_back(column);
                coefficients.push_back(coefficient);
            } else {
                coefficients[entry->second] += coefficient;
            }
        }

        //! True when no column survives accumulation — either nothing was added, or
        //! every column's terms cancelled (`c*x - c*x`). Such a row constrains the
        //! auxiliary against `constant` alone.
        bool HasNoColumns() const {
            for (auto coefficient : coefficients) {
                if (std::abs(coefficient) >= 1e-15) {
                    return false;
                }
            }
            return true;
        }

        void AppendTo(SolverInput::RawConstraint &rc) const {
            for (idx_t i = 0; i < indices.size(); i++) {
                if (std::abs(coefficients[i]) < 1e-15) {
                    continue;
                }
                rc.indices.push_back(indices[i]);
                rc.coefficients.push_back(coefficients[i]);
            }
        }
    };

    // Accumulate one row of the saved objective expression into `link`, negated:
    // the linking row is `z - expr op bound`, so the expression's coefficients
    // enter with the opposite sign and its constant part lands on the bound.
    // `scale` carries the inner-AVG 1/n_g factor at the PER sites; 1.0 elsewhere.
    auto AddObjectiveRowTerms = [&](MinMaxLinkRow &link, idx_t row, double scale) {
        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
            double coeff = saved_obj_coefficients[t].Get(row) * scale;
            if (std::abs(coeff) < 1e-15) {
                continue;
            }
            idx_t v = saved_obj_var_indices[t];
            if (v == DConstants::INVALID_INDEX) {
                link.constant += coeff;
            } else {
                link.AddColumn((int)var_indexer.Get(v, row), -coeff);
            }
        }
    };

    // Same accumulation for the composed paths, whose terms arrive as a
    // `Term` list plus per-term evaluated coefficient columns.
    auto AddComposedRowTerms = [&](MinMaxLinkRow &link, const vector<Term> &inner_terms,
                                   const vector<vector<double>> &per_term_coefs, idx_t row) {
        for (idx_t it = 0; it < inner_terms.size(); it++) {
            double coeff = per_term_coefs[it][row];
            idx_t v = inner_terms[it].variable_index;
            if (v == DConstants::INVALID_INDEX) {
                link.constant += coeff;
            } else {
                link.AddColumn((int)var_indexer.Get(v, row), -coeff);
            }
        }
    };

    if (per_inner_agg != ObjectiveAggregateType::NONE && !saved_obj_var_indices.empty() &&
        solver_input.objective_num_groups > 0) {
        // ================================================================
        // PATH B: PER objective with nested OUTER(INNER(expr)) aggregate
        // ================================================================
        idx_t K = solver_input.objective_num_groups;
        auto &row_groups = solver_input.objective_row_group_ids;

        // Build group→rows CSR index once, reuse across phases.
        BuildGroupCSR(row_groups, K,
                      solver_input.objective_group_offsets,
                      solver_input.objective_group_row_ids);
        auto &obj_offsets = solver_input.objective_group_offsets;
        auto &obj_flat_rows = solver_input.objective_group_row_ids;
        auto group_size = [&](idx_t g) {
            return obj_offsets[g + 1] - obj_offsets[g];
        };

        // Clear per-row objective (auxiliaries become the objective)
        solver_input.objective_coefficients.clear();
        solver_input.objective_variable_indices.clear();

        // Phase A: Inner aggregate — produces K per-group values
        // These are either group sums (no aux) or z_g auxiliaries (inner MIN/MAX)
        bool inner_is_minmax = (per_inner_agg == ObjectiveAggregateType::MIN_AGG || per_inner_agg == ObjectiveAggregateType::MAX_AGG);
        bool inner_is_min = (per_inner_agg == ObjectiveAggregateType::MIN_AGG);

        // group_value_indices[g] = solver variable index for group g's value
        // For inner SUM: not used (group sums go directly to outer as coefficients)
        // For inner MIN/MAX: index of z_g global variable
        vector<idx_t> group_value_indices(K);

        if (inner_is_minmax) {
            // Inner MIN/MAX: create z_g auxiliary per group
            bool inner_easy = per_inner_is_easy;
            double M = compute_big_m();

            idx_t z_base = var_indexer.global_block_start + solver_input.num_global_vars;
            for (idx_t g = 0; g < K; g++) {
                group_value_indices[g] = z_base + g;
                solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
                solver_input.global_lower_bounds.push_back(-1e30);
                solver_input.global_upper_bounds.push_back(1e30);
                solver_input.global_obj_coeffs.push_back(0.0); // set by outer phase
            }
            solver_input.num_global_vars += K;

            // Build a per-group active-rows CSR: drop rows whose every term coefficient
            // is zero. Mirrors PATH A's active_rows pre-filter (lines below). Without it
            // the easy path emits vacuous z_g op 0 rows and the hard path allocates an
            // indicator binary plus a Big-M row for each, then references them in the
            // sum_y >= 1 constraint — all wasted on rows that contribute nothing.
            vector<idx_t> active_offsets(K + 1, 0);
            vector<idx_t> active_flat_rows;
            active_flat_rows.reserve(obj_flat_rows.size());
            for (idx_t g = 0; g < K; g++) {
                active_offsets[g] = active_flat_rows.size();
                for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                    idx_t row = obj_flat_rows[k];
                    bool has_nonzero = false;
                    for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                        if (std::abs(saved_obj_coefficients[t][row]) >= 1e-15) {
                            has_nonzero = true;
                            break;
                        }
                    }
                    if (has_nonzero) active_flat_rows.push_back(row);
                }
            }
            active_offsets[K] = active_flat_rows.size();

            // For groups with no active rows, the original code emitted vacuous
            // z_g op 0 rows that — combined with the outer optimization direction —
            // implicitly pinned z_g at 0. Skipping those rows lets z_g float free,
            // so we instead pin z_g's bounds directly. Captured as a lambda so both
            // easy and hard branches use identical pinning logic.
            auto PinZGroupToZero = [&](idx_t g) {
                idx_t z_local = group_value_indices[g] - var_indexer.global_block_start;
                solver_input.global_lower_bounds[z_local] = 0.0;
                solver_input.global_upper_bounds[z_local] = 0.0;
            };

            if (inner_easy) {
                // Easy: z_g >= expr_r (for MAX) or z_g <= expr_r (for MIN)
                char sense_char = inner_is_min ? '<' : '>';
                for (idx_t g = 0; g < K; g++) {
                    if (active_offsets[g] == active_offsets[g + 1]) {
                        PinZGroupToZero(g);
                        continue;
                    }
                    for (idx_t k = active_offsets[g]; k < active_offsets[g + 1]; k++) {
                        idx_t row = active_flat_rows[k];
                        MinMaxLinkRow link;
                        AddObjectiveRowTerms(link, row, 1.0);
                        SolverInput::RawConstraint rc;
                        rc.sense = sense_char;
                        rc.rhs = link.constant;
                        rc.indices.push_back((int)group_value_indices[g]);
                        rc.coefficients.push_back(1.0);
                        link.AppendTo(rc);
                        solver_input.global_constraints.push_back(std::move(rc));
                    }
                }
            } else {
                // Hard: per-row indicators per group, allocated only for active rows.
                idx_t first_y = z_base + K;
                idx_t num_active = active_flat_rows.size();
                solver_input.num_global_vars += num_active;
                for (idx_t r = 0; r < num_active; r++) {
                    solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                    solver_input.global_lower_bounds.push_back(0.0);
                    solver_input.global_upper_bounds.push_back(1.0);
                    solver_input.global_obj_coeffs.push_back(0.0);
                }

                for (idx_t g = 0; g < K; g++) {
                    if (active_offsets[g] == active_offsets[g + 1]) {
                        PinZGroupToZero(g);
                        continue;
                    }
                    for (idx_t k = active_offsets[g]; k < active_offsets[g + 1]; k++) {
                        idx_t row = active_flat_rows[k];
                        idx_t active_idx = k; // position in active_flat_rows
                        MinMaxLinkRow link;
                        AddObjectiveRowTerms(link, row, 1.0);
                        SolverInput::RawConstraint rc;
                        rc.indices.push_back((int)group_value_indices[g]);
                        rc.coefficients.push_back(1.0);
                        link.AppendTo(rc);
                        idx_t y_idx = first_y + active_idx;
                        if (inner_is_min) {
                            // MINIMIZE MIN inner: z_g - expr_r - M*y_r >= -M
                            rc.indices.push_back((int)y_idx);
                            rc.coefficients.push_back(-M);
                            rc.sense = '>';
                            rc.rhs = -M + link.constant;
                        } else {
                            // MAXIMIZE MAX inner: z_g - expr_r + M*y_r <= M
                            rc.indices.push_back((int)y_idx);
                            rc.coefficients.push_back(M);
                            rc.sense = '<';
                            rc.rhs = M + link.constant;
                        }
                        solver_input.global_constraints.push_back(std::move(rc));
                    }
                    // SUM(y) >= 1 per group
                    SolverInput::RawConstraint sum_y;
                    for (idx_t k = active_offsets[g]; k < active_offsets[g + 1]; k++) {
                        sum_y.indices.push_back((int)(first_y + k));
                        sum_y.coefficients.push_back(1.0);
                    }
                    sum_y.sense = '>';
                    sum_y.rhs = 1.0;
                    solver_input.global_constraints.push_back(std::move(sum_y));
                }
            }
        }

        // Phase B: Outer aggregate — combines K group values into scalar objective
        bool outer_is_sum = (per_outer_agg == ObjectiveAggregateType::SUM);
        bool outer_is_minmax = (per_outer_agg == ObjectiveAggregateType::MIN_AGG || per_outer_agg == ObjectiveAggregateType::MAX_AGG);
        bool outer_is_min = (per_outer_agg == ObjectiveAggregateType::MIN_AGG);

        if (inner_is_minmax && outer_is_sum) {
            // Outer SUM: objective = sum of z_g's
            for (idx_t g = 0; g < K; g++) {
                solver_input.global_obj_coeffs[group_value_indices[g] - var_indexer.global_block_start] = 1.0;
            }
        } else if (inner_is_minmax && outer_is_minmax) {
            // Outer MIN/MAX over z_g's: create global w auxiliary
            bool outer_easy = per_outer_is_easy;

            idx_t w_idx = var_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars += 1;
            solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
            solver_input.global_lower_bounds.push_back(-1e30);
            solver_input.global_upper_bounds.push_back(1e30);
            solver_input.global_obj_coeffs.push_back(1.0); // objective = w

            if (outer_easy) {
                // w >= z_g (for outer MAX) or w <= z_g (for outer MIN)
                char sense_char = outer_is_min ? '<' : '>';
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.sense = sense_char;
                    rc.rhs = 0.0;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    rc.indices.push_back((int)group_value_indices[g]);
                    rc.coefficients.push_back(-1.0);
                    solver_input.global_constraints.push_back(std::move(rc));
                }
            } else {
                // Hard outer: indicators over K groups
                idx_t first_u = w_idx + 1;
                solver_input.num_global_vars += K;
                for (idx_t g = 0; g < K; g++) {
                    solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                    solver_input.global_lower_bounds.push_back(0.0);
                    solver_input.global_upper_bounds.push_back(1.0);
                    solver_input.global_obj_coeffs.push_back(0.0);
                }
                // Outer Big-M: compute_big_m() returns the global spread of the
                // objective expression (max_r exprmax - min_r exprmin), which bounds
                // the spread of (w - z_g) since each z_g lies within that range.
                double M_outer = compute_big_m();
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    rc.indices.push_back((int)group_value_indices[g]);
                    rc.coefficients.push_back(-1.0);
                    idx_t u_idx = first_u + g;
                    if (outer_is_min) {
                        // MINIMIZE MIN outer: w - z_g - M*u_g >= -M
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(-M_outer);
                        rc.sense = '>';
                        rc.rhs = -M_outer;
                    } else {
                        // MAXIMIZE MAX outer: w - z_g + M*u_g <= M
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(M_outer);
                        rc.sense = '<';
                        rc.rhs = M_outer;
                    }
                    solver_input.global_constraints.push_back(std::move(rc));
                }
                SolverInput::RawConstraint sum_u;
                for (idx_t g = 0; g < K; g++) {
                    sum_u.indices.push_back((int)(first_u + g));
                    sum_u.coefficients.push_back(1.0);
                }
                sum_u.sense = '>';
                sum_u.rhs = 1.0;
                solver_input.global_constraints.push_back(std::move(sum_u));
            }
        } else if (!inner_is_minmax && outer_is_sum) {
            if (per_inner_was_avg) {
                // Inner AVG + Outer SUM: scale each row's coefficient by 1/n_g
                // SUM over groups of AVG(expr) = Σ_g (Σ_{r∈g} c_r * x_r) / n_g
                for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                    auto &col = saved_obj_coefficients[t].MutableDense();
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (row_groups[row] != DConstants::INVALID_INDEX) {
                            idx_t g = row_groups[row];
                            col[row] /= static_cast<double>(group_size(g));
                        }
                    }
                }
            }
            // Restore (possibly scaled) objective coefficients
            solver_input.objective_coefficients = std::move(saved_obj_coefficients);
            solver_input.objective_variable_indices = std::move(saved_obj_var_indices);
        } else if (!inner_is_minmax && outer_is_minmax) {
            // Inner SUM + Outer MIN/MAX: compute per-group sums, then optimize over them
            // Create w auxiliary for outer MIN/MAX over group sums
            bool outer_easy = per_outer_is_easy;

            idx_t w_idx = var_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars += 1;
            solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
            solver_input.global_lower_bounds.push_back(-1e30);
            solver_input.global_upper_bounds.push_back(1e30);
            solver_input.global_obj_coeffs.push_back(1.0); // objective = w

            // For each group g: w >= (or <=) sum_g(coeffs * x)
            // sum_g = Σ_{r ∈ group_g} Σ_t coeff_t_r * x_{r,var_t}
            if (outer_easy) {
                char sense_char = outer_is_min ? '<' : '>';
                bool any_group_emitted = false;
                for (idx_t g = 0; g < K; g++) {
                    double scale = per_inner_was_avg ? 1.0 / static_cast<double>(group_size(g)) : 1.0;
                    MinMaxLinkRow link;
                    for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                        AddObjectiveRowTerms(link, obj_flat_rows[k], scale);
                    }
                    // Skip vacuous w op 0 rows: outer MIN/MAX of group sums settles
                    // dominated zero-sum groups via the optimization direction itself.
                    // A group left holding only a constant still bounds w, so it stays.
                    if (link.HasNoColumns() && std::abs(link.constant) < 1e-15) continue;
                    SolverInput::RawConstraint rc;
                    rc.sense = sense_char;
                    rc.rhs = link.constant;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    link.AppendTo(rc);
                    solver_input.global_constraints.push_back(std::move(rc));
                    any_group_emitted = true;
                }
                if (!any_group_emitted) {
                    // Every group is identically zero — pin w to 0 so outer
                    // optimization doesn't push the otherwise-unconstrained w to ±∞.
                    idx_t w_local = w_idx - var_indexer.global_block_start;
                    solver_input.global_lower_bounds[w_local] = 0.0;
                    solver_input.global_upper_bounds[w_local] = 0.0;
                }
            } else {
                // Hard outer: indicators over K groups
                // Outer Big-M over group SUMS: a group sum spans at most num_rows
                // times the per-row spread, so the global spread (compute_big_m())
                // scaled by num_rows bounds the spread of (w - group_sum).
                double M_outer = compute_big_m() * num_rows;
                idx_t first_u = w_idx + 1;
                solver_input.num_global_vars += K;
                for (idx_t g = 0; g < K; g++) {
                    solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                    solver_input.global_lower_bounds.push_back(0.0);
                    solver_input.global_upper_bounds.push_back(1.0);
                    solver_input.global_obj_coeffs.push_back(0.0);
                }
                for (idx_t g = 0; g < K; g++) {
                    double scale = per_inner_was_avg ? 1.0 / static_cast<double>(group_size(g)) : 1.0;
                    MinMaxLinkRow link;
                    for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) {
                        AddObjectiveRowTerms(link, obj_flat_rows[k], scale);
                    }
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    link.AppendTo(rc);
                    idx_t u_idx = first_u + g;
                    if (outer_is_min) {
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(-M_outer);
                        rc.sense = '>';
                        rc.rhs = -M_outer + link.constant;
                    } else {
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(M_outer);
                        rc.sense = '<';
                        rc.rhs = M_outer + link.constant;
                    }
                    solver_input.global_constraints.push_back(std::move(rc));
                }
                SolverInput::RawConstraint sum_u;
                for (idx_t g = 0; g < K; g++) {
                    sum_u.indices.push_back((int)(first_u + g));
                    sum_u.coefficients.push_back(1.0);
                }
                sum_u.sense = '>';
                sum_u.rhs = 1.0;
                solver_input.global_constraints.push_back(std::move(sum_u));
            }
        }
    } else if (flat_objective_agg != ObjectiveAggregateType::NONE && !saved_obj_var_indices.empty()) {
        // ================================================================
        // PATH A: Non-PER flat MIN/MAX objective (existing behavior)
        // ================================================================
        bool is_min_agg = (flat_objective_agg == ObjectiveAggregateType::MIN_AGG);
        bool is_easy = flat_objective_is_easy;

        // Compute active rows: pass WHEN, and have at least one nonzero coefficient.
        // - Easy path: skipping inactive rows just avoids vacuous linking constraints
        //   (the existing code already skipped zero coefficients individually, but still
        //   emitted an empty linking row).
        // - Hard path: this also reduces the number of indicator binaries and the size
        //   of the SUM(y) >= 1 constraint sent to the solver.
        vector<idx_t> active_rows;
        active_rows.reserve(num_rows);
        for (idx_t row = 0; row < num_rows; row++) {
            if (objective_has_when && !objective_when_mask[row]) continue;
            bool has_nonzero = false;
            for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                if (std::abs(saved_obj_coefficients[t].Get(row)) >= 1e-15) {
                    has_nonzero = true;
                    break;
                }
            }
            if (has_nonzero) {
                active_rows.push_back(row);
            }
        }
        if (active_rows.empty()) {
            throw InvalidInputException(
                "MIN/MAX objective has no active rows after WHEN filtering and zero-coefficient "
                "elimination. The auxiliary variable would have no pinning constraints, "
                "making the optimization unbounded or vacuous.");
        }

        idx_t z_idx = var_indexer.global_block_start + solver_input.num_global_vars;

        // Create global variable z (continuous, unbounded)
        solver_input.num_global_vars += 1;
        solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
        solver_input.global_lower_bounds.push_back(-1e30);
        solver_input.global_upper_bounds.push_back(1e30);
        solver_input.global_obj_coeffs.push_back(1.0); // objective = z

        // Clear per-row objective (z is the sole objective term now)
        solver_input.objective_coefficients.clear();
        solver_input.objective_variable_indices.clear();

        if (is_easy) {
            char sense_char = is_min_agg ? '<' : '>';
            for (idx_t row : active_rows) {
                MinMaxLinkRow link;
                AddObjectiveRowTerms(link, row, 1.0);
                SolverInput::RawConstraint rc;
                rc.sense = sense_char;
                rc.rhs = link.constant;
                rc.indices.push_back((int)z_idx);
                rc.coefficients.push_back(1.0);
                link.AppendTo(rc);
                solver_input.global_constraints.push_back(std::move(rc));
            }
        } else {
            double M = compute_big_m();

            // Allocate one indicator binary per ACTIVE row (not per total row).
            idx_t first_y_idx = z_idx + 1;
            idx_t num_active = active_rows.size();
            solver_input.num_global_vars += num_active;
            for (idx_t r = 0; r < num_active; r++) {
                solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                solver_input.global_lower_bounds.push_back(0.0);
                solver_input.global_upper_bounds.push_back(1.0);
                solver_input.global_obj_coeffs.push_back(0.0);
            }

            for (idx_t a = 0; a < active_rows.size(); a++) {
                idx_t row = active_rows[a];
                MinMaxLinkRow link;
                AddObjectiveRowTerms(link, row, 1.0);
                SolverInput::RawConstraint rc;
                rc.indices.push_back((int)z_idx);
                rc.coefficients.push_back(1.0);
                link.AppendTo(rc);
                idx_t y_idx = first_y_idx + a;
                if (is_min_agg) {
                    rc.indices.push_back((int)y_idx);
                    rc.coefficients.push_back(-M);
                    rc.sense = '>';
                    rc.rhs = -M + link.constant;
                } else {
                    rc.indices.push_back((int)y_idx);
                    rc.coefficients.push_back(M);
                    rc.sense = '<';
                    rc.rhs = M + link.constant;
                }
                solver_input.global_constraints.push_back(std::move(rc));
            }

            SolverInput::RawConstraint sum_y;
            for (idx_t a = 0; a < active_rows.size(); a++) {
                sum_y.indices.push_back((int)(first_y_idx + a));
                sum_y.coefficients.push_back(1.0);
            }
            sum_y.sense = '>';
            sum_y.rhs = 1.0;
            solver_input.global_constraints.push_back(std::move(sum_y));
        }
    }

    // Shared hard-direction indicator layer for one composed MIN/MAX term whose
    // global auxiliary is `z_idx`. The caller emits the base one-sided envelope pin
    // (z >= inner for MAX / z <= inner for MIN) for BOTH directions; that alone
    // suffices for the easy direction (the outer pressure drives z to the extreme).
    // The hard direction adds, per active row, a binary y_i, a SUM(y_i) >= 1 pin,
    // and a Big-M link on the *opposite* envelope side so z is pinned to the actual
    // MIN/MAX rather than floating:
    //   MAX: z <= inner_i + M(1 - y_i)  ->  z - sum(c*var) + M*y_i <= M + const
    //   MIN: z >= inner_i - M(1 - y_i)  ->  z - sum(c*var) - M*y_i >= -M + const
    // M is the signed spread of `inner` over the term's active rows (identical
    // formula to compute_big_m: global_max - global_min), which always dominates
    // |z - inner_i|; constant inner terms cancel in the spread. This mirrors the
    // flat (non-composed) hard MIN/MAX emission (PATH A) so both share one M model.
    auto EmitComposedHardMinMaxIndicators =
        [&](idx_t z_idx, bool is_max, const vector<Term> &inner_terms,
            const vector<vector<double>> &per_term_coefs, const vector<bool> &filter_mask,
            const string &label) {
        bool unbounded = false;
        double global_max = 0.0, global_min = 0.0;
        for (idx_t row = 0; row < num_rows; row++) {
            if (!filter_mask[row]) continue;
            double row_max = 0.0, row_min = 0.0;
            for (idx_t it = 0; it < inner_terms.size(); it++) {
                idx_t v = inner_terms[it].variable_index;
                if (v == DConstants::INVALID_INDEX) continue; // constant cancels in spread
                double c = per_term_coefs[it][row];
                if (std::abs(c) < 1e-15) continue;
                double lb = solver_input.lower_bounds[v];
                double ub = solver_input.upper_bounds[v];
                if (ub >= 1e20 || lb <= -1e20) { unbounded = true; continue; }
                if (c > 0.0) { row_max += c * ub; row_min += c * lb; }
                else { row_max += c * lb; row_min += c * ub; }
            }
            global_max = std::max(global_max, row_max);
            global_min = std::min(global_min, row_min);
        }
        double M = global_max - global_min;
        if (unbounded) M = std::max(M, DECIDE_BIGM_FALLBACK);

        SolverInput::RawConstraint sum_y;
        for (idx_t row = 0; row < num_rows; row++) {
            if (!filter_mask[row]) continue;
            idx_t y_idx = var_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars += 1;
            solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
            solver_input.global_lower_bounds.push_back(0.0);
            solver_input.global_upper_bounds.push_back(1.0);
            solver_input.global_obj_coeffs.push_back(0.0);
            solver_input.global_variable_labels.resize(y_idx - var_indexer.global_block_start);
            solver_input.global_variable_labels.push_back(label);

            sum_y.indices.push_back((int)y_idx);
            sum_y.coefficients.push_back(1.0);

            MinMaxLinkRow row_terms;
            AddComposedRowTerms(row_terms, inner_terms, per_term_coefs, row);
            SolverInput::RawConstraint link;
            link.indices.push_back((int)z_idx);
            link.coefficients.push_back(1.0);
            row_terms.AppendTo(link);
            if (is_max) {
                link.indices.push_back((int)y_idx);
                link.coefficients.push_back(M);
                link.sense = '<';
                link.rhs = M + row_terms.constant;
            } else {
                link.indices.push_back((int)y_idx);
                link.coefficients.push_back(-M);
                link.sense = '>';
                link.rhs = -M + row_terms.constant;
            }
            link.kind = ConstraintKind::USER_PARAMETER;
            solver_input.global_constraints.push_back(std::move(link));
        }
        sum_y.sense = '>';
        sum_y.rhs = 1.0;
        sum_y.kind = ConstraintKind::USER_PARAMETER;
        solver_input.global_constraints.push_back(std::move(sum_y));
    };

    // ================================================================
    // Composed MIN/MAX constraints: additive LHS mixing SUM/AVG/MIN/MAX.
    // Each MIN/MAX term gets a global auxiliary z_k pinned by per-row
    // constraints. The outer composed constraint is emitted as a
    // RawConstraint summing SUM/AVG contributions + z_k references.
    // Both directions are supported: the easy direction (MAX pushed down /
    // MIN pushed up) needs only the one-sided envelope pin; the hard
    // direction adds the per-row indicator layer above. Constant RHS,
    // no outer WHEN/PER wrappers.
    // ================================================================
    if (!composed_minmax_constraints.empty()) {
        // Helper: evaluate a Term's per-row coefficient (scaled by term.sign)
        auto EvaluateTermCoefs = [&](const Term &term) -> vector<double> {
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
                            "Composed MIN/MAX constraint: coefficient expression returned NULL.");
                    }
                    double d = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    if (!std::isfinite(d)) {
                        throw InvalidInputException(
                            "Composed MIN/MAX constraint: coefficient is not finite (NaN/Inf).");
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
        auto EvaluateQueryWideScale = [&](const Expression &scale, bool divides) -> double {
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
                        "DECIDE constraint: division by zero — the factor on an aggregate "
                        "evaluated to 0.");
                }
                return 1.0 / v;
            }
            return v;
        };

        for (auto &spec : composed_minmax_constraints) {
            // RHS must be row-invariant in v1. Decide that by foldability rather
            // than by matching a literal node: canonicalization rebuilds the bound
            // side as an additive chain, so a constant arrives as `(0 - 3) + 0`
            // just as legitimately as `-3`. IsFoldable is the same test the
            // per-row constraint path uses for a shared literal RHS.
            if (!spec.rhs_expr->IsFoldable()) {
                throw BinderException(
                    "Composed MIN/MAX in DECIDE v1 requires a constant RHS; got '%s'.",
                    spec.rhs_expr->ToString());
            }
            double rhs_val = ExpressionExecutor::EvaluateScalar(context, *spec.rhs_expr)
                                 .DefaultCastAs(LogicalType::DOUBLE)
                                 .GetValue<double>();

            struct TermAnalysis {
                LogicalDecide::ComposedMinMaxTerm::Kind kind;
                string agg_name;
                int sign;
                //! Factor the canonicalizer peeled off this reducer, already evaluated.
                //! It stays outside the aggregate all the way to here, which is what
                //! makes its sign irrelevant to correctness -- see
                //! LogicalDecide::ComposedMinMaxTerm::scale.
                double scale = 1.0;
                bool is_easy;
                vector<bool> filter_mask;
                vector<Term> inner_terms;
                vector<vector<double>> per_term_coefs;
                idx_t z_idx = DConstants::INVALID_INDEX;
                string label; //!< User source text (`MAX(x)`) naming this term's global z.
            };
            vector<TermAnalysis> analyses;

            // Collect filter expressions for batch evaluation (one scan for all terms).
            vector<const Expression *> composed_cond_ptrs;
            for (auto &term : spec.terms) {
                if (term.filter) composed_cond_ptrs.push_back(term.filter.get());
            }
            auto composed_masks = EvaluateBooleanMasks(composed_cond_ptrs);

            idx_t mask_slot = 0;
            for (auto &term : spec.terms) {
                TermAnalysis ta;
                ta.kind = term.kind;
                ta.agg_name = term.agg_name;
                ta.sign = term.sign;
                ta.is_easy = term.is_easy;
                if (term.scale) {
                    ta.scale = EvaluateQueryWideScale(*term.scale, term.scale_divides);
                }
                if (term.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
                    ta.label = StringUtil::Upper(term.agg_name) + "(" + term.inner_expr->ToString() + ")";
                }

                ExtractTerms(context, *term.inner_expr, ta.inner_terms);
                for (auto &inner_t : ta.inner_terms) {
                    ta.per_term_coefs.push_back(EvaluateTermCoefs(inner_t));
                }
                if (term.filter) {
                    ta.filter_mask = std::move(composed_masks[mask_slot++]);
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
                                                       term.qualifier_scope_idx, {});
                    for (idx_t row = 0; row < num_rows; row++) {
                        ta.filter_mask[row] = ta.filter_mask[row] && keep[row];
                    }
                }
                analyses.push_back(std::move(ta));
            }

            // Allocate global z_k for each MIN/MAX term. Both directions supported:
            // hard terms get the indicator layer emitted after the base envelope pin.
            for (auto &ta : analyses) {
                if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                // Reject empty WHEN on composed MIN/MAX terms: without this the
                // z_k auxiliary floats free (no per-row pinning), silently
                // vacating the entire additive constraint.
                idx_t cnt = 0;
                for (bool m : ta.filter_mask) if (m) cnt++;
                RejectEmptyAggregate(cnt, ta.agg_name.c_str(), "composed constraint");
                ta.z_idx = var_indexer.global_block_start + solver_input.num_global_vars;
                solver_input.num_global_vars += 1;
                solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
                solver_input.global_lower_bounds.push_back(-1e30);
                solver_input.global_upper_bounds.push_back(1e30);
                solver_input.global_obj_coeffs.push_back(0.0);
                // Name the z through the global label channel (as the aggregate-`<>` site
                // does) so a diagnosis renders `MAX(x)`, never an internal column name.
                // Pad first: earlier allocation sites may not have pushed labels.
                solver_input.global_variable_labels.resize(ta.z_idx - var_indexer.global_block_start);
                solver_input.global_variable_labels.push_back(ta.label);
            }
            // Also reject empty WHEN on composed SUM/AVG terms for consistency
            // with the reject-all rule. Without the check, an empty SUM just
            // contributes 0 (vacuous but defined); an empty AVG currently
            // divides by zero at line ~3662 and skips, silently losing the term.
            for (auto &ta : analyses) {
                if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                idx_t cnt = 0;
                for (bool m : ta.filter_mask) if (m) cnt++;
                RejectEmptyAggregate(cnt, ta.agg_name.c_str(), "composed constraint");
            }

            // Emit the base one-sided envelope pin for each MIN/MAX term (both
            // directions): MAX → z_k >= inner_expr per row (z_k >= max), MIN →
            // z_k <= inner_expr per row (z_k <= min). For the easy direction the
            // outer pressure drives z_k to the extreme; the hard direction adds an
            // indicator layer below to pin z_k to the actual MIN/MAX.
            for (auto &ta : analyses) {
                if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                bool is_max = (ta.agg_name == "max");
                char sense = is_max ? '>' : '<';
                for (idx_t row = 0; row < num_rows; row++) {
                    if (!ta.filter_mask[row]) continue;
                    MinMaxLinkRow link;
                    AddComposedRowTerms(link, ta.inner_terms, ta.per_term_coefs, row);
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)ta.z_idx);
                    rc.coefficients.push_back(1.0);
                    link.AppendTo(rc);
                    rc.sense = sense;
                    rc.rhs = link.constant;
                    solver_input.global_constraints.push_back(std::move(rc));
                }
            }

            // Hard-direction terms: add the indicator layer so z_k is pinned to the
            // actual MIN/MAX (the outer constraint pushes z_k the "wrong" way, so the
            // base envelope pin alone would let it float).
            for (auto &ta : analyses) {
                if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                if (ta.is_easy) continue;
                EmitComposedHardMinMaxIndicators(ta.z_idx, ta.agg_name == "max",
                                                 ta.inner_terms, ta.per_term_coefs,
                                                 ta.filter_mask, ta.label);
            }

            // Build the outer composed RawConstraint
            std::unordered_map<int, double> outer_accum;
            double outer_rhs = rhs_val;
            for (auto &ta : analyses) {
                if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
                    outer_accum[(int)ta.z_idx] += (double)ta.sign * ta.scale;
                } else {
                    // SUM/AVG term. For AVG, divide by filtered row count.
                    double avg_divisor = 1.0;
                    if (ta.agg_name == "avg") {
                        idx_t cnt = 0;
                        for (idx_t r = 0; r < num_rows; r++) {
                            if (ta.filter_mask[r]) cnt++;
                        }
                        if (cnt == 0) {
                            // Empty aggregate — contributes 0; skip.
                            continue;
                        }
                        avg_divisor = static_cast<double>(cnt);
                    }
                    for (idx_t it = 0; it < ta.inner_terms.size(); it++) {
                        auto &inner_t = ta.inner_terms[it];
                        for (idx_t row = 0; row < num_rows; row++) {
                            if (!ta.filter_mask[row]) continue;
                            double coef = ta.per_term_coefs[it][row] * (double)ta.sign * ta.scale / avg_divisor;
                            if (inner_t.variable_index == DConstants::INVALID_INDEX) {
                                outer_rhs -= coef;
                            } else {
                                int abs_idx = (int)var_indexer.Get(inner_t.variable_index, row);
                                outer_accum[abs_idx] += coef;
                            }
                        }
                    }
                }
            }

            SolverInput::RawConstraint outer;
            for (auto &p : outer_accum) {
                if (p.second != 0.0) {
                    outer.indices.push_back(p.first);
                    outer.coefficients.push_back(p.second);
                }
            }
            switch (spec.outer_cmp) {
            case ExpressionType::COMPARE_LESSTHANOREQUALTO:
            case ExpressionType::COMPARE_LESSTHAN:
                outer.sense = '<';
                outer.rhs = outer_rhs;
                break;
            case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
            case ExpressionType::COMPARE_GREATERTHAN:
                outer.sense = '>';
                outer.rhs = outer_rhs;
                break;
            default:
                throw InternalException("Composed MIN/MAX: unexpected comparison type.");
            }
            outer.kind = ConstraintKind::USER_PARAMETER;
            solver_input.global_constraints.push_back(std::move(outer));
        }
    }

    // ================================================================
    // Composed MIN/MAX objective: `MAXIMIZE|MINIMIZE T1 + T2 + ...`
    // Each MIN/MAX term gets a global z_k pinned by per-row constraints;
    // SUM/AVG terms populate objective_coefficients. v1: easy-direction
    // terms only, no outer PER/WHEN on the objective.
    // ================================================================
    if (!composed_minmax_objective_terms.empty()) {
        auto EvaluateTermCoefsObj = [&](const Term &term) -> vector<double> {
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
                            "Composed MIN/MAX objective: coefficient expression returned NULL.");
                    }
                    double d = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    if (!std::isfinite(d)) {
                        throw InvalidInputException(
                            "Composed MIN/MAX objective: coefficient is not finite.");
                    }
                    coefs.push_back(d * term.sign);
                }
            }
            return coefs;
        };

        //! Objective-side twin of the constraint path's helper. Same rule: `scale` must
        //! be the term's own expression, not a copy -- the chunk-expression cache is
        //! keyed by address.
        auto EvaluateQueryWideScale = [&](const Expression &scale, bool divides) -> double {
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
                        "DECIDE objective: division by zero — the factor on an aggregate "
                        "evaluated to 0.");
                }
                return 1.0 / v;
            }
            return v;
        };

        // Clear any existing objective terms — the placeholder constant produced
        // none, but be defensive in case other paths populated them.
        solver_input.objective_coefficients.clear();
        solver_input.objective_variable_indices.clear();

        struct ObjTermAnalysis {
            LogicalDecide::ComposedMinMaxTerm::Kind kind;
            string agg_name;
            int sign;
            //! Factor the canonicalizer peeled off this reducer, already evaluated.
            //! See LogicalDecide::ComposedMinMaxTerm::scale.
            double scale = 1.0;
            bool is_easy;
            vector<bool> filter_mask;
            vector<Term> inner_terms;
            vector<vector<double>> per_term_coefs;
            idx_t z_idx = DConstants::INVALID_INDEX;
            string label; //!< User source text (`MAX(x)`) naming this term's global z.
        };
        vector<ObjTermAnalysis> obj_analyses;

        // Batch-evaluate all per-term filter conditions for composed objective terms.
        {
            vector<const Expression *> obj_comp_cond_ptrs;
            for (auto &term : composed_minmax_objective_terms) {
                if (term.filter) obj_comp_cond_ptrs.push_back(term.filter.get());
            }
            auto obj_comp_masks = EvaluateBooleanMasks(obj_comp_cond_ptrs);

            idx_t mask_slot = 0;
            for (auto &term : composed_minmax_objective_terms) {
                ObjTermAnalysis ta;
                ta.kind = term.kind;
                ta.agg_name = term.agg_name;
                ta.sign = term.sign;
                ta.is_easy = term.is_easy;
                if (term.scale) {
                    ta.scale = EvaluateQueryWideScale(*term.scale, term.scale_divides);
                }
                if (term.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
                    ta.label = StringUtil::Upper(term.agg_name) + "(" + term.inner_expr->ToString() + ")";
                }
                ExtractTerms(context, *term.inner_expr, ta.inner_terms);
                for (auto &inner_t : ta.inner_terms) {
                    ta.per_term_coefs.push_back(EvaluateTermCoefsObj(inner_t));
                }
                if (term.filter) {
                    ta.filter_mask = std::move(obj_comp_masks[mask_slot++]);
                } else {
                    ta.filter_mask.assign(num_rows, true);
                }
                // Same qualifier de-duplication as the composed constraint path above.
                if (term.qualifier_scope_idx != DConstants::INVALID_INDEX) {
                    auto keep = BuildQualifierKeepMask(solver_input.entity_mappings,
                                                       term.qualifier_scope_idx, {});
                    for (idx_t row = 0; row < num_rows; row++) {
                        ta.filter_mask[row] = ta.filter_mask[row] && keep[row];
                    }
                }
                obj_analyses.push_back(std::move(ta));
            }
        }

        // Allocate z_k per MIN/MAX term. v1 rejects hard direction.
        for (auto &ta : obj_analyses) {
            if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
            // Reject empty WHEN on composed MIN/MAX objective terms: without
            // this the z_k floats free and the objective silently ignores the
            // missing piece.
            idx_t cnt = 0;
            for (bool m : ta.filter_mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, ta.agg_name.c_str(), "composed objective");
            ta.z_idx = var_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars += 1;
            solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
            solver_input.global_lower_bounds.push_back(-1e30);
            solver_input.global_upper_bounds.push_back(1e30);
            solver_input.global_obj_coeffs.push_back(0.0);
            // Name the z through the global label channel (as the aggregate-`<>` site
            // does) so a diagnosis renders `MAX(x)`, never an internal column name.
            // Pad first: earlier allocation sites may not have pushed labels.
            solver_input.global_variable_labels.resize(ta.z_idx - var_indexer.global_block_start);
            solver_input.global_variable_labels.push_back(ta.label);
        }
        // Mirror the SUM/AVG empty-set rejection from the composed constraint path.
        for (auto &ta : obj_analyses) {
            if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
            idx_t cnt = 0;
            for (bool m : ta.filter_mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, ta.agg_name.c_str(), "composed objective");
        }

        // Pinning constraints for MIN/MAX terms.
        for (auto &ta : obj_analyses) {
            if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
            bool is_max = (ta.agg_name == "max");
            // MAXIMIZE+MIN: z_k <= expr_i per row (solver drives z_k up to min)
            // MINIMIZE+MAX: z_k >= expr_i per row (solver drives z_k down to max)
            char sense = is_max ? '>' : '<';
            for (idx_t row = 0; row < num_rows; row++) {
                if (!ta.filter_mask[row]) continue;
                MinMaxLinkRow link;
                AddComposedRowTerms(link, ta.inner_terms, ta.per_term_coefs, row);
                SolverInput::RawConstraint rc;
                rc.indices.push_back((int)ta.z_idx);
                rc.coefficients.push_back(1.0);
                link.AppendTo(rc);
                rc.sense = sense;
                rc.rhs = link.constant;
                solver_input.global_constraints.push_back(std::move(rc));
            }
        }

        // Hard-direction terms: add the indicator layer. Without it a hard term
        // (MAXIMIZE+MAX / MINIMIZE+MIN) leaves z_k unpinned on the side the
        // objective drives it, making the objective unbounded.
        for (auto &ta : obj_analyses) {
            if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
            if (ta.is_easy) continue;
            EmitComposedHardMinMaxIndicators(ta.z_idx, ta.agg_name == "max",
                                             ta.inner_terms, ta.per_term_coefs,
                                             ta.filter_mask, ta.label);
        }

        // Populate objective coefficients. For MIN/MAX terms, the obj coef on z_k
        // is ta.sign (i.e., sign×1.0); set via global_obj_coeffs. For SUM/AVG
        // terms, accumulate per-row linear coefficients into objective_coefficients
        // keyed by decide variable.
        // Accumulator: decide_var_index -> per-row coefficient vector.
        std::unordered_map<idx_t, vector<double>> obj_coef_accum;
        for (auto &ta : obj_analyses) {
            if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
                // The z_k's obj coef is ta.sign (the MIN/MAX term's sign in the additive sum).
                idx_t gslot = ta.z_idx - var_indexer.global_block_start;
                solver_input.global_obj_coeffs[gslot] = (double)ta.sign * ta.scale;
            } else {
                double avg_divisor = 1.0;
                if (ta.agg_name == "avg") {
                    idx_t cnt = 0;
                    for (idx_t r = 0; r < num_rows; r++) if (ta.filter_mask[r]) cnt++;
                    if (cnt == 0) continue;
                    avg_divisor = (double)cnt;
                }
                for (idx_t it = 0; it < ta.inner_terms.size(); it++) {
                    auto &inner_t = ta.inner_terms[it];
                    if (inner_t.variable_index == DConstants::INVALID_INDEX) continue;
                    auto &dst = obj_coef_accum[inner_t.variable_index];
                    if (dst.empty()) dst.assign(num_rows, 0.0);
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (!ta.filter_mask[row]) continue;
                        dst[row] += ta.per_term_coefs[it][row] * (double)ta.sign * ta.scale / avg_divisor;
                    }
                }
            }
        }
        for (auto &p : obj_coef_accum) {
            solver_input.objective_variable_indices.push_back(p.first);
            solver_input.objective_coefficients.push_back(CoefficientColumn::FromVector(std::move(p.second)));
        }
    }

    // Refresh total_vars: the row/entity blocks were finalized at construction,
    // but global aux vars were appended throughout deferred-NE and MIN/MAX expansion.
    var_indexer.total_vars = var_indexer.global_block_start + solver_input.num_global_vars;

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
        SolveModel(solver_input, var_indexer, solve_options, diagnosis_armed ? &retained_model : nullptr,
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
        bool has_nonlinear_terms =
            retained_model.has_quadratic_obj || !retained_model.quadratic_constraints.empty();
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
        vector<string> var_labels;
        vector<bool> var_is_aux;
        build_var_labels(var_labels, var_is_aux);

        auto diag_params = GetDecideDiagnosticParams(context);
        SolverBackend backend = SelectSolverBackend();

        // Decision 1a: a user constraint like `x <= 10` / `x BETWEEN a AND b` was
        // absorbed into the column-bound arrays, not emitted as a matrix row, so it
        // is invisible to the elastic engine. Re-emit each absorbed user bound as a
        // USER_PARAMETER slackable row on the retained model and relax the rigid
        // column bound it produced, so the bound is enforced only by the (loosenable)
        // row. The bound `x <= k` is ONE editable knob; on a multi-instance variable
        // it fans into one row per instance under a single (synthetic) clause id and
        // shape SHARED_LITERAL, so the elastic engine collapses them to one shared
        // slack and reports the max overshoot (I2.a). Single-instance is the N=1 case
        // (a size-1 block). Only genuine user bounds reach here — a variable's
        // intrinsic domain (BOOLEAN 0/1, default non-negativity) is never recorded in
        // user_absorbed_bounds (see TraverseBoundsConstraints), so it stays rigid.
        // A BOOLEAN pin (`x <= 0`, `x >= 1`, `x = 1`) IS recorded; its column is
        // opened only back to the intrinsic [0,1] — never past it — so the pin
        // becomes loosenable while the 0/1 domain itself stays rigid.
        // C3: if a recorded bound cannot be re-emitted (its column is missing from
        // the retained model), the elastic model is missing a user constraint —
        // flag it so the engine won't claim an elastic-infeasible verdict.
        bool has_unhandled_user_bounds = false;
        idx_t synthetic_clause_id = solver_input.constraints.size();
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
                row.provenance.clause_id = bound_clause_id;
                row.provenance.group_key = DConstants::INVALID_INDEX;
                row.provenance.kind = ConstraintKind::USER_PARAMETER;
                row.provenance.shape = ElasticShape::SHARED_LITERAL;
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
        current_row_offset = 0;
    }

    ColumnDataScanState scan_state;
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

    // Scan the original buffered data
    gstate.data.Scan(source_state.scan_state, chunk);
    if (chunk.size() == 0) {
        return SourceResultType::FINISHED;
    }

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
