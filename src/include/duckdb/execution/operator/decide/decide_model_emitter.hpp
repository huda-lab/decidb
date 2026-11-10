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

//! The variants of a signature over the rows a node holds (language/guide.md §7.1, D70), numbered as flat numbers them,
//! and the classes of its key, numbered by the key's values: each row's, INVALID_INDEX for a row the node does not
//! hold in a class.
struct DecideVariants {
	vector<idx_t> of_row;
	vector<idx_t> class_of_row;
	idx_t count = 0;
	idx_t num_classes = 0;
};

//! Each row `node`'s entries stand for, with the entry standing for it: a set E's routed rows, a node per row's own,
//! read in place from its container.
class DecideEntryRows {
public:
	DecideEntryRows(const DecideTree &tree, const DecideDescent &descent, idx_t node);
	idx_t size() const {
		return routed ? routed->size() : rows->size();
	}
	pair<idx_t, idx_t> operator[](idx_t i) const {
		return routed ? (*routed)[i] : pair<idx_t, idx_t>((*rows)[i], i);
	}
	class Iterator {
	public:
		Iterator(const DecideEntryRows &rows, idx_t i) : rows(rows), i(i) {
		}
		pair<idx_t, idx_t> operator*() const {
			return rows[i];
		}
		Iterator &operator++() {
			i++;
			return *this;
		}
		bool operator!=(const Iterator &other) const {
			return i != other.i;
		}

	private:
		const DecideEntryRows &rows;
		idx_t i;
	};
	Iterator begin() const {
		return Iterator(*this, 0);
	}
	Iterator end() const {
		return Iterator(*this, size());
	}
	//! `read(row, entry)` for each row and the entry standing for it, in order.
	template <class F>
	void ForEach(F &&read) const {
		if (routed) {
			for (auto &pair : *routed) {
				read(pair.first, pair.second);
			}
			return;
		}
		const auto data = rows->data();
		for (idx_t entry = 0; entry < rows->size(); entry++) {
			read(data[entry], entry);
		}
	}

private:
	//! A set E's routed rows; null for a node per row, whose entries are its rows.
	const unsafe_vector<pair<idx_t, idx_t>> *routed = nullptr;
	const unsafe_vector<idx_t> *rows = nullptr;
};

//! Writes what PhysicalDecide::Evaluate writes for the model, from the tree's containers: gstate's evaluated clauses
//! and the keyed mappings, in flat's order, so frame reads and repair groups are numbered as flat numbers them.
class DecideModelEmitter {
public:
	DecideModelEmitter(const PhysicalDecide &op, ClientContext &context, const DecideTree &tree,
	                   const DecideDescent &descent, DecideTreeColumns &columns, DecideGlobalSinkState &gstate,
	                   PerGroupCache &groups, const DecideSourceNames &names, idx_t num_rows, DecideErrorLedger &errors,
	                   DataChunk &first);

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
	//! kept rows those its entries `tested` marks stand for: one per variant of each class of K, at its first row with
	//! a value (D88), numbered and named as flat numbers and names them, and the classes each stands for under
	//! DIAGNOSE.
	void Classes(idx_t root, const vector<bool> &tested, EvaluatedConstraint &evaluated);
	//! The variants of `signature` (null: none, so its key's classes) over the rows `node` holds, its instances, in
	//! flat's numbering (BuildSignatureIds); with `by_row` false only how many there are.
	DecideVariants Variants(idx_t node, const DecideSignature *signature, bool by_row = true) const;
	//! What tells `entry` of `node` apart as a variant (language/guide.md §7.1, D70): its class of the node's key and
	//! its sigma's values, a frame by its keys.
	vector<Value> Identity(idx_t node, idx_t entry) const;
	//! What frame `frame`'s `else` reads at `row` (D82, flat's FrameFieldClasses): whether the anchor reaches it, and
	//! where it does, its terms' coefficients and its decisions' instances, which split a variant.
	vector<Value> Fallback(idx_t frame, idx_t row) const;
	//! The scope check's message for an objective `root` with more than one variant over its kept rows (D72,
	//! DecideObjectiveTwiceMessage): its sigma read on the entries `surviving` names, a variant each, and its frames
	//! by their keys.
	string ObjectiveDifference(idx_t root, const vector<idx_t> &surviving) const;
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
	//! A range frame's helper's members (language/guide.md §5.6), per window of its frame (DecideListFrameMembers): its
	//! payload's terms on the rows the windows read, its `else`'s at the anchors that reach it, each a program on the
	//! frame's O `node`, as flat lists them (PhysicalDecide::EvaluateFrameLink).
	void FrameMembers(idx_t node, const LogicalDecide::ReducerValueLink &link, ReducerValueMembers &out);
	//! A grouped clause's bound on each of `rows` (its kept rows and their root's entries), a NULL one marking its row
	//! in `null_rows`; for a tightest frame bound (D85) the rows where it bounds nothing instead, which are returned
	//! (TakeTightestFrameBound): empty for any other bound, and for one the whole clause shares (DescribeBound).
	vector<bool> GroupBound(idx_t root, const DecideEntryRows &rows, vector<bool> &null_rows,
	                        EvaluatedConstraint &evaluated);
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
	//! keeps every kept row), and the rows where a term's coefficient is NULL (`null_rows`). Returns whether some
	//! filter keeps fewer than the kept rows.
	bool ReadGroupedTerms(idx_t root, const DecideEntryRows &rows, const vector<bool> &kept,
	                      EvaluatedConstraint &evaluated, vector<TermFilterState> &filters, vector<bool> &active,
	                      vector<bool> &null_rows);
	//! The rows a term in a reducer is read on, each with the entry of its site `site` standing for it: of the entries
	//! `reads` marks, every row each stands for, but a relation-qualified reducer's term its entry's first row alone,
	//! the first of its class (language/guide.md §5.1).
	vector<pair<idx_t, idx_t>> ReadRows(idx_t site, const vector<bool> &reads, bool qualified) const;
	//! ReadRows' rows, each passed to `read(row, entry)` in order, none listed.
	template <class F>
	void ForEachRead(idx_t site, const vector<bool> &reads, bool qualified, F &&read) const;
	//! The entries of `site` a term in a reducer is read on: those its member tests keep, a relation-qualified
	//! reducer's first of each class (language/guide.md §5.1), which `first` then holds.
	const vector<bool> &ReadEntriesOf(idx_t site, bool qualified, vector<bool> &first);
	//! A reducer's filter of a part of a grouped clause (a term, a product, a square), its site `site`: the rows the
	//! part is read on (ReadRows, ReadEntriesOf), each passed to `read(row, entry)`, and every row its members stand
	//! for (`members`), which decide whether the filter keeps fewer than the clause.
	template <class F>
	void Filter(idx_t site, bool qualified, TermFilterState &filter, vector<bool> &members, F &&read);
	//! A term beside a grouped clause's reducers, on each of `rows` (the kept rows and their root's entries), a NULL
	//! coefficient marking its row (`null_rows`).
	void ReadBesideReducers(idx_t root, const DecideTerm &term, const DecideTermSite &site, const DecideEntryRows &rows,
	                        CoefficientColumn &column, vector<bool> &null_rows);
	//! A term in a reducer, on each member of the G it ranges over: an entry of its operand, which the reducer's `when`
	//! routed and its member tests keep, a kept row in no group among them, since flat computes the term on every kept
	//! row; a NULL coefficient marks its row (`null_rows`). Returns the rows its filter keeps.
	vector<bool> ReadInReducer(const DecideTerm &term, const DecideTermSite &site, CoefficientColumn &column,
	                           TermFilterState &filter, vector<bool> &null_rows);
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



} // namespace duckdb
