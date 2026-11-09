//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_tree_columns.cpp
//
// Each node's programs over its entries (decide_tree_columns.hpp): partial evaluation, as DuckDB's executor runs a
// projection, with no expression interpreted per entry.
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_tree_columns.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//! A program's values on its node's entries, and the entries its evaluation raised on, with the error.
struct DecideTreeColumns::ProgramColumn {
	unique_ptr<Expression> folded;
	unique_ptr<Vector> values;
	std::map<idx_t, ErrorData> errors;
};

struct DecideTreeColumns::NodeColumns {
	vector<ProgramColumn> programs;
	vector<bool> tested;
};

DecideTreeColumns::DecideTreeColumns(const DecideTree &tree, const DecideDescent &descent, ClientContext &context,
                                     const DecideSourceNames &names, std::function<string(idx_t)> written_clause,
                                     DecideErrorLedger &errors)
    : tree(tree), descent(descent), context(context), names(names), written_clause(std::move(written_clause)),
      errors(errors), nodes(tree.nodes.size()) {
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
	// The node's layout (DecideProgram): its carried columns, then its program columns, as its entries hold them.
	vector<LogicalType> layout;
	for (auto *carried : {&node.columns, &node.program_columns}) {
		for (auto &column : *carried) {
			layout.push_back(column->return_type);
		}
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
			}
			for (idx_t a = 0; a < arguments.size(); a++) {
				chunk.data[layout.size() + a].Slice(*columns.programs[arguments[a]].values, start, start + size);
			}
			chunk.SetCardinality(size);
			for (idx_t e = 0; e < programs.size(); e++) {
				auto &column = columns.programs[programs[e]];
				DecideEvaluateWindow(context, executor, e, remake, *column.folded, *column.values, column.errors, chunk,
				                     start);
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
	auto rank = tree.nodes[program.owner == DConstants::INVALID_INDEX ? node : program.owner]->value_rank;
	const auto row = descent.Containers()[node].entries[entry].row;
	rank.expression = ((program.stage + 1) << 56) | ((row / STANDARD_VECTOR_SIZE) << 1) | idx_t(value);
	rank.group = program.group;
	rank.position = program.position;
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
	const auto absent = Absent(node, variable);
	for (idx_t e = 0; e < reads.size(); e++) {
		reads[e] = reads[e] && !absent[e];
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

vector<bool> DecideTreeColumns::Absent(idx_t node, idx_t variable) const {
	auto &entries = descent.Containers()[node].entries;
	vector<bool> absent(entries.size(), false);
	// A term of no decision, a constant term of the objective, reads none.
	if (variable == DConstants::INVALID_INDEX || !tree.variable_scopes[variable].IsKeyed()) {
		return absent;
	}
	auto &rows = descent.DecisionRows()[tree.variable_scopes[variable].keyed_scope_idx];
	for (idx_t e = 0; e < entries.size(); e++) {
		absent[e] = rows[entries[e].row] == DConstants::INVALID_INDEX;
	}
	return absent;
}

string DecideTreeColumns::Uncomputed(const DecideTree &tree) {
	for (auto &node : tree.nodes) {
		if (!node->compiled.uncompiled.empty()) {
			return node->compiled.uncompiled;
		}
		if (node->hidden) {
			return node->text;
		}
	}
	return string();
}

} // namespace duckdb
