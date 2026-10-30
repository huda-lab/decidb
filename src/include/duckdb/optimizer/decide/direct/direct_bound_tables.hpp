#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class DirectSourceReader;
class LogicalDecide;
class Optimizer;

//! Gives the reader the reducers the bounds of `decide` read (LogicalDecide::bound_aggregates), moved out of the node,
//! as side tables (DirectSourceReader::AddSideTable), in the order they were recorded. A bound is the keyed, correlated
//! case of the row checks' one-row verdict: a scalar subquery over the rows of its group, which DuckDB decorrelates
//! into a grouped aggregate joined back to the rows by the key (FlattenDependentJoins). Each table is the aggregate of
//! one ungated read of the source, one row per group of the reducer's `by` key:
//!  - the reducer's function over the rows it reads (its FILTER), which are the clause's kept rows that pass its own
//!    `when`. A row outside them reads its group's value, and nothing reads it. A group with no row it reads gives the
//!    function's value over no rows, as SQL's does (NULL for SUM, 0 for COUNT);
//!  - `sum(per K: body)` counts each class of K in the group once: a first aggregate per group and class takes the
//!    class's body from the rows it reads (ARBITRARY: the key determines the body, which a scope check proves), and
//!    the reducer's function runs over the classes of each group. A row whose written `per` column is NULL is in no
//!    class.
//! The tables are added before the row checks, so the checks read the rows with their bounds, as the solver reads them.
void DirectAddBoundTables(Optimizer &optimizer, DirectSourceReader &reader, LogicalDecide &decide);

} // namespace duckdb
