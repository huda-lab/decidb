// src/planner/operator/logical_decide.cpp
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

bool DecideBoundAggregate::IsReadBy(const Expression &expression) const {
	return expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	       expression.Cast<BoundColumnRefExpression>().binding == column;
}

DecideKeyElement DecideKeyElement::Copy() const {
	DecideKeyElement result;
	result.expression = expression->Copy();
	result.name = name;
	result.null_mode = null_mode;
	if (collated) {
		result.collated = collated->Copy();
	}
	return result;
}

DecideKeyElement DecideKeyElement::Bind(ClientContext &context, unique_ptr<Expression> expression, string name,
                                        DecideKeyNullMode null_mode) {
	DecideKeyElement result;
	result.name = std::move(name);
	result.null_mode = null_mode;
	// A bound subquery cannot be copied, so it groups and is labelled under its collation, as a GROUP BY group is.
	if (expression->HasSubquery()) {
		ExpressionBinder::PushCollation(context, expression, expression->return_type);
	} else {
		auto collated = expression->Copy();
		if (ExpressionBinder::PushCollation(context, collated, collated->return_type)) {
			result.collated = std::move(collated);
		}
	}
	result.expression = std::move(expression);
	return result;
}

//! Whether `expr` holds a bound subquery, which equals nothing, not even itself.
static bool HoldsSubquery(const Expression &expr) {
	bool found = expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { found |= HoldsSubquery(child); });
	return found;
}

//! The written column wins over the same column a relation element stands for: its name,
//! and its NULL mode, which is no group (NO_GROUP) rather than a value (VALUE).
static bool WrittenWins(const DecideKeyElement &candidate, const DecideKeyElement &kept) {
	return candidate.null_mode == DecideKeyNullMode::NO_GROUP && kept.null_mode == DecideKeyNullMode::VALUE;
}

//! One order on key elements, so a key's elements are the same list however it was
//! written: columns by binding, then any other expression by its text.
static bool ElementBefore(const DecideKeyElement &a, const DecideKeyElement &b) {
	auto is_column = [](const DecideKeyElement &element) {
		return element.expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
	};
	if (is_column(a) != is_column(b)) {
		return is_column(a);
	}
	if (is_column(a)) {
		auto &left = a.expression->Cast<BoundColumnRefExpression>().binding;
		auto &right = b.expression->Cast<BoundColumnRefExpression>().binding;
		return left.table_index != right.table_index ? left.table_index < right.table_index
		                                             : left.column_index < right.column_index;
	}
	return a.expression->ToString() < b.expression->ToString();
}

idx_t FindOrCreateDecideKey(vector<DecideKeyElement> elements, const string &text, vector<DecideKey> &keys) {
	vector<DecideKeyElement> unique;
	for (auto &element : elements) {
		auto kept = std::find_if(unique.begin(), unique.end(), [&](const DecideKeyElement &member) {
			return member.Grouping().Equals(element.Grouping());
		});
		if (kept == unique.end()) {
			unique.push_back(std::move(element));
		} else if (WrittenWins(element, *kept)) {
			*kept = std::move(element);
		}
	}
	if (unique.empty()) {
		return DConstants::INVALID_INDEX;
	}
	std::stable_sort(unique.begin(), unique.end(), ElementBefore);
	auto holds = [](const vector<DecideKeyElement> &set, const DecideKeyElement &element) {
		for (auto &member : set) {
			if (member.null_mode == element.null_mode && member.Grouping().Equals(element.Grouping())) {
				return true;
			}
		}
		return false;
	};
	bool subquery = false;
	for (auto &element : unique) {
		subquery |= HoldsSubquery(*element.expression);
	}
	for (idx_t i = 0; i < keys.size(); i++) {
		auto &existing = keys[i].elements;
		bool equal = existing.size() == unique.size();
		for (auto &element : unique) {
			equal = equal && holds(existing, element);
		}
		// A key holding a subquery is recognised by its text: written the same way in one
		// query, it is the same key.
		if (equal || (subquery && keys[i].text == text)) {
			return i;
		}
	}
	DecideKey key;
	key.elements = std::move(unique);
	key.text = text;
	keys.push_back(std::move(key));
	return keys.size() - 1;
}

const vector<DecideKeyElement> &DecideKeyElements(const vector<DecideKey> &keys, idx_t key) {
	static const vector<DecideKeyElement> no_elements;
	return key == DConstants::INVALID_INDEX ? no_elements : keys[key].elements;
}

string DecideKeyText(const vector<DecideKey> &keys, idx_t key) {
	return key == DConstants::INVALID_INDEX ? string("()") : keys[key].text;
}

vector<string> DecideKey::Names() const {
	vector<string> names;
	for (auto &element : elements) {
		names.push_back(element.name);
	}
	return names;
}

LogicalDecide::LogicalDecide(idx_t decide_index, vector<unique_ptr<Expression>> decide_variables,
                             unique_ptr<Expression> decide_constraints, DecideSense written_sense,
                             unique_ptr<Expression> decide_objective)
    : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE), decide_index(decide_index),
      decide_variables(std::move(decide_variables)), decide_constraints(std::move(decide_constraints)),
      written_sense(written_sense), decide_objective(std::move(decide_objective)) {
}

LogicalDecide::LogicalDecide() : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE) {
}

//! The operator in `plan` whose own output carries `table_index`, or null.
static const LogicalOperator *DecideBindingProducer(const LogicalOperator &plan, idx_t table_index) {
	for (auto index : plan.GetTableIndex()) {
		if (index == table_index) {
			return &plan;
		}
	}
	for (auto &child : plan.children) {
		if (auto *producer = DecideBindingProducer(*child, table_index)) {
			return producer;
		}
	}
	return nullptr;
}

//! The column of `producer`'s input that `expression` carries unchanged: a plain reference, bound or resolved, or one
//! under the compression the optimizer puts around a join (CompressedMaterialization), which changes how the column is
//! held and not which column it is. False for any other expression.
static bool DecideForwardedColumn(const LogicalOperator &producer, const Expression &expression,
                                  ColumnBinding &forwarded) {
	auto expression_class = expression.GetExpressionClass();
	if (expression_class == ExpressionClass::BOUND_FUNCTION) {
		auto &function = expression.Cast<BoundFunctionExpression>();
		const auto &name = function.function.name;
		auto compression = StringUtil::StartsWith(name, "__internal_compress_") ||
		                   StringUtil::StartsWith(name, "__internal_decompress_");
		return compression && !function.children.empty() &&
		       DecideForwardedColumn(producer, *function.children[0], forwarded);
	}
	if (expression_class == ExpressionClass::BOUND_COLUMN_REF) {
		forwarded = expression.Cast<BoundColumnRefExpression>().binding;
		return true;
	}
	if (expression_class == ExpressionClass::BOUND_REF && producer.children.size() == 1) {
		auto input = producer.children[0]->GetColumnBindings();
		auto index = expression.Cast<BoundReferenceExpression>().index;
		if (index < input.size()) {
			forwarded = input[index];
			return true;
		}
	}
	return false;
}

ColumnBinding DecideSourceColumnKey(const LogicalOperator &source, const ColumnBinding &binding,
                                    const vector<DecideSourceColumnName> &named) {
	auto *producer = DecideBindingProducer(source, binding.table_index);
	if (producer && producer->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &expr = *producer->expressions[binding.column_index];
		auto is_named = std::any_of(named.begin(), named.end(),
		                            [&](const DecideSourceColumnName &column) { return column.binding == binding; });
		if (!is_named) {
			// The binder named no such column: the optimizer put this projection there, and its column is the one it
			// reads.
			ColumnBinding forwarded;
			return DecideForwardedColumn(*producer, expr, forwarded)
			           ? DecideSourceColumnKey(source, forwarded, named)
			           : ColumnBinding(binding.table_index, DConstants::INVALID_INDEX);
		}
		auto forwards = expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
		                expr.GetExpressionClass() == ExpressionClass::BOUND_REF;
		return forwards || expr.HasAlias() ? binding : ColumnBinding(binding.table_index, DConstants::INVALID_INDEX);
	}
	if (!producer || producer->type != LogicalOperatorType::LOGICAL_GET) {
		return binding;
	}
	auto &get = producer->Cast<LogicalGet>();
	auto &column_ids = get.GetColumnIds();
	// A binding of a scan indexes its column ids whether or not the scan has projection_ids
	// (LogicalGet::GetColumnBindings lists the projected ids as the binding's column), so the slot is a place in the
	// column ids for any scan the walk reaches. A scan with no column ids answers (table, 0) from GetColumnBindings;
	// that slot names no column.
	auto slot = binding.column_index;
	auto ordinal = slot >= column_ids.size() || column_ids[slot].IsRowIdColumn() ? DConstants::INVALID_INDEX
	                                                                             : column_ids[slot].GetPrimaryIndex();
	return ColumnBinding(binding.table_index, ordinal);
}

vector<ColumnBinding> LogicalDecide::GetColumnBindings() {
	// Return all child columns plus ALL decide variables (including auxiliary).
	// Auxiliary vars (e.g. from ABS linearization) must be visible for column binding
	// resolution in constraint/objective expressions. The projection above prunes them.
	auto result = children[0]->GetColumnBindings();
	for (idx_t i = 0; i < decide_variables.size(); i++) {
		result.emplace_back(decide_index, i);
	}
	return result;
}

void LogicalDecide::ResolveTypes() {
	types = children[0]->types;
	// Include ALL decide variable types (user + auxiliary).
	// Auxiliary vars are pruned by the projection operator above.
	for (idx_t i = 0; i < decide_variables.size(); i++) {
		types.push_back(decide_variables[i]->return_type);
	}
}

vector<idx_t> LogicalDecide::GetTableIndex() const {
	return vector<idx_t> {decide_index};
}

string LogicalDecide::GetName() const {
	return "DECIDE";
}

void LogicalDecide::AddConstraint(ClientContext &context, unique_ptr<Expression> constraint) {
	DecideCanonicalizer canonicalizer(context, decide_index, variable_scopes, keyed_scopes, keys, is_boolean_var);
	auto canonical = canonicalizer.CanonicalizeTree(*constraint);
	canonicalizer.VerifyCanonical(*canonical);

	if (!decide_constraints) {
		decide_constraints = std::move(canonical);
	} else {
		auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		conj->children.push_back(std::move(decide_constraints));
		conj->children.push_back(std::move(canonical));
		decide_constraints = std::move(conj);
	}
}

void LogicalDecide::SetObjective(ClientContext &context, unique_ptr<Expression> objective) {
	if (!objective) {
		decide_objective = nullptr;
		return;
	}
	DecideCanonicalizer canonicalizer(context, decide_index, variable_scopes, keyed_scopes, keys, is_boolean_var);
	DecideSourceNames names {source_text, keyed_scopes, keys};
	canonicalizer.QuoteMessagesWith(names);
	decide_objective = canonicalizer.CanonicalizeObjective(*objective, objective_constant_offset);
	canonicalizer.VerifyCanonicalObjective(*decide_objective);
}

void LogicalDecide::EnumerateExpressions(const std::function<void(unique_ptr<Expression> *)> &callback) {
	for (auto &expr : decide_variables) {
		callback(&expr);
	}
	if (decide_constraints) {
		callback(&decide_constraints);
	}
	if (decide_objective) {
		callback(&decide_objective);
	}
	// The value rewrite lifts each reducer it replaces out of the constraint and objective trees, so visiting them
	// above no longer reaches it.
	for (auto &link : reducer_value_links) {
		for (auto *expr : {&link.inner_expr, &link.filter, &link.when}) {
			if (*expr) {
				callback(expr);
			}
		}
	}
	// Nonlinear reducer bodies are tree-owned F definitions, retained after their clause-list placeholders are made.
	for (auto &definition : definition_functions) {
		for (auto *expr : {&definition.body, &definition.condition}) {
			if (*expr) {
				callback(expr);
			}
		}
	}
	// A key's columns are live even where no clause reads them: a decision keyed by a
	// column the query never mentions still needs it in the scan.
	for (auto &key : keys) {
		for (auto &element : key.elements) {
			callback(&element.expression);
			if (element.collated) {
				callback(&element.collated);
			}
		}
	}
	// A frame's `within` key is an entry of `keys`, enumerated with them.
	for (auto &frame : frames) {
		for (auto *expr : {&frame.fallback, &frame.order, &frame.rows}) {
			if (*expr) {
				callback(expr);
			}
		}
	}
	for (auto &check : value_checks) {
		callback(&check);
	}
	for (auto &signature : signatures) {
		if (signature.rows) {
			callback(&signature.rows);
		}
		for (auto &field : signature.fields) {
			if (field.value) {
				callback(&field.value);
			}
		}
	}
	for (auto &aggregate : bound_aggregates) {
		for (auto &argument : aggregate.arguments) {
			callback(&argument);
		}
		if (aggregate.reads) {
			callback(&aggregate.reads);
		}
	}
	for (auto &expression : retained_expressions) {
		callback(&expression);
	}
	for (auto &function : abs_functions) {
		if (function.operand) {
			callback(&function.operand);
		}
		if (function.when) {
			callback(&function.when);
		}
	}
	for (auto &group : bilinear_function_groups) {
		if (group.expansion) {
			callback(&group.expansion);
		}
	}
	for (auto &function : l0_indicator_functions) {
		for (auto &link : function.links) {
			if (link) {
				callback(&link);
			}
		}
	}
}

//! `<` and `<=` push a positive term down, `>` and `>=` a negative one; nothing else pushes (DecideOwnedBody::push).
static int ComparisonPush(const Expression &leaf) {
	if (leaf.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
		return 0;
	}
	switch (leaf.type) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return 1;
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return -1;
	default:
		return 0;
	}
}

//! The sign a term of a value link's members must carry to be pushed down, from the sides the link states
//! (ReducerValueLink::need_envelope, need_closing), which join every reader's push (RewriteScopedReducers): `value >=
//! members` of a SUM, AVG or MAX holds the members down, `value <= members` of a MIN holds them up, and `value <=
//! members` of a SUM or AVG holds them up as well, the sign it reads them with the other way round. A MIN or MAX's
//! closing side is the hard direction, encoded with a Big-M over its members, and both sides read the members from
//! both directions: no sign is pinned, which is 0.
static int ValueLinkPush(const LogicalDecide::ReducerValueLink &link) {
	if (link.need_envelope == link.need_closing) {
		return 0;
	}
	const bool extremum = link.agg_name == "min" || link.agg_name == "max";
	if (link.need_envelope) {
		return link.agg_name == "min" ? -1 : 1;
	}
	return extremum ? 0 : -1;
}

//! Every leaf of a clause tree, with the scope and the source clause it stands in: the walk every pass over clauses
//! makes, which descends into a wrapper's constraint and never into its condition (decide_constraint_walk.hpp).
static void EnumerateClauseLeaves(unique_ptr<Expression> &slot, DecideEnclosingScope scope, const string &inherited,
                                  DecideOwnedBody::Owner owner, idx_t index,
                                  const std::function<void(DecideOwnedBody &)> &callback) {
	const auto source_alias = DescendSourceAlias(*slot, inherited);
	scope = EnterDecideScope(*slot, scope);
	if (slot->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = slot->Cast<BoundConjunctionExpression>();
		for (idx_t i = 0; i < conjunction.children.size(); i++) {
			if (IsConstraintChild(conjunction, i)) {
				EnumerateClauseLeaves(conjunction.children[i], scope, source_alias, owner, index, callback);
			}
		}
		return;
	}
	DecideOwnedBody body(owner, index, slot);
	body.push = ComparisonPush(*slot);
	body.rows = scope.Condition();
	body.source_alias = source_alias;
	body.scope = scope;
	callback(body);
}

void LogicalDecide::EnumerateBodies(const std::function<void(DecideOwnedBody &)> &callback) {
	using Owner = DecideOwnedBody::Owner;
	if (decide_objective) {
		// A `when` or `per` wrapper carries no sign: the body is its first child. The objective is minimized, which
		// pushes a positive term down.
		auto *slot = &decide_objective;
		DecideEnclosingScope scope;
		string source_alias = OBJECTIVE_ROW_TAG;
		while ((*slot)->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = (*slot)->Cast<BoundConjunctionExpression>();
			if (!IsConstraintWrapper(conjunction)) {
				break;
			}
			source_alias = DescendSourceAlias(conjunction, source_alias);
			scope = EnterDecideScope(conjunction, scope);
			slot = &conjunction.children[0];
		}
		DecideOwnedBody body(Owner::OBJECTIVE, DConstants::INVALID_INDEX, *slot);
		body.push = 1;
		body.rows = scope.Condition();
		body.source_alias = DescendSourceAlias(**slot, source_alias);
		body.scope = scope;
		callback(body);
	}
	if (decide_constraints) {
		EnumerateClauseLeaves(decide_constraints, DecideEnclosingScope(), string(), Owner::CLAUSE,
		                      DConstants::INVALID_INDEX, callback);
	}
	// A definition stands apart from the clause list; `t >= q` holds the body down, `t <= q` up, `t = q` neither.
	for (idx_t i = 0; i < definition_functions.size(); i++) {
		auto &definition = definition_functions[i];
		DecideOwnedBody body(Owner::DEFINITION, i, definition.body);
		body.push = definition.relation == ExpressionType::COMPARE_GREATERTHANOREQUALTO ? 1
		            : definition.relation == ExpressionType::COMPARE_LESSTHANOREQUALTO  ? -1
		                                                                                : 0;
		body.rows = definition.condition.get();
		body.source_alias = MakeDefinesTag(definition.value_var);
		AddDecideTag(body.source_alias, BODY_DEFINITION_TAG);
		callback(body);
	}
	// A reducer value helper's body belongs to no clause: it is read on the rows its path and its own `when` keep.
	for (idx_t i = 0; i < reducer_value_links.size(); i++) {
		auto &link = reducer_value_links[i];
		DecideOwnedBody body(Owner::VALUE_LINK, i, link.inner_expr);
		body.push = ValueLinkPush(link);
		body.kept_rows = DecideRowsKeptByBoth(link.when.get(), link.filter.get());
		body.rows = body.kept_rows.get();
		callback(body);
	}
	// An L0 indicator's three rows stand beside the clauses, where the tree's indicator F node reads them.
	for (idx_t i = 0; i < l0_indicator_functions.size(); i++) {
		for (auto &link : l0_indicator_functions[i].links) {
			EnumerateClauseLeaves(link, DecideEnclosingScope(), string(), Owner::L0_LINK, i, callback);
		}
	}
}

vector<ColumnBinding> LogicalDecide::SourceBindings() {
	auto result = children[0]->GetColumnBindings();
	for (auto &aggregate : bound_aggregates) {
		result.push_back(aggregate.column);
	}
	return result;
}

InsertionOrderPreservingMap<string> LogicalDecide::ParamsToString() const {
	auto result = RenderDecideParams(*this, rewrite_accounts, nullptr);
	SetParamsEstimatedCardinality(result);
	return result;
}

} // namespace duckdb
