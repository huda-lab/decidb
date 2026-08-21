#include "duckdb/decidb/solver_registry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/gurobi/gurobi_solver.hpp"
#include "duckdb/decidb/naive/deterministic_naive.hpp"
#include "duckdb/decidb/solver_session.hpp"

namespace duckdb {

namespace {

//! The one table. Order is preference order: Gurobi first because it is
//! empirically much faster on DeciDB workloads and strictly more capable; HiGHS
//! last because it is always available and is therefore the fallback that makes
//! selection total.
//!
//! Adding a backend is adding a row. Nothing else in the tree branches on which
//! backend is in play — capability questions go through SolverCapabilities, and
//! session behavior through SolverSession's virtuals.
const SolverBackendInfo REGISTERED_BACKENDS[] = {
    {"gurobi", "Gurobi", GurobiSolver::IsAvailable, GurobiSolver::Capabilities,
     GurobiSolver::CreateSession},
    {"highs", "HiGHS", DeterministicNaive::IsAvailable, DeterministicNaive::Capabilities,
     DeterministicNaive::CreateSession},
};

} // namespace

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

const SolverCapabilities &SolverBackend::Capabilities() const {
	D_ASSERT(info);
	return info->capabilities();
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
		if (SupportsModelClass(needed, backend.Capabilities())) {
			names.emplace_back(backend.DisplayName());
		}
	}
	return names;
}

} // namespace duckdb
