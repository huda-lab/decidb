//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_debug.hpp
//
// SET decide_debug: the evaluator's debug dumps and test hooks, one option read once when the operator is planned.
//===----------------------------------------------------------------------===//
//
// The value is a comma list. `tree`, `bodies`, `containers`, `values` and `steps` print the evaluator's dumps to
// stderr; `chunk=N` sets the descent's chunk size; `inject=N:data:V` stands V in for the data value of node N, which
// shows a parent reads the bottom-up values. A bad item is refused when the option is set (as `decide_direct_solve`
// is).
//
// A Parameter Object: physical planning parses the option once into a DecideDebug that the operator carries, so no
// stage of the evaluation reads a setting or the environment.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClientContext;
class DBConfig;

//! `inject=N:data:V`: node N, INVALID_INDEX when unset, and the value V, cast to the node's type (DecideTreeColumns).
struct DecideTreeInjection {
	idx_t node = DConstants::INVALID_INDEX;
	string data;
};

struct DecideDebug {
	//! DecideTree::ToString, the tree stage 08 evaluates.
	bool tree = false;
	//! DecideTree::BodiesToString, each body's terms with the node they range over.
	bool bodies = false;
	//! DecideDescent::Dump, every node's container once the descent has read every row.
	bool containers = false;
	//! DecideTreeColumns::Dump, what each node computes on its entries.
	bool values = false;
	//! DecideDescent::Step, the containers after each piece the descent reads: at chunk size 1, the deck's animation.
	bool steps = false;
	//! The descent's chunk size, in rows; INVALID_INDEX for the size of the chunk it is handed.
	idx_t chunk = DConstants::INVALID_INDEX;
	DecideTreeInjection inject;
};

//! `setting` as a DecideDebug. Throws InvalidInputException on an item it does not know.
DecideDebug ParseDecideDebug(const string &setting);
//! The option's value for `context`.
DecideDebug GetDecideDebug(ClientContext &context);
void RegisterDecideDebug(DBConfig &config);

} // namespace duckdb
