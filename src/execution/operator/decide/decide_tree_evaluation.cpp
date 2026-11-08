#include "duckdb/execution/operator/decide/decide_tree_evaluation.hpp"

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/decide/decide_row_evaluation.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/storage/arena_allocator.hpp"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

namespace duckdb {

static DecideTreeValuePtr DataValue(Value value) {
	auto result = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::DATA, value.type());
	result->data = std::move(value);
	return result;
}

static DecideTreeValuePtr AbsentValue(const LogicalType &type) {
	return make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::ABSENT, type);
}

static DecideTreeValuePtr UnsupportedValue(const LogicalType &type, const string &reason) {
	auto result = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::UNSUPPORTED, type);
	result->operation = reason;
	return result;
}

static DecideTreeValuePtr ErrorValue(const LogicalType &type, const ErrorData &error,
                                     DecideErrorPhase phase = DecideErrorPhase::INVALID) {
	auto result = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::ERROR, type);
	result->error = error;
	result->error_rank.phase = phase;
	return result;
}

static bool StopsEvaluation(const ErrorData &error) {
	auto type = error.Type();
	return type == ExceptionType::INTERRUPT || type == ExceptionType::OUT_OF_MEMORY || type == ExceptionType::FATAL;
}

string DecideTreeValue::ToString() const {
	switch (kind) {
	case DecideTreeValueKind::DATA:
		return data.ToString();
	case DecideTreeValueKind::DECISION:
		return "v" + to_string(variable) + "[" + to_string(instance) + "]";
	case DecideTreeValueKind::ABSENT:
		return "absent";
	case DecideTreeValueKind::ERROR:
		return "error(" + error.Message() + ")";
	case DecideTreeValueKind::UNSUPPORTED:
		return "unsupported(" + operation + ")";
	case DecideTreeValueKind::SCALAR:
	case DecideTreeValueKind::REDUCER: {
		string text = operation + "(";
		for (idx_t i = 0; i < operands.size(); i++) {
			text += (i ? ", " : "") + operands[i]->ToString();
		}
		return text + ")";
	}
	}
	throw InternalException("DECIDE tree: an unknown synthesized value kind");
}

DecideTreeEvaluation::DecideTreeEvaluation(const DecideTree &tree_p, const DecideDescent &descent_p,
                                           ClientContext &context_p)
    : tree(tree_p), descent(descent_p), context(context_p), results(tree.nodes.size()), rows(tree.nodes.size()),
      expression_ranks(tree.nodes.size()) {
	for (idx_t node = 0; node < tree.nodes.size(); node++) {
		auto &entries = descent.Containers()[node].entries;
		for (idx_t entry = 0; entry < entries.size(); entry++) {
			rows[node][entries[entry].row].push_back(entry);
		}
		auto &definition = *tree.nodes[node];
		auto assign = [&](const Expression &root, DecideErrorPhase phase, idx_t clause, idx_t &ordinal) {
			std::function<void(const Expression &)> visit = [&](const Expression &expression) {
				expression_ranks[node][&expression] = {phase, clause, ordinal++, DConstants::INVALID_INDEX};
				ExpressionIterator::EnumerateChildren(expression, [&](const Expression &child) { visit(child); });
			};
			visit(root);
		};
		idx_t ordinal = 0;
		for (auto &test : definition.member_tests) {
			assign(*test, definition.route_phase, definition.route_order, ordinal);
		}
		ordinal = 1000000;
		for (auto *expression : {definition.body.get(), definition.rhs.get(), definition.fallback.get()}) {
			if (expression) {
				assign(*expression, definition.value_phase, definition.value_order, ordinal);
			}
		}
		for (auto &argument : definition.arguments) {
			assign(*argument, definition.value_phase, definition.value_order, ordinal);
		}
	}
}

idx_t DecideTreeEvaluation::ChildEntry(idx_t child, const DecideDescentEntry &entry) const {
	if (child >= results.size()) {
		throw InternalException("DECIDE tree: a body reads missing node %llu", child);
	}
	auto found = rows[child].find(entry.row);
	if (found == rows[child].end()) {
		return DConstants::INVALID_INDEX;
	}
	auto &container = descent.Containers()[child];
	for (auto index : found->second) {
		if (container.entries[index].parent_slot == entry.parent_slot) {
			return index;
		}
	}
	if (found->second.size() == 1) {
		return found->second[0];
	}
	throw InternalException("DECIDE tree: a shared child has several unmatched parent contexts");
}

DecideTreeValuePtr DecideTreeEvaluation::ChildValue(idx_t child, const DecideDescentEntry &entry) const {
	const auto index = ChildEntry(child, entry);
	if (index == DConstants::INVALID_INDEX) {
		auto &node = *tree.nodes[child];
		const auto type =
		    node.function ? node.function->return_type : (node.body ? node.body->return_type : LogicalType::SQLNULL);
		return node.decision_value ? AbsentValue(type) : DataValue(Value(type));
	}
	if (index >= results[child].entries.size() || !results[child].entries[index]) {
		throw InternalException("DECIDE tree: node %llu is read before its entry is computed", child);
	}
	return results[child].entries[index];
}

void DecideTreeEvaluation::SignatureReads(const Expression &expression, idx_t node_index,
                                          const DecideDescentEntry &entry, vector<Value> &signature) const {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		const auto column = expression.Cast<BoundReferenceExpression>().index;
		if (column < tree.input_column_count) {
			auto &columns = tree.nodes[node_index]->columns;
			for (idx_t index = 0; index < columns.size(); index++) {
				if (columns[index]->GetExpressionClass() == ExpressionClass::BOUND_REF &&
				    columns[index]->Cast<BoundReferenceExpression>().index == column) {
					signature.push_back(entry.columns[index]);
					return;
				}
			}
			throw InternalException("DECIDE tree: a fallback signature reads a column its frame did not carry");
		}
		const auto child = column - tree.input_column_count;
		const auto index = ChildEntry(child, entry);
		if (index == DConstants::INVALID_INDEX) {
			signature.push_back(Value());
			return;
		}
		auto &source = descent.Containers()[child].entries[index];
		auto &key = descent.Containers()[child].groups[source.slot].key;
		signature.insert(signature.end(), key.begin(), key.end());
		if (tree.nodes[child]->kind == DecideNodeKind::FRAME) {
			if (tree.nodes[child]->frame_spec->Relative()) {
				signature.push_back(source.order_key);
			}
			auto &refinement = results[child].refinements[index];
			signature.insert(signature.end(), refinement.begin(), refinement.end());
		}
		return;
	}
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &reference = expression.Cast<BoundColumnRefExpression>();
		if (reference.binding.table_index == tree.decide_index) {
			auto value = DecisionValue(reference.binding.column_index, expression.return_type, entry);
			if (value->kind == DecideTreeValueKind::DECISION) {
				signature.push_back(Value::UBIGINT(value->variable));
				signature.push_back(Value::UBIGINT(value->instance));
			}
			return;
		}
	}
	ExpressionIterator::EnumerateChildren(
	    expression, [&](const Expression &child) { SignatureReads(child, node_index, entry, signature); });
}

DecideTreeValuePtr DecideTreeEvaluation::DecisionValue(idx_t variable, const LogicalType &type,
                                                       const DecideDescentEntry &entry) const {
	if (variable < tree.value_nodes.size() && tree.value_nodes[variable] != DConstants::INVALID_INDEX) {
		return ChildValue(tree.value_nodes[variable], entry);
	}
	idx_t instance = entry.row;
	if (variable < tree.variable_scopes.size()) {
		auto &scope = tree.variable_scopes[variable];
		if (scope.IsGlobal()) {
			instance = 0;
		} else if (scope.IsKeyed() || scope.scope == DecideVarScope::INLINED) {
			const auto &mapping = descent.DecisionRows();
			if (scope.keyed_scope_idx >= mapping.size() || entry.row >= mapping[scope.keyed_scope_idx].size()) {
				return UnsupportedValue(type, "a helper decision has no defining node or declaration mapping");
			}
			instance = mapping[scope.keyed_scope_idx][entry.row];
			if (instance == DConstants::INVALID_INDEX) {
				return AbsentValue(type);
			}
			const auto &groups = descent.DecisionGroups()[scope.keyed_scope_idx];
			if (instance < groups.size() && groups[instance].rank != DConstants::INVALID_INDEX) {
				instance = groups[instance].rank;
			}
		}
	}
	auto value = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::DECISION, type);
	value->variable = variable;
	value->instance = instance;
	value->row = entry.row;
	return value;
}

DecideTreeValuePtr DecideTreeEvaluation::DecisionRead(idx_t variable, const LogicalType &type, idx_t node_index,
                                                      const DecideDescentEntry &entry) const {
	auto value = DecisionValue(variable, type, entry);
	// A root reads the auxiliary it defines unconditionally: a dependency cycle through that address would otherwise
	// make every definition absent before it could be computed.
	if (value->kind != DecideTreeValueKind::DECISION || tree.nodes[node_index]->defined_var == variable) {
		return value;
	}
	if (variable < tree.definition_nodes.size() && !tree.definition_nodes[variable].empty()) {
		// A root defining the auxiliary stands for it where an instance of the entry's class survives.
		const auto scope =
		    variable < tree.variable_scopes.size() ? tree.variable_scopes[variable] : DecideVarScopeInfo::Row();
		const vector<idx_t> *mapping = nullptr;
		if ((scope.IsKeyed() || scope.scope == DecideVarScope::INLINED) &&
		    scope.keyed_scope_idx < descent.DecisionRows().size()) {
			mapping = &descent.DecisionRows()[scope.keyed_scope_idx];
		}
		bool present = false;
		for (auto definition : tree.definition_nodes[variable]) {
			auto &definition_entries = descent.Containers()[definition].entries;
			auto &definition_values = results[definition].entries;
			for (idx_t index = 0; index < definition_values.size(); index++) {
				const auto row = definition_entries[index].row;
				bool same = scope.IsGlobal() || row == entry.row;
				if (mapping) {
					same = row < mapping->size() && entry.row < mapping->size() &&
					       (*mapping)[row] != DConstants::INVALID_INDEX && (*mapping)[row] == (*mapping)[entry.row];
				}
				if (!same) {
					continue;
				}
				auto &definition_value = definition_values[index];
				if (definition_value->kind == DecideTreeValueKind::ERROR ||
				    definition_value->kind == DecideTreeValueKind::UNSUPPORTED) {
					return definition_value;
				}
				present |= definition_value->Present();
			}
		}
		if (!present) {
			return AbsentValue(type);
		}
	}
	for (auto &product : tree.bilinear_links) {
		if (product.aux_idx != variable) {
			continue;
		}
		for (auto factor : {product.bool_var_idx, product.other_var_idx}) {
			if (!DecisionRead(factor, type, node_index, entry)->Present()) {
				return AbsentValue(type);
			}
		}
	}
	return value;
}

DecideTreeValuePtr DecideTreeEvaluation::ExpressionValue(const Expression &expression, idx_t node_index,
                                                         const DecideDescentEntry &entry) {
	auto data = [&](Value value) {
		auto result = DataValue(std::move(value));
		result->operation = RenderDecideSource(expression, tree.source_fragments, tree.keyed_scopes, &tree.keys);
		return result;
	};
	try {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			return data(expression.Cast<BoundConstantExpression>().value);
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &reference = expression.Cast<BoundColumnRefExpression>();
			if (reference.binding.table_index == tree.decide_index) {
				return DecisionRead(reference.binding.column_index, expression.return_type, node_index, entry);
			}
			return UnsupportedValue(expression.return_type, "a source reference was not resolved before descent");
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			const auto column = expression.Cast<BoundReferenceExpression>().index;
			if (column >= tree.input_column_count) {
				return ChildValue(column - tree.input_column_count, entry);
			}
			auto &columns = tree.nodes[node_index]->columns;
			for (idx_t index = 0; index < columns.size(); index++) {
				if (columns[index]->GetExpressionClass() == ExpressionClass::BOUND_REF &&
				    columns[index]->Cast<BoundReferenceExpression>().index == column) {
					if (index >= entry.columns.size()) {
						throw InternalException("DECIDE tree: an entry did not carry column %llu", column);
					}
					return data(entry.columns[index]);
				}
			}
			return UnsupportedValue(expression.return_type, "a body reads a column its node did not carry");
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			return UnsupportedValue(expression.return_type, "an aggregate survived as a scalar body");
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_CASE) {
			auto &conditional = expression.Cast<BoundCaseExpression>();
			for (auto &check : conditional.case_checks) {
				auto when = ExpressionValue(*check.when_expr, node_index, entry);
				if (when->kind != DecideTreeValueKind::DATA) {
					return when->Symbolic() ? UnsupportedValue(expression.return_type, "a symbolic CASE condition")
					                        : when;
				}
				if (!when->data.IsNull() && when->data.GetValue<bool>()) {
					return ExpressionValue(*check.then_expr, node_index, entry);
				}
			}
			return ExpressionValue(*conditional.else_expr, node_index, entry);
		}
		vector<const Expression *> children;
		ExpressionIterator::EnumerateChildren(expression, [&](const Expression &child) { children.push_back(&child); });
		vector<DecideTreeValuePtr> operands;
		const auto expression_type = expression.GetExpressionType();
		if ((expression_type == ExpressionType::OPERATOR_IS_NULL ||
		     expression_type == ExpressionType::OPERATOR_IS_NOT_NULL) &&
		    children.size() == 1) {
			auto value = ExpressionValue(*children[0], node_index, entry);
			if (value->kind == DecideTreeValueKind::ERROR || value->kind == DecideTreeValueKind::UNSUPPORTED) {
				return value;
			}
			const bool is_null = value->kind == DecideTreeValueKind::ABSENT ||
			                     (value->kind == DecideTreeValueKind::DATA && value->data.IsNull());
			return data(Value::BOOLEAN(expression_type == ExpressionType::OPERATOR_IS_NULL ? is_null : !is_null));
		}
		if (expression_type == ExpressionType::OPERATOR_COALESCE) {
			for (auto child : children) {
				auto value = ExpressionValue(*child, node_index, entry);
				if (value->kind != DecideTreeValueKind::DATA || !value->data.IsNull()) {
					return value;
				}
			}
			return data(Value(expression.return_type));
		}
		if (expression_type == ExpressionType::CONJUNCTION_AND || expression_type == ExpressionType::CONJUNCTION_OR) {
			const bool is_and = expression_type == ExpressionType::CONJUNCTION_AND;
			bool unknown = false;
			for (auto child : children) {
				auto value = ExpressionValue(*child, node_index, entry);
				if (value->kind != DecideTreeValueKind::DATA) {
					return value->Symbolic() ? UnsupportedValue(expression.return_type, "a symbolic predicate") : value;
				}
				if (value->data.IsNull()) {
					unknown = true;
				} else if (value->data.GetValue<bool>() != is_and) {
					return data(Value::BOOLEAN(!is_and));
				}
			}
			return data(unknown ? Value(LogicalType::BOOLEAN) : Value::BOOLEAN(is_and));
		}
		const bool additive_objective = tree.nodes[node_index]->objective &&
		                                expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		                                (expression.Cast<BoundFunctionExpression>().function.name == "+" ||
		                                 expression.Cast<BoundFunctionExpression>().function.name == "-");
		bool symbolic = false;
		bool has_additive_value = false;
		for (auto child : children) {
			auto value = ExpressionValue(*child, node_index, entry);
			if (value->kind == DecideTreeValueKind::ERROR || value->kind == DecideTreeValueKind::UNSUPPORTED) {
				return value;
			}
			if (value->kind == DecideTreeValueKind::ABSENT ||
			    (additive_objective && value->kind == DecideTreeValueKind::DATA && value->data.IsNull())) {
				if (!additive_objective) {
					return value;
				}
				value = data(Value::INTEGER(0).DefaultCastAs(child->return_type));
			} else {
				has_additive_value = true;
			}
			symbolic |= value->Symbolic();
			operands.push_back(std::move(value));
		}
		if (additive_objective && !has_additive_value) {
			return AbsentValue(expression.return_type);
		}
		if (symbolic && expression.PropagatesNullValues()) {
			for (auto &operand : operands) {
				if (operand->kind == DecideTreeValueKind::DATA && operand->data.IsNull()) {
					return AbsentValue(expression.return_type);
				}
			}
		}
		auto scalar = expression.Copy();
		idx_t child_index = 0;
		ExpressionIterator::EnumerateChildren(*scalar, [&](unique_ptr<Expression> &child) {
			auto &value = operands[child_index];
			child = symbolic ? unique_ptr<Expression>(make_uniq<BoundReferenceExpression>(value->type, child_index))
			                 : unique_ptr<Expression>(make_uniq<BoundConstantExpression>(value->data));
			child_index++;
		});
		if (!symbolic) {
			if (!scalar->IsFoldable()) {
				return UnsupportedValue(expression.return_type, "a scalar body is not evaluable from its entries");
			}
			return data(ExpressionExecutor::EvaluateScalar(context, *scalar));
		}
		auto value = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::SCALAR, expression.return_type);
		value->node = node_index;
		value->operation = expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION
		                       ? expression.Cast<BoundFunctionExpression>().function.name
		                       : ExpressionTypeToString(expression_type);
		value->expression = std::move(scalar);
		value->operands = std::move(operands);
		return value;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (StopsEvaluation(error)) {
			throw;
		}
		auto value = ErrorValue(expression.return_type, error);
		value->node = node_index;
		value->row = entry.row;
		auto ranked = expression_ranks[node_index].find(&expression);
		value->error_rank = ranked == expression_ranks[node_index].end()
		                        ? DecideErrorRank {tree.nodes[node_index]->value_phase,
		                                           tree.nodes[node_index]->value_order, 1000000, entry.row}
		                        : ranked->second;
		value->error_rank.row = entry.row;
		value->error_ranked = true;
		return value;
	}
}

//! One typed SQL aggregate state, including bind data and state destruction on an exceptional update/finalize.
class TreeAggregateState {
public:
	TreeAggregateState(ClientContext &context, const BoundAggregateExpression &aggregate_p)
	    : aggregate(aggregate_p), allocator(BufferAllocator::Get(context)), input(aggregate.bind_info.get(), allocator),
	      pointer(LogicalType::POINTER),
	      state(allocator.AllocateAligned(aggregate.function.state_size(aggregate.function))) {
		aggregate.function.initialize(aggregate.function, state);
		FlatVector::GetData<data_ptr_t>(pointer)[0] = state;
	}
	~TreeAggregateState() {
		if (aggregate.function.destructor) {
			aggregate.function.destructor(pointer, input, 1);
		}
	}
	Value Fold(const vector<vector<DecideTreeValuePtr>> &arguments, const vector<idx_t> &members) {
		vector<idx_t> distinct_members;
		if (aggregate.IsDistinct()) {
			unordered_map<hash_t, vector<idx_t>> buckets;
			for (auto member : members) {
				hash_t hash = 0;
				for (auto &argument : arguments) {
					hash ^= argument[member]->data.Hash() + 0x9e3779b9 + (hash << 6) + (hash >> 2);
				}
				auto &bucket = buckets[hash];
				bool duplicate = false;
				for (auto previous : bucket) {
					bool equal = true;
					for (auto &argument : arguments) {
						equal &= Value::NotDistinctFrom(argument[member]->data, argument[previous]->data);
					}
					duplicate |= equal;
				}
				if (!duplicate) {
					bucket.push_back(member);
					distinct_members.push_back(member);
				}
			}
		}
		auto &reads = aggregate.IsDistinct() ? distinct_members : members;
		for (idx_t offset = 0; offset < reads.size(); offset += STANDARD_VECTOR_SIZE) {
			const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, reads.size() - offset);
			vector<Vector> sources;
			for (idx_t argument = 0; argument < arguments.size(); argument++) {
				const auto type = aggregate.children[argument]->return_type;
				sources.emplace_back(type);
				for (idx_t row = 0; row < count; row++) {
					sources.back().SetValue(row, arguments[argument][reads[offset + row]]->data);
				}
			}
			Vector targets(LogicalType::POINTER);
			auto pointers = FlatVector::GetData<data_ptr_t>(targets);
			for (idx_t row = 0; row < count; row++) {
				pointers[row] = state;
			}
			aggregate.function.update(sources.data(), input, sources.size(), targets, count);
		}
		Vector result(aggregate.return_type);
		aggregate.function.finalize(pointer, input, result, 1, 0);
		return result.GetValue(0);
	}

private:
	const BoundAggregateExpression &aggregate;
	ArenaAllocator allocator;
	AggregateInputData input;
	Vector pointer;
	data_ptr_t state;
};

DecideTreeValuePtr DecideTreeEvaluation::Reduce(idx_t node_index, const BoundAggregateExpression *function,
                                                const vector<vector<DecideTreeValuePtr>> &arguments,
                                                const vector<DecideTreeValuePtr> &members) {
	auto &node = *tree.nodes[node_index];
	const auto type = function ? function->return_type : (members.empty() ? LogicalType::DOUBLE : members[0]->type);
	vector<idx_t> kept;
	bool symbolic = false;
	for (idx_t member = 0; member < members.size(); member++) {
		auto &value = members[member];
		if (value->kind == DecideTreeValueKind::ABSENT) {
			continue;
		}
		if (value->kind == DecideTreeValueKind::ERROR || value->kind == DecideTreeValueKind::UNSUPPORTED) {
			return value;
		}
		for (auto &argument : arguments) {
			if (argument[member]->kind == DecideTreeValueKind::ERROR ||
			    argument[member]->kind == DecideTreeValueKind::UNSUPPORTED) {
				return argument[member];
			}
			if (argument[member]->kind == DecideTreeValueKind::ABSENT) {
				return AbsentValue(type);
			}
			symbolic |= argument[member]->Symbolic();
		}
		symbolic |= value->Symbolic();
		kept.push_back(member);
	}
	const auto aggregate =
	    node.aggregate.empty() && function ? StringUtil::Lower(function->function.name) : node.aggregate;
	if (symbolic || node.decision_value) {
		if (function && function->IsDistinct()) {
			return UnsupportedValue(type, "DISTINCT over decisions depends on values that are not solved yet");
		}
		auto result = make_shared_ptr<DecideTreeValue>(DecideTreeValueKind::REDUCER, type);
		result->node = node_index;
		result->operation = aggregate;
		for (auto member : kept) {
			auto &value = members[member];
			if (value->kind != DecideTreeValueKind::DATA || !value->data.IsNull()) {
				result->operands.push_back(value);
			}
		}
		return result->operands.empty() ? AbsentValue(type) : result;
	}
	if (!function) {
		return UnsupportedValue(type, "a data reducer has no bound aggregate function");
	}
	// The bound state is the fold; `aggregate` only names it. A collated MIN or MAX is bound as arg_min or arg_max over
	// its value and comparison key (BindMinMax). A rewrite renames a carrier only for a reducer over decisions, which
	// is symbolic above and never reaches this fold.
	try {
		if (aggregate != "count" && !arguments.empty()) {
			for (auto member : kept) {
				auto &value = arguments[0][member]->data;
				if (!value.IsNull() &&
				    (value.type().InternalType() == PhysicalType::DOUBLE ||
				     value.type().InternalType() == PhysicalType::FLOAT) &&
				    std::isnan(value.GetValue<double>())) {
					throw InvalidInputException(DecideFrameNanMessage(node.text));
				}
			}
		}
		TreeAggregateState state(context, *function);
		auto value = DataValue(state.Fold(arguments, kept));
		value->operation = node.text;
		return value;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (StopsEvaluation(error)) {
			throw;
		}
		return ErrorValue(type, error);
	}
}

void DecideTreeEvaluation::Scope(idx_t node_index) {
	auto &node = *tree.nodes[node_index];
	auto &result = results[node_index];
	auto &container = descent.Containers()[node_index];
	for (auto &entry : container.entries) {
		auto value = node.body ? ExpressionValue(*node.body, node_index, entry) : DataValue(Value::BOOLEAN(true));
		if (node.value_check && value->kind == DecideTreeValueKind::DATA) {
			value = value->data.IsNull()
			            ? DataValue(Value::BOOLEAN(true))
			            : ErrorValue(LogicalType::BOOLEAN, ErrorData(InvalidInputException(value->data.ToString())));
		}
		DecideTreeValuePtr rhs;
		if (node.rhs) {
			rhs = ExpressionValue(*node.rhs, node_index, entry);
			if ((rhs->kind == DecideTreeValueKind::ERROR &&
			     (value->kind != DecideTreeValueKind::ERROR || rhs->error_rank < value->error_rank)) ||
			    (rhs->kind == DecideTreeValueKind::UNSUPPORTED && value->kind != DecideTreeValueKind::ERROR)) {
				value = rhs;
			}
		}
		// Compute first, then settle membership. Errors of a reached entry remain visible even when a NULL membership
		// test subsequently removes it; an unreachable entry was never inserted in the first place.
		for (auto &test : node.member_tests) {
			auto membership = ExpressionValue(*test, node_index, entry);
			if (membership->kind == DecideTreeValueKind::ERROR) {
				if (value->kind != DecideTreeValueKind::ERROR || membership->error_rank < value->error_rank) {
					value = membership;
				}
				continue;
			}
			if (value->kind != DecideTreeValueKind::ERROR && value->kind != DecideTreeValueKind::UNSUPPORTED) {
				if (membership->kind != DecideTreeValueKind::DATA) {
					value = membership;
					continue;
				}
				if (membership->data.IsNull() || !membership->data.GetValue<bool>()) {
					value = AbsentValue(value->type);
				}
			}
		}
		if (rhs &&
		    (rhs->kind == DecideTreeValueKind::ABSENT ||
		     (rhs->kind == DecideTreeValueKind::DATA && rhs->data.IsNull())) &&
		    value->kind != DecideTreeValueKind::ERROR && value->kind != DecideTreeValueKind::UNSUPPORTED) {
			value = AbsentValue(value->type);
		}
		if (node.root && !node.value_check && value->kind == DecideTreeValueKind::DATA && value->data.IsNull()) {
			value = AbsentValue(value->type);
		}
		result.entries.push_back(std::move(value));
		if (rhs) {
			result.rhs.push_back(std::move(rhs));
		}
	}
	result.instances.assign(container.entries.size(), DConstants::INVALID_INDEX);
	vector<vector<Value>> signatures;
	unordered_map<hash_t, vector<idx_t>> buckets;
	for (idx_t index = 0; index < container.entries.size(); index++) {
		if (!result.entries[index]->Present()) {
			continue;
		}
		auto &entry = container.entries[index];
		auto signature = entry.signature;
		for (auto &read : node.child_reads) {
			if (tree.nodes[read.node]->kind != DecideNodeKind::FRAME) {
				continue;
			}
			auto child = ChildEntry(read.node, entry);
			if (child != DConstants::INVALID_INDEX) {
				auto &refinement = results[read.node].refinements[child];
				signature.push_back(Value::UBIGINT(read.node));
				signature.insert(signature.end(), refinement.begin(), refinement.end());
			}
		}
		hash_t hash = entry.slot;
		for (auto &value : signature) {
			hash ^= value.Hash() + 0x9e3779b9 + (hash << 6) + (hash >> 2);
		}
		auto &bucket = buckets[hash];
		idx_t instance = DConstants::INVALID_INDEX;
		for (auto candidate : bucket) {
			const auto representative = result.instance_entries[candidate];
			bool equal = container.entries[representative].slot == entry.slot &&
			             signatures[candidate].size() == signature.size();
			for (idx_t field = 0; equal && field < signature.size(); field++) {
				equal = Value::NotDistinctFrom(signatures[candidate][field], signature[field]);
			}
			if (equal) {
				instance = candidate;
				break;
			}
		}
		if (instance == DConstants::INVALID_INDEX) {
			instance = result.instance_entries.size();
			result.instance_entries.push_back(index);
			signatures.push_back(std::move(signature));
			bucket.push_back(instance);
		}
		result.instances[index] = instance;
	}
}

void DecideTreeEvaluation::Reducer(idx_t node_index) {
	auto &node = *tree.nodes[node_index];
	auto &container = descent.Containers()[node_index];
	auto &result = results[node_index];
	idx_t operand = DConstants::INVALID_INDEX;
	for (auto child : node.children) {
		if (tree.nodes[child]->kind == DecideNodeKind::SCOPE) {
			if (operand != DConstants::INVALID_INDEX) {
				operand = DConstants::INVALID_INDEX;
				break;
			}
			operand = child;
		}
	}
	if (operand == DConstants::INVALID_INDEX) {
		for (idx_t group = 0; group < container.groups.size(); group++) {
			result.groups.push_back(UnsupportedValue(LogicalType::SQLNULL, "a reducer has no unique operand scope"));
		}
	} else {
		auto &operand_node = *tree.nodes[operand];
		auto &operand_entries = descent.Containers()[operand].entries;
		vector<vector<idx_t>> group_entries(container.groups.size());
		for (idx_t entry = 0; entry < operand_entries.size(); entry++) {
			const auto group = operand_entries[entry].parent_slot;
			if (group < group_entries.size()) {
				group_entries[group].push_back(entry);
			}
		}
		for (idx_t group = 0; group < container.groups.size(); group++) {
			vector<DecideTreeValuePtr> members;
			vector<vector<DecideTreeValuePtr>> arguments(operand_node.arguments.size());
			std::unordered_set<idx_t> seen;
			for (auto entry_index : group_entries[group]) {
				auto &entry = operand_entries[entry_index];
				auto value = results[operand].entries[entry_index];
				if (node.body) {
					auto raw = ExpressionValue(*node.body, operand, entry);
					if (raw->kind == DecideTreeValueKind::ERROR || raw->kind == DecideTreeValueKind::UNSUPPORTED) {
						value = raw;
					} else if (raw->kind == DecideTreeValueKind::DATA && !raw->data.IsNull() &&
					           (raw->data.type().InternalType() == PhysicalType::DOUBLE ||
					            raw->data.type().InternalType() == PhysicalType::FLOAT) &&
					           std::isnan(raw->data.GetValue<double>())) {
						value = ErrorValue(node.function ? node.function->return_type : raw->type,
						                   ErrorData(InvalidInputException(DecideFrameNanMessage(node.text))));
					}
				}
				if (!value->Present()) {
					continue;
				}
				const bool failed =
				    value->kind == DecideTreeValueKind::ERROR || value->kind == DecideTreeValueKind::UNSUPPORTED;
				const auto identity =
				    results[operand].instances.empty() ? entry.instance : results[operand].instances[entry_index];
				if (!failed && !seen.insert(identity).second) {
					continue;
				}
				members.push_back(value);
				for (idx_t argument = 0; argument < operand_node.arguments.size(); argument++) {
					arguments[argument].push_back(
					    argument == 0 ? value : ExpressionValue(*operand_node.arguments[argument], operand, entry));
				}
			}
			if (arguments.empty() && node.function && !node.function->children.empty()) {
				arguments.push_back(members);
			}
			result.groups.push_back(Reduce(node_index, node.function.get(), arguments, members));
		}
	}
	for (auto &entry : container.entries) {
		result.entries.push_back(result.groups[entry.slot]);
	}
}

static int64_t FramePosition(DecideFrameAnchor end, int64_t k, int64_t anchor, int64_t count) {
	// The grammar bounds k to a 32-bit Iconst; a timeline's positions count its stored entries.
	switch (end) {
	case DecideFrameAnchor::FIRST:
		return 0;
	case DecideFrameAnchor::LAST:
		return count - 1;
	case DecideFrameAnchor::PREVIOUS:
		return anchor - k;
	case DecideFrameAnchor::NEXT:
		return anchor + k;
	}
	throw InternalException("DECIDE tree: a frame has no recognized selector");
}

void DecideTreeEvaluation::Frame(idx_t node_index) {
	auto &node = *tree.nodes[node_index];
	auto &container = descent.Containers()[node_index];
	auto &result = results[node_index];
	const auto type = node.frame_spec && node.frame_spec->range && node.function
	                      ? node.function->return_type
	                      : (node.body ? node.body->return_type : LogicalType::SQLNULL);
	auto missing = [&]() {
		return node.decision_value ? AbsentValue(type) : DataValue(Value(type));
	};
	result.entries.resize(container.entries.size());
	result.refinements.resize(container.entries.size());
	if (!node.frame_spec || !node.body) {
		for (auto &value : result.entries) {
			value = UnsupportedValue(type, "a frame has no bound selector or payload");
		}
		return;
	}
	auto &frame = *node.frame_spec;
	vector<DecideTreeValuePtr> payloads(container.entries.size());
	const auto limit = GetDecideFrameLimit(context);
	idx_t visits = 0;
	for (auto &timeline : container.timelines) {
		const auto count = static_cast<int64_t>(timeline.positions.size());
		unordered_map<string, DecideTreeValuePtr> windows;
		auto selected = [&](idx_t position) {
			auto &at = timeline.positions[position];
			const auto first = at.entries[0];
			if (at.conflict) {
				string location = frame.order_name + " = " + DecideSqlLiteral(at.order);
				auto &key = container.groups[timeline.slot].key;
				for (idx_t i = 0; i < MinValue(key.size(), frame.timeline_names.size()); i++) {
					location = frame.timeline_names[i] + " = " + DecideSqlLiteral(key[i]) + ", " + location;
				}
				return ErrorValue(type, ErrorData(InvalidInputException(
				                            "DECIDE: %s finds %llu rows at %s; a position must hold one row.",
				                            frame.text, at.entries.size(), location)));
			}
			if (!payloads[first]) {
				payloads[first] = ExpressionValue(*node.body, node_index, container.entries[first]);
			}
			return payloads[first];
		};
		for (idx_t anchor_position = 0; anchor_position < timeline.positions.size(); anchor_position++) {
			for (auto anchor_entry : timeline.positions[anchor_position].entries) {
				auto &anchor = container.entries[anchor_entry];
				const auto lo = FramePosition(frame.from, frame.from_k, anchor_position, count);
				if (!frame.range) {
					if (frame.cyclic) {
						result.entries[anchor_entry] = selected(static_cast<idx_t>((lo % count + count) % count));
					} else if (lo >= 0 && lo < count) {
						result.entries[anchor_entry] = selected(static_cast<idx_t>(lo));
					} else {
						result.entries[anchor_entry] =
						    node.fallback ? ExpressionValue(*node.fallback, node_index, anchor) : missing();
						if (node.fallback) {
							result.refinements[anchor_entry].push_back(Value::BOOLEAN(true));
							SignatureReads(*node.fallback, node_index, anchor, result.refinements[anchor_entry]);
						}
					}
					continue;
				}
				const auto hi = FramePosition(frame.to, frame.to_k, anchor_position, count);
				const auto step = frame.every;
				if (step <= 0) {
					throw InternalException("DECIDE tree: a range's step is not positive");
				}
				if (hi < lo) {
					result.entries[anchor_entry] = missing();
					continue;
				}
				const auto requested = static_cast<idx_t>((hi - lo) / step + 1);
				const auto first = lo >= 0 ? lo : lo + ((-lo + step - 1) / step) * step;
				const auto last = MinValue<int64_t>(hi, count - 1);
				const auto present = first <= last ? static_cast<idx_t>((last - first) / step + 1) : 0;
				const auto absent = frame.cyclic ? 0 : requested - present;
				if ((frame.all && absent > 0) || (!frame.cyclic && present == 0 && !node.fallback)) {
					result.entries[anchor_entry] = missing();
					continue;
				}
				if (node.fallback && absent) {
					result.refinements[anchor_entry].push_back(Value::BOOLEAN(true));
					SignatureReads(*node.fallback, node_index, anchor, result.refinements[anchor_entry]);
				}
				string key = to_string(lo) + ":" + to_string(hi);
				if (absent && node.fallback && !node.fallback->IsFoldable()) {
					key += ":" + to_string(anchor.row);
				}
				auto cached = windows.find(key);
				if (cached != windows.end()) {
					result.entries[anchor_entry] = cached->second;
					continue;
				}
				const auto reads = frame.cyclic ? requested : present + (node.fallback ? absent : 0);
				if (reads > limit || visits > limit - reads) {
					result.entries[anchor_entry] = ErrorValue(
					    type,
					    ErrorData(InvalidInputException(
					        "DECIDE: %s reads more than %llu values over its ranges, above decide_frame_limit (%llu). "
					        "Narrow the range, or raise the setting.",
					        frame.text, limit, limit)));
					continue;
				}
				visits += reads;
				vector<DecideTreeValuePtr> members;
				const idx_t argument_count = node.function ? node.function->children.size() : 1;
				vector<vector<DecideTreeValuePtr>> arguments(argument_count);
				auto append = [&](DecideTreeValuePtr value, const DecideDescentEntry &source, bool fallback) {
					members.push_back(value);
					for (idx_t argument = 0; argument < argument_count; argument++) {
						if (argument == 0) {
							arguments[argument].push_back(value);
						} else if (!fallback && argument < node.arguments.size()) {
							arguments[argument].push_back(
							    ExpressionValue(*node.arguments[argument], node_index, source));
						} else {
							arguments[argument].push_back(
							    UnsupportedValue(type, "a frame aggregate argument was not retained"));
						}
					}
				};
				if (frame.cyclic) {
					for (idx_t visit = 0; visit < requested; visit++) {
						const auto position =
						    static_cast<idx_t>(((lo + static_cast<int64_t>(visit) * step) % count + count) % count);
						append(selected(position), container.entries[timeline.positions[position].entries[0]], false);
					}
				} else {
					for (int64_t position = first; position <= last; position += step) {
						append(selected(position), container.entries[timeline.positions[position].entries[0]], false);
					}
					if (node.fallback && absent) {
						auto fallback = ExpressionValue(*node.fallback, node_index, anchor);
						// A NULL fallback supplies no missing position. A position that exists with a NULL payload
						// still exists, and COUNT over those positions is 0; COUNT over no position is absent (D84).
						if (fallback->kind != DecideTreeValueKind::ABSENT &&
						    !(fallback->kind == DecideTreeValueKind::DATA && fallback->data.IsNull())) {
							for (idx_t visit = 0; visit < absent; visit++) {
								append(fallback, anchor, true);
							}
						}
					}
				}
				auto value = members.empty() ? missing() : Reduce(node_index, node.function.get(), arguments, members);
				windows.emplace(key, value);
				result.entries[anchor_entry] = std::move(value);
			}
		}
	}
	for (auto &value : result.entries) {
		if (!value) {
			value = missing();
		}
	}
	if (!frame.partition_subject.empty()) {
		struct Partition {
			vector<idx_t> entries;
			DecideTreeValuePtr error;
		};
		vector<Partition> partitions;
		unordered_map<hash_t, vector<idx_t>> buckets;
		for (idx_t row = 0; row < container.entries.size(); row++) {
			auto &entry = container.entries[row];
			if (!entry.partition_present) {
				continue;
			}
			hash_t hash = 0;
			for (auto &value : entry.partition) {
				hash ^= value.Hash() + 0x9e3779b9 + (hash << 6) + (hash >> 2);
			}
			auto &bucket = buckets[hash];
			idx_t partition = DConstants::INVALID_INDEX;
			for (auto candidate : bucket) {
				auto &first = container.entries[partitions[candidate].entries[0]];
				bool equal = first.partition.size() == entry.partition.size();
				for (idx_t i = 0; equal && i < entry.partition.size(); i++) {
					equal = Value::NotDistinctFrom(first.partition[i], entry.partition[i]);
				}
				if (equal) {
					partition = candidate;
					break;
				}
			}
			if (partition == DConstants::INVALID_INDEX) {
				partition = partitions.size();
				partitions.emplace_back();
				bucket.push_back(partition);
			}
			auto &current = partitions[partition];
			if (!current.entries.empty() && !current.error) {
				auto &first = container.entries[current.entries[0]];
				if (first.slot != entry.slot) {
					vector<string> names;
					for (auto &element : DecideKeyElements(tree.keys, frame.partition_key)) {
						names.push_back(element.name);
					}
					auto describe = [](const vector<Value> &values) {
						string text;
						for (auto &value : values) {
							text += (text.empty() ? "" : ", ") + DecideSqlLiteral(value);
						}
						return values.size() > 1 ? "(" + text + ")" : text;
					};
					auto timeline_names = StringUtil::Join(frame.timeline_names, ", ");
					if (frame.timeline_names.size() > 1) {
						timeline_names = "(" + timeline_names + ")";
					}
					auto spans =
					    names.empty() ? string("Its rows span") : DescribeKeyValues(names, first.partition) + " spans";
					auto timelines =
					    describe(container.groups[first.slot].key) + ", " + describe(container.groups[entry.slot].key);
					current.error = ErrorValue(
					    type, ErrorData(InvalidInputException("DECIDE: %s. %s %s = %s.", frame.partition_subject, spans,
					                                          timeline_names, timelines)));
				}
			}
			current.entries.push_back(row);
		}
		for (auto &partition : partitions) {
			if (partition.error) {
				for (auto entry : partition.entries) {
					result.entries[entry] = partition.error;
				}
			}
		}
	}
}

void DecideTreeEvaluation::Evaluate() {
	if (evaluated) {
		return;
	}
	const char *injection = std::getenv("DECIDB_TREE_INJECT");
	idx_t injected_node = DConstants::INVALID_INDEX;
	string injected_data;
	if (injection) {
		auto parts = StringUtil::Split(string(injection), ':');
		if (parts.size() != 3 || parts[1] != "data") {
			throw InvalidInputException("DECIDB_TREE_INJECT: expected node:data:value");
		}
		try {
			size_t consumed = 0;
			injected_node = std::stoull(parts[0], &consumed);
			if (consumed != parts[0].size() || injected_node >= tree.nodes.size()) {
				throw InvalidInputException("DECIDB_TREE_INJECT: node is outside this tree");
			}
		} catch (std::invalid_argument &) {
			throw InvalidInputException("DECIDB_TREE_INJECT: node must be an unsigned node index");
		} catch (std::out_of_range &) {
			throw InvalidInputException("DECIDB_TREE_INJECT: node is outside this tree");
		}
		injected_data = parts[2];
	}
	for (auto node : tree.eval_order) {
		switch (tree.nodes[node]->kind) {
		case DecideNodeKind::SCOPE:
			Scope(node);
			break;
		case DecideNodeKind::REDUCER:
			Reducer(node);
			break;
		case DecideNodeKind::FRAME:
			Frame(node);
			break;
		}
		// This test-only perturbation happens before a parent is computed. A model emitted from an unrelated flat
		// registry cannot react to it, whereas a parent reading this node's synthesized data necessarily does (L7).
		if (node == injected_node) {
			bool changed = false;
			auto replace = [&](const vector<DecideTreeValuePtr> &values) {
				for (auto &value : values) {
					if (value && value->kind == DecideTreeValueKind::DATA) {
						value->data = Value(injected_data).DefaultCastAs(value->type);
						changed = true;
					}
				}
			};
			replace(results[node].groups);
			replace(results[node].entries);
			if (!changed) {
				throw InvalidInputException("DECIDB_TREE_INJECT: node %llu computed no data value", node);
			}
		}
	}
	evaluated = true;
}

static void RequireValue(const DecideTreeValuePtr &value, DecideErrorLedger &errors, DecideErrorRank fallback) {
	if (!value) {
		throw InternalException("DECIDE tree: a root reads a value that was not computed");
	}
	if (value->kind == DecideTreeValueKind::ERROR) {
		auto rank = value->error_ranked ? value->error_rank : fallback;
		if (value->error_rank.phase != DecideErrorPhase::INVALID) {
			rank.phase = value->error_rank.phase;
		}
		errors.Add(rank, value->error);
	}
	if (value->kind == DecideTreeValueKind::UNSUPPORTED) {
		errors.Add(fallback, ErrorData(NotImplementedException("DECIDE tree evaluation: %s", value->operation)));
	}
	for (auto &operand : value->operands) {
		RequireValue(operand, errors, fallback);
	}
}

void DecideTreeEvaluation::RequireSupported() const {
	if (!evaluated) {
		throw InternalException("DECIDE tree: root values requested before bottom-up evaluation");
	}
	auto errors = descent.Errors();
	for (auto root : tree.roots) {
		auto &node = *tree.nodes[root];
		auto &result = results[root];
		for (idx_t entry = 0; entry < result.entries.size(); entry++) {
			auto &value = result.entries[entry];
			if (!value->Present()) {
				continue;
			}
			DecideErrorRank rank {node.value_phase, node.value_order, 0, descent.Containers()[root].entries[entry].row};
			RequireValue(value, errors, rank);
			if (!result.rhs.empty()) {
				rank.expression = 1;
				RequireValue(result.rhs[entry], errors, rank);
			}
		}
	}
	errors.ThrowIfAny();
}

string DecideTreeEvaluation::Dump() const {
	std::ostringstream out;
	for (idx_t node = 0; node < tree.nodes.size(); node++) {
		auto &result = results[node];
		out << "node " << node << " " << tree.nodes[node]->text << "\n";
		for (idx_t group = 0; group < result.groups.size(); group++) {
			out << "  group " << group << " -> " << result.groups[group]->ToString() << "\n";
		}
		for (idx_t entry = 0; entry < result.entries.size(); entry++) {
			out << "  row " << descent.Containers()[node].entries[entry].row << " -> "
			    << result.entries[entry]->ToString();
			if (!result.rhs.empty()) {
				out << " " << ExpressionTypeToString(tree.nodes[node]->comparison_type) << " "
				    << result.rhs[entry]->ToString();
			}
			out << "\n";
		}
	}
	return out.str();
}

} // namespace duckdb
