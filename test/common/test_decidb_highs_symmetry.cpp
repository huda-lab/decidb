// HiGHS's symmetry detection stops at the time limit and at a user interrupt (third_party patch BUG-3). Each case
// gives the detector a stop that has already happened, so the stop is deterministic: no case depends on timing.

#include "catch.hpp"

#include "lp_data/HighsCallback.h"
#include "lp_data/HighsLp.h"
#include "parallel/HighsParallel.h"
#include "presolve/HighsSymmetry.h"
#include "util/HighsTimer.h"

#include <thread>

namespace {

void AddColumn(HighsLp &lp, double cost, double upper, HighsVarType type, std::initializer_list<HighsInt> rows,
               std::initializer_list<double> values) {
	lp.col_cost_.push_back(cost);
	lp.col_lower_.push_back(0.0);
	lp.col_upper_.push_back(upper);
	lp.integrality_.push_back(type);
	lp.a_matrix_.index_.insert(lp.a_matrix_.index_.end(), rows);
	lp.a_matrix_.value_.insert(lp.a_matrix_.value_.end(), values);
	lp.a_matrix_.start_.push_back(lp.a_matrix_.index_.size());
	lp.num_col_++;
}

void AddRows(HighsLp &lp, HighsInt count, double lower, double upper) {
	lp.row_lower_.insert(lp.row_lower_.end(), count, lower);
	lp.row_upper_.insert(lp.row_upper_.end(), count, upper);
	lp.num_row_ += count;
}

HighsLp EmptyLp() {
	HighsLp lp;
	lp.a_matrix_.format_ = MatrixFormat::kColwise;
	lp.a_matrix_.start_ = {0};
	return lp;
}

//! A path of `n` binaries, x_i + x_(i+1) <= 1. Its two ends differ from the rest, so the first refinement walks in
//! from both ends, one cell at a time: far more than the 100 cells between two stop checks.
HighsLp PathLp(HighsInt n) {
	HighsLp lp = EmptyLp();
	AddRows(lp, n - 1, -kHighsInf, 1.0);
	for (HighsInt i = 0; i < n; ++i) {
		if (i == 0) {
			AddColumn(lp, 1.0, 1.0, HighsVarType::kInteger, {0}, {1.0});
		} else if (i == n - 1) {
			AddColumn(lp, 1.0, 1.0, HighsVarType::kInteger, {i - 1}, {1.0});
		} else {
			AddColumn(lp, 1.0, 1.0, HighsVarType::kInteger, {i - 1, i}, {1.0, 1.0});
		}
	}
	return lp;
}

//! `n` interchangeable pairs: a binary x_i and a continuous y_i <= 0.5, with y_i - x_i <= 0 and sum(y) >= n / 4.
//! The first refinement is a few cells; the search then fixes one pair per node, each refinement a few cells.
HighsLp PairsLp(HighsInt n) {
	HighsLp lp = EmptyLp();
	AddRows(lp, n, -kHighsInf, 0.0);
	AddRows(lp, 1, n / 4.0, kHighsInf);
	for (HighsInt i = 0; i < n; ++i) {
		AddColumn(lp, 1.0, 1.0, HighsVarType::kInteger, {i}, {-1.0});
		AddColumn(lp, 0.0, 0.5, HighsVarType::kContinuous, {i, n}, {1.0, 1.0});
	}
	return lp;
}

struct DetectionResult {
	bool detect = false;
	HighsInt generators = -1;
};

//! Runs the detection as HiGHS does, on a thread with its own HiGHS scheduler (run() reads the worker deque).
DetectionResult Detect(const HighsLp &lp, double time_limit, const HighsCallback *callback) {
	DetectionResult result;
	std::thread worker([&]() {
		highs::parallel::initialize_scheduler(1);
		HighsTimer timer;
		HighsSymmetryDetection detection;
		detection.loadModelAsGraph(lp, 1e-9);
		result.detect = detection.initializeDetection(timer, time_limit, callback);
		if (result.detect) {
			HighsSymmetries symmetries;
			detection.run(symmetries);
			result.generators = symmetries.numGenerators;
		}
		HighsTaskExecutor::shutdown(true);
	});
	worker.join();
	return result;
}

} // namespace

TEST_CASE("HiGHS symmetry detection stops at the time limit", "[decidb][highs_symmetry]") {
	SECTION("inside a refinement: initializeDetection detects nothing") {
		// The check inside partitionRefinement stops the first refinement; initializeDetection then returns
		// false rather than hand a partial partition to removeFixPoints.
		REQUIRE_FALSE(Detect(PathLp(2000), 0.0, nullptr).detect);
	}
	SECTION("at a search node: run keeps what it found") {
		// The first refinement is under 100 cells, so it never checks; the check at the top of run()'s loop
		// stops the search before its first leaf.
		DetectionResult stopped = Detect(PairsLp(2000), 0.0, nullptr);
		REQUIRE(stopped.detect);
		REQUIRE(stopped.generators == 0);
		REQUIRE(Detect(PairsLp(2000), kHighsInf, nullptr).generators > 0);
	}
}

TEST_CASE("HiGHS symmetry detection stops at a user interrupt", "[decidb][highs_symmetry]") {
	// The interrupt callback answers through its own output and input; the search stops at its first node.
	HighsCallback callback(nullptr);
	callback.user_callback = [](int, const std::string &, const HighsCallbackOutput *, HighsCallbackInput *data_in,
	                            void *) {
		data_in->user_interrupt = true;
	};
	callback.active[kCallbackMipInterrupt] = true;
	DetectionResult stopped = Detect(PairsLp(2000), kHighsInf, &callback);
	REQUIRE(stopped.detect);
	REQUIRE(stopped.generators == 0);
}
