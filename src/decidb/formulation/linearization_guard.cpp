//===----------------------------------------------------------------------===//
//                         DecidB
//
// linearization_guard.cpp
//
// `IF b:` guards (DeciQL spec §7.4). A guarded constraint instance is the
// implication `b ⟹ row`. This pass decides, per clause, how the guard switches
// its rows and allocates the binaries that switching needs; the rows themselves
// exist only once SolverModel::Build enumerates the instances, which is where
// each one is stated as a conditional row on the binary chosen here.
//
//   - A guard that is one BOOL decision (`IF open`, `IF NOT open`) switches on that
//     decision's own column and needs nothing allocated.
//   - A linear comparison (`IF ship > 0`) gets one binary `y` per instance with
//     `y = 0 ⟹ NOT b`, so `b` forces `y = 1`, and `y = 1 ⟹ row`. The complement
//     `NOT b` is a row too, which is why the comparison has to step on an integer
//     lattice: `NOT (a·x >= k)` is `a·x <= ceil(k) - 1` only when `a·x` is
//     integer-valued.
//
//===----------------------------------------------------------------------===//

#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"
#include "duckdb/common/exception.hpp"

#include <cmath>

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

//! How many instances a clause emits, in the numbering SolverModel::Build uses:
//! its row index for a per-row body, its instance index otherwise.
static idx_t GuardInstanceSlots(const EvaluatedConstraint &ec, idx_t num_rows) {
    const bool per_row = !ec.lhs_is_aggregate && !ec.HasGeneralGrouping();
    if (per_row) {
        return num_rows;
    }
    return ec.row_group_ids.empty() ? 1 : MaxValue<idx_t>(ec.num_groups, 1);
}

//! The one value a column holds on every row, if it holds one. A constant coefficient
//! is evaluated per row and lands dense, so the column's own uniform flag is not enough.
static bool ColumnConstant(const CoefficientColumn &col, double &value) {
    if (col.Size() == 0) {
        value = 0.0;
        return true;
    }
    value = col.Get(0);
    if (col.IsUniform()) {
        return true;
    }
    for (idx_t r = 1; r < col.Size(); r++) {
        if (col.Get(r) != value) {
            return false;
        }
    }
    return true;
}

void LinearizeGuards(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names) {
    for (auto &ec : input.constraints) {
        if (!ec.guard) {
            continue;
        }
        auto &guard = *ec.guard;

        // One BOOL decision with a uniform coefficient against a uniform bound: read
        // which of its two values satisfy the guard.
        idx_t decision_terms = 0;
        idx_t only_var = DConstants::INVALID_INDEX;
        idx_t only_term = DConstants::INVALID_INDEX;
        for (idx_t t = 0; t < guard.variable_indices.size(); t++) {
            if (guard.variable_indices[t] != DConstants::INVALID_INDEX) {
                decision_terms++;
                only_var = guard.variable_indices[t];
                only_term = t;
            }
        }
        double c = 0.0, k = 0.0;
        bool uniform = decision_terms == 1 && ColumnConstant(guard.rhs_values, k);
        for (idx_t t = 0; t < guard.variable_indices.size() && uniform; t++) {
            double fixed;
            if (guard.variable_indices[t] == DConstants::INVALID_INDEX) {
                uniform = ColumnConstant(guard.row_coefficients[t], fixed);
                k -= fixed;
            } else {
                uniform = ColumnConstant(guard.row_coefficients[t], c);
            }
        }
        if (uniform && input.variable_types[only_var] == LogicalType::BOOLEAN) {
            auto holds = [&](double value) {
                double lhs = c * value;
                switch (guard.comparison_type) {
                case ExpressionType::COMPARE_LESSTHANOREQUALTO:
                    return lhs <= k;
                case ExpressionType::COMPARE_LESSTHAN:
                    return lhs < k;
                case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
                    return lhs >= k;
                default:
                    return lhs > k;
                }
            };
            const bool off = holds(0.0), on = holds(1.0);
            if (off && on) {
                guard.kind = EvaluatedGuard::Kind::ALWAYS;
            } else if (!off && !on) {
                guard.kind = EvaluatedGuard::Kind::NEVER;
            } else {
                guard.kind = EvaluatedGuard::Kind::VARIABLE;
                guard.bool_var = only_var;
                guard.bool_value = on ? 1 : 0;
            }
            continue;
        }

        // A comparison: its complement is a row on the integer lattice.
        for (idx_t t = 0; t < guard.variable_indices.size(); t++) {
            idx_t var = guard.variable_indices[t];
            if (var == DConstants::INVALID_INDEX) {
                continue;
            }
            const auto &type = input.variable_types[var];
            const bool real = type == LogicalType::DOUBLE || type == LogicalType::FLOAT;
            if (real || !guard.row_coefficients[t].AllIntegral()) {
                const string &name = var < var_names.size() ? var_names[var] : string("a decision");
                throw InvalidInputException(
                    "%s compares an expression that is not integer-valued (%s), so the solver cannot "
                    "state when it fails to hold. Guard on an INT or BOOL decision with integer "
                    "coefficients, or on a BOOL decision that stands for the condition.",
                    guard.label, real ? "'" + name + "' is REAL" : "a coefficient of '" + name + "' is fractional");
            }
        }
        guard.kind = EvaluatedGuard::Kind::COMPARISON;
        idx_t slots = GuardInstanceSlots(ec, input.num_rows);
        guard.aux_base = indexer.global_block_start + input.num_global_vars;
        for (idx_t i = 0; i < slots; i++) {
            AddGlobalBinaryAux(input, indexer, 0.0, guard.label);
        }
    }
}

} // namespace duckdb
