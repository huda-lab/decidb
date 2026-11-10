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
                                             idx_t slots, string text_p, bool rejects_nan_p)
    : aggregate(aggregate_p), allocator(BufferAllocator::Get(context)), input(aggregate.bind_info.get(), allocator),
      slot_count(slots), state_size(AlignValue(aggregate.function.state_size(aggregate.function))),
      states(allocator.AllocateAligned(MaxValue<idx_t>(slots, 1) * state_size)), text(std::move(text_p)),
      rejects_nan(rejects_nan_p) {
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

void DecideAggregateStates::Update(const vector<vector<Value>> &arguments, const vector<idx_t> &slots,
                                   const vector<Value> *read) {
	// A NaN has no place in an order or a sum: the reducer refuses it, wherever it lies among the members.
	if (!read && !arguments.empty()) {
		read = &arguments[0];
	}
	for (idx_t member = 0; rejects_nan && read && member < slots.size(); member++) {
		auto &value = (*read)[member];
		const auto type = value.type().InternalType();
		if (!value.IsNull() && (type == PhysicalType::DOUBLE || type == PhysicalType::FLOAT) &&
		    std::isnan(value.GetValue<double>())) {
			throw InvalidInputException(DecideFrameNanMessage(text));
		}
	}
	for (idx_t offset = 0; offset < slots.size(); offset += STANDARD_VECTOR_SIZE) {
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, slots.size() - offset);
		vector<Vector> sources;
		for (idx_t a = 0; a < arguments.size(); a++) {
			sources.emplace_back(aggregate.children[a]->return_type);
			for (idx_t i = 0; i < count; i++) {
				sources.back().SetValue(i, arguments[a][offset + i]);
			}
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
			values.push_back(result.GetValue(i));
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
                                     const DecideSourceNames &names, std::function<string(idx_t)> written_clause,
                                     DecideErrorLedger &errors)
    : tree(tree), descent(descent), context(context), names(names), written_clause(std::move(written_clause)),
      errors(errors), nodes(tree.nodes.size()), injection(ReadDecideTreeInjection(tree)) {
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
	auto &entries = descent.Containers()[index].entries;
	const idx_t count = entries.size();
	// The node's layout (DecideProgram): its carried columns, then its program columns, as its entries hold them, then
	// its holes, the data reducers and frame columns they read; with no frame read, a frame column's hole is NULL.
	vector<LogicalType> layout;
	for (auto *carried : {&node.columns, &node.program_columns}) {
		for (auto &column : *carried) {
			layout.push_back(column->return_type);
		}
	}
	vector<vector<Value>> holes;
	for (idx_t h = 0; h < node.holes.size(); h++) {
		layout.push_back(node.holes[h].type);
		holes.push_back(frames || node.holes[h].column == DConstants::INVALID_INDEX
		                    ? HoleValues(index, h)
		                    : vector<Value>(count, Value(node.holes[h].type)));
	}
	columns.programs.resize(node.programs.size());
	vector<idx_t> over_layout, computed;
	for (idx_t p = 0; p < node.programs.size(); p++) {
		auto &column = columns.programs[p];
		if (column.values || (!frames && ReadsFrame(index, p))) {
			continue;
		}
		computed.push_back(p);
		// Folded where it is executed, as flat folds what it evaluates (TransformToChunkExpression).
		column.folded = DecideFoldConstants(node.programs[p].expression->Copy(), context);
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
		for (idx_t start = 0; start < count; start += STANDARD_VECTOR_SIZE) {
			const idx_t size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - start);
			chunk.Reset();
			for (idx_t i = 0; i < size; i++) {
				auto &entry = entries[start + i];
				for (idx_t j = 0; j < entry.columns.size(); j++) {
					chunk.data[j].SetValue(i, entry.columns[j]);
				}
				for (idx_t j = 0; j < entry.program_columns.size(); j++) {
					chunk.data[entry.columns.size() + j].SetValue(i, entry.program_columns[j]);
				}
				for (idx_t h = 0; h < holes.size(); h++) {
					chunk.data[layout.size() - holes.size() + h].SetValue(i, holes[h][start + i]);
				}
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
	auto &entries = descent.Containers()[index].entries;
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
		for (idx_t e = 0; e < entries.size() && !carried.empty(); e++) {
			auto error = carried.find(entries[e].row);
			if (error != carried.end()) {
				column.errors[e] = error->second;
			}
		}
	});
}

void DecideTreeColumns::ReadTests(idx_t index, NodeColumns &columns) {
	const idx_t count = descent.Containers()[index].entries.size();
	// An entry whose member test is false is no member: flat's reducer reads no term on its row. A test that raises
	// where an entry was reached raises, as flat's raises reading the reducer's filter. A test is `X IS NOT NULL`, so
	// it is NULL only where it raised: that entry is no member either, so its undefined value is never read, and its
	// error ranks before every read of the node's other programs.
	columns.tested.assign(count, true);
	for (auto p : tree.nodes[index]->tests) {
		auto &column = columns.programs[p];
		for (auto &error : column.errors) {
			errors.Add(Rank(index, p, error.first, false), error.second);
		}
		UnifiedVectorFormat format;
		column.values->ToUnifiedFormat(count, format);
		auto truth = UnifiedVectorFormat::GetData<bool>(format);
		for (idx_t e = 0; e < count; e++) {
			const auto i = format.sel->get_index(e);
			columns.tested[e] = columns.tested[e] && format.validity.RowIsValid(i) && truth[i];
		}
	}
}

//! Flat's order inside its owner's evaluation (PhysicalDecide::EvaluateConstraints, EvaluateObjective), under the
//! owner's rank: a block runs over every window of the input's rows before the next, a window executes every
//! expression of its block before it checks any value, and then the program's group and its order in the block. The
//! descent's reads of the owner's rows (a `when`, a key) number in order of reading from 0, below every block, so a
//! block's bits are one above its stage (DecideProgram::stage). A window's number stays below 2^54 for any row, under
//! the block's bits. A member test ranks under its own node, whose terms' owner reaches it.
DecideErrorRank DecideTreeColumns::Rank(idx_t node, idx_t p, idx_t entry, bool value) const {
	auto &program = tree.nodes[node]->programs[p];
	auto rank = tree.nodes[program.owner == DConstants::INVALID_INDEX ? node : program.owner]->clause_rank;
	const auto row = descent.Containers()[node].entries[entry].row;
	rank.expression = DecideBlockExpression(program.block, row, value);
	rank.group = program.group;
	rank.position = program.position;
	rank.part = program.part;
	rank.row = row;
	return rank;
}

void DecideTreeColumns::Report(idx_t node, idx_t program, const vector<bool> &reads) {
	for (auto &error : Computed(node).programs[program].errors) {
		if (reads[error.first]) {
			errors.Add(Rank(node, program, error.first, false), error.second);
		}
	}
}

const vector<bool> &DecideTreeColumns::Tested(idx_t node) {
	return Computed(node).tested;
}

vector<bool> DecideTreeColumns::ReadEntries(idx_t node, const DecideTerm &term) {
	auto reads = Tested(node);
	auto &entries = descent.Containers()[node].entries;
	for (idx_t e = 0; e < reads.size(); e++) {
		reads[e] = reads[e] && !TermAbsentAt(term, entries[e].row);
	}
	return reads;
}

vector<double> DecideTreeColumns::Numbers(idx_t node, idx_t p, const vector<bool> &reads, vector<bool> *nulls,
                                          const DecideTermSite *site, const vector<unique_ptr<Expression>> *inputs) {
	auto &program = tree.nodes[node]->programs[p];
	Report(node, p, reads);
	auto &column = *Computed(node).programs[p].values;
	const idx_t count = descent.Containers()[node].entries.size();
	D_ASSERT(reads.size() == count && (!nulls || nulls->size() == count));
	// The entry a message reads an input on: the window's first, then the one entry a retry reads.
	idx_t base = 0;
	static const vector<unique_ptr<Expression>> no_inputs;
	DecideValueInputs computed_from {
	    inputs ? *inputs : no_inputs, [&](const Expression &input, idx_t i) {
		    for (idx_t k = 0; k < inputs->size(); k++) {
			    if ((*inputs)[k].get() != &input) {
				    continue;
			    }
			    // The input is computed wherever its coefficient is.
			    return Computed(site->node).programs[site->inputs[k]].values->GetValue(base + i);
		    }
		    throw InternalException("DECIDE tree: a coefficient's input has no program");
	    }};
	// A coefficient's message names its clause where DeciDB's own arithmetic left the DOUBLE range, a helper's member
	// the helper's reducer as written, which every clause reading it shares; a bound's never.
	auto &owner = *tree.nodes[program.owner];
	const ValueSource source {
	    program.role == DecideProgramRole::COEFFICIENT ? DecideValueRole::COEFFICIENT : DecideValueRole::BOUND,
	    *program.written, names,
	    owner.link == DConstants::INVALID_INDEX ? written_clause(owner.source_clause_id) : owner.text,
	    inputs ? &computed_from : nullptr};
	vector<double> out;
	out.reserve(count);
	for (idx_t start = 0; start < count; start += STANDARD_VECTOR_SIZE) {
		const idx_t size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - start);
		Vector window(column.GetType(), size);
		VectorOperations::Copy(column, window, start + size, start, 0);
		try {
			base = start;
			ExtractDoubleColumn(window, size, program.sign, out, source, nulls, &reads, start);
		} catch (std::exception &) {
			// The entry that fails, which flat's window raises first: each entry of the window alone. The ledger
			// raises at once what stops the query wherever it is raised.
			for (idx_t i = 0; i < size; i++) {
				Vector one(window.GetType(), 1);
				VectorOperations::Copy(window, one, i + 1, i, 0);
				try {
					base = start + i;
					ExtractDoubleColumn(one, 1, program.sign, out, source, nulls, &reads, start + i);
				} catch (std::exception &entry_ex) {
					errors.Add(Rank(node, p, start + i, true), ErrorData(entry_ex));
					// Entries are in row order, so no later window's value ranks before it.
					out.resize(count, 0.0);
					return out;
				}
			}
			throw InternalException("DECIDE tree: %s fails on its window and on no entry of it",
			                        program.written->ToString());
		}
	}
	return out;
}

vector<bool> DecideTreeColumns::NullEntries(idx_t node, idx_t program, const vector<bool> &reads,
                                            DecideErrorRank rank) {
	auto &computed = Computed(node).programs[program];
	auto &entries = descent.Containers()[node].entries;
	// Flat executes a definition's reads together, a window at a time: one block, whose window ranks first.
	for (auto &error : computed.errors) {
		if (!reads[error.first]) {
			continue;
		}
		rank.row = entries[error.first].row;
		rank.expression = DecideBlockExpression(DecideBlock::TERMS, rank.row, false);
		errors.Add(rank, error.second);
	}
	auto &column = *computed.values;
	const auto count = entries.size();
	UnifiedVectorFormat format;
	column.ToUnifiedFormat(count, format);
	vector<bool> nulls(count, false);
	for (idx_t e = 0; e < count; e++) {
		nulls[e] = reads[e] && !format.validity.RowIsValid(format.sel->get_index(e));
	}
	return nulls;
}

Value DecideTreeColumns::ValueAt(idx_t node, idx_t program, idx_t entry) {
	return Computed(node).programs[program].values->GetValue(entry);
}

bool DecideTreeColumns::AbsentAt(idx_t variable, idx_t row) const {
	// A term of no decision, a constant term of the objective, reads none.
	if (variable == DConstants::INVALID_INDEX) {
		return false;
	}
	if (absent_reads && !(*absent_reads)[variable].empty() && (*absent_reads)[variable][row]) {
		return true;
	}
	// A product's auxiliary has none where a factor has none (LogicalDecide::BilinearLink), whose absence is read
	// first, as what defines it.
	for (auto &product : tree.bilinear_links) {
		if (product.aux_idx == variable &&
		    (AbsentAt(product.bool_var_idx, row) || AbsentAt(product.other_var_idx, row))) {
			return true;
		}
	}
	auto &scope = tree.variable_scopes[variable];
	return scope.IsKeyed() && (*mappings)[scope.keyed_scope_idx].IsAbsent(row);
}

bool DecideTreeColumns::TermAbsentAt(const DecideTerm &term, idx_t row) const {
	if (term.variable_index == DConstants::INVALID_INDEX || term.frame_read == DConstants::INVALID_INDEX) {
		return AbsentAt(term.variable_index, row);
	}
	return DecideFrameReadsAbsent((*frame_maps)[term.frame_read], row,
	                              [&](idx_t at) { return AbsentAt(term.variable_index, at); });
}

void DecideTreeColumns::ReadAbsence(const vector<KeyedMapping> &classes, const vector<vector<bool>> &reads) {
	mappings = &classes;
	absent_reads = &reads;
}

vector<Value> DecideTreeColumns::HoleValues(idx_t node, idx_t hole) {
	auto &read = tree.nodes[node]->holes[hole];
	if (read.column != DConstants::INVALID_INDEX) {
		// A frame's column, at the entry's row: the anchor it reads the frame at.
		auto &column = *frame_values.values[read.column];
		vector<Value> result;
		for (auto &entry : descent.Containers()[node].entries) {
			result.push_back(column.GetValue(entry.row).DefaultCastAs(read.type));
		}
		return result;
	}
	auto &values = Folds(read.node);
	// The slot each row lies in: a G over data has an entry per row it reaches (DecideDescent::Visit).
	auto &slots = row_slots[read.node];
	if (slots.empty()) {
		for (auto &entry : descent.Containers()[read.node].entries) {
			slots.resize(MaxValue<idx_t>(slots.size(), entry.row + 1), DConstants::INVALID_INDEX);
			slots[entry.row] = entry.slot;
		}
	}
	vector<Value> result;
	for (auto &entry : descent.Containers()[node].entries) {
		const auto slot = entry.row < slots.size() ? slots[entry.row] : DConstants::INVALID_INDEX;
		result.push_back(slot == DConstants::INVALID_INDEX ? Value(read.type) : values[slot].DefaultCastAs(read.type));
	}
	return result;
}

const vector<Value> &DecideTreeColumns::Folds(idx_t reducer) {
	auto known = folds.find(reducer);
	if (known != folds.end()) {
		return known->second;
	}
	auto &node = *tree.nodes[reducer];
	const auto operand = DecideTreeOperand(tree, reducer);
	auto &arguments = tree.nodes[operand]->compiled.arguments;
	auto &entries = descent.Containers()[operand].entries;
	// Its members are the operand's entries its member tests keep, each variant of a class once, at its first row,
	// where the reducer reads a class per K (language/guide.md §5.3, §7.1).
	const auto &tested = Tested(operand);
	for (auto argument : arguments) {
		Report(operand, argument, tested);
	}
	const auto members = FirstOfEachInstance(operand, tested);
	const auto norm_body = tree.nodes[operand]->compiled.norm_body;
	vector<vector<Value>> values(arguments.size());
	vector<Value> body;
	vector<idx_t> slots;
	for (idx_t e = 0; e < entries.size(); e++) {
		if (!members[e]) {
			continue;
		}
		slots.push_back(entries[e].parent_slot);
		for (idx_t a = 0; a < arguments.size(); a++) {
			values[a].push_back(Computed(operand).programs[arguments[a]].values->GetValue(e));
		}
		if (norm_body != DConstants::INVALID_INDEX) {
			body.push_back(Computed(operand).programs[norm_body].values->GetValue(e));
		}
	}
	const auto slot_count = descent.Containers()[reducer].groups.size();
	auto &result = folds[reducer];
	if (reducer == injection.node) {
		// A test's stand-in for the fold, which every reader of the G then reads (DecideTreeInjection).
		result.assign(slot_count, Value(injection.data).DefaultCastAs(node.function->return_type));
		return result;
	}
	try {
		// A NaN among the values is the data check's to refuse, which reads every value a G over data folds here and
		// ranks first, as flat's does; a reducer over frames, which no check reads, refuses one itself, but a count,
		// which counts it (AppendDecideFrameAggregates).
		auto &over_frames = tree.frame_aggregate_nodes;
		const bool rejects_nan = std::find(over_frames.begin(), over_frames.end(), reducer) != over_frames.end() &&
		                         StringUtil::Lower(node.function->function.name) != "count";
		DecideAggregateStates states(context, *node.function, slot_count, node.text, rejects_nan);
		states.Update(values, slots, norm_body == DConstants::INVALID_INDEX ? nullptr : &body);
		result = states.Finalize();
	} catch (std::exception &ex) {
		errors.Add(node.value_rank, ErrorData(ex));
		result.assign(slot_count, Value(node.function->return_type));
	}
	return result;
}

void DecideTreeColumns::CheckData() {
	for (auto root : tree.roots) {
		auto &node = *tree.nodes[root];
		if (!node.value_check) {
			continue;
		}
		auto &entries = descent.Containers()[root].entries;
		const auto check = node.compiled.check;
		// The check stands for a side plan's, which DuckDB evaluates a chunk at a time: a window that raises raises
		// what its whole evaluation meets first, as the side plan's chunk would.
		auto &column = Computed(root).programs[check];
		for (auto &raised : column.window_errors) {
			errors.Add(Rank(root, check, raised.first, false), raised.second);
		}
		auto &messages = *column.values;
		for (idx_t e = 0; e < entries.size(); e++) {
			auto message = messages.GetValue(e);
			if (!message.IsNull()) {
				auto rank = node.value_rank;
				rank.row = entries[e].row;
				errors.Add(rank, ErrorData(InvalidInputException(message.ToString())));
			}
		}
	}
}

vector<bool> DecideTreeColumns::FirstOfEachInstance(idx_t node, const vector<bool> &reads) const {
	auto &container = descent.Containers()[node];
	vector<bool> first(reads.size(), false);
	vector<bool> seen(container.instance_entries.size(), false);
	for (idx_t e = 0; e < reads.size(); e++) {
		// A row in no class of the key has no value to read (DecideNode::holds_rows_in_no_class).
		const auto instance = container.entries[e].instance;
		if (reads[e] && container.groups[container.entries[e].slot].in_a_class && !seen[instance]) {
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
	for (auto &entry : container.entries) {
		map.anchors[entry.row] = true;
	}
	// Its timelines by value, each position a peer whose rows ascend (DecideDescent::Finish); a row whose `within`
	// column is NULL lies in none (D75).
	vector<idx_t> first_peer;
	for (auto &timeline : container.timelines) {
		if (!container.groups[timeline.slot].in_a_class) {
			continue;
		}
		const auto t = first_peer.size();
		first_peer.push_back(map.peer_begin.size());
		for (auto &position : timeline.positions) {
			map.peer_begin.push_back(map.peer_rows.size());
			peers.push_back({timeline.slot, &position});
			for (auto e : position.entries) {
				const auto row = container.entries[e].row;
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
	vector<idx_t> order(container.entries.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(),
	          [&](idx_t left, idx_t right) { return container.entries[left].row < container.entries[right].row; });
	// Each class's first anchor, by the class's values; an anchor in no timeline lies in none, so it spans none.
	std::map<vector<Value>, idx_t, bool (*)(const vector<Value> &, const vector<Value> &)> first(DecideDescentKeyLess);
	for (auto e : order) {
		auto &entry = container.entries[e];
		if (!entry.partition_present || !container.groups[entry.slot].in_a_class) {
			continue;
		}
		auto known = first.emplace(entry.partition, e);
		auto &earlier = container.entries[known.first->second];
		if (known.second || earlier.slot == entry.slot) {
			continue;
		}
		vector<string> names;
		for (auto &element : DecideKeyElements(tree.keys, frame.partition_key)) {
			names.push_back(element.name);
		}
		auto timeline = [&](const DecideDescentEntry &anchor) {
			return descent.Label(tree.nodes[node]->key, container.groups[anchor.slot].key);
		};
		auto rank = tree.nodes[node]->route_rank;
		rank.expression = DecideBlockExpression(DecideBlock::FILTERS, 0, false) + 1;
		rank.row = entry.row;
		errors.Add(rank, ErrorData(InvalidInputException(DecideFramePartitionMessage(
		                     frame, names, descent.Label(frame.partition_key, earlier.partition), timeline(earlier),
		                     timeline(entry)))));
		return;
	}
}

void DecideTreeColumns::ReadFrames(const vector<DecideFrameSpec> &frames, const DecidePreparedModel &prepared,
                                   const vector<LogicalDecide::ReducerValueLink> &links, idx_t num_rows,
                                   vector<DecideFrameMap> &maps) {
	frame_maps = &maps;
	// Each frame's timelines, frame after frame, where its rows are routed: its range's limit as its windows are
	// listed, after every read of its rows, then its partition (D75).
	frame_peers.resize(frames.size());
	for (idx_t f = 0; f < frames.size(); f++) {
		const auto node = tree.frame_nodes[f];
		try {
			maps.push_back(FrameMap(frames[f], node, num_rows, frame_peers[f]));
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (DecideErrorStopsTheQuery(error)) {
				throw;
			}
			auto rank = tree.nodes[node]->route_rank;
			rank.expression = DecideBlockExpression(DecideBlock::FILTERS, 0, false);
			errors.Add(rank, error);
			// The error ranks before any read of the frame's values: its anchors read nothing meanwhile.
			DecideFrameMap absent;
			absent.range = frames[f].range;
			absent.anchors.assign(num_rows, false);
			absent.timeline.assign(num_rows, DConstants::INVALID_INDEX);
			absent.peer = absent.selected_peer = absent.window = absent.timeline;
			absent.absent.assign(num_rows, true);
			absent.peer_begin.push_back(0);
			maps.push_back(std::move(absent));
			frame_peers[f].clear();
		}
		CheckPartition(frames[f], node);
	}
	// What each frame reads, through flat's blocks, each expression a program on the frame's O.
	const auto programs = FrameExpressions(prepared, links);
	const DecideFrameRead read = [&](const Expression &expression, const vector<bool> &rows) {
		auto found = programs.find(&expression);
		if (found == programs.end()) {
			throw InternalException("DECIDE tree: a frame reads %s, which no program computes", expression.ToString());
		}
		return FrameRows(found->second.first, found->second.second, rows);
	};
	SkipDecideFrameNullReads(prepared, links, maps, num_rows, read);
	frame_values = ComputeDecideFrameColumns(context, frames, maps, prepared, num_rows, read);
	CheckPositions(frames);
	DecideCountFrames(maps);
}

std::unordered_map<const Expression *, pair<idx_t, idx_t>>
DecideTreeColumns::FrameExpressions(const DecidePreparedModel &prepared,
                                    const vector<LogicalDecide::ReducerValueLink> &links) const {
	std::unordered_map<const Expression *, pair<idx_t, idx_t>> programs;
	for (idx_t j = 0; j < tree.frame_columns.size(); j++) {
		auto &column = prepared.frame_columns[j];
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
	// A range over decisions' payload: its helper's member terms, on the frame's O.
	for (auto &link : links) {
		if (link.frame == DConstants::INVALID_INDEX) {
			continue;
		}
		const auto node = tree.frame_nodes[link.frame];
		auto &sites = tree.nodes[node]->compiled.linear;
		for (idx_t t = 0; t < link.inner_terms.size(); t++) {
			programs[link.inner_terms[t].coefficient.get()] = {node, sites[t].program};
		}
	}
	return programs;
}

DecideFrameValues DecideTreeColumns::FrameRows(idx_t node, idx_t program, const vector<bool> &rows) {
	auto &column = FramePrograms(node).programs[program];
	const auto num_rows = rows.size();
	DecideFrameValues result(column.values->GetType(), num_rows + 1);
	FlatVector::Validity(result.values).SetAllInvalid(num_rows + 1);
	auto &entries = descent.Containers()[node].entries;
	for (idx_t e = 0; e < entries.size(); e++) {
		const auto row = entries[e].row;
		if (!rows[row]) {
			continue;
		}
		result.values.SetValue(row, column.values->GetValue(e));
		auto error = column.errors.find(e);
		if (error != column.errors.end()) {
			result.errors.emplace(row, error->second);
		}
	}
	return result;
}

void DecideTreeColumns::CheckPositions(const vector<DecideFrameSpec> &frames) {
	for (idx_t f = 0; f < frames.size(); f++) {
		const auto node = tree.frame_nodes[f];
		const auto read = DecideReadPeers((*frame_maps)[f]);
		auto &peers = frame_peers[f];
		for (idx_t peer = 0; peer < peers.size(); peer++) {
			auto &position = *peers[peer].position;
			if (!position.conflict || !read[peer]) {
				continue;
			}
			// Named by its timeline's values and its order key's least spelling (D69).
			auto named = descent.Label(tree.nodes[node]->key, descent.Containers()[node].groups[peers[peer].slot].key);
			named.push_back(position.order);
			auto rank = tree.nodes[node]->route_rank;
			rank.phase = DecideErrorPhase::FRAME_PEERS;
			rank.expression = peer;
			errors.Add(rank, ErrorData(InvalidInputException(
			                     DecideAmbiguousPositionMessage(frames[f], position.entries.size(), named))));
			break;
		}
	}
}

Value DecideTreeColumns::FrameValue(idx_t column, idx_t row) const {
	return frame_values.values[column]->GetValue(row);
}

Value DecideTreeColumns::PositionName(idx_t frame, idx_t row) const {
	return frame_peers[frame][(*frame_maps)[frame].peer[row]].position->order;
}

DecideTreeInjection ReadDecideTreeInjection(const DecideTree &tree) {
	DecideTreeInjection injection;
	const char *setting = std::getenv("DECIDB_TREE_INJECT");
	if (!setting) {
		return injection;
	}
	auto parts = StringUtil::Split(string(setting), ':');
	if (parts.size() != 3 || parts[1] != "data") {
		throw InvalidInputException("DECIDB_TREE_INJECT: expected node:data:value");
	}
	try {
		size_t consumed = 0;
		injection.node = std::stoull(parts[0], &consumed);
		if (consumed != parts[0].size() || injection.node >= tree.nodes.size()) {
			throw InvalidInputException("DECIDB_TREE_INJECT: node is outside this tree");
		}
	} catch (std::invalid_argument &) {
		throw InvalidInputException("DECIDB_TREE_INJECT: node must be an unsigned node index");
	} catch (std::out_of_range &) {
		throw InvalidInputException("DECIDB_TREE_INJECT: node is outside this tree");
	}
	// Only a G over data computes a value its readers read, its fold.
	auto &node = *tree.nodes[injection.node];
	if (node.kind != DecideNodeKind::REDUCER || node.decision_value || !node.function) {
		throw InvalidInputException("DECIDB_TREE_INJECT: node %llu computes no data value", injection.node);
	}
	injection.data = parts[2];
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
		auto &entries = descent.Containers()[n].entries;
		for (idx_t e = 0; e < entries.size(); e++) {
			out << "  row " << entries[e].row << " ->";
			for (idx_t p = 0; p < columns.programs.size(); p++) {
				auto &program = columns.programs[p];
				out << (p ? ", " : " ") << (program.errors.count(e) ? "error" : program.values->GetValue(e).ToString());
			}
			out << (columns.tested[e] ? "" : " untested") << " instance " << entries[e].instance << "\n";
		}
	}
	return out.str();
}

} // namespace duckdb
