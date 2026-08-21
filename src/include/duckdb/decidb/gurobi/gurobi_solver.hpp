//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/gurobi/gurobi_solver.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/decidb/solver_capabilities.hpp"
#include "duckdb/decidb/solver_result.hpp"

namespace duckdb {

class SolverSession;

class GurobiSolver {
public:
    //! Check if Gurobi is available at runtime (library linked + valid license)
    static bool IsAvailable();

    //! What upstream stages may assume about Gurobi on THIS host. Partly a runtime
    //! fact: a construct flag is true only when the dynamically loaded library
    //! actually exported the symbol that construct needs, so this is answered after
    //! the loader has run, not baked in at compile time.
    static const SolverCapabilities &Capabilities();

    //! Create a resumable Gurobi session: the one way a DECIDE query reaches
    //! Gurobi. Every solve runs as one or more chunks on a session, so a
    //! time-limited solve can be resumed rather than restarted.
    static unique_ptr<SolverSession> CreateSession();
};

} // namespace duckdb
