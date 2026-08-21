#pragma once

#include "duckdb/decidb/solver_input.hpp"
#include "duckdb/decidb/solver_result.hpp"

namespace duckdb {

class SolverSession;

class DeterministicNaive {
public:
    //! Create a resumable HiGHS session: the one way a DECIDE query reaches
    //! HiGHS. Every solve runs as one or more chunks on a session, so a
    //! time-limited solve can be resumed rather than restarted.
    static unique_ptr<SolverSession> CreateSession();
};

} // namespace duckdb
