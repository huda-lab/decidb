//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_value_extraction.cpp
//
// How stage 08 computes a value over the input's rows and refuses one the model cannot read
// (decide_value_extraction.hpp).
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/planner/decide/decide_tree_nodes.hpp"

#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/rule/constant_folding.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
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
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER ||
	    expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_FRAME) {
		// The operator evaluates row values only. A reducer of data is a column by now (the optimizer's bound-side
		// window rewrite); one of a decision is a set of terms, never evaluated.
		throw InternalException("DECIDE: a reducer reached row evaluation: %s", expr.ToString());
	}
	// A copy, nothing re-bound: each node keeps what DuckDB bound and where the user wrote it, which a DuckDB error
	// raised on it points at. A leaf keeps its type, so its parents stay bound as they were.
	auto copy = expr.Copy();
	ExpressionIterator::EnumerateChildren(
	    *copy, [&](unique_ptr<Expression> &child) { child = ToChunkReferences(*child, context, leaf); });
	return copy;
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

template <class T>
static void CollectNan(Vector &values, idx_t count, vector<idx_t> &entries) {
	UnifiedVectorFormat format;
	values.ToUnifiedFormat(count, format);
	auto data = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t i = 0; i < count; i++) {
		const auto index = format.sel->get_index(i);
		if (format.validity.RowIsValid(index) && std::isnan(data[index])) {
			entries.push_back(i);
		}
	}
}

vector<idx_t> DecideNanEntries(Vector &values, idx_t count) {
	vector<idx_t> entries;
	switch (values.GetType().InternalType()) {
	case PhysicalType::DOUBLE:
		CollectNan<double>(values, count, entries);
		break;
	case PhysicalType::FLOAT:
		CollectNan<float>(values, count, entries);
		break;
	default:
		break;
	}
	return entries;
}

Value DecideNanValue(const LogicalType &type) {
	const double nan = std::numeric_limits<double>::quiet_NaN();
	return type.id() == LogicalTypeId::FLOAT ? Value::FLOAT(float(nan)) : Value::DOUBLE(nan);
}

void ThrowInvalidValue(const ValueSource &source) {
	// What a NULL guard protects stands in the query, as the render quotes it.
	auto &value = DecideUnguarded(source.value);
	// A coefficient that reads no column is a constant: it fails on every row, so no `when` skips it.
	auto role = source.role;
	if (role == DecideValueRole::COEFFICIENT && value.IsFoldable()) {
		role = DecideValueRole::CONSTANT;
	}
	throw DecideInvalidValueError(role, RenderDecideSource(source.value, source.names),
	                              DecideSpanLocation(source.names.text, value.GetQueryLocation()));
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
			ThrowInvalidValue(source);
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

void CountProductsOnce(const vector<DecideProductGroup> &groups, vector<EvaluatedProductGroup> &evaluated,
                       const vector<bool> *kept, idx_t num_rows) {
	idx_t first_kept = FirstKeptRow(kept, num_rows);
	vector<idx_t> counted;
	if (first_kept != DConstants::INVALID_INDEX) {
		counted.push_back(first_kept);
	}
	for (idx_t i = 0; i < evaluated.size(); i++) {
		if (groups[i].reduction == LinearTermReduction::NONE) {
			CountOnce(evaluated[i].weight, counted);
		}
	}
}

vector<idx_t> FirstRowOfEachGroup(const vector<idx_t> &row_group_ids, idx_t num_groups) {
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

void CountTupleTermsOncePerGroup(const vector<DecideTerm> &terms, EvaluatedClauseColumns &evaluated) {
	auto counted = FirstRowOfEachGroup(evaluated.row_group_ids, evaluated.num_groups);
	for (idx_t term_idx = 0; term_idx < terms.size(); term_idx++) {
		if (CountsOnce(terms[term_idx])) {
			CountOnce(evaluated.row_coefficients[term_idx], counted);
		}
	}
}

idx_t CountInstances(const vector<idx_t> &row_group_ids, idx_t num_groups, idx_t num_rows, bool grouped) {
	if (row_group_ids.empty()) {
		return num_rows;
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

ClauseRowSets DecideClauseRowSets(const EvaluatedClauseColumns &evaluated, idx_t num_rows) {
	ClauseRowSets sets;
	auto &groups = evaluated.row_group_ids;
	if (!evaluated.lhs_is_aggregate) {
		for (idx_t row = 0; row < num_rows; row++) {
			if (!groups.empty() && groups[row] == DConstants::INVALID_INDEX) {
				continue;
			}
			sets.rows.push_back(row);
			sets.begin.push_back(sets.rows.size());
			sets.groups.push_back(groups.empty() ? DConstants::INVALID_INDEX : groups[row]);
		}
		return sets;
	}
	// A counting sort of the kept rows by group, then the groups that hold a row.
	vector<idx_t> offsets(evaluated.num_groups + 1, 0);
	for (auto group : groups) {
		if (group != DConstants::INVALID_INDEX) {
			offsets[group + 1]++;
		}
	}
	for (idx_t group = 0; group < evaluated.num_groups; group++) {
		offsets[group + 1] += offsets[group];
	}
	sets.rows.resize(offsets[evaluated.num_groups]);
	auto cursor = offsets;
	for (idx_t row = 0; row < groups.size(); row++) {
		if (groups[row] != DConstants::INVALID_INDEX) {
			sets.rows[cursor[groups[row]]++] = row;
		}
	}
	for (idx_t group = 0; group < evaluated.num_groups; group++) {
		if (offsets[group + 1] > offsets[group]) {
			sets.begin.push_back(offsets[group + 1]);
			sets.groups.push_back(group);
		}
	}
	return sets;
}

void DecideWriteClauseRecords(EvaluatedClauseColumns &evaluated, const ClauseRowSets &sets, const vector<idx_t> &rows,
                              const DecideClauseRef &written) {
	auto &records = evaluated.records;
	const idx_t count = sets.groups.size();
	const bool admits = evaluated.admitted_lower.logical_size != 0;
	records.groups = sets.groups;
	records.constants.assign(count, 0.0);
	records.bounds.reserve(count);
	records.data_rows.reserve(count);
	records.term_begin.reserve(count + 1);
	for (idx_t e = 0; e < count; e++) {
		const idx_t begin = sets.begin[e];
		const idx_t end = sets.begin[e + 1];
		for (idx_t t = 0; t < evaluated.variable_indices.size(); t++) {
			auto &column = evaluated.row_coefficients[t];
			const idx_t variable = evaluated.variable_indices[t];
			for (idx_t k = begin; k < end; k++) {
				const double coefficient = column.Get(sets.rows[k]);
				if (variable == DConstants::INVALID_INDEX) {
					AddCoefficient(records.constants[e], coefficient, written);
				} else if (coefficient != 0.0) {
					records.variables.push_back(variable);
					records.rows.push_back(rows[sets.rows[k]]);
					records.coefficients.push_back(coefficient);
				}
			}
		}
		records.term_begin.push_back(records.variables.size());
		// A group's bound is one value on its rows (RewriteRhsAggregates), read on its first.
		const idx_t first = sets.rows[begin];
#ifdef DEBUG
		for (idx_t k = begin; evaluated.comparison_type != ExpressionType::COMPARE_NOTEQUAL && k < end; k++) {
			D_ASSERT(evaluated.rhs_values.Get(sets.rows[k]) == evaluated.rhs_values.Get(first));
		}
#endif
		records.data_rows.push_back(rows[first]);
		records.bounds.push_back(evaluated.rhs_values.Get(first));
		if (admits) {
			records.admitted_lower.push_back(evaluated.admitted_lower.Get(first));
			records.admitted_upper.push_back(evaluated.admitted_upper.Get(first));
		}
	}
}

EvaluatedClauseColumns InitializeEvaluatedConstraint(const DecideConstraint &constraint, idx_t repair_group_id,
                                                     const std::function<idx_t(const DecideTerm &)> &read_variable) {
	EvaluatedClauseColumns evaluated;
	evaluated.comparison_type = constraint.comparison_type;
	evaluated.provenance.origin = constraint.origin;
	evaluated.provenance.origin.repair_group_id = repair_group_id;
	// Preserve whether the original LHS was an aggregate (e.g., SUM(...))
	evaluated.lhs_is_aggregate = constraint.lhs_is_aggregate;
	evaluated.defined_var = constraint.defined_var;
	evaluated.lifted_floor = constraint.lifted_floor;
	evaluated.provenance.kind = constraint.kind;

	// Initialize result storage
	evaluated.row_coefficients.resize(constraint.lhs_terms.size());

	// Store variable indices for all terms (before scanning data).
	for (auto &term : constraint.lhs_terms) {
		evaluated.variable_indices.push_back(read_variable(term));
	}
	return evaluated;
}

bool DescribeBound(const DecideConstraint &constraint, idx_t num_rows, bool diagnose,
                   const DecideSourceText &source_text, const vector<ConstraintSourceInfo> &constraint_sources,
                   const vector<KeyedScopeInfo> &keyed_scopes, const vector<DecideKey> &keys,
                   EvaluatedClauseColumns &evaluated) {
	if (constraint.rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		auto &const_expr = constraint.rhs_expr->Cast<BoundConstantExpression>();
		double rhs_constant = const_expr.value.GetValue<double>();
		evaluated.rhs_values.AssignScalar(num_rows, rhs_constant);
		// A constant RHS is one editable scalar shared across every row this
		// clause emits, so the elastic engine collapses them to one shared slack.
		// A constant canonicalization left of a written bound that reads data
		// (`x <= cap * o` is `x - cap * o <= 0`) repairs as that bound does.
		const bool shared = constraint.origin.source_clause_id >= constraint_sources.size() ||
		                    !constraint_sources[constraint.origin.source_clause_id].source_rhs_reads_data;
		evaluated.provenance.shape = shared ? ElasticShape::SHARED_SCALAR : ElasticShape::PER_ROW_DATA;
		return false;
	}
	// A bound of data, read per row. The optimizer's bound-side window rewrite made each reducer on it a
	// column, and the bound of a clause in the aggregate shape its group's tightest value, already checked by
	// the VALUE_IS_VALID scope checks. A row-wise bound is checked here, as it is read.
	// Canonicalization classifies the complete rebuilt bound. This is what
	// keeps forward/reversed subquery spellings and arithmetic over query-wide
	// values equivalent without re-deciding shape in the physical layer.
	evaluated.provenance.shape = constraint.query_wide_bound ? ElasticShape::SHARED_SCALAR : ElasticShape::PER_ROW_DATA;
	// Under DIAGNOSE, capture the RHS symbolic name (e.g. the data column `cap_col`) so query-mode
	// infeasible diagnosis can render `x <= cap_col + delta`. Only meaningful for a
	// genuine data RHS; a shared-scalar RHS reports the numeric knob instead.
	if (diagnose && evaluated.provenance.shape != ElasticShape::SHARED_SCALAR && !constraint.bound_holds_subquery) {
		evaluated.provenance.rhs_label = RenderDecideSource(*constraint.rhs_expr, source_text, keyed_scopes, &keys);
	}
	evaluated.rhs_values.Reserve(num_rows);
	return true;
}

vector<idx_t> DecideKeptGroups(const DecideKeyGroups &groups, idx_t num_rows,
                               const std::function<bool(idx_t)> &row_filter, vector<idx_t> &out_row_group_ids,
                               idx_t &out_num_groups, vector<idx_t> *out_every_row_group_ids) {
	// The call's kept groups, renumbered densely in key-value order.
	out_row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
	vector<idx_t> kept_groups;
	if (out_every_row_group_ids) {
		out_every_row_group_ids->assign(num_rows, DConstants::INVALID_INDEX);
	}
	if (groups.num_groups == 0 || num_rows == 0) {
		out_num_groups = 0;
		return kept_groups;
	}
	auto reads = [&](idx_t r) {
		return groups.row_group_ids[r] != DConstants::INVALID_INDEX;
	};
	vector<idx_t> remap(groups.num_groups, DConstants::INVALID_INDEX);
	for (idx_t r = 0; r < num_rows; r++) {
		if (reads(r) && row_filter(r)) {
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
		if (reads(r) && row_filter(r)) {
			out_row_group_ids[r] = remap[groups.row_group_ids[r]];
		}
	}
	out_num_groups = next_remap;

	// Same `remap`, so a row lands in the group the model builder will emit for.
	for (idx_t r = 0; out_every_row_group_ids && r < num_rows; r++) {
		if (reads(r)) {
			(*out_every_row_group_ids)[r] = remap[groups.row_group_ids[r]];
		}
	}
	return kept_groups;
}

vector<idx_t> GroupRowsByShape(const DecideConstraint &constraint, idx_t num_rows, bool has_local_filters,
                               const vector<bool> &local_row_active, const DecideKeyGroups *groups,
                               const vector<idx_t> &class_of_row, vector<idx_t> *kept_row_group_ids,
                               EvaluatedClauseColumns &evaluated, idx_t &grouped_instances) {
	// A reducer's members are the rows its own `when` keeps -- a MIN/MAX reads them as members -- while every kept row
	// of a group bounds it. A `<>` emits every kept row, since each one's bound is excluded.
	bool every_kept_row = constraint.comparison_type == ExpressionType::COMPARE_NOTEQUAL;
	std::function<bool(idx_t)> emitted = [&](idx_t row) {
		return every_kept_row || !has_local_filters || local_row_active[row];
	};
	vector<idx_t> kept_groups;
	if (groups) {
		kept_groups = DecideKeptGroups(*groups, num_rows, emitted, evaluated.row_group_ids, evaluated.num_groups,
		                               kept_row_group_ids);
		vector<bool> held(groups->num_groups, false);
		grouped_instances = 0;
		for (idx_t row = 0; row < num_rows; row++) {
			const idx_t group = groups->row_group_ids[row];
			if (group != DConstants::INVALID_INDEX && !held[group]) {
				held[group] = true;
				grouped_instances++;
			}
		}
	} else {
		grouped_instances = num_rows > 0;
	}
	if (!groups) {
		// With no `by` key the rows that emit are one group.
		evaluated.row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
		for (idx_t row = 0; row < num_rows; row++) {
			evaluated.row_group_ids[row] = emitted(row) ? 0 : DConstants::INVALID_INDEX;
		}
		evaluated.num_groups = 1;
	}
	// A `per K:` that names classes holds once per class, and a row in none gets no constraint from the clause
	// (language/guide.md §6.3, D5): a group of the kept rows none of which is in a class holds no instance, and with
	// every kept row's K NULL the clause holds none. With no `by` key the kept rows are one group. A class none of
	// whose rows is in a group (their `by` key is NULL) still holds one instance, which has no value.
	if (constraint.scope.per == DecideClauseScope::PerRole::NAMES_CLASSES) {
		auto group_of = [&](idx_t row) {
			return groups ? groups->row_group_ids[row] : 0;
		};
		vector<bool> classed(groups ? groups->num_groups : 1, false);
		std::set<idx_t> classes, reaching_a_group;
		for (idx_t row = 0; row < num_rows; row++) {
			if (class_of_row[row] == DConstants::INVALID_INDEX) {
				continue;
			}
			classes.insert(class_of_row[row]);
			if (group_of(row) != DConstants::INVALID_INDEX) {
				classed[group_of(row)] = true;
				reaching_a_group.insert(class_of_row[row]);
			}
		}
		grouped_instances = NumericCast<idx_t>(std::count(classed.begin(), classed.end(), true)) + classes.size() -
		                    reaching_a_group.size();
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

void ScaleAvgRows(CoefficientColumn &col, bool has_filter, const vector<bool> &filter_mask, idx_t num_rows,
                  const vector<idx_t> &row_group_ids, idx_t num_groups) {
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
		double scale = 1.0 / static_cast<double>(denominator);
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
		double scale = 1.0 / static_cast<double>(group_counts[gid]);
		coefficients[row] *= scale;
	}
}

void ScaleAvgTerms(const DecideConstraint &constraint, const vector<TermFilterState> &filters, idx_t num_rows,
                   EvaluatedClauseColumns &evaluated) {
	for (idx_t term_idx = 0; term_idx < constraint.lhs_terms.size(); term_idx++) {
		if (filters[term_idx].avg_scale) {
			ScaleAvgRows(evaluated.row_coefficients[term_idx], filters[term_idx].has_filter, filters[term_idx].mask,
			             num_rows, evaluated.row_group_ids, evaluated.num_groups);
		}
	}
}

bool HoistAvgIntoNotEqualBound(const DecideConstraint &constraint, vector<TermFilterState> &filters) {
	if (constraint.comparison_type != ExpressionType::COMPARE_NOTEQUAL || !constraint.lhs_is_aggregate ||
	    constraint.lhs_terms.empty()) {
		return false;
	}
	for (idx_t term_idx = 0; term_idx < constraint.lhs_terms.size(); term_idx++) {
		if (!filters[term_idx].avg_scale) {
			return false;
		}
	}
	for (idx_t term_idx = 0; term_idx < constraint.lhs_terms.size(); term_idx++) {
		filters[term_idx].avg_scale = false;
	}
	return true;
}

vector<idx_t> DecideGroupClassSets(const DecideClauseScope &scope, const vector<idx_t> &rows,
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
	for (idx_t row = 0; row < rows.size(); row++) {
		idx_t group = one_group ? idx_t(0) : kept_row_group[row];
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
			for (auto row : rows_of[group]) {
				set.rows.push_back(rows[row]);
			}
		}
		group_class_set[group] = class_sets.size();
		class_sets.push_back(std::move(set));
	}
	return group_class_set;
}

void DecideOneRowPerVariant(const vector<idx_t> &variant_of, const vector<idx_t> &class_of_row, idx_t num_variants,
                            idx_t num_classes, const std::function<bool(idx_t)> &defines_at_no_value,
                            vector<idx_t> &row_group_ids, vector<idx_t> &class_of, vector<idx_t> &first_rows,
                            vector<idx_t> &variants_per_class) {
	const idx_t num_rows = variant_of.size();
	row_group_ids.assign(num_rows, DConstants::INVALID_INDEX);
	class_of.assign(num_variants, DConstants::INVALID_INDEX);
	first_rows.assign(num_variants, DConstants::INVALID_INDEX);
	variants_per_class.assign(num_classes, 0);
	// An auxiliary has no value on a row whose reads have none (D88, `defines_at_no_value`), so its definition stands
	// on a row where it has one, if any: a row that comes first must not leave it undefined for the rest.
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
		key_values.names.push_back("row");
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
				values.push_back(Value::UBIGINT(first_rows[variant] + 1));
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

void DecideListMember(const vector<CoefficientColumn> &columns, const vector<idx_t> &variables, idx_t row, double scale,
                      const DecideClauseRef &clause, ReducerValueMembers &out) {
	double constant = 0.0;
	for (idx_t t = 0; t < variables.size(); t++) {
		if (variables[t] == DConstants::INVALID_INDEX) {
			constant += columns[t].Get(row) * scale;
		}
	}
	if (!std::isfinite(constant)) {
		throw DecideTooLargeError(clause);
	}
	out.AddMember(constant);
	for (idx_t t = 0; t < variables.size(); t++) {
		const double c = columns[t].Get(row) * scale;
		if (variables[t] != DConstants::INVALID_INDEX && c != 0.0) {
			out.AddTerm(variables[t], row, c);
		}
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

//! Why the kept rows are more than one variant: the first field that takes two values or instances over them, and
//! where the query writes it: the field's own expression, the key element, the decision's declaration or the frame.
static DecideVariantDifference VariantDifference(const vector<DecideSignatureField> &fields,
                                                 const vector<DecideKey> &keys,
                                                 const vector<DecideVarScopeInfo> &scopes,
                                                 const vector<DecideFrameSpec> &frames, const DecideSourceText &text,
                                                 const DecideVariantReader &read) {
	for (idx_t f = 0; f < fields.size(); f++) {
		auto &field = fields[f];
		string values;
		switch (field.kind) {
		case DecideSignatureFieldKind::VALUE:
			if (read.values(f, 0, *field.value, values)) {
				return {
				    StringUtil::Format("'%s' takes more than one value over the kept rows (%s)", field.text, values),
				    DecideSpanLocation(text, field.value->GetQueryLocation())};
			}
			break;
		case DecideSignatureFieldKind::BY_KEY: {
			auto &elements = DecideKeyElements(keys, field.key);
			for (idx_t e = 0; e < elements.size(); e++) {
				if (read.values(f, e, *elements[e].expression, values)) {
					return {StringUtil::Format("'%s' in %s takes more than one value over the kept rows (%s)",
					                           elements[e].name, field.text, values),
					        DecideSpanLocation(text, elements[e].expression->GetQueryLocation())};
				}
			}
			break;
		}
		case DecideSignatureFieldKind::DECISION:
			if (read.instances(f)) {
				auto &scope = scopes[field.decision];
				return {StringUtil::Format(
				            "the decision '%s' (declared per %s) has more than one instance over the kept rows",
				            field.text, scope.HasDeclaredKey() ? scope.declared_key : string("row")),
				        scope.declaration};
			}
			break;
		case DecideSignatureFieldKind::FRAME: {
			auto why = read.frame(f);
			if (!why.empty()) {
				return {why, frames[field.frame].location};
			}
			break;
		}
		}
	}
	throw InternalException("DECIDE: two variants of a signature agree on every field");
}

DecideVariantDifference DecideObjectiveTwice(const vector<DecideSignatureField> &fields, const vector<DecideKey> &keys,
                                             const vector<DecideVarScopeInfo> &scopes,
                                             const vector<DecideFrameSpec> &frames, const DecideSourceText &text,
                                             const DecideVariantReader &read) {
	auto difference = VariantDifference(fields, keys, scopes, frames, text, read);
	difference.message += ", so the objective would be produced more than once";
	return difference;
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

void TakeTightestFrameBound(ExpressionType comparison, const string &written, optional_idx place,
                            const vector<bool> &bound_null, const vector<idx_t> &kept_groups,
                            const vector<idx_t> &instances, idx_t num_groups, const vector<GroupName> &group_names,
                            const vector<SolverInput::KeyValues> &key_values, vector<double> &rhs,
                            vector<bool> &null_rows) {
	const bool one_group = kept_groups.empty();
	const idx_t groups = one_group ? 1 : num_groups;
	vector<bool> held(groups, instances.empty());
	for (auto group : instances) {
		if (group != DConstants::INVALID_INDEX) {
			held[one_group ? 0 : group] = true;
		}
	}
	for (idx_t row = 0; row < null_rows.size(); row++) {
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
	for (idx_t row = 0; row < bound_null.size(); row++) {
		const idx_t group = group_of(row);
		if (bound_null[row] || group == DConstants::INVALID_INDEX) {
			continue;
		}
		least[group] = MinValue(least[group], rhs[row]);
		greatest[group] = MaxValue(greatest[group], rhs[row]);
		has_value[group] = true;
	}
	const bool upper = comparison == ExpressionType::COMPARE_LESSTHAN ||
	                   comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	                   comparison == ExpressionType::COMPARE_EQUAL;
	// An `=` bound holds one value in each group: a `per K:` makes one instance per class and the groups merge them.
	auto disagreement = [&](idx_t group, double low, double high) {
		vector<string> names;
		vector<Value> values;
		if (group < group_names.size() && group_names[group].key_values != DConstants::INVALID_INDEX) {
			auto &key = key_values[group_names[group].key_values];
			auto read = key.values_of(group_names[group].group);
			for (idx_t i = 0; i < read.size(); i++) {
				if (std::find(names.begin(), names.end(), key.names[i]) == names.end()) {
					names.push_back(key.names[i]);
					values.push_back(read[i]);
				}
			}
		}
		auto message = DecideBoundDisagreementMessage(written, low, high, names, values);
		if (!place.IsValid()) {
			throw InvalidInputException(message);
		}
		throw InvalidInputException(message, Exception::InitializeExtraInfo(place));
	};
	for (idx_t row = 0; row < bound_null.size(); row++) {
		const idx_t group = group_of(row);
		if (group == DConstants::INVALID_INDEX) {
			continue;
		}
		if (!has_value[group]) {
			null_rows[row] = true;
			continue;
		}
		if (comparison == ExpressionType::COMPARE_EQUAL && least[group] != greatest[group]) {
			disagreement(group, least[group], greatest[group]);
		}
		rhs[row] = upper ? least[group] : greatest[group];
	}
}

} // namespace duckdb
