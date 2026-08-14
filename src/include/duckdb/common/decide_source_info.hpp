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
};

} // namespace duckdb
