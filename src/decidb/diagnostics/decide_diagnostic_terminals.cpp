//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/diagnostics/decide_diagnostic_terminals.cpp
//
// The two failure terminals of DIAGNOSE: the unbounded one and the infeasible one. Each takes what it reads as plain
// arguments, runs its engine (decide_diagnostic_engines.cpp) and returns the finding, or why there is none; the
// operator hands it off. They live apart from the engines, which stay free of the buffered rows and of the
// formulation stage: the unbounded terminal reads the rows and the infeasible one formulates the repair model.
//
//===----------------------------------------------------------------------===//

#include "duckdb/decidb/diagnostics/decide_diagnostic_terminals.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic_engines.hpp"
#include "duckdb/decidb/formulation/formulate_model.hpp"
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/execution/operator/decide/decide_key_map.hpp"
#include "duckdb/main/decide_number_warning.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace duckdb {

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

//! Builds the categorical-candidate groupings the unbounded engine consumes to
//! characterize *which* instances of an escaping variable run away. Operator-bound
//! (it reads the input's named columns, keyed scopes, and GroupColumn), so it lives
//! here rather than in the pure router/engine units; the terminal just invokes it.
//!
//! A stateful functor (not a set of lambdas) so its lazily-built caches — the
//! row-scoped candidates and the per-scope keyed candidates — survive across the
//! many calls the engine makes, one per escaping decision variable. The stored
//! references point at the terminal's arguments, which outlive the engine call.
namespace {
struct UnboundedCandidateProvider {
	ClientContext &context;
	const ColumnDataCollection &data;
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
	input.ForEachClauseRow([&](const EvaluatedConstraint &ec) {
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
	});

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
//! same FormulateModel pass the solved model went through — `FormulateModel` is a pure
//! function of `SolverInput` plus a box, which is what makes running it twice
//! meaningful rather than merely possible.
//!
//! Each call starts from a fresh copy of `elastic`, so no rows or auxiliaries from the
//! solved formulation can leak into the repair formulation.
static SolverModel FormulateElasticModel(const ElasticFormulation &elastic, VarIndexer &out_var_indexer,
                                         vector<string> &out_global_labels) {
	SolverInput input = elastic.input;
	VarIndexer indexer = elastic.var_indexer;

	OpenElasticColumnBox(input);
	FormulateModel(input, FormulationBox::OfSolvedModel(input), indexer);

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
// The terminals
//===--------------------------------------------------------------------===//

DecideDiagnostic UnboundedTerminal(ClientContext &context, const SolverResult &solve_result,
                                   const SolverModel &retained_model, const VarIndexer &var_indexer,
                                   const vector<string> &var_labels, const vector<bool> &var_is_aux,
                                   const ColumnDataCollection &data, const vector<string> &input_column_names,
                                   const vector<bool> &input_column_in_clause) {
	auto diag_params = GetDecideDiagnosticParams(context);
	// The keyed mappings were moved into the solver input, so the provider reads the live ones
	// off var_indexer.
	UnboundedDiagnosisInput diag_input {
	    solve_result,
	    var_indexer,
	    var_labels,
	    var_is_aux,
	    diag_params,
	    UnboundedCandidateProvider {context, data, data.Types(), diag_params, input_column_names,
	                                input_column_in_clause, var_indexer, var_indexer.keyed_mappings, data.Count()},
	};
	DecideDiagnostic diag = DiagnoseUnbounded(diag_input);
	// A valid diagnosis holds a finding (the builders in decide_diagnostic.cpp).
	if (diag.valid) {
		return diag;
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
	return BuildUndiagnosedDiagnostic(
	    solve_result.status == SolverStatus::INF_OR_UNBD ? "infeasible or unbounded" : "unbounded", reason);
}

DecideDiagnostic InfeasibleTerminal(ClientContext &context, const SolverResult &solve_result,
                                    const ElasticFormulation &elastic, const vector<string> &var_labels,
                                    const vector<bool> &var_is_aux, SolverBackend backend,
                                    const SolveModelOptions &diagnostic_solve_options, bool negate_objective) {
	// A residual INF_OR_UNBD is routed here only with an empty ray; normalize it
	// so the message (and any diagnosis) reads as infeasible.
	SolverResult terminal_result = solve_result;
	if (terminal_result.status == SolverStatus::INF_OR_UNBD) {
		terminal_result.status = SolverStatus::INFEASIBLE;
	}
	auto diag_params = GetDecideDiagnosticParams(context);
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
		elastic_model = FormulateElasticModel(elastic, elastic_indexer, elastic_global_labels);
	} catch (const InvalidInputException &ex) {
		// The only thing the formulation raises by design: a construct the widened box
		// cannot express. Report it rather than turning the user's DIAGNOSE into an
		// error — or, worse, falling back to constants derived against the narrow box.
		// Deliberately NOT a catch-all: an InternalException is a bug in this pass and
		// has to surface as one.
		return BuildUndiagnosedDiagnostic("infeasible",
		                                  StringUtil::Format("the repair model could not be built: %s", ex.what()));
	}
	// The repair model is a solver-neutral model of this query too. The warning is said once, so this only speaks
	// for a number the solved model did not hold (a one-decision row it folded into a bound).
	if (ModelHoldsNumberOutOfRange(elastic_model)) {
		DecideNumberWarningState::Of(context).Say();
	}
	InfeasibleDiagnosisInput diag_input {
	    elastic_model, elastic_indexer,        var_labels,       var_is_aux, elastic_global_labels,
	    diag_params,   solve_diagnostic_model, negate_objective,
	};
	diag = DiagnoseInfeasible(diag_input);
	// The engine reads every status it gets: a solve it can do without (the tie-break, the
	// certificate of a printed amount) keeps the repair it already has. So a repair it
	// reports stands, whatever a solve on the way to it returned.
	if (diag.valid) {
		return diag;
	}
	if (diagnostic_solver_failed) {
		// A backend that cannot load or solve the repair model. That is a limit on
		// what can be explained, not a failure of the query itself.
		return BuildUndiagnosedDiagnostic("infeasible",
		                                  "the solver could not analyse the repair model for this query, so no "
		                                  "least-change repair could be computed.");
	}
	// The elastic engine can decline to report when no actionable relaxation exists.
	// Say which of the two it was rather than returning an empty relation.
	return BuildUndiagnosedDiagnostic("infeasible",
	                                  diagnostic_solve_timed_out
	                                      ? "diagnosis ran out of time before it could find a least-change repair."
	                                      : "no loosening of the clauses you wrote restores feasibility.");
}

} // namespace duckdb
