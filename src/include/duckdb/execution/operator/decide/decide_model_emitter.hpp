//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_model_emitter.hpp
//
// The tree's back end (language/guide.md §6.4): an Adapter (GoF) from the computed containers to what stage 06 reads.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/decide/decide_tree_columns.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"

namespace duckdb {

class DecideGlobalSinkState;

//! One clause as the emitter reads it: the row, with the record of each row it emits (ClauseRecords), and what only
//! some kinds of clause carry. `Constraints` routes it to the list of its kind (Route), so a row never says what kind
//! it is.
struct EmittedClause {
	EvaluatedClauseColumns row;
	//! The rows the clause keeps, in the input's order: `row`'s columns hold one value for each, by its place here.
	vector<idx_t> rows;
	//! The kept rows of each row the clause emits, by place: what an aggregate clause's records are written from, and
	//! where a row's product groups are read.
	ClauseRowSets sets;
	//! A grouped `<>` row's NULL bounds, which exclude no value (NotEqualClause::excluded).
	vector<bool> no_exclusion;
	//! A `<>` whose AVG scales its bound by its group's size (NotEqualClause::excluded).
	bool avg_rhs_scale = false;
	//! The terms of degree 2 (QuadraticClause::products).
	vector<EvaluatedProductGroup> products;
};

//! The variants of a signature over the rows a node holds (language/guide.md §7.1, D70), numbered by value, and the
//! classes of its key, numbered by the key's values: each entry's of the node, INVALID_INDEX for one the node does not
//! hold in a class.
struct DecideVariants {
	vector<idx_t> of_entry;
	vector<idx_t> class_of_entry;
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

//! Writes what the model reads, from the tree's containers: gstate's evaluated clauses and the keyed mappings, in the
//! order of the written clauses, so repair groups are numbered in that order.
class DecideModelEmitter {
public:
	DecideModelEmitter(const PhysicalDecide &op, ClientContext &context, const DecideTree &tree,
	                   const DecideDescent &descent, DecideTreeColumns &columns, DecideGlobalSinkState &gstate,
	                   SolverInput &input, const DecideSourceNames &names, idx_t num_rows);

	//! Each declared decision's classes, numbered by its key's values, each relation-qualified reducer's, its classes
	//! split by its body's values, and each helper's, the groups that hold a member, a row in none in a class of its
	//! own (SolverInput::keyed_mappings); and the rows at which each auxiliary has no value (absent_reads).
	void KeyedMappings();
	//! Every clause in written order, then the folded bounds' rows under DIAGNOSE.
	void Constraints();
	//! The objective: its linear part as one coefficient per instance of each decision it reads, its constant
	//! (SolverInput::objective_coefficients, ::objective_constant) and its product groups (::objective_product_groups).
	void Objective();
	//! Each helper's members, per group of its `by` key that holds one, listed before any clause or the objective is
	//! read. A helper the plan keeps a column for is handed to stage 06 (SolverInput::reducer_value_links);
	//! one it writes where it is read (DecideHelperForm) is kept here, and each reader's term on it is written as its
	//! members (WriteMembersIn), so stage 06 meets no read of it.
	void ValueLinks();
	//! What stage 06 reads of the rows once every term is written, by instance: each auxiliary's instances that every
	//! row reads with no value (SolverInput::absent_instances).
	void AbsentInstances();

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
	//! What stage 06 reads, which every step here writes into.
	SolverInput &input;
	//! Per decision, the rows whose instance of it has no value beyond its keyed scope's absent class: an auxiliary
	//! whose definition reads an instance with none there (D88); empty where none.
	vector<vector<bool>> absent_reads;
	//! The objective's linear terms as they are read, a column over the input's rows each, with the decision each
	//! reads, which ObjectiveCoefficients sums and frees.
	vector<CoefficientColumn> objective_columns;
	vector<idx_t> objective_variables;
	//! The objective's linear terms and its product groups, each on the rows it reads, and each one's filter of those
	//! rows, which its AVGs read; none without an objective.
	void ReadObjective(vector<TermFilterState> &linear_filters, vector<TermFilterState> &group_filters);
	//! The objective's linear part summed per instance, once its columns are final. A term on a helper the plan writes
	//! where it is read is that helper's members.
	void ObjectiveCoefficients();
	const DecideSourceNames &names;
	idx_t num_rows;
	//! The place of each row the clause at hand keeps among its kept rows (Place), INVALID_INDEX for every other row:
	//! how a row another node holds is read into the clause's columns, which are as long as its kept rows. One vector
	//! for every clause, set and cleared on the kept rows alone, so a clause costs what it keeps.
	vector<idx_t> place_of_row;
	//! Per keyed scope, its class at each row (KeyedMappings).
	vector<DecideScopeRows> scope_rows;
	//! How many rows the clause at hand keeps.
	idx_t placed = 0;
	//! Per value-link node, its helper's members, read once where its classes are (HelperRows), which ValueLinks
	//! lists.
	vector<LinkMembers> link_members;
	//! A helper the plan writes where it is read: its members, and each instance's group among them. A SUM or AVG is
	//! written in `whole`. Of the two forms the data settles, the groups that pass the form's test are written in (a
	//! MIN or MAX group of one member, a SUM or AVG group one row reads), and an instance of another group keeps its
	//! column (INVALID_INDEX) and its link, which `link` holds until the helper is `settled`.
	struct WrittenHelper {
		ReducerValueMembers members;
		vector<idx_t> group_of_instance;
		bool whole = false;
		bool settled = false;
		string label;
		ReducerValueLinkSpec link;
	};
	vector<WrittenHelper> written_helpers;
	//! How many of them wait for the clause that reads them (DecideHelperForm::ONE_ROW_WRITTEN_IN).
	idx_t unsettled_helpers = 0;
	//! Per plan decision, its entry of `written_helpers`; INVALID_INDEX for every other.
	vector<idx_t> written_helper_of;
	//! The helpers listed, their instances, members and members' terms, and how many are handed no link.
	struct HelperCounts {
		idx_t helpers = 0, instances = 0, members = 0, terms = 0, written_in = 0;
	};
	HelperCounts helper_counts;

	//! Hands a listed helper to stage 06, or keeps the instances its plan form writes where they are read.
	void HandLink(ReducerValueLinkSpec &&spec, DecideHelperForm form);
	//! Ends a kept helper's listing: each group `written_in` names is written where it is read, and the others are
	//! handed to stage 06 with their columns.
	void Settle(idx_t entry, const std::function<bool(idx_t)> &written_in);
	//! Settles each waiting helper a clause's rows name, from how many of those rows read each of its instances: a
	//! group one row reads is written into that row.
	void SettleRowHelpers(const ClauseRecords &records);
	//! Settles each helper no clause's rows named: every group keeps its column.
	void SettleUnreadHelpers();
	//! How a data row gives the instance of `variable` it reads, through frame `frame_read` if a frame reads it there;
	//! of a term's decision, and of each of `terms`'; of no decision (INVALID_INDEX), nothing a reader asks for.
	DecideInstanceOfRow InstanceOfRow(idx_t variable, idx_t frame_read = DConstants::INVALID_INDEX) const;
	DecideInstanceOfRow InstanceOfRow(const DecideTerm &term) const {
		return InstanceOfRow(term.variable_index, term.frame_read);
	}
	vector<DecideInstanceOfRow> InstancesOfRows(const vector<DecideTerm> &terms) const;
	//! The objective's linear terms' (ObjectiveTerms), which ObjectiveCoefficients reads.
	vector<DecideInstanceOfRow> objective_instances;
	//! How many instances `variable` has: a row each for a decision per row, its key's classes, or 1.
	idx_t Instances(idx_t variable) const;
	//! The instances `term`'s decision has at the data rows `rows` lists by place, or at every data row with no list.
	TermInstances InstancesAt(const DecideTerm &term, const vector<idx_t> *rows) const;
	//! Writes each term of a list that reads a written-in instance as that instance's members, scaled by the term's
	//! coefficient, their constants added to the constant of the term's row: `begin` delimits the rows, as a clause's
	//! records and a helper's members do. A read of an instance with no member has no value: the objective drops it
	//! (`absent_reads_nothing`, D84), and no clause row or member reaches here with one. Returns whether any term
	//! named such a helper.
	bool WriteMembersIn(vector<idx_t> &begin, vector<idx_t> &variables, vector<idx_t> &rows,
	                    vector<double> &coefficients, vector<double> &constants, bool absent_reads_nothing) const;
	//! The same over a clause's records, whose decisions (EvaluatedConstraint::variable_indices) are then those its
	//! rows name.
	void WriteMembersIn(EvaluatedConstraint &clause);

	//! The rows the clause of `root` keeps, in the input's order, each with the entry standing for it, and numbered by
	//! its place among them (place_of_row); `rows` takes them. Unplace clears the numbering once the clause is read.
	vector<pair<idx_t, idx_t>> Keep(idx_t root, vector<idx_t> &rows);
	void Unplace(const vector<idx_t> &rows);
	//! Each clause's root by its place in stage 05's lists (DecideBody::clause_index).
	vector<idx_t> ClauseRoots() const;
	//! A clause in the per-row shape: one model row per kept row of its root, or under `per K:` one per variant of
	//! each class among them (Classes). Its records are written from the root's entries, with no column between.
	EmittedClause Rows(idx_t root, bool counted);
	//! Puts an evaluated clause in the list of its kind: a folded bound's among the repair model's rows, a `<>` in
	//! SolverInput::not_equals, one with terms of degree 2 in ::quadratics, any other in ::constraints.
	//! `clause` as a message refers to it, for a sum past a DOUBLE.
	DecideClauseRef Written(const DecideConstraint &clause) const;
	void Route(EmittedClause &&clause, bool folded);
	//! One of an ABS F node's two envelope rows, evaluated by the existing row evaluator, which supplies the scoped
	//! instances, into the node's record (SolverInput::abs_links).
	void AbsEnvelope(idx_t function, idx_t root, bool counted);
	//! The model rows of a clause in the per-row shape under `per K:` (DecideClauseScope::ONE_ROW_PER_CLASS): one per
	//! variant of each class of K, at its first row with a value (D88), numbered and named by value as Variants does,
	//! and the classes each stands for under DIAGNOSE.
	void Classes(idx_t root, const vector<pair<idx_t, idx_t>> &rows, EmittedClause &clause);
	//! The variants of `signature` (null: none, so its key's classes) over the rows `node` holds, its instances, in
	//! the numbering of DecideVariants; with `by_row` false only how many there are.
	DecideVariants Variants(idx_t node, const DecideSignature *signature, bool by_row = true) const;
	//! What tells `entry` of `node` apart as a variant (language/guide.md §7.1, D70): its class of the node's key and
	//! its sigma's values, a frame by its keys.
	vector<Value> Identity(idx_t node, idx_t entry) const;
	//! What frame `frame`'s `else` reads at `row` (D82): whether the anchor reaches it, and
	//! where it does, its terms' coefficients and its decisions' instances, which split a variant.
	vector<Value> Fallback(idx_t frame, idx_t row) const;
	//! The scope check's message for an objective `root` with more than one variant over its kept rows (D72,
	//! DecideObjectiveTwice), and where it points: its sigma read on the entries `surviving` names, a variant each, and
	//! its frames by their keys.
	DecideVariantDifference ObjectiveDifference(idx_t root, const vector<idx_t> &surviving) const;
	//! The names of `root`'s variants, by their class's values and their own, read at their first rows as written
	//! (DecideBody::names, DecideNameVariants), each at its place among the root's rows.
	vector<GroupName> NameVariants(idx_t root, const DecideSignature &signature, const vector<pair<idx_t, idx_t>> &rows,
	                               const vector<idx_t> &class_of, const vector<idx_t> &first_places,
	                               const vector<idx_t> &variants_per_class);
	//! Each row `root` holds, by place, its class of the root's `per K:` key, numbered by K's values (INVALID_INDEX in
	//! none), and each class's name, which the class sets read.
	void KeyClasses(idx_t root, const vector<pair<idx_t, idx_t>> &rows, vector<idx_t> &class_ids,
	                vector<GroupName> &class_names);
	//! A relation-qualified reducer's scope: the variants of its operand over the rows the reducer reads.
	DecideScopeRows SignatureRows(idx_t scope);
	//! The rows at which each auxiliary has no value (D88): a row its definition keeps where what the definition reads
	//! unreduced is NULL, and a row where a decision the definition reads has no instance; and each helper's classes,
	//! in the order the definitions read each other.
	void ReadAbsence(vector<KeyedMapping> &mappings);
	//! The rows at which the auxiliary `root`'s clause defines has no value.
	void DefinitionAbsence(idx_t root, idx_t variable);
	void FunctionAbsence(idx_t function);
	LinkMembers Members(idx_t node);
	//! A helper's classes: the groups of its `by` key that hold a member, each read by the rows of its group the
	//! clause keeps; its members kept for ValueLinks (link_members).
	DecideScopeRows HelperRows(idx_t node);
	//! A range frame's helper's members (language/guide.md §5.6), per window of its frame (DecideListFrameMembers): its
	//! payload's terms on the rows the windows read, its `else`'s at the anchors that reach it, each a program on the
	//! frame's O `node`.
	void FrameMembers(idx_t node, ReducerValueMembers &out);
	//! A grouped clause's bound on each of `rows` (its kept rows and their root's entries), a NULL one marking its row
	//! in `null_rows`; for a tightest frame bound (D85) the rows where it bounds nothing instead, which are returned
	//! (TakeTightestFrameBound): empty for any other bound, and for one the whole clause shares (DescribeBound).
	vector<bool> GroupBound(idx_t root, const vector<pair<idx_t, idx_t>> &rows, vector<bool> &null_rows,
	                        EmittedClause &clause);
	//! A bound on one decision: the values it admits on each row its term is read on, which the model builder folds
	//! into the decision's column.
	void Admit(const DecideBody &body, EvaluatedClauseColumns &evaluated);
	//! The domain check on each row a per-row bound on one decision writes (the instances `evaluated` keeps, an easy
	//! MIN's or MAX's members among them): a bound the decision's type cannot hold (`per g: x <= -1` on a BOOL or a
	//! non-negative INT) is an error pointing at the clause, as for a bound folded into the decision's box
	//! (DecideRefuseDomain). Only a bound written as a constant is checked, and its box takes the lowest floor any
	//! clause lowers the decision to (DecideTree::domains), so a clause the solve could satisfy is never
	//! refused.
	void CheckDomain(idx_t root, const EvaluatedClauseColumns &evaluated);
	//! A clause in the grouped shape, the aggregate shape or a MIN or MAX: one model row per group of its `by` key, its
	//! kept rows grouped as GroupRowsByShape groups them, or one per member of an easy MIN or MAX; its instances
	//! `counted` unless it is a folded bound's, which only a repair model emits. Its terms are read into columns as
	//! long as its kept rows, which its records are written from (DecideWriteClauseRecords).
	EmittedClause Groups(idx_t root, bool counted);
	//! A clause's product groups (EmittedClause::products), once its rows are grouped: each group's weight
	//! and its forms' coefficients on the rows its site's members stand for, its weight 0 on every other row and
	//! divided by its AVG's count (`filters`: the terms', then the groups'), each read as a term in its place is
	//! (ReadBesideReducers on `rows`, ReadInReducer).
	void Polynomial(idx_t root, const vector<pair<idx_t, idx_t>> &rows, const vector<TermFilterState> &filters,
	                vector<bool> &null_rows, EmittedClause &result);
	//! The objective's linear terms `terms` at their sites `sites`: each its decision, its column and its filter, the
	//! rows it is read on (ObjectivePart).
	vector<TermFilterState> ObjectiveTerms(const vector<DecideTerm> &terms, const vector<DecideTermSite> &sites,
	                                       vector<CoefficientColumn> &coefficients, vector<idx_t> &variables);
	//! An objective AVG of a `by` key (`avg_group_key`, D81), in the G `child`, its site `site`: each member's group
	//! there, which its 1/n divides by (TermFilterState::avg_group_ids).
	void AvgGroups(bool avg_scale, idx_t avg_group_key, idx_t child, idx_t site, TermFilterState &filter);
	//! A grouped clause's terms on `rows`, its kept rows and their root's entries, with each term's and product
	//! group's filter, and the rows some reducer's filter keeps (`active`, every row with a part that
	//! keeps every kept row), and the rows where a term's coefficient is NULL (`null_rows`). Returns whether some
	//! filter keeps fewer than the kept rows.
	bool ReadGroupedTerms(idx_t root, const vector<pair<idx_t, idx_t>> &rows, EvaluatedClauseColumns &evaluated,
	                      vector<TermFilterState> &filters, vector<bool> &active, vector<bool> &null_rows);
	//! The rows a term in a reducer is read on, each with the entry of its site `site` standing for it: of the entries
	//! `reads` marks, every row each stands for, but a relation-qualified reducer's term its entry's first row alone,
	//! the first of its class (language/guide.md §5.1).
	vector<pair<idx_t, idx_t>> ReadRows(idx_t site, const vector<bool> &reads, bool qualified) const;
	//! ReadRows' rows, each passed to `read(row, entry)` in order, none listed.
	template <class F>
	void ForEachRead(idx_t site, const vector<bool> &reads, bool qualified, F &&read) const;
	//! The entries of `site` a term in a reducer is read on, limited to rows selected by the exact `per` reducer
	//! occurrence. The two sites differ when a qualified reducer contains a nested reducer.
	const vector<bool> &ReadEntriesOf(const DecideTermSite &site, idx_t qualifier_scope, const vector<bool> &candidates,
	                                  vector<bool> &first, vector<bool> &selected_rows);
	//! Read the selected entries only at rows chosen by the relation-qualified reducer's operand.
	template <class F>
	void ForEachTermRead(idx_t site, const vector<bool> &reads, idx_t qualifier_scope,
	                     const vector<bool> &selected_rows, F &&read) const;
	//! A reducer's filter of a part of a grouped clause (a term or a product group), its site `site`: the rows the
	//! part is read on (ReadRows, ReadEntriesOf), each passed to `read(place, row, entry)`, and every row its members
	//! stand for (`members`), which decide whether the filter keeps fewer than the clause. Both are by the place of a
	//! row the clause keeps; a row it does not keep is passed over.
	template <class F>
	void Filter(idx_t site, idx_t qualifier_scope, const vector<bool> &reads, const vector<bool> &selected_rows,
	            TermFilterState &filter, vector<bool> &members, F &&read);
	//! Read entries when the qualified scope is the site's own node, as for a helper's members.
	const vector<bool> &ReadEntriesOf(idx_t site, bool qualified, vector<bool> &first);
	//! What is read into a column of the rows a reader holds: a term's coefficient, or a product group's weight or one
	//! of its forms' coefficients. Its site, the relation-qualified reducer its reads go through, the term whose
	//! decision it reads (none for a weight) and its value where stage 05 left its program out, a weight of 1.
	struct Part {
		const DecideTermSite &site;
		idx_t qualifier_scope;
		const DecideTerm *term;
		double unit;

		Part(const DecideTerm &term, const DecideTermSite &site)
		    : site(site), qualifier_scope(term.qualifier_scope_idx), term(&term), unit(term.sign) {
		}
		Part(const DecideTerm &term, const DecideTermSite &site, idx_t qualifier_scope)
		    : site(site), qualifier_scope(qualifier_scope), term(&term), unit(term.sign) {
		}
		Part(const DecideTermSite &site, idx_t qualifier_scope, double unit)
		    : site(site), qualifier_scope(qualifier_scope), term(nullptr), unit(unit) {
		}
	};
	//! A part's program on `entries` of its site (DecideTreeColumns::Numbers), or its unit on every entry.
	vector<double> Numbers(const Part &part, const vector<bool> &entries, vector<bool> *nulls);
	//! The three places a part is read in, each with its rule for a row where the part has no value (D84):
	//!
	//!   beside a clause's reducers   on each kept row; the row's instance has no value         ReadBesideReducers
	//!   in a clause's reducer        on each member; the row is no member                      ReadInReducer
	//!   in the objective             on each row it keeps or each member; the part is dropped  ObjectivePart
	//!
	//! and a NULL coefficient leaves a clause's instance no value (`null_rows`), while the objective has none (stage
	//! 05 read one as 0). A relation-qualified reducer's part is computed on every member and read on the first of
	//! each class (language/guide.md §5.1).
	//!
	//! A part beside a grouped clause's reducers, on each of `rows` (the kept rows and their root's entries).
	void ReadBesideReducers(const Part &part, const vector<pair<idx_t, idx_t>> &rows, CoefficientColumn &column,
	                        vector<bool> &null_rows);
	//! A part in a reducer, on each member of the G it ranges over: an entry of its operand, which the reducer's `when`
	//! routed and its member tests keep, a kept row in no group among them, since the part is computed on every kept
	//! row. Returns the rows its filter keeps.
	vector<bool> ReadInReducer(const Part &part, CoefficientColumn &column, TermFilterState &filter,
	                           vector<bool> &null_rows);
	//! A part of the objective by data row, and the rows it is read on.
	void ObjectivePart(const Part &part, CoefficientColumn &column, vector<bool> &read_rows);
	//! The grouped root's G of its `by` key.
	idx_t ByReducer(idx_t root) const;
	//! Each of `kept_groups`, groups of the G `by`, named by its key's values.
	void Name(idx_t by, const vector<idx_t> &kept_groups, EvaluatedClauseColumns &evaluated);
};

//! The declared keyed scopes' mappings from the descent's classes, numbered by value, with the rows in no class in one
//! of their own (KeyedMapping::absent_class).
vector<KeyedMapping> DecideDeclaredMappings(const DecideDescent &descent);

//! The values that name each group of the G `node`, by its rank: its key's least spelling over every row the query read
//! the key on (DecideDescent::Label, D69).
SolverInput::KeyValues DecideGroupKeyValues(const DecideTree &tree, const DecideDescent &descent, idx_t node);

} // namespace duckdb
