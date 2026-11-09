//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_model_emitter.hpp
//
// The tree's back end (language/guide.md §6.4): an Adapter (GoF) from the computed containers to what stage 06 reads,
// so the tree and flat formulate through one code path.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/decide/decide_tree_columns.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"

namespace duckdb {

class DecideGlobalSinkState;

//! The shapes stage 06 accepts, by what it reads of the evaluated clauses and the query (EvaluatedConstraint,
//! SolverInput): each has one arm, which calls flat's own blocks. The first four have theirs; a query with any other
//! has none yet (DecideModelEmitter::Uncovered).
enum class DecideModelShape : uint8_t {
	//! A clause with one model row per kept row.
	ROWS,
	//! A folded bound's rows, which only a repair model emits (DecidePreparedModel::singleton_row_constraints).
	REPAIR_ROWS,
	//! A declared decision's classes (KeyedScopeInfo).
	KEYED_DECLARED,
	//! An objective's linear terms, each a column over the rows it reads (SolverInput::objective_coefficients).
	OBJECTIVE_LINEAR,
	PRODUCT_TERMS,
	SQUARE_TERMS,
	TIGHTEST_BOUND,
	NOT_EQUAL_ROWS,
	DEFINED_READS,
	EXTREMUM_ROWS,
	MEMBER_ROWS,
	GROUP_ROWS,
	CLASS_ROWS,
	OBJECTIVE_SQUARES,
	OBJECTIVE_PRODUCTS,
	OBJECTIVE_EXTREMUM,
	OBJECTIVE_COMPOSED,
	COMPOSED,
	VALUE_LINKS,
	FRAME_READS,
	PRODUCT_LINKS,
	//! A relation-qualified reducer's classes, split by the values its body reads (KeyedScopeInfo::signature).
	KEYED_SIGNATURE
};

//! Writes what PhysicalDecide::Evaluate writes for the model, from the tree's containers: gstate's evaluated clauses
//! and the keyed mappings, in flat's order, so frame reads and repair groups are numbered as flat numbers them.
class DecideModelEmitter {
public:
	DecideModelEmitter(const PhysicalDecide &op, const DecideTree &tree, const DecideDescent &descent,
	                   DecideTreeColumns &columns, DecideGlobalSinkState &gstate, const DecideSourceNames &names,
	                   idx_t num_rows, DecideErrorLedger &errors);

	//! The query's first shape with no arm, empty when every shape it has has one. Such a query takes the interim
	//! formulator, or verify's flat second run, until every shape has one; so does one with a node the tree computes
	//! no value for (DecideTreeColumns::Uncomputed).
	static string Uncovered(const DecideTree &tree, const PhysicalDecide &op);

	//! Each declared decision's classes, numbered by its key's values, a row in none in a class of its own.
	vector<KeyedMapping> KeyedMappings();
	//! Every clause in flat's order, then the folded bounds' rows under DIAGNOSE.
	void Constraints();
	//! The objective's linear terms, each on the rows it reads, and each term's filter of those rows, which the
	//! qualifiers and AVG then read (PhysicalDecide::EvaluateObjective); none without an objective.
	vector<TermFilterState> Objective();

private:
	const PhysicalDecide &op;
	const DecideTree &tree;
	const DecideDescent &descent;
	DecideTreeColumns &columns;
	DecideGlobalSinkState &gstate;
	const DecideSourceNames &names;
	idx_t num_rows;
	DecideErrorLedger &errors;

	//! A clause in the per-row shape: one model row per kept row of its root.
	EvaluatedConstraint Rows(idx_t root, bool counted);
};

//! The declared keyed scopes' mappings from the descent's classes, numbered by value, with the rows in no class in one
//! of their own (KeyedMapping::absent_class).
vector<KeyedMapping> DecideDeclaredMappings(const DecideDescent &descent);

//! The scope check's message for an objective with more than one instance over its kept rows (D72,
//! DecideObjectiveTwiceMessage): `node`'s sigma read on the entries of `container` that `surviving` names, an instance
//! each.
string DecideObjectiveDifference(const DecideTree &tree, const DecideNode &node, const DecideNodeContainer &container,
                                 const vector<idx_t> &surviving);

} // namespace duckdb
