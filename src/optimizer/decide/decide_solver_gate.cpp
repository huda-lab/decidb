#include "duckdb/optimizer/decide/decide_solver_gate.hpp"

#include <algorithm>

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/solver/ilp_solver.hpp"
#include "duckdb/decidb/solver/solver_registry.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
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
		if (op.is_boolean_var[var]) {
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
	return is_minmax(op.flat_objective_agg);
}

//! How many distinct columns a product group's forms read; a constant term carries INVALID_INDEX. A decision read
//! through a frame (`x - at(previous: x)`) is the decision at another row: another column.
idx_t CountGroupDecisions(const DecideProductGroup &group) {
	vector<pair<idx_t, idx_t>> seen;
	for (auto *form : {&group.left, &group.right}) {
		for (auto &term : *form) {
			const auto column = std::make_pair(term.variable_index, term.frame_read);
			if (term.variable_index != DConstants::INVALID_INDEX &&
			    std::find(seen.begin(), seen.end(), column) == seen.end()) {
				seen.push_back(column);
			}
		}
	}
	return seen.size();
}

//! The clauses the model holds, in the tree's order: every clause stage 05 lowered into a node but a folded bound's,
//! which the repair model alone reads. Read off the nodes: the gate runs before DecideTreeConstruction::Finish numbers
//! them (DecideTree::Constraints).
vector<const DecideConstraint *> HeldClauses(const DecideTree &tree) {
	vector<const DecideConstraint *> clauses;
	for (auto &node : tree.nodes) {
		if (node->compiled.clause && !node->folded) {
			clauses.push_back(node->compiled.clause.get());
		}
	}
	return clauses;
}

//! The lowered model's shape, one AddTerm per product group. Read off the groups stage 05 already built, so it needs
//! no data, which is what lets the refusal land before a row is read. Each fact errs toward demanding more than the
//! built model will (SolverModelClass): an integral column the rewrites may add counts, and a square of a form over
//! two decisions counts as coupled whatever its coefficients turn out to be.
DecideModelShape ReadPreparedModelShape(const LogicalDecide &op, const DecideTree &tree) {
	DecideModelShape shape;
	shape.integral_column = MayHaveIntegralColumn(op);
	for (auto *constraint : HeldClauses(tree)) {
		for (auto &group : constraint->product_groups) {
			shape.AddTerm(false, group.square, group.weight_sign, CountGroupDecisions(group));
		}
	}
	if (auto objective = tree.Objective()) {
		for (auto &group : objective->product_groups) {
			shape.AddTerm(true, group.square, group.weight_sign, CountGroupDecisions(group));
		}
	}
	return shape;
}

//! The objective's first product group that is non-convex (IsNonconvexProductGroup), which the refusal names. The gate
//! asks only for a non-convex class, which an objective's group makes.
const DecideProductGroup &FirstNonconvexGroup(const DecideTree &tree) {
	for (auto &group : tree.Objective()->product_groups) {
		if (IsNonconvexProductGroup(group.square, group.weight_sign)) {
			return group;
		}
	}
	throw InternalException("DECIDE solver gate found no non-convex product group to name");
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
string FirstQuadraticClause(const LogicalDecide &op, const DecideTree &tree) {
	for (auto *constraint : HeldClauses(tree)) {
		if (!constraint->product_groups.empty()) {
			// A quadratic row without a source clause is an engine bug, not a query to refuse.
			D_ASSERT(constraint->source_clause_id < op.constraint_sources.size());
			return QuoteWrittenClause(op.constraint_sources, constraint->source_clause_id, op.source_text);
		}
	}
	throw InternalException("DECIDE solver gate found no quadratic clause to name");
}

RefusalText DescribeGap(const LogicalDecide &op, const DecideTree &tree, SolverModelClassGap gap) {
	switch (gap) {
	case SolverModelClassGap::QUADRATIC_CONSTRAINTS:
		return {"the SUCH THAT clause `" + FirstQuadraticClause(op, tree) +
		            "` squares or multiplies decision variables",
		        "rewrite that constraint so each term holds at most one decision variable"};
	case SolverModelClassGap::NONCONVEX_QUADRATIC: {
		auto &group = FirstNonconvexGroup(tree);
		if (!group.square) {
			return {"the objective multiplies two decision variables",
			        "use an objective that is linear in the decisions"};
		}
		if (group.weight_sign == 0) {
			return {"the objective multiplies a squared decision term by a value whose sign is known only when the "
			        "query runs",
			        "multiply the square by a constant, or use a linear objective"};
		}
		return {op.written_sense == DecideSense::MAXIMIZE ? "the objective MAXIMIZEs squared decision terms"
		                                                  : "the objective MINIMIZEs negated squared decision terms",
		        op.written_sense == DecideSense::MAXIMIZE
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
		if (backend.IsAvailable() && SupportsModelClass(needed, backend.ModelClasses())) {
			return true;
		}
	}
	return false;
}

} // namespace

SolverModelClass DeriveDecideModelClass(const LogicalDecide &op, const DecideTree &tree) {
	return ClassifyDecideModel(ReadPreparedModelShape(op, tree));
}

SolverModelClassGap FindDecideSolverGap(const SolverModelClass &needed, const SolverBackend &backend) {
	return FindModelClassGap(needed, backend.ModelClasses());
}

void RequireDecideSolverSupport(const LogicalDecide &op, const DecideTree &tree) {
	// The name was recorded by ChooseDecideSolver; resolving it back to a registry entry
	// is a table lookup, not a session, so the model-class question can still be asked
	// here without the plan itself holding a live backend.
	SolverBackend backend = SolverRegistry::Find(op.solver_backend_name);
	D_ASSERT(backend.IsValid());
	SolverModelClass needed = DeriveDecideModelClass(op, tree);
	SolverModelClassGap gap = FindDecideSolverGap(needed, backend);
	if (gap == SolverModelClassGap::NONE) {
		return;
	}

	RefusalText text = DescribeGap(op, tree, gap);
	ThrowDecideSolverGap(needed, text.cause, text.remedy);
}

void ThrowDecideSolverGap(const SolverModelClass &needed, const string &cause, const string &remedy) {
	// Name the solvers that could run it, from the registry rather than from a
	// hard-coded name here, so the sentence stays true as backends are added. The list is never empty: a backend's
	// model classes are a static table, so Gurobi is named on a host without its library or licence too.
	vector<string> capable = SolverRegistry::BackendsSupporting(needed);
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
