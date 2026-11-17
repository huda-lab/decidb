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
#include <cstdlib>
#include <fstream>
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
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic_engines.hpp"
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
// The slow-solve report needs peak process memory, and its prompt needs to know
// whether stdin is a terminal. Both are platform primitives; MSVC has neither
// <sys/resource.h> nor <unistd.h>. Same split as gurobi_loader.cpp.
#ifdef _WIN32
#include <cstdio> // _fileno
#include <io.h>   // _isatty
#else
#include <sys/resource.h> // getrusage — peak process memory
#include <unistd.h>       // isatty / STDIN_FILENO
#endif
#include <iostream> // std::cin / std::getline — read the continuation decision at a time-limit stop

namespace duckdb {

//! A composed objective is an objective E whose body holds its composed terms' G nodes.
static bool TreeHasComposedObjective(const DecideTree &tree) {
	for (auto root : tree.roots) {
		if (tree.nodes[root]->objective && !tree.nodes[root]->compiled.composed.empty()) {
			return true;
		}
	}
	return false;
}

//! Input column `column`'s rows grouped by value as GROUP BY groups them, NULL as a value (DecideKeyMap), in a scan of
//! their own: each row's group, and each group's value, numbered in the order the rows first read them. The diagnosis
//! orders what it reports itself (by escape rate, column and value), so no group's number reaches a message.
static void GroupColumn(ClientContext &context, const ColumnDataCollection &data, idx_t column,
                        vector<idx_t> &row_to_group, vector<Value> &values) {
	DecideKeyMap map(context, {data.Types()[column]});
	ColumnDataScanState scan;
	data.InitializeScan(scan, {column});
	DataChunk chunk;
	data.InitializeScanChunk(scan, chunk);
	row_to_group.assign(data.Count(), DConstants::INVALID_INDEX);
	values.clear();
	unsafe_vector<idx_t> slots;
	idx_t next = 0;
	for (idx_t row = 0; data.Scan(scan, chunk); row += chunk.size()) {
		slots.resize(chunk.size());
		map.FindOrCreate(chunk, slots.data(), next);
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (slots[i] == values.size()) {
				values.push_back(chunk.GetValue(0, i));
			}
			row_to_group[row + i] = slots[i];
		}
	}
}

//===--------------------------------------------------------------------===//
// Constructor
//===--------------------------------------------------------------------===//

PhysicalDecide::PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality,
                               unique_ptr<PhysicalOperator> child, idx_t decide_index,
                               vector<unique_ptr<Expression>> decide_variables,
                               unique_ptr<Expression> decide_constraints, DecideSense written_sense,
                               unique_ptr<Expression> decide_objective)
    : PhysicalOperator(PhysicalOperatorType::DECIDE, std::move(types), estimated_cardinality),
      decide_index(decide_index), decide_variables(std::move(decide_variables)),
      decide_constraints(std::move(decide_constraints)), written_sense(written_sense),
      decide_objective(std::move(decide_objective)) {
	children.push_back(std::move(child));
}

DecideSourceNames PhysicalDecide::Names() const {
	return DecidePlanNames(source_text, keyed_scopes, keys, decide_index, aux_var_expressions, frames);
}

string PhysicalDecide::WrittenClause(idx_t source_clause_id) const {
	return DecideWrittenClause(constraint_sources, DecideWrittenObjective(written_sense, written_objective),
	                           source_clause_id);
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
	for (auto &account : rewrite_accounts) {
		const bool replaced = std::any_of(
		    run_time_accounts.begin(), run_time_accounts.end(), [&](const LogicalDecide::RewriteAccount &run) {
			    return run.kind == LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_INLINED &&
			           run.source_clause_id == account.source_clause_id && run.subject == account.subject;
		    });
		if (!replaced) {
			accounts.push_back(account);
		}
	}
	accounts.insert(accounts.end(), run_time_accounts.begin(), run_time_accounts.end());
	auto result = RenderDecideParams(*this, accounts, TreeHasComposedObjective(*tree), tree.get());
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

//===--------------------------------------------------------------------===//
// Multi-variable per-row constraint helpers
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//

//! The chunk to the thread's descent, in pieces of `decide_debug`'s chunk size when one is set.
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

//! A thread's rows and its descent over them (language/guide.md §6.1), each row numbered from the thread's first: a
//! descent per thread, as a hash aggregate builds a table per thread, merged into the operator's in Combine.
class DecideLocalSinkState : public LocalSinkState {
public:
	DecideProfileTotals profile_sink {"execution.sink_append"};
	ColumnDataCollection data;
	ColumnDataAppendState append_state;
	unique_ptr<DecideDescent> descent;
	//! The rows after which the thread's are merged before its input ends: `decide_debug`'s split, which a test sets
	//! to merge every chunk as a thread of its own would; never otherwise.
	const idx_t split;

	explicit DecideLocalSinkState(ClientContext &context, const PhysicalDecide &op)
	    : data(context, op.children[0]->GetTypes()), split(op.debug.split) {
		first_input_row.Initialize(context, op.children[0]->GetTypes(), 1);
		data.InitializeAppend(append_state);
	}
	DataChunk first_input_row;
	bool has_first_input_row = false;
};

//! A thread's rows placed after those already placed, and its descent kept with the row they start at, which
//! Finalize merges (DecideDescent::MergeAll).
static void MergeDecideDescent(DecideGlobalSinkState &gstate, DecideLocalSinkState &lstate) {
	const auto count = lstate.data.Count();
	if (count == 0) {
		return;
	}
	lock_guard<mutex> guard(gstate.lock);
	if (!gstate.has_first_input_row && lstate.has_first_input_row) {
		lstate.first_input_row.Copy(gstate.first_input_row);
		gstate.has_first_input_row = true;
	}
	const auto offset = gstate.data.Count();
	gstate.data.Combine(lstate.data);
	lstate.data.InitializeAppend(lstate.append_state);
	gstate.descent_parts.emplace_back(std::move(lstate.descent), offset);
	gstate.descent_rows += count;
}

unique_ptr<GlobalSinkState> PhysicalDecide::GetGlobalSinkState(ClientContext &context) const {
	// The descent reads only the rows reaching each node (plan L2): checked once, before the first row.
	DecideDescent::RequireRowReads(*tree, children[0]->GetTypes().size());
	return make_uniq_base<GlobalSinkState, DecideGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalDecide::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq_base<LocalSinkState, DecideLocalSinkState>(context.client, *this);
}

//! Rows `chunk` read by the thread's descent at the rows its buffer gives them, which Combine rebases on the rows
//! placed before.
static void SinkDecideRows(const DecideTree &tree, const DecideDebug &debug, ClientContext &context,
                           DecideGlobalSinkState &gstate, DecideLocalSinkState &lstate, DataChunk &chunk) {
	const auto row_base = lstate.data.Count();
	if (chunk.size() > 0 && !lstate.has_first_input_row) {
		SelectionVector first_row(1);
		first_row.set_index(0, 0);
		chunk.Copy(lstate.first_input_row, first_row, 1);
		lstate.has_first_input_row = true;
	}
	lstate.data.Append(lstate.append_state, chunk);
	if (!lstate.descent) {
		lstate.descent = make_uniq<DecideDescent>(tree, context);
	}
	AppendDecideDescent(*lstate.descent, chunk, row_base, debug);
	if (lstate.data.Count() >= lstate.split) {
		MergeDecideDescent(gstate, lstate);
	}
}

SinkResultType PhysicalDecide::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<DecideLocalSinkState>();
	auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
	DecideProfileSample profile(lstate.profile_sink);
	// Top-down (language/guide.md §6.1): the thread's descent reads each chunk as it arrives; one split finer than
	// the chunk reads it a piece at a time, each merged as a thread of its own would be.
	if (chunk.size() <= lstate.split) {
		SinkDecideRows(*tree, debug, context.client, gstate, lstate, chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}
	for (idx_t begin = 0; begin < chunk.size(); begin += lstate.split) {
		const auto count = MinValue<idx_t>(lstate.split, chunk.size() - begin);
		SelectionVector selection(count);
		for (idx_t i = 0; i < count; i++) {
			selection.set_index(i, begin + i);
		}
		DataChunk piece;
		piece.InitializeEmpty(chunk.GetTypes());
		piece.Slice(chunk, selection, count);
		SinkDecideRows(*tree, debug, context.client, gstate, lstate, piece);
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDecide::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	DecideProfileScope profile("execution.sink_combine");
	MergeDecideDescent(input.global_state.Cast<DecideGlobalSinkState>(),
	                   input.local_state.Cast<DecideLocalSinkState>());
	return SinkCombineResultType::FINISHED;
}

//! Builds the categorical-candidate groupings the unbounded engine consumes to
//! characterize *which* instances of an escaping variable run away. Operator-bound
//! (it reads the input's named columns, keyed scopes, and GroupColumn), so it lives
//! here rather than in the pure router/engine units; Finalize just invokes it.
//!
//! A stateful functor (not a set of lambdas) so its lazily-built caches — the
//! row-scoped candidates and the per-scope keyed candidates — survive across the
//! many calls the engine makes, one per escaping decision variable. The stored
//! references point at Finalize-locals that outlive the engine call.
namespace {
struct UnboundedCandidateProvider {
	ClientContext &context;
	ColumnDataCollection &data;
	vector<LogicalType> types;
	const DecideDiagParams &params;
	const vector<string> &input_column_names;
	const vector<bool> &input_column_in_clause;
	const VarIndexer &var_indexer;
	const vector<KeyedMapping> &keyed_mappings;
	idx_t num_rows;

	//! Row-scoped candidate columns are variable-independent — built once.
	vector<ColumnGrouping> row_candidates;
	bool row_candidates_built = false;
	//! Keyed candidates cached per scope.
	std::map<idx_t, vector<ColumnGrouping>> keyed_candidates;

	//! The names and flags are sized to the child's columns (plan_decide.cpp), as `types` is.
	bool ColumnHasHarvestedName(idx_t ci) const {
		return !input_column_names[ci].empty();
	}

	//! Build a single-column row grouping, keeping it only if it is categorical
	//! (2 ≤ distinct ≤ max(min_categories, ratio × denom)).
	bool BuildRowGrouping(idx_t ci, idx_t denom, ColumnGrouping &out) {
		// Suppress rules over columns whose source name we never resolved (e.g.
		// referenced only in the outer SELECT): naming them with a positional
		// `colN` the user never wrote is worse than staying silent.
		if (!ColumnHasHarvestedName(ci)) {
			return false;
		}
		// The named columns are read in a scan of their own when the diagnosis first asks for one: it explains the
		// solved model after the evaluation, whose pass did not read them.
		vector<idx_t> row_to_group;
		vector<Value> values;
		GroupColumn(context, data, ci, row_to_group, values);
		const idx_t num_groups = values.size();
		idx_t cap = std::max(params.min_categories, (idx_t)(params.categorical_ratio * (double)denom));
		if (num_groups < 2 || num_groups > cap) {
			return false;
		}
		out.column = input_column_names[ci];
		out.clause_referenced = input_column_in_clause[ci];
		out.instance_to_group = std::move(row_to_group); // row-indexed
		out.group_value = std::move(values);
		return true;
	}

	vector<ColumnGrouping> &GetRowCandidates() {
		if (!row_candidates_built) {
			for (idx_t ci = 0; ci < types.size(); ci++) {
				ColumnGrouping cg;
				if (BuildRowGrouping(ci, num_rows, cg)) {
					row_candidates.push_back(std::move(cg));
				}
			}
			row_candidates_built = true;
		}
		return row_candidates;
	}

	//! Lift a row-indexed grouping to class granularity. The lift is accepted only
	//! when every joined row for a class maps to the same categorical group; this
	//! lets dimension-table labels characterize keyed escapes without inventing a
	//! single value for genuinely one-to-many columns.
	bool LiftRowGroupingToClasses(const ColumnGrouping &rowcg, const KeyedMapping &mapping, idx_t num_classes,
	                              ColumnGrouping &out) {
		out.column = rowcg.column;
		out.clause_referenced = rowcg.clause_referenced;
		out.group_value = rowcg.group_value;
		out.instance_to_group.assign(num_classes, DConstants::INVALID_INDEX);
		vector<bool> seen(num_classes, false);
		// Both maps have an entry per data row, and a row in no class has the spare class
		// (KeyedMapping::absent_class), so every class is below `num_classes`.
		for (idx_t r = 0; r < num_rows; r++) {
			idx_t e = mapping.row_to_class[r];
			idx_t g = rowcg.instance_to_group[r];
			if (!seen[e]) {
				out.instance_to_group[e] = g;
				seen[e] = true;
			} else if (out.instance_to_group[e] != g) {
				return false;
			}
		}
		return true;
	}

	//! Keyed candidate columns are any named DECIDE-clause input columns
	//! whose value is constant within each class; grouping is lifted to class
	//! granularity after the constancy check.
	vector<ColumnGrouping> &GetKeyedCandidates(idx_t scope_idx) {
		auto found = keyed_candidates.find(scope_idx);
		if (found != keyed_candidates.end()) {
			return found->second;
		}
		vector<ColumnGrouping> cands;
		auto &mapping = keyed_mappings[scope_idx];
		idx_t num_classes = mapping.num_classes;
		for (idx_t ci = 0; ci < types.size(); ci++) {
			ColumnGrouping rowcg;
			if (!BuildRowGrouping(ci, num_classes, rowcg)) {
				continue;
			}
			ColumnGrouping ecg;
			if (!LiftRowGroupingToClasses(rowcg, mapping, num_classes, ecg)) {
				continue;
			}
			cands.push_back(std::move(ecg));
		}
		return keyed_candidates.emplace(scope_idx, std::move(cands)).first->second;
	}

	//! The indexer has a scope for every decision, and a keyed one has its mapping (BuildVarIndexer). The engine asks
	//! only for a decision some of whose instances escape and which has more than one (DiagnoseUnbounded), so never for
	//! a query-wide one.
	vector<ColumnGrouping> operator()(idx_t decide_var_idx) {
		if (var_indexer.var_scope[decide_var_idx] == DecideVarScope::ROW) {
			return GetRowCandidates();
		}
		return GetKeyedCandidates(var_indexer.var_keyed_mapping_idx[decide_var_idx]);
	}
};
} // namespace

//===--------------------------------------------------------------------===//
// Slow-solve checkpoint report (S2)
//===--------------------------------------------------------------------===//
// On a time-limit termination we tell the SQL user what the solver already found
// and how far it can still improve, in plain language (no "incumbent"/"gap"/"bound"
// solver words per the user-output rule). S2 prints the informational block only;
// the interactive "keep going?" prompt and warm re-solve are S3.

//! Peak resident set size of the whole process, in bytes. Whole-process, not
//! solve-only — it answers "will continuing risk running out of RAM." ru_maxrss is
//! bytes on macOS but KiB on Linux, so normalize.
//! Returns 0 when the figure is unavailable; callers drop the memory line rather
//! than reporting a zero that reads as a measurement.
static double PeakProcessMemoryBytes() {
#ifdef _WIN32
	// Windows has no getrusage. The equivalent (GetProcessMemoryInfo) would add a
	// psapi link entry to both CMake and setup.py, so the report simply omits its
	// memory line here instead.
	return 0.0;
#else
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) != 0) {
		return 0.0;
	}
#ifdef __APPLE__
	return (double)usage.ru_maxrss;
#else
	return (double)usage.ru_maxrss * 1024.0;
#endif
#endif
}

//! Compact byte count, e.g. "1.2 GB" / "340 MB". The figure is a process's peak resident set, never under a megabyte.
static string FormatMemory(double bytes) {
	const char *unit = "MB";
	double value = bytes / (1024.0 * 1024.0);
	if (value >= 1024.0) {
		value /= 1024.0;
		unit = "GB";
	}
	return StringUtil::Format(value >= 100.0 ? "%.0f %s" : "%.1f %s", value, unit);
}

//! Seconds, integer when whole (300s), one decimal for sub-limit probe values (1.5s).
//! Sub-second values keep their significant digits (0.05s) — one decimal would round a
//! small time limit up to a misleading 0.1s (or down to 0.0s).
static string FormatDuration(double seconds) {
	if (std::fabs(seconds) < 1.0) {
		return StringUtil::Format("%.2gs", seconds);
	}
	if (std::fabs(seconds - std::round(seconds)) < 0.05) {
		return StringUtil::Format("%.0fs", std::round(seconds));
	}
	return StringUtil::Format("%.1fs", seconds);
}

string FormatDecideGapPercent(double gap) {
	// Fixed decimals round a gap of 2e-6 to "0.00%", which beside "not proven best" reads as a contradiction. Two
	// significant digits keep it (0.0002%); from 1% up, the whole number plus one decimal, so %g never switches a gap
	// of 100% or more to an exponent.
	double percent = gap * 100.0;
	int digits = 2 + (percent >= 1.0 ? static_cast<int>(std::floor(std::log10(percent))) : 0);
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.*g", digits, percent);
	return buffer;
}

//! Print the path-1 (solution found) / path-2 (no solution) status block to stderr.
//! Info only — the "keep improving it?" prompt lands with the S3 continuation loop. The model minimizes, so a query
//! written MAXIMIZE (`negate_objective`) shows its best objective so far as the negated model value, as it wrote it.
static void PrintDecideTimeoutReport(const SolverResult &result, double elapsed_seconds, double time_limit,
                                     bool negate_objective) {
	string limit_str = FormatDuration(time_limit);
	// 0 means "not measurable here" (Windows, or a getrusage failure), not "no memory
	// used" — say nothing rather than print "peak memory 0 B".
	double peak_bytes = PeakProcessMemoryBytes();
	string tail = peak_bytes > 0.0
	                  ? StringUtil::Format("  elapsed %s · peak memory %s\n", FormatDuration(elapsed_seconds).c_str(),
	                                       FormatMemory(peak_bytes).c_str())
	                  : StringUtil::Format("  elapsed %s\n", FormatDuration(elapsed_seconds).c_str());
	// A user Ctrl-C and a wall-clock timeout share the same best-so-far readback, but
	// must not read the same to the user: "you stopped it" vs "the clock ran out".
	if (result.has_solution) {
		const double shown = negate_objective ? 0.0 - result.objective_value : result.objective_value;
		// gap is a fraction on both backends; render as a percentage. It is NaN when
		// no proven bound exists (LP/QP timeouts, failed reads) — claim nothing then.
		string closeness = std::isfinite(result.gap)
		                       ? StringUtil::Format("  best objective so far: %g  (within %s%% of the best possible)\n",
		                                            shown, FormatDecideGapPercent(result.gap))
		                       : StringUtil::Format("  best objective so far: %g\n", shown);
		if (result.user_interrupted) {
			fprintf(stderr, "DECIDE stopped at your request with a usable solution (not proven best).\n%s%s",
			        closeness.c_str(), tail.c_str());
		} else {
			fprintf(stderr, "DECIDE hit the %s time limit with a usable solution (not proven best).\n%s%s",
			        limit_str.c_str(), closeness.c_str(), tail.c_str());
		}
	} else {
		// A stop at the user's request with no solution never gets here: it raises first (FinalizeSolveResult).
		fprintf(stderr, "DECIDE hit the %s time limit without finding a solution yet.\n%s", limit_str.c_str(),
		        tail.c_str());
	}
}

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
	DecideProfileScope profile("execution.finalize");
	bool bench = std::getenv("DECIDB_BENCH") != nullptr;
	Profiler model_timer;
	Profiler solver_timer;

	auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
	idx_t num_rows = gstate.data.Count();
	run_time_accounts.clear();

	if (bench) {
		model_timer.Start();
	}

	// Empty input → return empty result (mirrors standard SQL behavior)
	if (num_rows == 0) {
		return SinkFinalizeType::READY;
	}
	// The threads' descents, merged in the order their rows were placed.
	{
		DecideProfileScope::Counter("execution.descent_parts", static_cast<double>(gstate.descent_parts.size()));
		DecideProfileScope merge_scope("execution.descent_merge");
		gstate.descent = DecideDescent::MergeAll(std::move(gstate.descent_parts), context);
	}

	// The values each key's groups are named by in messages (D69), one table per naming site.
	vector<SolverInput::KeyValues> key_values;

	vector<KeyedMapping> keyed_mappings;
	EvaluatedClauses evaluated;
	EvaluateTree(context, gstate, num_rows, key_values, keyed_mappings, evaluated);
	// What the solve and the output read of the evaluation is copied into `evaluated` and gstate's fields, so the
	// node containers go now rather than with the global state.
	gstate.descent.reset();

	//===--------------------------------------------------------------------===//
	// PHASE 3: Build and Solve ILP
	//===--------------------------------------------------------------------===//

	// 3a. Assemble the model input and settle the column box. Nothing below this line
	// touches the data again.
	DecideProfileScope phase("execution.assemble_input");
	VarIndexer var_indexer;
	SolverInput solver_input =
	    BuildSolverInput(gstate, num_rows, std::move(keyed_mappings), std::move(key_values), evaluated, var_indexer);

	// Under DIAGNOSE, keep the model input and column space as assembly left them —
	// before a single constant was derived. An infeasibility repair searches a WIDER box
	// than the solved model declares, and a Big-M or a McCormick envelope is sound only
	// for the box it came from, so the INFEASIBLE terminal formulates a second model from
	// this instead of re-deriving nothing and inheriting the narrow one's constants. It
	// also emits the scoped bounds the column box enforces, which the solved model has no
	// row for. The copy is paid for only when the user asked for a diagnosis.
	unique_ptr<ElasticFormulation> elastic;
	if (diagnose) {
		elastic = make_uniq<ElasticFormulation>();
		elastic->input = solver_input;
		for (auto &folded : gstate.evaluated_singleton_row_constraints) {
			elastic->input.constraints.push_back(std::move(folded));
		}
		elastic->var_indexer = var_indexer;
	}

	// 3b. Formulate: derive the constants and emit the rows, against the box the model
	// declares. A pure function of what came before, which is what lets the elastic
	// (infeasibility-repair) path run it a second time against a widened box.
	phase.Next("execution.formulate");
	FormulateModel(solver_input, FormulationBox::OfSolvedModel(solver_input), var_indexer, evaluated);
	ReportAnEmptyObjective(solver_input);

	phase.Next("execution.solve_and_route");
	return FinalizeSolveResult(context, gstate, solver_input, var_indexer, evaluated, elastic.get(), bench, model_timer,
	                           solver_timer);
}

void PhysicalDecide::EvaluateTree(ClientContext &context, DecideGlobalSinkState &gstate, idx_t num_rows,
                                  vector<SolverInput::KeyValues> &key_values, vector<KeyedMapping> &keyed_mappings,
                                  EvaluatedClauses &evaluated) const {
	auto &descent = gstate.descent;
	// Top-down: the descent read every chunk as Sink received it, and the buffer is never scanned to evaluate a row.
	DecideProfileScope phase("execution.tree_descent");
	// A value the whole query shares is read (a composed term's or a square's factor) against the retained input row.
	DataChunk first;
	if (gstate.has_first_input_row) {
		first.Initialize(context, gstate.first_input_row.GetTypes(), 1);
		first.Reference(gstate.first_input_row);
	} else {
		// Preserve the empty-input path: no row was available to retain during Sink.
		gstate.data.InitializeScanChunk(first);
		gstate.data.FetchChunk(0, first);
	}
	descent->Finish();
	DecideProfileScope::Counter("execution.descent_rows", static_cast<double>(gstate.descent_rows));
	// One ledger for the evaluation: what the descent's routing raised, then what the programs raise where a read
	// reaches them, each at its rank; the least is raised once every phase has run.
	DecideErrorLedger errors;
	errors.Append(descent->Errors());
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
	DecideTreeColumns columns(
	    *tree, *descent, context, names, [&](idx_t clause) { return WrittenClause(clause); }, errors, debug);
	// The frames' timelines and the columns they read, before any program reads one (language/guide.md §5.6).
	columns.ReadFrames(frames, num_rows, gstate.frame_maps);
	// What the data checks and the data reducers no clause reads raise, at their ranks, before any clause is read.
	columns.CheckData();
	DecideModelEmitter emitter(*this, context, *tree, *descent, columns, gstate, key_values, names, num_rows, errors,
	                           first);
	emitter.KeyedMappings(keyed_mappings);
	emitter.Constraints();
	// A relation-qualified reducer reads each class's first entry, its first row, so the objective's columns are
	// qualified as they are read.
	emitter.Objective(evaluated);
	ScaleObjectiveAvg(gstate, num_rows, evaluated);
	emitter.Composed(evaluated);
	// What the data left of each clause is reported once its clauses, its objective and its composed clauses are
	// read, unless one raised.
	if (!errors.HasErrorBefore(DecideErrorPhase::VALUE_LINKS)) {
		ReportSkippedInstances(gstate);
	}
	emitter.ValueLinks();
	// The bottom-up values the evaluation computed, each node's programs on its entries.
	if (debug.values) {
		fprintf(stderr, "DECIDB_VALUES_BEGIN\n%sDECIDB_VALUES_END\n", columns.Dump().c_str());
	}
	errors.ThrowIfAny();
}

static double Id(idx_t id) {
	return id == DConstants::INVALID_INDEX ? -1.0 : static_cast<double>(id);
}

static vector<double> Ids(const vector<idx_t> &ids) {
	vector<double> numbers;
	numbers.reserve(ids.size());
	for (auto id : ids) {
		numbers.push_back(Id(id));
	}
	return numbers;
}

static vector<double> Mask(const vector<bool> &mask) {
	return vector<double>(mask.begin(), mask.end());
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 1.6 -- scope checks
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
	for (auto &entry : gstate.clause_instances) {
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
	if (written_sense == DecideSense::FEASIBILITY) {
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
	if (std::any_of(input.objective_coefficients.begin(), input.objective_coefficients.end(), reads) ||
	    std::any_of(input.objective_product_groups.begin(), input.objective_product_groups.end(),
	                [&](const EvaluatedProductGroup &group) { return reads(group.weight); }) ||
	    std::any_of(input.global_obj_coeffs.begin(), input.global_obj_coeffs.end(),
	                [](double coefficient) { return coefficient != 0.0; })) {
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
		if (tree->nodes[node]->link != DConstants::INVALID_INDEX) {
			linked_nodes.push_back(node);
		}
	}
	std::sort(linked_nodes.begin(), linked_nodes.end(),
	          [&](idx_t left, idx_t right) { return tree->nodes[left]->link < tree->nodes[right]->link; });
	for (auto node_index : linked_nodes) {
		const auto &node = *tree->nodes[node_index];
		if (!node.decision_value || node.value_var >= input.variable_scopes.size() || node.value_link_label.empty()) {
			throw InternalException("DECIDE: linked node %llu has incomplete runtime reporting metadata", node_index);
		}
		string line;
		auto kind = LogicalDecide::RewriteAccount::Kind::RUN_TIME;
		if (input.variable_scopes[node.value_var].scope == DecideVarScope::INLINED) {
			line = node.value_link_label;
			kind = LogicalDecide::RewriteAccount::Kind::VALUE_HELPER_INLINED;
		} else if (node.value_var < input.written_in.size() &&
		           std::find(input.written_in[node.value_var].begin(), input.written_in[node.value_var].end(), true) !=
		               input.written_in[node.value_var].end()) {
			line = node.value_link_label + ": each group of one member is written where it is read, with no column";
		}
		if (line.empty()) {
			continue;
		}
		// Each clause whose plan names the helper: an account of it is one of the helper (AddValueHelper).
		for (auto &account : rewrite_accounts) {
			if (account.subject == node.value_link_label) {
				RecordRunTimeAccount(account.source_clause_id, line, kind);
			}
		}
	}
}

//! One line EXPLAIN ANALYZE prints under clause `source_clause_id` (INVALID_INDEX: the objective), once.
void PhysicalDecide::RecordRunTimeAccount(idx_t source_clause_id, string subject,
                                          LogicalDecide::RewriteAccount::Kind kind) const {
	for (auto &recorded : run_time_accounts) {
		if (recorded.source_clause_id == source_clause_id && recorded.subject == subject) {
			return; // a clause the rewrites made several rows of
		}
	}
	LogicalDecide::RewriteAccount account;
	account.source_clause_id = source_clause_id;
	account.kind = kind;
	account.subject = std::move(subject);
	run_time_accounts.push_back(std::move(account));
}

void PhysicalDecide::ScaleObjectiveAvg(DecideGlobalSinkState &gstate, idx_t num_rows, EvaluatedClauses &evaluated) {
	for (idx_t term_idx = 0; term_idx < evaluated.linear_filters.size(); term_idx++) {
		auto &filter = evaluated.linear_filters[term_idx];
		if (filter.avg_scale) {
			ScaleAvgRows(gstate.evaluated_objective_coefficients[term_idx], filter.has_filter, filter.mask, num_rows,
			             filter.avg_group_ids, filter.avg_num_groups);
		}
	}
	// A product group's AVG divides its weight, which multiplies the whole product.
	for (idx_t group_idx = 0; group_idx < evaluated.group_filters.size(); group_idx++) {
		auto &filter = evaluated.group_filters[group_idx];
		if (filter.avg_scale) {
			ScaleAvgRows(gstate.evaluated_objective_groups[group_idx].weight, filter.has_filter, filter.mask, num_rows,
			             filter.avg_group_ids, filter.avg_num_groups);
		}
	}
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 2, sub-phase 3 -- Evaluate composed MIN/MAX clauses
//===--------------------------------------------------------------------===//

void PhysicalDecide::FinishComposedClause(DecideGlobalSinkState &gstate, idx_t num_rows, const DecideNode &node,
                                          bool null_factor, const std::function<vector<bool>()> &kept_rows,
                                          const std::function<idx_t(const vector<bool> &)> &class_set,
                                          EvaluatedClauses::ComposedConstraint clause,
                                          EvaluatedClauses &evaluated) const {
	// A NULL factor makes the clause's one instance NULL, which is skipped (deck p. 50), as does a reducer over no row.
	clause.skipped = null_factor || std::any_of(clause.terms.begin(), clause.terms.end(), DecideComposedTermReadsNoRow);
	clause.repair_group_id = tree->constraint_count + tree->singleton_row_count + evaluated.composed_constraints.size();
	clause.source_clause_id = node.source_clause_id;
	clause.removal_group_id = node.removal_group_id;
	// A `per K:` holds once per class: with every kept row's K NULL there is none, so no instance (D5). `kept_rows`
	// gives only the kept rows in a class (DecideTreePer::KEY), while its reducers read every kept row.
	const bool names_classes = node.facts && node.facts->scope.per == DecideClauseScope::PerRole::NAMES_CLASSES;
	vector<bool> kept(num_rows, true);
	if (names_classes || (node.facts && node.facts->scope.when && (diagnose || clause.skipped))) {
		kept = kept_rows();
	}
	// Its one instance, where its `when` keeps a row (in a class of its `per K:`).
	const idx_t instances = std::find(kept.begin(), kept.end(), true) != kept.end() ? 1 : 0;
	clause.skipped = clause.skipped || (names_classes && instances == 0);
	gstate.CountInstances(node.source_clause_id, instances, clause.skipped ? 0 : instances);
	// The classes its one row stands for (D28), under DIAGNOSE.
	if (diagnose) {
		clause.class_set = class_set(kept);
	}
	evaluated.composed_constraints.push_back(std::move(clause));
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 3a, assembly -- BuildSolverInput
//===--------------------------------------------------------------------===//

//! A frame read is its base decision read at another row, so its type and its box are its
//! base's (SolverInput::frame_reads): every pass that reads a term's box -- a Big-M, a row's
//! range -- then reads one that holds at every row the read lands on. Called wherever the
//! boxes are rewritten wholesale: after the floors and implied bounds, and after a diagnosis
//! widens them.
static void SyncFrameReadBoxes(SolverInput &input) {
	for (idx_t i = 0; i < input.frame_reads.size(); i++) {
		auto read = input.first_frame_read + i;
		auto base = input.frame_reads[i].base;
		input.variable_types[read] = input.variable_types[base];
		input.lower_bounds[read] = input.lower_bounds[base];
		input.upper_bounds[read] = input.upper_bounds[base];
		input.rigid_lower_bounds[read] = input.rigid_lower_bounds[base];
		input.rigid_upper_bounds[read] = input.rigid_upper_bounds[base];
	}
}

SolverInput PhysicalDecide::BuildSolverInput(DecideGlobalSinkState &gstate, idx_t num_rows,
                                             vector<KeyedMapping> keyed_mappings,
                                             vector<SolverInput::KeyValues> key_values, EvaluatedClauses &evaluated,
                                             VarIndexer &out_var_indexer) const {
	DecideProfileScope phase("assembly.var_indexer");
	idx_t num_decide_vars = decide_variables.size();

	// Construct SolverInput
	SolverInput solver_input;
	solver_input.num_rows = num_rows;
	solver_input.num_decide_vars = num_decide_vars;
	solver_input.keyed_mappings = std::move(keyed_mappings);
	solver_input.absent_reads = gstate.absent_reads;
	solver_input.variable_scopes = variable_scopes;
	// The decisions frames read at another row (SolverInput::frame_reads), after the plan's
	// own: each its base read, at each row, at the row its frame selects -- or the row
	// itself where it selects none, where every term reading it has no value or 0.
	solver_input.first_frame_read = num_decide_vars;
	if (!gstate.frame_reads.empty()) {
		solver_input.variable_scopes.resize(num_decide_vars, DecideVarScopeInfo::Row());
		solver_input.num_decide_vars = num_decide_vars + gstate.frame_reads.size();
	}
	for (auto &read : gstate.frame_reads) {
		SolverInput::FrameRead frame_read;
		frame_read.base = read.first;
		auto &map = gstate.frame_maps[read.second];
		frame_read.rows.resize(num_rows);
		for (idx_t row = 0; row < num_rows; row++) {
			auto selected = map.SelectedRow(row);
			frame_read.rows[row] = selected == DConstants::INVALID_INDEX ? row : selected;
		}
		solver_input.frame_reads.push_back(std::move(frame_read));
		solver_input.variable_scopes.push_back(
		    DecideVarScopeInfo(DecideVarScope::FRAME_READ, DConstants::INVALID_INDEX));
	}

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
		auto &decide_var = decide_variables[var]->Cast<BoundColumnRefExpression>();
		// A BOOLEAN-domain variable (declared `x(BOOL)`, or a boolean-valued
		// auxiliary such as an IN-domain/L0 indicator) is bound with an INTEGER
		// DuckDB type so it can appear in arithmetic (`M * z`, `x - v1*z1 - ...`),
		// but its solver-facing domain is BOOLEAN: `is_boolean_var` is the
		// authoritative signal, not `return_type`. Reporting it as BOOLEAN here
		// makes SolverModel::Build (ilp_model_builder.cpp) apply the `[0,1]` box
		// and mark the column binary directly from the type — the same path
		// optimizer-created BOOLEAN auxiliaries (MIN/MAX, NE indicators) already
		// use — with no constraint-tree representation of the domain at all.
		solver_input.variable_types[var] = is_boolean_var[var] ? LogicalType::BOOLEAN : decide_var.return_type;
	}
	solver_input.variable_types.resize(solver_input.num_decide_vars);

	// Bounds were folded in the gstate constructor from simple
	// `x OP const` / BETWEEN constraints; those comparisons were skipped in
	// AnalyzeConstraint so we don't re-emit them as per-row model rows.
	solver_input.constraint_sources = constraint_sources;
	solver_input.objective_written = DecideWrittenObjective(written_sense, written_objective);
	// A variable the query never explicitly lowered gets the default lower bound 0 and
	// stays non-negative. A variable with an explicit (possibly negative) lower bound
	// keeps it, and a written bound of -1e30 or below is no bound, as -1e29 is. Must
	// run before any consumer of lower_bounds (implied-bound propagation, Big-M,
	// McCormick, model builder).
	solver_input.lower_bounds.resize(gstate.singleton_row_boxes.size());
	solver_input.upper_bounds.resize(gstate.singleton_row_boxes.size());
	for (idx_t var = 0; var < gstate.singleton_row_boxes.size(); var++) {
		solver_input.lower_bounds[var] = gstate.singleton_row_boxes[var].EffectiveLower();
		solver_input.upper_bounds[var] = gstate.singleton_row_boxes[var].upper;
	}
	if (!solver_input.frame_reads.empty()) {
		solver_input.lower_bounds.resize(solver_input.num_decide_vars);
		solver_input.upper_bounds.resize(solver_input.num_decide_vars);
	}

	// Everything below this point is stage 06 working on the evaluated model, so
	// hand the constraints and the formulation links over before the first pass.
	solver_input.constraints = std::move(gstate.evaluated_constraints);
	solver_input.class_sets = std::move(gstate.class_sets);
	solver_input.key_values = std::move(key_values);
	solver_input.minmax_clause_labels = minmax_clause_labels;
	solver_input.ne_clause_sources = ne_clause_sources;
	for (auto &link : gstate.evaluated_bilinear_links) {
		solver_input.bilinear_links.push_back(BilinearLinkSpec {link.aux_idx, link.bool_var_idx, link.other_var_idx,
		                                                        link.source_clause_id, link.removal_group_id});
	}
	for (auto function : tree->function_nodes) {
		const auto &node = *tree->nodes[function];
		if (node.function_kind != DecideFunctionKind::ABS || !node.abs_needs_bigm) {
			continue;
		}
		solver_input.abs_maximize_links.push_back(AbsMaximizeLinkSpec {node.value_var, node.abs_indicator_var});
	}
	solver_input.reducer_value_links = std::move(gstate.evaluated_value_links);

	// The rigid box, captured before any propagation runs and with every user-folded
	// direction re-opened. What is left is the intrinsic domain alone — the only part of
	// the column box that survives infeasibility diagnosis unchanged, and therefore the
	// only part a structural rewrite may rely on. The re-opening mirrors the folded
	// bounds' clauses in the repair model (`singleton_row_constraints`), which relax the same
	// directions for the same reason.
	solver_input.rigid_lower_bounds = solver_input.lower_bounds;
	solver_input.rigid_upper_bounds = solver_input.upper_bounds;
	for (const auto &b : gstate.user_singleton_row_bounds) {
		idx_t v = b.decide_var_idx;
		bool bound_is_bool = is_boolean_var[v];
		if (b.sense == '<' || b.sense == '=') {
			solver_input.rigid_upper_bounds[v] = bound_is_bool ? 1.0 : NumericLimits<double>::Maximum();
		}
		if (b.sense == '>' || b.sense == '=') {
			solver_input.rigid_lower_bounds[v] = bound_is_bool ? 0.0 : -NumericLimits<double>::Maximum();
		}
	}

	// A constant lower bound under a `when` replaces the default floor of 0 on the instances its rows reach. After the
	// rigid box is captured, which it re-opens for the decisions it lowers, and before any pass reads the box.
	phase.Next("assembly.lifted_floors");
	ResolveLiftedFloors(solver_input, out_var_indexer, gstate.singleton_row_boxes);
	SyncFrameReadBoxes(solver_input);
	phase.Next("assembly.implied_bounds");

	// Data-driven implied-bound propagation: derive finite upper bounds for
	// otherwise-unbounded variables from non-negative `<=`/`=` constraints (the
	// knapsack/budget pattern), so the downstream Big-M can be finite and tight.
	// Only provably-implied bounds are applied; the feasible region is unchanged.
	// Seed reducer-value bounds first: a SUM/MAX helper over nonnegative decisions is itself nonnegative, which lets
	// this pass safely propagate a root equality's finite upper bound back through nested helpers. A product has no
	// default floor (AddBooleanProduct), so it is floored at its factor's first, once the lifted floors have lowered
	// the factors: `b * x` over x >= 0 is a non-negative term of a budget row.
	FloorBilinearAuxiliaries(solver_input);
	if (!solver_input.reducer_value_links.empty()) {
		DeriveReducerValueBounds(solver_input, out_var_indexer);
	}
	DecidePropagateImpliedBounds(solver_input, out_var_indexer, num_rows);
	SyncFrameReadBoxes(solver_input);
	phase.Next("assembly.objective_and_avg");
	// Objective (linear part)
	solver_input.objective_coefficients = std::move(gstate.evaluated_objective_coefficients);
	solver_input.objective_variable_indices = std::move(gstate.objective_variable_indices);
	solver_input.objective_constant = objective_constant_offset;

	solver_input.objective_product_groups = std::move(gstate.evaluated_objective_groups);

	// The helpers written where they are read own no column (stage 06, D89): decided here, on the assembled rows and
	// objective. The floors and implied bounds above read the helpers' columns and keep only what holds of a decision
	// or an instance, so the column space is laid out again, and every row read against the old one is read anew.
	phase.Next("assembly.inline_helpers");
	// What reads a decision's column outside the rows and the objective assembled here: a composed MIN/MAX term, a
	// bound folded into its column's box, and every term of a flat MIN/MAX objective, which its auxiliary pins.
	vector<idx_t> held;
	for (auto &clause : evaluated.composed_constraints) {
		for (auto &term : clause.terms) {
			held.insert(held.end(), term.variables.begin(), term.variables.end());
		}
	}
	for (auto &term : evaluated.composed_objective_terms) {
		held.insert(held.end(), term.variables.begin(), term.variables.end());
	}
	for (auto &bound : gstate.user_singleton_row_bounds) {
		held.push_back(bound.decide_var_idx);
	}
	if (flat_objective_agg != ObjectiveAggregateType::NONE) {
		held.insert(held.end(), solver_input.objective_variable_indices.begin(),
		            solver_input.objective_variable_indices.end());
	}
	vector<idx_t> minmax_values;
	for (auto &link : solver_input.reducer_value_links) {
		if (link.agg_name == "min" || link.agg_name == "max") {
			minmax_values.push_back(link.value_var);
		}
	}
	if (InlineReducerValueLinks(solver_input, held, minmax_values)) {
		ReportInlinedHelpers(solver_input);
		out_var_indexer = VarIndexer::Build(solver_input);
		for (auto &ec : solver_input.constraints) {
			ec.emitted_rows = EmittedRows();
		}
	}
	return solver_input;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 3b, formulation -- FormulateModel
//===--------------------------------------------------------------------===//

void PhysicalDecide::FormulateModel(SolverInput &solver_input, const FormulationBox &box, VarIndexer &var_indexer,
                                    const EvaluatedClauses &evaluated) const {
	DecideProfileScope phase("formulation.l0_constants");
	idx_t num_rows = solver_input.num_rows;

	// The flat column space is settled by assembly and is never rebuilt here: the row,
	// keyed and global blocks are final, and only the global block grows as auxiliaries
	// are appended below. That is what lets a second formulation, against a different
	// box, name exactly the same columns as the first — which the elastic path relies on
	// to read its solution back through the same labels.
#ifdef DEBUG
	const idx_t formulation_entry_columns = var_indexer.global_block_start;
#endif

	// Plain declared names, for the refusals the linearization can raise. `alias`
	// lives on BaseExpression, so no cast is needed to read it.
	vector<string> decide_var_names;
	decide_var_names.reserve(solver_input.num_decide_vars);
	for (auto &v : decide_variables) {
		decide_var_names.push_back(v->alias);
	}
	// A frame read is named as its base is: the bound a refusal asks for is the base's.
	for (auto &read : solver_input.frame_reads) {
		decide_var_names.push_back(decide_var_names[read.base]);
	}

	// Auto-M for L0 `norm(e, 0)`: the binder emitted the raw links `e <= M*z` and
	// `-e <= M*z` with a placeholder coefficient on each `__l0auto_ind_*` indicator.
	// Now that implied bounds are known, fill a tight, data-driven Big-M (mirrors
	// the <> path): zero the indicator's own coefficient, compute M from the
	// remaining (inner-expression) terms + RHS, then set the indicator coefficient
	// to -M. The links carry a negative indicator coefficient, so the implied-bound
	// propagation assembly ran already skipped them (no contamination from the
	// placeholder). This is a constant derived from `box`, which is why it is here and
	// not with the propagation: it has to be re-derived whenever the box changes.
	{
		unordered_set<idx_t> l0_auto;
		for (idx_t i = 0; i < decide_variables.size(); i++) {
			if (StringUtil::StartsWith(decide_variables[i]->GetName(), "__l0auto_ind_")) {
				l0_auto.insert(i);
			}
		}
		if (!l0_auto.empty()) {
			for (auto &ec : solver_input.constraints) {
				// Locate the auto-L0 indicator term, and confirm this is a LINK
				// (it also carries the inner expression's variable) rather than the
				// indicator's own 0<=z<=1 bound row — which we must not rewrite.
				idx_t z_term = DConstants::INVALID_INDEX;
				bool has_inner_var = false;
				for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
					idx_t v = ec.variable_indices[t];
					if (v == DConstants::INVALID_INDEX) {
						continue;
					}
					if (l0_auto.count(v)) {
						z_term = t;
					} else if (!StringUtil::StartsWith(decide_variables[solver_input.BaseVariable(v)]->GetName(),
					                                   "__abs_aux_")) {
						// The REVERSE link `ABS(inner) >= TOL*z` also references the L0
						// indicator, but its only other variable is the ABS aux and its
						// z-coefficient is the fixed tolerance — it must NOT be refilled.
						// Only the FORWARD links carry the inner decision variable, so an
						// ABS-aux companion marks the reverse link to skip.
						has_inner_var = true;
					}
				}
				if (z_term == DConstants::INVALID_INDEX || !has_inner_var) {
					continue;
				}
				// Zero the indicator's own contribution, compute the tight Big-M from
				// the remaining (inner) terms + RHS, then set the indicator coeff = +M.
				// The link is normalized to `M*z - inner >= rhs`, so the indicator
				// coefficient is positive.
				ec.row_coefficients[z_term].AssignScalar(num_rows, 0.0);
				double M =
				    DecideTightPerRowBigM(ec, box, num_rows, decide_var_names, InstanceBox {solver_input, var_indexer},
				                          DecideWrittenClause(solver_input, ec.source_clause_id), "norm(e, 0)");
				ec.row_coefficients[z_term].AssignScalar(num_rows, M);
			}
		}
	}

	// THE ROUTING, as stage 05 decided it. Whether ABS is stated natively or lowered is
	// a FORMULATION choice, and formulation belongs to stage 05 — so this reads the
	// decision off the plan rather than asking a backend. Native means no Big-M and
	// therefore no bound requirement, so the answer changes what is refused, not only
	// what is fast; that is exactly why it has to be the same answer the rewrites above
	// were selected against. Both arms below only translate.
	const bool native_abs = use_native_constructs.abs;
	// MIN/MAX arrives as a POLICY rather than an answer. Stage 05 decided both halves of
	// it — whether the backend can state the construct, and whether a declared construct
	// is used everywhere or only as a fallback — and neither is re-decided here. What is
	// left is a question about the data: whether THIS clause has a Big-M, which only a
	// stage that has seen the evaluated coefficients can answer. Applying a decision to
	// data is this layer's job; choosing the decision was not.
	const NativeConstructPolicy native_min_max {use_native_constructs.min_max, force_native_constructs};

	// A bilinear auxiliary may itself sit inside ABS (notably the reverse link of an
	// L0 norm). Temporarily expose its exact product box while descendant constructs
	// derive their own ranges. Restore the declared column box before McCormick emission
	// so ordinary solve models keep their existing, row-enforced auxiliary bounds.
	vector<pair<idx_t, pair<double, double>>> bilinear_declared_boxes;
	bilinear_declared_boxes.reserve(solver_input.bilinear_links.size());
	for (const auto &link : solver_input.bilinear_links) {
		bilinear_declared_boxes.push_back(
		    {link.aux_idx, {solver_input.lower_bounds[link.aux_idx], solver_input.upper_bounds[link.aux_idx]}});
	}
	// A factor that is a helper decision of the value rewrite (`b * sum(x)`) has no box until its members' boxes
	// are derived below, so with helpers the refusal waits for them.
	DeriveBilinearAuxiliaryBounds(solver_input, box, decide_var_names,
	                              /*refuse_when_unbounded=*/solver_input.reducer_value_links.empty());

	// ABS FIRST, and the order is load-bearing. Deriving an ABS auxiliary's range is
	// also what boxes its column, and every linearizer below computes its Big-M from
	// column boxes — run them first and an outer MIN/MAX or `<>` over ABS(...) sees an
	// unbounded column and refuses a query whose bound was there to be computed. Only
	// the LOWERING path refuses an underivable range; the native path leaves the
	// auxiliary open and answers.
	phase.Next("formulation.abs_bounds");
	if (solver_input.reducer_value_links.empty()) {
		DeriveAbsAuxiliaryBounds(solver_input, var_indexer, decide_var_names, !native_abs);
	} else {
		// The value rewrite's helper decisions are boxed by their members' reach, and a
		// member may be an ABS auxiliary or a product with a helper (`b * sum(x)`) while an
		// ABS may hold a helper: each box is the other's input. A helper's box also bounds
		// the helpers it is defined from (DeriveReducerMemberBounds). Every derivation only
		// tightens, so alternating them settles in as many passes as the constructs nest. The
		// refusals are decided last, on the settled boxes, naming a helper by its reducer.
		for (auto &link : solver_input.reducer_value_links) {
			decide_var_names[link.value_var] = link.label;
		}
		const idx_t max_passes = 2 * (solver_input.abs_maximize_links.size() + solver_input.bilinear_links.size() +
		                              solver_input.reducer_value_links.size() + 1);
		for (idx_t pass = 0;; pass++) {
			auto lower = solver_input.lower_bounds;
			auto upper = solver_input.upper_bounds;
			auto reach_lower = solver_input.instance_reach_lower;
			auto reach_upper = solver_input.instance_reach_upper;
			DeriveBilinearAuxiliaryBounds(solver_input, box, decide_var_names, /*refuse_when_unbounded=*/false);
			DeriveAbsAuxiliaryBounds(solver_input, var_indexer, decide_var_names, /*refuse_when_unbounded=*/false);
			DeriveReducerValueBounds(solver_input, var_indexer);
			DeriveReducerMemberBounds(solver_input, var_indexer);
			if (lower == solver_input.lower_bounds && upper == solver_input.upper_bounds &&
			    reach_lower == solver_input.instance_reach_lower && reach_upper == solver_input.instance_reach_upper) {
				break;
			}
			// Each pass leaves valid boxes, only tighter ones: one that still moves after as many passes as the
			// constructs nest is closing in by ever smaller steps, so the boxes stand as they are. Clauses that
			// contradict each other make a box cross, and from there each pass pushes its ends further apart: the
			// passes still bound every helper a later one reads, and the solver reports the crossed box as for any
			// empty one (DIAGNOSE formulates its own).
			if (pass == max_passes) {
				break;
			}
		}
		DeriveBilinearAuxiliaryBounds(solver_input, box, decide_var_names);
		DeriveAbsAuxiliaryBounds(solver_input, var_indexer, decide_var_names, !native_abs);
	}
	if (native_abs) {
		EmitNativeAbs(solver_input, var_indexer);
	} else {
		LinearizeAbsMaximize(solver_input);
	}

	// The helper decisions' definitions: an equality row per group for SUM/AVG, an
	// extremum link per group for MIN/MAX. A refusal names a helper by its reducer.
	phase.Next("formulation.value_links");
	LinearizeReducerValueLinks(solver_input, var_indexer, decide_var_names, native_min_max);

	// Encode every hard MIN/MAX constraint stage 05 tagged. Native states `z = MAX(t..)`
	// directly and needs no Big-M, so it also needs no bound on the contributing
	// variables; the lowering arm's indicator family does. Both read the same tag and
	// make the same bound classification first.
	if (!minmax_clause_labels.empty()) {
		LinearizeMinMaxConstraints(solver_input, var_indexer, decide_var_names, native_min_max);
	}

	// Encode `<>` as the disjunction it is: two conditional rows per instance. Whether a
	// condition is stated to the backend or encoded with a Big-M is settled once, by
	// LowerDecideConstructs at the end of this function.
	phase.Next("formulation.not_equal");
	if (!ne_clause_sources.empty()) {
		LinearizeNotEqual(solver_input, var_indexer, decide_var_names);
	}

	// Emit the McCormick envelope for every bilinear w = b * x auxiliary.
	for (const auto &saved : bilinear_declared_boxes) {
		solver_input.lower_bounds[saved.first] = saved.second.first;
		solver_input.upper_bounds[saved.first] = saved.second.second;
	}
	phase.Next("formulation.bilinear");
	LinearizeBilinear(solver_input, var_indexer, box);

	// Handle MIN/MAX objective: create global auxiliary variable z and linking constraints.
	// Easy (no indicators): MINIMIZE+MAX or MAXIMIZE+MIN; hard (Big-M indicators):
	// MINIMIZE+MIN or MAXIMIZE+MAX. A nested one is a helper decision, defined with the
	// other helpers (D81).

	// Global variables are appended at var_indexer.global_block_start.
	// As we add more global vars, their indices are global_block_start + g
	// where g is the position in the global vars array.

	// Encode a MIN/MAX objective (`MIN(expr)`/`MAX(expr)`) into a global auxiliary and its
	// envelope/indicator rows. Pure stage-06 work: it reads the coefficients PHASE 2
	// evaluated plus the flat column space, and needs no row of its own.
	phase.Next("formulation.minmax_objective");
	MinMaxObjectiveSpec minmax_objective_spec;
	minmax_objective_spec.flat_agg = flat_objective_agg;
	minmax_objective_spec.flat_is_easy = flat_objective_is_easy;
	{
		// The rows the objective's reducer reads -- its terms' filters, the objective's
		// `when` folded in -- are its members, whatever their coefficients.
		auto &members = minmax_objective_spec.member_rows;
		members.assign(num_rows, evaluated.linear_filters.empty());
		for (auto &filter : evaluated.linear_filters) {
			for (idx_t row = 0; row < num_rows; row++) {
				members[row] = members[row] || !filter.has_filter || filter.mask[row];
			}
		}
	}
	LinearizeMinMaxObjective(solver_input, var_indexer, minmax_objective_spec, decide_var_names, native_min_max);

	// Composed MIN/MAX clauses. PHASE 2 evaluated each one; stage 06 owns the auxiliary,
	// envelope and indicator emission. The linearizer fills each MIN/MAX term's `z_idx`,
	// so it is handed a mutable copy — the evaluated clauses stay reusable for a second
	// formulation against a different box.
	phase.Next("formulation.composed_minmax");
	for (idx_t i = 0; i < evaluated.composed_constraints.size(); i++) {
		auto terms = evaluated.composed_constraints[i].terms;
		auto &clause = evaluated.composed_constraints[i];
		if (clause.skipped) {
			continue;
		}
		LinearizeComposedMinMaxConstraint(solver_input, var_indexer, terms, clause.rhs, clause.comparison,
		                                  clause.source_clause_id, clause.removal_group_id, clause.repair_group_id,
		                                  clause.class_set, decide_var_names, native_min_max);
	}
	if (!evaluated.composed_objective_terms.empty()) {
		auto terms = evaluated.composed_objective_terms;
		LinearizeComposedMinMaxObjective(solver_input, var_indexer, terms, decide_var_names, native_min_max);
	}

	// Refresh total_vars: the row/keyed blocks were finalized before linearization,
	// but global aux vars were appended by the construct sites above.
	var_indexer.total_vars = var_indexer.global_block_start + solver_input.num_global_vars;

	// THE LOWERING, and the last pass of stage 06. Every site above emitted the semantic
	// form of its construct and nothing else; this is where the constructs the chosen
	// backend cannot state become ordinary rows. It runs last because it needs every
	// column box final — a Big-M reads them, and the sites above are what narrow them.
	//
	// What it reads is the answer stage 05 recorded on the plan, carried here as a value.
	// No backend is consulted at execution time, and nothing here re-decides a
	// formulation: a pass that answered differently from the rewrites would run a model
	// on a solver it was not built for.
	phase.Next("formulation.lower_constructs");
	LowerDecideConstructs(solver_input, var_indexer, box, decide_var_names, use_native_constructs);

#ifdef DEBUG
	// No decide variable was added: only the global block grew.
	D_ASSERT(var_indexer.global_block_start == formulation_entry_columns);
#endif
}

//===--------------------------------------------------------------------===//
// The elastic column box
//===--------------------------------------------------------------------===//

//! Widen every column direction a diagnosis is allowed to loosen, so each is enforced
//! only by the loosenable row emitted beside it rather than by a bound that cannot move.
//!
//! The solved box carries three things (see `SolverInput::rigid_lower_bounds`): the
//! intrinsic domain, user bounds stage 05 folded, and implied tightenings
//! `DecidePropagateImpliedBounds` derived. The last two are each backed by a row the
//! elastic engine may loosen, so leaving them in place would pin the loosenable row behind
//! a bound that cannot move — the engine would report an edit that changes nothing.
//! `rigid_*` is exactly the intrinsic domain, which is what makes the test "is this
//! direction narrower than rigid?" the same as "is a loosenable row holding it?":
//! propagation only ever narrows, and stage 05's folding is re-opened in the snapshot.
//!
//! The widening is bounded, and it is bounded RELATIVE TO EACH COLUMN. Opening a direction
//! all the way is not an option: an unbounded column has no finite Big-M, so a formulation
//! against a fully open box does not lose precision, it refuses the query ("Bilinear term
//! requires a finite upper bound on variable 'x'"). But the amount to open by has to come
//! from the column itself and never from the model as a whole. One model-wide number
//! couples every column to the largest value anywhere in the query — a price-weighted row
//! drags an unrelated `x <= 5` out to 1e15, the Big-M derived from it follows, and the
//! elastic model stops being solvable for reasons that have nothing to do with `x`.
//! Scaling each column from its own bounds and row demands prevents an unrelated large
//! value elsewhere in the model from inflating its constants. A row may still require a
//! much larger widening than the column's current magnitude; that coupling is local and
//! semantic rather than model-wide.
static void OpenElasticColumnBox(SolverInput &input) {
	//! Headroom every loosenable direction gets, as a multiple of the column's own
	//! magnitude. It is the floor, not the whole answer: it covers a column whose repair
	//! target is not visible in any row — the bound behind an `ABS(x) >= 5`, where the
	//! rows are the construct's own links and carry no target of their own.
	constexpr double ELASTIC_WIDENING = 10.0;
	//! A coefficient at or below the solver's own small-value threshold is noise, and
	//! dividing by it would manufacture a demand of 1e18 from a rounding artefact.
	constexpr double NEGLIGIBLE_COEFFICIENT = 1e-9;

	// What each column would have to reach to satisfy a row on its own. `SUM(y) >= 100`
	// says a repair may need `y` out at 100, however small `y <= 2` looks beside it — and
	// that is the difference between reporting one edit the user can apply and reporting
	// two because the first could not go far enough. Divided by the coefficient, so a row
	// measured in price-weighted units states its demand in the column's units and not in
	// its own.
	vector<double> row_demand(input.num_decide_vars, 0.0);
	for (const auto &ec : input.constraints) {
		for (idx_t r = 0; r < input.num_rows; r++) {
			if (!ec.row_group_ids.empty() && ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
				continue;
			}
			double rhs = ec.rhs_values.IsUniform() ? ec.rhs_values.UniformValue() : ec.rhs_values.Get(r);
			if (!std::isfinite(rhs) || rhs == 0.0) {
				continue;
			}
			for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
				idx_t v = ec.variable_indices[t];
				if (v == DConstants::INVALID_INDEX) {
					continue;
				}
				double coef = std::abs(ec.row_coefficients[t].Get(r));
				if (coef <= NEGLIGIBLE_COEFFICIENT) {
					continue;
				}
				double demand = std::abs(rhs) / coef;
				if (std::isfinite(demand)) {
					row_demand[v] = std::max(row_demand[v], demand);
				}
			}
		}
	}

	for (idx_t var = 0; var < input.num_decide_vars; var++) {
		// A BOOLEAN's 0/1 domain is intrinsic and never widens: a "repair" that let an
		// indicator sit at 1.4 would be a repair of the encoding, not of the query. A pin
		// the user wrote on one is still loosenable — that direction is narrower than
		// rigid, so the clamp lands it back on the domain edge, exactly where the
		// re-emitted row takes over as its enforcer.
		bool is_bool = input.variable_types[var] == LogicalType::BOOLEAN;
		double rigid_lower = is_bool ? std::max(input.rigid_lower_bounds[var], 0.0) : input.rigid_lower_bounds[var];
		double rigid_upper = is_bool ? std::min(input.rigid_upper_bounds[var], 1.0) : input.rigid_upper_bounds[var];
		// The column's own magnitude, from whichever of its bounds are finite. Floored at
		// one so a column pinned at zero still has somewhere to move.
		double magnitude = 1.0;
		if (std::abs(input.lower_bounds[var]) < DECIDE_EFFECTIVE_INFINITY) {
			magnitude = std::max(magnitude, std::abs(input.lower_bounds[var]));
		}
		if (std::abs(input.upper_bounds[var]) < DECIDE_EFFECTIVE_INFINITY) {
			magnitude = std::max(magnitude, std::abs(input.upper_bounds[var]));
		}
		double room = std::max(ELASTIC_WIDENING * magnitude, row_demand[var]);
		if (input.lower_bounds[var] > rigid_lower) {
			input.lower_bounds[var] = std::max(rigid_lower, input.lower_bounds[var] - room);
		}
		if (input.upper_bounds[var] < rigid_upper) {
			input.upper_bounds[var] = std::min(rigid_upper, input.upper_bounds[var] + room);
		}
	}
	SyncFrameReadBoxes(input);
}

//===--------------------------------------------------------------------===//
// The elastic model, as a formulation
//===--------------------------------------------------------------------===//

//! Formulate a second model against the widened box.
//!
//! A Big-M or a McCormick envelope is sound only for the box it was derived from, so an
//! elastic model that re-opens the box must re-derive them. That is why this runs the
//! same PHASE 3b pass the solved model went through — `FormulateModel` is a pure
//! function of the evaluated clauses plus a box, which is what makes running it twice
//! meaningful rather than merely possible.
//!
//! Each call starts from a fresh copy of `elastic`, so no rows or auxiliaries from the
//! solved formulation can leak into the repair formulation.
SolverModel PhysicalDecide::FormulateElasticModel(const ElasticFormulation &elastic, const EvaluatedClauses &evaluated,
                                                  VarIndexer &out_var_indexer,
                                                  vector<string> &out_global_labels) const {
	SolverInput input = elastic.input;
	VarIndexer indexer = elastic.var_indexer;
	input.keep_singleton_rows = true;

	OpenElasticColumnBox(input);
	FormulateModel(input, FormulationBox::OfSolvedModel(input), indexer, evaluated);

	// Same reconciliation the solved model gets before its build: the `<>` label channel
	// is a prefix of the global block, padded to its final length.
	D_ASSERT(input.global_variable_labels.size() <= input.num_global_vars);
	input.global_variable_labels.resize(input.num_global_vars);
	out_global_labels = input.global_variable_labels;

	// The decide-variable columns are settled by assembly and shared with the solved
	// model, so a repair reads back through the same labels. Only the global block may
	// differ, and only in length.
	D_ASSERT(indexer.global_block_start == elastic.var_indexer.global_block_start);

	SolverModel model = SolverModel::Build(input, indexer);
	out_var_indexer = std::move(indexer);
	return model;
}

//===--------------------------------------------------------------------===//
// Finalize: PHASE 3, solve + readback -- FinalizeSolveResult
//===--------------------------------------------------------------------===//

SinkFinalizeType PhysicalDecide::FinalizeSolveResult(ClientContext &context, DecideGlobalSinkState &gstate,
                                                     SolverInput &solver_input, VarIndexer &var_indexer,
                                                     const EvaluatedClauses &evaluated, ElasticFormulation *elastic,
                                                     bool bench, Profiler &model_timer, Profiler &solver_timer) const {
	idx_t num_rows = solver_input.num_rows;

	// Capture model size before solve (solver may move data)
	size_t bench_total_vars = var_indexer.total_vars;
	// Clause-level model size: the number of constraint *specs* the operator hands the
	// builder. A per-row spec still expands to one row per data row and a grouped spec to one
	// row per group, so this is not a matrix row count — the row count is read back off
	// the built model (`solve_result.model_constraint_rows`) after the solve below.
	size_t bench_constraint_specs = solver_input.constraints.size() + solver_input.global_constraints.size();

	if (bench) {
		model_timer.End();
		solver_timer.Start();
	}

	// H3: the statement's DIAGNOSE prefix, and nothing else, arms diagnosis. Under it,
	// pre-extract the unbounded ray so it is ready if the solve turns out unbounded, and
	// retain the built model so the INFEASIBLE terminal can hand it to the elastic
	// engine. An unprefixed query pays for neither, and both failure terminals are
	// prefix-only anyway (RouteSolveResult).
	bool diagnosis_armed = diagnose;
	// One warning latch for the whole query: every DECIDE node, direct plans included, and under DIAGNOSE the repair
	// model share it.
	auto &number_warning = DecideNumberWarningState::Of(context);
	SolveModelOptions solve_options;
	solve_options.extract_unbounded_ray = diagnosis_armed;
	solve_options.say_number_warning = [&number_warning]() {
		number_warning.Say();
	};
	// Keep an inverted column box (col_lower > col_upper) alive through Build under
	// diagnosis so the INFEASIBLE terminal can reset it to the intrinsic domain and
	// diagnose it (Bug 1, all-column-bound conflicts). An unprefixed query keeps the
	// fast throw.
	solver_input.keep_singleton_rows = diagnosis_armed;

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
	// The warm solver is retained only where a continuation can actually be asked for.
	bool want_session = can_prompt;
	// The first solve chunk uses the same per-chunk budget every Continue() chunk will,
	// so elapsed climbs in even multiples (300s, 600s, ...) across the loop.
	solve_options.time_limit_seconds = ResolveDecideTimeLimit();
	solve_options.mip_gap = GetDecideMipGap(context);
	// Ctrl-C cuts the solve short and returns the best-so-far instead of aborting the
	// query — also ordinary execution behaviour, so the poll is installed unconditionally.
	// Both backends honor it mid-solve.
	solve_options.interrupt_poll = [&context]() {
		return context.interrupted.load();
	};
	SolveModelOptions diagnostic_solve_options;
	diagnostic_solve_options.time_limit_seconds = ResolveDecideDiagnosticTimeLimit(solve_options.time_limit_seconds);
	diagnostic_solve_options.interrupt_poll = solve_options.interrupt_poll;

	// Reconcile the `<>` label channel with the final global-var count. Only the
	// aggregate-`<>` site labels its global z (for the infeasible removal dial),
	// and those z's are allocated first (the deferred-NE loop runs before all
	// MIN/MAX linking), so the labels form a contiguous prefix; pad the trailing
	// unlabeled MIN/MAX globals with "" to keep the vector parallel with
	// global_variable_types. (Invariant: never shrinks — only labeled globals
	// precede the resize point.)
	D_ASSERT(solver_input.global_variable_labels.size() <= solver_input.num_global_vars);
	solver_input.global_variable_labels.resize(solver_input.num_global_vars);

	// Retained only when diagnosis is armed; the move is trivial and the model is
	// freed when Finalize returns. SolveModel otherwise discards the built model.
	SolverModel retained_model;
	// Retained only when a continue-capable timeout mode is active: the live warm
	// solver, so the TIME_LIMIT terminal can Continue() it for more wall-clock.
	unique_ptr<SolverSession> retained_session;
	// Always-on wall-clock around the solve (independent of DECIDB_BENCH): the slow
	// checkpoint report (S2) needs the elapsed solve time to show the user.
	Profiler solve_wall_timer;
	solve_wall_timer.Start();
	SolverResult solve_result =
	    SolveModel(solver_input, var_indexer, PlannedSolverBackend(), solve_options,
	               diagnosis_armed ? &retained_model : nullptr, want_session ? &retained_session : nullptr);
	solve_wall_timer.End();

	// Per-decide-variable labels + is-aux flags for column provenance. Both failure
	// terminals need them (UNBOUNDED names the runaway variable; INFEASIBLE carries
	// them into the elastic engine), so build them once here; invoked lazily inside a
	// failing arm so the SOLVED path pays nothing.
	auto build_var_labels = [&](vector<string> &var_labels, vector<bool> &var_is_aux) {
		idx_t nvars = decide_variables.size();
		var_labels.assign(nvars, string());
		var_is_aux.assign(nvars, false);
		for (auto &ae : aux_var_expressions) {
			var_labels[ae.first] = ae.second;
			var_is_aux[ae.first] = true;
		}
		for (idx_t i = 0; i < nvars; i++) {
			if (!var_is_aux[i]) {
				var_labels[i] = decide_variables[i]->GetName();
			}
		}
		// A frame read is labelled as its base is.
		for (auto &read : solver_input.frame_reads) {
			var_labels.push_back(var_labels[read.base]);
			var_is_aux.push_back(var_is_aux[read.base]);
		}
	};

	// Under DIAGNOSE, a failure is the ANSWER, not an error: hand off the findings to the
	// DIAGNOSE operator above and return no rows. Without the prefix the same failure
	// raises, naming its state and nothing more — naming the clause *is* the elastic
	// solve, and that only happens when the user asks for it.
	auto report = [&](DecideDiagnostic diag) {
		D_ASSERT(diagnosis_armed);
		StashDecideDiagnostic(context, std::move(diag));
		gstate.diagnosis_only = true;
	};

	// H5: a time-limit stop (or a Ctrl-C, which arrives as one) is ordinary execution
	// behaviour and is handled here, before the router sees the result — with or without
	// the prefix. It either delivers an incumbent, resumes to a proven optimum, or
	// raises; it never diagnoses.
	bool deliver_incumbent = false;
	if (solve_result.status == SolverStatus::TIME_LIMIT) {
		// Before anything is printed: did another operator in this same query already
		// fail? A SELECT can hold several DECIDE clauses — two subqueries joined side
		// by side, say — and when one of them throws, DuckDB stops the rest by setting
		// the very query interrupt Ctrl-C sets (`Executor::PushError`). Every checkpoint
		// test below reads that interrupt, so this solve would be reported as a
		// cancellation the user never asked for, printed *above* the error that actually
		// stopped the query, with elapsed/memory figures describing a solve that was
		// never the problem. Say nothing and re-raise the real failure instead.
		auto &executor = Executor::Get(context);
		if (executor.HasError()) {
			executor.ThrowException();
		}
		// S2/S3/S4: at each chunk boundary print the checkpoint report (what was found
		// + how far it can still improve + elapsed/memory), then offer the choice: keep
		// solving, or take the incumbent with its gap to best. "Keep solving" re-runs
		// the SAME warm solver (retained_session) for another fresh chunk — the MIP
		// search resumes, elapsed accumulates. On stop, a usable incumbent is returned
		// as a SUCCESSFUL result (with a plain stderr caveat that it is not proven
		// best); no incumbent falls to the plain timeout error.
		double chunk = ResolveDecideTimeLimit();
		double cum_elapsed = solve_wall_timer.Elapsed();
		double cum_budget = chunk;

		// Did the user stop this, or the clock? `user_interrupted` is the backend's own
		// attribution (both backends cut the solve short mid-search and say so); a Ctrl-C
		// that lands after the solver's last check arrives indistinguishable from a
		// timeout. The query interrupt is set either way, so read it too. The answer is
		// written back into the result once, so the report and the error below read the
		// same decision.
		bool stopped_by_user = solve_result.user_interrupted || context.interrupted.load();
		solve_result.user_interrupted = stopped_by_user;
		if (stopped_by_user) {
			// Ctrl-C is not a question. The user has already said stop, so there is
			// nothing to offer: hand back the best-so-far, with the caveat that it is not
			// proven best. This is the one stop that does not need a terminal — the
			// decision was made at the keyboard, not in an answer to a prompt. With
			// nothing found there is nothing to hand back: the query was cancelled, so
			// it fails as DuckDB's own interrupt does, with one message.
			if (!solve_result.has_solution) {
				throw Exception(ExceptionType::INTERRUPT, "DECIDE stopped at your request before finding a solution.");
			}
			PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget, written_sense == DecideSense::MAXIMIZE);
			fprintf(stderr,
			        "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n");
			context.interrupted = false;
			deliver_incumbent = true;
		} else if (!can_prompt) {
			// The clock ran out and nobody is at the keyboard (tests, benchmarks, `-c`
			// pipes, the C API), so there is no one to answer the offer. Print the
			// checkpoint once and raise: never prompt where no one can answer.
			PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget, written_sense == DecideSense::MAXIMIZE);
			ThrowDecideSolveError(solve_result, PlannedSolverBackend());
		} else {
			// The interrupt poll installed on the first solve persists on the retained
			// session into every Continue() chunk, but the prompt below already stops the
			// user at each boundary, and a watcher thread contending with the interactive
			// getline destabilizes the prompt (found empirically) — so reset it to
			// boundary-only here. The entry interrupt already fired *before* this loop, so a
			// first-solve Ctrl-C still routed us in.
			if (retained_session) {
				retained_session->SetInterruptPoll({});
			}

			// Report at each boundary, then continue or stop.
			for (;;) {
				PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget, written_sense == DecideSense::MAXIMIZE);

				// stdin is a terminal (can_prompt guaranteed this), so it is safe to block
				// reading the user's decision. The CLI's line editor is inactive
				// mid-execution, so a plain getline does not fight it.
				if (solve_result.has_solution) {
					fprintf(stderr,
					        "Keep improving it?  [Enter] continue +%s  ·  s + Enter to stop and take this solution: ",
					        FormatDuration(chunk).c_str());
				} else {
					fprintf(stderr, "Keep searching?  [Enter] continue +%s  ·  s + Enter to give up: ",
					        FormatDuration(chunk).c_str());
				}
				fflush(stderr);
				bool go;
				string line;
				if (!std::getline(std::cin, line)) {
					go = false; // EOF (Ctrl-D / closed input) → stop
				} else {
					StringUtil::Trim(line);
					go = StringUtil::Lower(line) != "s";
				}
				if (!go) {
					break;
				}

				// Resume the warm solver for another chunk (warm start is automatic — the
				// solver never left scope).
				Profiler chunk_timer;
				chunk_timer.Start();
				solve_result = retained_session->Continue(chunk);
				chunk_timer.End();
				cum_elapsed += chunk_timer.Elapsed();
				cum_budget += chunk;

				if (solve_result.status == SolverStatus::OPTIMAL) {
					break; // proven optimum — fall through to the shared success stores
				}
				if (solve_result.status != SolverStatus::TIME_LIMIT) {
					// A resume can only reach a definitive INFEASIBLE/UNBOUNDED when no
					// incumbent ever existed (an incumbent proves feasibility); surface it
					// as the plain solver error.
					ThrowDecideSolveError(solve_result, PlannedSolverBackend());
				}
				// else: another time-limit stop — loop and re-report with fresh numbers.
			}

			// Loop exited on a stop decision or an OPTIMAL break.
			if (solve_result.status == SolverStatus::OPTIMAL || solve_result.has_solution) {
				if (solve_result.status != SolverStatus::OPTIMAL) {
					// Stopped early with a usable-but-unproven incumbent: succeed and
					// return it, but say plainly it is not proven best. There is no live
					// SQL NOTICE channel, so this rides on stderr with the checkpoint
					// report, which already printed the gap to best.
					fprintf(
					    stderr,
					    "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n");
					deliver_incumbent = true;
				}
				// If a Ctrl-C broke the loop, the interrupt is now handled (we stopped
				// solving and have a result to return); clear it so the rows flow to the
				// client instead of the executor aborting the query. Covers both the
				// incumbent stop and the rare "Ctrl-C landed as the optimum was proven".
				context.interrupted = false;
			} else {
				// Stopped with nothing found yet: the plain timeout error, same as the
				// non-interactive exit above.
				ThrowDecideSolveError(solve_result, PlannedSolverBackend());
			}
		}
	}

	// Route what is left to its diagnosis terminal. RouteSolveResult is a pure
	// classifier (status + armed → terminal); the operator owns the engine call, the
	// statement-scoped handoff, and the throw for each terminal.
	switch (deliver_incumbent ? DiagnosisTerminal::SOLVED : RouteSolveResult(solve_result, diagnosis_armed)) {
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
			// on the env var so user-facing output is unchanged — same pattern as
			// DECIDB_BENCH instrumentation.
			if (std::getenv("DECIDB_STATUS_MARKERS")) {
				fprintf(stderr, "DECIDB_STATUS: SUBOPTIMAL\n");
			}
		}
		break; // fall through to the success stores below
	case DiagnosisTerminal::UNBOUNDED: {
		vector<string> var_labels;
		vector<bool> var_is_aux;
		build_var_labels(var_labels, var_is_aux);

		auto diag_params = GetDecideDiagnosticParams(context);
		// The Finalize-local keyed mappings were moved into the solver input, so the provider reads the live ones
		// off var_indexer.
		UnboundedDiagnosisInput diag_input {
		    solve_result,
		    var_indexer,
		    var_labels,
		    var_is_aux,
		    diag_params,
		    UnboundedCandidateProvider {context, gstate.data, gstate.data.Types(), diag_params, input_column_names,
		                                input_column_in_clause, var_indexer, var_indexer.keyed_mappings, num_rows},
		};
		DecideDiagnostic diag = DiagnoseUnbounded(diag_input);
		// A valid diagnosis holds a finding (the builders in decide_diagnostic.cpp).
		if (diag.valid) {
			report(std::move(diag));
			break;
		}
		// The engine ran but named no variable. Say why rather than reporting nothing:
		// an empty ray is a non-linear limitation only when the retained model is
		// actually non-linear; otherwise it is a neutral "could not identify". A present
		// ray that named nothing means only internal auxiliaries escaped.
		bool has_nonlinear_terms = retained_model.has_quadratic_obj || !retained_model.quadratic_constraints.empty() ||
		                           !retained_model.general_constraints.empty() ||
		                           !retained_model.indicator_constraints.empty();
		string reason = BuildUnboundedDiagnosisUnavailableReason(solve_result.diagnostic_timed_out,
		                                                         solve_result.ray.empty(), has_nonlinear_terms);
		report(BuildUndiagnosedDiagnostic(
		    solve_result.status == SolverStatus::INF_OR_UNBD ? "infeasible or unbounded" : "unbounded", reason));
		break;
	}
	case DiagnosisTerminal::INFEASIBLE: {
		// A residual INF_OR_UNBD is routed here only with an empty ray; normalize it
		// so the message (and any diagnosis) reads as infeasible.
		SolverResult terminal_result = solve_result;
		if (terminal_result.status == SolverStatus::INF_OR_UNBD) {
			terminal_result.status = SolverStatus::INFEASIBLE;
		}
		vector<string> var_labels;
		vector<bool> var_is_aux;
		build_var_labels(var_labels, var_is_aux);

		auto diag_params = GetDecideDiagnosticParams(context);
		// The backend the primary solve ran on, named at plan time. Asking
		// SelectSolverBackend() again here would be a second, independent answer —
		// harmless while selection reads only the environment, but wrong the moment it
		// depends on the model, because the elastic re-solves would then run on a
		// different solver than the one that produced the failure being diagnosed.
		SolverBackend backend = PlannedSolverBackend();

		// `Finalize` retains `elastic` whenever `diagnose` is set, and this terminal is prefix-only.
		D_ASSERT(elastic);

		bool diagnostic_solve_timed_out = terminal_result.diagnostic_timed_out;
		bool diagnostic_solver_failed = false;
		auto solve_diagnostic_model = [backend, diagnostic_solve_options, &diagnostic_solve_timed_out,
		                               &diagnostic_solver_failed](const SolverModel &m) {
			SolverResult result;
			try {
				result = SolvePreparedModel(m, backend, diagnostic_solve_options);
			} catch (const Exception &) {
				// The widened repair program is harder than the query's own. Treat a
				// backend load/solve failure as an unavailable diagnosis, but keep this
				// guard at the callback boundary: exceptions raised by our diagnosis
				// logic must still surface as bugs instead of being mislabeled as a
				// solver limitation.
				result.status = SolverStatus::OTHER;
			}
			// OTHER is a solve the backend could not finish, thrown or returned (a model HiGHS refuses
			// to load returns it): no answer about the repair model, so it must not read as "no repair".
			if (result.status == SolverStatus::OTHER) {
				diagnostic_solver_failed = true;
			} else if (result.status == SolverStatus::TIME_LIMIT) {
				diagnostic_solve_timed_out = true;
			}
			return result;
		};

		// The repair model: formulated against the widened box rather than patched onto
		// the solved model's matrix, so its Big-M and McCormick constants describe the box
		// the repair actually searches instead of the narrower one the query solved in.
		DecideDiagnostic diag;
		SolverModel elastic_model;
		VarIndexer elastic_indexer;
		vector<string> elastic_global_labels;
		try {
			elastic_model = FormulateElasticModel(*elastic, evaluated, elastic_indexer, elastic_global_labels);
		} catch (const InvalidInputException &ex) {
			// The only thing the formulation raises by design: a construct the widened box
			// cannot express. Report it rather than turning the user's DIAGNOSE into an
			// error — or, worse, falling back to constants derived against the narrow box.
			// Deliberately NOT a catch-all: an InternalException is a bug in this pass and
			// has to surface as one.
			report(BuildUndiagnosedDiagnostic(
			    "infeasible", StringUtil::Format("the repair model could not be built: %s", ex.what())));
			break;
		}
		// The repair model is a solver-neutral model of this query too. The warning is said once, so this only speaks
		// for a number the solved model did not hold (a one-decision row it folded into a bound).
		if (ModelHoldsNumberOutOfRange(elastic_model)) {
			number_warning.Say();
		}
		InfeasibleDiagnosisInput diag_input {
		    elastic_model,
		    elastic_indexer,
		    var_labels,
		    var_is_aux,
		    elastic_global_labels,
		    diag_params,
		    solve_diagnostic_model,
		    written_sense == DecideSense::MAXIMIZE,
		};
		diag = DiagnoseInfeasible(diag_input);
		// The engine reads every status it gets: a solve it can do without (the tie-break, the
		// certificate of a printed amount) keeps the repair it already has. So a repair it
		// reports stands, whatever a solve on the way to it returned.
		if (diag.valid) {
			report(std::move(diag));
			break;
		}
		if (diagnostic_solver_failed) {
			// A backend that cannot load or solve the repair model. That is a limit on
			// what can be explained, not a failure of the query itself.
			report(BuildUndiagnosedDiagnostic("infeasible",
			                                  "the solver could not analyse the repair model for this query, so no "
			                                  "least-change repair could be computed."));
			break;
		}
		// The elastic engine can decline to report when no actionable relaxation exists.
		// Say which of the two it was rather than returning an empty relation.
		report(BuildUndiagnosedDiagnostic("infeasible",
		                                  diagnostic_solve_timed_out
		                                      ? "diagnosis ran out of time before it could find a least-change repair."
		                                      : "no loosening of the clauses you wrote restores feasibility."));
		break;
	}
	case DiagnosisTerminal::UNDIAGNOSED:
		// No DIAGNOSE prefix, or a status no engine covers (ITERATION_LIMIT, OTHER).
		// Either way the query reports its status and stops — the second solve that
		// would name a clause only happens when the user asks for it by name.
		ThrowDecideSolveError(solve_result, PlannedSolverBackend());
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

	if (bench) {
		solver_timer.End();
		fprintf(stderr, "DECIDB_BENCH: model_construction_ms=%.2f\n", model_timer.Elapsed() * 1000.0);
		fprintf(stderr, "DECIDB_BENCH: solver_ms=%.2f\n", solver_timer.Elapsed() * 1000.0);
		fprintf(stderr, "DECIDB_BENCH: total_variables=%zu\n", bench_total_vars);
		// Two separate units, never summed: `specs` is the clause-level view (which DECIDE
		// clause to look at), `rows` is what the solver actually received (where the matrix
		// pressure is). The old `total_constraints` added the two and reported neither.
		fprintf(stderr, "DECIDB_BENCH: total_constraint_specs=%zu\n", bench_constraint_specs);
		fprintf(stderr, "DECIDB_BENCH: total_constraint_rows=%zu\n", (size_t)solve_result.model_constraint_rows);
		fprintf(stderr, "DECIDB_BENCH: num_rows=%zu\n", (size_t)num_rows);
	}

	return SinkFinalizeType::READY;
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
static int64_t DecideSolutionToInteger(double solution_value, const LogicalType &var_type, const string &var_name) {
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
		// The wording is shared with A1's direct plan (`decide_numeric_policy.hpp`).
		throw InvalidInputException(DecideWholeNumberRangeMessage(var_name, StringUtil::Format("%.0f", rounded)));
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
	const idx_t input_columns = types.size() - decide_variables.size();
	for (idx_t col = 0; col < input_columns; col++) {
		chunk.data[col].Reference(source_state.scan_chunk.data[col]);
	}
	chunk.SetCardinality(source_state.scan_chunk.size());

	// All DECIDE vars (user + auxiliary) are in the output; projection above prunes aux vars
	idx_t total_decide_vars = decide_variables.size();
	idx_t chunk_size = chunk.size();

	// Fill in ALL DECIDE variable columns with solution values from ILP solver
	for (idx_t decide_var_idx = 0; decide_var_idx < total_decide_vars; decide_var_idx++) {
		// The DECIDE columns are appended at the end of the output
		idx_t column_idx = types.size() - total_decide_vars + decide_var_idx;

		auto &output_vector = chunk.data[column_idx];

		// An auxiliary's column only carries it to the projection that drops it: nobody
		// reads its value, so none is converted -- nor refused, which would name an
		// internal column for a value the user never asked for.
		if (decide_var_idx >= total_decide_vars - num_auxiliary_vars) {
			output_vector.SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(output_vector, true);
			continue;
		}

		// Set vector to flat (each row has its own value)
		output_vector.SetVectorType(VectorType::FLAT_VECTOR);

		// Get the logical type for this DECIDE variable
		auto &decide_var = decide_variables[decide_var_idx]->Cast<BoundColumnRefExpression>();
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
				    DecideSolutionToInteger(solution_at(row_in_chunk), var_type, decide_var.GetName()));
			}
		} else if (var_type == LogicalType::BIGINT) {
			auto output_data = FlatVector::GetData<int64_t>(output_vector);
			for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
				output_data[row_in_chunk] =
				    DecideSolutionToInteger(solution_at(row_in_chunk), var_type, decide_var.GetName());
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
