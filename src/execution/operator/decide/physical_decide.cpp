#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/execution/operator/decide/decide_frame_map.hpp"
#include "duckdb/execution/operator/decide/decide_row_evaluation.hpp"
#include "duckdb/execution/executor.hpp"
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
#include "duckdb/common/decide_profile.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/decidb/solver/ilp_solver.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic_engines.hpp"
#include "duckdb/decidb/diagnostics/decide_router.hpp"
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
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/decidb/solver/solver_config.hpp"
// The slow-solve report needs peak process memory, and its prompt needs to know
// whether stdin is a terminal. Both are platform primitives; MSVC has neither
// <sys/resource.h> nor <unistd.h>. Same split as gurobi_loader.cpp.
#ifdef _WIN32
#include <cstdio>    // _fileno
#include <io.h>      // _isatty
#else
#include <sys/resource.h>  // getrusage — peak process memory
#include <unistd.h>        // isatty / STDIN_FILENO
#endif
#include <iostream>  // std::cin / std::getline — read the continuation decision at a time-limit stop

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Transform Helpers
//===--------------------------------------------------------------------===//
// These static functions replace BoundColumnRefExpression nodes with
// BoundReferenceExpression nodes so that DuckDB's ExpressionExecutor can
// evaluate expressions against data chunks (which use positional indices).

//! Collect every reducer in an expression outside reducers. Descent stops at a match: a
//! reducer inside another is read per group of its path (syntax_reference.md §5.5), which the one around it
//! evaluates (EvaluateRhsReducerPerGroup).
static void CollectReducers(const Expression &expr, vector<const Expression *> &out) {
	idx_t frame;
	if (TryGetDecideFrame(expr, frame)) {
		// Stage 05 lowers every frame to the columns it reads (LowerFramesInPreparedModel).
		throw InternalException("DECIDE: frame selector %llu reached evaluation unlowered: %s", frame,
		                        expr.ToString());
	}
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
const Expression &CachedTransformToChunkExpression(ChunkExprCache &cache,
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

//! The pieces a failed extraction needs in order to name what was NULL: the expression
//! as the user wrote it, and the input chunk it was evaluated against.
//!
//! The tree handed to the ExpressionExecutor is no use for this. `TransformToChunkExpression`
//! rewrites every BoundColumnRefExpression into a positional BoundReferenceExpression, so
//! by evaluation time the names are gone. The ORIGINAL tree still carries them, and its
//! leaves still carry the same `binding.column_index` the transform used, which is exactly
//! the chunk column to test. Keeping the original beside the chunk is therefore enough to
//! turn "something was NULL" into "this column was NULL".
struct NullSourceContext {
	const Expression *source_expr = nullptr;
	const DataChunk *input_chunk = nullptr;
	//! Set by a scoped clause, whose messages quote its SQL only (syntax_reference.md
	//! §10.1): a flattened subquery is named by its source fragment, as DuckDB prints it.
	const vector<string> *fragments = nullptr;
};

//! Whether `expr` is a leaf reading a chunk column: its column and the name it carries.
//! Both leaf spellings appear here. A coefficient reaches execution already lowered to
//! positional BoundReferenceExpressions in the common case, and it is only their retained
//! alias that makes a message readable; a BoundColumnRefExpression survives on the paths
//! that never went through that lowering. The two carry the chunk column in different
//! fields, so each is read from its own.
static bool ChunkColumnLeaf(const Expression &expr, idx_t &col, string &name) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		col = colref.binding.column_index;
		name = colref.GetAlias();
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		col = ref.index;
		name = ref.GetAlias();
		return true;
	}
	return false;
}

//! The source fragment of a flattened subquery inside `expr` that is NULL on chunk row
//! `row`, or empty if none is.
static string NullSubqueryAtRow(const Expression &expr, const DataChunk &chunk, idx_t row,
                                const vector<string> &fragments) {
	idx_t col;
	string name;
	if (!ChunkColumnLeaf(expr, col, name)) {
		string found;
		ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
			if (found.empty()) {
				found = NullSubqueryAtRow(child, chunk, row, fragments);
			}
		});
		return found;
	}
	idx_t fragment_id;
	if (!TryParseSourceFragmentTag(name, fragment_id) || fragment_id >= fragments.size() ||
	    col >= chunk.ColumnCount() || !chunk.data[col].GetValue(row).IsNull()) {
		return string();
	}
	return fragments[fragment_id];
}

//! Names the columns inside `expr` that are NULL on chunk row `row`, skipping duplicates.
//! Reached only when an extraction has already failed, so it reads values one at a time
//! rather than making every successful scan carry per-leaf bookkeeping. A leaf with no
//! name to print is left to the caller's expression-quoting fallback rather than reported
//! as `#3`.
static void CollectNullColumnsAtRow(const Expression &expr, const DataChunk &chunk, idx_t row,
                                    vector<string> &out) {
	idx_t col = DConstants::INVALID_INDEX;
	string name;
	if (!ChunkColumnLeaf(expr, col, name)) {
		ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
			CollectNullColumnsAtRow(child, chunk, row, out);
		});
		return;
	}
	if (name.empty() || col >= chunk.ColumnCount() || !chunk.data[col].GetValue(row).IsNull()) {
		return;
	}
	for (auto &seen : out) {
		if (seen == name) {
			return;
		}
	}
	out.push_back(std::move(name));
}

//! The NULL message, in the terms the user wrote the query in: name the column that is
//! NULL, and give the two ways out — impute a value, or drop the row.
//!
//! A NULL that no column explains is possible (`NULLIF`, a TRY_CAST that failed): there
//! is no column to name then, so the expression is quoted instead. The advice is the same
//! either way, which is why the two branches differ only in what they point at.
static string DescribeNullSource(const NullSourceContext *ctx, idx_t row) {
	if (ctx && ctx->source_expr && ctx->input_chunk && ctx->fragments) {
		auto subquery = NullSubqueryAtRow(*ctx->source_expr, *ctx->input_chunk, row, *ctx->fragments);
		if (!subquery.empty()) {
			return StringUtil::Format("DECIDE: the subquery %s is NULL. Impute it with COALESCE(%s, 0), or filter "
			                          "those rows out with a WHERE clause.",
			                          subquery, subquery);
		}
	}
	vector<string> null_columns;
	if (ctx && ctx->source_expr && ctx->input_chunk) {
		CollectNullColumnsAtRow(*ctx->source_expr, *ctx->input_chunk, row, null_columns);
	}
	if (null_columns.empty()) {
		string what = (ctx && ctx->source_expr) ? ctx->source_expr->ToString() : string();
		if (what.empty()) {
			return "DECIDE: a value used in the optimization is NULL. Impute it with COALESCE(), "
			       "or filter those rows out with a WHERE clause.";
		}
		return StringUtil::Format("DECIDE: %s is NULL. Impute it with COALESCE(), or filter those "
		                          "rows out with a WHERE clause.", what);
	}
	if (null_columns.size() == 1) {
		return StringUtil::Format("DECIDE: column \"%s\" is NULL. Impute it with COALESCE(%s, 0) "
		                          "or filter those rows out with a WHERE clause.", null_columns[0],
		                          null_columns[0]);
	}
	string names;
	for (idx_t i = 0; i < null_columns.size(); i++) {
		if (i > 0) {
			names += (i + 1 == null_columns.size()) ? " and " : ", ";
		}
		names += "\"" + null_columns[i] + "\"";
	}
	return StringUtil::Format("DECIDE: columns %s are NULL. Impute them with COALESCE(), or filter "
	                          "those rows out with a WHERE clause.", names);
}

//! Runs each expression of `executor` over `chunk`, whose first row is row `row_offset` of
//! the data, into `results`: expression i only on the rows `*reads[i]` marks (every row
//! when null), NULL on the others. A scoped clause filters before it generates
//! (syntax_reference.md §6.1), so nothing is computed on a row its `when` drops:
//! `when w > 0: x <= ln(w)` takes no logarithm of 0.
static void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset,
                              const vector<const vector<bool> *> &reads, DataChunk &results) {
	const idx_t count = chunk.size();
	results.SetCardinality(count);
	if (count == 0) {
		return;
	}
	SelectionVector read_sel(count);
	for (idx_t expr_idx = 0; expr_idx < results.ColumnCount(); expr_idx++) {
		auto &result = results.data[expr_idx];
		auto *rows = reads[expr_idx];
		idx_t read_count = 0;
		for (idx_t i = 0; i < count; i++) {
			if (!rows || (*rows)[row_offset + i]) {
				read_sel.set_index(read_count++, i);
			}
		}
		if (read_count == count) {
			executor.SetChunk(chunk);
			executor.ExecuteExpression(expr_idx, result);
			continue;
		}
		// The read rows' values, then one NULL slot every other row points at.
		Vector values(result.GetType(), read_count + 1);
		if (read_count > 0) {
			DataChunk read_chunk;
			read_chunk.InitializeEmpty(chunk.GetTypes());
			read_chunk.Slice(chunk, read_sel, read_count);
			Vector read_result(result.GetType());
			executor.SetChunk(read_chunk);
			executor.ExecuteExpression(expr_idx, read_result);
			VectorOperations::Copy(read_result, values, read_count, 0, 0);
		}
		FlatVector::SetNull(values, read_count, true);
		// The result keeps this selection, so each expression gets its own.
		SelectionVector back(count);
		for (idx_t i = 0, slot = 0; i < count; i++) {
			bool read = slot < read_count && read_sel.get_index(slot) == i;
			back.set_index(i, read ? slot++ : read_count);
		}
		result.Slice(values, back, count);
	}
}

//! The same, with one set of rows for every expression; without one, every row.
void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset, const vector<bool> *rows,
                       DataChunk &results) {
	if (!rows) {
		executor.Execute(chunk, results);
		return;
	}
	ExecuteOnReadRows(executor, chunk, row_offset, vector<const vector<bool> *>(results.ColumnCount(), rows),
	                  results);
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
                                bool allow_infinite = false,
                                const NullSourceContext *null_ctx = nullptr,
                                const vector<bool> *read_rows = nullptr, idx_t row_offset = 0) {
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
		// A row the clause never reads (outside its `when`, or a reducer's) contributes
		// nothing, whatever it holds.
		if (read_rows && !(*read_rows)[row_offset + i]) {
			out.push_back(0.0);
			continue;
		}
		idx_t idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			throw InvalidInputException(DescribeNullSource(null_ctx, i));
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
//!   (a NULL key value is a value: NULL-keyed rows form one group, D5)
//!   out_row_to_group — sized num_rows; INVALID_INDEX for excluded rows, [0..K) otherwise
//!   out_num_groups   — K, count of distinct groups in input order
//!   read_rows        — the clause's kept rows: the key is computed on these only, and no
//!                      other row joins a group (null = every row)
//!
//! A key with no columns (`per ()`, `by ()`) has one class: every row the filter keeps.
void BuildGroupIds(const vector<const Expression *> &key_exprs,
                   ClientContext &context,
                   ColumnDataCollection &data,
                   idx_t num_rows,
                   const std::function<bool(idx_t)> &row_filter,
                   vector<idx_t> &out_row_to_group,
                   idx_t &out_num_groups,
                   vector<vector<Value>> *out_rep_keys,
                   const vector<bool> *read_rows) {
	DecideProfileScope profile("extraction.group_ids");
	out_row_to_group.assign(num_rows, DConstants::INVALID_INDEX);
	out_num_groups = 0;
	if (out_rep_keys) {
		out_rep_keys->assign(key_exprs.size(), vector<Value>());
	}
	if (num_rows == 0) {
		return;
	}
	if (key_exprs.empty()) {
		for (idx_t row = 0; row < num_rows; row++) {
			if ((!read_rows || (*read_rows)[row]) && (!row_filter || row_filter(row))) {
				out_row_to_group[row] = 0;
				out_num_groups = 1;
			}
		}
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
		ExecuteOnReadRows(key_executor, chunk, row_offset, read_rows, key_results);

		VectorOperations::Hash(key_results.data[0], chunk_hashes, count);
		for (idx_t c = 1; c < num_key_cols; c++) {
			VectorOperations::CombineHash(chunk_hashes, key_results.data[c], count);
		}
		// A key that is the same on every row (`by (1)`, a folded expression) hashes to a
		// constant vector holding one value; each row reads its own slot below.
		chunk_hashes.Flatten(count);
		auto hashes_data = FlatVector::GetData<hash_t>(chunk_hashes);

		// UnifiedVectorFormat lets us check NULLness without materializing a Value.
		for (idx_t c = 0; c < num_key_cols; c++) {
			key_results.data[c].ToUnifiedFormat(count, key_udatas[c]);
		}

		for (idx_t i = 0; i < count; i++) {
			idx_t row = row_offset + i;
			if ((read_rows && !(*read_rows)[row]) || (row_filter && !row_filter(row))) {
				continue;
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
//! private phase methods need PerGroupCache in their signatures). Cache for group
//! assignments: shares one full-data scan + group-map build across constraints/
//! objectives that use the same key expression set. The cached value is the
//! *unfiltered* row→group mapping (BuildGroupIds run with row_filter=nullptr); each
//! call site then applies its own `when` filter and remaps the surviving group IDs
//! to consecutive 0..K' to preserve today's "encounter-order, no holes" semantics.
//! Lifetime: one PerGroupCache per Finalize invocation; cleared at end.

//! Printable key for an (unfiltered) group: the per-key-column representative values
//! joined with ", " (composite key → `EU, 2024`); a NULL value renders "NULL".
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

//! Hash a key expression set into a size_t for the cache.
static size_t HashPerKey(const vector<unique_ptr<Expression>> &per_columns) {
	size_t h = 0;
	for (auto &e : per_columns) {
		size_t eh = static_cast<size_t>(e->Hash());
		h ^= eh + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
	}
	return h;
}

//! Returns true if the cache entry's expression set matches the key being evaluated.
static bool PerKeyMatches(const PerGroupCacheEntry &entry,
                          const vector<unique_ptr<Expression>> &per_columns) {
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
//! false get INVALID_INDEX in the output.
//! Surviving group IDs are remapped to consecutive 0..K' in their first-seen
//! row order so downstream code sees a dense [0..K') range exactly as today.
//!
//! `read_rows` is the clause's kept rows, which hold every row either filter keeps: the
//! key is computed on them only, so a clause that drops rows builds its groups afresh
//! instead of sharing the cache's, whose key was computed on every row.
static void LookupOrBuildPerGroupIds(PerGroupCache &cache,
                                     const vector<unique_ptr<Expression>> &per_columns,
                                     ChunkExprCache &chunk_expr_cache,
                                     ClientContext &context,
                                     ColumnDataCollection &data,
                                     idx_t num_rows,
                                     const std::function<bool(idx_t)> &row_filter,
                                     const vector<bool> *read_rows,
                                     vector<idx_t> &out_row_group_ids,
                                     idx_t &out_num_groups,
                                     vector<string> &out_group_labels,
                                     // Optional second map over the SAME group numbering but a
                                     // looser row filter. The right-hand side needs this: its
                                     // reducers must not inherit the LHS reducers' own `when`,
                                     // which `row_filter` folds in. Groups that no strict row
                                     // reached emit no constraint, so their rows stay excluded.
                                     const std::function<bool(idx_t)> *loose_row_filter = nullptr,
                                     vector<idx_t> *out_loose_row_group_ids = nullptr,
                                     // Each group's key values ([group][key column]), for a
                                     // message that quotes them as SQL literals.
                                     vector<vector<Value>> *out_group_values = nullptr) {
	// Build unfiltered group ids (over `read_rows` when given).
	auto build = [&](PerGroupCacheEntry &built, const vector<bool> *rows) {
		vector<const Expression *> key_exprs;
		key_exprs.reserve(per_columns.size());
		for (auto &col : per_columns) {
			key_exprs.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, *col, context));
		}
		std::function<bool(idx_t)> no_filter; // empty = include all
		BuildGroupIds(key_exprs, context, data, num_rows, no_filter, built.unfiltered_row_group_ids,
		              built.unfiltered_num_groups, &built.unfiltered_rep_keys, rows);
	};
	PerGroupCacheEntry read_entry;
	PerGroupCacheEntry *entry = nullptr;
	if (read_rows && std::find(read_rows->begin(), read_rows->end(), false) != read_rows->end()) {
		build(read_entry, read_rows);
		entry = &read_entry;
	} else {
		size_t key = HashPerKey(per_columns);
		auto &bucket = cache[key];
		for (auto &e : bucket) {
			if (PerKeyMatches(e, per_columns)) {
				entry = &e;
				break;
			}
		}
		if (entry == nullptr) {
			bucket.emplace_back();
			entry = &bucket.back();
			entry->exprs.reserve(per_columns.size());
			for (auto &e : per_columns) {
				entry->exprs.push_back(e.get());
			}
			build(*entry, nullptr);
		}
	}

	// Apply per-call filter and remap surviving group IDs to consecutive 0..K'
	// in encounter order, the dense numbering every consumer expects.
	out_row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
	out_group_labels.clear();
	if (out_group_values) {
		out_group_values->clear();
	}
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
			if (out_group_values) {
				vector<Value> values;
				for (auto &column : entry->unfiltered_rep_keys) {
					values.push_back(unf_gid < column.size() ? column[unf_gid] : Value());
				}
				out_group_values->push_back(std::move(values));
			}
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


//! What a signature's FRAME field reads (D76): the frames, their maps over the input, and
//! the columns and decisions their fallbacks read.
struct SignatureFrames {
	const vector<DecideFrameSpec> &frames;
	const vector<DecideFrameMap> &maps;
	const DecidePreparedModel &prepared;
};

//! Each row `rows` keeps (null: every row) numbered by its variant (syntax_reference.md
//! §7.1, D64): the class of `signature`'s key, split by the value of each of its fields --
//! NULL a value, a decision by its instance, a frame by its keys (D76). Numbered from 0 in
//! first-seen row order; INVALID_INDEX on every other row, where no field is computed.
static void BuildSignatureIds(const DecideSignature &signature, const vector<DecideGroupKey> &group_keys,
                              const vector<DecideVarScopeInfo> &variable_scopes,
                              const vector<EntityMapping> &entity_mappings, const SignatureFrames &frames,
                              ChunkExprCache &chunk_expr_cache, ClientContext &context, ColumnDataCollection &data,
                              idx_t num_rows, const vector<bool> *rows, vector<idx_t> &ids, idx_t &count) {
	vector<const Expression *> keys;
	auto add = [&](const Expression &expr) {
		keys.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, expr, context));
	};
	for (auto &column : signature.key) {
		add(*column);
	}
	vector<const vector<idx_t> *> classes;
	vector<unique_ptr<Expression>> frame_columns;
	vector<unique_ptr<vector<idx_t>>> frame_classes;
	bool per_row = false;
	for (auto &field : signature.fields) {
		switch (field.kind) {
		case DecideSignatureFieldKind::VALUE:
			add(*field.value);
			break;
		case DecideSignatureFieldKind::BY_KEY:
			for (auto &expr : group_keys[field.group_key].expressions) {
				add(*expr);
			}
			break;
		case DecideSignatureFieldKind::DECISION: {
			if (field.decision >= variable_scopes.size()) {
				per_row = true; // no scope recorded: one instance per row
				break;
			}
			auto &scope = variable_scopes[field.decision];
			if (scope.IsEntity()) {
				classes.push_back(&entity_mappings[scope.entity_scope_idx].row_to_entity);
			} else if (!scope.IsScalar()) {
				per_row = true;
			}
			break;
		}
		case DecideSignatureFieldKind::FRAME: {
			// Its timeline -- its anchor's position there, when it reads relative to the
			// anchor --, and what its fallback reads where the anchor reaches it: its data (a
			// frame over data's VALUE there, a frame over decisions' FALLBACK coefficients),
			// and its decisions' instances (D76).
			auto &frame = frames.frames[field.frame];
			auto &map = frames.maps[field.frame];
			classes.push_back(frame.Relative() ? &map.peer : &map.timeline);
			if (!frame.fallback) {
				break;
			}
			auto &columns = frames.prepared.frame_columns;
			vector<const Expression *> reached;
			for (idx_t j = 0; j < columns.size(); j++) {
				const bool fallback_data = columns[j].kind == DecideFrameColumn::Kind::VALUE ||
				                           columns[j].kind == DecideFrameColumn::Kind::FALLBACK;
				if (columns[j].frame == field.frame && fallback_data) {
					frame_columns.push_back(
					    make_uniq<BoundReferenceExpression>(columns[j].type, frames.prepared.frame_column_base + j));
					reached.push_back(frame_columns.back().get());
				}
			}
			if (!reached.empty()) {
				frame_classes.push_back(make_uniq<vector<idx_t>>());
				idx_t reached_count;
				BuildGroupIds(reached, context, data, num_rows, [&](idx_t row) { return map.Missing(row); },
				              *frame_classes.back(), reached_count, nullptr, rows);
				classes.push_back(frame_classes.back().get());
			}
			for (auto decision : frames.prepared.frame_fallback_decisions[field.frame]) {
				auto scope = decision < variable_scopes.size() ? variable_scopes[decision] : DecideVarScopeInfo::Row();
				if (scope.IsScalar()) {
					continue;
				}
				frame_classes.push_back(make_uniq<vector<idx_t>>(num_rows, num_rows));
				auto &reached = *frame_classes.back();
				for (idx_t row = 0; row < num_rows; row++) {
					if (map.Missing(row)) {
						reached[row] =
						    scope.IsEntity() ? entity_mappings[scope.entity_scope_idx].row_to_entity[row] : row;
					}
				}
				classes.push_back(&reached);
			}
			break;
		}
		}
	}
	BuildGroupIds(keys, context, data, num_rows, std::function<bool(idx_t)> {}, ids, count, nullptr, rows);
	if (per_row) {
		// A per-row decision: every kept row is an instance of its own.
		count = 0;
		for (auto &id : ids) {
			if (id != DConstants::INVALID_INDEX) {
				id = count++;
			}
		}
		return;
	}
	// A keyed decision splits each variant by the decision's class.
	struct PairHash {
		size_t operator()(const std::pair<idx_t, idx_t> &pair) const {
			return std::hash<idx_t>()(pair.first) * 0x9e3779b97f4a7c15ULL ^ std::hash<idx_t>()(pair.second);
		}
	};
	for (auto *row_class : classes) {
		std::unordered_map<std::pair<idx_t, idx_t>, idx_t, PairHash> numbered;
		idx_t next = 0;
		for (idx_t row = 0; row < num_rows; row++) {
			if (ids[row] == DConstants::INVALID_INDEX) {
				continue;
			}
			auto inserted = numbered.emplace(std::make_pair(ids[row], (*row_class)[row]), next);
			if (inserted.second) {
				next++;
			}
			ids[row] = inserted.first->second;
		}
		count = next;
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

// Reduce an aggregate constraint's right-hand side to one value per group.
//
// A reduced constraint emits ONE row per group, but the RHS is still a column, so a
// bound that varies row to row has to collapse. Paper §3.2.1 fixes the semantics:
// with no `per` key, the clause generates one instance per tuple, each pairing the reducer
// (evaluated over the whole selection) with that tuple's non-reduced values. The
// conjunction `LHS <= r_i` over every row is exactly `LHS <= min r_i`, so MIN for
// `<=` and MAX for `>=` is a derivation, not a policy choice. Over a `by` key the
// same rule applies per group.
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
// `group_ids` is the RHS's own map — the constraint's `when` and `per`, deliberately
// without the LHS reducers' own `when` filters, the same map the RHS reducers are
// evaluated over. A reducer's own `when` scopes that reducer, not the per-tuple
// fan-out, so `sum(when a: x) <= cap` takes the tightest `cap` over every selected row
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
    // `<>` does not collapse (standing decision 3): every excluded value is kept, not
    // just the tightest. So a varying bound under `<>` is left exactly as it arrived —
    // raw, per row — and ExpandAggregateNotEqual (ilp_linearization.cpp) reads the
    // distinct values straight out of it, one Big-M disjunction per value per group.
    const bool is_not_equal = cmp == ExpressionType::COMPARE_NOTEQUAL;

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
        if (is_not_equal) {
            // Keep going: the raw values stay in the column untouched (see below), so
            // there is nothing to reduce here, only the fact that it varies to record.
            varies = true;
            continue;
        }
        if (!take_min && !take_max) {
            throw InvalidInputException(
                "%s takes more than one value here (%g and %g) — an `=` bound cannot "
                "hold two different values at once, so this DECIDE query is infeasible.",
                rhs_text.c_str(), reduced[g], v);
        }
        varies = true;
        if (take_min ? (v < reduced[g]) : (v > reduced[g])) {
            reduced[g] = v;
        }
    }

    // A row of a group that reads no bound -- an anchor whose frame selected no position,
    // which bounds nothing (D79) -- still stands in its group's model row, so it carries
    // the group's bound: the tightest, or under `<>` one the group excludes anyway.
    auto unread_group = [&](idx_t row) -> idx_t {
        if (ec.row_group_ids.empty() || group_of(row) != DConstants::INVALID_INDEX) {
            return DConstants::INVALID_INDEX;
        }
        idx_t g = ec.row_group_ids[row];
        return g < num_groups && seen[g] ? g : DConstants::INVALID_INDEX;
    };
    bool has_unread = false;
    for (idx_t row = 0; row < rows && !has_unread; row++) {
        has_unread = unread_group(row) != DConstants::INVALID_INDEX;
    }
    if (!varies && !has_unread) {
        return; // every group already agreed — leave the column alone
    }
    if (is_not_equal && !has_unread) {
        return; // exclusions are kept, not collapsed — leave every row's raw value
    }
    auto &col = ec.rhs_values.MutableDense();
    for (idx_t row = 0; row < rows; row++) {
        idx_t g = group_of(row);
        if (g != DConstants::INVALID_INDEX && seen[g]) {
            if (!is_not_equal) {
                col[row] = reduced[g];
            }
            continue;
        }
        g = unread_group(row);
        if (g != DConstants::INVALID_INDEX) {
            col[row] = reduced[g];
        }
    }
    ec.rhs_values.SyncSize();
}

//===--------------------------------------------------------------------===//
// Phase 2 shared evaluation helpers
//===--------------------------------------------------------------------===//

// TermFilterState is declared in physical_decide.hpp (EvaluateObjective's
// EvaluatedClauses return value carries it across Finalize's phase methods).

// Which rows a reducer with its own key (`sum(per D: ...)`, `sum(D: ...)`) actually
// contributes. The join repeats each D tuple once per matching row; §5.3 asks for one
// term per instance -- per class of the key and value of its body's signature (D64,
// D65), which the scope's identity is when the body has a field
// (EntityScopeInfo::signature) -- and every row sharing an identity carries the same
// value, so keeping the first row of each identity and dropping the rest yields exactly
// one contribution per identity, whichever row is kept. De-duplication runs inside the
// `per` partition, after `when` selection and `per` partitioning, which is the
// construction order the paper pins.
//! `eligible`, when given, is the rows the reducer reads (its own `when` included): the
//! first row of each class among THEM is kept, so a class whose first joined row the
//! `when` drops is still counted once.
static vector<bool> BuildQualifierKeepMask(const vector<EntityMapping> &mappings, idx_t scope_idx,
                                           const vector<idx_t> &row_group_ids, idx_t num_rows,
                                           const vector<bool> *eligible = nullptr) {
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
            continue; // excluded by `when` or in no group — never a surviving identity
        }
        if (eligible && !(*eligible)[row]) {
            continue;
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
// zeroing and AVG's denominator (which counts surviving rows and therefore becomes the
// distinct-identity count) — treats a duplicate row exactly as it treats a
// `when`-excluded one.
static void ApplyQualifierToFilter(const vector<EntityMapping> &mappings, idx_t scope_idx,
                                   const vector<idx_t> &row_group_ids, idx_t num_rows,
                                   TermFilterState &state) {
    auto keep = BuildQualifierKeepMask(mappings, scope_idx, row_group_ids, num_rows,
                                       state.has_filter ? &state.mask : nullptr);
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

//! The classes each group's rows stand for (syntax_reference.md §8.1, D28), kept for
//! the diagnosis: under a `per K:`, K's values among the group's kept rows (first-seen
//! order); with no `per`, every kept row of the group; under `per ()`, the whole clause,
//! one set its groups share. One `SolverInput::ClassSet` per group with kept rows,
//! referenced from `ec.group_class_set`. `kept_row_group` maps each kept row to its group;
//! empty when the clause has one implicit group, which then holds every kept row.
static vector<idx_t> CollectGroupClasses(DecideConstraint::PerKeyRole role,
                                         const vector<unique_ptr<Expression>> &class_key,
                                         PerGroupCache &per_group_cache, ChunkExprCache &chunk_expr_cache,
                                         ClientContext &context, ColumnDataCollection &data, idx_t num_rows,
                                         const vector<bool> &kept_rows, const vector<idx_t> &kept_row_group,
                                         idx_t groups, vector<SolverInput::ClassSet> &class_sets) {
    using Kind = SolverInput::ClassSet::Kind;
    const bool keyed = role != DecideConstraint::PerKeyRole::NONE;
    vector<idx_t> group_class_set(groups, DConstants::INVALID_INDEX);
    if (keyed && class_key.empty()) {
        group_class_set.assign(groups, class_sets.size());
        class_sets.push_back({Kind::WHOLE_CLAUSE, {}, {}});
        return group_class_set;
    }
    const bool one_group = kept_row_group.empty();
    std::function<bool(idx_t)> kept = [&](idx_t row) { return kept_rows[row]; };
    vector<vector<idx_t>> rows_of(groups);
    for (idx_t row = 0; row < num_rows; row++) {
        idx_t group = one_group ? (kept_rows[row] ? idx_t(0) : DConstants::INVALID_INDEX) : kept_row_group[row];
        if (group != DConstants::INVALID_INDEX && group < groups) {
            rows_of[group].push_back(row);
        }
    }
    vector<idx_t> class_ids;
    idx_t num_classes = 0;
    vector<string> class_labels;
    if (keyed) {
        LookupOrBuildPerGroupIds(per_group_cache, class_key, chunk_expr_cache, context, data, num_rows,
                                 kept, &kept_rows, class_ids, num_classes, class_labels);
    }
    // `named_in[c]` is the last group that recorded class c; groups are walked one at a
    // time, so it says whether this group has it already.
    vector<idx_t> named_in(num_classes, DConstants::INVALID_INDEX);
    for (idx_t group = 0; group < groups; group++) {
        if (rows_of[group].empty()) {
            continue;
        }
        SolverInput::ClassSet set {keyed ? Kind::KEY_VALUES : Kind::KEPT_ROWS, {}, {}};
        if (keyed) {
            for (auto row : rows_of[group]) {
                idx_t cls = class_ids[row];
                if (cls != DConstants::INVALID_INDEX && named_in[cls] != group) {
                    named_in[cls] = group;
                    set.labels.push_back(class_labels[cls]);
                }
            }
        } else {
            set.rows = std::move(rows_of[group]);
        }
        group_class_set[group] = class_sets.size();
        class_sets.push_back(std::move(set));
    }
    return group_class_set;
}

//! Narrows `rows` to those where `condition` holds (NULL keeps none), as a scoped clause
//! reads a `when`: the condition is computed on `rows` only, and the parts of an AND or an
//! OR left to right, each only on the rows the parts before it leave undecided -- an AND's
//! on the rows they keep, an OR's on the rows they do not. The composed MIN/MAX rewrite
//! folds a clause's `when` in front of a term's own, which must not be read where the
//! clause's drops.
static void KeepRowsWhere(const Expression &condition, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                          ColumnDataCollection &data, vector<bool> &rows) {
	if (condition.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &part : condition.Cast<BoundConjunctionExpression>().children) {
			KeepRowsWhere(*part, chunk_expr_cache, context, data, rows);
		}
		return;
	}
	if (condition.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		vector<bool> undecided = rows;
		std::fill(rows.begin(), rows.end(), false);
		for (auto &part : condition.Cast<BoundConjunctionExpression>().children) {
			vector<bool> kept = undecided;
			KeepRowsWhere(*part, chunk_expr_cache, context, data, kept);
			for (idx_t row = 0; row < kept.size(); row++) {
				if (kept[row]) {
					rows[row] = true;
					undecided[row] = false;
				}
			}
		}
		return;
	}
	ExpressionExecutor executor(context);
	executor.AddExpression(CachedTransformToChunkExpression(chunk_expr_cache, condition, context));
	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	DataChunk result;
	result.Initialize(context, {LogicalType::BOOLEAN});
	idx_t row_offset = 0;
	while (data.Scan(scan, chunk)) {
		result.Reset();
		ExecuteOnReadRows(executor, chunk, row_offset, &rows, result);
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (rows[row_offset + i]) {
				Value value = result.data[0].GetValue(i);
				rows[row_offset + i] = !value.IsNull() && value.GetValue<bool>();
			}
		}
		row_offset += chunk.size();
	}
}

//! The rows among `read` (every row when null) that a scoped clause's `condition` keeps.
vector<bool> KeptRows(const Expression &condition, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                      ColumnDataCollection &data, idx_t num_rows, const vector<bool> *read) {
	vector<bool> rows = read ? *read : vector<bool>(num_rows, true);
	KeepRowsWhere(condition, chunk_expr_cache, context, data, rows);
	return rows;
}

//! Each term's own `when`, read among `read` (KeptRows).
static vector<vector<bool>> KeptRowsOfEach(const vector<const Expression *> &conditions, const vector<bool> *read,
                                           ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                           ColumnDataCollection &data, idx_t num_rows) {
	vector<vector<bool>> masks;
	for (auto *condition : conditions) {
		masks.push_back(KeptRows(*condition, chunk_expr_cache, context, data, num_rows, read));
	}
	return masks;
}

// Reduce one data-only reducer on the right-hand side to a value per group.
//
// Implements the construction order paper §3.2.2 fixes for every reducer:
//
//     when selection -> per partitioning -> qualifier de-duplication -> aggregation
//
// The first two arrive as `group_ids` (the reducer's own `by` groups among the clause's
// kept rows, without the LHS reducers' own `when` filters). This adds the reducer's own
// `when`, then the relation-qualifier de-duplication, then folds.
//
// Unlike the left-hand side, nothing here can be expressed as a coefficient: the
// LHS reduces by *summing a column*, which is why only SUM and AVG could ever be
// moved there and MIN/MAX/COUNT were refused. Folding per kind is what removes
// that asymmetry.
//! The reducer computes nothing outside the rows its group holds -- its own `when` is read
//! on those, its argument on the ones that `when` keeps -- and counts each class of its
//! `per` once among THOSE rows. Its messages quote the query's `fragments`.
//!
//! A reducer inside its argument is reduced first (syntax_reference.md §5.5): at a member row it is its group's
//! value, the group of its path key -- `path_keys`, the `by` keys from the outermost
//! reducer down to this one, and its own -- among the rows this one reads.
static vector<double> EvaluateRhsReducerPerGroup(const BoundAggregateExpression &agg,
                                                 const vector<idx_t> &group_ids, idx_t num_groups,
                                                 const vector<idx_t> &path_keys,
                                                 const vector<DecideGroupKey> &group_keys,
                                                 const vector<EntityMapping> &entity_mappings,
                                                 ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                                 ColumnDataCollection &data, idx_t num_rows,
                                                 const vector<string> &fragments, vector<bool> *absent) {
    const bool has_groups = !group_ids.empty();
    const idx_t groups = has_groups ? num_groups : 1;
    auto group_of = [&](idx_t row) -> idx_t {
        return has_groups ? group_ids[row] : 0;
    };

    // Stage 1: the reducer's own `when` (`sum(when w: b)`), which scopes this reducer
    // and nothing else, read on the rows of its group.
    vector<bool> keep;
    if (agg.filter) {
        vector<bool> in_group(num_rows);
        for (idx_t row = 0; row < num_rows; row++) {
            in_group[row] = group_of(row) != DConstants::INVALID_INDEX;
        }
        keep = KeptRows(*agg.filter, chunk_expr_cache, context, data, num_rows, &in_group);
    }
    // Stage 2: relation-qualified reducers (`sum(D: cost)`) contribute once per
    // distinct entity, not once per joined row.
    idx_t scope_idx = QualifierScopeOf(agg);
    vector<bool> read_rows(num_rows, false);
    for (idx_t row = 0; row < num_rows; row++) {
        read_rows[row] = group_of(row) != DConstants::INVALID_INDEX && (keep.empty() || keep[row]);
    }
    if (scope_idx != DConstants::INVALID_INDEX) {
        auto dedup = BuildQualifierKeepMask(entity_mappings, scope_idx, group_ids, num_rows, &read_rows);
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
        // The reducers nested in the argument, each to a value per group of its path key
        // among the rows this reducer reads, then read at each row as an extra column.
        vector<const Expression *> nested;
        CollectReducers(*agg.children[0], nested);
        std::unordered_map<const Expression *, idx_t> substitutions;
        vector<vector<double>> nested_columns;
        for (auto *expr : nested) {
            auto &inner = expr->Cast<BoundAggregateExpression>();
            auto inner_keys = path_keys;
            inner_keys.push_back(DecideReducerGroupKey(inner));
            auto key = DecidePathKeyExpressions(inner_keys, group_keys);
            vector<const Expression *> key_exprs;
            for (auto &part : key) {
                key_exprs.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, *part, context));
            }
            vector<idx_t> inner_ids;
            idx_t inner_count = 0;
            BuildGroupIds(key_exprs, context, data, num_rows, std::function<bool(idx_t)> {}, inner_ids, inner_count,
                          nullptr, &read_rows);
            vector<bool> inner_absent;
            auto per_group = EvaluateRhsReducerPerGroup(inner, inner_ids, inner_count, inner_keys, group_keys,
                                                        entity_mappings, chunk_expr_cache, context, data, num_rows,
                                                        fragments, &inner_absent);
            vector<double> column(num_rows, 0.0);
            for (idx_t row = 0; row < num_rows; row++) {
                if (inner_ids[row] != DConstants::INVALID_INDEX && inner_ids[row] < per_group.size()) {
                    column[row] = per_group[inner_ids[row]];
                    // An absent inner reducer drops the member reading it (syntax_reference.md §5.6).
                    if (inner_absent[inner_ids[row]]) {
                        read_rows[row] = false;
                        if (keep.empty()) {
                            keep.assign(num_rows, true);
                        }
                        keep[row] = false;
                    }
                }
            }
            substitutions.emplace(expr, data.ColumnCount() + nested_columns.size());
            nested_columns.push_back(std::move(column));
        }
        // A substituted tree is valid against the augmented chunk alone: never cached.
        unique_ptr<Expression> owned_arg;
        const Expression *arg_ptr;
        if (substitutions.empty()) {
            arg_ptr = &CachedTransformToChunkExpression(chunk_expr_cache, *agg.children[0], context);
        } else {
            owned_arg = TransformToChunkExpression(*agg.children[0], context, &substitutions);
            arg_ptr = owned_arg.get();
        }
        const Expression &arg = *arg_ptr;
        ExpressionExecutor arg_executor(context);
        arg_executor.AddExpression(arg);
        values.reserve(num_rows);
        ColumnDataScanState scan;
        data.InitializeScan(scan);
        DataChunk scan_chunk;
        scan_chunk.Initialize(context, data.Types());
        auto chunk_types = data.Types();
        for (idx_t i = 0; i < nested_columns.size(); i++) {
            chunk_types.push_back(LogicalType::DOUBLE);
        }
        DataChunk augmented;
        augmented.Initialize(context, chunk_types);
        DataChunk out_chunk;
        out_chunk.Initialize(context, vector<LogicalType>{arg.return_type});
        idx_t row_offset = 0;
        while (data.Scan(scan, scan_chunk)) {
            DataChunk *chunk = &scan_chunk;
            if (!nested_columns.empty()) {
                augmented.Reset();
                for (idx_t col = 0; col < scan_chunk.ColumnCount(); col++) {
                    augmented.data[col].Reference(scan_chunk.data[col]);
                }
                for (idx_t i = 0; i < nested_columns.size(); i++) {
                    auto out = FlatVector::GetData<double>(augmented.data[scan_chunk.ColumnCount() + i]);
                    for (idx_t row = 0; row < scan_chunk.size(); row++) {
                        out[row] = nested_columns[i][row_offset + row];
                    }
                }
                augmented.SetCardinality(scan_chunk.size());
                chunk = &augmented;
            }
            auto &in_chunk = *chunk;
            out_chunk.Reset();
            ExecuteOnReadRows(arg_executor, in_chunk, row_offset, &read_rows, out_chunk);
            if (is_count) {
                // `count(e)` counts the rows where `e` is not NULL, as SQL does: 1 for a
                // row it counts, 0 for one it does not; the value itself is never read.
                for (idx_t row = 0; row < in_chunk.size(); row++) {
                    const bool read = read_rows[row_offset + row];
                    values.push_back(read && !out_chunk.data[0].GetValue(row).IsNull() ? 1.0 : 0.0);
                }
                row_offset += in_chunk.size();
                continue;
            }
            // A reducer's input feeds a bound, so it follows the same rule as
            // every other RHS: ±inf is a value, not an error. A group whose
            // MAX(cap) folds to +inf is a group with no upper bound, and one
            // whose bound points out of reach is an infeasibility the solver
            // reports naming the clause — neither is ours to refuse here.
            // Reading it as an error would also make the outcome depend on spelling:
            // `per g: min(x) by (g) <= 1e1000::DOUBLE` would be classified per group
            // while `per g: min(x) by (g) <= max(cap) by (g)` would be rejected wholesale.
            // NaN stays refused, here and in the per-row extraction that reads
            // this value back, so `MAX(cap) - MIN(cap)` over infinities is
            // still caught.
            NullSourceContext null_ctx {agg.children[0].get(), &in_chunk, &fragments};
            ExtractDoubleColumn(out_chunk.data[0], in_chunk.size(), 1.0, values,
                                "constraint right-hand side aggregate",
                                /*allow_infinite=*/true, &null_ctx, &read_rows, row_offset);
            row_offset += in_chunk.size();
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
        if (is_count && !values.empty() && v == 0.0) {
            continue; // a NULL `count(e)` does not count
        }
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
    if (absent) {
        absent->assign(groups, false);
    }
    for (idx_t g = 0; g < groups; g++) {
        // A group with no member is one whose frames selected no position on any of its
        // rows: the group map is built from the kept rows, and the scope checks refused a
        // reducer that reads no row (D8). It is absent (syntax_reference.md §5.6). A
        // `count(e)` whose rows are all NULL, or absent, counts none of them, which is 0.
        if (counts[g] == 0) {
            if (is_count) {
                continue;
            }
            if (!absent) {
                throw InternalException("DECIDE: a bound-side reducer read no row of its group");
            }
            (*absent)[g] = true;
            continue;
        }
        if (is_avg) {
            acc[g] /= static_cast<double>(counts[g]);
        }
    }
    return acc;
}

//! A scoped objective's decision outside its reducers (`MAXIMIZE when c: sum(x) + w * s`)
//! is counted once (syntax_reference.md §4): its coefficient stays on the first
//! kept row and every other row is zeroed. The objective's signature has one variant
//! (D66), so that row stands for all of them -- one instance, one value of its data. The model reads a query-wide
//! decision's coefficient off row 0 and sums every other one over the rows, so a
//! query-wide one is moved to row 0.
static idx_t FirstKeptRow(const vector<bool> *kept, idx_t num_rows) {
	for (idx_t row = 0; row < num_rows; row++) {
		if (!kept || (*kept)[row]) {
			return row;
		}
	}
	return DConstants::INVALID_INDEX;
}

//! Keeps the coefficient a term has on `first_kept` at `target`, and zeroes every other row.
static void CountOnce(CoefficientColumn &coefficients, idx_t first_kept, idx_t target) {
	auto &values = coefficients.MutableDense();
	double value = first_kept < values.size() ? values[first_kept] : 0.0;
	std::fill(values.begin(), values.end(), 0.0);
	if (first_kept < values.size()) {
		values[target] = value;
	}
	coefficients.SyncSize();
}

static void CountTupleTermsOnce(const vector<DecideTerm> &terms, const vector<DecideVarScopeInfo> &variable_scopes,
                                vector<CoefficientColumn> &coefficients, const vector<bool> *kept, idx_t num_rows) {
	idx_t first_kept = FirstKeptRow(kept, num_rows);
	for (idx_t term_idx = 0; term_idx < terms.size() && term_idx < coefficients.size(); term_idx++) {
		auto &term = terms[term_idx];
		if (term.reduction != LinearTermReduction::NONE || term.variable_index == DConstants::INVALID_INDEX) {
			continue;
		}
		bool query_wide =
		    term.variable_index < variable_scopes.size() && variable_scopes[term.variable_index].IsScalar();
		CountOnce(coefficients[term_idx], first_kept, query_wide ? 0 : first_kept);
	}
}

// Evaluate every bilinear term's coefficient expression (constraint or objective side:
// the two are the same shape, differing only in which prepared-model term type feeds
// in and which struct the caller stages results into — both of which are field-
// identical, so this returns the one shared EvaluatedConstraint::BilinearTerm either
// way). Batches every term's coefficient expression into a single ExpressionExecutor
// and scans `data` once, then zeroes rows `filters[i]` excludes (a clause's `when` is
// folded into every term's filter upstream).
// AVG scaling is deliberately left to the caller: the constraint side applies it
// immediately per-constraint, the objective side defers it until its qualifier
// de-duplication has run in Phase 3 — a real difference in control flow, not something
// to collapse.
template <class BilinearTermSource>
static vector<EvaluatedConstraint::BilinearTerm> EvaluateBilinearTerms(
    const vector<BilinearTermSource> &terms, const vector<TermFilterState> &filters,
    ChunkExprCache &chunk_expr_cache, ClientContext &context, ColumnDataCollection &data,
    idx_t num_rows, const char *coefficient_err_label, const vector<bool> &read_rows,
    const vector<string> &fragments) {
    const idx_t num_bl = terms.size();
    // A term is computed on the rows the clause keeps that the term's own `when` keeps,
    // and nowhere else; its messages quote the query's `fragments`.
    vector<vector<bool>> term_reads(num_bl);
    for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
        term_reads[term_idx] = read_rows;
        if (filters[term_idx].has_filter) {
            for (idx_t row = 0; row < num_rows; row++) {
                term_reads[term_idx][row] = term_reads[term_idx][row] && filters[term_idx].mask[row];
            }
        }
    }
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

    vector<const vector<bool> *> bl_reads;
    for (idx_t term_idx : bl_route) {
        bl_reads.push_back(&term_reads[term_idx]);
    }
    if (!bl_route.empty()) {
        ColumnDataScanState bl_scan;
        data.InitializeScan(bl_scan);
        DataChunk bl_chunk;
        bl_chunk.Initialize(context, data.Types());
        DataChunk bl_results;
        bl_results.Initialize(context, bl_types);
        idx_t row_offset = 0;
        while (data.Scan(bl_scan, bl_chunk)) {
            bl_results.Reset();
            ExecuteOnReadRows(bl_executor, bl_chunk, row_offset, bl_reads, bl_results);
            for (idx_t j = 0; j < bl_route.size(); j++) {
                idx_t term_idx = bl_route[j];
                auto &col = ebts[term_idx].row_coefficients.MutableDense();
                NullSourceContext null_ctx {terms[term_idx].coefficient.get(), &bl_chunk, &fragments};
                ExtractDoubleColumn(bl_results.data[j], bl_chunk.size(), terms[term_idx].sign, col,
                                    coefficient_err_label, /*allow_infinite=*/false, &null_ctx,
                                    &term_reads[term_idx], row_offset);
                ebts[term_idx].row_coefficients.SyncSize();
            }
            row_offset += bl_chunk.size();
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
    }
    return ebts;
}

// AVG denominator scaling, shared by the constraint and objective sides and by the
// linear/bilinear (single-column) and quadratic-inner (sqrt-scaled) cases. The rows a
// term reads are its filter: a clause's `when` is folded into it upstream.
static void ScaleAvgRows(CoefficientColumn &col, bool has_filter, const vector<bool> &filter_mask,
                         bool quadratic_inner, idx_t num_rows, const vector<idx_t> &row_group_ids,
                         idx_t num_groups) {
    auto &coefficients = col.MutableDense();
    if (row_group_ids.empty()) {
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

	// What the optimizer made of the clauses: a column that stands for an expression
	// prints it, and each rewrite's account follows its clause, the objective's its own.
	auto names = DecidePlanNames(source_fragments, entity_scopes, group_keys, decide_index, aux_var_expressions);
	auto accounts = rewrite_accounts;
	accounts.insert(accounts.end(), variant_accounts.begin(), variant_accounts.end());
	vector<string> objective_accounts;
	for (auto &account : accounts) {
		if (account.source_clause_id == DConstants::INVALID_INDEX) {
			objective_accounts.push_back(RenderDecideRewriteAccount(account));
		}
	}
	// Constraints first: they also hand back the rows the rewrites added for the objective.
	vector<string> unattributed;
	vector<string> objective_rows;
	string constraints_text;
	if (decide_constraints || !constraint_sources.empty()) {
		auto layers = CollectDecideClauseLayers(decide_constraints.get(), names, constraint_sources, unattributed,
		                                        objective_rows, accounts);
		constraints_text = RenderDecideClauseLayers(layers, unattributed);
	}
	if (decide_objective) {
		string sense_prefix = (decide_sense == DecideSense::MAXIMIZE) ? "MAXIMIZE " : "MINIMIZE ";
		vector<string> objective_strs;
		CollectDecideExpressionStrings(*decide_objective, names, objective_strs, nullptr, nullptr);
		// A MIN/MAX objective leaves a carrier in the tree -- a SUM standing for the
		// extremum, or the composed objective's 0.0 placeholder -- that its accounts
		// describe and a row would misread. An AVG's sum stays, under its account, as in a
		// constraint.
		if (flat_objective_agg != ObjectiveAggregateType::NONE || !composed_minmax_objective_terms.empty()) {
			objective_strs.clear();
		}
		result["Objective"] = RenderDecideObjectiveLayers(sense_prefix, written_objective, canonical_objective,
		                                                  objective_strs, objective_accounts, objective_rows);
	} else {
		result["Objective"] = "FEASIBILITY";
	}
	if (decide_constraints || !constraint_sources.empty()) {
		result["Constraints"] = constraints_text;
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
        // A helper decision of the value rewrite has no intrinsic domain: it holds a
        // reducer's value, `sum(w * x)` negative wherever `w` is, so no bound on it
        // contradicts a type (BuildSolverInput frees its floor).
        vector<bool> is_helper(num_decide_vars, false);
        for (auto &link : op.reducer_value_links) {
            if (link.value_var < num_decide_vars) {
                is_helper[link.value_var] = true;
            }
        }
        for (idx_t var = 0; var < num_decide_vars; var++) {
            if (is_helper[var]) {
                continue;
            }
            bool is_bool = var < op.is_boolean_var.size() && op.is_boolean_var[var];
            double L = (absorbed_lower_bounds[var] <= ABSORBED_LOWER_UNSET)
                           ? 0.0
                           : absorbed_lower_bounds[var];
            double U = absorbed_upper_bounds[var];
            bool has_removable_bound = false;
            for (const auto &bound : user_absorbed_bounds) {
                has_removable_bound = has_removable_bound ||
                                      (bound.decide_var_idx == var &&
                                       bound.removal_group_id != DConstants::INVALID_INDEX);
            }
            if (has_removable_bound) {
                // A singleton/negative IN lowering can be absorbed into the box. It is
                // not an independently written bound, and exact diagnosis may omit its
                // whole source clause, so it must not trigger the pre-solve static bound
                // error. Reconstruct the box from intrinsic domain plus only retained
                // (non-removable) bounds; the actual primary solve still receives the
                // full box and therefore still proves the original query infeasible.
                double retained_l = is_bool ? 0.0 : ABSORBED_LOWER_UNSET;
                double retained_u = is_bool ? 1.0 : 1e30;
                for (const auto &bound : user_absorbed_bounds) {
                    if (bound.decide_var_idx != var ||
                        bound.removal_group_id != DConstants::INVALID_INDEX) {
                        continue;
                    }
                    if (bound.sense != '<') retained_l = std::max(retained_l, bound.k);
                    if (bound.sense != '>') retained_u = std::min(retained_u, bound.k);
                }
                L = retained_l <= ABSORBED_LOWER_UNSET ? 0.0 : retained_l;
                U = retained_u;
            }
            // An auxiliary decision (an ABS, a norm) is named by what it stands for.
            const bool is_aux = var >= num_decide_vars - op.num_auxiliary_vars;
            string vname = op.decide_variables[var]->GetName();
            for (auto &label : op.aux_var_expressions) {
                if (is_aux && label.first == var) {
                    vname = label.second;
                }
            }
            const char *domain = is_bool ? "BOOLEAN (0 or 1)" : "non-negative (>= 0)";
            // The clause that set the bound `k`, as written (`max(x) <= -5` as much as `x <= -5`).
            auto written_bound = [&](bool upper, double k) {
                for (const auto &bound : user_absorbed_bounds) {
                    if (bound.decide_var_idx == var && bound.sense != (upper ? '>' : '<') && bound.k == k &&
                        bound.source_clause_id < op.constraint_sources.size()) {
                        auto &source = op.constraint_sources[bound.source_clause_id];
                        return AttachDecideQualifier(WrittenDecideComparison(source), source.qualifier);
                    }
                }
                return StringUtil::Format("%s %s %g", vname, upper ? "<=" : ">=", k);
            };
            if (U < 0.0 && L >= 0.0 && is_aux) {
                throw InvalidInputException(StringUtil::Format(
                    "DECIDE optimization is infeasible: %s cannot hold because %s is never negative.",
                    written_bound(true, U), vname));
            }
            if (U < 0.0 && L >= 0.0) {
                throw InvalidInputException(StringUtil::Format(
                    "DECIDE optimization is infeasible: %s cannot hold because %s is %s. %s", written_bound(true, U),
                    vname, domain,
                    is_bool ? "Drop the bound or change the variable's type."
                            : "Add an explicit lower bound if it may be negative."));
            }
            if (is_bool && L > 1.0) {
                throw InvalidInputException(StringUtil::Format(
                    "DECIDE optimization is infeasible: %s cannot hold because %s is BOOLEAN (0 or 1). Drop the "
                    "bound or change the variable's type.",
                    written_bound(false, L), vname));
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
    const vector<unique_ptr<DecideConstraint>> &absorbed_constraints = op.prepared.absorbed_constraints;
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
    //! `absorbed_constraints`, evaluated under DIAGNOSE for the repair model alone.
    vector<EvaluatedConstraint> evaluated_absorbed_constraints;
    //! The classes each aggregate-shape row's group stands for (D28), referenced from the
    //! rows; filled under DIAGNOSE only and moved into SolverInput::class_sets.
    vector<SolverInput::ClassSet> class_sets;
    //! The value rewrite's helper decisions, each with its reducer's members evaluated
    //! (EvaluateReducerValueLinks); moved into SolverInput::reducer_value_links.
    vector<ReducerValueLinkSpec> evaluated_value_links;
    vector<CoefficientColumn> evaluated_objective_coefficients;  // [term_idx]
    vector<idx_t> objective_variable_indices;
    vector<LinearTermReduction> objective_term_reductions;       // [term_idx]

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

    //! The frame selectors' timelines and selections (PhysicalDecide::BuildFrames), one per
    //! frame, in LogicalDecide::frames order.
    vector<DecideFrameMap> frame_maps;
    //! The decisions a frame reads at another row (SolverInput::frame_reads): each pair of
    //! a decision and a frame once, the variable after the plan's decisions.
    vector<std::pair<idx_t, idx_t>> frame_reads;

    //! The variable a term reads at each row: its own, or -- for a frame's payload term --
    //! the read of it at the row the frame selects. A query-wide decision has one column,
    //! which every row reads, so it is its own read.
    idx_t ReadVariable(const DecideTerm &term) {
        if (term.frame_read == DConstants::INVALID_INDEX) {
            return term.variable_index;
        }
        auto var = term.variable_index;
        if (var < op.variable_scopes.size() && op.variable_scopes[var].IsScalar()) {
            return var;
        }
        auto read = std::make_pair(var, term.frame_read);
        auto it = std::find(frame_reads.begin(), frame_reads.end(), read);
        if (it == frame_reads.end()) {
            frame_reads.push_back(read);
            it = frame_reads.end() - 1;
        }
        return op.decide_variables.size() + static_cast<idx_t>(it - frame_reads.begin());
    }

    //! Set when the solve failed under DIAGNOSE: the findings were handed off instead of
    //! raised, so there is no solution to read back and this operator emits no rows. The
    //! DIAGNOSE operator above turns the handoff into the answer.
    bool diagnosis_only = false;
};

class DecideLocalSinkState : public LocalSinkState {
public:
    DecideProfileTotals profile_sink {"execution.sink_append"};
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
    DecideProfileSample profile(lstate.profile_sink);
    lstate.data.Append(lstate.append_state, chunk);
    return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDecide::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
    DecideProfileScope profile("execution.sink_combine");
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
	const vector<bool> &input_column_in_clause;
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
		              row_to_group, num_groups, &rep_keys);
		idx_t cap = std::max(params.min_categories, (idx_t)(params.categorical_ratio * (double)denom));
		if (num_groups < 2 || num_groups > cap || rep_keys.empty()) {
			return false;
		}
		out.column = input_column_names[ci];
		out.clause_referenced = ci < input_column_in_clause.size() && input_column_in_clause[ci];
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
		out.clause_referenced = rowcg.clause_referenced;
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
//! input by now, so var_indexer owns them) and returns a stateful
//! provider wrapped as the engine's std::function input.
static std::function<vector<ColumnGrouping>(idx_t, idx_t)>
BuildUnboundedCandidateProvider(ClientContext &context, ColumnDataCollection &data, vector<LogicalType> types,
                                const DecideDiagParams &params, const vector<string> &input_column_names,
                                const vector<bool> &input_column_in_clause, const VarIndexer &var_indexer,
                                const vector<EntityScopeInfo> &entity_scopes, idx_t num_rows) {
	return UnboundedCandidateProvider {context,           data,          std::move(types),
	                                   params,            input_column_names, input_column_in_clause,
	                                   var_indexer,       entity_scopes, var_indexer.entity_mappings,
	                                   num_rows};
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
//! Returns 0 when the figure is unavailable; callers drop the memory line rather
//! than reporting a zero that reads as a measurement.
static double PeakProcessMemoryBytes() {
#ifdef _WIN32
    // Windows has no getrusage. The equivalent (GetProcessMemoryInfo) would add a
    // psapi link entry to both CMake and setup.py, so the report simply omits its
    // memory line here instead.
    return 0.0;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
#ifdef __APPLE__
    return (double)usage.ru_maxrss;
#else
    return (double)usage.ru_maxrss * 1024.0;
#endif
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
    // 0 means "not measurable here" (Windows, or a getrusage failure), not "no memory
    // used" — say nothing rather than print "peak memory 0 B".
    double peak_bytes = PeakProcessMemoryBytes();
    string tail = peak_bytes > 0.0
                      ? StringUtil::Format("  elapsed %s · peak memory %s\n",
                                           FormatDuration(elapsed_seconds).c_str(),
                                           FormatMemory(peak_bytes).c_str())
                      : StringUtil::Format("  elapsed %s\n", FormatDuration(elapsed_seconds).c_str());
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

//===--------------------------------------------------------------------===//
// Finalize: PHASE 1.4 — Frame selectors
//===--------------------------------------------------------------------===//

void PhysicalDecide::BuildFrames(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                 ChunkExprCache &chunk_expr_cache) const {
    if (frames.empty()) {
        return;
    }
    for (auto &frame : frames) {
        gstate.frame_maps.push_back(
            BuildDecideFrameMap(context, frame, group_keys, gstate.data, num_rows, chunk_expr_cache));
    }
    AppendDecideFrameColumns(context, gstate.frame_maps, prepared, gstate.data, num_rows, chunk_expr_cache);
}

void PhysicalDecide::CheckFramePeers(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                     ChunkExprCache &chunk_expr_cache,
                                     const vector<EntityMapping> &entity_mappings) const {
    CheckDecideFramePeers(context, frames, gstate.frame_maps, prepared, variable_scopes, entity_mappings, group_keys,
                          gstate.data, num_rows, chunk_expr_cache);
}

vector<bool> PhysicalDecide::FramesPresent(const DecideGlobalSinkState &gstate, idx_t source_clause_id,
                                           idx_t num_rows) const {
    vector<bool> present;
    for (idx_t f = 0; f < frames.size(); f++) {
        auto &frame = frames[f];
        if (!frame.SkipsInstancesOf(source_clause_id)) {
            continue;
        }
        if (present.empty()) {
            present.assign(num_rows, true);
        }
        auto &map = gstate.frame_maps[f];
        for (idx_t row = 0; row < num_rows; row++) {
            present[row] = present[row] && !map.Missing(row);
        }
    }
    return present;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 1.5 — Build Entity Mappings for Keyed Decisions
//===--------------------------------------------------------------------===//

vector<EntityMapping> PhysicalDecide::BuildEntityMappings(ClientContext &context, DecideGlobalSinkState &gstate,
                                                           idx_t num_rows, ChunkExprCache &chunk_expr_cache) const {
    vector<EntityMapping> entity_mappings;
    auto data_types = gstate.data.Types();
    for (idx_t scope_idx = 0; scope_idx < entity_scopes.size(); scope_idx++) {
        auto &scope = entity_scopes[scope_idx];
        EntityMapping mapping;

        vector<unique_ptr<Expression>> key_exprs_owned;
        vector<const Expression *> key_exprs;
        // The rows a scope's key is never evaluated on are no group, and share one spare
        // entity so that every row has an id.
        auto spare_entity_for_dropped_rows = [](EntityMapping &mapping) {
            bool dropped = false;
            for (auto &entity : mapping.row_to_entity) {
                if (entity == DConstants::INVALID_INDEX) {
                    entity = mapping.num_entities;
                    dropped = true;
                }
            }
            if (dropped) {
                mapping.num_entities++;
            }
        };
        if (scope.value_link != DConstants::INVALID_INDEX) {
            // A helper decision's scope: the groups of its link's `by` key among the rows
            // the link's clause keeps, one entity per value the key takes there (NULL a
            // value).
            auto &link = reducer_value_links[scope.value_link];
            for (auto &expr : group_keys[link.group_key].expressions) {
                key_exprs.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, *expr, context));
            }
            vector<bool> kept;
            if (link.when) {
                kept = KeptRows(*link.when, chunk_expr_cache, context, gstate.data, num_rows);
            }
            BuildGroupIds(key_exprs, context, gstate.data, num_rows, std::function<bool(idx_t)> {},
                          mapping.row_to_entity, mapping.num_entities, nullptr,
                          link.when ? &kept : nullptr);
            spare_entity_for_dropped_rows(mapping);
            entity_mappings.push_back(std::move(mapping));
            continue;
        }
        if (scope.signature != DConstants::INVALID_INDEX) {
            // A relation-qualified reducer's scope: the classes of its key among the rows
            // it reads, each split by its body's signature (D64, D65), so that one value
            // per instance -- not per class -- reaches the reducer. The decisions' scopes
            // come first, so their classes are built by now.
            auto &signature = signatures[scope.signature];
            vector<bool> kept;
            if (signature.rows) {
                kept = KeptRows(*signature.rows, chunk_expr_cache, context, gstate.data, num_rows);
            }
            BuildSignatureIds(signature, group_keys, variable_scopes, entity_mappings,
                              SignatureFrames {frames, gstate.frame_maps, prepared}, chunk_expr_cache, context,
                              gstate.data, num_rows, signature.rows ? &kept : nullptr, mapping.row_to_entity,
                              mapping.num_entities);
            // How many classes of the key those values come from, for EXPLAIN ANALYZE (D65).
            vector<const Expression *> class_key;
            for (auto &column : signature.key) {
                class_key.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, *column, context));
            }
            vector<idx_t> class_ids;
            idx_t num_classes = 0;
            BuildGroupIds(class_key, context, gstate.data, num_rows, std::function<bool(idx_t)> {}, class_ids,
                          num_classes, nullptr, signature.rows ? &kept : nullptr);
            if (mapping.num_entities > num_classes) {
                RecordVariants(signature, "values", mapping.num_entities, num_classes);
            }
            spare_entity_for_dropped_rows(mapping);
            entity_mappings.push_back(std::move(mapping));
            continue;
        }
        // Build BoundReferenceExpression for each entity-key physical column index
        // so BuildGroupIds can run them through one ExpressionExecutor.
        key_exprs_owned.reserve(scope.entity_key_physical_indices.size());
        key_exprs.reserve(scope.entity_key_physical_indices.size());
        for (idx_t col_idx = 0; col_idx < scope.entity_key_physical_indices.size(); col_idx++) {
            idx_t phys_idx = scope.entity_key_physical_indices[col_idx];
            key_exprs_owned.push_back(make_uniq_base<Expression, BoundReferenceExpression>(
                data_types[phys_idx], phys_idx));
            key_exprs.push_back(key_exprs_owned.back().get());
        }

        // A reducer's `per ()` has no key columns: one class, every row in it.
        BuildGroupIds(key_exprs, context, gstate.data, num_rows,
                      std::function<bool(idx_t)>{}, mapping.row_to_entity, mapping.num_entities);
        entity_mappings.push_back(std::move(mapping));
    }
    return entity_mappings;
}

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
    DecideProfileScope profile("execution.finalize");
    bool bench = std::getenv("DECIDB_BENCH") != nullptr;
    Profiler model_timer;
    Profiler solver_timer;

    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    idx_t num_rows = gstate.data.Count();
    variant_accounts.clear();

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

    // Per-Finalize cache of unfiltered row→group assignments, shared across
    // every constraint and the objective so a key evaluated once is reused
    // by every other call site that asks for the same expression set.
    PerGroupCache per_group_cache;

    //===--------------------------------------------------------------------===//
    // PHASE 1.4: Frame selectors -- their timelines, and the values they read as columns
    //===--------------------------------------------------------------------===//

    DecideProfileScope phase("execution.frames");
    BuildFrames(context, gstate, num_rows, chunk_expr_cache);

    //===--------------------------------------------------------------------===//
    // PHASE 1.5: Build Entity Mappings for Keyed Decisions
    //===--------------------------------------------------------------------===//

    phase.Next("execution.entity_mapping");
    vector<EntityMapping> entity_mappings = BuildEntityMappings(context, gstate, num_rows, chunk_expr_cache);

    //===--------------------------------------------------------------------===//
    // PHASE 1.6: Scope checks of the clauses
    //===--------------------------------------------------------------------===//

    phase.Next("execution.scope_checks");
    RunScopeChecks(context, gstate, num_rows, chunk_expr_cache, per_group_cache, entity_mappings);
    // A frame's selected position must hold one payload (D67), read once the scope checks
    // have found the clauses well-defined.
    CheckFramePeers(context, gstate, num_rows, chunk_expr_cache, entity_mappings);

    //===--------------------------------------------------------------------===//
    // PHASE 2: Evaluate Coefficient Expressions
    //===--------------------------------------------------------------------===//

    // 1. Evaluate constraints
    phase.Next("execution.evaluate_constraints");
    EvaluateConstraints(context, gstate, num_rows, chunk_expr_cache, per_group_cache, entity_mappings);

    // 2. Evaluate objective
    phase.Next("execution.evaluate_objective");
    EvaluatedClauses evaluated =
        EvaluateObjective(context, gstate, num_rows, chunk_expr_cache, per_group_cache, entity_mappings);

    // 3. Evaluate composed MIN/MAX clauses (constraint and objective)
    phase.Next("execution.evaluate_composed");
    EvaluateComposedClauses(context, gstate, num_rows, chunk_expr_cache, per_group_cache, entity_mappings, evaluated);

    // 4. Evaluate the value rewrite's helper decisions (their reducers' members)
    phase.Next("execution.evaluate_value_links");
    EvaluateReducerValueLinks(context, gstate, num_rows, chunk_expr_cache, per_group_cache, entity_mappings);

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP
    //===--------------------------------------------------------------------===//

    // 3a. Assemble the model input and settle the column box. Nothing below this line
    // touches the data again.
    phase.Next("execution.assemble_input");
    VarIndexer var_indexer;
    SolverInput solver_input =
        BuildSolverInput(gstate, num_rows, std::move(entity_mappings), evaluated, var_indexer);

    // Under DIAGNOSE, keep the model input and column space as assembly left them —
    // before a single constant was derived. An infeasibility repair searches a WIDER box
    // than the solved model declares, and a Big-M or a McCormick envelope is sound only
    // for the box it came from, so the INFEASIBLE terminal formulates a second model from
    // this instead of re-deriving nothing and inheriting the narrow one's constants. It
    // also emits the scoped bounds the column box enforces, which the solved model has no
    // row for. The copy is paid for only when the user asked for a diagnosis.
    unique_ptr<ElasticFormulation> elastic;
    if (diagnose) {
        elastic = make_uniq<ElasticFormulation>();
        elastic->input = solver_input;
        for (auto &absorbed : gstate.evaluated_absorbed_constraints) {
            elastic->input.constraints.push_back(std::move(absorbed));
        }
        elastic->var_indexer = var_indexer;
    }

    // 3b. Formulate: derive the constants and emit the rows, against the box the model
    // declares. A pure function of what came before, which is what lets the elastic
    // (infeasibility-repair) path run it a second time against a widened box.
    phase.Next("execution.formulate");
    FormulateModel(solver_input, FormulationBox::OfSolvedModel(solver_input), var_indexer, evaluated);

    phase.Next("execution.solve_and_route");
    return FinalizeSolveResult(context, gstate, solver_input, var_indexer, evaluated, elastic.get(),
                               bench, model_timer, solver_timer);
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 2, sub-phase 1 -- Evaluate Constraints
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Finalize: PHASE 1.6 -- scope checks
//===--------------------------------------------------------------------===//

//! A value as SQL writes it, for a message: 'East', 10, NULL.
string SqlLiteral(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	if (value.type().IsNumeric() || value.type().id() == LogicalTypeId::BOOLEAN) {
		return value.ToString();
	}
	return "'" + StringUtil::Replace(value.ToString(), "'", "''") + "'";
}

//! A class or group, as a message names it: `D.depotID = 'D1', D.region = 'East'`.
string DescribeKeyValues(const vector<string> &names, const vector<Value> &values) {
	string result;
	for (idx_t i = 0; i < names.size() && i < values.size(); i++) {
		result += (i > 0 ? ", " : "") + names[i] + " = " + SqlLiteral(values[i]);
	}
	return result;
}

//! `expr` on each of `rows`, as values (NULL on the others): what tells two variants
//! apart in a message.
vector<Value> EvaluateRowValues(const Expression &expr, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                ColumnDataCollection &data, idx_t num_rows, const vector<bool> &rows) {
	ExpressionExecutor executor(context);
	auto &transformed = CachedTransformToChunkExpression(chunk_expr_cache, expr, context);
	executor.AddExpression(transformed);
	vector<Value> values;
	values.reserve(num_rows);
	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	DataChunk result;
	result.Initialize(context, {transformed.return_type});
	while (data.Scan(scan, chunk)) {
		result.Reset();
		ExecuteOnReadRows(executor, chunk, values.size(), &rows, result);
		for (idx_t row = 0; row < chunk.size(); row++) {
			values.push_back(result.data[0].GetValue(row));
		}
	}
	return values;
}

void PhysicalDecide::RunScopeChecks(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                    ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                    const vector<EntityMapping> &entity_mappings) const {
	DecideProfileScope profile("execution.scope_checks");
	for (auto &check : scope_checks) {
		vector<bool> rows(num_rows, true);
		if (check.rows) {
			rows = KeptRows(*check.rows, chunk_expr_cache, context, gstate.data, num_rows);
		}
		if (check.kind == DecideScopeCheckKind::KEEPS_A_ROW) {
			if (std::find(rows.begin(), rows.end(), true) == rows.end()) {
				throw InvalidInputException("DECIDE: when %s matches no rows.", check.subject);
			}
			continue;
		}
		if (check.kind == DecideScopeCheckKind::FRAME_PARTITION) {
			// D69: every class of the `per` key reads one of the frame's timelines.
			auto &frame = frames[check.frame];
			auto &map = gstate.frame_maps[check.frame];
			std::function<bool(idx_t)> kept_row = [&](idx_t row) {
				return rows[row];
			};
			vector<idx_t> ids;
			idx_t count = 0;
			vector<string> labels;
			vector<vector<Value>> key_values;
			LookupOrBuildPerGroupIds(per_group_cache, check.key, chunk_expr_cache, context, gstate.data, num_rows,
			                         kept_row, &rows, ids, count, labels, nullptr, nullptr, &key_values);
			vector<idx_t> first_row(count, DConstants::INVALID_INDEX);
			for (idx_t row = 0; row < num_rows; row++) {
				auto group = ids[row];
				if (group == DConstants::INVALID_INDEX) {
					continue;
				}
				if (first_row[group] == DConstants::INVALID_INDEX) {
					first_row[group] = row;
					continue;
				}
				if (map.timeline[row] == map.timeline[first_row[group]]) {
					continue;
				}
				// The timeline's values at the class's first row and at this one.
				vector<bool> two(num_rows, false);
				two[first_row[group]] = two[row] = true;
				vector<const Expression *> timeline_key;
				if (frame.scope_key != DConstants::INVALID_INDEX) {
					for (auto &expr : group_keys[frame.scope_key].expressions) {
						timeline_key.push_back(expr.get());
					}
				}
				for (auto &expr : frame.within) {
					timeline_key.push_back(expr.get());
				}
				vector<string> left, right;
				for (auto *expr : timeline_key) {
					auto values = EvaluateRowValues(*expr, chunk_expr_cache, context, gstate.data, num_rows, two);
					left.push_back(SqlLiteral(values[first_row[group]]));
					right.push_back(SqlLiteral(values[row]));
				}
				auto spanned = left.size() == 1 ? left[0] + ", " + right[0]
				                                : "(" + StringUtil::Join(left, ", ") + "), (" +
				                                      StringUtil::Join(right, ", ") + ")";
				auto names = StringUtil::Join(frame.timeline_names, ", ");
				if (frame.timeline_names.size() > 1) {
					names = "(" + names + ")";
				}
				auto spans = check.key.empty() ? string("Its rows span")
				                               : DescribeKeyValues(check.key_names, key_values[group]) + " spans";
				throw InvalidInputException("DECIDE: %s. %s %s = %s.", check.subject, spans, names, spanned);
			}
			continue;
		}
		if (check.kind != DecideScopeCheckKind::READS_A_ROW) {
			throw InternalException("DECIDE: a scope check of a retired kind reached execution");
		}
		// The groups of the check's key over every kept row: each needs a row the
		// reducer's own `when` keeps.
		auto filter = KeptRows(*check.filter, chunk_expr_cache, context, gstate.data, num_rows, &rows);
		std::function<bool(idx_t)> kept_row = [&](idx_t row) {
			return rows[row];
		};
		vector<idx_t> ids;
		idx_t count = 0;
		vector<string> labels;
		vector<vector<Value>> key_values;
		LookupOrBuildPerGroupIds(per_group_cache, check.key, chunk_expr_cache, context, gstate.data, num_rows,
		                         kept_row, &rows, ids, count, labels, nullptr, nullptr, &key_values);
		vector<bool> read(count, false);
		for (idx_t row = 0; row < num_rows; row++) {
			if (ids[row] != DConstants::INVALID_INDEX && filter[row]) {
				read[ids[row]] = true;
			}
		}
		for (idx_t group = 0; group < count; group++) {
			if (read[group]) {
				continue;
			}
			if (check.key.empty()) {
				throw InvalidInputException("DECIDE: %s has no rows.", check.subject);
			}
			throw InvalidInputException("DECIDE: %s has no rows for %s.", check.subject,
			                            DescribeKeyValues(check.key_names, key_values[group]));
		}
	}
	// The objective is produced once (D66): a field with a second value over its kept rows
	// would be a second objective.
	if (objective_signature != DConstants::INVALID_INDEX) {
		auto &signature = signatures[objective_signature];
		vector<bool> rows(num_rows, true);
		if (signature.rows) {
			rows = KeptRows(*signature.rows, chunk_expr_cache, context, gstate.data, num_rows);
		}
		vector<idx_t> ids;
		idx_t count = 0;
		BuildSignatureIds(signature, group_keys, variable_scopes, entity_mappings,
		                  SignatureFrames {frames, gstate.frame_maps, prepared}, chunk_expr_cache, context, gstate.data,
		                  num_rows, &rows, ids, count);
		if (count > 1) {
			throw InvalidInputException("DECIDE objective: %s, so the objective would be produced more than "
			                            "once; move it inside a reducer or fix it with when.",
			                            DescribeVariantDifference(signature, ids, chunk_expr_cache, context, gstate,
			                                                      num_rows, rows, entity_mappings));
		}
	}
}

//! One line for EXPLAIN ANALYZE: a scoped expression whose classes held more than one
//! variant (D64, D65), so the instances it generated -- or the values a reducer read --
//! outnumber its classes: `per P.depotID: 4 constraints from 2 classes, one per distinct
//! (P.lim, by (P.region)) within a class`.
void PhysicalDecide::RecordVariants(const DecideSignature &signature, const char *unit, idx_t variants,
                                    idx_t classes) const {
	vector<string> fields;
	for (auto &field : signature.fields) {
		fields.push_back(field.text);
	}
	LogicalDecide::RewriteAccount account;
	account.source_clause_id = signature.source_clause_id;
	account.kind = LogicalDecide::RewriteAccount::Kind::VARIANTS;
	account.subject = StringUtil::Format("%s: %llu %s from %llu %s, one per distinct (%s) within a class",
	                                     signature.text, variants, unit, classes,
	                                     classes == 1 ? "class" : "classes", StringUtil::Join(fields, ", "));
	for (auto &recorded : variant_accounts) {
		if (recorded.source_clause_id == account.source_clause_id && recorded.subject == account.subject) {
			return; // a clause the rewrites made several rows of
		}
	}
	variant_accounts.push_back(std::move(account));
}

//! Each variant's field values, as a diagnosis labels it beside its class: `100` for
//! `P.lim`, `East` for `by (P.region)`, joined as a composite key is (`100, East`).
//! `first_rows` holds each variant's id on its first row only, INVALID_INDEX elsewhere; a
//! decision is the row's own and adds nothing.
vector<string> PhysicalDecide::DescribeVariants(const DecideSignature &signature, const vector<idx_t> &first_rows,
                                                ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                                DecideGlobalSinkState &gstate, idx_t num_rows) const {
	vector<bool> firsts(num_rows, false);
	idx_t count = 0;
	for (idx_t row = 0; row < num_rows; row++) {
		if (first_rows[row] != DConstants::INVALID_INDEX) {
			firsts[row] = true;
			count = MaxValue<idx_t>(count, first_rows[row] + 1);
		}
	}
	vector<string> labels(count);
	auto append = [&](const Expression &expr) {
		auto values = EvaluateRowValues(expr, chunk_expr_cache, context, gstate.data, num_rows, firsts);
		for (idx_t row = 0; row < num_rows; row++) {
			if (!firsts[row]) {
				continue;
			}
			auto &label = labels[first_rows[row]];
			auto text = values[row].IsNull() ? string("NULL") : values[row].ToString();
			label += (label.empty() ? "" : ", ") + (text.empty() ? string("''") : text);
		}
	};
	for (auto &field : signature.fields) {
		if (field.kind == DecideSignatureFieldKind::VALUE) {
			append(*field.value);
		} else if (field.kind == DecideSignatureFieldKind::BY_KEY) {
			for (auto &expr : group_keys[field.group_key].expressions) {
				append(*expr);
			}
		} else if (field.kind == DecideSignatureFieldKind::FRAME) {
			// Its timeline, and its anchor's place there when it reads relative to it.
			auto &frame = frames[field.frame];
			for (auto &expr : frame.within) {
				append(*expr);
			}
			if (frame.Relative()) {
				append(*frame.order);
			}
		}
	}
	return labels;
}

//! What tells the first two variants of `ids` apart, as a message says it: `'w' takes
//! more than one value over the kept rows (3, 5)`, `the decision 'x' (declared per row)
//! has more than one instance over the kept rows`. `rows` is where the fields may be
//! computed.
string PhysicalDecide::DescribeVariantDifference(const DecideSignature &signature, const vector<idx_t> &ids,
                                                 ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                                 DecideGlobalSinkState &gstate, idx_t num_rows,
                                                 const vector<bool> &rows,
                                                 const vector<EntityMapping> &entity_mappings) const {
	idx_t first = DConstants::INVALID_INDEX;
	idx_t second = DConstants::INVALID_INDEX;
	for (idx_t row = 0; row < num_rows && second == DConstants::INVALID_INDEX; row++) {
		if (ids[row] == DConstants::INVALID_INDEX) {
			continue;
		}
		if (first == DConstants::INVALID_INDEX) {
			first = row;
		} else if (ids[row] != ids[first]) {
			second = row;
		}
	}
	if (second == DConstants::INVALID_INDEX) {
		throw InternalException("DECIDE: a signature with one variant was described as two");
	}
	vector<bool> two(num_rows, false);
	two[first] = two[second] = true;
	auto differs = [&](const Expression &expr, string &values) {
		auto at = EvaluateRowValues(expr, chunk_expr_cache, context, gstate.data, num_rows, two);
		if (Value::NotDistinctFrom(at[first], at[second])) {
			return false;
		}
		values = SqlLiteral(at[first]) + ", " + SqlLiteral(at[second]);
		return true;
	};
	for (auto &field : signature.fields) {
		string values;
		switch (field.kind) {
		case DecideSignatureFieldKind::VALUE:
			if (differs(*field.value, values)) {
				return StringUtil::Format("'%s' takes more than one value over the kept rows (%s)", field.text,
				                          values);
			}
			break;
		case DecideSignatureFieldKind::BY_KEY: {
			auto &by = group_keys[field.group_key];
			for (idx_t i = 0; i < by.expressions.size(); i++) {
				if (differs(*by.expressions[i], values)) {
					auto name = i < by.names.size() ? by.names[i] : by.expressions[i]->ToString();
					return StringUtil::Format("'%s' in %s takes more than one value over the kept rows (%s)", name,
					                          field.text, values);
				}
			}
			break;
		}
		case DecideSignatureFieldKind::DECISION: {
			auto &scope = field.decision < variable_scopes.size() ? variable_scopes[field.decision]
			                                                       : DecideVarScopeInfo::Row();
			bool split = !scope.IsEntity();
			if (scope.IsEntity()) {
				auto &row_class = entity_mappings[scope.entity_scope_idx].row_to_entity;
				split = row_class[first] != row_class[second];
			}
			if (split) {
				return StringUtil::Format("the decision '%s' (declared per %s) has more than one instance over the "
				                          "kept rows",
				                          field.text, scope.IsEntity() ? scope.declared_key : string("row"));
			}
			break;
		}
		case DecideSignatureFieldKind::FRAME: {
			auto &frame = frames[field.frame];
			auto &map = gstate.frame_maps[field.frame];
			if (map.timeline[first] != map.timeline[second]) {
				for (idx_t i = 0; i < frame.within.size(); i++) {
					if (differs(*frame.within[i], values)) {
						auto &name = frame.timeline_names[frame.timeline_names.size() - frame.within.size() + i];
						return StringUtil::Format("'%s' reads more than one timeline over the kept rows (%s = %s)",
						                          field.text, name, values);
					}
				}
				return StringUtil::Format("'%s' reads more than one timeline over the kept rows", field.text);
			}
			if (frame.Relative() && map.peer[first] != map.peer[second]) {
				differs(*frame.order, values);
				return StringUtil::Format("'%s' reads more than one position over the kept rows (%s = %s)",
				                          field.text, frame.order_name, values);
			}
			return StringUtil::Format("'%s' falls back to more than one value over the kept rows", field.text);
		}
		}
	}
	throw InternalException("DECIDE: two variants of a signature agree on every field");
}

void PhysicalDecide::EvaluateConstraints(ClientContext &context, DecideGlobalSinkState &gstate,
                                         idx_t num_rows, ChunkExprCache &chunk_expr_cache,
                                         PerGroupCache &per_group_cache,
                                         const vector<EntityMapping> &entity_mappings) const {
    // The absorbed scoped bounds follow under DIAGNOSE, numbered after every other clause:
    // only the repair model emits their rows.
    const idx_t num_constraints = gstate.constraints.size();
    const idx_t num_evaluated = num_constraints + (diagnose ? gstate.absorbed_constraints.size() : 0);
    for (idx_t c = 0; c < num_evaluated; c++) {
        DecideProfileScope clause_profile("extraction.constraint");
        DecideProfileScope::Counter("constraint.ordinal", c);
        DecideProfileScope clause_phase("extraction.constraint_setup");
        auto &constraint =
            c < num_constraints ? gstate.constraints[c] : gstate.absorbed_constraints[c - num_constraints];

        EvaluatedConstraint eval_const;
        eval_const.comparison_type = constraint->comparison_type;
        eval_const.source_clause_id = constraint->source_clause_id;
        eval_const.repair_group_id = c;
        eval_const.removal_group_id = constraint->removal_group_id;
        // Preserve whether the original LHS was an aggregate (e.g., SUM(...))
        eval_const.lhs_is_aggregate = constraint->lhs_is_aggregate;
        eval_const.minmax_clause_idx = constraint->minmax_clause_idx;
        eval_const.minmax_agg_type = constraint->minmax_agg_type;
        eval_const.ne_clause_idx = constraint->ne_clause_idx;
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
            eval_const.variable_indices.push_back(gstate.ReadVariable(term));
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
        // A clause reads only the rows its `when` keeps, and a term only those its own
        // `when` keeps too: nothing is computed anywhere else.
        vector<bool> scoped_kept(num_rows, true);
        if (constraint->when_condition) {
            scoped_kept = KeptRows(*constraint->when_condition, chunk_expr_cache, context, gstate.data, num_rows);
        }
        // The instances are fewer than the kept rows where the comparison reads a value that
        // is absent there (syntax_reference.md §5.6): a frame outside its reducers that
        // selects no position, or a bound-side reducer left with no member. Such a row is no
        // instance and nothing is read on it -- but a reducer still reads every kept row of
        // its group, so `scoped_kept` stays what the `when` keeps.
        vector<bool> instance_kept = scoped_kept;
        auto present = FramesPresent(gstate, constraint->source_clause_id, num_rows);
        bool skips_instances = !present.empty();
        for (idx_t row = 0; row < present.size(); row++) {
            instance_kept[row] = instance_kept[row] && present[row];
        }

        // The bound side's reducers, each to a value per group of the kept rows of its own
        // `by` key (syntax_reference.md §5.2), whatever the clause's shape.
        vector<const Expression *> rhs_reducers;
        vector<vector<double>> reducer_values;
        vector<vector<idx_t>> reducer_group_ids;
        if (constraint->rhs_expr->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
            CollectReducers(*constraint->rhs_expr, rhs_reducers);
            std::function<bool(idx_t)> reducer_kept = [&](idx_t row) {
                return scoped_kept[row];
            };
            for (idx_t i = 0; i < rhs_reducers.size(); i++) {
                auto &agg = rhs_reducers[i]->Cast<BoundAggregateExpression>();
                idx_t group_key;
                vector<unique_ptr<Expression>> no_key;
                bool has_by = TryParseGroupKeyTag(agg.alias, group_key);
                if (has_by && group_key >= group_keys.size()) {
                    throw InternalException("DECIDE bound-side reducer names an unknown by key: %s",
                                            agg.ToString());
                }
                vector<idx_t> ids;
                idx_t count = 0;
                vector<string> labels;
                LookupOrBuildPerGroupIds(per_group_cache, has_by ? group_keys[group_key].expressions : no_key,
                                         chunk_expr_cache, context, gstate.data, num_rows, reducer_kept, &scoped_kept,
                                         ids, count, labels);
                vector<bool> absent;
                reducer_values.push_back(EvaluateRhsReducerPerGroup(
                    agg, ids, count, {has_by ? group_key : DConstants::INVALID_INDEX}, group_keys, entity_mappings,
                    chunk_expr_cache, context, gstate.data, num_rows, source_fragments, &absent));
                // A reducer whose frames left it no member is absent, and so is the
                // instance reading it.
                for (idx_t row = 0; row < num_rows; row++) {
                    if (ids[row] != DConstants::INVALID_INDEX && ids[row] < absent.size() && absent[ids[row]]) {
                        instance_kept[row] = false;
                        skips_instances = true;
                    }
                }
                reducer_group_ids.push_back(std::move(ids));
            }
        }
        // A term outside every reducer is read on the instances alone; a reducer's on every
        // kept row of its group.
        const bool reads_groups = constraint->lhs_is_aggregate || constraint->was_minmax_easy;
        const vector<bool> &term_rows = reads_groups ? scoped_kept : instance_kept;

        {
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

                auto masks = KeptRowsOfEach(cond_ptrs, &scoped_kept, chunk_expr_cache, context, gstate.data, num_rows);

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
        clause_phase.Next("extraction.linear_coefficients");
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

        vector<vector<bool>> term_reads;
        vector<const vector<bool> *> term_read_ptrs;
        std::function<bool(idx_t)> kept = [&](idx_t row) {
            return instance_kept[row];
        };
        term_reads.resize(constraint->lhs_terms.size(), term_rows);
        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (term_filters[term_idx].has_filter) {
                for (idx_t row = 0; row < num_rows; row++) {
                    term_reads[term_idx][row] = term_rows[row] && term_filters[term_idx].mask[row];
                }
            }
            term_read_ptrs.push_back(&term_reads[term_idx]);
        }

        idx_t coefficient_row_base = 0;
        while (gstate.data.Scan(scan_state, chunk)) {
            if (constraint->lhs_terms.empty()) {
                continue;
            }
            coef_results.Reset();
            ExecuteOnReadRows(coef_executor, chunk, coefficient_row_base, term_read_ptrs, coef_results);
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                auto &col = eval_const.row_coefficients[term_idx].MutableDense();
                NullSourceContext null_ctx {constraint->lhs_terms[term_idx].coefficient.get(), &chunk,
                                            &source_fragments};
                ExtractDoubleColumn(coef_results.data[term_idx], chunk.size(),
                                    constraint->lhs_terms[term_idx].sign,
                                    col,
                                    "constraint coefficient",
                                    /*allow_infinite=*/false, &null_ctx, &term_reads[term_idx],
                                    coefficient_row_base);
                eval_const.row_coefficients[term_idx].SyncSize();
            }
            coefficient_row_base += chunk.size();
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

        // Row -> group assignment: row_group_ids and num_groups for the evaluated
        // constraint. Empty means "every row, one group".
        clause_phase.Next("extraction.when_per_and_qualifiers");
        // Rows outside the instances: the `when`'s, and those whose comparison reads an
        // absent value.
        bool has_when = constraint->when_condition != nullptr || skips_instances;

        // Group map for the right-hand side's own reducers: the clause's kept rows,
        // without the LHS reducers' own `when` filters. Empty means "every row, one
        // group", the same convention `row_group_ids` uses.
        vector<idx_t> rhs_row_group_ids;

        {
            // The clause's rows are the kept rows (syntax_reference.md §6.1):
            // grouped by the `by` key its reducers share in the aggregate shape, one per
            // class of its `per` key in the per-row shape, else one per kept row. NULL is a
            // key value. The scope checks have already run, so no group or class is empty.
            if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
                // A reducer's members are the rows its own `when` keeps -- a MIN/MAX reads
                // them as members -- while every kept row of a group bounds it. A `<>`
                // emits every kept row, since each one's bound is excluded.
                bool every_kept_row = constraint->comparison_type == ExpressionType::COMPARE_NOTEQUAL;
                std::function<bool(idx_t)> emitted = [&](idx_t row) {
                    return scoped_kept[row] && (every_kept_row || !has_local_filters || local_row_active[row]);
                };
                if (constraint->group_key != DConstants::INVALID_INDEX) {
                    LookupOrBuildPerGroupIds(per_group_cache, group_keys[constraint->group_key].expressions,
                                             chunk_expr_cache, context, gstate.data, num_rows,
                                             emitted, &scoped_kept,
                                             eval_const.row_group_ids, eval_const.num_groups,
                                             eval_const.group_labels, &kept, &rhs_row_group_ids);
                } else if (has_when || has_local_filters) {
                    eval_const.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
                    rhs_row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
                    for (idx_t row = 0; row < num_rows; row++) {
                        eval_const.row_group_ids[row] = emitted(row) ? 0 : DConstants::INVALID_INDEX;
                        rhs_row_group_ids[row] = kept(row) ? 0 : DConstants::INVALID_INDEX;
                    }
                    eval_const.num_groups = 1;
                }
                // A group none of whose kept rows is an instance has no bound, and so no
                // row: its every anchor reads an absent value (D79).
                if (skips_instances && !eval_const.row_group_ids.empty()) {
                    vector<bool> bounded(eval_const.num_groups, false);
                    for (auto group : rhs_row_group_ids) {
                        if (group != DConstants::INVALID_INDEX && group < bounded.size()) {
                            bounded[group] = true;
                        }
                    }
                    for (auto &group : eval_const.row_group_ids) {
                        if (group != DConstants::INVALID_INDEX && !bounded[group]) {
                            group = DConstants::INVALID_INDEX;
                        }
                    }
                }
                // The classes each group's rows stand for -- the aggregate row of a group,
                // or the easy MIN/MAX's member rows -- which an expanded diagnosis reports
                // one by one (D28). Diagnosis prep: paid for only under DIAGNOSE.
                if (diagnose) {
                    const bool one_group = eval_const.row_group_ids.empty();
                    eval_const.group_class_set =
                        CollectGroupClasses(constraint->per_key, constraint->class_key, per_group_cache,
                                            chunk_expr_cache, context, gstate.data, num_rows, scoped_kept,
                                            one_group ? vector<idx_t>() : rhs_row_group_ids,
                                            one_group ? 1 : eval_const.num_groups, gstate.class_sets);
                }
            } else if (constraint->per_key == DecideConstraint::PerKeyRole::ONE_ROW_PER_CLASS) {
                // One row per variant of each class of the `per` key among the kept rows
                // (D64): its first, which stands for the variant's rows because they agree
                // on every field the clause reads -- its data, its decisions' instances, its
                // reducers' `by` keys. Without a field, one row per class.
                vector<idx_t> class_ids;
                idx_t num_classes = 0;
                vector<string> labels;
                LookupOrBuildPerGroupIds(per_group_cache, constraint->class_key, chunk_expr_cache, context,
                                         gstate.data, num_rows, kept, &instance_kept,
                                         class_ids, num_classes, labels);
                vector<idx_t> variant_ids;
                idx_t num_variants = num_classes;
                if (constraint->signature != DConstants::INVALID_INDEX) {
                    BuildSignatureIds(signatures[constraint->signature], group_keys, variable_scopes,
                                      entity_mappings, SignatureFrames {frames, gstate.frame_maps, prepared},
                                      chunk_expr_cache, context, gstate.data, num_rows,
                                      &instance_kept, variant_ids, num_variants);
                }
                const auto &instance_ids = variant_ids.empty() ? class_ids : variant_ids;
                eval_const.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
                vector<idx_t> class_of(num_variants, DConstants::INVALID_INDEX);
                vector<idx_t> first_row(num_variants, DConstants::INVALID_INDEX);
                vector<idx_t> variants_per_class(num_classes, 0);
                for (idx_t row = 0; row < num_rows; row++) {
                    auto id = instance_ids[row];
                    if (id != DConstants::INVALID_INDEX && class_of[id] == DConstants::INVALID_INDEX) {
                        class_of[id] = class_ids[row];
                        first_row[id] = row;
                        variants_per_class[class_ids[row]]++;
                        eval_const.row_group_ids[row] = id;
                    }
                }
                eval_const.num_groups = num_variants;
                if (num_variants > num_classes) {
                    RecordVariants(signatures[constraint->signature], "constraints", num_variants, num_classes);
                }
                // A variant is labelled by its class, and by its own field values where its
                // class has more than one.
                eval_const.group_labels.resize(num_variants);
                vector<string> variant_labels;
                if (num_variants > num_classes) {
                    variant_labels = DescribeVariants(signatures[constraint->signature], eval_const.row_group_ids,
                                                      chunk_expr_cache, context, gstate, num_rows);
                }
                for (idx_t id = 0; id < num_variants; id++) {
                    auto class_id = class_of[id];
                    auto &label = eval_const.group_labels[id];
                    label = class_id < labels.size() ? labels[class_id] : string();
                    if (variants_per_class[class_id] > 1 && id < variant_labels.size() && !variant_labels[id].empty()) {
                        label += label.empty() ? variant_labels[id] : ", " + variant_labels[id];
                    }
                }
                // Variants that differ only by the decision instance they read (`per t.g: x
                // <= t.v` over two rows of one class with one `v`) share their values: each
                // is named by the row it is read at as well.
                unordered_map<string, idx_t> label_uses;
                for (idx_t id = 0; id < num_variants; id++) {
                    label_uses[eval_const.group_labels[id]]++;
                }
                for (idx_t id = 0; id < num_variants; id++) {
                    auto &label = eval_const.group_labels[id];
                    if (variants_per_class[class_of[id]] > 1 && label_uses[label] > 1) {
                        label += StringUtil::Format("%srow %llu", label.empty() ? "" : ", ", first_row[id] + 1);
                    }
                }
                eval_const.rows_stand_with_their_class = !has_when;
                // Each variant's row stands for its class (D28), under DIAGNOSE only, and is
                // named as its label says where its class has more than one.
                if (diagnose) {
                    eval_const.group_class_set =
                        CollectGroupClasses(constraint->per_key, constraint->class_key, per_group_cache,
                                            chunk_expr_cache, context, gstate.data, num_rows, scoped_kept,
                                            eval_const.row_group_ids, num_variants, gstate.class_sets);
                    for (idx_t id = 0; id < num_variants; id++) {
                        auto set = eval_const.group_class_set[id];
                        if (variants_per_class[class_of[id]] > 1 && set != DConstants::INVALID_INDEX) {
                            gstate.class_sets[set].labels = {eval_const.group_labels[id]};
                        }
                    }
                }
            } else if (has_when) {
                eval_const.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
                for (idx_t row = 0; row < num_rows; row++) {
                    eval_const.row_group_ids[row] = kept(row) ? 0 : DConstants::INVALID_INDEX;
                }
                eval_const.num_groups = 1;
            }
        }

        // Relation-qualified reducers: drop the duplicate rows the join introduced, now
        // that `when` selection and `per` partitioning have fixed the groups to de-duplicate
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
        // A MIN/MAX reads the rows of its group as members, so a row the de-duplication
        // drops leaves the group: left in with zero coefficients, it would be a member
        // pinned at 0. (A SUM or AVG row is unchanged by a zero member.)
        if (!constraint->minmax_agg_type.empty()) {
            for (idx_t row = 0; row < num_rows; row++) {
                bool member = constraint->lhs_terms.empty();
                for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size() && !member; term_idx++) {
                    member = !term_filters[term_idx].has_filter || term_filters[term_idx].mask[row];
                }
                if (member) {
                    continue;
                }
                if (eval_const.row_group_ids.empty()) {
                    eval_const.row_group_ids.assign(num_rows, 0);
                    eval_const.num_groups = 1;
                }
                eval_const.row_group_ids[row] = DConstants::INVALID_INDEX;
            }
        }


        // Evaluate RHS
        // RHS can be a constant, an aggregate (scalar), or a row-varying expression (for row-wise constraints)

        clause_phase.Next("extraction.rhs");
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
                eval_const.rhs_label =
                    RenderDecideSource(*constraint->rhs_expr, source_fragments, entity_scopes, &group_keys);
            }
            eval_const.rhs_values.Reserve(num_rows);

            // Reducers on this side were evaluated first, to one value per group, and are
            // replaced by an extra chunk column carrying that value broadcast over the
            // group's rows. What remains is an ordinary per-row expression, so a mixed
            // bound like `MIN(cap) + demand * 2` needs no special case: the reducer part
            // is constant within the group and the rest still varies, which is exactly
            // what the reduction after this block expects.
            std::unordered_map<const Expression *, idx_t> agg_substitutions;
            const idx_t data_columns = gstate.data.ColumnCount();
            for (idx_t i = 0; i < rhs_reducers.size(); i++) {
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
                            idx_t g = reducer_group_ids[i][abs_row];
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
                ExecuteOnReadRows(rhs_executor, *eval_chunk, scanned, &instance_kept, rhs_result);
                auto &rhs_col = eval_const.rhs_values.MutableDense();
                // Infinities are admitted here and nowhere else: this column becomes
                // the model row's `rhs`, where ±inf is a meaningful bound. Canonical-
                // ization rebuilds `x + v <= K` as `x <= K - v`, so an infinite K that
                // was absorbed as a column bound in its bare spelling must stay legal
                // in its rebuilt one. `inf - inf` still yields NaN and is still refused.
                NullSourceContext null_ctx {constraint->rhs_expr.get(), eval_chunk,
                                            &source_fragments};
                ExtractDoubleColumn(rhs_result.data[0], scan_chunk.size(), 1.0, rhs_col,
                                    "constraint right-hand side", /*allow_infinite=*/true,
                                    &null_ctx, &instance_kept, scanned);
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
                                  ? RenderDecideSource(*constraint->rhs_expr, source_fragments, entity_scopes, &group_keys)
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
        if (constraint->ne_clause_idx != DConstants::INVALID_INDEX &&
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
                                              context, gstate.data, num_rows, "bilinear constraint coefficient",
                                              scoped_kept, source_fragments);
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
        clause_phase.Next("extraction.quadratic_coefficients");
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
                    eqg.variable_indices.push_back(gstate.ReadVariable(term));
                    const Expression &cached =
                        CachedTransformToChunkExpression(chunk_expr_cache, *term.coefficient, context);
                    q_types.push_back(cached.return_type);
                    q_executor.AddExpression(cached);
                    eqg.row_coefficients[inner_idx].Reserve(num_rows);
                }

                // The group's terms are read on the rows the clause keeps that the group's
                // own `when` keeps too.
                vector<bool> q_reads = scoped_kept;
                if (quadratic_filters[group_idx].has_filter) {
                    for (idx_t row = 0; row < num_rows; row++) {
                        q_reads[row] = q_reads[row] && quadratic_filters[group_idx].mask[row];
                    }
                }
                if (!qg.inner_terms.empty()) {
                    ColumnDataScanState qscan;
                    gstate.data.InitializeScan(qscan);
                    DataChunk qchunk;
                    qchunk.Initialize(context, gstate.data.Types());
                    DataChunk q_results;
                    q_results.Initialize(context, q_types);
                    idx_t q_offset = 0;
                    while (gstate.data.Scan(qscan, qchunk)) {
                        q_results.Reset();
                        ExecuteOnReadRows(q_executor, qchunk, q_offset, &q_reads, q_results);
                        for (idx_t inner_idx = 0; inner_idx < qg.inner_terms.size(); inner_idx++) {
                            auto &col = eqg.row_coefficients[inner_idx].MutableDense();
                            NullSourceContext null_ctx {qg.inner_terms[inner_idx].coefficient.get(),
                                                        &qchunk, &source_fragments};
                            ExtractDoubleColumn(q_results.data[inner_idx], qchunk.size(),
                                                qg.inner_terms[inner_idx].sign,
                                                col,
                                                "quadratic constraint coefficient",
                                                /*allow_infinite=*/false, &null_ctx,
                                                &q_reads, q_offset);
                            eqg.row_coefficients[inner_idx].SyncSize();
                        }
                        q_offset += qchunk.size();
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

        (c < num_constraints ? gstate.evaluated_constraints : gstate.evaluated_absorbed_constraints)
            .push_back(std::move(eval_const));
    }
}

PhysicalDecide::EvaluatedClauses PhysicalDecide::EvaluateObjective(ClientContext &context,
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
    // The objective's `when` joins every term's own, only the rows a term keeps are read,
    // and a decision outside its reducers counts once (syntax_reference.md §4).
    // Its empty-set errors are the scope checks'.
    // A term's `when` with the objective's folded in.
    auto fold_objective_when = [&](TermFilterState &state) {
        if (!objective_has_when) {
            return;
        }
        if (!state.has_filter) {
            state.mask = objective_when_mask;
            state.has_filter = true;
            return;
        }
        for (idx_t row = 0; row < num_rows; row++) {
            state.mask[row] = state.mask[row] && objective_when_mask[row];
        }
    };

    if (gstate.objective) {
        if (gstate.objective->has_quadratic) {
            gstate.has_quadratic_objective = true;
            gstate.quadratic_sign = gstate.objective->quadratic_sign;
        }

        objective_has_when = (gstate.objective->when_condition != nullptr);
        if (objective_has_when) {
            objective_when_mask =
                KeptRows(*gstate.objective->when_condition, chunk_expr_cache, context, gstate.data, num_rows);
            if (objective_when_mask.size() != num_rows) {
                throw InternalException("DECIDE objective WHEN mask size mismatch: expected %llu rows, got %llu",
                                        num_rows, objective_when_mask.size());
            }
        }
        // The objective is produced once, read at its first kept row (D66): a frame its
        // root reads that selects no position there leaves it no value, and it cannot be
        // skipped as an instance can (syntax_reference.md §5.6).
        const idx_t objective_row = FirstKeptRow(objective_has_when ? &objective_when_mask : nullptr, num_rows);
        for (idx_t f = 0; f < frames.size() && objective_row < num_rows; f++) {
            auto &frame = frames[f];
            if (frame.MayLeaveObjectiveUnset() && gstate.frame_maps[f].Missing(objective_row)) {
                throw InvalidInputException("DECIDE objective: %s selects no position; the objective is produced "
                                            "once and cannot be skipped. Give the frame an else.",
                                            frame.text);
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
            //! Only set for the linear bucket: the model builder's scalar-once shortcut
            //! needs to tell a reducer-summed term from a row-invariant sibling term.
            //! Quadratic terms have no such shortcut, so this stays null there.
            vector<LinearTermReduction> *out_reductions = nullptr;
        };
        vector<ObjBucket> buckets;
        if (!gstate.objective->terms.empty()) {
            buckets.push_back({&gstate.objective->terms,
                               &gstate.evaluated_objective_coefficients,
                               &gstate.objective_variable_indices,
                               &obj_linear_term_filters,
                               &gstate.objective_term_reductions});
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
                    b.out_var_indices->push_back(gstate.ReadVariable(term));
                    if (b.out_reductions) {
                        b.out_reductions->push_back(term.reduction);
                    }
                }
            }

            if (!obj_filter_slots.empty()) {
                vector<const Expression *> cond_ptrs;
                cond_ptrs.reserve(obj_filter_slots.size());
                for (auto &s : obj_filter_slots) cond_ptrs.push_back(s.cond);
                auto masks = KeptRowsOfEach(cond_ptrs, objective_has_when ? &objective_when_mask : nullptr,
                                            chunk_expr_cache, context, gstate.data, num_rows);
                for (idx_t i = 0; i < obj_filter_slots.size(); i++) {
                    if (masks[i].size() != num_rows) {
                        throw InternalException(
                            "DECIDE objective aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                            num_rows, masks[i].size());
                    }
                    obj_filter_slots[i].state->mask = std::move(masks[i]);
                }
            }
            for (auto &b : buckets) {
                for (auto &state : *b.out_term_filters) {
                    fold_objective_when(state);
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

            // A scoped objective computes a term only on the rows the term reads.
            vector<const vector<bool> *> obj_reads;
            for (auto &entry : route) {
                auto &filter = (*buckets[entry.first].out_term_filters)[entry.second];
                obj_reads.push_back(filter.has_filter ? &filter.mask : nullptr);
            }

            idx_t obj_row_offset = 0;
            while (gstate.data.Scan(obj_scan_state, obj_chunk)) {
                if (route.empty()) {
                    continue;
                }
                obj_results.Reset();
                ExecuteOnReadRows(obj_executor, obj_chunk, obj_row_offset, obj_reads, obj_results);
                for (idx_t j = 0; j < route.size(); j++) {
                    auto &b = buckets[route[j].first];
                    idx_t term_idx = route[j].second;
                    auto &out = (*b.out_coeffs)[term_idx].MutableDense();
                    int term_sign = (*b.src_terms)[term_idx].sign;
                    NullSourceContext null_ctx {(*b.src_terms)[term_idx].coefficient.get(), &obj_chunk,
                                                &source_fragments};
                    auto &filter = (*b.out_term_filters)[term_idx];
                    ExtractDoubleColumn(obj_results.data[j], obj_chunk.size(),
                                        static_cast<double>(term_sign), out,
                                        "objective coefficient",
                                        /*allow_infinite=*/false, &null_ctx,
                                        filter.has_filter ? &filter.mask : nullptr, obj_row_offset);
                    (*b.out_coeffs)[term_idx].SyncSize();
                }
                obj_row_offset += obj_chunk.size();
            }

            // Apply each term's own `when` filter and the objective's `when`
            // mask once per bucket. Both masks are shared between linear and
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
                if (b.out_reductions) {
                    CountTupleTermsOnce(*b.src_terms, variable_scopes, *b.out_coeffs,
                                        objective_has_when ? &objective_when_mask : nullptr, num_rows);
                }
            }

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
                auto masks = KeptRowsOfEach(cond_ptrs, objective_has_when ? &objective_when_mask : nullptr,
                                            chunk_expr_cache, context, gstate.data, num_rows);
                for (idx_t i = 0; i < bil_slots.size(); i++) {
                    idx_t tidx = bil_slots[i].term_idx;
                    if (masks[i].size() != num_rows) {
                        throw InternalException(
                            "DECIDE objective aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                            num_rows, masks[i].size());
                    }
                    obj_bilinear_filters[tidx].mask = std::move(masks[i]);
                }
            }
            for (auto &state : obj_bilinear_filters) {
                fold_objective_when(state);
            }
        }

        // No extra debug here; solver output will show timings/objective

        // Evaluate bilinear term coefficients (non-Boolean pairs left by optimizer).
        if (gstate.objective->has_bilinear) {
            vector<bool> every_row(num_rows, true);
            gstate.evaluated_bilinear_terms = EvaluateBilinearTerms(
                gstate.objective->bilinear_terms, obj_bilinear_filters, chunk_expr_cache, context, gstate.data,
                num_rows, "bilinear objective coefficient", every_row, source_fragments);
            // A product outside every reducer (`MAXIMIZE sum(x) + s * u`) counts once, as a
            // linear term there does; one inside a reducer sums over its rows.
            idx_t first_kept = FirstKeptRow(objective_has_when ? &objective_when_mask : nullptr, num_rows);
            for (idx_t i = 0; i < gstate.evaluated_bilinear_terms.size(); i++) {
                if (gstate.objective->bilinear_terms[i].reduction == LinearTermReduction::NONE) {
                    CountOnce(gstate.evaluated_bilinear_terms[i].row_coefficients, first_kept, first_kept);
                }
            }
        }

        // An AVG with a `by` key -- a nested `sum(per k: avg(e) by (k))` the value rewrite
        // inlined (D75) -- divides by its group's count: the groups of its key among the
        // rows it reads, NULL a value.
        auto group_avg = [&](idx_t group_key, TermFilterState &state) {
            if (!state.avg_scale || group_key == DConstants::INVALID_INDEX) {
                return;
            }
            std::function<bool(idx_t)> reads = [&](idx_t row) {
                return !state.has_filter || state.mask[row];
            };
            vector<string> labels;
            LookupOrBuildPerGroupIds(per_group_cache, group_keys[group_key].expressions, chunk_expr_cache, context,
                                     gstate.data, num_rows, reads, nullptr, state.avg_group_ids, state.avg_num_groups,
                                     labels);
        };
        for (idx_t i = 0; i < gstate.objective->terms.size() && i < obj_linear_term_filters.size(); i++) {
            group_avg(gstate.objective->terms[i].avg_group_key, obj_linear_term_filters[i]);
        }
        for (idx_t i = 0; i < gstate.objective->squared_terms.size() && i < obj_quadratic_term_filters.size(); i++) {
            group_avg(gstate.objective->squared_terms[i].avg_group_key, obj_quadratic_term_filters[i]);
        }
        for (idx_t i = 0; i < gstate.objective->bilinear_terms.size() && i < obj_bilinear_filters.size(); i++) {
            group_avg(gstate.objective->bilinear_terms[i].avg_group_key, obj_bilinear_filters[i]);
        }
    }

    EvaluatedClauses result;
    result.linear_filters = std::move(obj_linear_term_filters);
    result.quadratic_filters = std::move(obj_quadratic_term_filters);
    result.bilinear_filters = std::move(obj_bilinear_filters);

    return result;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 2, sub-phase 3 -- Evaluate composed MIN/MAX clauses
//===--------------------------------------------------------------------===//

// Composed MIN/MAX: additive clauses mixing SUM/AVG/MIN/MAX, in a constraint
// (`SUM(a) + MAX(b) <= K`) or in the objective. This phase evaluates the parts that
// need a row -- each inner term's per-row coefficient, the query-wide factor on a
// reducer, the term's WHEN mask, the constant RHS. The rows and auxiliaries they
// become are emitted later, by FormulateModel; nothing evaluated here depends on a
// variable bound, so a second formulation against a widened box reuses it as is.
void PhysicalDecide::EvaluateComposedClauses(ClientContext &context, DecideGlobalSinkState &gstate,
                                             idx_t num_rows, ChunkExprCache &chunk_expr_cache,
                                             PerGroupCache &per_group_cache,
                                             const vector<EntityMapping> &entity_mappings,
                                             EvaluatedClauses &evaluated) const {
    if (composed_minmax_constraints.empty() && composed_minmax_objective_terms.empty()) {
        return;
    }
    // Evaluate a DecideTerm's per-row coefficient (scaled by term.sign). `what` is
    // "constraint" or "objective", naming the clause in any error. `read_rows` are the rows
    // the term reads: every other row is 0 and never evaluated for NULL.
    auto EvaluateTermCoefs = [&](const DecideTerm &term, const char *what,
                                 const vector<bool> &read_rows) -> vector<double> {
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
            ExecuteOnReadRows(exec, chunk, coefs.size(), &read_rows, result);
            for (idx_t r = 0; r < chunk.size(); r++) {
                if (!read_rows[coefs.size()]) {
                    coefs.push_back(0.0);
                    continue;
                }
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
        // Collect filter expressions for batch evaluation. The clause's terms share its
        // `when`, folded in front of each term's own.
        vector<const Expression *> cond_ptrs;
        for (auto &term : terms) {
            if (term.filter) cond_ptrs.push_back(term.filter.get());
        }
        auto masks = KeptRowsOfEach(cond_ptrs, nullptr, chunk_expr_cache, context, gstate.data, num_rows);

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
                // Named as the plan prints the reducer, so a diagnosis over the extremum
                // column reads `max(ship)`.
                ta.label = term.agg_name + "(" +
                           RenderDecideSource(*term.inner_expr, source_fragments, entity_scopes, &group_keys) + ")";
            }
            ta.inner_terms = &term.inner_terms;
            if (term.filter) {
                ta.filter_mask = std::move(masks[mask_slot++]);
            } else {
                ta.filter_mask.assign(num_rows, true);
            }
            // Fold in the relation qualifier's de-duplication mask, exactly as the
            // non-composed reducer paths do. A composed objective has one group, so
            // `row_group_ids` is empty. Applied uniformly:
            // for MIN/MAX it is provably a no-op (every row of an identity carries the
            // same value, so dropping repeats cannot move an extremum), which keeps
            // one code path instead of a special case that has to stay in sync. A term
            // keeps the first row of each class among the rows it reads.
            if (term.qualifier_scope_idx != DConstants::INVALID_INDEX) {
                auto keep = BuildQualifierKeepMask(entity_mappings, term.qualifier_scope_idx, {}, num_rows,
                                                   &ta.filter_mask);
                for (idx_t row = 0; row < num_rows; row++) {
                    ta.filter_mask[row] = ta.filter_mask[row] && keep[row];
                }
            }
            for (auto &inner_t : term.inner_terms) {
                ta.per_term_coefs.push_back(EvaluateTermCoefs(inner_t, what, ta.filter_mask));
            }
            evaluated.push_back(std::move(ta));
        }

        // Empty sets are the scope checks' errors (they have run).
        return evaluated;
    };

    for (auto &spec : composed_minmax_constraints) {
        // The bound is row-invariant: the optimizer asserted it (AssertComposable) and the
        // value rewrite takes every other shape. Decided by foldability, since
        // canonicalization rebuilds the bound side as an additive chain, so a constant
        // arrives as `(0 - 3) + 0` just as legitimately as `-3`.
        if (!spec.rhs_expr->IsFoldable()) {
            throw InternalException("DECIDE: a composed MIN/MAX clause reached execution with a bound that is "
                                    "not a constant: %s",
                                    spec.rhs_expr->ToString());
        }
        EvaluatedClauses::ComposedConstraint clause;
        clause.rhs = ExpressionExecutor::EvaluateScalar(context, *spec.rhs_expr)
                         .DefaultCastAs(LogicalType::DOUBLE)
                         .GetValue<double>();
        clause.terms = EvaluateComposedTerms(spec.terms, "constraint");
        clause.repair_group_id =
            gstate.constraints.size() + gstate.absorbed_constraints.size() + evaluated.composed_constraints.size();
        // The classes its one row stands for (D28), under DIAGNOSE: its `per` key's values
        // among the rows its `when` keeps, or those rows with no `per`.
        if (diagnose) {
            vector<bool> kept_rows(num_rows, true);
            if (spec.when) {
                kept_rows = KeptRows(*spec.when, chunk_expr_cache, context, gstate.data, num_rows);
            }
            clause.class_set = CollectGroupClasses(spec.per_key, spec.class_key, per_group_cache, chunk_expr_cache,
                                                   context, gstate.data, num_rows, kept_rows, {}, 1,
                                                   gstate.class_sets)[0];
        }
        evaluated.composed_constraints.push_back(std::move(clause));
    }

    if (!composed_minmax_objective_terms.empty()) {
        evaluated.composed_objective_terms =
            EvaluateComposedTerms(composed_minmax_objective_terms, "objective");
    }
}


//===--------------------------------------------------------------------===//
// Finalize: PHASE 2, sub-phase 4 -- the value rewrite's helper decisions
//===--------------------------------------------------------------------===//

//! Each term's coefficient (its sign applied) on the rows `read` keeps, 0 elsewhere, in
//! one scan: the columns of a lifted reducer's members.
static void EvaluateTermColumns(ClientContext &context, ColumnDataCollection &data, ChunkExprCache &chunk_expr_cache,
                                const vector<DecideTerm> &terms, const vector<bool> &read, idx_t num_rows,
                                const vector<string> &source_fragments, vector<CoefficientColumn> &columns) {
    columns.resize(terms.size());
    if (terms.empty()) {
        return;
    }
    ExpressionExecutor executor(context);
    vector<LogicalType> types;
    for (idx_t t = 0; t < terms.size(); t++) {
        const Expression &cached = CachedTransformToChunkExpression(chunk_expr_cache, *terms[t].coefficient, context);
        types.push_back(cached.return_type);
        executor.AddExpression(cached);
        columns[t].Reserve(num_rows);
    }
    DataChunk results;
    results.Initialize(context, types);
    ColumnDataScanState scan_state;
    data.InitializeScan(scan_state);
    DataChunk chunk;
    chunk.Initialize(context, data.Types());
    idx_t row_base = 0;
    while (data.Scan(scan_state, chunk)) {
        results.Reset();
        ExecuteOnReadRows(executor, chunk, row_base, &read, results);
        for (idx_t t = 0; t < terms.size(); t++) {
            NullSourceContext null_ctx {terms[t].coefficient.get(), &chunk, &source_fragments};
            ExtractDoubleColumn(results.data[t], chunk.size(), terms[t].sign, columns[t].MutableDense(),
                                "constraint coefficient", /*allow_infinite=*/false, &null_ctx, &read, row_base);
            columns[t].SyncSize();
        }
        row_base += chunk.size();
    }
}

void PhysicalDecide::EvaluateReducerValueLinks(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                               ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                               const vector<EntityMapping> &entity_mappings) const {
    for (auto &link : reducer_value_links) {
        DecideProfileScope clause_profile("extraction.value_link");
        ReducerValueLinkSpec spec;
        spec.value_var = link.value_var;
        spec.agg_name = link.agg_name;
        spec.need_envelope = link.need_envelope;
        spec.need_closing = link.need_closing;
        spec.label = link.label;
        auto &ec = spec.rows;
        ec.kind = ConstraintKind::STRUCTURAL;
        ec.lhs_is_aggregate = true;
        ec.comparison_type = ExpressionType::COMPARE_EQUAL;
        ec.source_clause_id = link.source_clause_id;
        ec.removal_group_id = link.removal_group_id;
        ec.rhs_values.AssignScalar(num_rows, 0.0);
        ec.rhs_is_shared_scalar = true;

        // The rows the clause keeps, then those the reducer's own `when` keeps: its
        // members. Nothing is computed on any other row.
        vector<bool> kept(num_rows, true);
        if (link.when) {
            kept = KeptRows(*link.when, chunk_expr_cache, context, gstate.data, num_rows);
        }
        vector<bool> read = link.filter ? KeptRows(*link.filter, chunk_expr_cache, context, gstate.data, num_rows, &kept)
                                        : kept;

        // One group per `by` value among the kept rows (NULL a value); a member row
        // stands in its group, every other row outside. The scope checks have passed, so
        // no group is empty.
        std::function<bool(idx_t)> is_member = [&](idx_t row) { return read[row]; };
        vector<unique_ptr<Expression>> no_key;
        LookupOrBuildPerGroupIds(per_group_cache,
                                 link.group_key != DConstants::INVALID_INDEX ? group_keys[link.group_key].expressions
                                                                             : no_key,
                                 chunk_expr_cache, context, gstate.data, num_rows, is_member,
                                 &kept, ec.row_group_ids, ec.num_groups, ec.group_labels);

        // The members' coefficients, computed on the member rows alone.
        EvaluateTermColumns(context, gstate.data, chunk_expr_cache, link.inner_terms, read, num_rows, source_fragments,
                            ec.row_coefficients);
        for (auto &term : link.inner_terms) {
            ec.variable_indices.push_back(gstate.ReadVariable(term));
            ec.linear_term_reductions.push_back(LinearTermReduction::SUM);
            ec.coefficient_labels.push_back(term.coefficient->GetName());
        }
        // Its own `per`: one member per class of that key within each group. A row the
        // de-duplication drops leaves the group, so that a MIN/MAX reads no member pinned
        // at 0. AVG then divides by the members a group has.
        TermFilterState members;
        members.has_filter = true;
        members.mask = read;
        if (link.qualifier_scope_idx != DConstants::INVALID_INDEX) {
            ApplyQualifierToFilter(entity_mappings, link.qualifier_scope_idx, ec.row_group_ids, num_rows, members);
            for (idx_t row = 0; row < num_rows; row++) {
                if (!members.mask[row]) {
                    ec.row_group_ids[row] = DConstants::INVALID_INDEX;
                }
            }
        }
        const bool is_avg = link.agg_name == "avg";
        for (auto &column : ec.row_coefficients) {
            MaskCoefficientColumn(column, members.mask);
            if (is_avg) {
                ScaleAvgRows(column, true, members.mask, /*quadratic_inner=*/false, num_rows, ec.row_group_ids,
                             ec.num_groups);
            }
        }
        ec.avg_scaled = is_avg;
        gstate.evaluated_value_links.push_back(std::move(spec));
    }
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 3a, assembly -- BuildSolverInput
//===--------------------------------------------------------------------===//

//! A frame read is its base decision read at another row, so its type and its box are its
//! base's (SolverInput::frame_reads): every pass that reads a term's box -- a Big-M, a row's
//! range -- then reads one that holds at every row the read lands on. Called wherever the
//! boxes are rewritten wholesale: after implied bounds, and after a diagnosis widens them.
static void SyncFrameReadBoxes(SolverInput &input) {
    for (idx_t i = 0; i < input.frame_reads.size(); i++) {
        auto read = input.first_frame_read + i;
        auto base = input.frame_reads[i].base;
        input.variable_types[read] = input.variable_types[base];
        input.lower_bounds[read] = input.lower_bounds[base];
        input.upper_bounds[read] = input.upper_bounds[base];
        input.rigid_lower_bounds[read] = input.rigid_lower_bounds[base];
        input.rigid_upper_bounds[read] = input.rigid_upper_bounds[base];
    }
}

SolverInput PhysicalDecide::BuildSolverInput(DecideGlobalSinkState &gstate, idx_t num_rows,
                                             vector<EntityMapping> entity_mappings,
                                             EvaluatedClauses &evaluated,
                                             VarIndexer &out_var_indexer) const {
    DecideProfileScope phase("assembly.copy_and_bounds");
    idx_t num_decide_vars = decide_variables.size();

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
    // A helper decision of the value rewrite holds a reducer's value, which may be
    // negative: no default floor. The formulation pass boxes it by its members' reach
    // (DeriveReducerValueBounds), after the rigid box below is captured with it free. A
    // floor the user wrote on it stands, though: a nested reducer's clause is a bound on
    // its outer helper alone (`per g: sum(max(x) by (g, h)) by (g) >= 3`), which bound
    // absorption folds into the helper's box.
    vector<bool> is_helper(solver_input.lower_bounds.size(), false);
    for (auto &link : gstate.evaluated_value_links) {
        if (link.value_var < is_helper.size()) {
            is_helper[link.value_var] = true;
        }
    }
    for (idx_t var = 0; var < solver_input.lower_bounds.size(); var++) {
        auto &lb = solver_input.lower_bounds[var];
        if (lb <= ABSORBED_LOWER_UNSET) {
            lb = is_helper[var] ? -NumericLimits<double>::Maximum() : 0.0;
        }
    }
    solver_input.upper_bounds = gstate.absorbed_upper_bounds;

    // Everything below this point is stage 06 working on the evaluated model, so
    // hand the constraints and the formulation links over before the first pass.
    solver_input.constraints = std::move(gstate.evaluated_constraints);
    solver_input.class_sets = std::move(gstate.class_sets);
    solver_input.minmax_clause_labels = minmax_clause_labels;
    solver_input.ne_clause_labels = ne_clause_labels;
    for (auto &link : bilinear_links) {
        solver_input.bilinear_links.push_back(BilinearLinkSpec {
            link.aux_idx, link.bool_var_idx, link.other_var_idx,
            link.source_clause_id, link.removal_group_id});
    }
    for (auto &link : abs_maximize_links) {
        solver_input.abs_maximize_links.push_back(AbsMaximizeLinkSpec {link.aux_idx, link.y_idx});
    }
    solver_input.reducer_value_links = std::move(gstate.evaluated_value_links);

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
    // The decisions frames read at another row (SolverInput::frame_reads), after the plan's
    // own: each its base read, at each row, at the row its frame selects -- or the row
    // itself where it selects none, where every term reading it is 0.
    solver_input.first_frame_read = num_decide_vars;
    if (!gstate.frame_reads.empty()) {
        solver_input.variable_scopes.resize(num_decide_vars, DecideVarScopeInfo::Row());
    }
    for (auto &read : gstate.frame_reads) {
        SolverInput::FrameRead frame_read;
        frame_read.base = read.first;
        auto &map = gstate.frame_maps[read.second];
        frame_read.rows.resize(num_rows);
        for (idx_t row = 0; row < num_rows; row++) {
            auto selected = map.SelectedRow(row);
            frame_read.rows[row] = selected == DConstants::INVALID_INDEX ? row : selected;
        }
        solver_input.frame_reads.push_back(std::move(frame_read));
        DecideVarScopeInfo scope;
        scope.scope = DecideVarScope::FRAME_READ;
        solver_input.variable_scopes.push_back(scope);
    }
    if (!gstate.frame_reads.empty()) {
        auto total = num_decide_vars + gstate.frame_reads.size();
        solver_input.num_decide_vars = total;
        solver_input.variable_types.resize(total);
        solver_input.lower_bounds.resize(total);
        solver_input.upper_bounds.resize(total);
        solver_input.rigid_lower_bounds.resize(total);
        solver_input.rigid_upper_bounds.resize(total);
        SyncFrameReadBoxes(solver_input);
    }

    phase.Next("assembly.implied_bounds");
    DecidePropagateImpliedBounds(solver_input.constraints, solver_input.lower_bounds,
                                 solver_input.upper_bounds, num_rows);
    SyncFrameReadBoxes(solver_input);
    phase.Next("assembly.objective_and_avg");
    // Objective (linear part)
    solver_input.objective_coefficients = std::move(gstate.evaluated_objective_coefficients);
    solver_input.objective_variable_indices = std::move(gstate.objective_variable_indices);
    solver_input.objective_term_reductions = std::move(gstate.objective_term_reductions);
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
    // Relation-qualified reducers in the objective: same de-duplication as on the
    // constraint side, over the objective's one group, applied before AVG scaling reads the
    // surviving-row counts.
    const vector<idx_t> objective_row_group_ids;
    if (gstate.objective) {
        for (idx_t term_idx = 0; term_idx < gstate.objective->terms.size() &&
                                 term_idx < evaluated.linear_filters.size();
             term_idx++) {
            idx_t scope_idx = gstate.objective->terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.entity_mappings, scope_idx,
                                   objective_row_group_ids, num_rows,
                                   evaluated.linear_filters[term_idx]);
            MaskCoefficientColumn(solver_input.objective_coefficients[term_idx],
                                  evaluated.linear_filters[term_idx].mask);
        }
        for (idx_t term_idx = 0; term_idx < gstate.objective->squared_terms.size() &&
                                 term_idx < evaluated.quadratic_filters.size();
             term_idx++) {
            idx_t scope_idx = gstate.objective->squared_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.entity_mappings, scope_idx,
                                   objective_row_group_ids, num_rows,
                                   evaluated.quadratic_filters[term_idx]);
            MaskCoefficientColumn(solver_input.quadratic_inner_coefficients[term_idx],
                                  evaluated.quadratic_filters[term_idx].mask);
        }
        for (idx_t term_idx = 0; term_idx < gstate.objective->bilinear_terms.size() &&
                                 term_idx < evaluated.bilinear_filters.size();
             term_idx++) {
            idx_t scope_idx = gstate.objective->bilinear_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.entity_mappings, scope_idx,
                                   objective_row_group_ids, num_rows,
                                   evaluated.bilinear_filters[term_idx]);
            MaskCoefficientColumn(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
                                  evaluated.bilinear_filters[term_idx].mask);
        }
    }

    for (idx_t term_idx = 0; term_idx < evaluated.linear_filters.size(); term_idx++) {
        if (!evaluated.linear_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleAvgRows(solver_input.objective_coefficients[term_idx],
                    evaluated.linear_filters[term_idx].has_filter,
                    evaluated.linear_filters[term_idx].mask, /*quadratic_inner=*/false, num_rows,
                    evaluated.linear_filters[term_idx].avg_group_ids,
                    evaluated.linear_filters[term_idx].avg_num_groups);
    }
    for (idx_t term_idx = 0; term_idx < evaluated.quadratic_filters.size(); term_idx++) {
        if (!evaluated.quadratic_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleAvgRows(solver_input.quadratic_inner_coefficients[term_idx],
                    evaluated.quadratic_filters[term_idx].has_filter,
                    evaluated.quadratic_filters[term_idx].mask, /*quadratic_inner=*/true, num_rows,
                    evaluated.quadratic_filters[term_idx].avg_group_ids,
                    evaluated.quadratic_filters[term_idx].avg_num_groups);
    }

    for (idx_t term_idx = 0; term_idx < evaluated.bilinear_filters.size(); term_idx++) {
        if (!evaluated.bilinear_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleAvgRows(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
                    evaluated.bilinear_filters[term_idx].has_filter,
                    evaluated.bilinear_filters[term_idx].mask, /*quadratic_inner=*/false, num_rows,
                    evaluated.bilinear_filters[term_idx].avg_group_ids,
                    evaluated.bilinear_filters[term_idx].avg_num_groups);
    }
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
    phase.Next("assembly.var_indexer");
    VarIndexer var_indexer = VarIndexer::Build(solver_input);

    out_var_indexer = std::move(var_indexer);
    return solver_input;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 3b, formulation -- FormulateModel
//===--------------------------------------------------------------------===//

void PhysicalDecide::FormulateModel(SolverInput &solver_input, const FormulationBox &box,
                                    VarIndexer &var_indexer,
                                    const EvaluatedClauses &evaluated) const {
    DecideProfileScope phase("formulation.l0_constants");
    idx_t num_rows = solver_input.num_rows;

    // The flat column space is settled by assembly and is never rebuilt here: the row,
    // entity and scalar blocks are final, and only the global block grows as auxiliaries
    // are appended below. That is what lets a second formulation, against a different
    // box, name exactly the same columns as the first — which the elastic path relies on
    // to read its solution back through the same labels.
#ifdef DEBUG
    const idx_t formulation_entry_columns = var_indexer.global_block_start;
#endif

    // Plain declared names, for the refusals the linearization can raise. `alias`
    // lives on BaseExpression, so no cast is needed to read it.
    vector<string> decide_var_names;
    decide_var_names.reserve(solver_input.num_decide_vars);
    for (auto &v : decide_variables) {
        decide_var_names.push_back(v->alias);
    }
    // A frame read is named as its base is: the bound a refusal asks for is the base's.
    for (auto &read : solver_input.frame_reads) {
        decide_var_names.push_back(decide_var_names[read.base]);
    }

    // Auto-M for L0 `norm(e, 0)`: the binder emitted the raw links `e <= M*z` and
    // `-e <= M*z` with a placeholder coefficient on each `__l0auto_ind_*` indicator.
    // Now that implied bounds are known, fill a tight, data-driven Big-M (mirrors
    // the <> path): zero the indicator's own coefficient, compute M from the
    // remaining (inner-expression) terms + RHS, then set the indicator coefficient
    // to -M. The links carry a negative indicator coefficient, so the implied-bound
    // propagation assembly ran already skipped them (no contamination from the
    // placeholder). This is a constant derived from `box`, which is why it is here and
    // not with the propagation: it has to be re-derived whenever the box changes.
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
                    } else if (!StringUtil::StartsWith(decide_variables[solver_input.BaseVariable(v)]->GetName(),
                                                       "__abs_aux_")) {
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
                double M = DecideTightPerRowBigM(
                    ec, box, num_rows, decide_var_names);
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

	// A bilinear auxiliary may itself sit inside ABS (notably the reverse link of an
	// L0 norm). Temporarily expose its exact product box while descendant constructs
	// derive their own ranges. Restore the declared column box before McCormick emission
	// so ordinary solve models keep their existing, row-enforced auxiliary bounds.
	vector<pair<idx_t, pair<double, double>>> bilinear_declared_boxes;
	bilinear_declared_boxes.reserve(solver_input.bilinear_links.size());
	for (const auto &link : solver_input.bilinear_links) {
		bilinear_declared_boxes.push_back(
		    {link.aux_idx, {solver_input.lower_bounds[link.aux_idx],
		                    solver_input.upper_bounds[link.aux_idx]}});
	}
	DeriveBilinearAuxiliaryBounds(solver_input, box, decide_var_names);

    // ABS FIRST, and the order is load-bearing. Deriving an ABS auxiliary's range is
    // also what boxes its column, and every linearizer below computes its Big-M from
    // column boxes — run them first and an outer MIN/MAX or `<>` over ABS(...) sees an
    // unbounded column and refuses a query whose bound was there to be computed. Only
    // the LOWERING path refuses an underivable range; the native path leaves the
    // auxiliary open and answers.
    phase.Next("formulation.abs_bounds");
    if (solver_input.reducer_value_links.empty()) {
        DeriveAbsAuxiliaryBounds(solver_input, decide_var_names, !native_abs);
    } else {
        // The value rewrite's helper decisions are boxed by their members' reach, and a
        // member may be an ABS auxiliary while an ABS may hold a helper: each box is the
        // other's input. A helper's box also bounds the helpers it is defined from
        // (DeriveReducerMemberBounds). Every derivation only tightens, so alternating
        // them settles in as many passes as the constructs nest. The lowering arm's
        // refusal is decided last, on the settled boxes, naming a helper by its reducer.
        for (auto &link : solver_input.reducer_value_links) {
            if (link.value_var < decide_var_names.size() && !link.label.empty()) {
                decide_var_names[link.value_var] = link.label;
            }
        }
        const idx_t max_passes =
            2 * (solver_input.abs_maximize_links.size() + solver_input.reducer_value_links.size() + 1);
        for (idx_t pass = 0;; pass++) {
            auto lower = solver_input.lower_bounds;
            auto upper = solver_input.upper_bounds;
            DeriveAbsAuxiliaryBounds(solver_input, decide_var_names, /*refuse_when_unbounded=*/false);
            DeriveReducerValueBounds(solver_input);
            DeriveReducerMemberBounds(solver_input, var_indexer);
            if (lower == solver_input.lower_bounds && upper == solver_input.upper_bounds) {
                break;
            }
            // Clauses that contradict each other make a box cross, and from there each pass
            // pushes its ends further apart, forever. A crossed box is the proof that the
            // model is infeasible: stop at the first one, whose ends are still finite, and
            // the solver reports it as for any empty box (DIAGNOSE formulates its own).
            bool crossed = false;
            for (idx_t var = 0; var < solver_input.lower_bounds.size() && !crossed; var++) {
                const double lo = solver_input.lower_bounds[var];
                const double hi = solver_input.upper_bounds[var];
                crossed = lo > hi + 1e-9 * MaxValue<double>(1.0, std::abs(hi));
            }
            if (crossed) {
                break;
            }
            // Each pass leaves valid boxes, only tighter ones: one that still moves after as
            // many passes as the constructs nest is closing in by ever smaller steps (an
            // AVG over a group whose members repeat a helper), so the boxes stand as they are.
            if (pass == max_passes) {
                break;
            }
        }
        DeriveAbsAuxiliaryBounds(solver_input, decide_var_names, !native_abs);
    }
    if (native_abs) {
        EmitNativeAbs(solver_input, var_indexer);
    } else {
        LinearizeAbsMaximize(solver_input);
    }

    // The helper decisions' definitions: an equality row per group for SUM/AVG, an
    // extremum link per group for MIN/MAX. A refusal names a helper by its reducer.
    phase.Next("formulation.value_links");
    LinearizeReducerValueLinks(solver_input, var_indexer, decide_var_names, native_min_max);

    // Encode every hard MIN/MAX constraint stage 05 tagged. Native states `z = MAX(t..)`
    // directly and needs no Big-M, so it also needs no bound on the contributing
    // variables; the lowering arm's indicator family does. Both read the same tag and
    // make the same bound classification first.
    if (!minmax_clause_labels.empty()) {
        LinearizeMinMaxConstraints(solver_input, var_indexer, decide_var_names, native_min_max);
    }

    // Encode `<>` as the disjunction it is: two conditional rows per instance. Whether a
    // condition is stated to the backend or encoded with a Big-M is settled once, by
    // LowerDecideConstructs at the end of this function.
    phase.Next("formulation.not_equal");
    if (!ne_clause_labels.empty()) {
        LinearizeNotEqual(solver_input, var_indexer, decide_var_names);
    }

    // Emit the McCormick envelope for every bilinear w = b * x auxiliary.
	for (const auto &saved : bilinear_declared_boxes) {
		solver_input.lower_bounds[saved.first] = saved.second.first;
		solver_input.upper_bounds[saved.first] = saved.second.second;
	}
    phase.Next("formulation.bilinear");
    LinearizeBilinear(solver_input, box, decide_var_names);

    // Handle MIN/MAX objective: create global auxiliary variable z and linking constraints.
    // Easy (no indicators): MINIMIZE+MAX or MAXIMIZE+MIN; hard (Big-M indicators):
    // MINIMIZE+MIN or MAXIMIZE+MAX. A nested one is a helper decision, defined with the
    // other helpers (D75).

    // Global variables are appended at var_indexer.global_block_start.
    // As we add more global vars, their indices are global_block_start + g
    // where g is the position in the global vars array.

    // Encode a MIN/MAX objective (`MIN(expr)`/`MAX(expr)`) into a global auxiliary and its
    // envelope/indicator rows. Pure stage-06 work: it reads the coefficients PHASE 2
    // evaluated plus the flat column space, and needs no row of its own.
    phase.Next("formulation.minmax_objective");
    MinMaxObjectiveSpec minmax_objective_spec;
    minmax_objective_spec.flat_agg = flat_objective_agg;
    minmax_objective_spec.flat_is_easy = flat_objective_is_easy;
    {
        // The rows the objective's reducer reads -- its terms' filters, the objective's
        // `when` folded in -- are its members, whatever their coefficients.
        auto &members = minmax_objective_spec.member_rows;
        members.assign(num_rows, evaluated.linear_filters.empty());
        for (auto &filter : evaluated.linear_filters) {
            for (idx_t row = 0; row < num_rows; row++) {
                members[row] = members[row] || !filter.has_filter || filter.mask[row];
            }
        }
    }
    LinearizeMinMaxObjective(solver_input, var_indexer, minmax_objective_spec, decide_var_names,
                             native_min_max);

    // Composed MIN/MAX clauses. PHASE 2 evaluated each one; stage 06 owns the auxiliary,
    // envelope and indicator emission. The linearizer fills each MIN/MAX term's `z_idx`,
    // so it is handed a mutable copy — the evaluated clauses stay reusable for a second
    // formulation against a different box.
    phase.Next("formulation.composed_minmax");
    for (idx_t i = 0; i < evaluated.composed_constraints.size() &&
                      i < composed_minmax_constraints.size();
         i++) {
        auto &spec = composed_minmax_constraints[i];
        auto terms = evaluated.composed_constraints[i].terms;
        auto &clause = evaluated.composed_constraints[i];
        LinearizeComposedMinMaxConstraint(solver_input, var_indexer, terms, clause.rhs, spec.outer_cmp,
                                          spec.source_clause_id, spec.removal_group_id, clause.repair_group_id,
                                          clause.class_set, decide_var_names, native_min_max);
    }
    if (!evaluated.composed_objective_terms.empty()) {
        auto terms = evaluated.composed_objective_terms;
        LinearizeComposedMinMaxObjective(solver_input, var_indexer, terms, decide_var_names,
                                         native_min_max);
    }

    // Refresh total_vars: the row/entity blocks were finalized before linearization,
    // but global aux vars were appended by the construct sites above.
    var_indexer.total_vars = var_indexer.global_block_start + solver_input.num_global_vars;

    // THE LOWERING, and the last pass of stage 06. Every site above emitted the semantic
    // form of its construct and nothing else; this is where the constructs the chosen
    // backend cannot state become ordinary rows. It runs last because it needs every
    // column box final — a Big-M reads them, and the sites above are what narrow them.
    //
    // What it reads is the answer stage 05 recorded on the plan, carried here as a value.
    // No backend is consulted at execution time, and nothing here re-decides a
    // formulation: a pass that answered differently from the rewrites would run a model
    // on a solver it was not built for.
    phase.Next("formulation.lower_constructs");
    LowerDecideConstructs(solver_input, var_indexer, box, decide_var_names, use_native_constructs);

#ifdef DEBUG
    // No decide variable was added: only the global block grew.
    D_ASSERT(var_indexer.global_block_start == formulation_entry_columns);
#endif
}


//===--------------------------------------------------------------------===//
// The elastic column box
//===--------------------------------------------------------------------===//

//! Widen every column direction a diagnosis is allowed to loosen, so each is enforced
//! only by the loosenable row emitted beside it rather than by a bound that cannot move.
//!
//! The solved box carries three things (see `SolverInput::rigid_lower_bounds`): the
//! intrinsic domain, user bounds stage 05 absorbed, and implied tightenings
//! `DecidePropagateImpliedBounds` derived. The last two are each backed by a row the
//! elastic engine may loosen, so leaving them in place would pin the loosenable row behind
//! a bound that cannot move — the engine would report an edit that changes nothing.
//! `rigid_*` is exactly the intrinsic domain, which is what makes the test "is this
//! direction narrower than rigid?" the same as "is a loosenable row holding it?":
//! propagation only ever narrows, and stage 05's absorption is re-opened in the snapshot.
//!
//! The widening is bounded, and it is bounded RELATIVE TO EACH COLUMN. Opening a direction
//! all the way is not an option: an unbounded column has no finite Big-M, so a formulation
//! against a fully open box does not lose precision, it refuses the query ("Bilinear term
//! requires a finite upper bound on variable 'x'"). But the amount to open by has to come
//! from the column itself and never from the model as a whole. One model-wide number
//! couples every column to the largest value anywhere in the query — a price-weighted row
//! drags an unrelated `x <= 5` out to 1e15, the Big-M derived from it follows, and the
//! elastic model stops being solvable for reasons that have nothing to do with `x`.
//! Scaling each column from its own bounds and row demands prevents an unrelated large
//! value elsewhere in the model from inflating its constants. A row may still require a
//! much larger widening than the column's current magnitude; that coupling is local and
//! semantic rather than model-wide.
static void OpenElasticColumnBox(SolverInput &input) {
    //! Headroom every loosenable direction gets, as a multiple of the column's own
    //! magnitude. It is the floor, not the whole answer: it covers a column whose repair
    //! target is not visible in any row — the bound behind an `ABS(x) >= 5`, where the
    //! rows are the construct's own links and carry no target of their own.
    constexpr double ELASTIC_WIDENING = 10.0;
    //! A coefficient at or below the solver's own small-value threshold is noise, and
    //! dividing by it would manufacture a demand of 1e18 from a rounding artefact.
    constexpr double NEGLIGIBLE_COEFFICIENT = 1e-9;

    // What each column would have to reach to satisfy a row on its own. `SUM(y) >= 100`
    // says a repair may need `y` out at 100, however small `y <= 2` looks beside it — and
    // that is the difference between reporting one edit the user can apply and reporting
    // two because the first could not go far enough. Divided by the coefficient, so a row
    // measured in price-weighted units states its demand in the column's units and not in
    // its own.
    vector<double> row_demand(input.num_decide_vars, 0.0);
    for (const auto &ec : input.constraints) {
        for (idx_t r = 0; r < input.num_rows; r++) {
            if (!ec.row_group_ids.empty() && ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                continue;
            }
            double rhs = ec.rhs_values.IsUniform() ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(r);
            if (!std::isfinite(rhs) || rhs == 0.0) {
                continue;
            }
            for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
                idx_t v = ec.variable_indices[t];
                if (v == DConstants::INVALID_INDEX || v >= row_demand.size()) {
                    continue;
                }
                double coef = std::abs(ec.row_coefficients[t].Get(r));
                if (coef <= NEGLIGIBLE_COEFFICIENT) {
                    continue;
                }
                double demand = std::abs(rhs) / coef;
                if (std::isfinite(demand)) {
                    row_demand[v] = std::max(row_demand[v], demand);
                }
            }
        }
    }

    for (idx_t var = 0; var < input.num_decide_vars; var++) {
        if (var >= input.rigid_lower_bounds.size() || var >= input.lower_bounds.size()) {
            continue;
        }
        // A BOOLEAN's 0/1 domain is intrinsic and never widens: a "repair" that let an
        // indicator sit at 1.4 would be a repair of the encoding, not of the query. A pin
        // the user wrote on one is still loosenable — that direction is narrower than
        // rigid, so the clamp lands it back on the domain edge, exactly where the
        // re-emitted row takes over as its enforcer.
        bool is_bool = var < input.variable_types.size() &&
                       input.variable_types[var] == LogicalType::BOOLEAN;
        double rigid_lower = is_bool ? std::max(input.rigid_lower_bounds[var], 0.0)
                                     : input.rigid_lower_bounds[var];
        double rigid_upper = is_bool ? std::min(input.rigid_upper_bounds[var], 1.0)
                                     : input.rigid_upper_bounds[var];
        // The column's own magnitude, from whichever of its bounds are finite. Floored at
        // one so a column pinned at zero still has somewhere to move.
        double magnitude = 1.0;
        if (std::abs(input.lower_bounds[var]) < 1e20) {
            magnitude = std::max(magnitude, std::abs(input.lower_bounds[var]));
        }
        if (std::abs(input.upper_bounds[var]) < 1e20) {
            magnitude = std::max(magnitude, std::abs(input.upper_bounds[var]));
        }
        double room = std::max(ELASTIC_WIDENING * magnitude, row_demand[var]);
        if (input.lower_bounds[var] > rigid_lower) {
            input.lower_bounds[var] = std::max(rigid_lower, input.lower_bounds[var] - room);
        }
        if (input.upper_bounds[var] < rigid_upper) {
            input.upper_bounds[var] = std::min(rigid_upper, input.upper_bounds[var] + room);
        }
    }
    SyncFrameReadBoxes(input);
}

//===--------------------------------------------------------------------===//
// The elastic model, as a formulation
//===--------------------------------------------------------------------===//

//! Formulate a second model against the widened box.
//!
//! A Big-M or a McCormick envelope is sound only for the box it was derived from, so an
//! elastic model that re-opens the box must re-derive them. That is why this runs the
//! same PHASE 3b pass the solved model went through — `FormulateModel` is a pure
//! function of the evaluated clauses plus a box, which is what makes running it twice
//! meaningful rather than merely possible.
//!
//! Each call starts from a fresh copy of `elastic`, so no rows or auxiliaries from the
//! solved formulation can leak into the repair formulation.
SolverModel PhysicalDecide::FormulateElasticModel(const ElasticFormulation &elastic,
                                                  const EvaluatedClauses &evaluated,
                                                  bool tolerate_infeasible_bounds,
                                                  VarIndexer &out_var_indexer,
                                                  vector<string> &out_global_labels) const {
    SolverInput input = elastic.input;
    VarIndexer indexer = elastic.var_indexer;
    input.tolerate_infeasible_bounds = tolerate_infeasible_bounds;

    OpenElasticColumnBox(input);
    FormulateModel(input, FormulationBox::OfSolvedModel(input), indexer, evaluated);

    // Same reconciliation the solved model gets before its build: the `<>` label channel
    // is a prefix of the global block, padded to its final length.
    D_ASSERT(input.global_variable_labels.size() <= input.num_global_vars);
    input.global_variable_labels.resize(input.num_global_vars);
    out_global_labels = input.global_variable_labels;

    // The decide-variable columns are settled by assembly and shared with the solved
    // model, so a repair reads back through the same labels. Only the global block may
    // differ, and only in length.
    D_ASSERT(indexer.global_block_start == elastic.var_indexer.global_block_start);

    SolverModel model = SolverModel::Build(input, indexer);
    out_var_indexer = std::move(indexer);
    return model;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 3, solve + readback -- FinalizeSolveResult
//===--------------------------------------------------------------------===//

SinkFinalizeType PhysicalDecide::FinalizeSolveResult(ClientContext &context, DecideGlobalSinkState &gstate,
                                                     SolverInput &solver_input, VarIndexer &var_indexer,
                                                     const EvaluatedClauses &evaluated,
                                                     ElasticFormulation *elastic, bool bench,
                                                     Profiler &model_timer,
                                                     Profiler &solver_timer) const {
    idx_t num_rows = solver_input.num_rows;

    // Capture model size before solve (solver may move data)
    size_t bench_total_vars = var_indexer.total_vars;
    // Clause-level model size: the number of constraint *specs* the operator hands the
    // builder. A per-row spec still expands to one row per data row and a grouped spec to one
    // row per group, so this is not a matrix row count — the row count is read back off
    // the built model (`solve_result.model_constraint_rows`) after the solve below.
    size_t bench_constraint_specs = solver_input.constraints.size() + solver_input.global_constraints.size();

    if (bench) {
        model_timer.End();
        solver_timer.Start();
    }

    // H3: the statement's DIAGNOSE prefix, and nothing else, arms diagnosis. Under it,
    // pre-extract the unbounded ray so it is ready if the solve turns out unbounded, and
    // retain the built model so the INFEASIBLE terminal can hand it to the elastic
    // engine. An unprefixed query pays for neither, and both failure terminals are
    // prefix-only anyway (RouteSolveResult).
    bool diagnosis_armed = diagnose;
    SolveModelOptions solve_options;
    solve_options.extract_unbounded_ray = diagnosis_armed;
    // Keep an inverted column box (col_lower > col_upper) alive through Build under
    // diagnosis so the INFEASIBLE terminal can reset it to the intrinsic domain and
    // diagnose it (Bug 1, all-column-bound conflicts). An unprefixed query keeps the
    // fast throw.
    solver_input.tolerate_infeasible_bounds = diagnosis_armed;

    // H5: slow-solve continuation is ordinary execution behaviour, not diagnosis, so it
    // runs with or without the prefix. The interactive prompt needs a human, so it falls
    // back to a plain error when stdin is not a terminal (tests, benchmarks, -c pipes,
    // C-API) — never prompt where no one can answer. That tty test IS the rule now; there
    // is no mode to set.
#ifdef _WIN32
    bool stdin_is_tty = _isatty(_fileno(stdin)) != 0;
#else
    bool stdin_is_tty = isatty(STDIN_FILENO) != 0;
#endif
    bool can_prompt = stdin_is_tty;
    // The warm solver is retained only where a continuation can actually be asked for.
    bool want_session = can_prompt;
    // The first solve chunk uses the same per-chunk budget every Continue() chunk will,
    // so elapsed climbs in even multiples (300s, 600s, ...) across the loop.
    solve_options.time_limit_seconds = ResolveDecideTimeLimit();
    // Ctrl-C cuts the solve short and returns the best-so-far instead of aborting the
    // query — also ordinary execution behaviour, so the poll is installed unconditionally.
    // Gurobi honors it mid-solve; HiGHS stays boundary-only.
    solve_options.interrupt_poll = [&context]() { return context.interrupted.load(); };
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

    // Under DIAGNOSE, a failure is the ANSWER, not an error: hand off the findings to the
    // DIAGNOSE operator above and return no rows. Without the prefix the same failure
    // raises, naming its state and nothing more — naming the clause *is* the elastic
    // solve, and that only happens when the user asks for it.
    auto report = [&](DecideDiagnostic diag) {
        D_ASSERT(diagnosis_armed);
        StashDecideDiagnostic(context, std::move(diag));
        gstate.diagnosis_only = true;
    };

    // H5: a time-limit stop (or a Ctrl-C, which arrives as one) is ordinary execution
    // behaviour and is handled here, before the router sees the result — with or without
    // the prefix. It either delivers an incumbent, resumes to a proven optimum, or
    // raises; it never diagnoses.
    bool deliver_incumbent = false;
    if (solve_result.status == SolverStatus::TIME_LIMIT) {
        // Before anything is printed: did another operator in this same query already
        // fail? A SELECT can hold several DECIDE clauses — two subqueries joined side
        // by side, say — and when one of them throws, DuckDB stops the rest by setting
        // the very query interrupt Ctrl-C sets (`Executor::PushError`). Every checkpoint
        // test below reads that interrupt, so this solve would be reported as a
        // cancellation the user never asked for, printed *above* the error that actually
        // stopped the query, with elapsed/memory figures describing a solve that was
        // never the problem. Say nothing and re-raise the real failure instead.
        auto &executor = Executor::Get(context);
        if (executor.HasError()) {
            executor.ThrowException();
        }
        // S2/S3/S4: at each chunk boundary print the checkpoint report (what was found
        // + how far it can still improve + elapsed/memory), then offer the choice: keep
        // solving, or take the incumbent with its gap to best. "Keep solving" re-runs
        // the SAME warm solver (retained_session) for another fresh chunk — the MIP
        // search resumes, elapsed accumulates. On stop, a usable incumbent is returned
        // as a SUCCESSFUL result (with a plain stderr caveat that it is not proven
        // best); no incumbent falls to the plain timeout error.
        double chunk = ResolveDecideTimeLimit();
        double cum_elapsed = solve_wall_timer.Elapsed();
        double cum_budget = chunk;

        // Did the user stop this, or the clock? `user_interrupted` is the backend's own
        // attribution (Gurobi cuts the solve short mid-search and says so); HiGHS has no
        // thread-safe terminate, so its Ctrl-C is only noticed at the chunk boundary and
        // arrives indistinguishable from a timeout. The query interrupt is set either
        // way, so read it too — a user who pressed Ctrl-C gets the same answer on both.
        bool stopped_by_user = solve_result.user_interrupted || context.interrupted.load();
        if (stopped_by_user) {
            // Ctrl-C is not a question. The user has already said stop, so there is
            // nothing to offer: print what was found and hand back the best-so-far,
            // with the caveat that it is not proven best. This is the one stop that
            // does not need a terminal — the decision was made at the keyboard, not in
            // an answer to a prompt.
            PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget);
            if (!solve_result.has_solution) {
                ThrowDecideSolveError(solve_result);
            }
            fprintf(stderr,
                    "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n");
            context.interrupted = false;
            deliver_incumbent = true;
        } else if (!can_prompt) {
            // The clock ran out and nobody is at the keyboard (tests, benchmarks, `-c`
            // pipes, the C API), so there is no one to answer the offer. Print the
            // checkpoint once and raise: never prompt where no one can answer.
            PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget);
            ThrowDecideSolveError(solve_result);
        } else {
            // The interrupt poll installed on the first solve persists on the retained
            // session into every Continue() chunk, but the prompt below already stops the
            // user at each boundary, and a watcher thread contending with the interactive
            // getline destabilizes the prompt (found empirically) — so reset it to
            // boundary-only here. The entry interrupt already fired *before* this loop, so a
            // first-solve Ctrl-C still routed us in. HiGHS ignores the poll either way.
            if (retained_session) {
                retained_session->SetInterruptPoll({});
            }

            // Report at each boundary, then continue or stop.
            for (;;) {
                PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget);

                // stdin is a terminal (can_prompt guaranteed this), so it is safe to block
                // reading the user's decision. The CLI's line editor is inactive
                // mid-execution, so a plain getline does not fight it.
                if (solve_result.has_solution) {
                    fprintf(stderr, "Keep improving it?  [Enter] continue +%s  ·  s + Enter to stop and take this solution: ",
                            FormatDuration(chunk).c_str());
                } else {
                    fprintf(stderr, "Keep searching?  [Enter] continue +%s  ·  s + Enter to give up: ",
                            FormatDuration(chunk).c_str());
                }
                fflush(stderr);
                bool go;
                string line;
                if (!std::getline(std::cin, line)) {
                    go = false; // EOF (Ctrl-D / closed input) → stop
                } else {
                    StringUtil::Trim(line);
                    go = StringUtil::Lower(line) != "s";
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
                    // as the plain solver error.
                    ThrowDecideSolveError(solve_result);
                }
                // else: another time-limit stop — loop and re-report with fresh numbers.
            }

            // Loop exited on a stop decision or an OPTIMAL break.
            if (solve_result.status == SolverStatus::OPTIMAL || solve_result.has_solution) {
                if (solve_result.status != SolverStatus::OPTIMAL) {
                    // Stopped early with a usable-but-unproven incumbent: succeed and
                    // return it, but say plainly it is not proven best. There is no live
                    // SQL NOTICE channel, so this rides on stderr with the checkpoint
                    // report, which already printed the gap to best.
                    fprintf(stderr,
                            "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n");
                    deliver_incumbent = true;
                }
                // If a Ctrl-C broke the loop, the interrupt is now handled (we stopped
                // solving and have a result to return); clear it so the rows flow to the
                // client instead of the executor aborting the query. Covers both the
                // incumbent stop and the rare "Ctrl-C landed as the optimum was proven".
                context.interrupted = false;
            } else {
                // Stopped with nothing found yet: the plain timeout error, same as the
                // non-interactive exit above.
                ThrowDecideSolveError(solve_result);
            }
        }
    }

    // Route what is left to its diagnosis terminal. RouteSolveResult is a pure
    // classifier (status + armed → terminal); the operator owns the engine call, the
    // statement-scoped handoff, and the throw for each terminal.
    switch (deliver_incumbent ? DiagnosisTerminal::SOLVED
                              : RouteSolveResult(solve_result, diagnosis_armed)) {
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
                                            input_column_names, input_column_in_clause, var_indexer,
                                            entity_scopes, num_rows);

        UnboundedDiagnosisInput diag_input {
            solve_result, var_indexer, var_labels, var_is_aux, diag_params, get_candidates,
        };
        DecideDiagnostic diag = DiagnoseUnbounded(diag_input);
        if (diag.valid && !diag.findings.empty()) {
            report(std::move(diag));
            break;
        }
        // The engine ran but named no variable. Say why rather than reporting nothing:
        // an empty ray is a non-linear limitation only when the retained model is
        // actually non-linear; otherwise it is a neutral "could not identify". A present
        // ray that named nothing means only internal auxiliaries escaped.
        bool has_nonlinear_terms = retained_model.has_quadratic_obj ||
                                   !retained_model.quadratic_constraints.empty() ||
                                   !retained_model.general_constraints.empty() ||
                                   !retained_model.indicator_constraints.empty();
        string reason = BuildUnboundedDiagnosisUnavailableReason(
            solve_result.diagnostic_timed_out, solve_result.ray.empty(), has_nonlinear_terms);
        report(BuildUndiagnosedDiagnostic(
            solve_result.status == SolverStatus::INF_OR_UNBD ? "infeasible or unbounded" : "unbounded",
            reason));
        break;
    }
    case DiagnosisTerminal::INFEASIBLE: {
        // A residual INF_OR_UNBD is routed here only with an empty ray; normalize it
        // so the message (and any diagnosis) reads as infeasible.
        SolverResult terminal_result = solve_result;
        if (terminal_result.status == SolverStatus::INF_OR_UNBD) {
            terminal_result.status = SolverStatus::INFEASIBLE;
        }
        // No model was retained: SolveModel returned INFEASIBLE from a Build that threw
        // before it finished (conflicting column bounds on an unprefixed statement), so
        // `retained_model` is still default-constructed — no columns, no rows. Every
        // step below indexes it by column, so bail to the static error instead. Diagnosis
        // has nothing to work from; saying so plainly beats walking an empty model.
        if (retained_model.num_vars == 0) {
            report(BuildUndiagnosedDiagnostic(
                "infeasible", "the model could not be built, so there is nothing to diagnose."));
            break;
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

        // Nothing was retained to formulate the repair model from. `Finalize` retains it
        // whenever `diagnose` is set and this terminal is prefix-only, so reaching here
        // means the two disagreed — a contract violation, not a user situation. Report
        // rather than walk a null: a diagnosis that cannot run is still an answer.
        if (!elastic) {
            D_ASSERT(false);
            report(BuildUndiagnosedDiagnostic(
                "infeasible", "the repair model is unavailable, so there is nothing to diagnose."));
            break;
        }

        bool diagnostic_solve_timed_out = terminal_result.diagnostic_timed_out;
        bool diagnostic_solver_failed = false;
        auto solve_diagnostic_model = [backend, diagnostic_solve_options,
                                       &diagnostic_solve_timed_out,
                                       &diagnostic_solver_failed](const SolverModel &m) {
            SolverResult result;
            try {
                result = SolvePreparedModel(m, backend, diagnostic_solve_options);
            } catch (const Exception &) {
                // The widened repair program is harder than the query's own. Treat a
                // backend load/solve failure as an unavailable diagnosis, but keep this
                // guard at the callback boundary: exceptions raised by our diagnosis
                // logic must still surface as bugs instead of being mislabeled as a
                // solver limitation.
                diagnostic_solver_failed = true;
                result.status = SolverStatus::OTHER;
                return result;
            }
            if (result.status == SolverStatus::TIME_LIMIT) {
                diagnostic_solve_timed_out = true;
            }
            return result;
        };

        // The repair model: formulated against the widened box rather than patched onto
        // the solved model's matrix, so its Big-M and McCormick constants describe the box
        // the repair actually searches instead of the narrower one the query solved in.
        DecideDiagnostic diag;
        SolverModel elastic_model;
        VarIndexer elastic_indexer;
        vector<string> elastic_global_labels;
        try {
            elastic_model = FormulateElasticModel(*elastic, evaluated,
                                                  /*tolerate_infeasible_bounds=*/true,
                                                  elastic_indexer, elastic_global_labels);
        } catch (const InvalidInputException &ex) {
            // The only thing the formulation raises by design: a construct the widened box
            // cannot express. Report it rather than turning the user's DIAGNOSE into an
            // error — or, worse, falling back to constants derived against the narrow box.
            // Deliberately NOT a catch-all: an InternalException is a bug in this pass and
            // has to surface as one.
            report(BuildUndiagnosedDiagnostic(
                "infeasible",
                StringUtil::Format("the repair model could not be built: %s", ex.what())));
            break;
        }
        InfeasibleDiagnosisInput diag_input {
            elastic_model, elastic_indexer, var_labels, var_is_aux, elastic_global_labels, diag_params,
            solve_diagnostic_model,
        };
        diag = DiagnoseInfeasible(diag_input);
        if (diagnostic_solver_failed) {
            // A backend that cannot load or solve the repair model. That is a limit on
            // what can be explained, not a failure of the query itself.
            report(BuildUndiagnosedDiagnostic(
                "infeasible",
                "the solver could not analyse the repair model for this query, so no "
                "least-change repair could be computed."));
            break;
        }
        if (diag.valid && !diag.findings.empty()) {
            report(std::move(diag));
            break;
        }
        // The elastic engine can decline to report when no actionable relaxation exists.
        // Say which of the two it was rather than returning an empty relation.
        report(BuildUndiagnosedDiagnostic(
            "infeasible",
            diagnostic_solve_timed_out
                ? "diagnosis ran out of time before it could find a least-change repair."
                : "no loosening of the clauses you wrote restores feasibility."));
        break;
    }
    case DiagnosisTerminal::UNDIAGNOSED:
        // No DIAGNOSE prefix, or a status no engine covers (ITERATION_LIMIT, OTHER).
        // Either way the query reports its status and stops — the second solve that
        // would name a clause only happens when the user asks for it by name.
        ThrowDecideSolveError(solve_result);
    }

    // Under DIAGNOSE a failure terminal handed off its findings instead of raising: there
    // is no solution to read back, so produce no rows and let the DIAGNOSE operator
    // above turn the handoff into the answer.
    if (gstate.diagnosis_only) {
        return SinkFinalizeType::READY;
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

    // The solve succeeded, so there is nothing wrong to report. Clear the handoff so a
    // DIAGNOSE above this operator sees "no findings" and answers `feasible`.
    ClearDecideDiagnostic(context);
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
    DecideProfileTotals profile_readback {"execution.output_readback"};
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

//! A solved value on its way into an integer output column.
//!
//! The solver works in doubles, so what comes back is not bounded by the column's type.
//! A declared bound is not the limit either: `SUM(x) <= 5000000000` bounds no single
//! decision, so nothing earlier in the pipeline can promise the value fits. Casting an
//! out-of-range double to a narrower integer is undefined in C++ and saturates on the
//! platforms we build, which would present the type's limit as the answer — a number
//! that violates the query's own constraints, reported with no error and no diagnostic.
//! Refuse it by name instead.
//!
//! `INT` decisions are BIGINT (see the type mapping in BindSelectNode), so the reachable
//! limit here is a double's exact-integer range rather than the column's: past 2^53 a
//! double no longer counts consecutively, and no wider integer type recovers a value the
//! solver never distinguished. That makes the refusal honest rather than a missing
//! widening. `BOOL` decisions and boolean auxiliaries stay INTEGER and cannot reach it.
static int64_t DecideSolutionToInteger(double solution_value, const LogicalType &var_type,
                                       const string &var_name) {
    double rounded = std::round(solution_value);
    // Both ends spelled out rather than a symmetric magnitude: int32's range is not
    // symmetric, and `-2147483648` is a value the column can hold.
    double lo = var_type == LogicalType::INTEGER
                    ? static_cast<double>(NumericLimits<int32_t>::Minimum())
                    : -9007199254740992.0; // -2^53
    double hi = var_type == LogicalType::INTEGER
                    ? static_cast<double>(NumericLimits<int32_t>::Maximum())
                    : 9007199254740992.0; // 2^53
    // Negated so a NaN — which compares false against everything — is refused rather
    // than passed through to the cast this exists to protect.
    if (!(rounded >= lo && rounded <= hi)) {
        throw InvalidInputException(
            "DECIDE variable '%s' solves to %s, which is beyond the range DeciDB can "
            "return as a whole number. Tighten the bound on '%s', or declare it "
            "'%s(REAL)' to read the value approximately.",
            var_name, StringUtil::Format("%.0f", rounded), var_name, var_name);
    }
    return static_cast<int64_t>(rounded);
}

unique_ptr<GlobalSourceState> PhysicalDecide::GetGlobalSourceState(ClientContext &context) const {
    auto &sink = sink_state->Cast<DecideGlobalSinkState>();
    return make_uniq_base<GlobalSourceState, DecideGlobalSourceState>(*this, sink);
}

SourceResultType PhysicalDecide::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
    auto &gstate = sink_state->Cast<DecideGlobalSinkState>();
    auto &source_state = input.global_state.Cast<DecideGlobalSourceState>();
    DecideProfileSample profile(source_state.profile_readback);

    // The solve failed under DIAGNOSE: there is no assignment to read back, so this
    // operator produces nothing and the diagnosis it handed off is the statement's answer.
    if (gstate.diagnosis_only) {
        return SourceResultType::FINISHED;
    }

    // Scan the original buffered data into a chunk sized to match the collection;
    // `chunk` is wider (it has the appended DECIDE variable columns), so scanning
    // directly into it would violate ColumnDataCollectionSegment::ReadChunk's
    // column-count assertion.
    gstate.data.Scan(source_state.scan_state, source_state.scan_chunk);
    if (source_state.scan_chunk.size() == 0) {
        return SourceResultType::FINISHED;
    }

    // Reference the input's columns into the leading columns of the wide output chunk. The
    // collection may hold more after them -- the values frame selectors read
    // (BuildFrames) -- which are no output.
    const idx_t input_columns = types.size() - decide_variables.size();
    for (idx_t col = 0; col < input_columns; col++) {
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

        // An auxiliary's column only carries it to the projection that drops it: nobody
        // reads its value, so none is converted -- nor refused, which would name an
        // internal column for a value the user never asked for.
        if (decide_var_idx >= total_decide_vars - num_auxiliary_vars) {
            output_vector.SetVectorType(VectorType::CONSTANT_VECTOR);
            ConstantVector::SetNull(output_vector, true);
            continue;
        }

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
                    output_data[row_in_chunk] = static_cast<int32_t>(
                        DecideSolutionToInteger(solution_value, var_type, decide_var.GetName()));
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
                    output_data[row_in_chunk] =
                        DecideSolutionToInteger(solution_value, var_type, decide_var.GetName());
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
