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

//! Where one term of a body is computed (language/guide.md §6.1): the E or O whose entries it ranges over.
struct DecideTermSite {
	idx_t node = DConstants::INVALID_INDEX;
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
	vector<DecideTerm> fallback;
	vector<DecideTermSite> linear, products, fallback_sites;
	//! Per quadratic group (the objective has one, its squared terms), its inner terms' sites.
	vector<vector<DecideTermSite>> squares;
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
	//! A hidden root's non-NULL string is the data check's error message.
	bool value_check = false;
	//! Immutable clause metadata shared with flat lowering. Its term vectors are
	//! empty: the graph's bodies and child results supply the model's terms.
	unique_ptr<DecideConstraint> facts;
	//! Stage 05's terms for what the node holds (CompileDecideTree).
	DecideBody compiled;
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
	vector<LogicalDecide::BilinearLink> bilinear_links;
	vector<LogicalDecide::AbsMaximizeLink> abs_maximize_links;
	vector<DecideBoundBox> singleton_row_boxes;
	vector<LogicalDecide::UserBoundSpec> user_singleton_row_bounds;
	vector<idx_t> signed_helper_vars;

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

//! Binds the clause roots to stage 05's clauses in leaf order, copies stage 05's terms into each node's body and finds
//! where each is computed; a binding that does not match is an internal error.
void CompileDecideTree(const LogicalDecide &op, DecideTree &tree, const DecideTreeSources &sources);

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
