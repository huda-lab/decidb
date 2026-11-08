#include "duckdb/execution/operator/decide/decide_evaluator.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace duckdb {

DecideEvaluator SelectedDecideEvaluator() {
	static const DecideEvaluator selected = []() {
		const char *setting = std::getenv("DECIDB_EVALUATOR");
		const string value = setting ? setting : "flat";
		if (value == "flat") {
			return DecideEvaluator::FLAT;
		}
		if (value == "verify") {
			return DecideEvaluator::VERIFY;
		}
		if (value == "tree") {
			return DecideEvaluator::TREE;
		}
		if (value == "tree-noside") {
			return DecideEvaluator::TREE_NOSIDE;
		}
		throw InvalidInputException("DECIDB_EVALUATOR=%s: expected flat, verify, tree or tree-noside", value);
	}();
	return selected;
}

static int64_t OrderedBits(double value) {
	int64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

bool DecideTreeRequested() {
	const auto selected = SelectedDecideEvaluator();
	return selected == DecideEvaluator::TREE || selected == DecideEvaluator::TREE_NOSIDE ||
	       std::getenv("DECIDB_DUMP_TREE") || std::getenv("DECIDB_DUMP_CONTAINERS");
}

//! Within a few representable values of each other; NaN equals NaN, as both runs read one.
static bool CloseEnough(double a, double b) {
	static constexpr int64_t MAX_ULPS = 4;
	if (a == b || (std::isnan(a) && std::isnan(b))) {
		return true;
	}
	if (!std::isfinite(a) || !std::isfinite(b) || std::signbit(a) != std::signbit(b)) {
		return false;
	}
	const int64_t distance = OrderedBits(a) - OrderedBits(b);
	return distance <= MAX_ULPS && distance >= -MAX_ULPS;
}

//! DECIDB_EVALUATOR_INJECT: a test's difference in `record`, so a comparison that can no longer fail is seen.
static void InjectDifference(DecideEvaluationRecord &record) {
	const char *setting = std::getenv("DECIDB_EVALUATOR_INJECT");
	if (!setting) {
		return;
	}
	auto parts = StringUtil::Split(string(setting), ':');
	const bool one_ulp = parts.size() == 2 && parts[1] == "ulp";
	for (auto &field : record.fields) {
		if (field.group != parts[0]) {
			continue;
		}
		if (field.numbers.empty()) {
			field.text += "!";
		} else if (one_ulp) {
			field.numbers[0] = std::nextafter(field.numbers[0], field.numbers[0] + 1.0);
		} else {
			field.numbers[0] += 1.0;
		}
		return;
	}
	throw InternalException("DECIDE: DECIDB_EVALUATOR_INJECT found no field in group %s", parts[0]);
}

[[noreturn]] static void ThrowDifference(const string &what, const string &flat, const string &second) {
	throw InternalException("DECIDE evaluators differ in %s: flat has %s, the second run %s", what, flat, second);
}

void CompareDecideEvaluations(const DecideEvaluationRecord &flat, const ErrorData &flat_error,
                              DecideEvaluationRecord second, const ErrorData &second_error) {
	auto raised = [](const ErrorData &error) {
		return error.HasError() ? error.Message() : string("no error");
	};
	if (raised(flat_error) != raised(second_error)) {
		ThrowDifference("what they raised", raised(flat_error), raised(second_error));
	}
	if (flat_error.HasError()) {
		return;
	}
	InjectDifference(second);
	if (flat.fields.size() != second.fields.size()) {
		ThrowDifference("how much they record", to_string(flat.fields.size()), to_string(second.fields.size()));
	}
	for (idx_t i = 0; i < flat.fields.size(); i++) {
		auto &a = flat.fields[i];
		auto &b = second.fields[i];
		const string where = a.group + " (" + a.name + ")";
		if (a.group != b.group || a.name != b.name || a.text != b.text) {
			ThrowDifference(where, a.group + " " + a.name + " " + a.text, b.group + " " + b.name + " " + b.text);
		}
		if (a.numbers.size() != b.numbers.size()) {
			ThrowDifference(where, to_string(a.numbers.size()) + " numbers", to_string(b.numbers.size()));
		}
		for (idx_t n = 0; n < a.numbers.size(); n++) {
			if (!CloseEnough(a.numbers[n], b.numbers[n])) {
				ThrowDifference(where + " at " + to_string(n), to_string(a.numbers[n]), to_string(b.numbers[n]));
			}
		}
	}
}

} // namespace duckdb
