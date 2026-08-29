#include "duckdb/optimizer/decide_solver_gate.hpp"

#include <algorithm>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/decidb/solver_registry.hpp"
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

//! Does the objective's quadratic part couple two decision variables, making Q
//! rank-deficient? Squaring a linear form over k >= 2 decisions always does: expanding
//! `(p*x + q*y + r)^2` fills an off-diagonal Q entry, and the resulting 2x2 block is
//! singular for any p and q. One decision variable per squared expression keeps Q
//! diagonal, whatever the row count, which is the ordinary `POWER(x - target, 2)` shape.
//!
//! Read off `squared_terms`, the flattened inner expression stage 05 already built, so
//! this needs no data — which is what lets the refusal land before a row is read.
//! Constant terms carry INVALID_INDEX and are skipped. A bilinear objective counts too:
//! a product of two decisions is off-diagonal by construction. That keeps this
//! prediction at least as demanding as what `SolverModel::ModelClass()` later reads off
//! the built matrix, which is the direction the contract requires — over-reporting costs
//! a refused query, under-reporting hands a backend a model it answers wrong.
bool HasCoupledQuadraticTerms(const DecideObjective &objective) {
	if (objective.has_bilinear) {
		return true;
	}
	vector<idx_t> seen;
	for (auto &term : objective.squared_terms) {
		if (term.variable_index == DConstants::INVALID_INDEX) {
			continue;
		}
		if (std::find(seen.begin(), seen.end(), term.variable_index) != seen.end()) {
			continue;
		}
		seen.push_back(term.variable_index);
		if (seen.size() >= 2) {
			return true;
		}
	}
	return false;
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
	case SolverModelClassGap::SINGULAR_QUADRATIC:
		// The remedy deliberately does not offer "square each variable separately":
		// stage 05 allows only one quadratic group per objective, so
		// `POWER(x, 2) + POWER(y, 2)` trades this refusal for a different one.
		return {"the objective squares an expression containing more than one decision variable",
		        "keep at most one decision variable inside the POWER, as in "
		        "SUM(POWER(x - target, 2)), or use a linear objective"};
	default:
		throw InternalException("DECIDE solver gate asked to describe a satisfied model class");
	}
}

} // namespace

void ChooseDecideSolver(LogicalDecide &op) {
	if (!op.solver_backend_name.empty()) {
		return;
	}
	SolverBackend backend = SelectSolverBackend();
	op.solver_backend_name = backend.Name();
	// The formulation decision, made HERE and read everywhere else. `Capabilities()`
	// answers what the backend CAN state; recording it on the plan turns that into what
	// this query WILL leave native. The two are the same table, which is why one type
	// carries both — but the question is asked exactly once, at the only stage entitled
	// to choose a formulation.
	op.use_native_constructs = backend.Capabilities().constructs;
	// ...and the policy that governs their use. Read here, with the rest of the
	// formulation decision, rather than at the emission site: an environment read
	// scattered across stage 08 would be a second place the formulation gets decided.
	op.force_native_constructs = NativeConstructsForced();
}

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
		// when its sign fights the sense, which is IsNonconvexQuadraticObjective — the
		// same predicate the model builder later applies to the built Q, so the
		// prediction and the fact cannot drift. `quadratic_sign` is a plan-time constant
		// (stage 05 already refuses a scale factor it cannot fold), which is what makes
		// the test available this early.
		if (objective->has_bilinear) {
			needed.nonconvex_quadratic = true;
		} else {
			needed.nonconvex_quadratic =
			    IsNonconvexQuadraticObjective(objective->quadratic_sign, op.decide_sense);
		}
		needed.miqp = MayHaveIntegralColumn(op);
		needed.singular_quadratic = HasCoupledQuadraticTerms(*objective);
	}

	return needed;
}

void RequireDecideSolverSupport(const LogicalDecide &op) {
	// The name was recorded by ChooseDecideSolver; resolving it back to a registry entry
	// is a table lookup, not a session, so the model-class question can still be asked
	// here without the plan itself holding a live backend.
	SolverBackend backend = SolverRegistry::Find(op.solver_backend_name);
	D_ASSERT(backend.IsValid());
	SolverModelClass needed = DeriveDecideModelClass(op);
	SolverModelClassGap gap = FindModelClassGap(needed, backend.Capabilities().model_classes);
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
