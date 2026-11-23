//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/formulate_model.cpp
//
// Stage 06's driver. See formulate_model.hpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/formulate_model.hpp"
#include "duckdb/common/decide_profile.hpp"
#include "duckdb/decidb/formulation/ilp_linearization.hpp"

namespace duckdb {

void FormulateModel(SolverInput &input, const FormulationBox &box, VarIndexer &var_indexer) {
	DecideProfileScope phase("formulation.abs_bounds");
	idx_t num_rows = input.num_rows;

	// The flat column space is settled by assembly and is never rebuilt here: the row,
	// keyed and global blocks are final, and only the global block grows as auxiliaries
	// are appended below. That is what lets a second formulation, against a different
	// box, name exactly the same columns as the first — which the elastic path relies on
	// to read its solution back through the same labels.
#ifdef DEBUG
	const idx_t formulation_entry_columns = var_indexer.global_block_start;
#endif

	// Plain declared names, for the refusals the linearization can raise.
	vector<string> decide_var_names = input.variable_names;
	decide_var_names.reserve(input.num_decide_vars);
	// A frame read is named as its base is: the bound a refusal asks for is the base's.
	for (auto &read : input.frame_reads) {
		decide_var_names.push_back(decide_var_names[read.base]);
	}

	// THE ROUTING of every construct this stage may state natively, one rule for all of them (NativeConstructPolicy).
	// Stage 05 recorded the shape and the backend's capabilities; whether a declared construct is used is decided here,
	// per site, because only a stage that has seen the evaluated coefficients knows whether THIS clause has a Big-M.
	// Native is the default; MIN/MAX is lowered because the lowering is measured faster, ABS is not (a measured tie).
	const NativeConstructPolicy native_abs {input.use_native_constructs.abs, input.force_native_constructs,
	                                        /*lowering_measured_faster=*/false};
	const NativeConstructPolicy native_min_max {input.use_native_constructs.min_max, input.force_native_constructs,
	                                            /*lowering_measured_faster=*/true};

	// A bilinear auxiliary may itself sit inside ABS (notably the reverse link of an
	// L0 norm). Temporarily expose its exact product box while descendant constructs
	// derive their own ranges. Restore the declared column box before McCormick emission
	// so ordinary solve models keep their existing, row-enforced auxiliary bounds.
	vector<pair<idx_t, pair<double, double>>> bilinear_declared_boxes;
	bilinear_declared_boxes.reserve(input.bilinear_links.size());
	for (const auto &link : input.bilinear_links) {
		bilinear_declared_boxes.push_back(
		    {link.aux_idx, {input.lower_bounds[link.aux_idx], input.upper_bounds[link.aux_idx]}});
	}
	// THE ORDER, one for every query, and it is load-bearing. Deriving an ABS auxiliary's range is also what boxes its
	// column, and every linearizer below computes its Big-M from column boxes: run them first and an outer MIN/MAX,
	// `<>` or product over ABS(...) sees an unbounded column and refuses a query whose bound was there to be computed.
	// The boxes are each other's inputs. A product's box is its factor's (`b * abs(x)` reads the ABS's), an ABS may
	// hold a product or a helper decision of the value rewrite in its operand, and a helper decision (`b * sum(x)`,
	// `sum(abs(x))`) is boxed by its members' reach, which may be an ABS auxiliary or a product with a helper; a
	// helper's box also bounds the helpers it is defined from (DeriveReducerMemberBounds). Every derivation only
	// tightens, so alternating them settles in as many passes as the constructs nest. The refusals are decided last,
	// on the settled boxes, naming a helper by its reducer: a product refuses a factor with no finite top, and the
	// lowering of an exact ABS refuses an operand with no finite reach (the native path leaves it open and answers).
	for (auto &link : input.reducer_value_links) {
		decide_var_names[link.value_var] = link.label;
	}
	for (auto &link : input.abs_links) {
		decide_var_names[link.aux_idx] = link.written;
	}
	const idx_t max_passes =
	    2 * (input.abs_links.size() + input.bilinear_links.size() + input.reducer_value_links.size() + 1);
	for (idx_t pass = 0;; pass++) {
		auto lower = input.lower_bounds;
		auto upper = input.upper_bounds;
		auto reach_lower = input.instance_reach_lower;
		auto reach_upper = input.instance_reach_upper;
		DeriveBilinearAuxiliaryBounds(input, box, decide_var_names, /*refuse_when_unbounded=*/false);
		DeriveAbsAuxiliaryBounds(input, var_indexer, decide_var_names, /*refuse_when_unbounded=*/false);
		DeriveReducerValueBounds(input, var_indexer);
		DeriveReducerMemberBounds(input, var_indexer);
		if (lower == input.lower_bounds && upper == input.upper_bounds && reach_lower == input.instance_reach_lower &&
		    reach_upper == input.instance_reach_upper) {
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
	// The Big-M of a count of nonzeros `norm(e, 0)` written with none: its rows `M * z >= e` and `M * z >= -e` arrive
	// with a placeholder on the indicator `z`, and M is the reach of the rest of the row in `box`, so it is derived
	// again whenever the box changes. The boxes above are settled first: `e` may hold an ABS or a helper decision. It
	// is derived before the refusals below, so a count over an open decision is refused as the count it is.
	phase.Next("formulation.l0_constants");
	for (auto &ec : input.constraints) {
		const idx_t z = ec.sized_indicator;
		if (z == DConstants::INVALID_INDEX) {
			continue;
		}
		auto set_indicator = [&](double value) {
			for (idx_t t = 0; t < ec.records.variables.size(); t++) {
				if (ec.records.variables[t] == z) {
					ec.records.coefficients[t] = value;
				}
			}
		};
		set_indicator(0.0);
		set_indicator(DecideTightPerRowBigM(ec, box, decide_var_names, InstanceBox {input, var_indexer},
		                                    DecideWrittenClause(input, ec.provenance.origin.source_clause_id),
		                                    "norm(e, 0)"));
	}
	phase.Next("formulation.abs_refusals");
	DeriveBilinearAuxiliaryBounds(input, box, decide_var_names);
	DeriveAbsAuxiliaryBounds(input, var_indexer, decide_var_names, !native_abs.available);

	EmitAbsConstraints(input, var_indexer, native_abs);

	// The helper decisions' definitions: an equality row per group for SUM/AVG, an
	// extremum link per group for MIN/MAX. A refusal names a helper by its reducer.
	phase.Next("formulation.value_links");
	LinearizeReducerValueLinks(input, var_indexer, decide_var_names, native_min_max);

	// Encode `<>` as the disjunction it is: two conditional rows per instance. Whether a
	// condition is stated to the backend or encoded with a Big-M is settled once, by
	// LowerDecideConstructs at the end of this function.
	phase.Next("formulation.not_equal");
	if (!input.not_equals.empty()) {
		LinearizeNotEqual(input, var_indexer);
	}

	// Emit the McCormick envelope for every bilinear w = b * x auxiliary.
	for (const auto &saved : bilinear_declared_boxes) {
		input.lower_bounds[saved.first] = saved.second.first;
		input.upper_bounds[saved.first] = saved.second.second;
	}
	phase.Next("formulation.bilinear");
	LinearizeBilinear(input, var_indexer, box);

	// Global variables are appended at var_indexer.global_block_start.
	// As we add more global vars, their indices are global_block_start + g
	// where g is the position in the global vars array.

	// Refresh total_vars: the row/keyed blocks were finalized before linearization,
	// but global aux vars were appended by the construct sites above.
	var_indexer.total_vars = var_indexer.global_block_start + input.num_global_vars;

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
	LowerDecideConstructs(input, var_indexer, box, decide_var_names, input.use_native_constructs);

#ifdef DEBUG
	// No decide variable was added: only the global block grew.
	D_ASSERT(var_indexer.global_block_start == formulation_entry_columns);
#endif
}

} // namespace duckdb
