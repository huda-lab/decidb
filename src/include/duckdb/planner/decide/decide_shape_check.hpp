//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_shape_check.hpp
//
// The one place the algebra of a DECIDE clause is judged.
//===----------------------------------------------------------------------===//
//
// DecideShapeCheck runs once, in PlanSelectNode, right after canonicalization: every
// clause and the objective are in the one spelling (decide_product_form.hpp), so a
// shape has one reading here, whatever way the user wrote it. It is the semantic-analysis
// pass of a compiler front end, after the parser and the binder have refused only what
// they alone can see. It refuses, for each clause as written:
//   - a function or operator over a decision outside the model's vocabulary (+, -, *,
//     a division by data, a whole-exponent POWER, ABS, the reducers and the markers);
//   - a POWER over a decision whose exponent is not a whole number known before the
//     query runs;
//   - a division by a decision;
//   - a CASE over a decision (a CASE over data is a per-row coefficient);
//   - a term of degree above 2, which no backend takes;
//   - a clause that reads no decision at all (a comparison, or the objective's body);
//   - a square, a product or an ABS of decisions inside a frame.
// Each refusal states what the model does not take and points at the term.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;
class BoundComparisonExpression;
struct DecideSourceNames;
struct BoundDecideClause;
struct DecideFrameSpec;

//! A strict or <> comparison over a quantity that can take fractional values: `<`, `>` and `<>` step the bound onto
//! the whole numbers, which is exact only on them.
string FractionalComparisonMessage(const string &comparison, const string &operand);

//! Whether every value `expr` can take is whole, by declared types, every addend counted: `sum(x + 0.5)` is not,
//! `sum(power(x, 2))` is. The test a helper decision standing for a reducer takes (stage 05's value rewrite), by the
//! same rules as the integer-step check of a comparison.
bool DecideValueIsWholeNumbered(const Expression &expr, idx_t decide_index);

class DecideShapeCheck {
public:
	DecideShapeCheck(ClientContext &context, idx_t decide_index, const DecideSourceNames &names,
	                 const vector<DecideFrameSpec> &frames);

	//! Judge every comparison of the canonical clauses.
	void CheckConstraints(const vector<BoundDecideClause> &clauses) const;
	//! Judge the canonical objective.
	void CheckObjective(const BoundDecideClause &objective) const;

private:
	//! One clause of the query, `whole`, read as `sides`: a comparison and its two sides, or the objective's body. It
	//! raises when no side reads a decision, then judges each side.
	void CheckClause(const BoundDecideClause &clause, const vector<const Expression *> &sides) const;
	//! A `<`, `>` or `<>` compares a side that is whole-numbered by declared types, else it is refused.
	void CheckIntegerStep(const BoundComparisonExpression &comparison) const;
	//! One side of a comparison, or the objective: its vocabulary, its frames, then the degree of each term.
	void CheckSide(const Expression &side) const;
	//! The frames under `expr`: each reads its payload and `else` linearly.
	void CheckFrames(const Expression &expr) const;
	void CheckFramePart(const Expression &part) const;
	//! Refuse the first decision-bearing node under `expr` the model has no reading for.
	void CheckVocabulary(const Expression &expr) const;
	[[noreturn]] void Refuse(const Expression &term, const string &problem) const;

	ClientContext &context;
	idx_t decide_index;
	const DecideSourceNames &names;
	//! The frame selectors (LogicalDecide::frames), whose `else` stands in no bound tree.
	const vector<DecideFrameSpec> &frames;
};

} // namespace duckdb
