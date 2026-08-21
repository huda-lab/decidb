#include "catch.hpp"

#include "duckdb/decidb/ilp_solver.hpp"
#include "duckdb/decidb/solver_registry.hpp"

#include <cstdlib>
#include <string>

using namespace duckdb;

// The registry is the single table naming every backend DeciDB can run. These
// assertions are about the TABLE, not about any solve: that every entry answers
// every question, that the invariant making selection total holds (the last entry
// is always available), and that capability declarations are present rather than
// left to a default nobody wrote down.
TEST_CASE("DeciDB solver registry", "[decidb][solver][registry]") {
	auto &backends = SolverRegistry::Backends();

	SECTION("every registered backend answers every question") {
		REQUIRE(!backends.empty());
		for (auto &backend : backends) {
			REQUIRE(backend.IsValid());
			CHECK(std::string(backend.Name()).size() > 0);
			// Both probes must be callable on any host. IsAvailable() may answer
			// false — that is a fact about the machine, not a gap in the table.
			(void)backend.IsAvailable();
			(void)backend.Capabilities();
		}
	}

	SECTION("selection is total: the last entry is always available") {
		// SelectSolverBackend walks the table in order and takes the first available
		// entry. It can only be total because the fallback backend is unconditional.
		CHECK(backends.back().IsAvailable());
	}

	SECTION("lookup by name is case-insensitive and rejects unknown names") {
		for (auto &backend : backends) {
			std::string name = backend.Name();
			CHECK(SolverRegistry::Find(name) == backend);
			std::string shouted = name;
			for (auto &c : shouted) {
				c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
			}
			CHECK(SolverRegistry::Find(shouted) == backend);
		}
		CHECK(!SolverRegistry::Find("no-such-solver").IsValid());
	}

	SECTION("both backends declare their capability table") {
		SolverBackend gurobi = SolverRegistry::Find("gurobi");
		SolverBackend highs = SolverRegistry::Find("highs");
		REQUIRE(gurobi.IsValid());
		REQUIRE(highs.IsValid());

		// HiGHS is the floor: plain linear and convex quadratic objectives only.
		// Every model-class flag false means a query needing one is refused, and
		// every construct flag false means everything arrives fully lowered.
		auto &highs_caps = highs.Capabilities();
		CHECK(!highs_caps.quadratic_constraints);
		CHECK(!highs_caps.nonconvex_quadratic);
		CHECK(!highs_caps.miqp);
		CHECK(!highs_caps.abs);
		CHECK(!highs_caps.min_max);
		CHECK(!highs_caps.not_equal);
		CHECK(!highs_caps.in_list);
		CHECK(!highs_caps.bilinear);

		// Gurobi takes every model class DeciDB can build. Its construct flags are
		// declared as they are implemented, each with the loader symbol behind it,
		// so this only asserts the model-class gates it must always satisfy.
		auto &gurobi_caps = gurobi.Capabilities();
		CHECK(gurobi_caps.quadratic_constraints);
		CHECK(gurobi_caps.nonconvex_quadratic);
		CHECK(gurobi_caps.miqp);
	}

	SECTION("selection honors DECIDB_FORCE_SOLVER and falls through on a bad name") {
		// The override is test-only and process-global, so restore it afterwards.
		const char *saved = std::getenv("DECIDB_FORCE_SOLVER");
		std::string saved_value = saved ? saved : std::string();
		bool had_value = saved != nullptr;

		setenv("DECIDB_FORCE_SOLVER", "HiGHS", 1);
		CHECK(SelectSolverBackend() == SolverRegistry::Find("highs"));

		// An unrecognized name is not an error: selection falls through to the
		// normal preference walk, which always yields an available backend.
		setenv("DECIDB_FORCE_SOLVER", "no-such-solver", 1);
		CHECK(SelectSolverBackend().IsAvailable());

		if (had_value) {
			setenv("DECIDB_FORCE_SOLVER", saved_value.c_str(), 1);
		} else {
			unsetenv("DECIDB_FORCE_SOLVER");
		}
	}
}
