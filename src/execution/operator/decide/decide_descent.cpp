#include "duckdb/execution/operator/decide/decide_descent.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
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

//! A UBIGINT column of `count` values.
static void DescentIds(Vector &result, const idx_t *values, idx_t count) {
	auto data = FlatVector::GetData<uint64_t>(result);
	for (idx_t i = 0; i < count; i++) {
		data[i] = values[i];
	}
}

static void DescentIds(Vector &result, const vector<idx_t> &values) {
	DescentIds(result, values.data(), values.size());
}

void DecideColumns::Initialize(Allocator &allocator_p, vector<LogicalType> types_p) {
	allocator = &allocator_p;
	types = std::move(types_p);
	chunks.clear();
	count = 0;
}

void DecideColumns::Append(DataChunk &source, const SelectionVector &selection, idx_t append) {
	if (types.empty()) {
		count += append;
		return;
	}
	for (idx_t done = 0; done < append;) {
		const auto offset = count % STANDARD_VECTOR_SIZE;
		if (offset == 0) {
			chunks.push_back(make_uniq<DataChunk>());
			chunks.back()->Initialize(*allocator, types);
		}
		auto &target = *chunks.back();
		const auto take = MinValue<idx_t>(append - done, STANDARD_VECTOR_SIZE - offset);
		for (idx_t c = 0; c < types.size(); c++) {
			VectorOperations::Copy(source.data[c], target.data[c], selection, done + take, done, offset);
		}
		target.SetCardinality(offset + take);
		count += take;
		done += take;
	}
}

Value DecideColumns::GetValue(idx_t column, idx_t row) const {
	return chunks[row / STANDARD_VECTOR_SIZE]->GetValue(column, row % STANDARD_VECTOR_SIZE);
}

vector<Value> DecideColumns::GetValues(idx_t row, idx_t begin, idx_t end) const {
	vector<Value> values;
	values.reserve(end - begin);
	for (idx_t c = begin; c < end; c++) {
		values.push_back(GetValue(c, row));
	}
	return values;
}

vector<Value> DecideNodeGroups::Key(idx_t slot) const {
	if (per_row) {
		return {Value::UBIGINT(first_rows[slot])};
	}
	return keys.GetValues(slot, 0, keys.Types().size());
}

const DecideSpelling &DecideNodeGroups::Spelling(idx_t slot) const {
	return spellings[slot];
}

//! Ranks numbering `count` items by their sort keys, `keys` of `count` BLOBs as CreateSortKey writes them, equal keys
//! in item order: flat's NumberGroupsByValue.
static vector<idx_t> DescentRanks(const vector<string_t> &keys) {
	vector<idx_t> order(keys.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](idx_t left, idx_t right) {
		auto &l = keys[left];
		auto &r = keys[right];
		const auto compared = memcmp(l.GetData(), r.GetData(), MinValue(l.GetSize(), r.GetSize()));
		return compared != 0 ? compared < 0 : l.GetSize() < r.GetSize();
	});
	vector<idx_t> ranks(keys.size());
	for (idx_t rank = 0; rank < order.size(); rank++) {
		ranks[order[rank]] = rank;
	}
	return ranks;
}

//! The sort keys of `columns`' rows `count` of them, each column ascending with NULL after every value, as ORDER BY
//! places them and flat numbers a key's groups (EncodeSortKeys), held in `heap`.
static void DescentSortKeys(DataChunk &columns, StringHeap &heap, vector<string_t> &keys) {
	const vector<OrderModifiers> modifiers(columns.ColumnCount(),
	                                       OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST));
	Vector encoded(LogicalType::BLOB, MaxValue<idx_t>(columns.size(), 1));
	CreateSortKeyHelpers::CreateSortKey(columns, modifiers, encoded);
	auto data = FlatVector::GetData<string_t>(encoded);
	for (idx_t i = 0; i < columns.size(); i++) {
		keys.push_back(heap.AddBlob(data[i]));
	}
}

//! A node's slots by value: of the enclosing slot, then of the key (flat's NumberGroupsByValue). A node per row's slot
//! is keyed by its row, so its slots rank by enclosing slot and then row: they are made in row order within one
//! enclosing slot, unless two paths reach the node, which a sort of that slot's rows orders.
static void DescentRankGroups(DecideNodeGroups &groups) {
	const auto count = groups.size();
	if (groups.per_row) {
		idx_t enclosing = 0;
		for (auto parent : groups.parent_slots) {
			if (parent != DConstants::INVALID_INDEX) {
				enclosing = MaxValue(enclosing, parent + 1);
			}
		}
		// A counting sort by the enclosing slot, INVALID last.
		vector<idx_t> begin(enclosing + 2, 0);
		auto bucket = [&](idx_t slot) {
			const auto parent = groups.parent_slots[slot];
			return parent == DConstants::INVALID_INDEX ? enclosing : parent;
		};
		for (idx_t slot = 0; slot < count; slot++) {
			begin[bucket(slot) + 1]++;
		}
		for (idx_t b = 1; b < begin.size(); b++) {
			begin[b] += begin[b - 1];
		}
		vector<idx_t> order(count);
		auto next = begin;
		for (idx_t slot = 0; slot < count; slot++) {
			order[next[bucket(slot)]++] = slot;
		}
		for (idx_t b = 0; b + 1 < begin.size(); b++) {
			auto first = order.begin() + begin[b];
			auto last = order.begin() + begin[b + 1];
			auto by_row = [&](idx_t left, idx_t right) {
				return groups.first_rows[left] < groups.first_rows[right];
			};
			if (!std::is_sorted(first, last, by_row)) {
				std::stable_sort(first, last, by_row);
			}
		}
		groups.ranks.assign(count, 0);
		for (idx_t rank = 0; rank < count; rank++) {
			groups.ranks[order[rank]] = rank;
		}
		return;
	}
	StringHeap heap;
	vector<string_t> keys;
	keys.reserve(count);
	const auto key_columns = groups.keys.Types().size();
	vector<LogicalType> types {LogicalType::UBIGINT};
	types.insert(types.end(), groups.keys.Types().begin(), groups.keys.Types().end());
	DataChunk columns;
	columns.InitializeEmpty(types);
	Vector parents(LogicalType::UBIGINT);
	for (idx_t start = 0; start < count; start += STANDARD_VECTOR_SIZE) {
		const auto size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - start);
		DescentIds(parents, groups.parent_slots.data() + start, size);
		columns.data[0].Reference(parents);
		for (idx_t c = 0; c < key_columns; c++) {
			columns.data[1 + c].Reference(groups.keys.Window(start / STANDARD_VECTOR_SIZE).data[c]);
		}
		columns.SetCardinality(size);
		DescentSortKeys(columns, heap, keys);
	}
	groups.ranks = DescentRanks(keys);
}

DecideDescent::DecideDescent(const DecideTree &tree_p, ClientContext &context_p)
    : tree(tree_p), context(context_p), containers(tree.nodes.size()), group_maps(tree.nodes.size()),
      entry_maps(tree.nodes.size()), instance_maps(tree.nodes.size()), signature_types(tree.nodes.size()),
      shared(tree.nodes.size(), false), seen(tree.nodes.size()), landed(tree.nodes.size()),
      decision_groups(tree.keyed_scopes.size()), decision_sort_keys(tree.keyed_scopes.size()),
      decision_rows(tree.keyed_scopes.size()), decision_recorded(tree.keyed_scopes.size()),
      decision_layouts(tree.keyed_scopes.size()), spellings(tree.keys.size()), failed_rows(tree.nodes.size()) {
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
	auto &allocator = Allocator::Get(context);
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		auto &node = *tree.nodes[n];
		auto &container = containers[n];
		shared[n] = paths[n] > 1;
		signature_types[n] = DescentSignatureTypes(tree, node);
		// An entry's values: its signature's, its carried and program columns, and an O's order key and partition.
		auto types = signature_types[n];
		auto &layout = container.layout;
		layout.columns = types.size();
		auto columns = DescentTypes(node.columns);
		types.insert(types.end(), columns.begin(), columns.end());
		layout.program_columns = types.size();
		auto program_columns = DescentTypes(node.program_columns);
		types.insert(types.end(), program_columns.begin(), program_columns.end());
		if (node.kind == DecideNodeKind::FRAME && node.over) {
			layout.order = types.size();
			types.push_back(node.over->return_type);
			layout.order_key = types.size();
			types.push_back(LogicalType::BLOB);
			if (node.frame_spec) {
				layout.partition = types.size();
				auto partition = DescentKeyTypes(tree, node.frame_spec->partition_key);
				types.insert(types.end(), partition.begin(), partition.end());
				layout.partition_present = types.size();
				types.push_back(LogicalType::BOOLEAN);
			}
		}
		layout.end = types.size();
		container.values.Initialize(allocator, types);
		const auto scope = node.kind == DecideNodeKind::SCOPE;
		// A node per row has no key (DecideTreePer): each row is a slot of its own.
		D_ASSERT(!scope || node.per != DecideTreePer::ROW || node.key == DConstants::INVALID_INDEX);
		if (scope && node.per == DecideTreePer::ROW) {
			container.groups.per_row = true;
			continue;
		}
		auto key = DescentKeyTypes(tree, node.key);
		container.groups.keys.Initialize(allocator, key);
		vector<LogicalType> slot_types;
		if (scope) {
			slot_types.push_back(LogicalType::UBIGINT);
		}
		slot_types.insert(slot_types.end(), key.begin(), key.end());
		if (!slot_types.empty()) {
			group_maps[n] = make_uniq<DecideKeyMap>(context, slot_types);
		}
		if (!scope) {
			continue;
		}
		// A set E: its instances by (slot, signature), its entries by those, its carried columns and its reads.
		vector<LogicalType> instance {LogicalType::UBIGINT};
		instance.insert(instance.end(), signature_types[n].begin(), signature_types[n].end());
		instance_maps[n] = make_uniq<DecideKeyMap>(context, instance);
		auto entry = instance;
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
	// A slot made by row `i`, keyed by its row of `keys`, whose values the caller appends.
	auto create = [&](idx_t i) {
		groups.parent_slots.push_back(scope ? rows.parents[i] : DConstants::INVALID_INDEX);
		groups.first_rows.push_back(row_base + rows.rows[i]);
		if (!groups.per_row) {
			groups.in_a_class.push_back(present[i]);
		}
	};
	auto &map = group_maps[node_index];
	if (!map) {
		for (auto i : selected) {
			if (scope) {
				// An E's per never merges a node per row's rows: each is a slot of its own, keyed by its row.
				slots[i] = groups.size();
				create(i);
				continue;
			}
			// A G's or O's empty key is one slot.
			if (groups.size() == 0) {
				create(i);
				groups.keys.Append(keys, *FlatVector::IncrementalSelectionVector(), 1);
			}
			groups.first_rows[0] = MinValue(groups.first_rows[0], row_base + rows.rows[i]);
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
	SelectionVector created(count);
	idx_t created_count = 0;
	for (idx_t j = 0; j < count; j++) {
		const auto i = selected[j];
		if (found[j] == groups.size()) {
			create(i);
			created.set_index(created_count++, i);
		}
		auto &first = groups.first_rows[found[j]];
		first = MinValue(first, row_base + rows.rows[i]);
		slots[i] = found[j];
	}
	groups.keys.Append(keys, created, created_count);
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
	// A class made by row `i`: its value order is its key's (Finish), so only its sort key is kept.
	auto &sort_keys = decision_sort_keys[scope];
	auto create = [&](idx_t i) {
		groups.parent_slots.push_back(DConstants::INVALID_INDEX);
		groups.first_rows.push_back(rows[i]);
		groups.in_a_class.push_back(true);
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
				create(i);
				sort_keys.push_back(string_t());
			}
			auto &first = groups.first_rows[layout->empty_slot];
			first = MinValue(first, rows[i]);
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
	SelectionVector created(selected.size());
	idx_t created_count = 0;
	for (idx_t j = 0; j < selected.size(); j++) {
		const auto i = selected[j];
		if (found[j] == groups.size()) {
			create(i);
			created.set_index(created_count++, j);
		}
		auto &first = groups.first_rows[found[j]];
		first = MinValue(first, rows[i]);
		classes[i] = found[j];
	}
	if (created_count > 0) {
		DataChunk made;
		made.InitializeEmpty(types);
		made.Slice(probe, created, created_count);
		DescentSortKeys(made, decision_heap, sort_keys);
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
	// Where its row reads each node it depends on (DecideNodeContainer::routed).
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
	idx_t next = container.Count();
	entry_maps[batch.node]->FindOrCreate(probe, entries.data(), next);
	// The rows that make a new entry, each the first to read its values.
	vector<idx_t> created;
	for (idx_t i = 0; i < count; i++) {
		container.routed.emplace_back(row_base + batch.rows.rows[i], entries[i]);
		if (entries[i] == container.Count() + created.size()) {
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
			container.instance_entries.push_back(container.Count());
		}
		container.rows.push_back(row_base + batch.rows.rows[i]);
		container.parent_slots.push_back(batch.rows.parents[i]);
		container.slots.push_back(batch.slots[i]);
		container.instances.push_back(instances[k]);
	}
	container.values.Append(batch.values, selection, created.size());
}

void DecideDescent::FrameOrder(const DecideNode &node, const DecideEntryLayout &layout, DataChunk &chunk,
                               const Reach &stored, DataChunk &values) {
	if (node.frame_spec) {
		DataChunk partition;
		vector<bool> present;
		Key(node.frame_spec->partition_key, chunk, stored, partition, present);
		for (idx_t c = 0; c < partition.ColumnCount(); c++) {
			values.data[layout.partition + c].Reference(partition.data[c]);
		}
		auto flags = FlatVector::GetData<bool>(values.data[layout.partition_present]);
		for (idx_t i = 0; i < stored.size(); i++) {
			flags[i] = present[i];
		}
	}
	auto &order = values.data[layout.order];
	Evaluate(*node.over, chunk, stored, order);
	// Use ORDER BY's comparison bytes, including its bound collation, direction and configured NULL placement, rather
	// than Value's order.
	Vector *compared = &order;
	unique_ptr<Vector> collated;
	if (node.frame_spec && node.frame_spec->order_collation) {
		DataChunk raw;
		raw.InitializeEmpty({node.over->return_type});
		raw.data[0].Reference(order);
		raw.SetCardinality(stored.size());
		Reach positions;
		for (idx_t i = 0; i < stored.size(); i++) {
			positions.Add(NumericCast<sel_t>(i), 0);
		}
		collated = make_uniq<Vector>(node.frame_spec->order_collation->return_type);
		Evaluate(*node.frame_spec->order_collation, raw, positions, *collated);
		compared = collated.get();
	}
	const auto direction =
	    node.frame_spec && node.frame_spec->descending ? OrderType::DESCENDING : OrderType::ASCENDING;
	OrderModifiers modifiers(direction,
	                         DBConfig::GetConfig(context).ResolveNullOrder(direction, OrderByNullType::ORDER_DEFAULT));
	CreateSortKeyHelpers::CreateSortKey(*compared, stored.size(), modifiers, values.data[layout.order_key]);
}

void DecideDescent::Store(idx_t node_index, DataChunk &chunk, const Reach &stored, const vector<idx_t> &slots,
                          idx_t row_base) {
	auto &node = *tree.nodes[node_index];
	auto &container = containers[node_index];
	auto &layout = container.layout;
	// A node records the classes of the keyed decisions it defines or reads where it is reached. Its sigma need not
	// name them: a root's names only what its written clause reads.
	for (auto variable : node.decisions) {
		if (variable < tree.variable_scopes.size() && tree.variable_scopes[variable].IsKeyed()) {
			DecisionClasses(variable, chunk, stored, row_base);
		}
	}
	// Its entries' values, as its layout holds them: its signature's, its carried and program columns, an O's order
	// key and partition.
	DataChunk values;
	if (layout.end > 0) {
		values.Initialize(Allocator::Get(context), container.values.Types());
	}
	DataChunk signatures;
	Signature(node_index, chunk, stored, row_base, signatures);
	for (idx_t c = 0; c < layout.columns; c++) {
		values.data[c].Reference(signatures.data[c]);
	}
	for (idx_t c = 0; c < node.columns.size(); c++) {
		Evaluate(*node.columns[c], chunk, stored, values.data[layout.columns + c]);
	}
	for (idx_t c = 0; c < node.program_columns.size(); c++) {
		Evaluate(*node.program_columns[c], chunk, stored, values.data[layout.program_columns + c]);
	}
	if (layout.order != DConstants::INVALID_INDEX && !stored.empty()) {
		FrameOrder(node, layout, chunk, stored, values);
	}
	values.SetCardinality(stored.size());
	// An entry per row no read failed on.
	SelectionVector kept(MaxValue<idx_t>(stored.size(), 1));
	idx_t kept_count = 0;
	for (idx_t i = 0; i < stored.size(); i++) {
		if (!Failed(stored.rows[i])) {
			kept.set_index(kept_count++, i);
		}
	}
	if (kept_count == 0) {
		return;
	}
	if (node.kind == DecideNodeKind::SCOPE && node.per != DecideTreePer::ROW) {
		// A set E's row reads the G and O nodes it depends on, its children and the helpers its decisions stand for,
		// which another root may route later in the window. So its entries wait for every root.
		auto batch = make_uniq<Pending>();
		batch->node = node_index;
		for (idx_t k = 0; k < kept_count; k++) {
			const auto i = kept.get_index(k);
			batch->rows.Add(stored.rows[i], stored.parents[i]);
			batch->slots.push_back(slots[i]);
		}
		if (layout.end > 0) {
			batch->values.InitializeEmpty(container.values.Types());
			batch->values.Slice(values, kept, kept_count);
		}
		pending.push_back(std::move(batch));
		return;
	}
	for (idx_t k = 0; k < kept_count; k++) {
		const auto i = kept.get_index(k);
		container.rows.push_back(row_base + stored.rows[i]);
		container.parent_slots.push_back(stored.parents[i]);
		container.slots.push_back(slots[i]);
		container.instances.push_back(container.instance_entries.size());
		container.instance_entries.push_back(container.Count() - 1);
	}
	container.values.Append(values, kept, kept_count);
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
		// A collated key's own least spelling over the rows the node holds in the group.
		if (!spelled.sort_keys.empty()) {
			auto &groups = container.groups;
			groups.spellings.resize(groups.size());
			auto &spelling = groups.spellings[slot];
			spelling.Fold(spelling.least.empty() ? groups.Key(slot) : spelling.label, spelled.spelled,
			              spelled.sort_keys, i);
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
		// (language/guide.md §5.6; DecideNodeContainer::routed).
		if (!scope && landing[row] == DConstants::INVALID_INDEX) {
			landing[row] = node.fallback ? row_base + row : slot;
		}
	}
	Store(node_index, chunk, stored, stored_slots, row_base);
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

void DecideDescent::Timelines(idx_t node_index) {
	auto &node = *tree.nodes[node_index];
	auto &container = containers[node_index];
	auto &layout = container.layout;
	auto &timelines = container.timelines;
	const auto count = container.Count();
	// Each slot's entries in entry order, which is row order: a counting sort by slot.
	const auto slot_count = container.groups.size();
	vector<idx_t> begin(slot_count + 1, 0);
	for (auto slot : container.slots) {
		begin[slot + 1]++;
	}
	for (idx_t slot = 0; slot < slot_count; slot++) {
		begin[slot + 1] += begin[slot];
	}
	vector<idx_t> by_slot(count);
	auto next = begin;
	for (idx_t e = 0; e < count; e++) {
		by_slot[next[container.slots[e]]++] = e;
	}
	auto order_key = [&](idx_t e) {
		return FlatVector::GetData<string_t>(
		    container.values.Window(e / STANDARD_VECTOR_SIZE).data[layout.order_key])[e % STANDARD_VECTOR_SIZE];
	};
	auto compare = [](const string_t &left, const string_t &right) {
		const auto compared = memcmp(left.GetData(), right.GetData(), MinValue(left.GetSize(), right.GetSize()));
		return compared != 0 ? compared : (left.GetSize() < right.GetSize() ? -1 : (left.GetSize() > right.GetSize()));
	};
	// A timeline's entries by their order key's comparison bytes, then by row.
	auto before = [&](idx_t left, idx_t right) {
		const auto compared = compare(order_key(left), order_key(right));
		return compared != 0 ? compared < 0 : container.rows[left] < container.rows[right];
	};
	vector<idx_t> by_rank(slot_count);
	for (idx_t slot = 0; slot < slot_count; slot++) {
		by_rank[container.groups.ranks[slot]] = slot;
	}
	for (auto slot : by_rank) {
		auto first = by_slot.begin() + begin[slot];
		auto last = by_slot.begin() + begin[slot + 1];
		if (!std::is_sorted(first, last, before)) {
			std::stable_sort(first, last, before);
		}
		timelines.slots.push_back(slot);
		timelines.first_position.push_back(timelines.Positions());
		for (auto at = first; at != last;) {
			// A position: the entries of one order key.
			auto end = at + 1;
			while (end != last && compare(order_key(*at), order_key(*end)) == 0) {
				end++;
			}
			timelines.first_entry.push_back(timelines.entries.size());
			timelines.entries.insert(timelines.entries.end(), at, end);
			// Named by its least spelling (D69; flat's DecidePositionRows), and a duplicate position conflicts where
			// its entries' payload signatures differ.
			idx_t least = 0;
			bool conflict = false;
			if (end - at > 1) {
				vector<Value> spelled;
				for (auto e = at; e != end; e++) {
					spelled.push_back(container.Order(*e));
				}
				const auto sort_keys = SpellingKeys(spelled, node.over->return_type);
				const auto signature = container.Signature(*at);
				for (idx_t k = 1; k < sort_keys.size(); k++) {
					conflict = conflict || !DescentEqual(signature, container.Signature(at[k]));
					if (sort_keys[k] < sort_keys[least]) {
						least = k;
					}
				}
			}
			timelines.named_by.push_back(at[least]);
			timelines.conflict.push_back(conflict);
			at = end;
		}
	}
	timelines.first_position.push_back(timelines.Positions());
	timelines.first_entry.push_back(timelines.entries.size());
}

void DecideDescent::Finish() {
	if (finished) {
		return;
	}
	for (idx_t scope = 0; scope < decision_groups.size(); scope++) {
		decision_groups[scope].ranks = DescentRanks(decision_sort_keys[scope]);
	}
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		DescentRankGroups(containers[n].groups);
		if (tree.nodes[n]->kind == DecideNodeKind::FRAME && containers[n].layout.order != DConstants::INVALID_INDEX) {
			Timelines(n);
		}
	}
	finished = true;
}

string DecideDescent::Dump() const {
	std::ostringstream out;
	for (idx_t n = 0; n < containers.size(); n++) {
		auto &container = containers[n];
		auto &node = *tree.nodes[n];
		auto &groups = container.groups;
		const auto kind = node.kind == DecideNodeKind::SCOPE ? "E" : node.kind == DecideNodeKind::REDUCER ? "G" : "O";
		out << "node " << n << " " << kind << " rows " << container.rows_in << " -> " << container.rows_out
		    << " groups=" << groups.size() << " entries=" << container.Count()
		    << " instances=" << container.instance_entries.size() << " text=" << node.text << "\n";
		auto print = [&](const vector<Value> &values) {
			for (idx_t i = 0; i < values.size(); i++) {
				out << (i ? ", " : "") << values[i].ToString();
			}
		};
		for (idx_t slot = 0; slot < groups.size(); slot++) {
			out << "  group " << slot << " rank " << groups.ranks[slot] << " parent " << groups.parent_slots[slot]
			    << " key=(";
			print(groups.Key(slot));
			out << ")\n";
		}
		for (idx_t e = 0; e < container.Count(); e++) {
			out << "  row " << container.rows[e] << " parent " << container.parent_slots[e] << " slot "
			    << container.slots[e] << " instance " << container.instances[e] << " (";
			print(container.Signature(e));
			out << ") data=(";
			print(container.Columns(e));
			out << ")";
			if (node.kind == DecideNodeKind::FRAME) {
				out << " over=" << container.Order(e).ToString();
			}
			out << "\n";
		}
		auto &timelines = container.timelines;
		for (idx_t t = 0; t < timelines.size(); t++) {
			out << "  timeline " << timelines.slots[t] << " {";
			for (auto p = timelines.first_position[t]; p < timelines.first_position[t + 1]; p++) {
				out << container.Order(timelines.named_by[p]).ToString() << ":"
				    << timelines.first_entry[p + 1] - timelines.first_entry[p] << (timelines.conflict[p] ? "!" : "")
				    << ";";
			}
			out << "}\n";
		}
	}
	return out.str();
}

} // namespace duckdb
