//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide/decide_declarations_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

class BoundSelectNode;
class SelectNode;

//! Binds the DECIDE clause of a SELECT statement: the variable declarations
//! themselves (name, type, scope), then the SUCH THAT and objective trees via
//! DecideConstraintsBinder / DecideObjectiveBinder.
//!
//! This runs before star expansion, because `SELECT *` must be able to see the
//! declared decisions as columns, and the auxiliary decisions the optimizer will
//! later add must stay hidden from it. Everything the bound plan needs about the
//! decisions -- their types, their `0/1` domains, their entity scopes and the
//! entity-key columns that must survive column pruning -- is written onto the
//! BoundSelectNode here.
class DecideDeclarationsBinder {
public:
	DecideDeclarationsBinder(Binder &binder, ClientContext &context);

	//! Bind `statement`'s DECIDE clause onto `result`. Assumes the statement has one.
	void BindDeclarations(SelectNode &statement, BoundSelectNode &result);

private:
	Binder &binder;
	ClientContext &context;
};

} // namespace duckdb
