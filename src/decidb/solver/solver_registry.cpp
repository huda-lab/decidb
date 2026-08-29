#include "duckdb/decidb/solver/solver_registry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/gurobi/gurobi_solver.hpp"
#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/decidb/solver/solver_session.hpp"

#include <cstdlib>

namespace duckdb {

namespace {

//! The one table. Order is preference order: Gurobi first because it is
//! empirically much faster on DeciDB workloads and strictly more capable; HiGHS
//! last because it is always available and is therefore the fallback that makes
//! selection total.
//!
//! Adding a backend is adding a row. Nothing else in the tree branches on which
//! backend is in play — capability questions go through the capability types, and
//! session behavior through SolverSession's virtuals.
const SolverBackendInfo REGISTERED_BACKENDS[] = {
    {"gurobi", "Gurobi", GurobiSolver::IsAvailable, GurobiSolver::Capabilities,
     GurobiSolver::CreateSession},
    {"highs", "HiGHS", DeterministicNaive::IsAvailable, DeterministicNaive::Capabilities,
     DeterministicNaive::CreateSession},
};

//! Test-only A/B switch, mirroring DECIDB_FORCE_SOLVER. It is three-valued because the
//! shipping policy sits between its two extremes:
//!
//!   off     every construct capability is turned off, so the query takes the lowering
//!           path and must reach the same optimum.
//!   on      the default, and what an unset variable means. A construct is stated
//!           natively only where the lowering has no valid Big-M to use — the lowering
//!           is the smaller model wherever it works. Spelled out so a test can pin the
//!           shipping policy instead of relying on the variable being absent, which an
//!           ambient setting would silently override.
//!   force   stated natively wherever the backend declares it, which is what the
//!           default used to do.
//!
//! `off` and `force` are the two arms of the A/B equivalence test, and that equivalence
//! is the standard a construct flag has to meet before it goes in the table at all —
//! and the only way to test it without a second machine. The default is deliberately
//! NEITHER arm: it picks per site, so neither setting alone would exercise both.
//!
//! It lives HERE, above every backend, rather than inside the one backend that happens
//! to declare a construct today: the switch is part of the capability contract, so a
//! second capable backend inherits it instead of copy-pasting it.
//!
//! Read once. The environment of a running process does not change under DeciDB, and
//! caching keeps this off the per-query path.
bool NativeConstructsEnabled() {
	static const bool enabled = []() {
		const char *setting = std::getenv("DECIDB_NATIVE_CONSTRUCTS");
		if (!setting) {
			return true;
		}
		string value(setting);
		return !(value == "off" || value == "OFF" || value == "0");
	}();
	return enabled;
}

} // namespace

bool NativeConstructsForced() {
	static const bool forced = []() {
		const char *setting = std::getenv("DECIDB_NATIVE_CONSTRUCTS");
		if (!setting) {
			return false;
		}
		string value(setting);
		return value == "force" || value == "FORCE";
	}();
	return forced;
}

const char *SolverBackend::Name() const {
	D_ASSERT(info);
	return info->name;
}

const char *SolverBackend::DisplayName() const {
	D_ASSERT(info);
	return info->display_name;
}

bool SolverBackend::IsAvailable() const {
	D_ASSERT(info);
	return info->is_available();
}

SolverCapabilities SolverBackend::Capabilities() const {
	D_ASSERT(info);
	SolverCapabilities capabilities = info->capabilities();
	// The one place the A/B switch is applied. Model classes are untouched on purpose:
	// a model class has no lowering path to fall back to, so masking one would not slow
	// a query down, it would refuse it.
	if (!NativeConstructsEnabled()) {
		capabilities.constructs = SolverConstructSupport();
	}
	return capabilities;
}

unique_ptr<SolverSession> SolverBackend::CreateSession() const {
	if (!info) {
		throw InternalException("DECIDE solve reached the solver with no backend chosen");
	}
	return info->create_session();
}

const vector<SolverBackend> &SolverRegistry::Backends() {
	static const vector<SolverBackend> backends = []() {
		vector<SolverBackend> result;
		for (auto &entry : REGISTERED_BACKENDS) {
			result.emplace_back(entry);
		}
		return result;
	}();
	return backends;
}

SolverBackend SolverRegistry::Find(const string &name) {
	for (auto &backend : Backends()) {
		if (StringUtil::CIEquals(name, backend.Name())) {
			return backend;
		}
	}
	return SolverBackend();
}

vector<string> SolverRegistry::BackendsSupporting(const SolverModelClass &needed) {
	vector<string> names;
	for (auto &backend : Backends()) {
		if (SupportsModelClass(needed, backend.Capabilities().model_classes)) {
			names.emplace_back(backend.DisplayName());
		}
	}
	return names;
}

} // namespace duckdb
