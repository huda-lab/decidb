//===----------------------------------------------------------------------===//
//                         DeciDB
//
// The reporting order of reached DECIDE errors, independent of traversal order.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector_size.hpp"

#include <algorithm>
#include <tuple>

namespace duckdb {

enum class DecideErrorPhase : uint8_t {
	SOURCE_CHECK,
	FRAMES,
	KEYED_MAPPING,
	SCOPE_CHECK,
	FRAME_PEERS,
	CONSTRAINTS,
	OBJECTIVE,
	COMPOSED,
	VALUE_LINKS,
	INVALID = 255
};

struct DecideErrorRank {
	DecideErrorPhase phase = DecideErrorPhase::INVALID;
	//! A part of a phase that runs whole before the next, whatever the clauses: SOURCE_CHECK runs the data checks (0)
	//! before it reads a bound's reducer (1), FRAMES checks every frame's timelines (0) before it reads any frame's
	//! values (1), KEYED_MAPPING reads an auxiliary's definitions (its index) before the next auxiliary's, and
	//! COMPOSED reads the clauses (0) before the objective (1).
	idx_t step = 0;
	idx_t clause = DConstants::INVALID_INDEX;
	idx_t expression = DConstants::INVALID_INDEX;
	//! Inside an expression's evaluation, the part that raised, in the order flat evaluates its parts: a tree program's
	//! group and its place in its block (DecideProgram).
	idx_t group = 0;
	idx_t position = 0;
	idx_t row = DConstants::INVALID_INDEX;

	bool operator<(const DecideErrorRank &other) const {
		return std::tie(phase, step, clause, expression, group, position, row) <
		       std::tie(other.phase, other.step, other.clause, other.expression, other.group, other.position,
		                other.row);
	}
};

//! Where flat reads an expression inside its evaluation of a clause or the objective (PhysicalDecide::Evaluate): first
//! the reads that route its rows (ROUTE, its `when`), then its blocks, each over every window of the input's rows
//! before the next: the term filters, the terms, the grouping of the kept rows by the `by` key, the bound and admitted
//! ends, the products, the squares.
enum class DecideBlock : uint8_t { ROUTE, FILTERS, TERMS, BY_KEY, BOUND, PRODUCTS, SQUARES };

//! Flat's order inside its evaluation of a clause or the objective (DecideErrorRank::expression), after the reads that
//! route its rows: a block runs over every window of the input's rows before the next, and a window executes every
//! expression of its block before it checks any `value`. A window's number stays below 2^54 for any row, under the
//! block's bits.
inline idx_t DecideBlockExpression(DecideBlock block, idx_t row, bool value) {
	return (idx_t(block) << 56) | ((row / STANDARD_VECTOR_SIZE) << 1) | idx_t(value);
}

//! Errors are added only when a read reaches their value. A computed frame value's latent error can therefore stay
//! in its node without entering this ledger when the only anchor reading it has no surviving instance.
class DecideErrorLedger {
public:
	void Add(DecideErrorRank rank, const ErrorData &error) {
		if (!error.HasError()) {
			return;
		}
		const auto type = error.Type();
		if (type == ExceptionType::INTERRUPT || type == ExceptionType::OUT_OF_MEMORY || type == ExceptionType::FATAL) {
			error.Throw();
		}
		errors.push_back({rank, error});
	}
	//! Another ledger's errors, each at its own rank: one evaluation's walks report as one.
	void Append(const DecideErrorLedger &other) {
		errors.insert(errors.end(), other.errors.begin(), other.errors.end());
	}
	//! Whether an error ranks in a phase before `phase`: one flat raises before that phase runs.
	bool HasErrorBefore(DecideErrorPhase phase) const {
		return std::any_of(errors.begin(), errors.end(), [&](const Entry &entry) { return entry.rank.phase < phase; });
	}
	void ThrowIfAny() const {
		if (errors.empty()) {
			return;
		}
		idx_t first = 0;
		for (idx_t i = 1; i < errors.size(); i++) {
			if (errors[i].rank < errors[first].rank) {
				first = i;
			}
		}
		errors[first].error.Throw();
	}

private:
	struct Entry {
		DecideErrorRank rank;
		ErrorData error;
	};
	vector<Entry> errors;
};

} // namespace duckdb
