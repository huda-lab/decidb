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
//! SolverInput): each has one arm, which calls flat's own blocks. The first ten have theirs; a query with any other
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
	//! A clause in the aggregate shape (EvaluatedConstraint::lhs_is_aggregate): one model row per group of its `by`
	//! key, named by the key's values.
	GROUP_ROWS,
	//! A `<>` clause's rows, each excluding its bound (EvaluatedConstraint::ne_clause_idx), which stage 06 expands.
	NOT_EQUAL_ROWS,
	//! A row defining an auxiliary, which has no value where the row's reads have none (SolverInput::absent_reads).
	DEFINED_READS,
	//! A clause in the per-row shape under `per K:` (DecideClauseScope::ONE_ROW_PER_CLASS): one model row per variant
	//! of each class of K, named by the class.
	CLASS_ROWS,
	//! The classes a clause's rows stand for, which a diagnosis reports (SolverInput::class_sets).
	CLASS_SETS,
	//! A relation-qualified reducer's classes, split by the values its body reads (KeyedScopeInfo::signature).
	KEYED_SIGNATURE,
	PRODUCT_TERMS,
	SQUARE_TERMS,
	TIGHTEST_BOUND,
	EXTREMUM_ROWS,
	MEMBER_ROWS,
	OBJECTIVE_SQUARES,
	OBJECTIVE_PRODUCTS,
	OBJECTIVE_EXTREMUM,
	OBJECTIVE_COMPOSED,
	COMPOSED,
	VALUE_LINKS,
	FRAME_READS,
	PRODUCT_LINKS
};

//! The variants of a signature over the rows a node holds (language/guide.md §7.1, D70), numbered as flat numbers them,
//! and the classes of its key, numbered by the key's values: each row's, INVALID_INDEX for a row the node does not
//! hold in a class.
struct DecideVariants {
	vector<idx_t> of_row;
	vector<idx_t> class_of_row;
	idx_t count = 0;
	idx_t num_classes = 0;
};

//! Writes what PhysicalDecide::Evaluate writes for the model, from the tree's containers: gstate's evaluated clauses
//! and the keyed mappings, in flat's order, so frame reads and repair groups are numbered as flat numbers them.
class DecideModelEmitter {
public:
	DecideModelEmitter(const PhysicalDecide &op, const DecideTree &tree, const DecideDescent &descent,
	                   DecideTreeColumns &columns, DecideGlobalSinkState &gstate, PerGroupCache &groups,
	                   const DecideSourceNames &names, idx_t num_rows, DecideErrorLedger &errors);

	//! The query's first shape with no arm, empty when every shape it has has one. Such a query takes the interim
	//! formulator, or verify's flat second run, until every shape has one; so does one with a node the tree computes
	//! no value for (DecideTreeColumns::Uncomputed).
	static string Uncovered(const DecideTree &tree, const PhysicalDecide &op);

	//! Each declared decision's classes, numbered by its key's values, and each relation-qualified reducer's, its
	//! classes split by its body's values, a row in none in a class of its own; then the rows at which each auxiliary
	//! has no value (DecideGlobalSinkState::absent_reads).
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
	//! The names' key values, which stage 06 reads a group's name by (SolverInput::key_values).
	PerGroupCache &groups;
	const DecideSourceNames &names;
	idx_t num_rows;
	DecideErrorLedger &errors;

	//! Each clause's root by its place in stage 05's lists (DecideBody::clause_index).
	vector<idx_t> ClauseRoots() const;
	//! A clause in the per-row shape: one model row per kept row of its root, or under `per K:` one per variant of
	//! each class among them (Classes).
	EvaluatedConstraint Rows(idx_t root, bool counted);
	//! The model rows of a clause in the per-row shape under `per K:` (DecideClauseScope::ONE_ROW_PER_CLASS), its
	//! kept rows `rows`: one per variant of each class of K, at its first row with a value (D88), numbered and named
	//! as flat numbers and names them, and the classes each stands for under DIAGNOSE.
	void Classes(idx_t root, const vector<pair<idx_t, idx_t>> &rows, EvaluatedConstraint &evaluated);
	//! The variants of `signature` (null: none, so its key's classes) over the rows `node` holds, its instances, in
	//! flat's numbering (BuildSignatureIds).
	DecideVariants Variants(idx_t node, const DecideSignature *signature) const;
	//! The names of `root`'s variants, by their class's values and their own, read at their first rows as written
	//! (DecideBody::names, DecideNameVariants).
	vector<GroupName> NameVariants(idx_t root, const DecideSignature &signature, const vector<idx_t> &class_of,
	                               const vector<idx_t> &first_rows, const vector<idx_t> &variants_per_class);
	//! Each row `root` holds in a class of its `per K:` key, its class, numbered by K's values, and each class's
	//! name, which the class sets read.
	void KeyClasses(idx_t root, vector<idx_t> &class_ids, vector<GroupName> &class_names);
	//! A relation-qualified reducer's scope: the variants of its operand over the rows the reducer reads.
	KeyedMapping SignatureMapping(idx_t scope);
	//! The rows at which each auxiliary has no value (D88): a row its definition keeps where what the definition reads
	//! unreduced is NULL, and a row where a decision the definition reads has no instance.
	void ReadAbsence();
	//! A clause in the aggregate shape: one model row per group of its `by` key, its kept rows grouped by flat's own
	//! rules (GroupRowsByShape).
	EvaluatedConstraint Groups(idx_t root);
	//! A grouped clause's terms on `rows`, its kept rows and their root's entries, with each term's filter as flat
	//! reads it, and the rows some reducer's filter keeps (`active`, every row with a term that keeps every kept row).
	//! Returns whether some term's filter keeps fewer than the kept rows.
	bool ReadGroupedTerms(idx_t root, const vector<pair<idx_t, idx_t>> &rows, const vector<bool> &kept,
	                      EvaluatedConstraint &evaluated, vector<TermFilterState> &filters, vector<bool> &active);
	//! A term beside a grouped clause's reducers, on each of `rows` (the kept rows and their root's entries).
	void ReadBesideReducers(idx_t root, const DecideTerm &term, const DecideTermSite &site,
	                        const vector<pair<idx_t, idx_t>> &rows, CoefficientColumn &column);
	//! A term in a reducer, on each member of the G it ranges over: an entry of its operand, which the reducer's `when`
	//! routed and its member tests keep, a kept row in no group among them, since flat computes the term on every kept
	//! row. Returns the rows its filter keeps.
	vector<bool> ReadInReducer(const DecideTerm &term, const DecideTermSite &site, CoefficientColumn &column,
	                           TermFilterState &filter);
	//! The grouped root's G of its `by` key.
	idx_t ByReducer(idx_t root) const;
	//! Each of `kept_groups`, groups of the G `by`, named by its key's values.
	void Name(idx_t by, const vector<idx_t> &kept_groups, EvaluatedConstraint &evaluated);
};

//! The declared keyed scopes' mappings from the descent's classes, numbered by value, with the rows in no class in one
//! of their own (KeyedMapping::absent_class).
vector<KeyedMapping> DecideDeclaredMappings(const DecideDescent &descent);

//! The values that name each group of the G `node`, by its rank: its key's least spelling over every row the query read
//! the key on (DecideDescent::Label, D69).
SolverInput::KeyValues DecideGroupKeyValues(const DecideTree &tree, const DecideDescent &descent, idx_t node);

//! Each row `node`'s entries stand for, with the entry standing for it: a set E's routed rows, a node per row's own.
vector<pair<idx_t, idx_t>> DecideEntryRows(const DecideTree &tree, const DecideDescent &descent, idx_t node);

//! The scope check's message for an objective with more than one instance over its kept rows (D72,
//! DecideObjectiveTwiceMessage): `node`'s sigma read on the entries of `container` that `surviving` names, an instance
//! each.
string DecideObjectiveDifference(const DecideTree &tree, const DecideNode &node, const DecideNodeContainer &container,
                                 const vector<idx_t> &surviving);

} // namespace duckdb
