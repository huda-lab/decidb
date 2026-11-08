//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_tree_evaluation.hpp
//
// Children-first evaluation of the E/G/O containers. No input buffer is accessible.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_error_ledger.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/execution/operator/decide/decide_descent.hpp"

namespace duckdb {

enum class DecideTreeValueKind : uint8_t { DATA, DECISION, SCALAR, REDUCER, ABSENT, ERROR, UNSUPPORTED };

//! A synthesized value of the annotated tree. Scalar and reducer operands retain the actual child values: a nested
//! MAX remains a MAX of its members, rather than merely the address of a helper allocated by a previous rewrite.
struct DecideTreeValue {
	explicit DecideTreeValue(DecideTreeValueKind kind, LogicalType type = LogicalType::SQLNULL)
	    : kind(kind), type(std::move(type)) {
	}

	DecideTreeValueKind kind;
	LogicalType type;
	Value data;
	idx_t node = DConstants::INVALID_INDEX;
	idx_t variable = DConstants::INVALID_INDEX;
	idx_t instance = DConstants::INVALID_INDEX;
	idx_t row = DConstants::INVALID_INDEX;
	string operation;
	//! A scalar template whose immediate children are references to operands, numbered from zero.
	unique_ptr<Expression> expression;
	vector<shared_ptr<DecideTreeValue>> operands;
	ErrorData error;
	DecideErrorRank error_rank;
	bool error_ranked = false;

	bool Present() const {
		return kind != DecideTreeValueKind::ABSENT;
	}
	bool Symbolic() const {
		return kind == DecideTreeValueKind::DECISION || kind == DecideTreeValueKind::SCALAR ||
		       kind == DecideTreeValueKind::REDUCER;
	}
	string ToString() const;
};

using DecideTreeValuePtr = shared_ptr<DecideTreeValue>;

struct DecideTreeNodeResult {
	//! Aligned to this node's descent entries, including entries whose value or deferred error is not consumed.
	vector<DecideTreeValuePtr> entries;
	//! G: one value per group. E/O retain their per-entry values.
	vector<DecideTreeValuePtr> groups;
	//! Root E: its right side, aligned to entries; empty for an objective or operand scope.
	vector<DecideTreeValuePtr> rhs;
	//! E's final instances after absence and consumed fallbacks settle. INVALID marks an absent entry; carried error
	//! witnesses can share one instance. Only instance_entries' representatives contribute to a reducer/model.
	vector<idx_t> instances;
	vector<idx_t> instance_entries;
	//! O's extra signature fields at each anchor, present only when that anchor reaches its fallback.
	vector<vector<Value>> refinements;
};

//! Visitor over the explicit dependency post-order. Containers own every source value the visitor may read.
class DecideTreeEvaluation {
public:
	DecideTreeEvaluation(const DecideTree &tree, const DecideDescent &descent, ClientContext &context);
	void Evaluate();
	const vector<DecideTreeNodeResult> &Results() const {
		return results;
	}
	//! Refuse an unsupported construct or a deferred error only when a surviving root instance reads it.
	void RequireSupported() const;
	string Dump() const;

private:
	const DecideTree &tree;
	const DecideDescent &descent;
	ClientContext &context;
	vector<DecideTreeNodeResult> results;
	vector<unordered_map<idx_t, vector<idx_t>>> rows;
	vector<unordered_map<const Expression *, DecideErrorRank>> expression_ranks;
	bool evaluated = false;

	DecideTreeValuePtr ExpressionValue(const Expression &expression, idx_t node, const DecideDescentEntry &entry);
	DecideTreeValuePtr ChildValue(idx_t child, const DecideDescentEntry &entry) const;
	idx_t ChildEntry(idx_t child, const DecideDescentEntry &entry) const;
	void SignatureReads(const Expression &expression, idx_t node, const DecideDescentEntry &entry,
	                    vector<Value> &signature) const;
	DecideTreeValuePtr DecisionValue(idx_t variable, const LogicalType &type, const DecideDescentEntry &entry) const;
	DecideTreeValuePtr Reduce(idx_t node, const BoundAggregateExpression *function,
	                          const vector<vector<DecideTreeValuePtr>> &arguments,
	                          const vector<DecideTreeValuePtr> &members);
	void Scope(idx_t node);
	void Reducer(idx_t node);
	void Frame(idx_t node);
};

} // namespace duckdb
