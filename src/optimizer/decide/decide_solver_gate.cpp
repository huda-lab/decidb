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

//! What the query does that this installation's solver cannot take, in the user's own vocabulary, and where in the
//! query it stands.
struct RefusalText {
	string cause;
	optional_idx place;
};

//! The span of the first SUCH THAT clause that squares or multiplies decisions. DeriveDecideModelClass reads the same
//! constraints, so a query refused for quadratic constraints has one. A row a rewrite defines (a reducer's non-linear
//! body held by a helper) records the clause that reads the helper; a row no clause claims has no span.
optional_idx FirstQuadraticClauseLocation(const LogicalDecide &op, const DecideTree &tree) {
	for (auto *constraint : HeldClauses(tree)) {
		if (!constraint->product_groups.empty()) {
			return DecideClauseLocation(op.constraint_sources, constraint->source_clause_id);
		}
	}
	throw InternalException("DECIDE solver gate found no quadratic clause to name");
}

RefusalText DescribeGap(const LogicalDecide &op, const DecideTree &tree, SolverModelClassGap gap) {
	const auto objective = DecideObjectiveLocation(op.source_text);
	switch (gap) {
	case SolverModelClassGap::QUADRATIC_CONSTRAINTS:
		return {"squaring or multiplying decisions in a constraint", FirstQuadraticClauseLocation(op, tree)};
	case SolverModelClassGap::NONCONVEX_QUADRATIC: {
		auto &group = FirstNonconvexGroup(tree);
		if (!group.square) {
			return {"multiplying two decisions in the objective", objective};
		}
		if (group.weight_sign == 0) {
			return {"a square scaled by a value whose sign is known only when the query runs", objective};
		}
		return {op.written_sense == DecideSense::MAXIMIZE ? "maximizing a squared decision"
		                                                  : "minimizing a negated squared decision",
		        objective};
	}
	case SolverModelClassGap::MIQP:
		return {"squaring a decision that is not REAL", objective};
	case SolverModelClassGap::SINGULAR_QUADRATIC:
		return {"squaring an expression of more than one decision", objective};
	default:
		throw InternalException("DECIDE solver gate asked to describe a satisfied model class");
	}
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

	auto text = DescribeGap(op, tree, gap);
	ThrowDecideSolverGap(text.cause, text.place);
}

void ThrowDecideSolverGap(const string &cause, optional_idx place) {
	// The one text: the installation is the place, so a host with the solver (or one forced off it by
	// DECIDB_FORCE_SOLVER) answers the same query.
	throw NotImplementedException(place, cause + " is not supported on this installation");
}

} // namespace duckdb
