#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include <cmath>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "duckdb/common/profiler.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/packdb/utility/debug.hpp"
#include "duckdb/packdb/ilp_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
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
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Transform Helpers
//===--------------------------------------------------------------------===//
// These static functions replace BoundColumnRefExpression nodes with
// BoundReferenceExpression nodes so that DuckDB's ExpressionExecutor can
// evaluate expressions against data chunks (which use positional indices).

//! Transform a coefficient/value expression for ExpressionExecutor.
//! Handles: BOUND_COLUMN_REF, BOUND_FUNCTION, BOUND_CAST, BOUND_AGGREGATE (count_star→constant),
//! BOUND_COMPARISON, BOUND_CONJUNCTION, BOUND_OPERATOR. Falls back to Copy() for others.
static unique_ptr<Expression> TransformToChunkExpression(const Expression &expr, ClientContext &context,
                                                         idx_t num_rows = 0) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		vector<unique_ptr<Expression>> new_children;
		for (auto &child : func.children) {
			new_children.push_back(TransformToChunkExpression(*child, context, num_rows));
		}
		unique_ptr<FunctionData> new_bind_info;
		if (func.bind_info) {
			new_bind_info = func.bind_info->Copy();
		}
		return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
		                                                           std::move(new_children), std::move(new_bind_info));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		auto transformed_child = TransformToChunkExpression(*cast.child, context, num_rows);
		return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = expr.Cast<BoundAggregateExpression>();
		if (agg.function.name == "count_star") {
			return make_uniq_base<Expression, BoundConstantExpression>(Value::BIGINT(num_rows));
		}
		throw InternalException("Unsupported aggregate '%s' in constraint RHS. "
		                        "Only count_star() is supported.", agg.function.name);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		auto left = TransformToChunkExpression(*comp.left, context, num_rows);
		auto right = TransformToChunkExpression(*comp.right, context, num_rows);
		return make_uniq_base<Expression, BoundComparisonExpression>(comp.type, std::move(left), std::move(right));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
		for (auto &child : conj.children) {
			result->children.push_back(TransformToChunkExpression(*child, context, num_rows));
		}
		return std::move(result);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		auto result = make_uniq<BoundOperatorExpression>(op_expr.type, op_expr.return_type);
		for (auto &child : op_expr.children) {
			result->children.push_back(TransformToChunkExpression(*child, context, num_rows));
		}
		return std::move(result);
	} else {
		return expr.Copy();
	}
}

//! Per-Finalize cache for TransformToChunkExpression. Keyed on input Expression
//! pointer identity — addresses are stable for the duration of one Finalize, and
//! `num_rows` is constant within Finalize so it's not part of the key. The cache
//! owns lifetime; callers that pass the returned reference to ExpressionExecutor
//! must keep the cache alive until the executor is no longer used.
using ChunkExprCache = std::unordered_map<const Expression*, unique_ptr<Expression>>;

static const Expression &CachedTransformToChunkExpression(ChunkExprCache &cache,
                                                          const Expression &expr,
                                                          ClientContext &context,
                                                          idx_t num_rows = 0) {
	auto it = cache.find(&expr);
	if (it != cache.end()) {
		return *it->second;
	}
	auto transformed = TransformToChunkExpression(expr, context, num_rows);
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
                          idx_t &out_num_groups) {
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
};

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
                                     idx_t &out_num_groups) {
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
		              entry->unfiltered_row_group_ids, entry->unfiltered_num_groups);
	}

	// Apply per-call filter and remap surviving group IDs to consecutive 0..K'
	// in encounter order, matching the legacy BuildGroupIds output exactly.
	out_row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
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
		}
		out_row_group_ids[r] = mapped;
	}
	out_num_groups = next_remap;
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

static unique_ptr<Expression> BuildCoefficientFromFactors(const vector<const Expression *> &factors,
                                                          const BoundFunctionExpression &mul_func) {
	if (factors.empty()) {
		return nullptr;
	}
	if (factors.size() == 1) {
		return factors[0]->Copy();
	}

	auto result = factors[0]->Copy();
	for (idx_t i = 1; i < factors.size(); i++) {
		vector<unique_ptr<Expression>> mul_children;
		mul_children.push_back(std::move(result));
		mul_children.push_back(factors[i]->Copy());
		unique_ptr<FunctionData> bind_info;
		if (mul_func.bind_info) {
			bind_info = mul_func.bind_info->Copy();
		}
		result = make_uniq_base<Expression, BoundFunctionExpression>(
		    mul_func.return_type, mul_func.function, std::move(mul_children), std::move(bind_info));
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
TryDistributeMultiplyOverAdd(const BoundFunctionExpression &mul_expr) {
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
		auto prod = BuildCoefficientFromFactors(new_factors, mul_expr);
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

unique_ptr<Expression> PhysicalDecide::ExtractCoefficientWithoutVariable(const Expression &expr, idx_t var_idx) const {
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
                }
            }

            if (filtered_children.empty()) {
                return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
            }
            if (filtered_children.size() == 1) {
                return std::move(filtered_children[0]);
            }

            // Rebuild multiplication with remaining children
            return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
                                                     std::move(filtered_children),
                                                     func.bind_info ? func.bind_info->Copy() : nullptr);
        }
    }

    // If it's a cast, recurse into child
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        return ExtractCoefficientWithoutVariable(*cast.child, var_idx);
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

void PhysicalDecide::ExtractTerms(const Expression &expr, vector<Term> &out_terms) const {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();

        // Addition: recursively process all children
        if (func.function.name == "+") {
            for (auto &child : func.children) {
                ExtractTerms(*child, out_terms);
            }
            return;
        }

        // Subtraction: first child positive, second child negated
        if (func.function.name == "-" && func.children.size() == 2) {
            ExtractTerms(*func.children[0], out_terms);
            idx_t before = out_terms.size();
            ExtractTerms(*func.children[1], out_terms);
            for (idx_t i = before; i < out_terms.size(); i++) {
                out_terms[i].sign *= -1;
            }
            return;
        }

        // Unary minus: recurse and flip sign of every produced term.
        if (func.function.name == "-" && func.children.size() == 1) {
            idx_t before = out_terms.size();
            ExtractTerms(*func.children[0], out_terms);
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
            auto distributed = TryDistributeMultiplyOverAdd(func);
            if (!distributed.empty()) {
                for (auto &kv : distributed) {
                    idx_t before = out_terms.size();
                    ExtractTerms(*kv.second, out_terms);
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
                auto coef = ExtractCoefficientWithoutVariable(func, var_idx);
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
            ExtractTerms(*func.children[0], out_terms);
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
        ExtractTerms(*cast.child, out_terms);
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

// PackDB: reject an aggregate whose effective row set is empty (after WHEN
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

static void CollectConstraintStringsPhysical(const Expression &expr, vector<string> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
			string per_suffix = " PER ";
			for (idx_t i = 1; i < conj.children.size(); i++) {
				if (i > 1) {
					per_suffix += ", ";
				}
				per_suffix += conj.children[i]->GetName();
			}
			vector<string> inner;
			CollectConstraintStringsPhysical(*conj.children[0], inner);
			for (auto &s : inner) {
				out.push_back(s + per_suffix);
			}
			return;
		}
		if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
			string when_suffix = " WHEN " + conj.children[1]->GetName();
			vector<string> inner;
			CollectConstraintStringsPhysical(*conj.children[0], inner);
			for (auto &s : inner) {
				out.push_back(s + when_suffix);
			}
			return;
		}
		for (auto &child : conj.children) {
			CollectConstraintStringsPhysical(*child, out);
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
		string obj_info = (decide_sense == DecideSense::MAXIMIZE) ? "MAXIMIZE " : "MINIMIZE ";
		obj_info += decide_objective->GetName();
		result["Objective"] = obj_info;
	} else {
		result["Objective"] = "FEASIBILITY";
	}

	if (decide_constraints) {
		vector<string> constraint_strs;
		CollectConstraintStringsPhysical(*decide_constraints, constraint_strs);
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
            // the coefficient magnitude (handled separately by FindVarCoefficient).
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

//! Replace all DECIDE variable references in a bound expression with constant 0.
//! Returns a data-only expression that can be evaluated per-row from the input chunk.
static unique_ptr<Expression> StripDecideVars(const Expression &expr, const PhysicalDecide &op) {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        idx_t var_idx = op.FindDecideVariable(expr);
        if (var_idx != DConstants::INVALID_INDEX) {
            return make_uniq_base<Expression, BoundConstantExpression>(Value::DOUBLE(0.0));
        }
        return expr.Copy();
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        vector<unique_ptr<Expression>> new_children;
        for (auto &child : func.children) {
            new_children.push_back(StripDecideVars(*child, op));
        }
        unique_ptr<FunctionData> new_bind_info;
        if (func.bind_info) {
            new_bind_info = func.bind_info->Copy();
        }
        return make_uniq_base<Expression, BoundFunctionExpression>(
            func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        auto new_child = StripDecideVars(*cast.child, op);
        // Recreate the cast
        return make_uniq_base<Expression, BoundCastExpression>(
            std::move(new_child), cast.return_type, cast.bound_cast.Copy(), cast.try_cast);
    }
    return expr.Copy();
}

//! Walk through +/- nodes to find the sub-expression containing a specific decide
//! variable, then extract its coefficient using ExtractCoefficientWithoutVariable.
//! Returns the unsigned coefficient (sign is tracked separately by CollectDecideVarRefs).
static unique_ptr<Expression> FindVarCoefficient(
    const Expression &expr, idx_t var_idx, const PhysicalDecide &op) {
    if (!op.ContainsVariable(expr, var_idx)) {
        return nullptr;
    }
    // Bare variable reference: coefficient is 1
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        if (op.FindDecideVariable(expr) == var_idx) {
            return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
        }
        return nullptr;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        // Multiplication: this is the coefficient node — extract the non-variable part
        if (func.function.name == "*") {
            return op.ExtractCoefficientWithoutVariable(expr, var_idx);
        }
        // Addition/subtraction: recurse into the child that contains the variable
        if ((func.function.name == "+" || func.function.name == "-") && func.children.size() == 2) {
            for (auto &child : func.children) {
                auto result = FindVarCoefficient(*child, var_idx, op);
                if (result) {
                    return result;
                }
            }
        }
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        return FindVarCoefficient(*cast.child, var_idx, op);
    }
    return nullptr;
}

//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//
class DecideGlobalSinkState : public GlobalSinkState {
public:
    explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()), op(op) {
        // Pre-absorb simple variable bounds (x OP const / BETWEEN) into column-bound
        // arrays so AnalyzeConstraint can skip emitting one DecideConstraint per row
        // for constraints that are fully captured by column bounds.
        idx_t num_decide_vars = op.decide_variables.size();
        absorbed_lower_bounds.assign(num_decide_vars, 0.0);
        absorbed_upper_bounds.assign(num_decide_vars, 1e30);
        for (idx_t var = 0; var < num_decide_vars; var++) {
            auto &decide_var = op.decide_variables[var]->Cast<BoundColumnRefExpression>();
            if (decide_var.return_type == LogicalType::BOOLEAN) {
                absorbed_upper_bounds[var] = 1.0;
            }
        }
        if (op.decide_constraints) {
            TraverseBoundsConstraints(*op.decide_constraints, absorbed_lower_bounds,
                                      absorbed_upper_bounds);
        }

        // Analyze constraints and objective using new visitor-based approach
        AnalyzeConstraint(op.decide_constraints);
        if (op.decide_objective) {
            AnalyzeObjective(op.decide_objective);
        }

        // Minimal: keep constructor lean; detailed solver output comes from HiGHS
    }

    static void ApplyAggregateMetadata(vector<Term> &terms, idx_t begin, const BoundAggregateExpression &agg) {
        bool is_avg = (agg.alias == AVG_REWRITE_TAG);
        for (idx_t i = begin; i < terms.size(); i++) {
            if (agg.filter) {
                terms[i].filter = agg.filter->Copy();
            }
            terms[i].avg_scale = is_avg;
        }
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
        bool is_avg = (agg.alias == AVG_REWRITE_TAG);

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
        }
        for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
            if (agg.filter) {
                constraint.quadratic_groups[i].filter = agg.filter->Copy();
            }
            constraint.quadratic_groups[i].avg_scale = is_avg;
        }

        if (agg.alias.size() > strlen(MINMAX_INDICATOR_TAG_PREFIX) + 2 &&
            agg.alias.substr(0, strlen(MINMAX_INDICATOR_TAG_PREFIX)) == MINMAX_INDICATOR_TAG_PREFIX) {
            auto payload = agg.alias.substr(strlen(MINMAX_INDICATOR_TAG_PREFIX));
            payload = payload.substr(0, payload.size() - 2);
            auto sep = payload.find('_');
            constraint.minmax_indicator_idx = std::stoull(payload.substr(0, sep));
            constraint.minmax_agg_type = payload.substr(sep + 1);
        }
    }

    void AnalyzeConstraint(const unique_ptr<Expression>& expr_ptr,
                           unique_ptr<Expression> when_condition = nullptr,
                           vector<unique_ptr<Expression>> per_columns = {}) {
        auto &expr = *expr_ptr;
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // PackDB: PER wrapper — outermost layer
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
                // PackDB: Check if this is a WHEN constraint wrapper
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
                }

                // Parse ABS MAXIMIZE upper-bound tag: marks a lower-bound ABS constraint
                // (aux >= inner or aux >= -inner) that needs Big-M upper bounds at finalization.
                if (comp.alias.size() > strlen(ABS_UB_POS_TAG_PREFIX) + 2 &&
                    comp.alias.substr(0, strlen(ABS_UB_POS_TAG_PREFIX)) == ABS_UB_POS_TAG_PREFIX) {
                    auto payload = comp.alias.substr(strlen(ABS_UB_POS_TAG_PREFIX));
                    constraint->abs_y_idx = std::stoull(payload.substr(0, payload.size() - 2));
                    constraint->abs_is_pos_bound = true;
                } else if (comp.alias.size() > strlen(ABS_UB_NEG_TAG_PREFIX) + 2 &&
                           comp.alias.substr(0, strlen(ABS_UB_NEG_TAG_PREFIX)) == ABS_UB_NEG_TAG_PREFIX) {
                    auto payload = comp.alias.substr(strlen(ABS_UB_NEG_TAG_PREFIX));
                    constraint->abs_y_idx = std::stoull(payload.substr(0, payload.size() - 2));
                    constraint->abs_is_pos_bound = false;
                }

                // Detect easy-direction MIN/MAX optimizer rewrite (see decide.hpp).
                if (comp.alias == MINMAX_EASY_REWRITE_TAG) {
                    constraint->was_minmax_easy = true;
                }

                // PackDB: Store WHEN condition and PER columns if present
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

                    // Check if RHS contains DECIDE variables (multi-variable constraint)
                    vector<ExprVarRef> rhs_refs;
                    CollectDecideVarRefs(*comp.right, +1, rhs_refs, op);

                    if (!rhs_refs.empty()) {
                        // Multi-variable per-row constraint (e.g., ABS linearization: d >= x - c)
                        // Collect LHS DECIDE vars
                        vector<ExprVarRef> lhs_refs;
                        CollectDecideVarRefs(*lhs, +1, lhs_refs, op);

                        // LHS vars: extract row-varying coefficients, keep sign
                        for (auto &ref : lhs_refs) {
                            auto coef = FindVarCoefficient(*lhs, ref.var_idx, op);
                            if (!coef) {
                                coef = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
                            }
                            constraint->lhs_terms.push_back(Term{ref.var_idx, std::move(coef), ref.sign});
                        }
                        // RHS vars: extract row-varying coefficients, negate sign (moving to LHS)
                        for (auto &ref : rhs_refs) {
                            auto coef = FindVarCoefficient(*comp.right, ref.var_idx, op);
                            if (!coef) {
                                coef = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
                            }
                            constraint->lhs_terms.push_back(Term{ref.var_idx, std::move(coef), -ref.sign});
                        }

                        // RHS becomes data-only: DECIDE vars replaced with constant 0
                        constraint->rhs_expr = StripDecideVars(*comp.right, op);
                    } else if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
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
        // PackDB: detect quadratic patterns (POWER / x*x / negated / const * POWER)
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
            op.ExtractTerms(*quad_pattern.inner_linear_expr, obj.squared_terms);
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
                    auto distributed = TryDistributeMultiplyOverAdd(func);
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
                        bt.coefficient = BuildCoefficientFromFactors(product.coefficient_factors, *product.mul_func);
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
        op.ExtractTerms(expr, obj.terms);
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
                    op.ExtractTerms(*inner, qg.inner_terms);
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
                                    op.ExtractTerms(*inner, qg.inner_terms);
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
                    auto distributed = TryDistributeMultiplyOverAdd(func);
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
                        bt.coefficient = BuildCoefficientFromFactors(product.coefficient_factors, *product.mul_func);
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
        op.ExtractTerms(expr, constr.lhs_terms);
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
        bool is_avg = (agg.alias == AVG_REWRITE_TAG);

        idx_t before = obj.terms.size();
        idx_t bilinear_before = obj.bilinear_terms.size();
        idx_t squared_before = obj.squared_terms.size();
        ExtractLinearAndBilinearTerms(*agg.children[0], obj, sign, agg.filter.get());
        for (idx_t i = before; i < obj.terms.size(); i++) {
            obj.terms[i].avg_scale = is_avg;
        }
        for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
            obj.bilinear_terms[i].avg_scale = is_avg;
        }
        for (idx_t i = squared_before; i < obj.squared_terms.size(); i++) {
            obj.squared_terms[i].avg_scale = is_avg;
        }
    }

    void AnalyzeObjective(const unique_ptr<Expression>& expr_ptr) {
        auto *expr = expr_ptr.get();
        while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            expr = expr->Cast<BoundCastExpression>().child.get();
        }

        // PackDB: Check for PER wrapper on objective (outermost layer)
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

        // PackDB: Check for WHEN wrapper on objective (inside PER, if present)
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
            if (agg.alias == AVG_REWRITE_TAG) {
                for (idx_t i = before; i < objective->terms.size(); i++) {
                    objective->terms[i].avg_scale = true;
                }
                for (idx_t i = bilinear_before; i < objective->bilinear_terms.size(); i++) {
                    objective->bilinear_terms[i].avg_scale = true;
                }
                for (idx_t i = squared_before; i < objective->squared_terms.size(); i++) {
                    objective->squared_terms[i].avg_scale = true;
                }
            }

            objective->when_condition = std::move(when_cond);
            objective->per_columns = std::move(per_cols);
        } else if (BoundExpressionContainsAggregate(*expr)) {
            objective = make_uniq<Objective>();
            ExtractAggregateObjectiveTerms(*expr, *objective, 1);
            objective->when_condition = std::move(when_cond);
            objective->per_columns = std::move(per_cols);
        }
    }

    //===--------------------------------------------------------------------===//
    // Variable Bounds Extraction (Part 3)
    //===--------------------------------------------------------------------===//

    void ExtractVariableBounds(vector<double> &lower_bounds, vector<double> &upper_bounds) {
        // Traverse decide_constraints to find variable-level bounds
        TraverseBoundsConstraints(*op.decide_constraints, lower_bounds, upper_bounds);
    }

    void TraverseBoundsConstraints(const Expression &expr,
                                   vector<double> &lower_bounds,
                                   vector<double> &upper_bounds) {
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // PackDB PER: only recurse into the constraint (child[0]), skip the columns
                if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
                    TraverseBoundsConstraints(*conj.children[0], lower_bounds, upper_bounds);
                    break;
                }
                // PackDB WHEN: skip entirely — WHEN constraints are conditional (per-row),
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
                            bool absorbed = true;
                            // Apply bound based on comparison type
                            if (comp.type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value);
                            } else if (comp.type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value);
                            } else if (comp.type == ExpressionType::COMPARE_EQUAL) {
                                lower_bounds[var_idx] = bound_value;
                                upper_bounds[var_idx] = bound_value;
                            } else if (comp.type == ExpressionType::COMPARE_LESSTHAN && is_integer_var) {
                                // x < bound → x <= bound-1 for integers. REAL strict
                                // inequality has no valid absorption — leave it for
                                // the constraint path which rejects with a clear error.
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value - 1.0);
                            } else if (comp.type == ExpressionType::COMPARE_GREATERTHAN && is_integer_var) {
                                // x > bound → x >= bound+1 for integers.
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value + 1.0);
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

                        if (!std::isnan(lo)) {
                            if (!between.lower_inclusive && is_integer_var) lo += 1.0;
                            lower_bounds[var_idx] = std::max(lower_bounds[var_idx], lo);
                        }
                        if (!std::isnan(hi)) {
                            if (!between.upper_inclusive && is_integer_var) hi -= 1.0;
                            upper_bounds[var_idx] = std::min(upper_bounds[var_idx], hi);
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

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
    bool bench = std::getenv("PACKDB_BENCH") != nullptr;
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

        // Initialize result storage
        eval_const.row_coefficients.resize(constraint->lhs_terms.size());

        // Scan data and evaluate LHS coefficients
        ColumnDataScanState scan_state;
        gstate.data.InitializeScan(scan_state);

        DataChunk chunk;
        chunk.Initialize(context, gstate.data.Types());

        // Store variable indices for all terms (before scanning data)
        for (auto &term : constraint->lhs_terms) {
            eval_const.variable_indices.push_back(term.variable_index);
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

        // Evaluate RHS
        // RHS can be a constant, an aggregate (scalar), or a row-varying expression (for row-wise constraints)

        if (constraint->rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
            auto &const_expr = constraint->rhs_expr->Cast<BoundConstantExpression>();
            double rhs_constant = const_expr.value.GetValue<double>();
            eval_const.rhs_values.AssignScalar(num_rows, rhs_constant);
        } else {
            // RHS is a complex expression. It might be row-varying (e.g., column ref) or scalar (aggregate).
            // We evaluate it against the data chunks.
            eval_const.rhs_values.Reserve(num_rows);

            const Expression &transformed_rhs =
                CachedTransformToChunkExpression(chunk_expr_cache, *constraint->rhs_expr, context, num_rows);

            // Prepare executor
            ExpressionExecutor rhs_executor(context);
            rhs_executor.AddExpression(transformed_rhs);

            // Scan data and evaluate
            ColumnDataScanState rhs_scan_state;
            gstate.data.InitializeScan(rhs_scan_state);
            DataChunk rhs_chunk;
            rhs_chunk.Initialize(context, gstate.data.Types());

            DataChunk rhs_result;
            vector<LogicalType> result_types = {transformed_rhs.return_type};
            rhs_result.Initialize(context, result_types);
            while (gstate.data.Scan(rhs_scan_state, rhs_chunk)) {
                rhs_result.Reset();
                rhs_executor.Execute(rhs_chunk, rhs_result);
                auto &rhs_col = eval_const.rhs_values.MutableDense();
                ExtractDoubleColumn(rhs_result.data[0], rhs_chunk.size(), 1.0,
                                    rhs_col,
                                    "constraint right-hand side");
                eval_const.rhs_values.SyncSize();
            }
        }

        // PackDB: Unified WHEN+PER row→group assignment
        // Produces row_group_ids and num_groups for the evaluated constraint.
        // - No WHEN, no PER: row_group_ids stays empty, num_groups = 0 (fast path)
        // - WHEN only: row_group_ids[row] = 0 (matching) or INVALID_INDEX (excluded), num_groups = 1
        // - PER only: row_group_ids[row] = 0..K-1 (group id), INVALID_INDEX for NULL PER values, num_groups = K
        // - WHEN+PER: WHEN filters first, then PER groups the remaining rows
        bool has_when = (constraint->when_condition != nullptr);
        bool has_per = (!constraint->per_columns.empty());

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

            if (has_per) {
                LookupOrBuildPerGroupIds(per_group_cache, constraint->per_columns,
                                         chunk_expr_cache, context, gstate.data, num_rows,
                                         /*null_excludes=*/true, row_is_included,
                                         eval_const.row_group_ids, eval_const.num_groups);
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
                // WHEN and/or aggregate-local WHEN (no PER): one group (group 0) for matching rows
                idx_t included_rows = 0;
                for (idx_t row = 0; row < num_rows; row++) {
                    bool inc = row_is_included(row);
                    eval_const.row_group_ids[row] = inc ? 0 : DConstants::INVALID_INDEX;
                    if (inc) included_rows++;
                }
                eval_const.num_groups = 1;
                if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
                    RejectEmptyAggregate(included_rows, "aggregate", "constraint");
                }
            }
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

        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (term_filters[term_idx].avg_scale) {
                ScaleAvgRowCoefficients(eval_const.row_coefficients[term_idx], term_filters[term_idx].has_filter,
                                        term_filters[term_idx].mask);
            }
        }

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

        // PackDB: evaluate both the linear and quadratic-inner term lists so that
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
    solver_input.variable_entity_scope = variable_entity_scope;
    
    // Variable types and bounds
    solver_input.variable_types.resize(num_decide_vars);
    for (idx_t var = 0; var < num_decide_vars; var++) {
        auto &decide_var = decide_variables[var]->Cast<BoundColumnRefExpression>();
        solver_input.variable_types[var] = decide_var.return_type;
    }

    // Bounds were absorbed in the gstate constructor from simple
    // `x OP const` / BETWEEN constraints; those comparisons were skipped in
    // AnalyzeConstraint so we don't re-emit them as per-row model rows.
    solver_input.lower_bounds = gstate.absorbed_lower_bounds;
    solver_input.upper_bounds = gstate.absorbed_upper_bounds;

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
            double M = 1e6;
            for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
                idx_t var_idx = ec.variable_indices[t];
                if (var_idx == DConstants::INVALID_INDEX) continue;
                double ub = solver_input.upper_bounds[var_idx];
                if (ub < 1e20) {
                    double max_coef = 0.0;
                    auto &col = ec.row_coefficients[t];
                    for (idx_t r = 0; r < col.Size(); r++) {
                        max_coef = std::max(max_coef, std::abs(col.Get(r)));
                    }
                    M = std::max(M, max_coef * ub);
                }
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
        double big_M;
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
                // Compute M from variable bounds (only consider active rows)
                double M = 1e6; // default
                for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
                    idx_t var_idx = ec.variable_indices[t];
                    double ub = solver_input.upper_bounds[var_idx];
                    if (ub < 1e20) {
                        double max_coef = 0.0;
                        auto &col = ec.row_coefficients[t];
                        for (idx_t r = 0; r < col.Size(); r++) {
                            if (!ec.row_group_ids.empty() &&
                                ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                                continue;
                            }
                            max_coef = std::max(max_coef, std::abs(col.Get(r)));
                        }
                        M = std::max(M, max_coef * ub);
                    }
                }

                if (ec.lhs_is_aggregate) {
                    // Aggregate NE: defer to after var_indexer is built.
                    // Will be expanded with a single global z per group.
                    // The per-group integer-RHS check (for AVG <> where the
                    // rescaled K*N_g may or may not be integer) lives in the
                    // deferred expansion below; we don't filter here.
                    DeferredAggregateNE deferred;
                    deferred.original = ec; // copy before the loop moves on
                    deferred.big_M = M;
                    deferred_ne_aggregate.push_back(std::move(deferred));
                    // Don't add to new_constraints — handled via global_constraints later
                } else {
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
                    new_constraints.push_back(std::move(ec2));
                }
            } else {
                new_constraints.push_back(std::move(ec));
            }
        }
        gstate.evaluated_constraints = std::move(new_constraints);
    }

    // Generate McCormick Big-M constraints for bilinear auxiliary variables (w = b * x).
    // The structural constraint w <= x was generated at optimizer time.
    // Here we add: w <= U*b and w >= x - U*(1-b), which require the execution-time bound U.
    for (auto &link : bilinear_links) {
        double U = solver_input.upper_bounds[link.other_var_idx];
        if (U >= 1e20) {
            throw InvalidInputException(
                "Bilinear term requires a finite upper bound on variable '%s'. "
                "Add a constraint like '%s <= <bound>' to provide one.",
                decide_variables[link.other_var_idx]->Cast<BoundColumnRefExpression>().alias,
                decide_variables[link.other_var_idx]->Cast<BoundColumnRefExpression>().alias);
        }

        // Constraint: w <= U * b  (i.e., w - U*b <= 0)
        EvaluatedConstraint ec1;
        ec1.variable_indices = {link.aux_idx, link.bool_var_idx};
        ec1.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));
        ec1.row_coefficients.push_back(CoefficientColumn::MakeScalar(-U, num_rows));
        ec1.rhs_values.AssignScalar(num_rows, 0.0);
        ec1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec1.lhs_is_aggregate = false;
        gstate.evaluated_constraints.push_back(std::move(ec1));

        // Constraint: w >= x - U*(1-b) = x - U + U*b
        // Rearranged: w - x + U*b >= -U  →  1*w + (-1)*x + (-U)*b >= -U
        EvaluatedConstraint ec2;
        ec2.variable_indices = {link.aux_idx, link.other_var_idx, link.bool_var_idx};
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(1.0, num_rows));   // +w
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(-1.0, num_rows));  // -x
        ec2.row_coefficients.push_back(CoefficientColumn::MakeScalar(-U, num_rows));    // -U*b
        ec2.rhs_values.AssignScalar(num_rows, -U);
        ec2.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
        ec2.lhs_is_aggregate = false;
        gstate.evaluated_constraints.push_back(std::move(ec2));
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

            // Compute M = max over rows of |rhs[r]| + sum_{t: var != aux} |coeff[t][r]| * max(|lb|, |ub|).
            // This upper-bounds |inner| across all rows and variable values.
            double M = 0.0;
            for (idx_t r = 0; r < num_rows; r++) {
                double row_bound = std::abs(c1.rhs_values.Get(r));
                for (idx_t t = 0; t < c1.variable_indices.size(); t++) {
                    if (c1.variable_indices[t] == link.aux_idx) {
                        continue;
                    }
                    double lb = solver_input.lower_bounds[c1.variable_indices[t]];
                    double ub = solver_input.upper_bounds[c1.variable_indices[t]];
                    if (ub >= 1e20 || lb <= -1e20) {
                        throw InvalidInputException(
                            "ABS over decision variable requires a finite bound on '%s' "
                            "for the Big-M sign-indicator linearization. Add constraints "
                            "'%s >= <lower>' and '%s <= <upper>'. (Triggered by "
                            "MAXIMIZE SUM(ABS(...)) or by a hard-direction ABS constraint "
                            "such as ABS(...) >= K or ABS(...) = K.)",
                            decide_variables[c1.variable_indices[t]]->Cast<BoundColumnRefExpression>().alias,
                            decide_variables[c1.variable_indices[t]]->Cast<BoundColumnRefExpression>().alias,
                            decide_variables[c1.variable_indices[t]]->Cast<BoundColumnRefExpression>().alias);
                    }
                    row_bound += std::abs(c1.row_coefficients[t].Get(r)) * std::max(std::abs(lb), std::abs(ub));
                }
                M = std::max(M, row_bound);
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

        LookupOrBuildPerGroupIds(per_group_cache, gstate.objective->per_columns,
                                 chunk_expr_cache, context, gstate.data, num_rows,
                                 /*null_excludes=*/true, obj_row_is_included,
                                 solver_input.objective_row_group_ids,
                                 solver_input.objective_num_groups);
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
        double M = deferred.big_M;
        bool has_groups = !ec.row_group_ids.empty();

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

        // Base (unscaled) RHS. For AVG(x) <> K we store the original K in rhs_values and
        // multiply by group size per non-empty group below.
        double base_rhs = ec.rhs_values.Get(0);

        for (idx_t g = 0; g < num_groups_to_process; g++) {
            idx_t g_begin = offsets[g];
            idx_t g_end = offsets[g + 1];
            if (g_begin == g_end) {
                continue;
            }
            idx_t g_size = g_end - g_begin;

            double rhs = base_rhs;
            if (ec.ne_avg_rhs_scale) {
                rhs *= static_cast<double>(g_size);
            }

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

            // Allocate one global binary z for this group
            idx_t z_idx = var_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars++;
            solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
            solver_input.global_lower_bounds.push_back(0.0);
            solver_input.global_upper_bounds.push_back(1.0);
            solver_input.global_obj_coeffs.push_back(0.0);

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
            solver_input.global_constraints.push_back(std::move(rc1));

            // ec2: SUM(coeffs) - M*z >= K + 1 - M
            SolverInput::RawConstraint rc2;
            rc2.sense = '>';
            rc2.rhs = rhs + 1.0 - M;
            rc2.indices = std::move(common_indices);
            rc2.coefficients = std::move(common_coefs);
            rc2.indices.push_back(static_cast<int>(z_idx));
            rc2.coefficients.push_back(-M);
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

    // Compute Big-M from variable bounds (shared by both paths)
    auto compute_big_m = [&]() -> double {
        double M = 1e6;
        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
            idx_t var = saved_obj_var_indices[t];
            double ub = solver_input.upper_bounds[var];
            if (ub < 1e20) {
                double max_coef = 0.0;
                auto &col = saved_obj_coefficients[t];
                for (idx_t r = 0; r < col.Size(); r++) {
                    max_coef = std::max(max_coef, std::abs(col.Get(r)));
                }
                M = std::max(M, max_coef * ub);
            }
        }
        return M;
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
                        SolverInput::RawConstraint rc;
                        rc.sense = sense_char;
                        rc.rhs = 0.0;
                        rc.indices.push_back((int)group_value_indices[g]);
                        rc.coefficients.push_back(1.0);
                        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                            double coeff = saved_obj_coefficients[t][row];
                            if (std::abs(coeff) < 1e-15) continue;
                            idx_t var_idx = var_indexer.Get(saved_obj_var_indices[t], row);
                            rc.indices.push_back((int)var_idx);
                            rc.coefficients.push_back(-coeff);
                        }
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
                        SolverInput::RawConstraint rc;
                        rc.indices.push_back((int)group_value_indices[g]);
                        rc.coefficients.push_back(1.0);
                        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                            double coeff = saved_obj_coefficients[t][row];
                            if (std::abs(coeff) < 1e-15) continue;
                            idx_t var_idx = var_indexer.Get(saved_obj_var_indices[t], row);
                            rc.indices.push_back((int)var_idx);
                            rc.coefficients.push_back(-coeff);
                        }
                        idx_t y_idx = first_y + active_idx;
                        if (inner_is_min) {
                            // MINIMIZE MIN inner: z_g - expr_r - M*y_r >= -M
                            rc.indices.push_back((int)y_idx);
                            rc.coefficients.push_back(-M);
                            rc.sense = '>';
                            rc.rhs = -M;
                        } else {
                            // MAXIMIZE MAX inner: z_g - expr_r + M*y_r <= M
                            rc.indices.push_back((int)y_idx);
                            rc.coefficients.push_back(M);
                            rc.sense = '<';
                            rc.rhs = M;
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
                // Big-M for outer: use a large value (z_g's are bounded by inner M)
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
                    SolverInput::RawConstraint rc;
                    rc.sense = sense_char;
                    rc.rhs = 0.0;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) { idx_t row = obj_flat_rows[k];
                        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                            double coeff = saved_obj_coefficients[t][row];
                            if (per_inner_was_avg) {
                                coeff /= static_cast<double>(group_size(g));
                            }
                            if (std::abs(coeff) < 1e-15) continue;
                            idx_t var_idx = var_indexer.Get(saved_obj_var_indices[t], row);
                            rc.indices.push_back((int)var_idx);
                            rc.coefficients.push_back(-coeff);
                        }
                    }
                    // Skip vacuous w op 0 rows: outer MIN/MAX of group sums settles
                    // dominated zero-sum groups via the optimization direction itself.
                    if (rc.indices.size() == 1) continue;
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
                double M_outer = compute_big_m() * num_rows; // group sums can be large
                idx_t first_u = w_idx + 1;
                solver_input.num_global_vars += K;
                for (idx_t g = 0; g < K; g++) {
                    solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                    solver_input.global_lower_bounds.push_back(0.0);
                    solver_input.global_upper_bounds.push_back(1.0);
                    solver_input.global_obj_coeffs.push_back(0.0);
                }
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    for (idx_t k = obj_offsets[g]; k < obj_offsets[g + 1]; k++) { idx_t row = obj_flat_rows[k];
                        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                            double coeff = saved_obj_coefficients[t][row];
                            if (per_inner_was_avg) {
                                coeff /= static_cast<double>(group_size(g));
                            }
                            if (std::abs(coeff) < 1e-15) continue;
                            idx_t var_idx = var_indexer.Get(saved_obj_var_indices[t], row);
                            rc.indices.push_back((int)var_idx);
                            rc.coefficients.push_back(-coeff);
                        }
                    }
                    idx_t u_idx = first_u + g;
                    if (outer_is_min) {
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(-M_outer);
                        rc.sense = '>';
                        rc.rhs = -M_outer;
                    } else {
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
                SolverInput::RawConstraint rc;
                rc.sense = sense_char;
                rc.rhs = 0.0;
                rc.indices.push_back((int)z_idx);
                rc.coefficients.push_back(1.0);
                for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                    double coeff = saved_obj_coefficients[t].Get(row);
                    if (std::abs(coeff) < 1e-15) continue;
                    idx_t var_idx = var_indexer.Get(saved_obj_var_indices[t], row);
                    rc.indices.push_back((int)var_idx);
                    rc.coefficients.push_back(-coeff);
                }
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
                SolverInput::RawConstraint rc;
                rc.indices.push_back((int)z_idx);
                rc.coefficients.push_back(1.0);
                for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                    double coeff = saved_obj_coefficients[t].Get(row);
                    if (std::abs(coeff) < 1e-15) continue;
                    idx_t var_idx = var_indexer.Get(saved_obj_var_indices[t], row);
                    rc.indices.push_back((int)var_idx);
                    rc.coefficients.push_back(-coeff);
                }
                idx_t y_idx = first_y_idx + a;
                if (is_min_agg) {
                    rc.indices.push_back((int)y_idx);
                    rc.coefficients.push_back(-M);
                    rc.sense = '>';
                    rc.rhs = -M;
                } else {
                    rc.indices.push_back((int)y_idx);
                    rc.coefficients.push_back(M);
                    rc.sense = '<';
                    rc.rhs = M;
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

    // ================================================================
    // Composed MIN/MAX constraints: additive LHS mixing SUM/AVG/MIN/MAX.
    // Each MIN/MAX term gets a global auxiliary z_k pinned by per-row
    // constraints. The outer composed constraint is emitted as a
    // RawConstraint summing SUM/AVG contributions + z_k references.
    // v1 scope: easy cases only (MAX pushed down / MIN pushed up),
    // constant RHS, no outer WHEN/PER wrappers.
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

        for (auto &spec : composed_minmax_constraints) {
            // RHS must be constant (possibly cast-wrapped) in v1.
            const Expression *rhs_inner = spec.rhs_expr.get();
            while (rhs_inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                rhs_inner = rhs_inner->Cast<BoundCastExpression>().child.get();
            }
            if (rhs_inner->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
                throw BinderException(
                    "Composed MIN/MAX in DECIDE v1 requires a constant RHS; got '%s'.",
                    spec.rhs_expr->ToString());
            }
            double rhs_val = rhs_inner->Cast<BoundConstantExpression>()
                                 .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();

            struct TermAnalysis {
                LogicalDecide::ComposedMinMaxTerm::Kind kind;
                string agg_name;
                int sign;
                bool is_easy;
                vector<bool> filter_mask;
                vector<Term> inner_terms;
                vector<vector<double>> per_term_coefs;
                idx_t z_idx = DConstants::INVALID_INDEX;
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

                ExtractTerms(*term.inner_expr, ta.inner_terms);
                for (auto &inner_t : ta.inner_terms) {
                    ta.per_term_coefs.push_back(EvaluateTermCoefs(inner_t));
                }
                if (term.filter) {
                    ta.filter_mask = std::move(composed_masks[mask_slot++]);
                } else {
                    ta.filter_mask.assign(num_rows, true);
                }
                analyses.push_back(std::move(ta));
            }

            // Allocate global z_k for each MIN/MAX term. Reject hard cases in v1.
            for (auto &ta : analyses) {
                if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                if (!ta.is_easy) {
                    throw BinderException(
                        "Composed MIN/MAX in DECIDE v1 supports only easy-direction "
                        "MIN/MAX terms (MAX pushed down by <=, MIN pushed up by >=). "
                        "The '%s' term here requires Big-M indicator linearization, "
                        "which is not yet implemented for composed expressions.",
                        ta.agg_name);
                }
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

            // Emit per-row pinning constraints for each MIN/MAX term.
            for (auto &ta : analyses) {
                if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                bool is_max = (ta.agg_name == "max");
                // Easy case:
                //   MAX pushed down: z_k >= inner_expr  (solver drives z_k to max)
                //   MIN pushed up:   z_k <= inner_expr  (solver drives z_k to min)
                char sense = is_max ? '>' : '<';
                for (idx_t row = 0; row < num_rows; row++) {
                    if (!ta.filter_mask[row]) continue;
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)ta.z_idx);
                    rc.coefficients.push_back(1.0);
                    double row_rhs = 0.0;
                    for (idx_t it = 0; it < ta.inner_terms.size(); it++) {
                        auto &inner_t = ta.inner_terms[it];
                        double coef = ta.per_term_coefs[it][row];
                        if (inner_t.variable_index == DConstants::INVALID_INDEX) {
                            row_rhs += coef;
                        } else {
                            idx_t abs_idx = var_indexer.Get(inner_t.variable_index, row);
                            rc.indices.push_back((int)abs_idx);
                            rc.coefficients.push_back(-coef);
                        }
                    }
                    rc.sense = sense;
                    rc.rhs = row_rhs;
                    solver_input.global_constraints.push_back(std::move(rc));
                }
            }

            // Build the outer composed RawConstraint
            std::unordered_map<int, double> outer_accum;
            double outer_rhs = rhs_val;
            for (auto &ta : analyses) {
                if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
                    outer_accum[(int)ta.z_idx] += (double)ta.sign;
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
                            double coef = ta.per_term_coefs[it][row] * (double)ta.sign / avg_divisor;
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

        // Clear any existing objective terms — the placeholder constant produced
        // none, but be defensive in case other paths populated them.
        solver_input.objective_coefficients.clear();
        solver_input.objective_variable_indices.clear();

        struct ObjTermAnalysis {
            LogicalDecide::ComposedMinMaxTerm::Kind kind;
            string agg_name;
            int sign;
            bool is_easy;
            vector<bool> filter_mask;
            vector<Term> inner_terms;
            vector<vector<double>> per_term_coefs;
            idx_t z_idx = DConstants::INVALID_INDEX;
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
                ExtractTerms(*term.inner_expr, ta.inner_terms);
                for (auto &inner_t : ta.inner_terms) {
                    ta.per_term_coefs.push_back(EvaluateTermCoefsObj(inner_t));
                }
                if (term.filter) {
                    ta.filter_mask = std::move(obj_comp_masks[mask_slot++]);
                } else {
                    ta.filter_mask.assign(num_rows, true);
                }
                obj_analyses.push_back(std::move(ta));
            }
        }

        // Allocate z_k per MIN/MAX term. v1 rejects hard direction.
        for (auto &ta : obj_analyses) {
            if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
            if (!ta.is_easy) {
                throw BinderException(
                    "Composed MIN/MAX objective in DECIDE v1 supports only "
                    "easy-direction terms (MAXIMIZE+MIN or MINIMIZE+MAX). "
                    "The '%s' term here requires indicator linearization, "
                    "which is not yet implemented for composed objectives.",
                    ta.agg_name);
            }
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
                SolverInput::RawConstraint rc;
                rc.indices.push_back((int)ta.z_idx);
                rc.coefficients.push_back(1.0);
                double row_rhs = 0.0;
                for (idx_t it = 0; it < ta.inner_terms.size(); it++) {
                    auto &inner_t = ta.inner_terms[it];
                    double coef = ta.per_term_coefs[it][row];
                    if (inner_t.variable_index == DConstants::INVALID_INDEX) {
                        row_rhs += coef;
                    } else {
                        idx_t abs_idx = var_indexer.Get(inner_t.variable_index, row);
                        rc.indices.push_back((int)abs_idx);
                        rc.coefficients.push_back(-coef);
                    }
                }
                rc.sense = sense;
                rc.rhs = row_rhs;
                solver_input.global_constraints.push_back(std::move(rc));
            }
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
                solver_input.global_obj_coeffs[gslot] = (double)ta.sign;
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
                        dst[row] += ta.per_term_coefs[it][row] * (double)ta.sign / avg_divisor;
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
    size_t bench_total_constraints = solver_input.constraints.size() + solver_input.global_constraints.size();

    if (bench) {
        model_timer.End();
        solver_timer.Start();
    }

    gstate.ilp_solution = SolveModel(solver_input, var_indexer);
    // Move the indexer onto gstate now that solve is complete; readback in
    // Execute() needs it to outlive solver_input.
    gstate.var_indexer = std::move(var_indexer);

    if (bench) {
        solver_timer.End();
        fprintf(stderr, "PACKDB_BENCH: model_construction_ms=%.2f\n", model_timer.Elapsed() * 1000.0);
        fprintf(stderr, "PACKDB_BENCH: solver_ms=%.2f\n", solver_timer.Elapsed() * 1000.0);
        fprintf(stderr, "PACKDB_BENCH: total_variables=%zu\n", bench_total_vars);
        fprintf(stderr, "PACKDB_BENCH: total_constraints=%zu\n", bench_total_constraints);
        fprintf(stderr, "PACKDB_BENCH: num_rows=%zu\n", (size_t)num_rows);
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
