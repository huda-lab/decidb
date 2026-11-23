//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_tree_columns.cpp
//
// Each node's programs over its entries (decide_tree_columns.hpp): partial evaluation, as DuckDB's executor runs a
// projection, with no expression interpreted per entry.
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_tree_columns.hpp"

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <sstream>

namespace duckdb {

DecideAggregateStates::DecideAggregateStates(ClientContext &context, const BoundAggregateExpression &aggregate_p,
                                             idx_t slots)
    : aggregate(aggregate_p), allocator(BufferAllocator::Get(context)), input(aggregate.bind_info.get(), allocator),
      slot_count(slots), state_size(AlignValue(aggregate.function.state_size(aggregate.function))),
      states(allocator.AllocateAligned(MaxValue<idx_t>(slots, 1) * state_size)),
      carries_nan(StringUtil::Lower(aggregate.function.name) != "count"), read_nan(slots, false) {
	// The binder admits no DISTINCT reducer, so every member is folded.
	D_ASSERT(!aggregate.IsDistinct());
	for (idx_t slot = 0; slot < slot_count; slot++) {
		aggregate.function.initialize(aggregate.function, states + slot * state_size);
	}
}

DecideAggregateStates::~DecideAggregateStates() {
	if (!aggregate.function.destructor) {
		return;
	}
	for (idx_t offset = 0; offset < slot_count; offset += STANDARD_VECTOR_SIZE) {
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, slot_count - offset);
		Vector pointers(LogicalType::POINTER);
		auto addresses = FlatVector::GetData<data_ptr_t>(pointers);
		for (idx_t i = 0; i < count; i++) {
			addresses[i] = states + (offset + i) * state_size;
		}
		aggregate.function.destructor(pointers, input, count);
	}
}

void DecideAggregateStates::Update(const vector<const Vector *> &arguments, const vector<idx_t> &members,
                                   const vector<idx_t> &slots) {
	// A NaN has no place in an order or a sum: the slot that reads one is NaN, wherever it lies among the members.
	const Vector *read = arguments.empty() ? nullptr : arguments[0];
	for (idx_t offset = 0; offset < members.size(); offset += STANDARD_VECTOR_SIZE) {
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, members.size() - offset);
		SelectionVector selection(count);
		for (idx_t i = 0; i < count; i++) {
			selection.set_index(i, members[offset + i]);
		}
		if (carries_nan && read) {
			Vector values(*read, selection, count);
			for (auto entry : DecideNanEntries(values, count)) {
				read_nan[slots[offset + entry]] = true;
			}
		}
		vector<Vector> sources;
		for (idx_t a = 0; a < arguments.size(); a++) {
			// A program computes its argument as the reducer binds it.
			D_ASSERT(arguments[a]->GetType() == aggregate.children[a]->return_type);
			sources.emplace_back(*arguments[a], selection, count);
		}
		Vector targets(LogicalType::POINTER);
		auto addresses = FlatVector::GetData<data_ptr_t>(targets);
		for (idx_t i = 0; i < count; i++) {
			addresses[i] = states + slots[offset + i] * state_size;
		}
		aggregate.function.update(sources.data(), input, sources.size(), targets, count);
	}
}

vector<Value> DecideAggregateStates::Finalize() {
	vector<Value> values;
	values.reserve(slot_count);
	for (idx_t offset = 0; offset < slot_count; offset += STANDARD_VECTOR_SIZE) {
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, slot_count - offset);
		Vector pointers(LogicalType::POINTER);
		auto addresses = FlatVector::GetData<data_ptr_t>(pointers);
		for (idx_t i = 0; i < count; i++) {
			addresses[i] = states + (offset + i) * state_size;
		}
		Vector result(aggregate.return_type);
		aggregate.function.finalize(pointers, input, result, count, 0);
		for (idx_t i = 0; i < count; i++) {
			values.push_back(read_nan[offset + i] ? DecideNanValue(aggregate.return_type) : result.GetValue(i));
		}
	}
	return values;
}

//! A program's values on its node's entries, and the entries its evaluation raised on, with the error.
struct DecideTreeColumns::ProgramColumn {
	unique_ptr<Expression> folded;
	unique_ptr<Vector> values;
	std::map<idx_t, ErrorData> errors;
	//! Per window that raised, by its first entry, the error its whole evaluation raised.
	std::map<idx_t, ErrorData> window_errors;
};

struct DecideTreeColumns::NodeColumns {
	vector<ProgramColumn> programs;
	vector<bool> tested;
	//! Whether every program is computed and the member tests are read, not only an O's frame programs.
	bool complete = false;
};

DecideTreeColumns::DecideTreeColumns(const DecideTree &tree, const DecideDescent &descent, ClientContext &context,
                                     const DecideSourceNames &names, const DecideDebug &debug)
    : tree(tree), descent(descent), context(context), names(names), nodes(tree.nodes.size()), absence(tree),
      injection(ReadDecideTreeInjection(tree, debug)) {
}

DecideTreeColumns::~DecideTreeColumns() {
}

DecideTreeColumns::NodeColumns &DecideTreeColumns::Computed(idx_t node) {
	if (!nodes[node]) {
		nodes[node] = make_uniq<NodeColumns>();
	}
	auto &columns = *nodes[node];
	if (!columns.complete) {
		ComputeNode(node, columns, true);
		ReadTests(node, columns);
		columns.complete = true;
	}
	return columns;
}

DecideTreeColumns::NodeColumns &DecideTreeColumns::FramePrograms(idx_t node) {
	if (!nodes[node]) {
		nodes[node] = make_uniq<NodeColumns>();
		ComputeNode(node, *nodes[node], false);
	}
	return *nodes[node];
}

bool DecideTreeColumns::ReadsFrame(idx_t index, idx_t program) const {
	auto &node = *tree.nodes[index];
	const auto holes = node.columns.size() + node.program_columns.size();
	bool reads = false;
	ExpressionIterator::EnumerateExpression(node.programs[program].expression, [&](Expression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			const auto index = expression.Cast<BoundReferenceExpression>().index;
			reads = reads || (index >= holes && index - holes < node.holes.size() &&
			                  node.holes[index - holes].column != DConstants::INVALID_INDEX);
		}
	});
	return reads;
}

void DecideTreeColumns::ComputeNode(idx_t index, NodeColumns &columns, bool frames) {
	auto &node = *tree.nodes[index];
	auto &container = descent.Containers()[index];
	const idx_t count = container.Count();
	// The node's layout (DecideProgram): its carried columns, then its program columns, as its entries hold them, then
	// its holes, the data reducers and frame columns they read; with no frame read, a frame column's hole is NULL.
	vector<LogicalType> layout;
	for (auto *carried : {&node.columns, &node.program_columns}) {
		for (auto &column : *carried) {
			layout.push_back(column->return_type);
		}
	}
	vector<unique_ptr<Vector>> holes;
	for (idx_t h = 0; h < node.holes.size(); h++) {
		layout.push_back(node.holes[h].type);
		if (frames || node.holes[h].column == DConstants::INVALID_INDEX) {
			holes.push_back(HoleValues(index, h));
			continue;
		}
		holes.push_back(make_uniq<Vector>(node.holes[h].type));
		holes.back()->SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(*holes.back(), true);
	}
	columns.programs.resize(node.programs.size());
	vector<idx_t> over_layout, computed;
	for (idx_t p = 0; p < node.programs.size(); p++) {
		auto &column = columns.programs[p];
		if (column.values || (!frames && ReadsFrame(index, p))) {
			continue;
		}
		// Folded where it is executed, as every expression stage 08 evaluates is (TransformToChunkExpression). One that
		// folds to a constant is that constant on every entry, read in place.
		column.folded = DecideFoldConstants(node.programs[p].expression->Copy(), context);
		if (column.folded->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			column.values = make_uniq<Vector>(column.folded->Cast<BoundConstantExpression>().value);
			continue;
		}
		computed.push_back(p);
		column.values = make_uniq<Vector>(column.folded->return_type, MaxValue<idx_t>(count, 1));
		if (node.programs[p].arguments.empty()) {
			over_layout.push_back(p);
		}
	}
	// One executor runs every program over the layout, a window of entries at a time; then each program that reads
	// others' results (an admitted end its a and r) runs over the layout and theirs.
	auto evaluate = [&](const vector<idx_t> &programs, const vector<idx_t> &arguments) {
		auto remake = [&]() {
			auto executor = make_uniq<ExpressionExecutor>(context);
			for (auto p : programs) {
				executor->AddExpression(*columns.programs[p].folded);
			}
			return executor;
		};
		auto executor = remake();
		auto types = layout;
		for (auto argument : arguments) {
			types.push_back(columns.programs[argument].values->GetType());
		}
		DataChunk chunk;
		chunk.Initialize(context, types);
		// The carried and program columns as the entries hold them, a window of entries in place.
		const auto carried = node.columns.size() + node.program_columns.size();
		for (idx_t start = 0; start < count; start += STANDARD_VECTOR_SIZE) {
			const idx_t size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - start);
			chunk.Reset();
			for (idx_t j = 0; j < carried; j++) {
				chunk.data[j].Reference(
				    container.values.Window(start / STANDARD_VECTOR_SIZE).data[container.layout.columns + j]);
			}
			for (idx_t h = 0; h < holes.size(); h++) {
				chunk.data[layout.size() - holes.size() + h].Slice(*holes[h], start, start + size);
			}
			for (idx_t a = 0; a < arguments.size(); a++) {
				chunk.data[layout.size() + a].Slice(*columns.programs[arguments[a]].values, start, start + size);
			}
			chunk.SetCardinality(size);
			for (idx_t e = 0; e < programs.size(); e++) {
				auto &column = columns.programs[programs[e]];
				ErrorData raised;
				DecideEvaluateWindow(context, executor, e, remake, *column.folded, *column.values, column.errors, chunk,
				                     start, &raised);
				if (raised.HasError()) {
					column.window_errors.emplace(start, std::move(raised));
				}
			}
		}
	};
	evaluate(over_layout, {});
	for (auto p : computed) {
		if (!node.programs[p].arguments.empty()) {
			evaluate({p}, node.programs[p].arguments);
		}
	}
	for (auto p : computed) {
		if (frames && ReadsFrame(index, p)) {
			CarryFrameErrors(index, p, columns.programs[p]);
		}
	}
}

void DecideTreeColumns::CarryFrameErrors(idx_t index, idx_t program, ProgramColumn &column) {
	auto &node = *tree.nodes[index];
	auto &rows = descent.Containers()[index].rows;
	const auto base = node.columns.size() + node.program_columns.size();
	ExpressionIterator::EnumerateExpression(node.programs[program].expression, [&](Expression &expression) {
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_REF) {
			return;
		}
		const auto hole = expression.Cast<BoundReferenceExpression>().index - base;
		if (hole >= node.holes.size() || node.holes[hole].column == DConstants::INVALID_INDEX) {
			return;
		}
		auto &carried = frame_values.errors[node.holes[hole].column];
		for (idx_t e = 0; e < rows.size() && !carried.empty(); e++) {
			auto error = carried.find(rows[e]);
			if (error != carried.end()) {
				column.errors[e] = error->second;
			}
		}
	});
}

void DecideTreeColumns::ReadTests(idx_t index, NodeColumns &columns) {
	const idx_t count = descent.Containers()[index].Count();
	// An entry whose member test is false is no member: its reducer reads no term on its row. A test that raises
	// where an entry was reached raises, as it does reading the reducer's filter.
	columns.tested.assign(count, true);
	for (auto p : tree.nodes[index]->tests) {
		auto &column = columns.programs[p];
		if (!column.errors.empty()) {
			column.errors.begin()->second.Throw();
		}
		UnifiedVectorFormat format;
		column.values->ToUnifiedFormat(count, format);
		auto truth = UnifiedVectorFormat::GetData<bool>(format);
		std::vector<bool> &tested = columns.tested;
		for (idx_t e = 0; e < count; e++) {
			const auto i = format.sel->get_index(e);
			if (!format.validity.RowIsValid(i) || !truth[i]) {
				tested[e] = false;
			}
		}
	}
}

void DecideTreeColumns::Report(idx_t node, idx_t program, const vector<bool> &reads) {
	for (auto &error : Computed(node).programs[program].errors) {
		if (reads[error.first]) {
			error.second.Throw();
		}
	}
}

const vector<bool> &DecideTreeColumns::Tested(idx_t node) {
	return Computed(node).tested;
}

const vector<bool> &DecideTreeColumns::ReadEntries(idx_t node, const DecideTerm &term, vector<bool> &present) {
	auto &tested = Tested(node);
	if (!absence.MayBe(term)) {
		return tested;
	}
	// A non-row E entry can stand for several input rows. An auxiliary that is absent on the entry's first row must
	// not hide a value it has on another row. Keep the entry when any represented row can read the term; the emitter
	// checks absence again for each row.
	auto &container = descent.Containers()[node];
	present.assign(tested.size(), false);
	if (tree.nodes[node]->per == DecideTreePer::ROW) {
		for (idx_t entry = 0; entry < present.size(); entry++) {
			present[entry] = tested[entry] && !absence.TermAt(term, container.rows[entry]);
		}
	} else {
		for (auto &read : container.routed) {
			present[read.second] = present[read.second] || (tested[read.second] && !absence.TermAt(term, read.first));
		}
	}
	return present;
}

vector<double> DecideTreeColumns::Numbers(idx_t node, idx_t p, const vector<bool> &reads, vector<bool> *nulls) {
	auto &program = tree.nodes[node]->programs[p];
	Report(node, p, reads);
	auto &column = *Computed(node).programs[p].values;
	const idx_t count = descent.Containers()[node].Count();
	D_ASSERT(reads.size() == count && (!nulls || nulls->size() == count));
	const ValueSource source {program.role == DecideProgramRole::COEFFICIENT ? DecideValueRole::COEFFICIENT
	                                                                         : DecideValueRole::BOUND,
	                          *program.written, names};
	vector<double> out;
	out.reserve(count);
	for (idx_t start = 0; start < count; start += STANDARD_VECTOR_SIZE) {
		const idx_t size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - start);
		Vector window(column, start, start + size);
		ExtractDoubleColumn(window, size, program.sign, out, source, nulls, &reads, start);
	}
	return out;
}

vector<bool> DecideTreeColumns::NullEntries(idx_t node, idx_t program, const vector<bool> &reads) {
	auto &computed = Computed(node).programs[program];
	auto &rows = descent.Containers()[node].rows;
	for (auto &error : computed.errors) {
		if (reads[error.first]) {
			error.second.Throw();
		}
	}
	auto &column = *computed.values;
	const auto count = rows.size();
	UnifiedVectorFormat format;
	column.ToUnifiedFormat(count, format);
	// None where the program holds no NULL.
	vector<bool> nulls;
	if (format.validity.AllValid()) {
		return nulls;
	}
	nulls.assign(count, false);
	std::vector<bool> &marked = nulls;
	const std::vector<bool> &read = reads;
	for (idx_t e = 0; e < count; e++) {
		if (read[e] && !format.validity.RowIsValid(format.sel->get_index(e))) {
			marked[e] = true;
		}
	}
	return nulls;
}

Value DecideTreeColumns::ValueAt(idx_t node, idx_t program, idx_t entry) {
	return Computed(node).programs[program].values->GetValue(entry);
}

//! `source`'s values at `selected` (INVALID for none, NULL) as `type`, a vector of `selected.size()` values.
static unique_ptr<Vector> DecideGather(const Vector &source, idx_t source_count, const vector<idx_t> &selected,
                                       const LogicalType &type) {
	const auto count = selected.size();
	// One NULL after the source's values stands for every row that reads none.
	Vector extended(source.GetType(), source_count + 1);
	VectorOperations::Copy(source, extended, source_count, 0, 0);
	FlatVector::SetNull(extended, source_count, true);
	SelectionVector selection(MaxValue<idx_t>(count, 1));
	for (idx_t i = 0; i < count; i++) {
		selection.set_index(i, selected[i] == DConstants::INVALID_INDEX ? source_count : selected[i]);
	}
	auto result = make_uniq<Vector>(source.GetType(), MaxValue<idx_t>(count, 1));
	VectorOperations::Copy(extended, *result, selection, count, 0, 0);
	if (type == source.GetType()) {
		return result;
	}
	auto cast = make_uniq<Vector>(type, MaxValue<idx_t>(count, 1));
	VectorOperations::DefaultCast(*result, *cast, count);
	return cast;
}

unique_ptr<Vector> DecideTreeColumns::HoleValues(idx_t node, idx_t hole) {
	auto &read = tree.nodes[node]->holes[hole];
	auto &rows = descent.Containers()[node].rows;
	if (read.column != DConstants::INVALID_INDEX) {
		// A frame's column, at the entry's row: the anchor it reads the frame at.
		auto &column = *frame_values.values[read.column];
		return DecideGather(column, frame_rows, vector<idx_t>(rows.begin(), rows.end()), read.type);
	}
	auto &values = Folds(read.node);
	// The slot each row lies in: a G over data has an entry per row it reaches (DecideDescent::Visit).
	auto &slots = row_slots[read.node];
	if (slots.empty()) {
		auto &reducer = descent.Containers()[read.node];
		for (idx_t e = 0; e < reducer.Count(); e++) {
			slots.resize(MaxValue<idx_t>(slots.size(), reducer.rows[e] + 1), DConstants::INVALID_INDEX);
			slots[reducer.rows[e]] = reducer.Slot(e);
		}
	}
	Vector folds(tree.nodes[read.node]->Cast<DecideReducerNode>().function->return_type,
	             MaxValue<idx_t>(values.size(), 1));
	for (idx_t slot = 0; slot < values.size(); slot++) {
		folds.SetValue(slot, values[slot]);
	}
	vector<idx_t> read_slots;
	read_slots.reserve(rows.size());
	for (auto row : rows) {
		read_slots.push_back(row < slots.size() ? slots[row] : DConstants::INVALID_INDEX);
	}
	return DecideGather(folds, values.size(), read_slots, read.type);
}

const vector<Value> &DecideTreeColumns::Folds(idx_t reducer) {
	auto known = folds.find(reducer);
	if (known != folds.end()) {
		return known->second;
	}
	auto &node = tree.nodes[reducer]->Cast<DecideReducerNode>();
	const auto operand = node.operand;
	auto &arguments = tree.nodes[operand]->compiled.arguments;
	auto &entries = descent.Containers()[operand];
	// Its members are the operand's entries its member tests keep, each variant of a class once, at its first row,
	// where the reducer reads a class per K (language/guide.md §5.3, §7.1).
	const auto &tested = Tested(operand);
	for (auto argument : arguments) {
		Report(operand, argument, tested);
	}
	const auto members = FirstOfEachInstance(operand, tested);
	vector<idx_t> read;
	vector<idx_t> slots;
	for (idx_t e = 0; e < entries.Count(); e++) {
		if (members[e]) {
			read.push_back(e);
			slots.push_back(entries.parent_slots[e]);
		}
	}
	auto &computed = Computed(operand);
	vector<const Vector *> values;
	for (auto argument : arguments) {
		values.push_back(computed.programs[argument].values.get());
	}
	const auto slot_count = descent.Containers()[reducer].groups.size();
	auto &result = folds[reducer];
	if (reducer == injection.node) {
		// A test's stand-in for the fold, which every reader of the G then reads (DecideTreeInjection).
		result.assign(slot_count, Value(injection.data).DefaultCastAs(node.function->return_type));
		return result;
	}
	DecideAggregateStates states(context, *node.function, slot_count);
	states.Update(values, read, slots);
	result = states.Finalize();
	return result;
}

void DecideTreeColumns::CheckData() {
	for (auto root : tree.roots) {
		auto &node = *tree.nodes[root];
		if (!DecideIsValueCheck(node)) {
			continue;
		}
		const auto check = node.compiled.check;
		// The check raises where it finds a value the model cannot read, as its own call
		// (`__internal_decide_invalid_value`) does when evaluated. It stands for a side plan's, which DuckDB evaluates
		// a chunk at a time: a window that raises raises what its whole evaluation meets first, as the side plan's
		// chunk would.
		auto &column = Computed(root).programs[check];
		if (!column.window_errors.empty()) {
			column.window_errors.begin()->second.Throw();
		}
	}
}

vector<bool> DecideTreeColumns::FirstOfEachInstance(idx_t node, const vector<bool> &reads) const {
	auto &container = descent.Containers()[node];
	vector<bool> first(reads.size(), false);
	vector<bool> seen(container.Instances(), false);
	for (idx_t e = 0; e < reads.size(); e++) {
		// A row in no class of the key has no value to read (DecideNode::holds_rows_in_no_class).
		const auto instance = container.Instance(e);
		if (reads[e] && container.groups.InAClass(container.Slot(e)) && !seen[instance]) {
			seen[instance] = true;
			first[e] = true;
		}
	}
	return first;
}

DecideFrameMap DecideTreeColumns::FrameMap(const DecideFrameSpec &frame, idx_t node, idx_t num_rows,
                                           vector<FramePeer> &peers) const {
	auto &container = descent.Containers()[node];
	DecideFrameMap map;
	map.range = frame.range;
	map.fallback = frame.fallback != nullptr;
	map.anchors.assign(num_rows, false);
	map.timeline.assign(num_rows, DConstants::INVALID_INDEX);
	map.peer.assign(num_rows, DConstants::INVALID_INDEX);
	for (auto row : container.rows) {
		map.anchors[row] = true;
	}
	// Its timelines by value, each position a peer whose rows ascend (DecideDescent::Finish); a row whose `within`
	// column is NULL lies in none (D75).
	vector<idx_t> first_peer;
	auto &timelines = container.timelines;
	for (idx_t timeline = 0; timeline < timelines.size(); timeline++) {
		const auto slot = timelines.slots[timeline];
		if (!container.groups.InAClass(slot)) {
			continue;
		}
		const auto t = first_peer.size();
		first_peer.push_back(map.peer_begin.size());
		for (auto position = timelines.first_position[timeline]; position < timelines.first_position[timeline + 1];
		     position++) {
			map.peer_begin.push_back(map.peer_rows.size());
			peers.push_back({slot, position});
			for (auto e = timelines.first_entry[position]; e < timelines.first_entry[position + 1]; e++) {
				const auto row = container.rows[timelines.entries[e]];
				map.peer_rows.push_back(row);
				map.peer[row] = map.peer_begin.size() - 1;
				map.timeline[row] = t;
			}
		}
	}
	first_peer.push_back(map.peer_begin.size());
	map.peer_begin.push_back(map.peer_rows.size());
	SelectDecideFramePeers(context, frame, first_peer, map);
	return map;
}

void DecideTreeColumns::CheckPartition(const DecideFrameSpec &frame, idx_t node) {
	if (frame.partition_subject.empty()) {
		return;
	}
	auto &container = descent.Containers()[node];
	auto &layout = container.layout;
	vector<idx_t> order(container.Count());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(),
	          [&](idx_t left, idx_t right) { return container.rows[left] < container.rows[right]; });
	auto partition = [&](idx_t e) {
		return container.values.GetValues(e, layout.partition, layout.partition_present);
	};
	// Each class's first anchor, by the class's values; an anchor in no timeline lies in none, so it spans none.
	std::map<vector<Value>, idx_t, bool (*)(const vector<Value> &, const vector<Value> &)> first(DecideDescentKeyLess);
	for (auto e : order) {
		if (!container.values.GetValue(layout.partition_present, e).GetValue<bool>() ||
		    !container.groups.InAClass(container.Slot(e))) {
			continue;
		}
		auto known = first.emplace(partition(e), e);
		const auto earlier = known.first->second;
		if (known.second || container.Slot(earlier) == container.Slot(e)) {
			continue;
		}
		vector<string> names;
		for (auto &element : DecideKeyElements(tree.keys, frame.partition_key)) {
			names.push_back(element.name);
		}
		auto timeline = [&](idx_t anchor) {
			return descent.Label(tree.nodes[node]->key, container.groups.Key(container.Slot(anchor)));
		};
		throw InvalidInputException(DecideFramePartitionMessage(frame, names,
		                                                        descent.Label(frame.partition_key, partition(earlier)),
		                                                        timeline(earlier), timeline(e)),
		                            Exception::InitializeExtraInfo(frame.location));
	}
}

void DecideTreeColumns::ReadFrames(const vector<DecideFrameSpec> &frames, idx_t num_rows,
                                   vector<DecideFrameMap> &maps) {
	frame_maps = &maps;
	absence.ReadFrames(maps);
	frame_rows = num_rows;
	// Each frame's timelines, frame after frame, where its rows are routed: its range's limit as its windows are
	// listed, after every read of its rows, then its partition (D75).
	frame_peers.resize(frames.size());
	for (idx_t f = 0; f < frames.size(); f++) {
		const auto node = tree.frame_nodes[f];
		maps.push_back(FrameMap(frames[f], node, num_rows, frame_peers[f]));
		CheckPartition(frames[f], node);
	}
	// What each frame reads, each expression a program on the frame's O.
	const auto programs = FrameExpressions();
	const DecideFrameRead read = [&](const Expression &expression, const vector<bool> &rows) {
		auto found = programs.find(&expression);
		if (found == programs.end()) {
			throw InternalException("DECIDE tree: a frame reads %s, which no program computes", expression.ToString());
		}
		return FrameRows(found->second.first, found->second.second, rows);
	};
	// A keyed decision has an instance at a row only where the row is in a class of its key. A range's payload reads a
	// declared decision alone, since a reducer inside a frame is refused, and the descent records a declared
	// decision's class on every row.
	auto absent = [&](idx_t variable, idx_t row) {
		if (variable == DConstants::INVALID_INDEX || !tree.variable_scopes[variable].IsKeyed()) {
			return false;
		}
		return descent.DecisionRows()[tree.variable_scopes[variable].keyed_scope_idx][row] == DConstants::INVALID_INDEX;
	};
	SkipDecideFrameNullReads(tree, maps, num_rows, read, absent);
	frame_values = ComputeDecideFrameColumns(context, frames, maps, tree.frame_reads, num_rows, read);
	CheckPositions(frames);
	DecideCountFrames(maps);
}

std::unordered_map<const Expression *, pair<idx_t, idx_t>> DecideTreeColumns::FrameExpressions() const {
	std::unordered_map<const Expression *, pair<idx_t, idx_t>> programs;
	for (idx_t j = 0; j < tree.frame_columns.size(); j++) {
		auto &column = tree.frame_reads.columns[j];
		auto &computed = tree.frame_columns[j];
		if (computed.value != DConstants::INVALID_INDEX) {
			programs[column.value.get()] = {computed.node, computed.value};
		}
		if (computed.fallback != DConstants::INVALID_INDEX) {
			programs[column.fallback.get()] = {computed.node, computed.fallback};
		}
		for (idx_t a = 0; a < computed.arguments.size(); a++) {
			programs[column.value->Cast<BoundAggregateExpression>().children[a].get()] = {computed.node,
			                                                                              computed.arguments[a]};
		}
	}
	// A range over decisions' payload: its helper's member terms, on the frame O that owns the value link.
	for (idx_t frame = 0; frame < tree.frame_nodes.size(); frame++) {
		const auto node_index = tree.frame_nodes[frame];
		const auto &node = *tree.nodes[node_index];
		if (!node.Linked()) {
			continue;
		}
		if (node.kind != DecideNodeKind::FRAME || node.Cast<DecideFrameNode>().frame != frame || !node.decision_value) {
			throw InternalException("DECIDE tree: frame node %llu has an invalid helper value", node_index);
		}
		auto &body = node.compiled;
		for (idx_t t = 0; t < body.members.size(); t++) {
			programs[body.members[t].coefficient.get()] = {node_index, body.linear[t].program};
		}
	}
	return programs;
}

DecideFrameValues DecideTreeColumns::FrameRows(idx_t node, idx_t program, const vector<bool> &rows) {
	auto &column = FramePrograms(node).programs[program];
	const auto num_rows = rows.size();
	auto &entries = descent.Containers()[node].rows;
	// Each row's anchor entry where `rows` marks it, gathered: NULL at every other row, and at the one past the end.
	vector<idx_t> read(num_rows + 1, DConstants::INVALID_INDEX);
	for (idx_t e = 0; e < entries.size(); e++) {
		const auto row = entries[e];
		if (rows[row]) {
			read[row] = e;
		}
	}
	DecideFrameValues result(column.values->GetType(), num_rows + 1);
	result.values.Reference(*DecideGather(*column.values, entries.size(), read, column.values->GetType()));
	for (auto &error : column.errors) {
		const auto row = entries[error.first];
		if (rows[row]) {
			result.errors.emplace(row, error.second);
		}
	}
	return result;
}

void DecideTreeColumns::CheckPositions(const vector<DecideFrameSpec> &frames) {
	for (idx_t f = 0; f < frames.size(); f++) {
		const auto node = tree.frame_nodes[f];
		const auto read = DecideReadPeers((*frame_maps)[f]);
		auto &peers = frame_peers[f];
		auto &container = descent.Containers()[node];
		auto &timelines = container.timelines;
		for (idx_t peer = 0; peer < peers.size(); peer++) {
			const auto position = peers[peer].position;
			if (!timelines.conflict[position] || !read[peer]) {
				continue;
			}
			// Named by its timeline's values and its order key's least spelling (D69).
			auto named = descent.Label(tree.nodes[node]->key, container.groups.Key(peers[peer].slot));
			named.push_back(container.Order(timelines.named_by[position]));
			const auto entries = timelines.first_entry[position + 1] - timelines.first_entry[position];
			throw InvalidInputException(DecideAmbiguousPositionMessage(frames[f], entries, named),
			                            Exception::InitializeExtraInfo(frames[f].location));
		}
	}
}

Value DecideTreeColumns::FrameValue(idx_t column, idx_t row) const {
	return frame_values.values[column]->GetValue(row);
}

Value DecideTreeColumns::PositionName(idx_t frame, idx_t row) const {
	auto &container = descent.Containers()[tree.frame_nodes[frame]];
	const auto position = frame_peers[frame][(*frame_maps)[frame].peer[row]].position;
	return container.Order(container.timelines.named_by[position]);
}

DecideTreeInjection ReadDecideTreeInjection(const DecideTree &tree, const DecideDebug &debug) {
	auto &injection = debug.inject;
	if (injection.node == DConstants::INVALID_INDEX) {
		return injection;
	}
	if (injection.node >= tree.nodes.size()) {
		throw InvalidInputException("decide_debug: inject node is outside this tree");
	}
	// Only a G over data computes a value its readers read, its fold.
	auto &node = *tree.nodes[injection.node];
	if (!DecideDataFold(node)) {
		throw InvalidInputException("decide_debug: inject node %llu computes no data value", injection.node);
	}
	return injection;
}

string DecideTreeColumns::Dump() const {
	std::ostringstream out;
	for (idx_t n = 0; n < tree.nodes.size(); n++) {
		if (!nodes[n] || !nodes[n]->complete || nodes[n]->programs.empty()) {
			continue;
		}
		auto &columns = *nodes[n];
		out << "node " << n << " " << tree.nodes[n]->text << "\n";
		auto &entries = descent.Containers()[n];
		for (idx_t e = 0; e < entries.Count(); e++) {
			out << "  row " << entries.rows[e] << " ->";
			for (idx_t p = 0; p < columns.programs.size(); p++) {
				auto &program = columns.programs[p];
				out << (p ? ", " : " ") << (program.errors.count(e) ? "error" : program.values->GetValue(e).ToString());
			}
			out << (columns.tested[e] ? "" : " untested") << " instance " << entries.Instance(e) << "\n";
		}
	}
	return out.str();
}

} // namespace duckdb
