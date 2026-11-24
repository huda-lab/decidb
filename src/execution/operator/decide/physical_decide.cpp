#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/execution/operator/decide/decide_global_sink_state.hpp"
#include "duckdb/execution/operator/decide/decide_model_emitter.hpp"
#include "duckdb/execution/operator/decide/decide_tree_columns.hpp"
#include "duckdb/execution/operator/decide/decide_frame_map.hpp"
#include "duckdb/execution/operator/decide/decide_descent.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/main/decide_number_warning.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/decide_profile.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/decidb/solver/ilp_solver.hpp"
#include "duckdb/decidb/solver/decide_solve.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/formulate_model.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic_terminals.hpp"
#include "duckdb/decidb/diagnostics/decide_router.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/decidb/solver/solver_config.hpp"
// The continuation prompt needs to know whether stdin is a terminal. It is a platform primitive; MSVC has no
// <unistd.h>. Same split as gurobi_loader.cpp.
#ifdef _WIN32
#include <cstdio> // _fileno
#include <io.h>   // _isatty
#else
#include <unistd.h> // isatty / STDIN_FILENO
#endif

namespace duckdb {

//===--------------------------------------------------------------------===//
// Constructor
//===--------------------------------------------------------------------===//

PhysicalDecide::PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality,
                               unique_ptr<PhysicalOperator> child)
    : PhysicalOperator(PhysicalOperatorType::DECIDE, std::move(types), estimated_cardinality) {
	children.push_back(std::move(child));
}

DecideSourceNames PhysicalDecide::Names() const {
	auto names = DecidePlanNames(display.source_text, tree->keyed_scopes, tree->keys, display.decide_index,
	                             display.aux_var_expressions, tree->frames);
	names.source_columns = tree->input_column_count;
	return names;
}

string PhysicalDecide::WrittenClause(idx_t source_clause_id) const {
	return source_clause_id < display.constraint_sources.size()
	           ? QuoteWrittenClause(display.constraint_sources, source_clause_id, display.source_text)
	           : DecideWrittenObjective(tree->written_sense, display.written_objective);
}

SolverBackend PhysicalDecide::PlannedSolverBackend() const {
	SolverBackend backend = SolverRegistry::Find(solver_backend_name);
	if (!backend.IsValid()) {
		// Physical planning only accepts a plan the DECIDE optimizer named a backend for, so
		// an unresolvable name means the plan and the registry disagree — an internal
		// defect, never a user error.
		throw InternalException("DECIDE reached the solver with no backend named on the plan");
	}
	return backend;
}

//===--------------------------------------------------------------------===//
// EXPLAIN Support
//===--------------------------------------------------------------------===//

string PhysicalDecide::GetName() const {
	return "DECIDE";
}

InsertionOrderPreservingMap<string> PhysicalDecide::ParamsToString() const {
	// The run-time accounts follow the plan's, so EXPLAIN ANALYZE prints them under their clause. A helper the run
	// wrote where it is read holds no column, so its line replaces the plan's, which says it becomes one.
	vector<LogicalDecide::RewriteAccount> accounts;
	for (auto &account : display.rewrite_accounts) {
		const bool replaced = std::any_of(
		    run_time_accounts.begin(), run_time_accounts.end(), [&](const LogicalDecide::RewriteAccount &run) {
			    return run.kind == LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_INLINED &&
			           run.source_clause_id == account.source_clause_id && run.helper == account.helper;
		    });
		if (!replaced) {
			accounts.push_back(account);
		}
	}
	accounts.insert(accounts.end(), run_time_accounts.begin(), run_time_accounts.end());
	DecidePlanParams plan {tree->decide_variables,
	                       tree->num_auxiliary_vars,
	                       tree->keyed_scopes,
	                       tree->keys,
	                       tree->frames,
	                       tree->written_sense,
	                       display.decide_index,
	                       display.source_text,
	                       display.aux_var_expressions,
	                       display.clauses,
	                       display.constraint_sources,
	                       display.objective,
	                       display.written_objective,
	                       display.canonical_objective,
	                       display.objective_canonical_differs};
	auto result = RenderDecideParams(plan, accounts, tree.get());
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

//===--------------------------------------------------------------------===//
// Multi-variable per-row constraint helpers
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//

//! The chunk to the descent, in pieces of `decide_debug`'s chunk size when one is set.
static void AppendDecideDescent(DecideDescent &descent, DataChunk &chunk, idx_t row_base, const DecideDebug &debug) {
	const idx_t chunk_size = debug.chunk == DConstants::INVALID_INDEX ? STANDARD_VECTOR_SIZE : debug.chunk;
	auto append = [&](DataChunk &piece, idx_t base) {
		descent.Append(piece, base);
		// The containers after each piece, the deck's animation at chunk size 1 (DecideDescent::Step).
		if (debug.steps) {
			fprintf(stderr, "DECIDB_STEP_BEGIN %llu\n%sDECIDB_STEP_END\n", (unsigned long long)(base + piece.size()),
			        descent.Step().c_str());
		}
	};
	if (chunk.size() <= chunk_size) {
		append(chunk, row_base);
		return;
	}
	for (idx_t first = 0; first < chunk.size(); first += chunk_size) {
		auto count = MinValue<idx_t>(chunk_size, chunk.size() - first);
		DataChunk slice;
		slice.InitializeEmpty(chunk.GetTypes());
		SelectionVector selected(count);
		for (idx_t row = 0; row < count; row++) {
			selected.set_index(row, first + row);
		}
		slice.Slice(chunk, selected, count);
		append(slice, row_base + first);
	}
}

//! One descent over the buffered rows (language/guide.md §6.1), a chunk of the scan at a time, each numbered by the
//! rows before it.
static unique_ptr<DecideDescent> RouteDecideRows(const DecideTree &tree, const DecideDebug &debug,
                                                 ClientContext &context, const ColumnDataCollection &data) {
	auto descent = make_uniq<DecideDescent>(tree, context);
	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	data.InitializeScanChunk(chunk);
	idx_t row_base = 0;
	while (data.Scan(scan, chunk)) {
		AppendDecideDescent(*descent, chunk, row_base, debug);
		row_base += chunk.size();
	}
	return descent;
}

//! A thread's rows, buffered as they arrive: the descent routes them once Finalize has every thread's, as a hash join
//! builds its table once the build side is complete.
class DecideLocalSinkState : public LocalSinkState {
public:
	DecideProfileTotals profile_sink {"execution.sink_append"};
	ColumnDataCollection data;
	ColumnDataAppendState append_state;

	explicit DecideLocalSinkState(ClientContext &context, const PhysicalDecide &op)
	    : data(context, op.children[0]->GetTypes()) {
		data.InitializeAppend(append_state);
	}
};

unique_ptr<GlobalSinkState> PhysicalDecide::GetGlobalSinkState(ClientContext &context) const {
	// The descent reads only the rows reaching each node (plan L2): checked once, before the first row.
	DecideDescent::RequireRowReads(*tree, children[0]->GetTypes().size());
	return make_uniq_base<GlobalSinkState, DecideGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalDecide::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq_base<LocalSinkState, DecideLocalSinkState>(context.client, *this);
}

SinkResultType PhysicalDecide::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<DecideLocalSinkState>();
	DecideProfileSample profile(lstate.profile_sink);
	lstate.data.Append(lstate.append_state, chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDecide::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	DecideProfileScope profile("execution.sink_combine");
	auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
	auto &lstate = input.local_state.Cast<DecideLocalSinkState>();
	// The thread's rows placed after those already placed, in the order Finalize routes them.
	lock_guard<mutex> guard(gstate.lock);
	gstate.data.Combine(lstate.data);
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
	DecideProfileScope profile("execution.finalize");

	auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
	idx_t num_rows = gstate.data.Count();
	run_time_accounts.clear();

	// Empty input → return empty result (mirrors standard SQL behavior)
	if (num_rows == 0) {
		return SinkFinalizeType::READY;
	}
	// The evaluation writes what the model reads of the data into the solver input, so the node containers go now
	// rather than with the global state.
	SolverInput solver_input;
	EvaluateTree(context, gstate, num_rows, solver_input);
	gstate.descent.reset();

	// BuildSolverInput: the plan's facts and the column box. Nothing below this line touches the data again.
	DecideProfileScope phase("execution.assemble_input");
	VarIndexer var_indexer;
	BuildSolverInput(num_rows, solver_input, var_indexer);

	// Under DIAGNOSE, keep the model input and column space as assembly left them —
	// before a single constant was derived. An infeasibility repair searches a WIDER box
	// than the solved model declares, and a Big-M or a McCormick envelope is sound only
	// for the box it came from, so the INFEASIBLE terminal formulates a second model from
	// this instead of re-deriving nothing and inheriting the narrow one's constants. It
	// also emits the scoped bounds the column box enforces, which the solved model has no
	// row for. The copy is paid for only when the user asked for a diagnosis.
	unique_ptr<ElasticFormulation> elastic;
	if (tree->diagnose) {
		elastic = make_uniq<ElasticFormulation>();
		elastic->input = solver_input;
		for (auto &folded : gstate.evaluated_singleton_row_constraints) {
			elastic->input.constraints.push_back(std::move(folded));
		}
		elastic->var_indexer = var_indexer;
	}

	// FormulateModel: derive the constants and emit the rows, against the box the model
	// declares. A pure function of what came before, which is what lets the elastic
	// (infeasibility-repair) path run it a second time against a widened box.
	phase.Next("execution.formulate");
	FormulateModel(solver_input, FormulationBox::OfSolvedModel(solver_input), var_indexer);
	ReportAnEmptyObjective(solver_input);

	phase.Next("execution.solve_and_route");
	return FinalizeSolveResult(context, gstate, solver_input, var_indexer, elastic.get());
}

void PhysicalDecide::EvaluateTree(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                  SolverInput &input) const {
	auto &descent = gstate.descent;
	// Top-down: one descent reads the buffer a chunk at a time, each numbered by the rows before it, and the buffer is
	// never scanned again to evaluate a row.
	DecideProfileScope phase("execution.tree_descent");
	gstate.descent = RouteDecideRows(*tree, debug, context, gstate.data);
	descent->Finish();
	DecideProfileScope::Counter("execution.descent_rows", static_cast<double>(gstate.data.Count()));
	if (debug.bodies) {
		fprintf(stderr, "DECIDB_BODIES_BEGIN\n%sDECIDB_BODIES_END\n", tree->BodiesToString().c_str());
	}
	if (debug.tree) {
		fprintf(stderr, "DECIDB_TREE_BEGIN\n%sDECIDB_TREE_END\n", tree->ToString().c_str());
	}
	if (debug.containers) {
		fprintf(stderr, "DECIDB_CONTAINERS_BEGIN\n%sDECIDB_CONTAINERS_END\n", descent->Dump().c_str());
	}
	// Bottom-up: each node's programs over its entries, written as stage 06 reads them.
	phase.Next("execution.tree_model");
	const auto names = Names();
	DecideTreeColumns columns(*tree, *descent, context, names, debug);
	// The frames' timelines and the columns they read, before any program reads one (language/guide.md §5.6).
	columns.ReadFrames(tree->frames, num_rows, gstate.frame_maps);
	// What the data checks and the data reducers no clause reads raise, before any clause is read.
	columns.CheckData();
	DecideModelEmitter emitter(*this, context, *tree, *descent, columns, gstate, input, names, num_rows);
	emitter.KeyedMappings();
	// The helpers first: a clause, the objective or another helper that reads one the plan writes in reads its
	// members.
	emitter.ValueLinks();
	emitter.Constraints();
	// A relation-qualified reducer reads each class's first entry, its first row, so the objective's columns are
	// qualified as they are read.
	emitter.Objective();
	emitter.AbsentInstances();
	// What the data left of each clause is reported once its clauses and its objective are read.
	ReportSkippedInstances(gstate);
	// The bottom-up values the evaluation computed, each node's programs on its entries.
	if (debug.values) {
		fprintf(stderr, "DECIDB_VALUES_BEGIN\n%sDECIDB_VALUES_END\n", columns.Dump().c_str());
	}
}

//===--------------------------------------------------------------------===//
// Finalize: EXPLAIN ANALYZE accounts
//===--------------------------------------------------------------------===//

//! One line for EXPLAIN ANALYZE: a scoped expression whose classes held more than one variant (D70, D71), so the
//! instances it generated -- or the values a reducer read -- outnumber its classes: `per P.depotID: 4 constraints
//! from 2 classes, one per distinct (P.lim, by (P.region)) within a class`.
void PhysicalDecide::RecordVariants(const DecideSignature &signature, const char *unit, idx_t variants,
                                    idx_t classes) const {
	vector<string> fields;
	for (auto &field : signature.fields) {
		fields.push_back(field.text);
	}
	RecordRunTimeAccount(signature.source_clause_id,
	                     StringUtil::Format("%s: %llu %s from %llu %s, one per distinct (%s) within a class",
	                                        signature.text, variants, unit, classes, classes == 1 ? "class" : "classes",
	                                        StringUtil::Join(fields, ", ")));
}

void PhysicalDecide::ReportSkippedInstances(const DecideGlobalSinkState &gstate) const {
	// A BETWEEN is two comparisons by now, each counted under its own id; the report is one per clause the user wrote,
	// with the most any of its comparisons holds, as the rows of one comparison are counted.
	map<idx_t, DecideGlobalSinkState::ClauseInstances> written;
	for (auto &half : gstate.clause_instances) {
		auto &counts = written[WrittenClauseId(display.constraint_sources, half.first)];
		counts.grouped = MaxValue(counts.grouped, half.second.grouped);
		counts.kept = MaxValue(counts.kept, half.second.kept);
	}
	for (auto &entry : written) {
		auto &counts = entry.second;
		if (counts.kept >= counts.grouped) {
			continue;
		}
		const idx_t skipped = counts.grouped - counts.kept;
		RecordRunTimeAccount(
		    entry.first,
		    counts.grouped == 1
		        ? string("its one instance has no value and is skipped")
		        : StringUtil::Format("%llu of %llu instances have no value and are skipped", skipped, counts.grouped));
		if (counts.kept == 0) {
			fprintf(stderr, "DECIDE warning: '%s' constrains nothing: %s no value, so %s skipped.\n",
			        WrittenClause(entry.first).c_str(),
			        counts.grouped == 1 ? "its one instance has"
			                            : StringUtil::Format("each of its %llu instances has", counts.grouped).c_str(),
			        counts.grouped == 1 ? "it is" : "each is");
		}
	}
}

void PhysicalDecide::ReportAnEmptyObjective(const SolverInput &input) const {
	if (tree->written_sense == DecideSense::FEASIBILITY) {
		return;
	}
	auto reads = [&](const CoefficientColumn &column) {
		for (idx_t row = 0; row < column.Size(); row++) {
			if (column.Get(row) != 0.0) {
				return true;
			}
		}
		return false;
	};
	auto nonzero = [](double coefficient) {
		return coefficient != 0.0;
	};
	if (std::any_of(input.objective_coefficients.begin(), input.objective_coefficients.end(),
	                [&](const vector<double> &instances) {
		                return std::any_of(instances.begin(), instances.end(), nonzero);
	                }) ||
	    std::any_of(input.objective_product_groups.begin(), input.objective_product_groups.end(),
	                [&](const EvaluatedProductGroup &group) { return reads(group.weight); }) ||
	    std::any_of(input.global_obj_coeffs.begin(), input.global_obj_coeffs.end(), nonzero)) {
		return;
	}
	fprintf(stderr, "DECIDE warning: the objective is 0 whatever the decisions: each of its terms has no value or a "
	                "zero factor on every row, so any feasible answer is optimal.\n");
	RecordRunTimeAccount(DConstants::INVALID_INDEX,
	                     "is 0 whatever the decisions: each term has no value or a zero factor on every row");
}

void PhysicalDecide::ReportInlinedHelpers(const SolverInput &input) const {
	vector<idx_t> linked_nodes;
	for (idx_t node = 0; node < tree->nodes.size(); node++) {
		if (tree->nodes[node]->Linked()) {
			linked_nodes.push_back(node);
		}
	}
	std::sort(linked_nodes.begin(), linked_nodes.end(),
	          [&](idx_t left, idx_t right) { return tree->nodes[left]->definition < tree->nodes[right]->definition; });
	for (auto node_index : linked_nodes) {
		auto &definition = tree->Definition(*tree->nodes[node_index])->Cast<DecideReducerValueDefinition>();
		const auto helper = definition.helper;
		// A SUM or AVG the plan writes in says so in the plan. What the run adds is the data's: the groups of one
		// member of a MIN or MAX, and the groups one row reads of a SUM or AVG.
		if (definition.form == DecideHelperForm::WRITTEN_IN) {
			continue;
		}
		if (helper >= input.written_in.size() ||
		    std::find(input.written_in[helper].begin(), input.written_in[helper].end(), true) ==
		        input.written_in[helper].end()) {
			continue;
		}
		// A query-wide MIN or MAX has the one group.
		const bool query_wide = tree->variable_scopes[helper].IsGlobal();
		const auto line =
		    query_wide ? definition.label
		    : definition.form == DecideHelperForm::ONE_ROW_WRITTEN_IN
		        ? definition.label + ": each group one row reads is written into that row, with no helper decision"
		        : definition.label + ": each group of one member is written where it is read, with no helper decision";
		const auto kind = query_wide ? LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_INLINED
		                             : LogicalDecide::RewriteAccount::Kind::RUN_TIME;
		// Each clause whose plan names the helper: an account of it is one of the helper (AddValueHelper).
		for (auto &account : display.rewrite_accounts) {
			if (account.helper == helper) {
				RecordRunTimeAccount(account.source_clause_id, line, kind, helper);
			}
		}
	}
}

//! One line EXPLAIN ANALYZE prints under clause `source_clause_id` (INVALID_INDEX: the objective), once.
void PhysicalDecide::RecordRunTimeAccount(idx_t source_clause_id, string subject,
                                          LogicalDecide::RewriteAccount::Kind kind, idx_t helper) const {
	for (auto &recorded : run_time_accounts) {
		if (recorded.source_clause_id == source_clause_id && recorded.subject == subject) {
			return; // a clause the rewrites made several rows of
		}
	}
	LogicalDecide::RewriteAccount account;
	account.source_clause_id = source_clause_id;
	account.kind = kind;
	account.subject = std::move(subject);
	account.helper = helper;
	run_time_accounts.push_back(std::move(account));
}

//===--------------------------------------------------------------------===//
// EvaluateTree: the objective's AVG scale
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Finalize: BuildSolverInput
//===--------------------------------------------------------------------===//

void PhysicalDecide::BuildSolverInput(idx_t num_rows, SolverInput &solver_input, VarIndexer &out_var_indexer) const {
	DecideProfileScope phase("assembly.var_indexer");
	idx_t num_decide_vars = tree->decide_variables.size();

	solver_input.diagnose = tree->diagnose;
	solver_input.num_rows = num_rows;
	solver_input.num_decide_vars = num_decide_vars;
	ReportInlinedHelpers(solver_input);
	solver_input.variable_scopes = tree->variable_scopes;

	// The flat column space, built ONCE and handed to every consumer below. It reads only the scopes and the class
	// mappings, which are final here, and the floors a `when` lowers (ResolveLiftedFloors) need its row to instance
	// map before the first box pass.
	//
	// It is reused for: (1) the row to instance map of the floors, (2) the flat indices every construct site emits
	// against, a general or indicator constraint naming flat columns, so a site that may state one natively needs the
	// index before it can emit, (3) the SolverModel::Build() call inside SolveModel(), and (4) gstate.var_indexer for
	// solution readback after the solve. Nothing below adds a decide variable, so the row / keyed / global blocks are
	// final from this point; only `solver_input.num_global_vars` keeps growing as auxiliary globals are appended,
	// and `total_vars` is refreshed just before the solve. The owning form survives past `solver_input` once that is
	// moved onto gstate.
	out_var_indexer = VarIndexer::Build(solver_input);
	phase.Next("assembly.copy_and_bounds");

	// Variable types and bounds
	solver_input.variable_types.resize(num_decide_vars);
	for (idx_t var = 0; var < num_decide_vars; var++) {
		auto &decide_var = tree->decide_variables[var]->Cast<BoundColumnRefExpression>();
		// A BOOLEAN-domain variable (declared `x(BOOL)`, or a boolean-valued
		// auxiliary such as an IN-domain/L0 indicator) is bound with an INTEGER
		// DuckDB type so it can appear in arithmetic (`M * z`, `x - v1*z1 - ...`),
		// but its solver-facing domain is BOOLEAN: `is_boolean_var` is the
		// authoritative signal, not `return_type`. Reporting it as BOOLEAN here
		// makes SolverModel::Build (ilp_model_builder.cpp) apply the `[0,1]` box
		// and mark the column binary directly from the type — the same path
		// optimizer-created BOOLEAN auxiliaries (MIN/MAX, NE indicators) already
		// use — with no constraint-tree representation of the domain at all.
		solver_input.variable_types[var] = tree->is_boolean_var[var] ? LogicalType::BOOLEAN : decide_var.return_type;
	}
	solver_input.variable_types.resize(solver_input.num_decide_vars);
	solver_input.variable_names.reserve(num_decide_vars);
	for (auto &decide_var : tree->decide_variables) {
		solver_input.variable_names.push_back(decide_var->alias);
	}
	solver_input.use_native_constructs = use_native_constructs;
	solver_input.force_native_constructs = force_native_constructs;

	// Bounds were folded in the gstate constructor from simple `x OP const` / BETWEEN constraints; the lowering walk
	// skips those comparisons, so they are not emitted again as per-row model rows.
	solver_input.constraint_sources = display.constraint_sources;
	solver_input.source_text = display.source_text;
	solver_input.objective_written =
	    DecideClauseRefOf(display.constraint_sources, display.source_text, DConstants::INVALID_INDEX);
	// A variable the query never explicitly lowered gets the default lower bound 0 and
	// stays non-negative. A variable with an explicit (possibly negative) lower bound
	// keeps it, and a written bound of -1e30 or below is no bound, as -1e29 is. Must
	// run before any consumer of lower_bounds (implied-bound propagation, Big-M,
	// McCormick, model builder).
	solver_input.lower_bounds.resize(tree->singleton_row_boxes.size());
	solver_input.upper_bounds.resize(tree->singleton_row_boxes.size());
	for (idx_t var = 0; var < tree->singleton_row_boxes.size(); var++) {
		solver_input.lower_bounds[var] = tree->singleton_row_boxes[var].EffectiveLower();
		solver_input.upper_bounds[var] = tree->singleton_row_boxes[var].upper;
	}

	// The rigid box, captured before any propagation runs and with every user-folded
	// direction re-opened. What is left is the intrinsic domain alone — the only part of
	// the column box that survives infeasibility diagnosis unchanged, and therefore the
	// only part a structural rewrite may rely on. The re-opening mirrors the folded
	// bounds' clauses in the repair model (`singleton_row_constraints`), which relax the same
	// directions for the same reason.
	solver_input.rigid_lower_bounds = solver_input.lower_bounds;
	solver_input.rigid_upper_bounds = solver_input.upper_bounds;
	for (const auto &b : tree->user_singleton_row_bounds) {
		idx_t v = b.decide_var_idx;
		bool bound_is_bool = tree->is_boolean_var[v];
		if (b.sense == '<' || b.sense == '=') {
			solver_input.rigid_upper_bounds[v] = bound_is_bool ? 1.0 : NumericLimits<double>::Maximum();
		}
		if (b.sense == '>' || b.sense == '=') {
			solver_input.rigid_lower_bounds[v] = bound_is_bool ? 0.0 : -NumericLimits<double>::Maximum();
		}
	}

	// Stage 06 from here: what it settles once for the query, before the model is formulated against a box.
	PrepareModel(solver_input, out_var_indexer, tree->singleton_row_boxes);
}

//===--------------------------------------------------------------------===//
// Finalize: FinalizeSolveResult
//===--------------------------------------------------------------------===//

SinkFinalizeType PhysicalDecide::FinalizeSolveResult(ClientContext &context, DecideGlobalSinkState &gstate,
                                                     SolverInput &solver_input, VarIndexer &var_indexer,
                                                     ElasticFormulation *elastic) const {
	// Clause-level model size: the number of constraint *specs* the operator hands the
	// builder. A per-row spec still expands to one row per data row and a grouped spec to one
	// row per group, so this is not a matrix row count — the row count is read back off
	// the built model (`solve_result.model_constraint_rows`) after the solve below.
	size_t constraint_specs = solver_input.constraints.size() + solver_input.not_equals.size() +
	                          solver_input.quadratics.size() + solver_input.global_constraints.size();

	// H5: slow-solve continuation is ordinary execution behaviour, not diagnosis, so it
	// runs with or without the prefix. The interactive prompt needs a human, so it falls
	// back to a plain error when stdin is not a terminal (tests, benchmarks, -c pipes,
	// C-API) — never prompt where no one can answer. That tty test IS the rule now; there
	// is no mode to set.
#ifdef _WIN32
	bool stdin_is_tty = _isatty(_fileno(stdin)) != 0;
#else
	bool stdin_is_tty = isatty(STDIN_FILENO) != 0;
#endif
	bool can_prompt = stdin_is_tty;
	// The backend the primary solve ran on, named at plan time. Asking
	// SelectSolverBackend() again here would be a second, independent answer —
	// harmless while selection reads only the environment, but wrong the moment it
	// depends on the model, because the elastic re-solves would then run on a
	// different solver than the one that produced the failure being diagnosed.
	SolverBackend backend = PlannedSolverBackend();
	// H3: the statement's DIAGNOSE prefix, and nothing else, arms diagnosis. Under it,
	// pre-extract the unbounded ray so it is ready if the solve turns out unbounded, and
	// retain the built model so the INFEASIBLE terminal can hand it to the elastic
	// engine. An unprefixed query pays for neither, and both failure terminals are
	// prefix-only anyway (RouteSolveResult).
	bool diagnosis_armed = tree->diagnose;
	DecideSolveOutcome solved = SolveDecide(context, solver_input, var_indexer, backend,
	                                        tree->written_sense == DecideSense::MAXIMIZE, diagnosis_armed, can_prompt);
	SolverResult &solve_result = solved.result;

	// Under DIAGNOSE, a failure is the ANSWER, not an error: hand off the findings to the
	// DIAGNOSE operator above and return no rows. Without the prefix the same failure
	// raises, naming its state and nothing more — naming the clause *is* the elastic
	// solve, and that only happens when the user asks for it.
	auto report = [&](DecideDiagnostic diag) {
		D_ASSERT(diagnosis_armed);
		StashDecideDiagnostic(context, std::move(diag));
		gstate.diagnosis_only = true;
	};

	// Route what is left to its diagnosis terminal. RouteSolveResult is a pure
	// classifier (status + armed → terminal); the operator owns the engine call, the
	// statement-scoped handoff, and the throw for each terminal.
	switch (solved.deliver_incumbent ? DiagnosisTerminal::SOLVED : RouteSolveResult(solve_result, diagnosis_armed)) {
	case DiagnosisTerminal::SOLVED:
		if (solve_result.status == SolverStatus::SUBOPTIMAL) {
			// A feasible incumbent returned without a proof of optimality (a numerically
			// hard QCP where the barrier stopped before satisfying tolerances). Deliver
			// the rows, but say plainly they are not proven best — the same caveat as an
			// early time-limit stop. There is no live SQL NOTICE channel, so it rides on
			// stderr with the results.
			fprintf(stderr,
			        "DECIDE is returning a feasible solution — the solver could not prove it is the best possible.\n");
			// Machine-readable twin of the caveat above, for the test harness only. The
			// caveat's wording is owned by the user-facing-output principle and will keep
			// moving; a suite that greps its prose silently stops matching (it already did
			// once, turning the QCQP retry into a flake). Tests key on this instead, and
			// `decidb_cli.py` strips DECIDB_STATUS lines before classifying stderr. Gated
			// on the env var so user-facing output is unchanged.
			if (std::getenv("DECIDB_STATUS_MARKERS")) {
				fprintf(stderr, "DECIDB_STATUS: SUBOPTIMAL\n");
			}
		}
		break; // fall through to the success stores below
	case DiagnosisTerminal::UNBOUNDED: {
		vector<string> var_labels;
		vector<bool> var_is_aux;
		BuildVarLabels(solver_input, var_labels, var_is_aux);
		report(UnboundedTerminal(context, solve_result, solved.model, var_indexer, var_labels, var_is_aux, gstate.data,
		                         display.input_column_names, display.input_column_in_clause));
		break;
	}
	case DiagnosisTerminal::INFEASIBLE: {
		vector<string> var_labels;
		vector<bool> var_is_aux;
		BuildVarLabels(solver_input, var_labels, var_is_aux);
		// `Finalize` retains `elastic` whenever `diagnose` is set, and this terminal is prefix-only.
		D_ASSERT(elastic);
		report(InfeasibleTerminal(context, solve_result, *elastic, var_labels, var_is_aux, backend,
		                          solved.diagnostic_options, tree->written_sense == DecideSense::MAXIMIZE));
		break;
	}
	case DiagnosisTerminal::UNDIAGNOSED:
		// No DIAGNOSE prefix, or a status no engine covers (ITERATION_LIMIT, OTHER).
		// Either way the query reports its status and stops — the second solve that
		// would name a clause only happens when the user asks for it by name.
		ThrowDecideSolveError(solve_result);
	}

	// Under DIAGNOSE a failure terminal handed off its findings instead of raising: there
	// is no solution to read back, so produce no rows and let the DIAGNOSE operator
	// above turn the handoff into the answer.
	if (gstate.diagnosis_only) {
		return SinkFinalizeType::READY;
	}

	// We are past the switch, so we are delivering rows (every failure terminal threw).
	// With the interrupt poll armed, the rare race where a user Ctrl-C landed just as the
	// solver proved OPTIMAL leaves `context.interrupted` set — which would make the
	// executor abort a query that actually succeeded. Clear it so the rows flow. (The
	// TIME_LIMIT terminal already clears it on its incumbent-stop path; this covers the
	// SOLVED path. The poll is installed on every solve.)
	context.interrupted = false;

	// The solve succeeded, so there is nothing wrong to report. Clear the handoff so a
	// DIAGNOSE above this operator sees "no findings" and answers `feasible`.
	ClearDecideDiagnostic(context);
	gstate.ilp_solution = std::move(solve_result.solution);
	// Move the indexer onto gstate now that solve is complete; readback in
	// Execute() needs it to outlive solver_input.
	gstate.var_indexer = std::move(var_indexer);

	// Two separate units, never summed: `clause_specs` is the clause-level view (which DECIDE clause to look at),
	// `constraint_rows` is what the solver actually received (where the matrix pressure is).
	DecideProfileScope::Counter("model.clause_specs", static_cast<double>(constraint_specs));
	DecideProfileScope::Counter("model.constraint_rows", static_cast<double>(solve_result.model_constraint_rows));

	return SinkFinalizeType::READY;
}

//! Per-decide-variable labels + is-aux flags for column provenance. Both failure
//! terminals need them (UNBOUNDED names the runaway variable; INFEASIBLE carries
//! them into the elastic engine); built only inside a failing arm so the SOLVED path pays nothing.
void PhysicalDecide::BuildVarLabels(const SolverInput &solver_input, vector<string> &var_labels,
                                    vector<bool> &var_is_aux) const {
	idx_t nvars = tree->decide_variables.size();
	var_labels.assign(nvars, string());
	var_is_aux.assign(nvars, false);
	for (auto &ae : display.aux_var_expressions) {
		var_labels[ae.first] = ae.second;
		var_is_aux[ae.first] = true;
	}
	for (idx_t i = 0; i < nvars; i++) {
		if (!var_is_aux[i]) {
			var_labels[i] = tree->decide_variables[i]->GetName();
		}
	}
}

//===--------------------------------------------------------------------===//
// Source (Producing Output)
//===--------------------------------------------------------------------===//
class DecideGlobalSourceState : public GlobalSourceState {
public:
	DecideProfileTotals profile_readback {"execution.output_readback"};
	explicit DecideGlobalSourceState(DecideGlobalSinkState &sink) {
		sink.data.InitializeScan(scan_state);
		sink.data.InitializeScanChunk(scan_chunk);
	}

	ColumnDataScanState scan_state;
	DataChunk scan_chunk;         // Sized to the buffered input relation, not the wider output chunk
	idx_t current_row_offset = 0; // Track which row we're at in the solution vector

	idx_t MaxThreads() override {
		return 1; // For simplicity, we'll make the source single-threaded.
	}
};

//! A solved value on its way into an integer output column.
//!
//! The solver works in doubles, so what comes back is not bounded by the column's type.
//! A declared bound is not the limit either: `SUM(x) <= 5000000000` bounds no single
//! decision, so nothing earlier in the pipeline can promise the value fits. Casting an
//! out-of-range double to a narrower integer is undefined in C++ and saturates on the
//! platforms we build, which would present the type's limit as the answer — a number
//! that violates the query's own constraints, reported with no error and no diagnostic.
//! Refuse it by name instead.
//!
//! `INT` decisions are BIGINT (see the type mapping in BindSelectNode), so the reachable
//! limit here is a double's exact-integer range rather than the column's: past 2^53 a
//! double no longer counts consecutively, and no wider integer type recovers a value the
//! solver never distinguished. That makes the refusal honest rather than a missing
//! widening. `BOOL` decisions and boolean auxiliaries stay INTEGER and cannot reach it.
static int64_t DecideSolutionToInteger(double solution_value, const LogicalType &var_type, const string &var_name,
                                       optional_idx declaration) {
	double rounded = std::round(solution_value);
	// Both ends spelled out rather than a symmetric magnitude: int32's range is not
	// symmetric, and `-2147483648` is a value the column can hold.
	double lo = var_type == LogicalType::INTEGER ? static_cast<double>(NumericLimits<int32_t>::Minimum())
	                                             : -DECIDE_MAX_EXACT_INTEGER;
	double hi = var_type == LogicalType::INTEGER ? static_cast<double>(NumericLimits<int32_t>::Maximum())
	                                             : DECIDE_MAX_EXACT_INTEGER;
	// The helper refuses a NaN, which compares false against everything, rather than
	// passing it through to the cast this exists to protect.
	if (!DecideRoundedAnswerWithin(solution_value, lo, hi)) {
		// The wording is shared with A1's direct plan (`decide_numeric_policy.hpp`); the error points at the decision's
		// declaration.
		throw DecideInvalidValueError(DecideValueRole::INTEGER_ANSWER,
		                              DecideWholeNumberRangeBefore(var_name) + StringUtil::Format("%.0f", rounded),
		                              declaration);
	}
	return static_cast<int64_t>(rounded);
}

unique_ptr<GlobalSourceState> PhysicalDecide::GetGlobalSourceState(ClientContext &context) const {
	auto &sink = sink_state->Cast<DecideGlobalSinkState>();
	return make_uniq_base<GlobalSourceState, DecideGlobalSourceState>(sink);
}

SourceResultType PhysicalDecide::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<DecideGlobalSinkState>();
	auto &source_state = input.global_state.Cast<DecideGlobalSourceState>();
	DecideProfileSample profile(source_state.profile_readback);

	// The solve failed under DIAGNOSE: there is no assignment to read back, so this
	// operator produces nothing and the diagnosis it handed off is the statement's answer.
	if (gstate.diagnosis_only) {
		return SourceResultType::FINISHED;
	}

	// Scan the original buffered data into a chunk sized to match the collection;
	// `chunk` is wider (it has the appended DECIDE variable columns), so scanning
	// directly into it would violate ColumnDataCollectionSegment::ReadChunk's
	// column-count assertion.
	gstate.data.Scan(source_state.scan_state, source_state.scan_chunk);
	if (source_state.scan_chunk.size() == 0) {
		return SourceResultType::FINISHED;
	}

	// Reference the input's columns into the leading columns of the wide output chunk. The
	// collection may hold more after them -- the values frame selectors read
	// -- which are no output.
	const idx_t input_columns = types.size() - tree->decide_variables.size();
	for (idx_t col = 0; col < input_columns; col++) {
		chunk.data[col].Reference(source_state.scan_chunk.data[col]);
	}
	chunk.SetCardinality(source_state.scan_chunk.size());

	// All DECIDE vars (user + auxiliary) are in the output; projection above prunes aux vars
	idx_t total_decide_vars = tree->decide_variables.size();
	idx_t chunk_size = chunk.size();

	// Fill in ALL DECIDE variable columns with solution values from ILP solver
	for (idx_t decide_var_idx = 0; decide_var_idx < total_decide_vars; decide_var_idx++) {
		// The DECIDE columns are appended at the end of the output
		idx_t column_idx = types.size() - total_decide_vars + decide_var_idx;

		auto &output_vector = chunk.data[column_idx];

		// An auxiliary's column only carries it to the projection that drops it: nobody
		// reads its value, so none is converted -- nor refused, which would name an
		// internal column for a value the user never asked for.
		if (decide_var_idx >= total_decide_vars - tree->num_auxiliary_vars) {
			output_vector.SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(output_vector, true);
			continue;
		}

		// Set vector to flat (each row has its own value)
		output_vector.SetVectorType(VectorType::FLAT_VECTOR);

		// Get the logical type for this DECIDE variable
		auto &decide_var = tree->decide_variables[decide_var_idx]->Cast<BoundColumnRefExpression>();
		auto var_type = decide_var.return_type;

		// The solver's value for each row of this chunk: 0 where it returned none, or where the row's instance has no
		// column (VarIndexer::ColumnOrNone).
		auto solution_at = [&](idx_t row_in_chunk) {
			idx_t solution_idx =
			    gstate.var_indexer.ColumnOrNone(decide_var_idx, source_state.current_row_offset + row_in_chunk);
			return solution_idx < gstate.ilp_solution.size() ? gstate.ilp_solution[solution_idx] : 0.0;
		};
		if (var_type == LogicalType::INTEGER) {
			auto output_data = FlatVector::GetData<int32_t>(output_vector);
			for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
				output_data[row_in_chunk] = static_cast<int32_t>(
				    DecideSolutionToInteger(solution_at(row_in_chunk), var_type, decide_var.GetName(),
				                            tree->variable_scopes[decide_var_idx].declaration));
			}
		} else if (var_type == LogicalType::BIGINT) {
			auto output_data = FlatVector::GetData<int64_t>(output_vector);
			for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
				output_data[row_in_chunk] =
				    DecideSolutionToInteger(solution_at(row_in_chunk), var_type, decide_var.GetName(),
				                            tree->variable_scopes[decide_var_idx].declaration);
			}
		} else {
			// A user decision is INTEGER (BOOL), BIGINT (INT) or DOUBLE (REAL) (DecideDeclarationsBinder).
			auto output_data = FlatVector::GetData<double>(output_vector);
			for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
				output_data[row_in_chunk] = solution_at(row_in_chunk);
			}
		}

		// A row in no class of a keyed decision's key has no decision: its value is NULL.
		if (decide_var_idx < gstate.var_indexer.var_scope.size() &&
		    gstate.var_indexer.var_scope[decide_var_idx] == DecideVarScope::KEYED) {
			auto &mapping = gstate.var_indexer.keyed_mappings[gstate.var_indexer.var_keyed_mapping_idx[decide_var_idx]];
			for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
				if (mapping.IsAbsent(source_state.current_row_offset + row_in_chunk)) {
					FlatVector::SetNull(output_vector, row_in_chunk, true);
				}
			}
		}
	}

	// Update row offset for next chunk
	source_state.current_row_offset += chunk_size;

	return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
