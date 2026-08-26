//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/common/decide_source_info.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! How diagnostics should render the right-hand side of a source constraint.
enum class ConstraintSourceRhsKind : uint8_t {
	//! No directly editable symbolic RHS. Diagnostics quote its evaluated value.
	NUMERIC_FALLBACK = 0,
	//! A user-visible data expression. Diagnostics render a symbolic offset.
	DATA_EXPRESSION = 1
};

//! Stable, solver-neutral display provenance for one canonical source comparison.
//! The vector that owns these entries is indexed by source_clause_id.
struct ConstraintSourceInfo {
	//! Canonical user-facing pieces after canonicalization.
	string canonical_lhs;
	string canonical_rhs;
	string qualifier;
	ConstraintSourceRhsKind rhs_kind = ConstraintSourceRhsKind::NUMERIC_FALLBACK;
	//! The clause as WRITTEN, captured before canonicalization moved anything.
	//!
	//! Canonicalization is free to move a decision-bearing term across the
	//! comparison, and usually there is nothing to see: `SUM(x) >= 100` is already
	//! canonical, so re-rendering the canonical tree reproduces what the user typed.
	//! But a bound that CONTAINS a decision genuinely has to move --
	//! `ship <= capacity * open` becomes `ship - capacity * open <= 0` -- and
	//! re-rendering then shows the user algebra they never wrote, against a clause
	//! they cannot find in their own query.
	//!
	//! These two fields keep the written spelling so a diagnosis can quote it. The
	//! offset a repair computes is valid against EITHER form: moving a term across
	//! the comparison does not change what adding a constant to the bound means, so
	//! `<canonical_rhs> + d` and `<source_rhs> + d` relax by the same amount.
	//! Empty when the source and canonical forms agree, which is the common case.
	string source_lhs;
	string source_rhs;
};

} // namespace duckdb
