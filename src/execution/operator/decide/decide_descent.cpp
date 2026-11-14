#include "duckdb/execution/operator/decide/decide_descent.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>

namespace duckdb {

static bool IsScopeNode(const DecideNode &node) {
	return node.kind == DecideNodeKind::SCOPE || node.kind == DecideNodeKind::FUNCTION;
}

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

//! A value's place in a key's order: NULL after every value, as ORDER BY places it.
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
//!: equal bytes are one spelling, so the least does not depend on the order rows are read in.
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
static vector<LogicalType> DescentSignatureTypes(const DecideTree &tree, const DecideNode &node,
                                                 const DecideSignatureField *one = nullptr) {
	vector<LogicalType> types;
	for (auto &field : node.signature) {
		if (one && &field != one) {
			continue;
		}
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

void DecideColumns::Resize(idx_t new_count) {
	D_ASSERT(new_count >= count);
	if (types.empty()) {
		count = new_count;
		return;
	}
	for (idx_t window = 0; window * STANDARD_VECTOR_SIZE < new_count; window++) {
		if (window == chunks.size()) {
			chunks.push_back(make_uniq<DataChunk>());
			chunks.back()->Initialize(*allocator, types);
		}
		chunks[window]->SetCardinality(
		    MinValue<idx_t>(STANDARD_VECTOR_SIZE, new_count - window * STANDARD_VECTOR_SIZE));
	}
	count = new_count;
}

void DecideColumns::CopyRows(idx_t target_row, const DecideColumns &source, idx_t source_row, idx_t rows) {
	if (types.empty()) {
		return;
	}
	auto &target = *chunks[target_row / STANDARD_VECTOR_SIZE];
	D_ASSERT((target_row + rows - 1) / STANDARD_VECTOR_SIZE == target_row / STANDARD_VECTOR_SIZE);
	for (idx_t done = 0; done < rows;) {
		const auto window = (source_row + done) / STANDARD_VECTOR_SIZE;
		const auto at = (source_row + done) % STANDARD_VECTOR_SIZE;
		const auto take = MinValue<idx_t>(rows - done, STANDARD_VECTOR_SIZE - at);
		for (idx_t c = 0; c < types.size(); c++) {
			VectorOperations::Copy(source.chunks[window]->data[c], target.data[c], at + take, at,
			                       (target_row + done) % STANDARD_VECTOR_SIZE);
		}
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
	D_ASSERT(!per_row);
	return keys.GetValues(slot, 0, keys.Types().size());
}

const DecideSpelling &DecideNodeGroups::Spelling(idx_t slot) const {
	return spellings[slot];
}

//! Ranks numbering `count` items by their sort keys, `keys` of `count` BLOBs as CreateSortKey writes them, equal keys
//! in item order.
static unsafe_vector<idx_t> DescentRanks(const vector<string_t> &keys) {
	vector<idx_t> order(keys.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](idx_t left, idx_t right) {
		auto &l = keys[left];
		auto &r = keys[right];
		const auto compared = memcmp(l.GetData(), r.GetData(), MinValue(l.GetSize(), r.GetSize()));
		return compared != 0 ? compared < 0 : l.GetSize() < r.GetSize();
	});
	unsafe_vector<idx_t> ranks(keys.size());
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

//! A node's slots by value: of the enclosing slot, then of the key.
static unsafe_vector<idx_t> DescentGroupRanks(const DecideNodeGroups &groups) {
	const auto count = groups.size();
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
	return DescentRanks(keys);
}

DecideDescent::DecideDescent(const DecideTree &tree_p, ClientContext &context_p)
    : tree(tree_p), context(context_p), containers(tree.nodes.size()), group_maps(tree.nodes.size()),
      dictionaries(tree.keys.size()), own_slots(tree.nodes.size()), key_reads(tree.keys.size()),
      empty_key_slots(tree.nodes.size()), entry_maps(tree.nodes.size()), instance_maps(tree.nodes.size()),
      signature_types(tree.nodes.size()), shared(tree.nodes.size(), false), enclosed(tree.nodes.size(), false),
      seen(tree.nodes.size()), landed(tree.nodes.size()), decision_groups(tree.keyed_scopes.size()),
      decision_sort_keys(tree.keyed_scopes.size()), decision_rows(tree.keyed_scopes.size()),
      decision_recorded(tree.keyed_scopes.size()), decision_layouts(tree.keyed_scopes.size()),
      decision_class_layouts(tree.keyed_scopes.size()), merged_classes(tree.keyed_scopes.size()),
      enclosing(tree.nodes.size(), DConstants::INVALID_INDEX), spellings(tree.keys.size()),
      failed_rows(tree.nodes.size()) {
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
	// The G or O whose slots a node's rows arrive in, `arriving` along one path: its parent's own, or the one its
	// parent's arrive in (HoldKeyed); MIXED_ENCLOSING where paths differ, as for an O two reducers' operands read.
	std::set<pair<idx_t, idx_t>> enclosed_paths;
	std::function<void(idx_t, idx_t)> enclose = [&](idx_t node, idx_t arriving) {
		if (!enclosed_paths.emplace(node, arriving).second) {
			return;
		}
		if (arriving != DConstants::INVALID_INDEX) {
			enclosing[node] = enclosing[node] == DConstants::INVALID_INDEX || enclosing[node] == arriving
			                      ? arriving
			                      : MIXED_ENCLOSING;
		}
		const auto &parent_node = *tree.nodes[node];
		const auto own = (parent_node.kind == DecideNodeKind::SCOPE ||
		                  (parent_node.kind == DecideNodeKind::FUNCTION && parent_node.per == DecideTreePer::ROW))
		                     ? arriving
		                     : node;
		for (auto child : tree.nodes[node]->children) {
			enclose(child, own);
		}
	};
	for (auto root : tree.roots) {
		enclose(root, DConstants::INVALID_INDEX);
	}
	// An E no node routes rows to is reached as a root alone, so its rows lie in no enclosing slot.
	for (auto &node : tree.nodes) {
		for (auto child : node->children) {
			const auto &parent = *node;
			enclosed[child] = IsScopeNode(*tree.nodes[child]) &&
			                  !(parent.kind == DecideNodeKind::FUNCTION && parent.per == DecideTreePer::ROW);
		}
	}
	auto &allocator = Allocator::Get(context);
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		auto &node = *tree.nodes[n];
		auto &container = containers[n];
		shared[n] = paths[n] > 1;
		signature_types[n] = DescentSignatureTypes(tree, node);
		// An entry's values: its signature's, its carried and program columns, and an O's order key and partition. An
		// entry standing for its row alone holds no decision's instance, which the row gives.
		const auto set = IsScopeNode(node) && node.per != DecideTreePer::ROW;
		auto &layout = container.layout;
		vector<LogicalType> types;
		idx_t value = 0;
		for (auto &field : node.signature) {
			const auto width = DescentSignatureTypes(tree, node, &field).size();
			for (idx_t w = 0; w < width; w++, value++) {
				const auto held = set || field.kind != DecideSignatureFieldKind::DECISION;
				layout.signature.push_back(held ? types.size() : DConstants::INVALID_INDEX);
				if (held) {
					types.push_back(signature_types[n][value]);
				}
			}
		}
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
		const auto scope = IsScopeNode(node);
		// A node per row has no key (DecideTreePer): each row is a slot of its own.
		D_ASSERT(!scope || node.per != DecideTreePer::ROW || node.key == DConstants::INVALID_INDEX);
		if (scope && node.per == DecideTreePer::ROW) {
			container.groups.per_row = true;
			continue;
		}
		auto key = DescentKeyTypes(tree, node.key);
		container.groups.keys.Initialize(allocator, key);
		// An empty key's slots are found by their enclosing slot alone, in a table (FindGroups); a key outside an
		// enclosing slot by its values alone, in the dictionary its nodes share.
		if (!key.empty() && enclosed[n]) {
			vector<LogicalType> slot_types {LogicalType::UBIGINT};
			slot_types.insert(slot_types.end(), key.begin(), key.end());
			group_maps[n] = make_uniq<DecideKeyMap>(context, slot_types);
		} else if (!key.empty() && !dictionaries[node.key]) {
			dictionaries[node.key] = make_uniq<SharedDictionary>();
			dictionaries[node.key]->map = make_uniq<DecideKeyMap>(context, key);
		}
		if (!scope) {
			continue;
		}
		// A set E: its instances by (slot, signature), its entries by those, its carried columns and its reads. A key
		// of the descent's own numbers alone -- a signature whose values are decisions' instances, no carried column --
		// is remembered by its slot (DecideKeyMap's memo).
		container.shares_instances = true;
		bool numbered = true;
		for (auto &field : node.signature) {
			numbered = numbered && (field.kind == DecideSignatureFieldKind::DECISION ||
			                        DescentSignatureTypes(tree, node, &field).empty());
		}
		vector<LogicalType> instance {LogicalType::UBIGINT};
		instance.insert(instance.end(), signature_types[n].begin(), signature_types[n].end());
		instance_maps[n] = make_uniq<DecideKeyMap>(context, instance, numbered);
		auto entry = instance;
		entry.insert(entry.end(), columns.begin(), columns.end());
		entry.insert(entry.end(), node.dependencies.size(), LogicalType::UBIGINT);
		entry_maps[n] = make_uniq<DecideKeyMap>(context, entry, numbered && columns.empty());
	}
}

DecideDescent::~DecideDescent() {
}

void DecideDescent::Evaluate(const Expression &expression, DataChunk &chunk, const Reach &rows, Vector &result) {
	if (rows.empty()) {
		return;
	}
	// A window's rows reach a node distinct and in order, so all of them are the window itself.
	DataChunk sliced;
	if (rows.size() != chunk.size()) {
		SelectionVector selection(rows.size());
		for (idx_t i = 0; i < rows.size(); i++) {
			selection.set_index(i, rows.rows[i]);
		}
		sliced.InitializeEmpty(chunk.GetTypes());
		sliced.Slice(chunk, selection, rows.size());
	}
	auto &selected = rows.size() != chunk.size() ? sliced : chunk;
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
			raised++;
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

void DecideDescent::KeyReads::Initialize(Allocator &allocator, const vector<LogicalType> &types, idx_t columns,
                                         idx_t window) {
	values.Initialize(allocator, types);
	spelled.spelled.resize(columns);
	spelled.sort_keys.resize(columns);
	position.assign(window, DConstants::INVALID_INDEX);
}

void DecideDescent::KeyReads::Reset(idx_t window) {
	if (!whole && values.size() > 0) {
		std::fill(position.begin(), position.end(), DConstants::INVALID_INDEX);
	}
	position.resize(window, DConstants::INVALID_INDEX);
	whole = false;
	values.Reset();
	for (idx_t c = 0; c < spelled.spelled.size(); c++) {
		spelled.spelled[c].clear();
		spelled.sort_keys[c].clear();
	}
}

void DecideDescent::KeyReads::CopyInto(Allocator &allocator, KeyReads &copy, idx_t room) const {
	copy.values.Initialize(allocator, values.GetTypes(), MaxValue<idx_t>(values.size() + room, 1));
	copy.values.Append(values, true);
	copy.spelled = spelled;
	copy.whole = whole;
	copy.position = position;
}

const DecideDescent::Reach &DecideDescent::KeyReads::Missing(const Reach &rows, Reach &own) const {
	if (whole) {
		return own;
	}
	if (values.size() == 0) {
		return rows;
	}
	for (idx_t i = 0; i < rows.size(); i++) {
		if (position[rows.rows[i]] == DConstants::INVALID_INDEX) {
			own.Add(rows.rows[i], rows.parents[i]);
		}
	}
	return own;
}

void DecideDescent::KeyReads::Hold(DataChunk &read, const KeySpellings &read_spelled, const Reach &missing,
                                   idx_t window) {
	for (idx_t c = 0; c < spelled.spelled.size(); c++) {
		auto &column = spelled.spelled[c];
		column.insert(column.end(), read_spelled.spelled[c].begin(), read_spelled.spelled[c].end());
		auto &sort_keys = spelled.sort_keys[c];
		sort_keys.insert(sort_keys.end(), read_spelled.sort_keys[c].begin(), read_spelled.sort_keys[c].end());
	}
	if (missing.size() == window && values.size() == 0) {
		// The first read of the window reads all of it: its values are held in place.
		for (idx_t c = 0; c < values.ColumnCount(); c++) {
			values.data[c].Reference(read.data[c]);
		}
		values.SetCardinality(window);
		whole = true;
		return;
	}
	const auto base = values.size();
	values.Append(read, true);
	for (idx_t j = 0; j < missing.size(); j++) {
		position[missing.rows[j]] = base + j;
	}
}

void DecideDescent::KeyReads::Slice(const Reach &rows, idx_t window, DataChunk &result) const {
	result.InitializeEmpty(values.GetTypes());
	const auto in_place = whole && rows.size() == window;
	SelectionVector selection(MaxValue<idx_t>(rows.size(), 1));
	for (idx_t i = 0; i < rows.size() && !in_place; i++) {
		selection.set_index(i, At(rows, i));
	}
	for (idx_t c = 0; c < values.ColumnCount(); c++) {
		if (in_place) {
			result.data[c].Reference(values.data[c]);
		} else {
			result.data[c].Slice(values.data[c], selection, rows.size());
		}
	}
	result.SetCardinality(rows.size());
}

void DecideDescent::ReadKey(idx_t key, DataChunk &chunk, const Reach &rows, const Reach &missing, DataChunk &values,
                            KeySpellings &spelled) {
	auto &elements = DecideKeyElements(tree.keys, key);
	values.Initialize(Allocator::Get(context), DescentKeyTypes(tree, key), MaxValue<idx_t>(missing.size(), 1));
	spelled.spelled.resize(elements.size());
	spelled.sort_keys.resize(elements.size());
	// A held row counts as read again, so the ranks of what the node reads next are those of a read of every row.
	auto evaluate = [&](const Expression &expression, Vector &result) {
		if (missing.empty()) {
			current_rank.expression += rows.empty() ? 0 : 1;
			return;
		}
		Evaluate(expression, chunk, missing, result);
	};
	// A collated column groups under its collation and is named by its spelling, read as GROUP BY reads both (D69).
	// The spelling is the same read of the column, at its rank: a row it fails on failed the grouping first.
	bool collated = false;
	for (idx_t c = 0; c < elements.size(); c++) {
		auto &element = elements[c];
		if (read_block != DecideBlock::ROUTE) {
			current_rank.position = c;
		}
		const auto rank = current_rank.expression;
		evaluate(element.Grouping(), values.data[c]);
		if (!element.collated) {
			continue;
		}
		current_rank.expression = rank;
		Vector spelling(element.expression->return_type);
		evaluate(*element.expression, spelling);
		collated = true;
		if (!missing.empty()) {
			spelled.spelled[c] = DescentColumnValues(spelling, missing.size());
			spelled.sort_keys[c] = SpellingKeys(spelled.spelled[c], element.expression->return_type);
		}
	}
	values.SetCardinality(missing.size());
	if (collated && !missing.empty()) {
		vector<vector<Value>> groups;
		for (idx_t i = 0; i < missing.size(); i++) {
			groups.push_back(DescentRowValues(values, 0, values.ColumnCount(), i));
		}
		Spell(key, groups, spelled.spelled, spelled.sort_keys);
	}
}

void DecideDescent::Key(idx_t key, DataChunk &chunk, const Reach &rows, DataChunk &values, unsafe_vector<bool> &present,
                        KeySpellings *read) {
	present.assign(rows.size(), true);
	auto &elements = DecideKeyElements(tree.keys, key);
	if (elements.empty()) {
		return;
	}
	auto &held = key_reads[key];
	if (held.values.ColumnCount() == 0) {
		held.Initialize(Allocator::Get(context), DescentKeyTypes(tree, key), elements.size(), chunk.size());
	}
	// The rows no read of the window holds yet, which this read evaluates. What it evaluates joins the held reads,
	// unless it raised on a row: then it is read beside them, this once, and each later read of the row raises anew.
	Reach own;
	auto &missing = held.Missing(rows, own);
	DataChunk evaluated;
	KeySpellings spelled;
	const auto raised_before = raised;
	ReadKey(key, chunk, rows, missing, evaluated, spelled);
	KeyReads once;
	auto &source = raised == raised_before ? held : once;
	if (raised != raised_before) {
		held.CopyInto(Allocator::Get(context), once, missing.size());
	}
	source.Hold(evaluated, spelled, missing, chunk.size());
	source.Slice(rows, chunk.size(), values);
	for (idx_t c = 0; c < elements.size(); c++) {
		if (elements[c].null_mode != DecideKeyNullMode::NO_GROUP || rows.empty()) {
			continue;
		}
		UnifiedVectorFormat format;
		values.data[c].ToUnifiedFormat(rows.size(), format);
		for (idx_t i = 0; i < rows.size() && !format.validity.AllValid(); i++) {
			if (!format.validity.RowIsValid(format.sel->get_index(i))) {
				present[i] = false;
			}
		}
	}
	bool collated = false;
	for (auto &element : elements) {
		collated = collated || element.collated;
	}
	if (!read || !collated) {
		return;
	}
	read->spelled.assign(elements.size(), vector<Value>());
	read->sort_keys.assign(elements.size(), vector<string>());
	for (idx_t c = 0; c < elements.size(); c++) {
		for (idx_t i = 0; i < rows.size() && elements[c].collated; i++) {
			read->spelled[c].push_back(source.spelled.spelled[c][source.At(rows, i)]);
			read->sort_keys[c].push_back(source.spelled.sort_keys[c][source.At(rows, i)]);
		}
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
		case DecideSignatureFieldKind::BY_KEY:
		case DecideSignatureFieldKind::FRAME: {
			// A field naming another node -- a reducer's `by` key, a frame's timeline and position -- reads that
			// node's values as a `by` key's read, which keeps the row: the node raises what it raises, where flat
			// reads it (a helper's G where flat builds the helper's classes, a frame's O where flat builds every
			// frame before any clause, language/guide.md §5.6).
			const auto block = read_block;
			const auto position = current_rank.position;
			read_block = DecideBlock::BY_KEY;
			const DecideNode *frame = nullptr;
			if (field.kind == DecideSignatureFieldKind::FRAME) {
				frame = DescentFrameNode(tree, field.frame);
				if (!frame || !frame->over) {
					throw InternalException("DECIDE tree: signature names missing frame %llu", field.frame);
				}
			}
			DataChunk keys;
			unsafe_vector<bool> present;
			Key(frame ? frame->key : field.key, chunk, rows, keys, present);
			for (idx_t c = 0; c < keys.ColumnCount(); c++) {
				values.data[column++].Reference(keys.data[c]);
			}
			if (frame && DescentFrameReadsOrder(*frame)) {
				Evaluate(*frame->over, chunk, rows, values.data[column++]);
			}
			read_block = block;
			current_rank.position = position;
			break;
		}
		case DecideSignatureFieldKind::DECISION: {
			vector<idx_t> classes;
			bool global = false;
			const auto scope = DecisionScope(field.decision, global);
			if (scope < tree.keyed_scopes.size()) {
				classes = DecisionClasses(field.decision, chunk, rows, row_base);
			}
			auto decisions = FlatVector::GetData<uint64_t>(values.data[column++]);
			auto instances = FlatVector::GetData<uint64_t>(values.data[column++]);
			for (idx_t i = 0; i < rows.size(); i++) {
				decisions[i] = field.decision;
				instances[i] = scope < tree.keyed_scopes.size() ? classes[i] : (global ? 0 : row_base + rows.rows[i]);
			}
			break;
		}
		}
	}
	values.SetCardinality(rows.size());
}

void DecideDescent::FindGroups(idx_t node_index, const Reach &rows, const unsafe_vector<idx_t> &selected,
                               DataChunk &keys, const unsafe_vector<bool> &present, idx_t row_base,
                               unsafe_vector<idx_t> &slots) {
	auto &node = *tree.nodes[node_index];
	auto &groups = containers[node_index].groups;
	const auto scope = IsScopeNode(node);
	// A slot made by row `i`, keyed by its row of `keys`, whose values the caller appends.
	auto create = [&](idx_t i) {
		groups.parent_slots.push_back(scope ? rows.parents[i] : DConstants::INVALID_INDEX);
		groups.first_rows.push_back(row_base + rows.rows[i]);
		groups.in_a_class.push_back(present[i]);
	};
	auto &map = group_maps[node_index];
	if (groups.keys.Types().empty()) {
		// An empty key: a G's or O's is one slot, an E's one per enclosing slot, found by it in a direct-address
		// table, the slot outside any last.
		auto &table = empty_key_slots[node_index];
		if (!enclosed[node_index] && !selected.empty()) {
			// Outside an enclosing slot every row lies in the one slot, the window's first row least among them.
			if (table.empty()) {
				table.push_back(groups.size());
				create(selected[0]);
				groups.keys.Append(keys, *FlatVector::IncrementalSelectionVector(), 1);
			}
			auto &first = groups.first_rows[table[0]];
			first = MinValue(first, row_base + rows.rows[selected[0]]);
			for (auto i : selected) {
				slots[i] = table[0];
			}
			return;
		}
		for (auto i : selected) {
			const auto parent = scope ? rows.parents[i] : DConstants::INVALID_INDEX;
			const auto at = parent == DConstants::INVALID_INDEX ? 0 : parent + 1;
			if (at >= table.size()) {
				table.resize(at + 1, DConstants::INVALID_INDEX);
			}
			if (table[at] == DConstants::INVALID_INDEX) {
				table[at] = groups.size();
				create(i);
				groups.keys.Append(keys, *FlatVector::IncrementalSelectionVector(), 1);
			}
			auto &first = groups.first_rows[table[at]];
			first = MinValue(first, row_base + rows.rows[i]);
			slots[i] = table[at];
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
	unsafe_vector<idx_t> found(count);
	if (map) {
		SelectionVector selection(count);
		for (idx_t j = 0; j < count; j++) {
			selection.set_index(j, selected[j]);
		}
		auto key_types = keys.GetTypes();
		vector<LogicalType> types {LogicalType::UBIGINT};
		types.insert(types.end(), key_types.begin(), key_types.end());
		DataChunk probe;
		probe.InitializeEmpty(types);
		Vector parents(LogicalType::UBIGINT);
		auto data = FlatVector::GetData<uint64_t>(parents);
		for (idx_t j = 0; j < count; j++) {
			data[j] = rows.parents[selected[j]];
		}
		probe.data[0].Reference(parents);
		for (idx_t c = 0; c < keys.ColumnCount(); c++) {
			probe.data[1 + c].Slice(keys.data[c], selection, count);
		}
		probe.SetCardinality(count);
		idx_t next = groups.size();
		map->FindOrCreate(probe, found.data(), next);
	} else {
		FindShared(node_index, rows, selected, keys, found);
	}
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

void DecideDescent::FindShared(idx_t node_index, const Reach &rows, const unsafe_vector<idx_t> &selected,
                               DataChunk &keys, unsafe_vector<idx_t> &found) {
	auto &dictionary = *dictionaries[tree.nodes[node_index]->key];
	auto &window_slots = dictionary.window_slots;
	// The rows whose slot no node has found in this window.
	const idx_t count = selected.size();
	SelectionVector missing(count);
	idx_t missing_count = 0;
	for (idx_t j = 0; j < count; j++) {
		if (window_slots[rows.rows[selected[j]]] == DConstants::INVALID_INDEX) {
			missing.set_index(missing_count++, selected[j]);
		}
	}
	if (missing_count > 0) {
		DataChunk probe;
		probe.InitializeEmpty(keys.GetTypes());
		probe.Slice(keys, missing, missing_count);
		unsafe_vector<idx_t> shared_slots(missing_count);
		dictionary.map->FindOrCreate(probe, shared_slots.data(), dictionary.count);
		for (idx_t k = 0; k < missing_count; k++) {
			window_slots[rows.rows[missing.get_index(k)]] = shared_slots[k];
		}
	}
	// The node numbers its own slots in the order its rows reach them: one it has not reached is its next.
	auto &own = own_slots[node_index];
	auto next = containers[node_index].groups.size();
	for (idx_t j = 0; j < count; j++) {
		const auto shared_slot = window_slots[rows.rows[selected[j]]];
		if (shared_slot >= own.size()) {
			own.resize(MaxValue<idx_t>(shared_slot + 1, own.size() * 2), DConstants::INVALID_INDEX);
		}
		if (own[shared_slot] == DConstants::INVALID_INDEX) {
			own[shared_slot] = next++;
		}
		found[j] = own[shared_slot];
	}
}

void DecideDescent::FindDecisionGroups(idx_t scope, DataChunk &keys, const unsafe_vector<bool> &present,
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
		decision_class_layouts[scope].push_back(NumericCast<idx_t>(layout - layouts.begin()));
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

idx_t DecideDescent::DecisionScope(idx_t variable, bool &global) const {
	global = false;
	if (variable >= tree.variable_scopes.size()) {
		return DConstants::INVALID_INDEX;
	}
	auto &scope = tree.variable_scopes[variable];
	global = scope.IsGlobal();
	return scope.IsKeyed() || scope.scope == DecideVarScope::INLINED ? scope.keyed_scope_idx
	                                                                 : DConstants::INVALID_INDEX;
}

vector<Value> DecideDescent::Signature(idx_t node_index, idx_t entry) const {
	auto &node = *tree.nodes[node_index];
	auto &container = containers[node_index];
	auto &layout = container.layout;
	vector<Value> values;
	idx_t value = 0;
	for (auto &field : node.signature) {
		if (field.kind == DecideSignatureFieldKind::DECISION && layout.signature[value] == DConstants::INVALID_INDEX) {
			// Its instance where its row reads it: a keyed decision's class there, a query-wide one's one, a row's
			// own.
			bool global;
			const auto scope = DecisionScope(field.decision, global);
			const auto row = container.rows[entry];
			values.push_back(Value::UBIGINT(field.decision));
			values.push_back(
			    Value::UBIGINT(scope < tree.keyed_scopes.size() ? decision_rows[scope][row] : (global ? 0 : row)));
			value += 2;
			continue;
		}
		const auto width = DescentSignatureTypes(tree, node, &field).size();
		for (idx_t w = 0; w < width; w++, value++) {
			values.push_back(container.values.GetValue(layout.signature[value], entry));
		}
	}
	return values;
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
		unsafe_vector<bool> present;
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
	probe.InitializeEmpty(types);
	Vector slots(LogicalType::UBIGINT, count);
	DescentIds(slots, batch.slots.data(), count);
	probe.data[0].Reference(slots);
	for (idx_t c = 0; c < value_columns; c++) {
		probe.data[1 + c].Reference(batch.values.data[c]);
	}
	// Where its row reads each node it depends on (DecideNodeContainer::routed); INVALID for a node whose rows land
	// nowhere (an E).
	for (idx_t d = 0; d < node.dependencies.size(); d++) {
		auto &landing = landed[node.dependencies[d]];
		Vector read(LogicalType::UBIGINT, count);
		auto reads = FlatVector::GetData<uint64_t>(read);
		for (idx_t i = 0; i < count; i++) {
			reads[i] = landing.empty() ? DConstants::INVALID_INDEX : landing.data()[batch.rows.rows[i]];
		}
		probe.data[1 + value_columns + d].Reference(read);
	}
	probe.SetCardinality(count);
	unsafe_vector<idx_t> entries(count);
	idx_t next = container.Count();
	entry_maps[batch.node]->FindOrCreate(probe, entries.data(), next);
	// The rows that make a new entry, each the first to read its values; every row routed, appended whole.
	unsafe_vector<idx_t> created;
	unsafe_vector<pair<idx_t, idx_t>> routed(count);
	for (idx_t i = 0; i < count; i++) {
		routed[i] = {row_base + batch.rows.rows[i], entries[i]};
		if (entries[i] == container.Count() + created.size()) {
			created.push_back(i);
		}
	}
	container.routed.insert(container.routed.end(), routed.begin(), routed.end());
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
	unsafe_vector<idx_t> instances(created.size());
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
		for (idx_t d = 0; d < node.dependencies.size(); d++) {
			container.reads.push_back(FlatVector::GetData<uint64_t>(probe.data[1 + value_columns + d])[i]);
		}
	}
	container.values.Append(batch.values, selection, created.size());
}

void DecideDescent::FrameOrder(const DecideNode &node, const DecideEntryLayout &layout, DataChunk &chunk,
                               const Reach &stored, DataChunk &values) {
	if (node.frame_spec) {
		DataChunk partition;
		unsafe_vector<bool> present;
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

void DecideDescent::Store(idx_t node_index, DataChunk &chunk, const Reach &stored, const unsafe_vector<idx_t> &slots,
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
	for (idx_t c = 0; c < layout.signature.size(); c++) {
		if (layout.signature[c] != DConstants::INVALID_INDEX) {
			values.data[layout.signature[c]].Reference(signatures.data[c]);
		}
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
	SelectionVector unfailed;
	const SelectionVector *kept = FlatVector::IncrementalSelectionVector();
	idx_t kept_count = stored.size();
	if (active_failed && !active_failed->empty()) {
		unfailed.Initialize(MaxValue<idx_t>(stored.size(), 1));
		kept_count = 0;
		for (idx_t i = 0; i < stored.size(); i++) {
			if (!Failed(stored.rows[i])) {
				unfailed.set_index(kept_count++, i);
			}
		}
		kept = &unfailed;
	}
	if (kept_count == 0) {
		return;
	}
	if (node.kind == DecideNodeKind::SCOPE && node.per != DecideTreePer::ROW) {
		// A set E's row reads the G and O nodes it depends on, its children and the helpers its decisions stand for,
		// which another root may route later in the window. So its entries wait for every root.
		pending.push_back(make_uniq<Pending>());
		auto &batch = *pending.back();
		batch.node = node_index;
		batch.rows.rows.resize(kept_count);
		batch.rows.parents.resize(kept_count);
		batch.slots.resize(kept_count);
		for (idx_t k = 0; k < kept_count; k++) {
			const auto i = kept->get_index(k);
			batch.rows.rows[k] = stored.rows[i];
			batch.rows.parents[k] = stored.parents[i];
			batch.slots[k] = slots[i];
		}
		if (layout.end > 0) {
			batch.values.InitializeEmpty(container.values.Types());
			batch.values.Slice(values, *kept, kept_count);
		}
		return;
	}
	// Each entry is an instance of its own; a node per row's entry is its own slot too (DecideNodeGroups::per_row).
	// Gathered a window at a time and appended whole, so the arrays' new room is written once.
	idx_t gathered[STANDARD_VECTOR_SIZE];
	for (idx_t k = 0; k < kept_count; k++) {
		gathered[k] = row_base + stored.rows[kept->get_index(k)];
	}
	container.rows.insert(container.rows.end(), gathered, gathered + kept_count);
	// A slot of two nodes' is told apart by neither, and no reader reads it: only a reducer's operand's are, an E
	// whose rows arrive in its reducer's slots alone. None is kept, and so none is merged (Merge).
	const auto mixed = enclosing[node_index] == MIXED_ENCLOSING;
	for (idx_t k = 0; k < kept_count; k++) {
		gathered[k] = mixed ? DConstants::INVALID_INDEX : stored.parents[kept->get_index(k)];
	}
	container.parent_slots.insert(container.parent_slots.end(), gathered, gathered + kept_count);
	if (container.groups.per_row) {
		container.groups.per_row_slots += kept_count;
	} else {
		for (idx_t k = 0; k < kept_count; k++) {
			gathered[k] = slots[kept->get_index(k)];
		}
		container.slots.insert(container.slots.end(), gathered, gathered + kept_count);
	}
	container.values.Append(values, *kept, kept_count);
}

const DecideDescent::Reach &DecideDescent::Unfailed(const Reach &rows, Reach &kept) const {
	if (!active_failed || active_failed->empty()) {
		return rows;
	}
	kept.Reserve(rows.size());
	for (idx_t i = 0; i < rows.size(); i++) {
		if (!Failed(rows.rows[i])) {
			kept.Add(rows.rows[i], rows.parents[i]);
		}
	}
	return kept;
}

const DecideDescent::Reach *DecideDescent::Gate(const DecideNode &node, DataChunk &chunk, const Reach *reaching,
                                                Reach &own) {
	for (idx_t k = 0; k < node.when.size(); k++) {
		// A reducer's own `when` is read in its terms' filter, at its place there (DecideNode::filter_group,
		// filter_part).
		const auto filter = node.filter_group != DConstants::INVALID_INDEX;
		read_block = filter ? DecideBlock::FILTERS : DecideBlock::ROUTE;
		current_rank.part = node.filter_part;
		current_rank.group = filter ? node.filter_group : 0;
		current_rank.position = filter ? k : 0;
		auto truth = Truth(*node.when[k], chunk, *reaching);
		read_block = DecideBlock::ROUTE;
		current_rank.part = current_rank.group = current_rank.position = 0;
		Reach kept;
		kept.Reserve(reaching->size());
		for (idx_t i = 0; i < reaching->size(); i++) {
			if (truth[i] == 1 && !Failed(reaching->rows[i])) {
				kept.Add(reaching->rows[i], reaching->parents[i]);
			}
		}
		own = std::move(kept);
		reaching = &own;
		if (own.empty()) {
			return nullptr;
		}
	}
	return reaching;
}

const DecideDescent::Reach &DecideDescent::HoldKeyed(idx_t node_index, DataChunk &chunk, const Reach &rows,
                                                     idx_t row_base, Reach &own_children) {
	auto &node = *tree.nodes[node_index];
	auto &container = containers[node_index];
	unsafe_vector<bool> present;
	KeySpellings spelled;
	DataChunk keys;
	// A grouped clause's keys are read where flat groups its kept rows, after its terms (DecideNode::key_block).
	read_block = node.key_block;
	Key(node.key, chunk, rows, keys, present, &spelled);
	read_block = DecideBlock::ROUTE;
	current_rank.position = 0;
	const auto scope = IsScopeNode(node);
	// Flat reads a reducer's operand on every row its G reads, except the objective's, which it reads in place, on the
	// rows of each group alone (D81).
	const auto reducer = node.kind == DecideNodeKind::REDUCER && node.value_rank.phase != DecideErrorPhase::OBJECTIVE;
	// A row in no class of the key stops here, unless the node holds such rows or is a reducer; an E's still reaches
	// its children, since `per` never gates them.
	auto unclassed = [&](idx_t i) {
		return !present[i] && !node.holds_rows_in_no_class && !reducer;
	};
	// The rows that take a slot: those no read failed on, in a class of the key or held anyway.
	const auto count = rows.size();
	const auto none_failed = !active_failed || active_failed->empty();
	unsafe_vector<idx_t> grouped;
	grouped.reserve(count);
	for (idx_t i = 0; i < count; i++) {
		if ((none_failed || !Failed(rows.rows[i])) && !unclassed(i)) {
			grouped.push_back(i);
		}
	}
	// Each row's slot, read for the rows that take one alone: a buffer kept across visits, as nothing reads it once
	// the node's children are reached.
	auto &slots = slot_buffer;
	slots.resize(MaxValue<idx_t>(slots.size(), count));
	FindGroups(node_index, rows, grouped, keys, present, row_base, slots);
	// A collated key's own least spelling over the rows the node holds in the group.
	if (!spelled.sort_keys.empty()) {
		auto &groups = container.groups;
		groups.spellings.resize(groups.size());
		for (auto i : grouped) {
			auto &spelling = groups.spellings[slots[i]];
			spelling.Fold(spelling.least.empty() ? groups.Key(slots[i]) : spelling.label, spelled.spelled,
			              spelled.sort_keys, i);
		}
	}
	// A row in no group of a G has no value of it (language/guide.md §5.2), so it holds no entry of the G; it still
	// reaches the G's operand, as flat computes a clause's terms on every kept row and a side plan a data reducer's
	// arguments, so an error there is raised (§7.2). Where every row takes a slot and an entry, they are the rows
	// themselves.
	Reach own_stored;
	unsafe_vector<idx_t> own_slots;
	const Reach *stored = &rows;
	const unsafe_vector<idx_t> *stored_slots = &slots;
	bool every_row = grouped.size() == count;
	for (idx_t k = 0; k < grouped.size() && every_row && reducer; k++) {
		every_row = present[grouped[k]];
	}
	if (!every_row) {
		own_stored.Reserve(grouped.size());
		own_slots.reserve(grouped.size());
		for (auto i : grouped) {
			if (present[i] || !reducer) {
				own_stored.Add(rows.rows[i], rows.parents[i]);
				own_slots.push_back(slots[i]);
			}
		}
		stored = &own_stored;
		stored_slots = &own_slots;
	}
	// A reader reads a G's or O's value at its group or timeline, and an `else` at the anchor's own row
	// (language/guide.md §5.6; DecideNodeContainer::routed).
	if (!scope) {
		auto landing = landed[node_index].data();
		for (idx_t k = 0; k < stored->size(); k++) {
			const auto row = stored->rows[k];
			if (landing[row] == DConstants::INVALID_INDEX) {
				landing[row] = node.fallback ? row_base + row : (*stored_slots)[k];
			}
		}
	}
	Store(node_index, chunk, *stored, *stored_slots, row_base);
	// An E's rows reach its children in the slots they arrived in, every row it reads, as `per` never gates them; a
	// G's or O's in its own, those that took one.
	if (node.kind == DecideNodeKind::SCOPE) {
		return rows;
	}
	own_children.rows.resize(grouped.size());
	own_children.parents.resize(grouped.size());
	for (idx_t k = 0; k < grouped.size(); k++) {
		own_children.rows[k] = rows.rows[grouped[k]];
		own_children.parents[k] = slots[grouped[k]];
	}
	return own_children;
}

void DecideDescent::Visit(idx_t node_index, DataChunk &chunk, idx_t row_base, const Reach &incoming) {
	auto &node = *tree.nodes[node_index];
	active_node = node_index;
	active_failed = &failed_rows[node_index];
	active_row_base = row_base;
	// Its `when` first, where flat reads it (DecideNode::when_rank), then the rest at its own rank.
	current_rank = node.when_rank;
	if (context.interrupted) {
		throw InterruptException();
	}
	auto &container = containers[node_index];
	// Shared DAG edges may route the same row and context twice. Its input value is inserted once, independently of
	// which reader visits first.
	Reach own;
	const Reach *reaching = &incoming;
	if (shared[node_index]) {
		for (idx_t i = 0; i < incoming.size(); i++) {
			if (seen[node_index].emplace(incoming.rows[i], incoming.parents[i]).second) {
				own.Add(incoming.rows[i], incoming.parents[i]);
			}
		}
		reaching = &own;
	}
	container.rows_in += reaching->size();
	reaching = Gate(node, chunk, reaching, own);
	if (!reaching) {
		return;
	}
	auto &rows = *reaching;
	current_rank.phase = node.route_rank.phase;
	current_rank.step = node.route_rank.step;
	current_rank.clause = node.route_rank.clause;
	if (rows.empty()) {
		return;
	}
	Reach own_children;
	const Reach *children = &rows;
	if (container.groups.per_row) {
		// An E per row: each row it holds is a slot of its own, its entry, and its children read every row it holds,
		// as `per` never gates them.
		current_rank.position = 0;
		Reach unfailed;
		auto &stored = Unfailed(rows, unfailed);
		Store(node_index, chunk, stored, {}, row_base);
	} else {
		children = &HoldKeyed(node_index, chunk, rows, row_base, own_children);
	}
	Reach unfailed;
	auto &reached = Unfailed(*children, unfailed);
	container.rows_out += reached.size();
	for (auto child : node.children) {
		Visit(child, chunk, row_base, reached);
	}
}

void DecideDescent::Append(DataChunk &chunk, idx_t row_base) {
	if (finished || merged) {
		throw InternalException("DECIDE tree: a row arrived after top-down construction finished");
	}
	Reach rows;
	for (idx_t row = 0; row < chunk.size(); row++) {
		rows.Add(row, DConstants::INVALID_INDEX);
	}
	// What a window's rows read and found is held for that window alone.
	for (auto &held : key_reads) {
		if (held.values.ColumnCount() > 0) {
			held.Reset(chunk.size());
		}
	}
	for (auto &dictionary : dictionaries) {
		if (dictionary) {
			dictionary->window_slots.assign(chunk.size(), DConstants::INVALID_INDEX);
		}
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
		unsafe_vector<bool> present;
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
		if (!IsScopeNode(*tree.nodes[n])) {
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

//! Rows `rows` of `source`, ascending, appended to `target` a window at a time.
static void DescentCopyRows(DecideColumns &target, const DecideColumns &source, const unsafe_vector<idx_t> &rows) {
	// No column holds a row of a key with none.
	if (source.Types().empty()) {
		return;
	}
	for (idx_t begin = 0; begin < rows.size();) {
		const auto window = rows[begin] / STANDARD_VECTOR_SIZE;
		SelectionVector selection(STANDARD_VECTOR_SIZE);
		idx_t count = 0;
		for (; begin < rows.size() && rows[begin] / STANDARD_VECTOR_SIZE == window; begin++) {
			selection.set_index(count++, rows[begin] % STANDARD_VECTOR_SIZE);
		}
		target.Append(source.Window(window), selection, count);
	}
}

void DecideSpelling::Fold(const DecideSpelling &other) {
	// Every slot of a collated key is spelled by the rows that made it (HoldKeyed).
	D_ASSERT(!other.least.empty());
	if (least.empty()) {
		*this = other;
		return;
	}
	for (idx_t c = 0; c < least.size(); c++) {
		if (!other.least[c].empty() && (least[c].empty() || other.least[c] < least[c])) {
			least[c] = other.least[c];
			label[c] = other.label[c];
		}
	}
}

void DecideDescent::Merge(MergedPart &part) {
	auto &other = *part.source;
	const auto offset = part.offset;
	if (finished || other.finished || &tree != &other.tree) {
		throw InternalException("DECIDE tree: a descent merges another of its tree before either finishes");
	}
	// Its keyed classes take the class of their layout and sort key here, a new one after every class here. A row
	// is not visited after a merge, so neither the layouts' dictionaries nor the rows recorded hold the merged ones.
	merged = true;
	vector<unsafe_vector<idx_t>> class_map(tree.keyed_scopes.size());
	auto class_key = [](const vector<LogicalType> &types, const string_t &sort_key) {
		string key;
		for (auto &type : types) {
			key += type.ToString() + ",";
		}
		key.append(sort_key.GetData(), sort_key.GetSize());
		return key;
	};
	for (idx_t scope = 0; scope < tree.keyed_scopes.size(); scope++) {
		auto &groups = decision_groups[scope];
		auto &theirs = other.decision_groups[scope];
		auto &known = merged_classes[scope];
		if (known.empty()) {
			for (idx_t c = 0; c < groups.size(); c++) {
				auto &types = decision_layouts[scope][decision_class_layouts[scope][c]].types;
				known.emplace(class_key(types, decision_sort_keys[scope][c]), c);
			}
		}
		auto &map = class_map[scope];
		map.resize(theirs.size());
		for (idx_t c = 0; c < theirs.size(); c++) {
			auto &sort_key = other.decision_sort_keys[scope][c];
			auto &types = other.decision_layouts[scope][other.decision_class_layouts[scope][c]].types;
			auto found = known.emplace(class_key(types, sort_key), groups.size());
			map[c] = found.first->second;
			if (!found.second) {
				continue;
			}
			groups.parent_slots.push_back(DConstants::INVALID_INDEX);
			groups.first_rows.push_back(theirs.first_rows[c] + offset);
			groups.in_a_class.push_back(true);
			decision_sort_keys[scope].push_back(sort_key.GetSize() == 0 ? string_t() : decision_heap.AddBlob(sort_key));
		}
		auto &mapping = decision_rows[scope];
		D_ASSERT(mapping.size() <= offset);
		mapping.resize(offset, DConstants::INVALID_INDEX);
		for (auto found : other.decision_rows[scope]) {
			mapping.push_back(found == DConstants::INVALID_INDEX ? found : map[found]);
		}
	}
	// Each node's slots, a G's or O's before an E's, whose enclosing slot is a G's or an O's: found by their key -- and
	// enclosing slot -- as a row of theirs would find them.
	auto &slot_map = part.slot_map;
	slot_map.assign(tree.nodes.size(), unsafe_vector<idx_t>());
	part.entry_map.assign(tree.nodes.size(), unsafe_vector<idx_t>());
	part.target.assign(tree.nodes.size(), 0);
	part.routed_target.assign(tree.nodes.size(), 0);
	auto parent_of = [&](idx_t node, idx_t slot) {
		return PartParent(part, node, slot);
	};
	auto merge_slots = [&](idx_t n) {
		auto &node = *tree.nodes[n];
		auto &groups = containers[n].groups;
		auto &theirs = other.containers[n].groups;
		if (theirs.per_row) {
			groups.per_row_slots += theirs.per_row_slots;
			return;
		}
		auto &map = slot_map[n];
		map.resize(theirs.size());
		unsafe_vector<idx_t> created;
		auto create = [&](idx_t slot, idx_t parent) {
			groups.parent_slots.push_back(parent);
			groups.first_rows.push_back(theirs.first_rows[slot] + offset);
			groups.in_a_class.push_back(theirs.in_a_class[slot]);
			created.push_back(slot);
		};
		if (theirs.keys.Types().empty()) {
			auto &table = empty_key_slots[n];
			for (idx_t slot = 0; slot < theirs.size(); slot++) {
				const auto parent = parent_of(n, theirs.parent_slots[slot]);
				const auto at = parent == DConstants::INVALID_INDEX ? 0 : parent + 1;
				if (at >= table.size()) {
					table.resize(at + 1, DConstants::INVALID_INDEX);
				}
				if (table[at] == DConstants::INVALID_INDEX) {
					table[at] = groups.size();
					create(slot, parent);
				}
				map[slot] = table[at];
			}
		} else {
			for (idx_t window = 0; window < theirs.keys.Windows(); window++) {
				auto &keys = theirs.keys.Window(window);
				const auto begin = window * STANDARD_VECTOR_SIZE;
				const auto count = keys.size();
				unsafe_vector<idx_t> found(count);
				if (group_maps[n]) {
					vector<LogicalType> types {LogicalType::UBIGINT};
					auto key_types = keys.GetTypes();
					types.insert(types.end(), key_types.begin(), key_types.end());
					DataChunk probe;
					probe.InitializeEmpty(types);
					Vector parents(LogicalType::UBIGINT, count);
					auto data = FlatVector::GetData<uint64_t>(parents);
					for (idx_t i = 0; i < count; i++) {
						data[i] = parent_of(n, theirs.parent_slots[begin + i]);
					}
					probe.data[0].Reference(parents);
					for (idx_t c = 0; c < keys.ColumnCount(); c++) {
						probe.data[1 + c].Reference(keys.data[c]);
					}
					probe.SetCardinality(count);
					idx_t next = groups.size();
					group_maps[n]->FindOrCreate(probe, found.data(), next);
				} else {
					auto &dictionary = *dictionaries[node.key];
					unsafe_vector<idx_t> shared_slots(count);
					dictionary.map->FindOrCreate(keys, shared_slots.data(), dictionary.count);
					auto &own = own_slots[n];
					auto next = groups.size();
					for (idx_t i = 0; i < count; i++) {
						if (shared_slots[i] >= own.size()) {
							own.resize(MaxValue<idx_t>(shared_slots[i] + 1, own.size() * 2), DConstants::INVALID_INDEX);
						}
						if (own[shared_slots[i]] == DConstants::INVALID_INDEX) {
							own[shared_slots[i]] = next++;
						}
						found[i] = own[shared_slots[i]];
					}
				}
				for (idx_t i = 0; i < count; i++) {
					if (found[i] == groups.size()) {
						create(begin + i, parent_of(n, theirs.parent_slots[begin + i]));
					}
					map[begin + i] = found[i];
				}
			}
		}
		DescentCopyRows(groups.keys, theirs.keys, created);
		if (!theirs.spellings.empty()) {
			groups.spellings.resize(groups.size());
			for (idx_t slot = 0; slot < theirs.spellings.size(); slot++) {
				groups.spellings[map[slot]].Fold(theirs.spellings[slot]);
			}
		}
	};
	for (auto scopes : {false, true}) {
		for (idx_t n = 0; n < tree.nodes.size(); n++) {
			if (IsScopeNode(*tree.nodes[n]) == scopes) {
				merge_slots(n);
			}
		}
	}
	// Each node's entries, rebased: a set E's found again by their slot, signature, carried columns and reads, a
	// decision's instance and a read rebased as the slots and classes are.
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		auto &node = *tree.nodes[n];
		auto &container = containers[n];
		auto &theirs = other.containers[n];
		container.rows_in += theirs.rows_in;
		container.rows_out += theirs.rows_out;
		auto &slots = slot_map[n];
		if (!container.shares_instances) {
			// Each entry is an instance of its own: copied after the entries held and those of the parts before
			// (CopyMergedParts).
			part.target[n] = container.Count() + planned_entries[n];
			planned_entries[n] += theirs.Count();
			continue;
		}
		const auto signature_columns = signature_types[n].size();
		const auto value_columns = signature_columns + node.columns.size();
		const auto reads = node.dependencies.size();
		// Per decision of the signature, its instance's column and its scope: rebased by the scope's classes, or by
		// the row for a decision per row. A query-wide decision is no field (DecideTree's sigma).
		vector<pair<idx_t, idx_t>> instances;
		idx_t value = 0;
		for (auto &field : node.signature) {
			if (field.kind == DecideSignatureFieldKind::DECISION) {
				bool global;
				instances.emplace_back(container.layout.signature[value + 1], DecisionScope(field.decision, global));
				D_ASSERT(!global);
			}
			value += DescentSignatureTypes(tree, node, &field).size();
		}
		auto rebase_read = [&](idx_t d, idx_t read) {
			auto &dependency = *tree.nodes[node.dependencies[d]];
			// An E's rows land nowhere: its read is INVALID.
			if (read == DConstants::INVALID_INDEX) {
				return read;
			}
			return dependency.fallback ? read + offset : slot_map[node.dependencies[d]][read];
		};
		auto types = container.values.Types();
		vector<LogicalType> probe_types {LogicalType::UBIGINT};
		probe_types.insert(probe_types.end(), types.begin(), types.begin() + value_columns);
		probe_types.insert(probe_types.end(), reads, LogicalType::UBIGINT);
		auto &entry_map = part.entry_map[n];
		entry_map.resize(theirs.Count());
		for (idx_t begin = 0; begin < theirs.Count(); begin += STANDARD_VECTOR_SIZE) {
			const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, theirs.Count() - begin);
			DataChunk values;
			if (!types.empty()) {
				values.InitializeEmpty(types);
				auto &window = theirs.values.Window(begin / STANDARD_VECTOR_SIZE);
				for (idx_t c = 0; c < types.size(); c++) {
					values.data[c].Reference(window.data[c]);
				}
			}
			for (auto &instance : instances) {
				const auto scope = instance.second;
				Vector rebased(LogicalType::UBIGINT, count);
				auto source = FlatVector::GetData<uint64_t>(values.data[instance.first]);
				auto target = FlatVector::GetData<uint64_t>(rebased);
				for (idx_t i = 0; i < count; i++) {
					const auto id = source[i];
					target[i] = id == DConstants::INVALID_INDEX    ? id
					            : scope < tree.keyed_scopes.size() ? class_map[scope][id]
					                                               : id + offset;
				}
				values.data[instance.first].Reference(rebased);
			}
			if (!types.empty()) {
				values.SetCardinality(count);
			}
			DataChunk probe;
			probe.InitializeEmpty(probe_types);
			Vector slot_ids(LogicalType::UBIGINT, count);
			auto slot_data = FlatVector::GetData<uint64_t>(slot_ids);
			for (idx_t i = 0; i < count; i++) {
				slot_data[i] = slots[theirs.slots[begin + i]];
			}
			probe.data[0].Reference(slot_ids);
			for (idx_t c = 0; c < value_columns; c++) {
				probe.data[1 + c].Reference(values.data[c]);
			}
			for (idx_t d = 0; d < reads; d++) {
				Vector read(LogicalType::UBIGINT, count);
				auto data = FlatVector::GetData<uint64_t>(read);
				for (idx_t i = 0; i < count; i++) {
					data[i] = rebase_read(d, theirs.reads[(begin + i) * reads + d]);
				}
				probe.data[1 + value_columns + d].Reference(read);
			}
			probe.SetCardinality(count);
			unsafe_vector<idx_t> entries(count);
			idx_t next = container.Count();
			entry_maps[n]->FindOrCreate(probe, entries.data(), next);
			unsafe_vector<idx_t> created;
			for (idx_t i = 0; i < count; i++) {
				entry_map[begin + i] = entries[i];
				if (entries[i] == container.Count() + created.size()) {
					created.push_back(i);
				}
			}
			SelectionVector selection(created.size());
			for (idx_t k = 0; k < created.size(); k++) {
				selection.set_index(k, created[k]);
			}
			DataChunk signatures;
			signatures.InitializeEmpty(
			    vector<LogicalType>(probe_types.begin(), probe_types.begin() + 1 + signature_columns));
			for (idx_t c = 0; c < signatures.ColumnCount(); c++) {
				signatures.data[c].Slice(probe.data[c], selection, created.size());
			}
			signatures.SetCardinality(created.size());
			unsafe_vector<idx_t> instance_ids(created.size());
			idx_t next_instance = container.instance_entries.size();
			instance_maps[n]->FindOrCreate(signatures, instance_ids.data(), next_instance);
			for (idx_t k = 0; k < created.size(); k++) {
				const auto i = created[k];
				if (instance_ids[k] == container.instance_entries.size()) {
					container.instance_entries.push_back(container.Count());
				}
				container.rows.push_back(theirs.rows[begin + i] + offset);
				container.parent_slots.push_back(parent_of(n, theirs.parent_slots[begin + i]));
				container.slots.push_back(slot_data[i]);
				container.instances.push_back(instance_ids[k]);
				for (idx_t d = 0; d < reads; d++) {
					container.reads.push_back(FlatVector::GetData<uint64_t>(probe.data[1 + value_columns + d])[i]);
				}
			}
			container.values.Append(values, selection, created.size());
		}
		part.routed_target[n] = container.routed.size() + planned_routed[n];
		planned_routed[n] += theirs.routed.size();
	}
	// The groups each key's reads spelled, and what the rows raised, at their rows here. What a visit reads of the
	// rows a read failed on is not merged: no row is visited after a merge.
	for (idx_t key = 0; key < spellings.size(); key++) {
		for (auto &bucket : other.spellings[key]) {
			auto &held = spellings[key][bucket.first];
			for (auto &spelling : bucket.second) {
				auto known = std::find_if(held.begin(), held.end(), [&](const KeySpelling &candidate) {
					return DescentEqual(candidate.group, spelling.group);
				});
				if (known == held.end()) {
					held.push_back(spelling);
				} else {
					known->spelling.Fold(spelling.spelling);
				}
			}
		}
	}
	other.errors.Rebase(offset);
	errors.Append(other.errors);
}

idx_t DecideDescent::PartParent(const MergedPart &part, idx_t node, idx_t slot) const {
	if (slot == DConstants::INVALID_INDEX) {
		return slot;
	}
	// A node in two nodes' slots keeps none (Store).
	D_ASSERT(enclosing[node] != DConstants::INVALID_INDEX && enclosing[node] != MIXED_ENCLOSING);
	return part.slot_map[enclosing[node]][slot];
}

namespace {

//! One of MergeAll's copies, run by DuckDB's scheduler.
class DescentCopyTask : public BaseExecutorTask {
public:
	DescentCopyTask(TaskExecutor &executor, std::function<void()> copy)
	    : BaseExecutorTask(executor), copy(std::move(copy)) {
	}

	void ExecuteTask() override {
		copy();
	}

private:
	std::function<void()> copy;
};

} // namespace

void DecideDescent::CopyMergedParts(vector<MergedPart> &parts, ClientContext &context) {
	// Each node's containers take their merged size first, a task per node, so the copies only write: a window of a
	// node's entries, and a part's routed rows of a set E, each by one task.
	vector<idx_t> held(tree.nodes.size());
	vector<idx_t> routed_held(tree.nodes.size());
	{
		TaskExecutor sizing(context);
		for (idx_t n = 0; n < tree.nodes.size(); n++) {
			auto &container = containers[n];
			held[n] = container.Count();
			routed_held[n] = container.routed.size();
			sizing.ScheduleTask(make_uniq<DescentCopyTask>(sizing, [&, n]() {
				const auto total = held[n] + planned_entries[n];
				container.rows.resize(total);
				container.parent_slots.resize(total);
				if (!container.groups.per_row) {
					container.slots.resize(total);
				}
				container.values.Resize(total);
				container.routed.resize(routed_held[n] + planned_routed[n]);
			}));
		}
		sizing.WorkOnTasks();
	}
	TaskExecutor executor(context);
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		auto &container = containers[n];
		const auto total = held[n] + planned_entries[n];
		for (idx_t window = held[n] / STANDARD_VECTOR_SIZE; window * STANDARD_VECTOR_SIZE < total; window++) {
			executor.ScheduleTask(make_uniq<DescentCopyTask>(executor, [&, n, window, total]() {
				const auto low = MaxValue<idx_t>(window * STANDARD_VECTOR_SIZE, held[n]);
				const auto high = MinValue<idx_t>((window + 1) * STANDARD_VECTOR_SIZE, total);
				for (auto &part : parts) {
					auto &theirs = part.source->containers[n];
					const auto begin = MaxValue<idx_t>(low, part.target[n]);
					const auto end = MinValue<idx_t>(high, part.target[n] + theirs.Count());
					for (idx_t e = begin; e < end; e++) {
						const auto source = e - part.target[n];
						container.rows[e] = theirs.rows[source] + part.offset;
						container.parent_slots[e] = PartParent(part, n, theirs.parent_slots[source]);
						if (!container.groups.per_row) {
							container.slots[e] = part.slot_map[n][theirs.slots[source]];
						}
					}
					if (begin < end) {
						container.values.CopyRows(begin, theirs.values, begin - part.target[n], end - begin);
					}
				}
			}));
		}
		for (auto &part : parts) {
			auto &theirs = part.source->containers[n].routed;
			executor.ScheduleTask(make_uniq<DescentCopyTask>(executor, [&, n]() {
				auto &map = part.entry_map[n];
				for (idx_t i = 0; i < theirs.size(); i++) {
					container.routed[part.routed_target[n] + i] = {theirs[i].first + part.offset,
					                                               map[theirs[i].second]};
				}
			}));
		}
	}
	executor.WorkOnTasks();
	planned_entries.assign(tree.nodes.size(), 0);
	planned_routed.assign(tree.nodes.size(), 0);
}

unique_ptr<DecideDescent> DecideDescent::MergeAll(vector<pair<unique_ptr<DecideDescent>, idx_t>> parts,
                                                  ClientContext &context) {
	D_ASSERT(!parts.empty() && parts[0].second == 0);
	auto merged = std::move(parts[0].first);
	merged->planned_entries.assign(merged->tree.nodes.size(), 0);
	merged->planned_routed.assign(merged->tree.nodes.size(), 0);
	vector<MergedPart> planned(parts.size() - 1);
	for (idx_t p = 1; p < parts.size(); p++) {
		planned[p - 1].source = parts[p].first.get();
		planned[p - 1].offset = parts[p].second;
		merged->Merge(planned[p - 1]);
	}
	merged->CopyMergedParts(planned, context);
	return merged;
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
			// Named by its least spelling (D69), and a duplicate position conflicts where
			// its entries' payload signatures differ.
			idx_t least = 0;
			bool conflict = false;
			if (end - at > 1) {
				vector<Value> spelled;
				for (auto e = at; e != end; e++) {
					spelled.push_back(container.Order(*e));
				}
				const auto sort_keys = SpellingKeys(spelled, node.over->return_type);
				const auto signature = Signature(node_index, *at);
				for (idx_t k = 1; k < sort_keys.size(); k++) {
					conflict = conflict || !DescentEqual(signature, Signature(node_index, at[k]));
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
		if (!containers[n].groups.per_row) {
			containers[n].groups.ranks = DescentGroupRanks(containers[n].groups);
		}
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
		const auto kind = node.kind == DecideNodeKind::SCOPE       ? "E"
		                  : node.kind == DecideNodeKind::REDUCER   ? "G"
		                  : node.kind == DecideNodeKind::FRAME     ? "O"
		                                                           : "F";
		out << "node " << n << " " << kind << " rows " << container.rows_in << " -> " << container.rows_out
		    << " groups=" << groups.size() << " entries=" << container.Count() << " instances=" << container.Instances()
		    << " text=" << node.text << "\n";
		auto print = [&](const vector<Value> &values) {
			for (idx_t i = 0; i < values.size(); i++) {
				out << (i ? ", " : "") << values[i].ToString();
			}
		};
		if (groups.per_row) {
			// A node per row's slots are its entries, numbered here alone, by enclosing slot and then row: no reader
			// numbers them.
			vector<idx_t> order(container.Count());
			std::iota(order.begin(), order.end(), 0);
			std::stable_sort(order.begin(), order.end(), [&](idx_t left, idx_t right) {
				return container.parent_slots[left] != container.parent_slots[right]
				           ? container.parent_slots[left] < container.parent_slots[right]
				           : container.rows[left] < container.rows[right];
			});
			vector<idx_t> ranks(order.size());
			for (idx_t rank = 0; rank < order.size(); rank++) {
				ranks[order[rank]] = rank;
			}
			for (idx_t slot = 0; slot < container.Count(); slot++) {
				out << "  group " << slot << " rank " << ranks[slot] << " parent " << container.parent_slots[slot]
				    << " key=(" << container.rows[slot] << ")\n";
			}
		}
		for (idx_t slot = 0; slot < groups.size() && !groups.per_row; slot++) {
			out << "  group " << slot << " rank " << groups.ranks[slot] << " parent " << groups.parent_slots[slot]
			    << " key=(";
			print(groups.Key(slot));
			out << ") first " << groups.first_rows[slot] << (groups.in_a_class[slot] ? "" : " unclassed");
			if (slot < groups.spellings.size() && !groups.spellings[slot].least.empty()) {
				out << " spelled=(";
				print(groups.spellings[slot].label);
				out << ")";
			}
			out << "\n";
		}
		for (idx_t e = 0; e < container.Count(); e++) {
			out << "  row " << container.rows[e] << " parent " << container.parent_slots[e] << " slot "
			    << container.Slot(e) << " instance " << container.Instance(e) << " (";
			print(Signature(n, e));
			out << ") data=(";
			print(container.Columns(e));
			out << ")";
			if (node.kind == DecideNodeKind::FRAME) {
				out << " over=" << container.Order(e).ToString();
			}
			for (idx_t d = 0; container.shares_instances && d < node.dependencies.size(); d++) {
				const auto read = container.reads[e * node.dependencies.size() + d];
				out << (d ? ", " : " reads=(") << (read == DConstants::INVALID_INDEX ? "-" : std::to_string(read))
				    << (d + 1 == node.dependencies.size() ? ")" : "");
			}
			out << "\n";
		}
		if (!container.routed.empty()) {
			out << "  routed";
			for (auto &routed : container.routed) {
				out << " " << routed.first << ":" << routed.second;
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
	// The keyed scopes' classes, and the class each row reads.
	for (idx_t scope = 0; scope < decision_groups.size(); scope++) {
		auto &groups = decision_groups[scope];
		out << "scope " << scope << " classes=" << groups.size() << "\n";
		for (idx_t c = 0; c < groups.size(); c++) {
			out << "  class " << c << " rank " << groups.ranks[c] << " first " << groups.first_rows[c] << "\n";
		}
		out << "  rows";
		for (idx_t row = 0; row < decision_rows[scope].size(); row++) {
			const auto found = decision_rows[scope][row];
			if (found != DConstants::INVALID_INDEX) {
				out << " " << row << ":" << found;
			}
		}
		out << "\n";
	}
	return out.str();
}

} // namespace duckdb
