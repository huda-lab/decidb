//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_debug.cpp
//
// SET decide_debug. See decide_debug.hpp.
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_debug.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <algorithm>

namespace duckdb {

//! `text` as a count of digits only; false for anything else, a sign included.
static bool ParseCount(const string &text, idx_t &count) {
	if (text.empty() || text.size() > 18 ||
	    !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
		return false;
	}
	count = std::stoull(text);
	return true;
}

DecideDebug ParseDecideDebug(const string &setting) {
	DecideDebug debug;
	for (auto item : StringUtil::Split(setting, ',')) {
		StringUtil::Trim(item);
		if (item.empty()) {
			continue;
		}
		const auto equals = item.find('=');
		const auto name = StringUtil::Lower(item.substr(0, equals));
		const auto value = equals == string::npos ? string() : item.substr(equals + 1);
		idx_t count = 0;
		if (equals == string::npos) {
			if (name == "tree") {
				debug.tree = true;
			} else if (name == "bodies") {
				debug.bodies = true;
			} else if (name == "containers") {
				debug.containers = true;
			} else if (name == "values") {
				debug.values = true;
			} else if (name == "steps") {
				debug.steps = true;
			} else {
				throw InvalidInputException("decide_debug: unknown item '%s'", item);
			}
		} else if (name == "chunk") {
			if (!ParseCount(value, count) || count == 0 || count > STANDARD_VECTOR_SIZE) {
				throw InvalidInputException("decide_debug: chunk must be between 1 and %llu", STANDARD_VECTOR_SIZE);
			}
			debug.chunk = count;
		} else if (name == "split") {
			if (!ParseCount(value, count) || count == 0) {
				throw InvalidInputException("decide_debug: split must be a positive row count");
			}
			debug.split = count;
		} else if (name == "inject") {
			auto parts = StringUtil::Split(value, ':');
			if (parts.size() != 3 || parts[1] != "data" || !ParseCount(parts[0], count)) {
				throw InvalidInputException("decide_debug: inject must be node:data:value");
			}
			debug.inject.node = count;
			debug.inject.data = parts[2];
		} else {
			throw InvalidInputException("decide_debug: unknown item '%s'", item);
		}
	}
	return debug;
}

DecideDebug GetDecideDebug(ClientContext &context) {
	Value value;
	// The option is registered with a default, and its callback rejects any value ParseDecideDebug refuses.
	bool found = context.TryGetCurrentSetting("decide_debug", value);
	D_ASSERT(found && !value.IsNull());
	(void)found;
	return ParseDecideDebug(value.GetValue<string>());
}

static void DecideDebugSetCallback(ClientContext &, SetScope, Value &parameter) {
	ParseDecideDebug(parameter.GetValue<string>());
}

void RegisterDecideDebug(DBConfig &config) {
	config.AddExtensionOption("decide_debug",
	                          "DECIDE evaluator debugging, a comma list: tree, bodies, containers, values and steps "
	                          "print the evaluator's dumps to stderr; chunk=N and split=N set the descent's chunk size "
	                          "and the rows after which a thread's descent is merged; inject=N:data:V stands V in for "
	                          "the data value of node N",
	                          LogicalType::VARCHAR, Value(""), DecideDebugSetCallback);
}

} // namespace duckdb
