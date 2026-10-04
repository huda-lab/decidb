#include "duckdb/planner/expression_binder/decide/decide_generation.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/decide/decide_frame_spec.hpp"
#include "duckdb/planner/bind_context.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
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

	//! The repair for a data column: the four ways a column becomes one value per
	//! instance. `can_declare_key` is false when the column's relation is not a base
	//! table (a VALUES list, a subquery, a CTE), where no PRIMARY KEY can be declared.
	string RepairColumn(const string &what, bool can_declare_key) const {
		if (scope.kind == DecideScopeKind::GLOBAL) {
			return StringUtil::Format(
			    "%s varies across rows, but PER () generates one instance for the whole query; "
			    "reduce it (sum, avg, min or max) or generate PER a key that determines it",
			    what);
		}
		return StringUtil::Format(
		    "%s is not determined by the generation key PER %s: add it to the key, name its relation in "
		    "the key, %sor reduce it with BY (%s)",
		    what, scope_text, can_declare_key ? "declare a PRIMARY KEY the key covers, " : "", scope_text,
		    scope_text);
	}

	//! The repair for a decision, which a key can never name: declare it at the
	//! generation key, generate at the decision's own key, or reduce it.
	string RepairDecision(const string &name, const DecideVarScopeInfo &var_scope) const {
		string own_key = var_scope.IsEntity() ? "PER " + (*ctx.entity_scopes)[var_scope.entity_scope_idx].table_alias
		                                      : "once per row";
		if (scope.kind == DecideScopeKind::GLOBAL) {
			return StringUtil::Format(
			    "decision '%s' is generated %s, but PER () generates one instance for the whole query; "
			    "reduce it (SUM(%s)) or declare it PER ()",
			    name, own_key, name);
		}
		if (var_scope.IsEntity()) {
			return StringUtil::Format("decision '%s' is generated %s, which PER %s does not determine: add its key "
			                          "to the generation key, or reduce it with SUM(%s) BY (%s)",
			                          name, own_key, scope_text, name, scope_text);
		}
		return StringUtil::Format("decision '%s' is generated once per row, which PER %s does not identify: "
		                          "declare it PER %s, drop the PER for one instance per row, or reduce it with "
		                          "SUM(%s) BY (%s)",
		                          name, scope_text, scope_text, name, scope_text);
	}

	//! The repair for a reducer's group or a frame's partition, which are keys, not values.
	string RepairKey(const string &what) const {
		if (scope.kind == DecideScopeKind::GLOBAL) {
			return StringUtil::Format("%s varies across rows, but PER () generates one instance for the whole "
			                          "query; drop the key (BY ()) or generate PER a key that determines it",
			                          what);
		}
		return StringUtil::Format("%s is not determined by the generation key PER %s: add its columns to the "
		                          "generation key, or use the generation key's own columns there (%s)",
		                          what, scope_text, scope_text);
	}

	//! Whether every column of a relation is a function of this key: the relation is
	//! named wholly in the key, or its PRIMARY KEY / UNIQUE columns lie in the key.
	bool RelationDetermined(idx_t table_index) const {
		if (key.whole_relations.count(table_index)) {
			return true;
		}
		auto binding = FindBinding(bind_context, table_index);
		return binding && TableKeyInsideKey(*binding, key);
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
		return key.Contains(colref.binding) || RelationDetermined(colref.binding.table_index);
	}

	//! Whether a PRIMARY KEY could be declared for the column's relation at all.
	bool ColumnRelationIsBaseTable(const BoundColumnRefExpression &colref) const {
		auto binding = FindBinding(bind_context, colref.binding.table_index);
		return binding && binding->binding_type == BindingType::TABLE;
	}

	//! A decision is determined when its own generation key is a function of this key:
	//! a query-wide decision always, a keyed decision when its key columns are (the same
	//! rule a reducer's BY key follows), and a row decision when the key identifies a
	//! row of the result -- a table key of every relation in the FROM clause lies in it.
	bool DecisionDetermined(const BoundColumnRefExpression &colref) const {
		if (scope.kind == DecideScopeKind::ROW) {
			return true;
		}
		idx_t var_idx = colref.binding.column_index;
		auto &scopes = *ctx.variable_scopes;
		if (var_idx >= scopes.size()) {
			return false;
		}
		auto &var_scope = scopes[var_idx];
		if (var_scope.IsScalar()) {
			return true;
		}
		if (scope.kind == DecideScopeKind::GLOBAL) {
			return false;
		}
		if (var_scope.IsEntity()) {
			return KeyDetermined(var_scope.entity_scope_idx);
		}
		// Row-scoped: one decision per result row.
		bool any_relation = false;
		for (auto &binding : bind_context.GetBindingsList()) {
			if (binding->index == ctx.decide_index) {
				continue;
			}
			any_relation = true;
			if (!RelationDetermined(binding->index)) {
				return false;
			}
		}
		return any_relation;
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
			if (!key.columns.count(column) && !RelationDetermined(column.first)) {
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
				if (DecisionDetermined(colref)) {
					return "";
				}
				idx_t var_idx = colref.binding.column_index;
				auto &scopes = *ctx.variable_scopes;
				return var_idx < scopes.size() ? RepairDecision(colref.GetName(), scopes[var_idx])
				                               : RepairColumn("decision '" + colref.GetName() + "'", false);
			}
			return DataColumnDetermined(colref)
			           ? ""
			           : RepairColumn("column '" + StripDecideTags(colref.GetName()) + "'",
			                          ColumnRelationIsBaseTable(colref));
		}
		case ExpressionClass::BOUND_AGGREGATE: {
			// A reducer's value is a function of its group, which the key must identify.
			// Its body is judged by the reducer's own generation, so the walk stops here;
			// its filter reads rows, not the instance, and needs no proof either.
			auto &agg = expr.Cast<BoundAggregateExpression>();
			idx_t by_scope;
			if (TryParseReduceByTag(agg.GetAlias(), by_scope) && !KeyDetermined(by_scope)) {
				return RepairKey(StringUtil::Format("the BY (%s) group of %s(...)",
				                                    (*ctx.entity_scopes)[by_scope].table_alias,
				                                    StringUtil::Upper(agg.function.name)));
			}
			// A frame navigates a timeline the key must identify (its WITHIN partition,
			// spec §7.3). A relative selector (PREVIOUS / NEXT) also counts from the
			// instance's own position, so the order key must be one value per instance;
			// an absolute one (FIRST / LAST) reads the same position for every row of
			// the timeline and needs no such proof (deck p52).
			idx_t frame_idx;
			if (TryParseFrameRefTag(agg.GetAlias(), frame_idx) && ctx.frames && frame_idx < ctx.frames->size()) {
				auto &frame = (*ctx.frames)[frame_idx];
				auto spec = DecideFrameSpec::Decode(frame.spec);
				auto relative = [](const DecideFrameSelector &selector) {
					return selector.kind == DecideFrameSelectorKind::PREVIOUS ||
					       selector.kind == DecideFrameSelectorKind::NEXT;
				};
				const bool navigates_from_own_position =
				    relative(spec.from_selector) || (spec.is_range && relative(spec.to_selector));
				if (scope.kind == DecideScopeKind::GLOBAL) {
					return navigates_from_own_position
					           ? RepairKey("the position of a frame (AT / FROM .. TO .. OVER)")
					           : (frame.within_scope_idx == DConstants::INVALID_INDEX
					                  ? ""
					                  : RepairKey("the WITHIN partition of a frame"));
				}
				if (frame.within_scope_idx != DConstants::INVALID_INDEX && !KeyDetermined(frame.within_scope_idx)) {
					return RepairKey(StringUtil::Format("the WITHIN (%s) partition of a frame",
					                                    (*ctx.entity_scopes)[frame.within_scope_idx].table_alias));
				}
				if (navigates_from_own_position && frame.order_key) {
					string error = Check(*frame.order_key);
					if (!error.empty()) {
						return "a frame's PREVIOUS / NEXT counts from the instance's own position on its OVER key: " +
						       error;
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
			if (scope.kind == DecideScopeKind::GLOBAL) {
				return "a correlated subquery reads one row at a time, but PER () generates one instance for "
				       "the whole query; join its value into the FROM clause or drop the PER";
			}
			return StringUtil::Format("a correlated subquery reads one row at a time, which PER %s does not "
			                          "identify; join its value into the FROM clause as a column the key determines",
			                          scope_text);
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

//! `factor * agg(...) BY (k)` and `agg(...) BY (k) / factor`: a decision-free factor
//! the reducer's BY key determines is one value per reduced group, so it may scale
//! the reducer -- deck p21's `ship <= D.maxShipmentShare * SUM(ship) BY (D.depotID)`.
//! The proof is this layer's, since it needs the key scopes and the schema keys; the
//! shape decision, admitting the tagged factor as a scale, is the canonicalizer's,
//! and the fold is exact because the factor is constant on every group it scales.
void TagGroupWideReducerFactors(Expression &constraints, const DecideQualifierContext &ctx,
                                BindContext &bind_context) {
	auto strip_casts = [](Expression &expr) -> Expression & {
		Expression *current = &expr;
		while (current->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			current = current->Cast<BoundCastExpression>().child.get();
		}
		return *current;
	};
	auto references_decision = [&](const Expression &expr) {
		bool found = false;
		std::function<void(const Expression &)> scan = [&](const Expression &node) {
			if (found) {
				return;
			}
			if (node.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			    node.Cast<BoundColumnRefExpression>().binding.table_index == ctx.decide_index) {
				found = true;
				return;
			}
			if (node.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
				found = true; // a reducer is judged as a term, never as a factor
				return;
			}
			ExpressionIterator::EnumerateChildren(node, scan);
		};
		scan(expr);
		return found;
	};
	std::function<void(Expression &)> walk = [&](Expression &node) {
		if (node.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = node.Cast<BoundFunctionExpression>();
			const bool is_mult = func.function.name == "*";
			const bool is_div = func.function.name == "/";
			if ((is_mult || is_div) && func.children.size() == 2) {
				for (idx_t agg_child = 0; agg_child < 2; agg_child++) {
					if (is_div && agg_child != 0) {
						continue; // only `agg / factor` divides
					}
					auto &agg = strip_casts(*func.children[agg_child]);
					if (agg.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
						continue;
					}
					idx_t by_scope;
					if (!TryParseReduceByTag(agg.GetAlias(), by_scope)) {
						continue; // `BY ()`: the factor must be query-wide, which needs no proof here
					}
					auto &factor = *func.children[1 - agg_child];
					if (factor.IsFoldable() || references_decision(factor)) {
						continue;
					}
					if (CheckDeterminedByGeneration(factor, DecideGenerationScope::Key(by_scope), ctx, bind_context)
					        .empty()) {
						auto alias = factor.GetAlias();
						AddDecideTag(alias, GROUP_WIDE_FACTOR_TAG);
						factor.SetAlias(alias);
					}
				}
			}
		}
		ExpressionIterator::EnumerateChildren(node, [&](Expression &child) { walk(child); });
	};
	walk(constraints);
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
