#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <numeric>
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
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
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

//! Transform a coefficient/value expression for ExpressionExecutor.
//! Handles: BOUND_COLUMN_REF, BOUND_FUNCTION, BOUND_CAST, BOUND_COMPARISON,
//! BOUND_CONJUNCTION, BOUND_OPERATOR. Falls back to Copy() for others.
static unique_ptr<Expression> TransformToChunkExpression(const Expression &expr, ClientContext &context) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		vector<unique_ptr<Expression>> new_children;
		for (auto &child : func.children) {
			new_children.push_back(TransformToChunkExpression(*child, context));
		}
		unique_ptr<FunctionData> new_bind_info;
		if (func.bind_info) {
			new_bind_info = func.bind_info->Copy();
		}
		return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
		                                                           std::move(new_children), std::move(new_bind_info));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		auto transformed_child = TransformToChunkExpression(*cast.child, context);
		return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// The operator evaluates row values only. A reducer of data is a column by now (the optimizer's bound-side
		// window rewrite); one of a decision is a set of terms, never evaluated.
		throw InternalException("DECIDE: a reducer reached row evaluation: %s", expr.ToString());
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		auto left = TransformToChunkExpression(*comp.left, context);
		auto right = TransformToChunkExpression(*comp.right, context);
		return make_uniq_base<Expression, BoundComparisonExpression>(comp.type, std::move(left), std::move(right));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
		for (auto &child : conj.children) {
			result->children.push_back(TransformToChunkExpression(*child, context));
		}
		return std::move(result);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		auto result = make_uniq<BoundOperatorExpression>(op_expr.type, op_expr.return_type);
		for (auto &child : op_expr.children) {
			result->children.push_back(TransformToChunkExpression(*child, context));
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

//! Runs each expression of `executor` over `chunk`, whose first row is row `row_offset` of
//! the data, into `results`: expression i only on the rows `*reads[i]` marks (every row
//! when null), NULL on the others. A scoped clause filters before it generates
//! (language/guide.md §6.1), so nothing is computed on a row its `when` drops:
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
static void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset,
                              const vector<bool> *rows, DataChunk &results) {
	if (!rows) {
		executor.Execute(chunk, results);
		return;
	}
	ExecuteOnReadRows(executor, chunk, row_offset, vector<const vector<bool> *>(results.ColumnCount(), rows), results);
}

//! What a coefficient DeciDB computed is computed from (DecideTerm::inputs), and the chunk the coefficient was computed
//! over, whose failing row reads them.
struct ValueInputs {
	const vector<unique_ptr<Expression>> &values;
	DataChunk &chunk;
	ClientContext &context;
};

//! A number read from the source, and what the message that refuses it calls it: `value` is the expression the number
//! comes from, printed as the user wrote it (DecideInvalidValueMessage). `clause` is the clause (or the objective) it
//! belongs to, which a coefficient past a DOUBLE that no column's value made so names (TOO_LARGE), as AddCoefficient
//! does. `inputs` are what a coefficient DeciDB computed is computed from; none for one as written.
struct ValueSource {
	DecideValueRole role;
	const Expression &value;
	const DecideSourceNames &names;
	string clause = string();
	const ValueInputs *inputs = nullptr;
};

//! Whether `input` is a finite number or NULL on row `row` of the chunk: the one row a failing coefficient reads it on.
static bool IsFiniteOnRow(const ValueInputs &inputs, const Expression &input, idx_t row) {
	auto transformed = TransformToChunkExpression(input, inputs.context);
	SelectionVector one_row(1);
	one_row.set_index(0, row);
	DataChunk slice;
	slice.InitializeEmpty(inputs.chunk.GetTypes());
	slice.Slice(inputs.chunk, one_row, 1);
	ExpressionExecutor executor(inputs.context, *transformed);
	Vector result(transformed->return_type);
	executor.ExecuteExpression(slice, result);
	auto value = result.GetValue(0);
	return value.IsNull() || std::isfinite(value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
}

//! Refuses a number the model cannot read (NaN, or infinite where a bound is not allowed to be), naming `source`. A
//! coefficient names the first value it is computed from that a column holds and that is not finite on `row`, the row
//! it fails on; with none, DeciDB's own arithmetic (or a literal) left the DOUBLE range, and it names its clause. A
//! coefficient as written is the one value it is computed from.
[[noreturn]] static void ThrowInvalidValue(const ValueSource &source, idx_t row = 0) {
	auto refuse = [&](DecideValueRole role, const Expression &value) {
		return InvalidInputException(DecideInvalidValueMessage(role, RenderDecideSource(value, source.names)));
	};
	auto too_large = InvalidInputException(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, source.clause));
	if (source.role != DecideValueRole::COEFFICIENT) {
		throw refuse(source.role, source.value);
	}
	if (!source.inputs || source.inputs->values.empty()) {
		if (source.value.IsFoldable()) {
			throw too_large;
		}
		throw refuse(DecideValueRole::COEFFICIENT, source.value);
	}
	for (auto &input : source.inputs->values) {
		if (!input->IsFoldable() && !IsFiniteOnRow(*source.inputs, *input, row)) {
			throw refuse(DecideValueRole::COEFFICIENT, *input);
		}
	}
	throw too_large;
}

//! Vectorized extraction of a chunk-result column into a vector<double>, multiplied by `sign`.
//! Throws InvalidInputException on a number the model cannot read, naming `source`. A NULL reads
//! 0 and marks its row in `null_rows`: the caller skips what it reaches (deck p. 50). With
//! no `null_rows` the value cannot be NULL (a reducer's `when` tests its data,
//! DecideCanonicalizer::SkipNullData).
//! Fast path for DOUBLE; otherwise casts via VectorOperations::DefaultCast.
//!
//! A BOUND relaxes the check to NaN-only, and is used exclusively for values
//! on the path to a model row's `rhs` — the row's own bound, and the reducer input
//! that a per-group bound is folded from. An infinite bound is not an error there:
//! it is the absence of a constraint (`<= +inf`) or a constraint nothing satisfies
//! (`>= +inf`), both of which the solver contract already expresses — the model
//! validator accepts a non-finite rhs and rejects only NaN, and the constant and
//! singleton-row-bound RHS paths have always passed infinities straight through. A
//! coefficient has no such reading, so every other caller stays strict.
static void ExtractDoubleColumn(Vector &result_vec, idx_t count, double sign, vector<double> &out,
                                const ValueSource &source, vector<bool> *null_rows = nullptr,
                                const vector<bool> *read_rows = nullptr, idx_t row_offset = 0) {
	const bool allow_infinite = source.role == DecideValueRole::BOUND;
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
			if (!null_rows) {
				throw InternalException("DECIDE: a %s is NULL on a row its reducer reads",
				                        allow_infinite ? "bound" : "coefficient");
			}
			(*null_rows)[row_offset + i] = true;
			out.push_back(0.0);
			continue;
		}
		double dv = data[idx];
		if (allow_infinite ? std::isnan(dv) : !std::isfinite(dv)) {
			ThrowInvalidValue(source, i);
		}
		out.push_back(dv * sign);
	}
}

//! Numbers the groups by key value, so no id depends on row order: columns compare in key
//! order, NULL after every value as `ORDER BY` places it. Group g, in creation order, has
//! values `rep_keys[column][g]` and is found at `group_address[g]`; row r is in the group at
//! `row_address[r]` (null: in none). `rep_keys` is rewritten in the new numbering and
//! `row_to_group` gets each row's number, INVALID_INDEX for none.
static void NumberGroupsByValue(vector<vector<Value>> &rep_keys, const vector<data_ptr_t> &group_address,
                                const vector<data_ptr_t> &row_address, vector<idx_t> &row_to_group) {
	const idx_t num_groups = group_address.size();
	auto before = [&](idx_t a, idx_t b) {
		for (auto &column : rep_keys) {
			auto &va = column[a];
			auto &vb = column[b];
			if (va.IsNull() || vb.IsNull()) {
				if (va.IsNull() != vb.IsNull()) {
					return vb.IsNull();
				}
				continue;
			}
			if (va < vb) {
				return true;
			}
			if (vb < va) {
				return false;
			}
		}
		return false;
	};
	vector<idx_t> order(num_groups);
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), before);
	vector<idx_t> rank(num_groups);
	for (idx_t position = 0; position < num_groups; position++) {
		rank[order[position]] = position;
	}
	// A row's group is found by its address among the groups' addresses, sorted.
	vector<idx_t> by_address(num_groups);
	std::iota(by_address.begin(), by_address.end(), 0);
	std::sort(by_address.begin(), by_address.end(),
	          [&](idx_t a, idx_t b) { return group_address[a] < group_address[b]; });
	for (idx_t row = 0; row < row_address.size(); row++) {
		if (!row_address[row]) {
			row_to_group[row] = DConstants::INVALID_INDEX;
			continue;
		}
		auto found = std::lower_bound(by_address.begin(), by_address.end(), row_address[row],
		                              [&](idx_t group, data_ptr_t address) { return group_address[group] < address; });
		row_to_group[row] = rank[*found];
	}
	for (auto &column : rep_keys) {
		vector<Value> sorted;
		sorted.reserve(num_groups);
		for (auto gid : order) {
			sorted.push_back(std::move(column[gid]));
		}
		column = std::move(sorted);
	}
}

//! Groups the rows by their key values as GROUP BY does, in one chunk scan: DuckDB's
//! aggregate hash table, with no aggregate, finds or creates each row's group, comparing
//! in place (NULL equals NULL). A group's key values are made Values once, for its first
//! row (size O(num_groups)).
//!
//!   key_exprs       — pre-transformed expressions for chunk evaluation, one per key column
//!   row_filter      — return true to include row in grouping (empty function = include all)
//!   null_no_group   — per key column: a NULL there puts the row in no group (a written
//!                      column, NO_GROUP); else NULL is a value (a relation's column, VALUE)
//!   out_row_to_group — sized num_rows; INVALID_INDEX for excluded rows, [0..K) otherwise
//!   out_num_groups   — K, count of distinct groups, numbered in key-value order
//!   read_rows        — the clause's kept rows: the key is computed on these only, and no
//!                      other row joins a group (null = every row)
//!   label_exprs      — per key column, the value a label quotes when it differs from the
//!                      one rows group on (a collated column): `out_rep_keys` then holds
//!                      the group's least value of it, so a label is the same in any row
//!                      order; null entries quote the grouped value
//!
//! A key with no columns (`per ()`, `by ()`) has one class: every row the filter keeps.
static void BuildGroupIds(const vector<const Expression *> &key_exprs, ClientContext &context,
                          ColumnDataCollection &data, idx_t num_rows, const std::function<bool(idx_t)> &row_filter,
                          vector<idx_t> &out_row_to_group, idx_t &out_num_groups,
                          vector<vector<Value>> *out_rep_keys = nullptr, const vector<bool> *read_rows = nullptr,
                          const vector<bool> &null_no_group = {}, const vector<const Expression *> &label_exprs = {}) {
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
	// The label columns follow the key columns in the same chunk; label_column[c] is key
	// column c's, or c itself.
	vector<idx_t> label_column(num_key_cols);
	std::iota(label_column.begin(), label_column.end(), 0);
	for (idx_t c = 0; c < label_exprs.size(); c++) {
		if (label_exprs[c]) {
			label_column[c] = key_types.size();
			key_executor.AddExpression(*label_exprs[c]);
			key_types.push_back(label_exprs[c]->return_type);
		}
	}

	// rep_keys[j][gid] — result column j's value for the representative row of group `gid`:
	// the key columns, then the label columns. Size grows with num_groups, not num_rows.
	vector<vector<Value>> rep_keys(key_types.size());
	// The table names a group by the address of its row. With radix_bits 0 every row is in
	// one partition appended with KEEP_EVERYTHING_PINNED (InitializePartitionedData), and a
	// resize only reinserts the pointers (ReinsertTuples), so the address is fixed for the
	// table's lifetime.
	vector<LogicalType> group_types(key_types.begin(), key_types.begin() + NumericCast<int64_t>(num_key_cols));
	GroupedAggregateHashTable groups(context, BufferAllocator::Get(context), group_types);
	vector<data_ptr_t> group_address;
	vector<data_ptr_t> row_address(num_rows, nullptr);

	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	DataChunk key_results;
	key_results.Initialize(context, key_types);
	DataChunk group_chunk;
	group_chunk.InitializeEmpty(group_types);
	SelectionVector grouped(STANDARD_VECTOR_SIZE);
	SelectionVector new_groups(STANDARD_VECTOR_SIZE);
	Vector addresses(LogicalType::POINTER);
	vector<UnifiedVectorFormat> key_udatas(num_key_cols);
	idx_t row_offset = 0;
	while (data.Scan(scan, chunk)) {
		idx_t count = chunk.size();
		key_results.Reset();
		ExecuteOnReadRows(key_executor, chunk, row_offset, read_rows, key_results);
		for (idx_t c = 0; c < num_key_cols; c++) {
			key_results.data[c].ToUnifiedFormat(count, key_udatas[c]);
		}
		// The rows that join a group: those read and kept, with no NULL in a NO_GROUP column.
		idx_t grouped_count = 0;
		for (idx_t i = 0; i < count; i++) {
			idx_t row = row_offset + i;
			if ((read_rows && !(*read_rows)[row]) || (row_filter && !row_filter(row))) {
				continue;
			}
			bool no_group = false;
			for (idx_t c = 0; c < null_no_group.size() && !no_group; c++) {
				no_group = null_no_group[c] && !key_udatas[c].validity.RowIsValid(key_udatas[c].sel->get_index(i));
			}
			if (!no_group) {
				grouped.set_index(grouped_count++, i);
			}
		}
		if (grouped_count > 0) {
			for (idx_t c = 0; c < num_key_cols; c++) {
				group_chunk.data[c].Slice(key_results.data[c], grouped, grouped_count);
			}
			group_chunk.SetCardinality(grouped_count);
			idx_t new_count = groups.FindOrCreateGroups(group_chunk, addresses, new_groups);
			auto address_data = FlatVector::GetData<data_ptr_t>(addresses);
			for (idx_t n = 0; n < new_count; n++) {
				auto slot = new_groups.get_index(n);
				for (idx_t j = 0; j < rep_keys.size(); j++) {
					rep_keys[j].push_back(key_results.data[j].GetValue(grouped.get_index(slot)));
				}
				group_address.push_back(address_data[slot]);
			}
			for (idx_t slot = 0; slot < grouped_count; slot++) {
				row_address[row_offset + grouped.get_index(slot)] = address_data[slot];
			}
		}
		row_offset += count;
	}

	if (row_offset != num_rows) {
		throw InternalException(
			"DECIDE BuildGroupIds: chunk scan produced %llu rows, expected %llu",
			row_offset, num_rows);
	}

	// The label columns trail the key columns: distinct groups differ in a key column, so
	// they never decide the order, and they are permuted with it.
	NumberGroupsByValue(rep_keys, group_address, row_address, out_row_to_group);
	out_num_groups = group_address.size();
	// A collated column's label is the least spelling among its group's rows, so it is the
	// same in any row order: a second scan reads the labels of the rows each group holds.
	if (key_types.size() > num_key_cols) {
		data.InitializeScan(scan);
		row_offset = 0;
		while (data.Scan(scan, chunk)) {
			key_results.Reset();
			ExecuteOnReadRows(key_executor, chunk, row_offset, read_rows, key_results);
			for (idx_t i = 0; i < chunk.size(); i++) {
				auto gid = out_row_to_group[row_offset + i];
				for (idx_t c = 0; c < num_key_cols && gid != DConstants::INVALID_INDEX; c++) {
					if (label_column[c] == c) {
						continue;
					}
					Value label = key_results.data[label_column[c]].GetValue(i);
					if (!label.IsNull() && label < rep_keys[label_column[c]][gid]) {
						rep_keys[label_column[c]][gid] = std::move(label);
					}
				}
			}
			row_offset += chunk.size();
		}
	}
	if (out_rep_keys) {
		// Expose each group's label values (group g's value for each key column), which
		// messages and the unbounded diagnosis quote.
		for (idx_t c = 0; c < num_key_cols; c++) {
			(*out_rep_keys)[c] = std::move(rep_keys[label_column[c]]);
		}
	}
}

//! PerGroupCacheEntry/PerGroupCache are declared in physical_decide.hpp (Finalize's
//! private phase methods need PerGroupCache in their signatures). One entry per key
//! table entry: the key's groups over the rows it was computed on, numbered by key
//! value, so every call site reading the key sees the same ids and takes its kept
//! groups as a subsequence. Lifetime: one PerGroupCache per Finalize invocation.

//! The cache's grouping of `keys[key]`, computed on every row of `read_rows` (null: every
//! row). Keys are interned, so the key's index is its identity; INVALID_INDEX is the
//! empty key, which has one entry like any other. An entry computed on fewer rows is
//! rebuilt over both row sets, its values replacing the key's: a group a clause named
//! before keeps its row, from which assembly finds it again (ResolveGroupNames).
static PerGroupCacheEntry &CachedKeyGroups(PerGroupCache &cache, const vector<DecideKey> &keys, idx_t key,
                                           ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                           ColumnDataCollection &data, idx_t num_rows, const vector<bool> *read_rows) {
	if (read_rows && std::find(read_rows->begin(), read_rows->end(), false) == read_rows->end()) {
		read_rows = nullptr;
	}
	auto found = cache.entries.find(key);
	bool covers = found != cache.entries.end();
	if (covers && !found->second.evaluated_rows.empty()) {
		auto &evaluated = found->second.evaluated_rows;
		for (idx_t r = 0; r < num_rows && covers; r++) {
			covers = evaluated[r] || (read_rows && !(*read_rows)[r]);
		}
	}
	if (!covers) {
		vector<bool> rows;
		if (read_rows) {
			rows = *read_rows;
			if (found != cache.entries.end() && !found->second.evaluated_rows.empty()) {
				for (idx_t r = 0; r < num_rows; r++) {
					rows[r] = rows[r] || found->second.evaluated_rows[r];
				}
			}
		}
		if (found == cache.entries.end()) {
			found = cache.entries.emplace(key, PerGroupCacheEntry()).first;
		}
		auto &entry = found->second;
		SolverInput::KeyValues key_values;
		vector<const Expression *> key_exprs;
		vector<const Expression *> label_exprs;
		vector<bool> null_no_group;
		for (auto &element : DecideKeyElements(keys, key)) {
			key_values.null_modes.push_back(element.null_mode);
			key_exprs.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, element.Grouping(), context));
			label_exprs.push_back(
			    element.collated ? &CachedTransformToChunkExpression(chunk_expr_cache, *element.expression, context)
			                     : nullptr);
			null_no_group.push_back(element.null_mode == DecideKeyNullMode::NO_GROUP);
		}
		BuildGroupIds(key_exprs, context, data, num_rows, std::function<bool(idx_t)> {}, entry.unfiltered_row_group_ids,
		              entry.unfiltered_num_groups, &key_values.values, rows.empty() ? nullptr : &rows, null_no_group,
		              label_exprs);
		entry.evaluated_rows = std::move(rows);
		entry.group_rows.assign(entry.unfiltered_num_groups, DConstants::INVALID_INDEX);
		for (idx_t r = num_rows; r-- > 0;) {
			if (entry.unfiltered_row_group_ids[r] != DConstants::INVALID_INDEX) {
				entry.group_rows[entry.unfiltered_row_group_ids[r]] = r;
			}
		}
		if (entry.key_values == DConstants::INVALID_INDEX) {
			entry.key_values = cache.key_values.size();
			cache.key_values.emplace_back();
		}
		cache.key_values[entry.key_values] = std::move(key_values);
	}
	return found->second;
}

//! Look up (or build + cache) the group ids of `keys[key]`, then materialize the
//! call's view in `out_row_group_ids` / `out_num_groups`.
//!
//! Rows where row_filter(r) is false, or outside `read_rows`, get INVALID_INDEX. The
//! groups the call keeps are renumbered 0..K' in key-value order, the subsequence of
//! the key's own numbering.
//!
//! `read_rows` is the clause's kept rows, which hold every row either filter keeps: the
//! key is computed on no row outside them (`CachedKeyGroups`).
//!
//! Returns the kept groups: the call's group g is group `[g]` of the key's cache entry,
//! whose representative values a caller that names a group reads (`GroupNames`).
static vector<idx_t> LookupOrBuildPerGroupIds(
    PerGroupCache &cache, const vector<DecideKey> &keys, idx_t key, ChunkExprCache &chunk_expr_cache,
    ClientContext &context, ColumnDataCollection &data, idx_t num_rows, const std::function<bool(idx_t)> &row_filter,
    const vector<bool> *read_rows, vector<idx_t> &out_row_group_ids, idx_t &out_num_groups,
    // Optional second map over the SAME group numbering but a
    // looser row filter. The right-hand side needs this: its
    // reducers must not inherit the LHS reducers' own `when`,
    // which `row_filter` folds in. Groups that no strict row
    // reached emit no constraint, so their rows stay excluded.
    const std::function<bool(idx_t)> *loose_row_filter = nullptr, vector<idx_t> *out_loose_row_group_ids = nullptr) {
	auto *entry = &CachedKeyGroups(cache, keys, key, chunk_expr_cache, context, data, num_rows, read_rows);

	// The call's kept groups, renumbered densely in key-value order.
	out_row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
	vector<idx_t> kept_groups;
	if (out_loose_row_group_ids) {
		out_loose_row_group_ids->assign(num_rows, DConstants::INVALID_INDEX);
	}
	if (entry->unfiltered_num_groups == 0 || num_rows == 0) {
		out_num_groups = 0;
		return kept_groups;
	}
	auto reads = [&](idx_t r) {
		return (!read_rows || (*read_rows)[r]) && entry->unfiltered_row_group_ids[r] != DConstants::INVALID_INDEX;
	};
	vector<idx_t> remap(entry->unfiltered_num_groups, DConstants::INVALID_INDEX);
	for (idx_t r = 0; r < num_rows; r++) {
		if (reads(r) && (!row_filter || row_filter(r))) {
			remap[entry->unfiltered_row_group_ids[r]] = 0;
		}
	}
	idx_t next_remap = 0;
	for (idx_t unf_gid = 0; unf_gid < remap.size(); unf_gid++) {
		if (remap[unf_gid] == DConstants::INVALID_INDEX) {
			continue;
		}
		remap[unf_gid] = next_remap++;
		kept_groups.push_back(unf_gid);
	}
	for (idx_t r = 0; r < num_rows; r++) {
		if (reads(r) && (!row_filter || row_filter(r))) {
			out_row_group_ids[r] = remap[entry->unfiltered_row_group_ids[r]];
		}
	}
	out_num_groups = next_remap;

	if (out_loose_row_group_ids) {
		// Same `remap`, so a row lands in the group the model builder will emit for.
		for (idx_t r = 0; r < num_rows; r++) {
			if (!reads(r) || (loose_row_filter && *loose_row_filter && !(*loose_row_filter)(r))) {
				continue;
			}
			idx_t mapped = remap[entry->unfiltered_row_group_ids[r]];
			if (mapped == DConstants::INVALID_INDEX) continue; // group emits no constraint
			(*out_loose_row_group_ids)[r] = mapped;
		}
	}
	return kept_groups;
}

//! The name of each of `groups`, groups of key `key` as its cache entry numbers them now, by a row of each: a later
//! read of the key on more rows can renumber them (ResolveGroupNames). The empty key has no value to name a group by.
static vector<GroupName> GroupNames(const PerGroupCache &cache, idx_t key, const vector<idx_t> &groups) {
	vector<GroupName> names;
	if (key == DConstants::INVALID_INDEX) {
		return names;
	}
	auto &entry = cache.entries.at(key);
	for (auto group : groups) {
		names.push_back({entry.key_values, DConstants::INVALID_INDEX, entry.group_rows[group]});
	}
	return names;
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





//===--------------------------------------------------------------------===//
// Phase 2 shared evaluation helpers
//===--------------------------------------------------------------------===//

// TermFilterState is declared in physical_decide.hpp (EvaluateObjective's
// EvaluatedClauses return value carries it across Finalize's phase methods).

// Which rows a reducer with its own key (`sum(per D: ...)`, `sum(D: ...)`) actually
// contributes. The join repeats each D tuple once per matching row; §3.2.2 asks for one
// term per tuple identity, and the key's check on the data (DecideScopeCheckKind::
// DETERMINES) guarantees that every row sharing an identity carries the same value — so
// keeping the first row of each identity and
// dropping the rest yields exactly one contribution per identity, whichever row is
// kept. De-duplication runs inside the `per` partition, after `when` selection and
// `per` partitioning, which is the construction order the paper pins.
//! `eligible`, when given, is the rows the reducer reads (its own `when` included): the
//! first row of each class among THEM is kept, so a class whose first joined row the
//! `when` drops is still counted once.
static vector<bool> BuildQualifierKeepMask(const vector<KeyedMapping> &mappings, idx_t scope_idx,
                                           const vector<idx_t> &row_group_ids, idx_t num_rows,
                                           const vector<bool> *eligible = nullptr) {
	auto &mapping = mappings[scope_idx];
	vector<bool> keep(num_rows, false);
	// Group ids are dense in [0, num_groups) and class ids in [0, num_classes),
    // so (group, class) flattens into one dense index with no hashing.
    idx_t num_groups = 1;
    if (!row_group_ids.empty()) {
        for (idx_t gid : row_group_ids) {
            if (gid != DConstants::INVALID_INDEX && gid + 1 > num_groups) {
                num_groups = gid + 1;
            }
        }
    }
    vector<bool> seen(num_groups * mapping.num_classes, false);
    for (idx_t row = 0; row < num_rows; row++) {
        idx_t group = row_group_ids.empty() ? 0 : row_group_ids[row];
        if (group == DConstants::INVALID_INDEX) {
			continue; // excluded by `when` or in no group — never a surviving identity
		}
		if ((eligible && !(*eligible)[row]) || mapping.IsAbsent(row)) {
			continue; // a row in no class of the reducer's `per` is not read
		}
		idx_t slot = group * mapping.num_classes + mapping.row_to_class[row];
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
static void ApplyQualifierToFilter(const vector<KeyedMapping> &mappings, idx_t scope_idx,
                                   const vector<idx_t> &row_group_ids, idx_t num_rows,
                                   TermFilterState &state) {
    auto keep =
        BuildQualifierKeepMask(mappings, scope_idx, row_group_ids, num_rows, state.has_filter ? &state.mask : nullptr);
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

//! The classes each group's rows stand for (language/guide.md §8.1, D28), kept for
//! the diagnosis: under a `per K:`, K's values among the group's kept rows (key-value
//! order); with no `per`, every kept row of the group; under `per ()`, the whole clause,
//! one set its groups share. One `SolverInput::ClassSet` per group with kept rows,
//! referenced from `ec.group_class_set`. `kept_row_group` maps each kept row to its group;
//! empty when the clause has one implicit group, which then holds every kept row.
static vector<idx_t> CollectGroupClasses(const DecideClauseScope &scope, const vector<DecideKey> &keys,
                                         PerGroupCache &per_group_cache, ChunkExprCache &chunk_expr_cache,
                                         ClientContext &context, ColumnDataCollection &data, idx_t num_rows,
                                         const vector<bool> &kept_rows, const vector<idx_t> &kept_row_group,
                                         idx_t groups, vector<SolverInput::ClassSet> &class_sets) {
	using Kind = SolverInput::ClassSet::Kind;
	const bool keyed = scope.per != DecideClauseScope::PerRole::NONE;
	const idx_t class_key = scope.class_key;
	vector<idx_t> group_class_set(groups, DConstants::INVALID_INDEX);
	if (keyed && class_key == DConstants::INVALID_INDEX) {
		group_class_set.assign(groups, class_sets.size());
		class_sets.push_back({Kind::WHOLE_CLAUSE, {}, {}});
		return group_class_set;
	}
	const bool one_group = kept_row_group.empty();
	std::function<bool(idx_t)> kept = [&](idx_t row) {
		return kept_rows[row];
	};
	vector<vector<idx_t>> rows_of(groups);
	for (idx_t row = 0; row < num_rows; row++) {
		idx_t group = one_group ? (kept_rows[row] ? idx_t(0) : DConstants::INVALID_INDEX) : kept_row_group[row];
		if (group != DConstants::INVALID_INDEX && group < groups) {
			rows_of[group].push_back(row);
		}
	}
	vector<idx_t> class_ids;
	idx_t num_classes = 0;
	vector<GroupName> class_names;
	if (keyed) {
		auto classes = LookupOrBuildPerGroupIds(per_group_cache, keys, class_key, chunk_expr_cache, context, data,
		                                        num_rows, kept, &kept_rows, class_ids, num_classes);
		class_names = GroupNames(per_group_cache, class_key, classes);
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
					set.classes.push_back(class_names[cls]);
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

//! Narrows `rows`, one per row of `chunk`, to those where `condition` holds (NULL keeps
//! none), as a scoped clause reads a `when`: the condition is computed on `rows` only, and
//! the parts of an AND or an OR left to right, each only on the rows the parts before it
//! leave undecided -- an AND's on the rows they keep, an OR's on the rows they do not. The
//! composed MIN/MAX rewrite folds a clause's `when` in front of a term's own, which must
//! not be read where the clause's drops.
static void KeepChunkRowsWhere(const Expression &condition, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                               DataChunk &chunk, vector<bool> &rows) {
	if (condition.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &part : condition.Cast<BoundConjunctionExpression>().children) {
			KeepChunkRowsWhere(*part, chunk_expr_cache, context, chunk, rows);
		}
		return;
	}
	if (condition.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		vector<bool> undecided = rows;
		std::fill(rows.begin(), rows.end(), false);
		for (auto &part : condition.Cast<BoundConjunctionExpression>().children) {
			vector<bool> kept = undecided;
			KeepChunkRowsWhere(*part, chunk_expr_cache, context, chunk, kept);
			for (idx_t row = 0; row < kept.size(); row++) {
				if (kept[row]) {
					rows[row] = true;
					undecided[row] = false;
				}
			}
		}
		return;
	}
	ExpressionExecutor executor(context, CachedTransformToChunkExpression(chunk_expr_cache, condition, context));
	DataChunk result;
	result.Initialize(context, {LogicalType::BOOLEAN});
	ExecuteOnReadRows(executor, chunk, 0, &rows, result);
	UnifiedVectorFormat kept;
	result.data[0].ToUnifiedFormat(chunk.size(), kept);
	auto values = UnifiedVectorFormat::GetData<bool>(kept);
	for (idx_t i = 0; i < chunk.size(); i++) {
		if (rows[i]) {
			auto index = kept.sel->get_index(i);
			rows[i] = kept.validity.RowIsValid(index) && values[index];
		}
	}
}

//! The rows among `read` (every row when null) that each of `conditions` keeps (KeepChunkRowsWhere), all read in one
//! scan of `data`.
static vector<vector<bool>> KeptRowsOfEach(const vector<const Expression *> &conditions, const vector<bool> *read,
                                           ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                           ColumnDataCollection &data, idx_t num_rows) {
	vector<vector<bool>> masks(conditions.size(), read ? *read : vector<bool>(num_rows, true));
	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	idx_t row_offset = 0;
	while (!conditions.empty() && data.Scan(scan, chunk)) {
		auto begin = NumericCast<int64_t>(row_offset);
		auto end = NumericCast<int64_t>(row_offset + chunk.size());
		for (idx_t c = 0; c < conditions.size(); c++) {
			vector<bool> chunk_rows(masks[c].begin() + begin, masks[c].begin() + end);
			KeepChunkRowsWhere(*conditions[c], chunk_expr_cache, context, chunk, chunk_rows);
			std::copy(chunk_rows.begin(), chunk_rows.end(), masks[c].begin() + begin);
		}
		row_offset += chunk.size();
	}
	return masks;
}

//! The rows among `read` (every row when null) that a scoped clause's `condition` keeps.
static vector<bool> KeptRows(const Expression &condition, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                             ColumnDataCollection &data, idx_t num_rows, const vector<bool> *read = nullptr) {
	return std::move(KeptRowsOfEach({&condition}, read, chunk_expr_cache, context, data, num_rows)[0]);
}

//! A term's own `when`, and the state its rows are read into.
struct TermFilterSlot {
	const Expression *condition;
	TermFilterState *state;
};

//! Reads each slot's filter among `read` (every row when null), all in one scan (KeptRowsOfEach), into its state. A
//! filter that keeps every row it reads (a NULL test on data with no NULL) filters nothing: the state keeps no filter,
//! and the term reads like one written without it.
static void ReadTermFilters(const vector<TermFilterSlot> &slots, const vector<bool> *read,
                            ChunkExprCache &chunk_expr_cache, ClientContext &context, ColumnDataCollection &data,
                            idx_t num_rows) {
	vector<const Expression *> conditions;
	for (auto &slot : slots) {
		conditions.push_back(slot.condition);
	}
	auto masks = KeptRowsOfEach(conditions, read, chunk_expr_cache, context, data, num_rows);
	for (idx_t i = 0; i < slots.size(); i++) {
		bool keeps_every_read_row =
		    read ? masks[i] == *read : std::find(masks[i].begin(), masks[i].end(), false) == masks[i].end();
		if (!keeps_every_read_row) {
			slots[i].state->mask = std::move(masks[i]);
			slots[i].state->has_filter = true;
		}
	}
}

//! A decision outside the reducers (`MAXIMIZE when c: sum(x) + w * s`, `sum(x) by (g) + s <= 8`) is counted once
//! (language/guide.md §4): once in the objective, once in each group's row of a clause in the aggregate shape. Its
//! coefficient stays on one row -- the first kept row, the group's first row -- and every other row is zeroed; the
//! scope checks made sure that row stands for all of them (one instance, one value of its data). This is the one
//! place the rule is applied: the model sums every row of every term (ilp_model_builder.cpp).
static idx_t FirstKeptRow(const vector<bool> *kept, idx_t num_rows) {
	for (idx_t row = 0; row < num_rows; row++) {
		if (!kept || (*kept)[row]) {
			return row;
		}
	}
	return DConstants::INVALID_INDEX;
}

//! Keeps the coefficient a term has on each row of `counted`, and zeroes every other row.
static void CountOnce(CoefficientColumn &coefficients, const vector<idx_t> &counted) {
	auto &values = coefficients.MutableDense();
	vector<double> kept(values.size(), 0.0);
	for (auto row : counted) {
		if (row < values.size()) {
			kept[row] = values[row];
		}
	}
	values = std::move(kept);
	coefficients.SyncSize();
}

//! Whether a term stands outside every reducer, which counts once.
static bool CountsOnce(const DecideTerm &term) {
	return term.reduction == LinearTermReduction::NONE;
}

//! The objective's terms outside its reducers, counted on its first kept row.
static void CountTupleTermsOnce(const vector<DecideTerm> &terms, vector<CoefficientColumn> &coefficients,
                                const vector<bool> *kept, idx_t num_rows) {
	idx_t first_kept = FirstKeptRow(kept, num_rows);
	vector<idx_t> counted;
	if (first_kept != DConstants::INVALID_INDEX) {
		counted.push_back(first_kept);
	}
	for (idx_t term_idx = 0; term_idx < terms.size() && term_idx < coefficients.size(); term_idx++) {
		if (CountsOnce(terms[term_idx])) {
			CountOnce(coefficients[term_idx], counted);
		}
	}
}

//! The first row of each group of a clause in the aggregate shape: every row for `row_group_ids` empty (one group),
//! none for a group with no row.
static vector<idx_t> FirstRowOfEachGroup(const vector<idx_t> &row_group_ids, idx_t num_groups, idx_t num_rows) {
	if (row_group_ids.empty()) {
		return num_rows > 0 ? vector<idx_t> {0} : vector<idx_t>();
	}
	vector<idx_t> first(num_groups, DConstants::INVALID_INDEX);
	for (idx_t row = 0; row < row_group_ids.size(); row++) {
		auto group = row_group_ids[row];
		if (group < num_groups && first[group] == DConstants::INVALID_INDEX) {
			first[group] = row;
		}
	}
	vector<idx_t> rows;
	for (auto row : first) {
		if (row != DConstants::INVALID_INDEX) {
			rows.push_back(row);
		}
	}
	return rows;
}

#ifdef DEBUG
//! Verify (DuckDB's Verify() style): no writer left a term outside the reducers with its coefficient on more than one
//! row of a group, which the model would count once per row.
static void VerifyCountedOnce(const vector<DecideTerm> &terms, const vector<CoefficientColumn> &coefficients,
                              const vector<idx_t> &row_group_ids) {
	for (idx_t term_idx = 0; term_idx < terms.size() && term_idx < coefficients.size(); term_idx++) {
		if (!CountsOnce(terms[term_idx])) {
			continue;
		}
		auto &column = coefficients[term_idx];
		unordered_set<idx_t> counted_groups;
		for (idx_t row = 0; row < column.Size(); row++) {
			if (column.Get(row) == 0.0) {
				continue;
			}
			auto group = row_group_ids.empty() ? idx_t(0) : row_group_ids[row];
			if (group != DConstants::INVALID_INDEX && !counted_groups.insert(group).second) {
				throw InternalException("DECIDE: a term outside the reducers is counted on more than one row of a "
				                        "group");
			}
		}
	}
}
#endif

// Evaluate every bilinear term's coefficient expression (constraint or objective side:
// the two are the same shape, differing only in which prepared-model term type feeds
// in and which struct the caller stages results into — both of which are field-
// identical, so this returns the one shared EvaluatedConstraint::BilinearTerm either
// way). Batches every term's coefficient expression into a single ExpressionExecutor
// and scans `data` once, then zeroes rows `filters[i]` excludes (a clause's `when` is
// folded into every term's filter upstream).
// A NULL coefficient marks its row in `null_rows` (a constraint skips that instance); with no
// `null_rows` it is an internal error (the objective has none, DropNullObjectiveTerms).
// AVG scaling is deliberately left to the caller: the constraint side applies it
// immediately per-constraint, the objective side defers it until its two-level grouping is
// resolved in Phase 3 — a real difference in control flow, not something to collapse.
template <class BilinearTermSource>
static vector<EvaluatedConstraint::BilinearTerm>
EvaluateBilinearTerms(const vector<BilinearTermSource> &terms, const vector<TermFilterState> &filters,
                      ChunkExprCache &chunk_expr_cache, ClientContext &context, ColumnDataCollection &data,
                      idx_t num_rows, const DecideSourceNames &names, const string &clause,
                      const vector<bool> &read_rows, vector<bool> *null_rows) {
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
				ValueInputs inputs {terms[term_idx].inputs, bl_chunk, context};
				ExtractDoubleColumn(
				    bl_results.data[j], bl_chunk.size(), terms[term_idx].sign, col,
				    {DecideValueRole::COEFFICIENT, *terms[term_idx].coefficient, names, clause, &inputs}, null_rows,
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
static void ScaleAvgRows(CoefficientColumn &col, bool has_filter, const vector<bool> &filter_mask, bool quadratic_inner,
                         idx_t num_rows, const vector<idx_t> &row_group_ids, idx_t num_groups) {
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

DecideSourceNames PhysicalDecide::Names() const {
	return DecidePlanNames(source_fragments, keyed_scopes, keys, decide_index, aux_var_expressions);
}

string PhysicalDecide::WrittenClause(idx_t source_clause_id) const {
	return DecideWrittenClause(constraint_sources, DecideWrittenObjective(decide_sense, written_objective),
	                           source_clause_id);
}

SolverBackend PhysicalDecide::PlannedSolverBackend() const {
	SolverBackend backend = SolverRegistry::Find(solver_backend_name);
	if (!backend.IsValid()) {
		// Physical planning only accepts a plan the DECIDE optimizer named a backend for, so
		// an unresolvable name means the plan and the registry disagree — an internal
		// defect, never a user error.
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
	auto names = DecidePlanNames(source_fragments, keyed_scopes, keys, decide_index, aux_var_expressions);
	vector<string> objective_accounts;
	for (auto &account : rewrite_accounts) {
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
		                                        objective_rows, rewrite_accounts);
		constraints_text = RenderDecideClauseLayers(layers, unattributed);
	}
	if (decide_objective) {
		string sense_prefix = (decide_sense == DecideSense::MAXIMIZE) ? "MAXIMIZE " : "MINIMIZE ";
		vector<string> objective_strs;
		CollectDecideExpressionStrings(*decide_objective, names, objective_strs, nullptr, nullptr);
		// A MIN/MAX objective leaves a carrier in the tree -- a SUM standing for the
		// extremum, or what the composed objective keeps (its 0.0 placeholder or the terms
		// beside its reducers) -- that its accounts describe and a row would misread. An AVG's sum stays, under its
		// account, as in a constraint.
		if (flat_objective_agg != ObjectiveAggregateType::NONE || !composed_minmax_objective_terms.empty() ||
		    per_inner_agg == ObjectiveAggregateType::MIN_AGG || per_inner_agg == ObjectiveAggregateType::MAX_AGG) {
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

//! Singleton-row folding is decided at layer 05 (DecideOptimizer::FoldSingletonRows).
//! `UserBoundSpec` lives on LogicalDecide with it; execution reads the resulting box
//! rather than deriving one.
using UserBoundSpec = LogicalDecide::UserBoundSpec;

class DecideGlobalSinkState : public GlobalSinkState {
public:
	explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
	    : data(context, op.children[0]->GetTypes()), context(context), op(op),
	      canonicalizer(context, op.decide_index, op.variable_scopes, op.keyed_scopes, op.is_boolean_var) {
        // The decision column box was resolved by DecideOptimizer::FoldSingletonRows.
        // A variable whose box never had a lower bound written is one the query never
        // lowered, and Finalize resolves it to the default 0 floor.
        idx_t num_decide_vars = op.decide_variables.size();
        singleton_row_boxes = op.singleton_row_boxes;
        user_singleton_row_bounds = op.user_singleton_row_bounds;

        // A user bound that contradicts the variable's INTRINSIC domain (a non-negative
        // REAL/INTEGER's `>= 0` floor, or a BOOLEAN's `0/1` box) is a deterministic semantic
        // error, not a constraint conflict to diagnose — loosening can't help, the type can.
        // It is raised here (the main pipeline), before any row is read, so it never reaches the
        // elastic engine and supersedes the generic infeasible error.
        //   - `x <= -1` (REAL): upper below the non-negativity floor → infeasible. But
        //     `x = -1` or `x <= -1 AND x >= -5` explicitly lower the floor below 0, so the
        //     guard is `U < 0 AND L >= 0` (floor still at/above the intrinsic 0).
        //   - `x >= 2` / `x <= -1` (BOOLEAN): outside the 0/1 box. This is Boolean arithmetic, not
        //     a wider variable, so the box stays [0,1] and the query is infeasible; the error quotes
        //     the bound.
        // Both are raised under DIAGNOSE too. A purely user-vs-user inverted box (e.g. `x >= 5 AND
        // x <= 1`, both >= 0) does NOT match and proceeds to the elastic engine, which reports a
        // least-change loosen.
        for (idx_t var = 0; var < num_decide_vars; var++) {
            bool is_bool = var < op.is_boolean_var.size() && op.is_boolean_var[var];
            // The box of the bounds as written. The folded box holds the admitted ends instead
            // (`x = -0.5` admits no INT, which the solve reports as infeasible), so it is not the
            // box the user wrote.
            DecideWrittenBox written;
            for (const auto &bound : user_singleton_row_bounds) {
                if (bound.decide_var_idx != var) {
                    continue;
                }
                if (bound.removal_group_id == DConstants::INVALID_INDEX) {
                    written.Add(bound.sense, bound.k);
                } else {
                    written.AddLowered(bound.sense, bound.k);
                }
            }
            // A floor under a `when` replaces the default on the rows it keeps (AddLowered counts it only when
            // negative). The solve says whether the clauses on the other rows leave a value.
            for (const auto &constraint : op.prepared.constraints) {
                if (constraint->lifted_floor == var) {
                    written.AddLowered('>', constraint->lifted_floor_k);
                }
            }
            if (!written.ContradictsDomain(is_bool)) {
                continue;
            }
            const auto &box = written.Box();
			// An auxiliary decision (an ABS, a norm) is named by what it stands for.
			const bool is_aux = var >= num_decide_vars - op.num_auxiliary_vars;
			string vname = op.decide_variables[var]->GetName();
			for (auto &label : op.aux_var_expressions) {
				if (is_aux && label.first == var) {
					vname = label.second;
				}
			}
			// The clause that set the bound `k`, as written (`max(x) <= -5` as much as `x <= -5`, a strict
			// `x < 0` as typed rather than the folded `x <= -1`).
			auto written_bound = [&](bool upper, double k) {
				for (const auto &bound : user_singleton_row_bounds) {
					if (bound.decide_var_idx == var && bound.sense != (upper ? '>' : '<') && bound.k == k &&
					    bound.source_clause_id < op.constraint_sources.size()) {
						auto &source = op.constraint_sources[bound.source_clause_id];
						return QuoteWrittenClause(source);
					}
				}
				return StringUtil::Format("%s %s %g", vname, upper ? "<=" : ">=", k);
			};
			if (is_bool) {
				// Both sides can contradict at once (`x <= -1 AND x >= 2`); the upper bound is reported.
				bool upper = box.upper < 0.0;
				throw InvalidInputException(
				    StringUtil::Format("DECIDE optimization is infeasible: %s cannot hold for a BOOL variable.",
				                       written_bound(upper, upper ? box.upper : box.lower)));
			}
			if (is_aux) {
				throw InvalidInputException(StringUtil::Format(
				    "DECIDE optimization is infeasible: %s cannot hold because %s is never negative.",
				    written_bound(true, box.upper), vname));
			}
			throw InvalidInputException(StringUtil::Format(
			    "DECIDE optimization is infeasible: %s cannot hold because %s is non-negative (>= 0). "
			    "Add an explicit lower bound if it may be negative.",
			    written_bound(true, box.upper), vname));
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
    const vector<unique_ptr<DecideConstraint>> &singleton_row_constraints = op.prepared.singleton_row_constraints;
    const unique_ptr<DecideObjective> &objective = op.prepared.objective;

    //! Local copies of the decision column box and the singleton-row-bound records that
    //! DecideOptimizer::FoldSingletonRows resolved. Finalize copies the box into
    //! solver_input; the records quote a bound that contradicts its domain and re-open its
    //! direction of the rigid box, while its clause reaches the repair model through
    //! `singleton_row_constraints`.
    vector<DecideBoundBox> singleton_row_boxes;
    vector<UserBoundSpec> user_singleton_row_bounds;

    //===--------------------------------------------------------------------===//
    // Evaluated Coefficients (Phase 2)
    //===--------------------------------------------------------------------===//

    vector<EvaluatedConstraint> evaluated_constraints;
    //! `singleton_row_constraints`, evaluated under DIAGNOSE for the repair model alone.
    vector<EvaluatedConstraint> evaluated_singleton_row_constraints;
    //! The classes each aggregate-shape row's group stands for (D28), referenced from the
    //! rows; filled under DIAGNOSE only and moved into SolverInput::class_sets.
    vector<SolverInput::ClassSet> class_sets;
    //! The value rewrite's helper decisions, each with its reducer's members evaluated
    //! (EvaluateReducerValueLinks); moved into SolverInput::reducer_value_links.
    vector<ReducerValueLinkSpec> evaluated_value_links;
    vector<CoefficientColumn> evaluated_objective_coefficients; // [term_idx]
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

    //! Set when the solve failed under DIAGNOSE: the findings were handed off instead of
    //! raised, so there is no solution to read back and this operator emits no rows. The
    //! DIAGNOSE operator above turns the handoff into the answer.
    bool diagnosis_only = false;
};

//! Late materialization of the group names extraction made: each names its group by a row, which falls in one group
//! of its key's final grouping, computed over every row a clause read the key on. So a group is spelled one way in
//! every message of a query, whichever clause names it and in whatever order (D69).
static void ResolveGroupNames(const PerGroupCache &cache, DecideGlobalSinkState &gstate) {
	vector<const vector<idx_t> *> group_of_row(cache.key_values.size());
	for (auto &entry : cache.entries) {
		if (entry.second.key_values != DConstants::INVALID_INDEX) {
			group_of_row[entry.second.key_values] = &entry.second.unfiltered_row_group_ids;
		}
	}
	auto resolve = [&](vector<GroupName> &names) {
		for (auto &name : names) {
			name.group = (*group_of_row[name.key_values])[name.row];
			D_ASSERT(name.group != DConstants::INVALID_INDEX);
		}
	};
	for (auto *constraints : {&gstate.evaluated_constraints, &gstate.evaluated_singleton_row_constraints}) {
		for (auto &ec : *constraints) {
			resolve(ec.group_names);
		}
	}
	for (auto &link : gstate.evaluated_value_links) {
		resolve(link.rows.group_names);
	}
	for (auto &set : gstate.class_sets) {
		resolve(set.classes);
	}
}

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
//! (it reads the executor chunks, keyed scopes, and BuildGroupIds), so it lives
//! here rather than in the pure router/engine units; Finalize just invokes it.
//!
//! A stateful functor (not a set of lambdas) so its lazily-built caches — the
//! row-scoped candidates and the per-scope keyed candidates — survive across the
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
	const vector<KeyedScopeInfo> &keyed_scopes;
	const vector<KeyedMapping> &keyed_mappings;
	idx_t num_rows;

	//! Row-scoped candidate columns are variable-independent — built once.
	vector<ColumnGrouping> row_candidates;
	bool row_candidates_built = false;
	//! Keyed candidates cached per scope.
	std::map<idx_t, vector<ColumnGrouping>> keyed_candidates;

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
		BuildGroupIds(keys, context, data, num_rows, std::function<bool(idx_t)> {}, row_to_group, num_groups,
		              &rep_keys);
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
			if (rep_keys[0][g].IsNull()) {
				out.null_group = g;
			}
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

	//! Lift a row-indexed grouping to class granularity. The lift is accepted only
	//! when every joined row for a class maps to the same categorical group; this
	//! lets dimension-table labels characterize keyed escapes without inventing a
	//! single value for genuinely one-to-many columns.
	bool LiftRowGroupingToClasses(const ColumnGrouping &rowcg, const KeyedMapping &mapping, idx_t num_classes,
	                              ColumnGrouping &out) {
		out.column = rowcg.column;
		out.clause_referenced = rowcg.clause_referenced;
		out.group_value = rowcg.group_value;
		out.null_group = rowcg.null_group;
		out.instance_to_group.assign(num_classes, DConstants::INVALID_INDEX);
		vector<bool> seen(num_classes, false);
		for (idx_t r = 0; r < num_rows && r < mapping.row_to_class.size() && r < rowcg.instance_to_group.size(); r++) {
			idx_t e = mapping.row_to_class[r];
			if (e >= num_classes) {
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

	//! Keyed candidate columns are any named DECIDE-clause input columns
	//! whose value is constant within each class; grouping is lifted to class
	//! granularity after the constancy check.
	vector<ColumnGrouping> &GetKeyedCandidates(idx_t scope_idx) {
		auto found = keyed_candidates.find(scope_idx);
		if (found != keyed_candidates.end()) {
			return found->second;
		}
		vector<ColumnGrouping> cands;
		auto &mapping = keyed_mappings[scope_idx];
		idx_t num_classes = mapping.num_classes;
		for (idx_t ci = 0; ci < types.size(); ci++) {
			ColumnGrouping rowcg;
			if (!BuildRowGrouping(ci, num_classes, rowcg)) {
				continue;
			}
			ColumnGrouping ecg;
			if (!LiftRowGroupingToClasses(rowcg, mapping, num_classes, ecg)) {
				continue;
			}
			cands.push_back(std::move(ecg));
		}
		return keyed_candidates.emplace(scope_idx, std::move(cands)).first->second;
	}

	vector<ColumnGrouping> operator()(idx_t decide_var_idx, idx_t total_instances) {
		(void)total_instances;
		auto scope = decide_var_idx < var_indexer.var_scope.size()
		                 ? var_indexer.var_scope[decide_var_idx]
		                 : DecideVarScope::ROW;
		if (scope == DecideVarScope::GLOBAL) {
			// One column for the whole query: there is no subset of rows or
			// classes to characterize, so offer no grouping candidates.
			return {};
		}
		if (scope == DecideVarScope::ROW) {
			return GetRowCandidates();
		}
		idx_t scope_idx = decide_var_idx < var_indexer.var_keyed_mapping_idx.size()
		                      ? var_indexer.var_keyed_mapping_idx[decide_var_idx]
		                      : DConstants::INVALID_INDEX;
		if (scope_idx == DConstants::INVALID_INDEX || scope_idx >= keyed_scopes.size() ||
		    scope_idx >= keyed_mappings.size()) {
			return {};
		}
		return GetKeyedCandidates(scope_idx);
	}
};
} // namespace

//! Assemble the unbounded engine's `get_candidates` callback. Resolves the live
//! keyed mappings (the Finalize-local mappings have been moved into the solver
//! input by now, so var_indexer owns them) and returns a stateful
//! provider wrapped as the engine's std::function input.
static std::function<vector<ColumnGrouping>(idx_t, idx_t)>
BuildUnboundedCandidateProvider(ClientContext &context, ColumnDataCollection &data, vector<LogicalType> types,
                                const DecideDiagParams &params, const vector<string> &input_column_names,
                                const vector<bool> &input_column_in_clause, const VarIndexer &var_indexer,
                                const vector<KeyedScopeInfo> &keyed_scopes, idx_t num_rows) {
	return UnboundedCandidateProvider {
	    context,     data,         std::move(types),           params,  input_column_names, input_column_in_clause,
	    var_indexer, keyed_scopes, var_indexer.keyed_mappings, num_rows};
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

string FormatDecideGapPercent(double gap) {
	// Fixed decimals round a gap of 2e-6 to "0.00%", which beside "not proven best" reads as a contradiction. Two
	// significant digits keep it (0.0002%); from 1% up, the whole number plus one decimal, so %g never switches a gap
	// of 100% or more to an exponent.
	double percent = gap * 100.0;
	int digits = 2 + (percent >= 1.0 ? static_cast<int>(std::floor(std::log10(percent))) : 0);
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.*g", digits, percent);
	return buffer;
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
		                       ? StringUtil::Format("  best objective so far: %g  (within %s%% of the best possible)\n",
		                                            result.objective_value, FormatDecideGapPercent(result.gap))
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
// Finalize: PHASE 1.5 — Build Keyed Mappings for Keyed Decisions
//===--------------------------------------------------------------------===//

vector<KeyedMapping> PhysicalDecide::BuildKeyedMappings(ClientContext &context, DecideGlobalSinkState &gstate,
                                                          idx_t num_rows, ChunkExprCache &chunk_expr_cache,
                                                          PerGroupCache &per_group_cache) const {
	vector<KeyedMapping> keyed_mappings;
	for (auto &scope : keyed_scopes) {
		KeyedMapping mapping;
		// A declaration's scope: the classes of its key over every row. A helper decision's
		// scope: the groups of its link's `by` key among the rows the link's clause keeps
		// that hold a member, numbered as its definition numbers them; the key is never
		// evaluated on a row the clause drops. A group with no member has no helper: its
		// reducer is NULL there, so a read of it skips its instance.
		if (scope.value_link != DConstants::INVALID_INDEX) {
			auto &link = reducer_value_links[scope.value_link];
			vector<bool> kept(num_rows, true);
			if (link.when) {
				kept = KeptRows(*link.when, chunk_expr_cache, context, gstate.data, num_rows);
			}
			vector<bool> read =
			    link.filter ? KeptRows(*link.filter, chunk_expr_cache, context, gstate.data, num_rows, &kept) : kept;
			std::function<bool(idx_t)> is_member = [&](idx_t row) {
				return read[row];
			};
			std::function<bool(idx_t)> every_kept_row;
			vector<idx_t> member_ids;
			LookupOrBuildPerGroupIds(per_group_cache, keys, link.group_key, chunk_expr_cache, context, gstate.data,
			                         num_rows, is_member, &kept, member_ids, mapping.num_classes, &every_kept_row,
			                         &mapping.row_to_class);
		} else {
			LookupOrBuildPerGroupIds(per_group_cache, keys, scope.key, chunk_expr_cache, context, gstate.data, num_rows,
			                         std::function<bool(idx_t)> {}, nullptr, mapping.row_to_class, mapping.num_classes);
		}
		// A row in no class (a NULL written key, a dropped row) has no decision: it maps to
		// one spare class, so that every row has an id (KeyedMapping::absent_class).
		for (auto &id : mapping.row_to_class) {
			if (id == DConstants::INVALID_INDEX) {
				mapping.absent_class = mapping.num_classes;
				id = mapping.absent_class;
			}
		}
		if (mapping.absent_class != DConstants::INVALID_INDEX) {
			mapping.num_classes++;
		}
		keyed_mappings.push_back(std::move(mapping));
	}
	return keyed_mappings;
}

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
    DecideProfileScope profile("execution.finalize");
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

    // Per-Finalize cache of unfiltered row→group assignments, shared across
    // every constraint and the objective so a key evaluated once is reused
    // by every other call site that asks for the same expression set.
    PerGroupCache per_group_cache;

    //===--------------------------------------------------------------------===//
    // PHASE 1.5: Build Keyed Mappings for Keyed Decisions
    //===--------------------------------------------------------------------===//

    DecideProfileScope phase("execution.keyed_mapping");
    vector<KeyedMapping> keyed_mappings =
        BuildKeyedMappings(context, gstate, num_rows, chunk_expr_cache, per_group_cache);

    //===--------------------------------------------------------------------===//
    // PHASE 1.6: Scope checks of the clauses
    //===--------------------------------------------------------------------===//

    phase.Next("execution.scope_checks");
    RunScopeChecks(context, gstate, num_rows, chunk_expr_cache, per_group_cache, keyed_mappings);

    //===--------------------------------------------------------------------===//
    // PHASE 2: Evaluate Coefficient Expressions
    //===--------------------------------------------------------------------===//

    // 1. Evaluate constraints
    phase.Next("execution.evaluate_constraints");
    EvaluateConstraints(context, gstate, num_rows, chunk_expr_cache, per_group_cache, keyed_mappings);

    // 2. Evaluate objective
    phase.Next("execution.evaluate_objective");
    EvaluatedClauses evaluated =
        EvaluateObjective(context, gstate, num_rows, chunk_expr_cache, per_group_cache, keyed_mappings);

    // 3. Evaluate composed MIN/MAX clauses (constraint and objective)
    phase.Next("execution.evaluate_composed");
    EvaluateComposedClauses(context, gstate, num_rows, chunk_expr_cache, per_group_cache, keyed_mappings, evaluated);

    // 4. Evaluate the value rewrite's helper decisions (their reducers' members)
    phase.Next("execution.evaluate_value_links");
    EvaluateReducerValueLinks(context, gstate, num_rows, chunk_expr_cache, per_group_cache, keyed_mappings);

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP
    //===--------------------------------------------------------------------===//

    // 3a. Assemble the model input and settle the column box. Nothing below this line
    // touches the data again.
    phase.Next("execution.assemble_input");
    ResolveGroupNames(per_group_cache, gstate);
    VarIndexer var_indexer;
    SolverInput solver_input =
        BuildSolverInput(gstate, num_rows, std::move(keyed_mappings), std::move(per_group_cache.key_values),
                         evaluated, var_indexer);

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
		for (auto &folded : gstate.evaluated_singleton_row_constraints) {
			elastic->input.constraints.push_back(std::move(folded));
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

//! A class or group, as a message names it: `D.depotID = 'D1', D.region = 'East'`.
//! `rep_keys` holds each group's value of each column ([column][group]).
static string DescribeKeyValues(const vector<string> &names, const vector<vector<Value>> &rep_keys, idx_t group) {
	string result;
	for (idx_t i = 0; i < names.size(); i++) {
		result += (i > 0 ? ", " : "") + names[i] + " = " + DecideSqlLiteral(rep_keys[i][group]);
	}
	return result;
}

//! The scope checks left for the operator: the determination of a keyed decision, which needs the class
//! mapping the operator builds. Every other check is a plan under the DECIDE node (DirectAddRowChecks, installed by
//! DecideOptimizer::InstallRowChecks) and has passed by now.
void PhysicalDecide::RunScopeChecks(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                    ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                    const vector<KeyedMapping> &keyed_mappings) const {
	for (auto &check : scope_checks) {
		D_ASSERT(check.kind == DecideScopeCheckKind::DETERMINES && check.decision != DConstants::INVALID_INDEX &&
		         variable_scopes[check.decision].IsKeyed());
		vector<bool> rows(num_rows, true);
		if (check.rows) {
			rows = KeptRows(*check.rows, chunk_expr_cache, context, gstate.data, num_rows);
		}
		vector<bool> filter;
		if (check.filter) {
			filter = KeptRows(*check.filter, chunk_expr_cache, context, gstate.data, num_rows, &rows);
		}
		// The classes of the check's key, over the rows the reducer's own `when` keeps.
		vector<bool> looked_at(num_rows);
		for (idx_t row = 0; row < num_rows; row++) {
			looked_at[row] = rows[row] && (filter.empty() || filter[row]);
		}
		std::function<bool(idx_t)> looked_at_row = [&](idx_t row) {
			return looked_at[row];
		};
		vector<idx_t> ids;
		idx_t count = 0;
		auto groups = LookupOrBuildPerGroupIds(per_group_cache, keys, check.key, chunk_expr_cache, context, gstate.data,
		                                       num_rows, looked_at_row, &looked_at, ids, count);
		auto &scope = variable_scopes[check.decision];
		vector<idx_t> instance(count, DConstants::INVALID_INDEX);
		// Several classes can fail; the message names the least by key value (the groups are numbered in that order).
		idx_t failing = DConstants::INVALID_INDEX;
		for (idx_t row = 0; row < num_rows; row++) {
			auto group = ids[row];
			if (group == DConstants::INVALID_INDEX || keyed_mappings[scope.keyed_scope_idx].IsAbsent(row)) {
				continue; // no decision on this row: a reducer skips it
			}
			auto id = keyed_mappings[scope.keyed_scope_idx].row_to_class[row];
			if (instance[group] == DConstants::INVALID_INDEX) {
				instance[group] = id;
			} else if (instance[group] != id) {
				failing = MinValue(failing, group);
			}
		}
		if (failing != DConstants::INVALID_INDEX) {
			auto text = check.DeterminesText();
			auto &key_values = per_group_cache.key_values[per_group_cache.entries.at(check.key).key_values];
			throw InvalidInputException(
			    text.one_class
			        ? StringUtil::Format(text.Format())
			        : StringUtil::Format(text.Format(), DescribeKeyValues(keys[check.key].Names(), key_values.values,
			                                                              groups[failing])));
		}
	}
}

//! A row in no class of a keyed decision's key has no decision there
//! (KeyedMapping::absent_class), which reads like a NULL: a term that reads it on that row
//! marks the row in `null_rows`, so its instance is skipped. A reducer over the decision never
//! reads the row (DecideCanonicalizer::SkipNullData), and a row outside the clause's `when`
//! is no instance.
static void MarkAbsentDecisionReads(const vector<DecideVarScopeInfo> &scopes, const vector<KeyedMapping> &mappings,
                                    idx_t var, const vector<bool> &scoped_kept, const TermFilterState &filter,
                                    vector<bool> &null_rows) {
	if (var >= scopes.size() || !scopes[var].IsKeyed()) {
		return;
	}
	auto &mapping = mappings[scopes[var].keyed_scope_idx];
	if (mapping.absent_class == DConstants::INVALID_INDEX) {
		return;
	}
	for (idx_t row = 0; row < null_rows.size(); row++) {
		if (scoped_kept[row] && (!filter.has_filter || filter.mask[row]) && mapping.IsAbsent(row)) {
			null_rows[row] = true;
		}
	}
}

//! A NULL that reaches a comparison unreduced makes that instance NULL, and a NULL instance
//! is skipped (deck p. 50): in the aggregate shape the group the row is in (its bound and its
//! terms beside the reducers are one value per group), else the row. `row_group_ids` empty
//! is every row in one group.
static void SkipNullInstances(const vector<bool> &null_rows, bool grouped, idx_t num_rows, vector<idx_t> &row_group_ids,
                              idx_t &num_groups) {
	if (std::find(null_rows.begin(), null_rows.end(), true) == null_rows.end()) {
		return;
	}
	if (row_group_ids.empty()) {
		row_group_ids.assign(num_rows, 0);
		num_groups = 1;
	}
	vector<bool> skipped(num_groups, false);
	for (idx_t row = 0; row < num_rows; row++) {
		if (null_rows[row] && row_group_ids[row] != DConstants::INVALID_INDEX) {
			if (grouped) {
				skipped[row_group_ids[row]] = true;
			} else {
				row_group_ids[row] = DConstants::INVALID_INDEX;
			}
		}
	}
	for (auto &group : row_group_ids) {
		if (group != DConstants::INVALID_INDEX && skipped[group]) {
			group = DConstants::INVALID_INDEX;
		}
	}
}

//! A reducer over no rows is NULL, and a NULL instance is skipped (deck p. 56): in the
//! aggregate shape, a group in which some reducer reads no row has no instance. Only a
//! reducer's term carries a filter (its own `when`, its NULL-data and absent-decision tests,
//! its de-duplication), so a term with none reads every row of its group. `row_group_ids`
//! empty is every row in one group.
static void SkipGroupsWithAnEmptyReducer(const vector<const TermFilterState *> &filters, idx_t num_rows,
                                         vector<idx_t> &row_group_ids, idx_t &num_groups) {
	auto filtered = [](const TermFilterState *filter) {
		return filter->has_filter;
	};
	if (std::none_of(filters.begin(), filters.end(), filtered)) {
		return;
	}
	if (row_group_ids.empty()) {
		row_group_ids.assign(num_rows, 0);
		num_groups = 1;
	}
	vector<bool> skipped(num_groups, false);
	for (auto filter : filters) {
		if (!filter->has_filter) {
			continue;
		}
		vector<bool> read(num_groups, false);
		for (idx_t row = 0; row < num_rows; row++) {
			if (row_group_ids[row] != DConstants::INVALID_INDEX && filter->mask[row]) {
				read[row_group_ids[row]] = true;
			}
		}
		for (idx_t group = 0; group < num_groups; group++) {
			skipped[group] = skipped[group] || !read[group];
		}
	}
	for (auto &group : row_group_ids) {
		if (group != DConstants::INVALID_INDEX && skipped[group]) {
			group = DConstants::INVALID_INDEX;
		}
	}
}

void PhysicalDecide::EvaluateConstraints(ClientContext &context, DecideGlobalSinkState &gstate,
                                         idx_t num_rows, ChunkExprCache &chunk_expr_cache,
                                         PerGroupCache &per_group_cache,
                                         const vector<KeyedMapping> &keyed_mappings) const {
    // The folded scoped bounds follow under DIAGNOSE, numbered after every other clause:
    // only the repair model emits their rows.
    const idx_t num_constraints = gstate.constraints.size();
    const idx_t num_evaluated = num_constraints + (diagnose ? gstate.singleton_row_constraints.size() : 0);
    for (idx_t c = 0; c < num_evaluated; c++) {
        DecideProfileScope clause_profile("extraction.constraint");
        DecideProfileScope::Counter("constraint.ordinal", c);
        DecideProfileScope clause_phase("extraction.constraint_setup");
        auto &constraint =
            c < num_constraints ? gstate.constraints[c] : gstate.singleton_row_constraints[c - num_constraints];

        EvaluatedConstraint eval_const;
        eval_const.comparison_type = constraint->comparison_type;
        eval_const.source_clause_id = constraint->source_clause_id;
        eval_const.repair_group_id = c;
        eval_const.removal_group_id = constraint->removal_group_id;
        // Preserve whether the original LHS was an aggregate (e.g., SUM(...))
        eval_const.lhs_is_aggregate = constraint->lhs_is_aggregate;
        eval_const.lifted_floor = constraint->lifted_floor;
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
		// A clause reads only the rows its `when` keeps, and a term only those its own
		// `when` keeps too: nothing is computed anywhere else.
		vector<bool> scoped_kept(num_rows, true);
		if (constraint->scope.when) {
			scoped_kept = KeptRows(*constraint->scope.when, chunk_expr_cache, context, gstate.data, num_rows);
		}

		{
			// Every term's own `when`, read in one scan (ReadTermFilters).
			vector<TermFilterSlot> filter_slots;

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

            ReadTermFilters(filter_slots, &scoped_kept, chunk_expr_cache, context, gstate.data, num_rows);
            for (auto &slot : filter_slots) {
                if (!slot.state->has_filter) {
                    has_unfiltered_aggregate_part = true;
                    continue;
                }
                has_local_filters = true;
                for (idx_t row = 0; row < num_rows; row++) {
                    if (slot.state->mask[row]) {
                        local_row_active[row] = true;
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

		// The rows where a value the clause reads unreduced is NULL: their instances are
		// skipped once everything is read (SkipNullInstances).
		vector<bool> null_rows(num_rows, false);
		vector<vector<bool>> term_reads;
		vector<const vector<bool> *> term_read_ptrs;
		std::function<bool(idx_t)> kept = [&](idx_t row) {
			return scoped_kept[row];
		};
		term_reads.resize(constraint->lhs_terms.size(), scoped_kept);
		for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
			if (term_filters[term_idx].has_filter) {
				for (idx_t row = 0; row < num_rows; row++) {
					term_reads[term_idx][row] = scoped_kept[row] && term_filters[term_idx].mask[row];
				}
			}
			term_read_ptrs.push_back(&term_reads[term_idx]);
		}

		const auto names = Names();
		const auto clause = WrittenClause(constraint->source_clause_id);
		idx_t coefficient_row_base = 0;
		while (gstate.data.Scan(scan_state, chunk)) {
			if (constraint->lhs_terms.empty()) {
                continue;
            }
            coef_results.Reset();
			ExecuteOnReadRows(coef_executor, chunk, coefficient_row_base, term_read_ptrs, coef_results);
			for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
				auto &col = eval_const.row_coefficients[term_idx].MutableDense();
				auto &term = constraint->lhs_terms[term_idx];
				ValueInputs inputs {term.inputs, chunk, context};
				ExtractDoubleColumn(coef_results.data[term_idx], chunk.size(), term.sign, col,
				                    {DecideValueRole::COEFFICIENT, *term.coefficient, names, clause, &inputs},
				                    &null_rows, &term_reads[term_idx], coefficient_row_base);
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
		bool has_when = (constraint->scope.when != nullptr);

		// Each kept row's group in the aggregate shape: the clause's kept rows by its `by`
		// key, without the reducers' own `when` filters -- the rows whose classes a
		// diagnosis reports for the group. Empty means "every kept row, one group", the same
		// convention `row_group_ids` uses, which is all a clause with no `by` key has. Diagnosis
		// prep: built under DIAGNOSE only, its one reader.
		vector<idx_t> kept_row_group_ids;

		{
			// The clause's rows are the kept rows (language/guide.md §6.1):
			// grouped by the `by` key its reducers share in the aggregate shape, one per
			// class of its `per` key in the per-row shape, else one per kept row. A row whose
			// written key is NULL is in no group; a `when` that keeps no row leaves none.
			if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
				// A reducer's members are the rows its own `when` keeps -- a MIN/MAX reads
				// them as members -- while every kept row of a group bounds it. A `<>`
				// emits every kept row, since each one's bound is excluded.
				bool every_kept_row = constraint->comparison_type == ExpressionType::COMPARE_NOTEQUAL;
				std::function<bool(idx_t)> emitted = [&](idx_t row) {
					return scoped_kept[row] && (every_kept_row || !has_local_filters || local_row_active[row]);
				};
				if (constraint->group_key != DConstants::INVALID_INDEX) {
					auto groups = LookupOrBuildPerGroupIds(
					    per_group_cache, keys, constraint->group_key, chunk_expr_cache, context, gstate.data, num_rows,
					    emitted, &scoped_kept, eval_const.row_group_ids, eval_const.num_groups,
					    diagnose ? &kept : nullptr, diagnose ? &kept_row_group_ids : nullptr);
					eval_const.group_names = GroupNames(per_group_cache, constraint->group_key, groups);
				} else if (has_when || has_local_filters) {
					eval_const.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
					for (idx_t row = 0; row < num_rows; row++) {
						eval_const.row_group_ids[row] = emitted(row) ? 0 : DConstants::INVALID_INDEX;
					}
					eval_const.num_groups = 1;
				}
				// A `per K:` that names classes holds once per class; with every kept row's K
				// NULL there is no class, so no instance. The first kept row in a class settles it.
				if (constraint->scope.per == DecideClauseScope::PerRole::NAMES_CLASSES) {
					auto &classes = CachedKeyGroups(per_group_cache, keys, constraint->scope.class_key,
					                                chunk_expr_cache, context, gstate.data, num_rows, &scoped_kept);
					bool any_class = false;
					for (idx_t row = 0; row < num_rows && !any_class; row++) {
						any_class =
						    scoped_kept[row] && classes.unfiltered_row_group_ids[row] != DConstants::INVALID_INDEX;
					}
					if (!any_class) {
						eval_const.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
						eval_const.num_groups = 0;
					}
				}
				// The classes each group's rows stand for -- the aggregate row of a group,
				// or the easy MIN/MAX's member rows -- which an expanded diagnosis reports
				// one by one (D28). Diagnosis prep: paid for only under DIAGNOSE.
				if (diagnose) {
					const bool one_group = eval_const.row_group_ids.empty();
					eval_const.group_class_set = CollectGroupClasses(
					    constraint->scope, keys, per_group_cache, chunk_expr_cache, context, gstate.data, num_rows,
					    scoped_kept, one_group ? vector<idx_t>() : kept_row_group_ids,
					    one_group ? 1 : eval_const.num_groups, gstate.class_sets);
				}
			} else if (constraint->scope.per == DecideClauseScope::PerRole::ONE_ROW_PER_CLASS) {
				// One row per class of the `per` key among the kept rows: its first, which
				// stands for the class because the key determines the whole clause.
				vector<idx_t> class_ids;
				idx_t num_classes = 0;
				auto classes = LookupOrBuildPerGroupIds(per_group_cache, keys, constraint->scope.class_key,
				                                        chunk_expr_cache, context, gstate.data, num_rows, kept,
				                                        &scoped_kept, class_ids, num_classes);
				eval_const.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
				vector<bool> seen(num_classes, false);
				for (idx_t row = 0; row < num_rows; row++) {
					auto class_id = class_ids[row];
					if (class_id != DConstants::INVALID_INDEX && !seen[class_id]) {
						seen[class_id] = true;
						eval_const.row_group_ids[row] = class_id;
					}
				}
				eval_const.num_groups = num_classes;
				eval_const.group_names = GroupNames(per_group_cache, constraint->scope.class_key, classes);
				eval_const.rows_stand_with_their_class = !has_when;
				// Each class's row stands for its class (D28), under DIAGNOSE only.
				if (diagnose) {
					eval_const.group_class_set = CollectGroupClasses(
					    constraint->scope, keys, per_group_cache, chunk_expr_cache, context, gstate.data, num_rows,
					    scoped_kept, eval_const.row_group_ids, num_classes, gstate.class_sets);
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
            ApplyQualifierToFilter(keyed_mappings, scope_idx, eval_const.row_group_ids, num_rows,
                                   term_filters[term_idx]);
            MaskCoefficientColumn(eval_const.row_coefficients[term_idx], term_filters[term_idx].mask);
		}
		for (idx_t term_idx = 0; term_idx < constraint->bilinear_terms.size(); term_idx++) {
			idx_t scope_idx = constraint->bilinear_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(keyed_mappings, scope_idx, eval_const.row_group_ids, num_rows,
                                   bilinear_filters[term_idx]);
		}
		for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
            idx_t scope_idx = constraint->quadratic_groups[group_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(keyed_mappings, scope_idx, eval_const.row_group_ids, num_rows,
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
		if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
			vector<const TermFilterState *> filters;
			for (auto *each : {&term_filters, &bilinear_filters, &quadratic_filters}) {
				for (auto &filter : *each) {
					filters.push_back(&filter);
				}
			}
			SkipGroupsWithAnEmptyReducer(filters, num_rows, eval_const.row_group_ids, eval_const.num_groups);
		}
		// A decision beside the reducers counts once in each group's row.
		if (constraint->lhs_is_aggregate) {
			auto counted = FirstRowOfEachGroup(eval_const.row_group_ids, eval_const.num_groups, num_rows);
			for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
				if (CountsOnce(constraint->lhs_terms[term_idx])) {
					CountOnce(eval_const.row_coefficients[term_idx], counted);
				}
			}
#ifdef DEBUG
			VerifyCountedOnce(constraint->lhs_terms, eval_const.row_coefficients, eval_const.row_group_ids);
#endif
		}

		// Evaluate RHS: a constant, or a value of each row.

		clause_phase.Next("extraction.rhs");
		eval_const.rhs_written = RenderDecideSource(*constraint->rhs_expr, names);
		if (constraint->rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			auto &const_expr = constraint->rhs_expr->Cast<BoundConstantExpression>();
            double rhs_constant = const_expr.value.GetValue<double>();
            eval_const.rhs_values.AssignScalar(num_rows, rhs_constant);
            // A constant RHS is one editable scalar shared across every row this
            // clause emits, so the elastic engine collapses them to one shared slack.
            eval_const.rhs_is_shared_scalar = true;
		} else {
			// A bound of data, read per row. The optimizer's bound-side window rewrite made each reducer on it a
			// column, and the bound of a clause in the aggregate shape its group's tightest value, already checked by
			// the VALUE_IS_VALID scope checks. A row-wise bound is checked here, as it is read.
			// Canonicalization classifies the complete rebuilt bound. This is what
			// keeps forward/reversed subquery spellings and arithmetic over query-wide
			// values equivalent without re-deciding shape in the physical layer.
			eval_const.rhs_is_shared_scalar = IsQueryWideBoundTag(constraint->rhs_expr->GetAlias());
			// Capture the RHS symbolic name (e.g. the data column `cap_col`) so query-mode
			// infeasible diagnosis can render `x <= cap_col + delta`. Only meaningful for a
			// genuine data RHS; a shared-scalar RHS reports the numeric knob instead.
			if (!eval_const.rhs_is_shared_scalar &&
                !ContainsScalarSubqueryProvenance(*constraint->rhs_expr)) {
				eval_const.rhs_label = RenderDecideSource(*constraint->rhs_expr, source_fragments, keyed_scopes, &keys);
			}
			eval_const.rhs_values.Reserve(num_rows);
		}

		// One pass over the rows reads what each row holds: a right side of data, and, for a bound on one decision,
		// the values it admits there, which the model builder folds into the decision's column. The right side is
		// read first, so a NULL or NaN there is reported as the right side's.
		bool rhs_per_row = constraint->rhs_expr->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT;
		bool admitted = constraint->admitted_lower != nullptr;
		if (rhs_per_row || admitted) {
			ExpressionExecutor row_executor(context);
			vector<LogicalType> row_types;
			if (rhs_per_row) {
				auto &transformed_rhs =
				    CachedTransformToChunkExpression(chunk_expr_cache, *constraint->rhs_expr, context);
				row_executor.AddExpression(transformed_rhs);
				row_types.push_back(transformed_rhs.return_type);
			}
			if (admitted) {
				row_executor.AddExpression(
				    CachedTransformToChunkExpression(chunk_expr_cache, *constraint->admitted_lower, context));
				row_executor.AddExpression(
				    CachedTransformToChunkExpression(chunk_expr_cache, *constraint->admitted_upper, context));
				row_types.push_back(LogicalType::DOUBLE);
				row_types.push_back(LogicalType::DOUBLE);
				eval_const.admitted_lower.Reserve(num_rows);
				eval_const.admitted_upper.Reserve(num_rows);
			}
			ColumnDataScanState scan_state;
			gstate.data.InitializeScan(scan_state);
			DataChunk scan_chunk;
			scan_chunk.Initialize(context, gstate.data.Types());
			DataChunk row_result;
			row_result.Initialize(context, row_types);
			idx_t scanned = 0;
			while (gstate.data.Scan(scan_state, scan_chunk)) {
				row_result.Reset();
				ExecuteOnReadRows(row_executor, scan_chunk, scanned, &scoped_kept, row_result);
				idx_t column = 0;
				if (rhs_per_row) {
					// Infinities are admitted here and nowhere else: this column becomes
					// the model row's `rhs`, where ±inf is a meaningful bound. Canonical-
					// ization rebuilds `x + v <= K` as `x <= K - v`, so an infinite K that
					// was folded as a column bound in its bare spelling must stay legal
					// in its rebuilt one. `inf - inf` still yields NaN and is still refused.
					ExtractDoubleColumn(
					    row_result.data[column++], scan_chunk.size(), 1.0, eval_const.rhs_values.MutableDense(),
					    {DecideValueRole::BOUND, *constraint->rhs_expr, names}, &null_rows, &scoped_kept, scanned);
					eval_const.rhs_values.SyncSize();
				}
				if (admitted) {
					ExtractDoubleColumn(
					    row_result.data[column++], scan_chunk.size(), 1.0, eval_const.admitted_lower.MutableDense(),
					    {DecideValueRole::BOUND, *constraint->rhs_expr, names}, &null_rows, &scoped_kept, scanned);
					ExtractDoubleColumn(
					    row_result.data[column++], scan_chunk.size(), 1.0, eval_const.admitted_upper.MutableDense(),
					    {DecideValueRole::BOUND, *constraint->rhs_expr, names}, &null_rows, &scoped_kept, scanned);
					eval_const.admitted_lower.SyncSize();
					eval_const.admitted_upper.SyncSize();
				}
				scanned += scan_chunk.size();
			}
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
			auto ebts = EvaluateBilinearTerms(constraint->bilinear_terms, bilinear_filters, chunk_expr_cache, context,
			                                  gstate.data, num_rows, Names(),
			                                  WrittenClause(constraint->source_clause_id), scoped_kept, &null_rows);
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
						got_scale = true;
						if (value.IsNull()) {
							// A NULL factor makes every instance of the clause NULL.
							for (idx_t row = 0; row < num_rows; row++) {
								null_rows[row] = null_rows[row] || scoped_kept[row];
							}
							continue;
						}
                        double factor = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                        if (!std::isfinite(factor)) {
							ThrowInvalidValue({DecideValueRole::COEFFICIENT, *qg.scale, names, clause});
						}
                        if (qg.scale_divides && factor == 0.0) {
                            throw InvalidInputException(
                                "DECIDE constraint: division by zero -- the factor on a squared aggregate evaluated to 0.");
                        }
                        eqg.sign *= qg.scale_divides ? 1.0 / factor : factor;
                        // The reciprocal of a finite factor is a number DeciDB computes: past a DOUBLE it names the
                        // clause.
                        if (!std::isfinite(eqg.sign)) {
                            throw InvalidInputException(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, clause));
                        }
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
							auto &term = qg.inner_terms[inner_idx];
							ValueInputs inputs {term.inputs, qchunk, context};
							ExtractDoubleColumn(
							    q_results.data[inner_idx], qchunk.size(), term.sign, col,
							    {DecideValueRole::COEFFICIENT, *term.coefficient, names, clause, &inputs}, &null_rows,
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

		for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
			MarkAbsentDecisionReads(variable_scopes, keyed_mappings, constraint->lhs_terms[term_idx].variable_index,
			                        scoped_kept, term_filters[term_idx], null_rows);
		}
		for (idx_t term_idx = 0; term_idx < constraint->bilinear_terms.size(); term_idx++) {
			for (auto var : {constraint->bilinear_terms[term_idx].var_a, constraint->bilinear_terms[term_idx].var_b}) {
				MarkAbsentDecisionReads(variable_scopes, keyed_mappings, var, scoped_kept, bilinear_filters[term_idx],
				                        null_rows);
			}
		}
		for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
			for (auto &term : constraint->quadratic_groups[group_idx].inner_terms) {
				MarkAbsentDecisionReads(variable_scopes, keyed_mappings, term.variable_index, scoped_kept,
				                        quadratic_filters[group_idx], null_rows);
			}
		}
		SkipNullInstances(null_rows, constraint->lhs_is_aggregate || constraint->was_minmax_easy, num_rows,
		                  eval_const.row_group_ids, eval_const.num_groups);
		(c < num_constraints ? gstate.evaluated_constraints : gstate.evaluated_singleton_row_constraints)
		    .push_back(std::move(eval_const));
	}
}

PhysicalDecide::EvaluatedClauses PhysicalDecide::EvaluateObjective(ClientContext &context,
                                                                   DecideGlobalSinkState &gstate,
                                                                   idx_t num_rows,
                                                                   ChunkExprCache &chunk_expr_cache,
                                                                   PerGroupCache &per_group_cache,
                                                                   const vector<KeyedMapping> &keyed_mappings) const {
    // 2. Evaluate objective
    vector<TermFilterState> obj_linear_term_filters;
    vector<TermFilterState> obj_quadratic_term_filters;
    vector<TermFilterState> obj_bilinear_filters;
    vector<bool> objective_when_mask;
    bool objective_has_when = false;
	// The objective's `when` joins every term's own, only the rows a term keeps are read,
	// and a decision outside its reducers counts once (language/guide.md §4).
	// A reducer that keeps no row is NULL, so its terms are dropped.
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
		}

		// DeciDB: evaluate both the linear and quadratic-inner term lists so that
        // mixed objectives (e.g. SUM(POWER(x-t, 2) + penalty * x)) emit coefficients
        // into both solver-input arrays. The two buckets are processed together
        // inside a single scan over gstate.data — doubling coefficient evaluators
        // was cheap, but doubling the ColumnDataCollection scan was not.
        struct ObjBucket {
            vector<DecideTerm> *src_terms;
            vector<CoefficientColumn> *out_coeffs;
            vector<idx_t> *out_var_indices;
            vector<TermFilterState> *out_term_filters;
            //! The linear bucket: a term outside the reducers counts once. Every squared term sits in a reducer.
            bool counts_once = false;
        };
        vector<ObjBucket> buckets;
        if (!gstate.objective->terms.empty()) {
            buckets.push_back({&gstate.objective->terms,
                               &gstate.evaluated_objective_coefficients,
                               &gstate.objective_variable_indices,
                               &obj_linear_term_filters,
                               true});
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
            vector<TermFilterSlot> obj_filter_slots;

            for (auto &b : buckets) {
                b.out_term_filters->resize(b.src_terms->size());
                b.out_coeffs->resize(b.src_terms->size());
                for (idx_t term_idx = 0; term_idx < b.src_terms->size(); term_idx++) {
                    auto &term = (*b.src_terms)[term_idx];
                    (*b.out_term_filters)[term_idx].avg_scale = term.avg_scale;
                    if (term.filter) {
                        obj_filter_slots.push_back({term.filter.get(), &(*b.out_term_filters)[term_idx]});
                    }
                    b.out_var_indices->push_back(term.variable_index);
                }
            }

            ReadTermFilters(obj_filter_slots, objective_has_when ? &objective_when_mask : nullptr, chunk_expr_cache,
                            context, gstate.data, num_rows);
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
			const auto names = Names();
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

			const auto objective = WrittenClause(DConstants::INVALID_INDEX);
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
					auto &filter = (*b.out_term_filters)[term_idx];
					// A NULL term was read as 0 before like terms merged (DropNullObjectiveTerms).
					auto &term = (*b.src_terms)[term_idx];
					ValueInputs inputs {term.inputs, obj_chunk, context};
					ExtractDoubleColumn(obj_results.data[j], obj_chunk.size(), static_cast<double>(term_sign), out,
					                    {DecideValueRole::COEFFICIENT, *term.coefficient, names, objective, &inputs},
					                    nullptr, filter.has_filter ? &filter.mask : nullptr, obj_row_offset);
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
				if (b.counts_once) {
					CountTupleTermsOnce(*b.src_terms, *b.out_coeffs,
					                    objective_has_when ? &objective_when_mask : nullptr, num_rows);
				}
			}
		}

		obj_bilinear_filters.resize(gstate.objective->bilinear_terms.size());
        {
            vector<TermFilterSlot> bil_slots;
            for (idx_t i = 0; i < gstate.objective->bilinear_terms.size(); i++) {
                auto &term = gstate.objective->bilinear_terms[i];
                obj_bilinear_filters[i].avg_scale = term.avg_scale;
                if (term.filter) {
                    bil_slots.push_back({term.filter.get(), &obj_bilinear_filters[i]});
                }
            }
            ReadTermFilters(bil_slots, objective_has_when ? &objective_when_mask : nullptr, chunk_expr_cache, context,
                            gstate.data, num_rows);
			for (auto &state : obj_bilinear_filters) {
				fold_objective_when(state);
			}
		}

		// No extra debug here; solver output will show timings/objective

		// Evaluate bilinear term coefficients (non-Boolean pairs left by optimizer).
        if (gstate.objective->has_bilinear) {
			vector<bool> every_row(num_rows, true);
			// A NULL product was read as 0 before the model was built (DropNullObjectiveTerms).
			gstate.evaluated_bilinear_terms = EvaluateBilinearTerms(
			    gstate.objective->bilinear_terms, obj_bilinear_filters, chunk_expr_cache, context, gstate.data,
			    num_rows, Names(), WrittenClause(DConstants::INVALID_INDEX), every_row, nullptr);
			// A product outside every reducer (`MAXIMIZE sum(x) + s * u`) counts once, as a
			// linear term there does; one inside a reducer sums over its rows.
			idx_t first_kept = FirstKeptRow(objective_has_when ? &objective_when_mask : nullptr, num_rows);
			vector<idx_t> counted;
			if (first_kept != DConstants::INVALID_INDEX) {
				counted.push_back(first_kept);
			}
			for (idx_t i = 0; i < gstate.evaluated_bilinear_terms.size(); i++) {
				if (gstate.objective->bilinear_terms[i].reduction == LinearTermReduction::NONE) {
					CountOnce(gstate.evaluated_bilinear_terms[i].row_coefficients, counted);
				}
			}
		}
	}

	EvaluatedClauses result;
	result.linear_filters = std::move(obj_linear_term_filters);
    result.quadratic_filters = std::move(obj_quadratic_term_filters);
    result.bilinear_filters = std::move(obj_bilinear_filters);

    // The two-level objective's grouping. It is a read of the data (its key columns), so it
    // belongs to this phase and not to the formulation pass — the grouping a widened
    // variable bound produces is the same grouping. It runs last here because the
    // "does this row carry a term at all" test below reads the filter masks settled above.
	if (gstate.objective && gstate.objective->two_level_key != DConstants::INVALID_INDEX) {
		bool objective_has_local_filters = false;
		bool objective_has_unfiltered_part = false;
        for (auto &f : result.linear_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }
        for (auto &f : result.quadratic_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }
        for (auto &f : result.bilinear_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }

        auto objective_row_has_local_term = [&](idx_t row) {
            if (!objective_has_local_filters || objective_has_unfiltered_part) {
                return true;
            }
            for (auto &f : result.linear_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            for (auto &f : result.quadratic_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            for (auto &f : result.bilinear_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            return false;
        };

		auto obj_row_is_included = [&](idx_t row) -> bool {
			if (objective_has_when && !objective_when_mask[row])
				return false;
			if (!objective_row_has_local_term(row))
				return false;
			return true;
		};

		// A row whose written key is NULL is in no class; a class no row reads has no term. Objective groups are not
		// diagnosed by clause key, so they carry no label.
		LookupOrBuildPerGroupIds(per_group_cache, keys, gstate.objective->two_level_key, chunk_expr_cache, context,
		                         gstate.data, num_rows, obj_row_is_included, nullptr, result.objective_row_group_ids,
		                         result.objective_num_groups);
	}

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
void PhysicalDecide::EvaluateComposedClauses(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                             ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                             const vector<KeyedMapping> &keyed_mappings,
                                             EvaluatedClauses &evaluated) const {
	if (composed_minmax_constraints.empty() && composed_minmax_objective_terms.empty()) {
		return;
	}
	// Evaluate a DecideTerm's per-row coefficient (scaled by term.sign). `what` is
	// "constraint" or "objective", naming the clause in any error. `read_rows` are the rows
	// the term reads: every other row is 0 and never evaluated for NULL.
	auto EvaluateTermCoefs = [&](const DecideTerm &term, const char *what, const string &clause,
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
					// A reducer's `when` tests its data (DecideCanonicalizer::SkipNullData).
					throw InternalException("Composed MIN/MAX %s: a coefficient is NULL on a row its reducer reads",
					                        what);
				}
				double d = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                if (!std::isfinite(d)) {
					ValueInputs inputs {term.inputs, chunk, context};
					ThrowInvalidValue({DecideValueRole::COEFFICIENT, *term.coefficient, Names(), clause, &inputs}, r);
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
	//! A NULL factor is NULL: `is_null` is set and 0 returned.
	auto EvaluateQueryWideScale = [&](const Expression &scale, bool divides, const char *what, const string &clause,
	                                  bool &is_null) -> double {
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
				is_null = true;
				return 0.0;
			}
            v = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
            if (!std::isfinite(v)) {
				ThrowInvalidValue({DecideValueRole::COEFFICIENT, scale, Names(), clause});
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
	// `null_factor` is set when a term's factor is NULL; the term's coefficients are then 0.
	auto EvaluateComposedTerms = [&](const vector<LogicalDecide::ComposedMinMaxTerm> &terms, const char *what,
	                                 const string &clause, bool &null_factor) -> vector<ComposedMinMaxTermData> {
		// Collect filter expressions for batch evaluation. The clause's terms share its
		// `when`, folded in front of each term's own.
		vector<const Expression *> cond_ptrs;
		for (auto &term : terms) {
			if (term.filter)
				cond_ptrs.push_back(term.filter.get());
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
				ta.scale = EvaluateQueryWideScale(*term.scale, term.scale_divides, what, clause, null_factor);
			}
            if (ta.is_minmax) {
				// Named as the plan prints the reducer, so a diagnosis over the extremum
				// column reads `max(ship)`.
				ta.label = term.agg_name + "(" +
				           RenderDecideSource(*term.inner_expr, source_fragments, keyed_scopes, &keys) + ")";
			}
			ta.inner_terms = &term.inner_terms;
			if (term.filter) {
                ta.filter_mask = std::move(masks[mask_slot++]);
            } else {
                ta.filter_mask.assign(num_rows, true);
            }
			// Fold in the relation qualifier's de-duplication mask, exactly as the
			// non-composed reducer paths do. A composed objective is never two-level, so every
			// row is in one group and `row_group_ids` is empty. Applied uniformly:
			// for MIN/MAX it is provably a no-op (every row of an identity carries the
			// same value, so dropping repeats cannot move an extremum), which keeps
			// one code path instead of a special case that has to stay in sync. A term
			// keeps the first row of each class among the rows it reads.
			if (term.qualifier_scope_idx != DConstants::INVALID_INDEX) {
				auto keep =
				    BuildQualifierKeepMask(keyed_mappings, term.qualifier_scope_idx, {}, num_rows, &ta.filter_mask);
				for (idx_t row = 0; row < num_rows; row++) {
					ta.filter_mask[row] = ta.filter_mask[row] && keep[row];
				}
			}
			for (auto &inner_t : term.inner_terms) {
				ta.per_term_coefs.push_back(EvaluateTermCoefs(inner_t, what, clause, ta.filter_mask));
			}
			evaluated.push_back(std::move(ta));
		}

		return evaluated;
	};
	// A reducer over no rows is NULL (deck p. 56): its clause is skipped, its objective term dropped.
	auto reads_no_row = [](const ComposedMinMaxTermData &term) {
		return std::find(term.filter_mask.begin(), term.filter_mask.end(), true) == term.filter_mask.end();
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
		// A NULL factor makes the clause's one instance NULL, which is skipped (deck p. 50).
		bool null_factor = false;
		clause.terms =
		    EvaluateComposedTerms(spec.terms, "constraint", WrittenClause(spec.source_clause_id), null_factor);
		clause.skipped = null_factor || std::any_of(clause.terms.begin(), clause.terms.end(), reads_no_row);
		clause.repair_group_id =
		    gstate.constraints.size() + gstate.singleton_row_constraints.size() + evaluated.composed_constraints.size();
		// The classes its one row stands for (D28), under DIAGNOSE: its `per` key's values
		// among the rows its `when` keeps, or those rows with no `per`.
		if (diagnose) {
			vector<bool> kept_rows(num_rows, true);
			if (spec.scope.when) {
				kept_rows = KeptRows(*spec.scope.when, chunk_expr_cache, context, gstate.data, num_rows);
			}
			clause.class_set = CollectGroupClasses(spec.scope, keys, per_group_cache, chunk_expr_cache, context,
			                                       gstate.data, num_rows, kept_rows, {}, 1, gstate.class_sets)[0];
		}
		evaluated.composed_constraints.push_back(std::move(clause));
	}

	if (!composed_minmax_objective_terms.empty()) {
		// A term with a NULL factor is dropped: its factor reads 0.
		bool null_factor = false;
		auto terms = EvaluateComposedTerms(composed_minmax_objective_terms, "objective",
		                                   WrittenClause(DConstants::INVALID_INDEX), null_factor);
		terms.erase(std::remove_if(terms.begin(), terms.end(), reads_no_row), terms.end());
		evaluated.composed_objective_terms = std::move(terms);
	}
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 2, sub-phase 4 -- the value rewrite's helper decisions
//===--------------------------------------------------------------------===//

//! Each term's coefficient (its sign applied) on the rows `read` keeps, 0 elsewhere, in
//! one scan: the columns of a lifted reducer's members.
static void EvaluateTermColumns(ClientContext &context, ColumnDataCollection &data, ChunkExprCache &chunk_expr_cache,
                                const vector<DecideTerm> &terms, const vector<bool> &read, idx_t num_rows,
                                const DecideSourceNames &names, const string &clause,
                                vector<CoefficientColumn> &columns) {
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
			ValueInputs inputs {terms[t].inputs, chunk, context};
			ExtractDoubleColumn(results.data[t], chunk.size(), terms[t].sign, columns[t].MutableDense(),
			                    {DecideValueRole::COEFFICIENT, *terms[t].coefficient, names, clause, &inputs}, nullptr,
			                    &read, row_base);
			columns[t].SyncSize();
		}
		row_base += chunk.size();
	}
}

void PhysicalDecide::EvaluateReducerValueLinks(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                               ChunkExprCache &chunk_expr_cache, PerGroupCache &per_group_cache,
                                               const vector<KeyedMapping> &keyed_mappings) const {
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
		ec.rhs_values.AssignScalar(num_rows, 0.0);
		ec.rhs_is_shared_scalar = true;

		// The rows the clause keeps, then those the reducer's own `when` keeps: its
		// members. Nothing is computed on any other row.
		vector<bool> kept(num_rows, true);
		if (link.when) {
			kept = KeptRows(*link.when, chunk_expr_cache, context, gstate.data, num_rows);
		}
		vector<bool> read =
		    link.filter ? KeptRows(*link.filter, chunk_expr_cache, context, gstate.data, num_rows, &kept) : kept;

		// One group per `by` value among the kept rows that holds a member, numbered as the
		// helper's scope numbers them (BuildKeyedMappings); a member row stands in its
		// group, every other row outside.
		std::function<bool(idx_t)> is_member = [&](idx_t row) {
			return read[row];
		};
		auto groups =
		    LookupOrBuildPerGroupIds(per_group_cache, keys, link.group_key, chunk_expr_cache, context, gstate.data,
		                             num_rows, is_member, &kept, ec.row_group_ids, ec.num_groups);
		ec.group_names = GroupNames(per_group_cache, link.group_key, groups);

		// The members' coefficients, computed on the member rows alone. A helper serves every clause that reads its
		// reducer, so a number past a DOUBLE names the reducer as written (as LinearizeReducerValueLinks does).
		EvaluateTermColumns(context, gstate.data, chunk_expr_cache, link.inner_terms, read, num_rows, Names(),
		                    link.label, ec.row_coefficients);
		for (auto &term : link.inner_terms) {
			ec.variable_indices.push_back(term.variable_index);
			ec.coefficient_labels.push_back(term.coefficient->GetName());
		}
		// Its own `per`: one member per class of that key within each group. A row the
		// de-duplication drops leaves the group, so that a MIN/MAX reads no member pinned
		// at 0. AVG then divides by the members a group has.
		TermFilterState members;
		members.has_filter = true;
		members.mask = read;
		if (link.qualifier_scope_idx != DConstants::INVALID_INDEX) {
			ApplyQualifierToFilter(keyed_mappings, link.qualifier_scope_idx, ec.row_group_ids, num_rows, members);
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

SolverInput PhysicalDecide::BuildSolverInput(DecideGlobalSinkState &gstate, idx_t num_rows,
                                             vector<KeyedMapping> keyed_mappings,
                                             vector<SolverInput::KeyValues> key_values,
                                             EvaluatedClauses &evaluated,
                                             VarIndexer &out_var_indexer) const {
    DecideProfileScope phase("assembly.var_indexer");
    idx_t num_decide_vars = decide_variables.size();

    // Construct SolverInput
    SolverInput solver_input;
    solver_input.num_rows = num_rows;
    solver_input.num_decide_vars = num_decide_vars;
    solver_input.keyed_mappings = std::move(keyed_mappings);
    solver_input.variable_scopes = variable_scopes;

    // The flat column space, built ONCE and handed to every consumer below. It reads only the scopes and the class
    // mappings, which are final here, and the floors a `when` lowers (ResolveLiftedFloors) need its row to instance
    // map before the first box pass.
    //
    // It is reused for: (1) the row to instance map of the floors, (2) the flat indices every construct site emits
    // against, a general or indicator constraint naming flat columns, so a site that may state one natively needs the
    // index before it can emit, (3) the SolverModel::Build() call inside SolveModel(), and (4) gstate.var_indexer for
    // solution readback after the solve. Nothing below adds a decide variable, so the row / keyed / global blocks are
    // final from this point; only `solver_input.num_global_vars` keeps growing as auxiliary globals are appended,
    // and `total_vars` is refreshed just before the solve. The owning form survives past `solver_input` once that is
    // moved onto gstate.
    out_var_indexer = VarIndexer::Build(solver_input);
    phase.Next("assembly.copy_and_bounds");

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

    // Bounds were folded in the gstate constructor from simple
    // `x OP const` / BETWEEN constraints; those comparisons were skipped in
    // AnalyzeConstraint so we don't re-emit them as per-row model rows.
    solver_input.constraint_sources = constraint_sources;
    solver_input.objective_written = DecideWrittenObjective(decide_sense, written_objective);
    // A variable the query never explicitly lowered gets the default lower bound 0 and
    // stays non-negative. A variable with an explicit (possibly negative) lower bound
    // keeps it, and a written bound of -1e30 or below is no bound, as -1e29 is. Must
    // run before any consumer of lower_bounds (implied-bound propagation, Big-M,
    // McCormick, model builder).
    solver_input.lower_bounds.resize(gstate.singleton_row_boxes.size());
    solver_input.upper_bounds.resize(gstate.singleton_row_boxes.size());
    for (idx_t var = 0; var < gstate.singleton_row_boxes.size(); var++) {
        solver_input.lower_bounds[var] = gstate.singleton_row_boxes[var].EffectiveLower();
        solver_input.upper_bounds[var] = gstate.singleton_row_boxes[var].upper;
    }

	// Everything below this point is stage 06 working on the evaluated model, so
	// hand the constraints and the formulation links over before the first pass.
	solver_input.constraints = std::move(gstate.evaluated_constraints);
	solver_input.class_sets = std::move(gstate.class_sets);
	solver_input.key_values = std::move(key_values);
	solver_input.minmax_clause_labels = minmax_clause_labels;
	solver_input.ne_clause_sources = ne_clause_sources;
	for (auto &link : bilinear_links) {
        solver_input.bilinear_links.push_back(BilinearLinkSpec {
            link.aux_idx, link.bool_var_idx, link.other_var_idx,
            link.source_clause_id, link.removal_group_id});
    }
    for (auto &link : abs_maximize_links) {
        solver_input.abs_maximize_links.push_back(AbsMaximizeLinkSpec {link.aux_idx, link.y_idx});
    }
    solver_input.reducer_value_links = std::move(gstate.evaluated_value_links);

    // The rigid box, captured before any propagation runs and with every user-folded
    // direction re-opened. What is left is the intrinsic domain alone — the only part of
    // the column box that survives infeasibility diagnosis unchanged, and therefore the
    // only part a structural rewrite may rely on. The re-opening mirrors the folded
    // bounds' clauses in the repair model (`singleton_row_constraints`), which relax the same
    // directions for the same reason.
    solver_input.rigid_lower_bounds = solver_input.lower_bounds;
    solver_input.rigid_upper_bounds = solver_input.upper_bounds;
    for (const auto &b : gstate.user_singleton_row_bounds) {
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

    // A constant lower bound under a `when` replaces the default floor of 0 on the instances its rows reach. After the
    // rigid box is captured, which it re-opens for the decisions it lowers, and before any pass reads the box.
    phase.Next("assembly.lifted_floors");
    ResolveLiftedFloors(solver_input, out_var_indexer, gstate.singleton_row_boxes);
    phase.Next("assembly.implied_bounds");

    // Data-driven implied-bound propagation: derive finite upper bounds for
    // otherwise-unbounded variables from non-negative `<=`/`=` constraints (the
    // knapsack/budget pattern), so the downstream Big-M can be finite and tight.
    // Only provably-implied bounds are applied; the feasible region is unchanged.
    DecidePropagateImpliedBounds(solver_input, out_var_indexer, num_rows);
    phase.Next("assembly.objective_and_avg");
    // Objective (linear part)
    solver_input.objective_coefficients = std::move(gstate.evaluated_objective_coefficients);
    solver_input.objective_variable_indices = std::move(gstate.objective_variable_indices);
#ifdef DEBUG
    if (gstate.objective) {
        VerifyCountedOnce(gstate.objective->terms, solver_input.objective_coefficients, vector<idx_t>());
    }
#endif
    solver_input.sense = decide_sense;
    solver_input.objective_constant = objective_constant_offset;

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
    // The two-level objective's grouping, settled by PHASE 2 off the data. It is copied onto
    // `SolverInput` here because every consumer below reads it there: the qualifier
    // de-duplication, AVG scaling, and the MIN/MAX objective encoding.
    solver_input.objective_row_group_ids = evaluated.objective_row_group_ids;
    solver_input.objective_num_groups = evaluated.objective_num_groups;

    // Relation-qualified reducers in the objective: same de-duplication as on the
    // constraint side, applied once the objective's two-level groups are settled and before
    // AVG scaling reads the surviving-row counts.
    if (gstate.objective) {
        for (idx_t term_idx = 0;
             term_idx < gstate.objective->terms.size() && term_idx < evaluated.linear_filters.size(); term_idx++) {
            idx_t scope_idx = gstate.objective->terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.keyed_mappings, scope_idx,
                                   solver_input.objective_row_group_ids, num_rows,
                                   evaluated.linear_filters[term_idx]);
            MaskCoefficientColumn(solver_input.objective_coefficients[term_idx],
                                  evaluated.linear_filters[term_idx].mask);
		}
		for (idx_t term_idx = 0; term_idx < gstate.objective->squared_terms.size() &&
                                 term_idx < evaluated.quadratic_filters.size();
             term_idx++) {
            idx_t scope_idx = gstate.objective->squared_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.keyed_mappings, scope_idx,
                                   solver_input.objective_row_group_ids, num_rows,
                                   evaluated.quadratic_filters[term_idx]);
            MaskCoefficientColumn(solver_input.quadratic_inner_coefficients[term_idx],
                                  evaluated.quadratic_filters[term_idx].mask);
        }
        for (idx_t term_idx = 0; term_idx < gstate.objective->bilinear_terms.size() &&
                                 term_idx < evaluated.bilinear_filters.size();
             term_idx++) {
            idx_t scope_idx = gstate.objective->bilinear_terms[term_idx].qualifier_scope_idx;
            if (scope_idx == DConstants::INVALID_INDEX) continue;
            ApplyQualifierToFilter(solver_input.keyed_mappings, scope_idx,
                                   solver_input.objective_row_group_ids, num_rows,
                                   evaluated.bilinear_filters[term_idx]);
            MaskCoefficientColumn(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
                                  evaluated.bilinear_filters[term_idx].mask);
        }
	}

	for (idx_t term_idx = 0; term_idx < evaluated.linear_filters.size(); term_idx++) {
		if (!evaluated.linear_filters[term_idx].avg_scale) {
            continue;
        }
		ScaleAvgRows(solver_input.objective_coefficients[term_idx], evaluated.linear_filters[term_idx].has_filter,
		             evaluated.linear_filters[term_idx].mask, /*quadratic_inner=*/false, num_rows,
		             solver_input.objective_row_group_ids, solver_input.objective_num_groups);
	}
	for (idx_t term_idx = 0; term_idx < evaluated.quadratic_filters.size(); term_idx++) {
		if (!evaluated.quadratic_filters[term_idx].avg_scale) {
            continue;
        }
		ScaleAvgRows(solver_input.quadratic_inner_coefficients[term_idx],
		             evaluated.quadratic_filters[term_idx].has_filter, evaluated.quadratic_filters[term_idx].mask,
		             /*quadratic_inner=*/true, num_rows, solver_input.objective_row_group_ids,
		             solver_input.objective_num_groups);
	}

	for (idx_t term_idx = 0; term_idx < evaluated.bilinear_filters.size(); term_idx++) {
		if (!evaluated.bilinear_filters[term_idx].avg_scale) {
            continue;
        }
		ScaleAvgRows(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
		             evaluated.bilinear_filters[term_idx].has_filter, evaluated.bilinear_filters[term_idx].mask,
		             /*quadratic_inner=*/false, num_rows, solver_input.objective_row_group_ids,
		             solver_input.objective_num_groups);
	}
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
    // keyed and global blocks are final, and only the global block grows as auxiliaries
    // are appended below. That is what lets a second formulation, against a different
    // box, name exactly the same columns as the first — which the elastic path relies on
    // to read its solution back through the same labels.
#ifdef DEBUG
    const idx_t formulation_entry_columns = var_indexer.global_block_start;
#endif

    // Plain declared names, for the refusals the linearization can raise. `alias`
    // lives on BaseExpression, so no cast is needed to read it.
    vector<string> decide_var_names;
    decide_var_names.reserve(decide_variables.size());
    for (auto &v : decide_variables) {
        decide_var_names.push_back(v->alias);
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
                double M = DecideTightPerRowBigM(ec, box, num_rows, decide_var_names,
                                                 InstanceBox {solver_input, var_indexer},
                                                 DecideWrittenClause(solver_input, ec.source_clause_id));
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
	// A factor that is a helper decision of the value rewrite (`b * sum(x)`) has no box until its members' boxes
	// are derived below, so with helpers the refusal waits for them.
	DeriveBilinearAuxiliaryBounds(solver_input, box, decide_var_names,
	                              /*refuse_when_unbounded=*/solver_input.reducer_value_links.empty());

    // ABS FIRST, and the order is load-bearing. Deriving an ABS auxiliary's range is
    // also what boxes its column, and every linearizer below computes its Big-M from
    // column boxes — run them first and an outer MIN/MAX or `<>` over ABS(...) sees an
    // unbounded column and refuses a query whose bound was there to be computed. Only
    // the LOWERING path refuses an underivable range; the native path leaves the
    // auxiliary open and answers.
    phase.Next("formulation.abs_bounds");
	if (solver_input.reducer_value_links.empty()) {
		DeriveAbsAuxiliaryBounds(solver_input, var_indexer, decide_var_names, !native_abs);
	} else {
		// The value rewrite's helper decisions are boxed by their members' reach, and a
		// member may be an ABS auxiliary or a product with a helper (`b * sum(x)`) while an
		// ABS may hold a helper: each box is the other's input. The derivations only
		// tighten, and a reducer holds no reducer, so alternating them settles in as many
		// passes as the constructs nest. The refusals are decided last, on the settled
		// boxes, naming a helper by its reducer.
		for (auto &link : solver_input.reducer_value_links) {
			if (link.value_var < decide_var_names.size() && !link.label.empty()) {
				decide_var_names[link.value_var] = link.label;
			}
		}
		const idx_t max_passes = solver_input.abs_maximize_links.size() + solver_input.bilinear_links.size() +
		                         solver_input.reducer_value_links.size() + 1;
		for (idx_t pass = 0;; pass++) {
			auto lower = solver_input.lower_bounds;
			auto upper = solver_input.upper_bounds;
			auto reach_lower = solver_input.instance_reach_lower;
			auto reach_upper = solver_input.instance_reach_upper;
			DeriveBilinearAuxiliaryBounds(solver_input, box, decide_var_names, /*refuse_when_unbounded=*/false);
			DeriveAbsAuxiliaryBounds(solver_input, var_indexer, decide_var_names, /*refuse_when_unbounded=*/false);
			DeriveReducerValueBounds(solver_input, var_indexer);
			if (lower == solver_input.lower_bounds && upper == solver_input.upper_bounds &&
			    reach_lower == solver_input.instance_reach_lower && reach_upper == solver_input.instance_reach_upper) {
				break;
			}
			if (pass == max_passes) {
				throw InternalException("DECIDE: the ABS and helper decision boxes did not settle");
			}
		}
		DeriveBilinearAuxiliaryBounds(solver_input, box, decide_var_names);
		DeriveAbsAuxiliaryBounds(solver_input, var_indexer, decide_var_names, !native_abs);
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
    if (!ne_clause_sources.empty()) {
        LinearizeNotEqual(solver_input, var_indexer, decide_var_names);
    }

    // Emit the McCormick envelope for every bilinear w = b * x auxiliary.
	for (const auto &saved : bilinear_declared_boxes) {
		solver_input.lower_bounds[saved.first] = saved.second.first;
		solver_input.upper_bounds[saved.first] = saved.second.second;
	}
    phase.Next("formulation.bilinear");
    LinearizeBilinear(solver_input, var_indexer, box);

    // Handle MIN/MAX objective: create global auxiliary variable z and linking constraints.
    // Two paths: (A) flat MIN/MAX, (B) the two-level form outer(per k: inner(e) by (k)).
    //
    // For a two-level objective, the auxiliary formulation:
    //   Phase A (inner): per-group auxiliary z_g for inner MIN/MAX aggregate
    //   Phase B (outer): global auxiliary w for outer MIN/MAX aggregate
    //
    // Easy/hard classification at each level:
    //   Easy (no indicators): MINIMIZE+MAX or MAXIMIZE+MIN
    //   Hard (Big-M indicators): MINIMIZE+MIN or MAXIMIZE+MAX

    // Global variables are appended at var_indexer.global_block_start.
    // As we add more global vars, their indices are global_block_start + g
    // where g is the position in the global vars array.

    // Encode a MIN/MAX objective (flat `MIN(expr)`/`MAX(expr)` or the two-level
    // `outer(per key: inner(expr) by (key))`) into global auxiliaries and their
    // envelope/indicator rows. Pure stage-06 work: it reads the coefficients PHASE 2
    // evaluated plus the flat column space, and needs no row of its own.
    phase.Next("formulation.minmax_objective");
    MinMaxObjectiveSpec minmax_objective_spec;
    minmax_objective_spec.flat_agg = flat_objective_agg;
    minmax_objective_spec.flat_is_easy = flat_objective_is_easy;
    minmax_objective_spec.per_inner_agg = per_inner_agg;
    minmax_objective_spec.per_outer_agg = per_outer_agg;
    minmax_objective_spec.per_inner_is_easy = per_inner_is_easy;
    minmax_objective_spec.per_outer_is_easy = per_outer_is_easy;
    minmax_objective_spec.per_inner_was_avg = per_inner_was_avg;
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
	LinearizeMinMaxObjective(solver_input, var_indexer, minmax_objective_spec, decide_var_names, native_min_max);

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
		if (clause.skipped) {
			continue;
		}
		LinearizeComposedMinMaxConstraint(solver_input, var_indexer, terms, clause.rhs, spec.outer_cmp,
		                                  spec.source_clause_id, spec.removal_group_id, clause.repair_group_id,
		                                  clause.class_set, decide_var_names, native_min_max);
	}
	if (!evaluated.composed_objective_terms.empty()) {
		auto terms = evaluated.composed_objective_terms;
        LinearizeComposedMinMaxObjective(solver_input, var_indexer, terms, decide_var_names,
                                         native_min_max);
	}

	// Refresh total_vars: the row/keyed blocks were finalized before linearization,
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
//! intrinsic domain, user bounds stage 05 folded, and implied tightenings
//! `DecidePropagateImpliedBounds` derived. The last two are each backed by a row the
//! elastic engine may loosen, so leaving them in place would pin the loosenable row behind
//! a bound that cannot move — the engine would report an edit that changes nothing.
//! `rigid_*` is exactly the intrinsic domain, which is what makes the test "is this
//! direction narrower than rigid?" the same as "is a loosenable row holding it?":
//! propagation only ever narrows, and stage 05's folding is re-opened in the snapshot.
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
        if (std::abs(input.lower_bounds[var]) < DECIDE_EFFECTIVE_INFINITY) {
            magnitude = std::max(magnitude, std::abs(input.lower_bounds[var]));
        }
        if (std::abs(input.upper_bounds[var]) < DECIDE_EFFECTIVE_INFINITY) {
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
                                                  bool keep_singleton_rows,
                                                  VarIndexer &out_var_indexer,
                                                  vector<string> &out_global_labels) const {
    SolverInput input = elastic.input;
    VarIndexer indexer = elastic.var_indexer;
    input.keep_singleton_rows = keep_singleton_rows;

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
    // One warning slot for the whole query: the solved model and, under DIAGNOSE, the repair model share it.
    LargeNumberWarning large_number_warning;
    SolveModelOptions solve_options;
    solve_options.extract_unbounded_ray = diagnosis_armed;
    solve_options.large_number_warning = &large_number_warning;
    // Keep an inverted column box (col_lower > col_upper) alive through Build under
    // diagnosis so the INFEASIBLE terminal can reset it to the intrinsic domain and
    // diagnose it (Bug 1, all-column-bound conflicts). An unprefixed query keeps the
    // fast throw.
    solver_input.keep_singleton_rows = diagnosis_armed;

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
	solve_options.mip_gap = GetDecideMipGap(context);
	// Ctrl-C cuts the solve short and returns the best-so-far instead of aborting the
	// query — also ordinary execution behaviour, so the poll is installed unconditionally.
	// Both backends honor it mid-solve.
	solve_options.interrupt_poll = [&context]() {
		return context.interrupted.load();
	};
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
        // attribution (both backends cut the solve short mid-search and say so); a Ctrl-C
        // that lands after the solver's last check arrives indistinguishable from a
        // timeout. The query interrupt is set either way, so read it too. The answer is
        // written back into the result once, so the report and the error below read the
        // same decision.
        bool stopped_by_user = solve_result.user_interrupted || context.interrupted.load();
        solve_result.user_interrupted = stopped_by_user;
        if (stopped_by_user) {
            // Ctrl-C is not a question. The user has already said stop, so there is
            // nothing to offer: hand back the best-so-far, with the caveat that it is not
            // proven best. This is the one stop that does not need a terminal — the
            // decision was made at the keyboard, not in an answer to a prompt. With
            // nothing found there is nothing to hand back: the query was cancelled, so
            // it fails as DuckDB's own interrupt does, with one message.
            if (!solve_result.has_solution) {
                throw Exception(ExceptionType::INTERRUPT, "DECIDE stopped at your request before finding a solution.");
            }
            PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget);
            fprintf(stderr,
                    "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n");
            context.interrupted = false;
            deliver_incumbent = true;
        } else if (!can_prompt) {
            // The clock ran out and nobody is at the keyboard (tests, benchmarks, `-c`
            // pipes, the C API), so there is no one to answer the offer. Print the
            // checkpoint once and raise: never prompt where no one can answer.
            PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget);
			ThrowDecideSolveError(solve_result, PlannedSolverBackend());
        } else {
            // The interrupt poll installed on the first solve persists on the retained
            // session into every Continue() chunk, but the prompt below already stops the
            // user at each boundary, and a watcher thread contending with the interactive
            // getline destabilizes the prompt (found empirically) — so reset it to
            // boundary-only here. The entry interrupt already fired *before* this loop, so a
            // first-solve Ctrl-C still routed us in.
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
					ThrowDecideSolveError(solve_result, PlannedSolverBackend());
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
				ThrowDecideSolveError(solve_result, PlannedSolverBackend());
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
                                            keyed_scopes, num_rows);

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
                result.status = SolverStatus::OTHER;
            }
			// OTHER is a solve the backend could not finish, thrown or returned (a model HiGHS refuses
			// to load returns it): no answer about the repair model, so it must not read as "no repair".
			if (result.status == SolverStatus::OTHER) {
				diagnostic_solver_failed = true;
			} else if (result.status == SolverStatus::TIME_LIMIT) {
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
                                                  /*keep_singleton_rows=*/true,
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
        // The repair model is a solver-neutral model of this query too. The warning is said once, so this only speaks
        // for a number the solved model did not hold (a one-decision row it folded into a bound).
        large_number_warning.Check(elastic_model);
        InfeasibleDiagnosisInput diag_input {
            elastic_model, elastic_indexer,        var_labels, var_is_aux, elastic_global_labels,
            diag_params,   solve_diagnostic_model,
        };
        diag = DiagnoseInfeasible(diag_input);
		// The engine reads every status it gets: a solve it can do without (the tie-break, the
		// certificate of a printed amount) keeps the repair it already has. So a repair it
		// reports stands, whatever a solve on the way to it returned.
		if (diag.valid && !diag.findings.empty()) {
            report(std::move(diag));
            break;
        }
        if (diagnostic_solver_failed) {
            // A backend that cannot load or solve the repair model. That is a limit on
            // what can be explained, not a failure of the query itself.
            report(BuildUndiagnosedDiagnostic(
                "infeasible",
                "the solver could not analyse the repair model for this query, so no "
                "least-change repair could be computed."));
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
		ThrowDecideSolveError(solve_result, PlannedSolverBackend());
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
                    : -DECIDE_MAX_EXACT_INTEGER;
    double hi = var_type == LogicalType::INTEGER
                    ? static_cast<double>(NumericLimits<int32_t>::Maximum())
                    : DECIDE_MAX_EXACT_INTEGER;
    // The helper refuses a NaN, which compares false against everything, rather than
    // passing it through to the cast this exists to protect.
    if (!DecideRoundedAnswerWithin(solution_value, lo, hi)) {
        // The wording is shared with A1's direct plan (`decide_numeric_policy.hpp`).
        throw InvalidInputException(DecideWholeNumberRangeMessage(var_name, StringUtil::Format("%.0f", rounded)));
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

		// A row in no class of a keyed decision's key has no decision: its value is NULL.
		if (decide_var_idx < gstate.var_indexer.var_scope.size() &&
		    gstate.var_indexer.var_scope[decide_var_idx] == DecideVarScope::KEYED) {
			auto &mapping = gstate.var_indexer.keyed_mappings[gstate.var_indexer.var_keyed_mapping_idx[decide_var_idx]];
			for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
				if (mapping.IsAbsent(source_state.current_row_offset + row_in_chunk)) {
					FlatVector::SetNull(output_vector, row_in_chunk, true);
				}
			}
		}
	}

    // Update row offset for next chunk
    source_state.current_row_offset += chunk_size;

    return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
