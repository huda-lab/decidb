//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_tree.hpp
//
// The executable E/G/O graph of the one-pass algorithm (ANR, pp. 79-110).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_error_ledger.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

#include <unordered_map>

namespace duckdb {

class ClientContext;
class BoundComparisonExpression;
struct DecideEnclosingScope;

enum class DecideNodeKind : uint8_t { SCOPE, REDUCER, FRAME };
enum class DecideTreePer : uint8_t { ROW, KEY, ONE };

//! A scalar body's reference to a child's result. The physical column is outside
//! the input's columns: input_column_count + node. Decision-valued results are
//! symbolic forms; a scalar evaluator must not interpret them as SQL values.
struct DecideTreeRead {
	idx_t node;
	idx_t column;
	bool decision_value;
};

//! Where one term of a body is computed (language/guide.md §6.1): the E or O whose entries it ranges over, its
//! coefficient's program there, and the programs of the values it is computed from, which only a message reads.
struct DecideTermSite {
	idx_t node = DConstants::INVALID_INDEX;
	//! Into the site's programs; INVALID for a coefficient of 1 that stage 05 left out (a product's).
	idx_t program = DConstants::INVALID_INDEX;
	vector<idx_t> inputs;
};

//! What a program computes on a node's entries: a term's coefficient, a bound (a tightest frame bound's own, whose NULL
//! bounds nothing rather than leaving its instance without a value, D85), a member test, or a value something else
//! reads (a coefficient's inputs, an admitted bound's a and r, a square's scale).
enum class DecideProgramRole : uint8_t { COEFFICIENT, BOUND, TIGHTEST_BOUND, TEST, VALUE };

//! A value a node's programs read from another node (language/guide.md §6.1): a G over data's, a data reducer's, at an
//! entry the G's fold in the slot the entry's row reads it in, NULL where the row reaches none; or an O's, a column of
//! its frame (`column`, DecidePreparedModel::frame_columns) at the entry's row, the anchor the frame is read at.
struct DecideHole {
	idx_t node;
	LogicalType type;
	idx_t column = DConstants::INVALID_INDEX;
};

//! What an O computes on its anchors for one of its frame's columns (DecidePreparedModel::frame_columns, flat's
//! blocks ComputeDecideFrameColumns and SkipDecideRangeNullReads read them): programs on the O, its `value`, its
//! `fallback` and a range's reducer's arguments; INVALID where the column has none. Its own frame's columns are
//! computed from them before the O's other programs, which may read them.
struct DecideFrameColumnPrograms {
	idx_t node = DConstants::INVALID_INDEX;
	idx_t value = DConstants::INVALID_INDEX;
	idx_t fallback = DConstants::INVALID_INDEX;
	vector<idx_t> arguments;
};

//! An expression a node's entries compute (language/guide.md §6.1), over the node's layout: its carried columns, then
//! its program columns, then its holes, then the results of `arguments`. Built by flat's own rules
//! (ToChunkReferences), so it is the expression flat evaluates, and folded where it is executed, as flat folds it.
struct DecideProgram {
	DecideProgramRole role;
	unique_ptr<Expression> expression;
	//! What a message names: stage 05's coefficient or bound as written.
	unique_ptr<Expression> written;
	int sign = 1;
	//! The node whose rank its errors take (the holder of its term or bound); INVALID for the node's own.
	idx_t owner = DConstants::INVALID_INDEX;
	//! Its place among flat's evaluations of its owner, after the descent's reads of the owner's rows: the block (a
	//! member test in the filters, a term, the bound or an admitted end, a product, a square), a square's group or a
	//! member test's first term, and its order in the block.
	DecideBlock block = DecideBlock::FILTERS;
	idx_t group = 0;
	idx_t position = 0;
	//! The part of its owner's evaluation it lies in (DecideErrorRank::part): a composed term's place among its
	//! clause's terms.
	idx_t part = 0;
	//! Programs of the same node whose results an admitted end reads as its inputs a and r (DecideAdmittedInput).
	vector<idx_t> arguments;
};

//! f(e1..ep, G1..Gq): stage 05's own records for what a node holds, copied once stage 05 lowered them, so a term keeps
//! its identity, order, label and inputs; and where each is computed, parallel to its term vectors.
struct DecideBody {
	//! A constraint root's clause: its terms, products, squares, bound, admitted ends and facts.
	unique_ptr<DecideConstraint> clause;
	//! The clause's place in stage 05's lists, which flat's repair_group_id numbers: constraints, then singleton rows.
	idx_t clause_index = DConstants::INVALID_INDEX;
	//! The objective root's objective.
	unique_ptr<DecideObjective> objective;
	//! A value link's or a composed term's G: its inner terms; a frame link's else terms.
	vector<DecideTerm> members;
	//! A composed clause's root, or the objective's with composed terms: each term's G, in the order of its terms
	//! (LogicalDecide::ComposedMinMaxTerm).
	vector<idx_t> composed;
	vector<DecideTerm> fallback;
	vector<DecideTermSite> linear, products, fallback_sites;
	//! Per quadratic group (the objective has one, its squared terms), its inner terms' sites.
	vector<vector<DecideTermSite>> squares;
	//! A clause's bound, a program on its root, and its admitted ends, programs on its one term's site, read on its
	//! members there; INVALID when it has none.
	idx_t rhs = DConstants::INVALID_INDEX;
	idx_t lower = DConstants::INVALID_INDEX;
	idx_t upper = DConstants::INVALID_INDEX;
	//! A clause whose rows a signature splits into variants (DecideClauseScope::signature): what a variant's name reads
	//! at its first row, programs on its root, as written: its class key's elements, then each value and each `by`
	//! key's elements its signature reads, and each frame's timeline and, read relative to the anchor, its order key
	//! (language/guide.md §7.1).
	vector<idx_t> names;
	//! Of those, each collated order key and its frame: a position is named by its least spelling over its rows (D69).
	vector<pair<idx_t, idx_t>> position_names;
	//! A data check's message, a program on its hidden root; INVALID for any other node.
	idx_t check = DConstants::INVALID_INDEX;
	//! A G over data's operand: its function's arguments, programs on it, which the G folds per slot; and a reducer
	//! over frames written as a norm, its written body, whose NaN its fold refuses (the norm's definition can hide one,
	//! as SIGN reads it as 0); INVALID for any other.
	vector<idx_t> arguments;
	idx_t norm_body = DConstants::INVALID_INDEX;
	//! The rank flat's evaluation gives the stage-05 list the body was copied from (PhysicalDecide::Evaluate):
	//! constraints by their place, the objective, composed clauses, then the objective's composed terms.
	DecideErrorPhase phase = DecideErrorPhase::INVALID;
	idx_t order = DConstants::INVALID_INDEX;
};

//! Composite: E owns entries, G groups an operand E, and O holds ordered positions.
//! Expressions are owned copies, with source columns resolved to chunk references. The construction borrows the
//! addresses of the operator's reducers only between building the nodes and stage 05 lowering them
//! (DecideTreeConstruction); `compiled` holds copies of stage 05's terms.
struct DecideNode {
	explicit DecideNode(DecideNodeKind kind) : kind(kind) {
	}

	DecideNodeKind kind;
	idx_t parent = DConstants::INVALID_INDEX;
	vector<idx_t> children;
	//! Children and non-routing dependencies (shared helpers and definitions).
	vector<idx_t> dependencies;
	vector<DecideTreeRead> child_reads;
	string text;
	//! Stable reporting priority, independent of when top-down/bottom-up visits the node: of what routes its rows and
	//! of what it computes. A frame's differ, since its timeline is checked before any value is read, and so do the
	//! objective's when the scope check reads its kept rows (D72).
	DecideErrorRank route_rank {DecideErrorPhase::CONSTRAINTS, 0, 0, 0};
	DecideErrorRank value_rank {DecideErrorPhase::CONSTRAINTS, 0, 0, 0};
	//! Where flat evaluates its clause or objective (PhysicalDecide::EvaluateConstraints), which reads its programs as
	//! numbers and checks each: its value_rank, but a definition's at its place among the clauses, since flat first
	//! computes a definition where it resolves its auxiliary's absence (BuildKeyedMappings).
	DecideErrorRank clause_rank {DecideErrorPhase::CONSTRAINTS, 0, 0, 0};
	//! Where its `when` is read: its route_rank, or earlier where a helper's G or a frame's O below it inherits a gate
	//! of it, which flat reads where it reads that node's rows, before the clause.
	DecideErrorRank when_rank;
	vector<unique_ptr<Expression>> when;
	//! Generated NULL membership tests are computed after descent, never gates.
	vector<unique_ptr<Expression>> member_tests;
	DecideTreePer per = DecideTreePer::ROW;
	//! E: per key, G: by/path key, O: timeline key.
	idx_t key = DConstants::INVALID_INDEX;
	vector<DecideSignatureField> signature;
	//! The decisions its own expressions read, a defined one among them, whether or not sigma names them.
	vector<idx_t> decisions;
	//! Bare source references carried by entries, including reads outside sigma.
	vector<unique_ptr<Expression>> columns;
	//! Scalar algebra with child results replaced by child_reads' physical columns.
	unique_ptr<Expression> body;
	unique_ptr<Expression> rhs;
	unique_ptr<Expression> fallback;
	unique_ptr<Expression> over;
	//! Every argument is retained: collated extrema have a value and comparison key.
	vector<unique_ptr<Expression>> arguments;
	unique_ptr<BoundAggregateExpression> function;
	unique_ptr<DecideFrameSpec> frame_spec;
	string aggregate;
	idx_t link = DConstants::INVALID_INDEX;
	idx_t frame = DConstants::INVALID_INDEX;
	idx_t value_var = DConstants::INVALID_INDEX;
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	idx_t removal_group_id = DConstants::INVALID_INDEX;
	idx_t defined_var = DConstants::INVALID_INDEX;
	ExpressionType comparison_type = ExpressionType::INVALID;
	bool root = false;
	bool objective = false;
	bool hidden = false;
	bool decision_value = false;
	bool folded = false;
	bool repair_rows = false;
	bool minmax_easy = false;
	bool tightest_bound = false;
	//! A grouped root whose `per K:` names the classes its rows stand for (DecideClauseScope::NAMES_CLASSES): its
	//! instances are its `by` key's groups (language/guide.md §6.4), so it holds a kept row in no class of K too, in a
	//! slot of its own (DecideNodeGroups::in_a_class). So does a relation-qualified reducer's operand: its reducer
	//! reads every row its `when` keeps, and its `per K'` keeps no value of a row in no class of K' (§5.3). So does an
	//! O an anchor in no timeline (§5.6, D75).
	bool holds_rows_in_no_class = false;
	//! Where flat reads its key: a grouped clause's root and its G's of its `by` key read it where flat groups the kept
	//! rows, after their terms, and a helper's G after its members' rows (DecideBlock::BY_KEY), so a row the key raises
	//! on still reaches them; any other key routes its node's rows (ROUTE).
	DecideBlock key_block = DecideBlock::ROUTE;
	//! A hidden root's non-NULL string is the data check's error message.
	bool value_check = false;
	//! Flat reads a reducer's own `when` inside the filter of its terms, window by window, before any term is
	//! computed: on a node that holds terms, the first of them (DecideProgram::group), which its `when` ranks under;
	//! INVALID on a node whose rows are routed before any term is read.
	idx_t filter_group = DConstants::INVALID_INDEX;
	//! The part of its owner's evaluation that filter is read in (DecideErrorRank::part): 0, but 1 for a node of the
	//! objective's products alone, whose filters flat reads after the objective's other terms.
	idx_t filter_part = 0;
	//! Immutable clause metadata shared with flat lowering. Its term vectors are
	//! empty: the graph's bodies and child results supply the model's terms.
	unique_ptr<DecideConstraint> facts;
	//! Stage 05's terms for what the node holds (CompileDecideTree).
	DecideBody compiled;
	//! What its entries compute, and its member tests among them (TEST programs).
	vector<DecideProgram> programs;
	vector<idx_t> tests;
	//! The source columns its programs read beyond `columns` (a query-wide scale on a reducer's operand), as chunk
	//! references. Its entries carry them (DecideEntryLayout::program_columns), but they are no field and split no
	//! entry.
	vector<unique_ptr<Expression>> program_columns;
	//! The data reducers its programs read, after its program columns in its layout.
	vector<DecideHole> holes;
};

struct DecideTree {
	idx_t decide_index = DConstants::INVALID_INDEX;
	idx_t input_column_count = 0;
	vector<unique_ptr<DecideNode>> nodes;
	vector<idx_t> roots;
	//! Deterministic children-first order, including shared and definition edges.
	vector<idx_t> eval_order;
	vector<string> source_fragments;
	vector<DecideKey> keys;
	vector<KeyedScopeInfo> keyed_scopes;
	vector<DecideVarScopeInfo> variable_scopes;
	//! The G/O defining each helper decision, INVALID for a user decision.
	vector<idx_t> value_nodes;
	//! Roots defining an ABS/body/indicator helper, in written order.
	vector<vector<idx_t>> definition_nodes;
	//! Per keyed scope, the operand of the reducer or link it qualifies, whose instances are its classes split by what
	//! its body reads (KeyedScopeInfo::signature); INVALID for a scope that qualifies none.
	vector<idx_t> qualified_operands;
	vector<LogicalDecide::BilinearLink> bilinear_links;
	vector<LogicalDecide::AbsMaximizeLink> abs_maximize_links;
	vector<DecideBoundBox> singleton_row_boxes;
	vector<LogicalDecide::UserBoundSpec> user_singleton_row_bounds;
	vector<idx_t> signed_helper_vars;
	//! Per frame (LogicalDecide::frames), its O; per reducer over data reading a frame
	//! (LogicalDecide::frame_aggregates), its G.
	vector<idx_t> frame_nodes;
	vector<idx_t> frame_aggregate_nodes;
	//! Per frame column (DecidePreparedModel::frame_columns), what its O computes for it; a reducer's over frames
	//! (AGGREGATE) is its G's fold.
	vector<DecideFrameColumnPrograms> frame_columns;

	//! Reject dangling edges, cycles, invalid keys and non-physical source reads.
	void Validate() const;
	//! Stable plan-order dump of nodes, sigma, routing and computation dependencies.
	string ToString() const;
	//! Each body's terms with the node they range over and where they are computed (DECIDB_DUMP_BODIES).
	string BodiesToString() const;
};

//! What the compile reads of the construction: the node each of the operator's reducers became, and the nodes stage
//! 05's lists bind to, in the order stage 05 walks them.
struct DecideTreeSources {
	//! The operator's own reducer -> its G (DecideTreeConstruction's Lower).
	std::unordered_map<const Expression *, idx_t> reducer_nodes;
	//! The clause roots in constraint-leaf order, the order stage 05 lowers the clauses in.
	vector<idx_t> clause_roots;
	idx_t objective_root = DConstants::INVALID_INDEX;
	//! [spec][term] -> the composed term's G; the objective's composed terms last.
	vector<vector<idx_t>> composed_reducers;
	//! [spec] -> the composed clause's root.
	vector<idx_t> composed_roots;
	//! [link] -> its helper's G, or its O for a frame.
	vector<idx_t> link_nodes;
	//! [record] -> its G (LogicalDecide::retained_bound_aggregates).
	vector<idx_t> record_nodes;
};

//! Builder with a director (plan_decide.cpp): the nodes come from the final clauses before stage 05 lowers them, the
//! bodies from what stage 05 lowered (language/guide.md §6.4). Between the two, ReducerChild is stage 05's hook.
class DecideTreeConstruction {
public:
	//! Builds the nodes, their edges, sigma, gates and definitions.
	DecideTreeConstruction(ClientContext &context, const LogicalDecide &op);
	~DecideTreeConstruction();
	//! The node whose operand entries a reducer stage 05 lowers ranges over (DecideReducerChild).
	idx_t ReducerChild(const BoundAggregateExpression &reducer) const;
	//! Copies stage 05's terms into the bodies, orders the nodes and ranks their errors, and validates the tree.
	unique_ptr<DecideTree> Finish();

private:
	struct Builder;
	unique_ptr<Builder> builder;
};

//! Binds the clause roots to stage 05's clauses in leaf order, copies stage 05's terms into each node's body, finds
//! where each is computed and compiles its coefficient, bounds and member tests into programs there; a binding that
//! does not match is an internal error.
void CompileDecideTree(ClientContext &context, const LogicalDecide &op, DecideTree &tree,
                       const DecideTreeSources &sources);

//! A G's operand: its one SCOPE child.
idx_t DecideTreeOperand(const DecideTree &tree, idx_t reducer);

//! The G whose operand entries `reducer`'s terms range over: `reducer` itself, or, while its operand's body is exactly
//! a read of another G (a nest stage 05 reads inlined), that G. Such a G is over decisions with no helper: stage 05
//! lowers a reducer of decisions, the binder refuses a cast over one, and a reducer of a helper's value is a helper
//! itself, whose terms stage 05 lowers with no G.
idx_t DecideTreeMembersOf(const DecideTree &tree, idx_t reducer);

//! The semantic name a carrier represents, shared by tree consumers.
string DecideTreeReducerName(const BoundAggregateExpression &aggregate);

//! Read shape, provenance and bound-admission facts once. Both graph construction
//! and flat term lowering use this function; neither infers a second clause shape.
unique_ptr<DecideConstraint> ReadDecideClauseFacts(ClientContext &context, const LogicalDecide &op,
                                                const BoundComparisonExpression &comparison,
                                                const DecideEnclosingScope &scope);

} // namespace duckdb
