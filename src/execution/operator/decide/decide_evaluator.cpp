#include "duckdb/execution/operator/decide/decide_evaluator.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>

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

bool DecideTreeDumpRequested() {
	return std::getenv("DECIDB_DUMP_TREE") || std::getenv("DECIDB_DUMP_CONTAINERS");
}

bool DecideTreeRequested() {
	return SelectedDecideEvaluator() != DecideEvaluator::FLAT || DecideTreeDumpRequested();
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

//! DECIDB_EVALUATOR_INJECT: a test's differences in `record`, so a comparison that can no longer fail is seen. Each of
//! its comma-separated entries names a group: the group's first field changes, nudged by one ulp with `:ulp`; with
//! `:extra` the group gains a field.
static void InjectDifference(DecideEvaluationRecord &record) {
	const char *setting = std::getenv("DECIDB_EVALUATOR_INJECT");
	if (!setting) {
		return;
	}
	for (auto &entry : StringUtil::Split(string(setting), ',')) {
		auto parts = StringUtil::Split(entry, ':');
		const auto mode = parts.size() == 2 ? parts[1] : string();
		if (mode == "extra") {
			record.Add(parts[0], "injected", string());
			continue;
		}
		auto field =
		    std::find_if(record.fields.begin(), record.fields.end(),
		                 [&](const DecideEvaluationRecord::Field &candidate) { return candidate.group == parts[0]; });
		if (field == record.fields.end()) {
			throw InternalException("DECIDE: DECIDB_EVALUATOR_INJECT found no field in group %s", parts[0]);
		}
		if (field->numbers.empty()) {
			field->text += "!";
		} else if (mode == "ulp") {
			field->numbers[0] = std::nextafter(field->numbers[0], field->numbers[0] + 1.0);
		} else {
			field->numbers[0] += 1.0;
		}
	}
}

//! A difference a test knows of (DECIDB_VERIFY_ALLOW): the fields of `group` whose name begins with the words of
//! `prefix`, every field of it for none, named by `bug`. A prefix ends at a word: `clause 1` covers `clause 1` and
//! `clause 1 term 0` but not `clause 10`, so it is kept with a space after it and matched against the name with one.
struct DecideAllowedDifference {
	string bug;
	string group;
	string prefix;

	bool Covers(const DecideEvaluationRecord::Field &field) const {
		return field.group == group && StringUtil::StartsWith(field.name + " ", prefix);
	}
};

//! DECIDB_VERIFY_ALLOW, comma-separated `bug:group[:field prefix]` entries; read once, as the environment of a running
//! process does not change.
static const vector<DecideAllowedDifference> &AllowedDifferences() {
	static const vector<DecideAllowedDifference> allowed = []() {
		vector<DecideAllowedDifference> result;
		const char *setting = std::getenv("DECIDB_VERIFY_ALLOW");
		if (!setting) {
			return result;
		}
		for (auto &entry : StringUtil::Split(string(setting), ',')) {
			auto parts = StringUtil::Split(entry, ':');
			if (parts.size() < 2 || parts.size() > 3) {
				throw InvalidInputException("DECIDB_VERIFY_ALLOW=%s: expected bug:group[:field prefix] entries",
				                            setting);
			}
			result.push_back({parts[0], parts[1], parts.size() == 3 ? parts[2] + " " : string()});
		}
		return result;
	}();
	return allowed;
}

//! Names an allowed difference that showed in DECIDB_VERIFY_ALLOW_LOG, one bug per line, for the test that marked it.
static void LogAllowedDifference(const DecideAllowedDifference &allowed) {
	const char *path = std::getenv("DECIDB_VERIFY_ALLOW_LOG");
	if (path) {
		std::ofstream(path, std::ios::app) << allowed.bug << "\n";
	}
}

[[noreturn]] static void ThrowDifference(const string &what, const string &flat, const string &second) {
	throw InternalException("DECIDE evaluators differ in %s: flat has %s, the second run %s", what, flat, second);
}

using DecideRecordFields = vector<const DecideEvaluationRecord::Field *>;

//! The fields of `record` in `group` that `keep` takes, in their order.
static DecideRecordFields GroupFields(const DecideEvaluationRecord &record, const string &group,
                                      const std::function<bool(const DecideEvaluationRecord::Field &)> &keep) {
	DecideRecordFields fields;
	for (auto &field : record.fields) {
		if (field.group == group && keep(field)) {
			fields.push_back(&field);
		}
	}
	return fields;
}

//! Raises the first thing two runs' fields of `group` differ in, or returns false when they differ in nothing and
//! `raise` is not set.
static bool Differ(const string &group, const DecideRecordFields &flat, const DecideRecordFields &second, bool raise) {
	auto differ = [&](const string &where, const string &a, const string &b) {
		if (raise) {
			ThrowDifference(where, a, b);
		}
		return true;
	};
	if (flat.size() != second.size()) {
		return differ("how much they record in " + group, to_string(flat.size()), to_string(second.size()));
	}
	for (idx_t i = 0; i < flat.size(); i++) {
		auto &a = *flat[i];
		auto &b = *second[i];
		const string where = a.group + " (" + a.name + ")";
		if (a.name != b.name || a.text != b.text) {
			return differ(where, a.group + " " + a.name + " " + a.text, b.group + " " + b.name + " " + b.text);
		}
		if (a.numbers.size() != b.numbers.size()) {
			return differ(where, to_string(a.numbers.size()) + " numbers", to_string(b.numbers.size()));
		}
		for (idx_t n = 0; n < a.numbers.size(); n++) {
			if (!CloseEnough(a.numbers[n], b.numbers[n])) {
				return differ(where + " at " + to_string(n), to_string(a.numbers[n]), to_string(b.numbers[n]));
			}
		}
	}
	return false;
}

void CompareDecideEvaluations(const DecideEvaluationRecord &flat, const ErrorData &flat_error,
                              DecideEvaluationRecord second, const ErrorData &second_error) {
	auto raised = [](const ErrorData &error) {
		return error.HasError() ? error.Message() : string("no error");
	};
	auto &allowed = AllowedDifferences();
	if (raised(flat_error) != raised(second_error)) {
		// A known difference in what they raised leaves nothing to compare: the runs stopped in different places.
		for (auto &known : allowed) {
			if (known.group == "raised") {
				LogAllowedDifference(known);
				return;
			}
		}
		ThrowDifference("what they raised", raised(flat_error), raised(second_error));
	}
	if (flat_error.HasError()) {
		return;
	}
	InjectDifference(second);
	// Group by group, in flat's order and then the second run's, so a group that records more cannot shift another.
	vector<string> groups;
	for (auto *record : {&flat, static_cast<const DecideEvaluationRecord *>(&second)}) {
		for (auto &field : record->fields) {
			if (std::find(groups.begin(), groups.end(), field.group) == groups.end()) {
				groups.push_back(field.group);
			}
		}
	}
	for (auto &group : groups) {
		// The fields a known difference covers are compared apart, and a difference there is logged, not raised.
		for (auto &known : allowed) {
			auto covered = [&](const DecideEvaluationRecord::Field &field) {
				return known.Covers(field);
			};
			if (Differ(group, GroupFields(flat, group, covered), GroupFields(second, group, covered), false)) {
				LogAllowedDifference(known);
			}
		}
		auto uncovered = [&](const DecideEvaluationRecord::Field &field) {
			return std::none_of(allowed.begin(), allowed.end(),
			                    [&](const DecideAllowedDifference &known) { return known.Covers(field); });
		};
		Differ(group, GroupFields(flat, group, uncovered), GroupFields(second, group, uncovered), true);
	}
}

} // namespace duckdb
