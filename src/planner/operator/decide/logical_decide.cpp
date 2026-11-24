// src/planner/operator/logical_decide.cpp
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_decide_reducer_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include <algorithm>

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

LogicalDecide::LogicalDecide(idx_t decide_index, BoundDecide bound_p)
    : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE), decide_index(decide_index), bound(std::move(bound_p)) {
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
	for (idx_t i = 0; i < bound.variables.size(); i++) {
		result.emplace_back(decide_index, i);
	}
	return result;
}

void LogicalDecide::ResolveTypes() {
	types = children[0]->types;
	// Include ALL decide variable types (user + auxiliary).
	// Auxiliary vars are pruned by the projection operator above.
	for (idx_t i = 0; i < bound.variables.size(); i++) {
		types.push_back(bound.variables[i]->return_type);
	}
}

vector<idx_t> LogicalDecide::GetTableIndex() const {
	return vector<idx_t> {decide_index};
}

string LogicalDecide::GetName() const {
	return "DECIDE";
}

void LogicalDecide::AddClause(ClientContext &context, BoundDecideClause clause) {
	CanonicalizeClause(context, clause);
	bound.clauses.push_back(std::move(clause));
}

void LogicalDecide::CanonicalizeClause(ClientContext &context, BoundDecideClause &clause) {
	DecideCanonicalizer canonicalizer(context, decide_index, bound);
	vector<BoundDecideClause> canonical;
	canonical.push_back(std::move(clause));
	canonicalizer.CanonicalizeClauses(canonical);
	canonicalizer.VerifyCanonical(canonical[0]);
	clause = std::move(canonical[0]);
}

void LogicalDecide::SetObjective(ClientContext &context, BoundDecideClause objective) {
	if (!objective.body) {
		bound.objective = BoundDecideClause();
		return;
	}
	DecideCanonicalizer canonicalizer(context, decide_index, bound);
	DecideSourceNames names {bound.source_text, bound.keyed_scopes, bound.keys};
	canonicalizer.QuoteMessagesWith(names);
	bound.objective = canonicalizer.CanonicalizeObjective(objective, bound.objective_constant_offset);
	canonicalizer.VerifyCanonicalObjective(bound.objective);
}

void BoundDecide::EnumerateExpressions(const std::function<void(unique_ptr<Expression> &expression)> &callback) {
	for (auto &expr : variables) {
		callback(expr);
	}
	for (auto &computed : computed_values) {
		callback(computed.value);
	}
	for (auto &clause : clauses) {
		clause.EnumerateExpressions(callback);
	}
	objective.EnumerateExpressions(callback);
	// A key's columns are live even where no clause reads them: a decision keyed by a
	// column the query never mentions still needs it in the scan.
	for (auto &key : keys) {
		for (auto &element : key.elements) {
			callback(element.expression);
			if (element.collated) {
				callback(element.collated);
			}
		}
	}
	// A frame's `within` key is an entry of `keys`, enumerated with them.
	for (auto &frame : frames) {
		for (auto *expr : {&frame.fallback, &frame.order, &frame.rows}) {
			if (*expr) {
				callback(*expr);
			}
		}
	}
	for (auto &signature : signatures) {
		if (signature.rows) {
			callback(signature.rows);
		}
		for (auto &field : signature.fields) {
			if (field.value) {
				callback(field.value);
			}
		}
	}
}

void LogicalDecide::EnumerateExpressions(const std::function<void(unique_ptr<Expression> *)> &callback) {
	bound.EnumerateExpressions([&](unique_ptr<Expression> &expression) { callback(&expression); });
	// A helper definition holds what a rewrite lifted out of the clauses, or copied beside them.
	for (auto &definition : helper_definitions) {
		definition->EnumerateExpressions([&](unique_ptr<Expression> &expression) { callback(&expression); });
	}
	for (auto &check : value_checks) {
		callback(&check);
	}
	for (auto &aggregate : bound.bound_aggregates) {
		for (auto &argument : aggregate.arguments) {
			callback(&argument);
		}
		if (aggregate.reads) {
			callback(&aggregate.reads);
		}
	}
}

BoundDecideReducerExpression &DecideReducerValueDefinition::Reducer() const {
	return body->Cast<BoundDecideReducerExpression>();
}

idx_t DecideReducerValueDefinition::Frame() const {
	return body->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_FRAME
	           ? body->Cast<BoundDecideFrameExpression>().frame
	           : DConstants::INVALID_INDEX;
}

unique_ptr<Expression> &DecideReducerValueDefinition::Member() const {
	return Frame() == DConstants::INVALID_INDEX ? Reducer().children[0]
	                                            : body->Cast<BoundDecideFrameExpression>().children[0];
}

void LogicalDecide::NameDefinition(Expression &call, idx_t index) {
	if (call.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		throw InternalException("DECIDE: a helper definition is kept for '%s', which is no function call",
		                        call.ToString());
	}
	auto &definition = call.Cast<BoundFunctionExpression>().definition;
	definition.kind = helper_definitions[index]->kind;
	definition.index = index;
}

idx_t LogicalDecide::HelperCount(DecideHelperKind kind) const {
	return NumericCast<idx_t>(
	    std::count_if(helper_definitions.begin(), helper_definitions.end(),
	                  [&](const unique_ptr<DecideHelperDefinition> &definition) { return definition->kind == kind; }));
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

//! The sign a term of a value link's members must carry to be pushed down: the members are pushed as the helper is
//! (DecideHelperDefinition::push), a SUM or AVG being linear in them and a MIN or MAX never decreasing when one grows.
//! A MIN or MAX's closing side is the hard direction, encoded with a Big-M over its members, which reads them from
//! both directions: no sign is pinned, which is 0, as under no push.
static int ValueLinkPush(const DecideReducerValueDefinition &link) {
	if (IsDecideExtremum(link.reducer) && link.NeedsClosing()) {
		return 0;
	}
	return link.push == ExtremumPush::DOWN ? 1 : link.push == ExtremumPush::UP ? -1 : 0;
}

//! The body of `clause`, which the clause list or an L0 indicator (`index`) holds.
static DecideOwnedBody ClauseBody(DecideOwnedBody::Owner owner, idx_t index, BoundDecideClause &clause) {
	DecideOwnedBody body(owner, index, clause.body);
	body.push = ComparisonPush(*clause.body);
	body.rows = clause.scope.when.get();
	body.clause = &clause;
	return body;
}

optional_ptr<const DecideReducerValueDefinition>
DecideValueHelperOfScope(const vector<unique_ptr<DecideHelperDefinition>> &definitions,
                         const vector<DecideVarScopeInfo> &variable_scopes, idx_t scope) {
	for (auto &definition : definitions) {
		const DecideHelperDefinition &entry = *definition;
		auto link = entry.TryCast<DecideReducerValueDefinition>();
		if (link && variable_scopes[link->helper].IsKeyed() && variable_scopes[link->helper].keyed_scope_idx == scope) {
			return link;
		}
	}
	return nullptr;
}

void DecideHelperDefinition::EnumerateExpressions(const std::function<void(unique_ptr<Expression> &)> &callback) {
	if (when) {
		callback(when);
	}
}

void DecideHelperDefinition::Bodies(idx_t, const std::function<void(DecideOwnedBody &)> &) {
}

void DecideHelperDefinition::Rows(ClientContext &, const LogicalDecide &,
                                  const std::function<void(const BoundDecideClause &)> &) const {
}

void DecideL0IndicatorDefinition::Rows(ClientContext &, const LogicalDecide &,
                                       const std::function<void(const BoundDecideClause &)> &callback) const {
	for (auto &link : links) {
		callback(link);
	}
}

void DecideAbsDefinition::EnumerateExpressions(const std::function<void(unique_ptr<Expression> &)> &callback) {
	if (body) {
		callback(body);
	}
	DecideHelperDefinition::EnumerateExpressions(callback);
}

void DecideProductDefinition::EnumerateExpressions(const std::function<void(unique_ptr<Expression> &)> &callback) {
	callback(body);
	DecideHelperDefinition::EnumerateExpressions(callback);
}

void DecideL0IndicatorDefinition::EnumerateExpressions(const std::function<void(unique_ptr<Expression> &)> &callback) {
	DecideHelperDefinition::EnumerateExpressions(callback);
	for (auto &link : links) {
		link.EnumerateExpressions(callback);
	}
}

void DecideL0IndicatorDefinition::Bodies(idx_t index, const std::function<void(DecideOwnedBody &)> &callback) {
	for (auto &link : links) {
		auto body = ClauseBody(DecideOwnedBody::Owner::HELPER, index, link);
		callback(body);
	}
}

void DecideBodyDefinition::EnumerateExpressions(const std::function<void(unique_ptr<Expression> &)> &callback) {
	callback(body);
	DecideHelperDefinition::EnumerateExpressions(callback);
}

void DecideBodyDefinition::Bodies(idx_t index, const std::function<void(DecideOwnedBody &)> &callback) {
	DecideOwnedBody owned(DecideOwnedBody::Owner::HELPER, index, body);
	owned.push = relation == ExpressionType::COMPARE_GREATERTHANOREQUALTO ? 1
	             : relation == ExpressionType::COMPARE_LESSTHANOREQUALTO  ? -1
	                                                                      : 0;
	owned.rows = when.get();
	callback(owned);
}

void DecideReducerValueDefinition::EnumerateExpressions(const std::function<void(unique_ptr<Expression> &)> &callback) {
	if (Frame() != DConstants::INVALID_INDEX) {
		callback(Member());
	} else {
		auto &reducer = Reducer();
		for (auto &child : reducer.children) {
			callback(child);
		}
		if (reducer.filter) {
			callback(reducer.filter);
		}
	}
	DecideHelperDefinition::EnumerateExpressions(callback);
}

void DecideReducerValueDefinition::Bodies(idx_t index, const std::function<void(DecideOwnedBody &)> &callback) {
	const bool frame = Frame() != DConstants::INVALID_INDEX;
	DecideOwnedBody owned(DecideOwnedBody::Owner::HELPER, index, Member());
	owned.push = ValueLinkPush(*this);
	owned.kept_rows = DecideRowsKeptByBoth(when.get(), frame ? nullptr : Reducer().filter.get());
	owned.rows = owned.kept_rows.get();
	callback(owned);
}

void LogicalDecide::RewriteBodies(ClientContext &context, const std::function<void(DecideOwnedBody &)> &callback) {
	vector<BoundDecideClause *> changed;
	EnumerateBodies([&](DecideOwnedBody &body) {
		callback(body);
		if (body.changed && body.owner != DecideOwnedBody::Owner::HELPER) {
			changed.push_back(body.clause.get());
		}
	});
	for (auto clause : changed) {
		if (clause == &bound.objective) {
			SetObjective(context, std::move(bound.objective));
		} else {
			CanonicalizeClause(context, *clause);
		}
	}
}

void LogicalDecide::EnumerateBodies(const std::function<void(DecideOwnedBody &)> &callback) {
	using Owner = DecideOwnedBody::Owner;
	if (bound.objective.body) {
		// The objective is minimized, which pushes a positive term down.
		DecideOwnedBody body(Owner::OBJECTIVE, DConstants::INVALID_INDEX, bound.objective.body);
		body.push = 1;
		body.rows = bound.objective.scope.when.get();
		body.clause = &bound.objective;
		callback(body);
	}
	for (auto &clause : bound.clauses) {
		auto body = ClauseBody(Owner::CLAUSE, DConstants::INVALID_INDEX, clause);
		callback(body);
	}
	for (idx_t i = 0; i < helper_definitions.size(); i++) {
		helper_definitions[i]->Bodies(i, callback);
	}
}

vector<ColumnBinding> LogicalDecide::SourceBindings() const {
	auto result = children[0]->GetColumnBindings();
	for (auto &aggregate : bound.bound_aggregates) {
		result.push_back(aggregate.column);
	}
	return result;
}

InsertionOrderPreservingMap<string> LogicalDecide::ParamsToString() const {
	DecidePlanParams plan {bound.variables,
	                       num_auxiliary_vars,
	                       bound.keyed_scopes,
	                       bound.keys,
	                       bound.frames,
	                       bound.sense,
	                       decide_index,
	                       bound.source_text,
	                       aux_var_expressions,
	                       bound.clauses,
	                       bound.constraint_sources,
	                       bound.objective,
	                       bound.written_objective,
	                       bound.canonical_objective,
	                       bound.objective_canonical_differs};
	auto result = RenderDecideParams(plan, rewrite_accounts, nullptr);
	SetParamsEstimatedCardinality(result);
	return result;
}

} // namespace duckdb
