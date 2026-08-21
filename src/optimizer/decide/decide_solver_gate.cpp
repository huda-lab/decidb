#include "duckdb/optimizer/decide_solver_gate.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/solver_registry.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"

namespace duckdb {

namespace {

//! Mirrors how the model builder reads a declared type into column integrality:
//! everything that is not floating point becomes an integer column.
bool IsIntegralColumnType(const LogicalType &type) {
	return !(type == LogicalType::DOUBLE || type == LogicalType::FLOAT);
}

//! Could the built model contain an integral column? True for a declared `x(INT)` or
//! `x(BOOL)`, and true whenever the query uses a construct that gets an auxiliary
//! column, since every DeciDB auxiliary is either a Big-M indicator (binary) or a
//! continuous partner of one.
//!
//! The auxiliary half is deliberately coarse. Some auxiliaries are appended to
//! `decide_variables` by stage 05 and counted in `num_auxiliary_vars`; others are
//! added to the global block during execution, when the Big-M constants are known.
//! Rather than enumerate the second group — a list that would silently rot as
//! constructs are added — this treats *any* construct that reaches execution with
//! work left to do as producing one. Over-reporting costs a refused query that a
//! quadratic-capable solver would have run anyway; under-reporting would hand a
//! backend a model it cannot load, which is the failure this gate exists to prevent.
bool MayHaveIntegralColumn(const LogicalDecide &op) {
	for (idx_t var = 0; var < op.decide_variables.size(); var++) {
		if (var < op.is_boolean_var.size() && op.is_boolean_var[var]) {
			return true;
		}
		if (IsIntegralColumnType(op.decide_variables[var]->return_type)) {
			return true;
		}
	}
	if (op.num_auxiliary_vars > 0) {
		return true;
	}
	if (!op.composed_minmax_constraints.empty() || !op.composed_minmax_objective_terms.empty()) {
		return true;
	}
	auto is_minmax = [](ObjectiveAggregateType agg) {
		return agg == ObjectiveAggregateType::MIN_AGG || agg == ObjectiveAggregateType::MAX_AGG;
	};
	return is_minmax(op.flat_objective_agg) || is_minmax(op.per_inner_agg) || is_minmax(op.per_outer_agg);
}

//! One line naming what the query does, in the user's own vocabulary, plus the
//! smallest edit that avoids it. The gap decides the wording, so a query is always
//! told which of its constructs is the one no solver here can take.
struct RefusalText {
	string cause;
	string remedy;
};

RefusalText DescribeGap(const LogicalDecide &op, SolverModelClassGap gap) {
	switch (gap) {
	case SolverModelClassGap::QUADRATIC_CONSTRAINTS:
		return {"a SUCH THAT clause squares or multiplies decision variables",
		        "rewrite that constraint so each term holds at most one decision variable"};
	case SolverModelClassGap::NONCONVEX_QUADRATIC:
		if (op.prepared.objective && op.prepared.objective->has_bilinear) {
			return {"the objective multiplies two decision variables",
			        "use an objective that is linear in the decisions"};
		}
		return {op.decide_sense == DecideSense::MAXIMIZE
		            ? "the objective MAXIMIZEs squared decision terms"
		            : "the objective MINIMIZEs negated squared decision terms",
		        op.decide_sense == DecideSense::MAXIMIZE
		            ? "MINIMIZE the squared terms instead, or use a linear objective"
		            : "MINIMIZE the squared terms without negating them, or use a linear objective"};
	case SolverModelClassGap::MIQP:
		return {"the objective squares decision variables that are not continuous",
		        "declare those variables REAL, as in x(REAL), or use a linear objective"};
	default:
		throw InternalException("DECIDE solver gate asked to describe a satisfied model class");
	}
}

} // namespace

SolverModelClass DeriveDecideModelClass(const LogicalDecide &op) {
	SolverModelClass needed;

	// A constraint carrying a squared or bilinear term becomes a quadratic row: the
	// model builder emits one per data row or per PER group, and never folds it back
	// into the linear matrix.
	for (auto &constraint : op.prepared.constraints) {
		if (constraint->has_quadratic || constraint->has_bilinear) {
			needed.quadratic_constraints = true;
			break;
		}
	}

	auto &objective = op.prepared.objective;
	if (objective && (objective->has_quadratic || objective->has_bilinear)) {
		// A product of two decisions is indefinite whatever the sense, so a bilinear
		// objective is unconditionally non-convex. A sum of squares is non-convex only
		// when its sign fights the sense: MAXIMIZE of a positive Q, MINIMIZE of a
		// negative one. `quadratic_sign` is a plan-time constant — stage 05 already
		// refuses a scale factor it cannot fold — so the same test the model builder
		// runs is available here.
		if (objective->has_bilinear) {
			needed.nonconvex_quadratic = true;
		} else {
			bool is_maximize = op.decide_sense == DecideSense::MAXIMIZE;
			needed.nonconvex_quadratic = (objective->quadratic_sign > 0.0) == is_maximize;
		}
		needed.miqp = MayHaveIntegralColumn(op);
	}

	return needed;
}

void RequireDecideSolverSupport(const LogicalDecide &op) {
	D_ASSERT(op.solver_backend.IsValid());
	SolverModelClass needed = DeriveDecideModelClass(op);
	SolverModelClassGap gap = FindModelClassGap(needed, op.solver_backend.Capabilities());
	if (gap == SolverModelClassGap::NONE) {
		return;
	}

	RefusalText text = DescribeGap(op, gap);
	// Name the solvers that could run it, from the registry rather than from a
	// hard-coded name here, so the sentence stays true as backends are added.
	vector<string> capable = SolverRegistry::BackendsSupporting(needed);
	if (capable.empty()) {
		throw InvalidInputException("DECIDE: %s, which no solver DeciDB supports can optimize. Instead, %s.",
		                            text.cause, text.remedy);
	}
	throw InvalidInputException("DECIDE: %s, which needs %s — not available on this machine. Install %s, "
	                            "or %s.",
	                            text.cause, StringUtil::Join(capable, " or "),
	                            StringUtil::Join(capable, " or "), text.remedy);
}

} // namespace duckdb
