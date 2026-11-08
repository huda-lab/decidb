//===----------------------------------------------------------------------===//
//                         DeciDB
//
// The reporting order of reached DECIDE errors, independent of traversal order.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/vector.hpp"

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
	idx_t clause = DConstants::INVALID_INDEX;
	idx_t expression = DConstants::INVALID_INDEX;
	idx_t row = DConstants::INVALID_INDEX;

	bool operator<(const DecideErrorRank &other) const {
		return std::tie(phase, clause, expression, row) <
		       std::tie(other.phase, other.clause, other.expression, other.row);
	}
};

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
