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

//! A value's place in a key's order: NULL after every value, as ORDER BY places it and flat numbers a key's groups
//! (NumberGroupsByValue), so a group's rank is flat's group number.
static bool DescentLess(const Value &left, const Value &right) {
	if (left.IsNull() || right.IsNull()) {
		return !left.IsNull() && right.IsNull();
	}
	return left < right;
}

bool DecideDescentKeyLess(const vector<Value> &left, const vector<Value> &right) {
	for (idx_t i = 0; i < MinValue(left.size(), right.size()); i++) {
		if (!Value::NotDistinctFrom(left[i], right[i])) {
			return DescentLess(left[i], right[i]);
		}
	}
	return left.size() < right.size();
}

//! The bytes a least spelling is taken by, ORDER BY's for `values` ascending with NULL last, as flat takes it
//! (LeastSpellingRows): equal bytes are one spelling, so the least does not depend on the order rows are read in.
static vector<string> SpellingKeys(const vector<Value> &values, const LogicalType &type) {
	Vector spelled(type, MaxValue<idx_t>(values.size(), 1));
	for (idx_t i = 0; i < values.size(); i++) {
		spelled.SetValue(i, values[i]);
	}
	Vector encoded(LogicalType::BLOB, MaxValue<idx_t>(values.size(), 1));
	CreateSortKeyHelpers::CreateSortKey(spelled, values.size(),
	                                    OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST), encoded);
	vector<string> result;
	result.reserve(values.size());
	auto keys = FlatVector::GetData<string_t>(encoded);
	for (idx_t i = 0; i < values.size(); i++) {
		result.push_back(keys[i].GetString());
	}
	return result;
}

//! The types of `key`'s values as its rows group on them (DecideKeyElement::Grouping).
static vector<LogicalType> DescentKeyTypes(const DecideTree &tree, idx_t key) {
	vector<LogicalType> types;
	for (auto &element : DecideKeyElements(tree.keys, key)) {
		types.push_back(element.Grouping().return_type);
	}
	return types;
}

//! The O of frame `frame`, null if none holds it.
static const DecideNode *DescentFrameNode(const DecideTree &tree, idx_t frame) {
	for (auto &candidate : tree.nodes) {
		if (candidate->kind == DecideNodeKind::FRAME && candidate->frame == frame) {
			return candidate.get();
		}
	}
	return nullptr;
}

//! Whether a frame field of a signature reads its order key: relative to the anchor, or with no spec.
static bool DescentFrameReadsOrder(const DecideNode &frame) {
	return !frame.frame_spec || frame.frame_spec->Relative();
}

//! The columns of `node`'s signature values, as Signature computes them: a value, a decision and its instance, a
//! key's elements, a frame's timeline and, read relative to the anchor, its order key.
static vector<LogicalType> DescentSignatureTypes(const DecideTree &tree, const DecideNode &node) {
	vector<LogicalType> types;
	for (auto &field : node.signature) {
		switch (field.kind) {
		case DecideSignatureFieldKind::VALUE:
			if (field.value) {
				types.push_back(field.value->return_type);
			}
			break;
		case DecideSignatureFieldKind::BY_KEY: {
			auto key = DescentKeyTypes(tree, field.key);
			types.insert(types.end(), key.begin(), key.end());
			break;
		}
		case DecideSignatureFieldKind::DECISION:
			types.push_back(LogicalType::UBIGINT);
			types.push_back(LogicalType::UBIGINT);
			break;
		case DecideSignatureFieldKind::FRAME: {
			auto frame = DescentFrameNode(tree, field.frame);
			if (!frame || !frame->over) {
				break;
			}
			auto key = DescentKeyTypes(tree, frame->key);
			types.insert(types.end(), key.begin(), key.end());
			if (DescentFrameReadsOrder(*frame)) {
				types.push_back(frame->over->return_type);
			}
			break;
		}
		}
	}
	return types;
}

static vector<LogicalType> DescentTypes(const vector<unique_ptr<Expression>> &expressions) {
	vector<LogicalType> types;
	for (auto &expression : expressions) {
		types.push_back(expression->return_type);
	}
	return types;
}

//! Row `i` of `values`' columns `begin` to `end`.
static vector<Value> DescentRowValues(DataChunk &values, idx_t begin, idx_t end, idx_t i) {
	vector<Value> result;
	result.reserve(end - begin);
	for (idx_t c = begin; c < end; c++) {
		result.push_back(values.data[c].GetValue(i));
	}
	return result;
}

static vector<Value> DescentColumnValues(Vector &values, idx_t count) {
	vector<Value> result;
	result.reserve(count);
	for (idx_t i = 0; i < count; i++) {
		result.push_back(values.GetValue(i));
	}
	return result;
}

//! A UBIGINT column of `values`.
static void DescentIds(Vector &result, const vector<idx_t> &values) {
	auto data = FlatVector::GetData<uint64_t>(result);
	for (idx_t i = 0; i < values.size(); i++) {
		data[i] = values[i];
	}
}

DecideDescent::DecideDescent(const DecideTree &tree_p, ClientContext &context_p)
    : tree(tree_p), context(context_p), containers(tree.nodes.size()), group_maps(tree.nodes.size()),
      entry_maps(tree.nodes.size()), instance_maps(tree.nodes.size()), signature_types(tree.nodes.size()),
      shared(tree.nodes.size(), false), seen(tree.nodes.size()), landed(tree.nodes.size()),
      decision_groups(tree.keyed_scopes.size()), decision_rows(tree.keyed_scopes.size()),
      decision_recorded(tree.keyed_scopes.size()), decision_layouts(tree.keyed_scopes.size()),
      spellings(tree.keys.size()), failed_rows(tree.nodes.size()) {
	// The paths that reach each node, roots first: a node reached by one reads each of a window's rows once.
	vector<idx_t> paths(tree.nodes.size(), 0);
	std::function<void(idx_t)> reach = [&](idx_t node) {
		paths[node]++;
		for (auto child : tree.nodes[node]->children) {
			if (paths[child] < 2) {
				reach(child);
			}
		}
	};
	for (auto root : tree.roots) {
		reach(root);
	}
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		auto &node = *tree.nodes[n];
		shared[n] = paths[n] > 1;
		signature_types[n] = DescentSignatureTypes(tree, node);
		const auto scope = node.kind == DecideNodeKind::SCOPE;
		// A node per row has no key (DecideTreePer): each row is a slot of its own.
		D_ASSERT(!scope || node.per != DecideTreePer::ROW || node.key == DConstants::INVALID_INDEX);
		if (scope && node.per == DecideTreePer::ROW) {
			continue;
		}
		vector<LogicalType> types;
		if (scope) {
			types.push_back(LogicalType::UBIGINT);
		}
		auto key = DescentKeyTypes(tree, node.key);
		types.insert(types.end(), key.begin(), key.end());
		if (!types.empty()) {
			group_maps[n] = make_uniq<DecideKeyMap>(context, types);
		}
		if (!scope) {
			continue;
		}
		// A set E: its instances by (slot, signature), its entries by those, its carried columns and its reads.
		vector<LogicalType> instance {LogicalType::UBIGINT};
		instance.insert(instance.end(), signature_types[n].begin(), signature_types[n].end());
		instance_maps[n] = make_uniq<DecideKeyMap>(context, instance);
		auto entry = instance;
		auto columns = DescentTypes(node.columns);
		entry.insert(entry.end(), columns.begin(), columns.end());
		entry.insert(entry.end(), node.dependencies.size(), LogicalType::UBIGINT);
		entry_maps[n] = make_uniq<DecideKeyMap>(context, entry);
	}
}

DecideDescent::~DecideDescent() {
}

void DecideDescent::Evaluate(const Expression &expression, DataChunk &chunk, const Reach &rows, Vector &result) {
	if (rows.empty()) {
		return;
	}
	SelectionVector selection(rows.size());
	for (idx_t i = 0; i < rows.size(); i++) {
		selection.set_index(i, rows.rows[i]);
	}
	DataChunk selected;
	selected.InitializeEmpty(chunk.GetTypes());
	selected.Slice(chunk, selection, rows.size());
	auto rank = current_rank;
	current_rank.expression++;
	try {
		auto &executor = executors[&expression];
		if (!executor) {
			executor = make_uniq<ExpressionExecutor>(context, expression);
		}
		executor->ExecuteExpression(selected, result);
		return;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::OUT_OF_MEMORY ||
		    error.Type() == ExceptionType::FATAL || error.Type() == ExceptionType::INTERNAL) {
			throw;
		}
		// An executor that raised is not read again.
		executors.erase(&expression);
	}
	// A failed vector identifies neither the bad row nor the other rows' values. Retry that selection row by row
	// and retain reached errors until the bottom-up reporting order is known.
	Vector retried(expression.return_type);
	result.Reference(retried);
	for (idx_t i = 0; i < rows.size(); i++) {
		if (Failed(rows.rows[i])) {
			FlatVector::SetNull(result, i, true);
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
			result.SetValue(i, value.GetValue(0));
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INTERNAL) {
				throw;
			}
			rank.row = active_row_base + rows.rows[i];
			if (read_block != DecideBlock::ROUTE) {
				rank.expression = DecideBlockExpression(read_block, rank.row, false);
			}
			errors.Add(rank, error);
			// A read flat makes before the terms keeps the row from them; one it makes after them leaves the row in no
			// class of the key, so its terms are still computed.
			if (active_node < failed_rows.size() && read_block <= DecideBlock::FILTERS) {
				failed_rows[active_node].insert(rank.row);
			}
			FlatVector::SetNull(result, i, true);
		}
	}
}

//! SQL's three truth values are kept until the whole predicate is complete.
//! In particular UNKNOWN OR TRUE is TRUE. Written order is preserved and a
//! later arm is evaluated only where an earlier arm did not decide the result.
vector<uint8_t> DecideDescent::Truth(const Expression &expression, DataChunk &chunk, const Reach &rows) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		const bool is_and = expression.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
		vector<uint8_t> result(rows.size(), is_and ? 1 : 0);
		for (auto &child : conjunction.children) {
			Reach unresolved;
			vector<idx_t> positions;
			for (idx_t i = 0; i < rows.size(); i++) {
				if (!Failed(rows.rows[i]) && ((is_and && result[i] != 0) || (!is_and && result[i] != 1))) {
					unresolved.Add(rows.rows[i], rows.parents[i]);
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
	D_ASSERT(expression.return_type.id() == LogicalTypeId::BOOLEAN);
	Vector values(expression.return_type);
	Evaluate(expression, chunk, rows, values);
	vector<uint8_t> result(rows.size());
	UnifiedVectorFormat format;
	values.ToUnifiedFormat(rows.size(), format);
	auto truth = UnifiedVectorFormat::GetData<bool>(format);
	for (idx_t i = 0; i < rows.size(); i++) {
		const auto index = format.sel->get_index(i);
		result[i] = !format.validity.RowIsValid(index) ? 2 : (truth[index] ? 1 : 0);
	}
	return result;
}

void DecideDescent::Key(idx_t key, DataChunk &chunk, const Reach &rows, DataChunk &values, vector<bool> &present,
                        KeySpellings *read) {
	present.assign(rows.size(), true);
	auto &elements = DecideKeyElements(tree.keys, key);
	if (elements.empty()) {
		return;
	}
	values.Initialize(Allocator::Get(context), DescentKeyTypes(tree, key));
	// A collated column groups under its collation and is named by its spelling, read as GROUP BY reads both (D69).
	// The spelling is the same read of the column, at its rank: a row it fails on failed the grouping first.
	vector<vector<Value>> spelled(elements.size());
	vector<vector<string>> sort_keys(elements.size());
	bool collated = false;
	for (idx_t c = 0; c < elements.size(); c++) {
		auto &element = elements[c];
		if (read_block != DecideBlock::ROUTE) {
			current_rank.position = c;
		}
		const auto rank = current_rank.expression;
		Evaluate(element.Grouping(), chunk, rows, values.data[c]);
		if (element.collated) {
			current_rank.expression = rank;
			Vector spelling(element.expression->return_type);
			Evaluate(*element.expression, chunk, rows, spelling);
			spelled[c] = DescentColumnValues(spelling, rows.size());
			sort_keys[c] = SpellingKeys(spelled[c], element.expression->return_type);
			collated = true;
		}
		if (element.null_mode == DecideKeyNullMode::NO_GROUP && !rows.empty()) {
			UnifiedVectorFormat format;
			values.data[c].ToUnifiedFormat(rows.size(), format);
			for (idx_t i = 0; i < rows.size(); i++) {
				if (!format.validity.RowIsValid(format.sel->get_index(i))) {
					present[i] = false;
				}
			}
		}
	}
	values.SetCardinality(rows.size());
	if (collated) {
		vector<vector<Value>> groups;
		for (idx_t i = 0; i < rows.size(); i++) {
			groups.push_back(DescentRowValues(values, 0, values.ColumnCount(), i));
		}
		Spell(key, groups, spelled, sort_keys);
	}
	if (collated && read) {
		read->spelled = std::move(spelled);
		read->sort_keys = std::move(sort_keys);
	}
}

void DecideSpelling::Fold(const vector<Value> &group, const vector<vector<Value>> &spelled,
                          const vector<vector<string>> &sort_keys, idx_t i) {
	if (least.empty()) {
		label = group;
		least.resize(group.size());
	}
	for (idx_t c = 0; c < sort_keys.size(); c++) {
		if (!sort_keys[c].empty() && (least[c].empty() || sort_keys[c][i] < least[c])) {
			least[c] = sort_keys[c][i];
			label[c] = spelled[c][i];
		}
	}
}

//! Folds the spellings `spelled` of `groups`, groups of `key`, into each group's least, as `min` folds a group's
//! values: so a group is spelled one way in every message, whichever node reads it and in whatever order (D69).
void DecideDescent::Spell(idx_t key, const vector<vector<Value>> &groups, const vector<vector<Value>> &spelled,
                          const vector<vector<string>> &sort_keys) {
	auto &table = spellings[key];
	for (idx_t i = 0; i < groups.size(); i++) {
		auto &bucket = table[DescentHash(0, groups[i])];
		auto known = std::find_if(bucket.begin(), bucket.end(),
		                          [&](const KeySpelling &spelling) { return DescentEqual(spelling.group, groups[i]); });
		if (known == bucket.end()) {
			bucket.push_back({groups[i], DecideSpelling()});
			known = bucket.end() - 1;
		}
		known->spelling.Fold(groups[i], spelled, sort_keys, i);
	}
}

vector<Value> DecideDescent::Label(idx_t key, const vector<Value> &group) const {
	if (key >= spellings.size() || spellings[key].empty()) {
		return group;
	}
	auto bucket = spellings[key].find(DescentHash(0, group));
	if (bucket != spellings[key].end()) {
		for (auto &spelling : bucket->second) {
			if (DescentEqual(spelling.group, group)) {
				return spelling.spelling.label;
			}
		}
	}
	throw InternalException("DECIDE tree: a group of a collated key was never read");
}

void DecideDescent::Signature(idx_t node_index, DataChunk &chunk, const Reach &rows, idx_t row_base,
                              DataChunk &values) {
	auto &node = *tree.nodes[node_index];
	auto &types = signature_types[node_index];
	if (!types.empty()) {
		values.Initialize(Allocator::Get(context), types);
	}
	idx_t column = 0;
	for (auto &field : node.signature) {
		switch (field.kind) {
		case DecideSignatureFieldKind::VALUE: {
			if (!field.value) {
				throw InternalException("DECIDE tree: a signature value has no expression");
			}
			Evaluate(*field.value, chunk, rows, values.data[column++]);
			break;
		}
		case DecideSignatureFieldKind::BY_KEY: {
			// A field naming a reducer's `by` key reads its values as a `by` key's read, which keeps the row: the node
			// grouping by the key raises what it raises, where flat reads it (a helper's G, where flat builds the
			// helper's classes).
			const auto block = read_block;
			const auto position = current_rank.position;
			read_block = DecideBlock::BY_KEY;
			DataChunk keys;
			vector<bool> present;
			Key(field.key, chunk, rows, keys, present);
			read_block = block;
			current_rank.position = position;
			for (idx_t c = 0; c < keys.ColumnCount(); c++) {
				values.data[column++].Reference(keys.data[c]);
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
					if (scope < tree.keyed_scopes.size()) {
						classes = DecisionClasses(field.decision, chunk, rows, row_base);
					}
				}
			}
			auto decisions = FlatVector::GetData<uint64_t>(values.data[column++]);
			auto instances = FlatVector::GetData<uint64_t>(values.data[column++]);
			for (idx_t i = 0; i < rows.size(); i++) {
				decisions[i] = field.decision;
				instances[i] = scope < tree.keyed_scopes.size() ? classes[i] : (global ? 0 : row_base + rows.rows[i]);
			}
			break;
		}
		case DecideSignatureFieldKind::FRAME: {
			auto frame = DescentFrameNode(tree, field.frame);
			if (!frame || !frame->over) {
				throw InternalException("DECIDE tree: signature names missing frame %llu", field.frame);
			}
			DataChunk keys;
			vector<bool> present;
			Key(frame->key, chunk, rows, keys, present);
			for (idx_t c = 0; c < keys.ColumnCount(); c++) {
				values.data[column++].Reference(keys.data[c]);
			}
			if (DescentFrameReadsOrder(*frame)) {
				Evaluate(*frame->over, chunk, rows, values.data[column++]);
			}
			break;
		}
		}
	}
	values.SetCardinality(rows.size());
}

void DecideDescent::FindGroups(idx_t node_index, const Reach &rows, const vector<idx_t> &selected, DataChunk &keys,
                               const vector<bool> &present, idx_t row_base, vector<idx_t> &slots) {
	auto &node = *tree.nodes[node_index];
	auto &groups = containers[node_index].groups;
	const auto scope = node.kind == DecideNodeKind::SCOPE;
	auto create = [&](idx_t i, vector<Value> key) {
		DecideDescentGroup group;
		group.parent_slot = scope ? rows.parents[i] : DConstants::INVALID_INDEX;
		group.key = std::move(key);
		group.first_row = row_base + rows.rows[i];
		group.in_a_class = present[i];
		groups.push_back(std::move(group));
	};
	auto &map = group_maps[node_index];
	if (!map) {
		for (auto i : selected) {
			if (scope) {
				// An E's per never merges a node per row's rows: each is a slot of its own, keyed by its row.
				slots[i] = groups.size();
				create(i, {Value::UBIGINT(row_base + rows.rows[i])});
				continue;
			}
			// A G's or O's empty key is one slot.
			if (groups.empty()) {
				create(i, {});
			}
			groups[0].first_row = MinValue(groups[0].first_row, row_base + rows.rows[i]);
			slots[i] = 0;
		}
		return;
	}
	const idx_t count = selected.size();
	if (count == 0) {
		return;
	}
	// An E's classes lie within the group it feeds. A G's or O's key is already its whole path key (the path rule,
	// language/guide.md §5.5; DecideReducerPath::KeysOf), so no enclosing slot adds a level. A G a rewrite generates,
	// as S3's tightest bound, is on no reducer's path: the reducers its operand reads keep their groups.
	SelectionVector selection(count);
	for (idx_t j = 0; j < count; j++) {
		selection.set_index(j, selected[j]);
	}
	vector<LogicalType> types;
	if (scope) {
		types.push_back(LogicalType::UBIGINT);
	}
	auto key_types = keys.GetTypes();
	types.insert(types.end(), key_types.begin(), key_types.end());
	DataChunk probe;
	probe.InitializeEmpty(types);
	idx_t column = 0;
	Vector parents(LogicalType::UBIGINT);
	if (scope) {
		auto data = FlatVector::GetData<uint64_t>(parents);
		for (idx_t j = 0; j < count; j++) {
			data[j] = rows.parents[selected[j]];
		}
		probe.data[column++].Reference(parents);
	}
	for (idx_t c = 0; c < keys.ColumnCount(); c++) {
		probe.data[column++].Slice(keys.data[c], selection, count);
	}
	probe.SetCardinality(count);
	vector<idx_t> found(count);
	idx_t next = groups.size();
	map->FindOrCreate(probe, found.data(), next);
	for (idx_t j = 0; j < count; j++) {
		const auto i = selected[j];
		if (found[j] == groups.size()) {
			create(i, DescentRowValues(keys, 0, keys.ColumnCount(), i));
		}
		auto &group = groups[found[j]];
		group.first_row = MinValue(group.first_row, row_base + rows.rows[i]);
		slots[i] = found[j];
	}
}

void DecideDescent::FindDecisionGroups(idx_t scope, DataChunk &keys, const vector<bool> &present,
                                       const vector<idx_t> &rows, vector<idx_t> &classes) {
	auto &groups = decision_groups[scope];
	auto &layouts = decision_layouts[scope];
	auto types = keys.GetTypes();
	auto layout =
	    std::find_if(layouts.begin(), layouts.end(), [&](const DecisionLayout &known) { return known.types == types; });
	if (layout == layouts.end()) {
		layouts.emplace_back();
		layout = layouts.end() - 1;
		layout->types = types;
		if (!types.empty()) {
			layout->map = make_uniq<DecideKeyMap>(context, types);
		}
	}
	auto create = [&](idx_t i, vector<Value> key) {
		DecideDescentGroup group;
		group.parent_slot = DConstants::INVALID_INDEX;
		group.key = std::move(key);
		group.first_row = rows[i];
		groups.push_back(std::move(group));
	};
	classes.assign(rows.size(), DConstants::INVALID_INDEX);
	vector<idx_t> selected;
	for (idx_t i = 0; i < rows.size(); i++) {
		if (present[i]) {
			selected.push_back(i);
		}
	}
	if (!layout->map) {
		for (auto i : selected) {
			if (layout->empty_slot == DConstants::INVALID_INDEX) {
				layout->empty_slot = groups.size();
				create(i, {});
			}
			auto &group = groups[layout->empty_slot];
			group.first_row = MinValue(group.first_row, rows[i]);
			classes[i] = layout->empty_slot;
		}
		return;
	}
	if (selected.empty()) {
		return;
	}
	SelectionVector selection(selected.size());
	for (idx_t j = 0; j < selected.size(); j++) {
		selection.set_index(j, selected[j]);
	}
	DataChunk probe;
	probe.InitializeEmpty(types);
	probe.Slice(keys, selection, selected.size());
	vector<idx_t> found(selected.size());
	idx_t next = groups.size();
	layout->map->FindOrCreate(probe, found.data(), next);
	for (idx_t j = 0; j < selected.size(); j++) {
		const auto i = selected[j];
		if (found[j] == groups.size()) {
			create(i, DescentRowValues(keys, 0, keys.ColumnCount(), i));
		}
		auto &group = groups[found[j]];
		group.first_row = MinValue(group.first_row, rows[i]);
		classes[i] = found[j];
	}
}

vector<idx_t> DecideDescent::DecisionClasses(idx_t variable, DataChunk &chunk, const Reach &rows, idx_t row_base) {
	const auto scope = tree.variable_scopes[variable].keyed_scope_idx;
	auto &mapping = decision_rows[scope];
	auto &recorded = decision_recorded[scope];
	mapping.resize(MaxValue<idx_t>(mapping.size(), row_base + chunk.size()), DConstants::INVALID_INDEX);
	recorded.resize(mapping.size(), false);
	Reach missing;
	for (idx_t i = 0; i < rows.size(); i++) {
		if (!recorded[row_base + rows.rows[i]]) {
			missing.Add(rows.rows[i], rows.parents[i]);
		}
	}
	if (!missing.empty()) {
		idx_t definition = DConstants::INVALID_INDEX;
		if (variable < tree.definition_nodes.size() && !tree.definition_nodes[variable].empty()) {
			definition = tree.definition_nodes[variable][0];
		}
		DataChunk keys;
		vector<bool> present;
		Key(definition != DConstants::INVALID_INDEX ? tree.nodes[definition]->key : tree.keyed_scopes[scope].key, chunk,
		    missing, keys, present);
		// A definition's classes are its key's, split by its signature.
		DataChunk variants;
		if (definition != DConstants::INVALID_INDEX) {
			Signature(definition, chunk, missing, row_base, variants);
		}
		auto types = keys.GetTypes();
		auto signature = variants.GetTypes();
		types.insert(types.end(), signature.begin(), signature.end());
		DataChunk layout;
		if (!types.empty()) {
			layout.InitializeEmpty(types);
		}
		for (idx_t c = 0; c < keys.ColumnCount(); c++) {
			layout.data[c].Reference(keys.data[c]);
		}
		for (idx_t c = 0; c < variants.ColumnCount(); c++) {
			layout.data[keys.ColumnCount() + c].Reference(variants.data[c]);
		}
		layout.SetCardinality(missing.size());
		vector<idx_t> absolute;
		for (auto row : missing.rows) {
			absolute.push_back(row_base + row);
		}
		vector<idx_t> classes;
		FindDecisionGroups(scope, layout, present, absolute, classes);
		for (idx_t i = 0; i < missing.size(); i++) {
			if (present[i]) {
				mapping[absolute[i]] = classes[i];
			}
			recorded[absolute[i]] = true;
		}
	}
	vector<idx_t> result;
	result.reserve(rows.size());
	for (auto row : rows.rows) {
		result.push_back(mapping[row_base + row]);
	}
	return result;
}

void DecideDescent::InsertDistinct(Pending &batch, idx_t row_base) {
	auto &node = *tree.nodes[batch.node];
	auto &container = containers[batch.node];
	const idx_t count = batch.rows.size();
	const auto signature_columns = signature_types[batch.node].size();
	const auto value_columns = signature_columns + node.columns.size();
	// Sigma numbers the instance. An entry stays apart where it reads another value -- a carried column, a G's group,
	// an O's timeline or `else` -- so a set E is finer than its instances until it settles bottom-up, once the frames
	// and the absence of what it reads are known (language/guide.md §7.1, §7.2).
	vector<LogicalType> types {LogicalType::UBIGINT};
	for (idx_t c = 0; c < value_columns; c++) {
		types.push_back(batch.values.data[c].GetType());
	}
	types.insert(types.end(), node.dependencies.size(), LogicalType::UBIGINT);
	DataChunk probe;
	probe.Initialize(Allocator::Get(context), types);
	DescentIds(probe.data[0], batch.slots);
	for (idx_t c = 0; c < value_columns; c++) {
		probe.data[1 + c].Reference(batch.values.data[c]);
	}
	// Where its row reads each node it depends on (DecideDescentEntry::reads).
	vector<vector<idx_t>> reads(node.dependencies.size(), vector<idx_t>(count, DConstants::INVALID_INDEX));
	for (idx_t d = 0; d < node.dependencies.size(); d++) {
		auto &landing = landed[node.dependencies[d]];
		for (idx_t i = 0; i < count && !landing.empty(); i++) {
			reads[d][i] = landing[batch.rows.rows[i]];
		}
		DescentIds(probe.data[1 + value_columns + d], reads[d]);
	}
	probe.SetCardinality(count);
	vector<idx_t> entries(count);
	idx_t next = container.entries.size();
	entry_maps[batch.node]->FindOrCreate(probe, entries.data(), next);
	// The rows that make a new entry, each the first to read its values.
	vector<idx_t> created;
	for (idx_t i = 0; i < count; i++) {
		container.routed.emplace_back(row_base + batch.rows.rows[i], entries[i]);
		if (entries[i] == container.entries.size() + created.size()) {
			created.push_back(i);
		}
	}
	if (created.empty()) {
		return;
	}
	SelectionVector selection(created.size());
	for (idx_t k = 0; k < created.size(); k++) {
		selection.set_index(k, created[k]);
	}
	DataChunk signatures;
	signatures.InitializeEmpty(vector<LogicalType>(types.begin(), types.begin() + 1 + signature_columns));
	for (idx_t c = 0; c < signatures.ColumnCount(); c++) {
		signatures.data[c].Slice(probe.data[c], selection, created.size());
	}
	signatures.SetCardinality(created.size());
	vector<idx_t> instances(created.size());
	idx_t next_instance = container.instance_entries.size();
	instance_maps[batch.node]->FindOrCreate(signatures, instances.data(), next_instance);
	for (idx_t k = 0; k < created.size(); k++) {
		const auto i = created[k];
		if (instances[k] == container.instance_entries.size()) {
			container.instance_entries.push_back(container.entries.size());
		}
		DecideDescentEntry entry;
		entry.row = row_base + batch.rows.rows[i];
		entry.parent_slot = batch.rows.parents[i];
		entry.slot = batch.slots[i];
		entry.instance = instances[k];
		entry.signature = DescentRowValues(batch.values, 0, signature_columns, i);
		entry.columns = DescentRowValues(batch.values, signature_columns, value_columns, i);
		entry.program_columns = DescentRowValues(batch.values, value_columns, batch.values.ColumnCount(), i);
		for (auto &read : reads) {
			entry.reads.push_back(read[i]);
		}
		container.groups[entry.slot].entries.push_back(container.entries.size());
		container.entries.push_back(std::move(entry));
	}
}

void DecideDescent::Visit(idx_t node_index, DataChunk &chunk, idx_t row_base, const Reach &incoming) {
	auto &node = *tree.nodes[node_index];
	active_node = node_index;
	active_row_base = row_base;
	// Its `when` first, where flat reads it (DecideNode::when_rank), then the rest at its own rank.
	current_rank = node.when_rank;
	if (context.interrupted) {
		throw InterruptException();
	}
	auto &container = containers[node_index];
	// Shared DAG edges may route the same row and context twice. Its input value is inserted once, independently of
	// which reader visits first.
	Reach rows;
	if (shared[node_index]) {
		for (idx_t i = 0; i < incoming.size(); i++) {
			if (seen[node_index].emplace(incoming.rows[i], incoming.parents[i]).second) {
				rows.Add(incoming.rows[i], incoming.parents[i]);
			}
		}
	} else {
		rows = incoming;
	}
	container.rows_in += rows.size();
	for (idx_t k = 0; k < node.when.size(); k++) {
		// A reducer's own `when` is read in its terms' filter, at its place there (DecideNode::filter_group,
		// filter_part).
		const auto filter = node.filter_group != DConstants::INVALID_INDEX;
		read_block = filter ? DecideBlock::FILTERS : DecideBlock::ROUTE;
		current_rank.part = node.filter_part;
		current_rank.group = filter ? node.filter_group : 0;
		current_rank.position = filter ? k : 0;
		auto truth = Truth(*node.when[k], chunk, rows);
		read_block = DecideBlock::ROUTE;
		current_rank.part = current_rank.group = current_rank.position = 0;
		Reach kept;
		for (idx_t i = 0; i < rows.size(); i++) {
			if (truth[i] == 1 && !Failed(rows.rows[i])) {
				kept.Add(rows.rows[i], rows.parents[i]);
			}
		}
		rows = std::move(kept);
		if (rows.empty()) {
			return;
		}
	}
	current_rank.phase = node.route_rank.phase;
	current_rank.step = node.route_rank.step;
	current_rank.clause = node.route_rank.clause;
	if (rows.empty()) {
		return;
	}
	vector<bool> present;
	KeySpellings spelled;
	DataChunk keys;
	// A grouped clause's keys are read where flat groups its kept rows, after its terms (DecideNode::key_block).
	read_block = node.key_block;
	Key(node.key, chunk, rows, keys, present, &spelled);
	read_block = DecideBlock::ROUTE;
	current_rank.position = 0;
	const auto scope = node.kind == DecideNodeKind::SCOPE;
	// Flat reads a reducer's operand on every row its G reads, except the objective's, which it reads in place, on the
	// rows of each group alone (D81).
	const auto reducer = node.kind == DecideNodeKind::REDUCER && node.value_rank.phase != DecideErrorPhase::OBJECTIVE;
	// A row in no class of the key stops here, unless the node holds such rows or is a reducer; an E's still reaches
	// its children, since `per` never gates them.
	auto unclassed = [&](idx_t i) {
		return !present[i] && !node.holds_rows_in_no_class && !reducer;
	};
	vector<idx_t> grouped;
	for (idx_t i = 0; i < rows.size(); i++) {
		if (!Failed(rows.rows[i]) && !unclassed(i)) {
			grouped.push_back(i);
		}
	}
	vector<idx_t> slots(rows.size(), DConstants::INVALID_INDEX);
	FindGroups(node_index, rows, grouped, keys, present, row_base, slots);
	Reach children;
	Reach stored;
	vector<idx_t> stored_slots;
	auto &landing = landed[node_index];
	for (idx_t i = 0; i < rows.size(); i++) {
		const auto row = rows.rows[i];
		if (Failed(row)) {
			continue;
		}
		if (unclassed(i)) {
			if (scope) {
				children.Add(row, rows.parents[i]);
			}
			continue;
		}
		const auto slot = slots[i];
		auto &group = container.groups[slot];
		// A collated key's own least spelling over the rows the node holds in the group.
		if (!spelled.sort_keys.empty()) {
			group.spelling.Fold(group.key, spelled.spelled, spelled.sort_keys, i);
		}
		children.Add(row, scope ? rows.parents[i] : slot);
		// A row in no group of a G has no value of it (language/guide.md §5.2), so it holds no entry of the G; it still
		// reaches the G's operand, as flat computes a clause's terms on every kept row and a side plan a data reducer's
		// arguments, so an error there is raised (§7.2).
		if (!present[i] && reducer) {
			continue;
		}
		stored.Add(row, rows.parents[i]);
		stored_slots.push_back(slot);
		// A reader reads a G's or O's value at its group or timeline, and an `else` at the anchor's own row
		// (language/guide.md §5.6; DecideDescentEntry::reads).
		if (!scope && landing[row] == DConstants::INVALID_INDEX) {
			landing[row] = node.fallback ? row_base + row : slot;
		}
	}
	// A node records the classes of the keyed decisions it defines or reads where it is reached. Its sigma need not
	// name them: a root's names only what its written clause reads.
	for (auto variable : node.decisions) {
		if (variable < tree.variable_scopes.size() && tree.variable_scopes[variable].IsKeyed()) {
			DecisionClasses(variable, chunk, stored, row_base);
		}
	}
	DataChunk signatures;
	Signature(node_index, chunk, stored, row_base, signatures);
	auto carry = [&](const vector<unique_ptr<Expression>> &carried, DataChunk &values) {
		if (carried.empty()) {
			return;
		}
		values.Initialize(Allocator::Get(context), DescentTypes(carried));
		for (idx_t c = 0; c < carried.size(); c++) {
			Evaluate(*carried[c], chunk, stored, values.data[c]);
		}
		values.SetCardinality(stored.size());
	};
	DataChunk columns;
	DataChunk program_columns;
	carry(node.columns, columns);
	carry(node.program_columns, program_columns);
	vector<Value> order, order_keys;
	vector<vector<Value>> partitions;
	vector<bool> partition_present;
	if (node.kind == DecideNodeKind::FRAME && node.over && !stored.empty()) {
		if (node.frame_spec) {
			DataChunk partition;
			Key(node.frame_spec->partition_key, chunk, stored, partition, partition_present);
			for (idx_t i = 0; i < stored.size(); i++) {
				partitions.push_back(DescentRowValues(partition, 0, partition.ColumnCount(), i));
			}
		}
		Vector over(node.over->return_type);
		Evaluate(*node.over, chunk, stored, over);
		order = DescentColumnValues(over, stored.size());
		order_keys = order;
		if (node.frame_spec && node.frame_spec->order_collation) {
			DataChunk raw;
			raw.Initialize(context, {node.over->return_type});
			raw.SetCardinality(stored.size());
			for (idx_t i = 0; i < stored.size(); i++) {
				raw.SetValue(0, i, order[i]);
			}
			Reach positions;
			for (idx_t i = 0; i < stored.size(); i++) {
				positions.Add(i, 0);
			}
			Vector collated(node.frame_spec->order_collation->return_type);
			Evaluate(*node.frame_spec->order_collation, raw, positions, collated);
			order_keys = DescentColumnValues(collated, stored.size());
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
	const auto distinct = scope && node.per != DecideTreePer::ROW;
	auto batch = make_uniq<Pending>();
	batch->node = node_index;
	vector<idx_t> kept;
	for (idx_t i = 0; i < stored.size(); i++) {
		if (Failed(stored.rows[i])) {
			continue;
		}
		if (distinct) {
			batch->rows.Add(stored.rows[i], stored.parents[i]);
			batch->slots.push_back(stored_slots[i]);
			kept.push_back(i);
			continue;
		}
		DecideDescentEntry entry;
		entry.row = row_base + stored.rows[i];
		entry.parent_slot = stored.parents[i];
		entry.slot = stored_slots[i];
		entry.signature = DescentRowValues(signatures, 0, signatures.ColumnCount(), i);
		entry.columns = DescentRowValues(columns, 0, columns.ColumnCount(), i);
		entry.program_columns = DescentRowValues(program_columns, 0, program_columns.ColumnCount(), i);
		if (!order.empty()) {
			entry.order = std::move(order[i]);
			entry.order_key = std::move(order_keys[i]);
			if (!partitions.empty()) {
				entry.partition = std::move(partitions[i]);
				entry.partition_present = partition_present[i];
			}
		}
		entry.instance = container.instance_entries.size();
		container.instance_entries.push_back(container.entries.size());
		container.groups[entry.slot].entries.push_back(container.entries.size());
		container.entries.push_back(std::move(entry));
	}
	if (!kept.empty()) {
		// A set E's row reads the G and O nodes it depends on, its children and the helpers its decisions stand for,
		// which another root may route later in the window. So its entries wait for every root.
		SelectionVector selection(kept.size());
		for (idx_t k = 0; k < kept.size(); k++) {
			selection.set_index(k, kept[k]);
		}
		vector<LogicalType> types;
		for (auto *source : {&signatures, &columns, &program_columns}) {
			auto source_types = source->GetTypes();
			types.insert(types.end(), source_types.begin(), source_types.end());
		}
		if (!types.empty()) {
			batch->values.InitializeEmpty(types);
		}
		idx_t column = 0;
		for (auto *source : {&signatures, &columns, &program_columns}) {
			for (idx_t c = 0; c < source->ColumnCount(); c++) {
				batch->values.data[column++].Slice(source->data[c], selection, kept.size());
			}
		}
		batch->values.SetCardinality(kept.size());
		pending.push_back(std::move(batch));
	}
	Reach reached;
	for (idx_t i = 0; i < children.size(); i++) {
		if (!Failed(children.rows[i])) {
			reached.Add(children.rows[i], children.parents[i]);
		}
	}
	container.rows_out += reached.size();
	for (auto child : node.children) {
		Visit(child, chunk, row_base, reached);
	}
}

void DecideDescent::Append(DataChunk &chunk, idx_t row_base) {
	if (finished) {
		throw InternalException("DECIDE tree: a row arrived after top-down construction finished");
	}
	Reach rows;
	for (idx_t row = 0; row < chunk.size(); row++) {
		rows.Add(row, DConstants::INVALID_INDEX);
	}
	// A declared decision's dictionary belongs to Source as well as the clauses:
	// it must include every row, even one no constraint reaches. Helper scopes
	// are instead populated when their defining/reading nodes are reached.
	for (idx_t scope = 0; scope < tree.keyed_scopes.size(); scope++) {
		auto &definition = tree.keyed_scopes[scope];
		if (definition.value_link != DConstants::INVALID_INDEX || definition.signature != DConstants::INVALID_INDEX) {
			continue;
		}
		DataChunk keys;
		vector<bool> present;
		Key(definition.key, chunk, rows, keys, present);
		keys.SetCardinality(chunk.size());
		auto &mapping = decision_rows[scope];
		mapping.resize(row_base + chunk.size(), DConstants::INVALID_INDEX);
		decision_recorded[scope].resize(mapping.size(), true);
		vector<idx_t> absolute(chunk.size());
		std::iota(absolute.begin(), absolute.end(), row_base);
		vector<idx_t> classes;
		FindDecisionGroups(scope, keys, present, absolute, classes);
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (present[i]) {
				mapping[row_base + i] = classes[i];
			}
		}
	}
	// Seen and landed are window-local: containers retain values, not an input-sized visited set.
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		seen[n].clear();
		if (tree.nodes[n]->kind != DecideNodeKind::SCOPE) {
			landed[n].assign(chunk.size(), DConstants::INVALID_INDEX);
		}
	}
	for (auto root : tree.roots) {
		Visit(root, chunk, row_base, rows);
	}
	// A set E's row reads the G and O nodes it depends on, its children and the helpers its decisions stand for, which
	// another root may route later in the chunk. So its entries wait for every root.
	for (auto &batch : pending) {
		InsertDistinct(*batch, row_base);
	}
	pending.clear();
}

void DecideDescent::Finish() {
	if (finished) {
		return;
	}
	for (auto &groups : decision_groups) {
		vector<idx_t> order(groups.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(), [&](idx_t left, idx_t right) {
			return DecideDescentKeyLess(groups[left].key, groups[right].key);
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
			return l.parent_slot != r.parent_slot ? l.parent_slot < r.parent_slot : DecideDescentKeyLess(l.key, r.key);
		});
		for (idx_t rank = 0; rank < order.size(); rank++) {
			container.groups[order[rank]].rank = rank;
		}
		if (node.kind != DecideNodeKind::FRAME) {
			continue;
		}
		// A position is named by its least spelling, as a group is (D69; flat's DecidePositionRows).
		vector<Value> spelled;
		for (auto &entry : container.entries) {
			spelled.push_back(entry.order);
		}
		const auto sort_keys = SpellingKeys(spelled, node.over->return_type);
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
			idx_t least = DConstants::INVALID_INDEX;
			for (auto index : entries) {
				auto &entry = container.entries[index];
				if (timeline.positions.empty() ||
				    !Value::NotDistinctFrom(container.entries[timeline.positions.back().entries[0]].order_key,
				                            entry.order_key)) {
					DecideDescentPosition position;
					position.order = entry.order;
					position.entries.push_back(index);
					timeline.positions.push_back(std::move(position));
					least = index;
				} else {
					auto &position = timeline.positions.back();
					position.conflict |= !DescentEqual(container.entries[position.entries[0]].signature, entry.signature);
					position.entries.push_back(index);
					if (sort_keys[index] < sort_keys[least]) {
						position.order = entry.order;
						least = index;
					}
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
