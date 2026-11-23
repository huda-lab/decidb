#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"

#include <functional>

namespace duckdb {

void PhysicalDecide::CheckDecisionDomains(const vector<LogicalDecide::UserBoundSpec> &member_singleton_row_bounds) {
	idx_t num_decide_vars = tree->decide_variables.size();
	// A user bound that contradicts the variable's INTRINSIC domain (a non-negative
	// REAL/INTEGER's `>= 0` floor, or a BOOLEAN's `0/1` box) is a deterministic semantic
	// error, not a constraint conflict to diagnose — loosening can't help, the type can.
	// It is raised at physical planning, before any row is read, so it never reaches the
	// elastic engine and supersedes the generic infeasible error.
	//   - `x <= -1` (REAL): upper below the non-negativity floor → infeasible. But
	//     `x = -1` or `x <= -1 AND x >= -5` explicitly lower the floor below 0, so the
	//     guard is `U < 0 AND L >= 0` (floor still at/above the intrinsic 0).
	//   - `x >= 2` / `x <= -1` (BOOLEAN): outside the 0/1 box. This is Boolean arithmetic, not
	//     a wider variable, so the box stays [0,1] and the query is infeasible; the error quotes
	//     the bound.
	// Both are raised under DIAGNOSE too. A purely user-vs-user inverted box (e.g. `x >= 5 AND
	// x <= 1`, both >= 0) does NOT match and proceeds to the elastic engine, which reports a
	// least-change loosen.
	// A helper decision of the value rewrite has no intrinsic domain: it holds a
	// reducer's value, `sum(w * x)` negative wherever `w` is, so no bound on it
	// contradicts a type (its floor is free, DecideOptimizer::FoldSingletonRows).
	vector<bool> is_helper(num_decide_vars, false);
	for (auto &node : tree->nodes) {
		if (node->Linked() && tree->ValueVar(*node) < num_decide_vars) {
			is_helper[tree->ValueVar(*node)] = true;
		}
	}
	// An auxiliary whose definition may read no value -- a data value (NULL), a frame, a keyed decision (a row in
	// no class), or another such auxiliary -- has none on those rows, where no bound on it binds (D88): the data
	// says whether one row is left that the bound contradicts, so the solve reports it.
	std::function<bool(idx_t, idx_t)> may_lack_value = [&](idx_t var, idx_t depth) {
		if (depth > num_decide_vars) {
			return true;
		}
		auto reads_none = [&](const DecideTerm &term) {
			if (term.coefficient && !term.coefficient->IsFoldable()) {
				return true;
			}
			auto read = term.variable_index;
			if (read == DConstants::INVALID_INDEX || read == var) {
				return false;
			}
			return term.frame_read != DConstants::INVALID_INDEX ||
			       (read < tree->variable_scopes.size() && tree->variable_scopes[read].IsKeyed()) ||
			       may_lack_value(read, depth + 1);
		};
		for (auto constraint : tree->Constraints()) {
			if (constraint->defined_var != var) {
				continue;
			}
			if (constraint->rhs_expr && !constraint->rhs_expr->IsFoldable()) {
				return true;
			}
			for (auto &term : constraint->terms) {
				if (reads_none(term)) {
					return true;
				}
			}
			for (auto &group : constraint->product_groups) {
				for (auto *form : {&group.left, &group.right}) {
					for (auto &term : *form) {
						if (reads_none(term)) {
							return true;
						}
					}
				}
			}
		}
		return false;
	};
	// The box of the bounds written on one decision. The folded box holds the admitted ends instead (`x = -0.5`
	// admits no INT, which the solve reports as infeasible), so it is not the box the user wrote.
	auto written_box = [&](idx_t var) {
		DecideWrittenBox written;
		for (const auto &bound : member_singleton_row_bounds) {
			if (bound.decide_var_idx == var) {
				written.Add(bound.sense, bound.k);
			}
		}
		for (const auto &bound : tree->user_singleton_row_bounds) {
			if (bound.decide_var_idx != var) {
				continue;
			}
			if (bound.origin.removal_group_id == DConstants::INVALID_INDEX) {
				written.Add(bound.sense, bound.k);
			} else {
				written.AddLowered(bound.sense, bound.k);
			}
		}
		// A floor under a `when` replaces the default on the rows it keeps (AddLowered counts it only when
		// negative). The solve says whether the clauses on the other rows leave a value.
		for (auto constraint : tree->Constraints()) {
			if (constraint->lifted_floor == var) {
				written.AddLowered('>', constraint->lifted_floor_k);
			}
		}
		return written;
	};
	vector<bool> is_signed(num_decide_vars, false);
	for (auto &definition : tree->definitions) {
		if (definition->Signed() && definition->helper < num_decide_vars) {
			is_signed[definition->helper] = true;
		}
	}
	tree->domains.resize(num_decide_vars);
	for (idx_t var = 0; var < num_decide_vars; var++) {
		if (is_helper[var] || may_lack_value(var, 0)) {
			continue;
		}
		bool is_bool = var < tree->is_boolean_var.size() && tree->is_boolean_var[var];
		DecideWrittenBox written = written_box(var);
		// A product takes its factor's sign (`b * x` with x >= -5 reaches -5, AddBooleanProduct): its floor is
		// the factor's where that is negative, and none where the factor holds a value of either sign.
		for (auto &definition : tree->definitions) {
			auto product = definition->TryCast<DecideBooleanProductDefinition>();
			if (!product || product->helper != var || product->other_factor >= num_decide_vars) {
				continue;
			}
			const auto other = product->other_factor;
			written.AddLowered('>',
			                   is_signed[other] ? -DECIDE_SOLVER_INFINITY : written_box(other).Box().EffectiveLower());
		}
		// An auxiliary decision (an ABS, a norm) is named by what it stands for.
		const bool is_aux = var >= num_decide_vars - tree->num_auxiliary_vars;
		string vname = tree->decide_variables[var]->GetName();
		for (auto &label : display.aux_var_expressions) {
			if (is_aux && label.first == var) {
				vname = label.second;
			}
		}
		tree->domains[var] = {true, written.Box(), is_bool, vname};
		// The clause that set the bound `k` (`max(x) <= -5` as much as `x <= -5`, a strict `x < 0` as typed rather than
		// the folded `x <= -1`).
		auto refusal = DecideRefuseDomain(written.Box(), is_bool, vname);
		if (!refusal.message.empty()) {
			idx_t clause = DConstants::INVALID_INDEX;
			const vector<LogicalDecide::UserBoundSpec> *lists[] = {&tree->user_singleton_row_bounds,
			                                                       &member_singleton_row_bounds};
			for (auto *bounds : lists) {
				for (const auto &bound : *bounds) {
					if (bound.decide_var_idx == var && bound.sense != (refusal.upper ? '>' : '<') &&
					    bound.k == refusal.k && bound.origin.source_clause_id < display.constraint_sources.size()) {
						clause = bound.origin.source_clause_id;
						break;
					}
				}
				if (clause != DConstants::INVALID_INDEX) {
					break;
				}
			}
			throw InvalidInputException(refusal.message, DecideClauseErrorInfo(display.constraint_sources, clause));
		}
	}
}

} // namespace duckdb
