//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_tree_columns.hpp
//
// The bottom-up half's values (language/guide.md §6.1): each node's programs computed over its entries, one executor
// per window of entries, and read as stage 08 reads a coefficient or a bound, so a value and a message are flat's.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_error_ledger.hpp"
#include "duckdb/execution/operator/decide/decide_descent.hpp"

#include <functional>

namespace duckdb {

class ClientContext;
struct DecideSourceNames;

//! A node's programs on its entries (DecideNode::programs), computed once its descent is finished: each program's
//! values on every entry, and the entries its evaluation raised on, with the error, which is held in the ledger at its
//! rank when the program is read (DecideEvaluateWindow).
class DecideTreeColumns {
public:
	DecideTreeColumns(const DecideTree &tree, const DecideDescent &descent, ClientContext &context,
	                  const DecideSourceNames &names, std::function<string(idx_t source_clause_id)> written_clause,
	                  DecideErrorLedger &errors);
	~DecideTreeColumns();

	//! `program` on each of `node`'s entries, as stage 08 reads a coefficient or a bound (ExtractDoubleColumn): signed,
	//! and 0 where it is NULL, which `nulls` marks. A coefficient computed from `inputs` (DecideTerm::inputs, computed
	//! at `site`) names the first of them that is not finite where it fails.
	vector<double> Numbers(idx_t node, idx_t program, vector<bool> &nulls, const DecideTermSite *site = nullptr,
	                       const vector<unique_ptr<Expression>> *inputs = nullptr);
	//! The entries of `node` that read no instance of decision `variable`: a keyed decision's at a row in no class of
	//! its key (D84).
	vector<bool> Absent(idx_t node, idx_t variable) const;

	//! Interim, apart from the model emitter's shapes: the first node these columns compute no value for yet, a read
	//! of a data reducer or a frame (DecideBody::uncompiled) or a hidden root (a value check, a definition no
	//! clause reads), empty when there is none. A query with one has no arm. It goes once they compute every node.
	static string Uncomputed(const DecideTree &tree);

private:
	struct ProgramColumn;
	struct NodeColumns;

	const DecideTree &tree;
	const DecideDescent &descent;
	ClientContext &context;
	const DecideSourceNames &names;
	std::function<string(idx_t)> written_clause;
	DecideErrorLedger &errors;
	vector<unique_ptr<NodeColumns>> nodes;

	NodeColumns &Computed(idx_t node);
	void ComputeNode(idx_t node, NodeColumns &columns);
	//! Holds each error of `program`, at its rank.
	void Report(idx_t node, idx_t program);
	DecideErrorRank Rank(idx_t node, idx_t program, idx_t entry, bool value) const;
};

} // namespace duckdb
