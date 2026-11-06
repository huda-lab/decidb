#include "duckdb/optimizer/decide/decide_solver_gate.hpp"

#include <algorithm>

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/solver/ilp_solver.hpp"
#include "duckdb/decidb/solver/solver_registry.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

namespace {

//! Mirrors how the model builder reads a declared type into column integrality:
//! everything that is not floating point becomes an integer column.
bool IsIntegralColumnType(const LogicalType &type) {
	return !(type == LogicalType::DOUBLE || type == LogicalType::FLOAT);
}

//! Could the built model contain an integral column? True for a declared `x(INT)` or
//! `x(BOOL)`, and for every helper stage 05 added to `decide_variables` the same way: a
//! helper carries its own type (whole-numbered when the value it holds is, an indicator
//! flagged in `is_boolean_var`), so the loop below reads it like any declared decision.
//!
//! The rest are added to the global block during execution, once the Big-M constants are
//! known, and only two lowerings add an integral one (`AddGlobalBinaryAux`): a `<>`, which
//! the binder admits over whole-numbered decisions only, so the loop has already answered;
//! and a MIN/MAX held by indicator rows, read here off the marks the MIN/MAX rewrites
//! leave. A lowering that starts adding a binary without a mark is caught when the built
//! model is checked (`AssertBackendAcceptsBuiltModel`), as an internal error rather than a
//! wrong answer.
bool MayHaveIntegralColumn(const LogicalDecide &op) {
	for (idx_t var = 0; var < op.decide_variables.size(); var++) {
		if (var < op.is_boolean_var.size() && op.is_boolean_var[var]) {
			return true;
		}
		if (IsIntegralColumnType(op.decide_variables[var]->return_type)) {
			return true;
		}
	}
	if (!op.minmax_clause_labels.empty()) {
		return true;
	}
	if (!op.composed_minmax_constraints.empty() || !op.composed_minmax_objective_terms.empty()) {
		return true;
	}
	for (auto &link : op.reducer_value_links) {
		if (link.agg_name == "min" || link.agg_name == "max") {
			return true;
		}
	}
	auto is_minmax = [](ObjectiveAggregateType agg) {
		return agg == ObjectiveAggregateType::MIN_AGG || agg == ObjectiveAggregateType::MAX_AGG;
	};
	return is_minmax(op.flat_objective_agg) || is_minmax(op.per_inner_agg) || is_minmax(op.per_outer_agg);
}

//! Does a product group of the objective couple two decision variables, making Q
//! rank-deficient? Squaring a linear form over k >= 2 decisions always does: expanding
//! `(p*x + q*y + r)^2` fills an off-diagonal Q entry, and the resulting 2x2 block is
//! singular for any p and q. One decision variable per squared form keeps that group's Q
//! diagonal, whatever the row count, which is the ordinary `POWER(x - target, 2)` shape. A
//! product of two forms couples its decisions by construction.
//!
//! Read off the groups stage 05 already built, so this needs no data — which is what lets
//! the refusal land before a row is read. Constant terms carry INVALID_INDEX and are
//! skipped. That keeps this prediction at least as demanding as what
//! `SolverModel::ModelClass()` later reads off the built matrix, which is the direction the
//! contract requires — over-reporting costs a refused query, under-reporting hands a backend
//! a model it answers wrong.
bool HasCoupledQuadraticTerms(const DecideObjective &objective) {
	for (auto &group : objective.product_groups) {
		if (!group.square) {
			return true;
		}
		vector<idx_t> seen;
		for (auto &term : group.left) {
			if (term.variable_index == DConstants::INVALID_INDEX ||
			    std::find(seen.begin(), seen.end(), term.variable_index) != seen.end()) {
				continue;
			}
			seen.push_back(term.variable_index);
			if (seen.size() >= 2) {
				return true;
			}
		}
	}
	return false;
}

//! The objective's first product group that is non-convex under the sense (IsNonconvexProductGroup), or null.
const DecideProductGroup *FirstNonconvexGroup(const LogicalDecide &op) {
	if (!op.prepared.objective) {
		return nullptr;
	}
	for (auto &group : op.prepared.objective->product_groups) {
		if (IsNonconvexProductGroup(group.square, group.weight_sign, op.decide_sense)) {
			return &group;
		}
	}
	return nullptr;
}

//! One line naming what the query does, in the user's own vocabulary, plus the
//! smallest edit that avoids it. The gap decides the wording, so a query is always
//! told which of its constructs is the one no solver here can take.
struct RefusalText {
	string cause;
	string remedy;
};

//! The first SUCH THAT clause that squares or multiplies decisions, as the user wrote it. DeriveDecideModelClass reads
//! the same constraints, so a query refused for quadratic constraints has one. A row a rewrite defines (a reducer's
//! non-linear body held by a helper) records the clause that reads the helper.
string FirstQuadraticClause(const LogicalDecide &op) {
	for (auto &constraint : op.prepared.constraints) {
		if (!constraint->product_groups.empty()) {
			// A quadratic row without a source clause is an engine bug, not a query to refuse.
			D_ASSERT(constraint->source_clause_id < op.constraint_sources.size());
			return QuoteWrittenClause(op.constraint_sources[constraint->source_clause_id]);
		}
	}
	throw InternalException("DECIDE solver gate found no quadratic clause to name");
}

RefusalText DescribeGap(const LogicalDecide &op, SolverModelClassGap gap) {
	switch (gap) {
	case SolverModelClassGap::QUADRATIC_CONSTRAINTS:
		return {"the SUCH THAT clause `" + FirstQuadraticClause(op) + "` squares or multiplies decision variables",
		        "rewrite that constraint so each term holds at most one decision variable"};
	case SolverModelClassGap::NONCONVEX_QUADRATIC: {
		auto *group = FirstNonconvexGroup(op);
		D_ASSERT(group);
		if (!group->square) {
			return {"the objective multiplies two decision variables",
			        "use an objective that is linear in the decisions"};
		}
		if (group->weight_sign == 0) {
			return {"the objective multiplies a squared decision term by a value whose sign is known only when the "
			        "query runs",
			        "multiply the square by a constant, or use a linear objective"};
		}
		return {op.decide_sense == DecideSense::MAXIMIZE
		            ? "the objective MAXIMIZEs squared decision terms"
		            : "the objective MINIMIZEs negated squared decision terms",
		        op.decide_sense == DecideSense::MAXIMIZE
		            ? "MINIMIZE the squared terms instead, or use a linear objective"
		            : "MINIMIZE the squared terms without negating them, or use a linear objective"};
	}
	case SolverModelClassGap::MIQP:
		return {"the objective squares decision variables that are not continuous",
		        "declare those variables REAL, as in x(REAL), or use a linear objective"};
	case SolverModelClassGap::SINGULAR_QUADRATIC:
		return {"the objective squares an expression containing more than one decision variable",
		        "keep at most one decision variable inside the POWER, as in "
		        "SUM(POWER(x - target, 2)), or use a linear objective"};
	default:
		throw InternalException("DECIDE solver gate asked to describe a satisfied model class");
	}
}

//! Is some backend that takes `needed` installed on this host?
bool AnyAvailable(const SolverModelClass &needed) {
	for (auto &backend : SolverRegistry::Backends()) {
		if (backend.IsAvailable() && SupportsModelClass(needed, backend.Capabilities().model_classes)) {
			return true;
		}
	}
	return false;
}

} // namespace

SolverModelClass DeriveDecideModelClass(const LogicalDecide &op) {
	SolverModelClass needed;

	// A constraint carrying a squared or bilinear term becomes a quadratic row: the
	// model builder emits one per data row or per group, and never folds it back
	// into the linear matrix.
	for (auto &constraint : op.prepared.constraints) {
		if (!constraint->product_groups.empty()) {
			needed.quadratic_constraints = true;
			break;
		}
	}

	auto &objective = op.prepared.objective;
	if (objective && !objective->product_groups.empty()) {
		// Convexity is per group (IsNonconvexProductGroup), the same predicate the model
		// builder later applies to the built model, so the prediction and the fact cannot
		// drift. A group's weight sign is what the plan knows of it (0 when only the query
		// knows it), which is what makes the test available this early.
		needed.nonconvex_quadratic = FirstNonconvexGroup(op) != nullptr;
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
	ThrowDecideSolverGap(needed, text.cause, text.remedy);
}

void ThrowDecideSolverGap(const SolverModelClass &needed, const string &cause, const string &remedy) {
	// Name the solvers that could run it, from the registry rather than from a
	// hard-coded name here, so the sentence stays true as backends are added.
	vector<string> capable = SolverRegistry::BackendsSupporting(needed);
	if (capable.empty()) {
		throw InvalidInputException("DECIDE: %s, which no solver DeciDB supports can optimize. Instead, %s.", cause,
		                            remedy);
	}
	// A solver the override excludes is not missing: the host has one that could run the query, and the override is
	// what keeps it out. The sentence then names the override, not an install.
	if (!ForcedSolverName().empty() && AnyAvailable(needed)) {
		throw InvalidInputException("DECIDE: %s, which needs %s, and DECIDB_FORCE_SOLVER selects %s. Unset "
		                            "DECIDB_FORCE_SOLVER, or %s.",
		                            cause, StringUtil::Join(capable, " or "),
		                            SolverRegistry::Find(ForcedSolverName()).DisplayName(), remedy);
	}
	throw InvalidInputException("DECIDE: %s, which needs %s — not available on this machine. Install %s, "
	                            "or %s.",
	                            cause, StringUtil::Join(capable, " or "), StringUtil::Join(capable, " or "), remedy);
}

} // namespace duckdb
