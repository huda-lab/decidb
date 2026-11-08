// HiGHS presolve asks the user interrupt beside its clock (third_party patch BUG-53). Each case answers every ask with
// "stop" and records which checks asked, so no case depends on timing: presolve's ask is the one whose message names
// presolve.

#include "catch.hpp"

#include "Highs.h"

#include <string>
#include <vector>

namespace {

//! `n` row singletons x_i <= 1, each a reduction for presolve. Presolve checks its limits once per 1,024 reductions,
//! so the model reaches that check before presolve ends.
HighsLp SingletonRowsLp(HighsInt n, HighsVarType type) {
	HighsLp lp;
	lp.a_matrix_.format_ = MatrixFormat::kColwise;
	lp.a_matrix_.start_ = {0};
	lp.sense_ = ObjSense::kMaximize;
	for (HighsInt i = 0; i < n; ++i) {
		lp.row_lower_.push_back(-kHighsInf);
		lp.row_upper_.push_back(1.0);
		lp.col_cost_.push_back(1.0 + i % 7);
		lp.col_lower_.push_back(0.0);
		lp.col_upper_.push_back(5.0);
		lp.integrality_.push_back(type);
		lp.a_matrix_.index_.push_back(i);
		lp.a_matrix_.value_.push_back(1.0);
		lp.a_matrix_.start_.push_back(lp.a_matrix_.index_.size());
	}
	lp.num_row_ = n;
	lp.num_col_ = n;
	return lp;
}

//! Solves `lp` with only `interrupt` started; returns the messages of the checks that asked it.
std::vector<std::string> InterruptAsks(const HighsLp &lp, HighsCallbackType interrupt) {
	std::vector<std::string> asks;
	Highs highs;
	highs.setOptionValue("output_flag", false);
	REQUIRE(highs.passModel(lp) == HighsStatus::kOk);
	highs.setCallback(
	    [&asks](int, const std::string &message, const HighsCallbackOutput *, HighsCallbackInput *data_in, void *) {
		    asks.push_back(message);
		    data_in->user_interrupt = true;
	    });
	highs.startCallback(interrupt);
	highs.run();
	REQUIRE(highs.getModelStatus() == HighsModelStatus::kInterrupt);
	return asks;
}

bool AskedByPresolve(const std::vector<std::string> &asks) {
	for (auto &message : asks) {
		if (message == "Presolve interrupt") {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("HiGHS presolve stops at a user interrupt", "[decidb][highs_presolve]") {
	SECTION("MIP presolve asks the MIP interrupt") {
		REQUIRE(AskedByPresolve(InterruptAsks(SingletonRowsLp(3000, HighsVarType::kInteger), kCallbackMipInterrupt)));
	}
	SECTION("LP presolve asks the simplex interrupt") {
		REQUIRE(AskedByPresolve(
		    InterruptAsks(SingletonRowsLp(3000, HighsVarType::kContinuous), kCallbackSimplexInterrupt)));
	}
}
