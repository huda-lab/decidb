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
struct DecideReducerValueDefinition;
struct DecideObjective;
struct DecideTree;

//! One atom of a clause's left side or of the objective, as the walk lowered it: the G its reducer became, which its
//! terms range over. INVALID_INDEX for a tuple term, which has no reducer.
struct DecideLoweredAtom {
	CanonicalAtom atom;
	idx_t reducer = DConstants::INVALID_INDEX;
};

//! The term half of the one lowering walk (DecideTreeConstruction, language/guide.md §6.4): the walk reads a canonical
//! clause's atoms once, lowers each into nodes and hands the atoms with their nodes here, which splits them into the
//! records the clause's root holds (DecideConstraint, DecideObjective, a helper's DecideBody::members). Every term a
//! reducer produces names the G it ranges over (DecideTerm::child), the atom's. A reducer body has two readings by
//! two readers: what its node reads (the walk) and its coefficient algebra (here).
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
	DecideTermLowering(ClientContext &context, LogicalDecide &op, const DecideTree &tree);
	~DecideTermLowering();
	//! The terms of a canonical clause, or of a row a function owns, into `constraint`, which holds its facts. `atoms`
	//! are its left side's.
	void Clause(const BoundDecideClause &clause, const vector<DecideLoweredAtom> &atoms, DecideConstraint &constraint);
	//! The objective's records, null for one that reads no decision. `atoms` are its body's.
	unique_ptr<DecideObjective> Objective(const BoundDecideClause &objective, const vector<DecideLoweredAtom> &atoms);
	//! A helper's member terms and its frame's `else` terms, into the body of its G or O.
	void Members(DecideReducerValueDefinition &link, DecideBody &body);
	//! Once every term is split: the frames left in the records become the columns they read, and the frame columns
	//! go to `tree` (DecideTree::frame_reads).
	void Frames(DecideTree &tree);

private:
	struct Builder;
	unique_ptr<Builder> builder;
};

} // namespace duckdb
