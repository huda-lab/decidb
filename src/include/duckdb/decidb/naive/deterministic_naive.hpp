#pragma once

#include "duckdb/decidb/solver_capabilities.hpp"
#include "duckdb/decidb/solver_input.hpp"
#include "duckdb/decidb/solver_result.hpp"

namespace duckdb {

class SolverSession;

class DeterministicNaive {
public:
    //! HiGHS is vendored and statically linked, so it is available on every host.
    //! Declared anyway so the registry asks every backend the same question.
    static bool IsAvailable();

    //! What upstream stages may assume about HiGHS. It is the floor of the
    //! registry: plain linear and convex quadratic objectives, nothing native.
    static const SolverCapabilities &Capabilities();

    //! Create a resumable HiGHS session: the one way a DECIDE query reaches
    //! HiGHS. Every solve runs as one or more chunks on a session, so a
    //! time-limited solve can be resumed rather than restarted.
    static unique_ptr<SolverSession> CreateSession();
};

} // namespace duckdb
