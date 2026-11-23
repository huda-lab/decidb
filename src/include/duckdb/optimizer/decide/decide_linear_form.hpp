//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/optimizer/decide/decide_linear_form.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
class ClientContext;
class Expression;
class LogicalDecide;
struct BoundDecideClause;
struct DecideBody;
struct DecideConstraint;
struct DecideHelperDefinition;
struct DecideObjective;
struct DecideTree;

//! The G each reducer of the clause in hand became, by the reducer's address in that clause.
using DecideReducerNodes = unordered_map<const Expression *, idx_t>;

//! The term half of the one lowering walk (DecideTreeConstruction, language/guide.md §6.4): the walk lowers a canonical
//! clause into nodes and hands the same clause here, which splits it into the records its root holds
//! (DecideConstraint, DecideObjective, a helper's DecideBody::members). Every term a reducer produces names the G it
//! ranges over (DecideTerm::child), read from `reducers`, which the walk fills as it lowers that clause.
//!
//! It runs at physical planning (PhysicalPlanGenerator::CreatePlan of a LogicalDecide), after the DECIDE optimizer has
//! finished and the column bindings are final. It must stay after every rewrite and after singleton-row folding: the
//! rewrites emit fresh constraint rows through `LogicalDecide::AddClause`, and folding marks the clauses it
//! folded into the variable box. Lowering earlier would miss the former and re-emit the latter.
//!
//! Doing the algebra here rather than at execution time is what lets a rebuilt
//! coefficient be bound through `FunctionBinder` instead of inheriting another
//! node's signature. Nothing in this pass reads a data row -- it needs types,
//! not values -- so every coefficient stays an unevaluated `Expression` for
//! stage 08 to evaluate against the relational input.
class DecideTermLowering {
public:
	DecideTermLowering(ClientContext &context, LogicalDecide &op, const DecideTree &tree,
	                   const DecideReducerNodes &reducers);
	~DecideTermLowering();
	//! The terms of a canonical clause, or of a row a function owns, into `constraint`, which holds its facts.
	void Clause(const BoundDecideClause &clause, DecideConstraint &constraint);
	//! The objective's records, null for one that reads no decision.
	unique_ptr<DecideObjective> Objective(const BoundDecideClause &objective);
	//! A helper's member terms and its frame's `else` terms, into the body of its G or O.
	void Members(DecideHelperDefinition &link, DecideBody &body);
	//! Once every term is split: the frames left in the records become the columns they read, and the frame columns
	//! go to `tree` (DecideTree::frame_reads).
	void Frames(DecideTree &tree);

private:
	struct Builder;
	unique_ptr<Builder> builder;
};

} // namespace duckdb
