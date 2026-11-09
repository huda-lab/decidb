//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/gurobi/gurobi_solver.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/decide_solver_capabilities.hpp"
#include "duckdb/decidb/solver/solver_result.hpp"

namespace duckdb {

class SolverSession;

class GurobiSolver {
public:
    //! Check if Gurobi is available at runtime (library linked + valid license)
    static bool IsAvailable();

    //! The model classes Gurobi loads: every class DeciDB builds. A fact about Gurobi,
    //! answered without opening the library.
    static SolverModelClass ModelClasses();

    //! The constructs Gurobi states natively on THIS host. A runtime fact: a flag is
    //! true only when the dynamically loaded library actually exported the symbol that
    //! construct needs, so this loads the library first rather than baking an answer in
    //! at compile time. Upstream stages read it through SolverBackend::Constructs(),
    //! which applies the DECIDB_NATIVE_CONSTRUCTS mask.
    static const SolverConstructSupport &Constructs();

    //! Create a resumable Gurobi session: the one way a DECIDE query reaches
    //! Gurobi. Every solve runs as one or more chunks on a session, so a
    //! time-limited solve can be resumed rather than restarted.
    static unique_ptr<SolverSession> CreateSession();
};

} // namespace duckdb
