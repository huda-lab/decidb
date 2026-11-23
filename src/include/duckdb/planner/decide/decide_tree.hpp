//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/decide/decide_tree.hpp
//
// The executable E/G/O graph of the one-pass algorithm (ANR, pp. 79-110).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

#include <unordered_map>

namespace duckdb {

class ClientContext;
class BoundComparisonExpression;

enum class DecideNodeKind : uint8_t { SCOPE, REDUCER, FRAME, FUNCTION };
enum class DecideTreePer : uint8_t { ROW, KEY, ONE };

//! Where one term of a body is computed (language/guide.md §6.1): the E or O whose entries it ranges over, its
//! coefficient's program there, and the programs of the values it is computed from, which only a message reads.
struct DecideTermSite {
	idx_t node = DConstants::INVALID_INDEX;
	//! Operand of the exact relation-qualified reducer occurrence, when one qualifies this term.
	idx_t qualifier_operand = DConstants::INVALID_INDEX;
	//! Into the site's programs; INVALID for a coefficient of 1 that stage 05 left out (a product group's weight).
	idx_t program = DConstants::INVALID_INDEX;
};

//! What a program computes on a node's entries: a term's coefficient, a bound (a tightest frame bound's own, whose NULL
//! bounds nothing rather than leaving its instance without a value, D85), a member test, or a value something else
//! reads (an admitted bound's a and r).
enum class DecideProgramRole : uint8_t { COEFFICIENT, BOUND, TIGHTEST_BOUND, TEST, VALUE };

//! A value a node's programs read from another node (language/guide.md §6.1): a G over data's, a data reducer's, at an
//! entry the G's fold in the slot the entry's row reads it in, NULL where the row reaches none; or an O's, a column of
//! its frame (`column`, DecideFrameReads::columns) at the entry's row, the anchor the frame is read at.
struct DecideHole {
	idx_t node;
	LogicalType type;
	idx_t column = DConstants::INVALID_INDEX;
};

//! What an O computes on its anchors for one of its frame's columns (DecideFrameReads::columns, which
//! ComputeDecideFrameColumns and SkipDecideRangeNullReads read): programs on the O, its `value`, its
//! `fallback` and a range's reducer's arguments; INVALID where the column has none. Its own frame's columns are
//! computed from them before the O's other programs, which may read them.
struct DecideFrameColumnPrograms {
	idx_t node = DConstants::INVALID_INDEX;
	idx_t value = DConstants::INVALID_INDEX;
	idx_t fallback = DConstants::INVALID_INDEX;
	vector<idx_t> arguments;
};

//! An expression a node's entries compute (language/guide.md §6.1), over the node's layout: its carried columns, then
//! its program columns, then its holes, then the results of `arguments`. Built by ToChunkReferences and folded where
//! it is executed (TransformToChunkExpression), as every expression stage 08 evaluates is.
struct DecideProgram {
	DecideProgramRole role;
	unique_ptr<Expression> expression;
	//! What a message names: stage 05's coefficient or bound as written.
	unique_ptr<Expression> written;
	int sign = 1;
	//! The node whose clause a message names (the holder of its term or bound); INVALID for the node's own.
	idx_t owner = DConstants::INVALID_INDEX;
	//! Programs of the same node whose results an admitted end reads as its inputs a and r (DecideAdmittedInput).
	vector<idx_t> arguments;
};

//! Where a product group's weight and each term of its forms are computed (DecideProductGroup): all on its site, the
//! operand of the G its reducer ranges over, else its holder.
struct DecideGroupSites {
	DecideTermSite weight;
	vector<DecideTermSite> left, right;
};

//! The stage-05 list a body was copied from (DecideBody::kind); NONE for a node of entries.
enum class DecideBodyKind : uint8_t { NONE, CONSTRAINT, OBJECTIVE, VALUE_LINK };

//! f(e1..ep, G1..Gq): the records stage 05's lowering (DecideTermLowering) writes for what a node holds, so a term
//! keeps its identity, order, label and inputs; and where each is computed, parallel to its term vectors.
struct DecideBody {
	//! A constraint root's clause: its terms, product groups, bound, admitted ends and facts.
	unique_ptr<DecideConstraint> clause;
	//! The clause's place in stage 05's lists, which number the repair groups: constraints, then singleton rows.
	idx_t clause_index = DConstants::INVALID_INDEX;
	//! The objective root's objective.
	unique_ptr<DecideObjective> objective;
	//! A value link's G: its inner terms; a frame link's else terms.
	vector<DecideTerm> members;
	vector<DecideTerm> fallback;
	vector<DecideTermSite> linear, fallback_sites;
	//! Per product group of the clause or the objective, where it is computed.
	vector<DecideGroupSites> groups;
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
	//! Parallel to `names`: the column or value each reads, as the query writes it (`t.g`, `P.lim`).
	vector<string> texts;
	//! Of those, each collated order key and its frame: a position is named by its least spelling over its rows (D69).
	vector<pair<idx_t, idx_t>> position_names;
	//! A data check's message, a program on its hidden root; INVALID for any other node.
	idx_t check = DConstants::INVALID_INDEX;
	//! A G over data's operand: its function's arguments, programs on it, which the G folds per slot; empty for any
	//! other.
	vector<idx_t> arguments;
	//! The stage-05 list the body was copied from, and its place there: constraints by their place, the objective.
	DecideBodyKind kind = DecideBodyKind::NONE;
	idx_t order = DConstants::INVALID_INDEX;
};

//! Composite: E owns entries, G groups an operand E, and O holds ordered positions.
//! Expressions are owned copies, with source columns resolved to chunk references. `compiled` holds the terms the
//! walk split into the node (DecideTreeConstruction).
struct DecideNode {
	explicit DecideNode(DecideNodeKind kind) : kind(kind) {
	}

	DecideNodeKind kind;
	idx_t parent = DConstants::INVALID_INDEX;
	vector<idx_t> children;
	//! Children and non-routing dependencies (shared helpers and definitions).
	vector<idx_t> dependencies;
	string text;
	vector<unique_ptr<Expression>> when;
	//! Generated NULL membership tests are computed after descent, never gates.
	vector<unique_ptr<Expression>> member_tests;
	DecideTreePer per = DecideTreePer::ROW;
	//! E: per key, G: by/path key, O: timeline key.
	idx_t key = DConstants::INVALID_INDEX;
	//! The fields the descent splits this node's entries by: what the descent and `Identity` read, where `signatures`
	//! is what `Variants` reads.
	vector<DecideSignatureField> signature;
	//! The decisions its own expressions read, a defined one among them, whether or not sigma names them.
	vector<idx_t> decisions;
	//! Bare source references carried by entries, including reads outside sigma.
	vector<unique_ptr<Expression>> columns;
	//! Scalar algebra with each child's result replaced by its physical column, input_column_count + child. A
	//! decision-valued result is a symbolic form; a scalar evaluator must not interpret it as an SQL value.
	unique_ptr<Expression> body;
	unique_ptr<Expression> rhs;
	unique_ptr<Expression> fallback;
	unique_ptr<Expression> over;
	//! Every argument is retained: collated extrema have a value and comparison key.
	vector<unique_ptr<Expression>> arguments;
	unique_ptr<BoundAggregateExpression> function;
	//! An F node: the roots of the rows it owns, its children. An ABS's two lower-envelope rows, `aux >= operand` then
	//! `aux >= -operand` (MakeAbsRow), which carry the operand per row to stage 06; an L0 indicator's link rows; the
	//! one row that defines a definition's helper. Such a root's `parent` is the F node.
	vector<idx_t> rows;
	string aggregate;
	//! The helper definition (DecideTree::definitions) the node holds the value of: an F node's, or the one whose
	//! helper a linked G or O defines. INVALID_INDEX for every other node.
	idx_t definition = DConstants::INVALID_INDEX;
	//! Whether the node is a G or an O that defines a helper's value (a REDUCER_VALUE definition's).
	bool Linked() const {
		return kind != DecideNodeKind::FUNCTION && definition != DConstants::INVALID_INDEX;
	}
	//! A FRAME node's selector: an index into DecideTree::frames, INVALID for any other node.
	idx_t frame = DConstants::INVALID_INDEX;
	//! A G node's relation-qualified scope, whose classes it reads at most once; INVALID_INDEX when unqualified.
	idx_t qualifier_scope = DConstants::INVALID_INDEX;
	DecideRowOrigin origin;
	idx_t defined_var = DConstants::INVALID_INDEX;
	bool root = false;
	//! A clause root whose instances are its distinct sigmas over every class: classes whose rows agree on all they
	//! read are one instance (language/guide.md §7.1). See DecideCanonicalizer::MergesClasses.
	bool merges_classes = false;
	bool objective = false;
	bool hidden = false;
	bool decision_value = false;
	bool folded = false;
	//! A grouped root whose `per K:` names the classes its rows stand for (DecideClauseScope::NAMES_CLASSES): its
	//! instances are its `by` key's groups (language/guide.md §6.4), so it holds a kept row in no class of K too, in a
	//! slot of its own (DecideNodeGroups::in_a_class). So does a relation-qualified reducer's operand: its reducer
	//! reads every row its `when` keeps, and its `per K'` keeps no value of a row in no class of K' (§5.3). So does an
	//! O an anchor in no timeline (§5.6, D75).
	bool holds_rows_in_no_class = false;
	//! A hidden root's non-NULL string is the data check's error message.
	bool value_check = false;
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
	idx_t input_column_count = 0;
	//! The direction the objective was written in (LogicalDecide::written_sense): FEASIBILITY for none.
	DecideSense written_sense = DecideSense::FEASIBILITY;
	vector<unique_ptr<DecideNode>> nodes;
	//! The clause roots holding a clause, numbered in leaf order (DecideBody::clause_index): the constraints first,
	//! then the folded bounds whose repair rows come from their clause.
	idx_t constraint_count = 0;
	idx_t singleton_row_count = 0;
	//! The frame columns the clauses' terms read (DecideTermLowering).
	DecideFrameReads frame_reads;
	//! The constraints' records, by clause_index; a folded bound's is none of them.
	vector<const DecideConstraint *> Constraints() const;
	//! The objective's records, null for none.
	const DecideObjective *Objective() const;
	vector<idx_t> roots;
	//! The F nodes: ABS helpers, Boolean products, L0 indicators and definitions.
	vector<idx_t> function_nodes;
	//! Deterministic children-first order, including shared and definition edges.
	vector<idx_t> eval_order;
	//! Every key of the clause, its expressions resolved against the input chunk.
	vector<DecideKey> keys;
	vector<KeyedScopeInfo> keyed_scopes;
	vector<DecideVarScopeInfo> variable_scopes;
	//! The G/O defining each helper decision, INVALID for a user decision.
	vector<idx_t> value_nodes;
	//! Nodes defining ABS/body/indicator helpers, in written order; F nodes join the clause roots here.
	vector<vector<idx_t>> definition_nodes;
	//! Per keyed scope, the operand of the reducer or link it qualifies, whose instances are its classes split by what
	//! its body reads (KeyedScopeInfo::signature); INVALID for a scope that qualifies none.
	vector<idx_t> qualified_operands;
	vector<DecideBoundBox> singleton_row_boxes;
	vector<LogicalDecide::UserBoundSpec> user_singleton_row_bounds;
	vector<idx_t> signed_helper_vars;
	//! Per frame (LogicalDecide::frames), its O; per reducer over data reading a frame
	//! (LogicalDecide::frame_aggregates), its G.
	vector<idx_t> frame_nodes;
	vector<idx_t> frame_aggregate_nodes;
	//! Per frame column (DecideFrameReads::columns), what its O computes for it; a reducer's over frames
	//! (AGGREGATE) is its G's fold.
	vector<DecideFrameColumnPrograms> frame_columns;

	// The facts the plan states of the query, which PlanDecide moves in from the LogicalDecide once the tree is
	// finished: the construction reads the LogicalDecide's own until then.

	//! The variables to be decided: the user's, then the auxiliaries (`num_auxiliary_vars`) at the end.
	vector<unique_ptr<Expression>> decide_variables;
	idx_t num_auxiliary_vars = 0;
	//! Per decision, whether it was declared IS BOOLEAN. A BOOLEAN is lowered to an INTEGER with a 0/1 domain, so its
	//! runtime type is INTEGER: this flag is the only signal that the 0/1 box is its intrinsic domain, not a bound the
	//! user wrote.
	vector<bool> is_boolean_var;
	//! The constant canonicalization peeled off the objective (LogicalDecide::objective_constant_offset), so the
	//! objective value a solve reports is the minimized objective's (the negation of a MAXIMIZE query's).
	double objective_constant_offset = 0.0;
	//! The variant signatures, the plan's verbatim: `Variants` (DecideModelEmitter) reads the field kinds in the order
	//! written, and a node's `signature` would change the order its variants are numbered in. The objective's,
	//! INVALID_INDEX for none.
	vector<DecideSignature> signatures;
	idx_t objective_signature = DConstants::INVALID_INDEX;
	//! The frame selectors (LogicalDecide::frames), one home for every reader: a frame node names its own by `frame`.
	vector<DecideFrameSpec> frames;
	//! The helper definitions (LogicalDecide::helper_definitions), one home for every reader: a node names its own by
	//! `definition`.
	vector<unique_ptr<DecideHelperDefinition>> definitions;
	//! The definition a node holds the value of, null for a node that holds none.
	const DecideHelperDefinition *Definition(const DecideNode &node) const {
		return node.definition == DConstants::INVALID_INDEX ? nullptr : definitions[node.definition].get();
	}
	//! Whether `root` is a row the F node of a definition of `kind` owns (DecideNode::rows).
	bool OwnedBy(const DecideNode &root, DecideHelperKind kind) const {
		return root.parent != DConstants::INVALID_INDEX && IsFunction(*nodes[root.parent], kind);
	}
	//! Whether `node` is the F node of a definition of `kind`.
	bool IsFunction(const DecideNode &node, DecideHelperKind kind) const {
		return node.kind == DecideNodeKind::FUNCTION && definitions[node.definition]->kind == kind;
	}
	//! The helper decision a node holds the value of, INVALID_INDEX for a node that holds none.
	idx_t ValueVar(const DecideNode &node) const {
		return node.definition == DConstants::INVALID_INDEX ? DConstants::INVALID_INDEX
		                                                    : definitions[node.definition]->helper;
	}
	//! The `DIAGNOSE` prefix the user wrote. It is the only thing that arms the diagnosis engines: when true a failed
	//! solve stashes its findings for the DIAGNOSE operator above to return as rows instead of raising, and none of the
	//! diagnostic prep (unbounded-ray extraction, model retention, the elastic re-solves) is done or paid for
	//! otherwise.
	bool diagnose = false;

	//! What the domain check reads of a decision: the box of the bounds written on it as constants, whose floor holds
	//! the floors a `when` or `per` lowers (conservatively: the lowest), and how its message names the decision.
	//! `checked` is false for a decision no bound contradicts a type of (a helper, an auxiliary that may read no
	//! value).
	struct DecisionDomain {
		bool checked = false;
		DecideBoundBox box;
		bool is_bool = false;
		string name;
	};
	//! One per decision, filled by CheckDecisionDomains. The run-time check of a per-row bound reads it
	//! (DecideModelEmitter::CheckDomain).
	vector<DecisionDomain> domains;

	//! Reject dangling edges, cycles, invalid keys and non-physical source reads.
	void Validate() const;
	//! Stable plan-order dump of nodes, sigma, routing and computation dependencies.
	string ToString() const;
	//! Each body's terms with the node they range over and where they are computed (`decide_debug` `bodies`).
	string BodiesToString() const;
};

//! Builder with a director (plan_decide.cpp): one walk over the final clauses, the objective and the helper
//! definitions makes each node and, from the same canonical expression, the records its root holds
//! (DecideTermLowering); then the bodies are compiled (language/guide.md §6.4).
class DecideTreeConstruction {
public:
	//! Prepare the plan-time metadata needed to number definition F nodes before MIN/MAX rewrites and term splitting
	//! read them. The full nodes and their instances are built by the constructor once the optimizer has settled
	//! clauses.
	static void PrepareDefinitionFunctions(ClientContext &context, LogicalDecide &op);
	//! Builds the nodes, their edges, sigma, gates and definitions, and each root's records.
	DecideTreeConstruction(ClientContext &context, LogicalDecide &op);
	~DecideTreeConstruction();
	//! The tree so far, which the solver gate reads once the clauses are lowered.
	const DecideTree &Tree() const;
	//! Numbers the clauses and compiles the bodies, orders the nodes, and validates the tree.
	unique_ptr<DecideTree> Finish();

private:
	struct Builder;
	unique_ptr<Builder> builder;
};

//! Numbers the clause roots in the order made, finds where each term is computed and compiles its coefficient, bounds
//! and member tests into programs there. `definition_nodes`: per helper definition, its node (a REDUCER_VALUE's G or
//! O holds its members); `record_nodes`: per record (LogicalDecide::bound_aggregates), its G.
void CompileDecideTree(ClientContext &context, const LogicalDecide &op, DecideTree &tree,
                       const vector<idx_t> &definition_nodes, const vector<idx_t> &record_nodes);

//! A G's operand: its one SCOPE child.
idx_t DecideTreeOperand(const DecideTree &tree, idx_t reducer);

//! The G whose operand entries `reducer`'s terms range over: `reducer` itself, or, while its operand's body is exactly
//! a read of another G (a nest stage 05 reads inlined), that G. Such a G is over decisions with no helper: stage 05
//! lowers a reducer of decisions, the binder refuses a cast over one, and a reducer of a helper's value is a helper
//! itself, whose terms stage 05 lowers with no G.
idx_t DecideTreeMembersOf(const DecideTree &tree, idx_t reducer);

//! The semantic name a carrier represents, shared by tree consumers.
string DecideTreeReducerName(const BoundDecideReducerExpression &aggregate);

} // namespace duckdb
