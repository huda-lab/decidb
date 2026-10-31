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
class Expression;
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

	//! Refuse a decision read anywhere in `where`, the bound WHERE clause of `result` (which has
	//! a DECIDE clause) or a part of it. WHERE picks the rows before the solve -- its filter is planned below the DECIDE
	//! operator -- so it may only use data. The bound condition is read, not the parsed one:
	//! only binding knows what a name stands for, so a decision reached through a SELECT alias,
	//! `COLUMNS(*)` or a correlated subquery is refused, and a lambda parameter is not.
	static void RefuseDecisionsInWhere(const Expression &where, const BoundSelectNode &result);

private:
	Binder &binder;
	ClientContext &context;
};

} // namespace duckdb
