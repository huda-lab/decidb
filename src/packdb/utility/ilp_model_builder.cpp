#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

// Shared logic for Build and BuildRef — populates all fields except entity data source
static void BuildVarIndexerCommon(VarIndexer &idx, const SolverInput &input,
                                   const vector<EntityMapping> &entity_mappings) {
    idx.num_rows = input.num_rows;
    idx_t num_decide_vars = input.num_decide_vars;

    idx.is_entity_scoped.resize(num_decide_vars, false);
    idx.row_var_offset.resize(num_decide_vars, DConstants::INVALID_INDEX);
    idx.entity_var_base.resize(num_decide_vars, DConstants::INVALID_INDEX);
    idx.var_entity_mapping_idx.resize(num_decide_vars, DConstants::INVALID_INDEX);

    // Classify variables and compute offsets
    idx_t row_var_count = 0;
    for (idx_t v = 0; v < num_decide_vars; v++) {
        bool scoped = !input.variable_entity_scope.empty() &&
                      v < input.variable_entity_scope.size() &&
                      input.variable_entity_scope[v] != DConstants::INVALID_INDEX;
        idx.is_entity_scoped[v] = scoped;
        if (!scoped) {
            idx.row_var_offset[v] = row_var_count;
            row_var_count++;
        }
    }
    idx.num_row_vars = row_var_count;

    // Row block: num_rows * num_row_vars
    idx.entity_block_start = input.num_rows * row_var_count;

    // Entity block: assign base offsets per entity-scoped variable
    idx_t entity_offset = 0;
    for (idx_t v = 0; v < num_decide_vars; v++) {
        if (!idx.is_entity_scoped[v]) {
            continue;
        }
        idx_t scope_idx = input.variable_entity_scope[v];
        D_ASSERT(scope_idx < entity_mappings.size());
        idx.var_entity_mapping_idx[v] = scope_idx;
        idx.entity_var_base[v] = idx.entity_block_start + entity_offset;
        entity_offset += entity_mappings[scope_idx].num_entities;
    }

    idx.global_block_start = idx.entity_block_start + entity_offset;
    idx.total_vars = idx.global_block_start + input.num_global_vars;
}

VarIndexer VarIndexer::Build(const SolverInput &input) {
    VarIndexer idx;
    // Own a copy so this VarIndexer can outlive the SolverInput
    idx.entity_mappings_owned = input.entity_mappings;
    idx.entity_mappings_ref = nullptr;
    BuildVarIndexerCommon(idx, input, idx.entity_mappings_owned);
    return idx;
}

VarIndexer VarIndexer::BuildRef(const SolverInput &input) {
    VarIndexer idx;
    // Reference without copying — caller must ensure SolverInput outlives this VarIndexer
    idx.entity_mappings_ref = &input.entity_mappings;
    BuildVarIndexerCommon(idx, input, input.entity_mappings);
    return idx;
}

void BuildGroupCSR(const vector<idx_t> &row_group_ids,
                   idx_t num_groups,
                   vector<idx_t> &offsets,
                   vector<idx_t> &flat_rows) {
    if (row_group_ids.empty() || num_groups == 0) {
        offsets.clear();
        flat_rows.clear();
        return;
    }
    if (!offsets.empty()) {
        // Already built; reuse.
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

SolverModel SolverModel::Build(SolverInput &input, const VarIndexer &indexer) {
    SolverModel model;

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
            per_var_upper[var] = 1e30;
        } else if (logical_type == LogicalType::BOOLEAN) {
            per_var_binary[var] = true;
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1.0;
        } else {
            // INTEGER / BIGINT or default
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1e30;
        }
    }

    // Merge with explicit bounds from input (intersect with type-based defaults)
    for (idx_t var = 0; var < num_decide_vars; var++) {
        if (var < input.lower_bounds.size()) {
            per_var_lower[var] = std::max(per_var_lower[var], input.lower_bounds[var]);
        }
        if (var < input.upper_bounds.size()) {
            per_var_upper[var] = std::min(per_var_upper[var], input.upper_bounds[var]);
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
            idx_t var_idx;
            if (!indexer.is_entity_scoped[var]) {
                var_idx = inst * indexer.num_row_vars + indexer.row_var_offset[var];
            } else {
                var_idx = indexer.entity_var_base[var] + inst;
            }
            model.col_lower[var_idx] = per_var_lower[var];
            model.col_upper[var_idx] = per_var_upper[var];
            model.is_integer[var_idx] = !(input.variable_types[var] == LogicalType::DOUBLE ||
                                          input.variable_types[var] == LogicalType::FLOAT);
            model.is_binary[var_idx] = per_var_binary[var];
        }
    }

    // Append global auxiliary variables after row+entity blocks
    for (idx_t g = 0; g < input.num_global_vars; g++) {
        idx_t var_idx = indexer.global_block_start + g;
        auto gtype = input.global_variable_types[g];
        model.col_lower[var_idx] = input.global_lower_bounds[g];
        model.col_upper[var_idx] = input.global_upper_bounds[g];
        model.is_integer[var_idx] = !(gtype == LogicalType::DOUBLE || gtype == LogicalType::FLOAT);
        model.is_binary[var_idx] = (gtype == LogicalType::BOOLEAN);
    }

    //===--------------------------------------------------------------------===//
    // 2. Set up objective function
    //===--------------------------------------------------------------------===//

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
            auto &col = input.objective_coefficients[term_idx];
            for (idx_t row = 0; row < num_rows; row++) {
                if (row >= col.Size()) break;
                idx_t var_idx = indexer.Get(decide_var_idx, row);
                // Use += because entity-scoped vars: multiple rows map to same solver var
                model.obj_coeffs[var_idx] += col.Get(row);
            }
        }
    }

    //===--------------------------------------------------------------------===//
    // 2b. Build quadratic objective (Q matrix) if present
    //===--------------------------------------------------------------------===//

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

            // Determine convexity: non-convex when sign and sense conflict
            // MAXIMIZE + PSD (sign>0) or MINIMIZE + NSD (sign<0) → non-convex
            bool is_maximize = (input.sense == DecideSense::MAXIMIZE);
            model.nonconvex_quadratic = (q_sign > 0.0) == is_maximize;

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
                        // GRBaddqpterms adds raw x_i*x_j terms (no 1/2 factor).
                        // Diagonal: coeff_i^2. Off-diagonal: 2*coeff_i*coeff_j
                        // (symmetry — we store lower triangle only).
                        double val = q_sign * row_terms[i].coeff * row_terms[j].coeff;
                        if (i != j) val *= 2.0;
                        q_map[pack_q_key(q_row, q_col)] += val;
                    }
                }

                // Handle constant term contributions to linear objective
                double c_row = 0.0;
                for (idx_t t = 0; t < num_q_terms; t++) {
                    if (t < input.quadratic_inner_coefficients.size() &&
                        row < input.quadratic_inner_coefficients[t].Size() &&
                        input.quadratic_inner_variable_indices[t] == DConstants::INVALID_INDEX) {
                        c_row += input.quadratic_inner_coefficients[t].Get(row);
                    }
                }
                if (c_row != 0.0) {
                    for (auto &vt : row_terms) {
                        model.obj_coeffs[vt.flat_idx] += q_sign * 2.0 * c_row * vt.coeff;
                    }
                }
            }
        }

        // Bilinear objective terms: off-diagonal Q entries for x_a * x_b
        // GRBaddqpterms adds raw terms (no 1/2 factor).
        // For SUM(c * x_a * x_b): Q[max(a,b), min(a,b)] += c
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
                    q_map[pack_q_key(q_row, q_col)] += coeff;
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
    auto IsRealType = [](const LogicalType &t) {
        return t == LogicalType::DOUBLE || t == LogicalType::FLOAT;
    };
    auto IsEvalConstraintLhsIntegerValued = [&](const EvaluatedConstraint &ec) -> bool {
        for (idx_t i = 0; i < ec.variable_indices.size(); i++) {
            idx_t vi = ec.variable_indices[i];
            if (vi == DConstants::INVALID_INDEX) continue;
            if (IsRealType(input.variable_types[vi])) return false;
            if (!ec.row_coefficients[i].AllIntegral()) return false;
        }
        for (const auto &bt : ec.bilinear_terms) {
            if (IsRealType(input.variable_types[bt.var_a])) return false;
            if (IsRealType(input.variable_types[bt.var_b])) return false;
            if (!bt.row_coefficients.AllIntegral()) return false;
        }
        for (const auto &qg : ec.quadratic_groups) {
            for (idx_t i = 0; i < qg.variable_indices.size(); i++) {
                idx_t vi = qg.variable_indices[i];
                if (vi == DConstants::INVALID_INDEX) continue;
                if (IsRealType(input.variable_types[vi])) return false;
                if (!qg.row_coefficients[i].AllIntegral()) return false;
            }
        }
        return true;
    };

    // Helper: apply comparison sense to a constraint.
    // Strict `<` / `>` require a provably integer-valued LHS; otherwise reject.
    auto ApplyComparisonSense = [](ModelConstraint &constr, ExpressionType cmp, double rhs,
                                   bool lhs_is_integer) {
        if (cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
            constr.sense = '>'; constr.rhs = rhs;
        } else if (cmp == ExpressionType::COMPARE_GREATERTHAN) {
            if (!lhs_is_integer) {
                throw InvalidInputException(
                    "Strict inequality '>' is not supported when the left-hand side "
                    "involves a REAL variable or a non-integer coefficient. Use '>=' instead.");
            }
            constr.sense = '>'; constr.rhs = std::floor(rhs) + 1.0;
        } else if (cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
            constr.sense = '<'; constr.rhs = rhs;
        } else if (cmp == ExpressionType::COMPARE_LESSTHAN) {
            if (!lhs_is_integer) {
                throw InvalidInputException(
                    "Strict inequality '<' is not supported when the left-hand side "
                    "involves a REAL variable or a non-integer coefficient. Use '<=' instead.");
            }
            constr.sense = '<'; constr.rhs = std::ceil(rhs) - 1.0;
        } else if (cmp == ExpressionType::COMPARE_EQUAL) {
            constr.sense = '='; constr.rhs = rhs;
        } else {
            throw InternalException("Unsupported comparison type in ILP model builder");
        }
    };

    // Drop constraints whose LHS reduced to 0 (all coefficients cancelled or
    // all variables absorbed into column bounds). A tautology is skipped; a
    // violated constant constraint is rejected early so the solver doesn't
    // have to diagnose it.
    auto PushNormalizedConstraint = [&](ModelConstraint &&constr) {
        if (!constr.indices.empty()) {
            model.constraints.push_back(std::move(constr));
            return;
        }
        constexpr double EPS = 1e-9;
        bool violated = false;
        if (constr.sense == '<') {
            violated = constr.rhs < -EPS;
        } else if (constr.sense == '>') {
            violated = constr.rhs > EPS;
        } else { // '='
            violated = std::abs(constr.rhs) > EPS;
        }
        if (violated) {
            throw InvalidInputException(
                "DECIDE optimization is infeasible: a constraint reduces to "
                "0 %c %g after absorbing variable bounds and cancelling terms.",
                constr.sense, constr.rhs);
        }
        // Tautology (0 <= k>=0, 0 >= k<=0, 0 = 0) — drop.
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

    for (auto &eval_const : input.constraints) {
        // Skip constraints with quadratic/bilinear terms — handled in quadratic section below
        if (!eval_const.bilinear_terms.empty() || eval_const.has_quadratic) {
            continue;
        }

        bool is_aggregate = eval_const.lhs_is_aggregate;
        bool has_groups = !eval_const.row_group_ids.empty();
        bool lhs_is_integer = IsEvalConstraintLhsIntegerValued(eval_const);

        // Detect whether this constraint can bypass the hash-map accumulator:
        // * every term must reference a row-scoped decide variable (entity-scoped
        //   vars collapse many rows to the same solver index — need dedup), AND
        // * no decide variable appears in more than one term (otherwise distinct
        //   terms at the same row collide on the same flat index).
        auto CanUseRowScopedFastPath = [&](const EvaluatedConstraint &ec) -> bool {
            std::unordered_set<idx_t> seen_vars;
            for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
                idx_t v = ec.variable_indices[term_idx];
                if (v == DConstants::INVALID_INDEX) continue;
                if (indexer.is_entity_scoped[v]) return false;
                if (!seen_vars.insert(v).second) return false;
            }
            return true;
        };

        if (is_aggregate) {
            if (!has_groups) {
                // FAST PATH: no WHEN, no PER — one constraint summing all rows.
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
                    // Entity-scoped vars or repeated decide vars — need to
                    // accumulate because multiple (term, row) pairs can map
                    // to the same flat solver index.
                    std::unordered_map<int, double> coeff_accum;
                    for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                        idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                        if (decide_var_idx != DConstants::INVALID_INDEX) {
                            for (idx_t row = 0; row < num_rows; row++) {
                                double coeff = eval_const.row_coefficients[term_idx][row];
                                int var_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                                coeff_accum[var_idx] += coeff;
                            }
                        }
                    }

                    for (auto &pair : coeff_accum) {
                        if (pair.second != 0.0) {
                            constr.indices.push_back(pair.first);
                            constr.coefficients.push_back(pair.second);
                        }
                    }
                }

                double rhs = eval_const.rhs_values.Get(0);
                if (!eval_const.rhs_values.IsUniform()) {
                    for (idx_t r = 1; r < eval_const.rhs_values.Size(); r++) {
                        if (eval_const.rhs_values.Get(r) != rhs) {
                            throw InvalidInputException(
                                "Aggregate constraint (SUM/AVG) requires a scalar right-hand side, "
                                "but the RHS evaluates to different values per row (row 0 = %g, row %llu = %g). "
                                "This can happen with correlated subqueries. "
                                "For per-row bounds, use a per-row constraint (e.g., x <= column) instead.",
                                rhs, r, eval_const.rhs_values.Get(r));
                        }
                    }
                }
                ApplyComparisonSense(constr, eval_const.comparison_type, rhs, lhs_is_integer);
                PushNormalizedConstraint(std::move(constr));

            } else {
                // UNIFIED PATH: WHEN and/or PER — build group→rows CSR once, emit one constraint per group
                BuildGroupCSR(eval_const.row_group_ids, eval_const.num_groups,
                              eval_const.group_offsets, eval_const.group_row_ids);
                auto &offsets = eval_const.group_offsets;
                auto &flat_rows = eval_const.group_row_ids;

                double rhs = eval_const.rhs_values.Get(0);
                if (!eval_const.rhs_values.IsUniform()) {
                    for (idx_t r = 1; r < eval_const.rhs_values.Size(); r++) {
                        if (eval_const.rhs_values.Get(r) != rhs) {
                            throw InvalidInputException(
                                "Aggregate PER constraint requires a scalar right-hand side, "
                                "but the RHS evaluates to different values per row (row 0 = %g, row %llu = %g). "
                                "This can happen with correlated subqueries.",
                                rhs, r, eval_const.rhs_values.Get(r));
                        }
                    }
                }

                bool row_scoped_fast = CanUseRowScopedFastPath(eval_const);

                // For the slow (accumulating) path, pick a strategy once per constraint
                // and reuse the scratch storage across all groups.
                // The decide-variable flat indices span [0, global_block_start),
                // so size the dense accumulator to that — tighter than total_vars.
                SparseCoeffAccumulator accum;
                if (!row_scoped_fast) {
                    constexpr idx_t DENSE_CAP = 1u << 20; // ~8MB scratch ceiling
                    idx_t decide_var_index_span = indexer.global_block_start;
                    if (decide_var_index_span <= DENSE_CAP) {
                        accum.BeginDense(decide_var_index_span);
                    } else {
                        idx_t expected = eval_const.variable_indices.size() *
                                         (eval_const.num_groups > 0 ? num_rows / eval_const.num_groups : num_rows);
                        accum.BeginSparse(expected);
                    }
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
                        for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                            idx_t decide_var_idx = eval_const.variable_indices[term_idx];
                            if (decide_var_idx == DConstants::INVALID_INDEX) continue;
                            auto &col = eval_const.row_coefficients[term_idx];
                            for (idx_t k = g_begin; k < g_end; k++) {
                                idx_t row = flat_rows[k];
                                double coeff = col.Get(row);
                                int var_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                                accum.Add(var_idx, coeff);
                            }
                        }
                        accum.Flush(constr.indices, constr.coefficients);
                    }

                    ApplyComparisonSense(constr, eval_const.comparison_type, rhs, lhs_is_integer);
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
                // Skip rows excluded by WHEN (row_group_ids with INVALID_INDEX)
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
                            coeff_by_var[var_idx] += coeff;
                        }
                    } else {
                        // Constant / row-data LHS term: subtract from RHS.
                        rhs_adjustment += coeff;
                    }
                }
                for (auto &kv : coeff_by_var) {
                    if (kv.second != 0.0) {
                        constr.indices.push_back(kv.first);
                        constr.coefficients.push_back(kv.second);
                    }
                }

                double rhs = eval_const.rhs_values[row] - rhs_adjustment;
                ApplyComparisonSense(constr, eval_const.comparison_type, rhs, lhs_is_integer);
                PushNormalizedConstraint(std::move(constr));
            }
        }
    }

    // Build quadratic constraints from bilinear terms and/or POWER(expr, 2) groups.
    // These constraints were skipped in the linear loop above.
    // A single QuadraticConstraint can contain linear terms, bilinear Q entries,
    // and POWER outer-product Q entries — all accumulated together.

    // Helper: build QuadraticConstraint from one set of rows (aggregate or per-row).
    // lhs_is_integer: precomputed from the user's EvaluatedConstraint; gates the
    // strict-inequality rewrite (`< K → <= K-1`) in the same way as the linear path.
    auto BuildQuadraticConstraint = [&](const EvaluatedConstraint &ec,
                                        const vector<idx_t> &row_set,
                                        double rhs,
                                        bool lhs_is_integer) -> SolverModel::QuadraticConstraint {
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
                    linear_accum[var_idx] += coeff;
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
                q_accum[pack_q_key(q_row, q_col)] += coeff;
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
                        c_row += a;
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
                        q_accum[pack_q_key(q_r, q_c)] += val;
                    }
                }

                // Variable × constant → linear terms (cross-product from expansion)
                if (c_row != 0.0) {
                    for (auto &vt : row_terms) {
                        linear_accum[vt.flat_idx] += q_sign * 2.0 * c_row * vt.coeff;
                    }
                }

                // Constant × constant → adjusts RHS
                rhs_adjustment += q_sign * c_row * c_row;
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
        switch (ec.comparison_type) {
        case ExpressionType::COMPARE_LESSTHANOREQUALTO:
            qc.sense = '<'; break;
        case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
            qc.sense = '>'; break;
        case ExpressionType::COMPARE_EQUAL:
            qc.sense = '='; break;
        case ExpressionType::COMPARE_LESSTHAN:
            if (!lhs_is_integer) {
                throw InvalidInputException(
                    "Strict inequality '<' is not supported when the left-hand side "
                    "involves a REAL variable or a non-integer coefficient. Use '<=' instead.");
            }
            qc.sense = '<'; qc.rhs = std::ceil(adjusted_rhs) - 1.0; break;
        case ExpressionType::COMPARE_GREATERTHAN:
            if (!lhs_is_integer) {
                throw InvalidInputException(
                    "Strict inequality '>' is not supported when the left-hand side "
                    "involves a REAL variable or a non-integer coefficient. Use '>=' instead.");
            }
            qc.sense = '>'; qc.rhs = std::floor(adjusted_rhs) + 1.0; break;
        default:
            throw InternalException("Unsupported comparison type in ILP model builder (quadratic)");
        }
        return qc;
    };

    for (auto &eval_const : input.constraints) {
        if (eval_const.bilinear_terms.empty() && !eval_const.has_quadratic) {
            continue;  // Already handled as linear above
        }

        bool is_aggregate = eval_const.lhs_is_aggregate;
        bool has_groups = !eval_const.row_group_ids.empty();
        bool lhs_is_integer = IsEvalConstraintLhsIntegerValued(eval_const);

        if (is_aggregate) {
            double rhs = eval_const.rhs_values.Empty() ? 0.0 : eval_const.rhs_values.Get(0);

            if (!has_groups) {
                // Single aggregate: all rows
                vector<idx_t> all_rows(num_rows);
                std::iota(all_rows.begin(), all_rows.end(), 0);
                model.quadratic_constraints.push_back(
                    BuildQuadraticConstraint(eval_const, all_rows, rhs, lhs_is_integer));
            } else {
                // PER groups: one QuadraticConstraint per group, reuse CSR
                BuildGroupCSR(eval_const.row_group_ids, eval_const.num_groups,
                              eval_const.group_offsets, eval_const.group_row_ids);
                auto &offsets = eval_const.group_offsets;
                auto &flat_rows = eval_const.group_row_ids;
                for (idx_t g = 0; g < eval_const.num_groups; g++) {
                    idx_t g_begin = offsets[g];
                    idx_t g_end = offsets[g + 1];
                    if (g_begin == g_end) {
                        continue;
                    }
                    vector<idx_t> group_rows_slice(flat_rows.begin() + g_begin,
                                                   flat_rows.begin() + g_end);
                    model.quadratic_constraints.push_back(
                        BuildQuadraticConstraint(eval_const, group_rows_slice, rhs, lhs_is_integer));
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
                model.quadratic_constraints.push_back(
                    BuildQuadraticConstraint(eval_const, single_row, rhs, lhs_is_integer));
            }
        }
    }

    // Append raw global constraints (for MIN/MAX objective linking, etc.).
    // Move the index/coefficient vectors out of `input` to avoid deep copies —
    // SolveModel() in ilp_solver.cpp does not read input.global_constraints after this call.
    for (auto &raw : input.global_constraints) {
        ModelConstraint constr;
        constr.indices = std::move(raw.indices);
        constr.coefficients = std::move(raw.coefficients);
        constr.sense = raw.sense;
        constr.rhs = raw.rhs;
        model.constraints.push_back(std::move(constr));
    }

    //===--------------------------------------------------------------------===//
    // 4. Sanity checks
    //===--------------------------------------------------------------------===//

    for (idx_t i = 0; i < total_vars; i++) {
        if (!std::isfinite(model.col_lower[i]) || !std::isfinite(model.col_upper[i])) {
            throw InternalException("Column bounds invalid at col %llu: [%f, %f]",
                                    i, model.col_lower[i], model.col_upper[i]);
        }
        if (model.col_lower[i] > model.col_upper[i]) {
            throw InvalidInputException(
                "DECIDE optimization is infeasible: contradictory bounds on variable %llu "
                "(lower=%f > upper=%f). Check your SUCH THAT constraints for conflicting bounds.",
                i, model.col_lower[i], model.col_upper[i]);
        }
        if (!std::isfinite(model.obj_coeffs[i])) {
            throw InternalException("Objective coefficient not finite at col %llu: %f",
                                    i, model.obj_coeffs[i]);
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
                throw InternalException("Constraint %llu: coefficient not finite at position %llu: %f",
                                        c, j, constr.coefficients[j]);
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
            throw InternalException("Q matrix entry %llu: value not finite: %f", k, model.q_vals[k]);
        }
    }

    return model;
}

//===----------------------------------------------------------------------===//
// SparseCoeffAccumulator
//===----------------------------------------------------------------------===//

void SparseCoeffAccumulator::BeginDense(idx_t total_vars) {
    use_dense = true;
    if (dense.size() < total_vars) {
        dense.assign(total_vars, 0.0);
    }
    // Cells previously written were already zeroed in Flush(); no full re-zero needed.
    touched.clear();
    pairs.clear();
}

void SparseCoeffAccumulator::BeginSparse(idx_t expected_size) {
    use_dense = false;
    pairs.clear();
    if (expected_size > pairs.capacity()) {
        pairs.reserve(expected_size);
    }
    touched.clear();
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
                sum += pairs[j].second;
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
