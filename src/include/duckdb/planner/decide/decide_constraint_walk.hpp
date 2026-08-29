//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_constraint_walk.hpp
//
// The single answer to "which children of this node are constraints".
//===----------------------------------------------------------------------===//
//
// A bound DECIDE constraint tree is AND-conjunctions plus WRAPPERS. A wrapper is a
// conjunction carrying a tag: `WHEN` holds the constraint in child 0 and its
// condition in child 1; `PER` holds the constraint in child 0 and its grouping
// columns in children 1..N. The condition and the grouping columns are METADATA --
// walking into them treats `WHEN c` as though the user had written another
// constraint, which is silently wrong rather than loudly wrong.
//
// So every pass over the tree has to know the same rule: at a wrapper, descend only
// into child 0. That rule used to be retyped as
// `HasDecideTag(alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(alias)` at fifteen
// sites, which meant a third wrapper kind would have to be found at all fifteen --
// and the one that got missed would start walking a condition as a constraint.
//
// It is stated once here instead. Every site asks the predicate, INCLUDING the
// handful that deliberately act differently on the answer: bound absorption refuses
// to descend into a WHEN at all (a conditional bound must not become a global one),
// and the MIN/MAX rewrite strips a PER wrapper as it goes. Those pass their own
// descent but not their own definition of what a wrapper is, which is where the
// safety actually comes from.
//
// OWNERSHIP. The rule is a property of the bound tree's tagging convention, which is
// established when the binder builds the wrappers and enforced by canonicalization
// (layer 4). It is consumed downward by the DECIDE optimizer (layer 5) and by
// diagnostics rendering. It lives here, beside the canonicalizer that owns constraint
// SHAPE, rather than in `common/enums/decide.hpp` beside the tag constants -- an
// enums header should not need to know what a BoundConjunctionExpression is.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"

namespace duckdb {

//! `<constraint> WHEN <condition>`: child 0 is the constraint, child 1 the condition.
inline bool IsWhenConstraintWrapper(const BoundConjunctionExpression &conjunction) {
	return HasDecideTag(conjunction.GetAlias(), WHEN_CONSTRAINT_TAG);
}

//! `<constraint> PER <col>, ...`: child 0 is the constraint, children 1..N the
//! grouping columns.
inline bool IsPerConstraintWrapper(const BoundConjunctionExpression &conjunction) {
	return IsPerConstraintTag(conjunction.GetAlias());
}

//! True for either wrapper. An untagged conjunction is a plain AND, every child of
//! which is a constraint in its own right.
inline bool IsConstraintWrapper(const BoundConjunctionExpression &conjunction) {
	return IsPerConstraintWrapper(conjunction) || IsWhenConstraintWrapper(conjunction);
}

//! Whether child `child_index` is a constraint rather than wrapper metadata.
inline bool IsConstraintChild(const BoundConjunctionExpression &conjunction, idx_t child_index) {
	return !IsConstraintWrapper(conjunction) || child_index == 0;
}

//! Visit every node that belongs to the constraint tree, parents before children,
//! skipping wrapper metadata. `visitor` returns false to stop the walk; the return
//! value is false iff it did.
//!
//! The visitor sees the conjunctions as well as what they hold, because the
//! canonicalizer's invariant checks have things to say about a wrapper itself.
//! A pass that only cares about the constraints under them wants
//! ForEachConstraintLeaf below.
template <class VISITOR>
bool VisitConstraintTree(const Expression &expression, VISITOR &&visitor) {
	if (!visitor(expression)) {
		return false;
	}
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return true;
	}
	auto &conjunction = expression.Cast<const BoundConjunctionExpression>();
	for (idx_t i = 0; i < conjunction.children.size(); i++) {
		if (IsConstraintChild(conjunction, i) && !VisitConstraintTree(*conjunction.children[i], visitor)) {
			return false;
		}
	}
	return true;
}

//! Mutating overload. The visitor may edit the nodes it is handed, but must not
//! reseat them -- it is given `Expression &`, not the owning `unique_ptr`, because a
//! pass that replaces a wrapper node is deciding tree shape and belongs at a boundary
//! that owns shape rather than inside a traversal.
template <class VISITOR>
bool VisitConstraintTree(Expression &expression, VISITOR &&visitor) {
	if (!visitor(expression)) {
		return false;
	}
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return true;
	}
	auto &conjunction = expression.Cast<BoundConjunctionExpression>();
	for (idx_t i = 0; i < conjunction.children.size(); i++) {
		if (IsConstraintChild(conjunction, i) && !VisitConstraintTree(*conjunction.children[i], visitor)) {
			return false;
		}
	}
	return true;
}

//! Call `callback` once per constraint-position LEAF -- every node reached by
//! VisitConstraintTree that is not itself a conjunction. That is one call per model
//! row: a comparison, a bound `IN` operator, a bare boolean decision, or a
//! placeholder an earlier rewrite left behind. The callback filters by class; it
//! cannot stop the walk.
//!
//! This is the shape almost every consumer wants, and stating it separately keeps
//! `return true` out of callbacks that have no reason to say it.
template <class CALLBACK>
void ForEachConstraintLeaf(const Expression &expression, CALLBACK &&callback) {
	VisitConstraintTree(expression, [&](const Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			callback(node);
		}
		return true;
	});
}

//! Mutating overload. See the note on VisitConstraintTree about reseating.
template <class CALLBACK>
void ForEachConstraintLeaf(Expression &expression, CALLBACK &&callback) {
	VisitConstraintTree(expression, [&](Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
			callback(node);
		}
		return true;
	});
}

} // namespace duckdb
