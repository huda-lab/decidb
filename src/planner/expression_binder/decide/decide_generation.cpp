#include "duckdb/planner/expression_binder/decide/decide_generation.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/planner/bind_context.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/table_binding.hpp"

namespace duckdb {

string DescribeGenerationScope(DecideGenerationScope scope, const DecideQualifierContext &ctx) {
	switch (scope.kind) {
	case DecideScopeKind::ROW:
		return "row";
	case DecideScopeKind::GLOBAL:
		return "()";
	case DecideScopeKind::KEY:
		if (ctx.entity_scopes && scope.scope_idx < ctx.entity_scopes->size()) {
			return (*ctx.entity_scopes)[scope.scope_idx].table_alias;
		}
		return "?";
	}
	return "?";
}

namespace {

//! The columns a key stands for, as a set, plus the relations it names wholly.
struct KeyColumns {
	set<pair<idx_t, idx_t>> columns;
	set<idx_t> whole_relations;

	bool Contains(const ColumnBinding &binding) const {
		return columns.count(make_pair(binding.table_index, binding.column_index)) > 0;
	}
	bool ContainsAll(const KeyColumns &other) const {
		for (auto &column : other.columns) {
			if (!columns.count(column)) {
				return false;
			}
		}
		return true;
	}
};

KeyColumns CollectKeyColumns(const EntityScopeInfo &scope) {
	KeyColumns result;
	for (auto &binding : scope.entity_key_bindings) {
		result.columns.insert(make_pair(binding.table_index, binding.column_index));
	}
	for (auto relation : scope.source_table_indices) {
		result.whole_relations.insert(relation);
	}
	return result;
}

//! Whether a base table's PRIMARY KEY or a UNIQUE constraint lies inside `key`, which
//! makes every column of that table a function of the key.
bool TableKeyInsideKey(const Binding &binding, const KeyColumns &key) {
	if (binding.binding_type != BindingType::TABLE) {
		return false;
	}
	auto &table_binding = const_cast<Binding &>(binding).Cast<TableBinding>();
	auto entry = table_binding.GetStandardEntry();
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return false;
	}
	auto &table = entry->Cast<TableCatalogEntry>();
	for (auto &constraint : table.GetConstraints()) {
		if (constraint->type != ConstraintType::UNIQUE) {
			continue;
		}
		auto &unique = constraint->Cast<UniqueConstraint>();
		vector<string> names;
		if (unique.HasIndex()) {
			names.push_back(table.GetColumn(unique.GetIndex()).Name());
		} else {
			names = unique.GetColumnNames();
		}
		bool all_in_key = !names.empty();
		for (auto &name : names) {
			column_t col_idx;
			if (!table_binding.TryGetBindingIndex(name, col_idx) ||
			    !key.Contains(table_binding.GetColumnBinding(col_idx))) {
				all_in_key = false;
				break;
			}
		}
		if (all_in_key) {
			return true;
		}
	}
	return false;
}

const Binding *FindBinding(BindContext &bind_context, idx_t table_index) {
	for (auto &binding : bind_context.GetBindingsList()) {
		if (binding->index == table_index) {
			return binding.get();
		}
	}
	return nullptr;
}

struct Prover {
	DecideGenerationScope scope;
	const DecideQualifierContext &ctx;
	BindContext &bind_context;
	KeyColumns key;
	string scope_text;

	Prover(DecideGenerationScope scope_p, const DecideQualifierContext &ctx_p, BindContext &bind_context_p)
	    : scope(scope_p), ctx(ctx_p), bind_context(bind_context_p) {
		if (scope.kind == DecideScopeKind::KEY) {
			key = CollectKeyColumns((*ctx.entity_scopes)[scope.scope_idx]);
		}
		scope_text = DescribeGenerationScope(scope, ctx);
	}

	string Repair(const string &what) const {
		if (scope.kind == DecideScopeKind::GLOBAL) {
			return StringUtil::Format(
			    "%s varies across rows, but PER () generates one instance for the whole query; "
			    "reduce it (sum, avg, min or max) or generate PER a key that determines it",
			    what);
		}
		return StringUtil::Format(
		    "%s is not determined by the generation key PER %s: add it to the key, name its relation in "
		    "the key, declare a PRIMARY KEY the key covers, or reduce it with BY (%s)",
		    what, scope_text, scope_text);
	}

	//! A data column is determined by the key when it is in the key, its relation is
	//! wholly in the key, or a table key of its relation is inside the key.
	bool DataColumnDetermined(const BoundColumnRefExpression &colref) const {
		if (scope.kind == DecideScopeKind::ROW) {
			return true;
		}
		if (scope.kind == DecideScopeKind::GLOBAL) {
			return false;
		}
		if (key.Contains(colref.binding) || key.whole_relations.count(colref.binding.table_index)) {
			return true;
		}
		auto binding = FindBinding(bind_context, colref.binding.table_index);
		return binding && TableKeyInsideKey(*binding, key);
	}

	//! A decision is determined when its own generation key lies inside this key.
	bool DecisionDetermined(const BoundColumnRefExpression &colref) const {
		if (scope.kind == DecideScopeKind::ROW) {
			return true;
		}
		idx_t var_idx = colref.binding.column_index;
		auto &scopes = *ctx.variable_scopes;
		if (var_idx >= scopes.size()) {
			return scope.kind == DecideScopeKind::ROW;
		}
		auto &var_scope = scopes[var_idx];
		if (var_scope.IsScalar()) {
			return true;
		}
		if (!var_scope.IsEntity()) {
			return false; // row-scoped: only per-row generation reads it directly
		}
		if (scope.kind == DecideScopeKind::GLOBAL) {
			return false;
		}
		auto var_key = CollectKeyColumns((*ctx.entity_scopes)[var_scope.entity_scope_idx]);
		return key.ContainsAll(var_key);
	}

	//! A keyed scope (a reducer's BY, a frame's WITHIN) is determined when each of its
	//! columns is.
	bool KeyDetermined(idx_t other_scope_idx) const {
		if (scope.kind == DecideScopeKind::ROW) {
			return true;
		}
		if (scope.kind == DecideScopeKind::GLOBAL) {
			return false;
		}
		auto other = CollectKeyColumns((*ctx.entity_scopes)[other_scope_idx]);
		if (key.ContainsAll(other)) {
			return true;
		}
		// Columns not literally in the key may still be determined through a whole
		// relation or a table key.
		for (auto &column : other.columns) {
			if (key.columns.count(column) || key.whole_relations.count(column.first)) {
				continue;
			}
			auto binding = FindBinding(bind_context, column.first);
			if (!binding || !TableKeyInsideKey(*binding, key)) {
				return false;
			}
		}
		return true;
	}

	string Check(const Expression &expr) const {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_CONSTANT:
		case ExpressionClass::BOUND_PARAMETER:
			return "";
		case ExpressionClass::BOUND_COLUMN_REF: {
			auto &colref = expr.Cast<BoundColumnRefExpression>();
			if (colref.binding.table_index == ctx.decide_index) {
				return DecisionDetermined(colref) ? "" : Repair("decision '" + colref.GetName() + "'");
			}
			return DataColumnDetermined(colref) ? "" : Repair("column '" + StripDecideTags(colref.GetName()) + "'");
		}
		case ExpressionClass::BOUND_AGGREGATE: {
			// A reducer's value is a function of its group, which the key must identify.
			// Its body is judged by the reducer's own generation, so the walk stops here;
			// its filter reads rows, not the instance, and needs no proof either.
			auto &agg = expr.Cast<BoundAggregateExpression>();
			idx_t by_scope;
			if (TryParseReduceByTag(agg.GetAlias(), by_scope) && !KeyDetermined(by_scope)) {
				return Repair(StringUtil::Format("the BY (%s) group of %s(...)",
				                                 (*ctx.entity_scopes)[by_scope].table_alias,
				                                 StringUtil::Upper(agg.function.name)));
			}
			// A frame navigates from the instance's own position: its partition and its
			// place on the order key must both be one value per instance (spec §7.3).
			idx_t frame_idx;
			if (TryParseFrameRefTag(agg.GetAlias(), frame_idx) && ctx.frames && frame_idx < ctx.frames->size()) {
				auto &frame = (*ctx.frames)[frame_idx];
				if (scope.kind == DecideScopeKind::GLOBAL) {
					return Repair("the position of a frame (AT / FROM .. TO .. OVER)");
				}
				if (frame.within_scope_idx != DConstants::INVALID_INDEX && !KeyDetermined(frame.within_scope_idx)) {
					return Repair(StringUtil::Format("the WITHIN (%s) partition of a frame",
					                                 (*ctx.entity_scopes)[frame.within_scope_idx].table_alias));
				}
				if (frame.order_key) {
					string error = Check(*frame.order_key);
					if (!error.empty()) {
						return error;
					}
				}
			}
			return "";
		}
		case ExpressionClass::BOUND_SUBQUERY: {
			// An uncorrelated scalar subquery is one value for the whole query; a
			// correlated one reads the row it is correlated with.
			auto &subquery = expr.Cast<BoundSubqueryExpression>();
			if (!subquery.IsCorrelated() || scope.kind == DecideScopeKind::ROW) {
				return "";
			}
			return Repair("a correlated subquery");
		}
		case ExpressionClass::BOUND_CONJUNCTION: {
			auto &conj = expr.Cast<BoundConjunctionExpression>();
			if (IsConstraintWrapper(conj)) {
				// A nested wrapper (a WHEN filter under a PER) reads rows, not the instance;
				// its constraint child is what the instance reads. A guard is read per
				// instance and must be determined.
				string error;
				if (!conj.children.empty()) {
					error = Check(*conj.children[0]);
				}
				if (error.empty() && IsIfConstraintWrapper(conj) && conj.children.size() == 2) {
					error = Check(*conj.children[1]);
				}
				return error;
			}
			break;
		}
		default:
			break;
		}
		string error;
		ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
			if (error.empty()) {
				error = Check(child);
			}
		});
		return error;
	}
};

} // namespace

string CheckDeterminedByGeneration(const Expression &expr, DecideGenerationScope scope,
                                   const DecideQualifierContext &ctx, BindContext &bind_context) {
	if (scope.kind == DecideScopeKind::ROW) {
		return "";
	}
	Prover prover(scope, ctx, bind_context);
	return prover.Check(expr);
}

void ValidateDecideGenerationTree(const Expression &constraints, const DecideQualifierContext &ctx,
                                  BindContext &bind_context) {
	VisitConstraintTree(constraints, [&](const Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			return true;
		}
		auto &conj = node.Cast<BoundConjunctionExpression>();
		if (!IsPerConstraintWrapper(conj) || conj.children.empty()) {
			return true;
		}
		DecideScopeKind kind;
		idx_t scope_idx;
		if (!TryParseGenScopeTag(conj.GetAlias(), kind, scope_idx)) {
			throw InternalException("PER wrapper carries no generation scope");
		}
		auto scope = kind == DecideScopeKind::GLOBAL ? DecideGenerationScope::Global()
		                                             : DecideGenerationScope::Key(scope_idx);
		auto error = CheckDeterminedByGeneration(*conj.children[0], scope, ctx, bind_context);
		if (!error.empty()) {
			throw BinderException(node, "%s", error);
		}
		return true;
	});
}

} // namespace duckdb
