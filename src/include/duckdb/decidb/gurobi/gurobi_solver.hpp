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

    //! What Gurobi declares on THIS host. Partly a runtime fact: a construct flag is
    //! true only when the dynamically loaded library actually exported the symbol that
    //! construct needs, so this loads the library first rather than baking an answer in
    //! at compile time. Upstream stages read it through SolverBackend::Capabilities(),
    //! which applies the DECIDB_NATIVE_CONSTRUCTS mask.
    static const SolverCapabilities &Capabilities();

    //! Create a resumable Gurobi session: the one way a DECIDE query reaches
    //! Gurobi. Every solve runs as one or more chunks on a session, so a
    //! time-limited solve can be resumed rather than restarted.
    static unique_ptr<SolverSession> CreateSession();
};

} // namespace duckdb
