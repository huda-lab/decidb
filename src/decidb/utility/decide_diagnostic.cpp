#include "duckdb/decidb/decide_diagnostic.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <cmath>
#include <set>

namespace duckdb {

//===----------------------------------------------------------------------===//
// F4: diagnose_decide session setting + filter gate
//===----------------------------------------------------------------------===//

static bool IsValidDiagnoseMode(const string &mode) {
	return mode == "none" || mode == "infeasible" || mode == "unbounded" || mode == "slow" ||
	       mode == "auto";
}

static void DiagnoseDecideSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	string mode = StringUtil::Lower(parameter.ToString());
	if (!IsValidDiagnoseMode(mode)) {
		throw InvalidInputException(
		    "Invalid diagnose_decide mode '" + parameter.ToString() +
		    "'. Valid modes: none, infeasible, unbounded, slow, auto.");
	}
	parameter = Value(mode); // normalize to lowercase
}

void RegisterDecideDiagnosticOptions(DBConfig &config) {
	config.AddExtensionOption(
	    "diagnose_decide",
	    "DECIDE failure-diagnosis mode: none (default), infeasible, unbounded, slow, or auto. "
	    "Filter semantics: a mode produces a diagnosis only when the solve actually lands in that "
	    "state; otherwise the query behaves normally.",
	    LogicalType::VARCHAR, Value("none"), DiagnoseDecideSetCallback);
}

string GetDiagnoseDecideMode(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting("diagnose_decide", value) && !value.IsNull()) {
		return StringUtil::Lower(value.ToString());
	}
	return "none";
}

bool DiagnosisApplies(const string &mode, SolverStatus status) {
	if (mode == "auto") {
		return status == SolverStatus::INFEASIBLE || status == SolverStatus::UNBOUNDED ||
		       status == SolverStatus::TIME_LIMIT;
	}
	if (mode == "infeasible") {
		return status == SolverStatus::INFEASIBLE;
	}
	if (mode == "unbounded") {
		return status == SolverStatus::UNBOUNDED;
	}
	if (mode == "slow") {
		return status == SolverStatus::TIME_LIMIT;
	}
	return false; // "none" or unrecognized
}

bool DiagnoseModeWantsUnboundedRay(const string &mode) {
	return mode == "unbounded" || mode == "auto";
}

//===----------------------------------------------------------------------===//
// Diagnosis construction + per-connection stash
//===----------------------------------------------------------------------===//

DecideDiagnostic BuildUnboundedDiagnostic(const SolverResult &result,
                                          const vector<ColumnProvenance> &columns) {
	// Name the escaping variables from the ray (F6). The U2 box-LP ray already fixes
	// any column with a finite upper bound to 0, so a non-zero ray entry is exactly a
	// variable that can grow without bound. We report, per escaping variable: its NAME,
	// the DIRECTION it escapes (the sign of its ray entry). `group_label` and
	// `suggested_bound` are deferred (left NULL). DeciDB never picks the bound value.
	static constexpr double RAY_ESCAPE_EPSILON = 1e-8;

	DecideDiagnostic diag;
	diag.valid = true;
	diag.status = SolverStatus::UNBOUNDED;
	diag.state = "unbounded";

	// Collect escaping variables, deduplicated by their user-facing name so a
	// row-scoped variable escaping across every row reports once. First-seen column
	// order is preserved for deterministic output; the sign of the first-seen ray
	// entry gives the escape direction (+ for an increase to +∞, - for -∞).
	vector<string> escaping_names; // display names, in first-seen column order
	vector<string> escaping_dirs;  // parallel: "+∞" / "-∞"
	std::set<string> seen;
	bool saw_unnamed_global = false;
	for (idx_t col = 0; col < result.ray.size() && col < columns.size(); col++) {
		double r = result.ray[col];
		if (std::fabs(r) <= RAY_ESCAPE_EPSILON) {
			continue;
		}
		const ColumnProvenance &prov = columns[col];
		if (prov.kind == ColumnKind::GLOBAL_AUX || prov.label.empty()) {
			saw_unnamed_global = true;
			continue;
		}
		// For an AUX column the label is the user's source expression; for a USER
		// column it is the variable name. Both go in as-is.
		if (seen.insert(prov.label).second) {
			escaping_names.push_back(prov.label);
			escaping_dirs.push_back(r > 0 ? "+∞" : "-∞");
		}
	}

	if (!escaping_names.empty()) {
		string names;
		for (idx_t i = 0; i < escaping_names.size(); i++) {
			names += (i == 0 ? "" : ", ") + escaping_names[i];
		}
		diag.summary = "The objective is unbounded: it can improve without limit because " +
		               string(escaping_names.size() == 1 ? "the variable " : "the variables ") +
		               names + " can grow without bound.";
		for (idx_t i = 0; i < escaping_names.size(); i++) {
			DiagnosticRow row;
			row.variable = escaping_names[i];
			row.direction = escaping_dirs[i];
			// group_label + suggested_bound intentionally left empty (=> NULL) for now.
			diag.rows.push_back(std::move(row));
		}
		return diag;
	}

	// No named escaping variable resolved — either no ray was attached (e.g. a
	// quadratic model, where U2 extracts none) or only an internal global auxiliary
	// escaped (which signals a model-generation issue, not a user error). Emit a
	// single detail-less row so the relation still reports the unbounded state; the
	// stderr summary carries the explanation.
	if (saw_unnamed_global) {
		diag.summary = "The objective is unbounded: an internal auxiliary variable grows "
		               "without bound (this likely indicates a model-generation issue).";
	} else {
		diag.summary = "The objective is unbounded: at least one decision variable can grow "
		               "without bound.";
	}
	diag.rows.emplace_back(); // all fields empty => one all-NULL detail row under state=unbounded
	return diag;
}

void StashDecideDiagnostic(ClientContext &context, DecideDiagnostic diag) {
	auto state =
	    context.registered_state->GetOrCreate<DecideDiagnosticState>(DECIDE_DIAGNOSTIC_STATE_KEY);
	// Stamp a per-connection id so every row of this diagnosis shares one query_id
	// (and a later diagnosis on the same connection gets a distinct one).
	diag.query_id = state->next_query_id++;
	state->latest = std::move(diag);
}

void ThrowDecideDiagnosisReady(const DecideDiagnostic &diag) {
	string msg = "DECIDE optimization is " + diag.state + ".\n\n" + diag.summary +
	             "\n\nDiagnosis ready: SELECT * FROM decide_diagnostics();";
	throw InvalidInputException(msg);
}

//===----------------------------------------------------------------------===//
// decide_diagnostics() table function
//===----------------------------------------------------------------------===//

namespace {

struct DecideDiagnosticsData : public GlobalTableFunctionState {
	DecideDiagnosticsData() : offset(0) {
	}
	DecideDiagnostic diag;
	idx_t offset;
};

unique_ptr<FunctionData> DecideDiagnosticsBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	names = {"query_id", "state", "variable", "direction", "group_label", "suggested_bound"};
	return_types = {LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DecideDiagnosticsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DecideDiagnosticsData>();
	// Snapshot the stash so the scan is stable even if another statement runs.
	auto state = context.registered_state->Get<DecideDiagnosticState>(DECIDE_DIAGNOSTIC_STATE_KEY);
	if (state) {
		result->diag = state->latest;
	}
	return std::move(result);
}

void DecideDiagnosticsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DecideDiagnosticsData>();
	if (!data.diag.valid || data.offset >= data.diag.rows.size()) {
		return; // nothing stashed, or all rows emitted
	}
	idx_t count = 0;
	while (data.offset < data.diag.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = data.diag.rows[data.offset++];
		idx_t col = 0;
		output.SetValue(col++, count, Value::BIGINT(data.diag.query_id));
		output.SetValue(col++, count, Value(data.diag.state));
		output.SetValue(col++, count, row.variable.empty() ? Value() : Value(row.variable));
		output.SetValue(col++, count, row.direction.empty() ? Value() : Value(row.direction));
		output.SetValue(col++, count, row.group_label.empty() ? Value() : Value(row.group_label));
		output.SetValue(col, count, row.suggested_bound.empty() ? Value() : Value(row.suggested_bound));
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void DecideDiagnosticsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("decide_diagnostics", {}, DecideDiagnosticsFunction, DecideDiagnosticsBind,
	                              DecideDiagnosticsInit));
}

} // namespace duckdb
