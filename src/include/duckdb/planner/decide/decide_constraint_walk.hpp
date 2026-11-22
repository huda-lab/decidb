//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_constraint_walk.hpp
//
// The single answer to "which children of this node are constraints".
//===----------------------------------------------------------------------===//
//
// A bound DECIDE constraint tree is AND-conjunctions plus WRAPPERS. A wrapper is a
// conjunction carrying a tag: `when c:` holds the constraint in child 0 and its
// condition in child 1; `per K:` holds the constraint in child 0 and names its key K by a
// tag (PerWrapperKey). The condition is METADATA -- walking into it treats `when c` as
// though the user had written another constraint, which is silently wrong rather than
// loudly wrong.
//
// So every pass over the tree has to know the same rule: at a wrapper, descend only
// into child 0. The rule is stated once, here, so that a third wrapper kind is added in
// one place and no site starts walking a condition as a constraint. Every site asks the predicate, INCLUDING the
// handful that deliberately act differently on the answer: singleton-row folding refuses
// to descend into a `when` at all (a conditional bound must not become a global one).
// Those pass their own descent but not their own definition of what a wrapper is,
// which is where the safety actually comes from.
//
// OWNERSHIP. The rule is a property of the bound tree's tagging convention, which is
// established when the binder builds the wrappers and enforced by canonicalization
// (layer 4). It is consumed downward by the DECIDE optimizer (layer 5) and by
// diagnostics rendering. It lives here, beside the canonicalizer that owns constraint
// SHAPE, rather than in `common/enums/decide.hpp` beside the tag constants -- an
// enums header should not need to know what a BoundConjunctionExpression is.
//
// The same holds for everything else the convention stores on a clause: the clause a row
// came from (ClauseOriginOf, StampClauseOrigin), the facts a row carries (a BETWEEN half,
// a structural row, the helper it defines, an easy MIN/MAX, a folded bound) and the
// wrappers themselves (MakeScopedRow). This header is the only place that names their
// tags; every other site reads and writes them through the accessors below.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"

namespace duckdb {

//! `when <condition>: <constraint>`: child 0 is the constraint, child 1 the condition.
inline bool IsWhenFilterWrapper(const BoundConjunctionExpression &conjunction) {
	return HasDecideTag(conjunction.GetAlias(), WHEN_FILTER_TAG);
}

//! `per K: <constraint>`: child 0 is the constraint; the tag names the key, none for
//! `per ()`. One constraint per class of the key; it groups no reducer.
inline bool IsPerClassWrapper(const BoundConjunctionExpression &conjunction) {
	return HasDecideTag(conjunction.GetAlias(), PER_CLASS_TAG);
}

//! The key a `per K:` wrapper names: an index into LogicalDecide::keys, INVALID_INDEX
//! for `per ()`.
inline idx_t PerWrapperKey(const BoundConjunctionExpression &wrapper) {
	idx_t key = DConstants::INVALID_INDEX;
	TryParsePerKeyTag(wrapper.GetAlias(), key);
	return key;
}

//! True for any wrapper. An untagged conjunction is a plain AND, every child of which
//! is a constraint in its own right.
inline bool IsConstraintWrapper(const BoundConjunctionExpression &conjunction) {
	return IsWhenFilterWrapper(conjunction) || IsPerClassWrapper(conjunction);
}

//! Whether child `child_index` is a constraint rather than wrapper metadata.
inline bool IsConstraintChild(const BoundConjunctionExpression &conjunction, idx_t child_index) {
	return !IsConstraintWrapper(conjunction) || child_index == 0;
}

//! The variant signature a `per K:` wrapper names: an index into LogicalDecide::signatures, INVALID_INDEX when the
//! clause reads no field, so one instance per class (D70).
inline idx_t PerWrapperSignature(const BoundConjunctionExpression &wrapper) {
	idx_t signature = DConstants::INVALID_INDEX;
	TryParseSignatureTag(wrapper.GetAlias(), signature);
	return signature;
}

//! Names `signature` on a `per K:` wrapper that names none.
inline void SetPerWrapperSignature(BoundConjunctionExpression &wrapper, idx_t signature) {
	AddDecideTag(wrapper.alias, MakeSignatureTag(signature));
}

//! The `per` of a scoped row (MakeScopedRow).
struct DecideRowPer {
	//! The key, an index into LogicalDecide::keys; INVALID_INDEX for `per ()`.
	idx_t key = DConstants::INVALID_INDEX;
	//! The variant signature, an index into LogicalDecide::signatures; INVALID_INDEX for none.
	idx_t signature = DConstants::INVALID_INDEX;
	//! Where the `per` stands in the query, for a wrapper the user wrote.
	optional_idx location;
};

//! `row` in a scope: under `when condition:` (null: every row), and under `per`, outermost (null: no `per`). The one
//! builder of a wrapper: the binders use it for a written scope, and a rewrite for the scope of a row it adds.
inline unique_ptr<Expression> MakeScopedRow(unique_ptr<Expression> row, unique_ptr<Expression> condition,
                                            optional_ptr<const DecideRowPer> per = nullptr) {
	if (condition) {
		auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		wrapper->children.push_back(std::move(row));
		wrapper->children.push_back(std::move(condition));
		wrapper->alias = WHEN_FILTER_TAG;
		row = std::move(wrapper);
	}
	if (per) {
		auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		wrapper->children.push_back(std::move(row));
		wrapper->alias = PER_CLASS_TAG;
		if (per->key != DConstants::INVALID_INDEX) {
			AddDecideTag(wrapper->alias, MakePerKeyTag(per->key));
		}
		if (per->signature != DConstants::INVALID_INDEX) {
			SetPerWrapperSignature(*wrapper, per->signature);
		}
		wrapper->SetQueryLocation(per->location);
		row = std::move(wrapper);
	}
	return row;
}

//! A `when` wrapper that holds `row` and no condition: a malformed tree, which only a test of the canonical verifier
//! builds.
inline unique_ptr<Expression> MakeWhenWrapperWithoutCondition(unique_ptr<Expression> row) {
	auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	wrapper->children.push_back(std::move(row));
	wrapper->alias = WHEN_FILTER_TAG;
	return std::move(wrapper);
}

//! The scope a clause is written under, borrowed from the tree: the `when c:` wrapper and the `per K:` wrapper
//! (PER_CLASS_TAG) that enclose it, null for none. The canonical tree holds at most one of each above a clause and no
//! plain AND below either (C0, DecideCanonicalizer::VerifyCanonical), so a walk carries its scope through a plain
//! AND: above every wrapper it is the empty scope, and below one there is none. A rewrite that replaces nodes keeps
//! its own recursion and takes each step with EnterDecideScope; a pass that only reads the clauses takes
//! ForEachScopedConstraintLeaf. A clause that leaves the tree owns its scope as a DecideClauseScope
//! (decide_tree_terms.hpp).
struct DecideEnclosingScope {
	const BoundConjunctionExpression *when_wrapper = nullptr;
	const BoundConjunctionExpression *per_wrapper = nullptr;

	//! The `when` condition, or null for every row.
	const Expression *Condition() const {
		return when_wrapper ? when_wrapper->children[1].get() : nullptr;
	}
	//! The `per` key (an index into LogicalDecide::keys); INVALID_INDEX for `per ()` or no `per`.
	idx_t PerKey() const {
		return per_wrapper ? PerWrapperKey(*per_wrapper) : DConstants::INVALID_INDEX;
	}
};

//! The scope in force below `node`, given the one above it: a `when c:` wrapper supplies the condition, a `per K:`
//! wrapper the key, and any other node passes `scope` on.
inline DecideEnclosingScope EnterDecideScope(const Expression &node, DecideEnclosingScope scope) {
	if (node.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return scope;
	}
	auto &conjunction = node.Cast<BoundConjunctionExpression>();
	if (IsWhenFilterWrapper(conjunction) && conjunction.children.size() == 2) {
		scope.when_wrapper = &conjunction;
	} else if (IsPerClassWrapper(conjunction) && !conjunction.children.empty()) {
		scope.per_wrapper = &conjunction;
	}
	return scope;
}

//! The clause `row` came from, read off the row: its source clause, its removal group, and whether a rewrite made it
//! for the objective. A row holds no repair group.
inline DecideRowOrigin ClauseOriginOf(const BaseExpression &row) {
	DecideRowOrigin origin;
	TryParseSourceClauseTag(row.GetAlias(), origin.source_clause_id);
	TryParseRemovalGroupTag(row.GetAlias(), origin.removal_group_id);
	origin.objective = HasDecideTag(row.GetAlias(), OBJECTIVE_ROW_TAG);
	return origin;
}

//! Writes onto `row` each part `origin` holds (its source clause, its removal group, the objective's mark), which the
//! row does not hold yet. The row keeps the parts it holds that `origin` does not.
inline void StampClauseOrigin(BaseExpression &row, const DecideRowOrigin &origin) {
	auto alias = row.GetAlias();
	if (origin.source_clause_id != DConstants::INVALID_INDEX) {
		AddDecideTag(alias, MakeSourceClauseTag(origin.source_clause_id));
	}
	if (origin.removal_group_id != DConstants::INVALID_INDEX) {
		AddDecideTag(alias, MakeRemovalGroupTag(origin.removal_group_id));
	}
	if (origin.objective) {
		AddDecideTag(alias, OBJECTIVE_ROW_TAG);
	}
	row.SetAlias(std::move(alias));
}

//! The user clause a walk is currently standing inside. A comparison carrying a source clause starts a new one, and so
//! does a row a rewrite made for the objective; every other node inherits its parent's (`inherited`). This is what lets
//! a row emitted deep inside a clause still name the clause it came from.
inline DecideRowOrigin DescendClauseOrigin(const Expression &expr, const DecideRowOrigin &inherited) {
	auto own = ClauseOriginOf(expr);
	return own.source_clause_id != DConstants::INVALID_INDEX || own.objective ? own : inherited;
}

//! Which comparison of the two `x BETWEEN lo AND hi` binds to a row is, `>=` being the lower one; NONE for any other
//! row. InitializeConstraintSourceInfo reads it to link the halves into the one clause the user wrote.
enum class DecideBetweenHalf : uint8_t { NONE, LOWER, UPPER };

inline DecideBetweenHalf BetweenHalfOf(const BaseExpression &row) {
	return HasDecideTag(row.GetAlias(), BETWEEN_LOWER_TAG)   ? DecideBetweenHalf::LOWER
	       : HasDecideTag(row.GetAlias(), BETWEEN_UPPER_TAG) ? DecideBetweenHalf::UPPER
	                                                         : DecideBetweenHalf::NONE;
}

//! Marks `row` as `half` of a BETWEEN, LOWER or UPPER.
inline void SetBetweenHalf(BaseExpression &row, DecideBetweenHalf half) {
	D_ASSERT(half != DecideBetweenHalf::NONE);
	AddDecideTag(row.alias, half == DecideBetweenHalf::LOWER ? BETWEEN_LOWER_TAG : BETWEEN_UPPER_TAG);
}

//! Whether a rewrite added `row` to define an auxiliary or to link its machinery, rather than the user writing it. Such
//! a row is rigid: no repair loosens it.
inline bool IsStructuralRow(const BaseExpression &row) {
	return HasDecideTag(row.GetAlias(), STRUCTURAL_CONSTRAINT_TAG);
}

inline void MarkStructuralRow(BaseExpression &row) {
	AddDecideTag(row.alias, STRUCTURAL_CONSTRAINT_TAG);
}

//! The auxiliary decision `row` defines (an ABS's envelope, a body helper's definition, an L0 indicator's links):
//! the auxiliary has no value where what its definition reads has none (D88). INVALID_INDEX for every other row.
inline idx_t DefinedHelperOf(const BaseExpression &row) {
	idx_t helper = DConstants::INVALID_INDEX;
	TryParseDecideIndexTag(row.GetAlias(), DEFINES_TAG_PREFIX, helper);
	return helper;
}

inline void SetDefinedHelper(BaseExpression &row, idx_t helper) {
	AddDecideTag(row.alias, MakeDefinesTag(helper));
}

//! Whether `row` is a comparison whose MIN or MAX is pushed its easy way (`max(e) <= K`, `min(e) >= K`) and keeps its
//! reducer: the linear form expands it to one row per member (RewriteMinMax).
inline bool IsEasyMinMaxRow(const BaseExpression &row) {
	return HasDecideTag(row.GetAlias(), MINMAX_EASY_REWRITE_TAG);
}

inline void MarkEasyMinMaxRow(BaseExpression &row) {
	AddDecideTag(row.alias, MINMAX_EASY_REWRITE_TAG);
}

//! Whether the whole meaning of `row`, a bound on one decision, is folded into the decision column's box
//! (LogicalDecide::singleton_row_boxes), so it is not also a model row. The row stays in the tree, which EXPLAIN
//! renders (FoldSingletonRows).
inline bool IsFoldedBound(const BaseExpression &row) {
	return HasDecideTag(row.GetAlias(), SINGLETON_ROW_BOUND_TAG);
}

inline void MarkFoldedBound(BaseExpression &row) {
	AddDecideTag(row.alias, SINGLETON_ROW_BOUND_TAG);
}

//! Whether a repair model loosens the folded bound `row` through its clause's own rows, one per kept row or class
//! (D28), not through one knob on the column.
inline bool FoldedBoundKeepsClauseRows(const BaseExpression &row) {
	return HasDecideTag(row.GetAlias(), SINGLETON_ROW_CLAUSE_ROWS_TAG);
}

inline void MarkFoldedBoundKeepsClauseRows(BaseExpression &row) {
	AddDecideTag(row.alias, SINGLETON_ROW_CLAUSE_ROWS_TAG);
}

//! The rows two conditions both keep, a copy of each as a conjunction so every conjunct keeps its tags (a reducer's
//! NULL test, SKIP_NULL_TAG); a null condition keeps every row, so the other stands alone, and null for both.
inline unique_ptr<Expression> DecideRowsKeptByBoth(const Expression *first, const Expression *second) {
	if (!first || !second) {
		const Expression *only = first ? first : second;
		return only ? only->Copy() : nullptr;
	}
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, first->Copy(), second->Copy());
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

//! ForEachConstraintLeaf with the scope each leaf is written under: `callback(leaf, scope)`.
template <class CALLBACK>
void ForEachScopedConstraintLeaf(const Expression &expression, CALLBACK &&callback,
                                 DecideEnclosingScope scope = DecideEnclosingScope()) {
	scope = EnterDecideScope(expression, scope);
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		callback(expression, scope);
		return;
	}
	auto &conjunction = expression.Cast<const BoundConjunctionExpression>();
	for (idx_t i = 0; i < conjunction.children.size(); i++) {
		if (IsConstraintChild(conjunction, i)) {
			ForEachScopedConstraintLeaf(*conjunction.children[i], callback, scope);
		}
	}
}

} // namespace duckdb
