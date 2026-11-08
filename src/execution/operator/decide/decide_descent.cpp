#include "duckdb/execution/operator/decide/decide_descent.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>

namespace duckdb {

static hash_t DescentHash(idx_t parent, const vector<Value> &values) {
	hash_t result = Hash<uint64_t>(parent);
	for (auto &value : values) {
		result = MurmurHash64(result ^ value.Hash());
	}
	return result;
}

static bool DescentEqual(const vector<Value> &left, const vector<Value> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (idx_t i = 0; i < left.size(); i++) {
		if (!Value::NotDistinctFrom(left[i], right[i])) {
			return false;
		}
	}
	return true;
}

static bool DescentLess(const Value &left, const Value &right) {
	if (left.IsNull() || right.IsNull()) {
		return left.IsNull() && !right.IsNull();
	}
	return left < right;
}

static bool DescentKeyLess(const vector<Value> &left, const vector<Value> &right) {
	for (idx_t i = 0; i < MinValue(left.size(), right.size()); i++) {
		if (!Value::NotDistinctFrom(left[i], right[i])) {
			return DescentLess(left[i], right[i]);
		}
	}
	return left.size() < right.size();
}

DecideDescent::DecideDescent(const DecideTree &tree_p, ClientContext &context_p)
    : tree(tree_p), context(context_p), containers(tree.nodes.size()), indexes(tree.nodes.size()),
      decision_groups(tree.keyed_scopes.size()), decision_rows(tree.keyed_scopes.size()),
      decision_recorded(tree.keyed_scopes.size()), decision_indexes(tree.keyed_scopes.size()),
      failed_rows(tree.nodes.size()) {
}

void DecideDescent::SetDecisionResolver(std::function<idx_t(idx_t, idx_t)> resolver) {
	decision_resolver = std::move(resolver);
}

vector<Value> DecideDescent::Evaluate(const Expression &expression, DataChunk &chunk,
                                     const vector<RoutingRow> &rows) {
	vector<Value> result;
	if (rows.empty()) {
		return result;
	}
	SelectionVector selection(rows.size());
	for (idx_t i = 0; i < rows.size(); i++) {
		selection.set_index(i, rows[i].row);
	}
	DataChunk selected;
	selected.InitializeEmpty(chunk.GetTypes());
	selected.Slice(chunk, selection, rows.size());
	auto rank = current_rank;
	current_rank.expression++;
	try {
		ExpressionExecutor executor(context, expression);
		Vector values(expression.return_type);
		executor.ExecuteExpression(selected, values);
		result.reserve(rows.size());
		for (idx_t i = 0; i < rows.size(); i++) {
			result.push_back(values.GetValue(i));
		}
		return result;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::OUT_OF_MEMORY ||
		    error.Type() == ExceptionType::FATAL || error.Type() == ExceptionType::INTERNAL) {
			throw;
		}
	}
	// A failed vector identifies neither the bad row nor the other rows' values. Retry that selection row by row
	// and retain reached errors until the bottom-up reporting order is known.
	for (idx_t i = 0; i < rows.size(); i++) {
		if (Failed(rows[i].row)) {
			result.emplace_back(expression.return_type);
			continue;
		}
		SelectionVector one(1);
		one.set_index(0, i);
		DataChunk input;
		input.InitializeEmpty(selected.GetTypes());
		input.Slice(selected, one, 1);
		try {
			ExpressionExecutor executor(context, expression);
			Vector value(expression.return_type);
			executor.ExecuteExpression(input, value);
			result.push_back(value.GetValue(0));
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INTERNAL) {
				throw;
			}
			rank.row = active_row_base + rows[i].row;
			errors.Add(rank, error);
			if (active_node < failed_rows.size()) {
				failed_rows[active_node].insert(rank.row);
			}
			result.emplace_back(expression.return_type);
		}
	}
	return result;
}

//! SQL's three truth values are kept until the whole predicate is complete.
//! In particular UNKNOWN OR TRUE is TRUE. Written order is preserved and a
//! later arm is evaluated only where an earlier arm did not decide the result.
vector<uint8_t> DecideDescent::Truth(const Expression &expression, DataChunk &chunk,
                                    const vector<RoutingRow> &rows) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		const bool is_and = expression.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
		vector<uint8_t> result(rows.size(), is_and ? 1 : 0);
		for (auto &child : conjunction.children) {
			vector<RoutingRow> unresolved;
			vector<idx_t> positions;
			for (idx_t i = 0; i < rows.size(); i++) {
				if (!Failed(rows[i].row) && ((is_and && result[i] != 0) || (!is_and && result[i] != 1))) {
					unresolved.push_back(rows[i]);
					positions.push_back(i);
				}
			}
			if (unresolved.empty()) {
				break;
			}
			auto values = Truth(*child, chunk, unresolved);
			for (idx_t i = 0; i < values.size(); i++) {
				auto &current = result[positions[i]];
				const auto next = values[i];
				if (is_and) {
					current = current == 0 || next == 0 ? 0 : (current == 2 || next == 2 ? 2 : 1);
				} else {
					current = current == 1 || next == 1 ? 1 : (current == 2 || next == 2 ? 2 : 0);
				}
			}
		}
		return result;
	}
	if (expression.GetExpressionType() == ExpressionType::OPERATOR_NOT &&
	    expression.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op = expression.Cast<BoundOperatorExpression>();
		auto result = Truth(*op.children[0], chunk, rows);
		for (auto &value : result) {
			if (value != 2) {
				value = 1 - value;
			}
		}
		return result;
	}
	auto values = Evaluate(expression, chunk, rows);
	vector<uint8_t> result;
	result.reserve(values.size());
	for (auto &value : values) {
		result.push_back(value.IsNull() ? 2 : (value.GetValue<bool>() ? 1 : 0));
	}
	return result;
}

vector<vector<Value>> DecideDescent::Key(idx_t key, DataChunk &chunk, const vector<RoutingRow> &rows,
                                        vector<bool> &present) {
	vector<vector<Value>> result(rows.size());
	present.assign(rows.size(), true);
	for (auto &element : DecideKeyElements(tree.keys, key)) {
		auto values = Evaluate(element.Grouping(), chunk, rows);
		for (idx_t i = 0; i < rows.size(); i++) {
			if (element.null_mode == DecideKeyNullMode::NO_GROUP && values[i].IsNull()) {
				present[i] = false;
			}
			result[i].push_back(std::move(values[i]));
		}
	}
	return result;
}

vector<vector<Value>> DecideDescent::Signature(const DecideNode &node, DataChunk &chunk,
                                              const vector<RoutingRow> &rows, idx_t row_base) {
	vector<vector<Value>> result(rows.size());
	for (auto &field : node.signature) {
		switch (field.kind) {
		case DecideSignatureFieldKind::VALUE: {
			if (!field.value) {
				throw InternalException("DECIDE tree: a signature value has no expression");
			}
			auto values = Evaluate(*field.value, chunk, rows);
			for (idx_t i = 0; i < rows.size(); i++) {
				result[i].push_back(std::move(values[i]));
			}
			break;
		}
		case DecideSignatureFieldKind::BY_KEY: {
			vector<bool> present;
			auto values = Key(field.key, chunk, rows, present);
			for (idx_t i = 0; i < rows.size(); i++) {
				for (auto &value : values[i]) {
					result[i].push_back(std::move(value));
				}
			}
			break;
		}
		case DecideSignatureFieldKind::DECISION: {
			vector<idx_t> classes;
			idx_t scope = DConstants::INVALID_INDEX;
			bool global = false;
			if (field.decision < tree.variable_scopes.size()) {
				auto &variable = tree.variable_scopes[field.decision];
				global = variable.IsGlobal();
				if (variable.IsKeyed() || variable.scope == DecideVarScope::INLINED) {
					scope = variable.keyed_scope_idx;
					if (scope < tree.keyed_scopes.size() && !decision_resolver) {
						classes = DecisionClasses(field.decision, chunk, rows, row_base);
					}
				}
			}
			for (idx_t i = 0; i < rows.size(); i++) {
				const auto row = row_base + rows[i].row;
				idx_t instance = global ? 0 : row;
				if (decision_resolver) {
					instance = decision_resolver(field.decision, row);
				} else if (scope < tree.keyed_scopes.size()) {
					instance = classes[i];
				}
				result[i].push_back(Value::UBIGINT(field.decision));
				result[i].push_back(Value::UBIGINT(instance));
			}
			break;
		}
		case DecideSignatureFieldKind::FRAME: {
			const DecideNode *frame = nullptr;
			for (auto &candidate : tree.nodes) {
				if (candidate->kind == DecideNodeKind::FRAME && candidate->frame == field.frame) {
					frame = candidate.get();
					break;
				}
			}
			if (!frame || !frame->over) {
				throw InternalException("DECIDE tree: signature names missing frame %llu", field.frame);
			}
			vector<bool> present;
			auto keys = Key(frame->key, chunk, rows, present);
			vector<Value> order;
			if (!frame->frame_spec || frame->frame_spec->Relative()) {
				order = Evaluate(*frame->over, chunk, rows);
			}
			for (idx_t i = 0; i < rows.size(); i++) {
				for (auto &value : keys[i]) {
					result[i].push_back(std::move(value));
				}
				if (!order.empty()) {
					result[i].push_back(std::move(order[i]));
				}
			}
			break;
		}
		}
	}
	return result;
}

idx_t DecideDescent::FindGroup(idx_t node, idx_t parent, vector<Value> key, idx_t row) {
	auto &container = containers[node];
	auto &bucket = indexes[node].groups[DescentHash(parent, key)];
	for (auto slot : bucket) {
		auto &group = container.groups[slot];
		if (group.parent_slot == parent && DescentEqual(group.key, key)) {
			group.first_row = MinValue(group.first_row, row);
			return slot;
		}
	}
	const auto slot = container.groups.size();
	DecideDescentGroup group;
	group.parent_slot = parent;
	group.key = std::move(key);
	group.first_row = row;
	container.groups.push_back(std::move(group));
	bucket.push_back(slot);
	return slot;
}

idx_t DecideDescent::FindDecisionGroup(idx_t scope, vector<Value> key, idx_t row) {
	auto &groups = decision_groups[scope];
	auto &bucket = decision_indexes[scope][DescentHash(0, key)];
	for (auto slot : bucket) {
		if (DescentEqual(groups[slot].key, key)) {
			groups[slot].first_row = MinValue(groups[slot].first_row, row);
			return slot;
		}
	}
	const auto slot = groups.size();
	DecideDescentGroup group;
	group.parent_slot = DConstants::INVALID_INDEX;
	group.key = std::move(key);
	group.first_row = row;
	groups.push_back(std::move(group));
	bucket.push_back(slot);
	return slot;
}

vector<idx_t> DecideDescent::DecisionClasses(idx_t variable, DataChunk &chunk, const vector<RoutingRow> &rows,
                                             idx_t row_base) {
	const auto scope = tree.variable_scopes[variable].keyed_scope_idx;
	auto &mapping = decision_rows[scope];
	auto &recorded = decision_recorded[scope];
	mapping.resize(MaxValue<idx_t>(mapping.size(), row_base + chunk.size()), DConstants::INVALID_INDEX);
	recorded.resize(mapping.size(), false);
	vector<RoutingRow> missing;
	for (auto &row : rows) {
		if (!recorded[row_base + row.row]) {
			missing.push_back(row);
		}
	}
	if (!missing.empty()) {
		const DecideNode *definition = nullptr;
		if (variable < tree.definition_nodes.size() && !tree.definition_nodes[variable].empty()) {
			definition = tree.nodes[tree.definition_nodes[variable][0]].get();
		}
		vector<bool> present;
		auto keys = Key(definition ? definition->key : tree.keyed_scopes[scope].key, chunk, missing, present);
		if (definition) {
			auto variants = Signature(*definition, chunk, missing, row_base);
			for (idx_t i = 0; i < missing.size(); i++) {
				for (auto &value : variants[i]) {
					keys[i].push_back(std::move(value));
				}
			}
		}
		for (idx_t i = 0; i < missing.size(); i++) {
			const auto row = row_base + missing[i].row;
			if (present[i]) {
				mapping[row] = FindDecisionGroup(scope, std::move(keys[i]), row);
			}
			recorded[row] = true;
		}
	}
	vector<idx_t> result;
	result.reserve(rows.size());
	for (auto &row : rows) {
		result.push_back(mapping[row_base + row.row]);
	}
	return result;
}

void DecideDescent::Insert(idx_t node, DecideDescentEntry entry, bool distinct) {
	auto &container = containers[node];
	if (distinct) {
		auto &bucket = indexes[node].signatures[DescentHash(entry.slot, entry.signature)];
		for (auto index : bucket) {
			auto &existing = container.entries[index];
			if (existing.slot == entry.slot && DescentEqual(existing.signature, entry.signature)) {
				entry.instance = existing.instance;
				if (DescentEqual(existing.columns, entry.columns)) {
					return;
				}
			}
		}
		bucket.push_back(container.entries.size());
	}
	if (entry.instance == DConstants::INVALID_INDEX) {
		entry.instance = container.instance_entries.size();
		container.instance_entries.push_back(container.entries.size());
	}
	container.groups[entry.slot].entries.push_back(container.entries.size());
	container.entries.push_back(std::move(entry));
}

void DecideDescent::Visit(idx_t node_index, DataChunk &chunk, idx_t row_base,
                           const vector<RoutingRow> &incoming) {
	auto &node = *tree.nodes[node_index];
	active_node = node_index;
	active_row_base = row_base;
	current_rank = {node.route_phase, node.route_order, 0, DConstants::INVALID_INDEX};
	if (context.interrupted) {
		throw InterruptException();
	}
	auto &container = containers[node_index];
	auto &index = indexes[node_index];
	vector<RoutingRow> rows;
	for (auto &row : incoming) {
		const auto hash = MurmurHash64(Hash<uint64_t>(row_base + row.row) ^ Hash<uint64_t>(row.parent_slot));
		auto &seen = index.seen[hash];
		const pair<idx_t, idx_t> identity(row_base + row.row, row.parent_slot);
		if (std::find(seen.begin(), seen.end(), identity) == seen.end()) {
			seen.push_back(identity);
			rows.push_back(row);
		}
	}
	container.rows_in += rows.size();
	for (auto &condition : node.when) {
		auto truth = Truth(*condition, chunk, rows);
		vector<RoutingRow> kept;
		for (idx_t i = 0; i < rows.size(); i++) {
			if (truth[i] == 1 && !Failed(rows[i].row)) {
				kept.push_back(rows[i]);
			}
		}
		rows = std::move(kept);
		if (rows.empty()) {
			return;
		}
	}
	if (rows.empty()) {
		return;
	}
	vector<bool> present;
	auto keys = Key(node.key, chunk, rows, present);
	vector<RoutingRow> children;
	vector<RoutingRow> stored;
	vector<idx_t> slots;
	for (idx_t i = 0; i < rows.size(); i++) {
		if (Failed(rows[i].row)) {
			continue;
		}
		auto child = rows[i];
		if (!present[i]) {
			if (node.kind == DecideNodeKind::SCOPE) {
				children.push_back(child);
			}
			continue;
		}
		auto key = std::move(keys[i]);
		if (node.kind == DecideNodeKind::SCOPE && node.per == DecideTreePer::ROW) {
			key.push_back(Value::UBIGINT(row_base + rows[i].row));
		}
		// An E's classes lie within the group it feeds. A G's or O's key is already its whole path key (the path rule,
		// language/guide.md §5.5; DecideReducerPath::KeysOf), so no enclosing slot adds a level. A G a rewrite
		// generates, as S3's tightest bound, is on no reducer's path: the reducers its operand reads keep their groups.
		const auto within = node.kind == DecideNodeKind::SCOPE ? rows[i].parent_slot : DConstants::INVALID_INDEX;
		const auto slot = FindGroup(node_index, within, std::move(key), row_base + rows[i].row);
		stored.push_back(rows[i]);
		slots.push_back(slot);
		if (node.kind != DecideNodeKind::SCOPE) {
			child.parent_slot = slot;
		}
		children.push_back(child);
	}
	// A node records the classes of the keyed decisions it defines or reads where it is reached. Its sigma need not
	// name them: a root's names only what its written clause reads.
	for (auto variable : node.decisions) {
		if (variable < tree.variable_scopes.size() && tree.variable_scopes[variable].IsKeyed()) {
			DecisionClasses(variable, chunk, stored, row_base);
		}
	}
	auto signatures = Signature(node, chunk, stored, row_base);
	vector<vector<Value>> columns(stored.size());
	for (auto &column : node.columns) {
		auto values = Evaluate(*column, chunk, stored);
		for (idx_t i = 0; i < stored.size(); i++) {
			columns[i].push_back(std::move(values[i]));
		}
	}
	vector<Value> order, order_keys;
	vector<vector<Value>> partitions;
	vector<bool> partition_present;
	if (node.kind == DecideNodeKind::FRAME && node.over) {
		if (node.frame_spec) {
			partitions = Key(node.frame_spec->partition_key, chunk, stored, partition_present);
		}
		order = Evaluate(*node.over, chunk, stored);
		order_keys = order;
		if (node.frame_spec && node.frame_spec->order_collation) {
			DataChunk raw;
			raw.Initialize(context, {node.over->return_type});
			raw.SetCardinality(stored.size());
			for (idx_t i = 0; i < stored.size(); i++) {
				raw.SetValue(0, i, order[i]);
			}
			vector<RoutingRow> positions;
			for (idx_t i = 0; i < stored.size(); i++) {
				positions.push_back({i, 0});
			}
			order_keys = Evaluate(*node.frame_spec->order_collation, raw, positions);
		}
		// Use ORDER BY's comparison bytes, including its bound collation,
		// direction and configured NULL placement, rather than Value's order.
		const auto direction = node.frame_spec && node.frame_spec->descending ? OrderType::DESCENDING
		                                                                    : OrderType::ASCENDING;
		OrderModifiers modifiers(direction,
		                         DBConfig::GetConfig(context).ResolveNullOrder(direction, OrderByNullType::ORDER_DEFAULT));
		const auto type = node.frame_spec && node.frame_spec->order_collation
		                      ? node.frame_spec->order_collation->return_type
		                      : node.over->return_type;
		Vector compared(type, MaxValue<idx_t>(stored.size(), 1));
		for (idx_t i = 0; i < stored.size(); i++) {
			compared.SetValue(i, order_keys[i]);
		}
		Vector encoded(LogicalType::BLOB, MaxValue<idx_t>(stored.size(), 1));
		CreateSortKeyHelpers::CreateSortKey(compared, stored.size(), modifiers, encoded);
		for (idx_t i = 0; i < stored.size(); i++) {
			order_keys[i] = encoded.GetValue(i);
		}
	}
	for (idx_t i = 0; i < stored.size(); i++) {
		if (Failed(stored[i].row)) {
			continue;
		}
		DecideDescentEntry entry;
		entry.row = row_base + stored[i].row;
		entry.parent_slot = stored[i].parent_slot;
		entry.slot = slots[i];
		entry.signature = std::move(signatures[i]);
		entry.columns = std::move(columns[i]);
		if (!order.empty()) {
			entry.order = std::move(order[i]);
			entry.order_key = std::move(order_keys[i]);
			if (!partitions.empty()) {
				entry.partition = std::move(partitions[i]);
				entry.partition_present = partition_present[i];
			}
		}
		Insert(node_index, std::move(entry), node.kind == DecideNodeKind::SCOPE && node.per != DecideTreePer::ROW);
	}
	children.erase(
	    std::remove_if(children.begin(), children.end(), [&](const RoutingRow &row) { return Failed(row.row); }),
	    children.end());
	container.rows_out += children.size();
	for (auto child : node.children) {
		Visit(child, chunk, row_base, children);
	}
}

void DecideDescent::Append(DataChunk &chunk, idx_t row_base) {
	if (finished) {
		throw InternalException("DECIDE tree: a row arrived after top-down construction finished");
	}
	vector<RoutingRow> rows;
	rows.reserve(chunk.size());
	for (idx_t row = 0; row < chunk.size(); row++) {
		rows.push_back({row, DConstants::INVALID_INDEX});
	}
	// A declared decision's dictionary belongs to Source as well as the clauses:
	// it must include every row, even one no constraint reaches. Helper scopes
	// are instead populated when their defining/reading nodes are reached.
	for (idx_t scope = 0; scope < tree.keyed_scopes.size(); scope++) {
		auto &definition = tree.keyed_scopes[scope];
		if (definition.value_link != DConstants::INVALID_INDEX || definition.signature != DConstants::INVALID_INDEX) {
			continue;
		}
		vector<bool> present;
		auto keys = Key(definition.key, chunk, rows, present);
		auto &mapping = decision_rows[scope];
		mapping.resize(row_base + chunk.size(), DConstants::INVALID_INDEX);
		decision_recorded[scope].resize(mapping.size(), true);
		for (idx_t i = 0; i < rows.size(); i++) {
			if (present[i]) {
				mapping[row_base + i] = FindDecisionGroup(scope, std::move(keys[i]), row_base + i);
			}
		}
	}
	for (auto root : tree.roots) {
		Visit(root, chunk, row_base, rows);
	}
	// Seen is chunk-local: containers retain values, not an input-sized visited set.
	for (auto &index : indexes) {
		index.seen.clear();
	}
}

void DecideDescent::Finish() {
	if (finished) {
		return;
	}
	for (auto &groups : decision_groups) {
		vector<idx_t> order(groups.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(), [&](idx_t left, idx_t right) {
			return DescentKeyLess(groups[left].key, groups[right].key);
		});
		for (idx_t rank = 0; rank < order.size(); rank++) {
			groups[order[rank]].rank = rank;
		}
	}
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		auto &node = *tree.nodes[n];
		auto &container = containers[n];
		vector<idx_t> order(container.groups.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(), [&](idx_t left, idx_t right) {
			auto &l = container.groups[left];
			auto &r = container.groups[right];
			return l.parent_slot != r.parent_slot ? l.parent_slot < r.parent_slot : DescentKeyLess(l.key, r.key);
		});
		for (idx_t rank = 0; rank < order.size(); rank++) {
			container.groups[order[rank]].rank = rank;
		}
		if (node.kind != DecideNodeKind::FRAME) {
			continue;
		}
		for (auto slot : order) {
			auto entries = container.groups[slot].entries;
			std::stable_sort(entries.begin(), entries.end(), [&](idx_t left, idx_t right) {
				auto &l = container.entries[left];
				auto &r = container.entries[right];
				if (Value::NotDistinctFrom(l.order_key, r.order_key)) {
					return l.row < r.row;
				}
				return DescentLess(l.order_key, r.order_key);
			});
			DecideDescentTimeline timeline;
			timeline.slot = slot;
			for (auto index : entries) {
				auto &entry = container.entries[index];
				if (timeline.positions.empty() ||
				    !Value::NotDistinctFrom(container.entries[timeline.positions.back().entries[0]].order_key,
				                            entry.order_key)) {
					DecideDescentPosition position;
					position.order = entry.order;
					position.entries.push_back(index);
					timeline.positions.push_back(std::move(position));
				} else {
					auto &position = timeline.positions.back();
					position.conflict |= !DescentEqual(container.entries[position.entries[0]].signature, entry.signature);
					position.entries.push_back(index);
				}
			}
			container.timelines.push_back(std::move(timeline));
		}
	}
	finished = true;
}

string DecideDescent::Dump() const {
	std::ostringstream out;
	for (idx_t n = 0; n < containers.size(); n++) {
		auto &container = containers[n];
		auto &node = *tree.nodes[n];
		const auto kind = node.kind == DecideNodeKind::SCOPE ? "E" : node.kind == DecideNodeKind::REDUCER ? "G" : "O";
		out << "node " << n << " " << kind << " rows " << container.rows_in << " -> " << container.rows_out
		    << " groups=" << container.groups.size() << " entries=" << container.entries.size()
		    << " instances=" << container.instance_entries.size() << " text=" << node.text << "\n";
		for (idx_t slot = 0; slot < container.groups.size(); slot++) {
			auto &group = container.groups[slot];
			out << "  group " << slot << " rank " << group.rank << " parent " << group.parent_slot << " key=(";
			for (idx_t i = 0; i < group.key.size(); i++) {
				out << (i ? ", " : "") << group.key[i].ToString();
			}
			out << ")\n";
		}
		for (auto &entry : container.entries) {
			out << "  row " << entry.row << " parent " << entry.parent_slot << " slot " << entry.slot
			    << " instance " << entry.instance << " (";
			for (idx_t i = 0; i < entry.signature.size(); i++) {
				out << (i ? ", " : "") << entry.signature[i].ToString();
			}
			out << ") data=(";
			for (idx_t i = 0; i < entry.columns.size(); i++) {
				out << (i ? ", " : "") << entry.columns[i].ToString();
			}
			out << ")";
			if (node.kind == DecideNodeKind::FRAME) {
				out << " over=" << entry.order.ToString();
			}
			out << "\n";
		}
		for (auto &timeline : container.timelines) {
			out << "  timeline " << timeline.slot << " {";
			for (auto &position : timeline.positions) {
				out << position.order.ToString() << ":" << position.entries.size()
				    << (position.conflict ? "!" : "") << ";";
			}
			out << "}\n";
		}
	}
	return out.str();
}

} // namespace duckdb
