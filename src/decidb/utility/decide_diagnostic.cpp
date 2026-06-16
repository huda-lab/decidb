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
	// variable that can grow without bound (the type/sign/bound "suspects" filter is
	// pre-baked). DeciDB never picks the bound value — any finite bound works and the
	// right number is domain knowledge.
	static constexpr double RAY_ESCAPE_EPSILON = 1e-8;

	DecideDiagnostic diag;
	diag.valid = true;
	diag.status = SolverStatus::UNBOUNDED;
	diag.state = "unbounded";

	// Collect escaping columns, deduplicated by their user-facing label so a
	// row-scoped variable escaping across every row reports once. Column order is
	// preserved for deterministic output.
	vector<string> escaping_labels; // display labels, in first-seen column order
	std::set<string> seen;
	bool saw_unnamed_global = false;
	for (idx_t col = 0; col < result.ray.size() && col < columns.size(); col++) {
		if (std::fabs(result.ray[col]) <= RAY_ESCAPE_EPSILON) {
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
			escaping_labels.push_back(prov.label);
		}
	}

	if (!escaping_labels.empty()) {
		string names;
		for (idx_t i = 0; i < escaping_labels.size(); i++) {
			names += (i == 0 ? "" : ", ") + escaping_labels[i];
		}
		diag.summary = "The objective is unbounded: it can improve without limit because " +
		               string(escaping_labels.size() == 1 ? "the variable " : "the variables ") +
		               names + " can grow without bound.";
		for (auto &label : escaping_labels) {
			DiagnosticRow row;
			row.edit_kind = "add bound";
			row.suggested_change = "'" + label +
			                       "' can grow without bound; add an upper bound (e.g. SUCH THAT " +
			                       label + " <= K), or fix an objective sign error.";
			diag.rows.push_back(std::move(row));
		}
		return diag;
	}

	// No named escaping variable resolved — either no ray was attached (e.g. a
	// quadratic model, where U2 extracts none) or only an internal global auxiliary
	// escaped (which signals a model-generation issue, not a user error). Fall back
	// to the generic prescription so behavior never regresses.
	diag.summary = "The objective is unbounded: it can improve without limit because "
	               "at least one decision variable can grow without bound.";
	DiagnosticRow row;
	row.edit_kind = "add bound";
	if (saw_unnamed_global) {
		row.suggested_change = "An internal auxiliary variable grows without bound (this "
		                       "likely indicates a model-generation issue). Add an upper "
		                       "bound to the relevant decision variable.";
	} else {
		row.suggested_change = "Add an upper bound to bound the objective "
		                       "(e.g. SUCH THAT <var> <= K), or fix an objective sign error.";
	}
	diag.rows.push_back(std::move(row));
	return diag;
}

void StashDecideDiagnostic(ClientContext &context, DecideDiagnostic diag) {
	auto state =
	    context.registered_state->GetOrCreate<DecideDiagnosticState>(DECIDE_DIAGNOSTIC_STATE_KEY);
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
	names = {"state", "clause", "group_key", "edit_kind", "suggested_change"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR};
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
		output.SetValue(col++, count, Value(data.diag.state));
		output.SetValue(col++, count, row.clause.empty() ? Value() : Value(row.clause));
		output.SetValue(col++, count, row.group_key.empty() ? Value() : Value(row.group_key));
		output.SetValue(col++, count, Value(row.edit_kind));
		output.SetValue(col, count, Value(row.suggested_change));
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
