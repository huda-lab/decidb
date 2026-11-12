//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_value_extraction.cpp
//
// How stage 08 computes a value over the input's rows and refuses one the model cannot read
// (decide_value_extraction.hpp).
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"

#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/rule/constant_folding.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Transform Helpers
//===--------------------------------------------------------------------===//
// These functions replace BoundColumnRefExpression nodes with
// BoundReferenceExpression nodes so that DuckDB's ExpressionExecutor can
// evaluate expressions against data chunks (which use positional indices).

unique_ptr<Expression> ToChunkReferences(const Expression &expr, ClientContext &context, const DecideChunkLeaf &leaf) {
	if (leaf) {
		auto mapped = leaf(expr);
		if (mapped) {
			return mapped;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		vector<unique_ptr<Expression>> new_children;
		for (auto &child : func.children) {
			new_children.push_back(ToChunkReferences(*child, context, leaf));
		}
		unique_ptr<FunctionData> new_bind_info;
		if (func.bind_info) {
			new_bind_info = func.bind_info->Copy();
		}
		return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
		                                                           std::move(new_children), std::move(new_bind_info));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		auto transformed_child = ToChunkReferences(*cast.child, context, leaf);
		return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type,
		                                          cast.try_cast);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// The operator evaluates row values only. A reducer of data is a column by now (the optimizer's bound-side
		// window rewrite); one of a decision is a set of terms, never evaluated.
		throw InternalException("DECIDE: a reducer reached row evaluation: %s", expr.ToString());
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		auto left = ToChunkReferences(*comp.left, context, leaf);
		auto right = ToChunkReferences(*comp.right, context, leaf);
		return make_uniq_base<Expression, BoundComparisonExpression>(comp.type, std::move(left), std::move(right));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
		for (auto &child : conj.children) {
			result->children.push_back(ToChunkReferences(*child, context, leaf));
		}
		return std::move(result);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		auto result = make_uniq<BoundOperatorExpression>(op_expr.type, op_expr.return_type);
		for (auto &child : op_expr.children) {
			result->children.push_back(ToChunkReferences(*child, context, leaf));
		}
		return std::move(result);
	} else {
		auto copy = expr.Copy();
		if (leaf) {
			// Another layout maps every leaf, so the children of a node copied whole are mapped too.
			ExpressionIterator::EnumerateChildren(
			    *copy, [&](unique_ptr<Expression> &child) { child = ToChunkReferences(*child, context, leaf); });
		}
		return copy;
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

bool DecideErrorStopsTheQuery(const ErrorData &error) {
	auto type = error.Type();
	return type == ExceptionType::INTERRUPT || type == ExceptionType::OUT_OF_MEMORY || type == ExceptionType::FATAL;
}

void DecideEvaluateWindow(ClientContext &context, unique_ptr<ExpressionExecutor> &executor, idx_t expr_idx,
                          const std::function<unique_ptr<ExpressionExecutor>()> &remake, const Expression &lowered,
                          Vector &values, std::map<idx_t, ErrorData> &errors, DataChunk &chunk, idx_t offset,
                          ErrorData *raised) {
	const idx_t count = chunk.size();
	const auto &type = lowered.return_type;
	Vector out(type);
	try {
		executor->SetChunk(chunk);
		executor->ExecuteExpression(expr_idx, out);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (DecideErrorStopsTheQuery(error)) {
			throw;
		}
		if (raised) {
			*raised = error;
		}
		executor = remake();
		Vector rows(type);
		auto row_executor = make_uniq<ExpressionExecutor>(context, lowered);
		for (idx_t i = 0; i < count; i++) {
			SelectionVector at_row(1);
			at_row.set_index(0, i);
			DataChunk row;
			row.InitializeEmpty(chunk.GetTypes());
			row.Slice(chunk, at_row, 1);
			Vector value(type);
			try {
				row_executor->ExecuteExpression(row, value);
				rows.SetValue(i, value.GetValue(0));
			} catch (std::exception &row_ex) {
				ErrorData row_error(row_ex);
				if (DecideErrorStopsTheQuery(row_error)) {
					throw;
				}
				errors.emplace(offset + i, std::move(row_error));
				rows.SetValue(i, Value(type));
				row_executor = make_uniq<ExpressionExecutor>(context, lowered);
			}
		}
		out.Reference(rows);
	}
	VectorOperations::Copy(out, values, count, 0, offset);
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

void CountProductsOnce(const vector<DecideObjective::BilinearTerm> &terms,
                       vector<EvaluatedConstraint::BilinearTerm> &products, const vector<bool> *kept, idx_t num_rows) {
	idx_t first_kept = FirstKeptRow(kept, num_rows);
	vector<idx_t> counted;
	if (first_kept != DConstants::INVALID_INDEX) {
		counted.push_back(first_kept);
	}
	for (idx_t i = 0; i < products.size(); i++) {
		if (terms[i].reduction == LinearTermReduction::NONE) {
			CountOnce(products[i].row_coefficients, counted);
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

void CountTupleTermsOncePerGroup(const vector<DecideTerm> &terms, idx_t num_rows, EvaluatedConstraint &evaluated) {
	auto counted = FirstRowOfEachGroup(evaluated.row_group_ids, evaluated.num_groups, num_rows);
	for (idx_t term_idx = 0; term_idx < terms.size(); term_idx++) {
		if (CountsOnce(terms[term_idx])) {
			CountOnce(evaluated.row_coefficients[term_idx], counted);
		}
	}
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

EvaluatedConstraint InitializeEvaluatedConstraint(const DecideConstraint &constraint, idx_t repair_group_id,
                                                  const std::function<idx_t(const DecideTerm &)> &read_variable) {
	EvaluatedConstraint evaluated;
	evaluated.comparison_type = constraint.comparison_type;
	evaluated.source_clause_id = constraint.source_clause_id;
	evaluated.repair_group_id = repair_group_id;
	evaluated.removal_group_id = constraint.removal_group_id;
	// Preserve whether the original LHS was an aggregate (e.g., SUM(...))
	evaluated.lhs_is_aggregate = constraint.lhs_is_aggregate;
	evaluated.lifted_floor = constraint.lifted_floor;
	evaluated.minmax_clause_idx = constraint.minmax_clause_idx;
	evaluated.minmax_agg_type = constraint.minmax_agg_type;
	evaluated.ne_clause_idx = constraint.ne_clause_idx;
	evaluated.abs_aux_idx = constraint.abs_aux_idx;
	evaluated.abs_is_pos_bound = constraint.abs_is_pos_bound;
	evaluated.kind = constraint.kind;

	// Initialize result storage
	evaluated.row_coefficients.resize(constraint.lhs_terms.size());

	// Store variable indices for all terms (before scanning data). Capture each term's
	// symbolic coefficient label too (`l_extendedprice` for `buy * l_extendedprice`) so
	// infeasible diagnosis can render a data-weighted SUM clause symbolically instead of
	// dumping the per-row numeric fan-out.
	for (auto &term : constraint.lhs_terms) {
		evaluated.variable_indices.push_back(read_variable(term));
		evaluated.coefficient_labels.push_back(term.coefficient ? term.coefficient->GetName() : string());
	}
	return evaluated;
}

bool DescribeBound(const DecideConstraint &constraint, idx_t num_rows, const DecideSourceNames &names,
                   const vector<string> &source_fragments, const vector<KeyedScopeInfo> &keyed_scopes,
                   const vector<DecideKey> &keys, EvaluatedConstraint &evaluated) {
	evaluated.rhs_written = RenderDecideSource(*constraint.rhs_expr, names);
	if (constraint.rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		auto &const_expr = constraint.rhs_expr->Cast<BoundConstantExpression>();
		double rhs_constant = const_expr.value.GetValue<double>();
		evaluated.rhs_values.AssignScalar(num_rows, rhs_constant);
		// A constant RHS is one editable scalar shared across every row this
		// clause emits, so the elastic engine collapses them to one shared slack.
		evaluated.rhs_is_shared_scalar = true;
		return false;
	}
	// A bound of data, read per row. The optimizer's bound-side window rewrite made each reducer on it a
	// column, and the bound of a clause in the aggregate shape its group's tightest value, already checked by
	// the VALUE_IS_VALID scope checks. A row-wise bound is checked here, as it is read.
	// Canonicalization classifies the complete rebuilt bound. This is what
	// keeps forward/reversed subquery spellings and arithmetic over query-wide
	// values equivalent without re-deciding shape in the physical layer.
	evaluated.rhs_is_shared_scalar = IsQueryWideBoundTag(constraint.rhs_expr->GetAlias());
	// Capture the RHS symbolic name (e.g. the data column `cap_col`) so query-mode
	// infeasible diagnosis can render `x <= cap_col + delta`. Only meaningful for a
	// genuine data RHS; a shared-scalar RHS reports the numeric knob instead.
	if (!evaluated.rhs_is_shared_scalar && !ContainsScalarSubqueryProvenance(*constraint.rhs_expr)) {
		evaluated.rhs_label = RenderDecideSource(*constraint.rhs_expr, source_fragments, keyed_scopes, &keys);
	}
	evaluated.rhs_values.Reserve(num_rows);
	return true;
}

vector<idx_t> DecideKeptGroups(const DecideKeyGroups &groups, idx_t num_rows,
                               const std::function<bool(idx_t)> &row_filter, const vector<bool> *read_rows,
                               vector<idx_t> &out_row_group_ids, idx_t &out_num_groups,
                               const std::function<bool(idx_t)> *loose_row_filter,
                               vector<idx_t> *out_loose_row_group_ids) {
	// The call's kept groups, renumbered densely in key-value order.
	out_row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
	vector<idx_t> kept_groups;
	if (out_loose_row_group_ids) {
		out_loose_row_group_ids->assign(num_rows, DConstants::INVALID_INDEX);
	}
	if (groups.num_groups == 0 || num_rows == 0) {
		out_num_groups = 0;
		return kept_groups;
	}
	auto reads = [&](idx_t r) {
		return (!read_rows || (*read_rows)[r]) && groups.row_group_ids[r] != DConstants::INVALID_INDEX;
	};
	vector<idx_t> remap(groups.num_groups, DConstants::INVALID_INDEX);
	for (idx_t r = 0; r < num_rows; r++) {
		if (reads(r) && (!row_filter || row_filter(r))) {
			remap[groups.row_group_ids[r]] = 0;
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
			out_row_group_ids[r] = remap[groups.row_group_ids[r]];
		}
	}
	out_num_groups = next_remap;

	if (out_loose_row_group_ids) {
		// Same `remap`, so a row lands in the group the model builder will emit for.
		for (idx_t r = 0; r < num_rows; r++) {
			if (!reads(r) || (loose_row_filter && *loose_row_filter && !(*loose_row_filter)(r))) {
				continue;
			}
			idx_t mapped = remap[groups.row_group_ids[r]];
			if (mapped == DConstants::INVALID_INDEX) {
				continue; // group emits no constraint
			}
			(*out_loose_row_group_ids)[r] = mapped;
		}
	}
	return kept_groups;
}

vector<idx_t> GroupRowsByShape(const DecideConstraint &constraint, idx_t num_rows, const vector<bool> &scoped_kept,
                               bool has_local_filters, const vector<bool> &local_row_active,
                               const DecideKeyGroups *groups, const vector<bool> &in_a_class,
                               vector<idx_t> *kept_row_group_ids, EvaluatedConstraint &evaluated,
                               idx_t &grouped_instances) {
	// A reducer's members are the rows its own `when` keeps -- a MIN/MAX reads them as members -- while every kept row
	// of a group bounds it. A `<>` emits every kept row, since each one's bound is excluded.
	bool every_kept_row = constraint.comparison_type == ExpressionType::COMPARE_NOTEQUAL;
	std::function<bool(idx_t)> emitted = [&](idx_t row) {
		return scoped_kept[row] && (every_kept_row || !has_local_filters || local_row_active[row]);
	};
	std::function<bool(idx_t)> kept = [&](idx_t row) {
		return scoped_kept[row];
	};
	vector<idx_t> kept_groups;
	if (groups) {
		kept_groups = DecideKeptGroups(*groups, num_rows, emitted, &scoped_kept, evaluated.row_group_ids,
		                               evaluated.num_groups, kept_row_group_ids ? &kept : nullptr, kept_row_group_ids);
		vector<bool> held(groups->num_groups, false);
		grouped_instances = 0;
		for (idx_t row = 0; row < num_rows; row++) {
			const idx_t group = groups->row_group_ids[row];
			if (scoped_kept[row] && group != DConstants::INVALID_INDEX && !held[group]) {
				held[group] = true;
				grouped_instances++;
			}
		}
	} else {
		grouped_instances = std::find(scoped_kept.begin(), scoped_kept.end(), true) != scoped_kept.end();
	}
	if (!groups && (constraint.scope.when || has_local_filters)) {
		evaluated.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
		for (idx_t row = 0; row < num_rows; row++) {
			evaluated.row_group_ids[row] = emitted(row) ? 0 : DConstants::INVALID_INDEX;
		}
		evaluated.num_groups = 1;
	}
	// A `per K:` that names classes holds once per class, and a row in none gets no constraint from the clause
	// (language/guide.md §6.3, D5): a group of the kept rows none of which is in a class holds no instance, and with
	// every kept row's K NULL the clause holds none. With no `by` key the kept rows are one group.
	if (constraint.scope.per == DecideClauseScope::PerRole::NAMES_CLASSES) {
		auto group_of = [&](idx_t row) {
			return groups ? groups->row_group_ids[row] : 0;
		};
		vector<bool> classed(groups ? groups->num_groups : 1, false);
		for (idx_t row = 0; row < num_rows; row++) {
			if (scoped_kept[row] && in_a_class[row] && group_of(row) != DConstants::INVALID_INDEX) {
				classed[group_of(row)] = true;
			}
		}
		grouped_instances = std::count(classed.begin(), classed.end(), true);
		if (evaluated.row_group_ids.empty() && !classed[0]) {
			evaluated.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
			evaluated.num_groups = 0;
		}
		for (auto *ids : {&evaluated.row_group_ids, kept_row_group_ids}) {
			for (idx_t row = 0; ids && row < ids->size(); row++) {
				if ((*ids)[row] != DConstants::INVALID_INDEX && !classed[group_of(row)]) {
					(*ids)[row] = DConstants::INVALID_INDEX;
				}
			}
		}
	}
	return kept_groups;
}

void SkipGroupsWithAnEmptyReducer(const vector<const TermFilterState *> &filters, idx_t num_rows,
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

ComposedMinMaxTermData DecideComposedTerm(const LogicalDecide::ComposedMinMaxTerm &term,
                                          const vector<string> &source_fragments,
                                          const vector<KeyedScopeInfo> &keyed_scopes, const vector<DecideKey> &keys) {
	ComposedMinMaxTermData evaluated;
	evaluated.is_minmax = (term.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND);
	evaluated.agg_name = term.agg_name;
	evaluated.sign = term.sign;
	evaluated.is_easy = term.is_easy;
	if (evaluated.is_minmax) {
		evaluated.label =
		    term.agg_name + "(" + RenderDecideSource(*term.inner_expr, source_fragments, keyed_scopes, &keys) + ")";
	}
	return evaluated;
}

double DecideQueryWideScale(const Value &value, bool divides, const char *what, const ValueSource &source,
                            bool &is_null) {
	if (value.IsNull()) {
		is_null = true;
		return 0.0;
	}
	double v = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	if (!std::isfinite(v)) {
		ThrowInvalidValue(source);
	}
	if (divides) {
		if (v == 0.0) {
			throw InvalidInputException("DECIDE %s: division by zero — the factor on an aggregate "
			                            "evaluated to 0.",
			                            what);
		}
		return 1.0 / v;
	}
	return v;
}

double ScaleSquaredGroup(const Value &value, const DecideConstraint::QuadraticGroup &group, const ValueSource &source,
                         bool &is_null) {
	if (value.IsNull()) {
		is_null = true;
		return group.sign;
	}
	double factor = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	if (!std::isfinite(factor)) {
		ThrowInvalidValue(source);
	}
	if (group.scale_divides && factor == 0.0) {
		throw InvalidInputException(
		    "DECIDE constraint: division by zero -- the factor on a squared aggregate evaluated to 0.");
	}
	double sign = group.sign;
	sign *= group.scale_divides ? 1.0 / factor : factor;
	// The reciprocal of a finite factor is a number DeciDB computes: past a DOUBLE it names the clause.
	if (!std::isfinite(sign)) {
		throw InvalidInputException(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, source.clause));
	}
	return sign;
}

bool DecideComposedTermReadsNoRow(const ComposedMinMaxTermData &term) {
	return std::find(term.filter_mask.begin(), term.filter_mask.end(), true) == term.filter_mask.end();
}

double DecideComposedBound(ClientContext &context, const LogicalDecide::ComposedMinMaxConstraint &spec) {
	if (!spec.rhs_expr->IsFoldable()) {
		throw InternalException("DECIDE: a composed MIN/MAX clause reached execution with a bound that is "
		                        "not a constant: %s",
		                        spec.rhs_expr->ToString());
	}
	return ExpressionExecutor::EvaluateScalar(context, *spec.rhs_expr)
	    .DefaultCastAs(LogicalType::DOUBLE)
	    .GetValue<double>();
}

void ScaleAvgRows(CoefficientColumn &col, bool has_filter, const vector<bool> &filter_mask, bool quadratic_inner,
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

void ScaleAvgTerms(const DecideConstraint &constraint, const vector<TermFilterState> &filters, idx_t num_rows,
                   EvaluatedConstraint &evaluated) {
	bool all_avg = !constraint.lhs_terms.empty() && !constraint.has_bilinear && !constraint.has_quadratic;
	for (idx_t term_idx = 0; term_idx < constraint.lhs_terms.size(); term_idx++) {
		if (filters[term_idx].avg_scale) {
			ScaleAvgRows(evaluated.row_coefficients[term_idx], filters[term_idx].has_filter, filters[term_idx].mask,
			             /*quadratic_inner=*/false, num_rows, evaluated.row_group_ids, evaluated.num_groups);
		} else {
			all_avg = false;
		}
	}
	evaluated.avg_scaled = all_avg;
}

void HoistAvgIntoNotEqualBound(const DecideConstraint &constraint, vector<TermFilterState> &filters,
                               EvaluatedConstraint &evaluated) {
	if (constraint.ne_clause_idx == DConstants::INVALID_INDEX || !constraint.lhs_is_aggregate ||
	    constraint.has_bilinear || constraint.has_quadratic || constraint.lhs_terms.empty()) {
		return;
	}
	for (idx_t term_idx = 0; term_idx < constraint.lhs_terms.size(); term_idx++) {
		if (!filters[term_idx].avg_scale) {
			return;
		}
	}
	evaluated.ne_avg_rhs_scale = true;
	for (idx_t term_idx = 0; term_idx < constraint.lhs_terms.size(); term_idx++) {
		filters[term_idx].avg_scale = false;
	}
}

vector<idx_t> DecideGroupClassSets(const DecideClauseScope &scope, idx_t num_rows, const vector<bool> &kept_rows,
                                   const vector<idx_t> &kept_row_group, idx_t groups,
                                   const std::function<void(vector<idx_t> &, vector<GroupName> &)> &classes,
                                   vector<SolverInput::ClassSet> &class_sets) {
	using Kind = SolverInput::ClassSet::Kind;
	const bool keyed = scope.per != DecideClauseScope::PerRole::NONE;
	vector<idx_t> group_class_set(groups, DConstants::INVALID_INDEX);
	if (keyed && scope.class_key == DConstants::INVALID_INDEX) {
		group_class_set.assign(groups, class_sets.size());
		class_sets.push_back({Kind::WHOLE_CLAUSE, {}, {}});
		return group_class_set;
	}
	const bool one_group = kept_row_group.empty();
	vector<vector<idx_t>> rows_of(groups);
	for (idx_t row = 0; row < num_rows; row++) {
		idx_t group = one_group ? (kept_rows[row] ? idx_t(0) : DConstants::INVALID_INDEX) : kept_row_group[row];
		if (group != DConstants::INVALID_INDEX && group < groups) {
			rows_of[group].push_back(row);
		}
	}
	vector<idx_t> class_ids;
	vector<GroupName> class_names;
	if (keyed) {
		classes(class_ids, class_names);
	}
	// `named_in[c]` is the last group that recorded class c; groups are walked one at a
	// time, so it says whether this group has it already.
	vector<idx_t> named_in(class_names.size(), DConstants::INVALID_INDEX);
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

void DecideOneRowPerVariant(const vector<idx_t> &variant_of, const vector<idx_t> &class_of_row, idx_t num_variants,
                            idx_t num_classes, const vector<vector<bool>> &absent_reads, idx_t defined_var,
                            vector<idx_t> &row_group_ids, vector<idx_t> &class_of, vector<idx_t> &first_rows,
                            vector<idx_t> &variants_per_class) {
	const idx_t num_rows = variant_of.size();
	row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
	class_of.assign(num_variants, DConstants::INVALID_INDEX);
	first_rows.assign(num_variants, DConstants::INVALID_INDEX);
	variants_per_class.assign(num_classes, 0);
	// An auxiliary has no value on a row whose reads have none (D88), so its definition stands on a row
	// where it has one, if any: a row that comes first must not leave it undefined for the rest.
	const auto defines_at_no_value = [&](idx_t row) {
		const auto var = defined_var;
		return var < absent_reads.size() && !absent_reads[var].empty() && absent_reads[var][row];
	};
	for (idx_t row = 0; row < num_rows; row++) {
		auto id = variant_of[row];
		if (id == DConstants::INVALID_INDEX) {
			continue;
		}
		if (class_of[id] == DConstants::INVALID_INDEX) {
			class_of[id] = class_of_row[row];
			variants_per_class[class_of_row[row]]++;
		} else if (defines_at_no_value(first_rows[id]) && !defines_at_no_value(row)) {
			row_group_ids[first_rows[id]] = DConstants::INVALID_INDEX;
		} else {
			continue;
		}
		first_rows[id] = row;
		row_group_ids[row] = id;
	}
}

void DecideNameVariantClassSets(const vector<idx_t> &class_of, const vector<idx_t> &variants_per_class,
                                const vector<GroupName> &group_names, vector<idx_t> &group_class_set,
                                vector<SolverInput::ClassSet> &class_sets) {
	// Only variants named as their own: a clause with more variants than classes (NameVariants).
	for (idx_t id = 0; id < class_of.size() && class_of.size() > variants_per_class.size(); id++) {
		if (variants_per_class[class_of[id]] > 1) {
			group_class_set[id] = class_sets.size();
			class_sets.push_back({SolverInput::ClassSet::Kind::KEY_VALUES, {group_names[id]}, {}});
		}
	}
}

void DecideNameVariants(const vector<DecideSignatureField> &fields, idx_t class_columns,
                        std::function<vector<Value>(idx_t)> read, const vector<idx_t> &class_of,
                        const vector<idx_t> &variants_per_class, const vector<idx_t> &first_rows,
                        SolverInput::KeyValues &key_values) {
	// Variants that differ only by the decision instance they read (`per t.g: x <= t.v` over two rows of one class with
	// one `v`) show the same values: each is named by the row it is read at as well.
	const bool reads_decisions = std::any_of(fields.begin(), fields.end(), [](const DecideSignatureField &field) {
		return field.kind == DecideSignatureFieldKind::DECISION;
	});
	if (reads_decisions) {
		key_values.null_modes.push_back(DecideKeyNullMode::NO_GROUP);
	}
	vector<vector<idx_t>> variants_of_class(variants_per_class.size());
	for (idx_t variant = 0; variant < class_of.size(); variant++) {
		variants_of_class[class_of[variant]].push_back(variant);
	}
	key_values.values_of = [read, class_of, variants_per_class, class_columns, reads_decisions, variants_of_class,
	                        first_rows](idx_t variant) {
		auto values = read(variant);
		if (variants_per_class[class_of[variant]] <= 1) {
			values.resize(class_columns);
			return values;
		}
		auto same = [&](const vector<Value> &other) {
			if (other.size() != values.size()) {
				return false;
			}
			for (idx_t i = 0; i < values.size(); i++) {
				if (!Value::NotDistinctFrom(values[i], other[i])) {
					return false;
				}
			}
			return true;
		};
		for (auto other : variants_of_class[class_of[variant]]) {
			if (reads_decisions && other != variant && same(read(other))) {
				values.push_back(Value(StringUtil::Format("row %llu", first_rows[variant] + 1)));
				break;
			}
		}
		return values;
	};
}

void DecideMapRowsInNoClass(KeyedMapping &mapping) {
	for (auto &id : mapping.row_to_class) {
		if (id == DConstants::INVALID_INDEX) {
			mapping.absent_class = mapping.num_classes;
			id = mapping.absent_class;
		}
	}
	if (mapping.absent_class != DConstants::INVALID_INDEX) {
		mapping.num_classes++;
	}
}

ReducerValueLinkSpec DecideValueLinkSpec(const LogicalDecide::ReducerValueLink &link) {
	ReducerValueLinkSpec spec;
	spec.value_var = link.value_var;
	spec.agg_name = link.agg_name;
	spec.need_envelope = link.need_envelope;
	spec.need_closing = link.need_closing;
	spec.label = link.label;
	spec.frame = link.frame != DConstants::INVALID_INDEX;
	return spec;
}

void DecideListMember(const vector<CoefficientColumn> &columns, const vector<idx_t> &variables, idx_t row, double scale,
                      const string &label, ReducerValueMembers &out) {
	double constant = 0.0;
	for (idx_t t = 0; t < variables.size(); t++) {
		if (variables[t] == DConstants::INVALID_INDEX) {
			constant += columns[t].Get(row) * scale;
		}
	}
	if (!std::isfinite(constant)) {
		throw InvalidInputException(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, label));
	}
	out.AddMember(constant);
	for (idx_t t = 0; t < variables.size(); t++) {
		const double c = columns[t].Get(row) * scale;
		if (variables[t] != DConstants::INVALID_INDEX && c != 0.0) {
			out.AddTerm(variables[t], row, c);
		}
	}
}

void DecideListRowMembers(const EvaluatedConstraint &ec, idx_t num_rows, const string &label,
                          ReducerValueMembers &out) {
	const bool grouped = !ec.row_group_ids.empty();
	const idx_t num_groups = grouped ? ec.num_groups : 1;
	vector<vector<idx_t>> rows_of(num_groups);
	for (idx_t row = 0; row < num_rows; row++) {
		const idx_t group = grouped ? ec.row_group_ids[row] : 0;
		if (group != DConstants::INVALID_INDEX && group < num_groups) {
			rows_of[group].push_back(row);
		}
	}
	for (auto &rows : rows_of) {
		if (rows.empty()) {
			continue;
		}
		out.AddGroup(rows[0]);
		for (auto row : rows) {
			DecideListMember(ec.row_coefficients, ec.variable_indices, row, 1.0, label, out);
		}
	}
}

void DecideCountValueLinks(const vector<ReducerValueLinkSpec> &links) {
	if (DecideProfileScope::Enabled() && !links.empty()) {
		idx_t groups = 0, members = 0, terms = 0;
		for (auto &spec : links) {
			groups += spec.members.GroupCount();
			members += spec.members.constants.size();
			terms += spec.members.variables.size();
		}
		DecideProfileScope::Counter("anr.helpers", static_cast<double>(links.size()));
		DecideProfileScope::Counter("anr.helper_instances", static_cast<double>(groups));
		DecideProfileScope::Counter("anr.members", static_cast<double>(members));
		DecideProfileScope::Counter("anr.member_terms", static_cast<double>(terms));
	}
}

void DecideValueRange::Add(const Value &value) {
	auto before = [](const Value &left, const Value &right) {
		return !left.IsNull() && (right.IsNull() || left < right);
	};
	if (!least || before(value, *least)) {
		least = &value;
	}
	if (!greatest || before(*greatest, value)) {
		greatest = &value;
	}
}

bool DecideValueRange::Spans(string &values) const {
	if (!least || Value::NotDistinctFrom(*least, *greatest)) {
		return false;
	}
	values = DecideSqlLiteral(*least) + ", " + DecideSqlLiteral(*greatest);
	return true;
}

//! Why the kept rows are more than one variant: the first field that takes two values or instances over them.
static string VariantDifference(const vector<DecideSignatureField> &fields, const vector<DecideKey> &keys,
                                const vector<DecideVarScopeInfo> &scopes, const DecideVariantReader &read) {
	for (idx_t f = 0; f < fields.size(); f++) {
		auto &field = fields[f];
		string values;
		switch (field.kind) {
		case DecideSignatureFieldKind::VALUE:
			if (read.values(f, 0, *field.value, values)) {
				return StringUtil::Format("'%s' takes more than one value over the kept rows (%s)", field.text, values);
			}
			break;
		case DecideSignatureFieldKind::BY_KEY: {
			auto &elements = DecideKeyElements(keys, field.key);
			for (idx_t e = 0; e < elements.size(); e++) {
				if (read.values(f, e, *elements[e].expression, values)) {
					return StringUtil::Format("'%s' in %s takes more than one value over the kept rows (%s)",
					                          elements[e].name, field.text, values);
				}
			}
			break;
		}
		case DecideSignatureFieldKind::DECISION:
			if (read.instances(f)) {
				auto &scope = scopes[field.decision];
				return StringUtil::Format(
				    "the decision '%s' (declared per %s) has more than one instance over the kept rows", field.text,
				    scope.HasDeclaredKey() ? scope.declared_key : string("row"));
			}
			break;
		case DecideSignatureFieldKind::FRAME: {
			auto why = read.frame(f);
			if (!why.empty()) {
				return why;
			}
			break;
		}
		}
	}
	throw InternalException("DECIDE: two variants of a signature agree on every field");
}

string DecideObjectiveTwiceMessage(const vector<DecideSignatureField> &fields, const vector<DecideKey> &keys,
                                   const vector<DecideVarScopeInfo> &scopes, const DecideVariantReader &read) {
	return StringUtil::Format(
	    "DECIDE objective: %s, so the objective would be produced more than once; move it inside a reducer or fix it "
	    "with when.",
	    VariantDifference(fields, keys, scopes, read));
}

string DecideFrameDifference(const DecideSignatureField &field, const DecideFrameSpec &frame,
                             const vector<DecideKey> &keys, const DecideFrameReader &read) {
	bool timelines = false;
	bool positions = false;
	idx_t first_peer = DConstants::INVALID_INDEX;
	idx_t last_peer = 0;
	std::unordered_map<idx_t, idx_t> peer_of_timeline;
	for (auto &row : read.rows) {
		auto seen = peer_of_timeline.emplace(row.first, row.second).first;
		positions = positions || seen->second != row.second;
		first_peer = MinValue(first_peer, row.second);
		last_peer = MaxValue(last_peer, row.second);
		timelines = timelines || row.first != read.rows[0].first;
	}
	if (frame.Relative() && positions) {
		const auto names = read.positions(first_peer, last_peer);
		return StringUtil::Format("'%s' reads more than one position over the kept rows (%s = %s, %s)", field.text,
		                          frame.order_name, DecideSqlLiteral(names.first), DecideSqlLiteral(names.second));
	}
	if (timelines) {
		auto &elements = DecideKeyElements(keys, frame.timeline_key);
		string values;
		for (idx_t e = 0; e < elements.size(); e++) {
			if (read.timeline(e, *elements[e].expression, values)) {
				return StringUtil::Format("'%s' reads more than one timeline over the kept rows (%s = %s)", field.text,
				                          elements[e].name, values);
			}
		}
		return StringUtil::Format("'%s' reads more than one timeline over the kept rows", field.text);
	}
	return read.fallbacks()
	           ? StringUtil::Format("'%s' falls back to more than one value over the kept rows", field.text)
	           : string();
}

void TakeTightestFrameBound(ExpressionType comparison, const string &written, const vector<bool> &kept,
                            const vector<bool> &bound_null, const vector<idx_t> &kept_groups,
                            const vector<idx_t> &instances, idx_t num_groups, bool grouped, vector<double> &rhs,
                            vector<bool> &null_rows) {
	const bool one_group = kept_groups.empty();
	const idx_t groups = one_group ? 1 : num_groups;
	vector<bool> held(groups, instances.empty());
	for (auto group : instances) {
		if (group != DConstants::INVALID_INDEX) {
			held[one_group ? 0 : group] = true;
		}
	}
	for (idx_t row = 0; grouped && row < null_rows.size(); row++) {
		const idx_t instance = instances.empty() ? 0 : instances[row];
		if (null_rows[row] && instance != DConstants::INVALID_INDEX) {
			held[one_group ? 0 : instance] = false;
		}
	}
	auto group_of = [&](idx_t row) {
		const idx_t group = one_group ? 0 : kept_groups[row];
		return group != DConstants::INVALID_INDEX && held[group] ? group : DConstants::INVALID_INDEX;
	};
	vector<double> least(groups, std::numeric_limits<double>::infinity());
	vector<double> greatest(groups, -std::numeric_limits<double>::infinity());
	vector<bool> has_value(groups, false);
	for (idx_t row = 0; row < kept.size(); row++) {
		const idx_t group = group_of(row);
		if (!kept[row] || bound_null[row] || group == DConstants::INVALID_INDEX) {
			continue;
		}
		least[group] = MinValue(least[group], rhs[row]);
		greatest[group] = MaxValue(greatest[group], rhs[row]);
		has_value[group] = true;
	}
	const bool upper = comparison == ExpressionType::COMPARE_LESSTHAN ||
	                   comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	                   comparison == ExpressionType::COMPARE_EQUAL;
	for (idx_t row = 0; row < kept.size(); row++) {
		const idx_t group = group_of(row);
		if (!kept[row] || group == DConstants::INVALID_INDEX) {
			continue;
		}
		if (!has_value[group]) {
			null_rows[row] = true;
			continue;
		}
		if (comparison == ExpressionType::COMPARE_EQUAL && least[group] != greatest[group]) {
			throw InvalidInputException("`%s` takes more than one value here (%g and %g) — an `=` bound cannot hold "
			                            "two different values at once, so this DECIDE query is infeasible.",
			                            written, least[group], greatest[group]);
		}
		rhs[row] = upper ? least[group] : greatest[group];
	}
}

} // namespace duckdb
