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
//! SolverInput): each has one arm, which calls flat's own blocks. The first twenty-one have theirs; a query with any
//! other has none yet (DecideModelEmitter::Uncovered).
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
	//! A MIN or a MAX the clause's direction does not drive (EvaluatedConstraint::minmax_agg_type): its group's rows,
	//! the members stage 06 takes the extremum over.
	EXTREMUM_ROWS,
	//! A MIN or a MAX its direction drives (DecideConstraint::was_minmax_easy): a model row per member, grouped by its
	//! `by` key, or a bound on one decision admitted on each member.
	MEMBER_ROWS,
	//! A MIN or a MAX objective, whose auxiliary stage 06 writes over its terms' rows.
	OBJECTIVE_EXTREMUM,
	//! A clause adding MIN, MAX, SUM and AVG reducers (EvaluatedClauses::composed_constraints): each a column over the
	//! members of its reducer, which stage 06 gives an auxiliary and one outer row.
	COMPOSED,
	//! The objective's composed terms (EvaluatedClauses::composed_objective_terms).
	OBJECTIVE_COMPOSED,
	//! A clause's products of two decisions (EvaluatedConstraint::bilinear_terms), each a column over the rows it is
	//! read on.
	PRODUCT_TERMS,
	//! A clause's squares (EvaluatedConstraint::quadratic_groups): each a sign, its query-wide factor applied, and a
	//! column per term of what it squares.
	SQUARE_TERMS,
	//! The objective's squares, a column per term of what it squares (SolverInput::quadratic_inner_coefficients).
	OBJECTIVE_SQUARES,
	//! The objective's products of two decisions (SolverInput::bilinear_objective_terms).
	OBJECTIVE_PRODUCTS,
	//! A product with a BOOLEAN factor the optimizer made an auxiliary of (LogicalDecide::BilinearLink), whose rows
	//! stage 06 writes, and which has no value where a factor has none (SolverInput::absent_reads).
	PRODUCT_LINKS,
	//! A helper decision of a reducer the value rewrite lifted (SolverInput::reducer_value_links): its members per
	//! group of its `by` key, and its classes, the groups that hold one (KeyedMapping).
	VALUE_LINKS,
	TIGHTEST_BOUND,
	FRAME_READS
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
	DecideModelEmitter(const PhysicalDecide &op, ClientContext &context, const DecideTree &tree,
	                   const DecideDescent &descent, DecideTreeColumns &columns, DecideGlobalSinkState &gstate,
	                   PerGroupCache &groups, const DecideSourceNames &names, idx_t num_rows, DecideErrorLedger &errors,
	                   DataChunk &first);

	//! The query's first shape with no arm, empty when every shape it has has one. Such a query takes the interim
	//! formulator, or verify's flat second run, until every shape has one; so does one with a node the tree computes
	//! no value for (DecideTreeColumns::Uncomputed).
	static string Uncovered(const DecideTree &tree, const PhysicalDecide &op);

	//! Each declared decision's classes, numbered by its key's values, each relation-qualified reducer's, its classes
	//! split by its body's values, and each helper's, the groups that hold a member, a row in none in a class of its
	//! own; and the rows at which each auxiliary has no value (DecideGlobalSinkState::absent_reads).
	void KeyedMappings(vector<KeyedMapping> &mappings);
	//! Every clause in flat's order, then the folded bounds' rows under DIAGNOSE.
	void Constraints();
	//! The objective's linear terms, its squared terms and its products, each on the rows it reads, and each one's
	//! filter of those rows in `evaluated`, which the qualifiers and AVG then read (PhysicalDecide::EvaluateObjective);
	//! none without an objective.
	void Objective(PhysicalDecide::EvaluatedClauses &evaluated);
	//! The composed MIN/MAX clauses, then the objective's composed terms, each term a column over the members of its G
	//! (DecideBody::composed), as flat reads them after the objective.
	void Composed(PhysicalDecide::EvaluatedClauses &evaluated);
	//! Each helper's members (DecideGlobalSinkState::evaluated_value_links), per group of its `by` key that holds
	//! one, as flat lists them after the composed clauses.
	void ValueLinks();

private:
	//! A helper's members (language/guide.md §7.2): the entries of its G's operand it reads, every decision they read
	//! with an instance, and those it lists, each with its row, its G's slots numbered by rank among those that hold
	//! one.
	struct LinkMembers {
		vector<bool> reads;
		vector<pair<idx_t, idx_t>> rows;
		vector<idx_t> group_of_slot;
		idx_t groups = 0;
	};
	const PhysicalDecide &op;
	ClientContext &context;
	const DecideTree &tree;
	const DecideDescent &descent;
	DecideTreeColumns &columns;
	DecideGlobalSinkState &gstate;
	//! The names' key values, which stage 06 reads a group's name by (SolverInput::key_values).
	PerGroupCache &groups;
	const DecideSourceNames &names;
	idx_t num_rows;
	DecideErrorLedger &errors;
	//! The input's first chunk, where a value the whole query shares is read (FirstRowValue).
	DataChunk &first;
	//! Per link, its helper's members, read once where its classes are (HelperMapping), which ValueLinks lists.
	vector<LinkMembers> link_members;

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
	//! unreduced is NULL, and a row where a decision the definition reads has no instance; and each helper's classes,
	//! in the order the definitions read each other.
	void ReadAbsence(vector<KeyedMapping> &mappings);
	//! The rows at which the auxiliary `root`'s clause defines has no value.
	void DefinitionAbsence(idx_t root);
	LinkMembers Members(idx_t link);
	//! A helper's classes: the groups of its `by` key that hold a member, each read by the rows of its group the
	//! clause keeps; its members kept for ValueLinks (link_members).
	KeyedMapping HelperMapping(idx_t link);
	//! A bound on one decision: the values it admits on each row its term is read on, which the model builder folds
	//! into the decision's column.
	void Admit(const DecideBody &body, EvaluatedConstraint &evaluated);
	//! A clause in the grouped shape, the aggregate shape or a MIN or MAX: one model row per group of its `by` key, its
	//! kept rows grouped by flat's own rules (GroupRowsByShape), or one per member of an easy MIN or MAX; its instances
	//! `counted` unless it is a folded bound's, which only a repair model emits.
	EvaluatedConstraint Groups(idx_t root, bool counted);
	//! The composed terms `terms` that `root`, a composed clause's or the objective's, holds: each read on the members
	//! of its G, its factor on the input's `first` chunk (`what` names the clause or the objective); `null_factor` set
	//! by a NULL one.
	vector<ComposedMinMaxTermData> ComposedTerms(idx_t root, const vector<LogicalDecide::ComposedMinMaxTerm> &terms,
	                                             const char *what, bool &null_factor);
	//! A factor the whole query shares (a composed term's, a square's), as flat reads it: on the input's first row,
	//! whatever a clause keeps.
	Value FirstRowValue(const Expression &factor);
	//! A clause's products and squares (EvaluatedConstraint::bilinear_terms, quadratic_groups), once its rows are
	//! grouped: each term on the rows its site's members stand for, each AVG over its group's rows its filter keeps
	//! (`filters`: the terms', then the products', then the squares'), and each square's factor applied; a NULL
	//! coefficient, a decision with no instance or a NULL factor leaves an instance no value (`null_rows`).
	void Polynomial(idx_t root, const vector<TermFilterState> &filters, vector<bool> &null_rows,
	                EvaluatedConstraint &evaluated);
	//! A term of a product or a square at its site `site`, its column on each row its site's members stand for, a
	//! relation-qualified one's first of each class, which flat computes it on once it has left the repeats out
	//! (language/guide.md §5.1): a NULL coefficient there, or a decision of `variables` with no instance there, leaves
	//! the row's instance no value (`null_rows`, D84).
	void ReadPart(const DecideTermSite &site, bool qualified, int sign, const vector<unique_ptr<Expression>> &inputs,
	              const vector<idx_t> &variables, CoefficientColumn &column, vector<bool> &null_rows);
	//! The sign of `root`'s square `g`, its factor applied (ScaleSquaredGroup), read as flat reads it on the input's
	//! first row, whatever the clause keeps, first in the square's part (DecideErrorRank::part); a NULL factor leaves
	//! every instance of the clause no value.
	double SquareSign(idx_t root, idx_t g, vector<bool> &null_rows);
	//! The objective's terms `terms` (its linear or its squared terms) at their sites `sites`: each its decision as a
	//! row reads it, its column and its filter, the rows it is read on (D84).
	vector<TermFilterState> ObjectiveTerms(idx_t root, const vector<DecideTerm> &terms,
	                                       const vector<DecideTermSite> &sites, vector<CoefficientColumn> &coefficients,
	                                       vector<idx_t> &variables);
	//! An objective AVG of a `by` key (`avg_group_key`, D81), in the G `child`, its site `site`: each member's group
	//! there, which its 1/n divides by (TermFilterState::avg_group_ids).
	void AvgGroups(bool avg_scale, idx_t avg_group_key, idx_t child, idx_t site, TermFilterState &filter);
	//! A grouped clause's terms on `rows`, its kept rows and their root's entries, with each term's, product's and
	//! square's filter as flat reads it, and the rows some reducer's filter keeps (`active`, every row with a part that
	//! keeps every kept row). Returns whether some filter keeps fewer than the kept rows.
	bool ReadGroupedTerms(idx_t root, const vector<pair<idx_t, idx_t>> &rows, const vector<bool> &kept,
	                      EvaluatedConstraint &evaluated, vector<TermFilterState> &filters, vector<bool> &active);
	//! The rows a term in a reducer is read on, each with the entry of its site `site` standing for it: of the entries
	//! `reads` marks, every row each stands for, but a relation-qualified reducer's term its entry's first row alone,
	//! the first of its class (language/guide.md §5.1).
	vector<pair<idx_t, idx_t>> ReadRows(idx_t site, const vector<bool> &reads, bool qualified) const;
	//! The entries of `site` a term in a reducer is read on: those its member tests keep, a relation-qualified
	//! reducer's first of each class (language/guide.md §5.1).
	vector<bool> ReadEntriesOf(idx_t site, bool qualified);
	//! A reducer's filter of a part of a grouped clause (a term, a product, a square), its site `site`: the rows the
	//! part is read on (ReadRows, ReadEntriesOf), returned with their entries, and every row its members stand for
	//! (`members`), which decide whether the filter keeps fewer than the clause.
	vector<pair<idx_t, idx_t>> Filter(idx_t site, bool qualified, TermFilterState &filter, vector<bool> &members);
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
