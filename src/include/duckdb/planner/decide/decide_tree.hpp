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

//! Composite: E owns entries, G groups an operand E, and O holds ordered positions.
//! Expressions are owned copies, with source columns resolved to chunk references.
//! No node borrows the optimizer's expression addresses or its prepared terms.
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
};

//! Rebuild the graph after bindings settle, before prepared-model frame lowering.
//! Records still present in bound_aggregates are expanded into data G nodes; a
//! source placeholder without its record remains a source column and is visible
//! in the dump rather than being misrepresented as a data reducer.
unique_ptr<DecideTree> BuildDecideTree(ClientContext &context, const LogicalDecide &op);

//! The semantic name a carrier represents, shared by tree consumers.
string DecideTreeReducerName(const BoundAggregateExpression &aggregate);

//! Read shape, provenance and bound-admission facts once. Both graph construction
//! and flat term lowering use this function; neither infers a second clause shape.
unique_ptr<DecideConstraint> ReadDecideClauseFacts(ClientContext &context, const LogicalDecide &op,
                                                const BoundComparisonExpression &comparison,
                                                const DecideEnclosingScope &scope);

} // namespace duckdb
