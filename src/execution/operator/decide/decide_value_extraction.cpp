//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_value_extraction.cpp
//
// How stage 08 computes a value over the input's rows and refuses one the model cannot read
// (decide_value_extraction.hpp).
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/rule/constant_folding.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Transform Helpers
//===--------------------------------------------------------------------===//
// These functions replace BoundColumnRefExpression nodes with
// BoundReferenceExpression nodes so that DuckDB's ExpressionExecutor can
// evaluate expressions against data chunks (which use positional indices).

unique_ptr<Expression> ToChunkReferences(const Expression &expr, ClientContext &context) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		vector<unique_ptr<Expression>> new_children;
		for (auto &child : func.children) {
			new_children.push_back(ToChunkReferences(*child, context));
		}
		unique_ptr<FunctionData> new_bind_info;
		if (func.bind_info) {
			new_bind_info = func.bind_info->Copy();
		}
		return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
		                                                           std::move(new_children), std::move(new_bind_info));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		auto transformed_child = ToChunkReferences(*cast.child, context);
		return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type,
		                                          cast.try_cast);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// The operator evaluates row values only. A reducer of data is a column by now (the optimizer's bound-side
		// window rewrite); one of a decision is a set of terms, never evaluated.
		throw InternalException("DECIDE: a reducer reached row evaluation: %s", expr.ToString());
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		auto left = ToChunkReferences(*comp.left, context);
		auto right = ToChunkReferences(*comp.right, context);
		return make_uniq_base<Expression, BoundComparisonExpression>(comp.type, std::move(left), std::move(right));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
		for (auto &child : conj.children) {
			result->children.push_back(ToChunkReferences(*child, context));
		}
		return std::move(result);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		auto result = make_uniq<BoundOperatorExpression>(op_expr.type, op_expr.return_type);
		for (auto &child : op_expr.children) {
			result->children.push_back(ToChunkReferences(*child, context));
		}
		return std::move(result);
	} else {
		return expr.Copy();
	}
}

unique_ptr<Expression> DecideFoldConstants(unique_ptr<Expression> expr, ClientContext &context) {
	ExpressionRewriter rewriter(context);
	rewriter.rules.push_back(make_uniq<ConstantFoldingRule>(rewriter));
	vector<unique_ptr<Expression>> tree;
	tree.push_back(std::move(expr));
	LogicalProjection holder(DConstants::INVALID_INDEX, std::move(tree));
	rewriter.VisitOperator(holder);
	return std::move(holder.expressions[0]);
}

unique_ptr<Expression> TransformToChunkExpression(const Expression &expr, ClientContext &context) {
	return DecideFoldConstants(ToChunkReferences(expr, context), context);
}

void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset,
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

void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset, const vector<bool> *rows,
                       DataChunk &results) {
	if (!rows) {
		executor.Execute(chunk, results);
		return;
	}
	ExecuteOnReadRows(executor, chunk, row_offset, vector<const vector<bool> *>(results.ColumnCount(), rows), results);
}

//! Whether `input` is a finite number or NULL on row `row` of the window, the one row a failing coefficient reads.
static bool IsFiniteOnRow(const DecideValueInputs &inputs, const Expression &input, idx_t row) {
	auto value = inputs.value_at(input, row);
	return value.IsNull() || std::isfinite(value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
}

void ThrowInvalidValue(const ValueSource &source, idx_t row) {
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

void ExtractDoubleColumn(Vector &result_vec, idx_t count, double sign, vector<double> &out, const ValueSource &source,
                         vector<bool> *null_rows, const vector<bool> *read_rows, idx_t row_offset) {
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

idx_t FirstKeptRow(const vector<bool> *kept, idx_t num_rows) {
	for (idx_t row = 0; row < num_rows; row++) {
		if (!kept || (*kept)[row]) {
			return row;
		}
	}
	return DConstants::INVALID_INDEX;
}

void CountOnce(CoefficientColumn &coefficients, const vector<idx_t> &counted) {
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

bool CountsOnce(const DecideTerm &term) {
	return term.reduction == LinearTermReduction::NONE;
}

void CountTupleTermsOnce(const vector<DecideTerm> &terms, vector<CoefficientColumn> &coefficients,
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

vector<idx_t> FirstRowOfEachGroup(const vector<idx_t> &row_group_ids, idx_t num_groups, idx_t num_rows) {
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

idx_t CountInstances(const vector<idx_t> &row_group_ids, idx_t num_groups, idx_t num_rows, bool grouped) {
	if (row_group_ids.empty()) {
		return grouped ? MinValue<idx_t>(num_rows, 1) : num_rows;
	}
	if (!grouped) {
		return NumericCast<idx_t>(std::count_if(row_group_ids.begin(), row_group_ids.end(),
		                                        [](idx_t group) { return group != DConstants::INVALID_INDEX; }));
	}
	vector<bool> seen(num_groups, false);
	idx_t count = 0;
	for (auto group : row_group_ids) {
		if (group != DConstants::INVALID_INDEX && !seen[group]) {
			seen[group] = true;
			count++;
		}
	}
	return count;
}

void SkipNullInstances(const vector<bool> &null_rows, bool grouped, idx_t num_rows, vector<idx_t> &row_group_ids,
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

} // namespace duckdb
