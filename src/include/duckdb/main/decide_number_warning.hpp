//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/main/decide_number_warning.hpp
//
// The query-scoped latch behind DECIDE's number-range warning. A query holds any number of DECIDE nodes, each on the
// solver path or a direct plan, and the warning is one line for all of them.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/client_context_state.hpp"

#include <atomic>

namespace duckdb {

//! Key under which DecideNumberWarningState is registered on the ClientContext.
static constexpr const char *DECIDE_NUMBER_WARNING_STATE_KEY = "decide_number_warning_state";

//! Whether the current query has said the number-range warning (DECIDE_NUMBER_RANGE_WARNING). The solver path says it
//! from the finished model, a direct plan from its own arithmetic (`__internal_decide_warn_numbers`), and both go
//! through Say, so a query prints the line once however many DECIDE nodes it holds. A direct plan's rows run on
//! several threads, so the latch is atomic.
class DecideNumberWarningState : public ClientContextState {
public:
	//! The state of `context`'s current query.
	static DecideNumberWarningState &Of(ClientContext &context);

	//! Prints the warning to stderr unless this query already did.
	void Say();

	void QueryBegin(ClientContext &context) override;

private:
	std::atomic<bool> said {false};
};

} // namespace duckdb
