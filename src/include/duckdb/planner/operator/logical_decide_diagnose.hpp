//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/operator/logical_decide_diagnose.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//! The `DIAGNOSE <select>` prefix, as a plan node. Its single child is the whole
//! decision query, whose LogicalDecide has been told to diagnose rather than throw.
//! This operator runs that child, discards its rows, and returns the diagnosis the
//! DECIDE operator produced as a flat relation:
//!
//!     state | clause | suggested_change | amount | total | scope | edit_source | group | row
//!
//! It is the ONLY reader of that relation, and DIAGNOSE is the only thing that puts
//! one there — an unprefixed query never builds a diagnosis at all.
class LogicalDecideDiagnose : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_DECIDE_DIAGNOSE;

public:
	explicit LogicalDecideDiagnose(idx_t table_index);

	//! Table index the diagnosis columns are bound under.
	idx_t table_index;

public:
	vector<ColumnBinding> GetColumnBindings() override;
	void ResolveTypes() override;
	string GetName() const override;

protected:
	vector<idx_t> GetTableIndex() const override;
};

} // namespace duckdb
