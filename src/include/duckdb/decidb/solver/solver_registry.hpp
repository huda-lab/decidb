//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/solver/solver_registry.hpp
//
// The set of solver backends DeciDB can run, and the handle the rest of the
// pipeline passes around to name one of them.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/decidb/solver/solver_capabilities.hpp"

namespace duckdb {

class SolverSession;

//! Everything DeciDB knows about one backend, as a table row. This is the ONLY
//! place a backend is named: registering a new one appends an entry here and
//! changes no `if` and no `switch` anywhere else in the tree.
struct SolverBackendInfo {
	//! Stable lowercase identifier, and the DECIDB_FORCE_SOLVER spelling.
	const char *name;
	//! How the backend is spelled in text a user reads ("Gurobi"). Kept in the table
	//! so a refusal can name the solver that would run the query without any code
	//! outside this file hard-coding a backend.
	const char *display_name;
	//! Runtime probe: is the library loadable and the license valid on THIS host?
	//! A backend that is always present answers true unconditionally.
	bool (*is_available)();
	//! What this backend declares, before the central DECIDB_NATIVE_CONSTRUCTS mask
	//! is applied (see SolverBackend::Capabilities). Queried through a function rather
	//! than stored inline because capability is partly a runtime fact — a dynamically
	//! loaded library may not export the symbol a native construct needs, so the
	//! answer is not known until the library is open.
	const SolverCapabilities &(*capabilities)();
	//! Fresh session factory. The returned session owns no solver state until Solve().
	unique_ptr<SolverSession> (*create_session)();
};

//! Names one registered backend. A value type wrapping a pointer into the
//! registry's static table, so it copies freely, compares by identity, and can be
//! carried on a plan node from stage 05 down to stage 08 — which is the point:
//! the backend is chosen ONCE, before any rewrite, and every later stage reads
//! that choice rather than asking again.
//!
//! A default-constructed handle is invalid ("no backend chosen yet"). Every
//! accessor requires a valid handle.
class SolverBackend {
public:
	SolverBackend() = default;
	explicit SolverBackend(const SolverBackendInfo &info_p) : info(&info_p) {
	}

	bool IsValid() const {
		return info != nullptr;
	}

	//! The registered identifier ("gurobi", "highs"). Test overrides and internal
	//! messages use it; nothing branches on it.
	const char *Name() const;
	//! The user-facing spelling ("Gurobi", "HiGHS").
	const char *DisplayName() const;
	//! Is this backend usable on this host right now?
	bool IsAvailable() const;
	//! What upstream stages may assume: the backend's own declaration with the
	//! test-only DECIDB_NATIVE_CONSTRUCTS switch applied. Returned BY VALUE because
	//! that switch is a property of the contract rather than of any one backend, so
	//! masking happens here, once, for every registered backend — not copy-pasted into
	//! each one. Read `.constructs` or `.model_classes`; nothing needs both.
	SolverCapabilities Capabilities() const;
	//! Open a fresh session on this backend.
	unique_ptr<SolverSession> CreateSession() const;

	bool operator==(const SolverBackend &other) const {
		return info == other.info;
	}
	bool operator!=(const SolverBackend &other) const {
		return info != other.info;
	}

private:
	const SolverBackendInfo *info = nullptr;
};

//! The registry itself: a static, ordered table of every backend linked into this
//! build. Order IS preference order — SelectSolverBackend takes the first available
//! entry — so reordering the table is how the default preference changes.
struct SolverRegistry {
	//! Every registered backend, in preference order. Availability is NOT filtered:
	//! the caller decides whether an unavailable backend is an error or a skip.
	static const vector<SolverBackend> &Backends();
	//! Look one up by name, case-insensitively. Returns an invalid handle when no
	//! backend answers to that name.
	static SolverBackend Find(const string &name);
	//! The display names of every registered backend that could take a model of this
	//! class, in preference order — regardless of whether it is installed here. This
	//! is what a plan-time refusal names as the thing to install, so the sentence
	//! stays correct as backends are added or their capabilities grow.
	static vector<string> BackendsSupporting(const SolverModelClass &needed);
};

//! Test-only: `DECIDB_NATIVE_CONSTRUCTS=force` asks for a construct to be stated
//! natively even where a valid Big-M exists — which is NOT the shipping policy, since
//! the lowering is the smaller model wherever it is valid (see NativeConstructPolicy).
//!
//! It exists so the A/B equivalence tests keep testing something. Without it, every
//! bounded query takes the lowering on both settings of the switch, and a test that
//! compares the two arms would compare the lowering against itself and pass while
//! proving nothing. `off` and `force` are the two arms; the default is neither.
bool NativeConstructsForced();

} // namespace duckdb
