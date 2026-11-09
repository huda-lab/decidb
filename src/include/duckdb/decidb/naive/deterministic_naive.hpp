#pragma once

#include "duckdb/common/decide_solver_capabilities.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"
#include "duckdb/decidb/solver/solver_result.hpp"

namespace duckdb {

class SolverSession;

class DeterministicNaive {
public:
	//! HiGHS is vendored and statically linked, so it is available on every host.
	//! Declared anyway so the registry asks every backend the same question.
	static bool IsAvailable();

	//! The model classes HiGHS loads. It is the floor of the registry: plain linear and
	//! convex quadratic objectives whose Q is diagonal, so every class is false.
	static SolverModelClass ModelClasses();

	//! The constructs HiGHS states natively: none. Upstream stages read this through
	//! SolverBackend::Constructs(), which applies the DECIDB_NATIVE_CONSTRUCTS mask.
	static const SolverConstructSupport &Constructs();

	//! Create a resumable HiGHS session: the one way a DECIDE query reaches
	//! HiGHS. Every solve runs as one or more chunks on a session, so a
	//! time-limited solve can be resumed rather than restarted.
	static unique_ptr<SolverSession> CreateSession();
};

} // namespace duckdb
