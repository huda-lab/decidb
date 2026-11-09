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
#include "duckdb/storage/buffer_manager.hpp"

#include <cmath>

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

void DecideAggregateStates::Update(const vector<vector<Value>> &arguments, const vector<idx_t> &slots) {
	// A NaN has no place in an order or a sum: the reducer refuses it, wherever it lies among the members.
	for (idx_t member = 0; rejects_nan && !arguments.empty() && member < slots.size(); member++) {
		auto &value = arguments[0][member];
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
		ComputeNode(node, *nodes[node]);
		ReadTests(node, *nodes[node]);
	}
	return *nodes[node];
}

void DecideTreeColumns::ComputeNode(idx_t index, NodeColumns &columns) {
	auto &node = *tree.nodes[index];
	auto &entries = descent.Containers()[index].entries;
	const idx_t count = entries.size();
	// The node's layout (DecideProgram): its carried columns, then its program columns, as its entries hold them, then
	// its holes, the data reducers they read.
	vector<LogicalType> layout;
	for (auto *carried : {&node.columns, &node.program_columns}) {
		for (auto &column : *carried) {
			layout.push_back(column->return_type);
		}
	}
	vector<vector<Value>> holes;
	for (idx_t h = 0; h < node.holes.size(); h++) {
		layout.push_back(node.holes[h].type);
		holes.push_back(HoleValues(index, h));
	}
	columns.programs.resize(node.programs.size());
	vector<idx_t> over_layout;
	for (idx_t p = 0; p < node.programs.size(); p++) {
		auto &column = columns.programs[p];
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
	for (idx_t p = 0; p < node.programs.size(); p++) {
		if (!node.programs[p].arguments.empty()) {
			evaluate({p}, node.programs[p].arguments);
		}
	}
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

vector<bool> DecideTreeColumns::ReadEntries(idx_t node, idx_t variable) {
	auto reads = Tested(node);
	auto &entries = descent.Containers()[node].entries;
	for (idx_t e = 0; e < reads.size(); e++) {
		reads[e] = reads[e] && !AbsentAt(variable, entries[e].row);
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
	// A coefficient's message names its clause where DeciDB's own arithmetic left the DOUBLE range; a bound's never.
	const ValueSource source {program.role == DecideProgramRole::COEFFICIENT ? DecideValueRole::COEFFICIENT
	                                                                         : DecideValueRole::BOUND,
	                          *program.written, names, written_clause(tree.nodes[program.owner]->source_clause_id),
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

vector<bool> DecideTreeColumns::NullEntries(idx_t node, idx_t program, DecideErrorRank rank) {
	auto &computed = Computed(node).programs[program];
	auto &entries = descent.Containers()[node].entries;
	// Flat executes a definition's reads together, a window at a time: one block, whose window ranks first.
	for (auto &error : computed.errors) {
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
		nulls[e] = !format.validity.RowIsValid(format.sel->get_index(e));
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
	auto &scope = tree.variable_scopes[variable];
	return scope.IsKeyed() && descent.DecisionRows()[scope.keyed_scope_idx][row] == DConstants::INVALID_INDEX;
}

void DecideTreeColumns::ReadAbsence(const vector<vector<bool>> &reads) {
	absent_reads = &reads;
}

vector<Value> DecideTreeColumns::HoleValues(idx_t node, idx_t hole) {
	auto &read = tree.nodes[node]->holes[hole];
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
	vector<vector<Value>> values(arguments.size());
	vector<idx_t> slots;
	for (idx_t e = 0; e < entries.size(); e++) {
		if (!members[e]) {
			continue;
		}
		slots.push_back(entries[e].parent_slot);
		for (idx_t a = 0; a < arguments.size(); a++) {
			values[a].push_back(Computed(operand).programs[arguments[a]].values->GetValue(e));
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
		// ranks first, as flat's does.
		DecideAggregateStates states(context, *node.function, slot_count, node.text, false);
		states.Update(values, slots);
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
	injection.data = parts[2];
	return injection;
}

string DecideTreeColumns::Uncomputed(const DecideTree &tree) {
	for (auto &node : tree.nodes) {
		if (!node->compiled.uncompiled.empty()) {
			return node->compiled.uncompiled;
		}
		// A hidden root other than a data check, a definition no clause reads, has no value here yet.
		if (node->hidden && !node->value_check) {
			return node->text;
		}
	}
	return string();
}

} // namespace duckdb
