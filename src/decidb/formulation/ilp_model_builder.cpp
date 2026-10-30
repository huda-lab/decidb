#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/decide_profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

double SumFixedAggregateLhsOffset(const EvaluatedConstraint &constraint, const vector<idx_t> *rows, idx_t begin,
                                  idx_t end, const string &written) {
    double offset = 0.0;
    for (idx_t term_idx = 0; term_idx < constraint.variable_indices.size(); term_idx++) {
        if (constraint.variable_indices[term_idx] != DConstants::INVALID_INDEX) {
            continue;
        }
        auto &column = constraint.row_coefficients[term_idx];
        for (idx_t k = begin; k < end; k++) {
            idx_t row = rows ? (*rows)[k] : k;
            AddCoefficient(offset, column.Get(row), written);
        }
    }
    return offset;
}

string DecideWrittenClause(const vector<ConstraintSourceInfo> &sources, const string &objective_written,
                           idx_t source_clause_id) {
    if (source_clause_id < sources.size()) {
        return QuoteWrittenClause(sources[source_clause_id]);
    }
    return objective_written;
}

string DecideWrittenClause(const SolverInput &input, idx_t source_clause_id) {
    return DecideWrittenClause(input.constraint_sources, input.objective_written, source_clause_id);
}

static void BuildVarIndexer(VarIndexer &idx, const SolverInput &input) {
    idx.num_rows = input.num_rows;
    idx_t num_decide_vars = input.num_decide_vars;

    idx.var_scope.resize(num_decide_vars, DecideVarScope::ROW);
    idx.row_var_offset.resize(num_decide_vars, DConstants::INVALID_INDEX);
    idx.keyed_var_base.resize(num_decide_vars, DConstants::INVALID_INDEX);
    idx.var_keyed_mapping_idx.resize(num_decide_vars, DConstants::INVALID_INDEX);
    idx.scalar_var_index.resize(num_decide_vars, DConstants::INVALID_INDEX);

    // Classify variables and compute row-block offsets
    idx_t row_var_count = 0;
    for (idx_t v = 0; v < num_decide_vars; v++) {
        // A variable with no recorded scope (synthesized auxiliaries appended after
        // the declaration list) is row-scoped, matching the previous default.
        auto scope = v < input.variable_scopes.size() ? input.variable_scopes[v].scope
                                                     : DecideVarScope::ROW;
        idx.var_scope[v] = scope;
        if (scope == DecideVarScope::ROW) {
            idx.row_var_offset[v] = row_var_count;
            idx.row_offset_var.push_back(v);
            row_var_count++;
        }
    }
    idx.num_row_vars = row_var_count;

    // Row block: num_rows * num_row_vars
    idx.keyed_block_start = input.num_rows * row_var_count;

    // Keyed block: assign base offsets per keyed variable
    idx_t keyed_offset = 0;
    for (idx_t v = 0; v < num_decide_vars; v++) {
        if (idx.var_scope[v] != DecideVarScope::KEYED) {
            continue;
        }
        idx_t scope_idx = input.variable_scopes[v].keyed_scope_idx;
        D_ASSERT(scope_idx < idx.keyed_mappings.size());
        idx.var_keyed_mapping_idx[v] = scope_idx;
        idx.keyed_var_base[v] = idx.keyed_block_start + keyed_offset;
        idx.keyed_block_var.push_back(v);
        keyed_offset += idx.keyed_mappings[scope_idx].num_classes;
    }

    // Scalar block: exactly one column per query-wide variable, independent of num_rows
    idx.scalar_block_start = idx.keyed_block_start + keyed_offset;
    idx_t scalar_count = 0;
    for (idx_t v = 0; v < num_decide_vars; v++) {
        if (idx.var_scope[v] != DecideVarScope::GLOBAL) {
            continue;
        }
        idx.scalar_var_index[v] = idx.scalar_block_start + scalar_count;
        idx.scalar_slot_var.push_back(v);
        scalar_count++;
    }

    idx.global_block_start = idx.scalar_block_start + scalar_count;
    idx.total_vars = idx.global_block_start + input.num_global_vars;
}

idx_t VarIndexer::OwnerOf(idx_t col) const {
    if (col >= global_block_start) {
        return DConstants::INVALID_INDEX; // global block: no decide variable owns it
    }
    if (col < keyed_block_start) {
        // Row block, laid out row-major: every row holds one slot per row-scoped var.
        return num_row_vars == 0 ? DConstants::INVALID_INDEX : row_offset_var[col % num_row_vars];
    }
    if (col < scalar_block_start) {
        // Keyed block: bases ascend in keyed_block_var order, so the owner is the
        // last variable whose base does not exceed `col`.
        idx_t owner = DConstants::INVALID_INDEX;
        for (idx_t v : keyed_block_var) {
            if (keyed_var_base[v] > col) {
                break;
            }
            owner = v;
        }
        return owner;
    }
    return scalar_slot_var[col - scalar_block_start];
}

VarIndexer VarIndexer::Build(const SolverInput &input) {
    VarIndexer idx;
    // Own a copy so this VarIndexer can outlive the SolverInput
    idx.keyed_mappings = input.keyed_mappings;
    BuildVarIndexer(idx, input);
    return idx;
}

#ifdef DEBUG
//! The shape contract a reduced constraint's RHS must satisfy by the time it reaches
//! this file: one value per group. The optimizer's bound-side window rewrite
//! (DecideOptimizer::RewriteRhsAggregates) establishes it, which is why the per-row
//! checks that used to live at the emission sites are gone. Debug-only — a regression here is a wrong model, not a bad
//! query, so it should fail loudly in development rather than produce a user-facing error.
static bool RhsIsConstantWithinGroups(const EvaluatedConstraint &ec) {
    if (ec.rhs_values.IsUniform() || ec.rhs_values.Empty()) {
        return true;
    }
    const bool has_groups = !ec.row_group_ids.empty();
    std::unordered_map<idx_t, double> first_seen;
    for (idx_t row = 0; row < ec.rhs_values.Size(); row++) {
        idx_t g = has_groups ? ec.row_group_ids[row] : 0;
        if (g == DConstants::INVALID_INDEX) {
            continue;
        }
        auto it = first_seen.find(g);
        if (it == first_seen.end()) {
            first_seen.emplace(g, ec.rhs_values.Get(row));
        } else if (it->second != ec.rhs_values.Get(row)) {
            return false;
        }
    }
    return true;
}
#endif

void BuildGroupCSR(const vector<idx_t> &row_group_ids,
                   idx_t num_groups,
                   vector<idx_t> &offsets,
                   vector<idx_t> &flat_rows) {
    if (row_group_ids.empty() || num_groups == 0) {
        offsets.clear();
        flat_rows.clear();
        return;
    }
    offsets.assign(num_groups + 1, 0);
    // First pass: count rows per group
    for (idx_t r = 0; r < row_group_ids.size(); r++) {
        idx_t gid = row_group_ids[r];
        if (gid == DConstants::INVALID_INDEX) continue;
        offsets[gid + 1]++;
    }
    // Prefix sum
    for (idx_t g = 0; g < num_groups; g++) {
        offsets[g + 1] += offsets[g];
    }
    // Second pass: scatter rows into their group bucket using a tmp cursor
    flat_rows.assign(offsets[num_groups], 0);
    vector<idx_t> cursor = offsets;
    for (idx_t r = 0; r < row_group_ids.size(); r++) {
        idx_t gid = row_group_ids[r];
        if (gid == DConstants::INVALID_INDEX) continue;
        flat_rows[cursor[gid]++] = r;
    }
}

const vector<idx_t> &ClauseRows(const EvaluatedConstraint &ec) {
    D_ASSERT(!ec.row_group_ids.empty());
    if (ec.group_offsets.empty()) {
        BuildGroupCSR(ec.row_group_ids, ec.num_groups, ec.group_offsets, ec.group_row_ids);
    }
    return ec.group_row_ids;
}

//! F2: stamp the row -> clause provenance fields every constraint-builder site sets,
//! linear or quadratic, aggregate or per-row. `group_key` defaults to INVALID_INDEX for
//! an ungrouped aggregate row; pass the `when`/`by` group id (and its label) for a grouped
//! aggregate row, or the row id for a per-row row. Leaves every other ConstraintProvenance
//! field at its default -- the aggregate-only and linear-aggregate-only fields are each
//! their own site (see StampAggregateProvenance and the avg/weight/folded-terms block in
//! the linear aggregate branches), because a per-row or quadratic row does not carry them:
//! FormatQuadraticLhs never reads avg_scaled/weight_labels/folded_terms, and FormatLhs's
//! is_aggregate wrapping is meaningless off the aggregate branch.
//! `lhs_offset` is the constant part of the LHS that this site folded into the RHS
//! (`SUM(x + c) >= K` is emitted as `SUM(x) >= K - Sum(c)`). The reconstructed clause
//! label still renders the LHS the user wrote, constant included, so the RHS it is
//! quoted against has to be the user's `K` and not the folded one — otherwise the report
//! names a bound nobody typed. It rides on `rhs_mechanism_offset` alongside whatever a
//! lowering added, because the reader needs their sum and neither alone.
static void StampConstraintProvenance(ConstraintProvenance &provenance, const EvaluatedConstraint &eval_const,
                                      idx_t repair_group_id, idx_t group_key = DConstants::INVALID_INDEX,
                                      const string &group_label = string(), double lhs_offset = 0.0) {
    provenance.source_clause_id = eval_const.source_clause_id;
    provenance.repair_group_id = repair_group_id;
    provenance.removal_group_id = eval_const.removal_group_id;
    provenance.kind = eval_const.kind;
    // A literal RHS is one editable knob shared across every row/group this clause
    // emits (paper §5); a data-derived RHS repairs as a symbolic offset instead.
    provenance.shape = eval_const.rhs_is_shared_scalar ? ElasticShape::SHARED_SCALAR
                                                        : ElasticShape::PER_ROW_DATA;
    if (!eval_const.rhs_is_shared_scalar) {
        provenance.rhs_label = eval_const.rhs_label;
    }
    provenance.rhs_mechanism_offset = eval_const.rhs_mechanism_offset - lhs_offset;
    if (group_key != DConstants::INVALID_INDEX) {
        provenance.group_key = group_key;
    }
    if (!group_label.empty()) {
        provenance.group_label = group_label;
    }
}

//! The data row a per-row row was emitted at (DIAGNOSE's `row`). A row that stands for
//! no class set is a kept row, a class of its own (D28). Stamped after `class_set`.
static void StampDataRow(ConstraintProvenance &provenance, idx_t row) {
    provenance.data_row = row;
    provenance.per_kept_row = provenance.class_set == DConstants::INVALID_INDEX;
}

//! The class set group `group`'s rows stand for (D28), INVALID_INDEX for none.
static idx_t ClassSetOf(const EvaluatedConstraint &eval_const, idx_t group) {
    return group < eval_const.group_class_set.size() ? eval_const.group_class_set[group] : DConstants::INVALID_INDEX;
}

//! F2, aggregate sites only: reducer-placement metadata for the diagnosis renderer's
//! SUM(...)-wrapping (Facet B). A per-row constraint has no reducer, so this is never
//! called there.
static void StampAggregateProvenance(ConstraintProvenance &provenance, const EvaluatedConstraint &eval_const) {
    provenance.is_aggregate = eval_const.lhs_is_aggregate;
}

//! The model class this built model demands, read straight off the facts the build
//! settled. Its counterpart is the plan-time prediction in stage 05
//! (DeriveDecideModelClass); SolveModel checks the two agree before handing the model
//! to a backend.
SolverModelClass SolverModel::ModelClass() const {
    SolverModelClass needed;
    needed.quadratic_constraints = !quadratic_constraints.empty();
    if (has_quadratic_obj) {
        needed.nonconvex_quadratic = nonconvex_quadratic;
        // MIQP is a quadratic objective over ANY integral column, auxiliaries
        // included — a Big-M indicator makes the program mixed-integer just as a
        // user's `x(INT)` does.
        for (idx_t col = 0; col < num_vars && col < is_integer.size(); col++) {
            if (is_integer[col]) {
                needed.miqp = true;
                break;
            }
        }
        // An off-diagonal entry means two decision variables are coupled in the
        // objective, which makes Q rank-deficient. Read as a fact here; stage 05
        // predicts the same thing from the unexpanded squared terms.
        for (idx_t k = 0; k < q_vals.size() && k < q_rows.size() && k < q_cols.size(); k++) {
            if (q_rows[k] != q_cols[k] && q_vals[k] != 0.0) {
                needed.singular_quadratic = true;
                break;
            }
        }
    }
    return needed;
}

bool SolverModel::ProvesInfeasible() const {
    for (const auto &row : constraints) {
        if ((row.indices.empty() && row.constant_fails) || IsUnreachableBound(row.sense, row.rhs)) {
            return true;
        }
    }
    for (const auto &row : quadratic_constraints) {
        if (IsUnreachableBound(row.sense, row.rhs)) {
            return true;
        }
    }
    for (idx_t col = 0; col < num_vars && col < col_lower.size() && col < col_upper.size(); col++) {
        if (col_lower[col] > col_upper[col]) {
            return true;
        }
    }
    return false;
}

SolverModel SolverModel::Build(SolverInput &input, const VarIndexer &indexer) {
    DecideProfileScope profile("model.build");
    DecideProfileScope phase("model.variables_and_bounds");
    SolverModel model;
    model.class_sets = std::move(input.class_sets);

    idx_t num_rows = input.num_rows;
    idx_t num_decide_vars = input.num_decide_vars;

    // VarIndexer is built once in PhysicalDecide::Finalize() and threaded in.
    idx_t total_vars = indexer.total_vars;

    model.num_vars = total_vars;

    //===--------------------------------------------------------------------===//
    // 1. Set up variables with bounds and types
    //===--------------------------------------------------------------------===//

    // Determine per-variable types and default bounds
    vector<double> per_var_lower(num_decide_vars);
    vector<double> per_var_upper(num_decide_vars);
    vector<bool> per_var_binary(num_decide_vars, false);

    for (idx_t var = 0; var < num_decide_vars; var++) {
        auto logical_type = input.variable_types[var];

        if (logical_type == LogicalType::DOUBLE || logical_type == LogicalType::FLOAT) {
            per_var_binary[var] = false;
            per_var_lower[var] = 0.0;
            per_var_upper[var] = DECIDE_SOLVER_INFINITY;
        } else if (logical_type == LogicalType::BOOLEAN) {
            per_var_binary[var] = true;
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1.0;
        } else {
            // INTEGER / BIGINT or default
            per_var_lower[var] = 0.0;
            per_var_upper[var] = DECIDE_SOLVER_INFINITY;
        }
    }

    // Merge with explicit bounds from input. The physical operator already
    // resolved each variable's lower bound (defaulting unconstrained variables to
    // 0 and honoring explicit negative bounds), so it is authoritative: take it
    // directly rather than std::max-ing with the type default, which would clamp a
    // legitimate negative bound back to 0. The upper bound is still intersected
    // (the type default is a true ceiling, e.g. 1.0 for BOOLEAN).
    for (idx_t var = 0; var < num_decide_vars; var++) {
        if (var < input.lower_bounds.size()) {
            per_var_lower[var] = input.lower_bounds[var];
        }
        if (var < input.upper_bounds.size()) {
            per_var_upper[var] = std::min(per_var_upper[var], input.upper_bounds[var]);
        }
    }

    // A BOOLEAN-domain variable's bounds are normally exactly [0,1] (the merge above
    // is a no-op for it — its lower default is already the resolved, folded 0, and
    // its upper is capped by the type default). But an explicit, unusual user pin that
    // widens past the domain (e.g. `x >= -1` on a declared BOOL) would leave `binary`
    // paired with an out-of-[0,1] bound, which solver backends don't accept. Downgrade
    // to a plain bounded integer in that case rather than sending a contradictory
    // (binary, out-of-range) column — same solve result, just not flagged binary.
    for (idx_t var = 0; var < num_decide_vars; var++) {
        if (per_var_binary[var] && (per_var_lower[var] < 0.0 || per_var_upper[var] > 1.0)) {
            per_var_binary[var] = false;
        }
    }

    // Expand per-variable config to all solver variables
    model.col_lower.resize(total_vars);
    model.col_upper.resize(total_vars);
    model.is_integer.resize(total_vars, false);
    model.is_binary.resize(total_vars, false);

    for (idx_t var = 0; var < num_decide_vars; var++) {
        idx_t num_instances = indexer.NumInstances(var);
        for (idx_t inst = 0; inst < num_instances; inst++) {
            idx_t var_idx = indexer.InstanceColumn(var, inst);
            // The instance's box: the decision's, with the floor raised by the refinement where the default floor of 0
            // still holds (a constant lower bound under a `when` replaces the default floor on the instances it keeps
            // alone), and a helper's ends at its own group's reach.
            model.col_lower[var_idx] = input.InstanceBoxLower(var, inst, per_var_lower);
            model.col_upper[var_idx] = input.InstanceBoxUpper(var, inst, per_var_upper);
            model.is_integer[var_idx] = !(input.variable_types[var] == LogicalType::DOUBLE ||
                                          input.variable_types[var] == LogicalType::FLOAT);
            model.is_binary[var_idx] = per_var_binary[var];
        }
		// A row with no decision maps to the absent class: fixed at 0, so an objective term
		// that reads it is dropped (KeyedMapping::absent_class).
		if (indexer.var_scope[var] == DecideVarScope::KEYED) {
			auto absent = indexer.keyed_mappings[indexer.var_keyed_mapping_idx[var]].absent_class;
			if (absent != DConstants::INVALID_INDEX) {
				idx_t var_idx = indexer.InstanceColumn(var, absent);
				model.col_lower[var_idx] = 0.0;
				model.col_upper[var_idx] = 0.0;
			}
		}
	}

    // Append global auxiliary variables after row+keyed blocks.
    //
    // Stage 05 derives every auxiliary's box from the expression it stands for, so an
    // infinite bound here means one of two things: the expression genuinely reaches an
    // unbounded decision variable (flagged, and legitimate), or a creation site skipped
    // the derivation. The second is a silent performance cliff rather than a wrong
    // answer — a free continuous column leaves the root simplex without a box and it
    // crawls — so it is asserted rather than left to a benchmark to notice.
    for (idx_t g = 0; g < input.num_global_vars; g++) {
        idx_t var_idx = indexer.global_block_start + g;
        auto gtype = input.global_variable_types[g];
        D_ASSERT(g >= input.global_bounds_unbounded.size() || input.global_bounds_unbounded[g] ||
                 (input.global_lower_bounds[g] > -DECIDE_EFFECTIVE_INFINITY &&
                  input.global_upper_bounds[g] < DECIDE_EFFECTIVE_INFINITY));
        model.col_lower[var_idx] = input.global_lower_bounds[g];
        model.col_upper[var_idx] = input.global_upper_bounds[g];
        model.is_integer[var_idx] = !(gtype == LogicalType::DOUBLE || gtype == LogicalType::FLOAT);
        model.is_binary[var_idx] = (gtype == LogicalType::BOOLEAN);
    }

    //===--------------------------------------------------------------------===//
    // 2. Set up objective function
    //===--------------------------------------------------------------------===//

    phase.Next("model.linear_objective");
    model.obj_coeffs.resize(total_vars, 0.0);

    // Set objective coefficients for global variables
    for (idx_t g = 0; g < input.num_global_vars; g++) {
        idx_t var_idx = indexer.global_block_start + g;
        if (g < input.global_obj_coeffs.size()) {
            model.obj_coeffs[var_idx] = input.global_obj_coeffs[g];
        }
    }
    model.maximize = (input.sense == DecideSense::MAXIMIZE);

    if (!input.objective_variable_indices.empty()) {
        for (idx_t term_idx = 0; term_idx < input.objective_variable_indices.size(); term_idx++) {
            idx_t decide_var_idx = input.objective_variable_indices[term_idx];

            // Skip constant terms (data columns without decide variables).
            // These don't affect the optimal solution — they add a constant offset
            // that doesn't change which assignment is optimal.
            if (decide_var_idx == DConstants::INVALID_INDEX) {
                continue;
            }

            if (term_idx >= input.objective_coefficients.size()) {
                continue;
            }
            // Every row's coefficient counts. A term beside the reducers counts once: execution kept its
            // coefficient on one row and zeroed the others (CountOnce, physical_decide.cpp).
            auto &col = input.objective_coefficients[term_idx];
            for (idx_t row = 0; row < num_rows; row++) {
                if (row >= col.Size()) break;
                idx_t var_idx = indexer.Get(decide_var_idx, row);
                // Use += because keyed vars: multiple rows map to same solver var
                AddCoefficient(model.obj_coeffs[var_idx], col.Get(row), input.objective_written);
            }
        }
    }

    //===--------------------------------------------------------------------===//
    // 2b. Build quadratic objective (Q matrix) if present
    //===--------------------------------------------------------------------===//

    phase.Next("model.quadratic_objective");
    bool has_power_quadratic = input.has_quadratic_objective && !input.quadratic_inner_variable_indices.empty();
    bool has_bilinear = !input.bilinear_objective_terms.empty();

    auto pack_q_key = [](int r, int c) -> uint64_t {
        return (static_cast<uint64_t>(r) << 32) | static_cast<uint32_t>(c);
    };

    if (has_power_quadratic || has_bilinear) {
        // Accumulate Q in a map: (var_i, var_j) -> value (lower triangle only, var_i >= var_j)
        std::unordered_map<uint64_t, double> q_map;

        // POWER(expr, 2) contributions: outer product A^T A
        if (has_power_quadratic) {
            model.has_quadratic_obj = true;
            double q_sign = input.quadratic_sign;  // +1.0 or -1.0

            // Non-convex when sign and sense conflict: MAXIMIZE + PSD (sign>0), or
            // MINIMIZE + NSD (sign<0). Shared with stage 05's plan-time prediction so
            // the refusal there and the fact recorded here cannot disagree.
            model.nonconvex_quadratic = IsNonconvexQuadraticObjective(q_sign, input.sense);

            idx_t num_q_terms = input.quadratic_inner_variable_indices.size();

            struct VarCoeff { int flat_idx; double coeff; };
            vector<VarCoeff> row_terms;
            for (idx_t row = 0; row < num_rows; row++) {
                row_terms.clear();

                for (idx_t t = 0; t < num_q_terms; t++) {
                    if (t >= input.quadratic_inner_coefficients.size() ||
                        row >= input.quadratic_inner_coefficients[t].Size()) {
                        continue;
                    }
                    idx_t decide_var_idx = input.quadratic_inner_variable_indices[t];
                    double a = input.quadratic_inner_coefficients[t].Get(row);
                    if (a == 0.0) continue;

                    if (decide_var_idx == DConstants::INVALID_INDEX) {
                        continue;
                    }

                    int flat_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                    row_terms.push_back({flat_idx, a});
                }

                for (idx_t i = 0; i < row_terms.size(); i++) {
                    for (idx_t j = 0; j <= i; j++) {
                        int ri = row_terms[i].flat_idx;
                        int rj = row_terms[j].flat_idx;
                        int q_row = std::max(ri, rj);
                        int q_col = std::min(ri, rj);
                        // Q holds the plain coefficient of each monomial (ilp_model.hpp),
                        // so store what expanding the square actually produces:
                        // diagonal coeff_i^2, off-diagonal 2*coeff_i*coeff_j (the pair
                        // appears twice in the expansion, and we keep one triangle).
                        double val = q_sign * row_terms[i].coeff * row_terms[j].coeff;
                        if (i != j) val *= 2.0;
                        AddCoefficient(q_map[pack_q_key(q_row, q_col)], val, input.objective_written);
                    }
                }

                // Handle constant term contributions to linear objective
                double c_row = 0.0;
                for (idx_t t = 0; t < num_q_terms; t++) {
                    if (t < input.quadratic_inner_coefficients.size() &&
                        row < input.quadratic_inner_coefficients[t].Size() &&
                        input.quadratic_inner_variable_indices[t] == DConstants::INVALID_INDEX) {
                        AddCoefficient(c_row, input.quadratic_inner_coefficients[t].Get(row), input.objective_written);
                    }
                }
                if (c_row != 0.0) {
                    for (auto &vt : row_terms) {
                        AddCoefficient(model.obj_coeffs[vt.flat_idx], q_sign * 2.0 * c_row * vt.coeff,
                                       input.objective_written);
                    }
                }
            }
        }

        // Bilinear objective terms: off-diagonal Q entries for x_a * x_b.
        // For SUM(c * x_a * x_b): Q[max(a,b), min(a,b)] += c -- c is already the plain
        // monomial coefficient the convention asks for, so it is stored as-is.
        if (has_bilinear) {
            model.has_quadratic_obj = true;
            model.nonconvex_quadratic = true; // Bilinear → always indefinite

            for (auto &bt : input.bilinear_objective_terms) {
                for (idx_t row = 0; row < num_rows; row++) {
                    double coeff = bt.row_coefficients[row];
                    if (coeff == 0.0) continue;

                    int flat_a = static_cast<int>(indexer.Get(bt.var_a, row));
                    int flat_b = static_cast<int>(indexer.Get(bt.var_b, row));
                    int q_row = std::max(flat_a, flat_b);
                    int q_col = std::min(flat_a, flat_b);
                    AddCoefficient(q_map[pack_q_key(q_row, q_col)], coeff, input.objective_written);
                }
            }
        }

        // Convert map to COO vectors
        model.q_rows.reserve(q_map.size());
        model.q_cols.reserve(q_map.size());
        model.q_vals.reserve(q_map.size());
        for (auto &entry : q_map) {
            if (entry.second == 0.0) continue;
            model.q_rows.push_back(static_cast<int>(entry.first >> 32));
            model.q_cols.push_back(static_cast<int>(entry.first & 0xFFFFFFFF));
            model.q_vals.push_back(entry.second);
        }
    }

    //===--------------------------------------------------------------------===//
    // 3. Build constraints
    //===--------------------------------------------------------------------===//

    phase.Next("model.constraints");

    // Helper: is the LHS of this EvaluatedConstraint provably integer-valued?
    //
    // The check runs on the user's LHS *before* any lowering (McCormick
    // linearization, auxiliary-variable introduction). Post-lowering auxiliaries
    // are often declared REAL even when they always take integer values (e.g.,
    // z = x * y with x Boolean and y Integer) — so inspecting `model.is_integer`
    // after the fact would spuriously reject valid integer LHS shapes.
    //
    // A term is integer-valued when every referenced DECIDE variable is
    // INTEGER/BOOLEAN (i.e., not `LogicalType::DOUBLE` or `LogicalType::FLOAT`)
    // and every coefficient is integral. For bilinear terms, both factors must
    // be integer-typed. For POWER(expr, 2) groups, the inner expression must be
    // integer-valued (integer-valued expressions squared remain integer-valued).
    //
    // Used to gate the strict-inequality rewrite (`< K → <= K-1`, `> K → >= K+1`),
    // which is semantically exact iff the LHS is confined to integer points.
    //
    // The two ways an LHS fails that test are reported differently, because only one
    // of them is still the user's to fix here. A REAL decision is knowable from the
    // declared type and is rejected at bind time by
    // `ValidateDecideNoStrictComparisonOnReal`, which can name the variable and quote
    // the clause; seeing one here means a strict comparison reached the model builder
    // without passing that gate, which is an invariant violation, not a query error.
    // A fractional coefficient comes from evaluating a data column and is knowable
    // only now, so it stays a user-facing refusal.
    enum class LhsIntegrality { INTEGER, REAL_VARIABLE, FRACTIONAL_COEFFICIENT };
    auto IsRealType = [](const LogicalType &t) {
        return t == LogicalType::DOUBLE || t == LogicalType::FLOAT;
    };
    auto IsEvalConstraintLhsIntegerValued = [&](const EvaluatedConstraint &ec) -> LhsIntegrality {
        for (idx_t i = 0; i < ec.variable_indices.size(); i++) {
            idx_t vi = ec.variable_indices[i];
            if (vi == DConstants::INVALID_INDEX) continue;
            if (IsRealType(input.variable_types[vi])) return LhsIntegrality::REAL_VARIABLE;
            if (!ec.row_coefficients[i].AllIntegral()) return LhsIntegrality::FRACTIONAL_COEFFICIENT;
        }
        for (const auto &bt : ec.bilinear_terms) {
            if (IsRealType(input.variable_types[bt.var_a])) return LhsIntegrality::REAL_VARIABLE;
            if (IsRealType(input.variable_types[bt.var_b])) return LhsIntegrality::REAL_VARIABLE;
            if (!bt.row_coefficients.AllIntegral()) return LhsIntegrality::FRACTIONAL_COEFFICIENT;
        }
        for (const auto &qg : ec.quadratic_groups) {
            for (idx_t i = 0; i < qg.variable_indices.size(); i++) {
                idx_t vi = qg.variable_indices[i];
                if (vi == DConstants::INVALID_INDEX) continue;
                if (IsRealType(input.variable_types[vi])) return LhsIntegrality::REAL_VARIABLE;
                if (!qg.row_coefficients[i].AllIntegral()) return LhsIntegrality::FRACTIONAL_COEFFICIENT;
            }
        }
        return LhsIntegrality::INTEGER;
    };

    // The one refusal both strict paths share, and it is no longer a user-facing one:
    // the whole integer-step gate is now stated on declared types at bind time, by
    // `ValidateDecideNoIntegerStepComparisonOnReal` for the decision and
    // `ValidateDecideIntegralComparisonOperands` for every other operand. Reaching here
    // means a strict comparison passed both and still evaluated to a non-integral LHS,
    // which is an invariant violation rather than a query error.
    //
    // Kept rather than deleted so a future rewrite that introduces a REAL auxiliary or a
    // fractional coefficient inside a strict constraint fails loudly, instead of silently
    // stepping a bound that is not on the lattice.
    auto RejectStrictOnNonIntegerLhs = [](LhsIntegrality integrality, const char *op, const char *relaxed) {
        throw InternalException(
            "Strict inequality '%s' with a non-integer-valued left-hand side (%s) reached "
            "the model builder; the bind-time type gate should have rejected it", op,
            integrality == LhsIntegrality::REAL_VARIABLE ? "REAL variable"
                                                         : "fractional coefficient");
    };

    // Helper: apply comparison sense to a constraint.
    // Strict `<` / `>` require a provably integer-valued LHS; otherwise reject.
    // `rhs` already has the LHS constant `lhs_offset` moved off it; the typed K added back.
    auto ApplyComparisonSense = [&](ModelConstraint &constr, ExpressionType cmp, double rhs,
                                    LhsIntegrality lhs_integrality, double lhs_offset) {
        if (cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
            constr.sense = '>'; constr.rhs = rhs;
        } else if (cmp == ExpressionType::COMPARE_GREATERTHAN) {
            if (lhs_integrality != LhsIntegrality::INTEGER) {
                RejectStrictOnNonIntegerLhs(lhs_integrality, ">", ">=");
            }
            // δ site: `> K` becomes `>= floor(K)+1`. Remember the user's typed K so
            // diagnosis can re-quote a suggestion against `> K`, not the δ-adjusted rhs.
            constr.sense = '>'; constr.rhs = DecideStrictBound(true, rhs);
            constr.provenance.strict = true; constr.provenance.typed_k = rhs + lhs_offset;
        } else if (cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
            constr.sense = '<'; constr.rhs = rhs;
        } else if (cmp == ExpressionType::COMPARE_LESSTHAN) {
            if (lhs_integrality != LhsIntegrality::INTEGER) {
                RejectStrictOnNonIntegerLhs(lhs_integrality, "<", "<=");
            }
            // δ site: `< K` becomes `<= ceil(K)-1`. Remember the user's typed K.
            constr.sense = '<'; constr.rhs = DecideStrictBound(false, rhs);
            constr.provenance.strict = true; constr.provenance.typed_k = rhs + lhs_offset;
        } else if (cmp == ExpressionType::COMPARE_EQUAL) {
            constr.sense = '='; constr.rhs = rhs;
        } else {
            throw InternalException("Unsupported comparison type in ILP model builder");
        }
    };

    // Handle constraints whose LHS reduced to 0 (all coefficients cancelled or
    // all variables folded into column bounds). A tautology is dropped; a
    // violated constant constraint (`0 <= -1`) is KEPT as a coefficient-free row,
    // so the whole model still gets built and retained.
    // Keeping the row is what lets the infeasible-diagnosis engine name and relax
    // the offending clause — it stays a USER_PARAMETER row with its own source
    // provenance, and the elastic weighting already guards an all-zero row. No
    // backend ever sees it: SolverModel::ProvesInfeasible answers the model first.
    auto PushNormalizedConstraint = [&](ModelConstraint &&constr) {
        // A coefficient-free row that fails is kept, with its provenance, so ProvesInfeasible
        // answers the model and diagnosis can relax the row; one that holds is a tautology.
        constr.constant_fails = constr.indices.empty() && ConstantRowFails(constr.sense, constr.rhs);
        if (!constr.indices.empty() || constr.constant_fails) {
            model.constraints.push_back(std::move(constr));
        }
    };

    // A per-row bound on one decision (`x <= cap`, `2 * x >= lo`, `0.1 * x <= c`) is a fact about one column, so it
    // goes into that column's box rather than becoming a model row, as a literal bound already does
    // (decide_singleton_row_folding.cpp). A literal is the same for every row and folds once in the optimizer; a
    // data-valued bound differs per row and is known only here, as the values it admits on each row (stage 08 reads
    // them from the classifier both paths share). The bound narrows the box it finds, as the row did: it never
    // replaces the default floor of 0 itself (D41). A constant floor under a `when` has set the floor of
    // the instances its rows keep before this runs (ResolveLiftedFloors), and its row then sets the exact value. An
    // infinite end on the side it binds (`x >= c` with c = +inf) admits no value and keeps the row, and a box the
    // folds invert stays inverted: ProvesInfeasible answers both, as it would the row.
    //
    // HiGHS's QP solver does not turn such rows back into bounds: a 9,600-row box-bounded QP ran past 60 s as rows
    // and solves in 0.4 s as bounds. Under diagnosis the rows stay, because the elastic engine relaxes rows and
    // quotes the clause from their provenance (`x <= cap_col + delta`).
    //
    // A coefficient of 0 on a row (`cap * x <= -1e-10` where `cap` is 0) leaves the comparison of a constant, which
    // holds for every value or for none. DuckDB's comparison says which (the admitted ends are all or nothing), not
    // the builder's tolerance, under diagnosis too; a row that fails is kept for ProvesInfeasible.
    auto TryFoldPerRowBound = [&](const EvaluatedConstraint &ec, idx_t row, ModelConstraint &constr) -> bool {
        if (ec.admitted_lower.logical_size == 0) {
            return false;
        }
        double lower = ec.admitted_lower[row];
        double upper = ec.admitted_upper[row];
        if (constr.indices.empty()) {
            constr.constant_fails = lower > upper;
            if (constr.constant_fails) {
                model.constraints.push_back(std::move(constr));
            }
            return true;
        }
        if (input.keep_singleton_rows || constr.indices.size() != 1) {
            return false;
        }
        if (lower == std::numeric_limits<double>::infinity() || upper == -std::numeric_limits<double>::infinity()) {
            return false;
        }
        // The column's box is finite, so an open end (±infinity) leaves it as it is.
        idx_t col = (idx_t)constr.indices[0];
        model.col_lower[col] = std::max(model.col_lower[col], lower);
        model.col_upper[col] = std::min(model.col_upper[col], upper);
        return true;
    };

    // Rough upper-bound on total model.constraints rows to avoid repeated
    // vector reallocation — aggregate w/o groups contributes 1, aggregate w/
    // groups contributes num_groups, per-row contributes num_rows. Plus any
    // raw global constraints appended after the main loop.
    {
        idx_t est_rows = input.global_constraints.size();
        for (auto &ec : input.constraints) {
            if (ec.has_quadratic || !ec.bilinear_terms.empty()) continue;
            if (ec.lhs_is_aggregate) {
                bool ec_has_groups = !ec.row_group_ids.empty();
                est_rows += ec_has_groups ? std::max<idx_t>(ec.num_groups, 1) : 1;
            } else {
                est_rows += num_rows;
            }
        }
        model.constraints.reserve(model.constraints.size() + est_rows);
    }

    // Scratch for the rows that sum several (term, row) pairs per column, reused across clauses: the dense strategy
    // allocates once.
    SparseCoeffAccumulator accum;
    for (auto &eval_const : input.constraints) {
        // Skip constraints with quadratic/bilinear terms — handled in quadratic section below
        if (!eval_const.bilinear_terms.empty() || eval_const.has_quadratic) {
            continue;
        }

        idx_t repair_group_id = eval_const.repair_group_id != DConstants::INVALID_INDEX
                                    ? eval_const.repair_group_id
                                    : static_cast<idx_t>(&eval_const - input.constraints.data());

        bool is_aggregate = eval_const.lhs_is_aggregate;
        bool has_groups = !eval_const.row_group_ids.empty();
        LhsIntegrality lhs_integrality = IsEvalConstraintLhsIntegerValued(eval_const);
        const string written = DecideWrittenClause(input, eval_const.source_clause_id);

        // Detect whether this constraint can bypass the hash-map accumulator:
        // * every non-fixed term must reference a row-scoped decide variable
        //   (keyed vars collapse many rows to the same solver index —
        //   need dedup), AND
        // * no decide variable appears in more than one term (otherwise distinct
        //   terms at the same row collide on the same flat index). Fixed LHS
        //   offsets use INVALID_INDEX and are subtracted from RHS separately.
        auto CanUseRowScopedFastPath = [&](const EvaluatedConstraint &ec) -> bool {
            std::unordered_set<idx_t> seen_vars;
            for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
                idx_t v = ec.variable_indices[term_idx];
                if (v == DConstants::INVALID_INDEX) continue;
                // Keyed and query-wide variables both collapse many rows onto
                // one column, so the per-row fan-out below would double-write them.
                if (indexer.var_scope[v] != DecideVarScope::ROW) return false;
                if (!seen_vars.insert(v).second) return false;
            }
            return true;
        };

        // Symbolic coefficient labels for a data-weighted aggregate: map each summed decide
        // variable to its coefficient expression (`buy → l_extendedprice`) so infeasible
        // diagnosis can render `SUM(buy * l_extendedprice)` rather than the numeric fan-out.
        auto StampWeightLabels = [&](ConstraintProvenance &prov) {
            for (idx_t t = 0; t < eval_const.variable_indices.size(); t++) {
                idx_t v = eval_const.variable_indices[t];
                if (v == DConstants::INVALID_INDEX || t >= eval_const.coefficient_labels.size() ||
                    eval_const.coefficient_labels[t].empty()) {
                    continue;
                }
                prov.weight_labels.emplace_back(v, eval_const.coefficient_labels[t]);
            }
        };

        // Written per-term coefficients, for the accumulating path only. There the row's
        // coefficient is a *sum* over the folded rows (keyed vars collapse many
        // rows onto one column), so the reconstruction cannot recover what the user typed
        // from the matrix alone.
        //
        // `coefficient_labels[t]` is the source text of the term's coefficient expression
        // (physical_decide.cpp:3069) — "1" for a bare `SUM(keepS)`, "l_extendedprice" for
        // `SUM(buy * l_extendedprice)`. A label that is wholly a number therefore *is* the
        // written coefficient; anything else is data-varying and has no single value to
        // quote (weight_labels covers that case). The column's own `IsUniform()` only
        // catches the Scalar representation — a written constant usually materializes
        // dense — so the label is the reliable discriminator.
        auto ParseLiteralCoefficient = [](const string &label, double &value) -> bool {
            if (label.empty()) {
                return false;
            }
            const char *begin = label.c_str();
            char *end = nullptr;
            double parsed = std::strtod(begin, &end);
            if (end != begin + label.size() || !std::isfinite(parsed)) {
                return false;
            }
            value = parsed;
            return true;
        };
        // Computed once per clause, not per group: the grouped path below emits one constraint
        // per group and would otherwise rebuild this (and its scratch set) for every one.
        // Empty unless the clause is an aggregate that takes the accumulating path, which
        // is the only case whose matrix coefficients are folded totals.
        vector<FoldedAggTerm> clause_folded_terms;
        if (is_aggregate && !CanUseRowScopedFastPath(eval_const)) {
            std::unordered_set<idx_t> seen;
            for (idx_t t = 0; t < eval_const.variable_indices.size(); t++) {
                idx_t v = eval_const.variable_indices[t];
                if (v == DConstants::INVALID_INDEX || !seen.insert(v).second) {
                    continue;
                }
                FoldedAggTerm term;
                term.decide_var_idx = v;
                if (eval_const.row_coefficients[t].IsUniform()) {
                    term.has_unit = true;
                    term.unit = eval_const.row_coefficients[t].Get(0);
                } else if (t < eval_const.coefficient_labels.size()) {
                    term.has_unit = ParseLiteralCoefficient(eval_const.coefficient_labels[t], term.unit);
                }
                clause_folded_terms.push_back(term);
            }
        }

        if (is_aggregate) {
            if (!has_groups) {
                // FAST PATH: no `when`, no groups — one constraint summing all rows.
                ModelConstraint constr;
                bool row_scoped_fast = CanUseRowScopedFastPath(eval_const);

                if (row_scoped_fast) {
                    idx_t active_terms = 0;
                    for (auto v : eval_const.variable_indices) {
                        if (v != DConstants::INVALID_INDEX) active_terms++;
                    }
                    constr.indices.reserve(active_terms * num_rows);
                    constr.coefficients.reserve(active_terms * num_rows);
                    for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                        idx_t decide_var_idx = eval_const.variable_indices[term_idx];
                        if (decide_var_idx == DConstants::INVALID_INDEX) continue;
                        auto &col = eval_const.row_coefficients[term_idx];
                        for (idx_t row = 0; row < num_rows; row++) {
                            double coeff = col[row];
                            if (coeff == 0.0) continue;
                            int var_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                            constr.indices.push_back(var_idx);
                            constr.coefficients.push_back(coeff);
                        }
                    }
                } else {
                    // Keyed vars or repeated decide vars — need to
                    // accumulate because multiple (term, row) pairs can map
                    // to the same flat solver index. A term beside the reducers counts
                    // once: execution kept its coefficient on one row (CountOnce).
                    accum.BeginForDecisions(indexer, eval_const.variable_indices.size() * num_rows, written);
                    AccumulateClauseRow(eval_const, indexer, nullptr, 0, num_rows, accum);
                    accum.Flush(constr.indices, constr.coefficients);
                }

                // One value for the whole constraint: the optimizer's bound-side
                // window rewrite reduced a row-varying bound to the tightest one on
                // every kept row (RewriteRhsAggregates). No per-row check is needed here.
                D_ASSERT(RhsIsConstantWithinGroups(eval_const));
                double rhs = eval_const.rhs_values.Get(0);
                double lhs_offset = SumFixedAggregateLhsOffset(eval_const, nullptr, 0, num_rows, written);
                rhs -= lhs_offset;
                ApplyComparisonSense(constr, eval_const.comparison_type, rhs, lhs_integrality, lhs_offset);
                // F2 site 1: aggregate, ungrouped.
                StampConstraintProvenance(constr.provenance, eval_const, repair_group_id,
                                          DConstants::INVALID_INDEX, string(), lhs_offset);
                StampAggregateProvenance(constr.provenance, eval_const);
                constr.provenance.class_set = ClassSetOf(eval_const, 0);
                constr.provenance.avg_scaled = eval_const.avg_scaled;
                StampWeightLabels(constr.provenance);
                constr.provenance.folded_terms = clause_folded_terms;
                PushNormalizedConstraint(std::move(constr));

            } else {
                // UNIFIED PATH: a `when` and/or groups — build group→rows CSR once, emit one constraint per group
                auto &flat_rows = ClauseRows(eval_const);
                auto &offsets = eval_const.group_offsets;

                // Each group's bound is read from one of its own rows below; the
                // physical operator guarantees it is constant within the group.
                D_ASSERT(RhsIsConstantWithinGroups(eval_const));

                bool row_scoped_fast = CanUseRowScopedFastPath(eval_const);

                // For the slow (accumulating) path, pick a strategy once per constraint
                // and reuse the scratch storage across all groups.
                if (!row_scoped_fast) {
                    accum.BeginForDecisions(indexer, eval_const.variable_indices.size() *
                                                         (eval_const.num_groups > 0 ? num_rows / eval_const.num_groups
                                                                                    : num_rows),
                                             written);
                }

                for (idx_t g = 0; g < eval_const.num_groups; g++) {
                    idx_t g_begin = offsets[g];
                    idx_t g_end = offsets[g + 1];
                    if (g_begin == g_end) {
                        continue;
                    }
                    idx_t g_size = g_end - g_begin;
                    ModelConstraint constr;

                    if (row_scoped_fast) {
                        idx_t active_terms = 0;
                        for (auto v : eval_const.variable_indices) {
                            if (v != DConstants::INVALID_INDEX) active_terms++;
                        }
                        constr.indices.reserve(active_terms * g_size);
                        constr.coefficients.reserve(active_terms * g_size);
                        for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                            idx_t decide_var_idx = eval_const.variable_indices[term_idx];
                            if (decide_var_idx == DConstants::INVALID_INDEX) continue;
                            auto &col = eval_const.row_coefficients[term_idx];
                            for (idx_t k = g_begin; k < g_end; k++) {
                                idx_t row = flat_rows[k];
                                double coeff = col[row];
                                if (coeff == 0.0) continue;
                                int var_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                                constr.indices.push_back(var_idx);
                                constr.coefficients.push_back(coeff);
                            }
                        }
                    } else {
                        AccumulateClauseRow(eval_const, indexer, &flat_rows, g_begin, g_end, accum);
                        accum.Flush(constr.indices, constr.coefficients);
                    }

                    // Read this group's own bound. The guard above still proves every
                    // row agrees, so today this equals Get(0); once the RHS carries one
                    // value per group it is what lets each group see its own bound.
                    // Row 0 would be wrong twice over: it may belong to another group,
                    // and it may be excluded by WHEN.
                    double group_lhs_offset =
                        SumFixedAggregateLhsOffset(eval_const, &flat_rows, g_begin, g_end, written);
                    double group_rhs = eval_const.rhs_values.Get(flat_rows[g_begin]) - group_lhs_offset;
                    ApplyComparisonSense(constr, eval_const.comparison_type, group_rhs, lhs_integrality,
                                         group_lhs_offset);
                    // F2 site 2: grouped aggregate (`by` and/or `when`).
                    string group_label = g < eval_const.group_labels.size() ? eval_const.group_labels[g] : string();
                    StampConstraintProvenance(constr.provenance, eval_const, repair_group_id, g, group_label,
                                              group_lhs_offset);
                    StampAggregateProvenance(constr.provenance, eval_const);
                    constr.provenance.class_set = ClassSetOf(eval_const, g);
                    constr.provenance.avg_scaled = eval_const.avg_scaled;
                    StampWeightLabels(constr.provenance);
                    constr.provenance.folded_terms = clause_folded_terms;
                    PushNormalizedConstraint(std::move(constr));
                }
            }

        } else {
            // PER-ROW CONSTRAINT: one constraint per row
            idx_t per_row_active_terms = 0;
            for (auto v : eval_const.variable_indices) {
                if (v != DConstants::INVALID_INDEX) per_row_active_terms++;
            }
            idx_t num_terms = eval_const.variable_indices.size();
            for (idx_t row = 0; row < num_rows; row++) {
                // Skip rows excluded by `when` (row_group_ids with INVALID_INDEX)
                if (has_groups && eval_const.row_group_ids[row] == DConstants::INVALID_INDEX) {
                    continue;
                }
                ModelConstraint constr;
                constr.indices.reserve(per_row_active_terms);
                constr.coefficients.reserve(per_row_active_terms);
                // LHS terms that don't reference a DECIDE variable (row-varying
                // data or constants, e.g., the `+3` in `x + 3 <= K` or the `col`
                // in `x - col <= K`) must move to RHS, not be dropped. The
                // symbolic normalizer only canonicalizes aggregate constraints;
                // per-row LHS arrives here with constant/data terms still attached.
                double rhs_adjustment = 0.0;

                // Aggregate coefficients per Gurobi variable index. Multiple
                // lhs_terms can reference the same decision variable (e.g. after
                // multiply-over-add distribution: `K * (1 - x)` becomes a
                // constant plus a `-K*x` term, and any other `*x` term in the
                // same constraint produces a duplicate column index that
                // GRBaddconstr rejects). Sum-then-emit avoids duplicates.
                std::unordered_map<int, double> coeff_by_var;
                for (idx_t term_idx = 0; term_idx < num_terms; term_idx++) {
                    idx_t decide_var_idx = eval_const.variable_indices[term_idx];
                    double coeff = eval_const.row_coefficients[term_idx][row];

                    if (decide_var_idx != DConstants::INVALID_INDEX) {
                        if (coeff != 0.0) {
                            int var_idx = (int)indexer.Get(decide_var_idx, row);
                            AddCoefficient(coeff_by_var[var_idx], coeff, written);
                        }
                    } else {
                        // Constant / row-data LHS term: subtract from RHS.
                        AddCoefficient(rhs_adjustment, coeff, written);
                    }
                }
                for (auto &kv : coeff_by_var) {
                    if (kv.second != 0.0) {
                        constr.indices.push_back(kv.first);
                        constr.coefficients.push_back(kv.second);
                    }
                }

                double rhs = eval_const.rhs_values[row] - rhs_adjustment;
                ApplyComparisonSense(constr, eval_const.comparison_type, rhs, lhs_integrality, rhs_adjustment);
                // F2 site 3: per-row. The group label is stamped here for the same reason
                // the aggregate sites stamp it: a diagnosis over a grouped clause has to
                // name the group its failure sits in, not just the clause. It matters most
                // for a hard MIN/MAX whose bound is out of every row's reach — the rewrite
                // re-emits that group as ordinary per-row rows, so this is the only site
                // that sees it (see ilp_linearization.cpp, the NO_SOLUTION branch).
                idx_t row_group_key = has_groups ? eval_const.row_group_ids[row] : DConstants::INVALID_INDEX;
                string row_group_label =
                    row_group_key != DConstants::INVALID_INDEX &&
                            row_group_key < eval_const.group_labels.size()
                        ? eval_const.group_labels[row_group_key]
                        : string();
                StampConstraintProvenance(constr.provenance, eval_const, repair_group_id, row_group_key,
                                          row_group_label, rhs_adjustment);
                // A scoped easy MIN/MAX's member row stands for its `by` group's classes, a
                // `per K:` clause's row for its class; any other per-row row for its data row.
                constr.provenance.class_set = ClassSetOf(eval_const, has_groups ? row_group_key : 0);
                StampDataRow(constr.provenance, row);
                if (TryFoldPerRowBound(eval_const, row, constr)) {
                    continue;
                }
                PushNormalizedConstraint(std::move(constr));
            }
        }
    }

    // Build quadratic constraints from bilinear terms and/or POWER(expr, 2) groups.
    // These constraints were skipped in the linear loop above.
    // A single QuadraticConstraint can contain linear terms, bilinear Q entries,
    // and POWER outer-product Q entries — all accumulated together.

    // Helper: build QuadraticConstraint from one set of rows (aggregate or per-row).
    // lhs_integrality: precomputed from the user's EvaluatedConstraint; gates the
    // strict-inequality rewrite (`< K → <= K-1`) in the same way as the linear path.
    // lhs_constant: out, the constant part of the LHS folded into the bound (the linear
    // path's `lhs_offset`), for the stamp call. written: the clause, for a sum past a DOUBLE.
    auto BuildQuadraticConstraint = [&](const EvaluatedConstraint &ec, const vector<idx_t> &row_set, double rhs,
                                        LhsIntegrality lhs_integrality, double &lhs_constant,
                                        const string &written) -> SolverModel::QuadraticConstraint {
        SolverModel::QuadraticConstraint qc;
        std::unordered_map<int, double> linear_accum;
        std::unordered_map<uint64_t, double> q_accum;
        double rhs_adjustment = 0.0;

        // Linear terms from the constraint LHS
        for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
            idx_t decide_var_idx = ec.variable_indices[term_idx];
            if (decide_var_idx != DConstants::INVALID_INDEX) {
                for (idx_t row : row_set) {
                    double coeff = ec.row_coefficients[term_idx][row];
                    int var_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                    AddCoefficient(linear_accum[var_idx], coeff, written);
                }
            } else {
                for (idx_t row : row_set) {
                    AddCoefficient(rhs_adjustment, ec.row_coefficients[term_idx].Get(row), written);
                }
            }
        }

        // Bilinear terms: x_a * x_b → off-diagonal Q entries
        for (auto &bt : ec.bilinear_terms) {
            for (idx_t row : row_set) {
                double coeff = bt.row_coefficients[row];
                if (coeff == 0.0) continue;
                int flat_a = static_cast<int>(indexer.Get(bt.var_a, row));
                int flat_b = static_cast<int>(indexer.Get(bt.var_b, row));
                int q_row = std::max(flat_a, flat_b);
                int q_col = std::min(flat_a, flat_b);
                AddCoefficient(q_accum[pack_q_key(q_row, q_col)], coeff, written);
            }
        }

        // POWER groups: outer product Q = sign * A^T A for each group
        // GRBaddqconstr uses x^T Q x directly (no 1/2 factor),
        // so diagonal entries use factor 1.0 and off-diagonal use factor 2.0.
        for (auto &qg : ec.quadratic_groups) {
            double q_sign = qg.sign;
            idx_t num_q_terms = qg.variable_indices.size();

            struct VarCoeff { int flat_idx; double coeff; };
            vector<VarCoeff> row_terms;
            for (idx_t row : row_set) {
                row_terms.clear();
                double c_row = 0.0;  // Constant term contribution for this row

                for (idx_t t = 0; t < num_q_terms; t++) {
                    if (t >= qg.row_coefficients.size() || row >= qg.row_coefficients[t].Size()) {
                        continue;
                    }
                    double a = qg.row_coefficients[t].Get(row);
                    if (a == 0.0) continue;
                    idx_t decide_var_idx = qg.variable_indices[t];
                    if (decide_var_idx == DConstants::INVALID_INDEX) {
                        AddCoefficient(c_row, a, written);
                        continue;
                    }
                    int flat_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                    row_terms.push_back({flat_idx, a});
                }

                // Variable × variable → Q entries (outer product)
                for (idx_t i = 0; i < row_terms.size(); i++) {
                    for (idx_t j = 0; j <= i; j++) {
                        int ri = row_terms[i].flat_idx;
                        int rj = row_terms[j].flat_idx;
                        int q_r = std::max(ri, rj);
                        int q_c = std::min(ri, rj);
                        double val = q_sign * row_terms[i].coeff * row_terms[j].coeff;
                        if (i != j) val *= 2.0;  // Off-diagonal: 2*a_i*a_j
                        AddCoefficient(q_accum[pack_q_key(q_r, q_c)], val, written);
                    }
                }

                // Variable × constant → linear terms (cross-product from expansion)
                if (c_row != 0.0) {
                    for (auto &vt : row_terms) {
                        AddCoefficient(linear_accum[vt.flat_idx], q_sign * 2.0 * c_row * vt.coeff, written);
                    }
                }

                // Constant × constant → adjusts RHS
                AddCoefficient(rhs_adjustment, q_sign * c_row * c_row, written);
            }
        }

        // Flush linear terms
        for (auto &pair : linear_accum) {
            if (pair.second != 0.0) {
                qc.linear_indices.push_back(pair.first);
                qc.linear_coefficients.push_back(pair.second);
            }
        }
        // Flush Q terms
        for (auto &entry : q_accum) {
            if (entry.second != 0.0) {
                qc.q_rows.push_back(static_cast<int>(entry.first >> 32));
                qc.q_cols.push_back(static_cast<int>(entry.first & 0xFFFFFFFF));
                qc.q_coefficients.push_back(entry.second);
            }
        }

        // RHS and sense (move constant terms to RHS).
        // Mirror the linear-path strict-inequality rule: apply `< K → <= K-1`
        // (or `> K → >= K+1`) only when the LHS is provably integer-valued;
        // otherwise reject.
        double adjusted_rhs = rhs - rhs_adjustment;
        qc.rhs = adjusted_rhs;
        lhs_constant = rhs_adjustment;
        switch (ec.comparison_type) {
        case ExpressionType::COMPARE_LESSTHANOREQUALTO:
            qc.sense = '<'; break;
        case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
            qc.sense = '>'; break;
        case ExpressionType::COMPARE_EQUAL:
            qc.sense = '='; break;
        case ExpressionType::COMPARE_LESSTHAN:
            if (lhs_integrality != LhsIntegrality::INTEGER) {
                RejectStrictOnNonIntegerLhs(lhs_integrality, "<", "<=");
            }
            qc.sense = '<';
            qc.rhs = DecideStrictBound(false, adjusted_rhs);
            qc.provenance.strict = true;
            qc.provenance.typed_k = rhs;
            break;
        case ExpressionType::COMPARE_GREATERTHAN:
            if (lhs_integrality != LhsIntegrality::INTEGER) {
                RejectStrictOnNonIntegerLhs(lhs_integrality, ">", ">=");
            }
            qc.sense = '>';
            qc.rhs = DecideStrictBound(true, adjusted_rhs);
            qc.provenance.strict = true;
            qc.provenance.typed_k = rhs;
            break;
        default:
            throw InternalException("Unsupported comparison type in ILP model builder (quadratic)");
        }
        return qc;
    };

    phase.Next("model.quadratic_constraints");
    for (auto &eval_const : input.constraints) {
        if (eval_const.bilinear_terms.empty() && !eval_const.has_quadratic) {
            continue;  // Already handled as linear above
        }

        idx_t repair_group_id = eval_const.repair_group_id != DConstants::INVALID_INDEX
                                    ? eval_const.repair_group_id
                                    : static_cast<idx_t>(&eval_const - input.constraints.data());

        bool is_aggregate = eval_const.lhs_is_aggregate;
        bool has_groups = !eval_const.row_group_ids.empty();
        LhsIntegrality lhs_integrality = IsEvalConstraintLhsIntegerValued(eval_const);
        const string written = DecideWrittenClause(input, eval_const.source_clause_id);

        if (is_aggregate) {
            double rhs = eval_const.rhs_values.Empty() ? 0.0 : eval_const.rhs_values.Get(0);

            // Before the RHS was reduced per group this path read row 0's value for
            // every group with no check at all, so a row-varying bound was silently
            // wrong here rather than refused. Now the invariant holds by construction.
            D_ASSERT(RhsIsConstantWithinGroups(eval_const));

            if (!has_groups) {
                // Single aggregate: all rows
                vector<idx_t> all_rows(num_rows);
                std::iota(all_rows.begin(), all_rows.end(), 0);
                double lhs_constant;
                auto qc = BuildQuadraticConstraint(eval_const, all_rows, rhs, lhs_integrality, lhs_constant, written);
                // F2 site 4: quadratic aggregate, ungrouped.
                StampConstraintProvenance(qc.provenance, eval_const, repair_group_id, DConstants::INVALID_INDEX, string(),
                                          lhs_constant);
                StampAggregateProvenance(qc.provenance, eval_const);
                qc.provenance.class_set = ClassSetOf(eval_const, 0);
                model.quadratic_constraints.push_back(std::move(qc));
            } else {
                // Groups: one QuadraticConstraint per group, reuse CSR
                auto &flat_rows = ClauseRows(eval_const);
                auto &offsets = eval_const.group_offsets;
                for (idx_t g = 0; g < eval_const.num_groups; g++) {
                    idx_t g_begin = offsets[g];
                    idx_t g_end = offsets[g + 1];
                    if (g_begin == g_end) {
                        continue;
                    }
                    vector<idx_t> group_rows_slice(flat_rows.begin() + g_begin,
                                                   flat_rows.begin() + g_end);
                    // This group's own bound — see the linear grouped path for why row 0
                    // is the wrong row to read once the RHS varies by group.
                    double group_rhs =
                        eval_const.rhs_values.Empty() ? 0.0 : eval_const.rhs_values.Get(flat_rows[g_begin]);
                    double lhs_constant;
                    auto qc = BuildQuadraticConstraint(eval_const, group_rows_slice, group_rhs, lhs_integrality,
                                                       lhs_constant, written);
                    // F2 site 5: grouped quadratic aggregate.
                    string group_label = g < eval_const.group_labels.size() ? eval_const.group_labels[g] : string();
                    StampConstraintProvenance(qc.provenance, eval_const, repair_group_id, g, group_label, lhs_constant);
                    StampAggregateProvenance(qc.provenance, eval_const);
                    qc.provenance.class_set = ClassSetOf(eval_const, g);
                    model.quadratic_constraints.push_back(std::move(qc));
                }
            }
        } else {
            // Per-row: each row gets its own QuadraticConstraint
            for (idx_t row = 0; row < num_rows; row++) {
                if (has_groups && eval_const.row_group_ids[row] == DConstants::INVALID_INDEX) {
                    continue;
                }
                vector<idx_t> single_row{row};
                double rhs = eval_const.rhs_values.Get(row);
                double lhs_constant;
                auto qc = BuildQuadraticConstraint(eval_const, single_row, rhs, lhs_integrality, lhs_constant, written);
                // F2 site 6: quadratic per-row.
                idx_t row_group_key = has_groups ? eval_const.row_group_ids[row] : DConstants::INVALID_INDEX;
                StampConstraintProvenance(qc.provenance, eval_const, repair_group_id, row_group_key, string(),
                                          lhs_constant);
                qc.provenance.class_set = ClassSetOf(eval_const, has_groups ? row_group_key : 0);
                StampDataRow(qc.provenance, row);
                model.quadratic_constraints.push_back(std::move(qc));
            }
        }
    }

    // Append raw global constraints (for MIN/MAX objective linking, etc.).
    // Move the index/coefficient vectors out of `input` to avoid deep copies —
    // SolveModel() in ilp_solver.cpp does not read input.global_constraints after this call.
    phase.Next("model.global_rows");
    for (auto &raw : input.global_constraints) {
        ModelConstraint constr;
        constr.indices = std::move(raw.indices);
        constr.coefficients = std::move(raw.coefficients);
        constr.sense = raw.sense;
        constr.rhs = raw.rhs;
        // Raw rows include both structural links and user-facing composed outer
        // constraints, so their provenance is explicit on RawConstraint rather than
        // inferred from an EvaluatedConstraint position.
        constr.provenance.kind = raw.kind;
        constr.provenance.shape = raw.shape;
        constr.provenance.source_clause_id = raw.source_clause_id;
        constr.provenance.repair_group_id = raw.repair_group_id;
        constr.provenance.removal_group_id = raw.removal_group_id;
        // I4 (aggregate `<>`): carry the disjunction-binary column so the infeasible
        // removal dial groups the two global rows just like a per-row `<>`.
        constr.provenance.indicator_col = raw.indicator_col;
        constr.provenance.class_set = raw.class_set;
        // The display half. A per-row clause emitted through EvaluatedConstraint gets
        // these stamped from its group columns; one emitted in flat columns states them
        // itself, so a clause reads the same in a diagnosis whichever shape its rows took.
        constr.provenance.group_key = raw.group_key;
        constr.provenance.group_label = raw.group_label;
        constr.provenance.is_aggregate = raw.is_aggregate;
        constr.provenance.rhs_mechanism_offset = raw.rhs_mechanism_offset;
        constr.provenance.rhs_label = raw.rhs_label;
        constr.provenance.strict = raw.strict;
        constr.provenance.typed_k = raw.typed_k;
        PushNormalizedConstraint(std::move(constr));
    }

    // Conditional rows the chosen backend states itself. Whatever `LowerDecideConstructs`
    // did not lower arrives here still conditioned; everything it did lower is an ordinary
    // row in `global_constraints` above. A model holds one or the other for a given
    // clause, never both. The row half carries the same provenance a matrix row does, so
    // this only re-homes it.
    model.indicator_constraints.reserve(input.indicator_constraints.size());
    for (auto &spec : input.indicator_constraints) {
        SolverModel::IndicatorConstraint ic;
        ic.binary_column = spec.binary_column;
        ic.binary_value = spec.binary_value;
        ic.indices = std::move(spec.row.indices);
        ic.coefficients = std::move(spec.row.coefficients);
        ic.sense = spec.row.sense;
        ic.rhs = spec.row.rhs;
        ic.provenance.kind = spec.row.kind;
        ic.provenance.shape = spec.row.shape;
        ic.provenance.source_clause_id = spec.row.source_clause_id;
        ic.provenance.repair_group_id = spec.row.repair_group_id;
        ic.provenance.removal_group_id = spec.row.removal_group_id;
        ic.provenance.indicator_col = spec.row.indicator_col;
        ic.provenance.group_key = spec.row.group_key;
        ic.provenance.group_label = spec.row.group_label;
        ic.provenance.is_aggregate = spec.row.is_aggregate;
        ic.provenance.rhs_mechanism_offset = spec.row.rhs_mechanism_offset;
        ic.provenance.rhs_label = spec.row.rhs_label;
        model.indicator_constraints.push_back(std::move(ic));
    }

    phase.Next("model.general_constraints");
    model.general_constraints.reserve(input.general_constraints.size());
    for (auto &spec : input.general_constraints) {
        SolverModel::GeneralConstraint gc;
        gc.kind = spec.kind;
        gc.result_column = spec.result_column;
        gc.argument_columns = std::move(spec.argument_columns);
        gc.provenance.kind = ConstraintKind::STRUCTURAL;
        gc.provenance.source_clause_id = spec.source_clause_id;
        gc.provenance.repair_group_id = spec.repair_group_id;
        gc.provenance.removal_group_id = spec.removal_group_id;
        model.general_constraints.push_back(std::move(gc));
    }

    //===--------------------------------------------------------------------===//
    // 4. Sanity checks
    //===--------------------------------------------------------------------===//

    phase.Next("model.sanity_checks");

    // Every number below was read finite or merged by AddCoefficient, so one that is not finite is a reach or a
    // Big-M DeciDB derived past a DOUBLE: the user's error names the clause it belongs to. A column belongs to the
    // first row that reads it, else to the objective. The structural checks stay internal: they are DeciDB's bugs.
    auto too_large = [&](idx_t source_clause_id) {
        return InvalidInputException(
            DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, DecideWrittenClause(input, source_clause_id)));
    };
    auto column_clause = [&](idx_t col) {
        for (auto &constr : model.constraints) {
            if (std::find(constr.indices.begin(), constr.indices.end(), (int)col) != constr.indices.end()) {
                return constr.provenance.source_clause_id;
            }
        }
        return DConstants::INVALID_INDEX;
    };
    for (idx_t i = 0; i < total_vars; i++) {
        if (!std::isfinite(model.col_lower[i]) || !std::isfinite(model.col_upper[i])) {
            throw too_large(column_clause(i));
        }
        if (!std::isfinite(model.obj_coeffs[i])) {
            throw too_large(DConstants::INVALID_INDEX);
        }
    }

    for (idx_t c = 0; c < model.constraints.size(); c++) {
        auto &constr = model.constraints[c];
        if (constr.indices.size() != constr.coefficients.size()) {
            throw InternalException("Constraint %llu: indices/coefficients size mismatch", c);
        }
        for (idx_t j = 0; j < constr.indices.size(); j++) {
            if ((idx_t)constr.indices[j] >= total_vars) {
                throw InternalException("Constraint %llu: variable index %d out of range (>= %llu)",
                                        c, constr.indices[j], total_vars);
            }
            if (!std::isfinite(constr.coefficients[j])) {
                throw too_large(constr.provenance.source_clause_id);
            }
        }
        if (!std::isfinite(constr.rhs) && !std::isinf(constr.rhs)) {
            throw InternalException("Constraint %llu: RHS is NaN", c);
        }
    }

    for (idx_t k = 0; k < model.q_vals.size(); k++) {
        if (model.q_rows[k] < 0 || (idx_t)model.q_rows[k] >= total_vars ||
            model.q_cols[k] < 0 || (idx_t)model.q_cols[k] >= total_vars) {
            throw InternalException("Q matrix entry %llu: index out of range (row=%d, col=%d, total_vars=%llu)",
                                    k, model.q_rows[k], model.q_cols[k], total_vars);
        }
        if (model.q_rows[k] < model.q_cols[k]) {
            throw InternalException("Q matrix entry %llu: not lower triangle (row=%d < col=%d)",
                                    k, model.q_rows[k], model.q_cols[k]);
        }
        if (!std::isfinite(model.q_vals[k])) {
            throw too_large(DConstants::INVALID_INDEX);
        }
    }

    // Moved last: every row above names its clause from them (DecideWrittenClause).
    model.constraint_sources = std::move(input.constraint_sources);
    return model;
}

vector<ColumnProvenance> BuildColumnProvenance(const VarIndexer &indexer,
                                               const vector<string> &var_labels,
                                               const vector<bool> &var_is_aux,
                                               const vector<string> &global_var_labels) {
    // Every flat column defaults to an unnamed global-block auxiliary; the row /
    // keyed blocks are then overwritten below. Columns in [global_block_start,
    // total_vars) (internal global auxiliaries) keep the default unless named via
    // global_var_labels (an aggregate `<>` indicator — see below).
    vector<ColumnProvenance> provenance(indexer.total_vars);

    idx_t num_decide_vars = var_labels.size();
    for (idx_t var = 0; var < num_decide_vars; var++) {
        ColumnKind kind = (var < var_is_aux.size() && var_is_aux[var]) ? ColumnKind::AUX
                                                                       : ColumnKind::USER;
        // Inverting Get(var, row) over all rows visits every column this variable
        // owns. Keyed variables collapse multiple rows onto one column;
        // the repeated writes are idempotent (same var/label, instance pinned to
        // the class id via Get's mapping).
        for (idx_t row = 0; row < indexer.num_rows; row++) {
            idx_t col = indexer.Get(var, row);
            if (col >= provenance.size()) {
                continue; // defensive: never index past the flat array
            }
            ColumnProvenance &slot = provenance[col];
            slot.kind = kind;
            slot.label = var_labels[var];
            slot.decide_var_idx = var;
            // For keyed vars, the instance is the class id (col - base);
            // for row-scoped vars it is the row; a scalar has exactly one instance.
            switch (indexer.var_scope[var]) {
            case DecideVarScope::KEYED:
                slot.instance = col - indexer.keyed_var_base[var];
                break;
            case DecideVarScope::GLOBAL:
                slot.instance = 0;
                break;
            default:
                slot.instance = row;
                break;
            }
        }
    }
    // Name any labeled global-block columns (today: aggregate `<>` indicators).
    // global_var_labels is parallel to the global block at [global_block_start, ...);
    // empty entries leave the default unnamed GLOBAL_AUX. Kind stays GLOBAL_AUX —
    // only the label is consumed (the removal dial reads columns[indicator_col].label).
    for (idx_t g = 0; g < global_var_labels.size(); g++) {
        if (global_var_labels[g].empty()) {
            continue;
        }
        idx_t col = indexer.global_block_start + g;
        if (col < provenance.size()) {
            provenance[col].label = global_var_labels[g];
        }
    }
    return provenance;
}

//===----------------------------------------------------------------------===//
// SparseCoeffAccumulator
//===----------------------------------------------------------------------===//

void SparseCoeffAccumulator::BeginDense(idx_t total_vars, const string &written_p) {
    written = &written_p;
    use_dense = true;
    if (dense.size() < total_vars) {
        dense.assign(total_vars, 0.0);
    }
    // Cells previously written were already zeroed in Flush(); no full re-zero needed.
    touched.clear();
    pairs.clear();
}

void SparseCoeffAccumulator::BeginSparse(idx_t expected_size, const string &written_p) {
    written = &written_p;
    use_dense = false;
    pairs.clear();
    if (expected_size > pairs.capacity()) {
        pairs.reserve(expected_size);
    }
    touched.clear();
}

void SparseCoeffAccumulator::BeginForDecisions(const VarIndexer &indexer, idx_t expected_size,
                                               const string &written_p) {
    constexpr idx_t DENSE_CAP = 1u << 20;
    if (indexer.global_block_start <= DENSE_CAP) {
        BeginDense(indexer.global_block_start, written_p);
    } else {
        BeginSparse(expected_size, written_p);
    }
}

void AccumulateClauseRow(const EvaluatedConstraint &ec, const VarIndexer &indexer, const vector<idx_t> *rows,
                         idx_t begin, idx_t end, SparseCoeffAccumulator &accum) {
    for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
        idx_t decide_var_idx = ec.variable_indices[term_idx];
        if (decide_var_idx == DConstants::INVALID_INDEX) {
            continue;
        }
        auto &col = ec.row_coefficients[term_idx];
        for (idx_t k = begin; k < end; k++) {
            idx_t row = rows ? (*rows)[k] : k;
            double coeff = col.Get(row);
            if (coeff != 0.0) {
                accum.Add(static_cast<int>(indexer.Get(decide_var_idx, row)), coeff);
            }
        }
    }
}

void SparseCoeffAccumulator::Flush(vector<int> &out_indices, vector<double> &out_coefficients) {
    if (use_dense) {
        out_indices.reserve(out_indices.size() + touched.size());
        out_coefficients.reserve(out_coefficients.size() + touched.size());
        for (int idx : touched) {
            double v = dense[idx];
            if (v != 0.0) {
                out_indices.push_back(idx);
                out_coefficients.push_back(v);
            }
            dense[idx] = 0.0;
        }
        touched.clear();
    } else {
        if (pairs.empty()) {
            return;
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](const std::pair<int, double> &a, const std::pair<int, double> &b) {
                      return a.first < b.first;
                  });
        // Merge consecutive equal indices, drop zeros.
        idx_t n = pairs.size();
        idx_t i = 0;
        while (i < n) {
            int idx = pairs[i].first;
            double sum = pairs[i].second;
            idx_t j = i + 1;
            while (j < n && pairs[j].first == idx) {
                AddCoefficient(sum, pairs[j].second, *written);
                j++;
            }
            if (sum != 0.0) {
                out_indices.push_back(idx);
                out_coefficients.push_back(sum);
            }
            i = j;
        }
        pairs.clear();
    }
}

} // namespace duckdb
