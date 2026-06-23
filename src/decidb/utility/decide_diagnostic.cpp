#include "duckdb/decidb/decide_diagnostic.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <algorithm>
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

static void EscapeRateSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	double v = parameter.GetValue<double>();
	if (!(v > 0.0 && v <= 1.0)) {
		throw InvalidInputException(
		    "diagnose_decide_escape_rate must be in (0, 1]; got " + parameter.ToString() + ".");
	}
}

static void CategoricalRatioSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	double v = parameter.GetValue<double>();
	if (!(v > 0.0 && v <= 1.0)) {
		throw InvalidInputException(
		    "diagnose_decide_categorical_ratio must be in (0, 1]; got " + parameter.ToString() + ".");
	}
}

static void MinCategoriesSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	int64_t v = parameter.GetValue<int64_t>();
	if (v < 1) {
		throw InvalidInputException(
		    "diagnose_decide_min_categories must be >= 1; got " + parameter.ToString() + ".");
	}
}

void RegisterDecideDiagnosticOptions(DBConfig &config) {
	config.AddExtensionOption(
	    "diagnose_decide",
	    "DECIDE failure-diagnosis mode: none (default), infeasible, unbounded, slow, or auto. "
	    "Filter semantics: a mode produces a diagnosis only when the solve actually lands in that "
	    "state; otherwise the query behaves normally.",
	    LogicalType::VARCHAR, Value("none"), DiagnoseDecideSetCallback);
	// Unbounded characterization knobs (see decide_diagnostics() escaping_instances).
	config.AddExtensionOption(
	    "diagnose_decide_escape_rate",
	    "Unbounded diagnosis: report a categorical group when its within-group escape rate "
	    "(escaping/total instances) is at least this value. Range (0, 1]. Default 0.8.",
	    LogicalType::DOUBLE, Value::DOUBLE(0.8), EscapeRateSetCallback);
	config.AddExtensionOption(
	    "diagnose_decide_categorical_ratio",
	    "Unbounded diagnosis: a column is treated as categorical for characterization when its "
	    "distinct-value count is at most ratio × num_rows (subject to the min-categories floor). "
	    "Range (0, 1]. Default 0.1.",
	    LogicalType::DOUBLE, Value::DOUBLE(0.1), CategoricalRatioSetCallback);
	config.AddExtensionOption(
	    "diagnose_decide_min_categories",
	    "Unbounded diagnosis: absolute floor on the categorical distinct-value cap, so small "
	    "tables still qualify when ratio × num_rows rounds below a few. Default 20.",
	    LogicalType::BIGINT, Value::BIGINT(20), MinCategoriesSetCallback);
}

string GetDiagnoseDecideMode(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting("diagnose_decide", value) && !value.IsNull()) {
		return StringUtil::Lower(value.ToString());
	}
	return "none";
}

DecideDiagParams GetDecideDiagnosticParams(ClientContext &context) {
	DecideDiagParams params; // defaults
	Value value;
	if (context.TryGetCurrentSetting("diagnose_decide_escape_rate", value) && !value.IsNull()) {
		params.escape_rate = value.GetValue<double>();
	}
	if (context.TryGetCurrentSetting("diagnose_decide_categorical_ratio", value) && !value.IsNull()) {
		params.categorical_ratio = value.GetValue<double>();
	}
	if (context.TryGetCurrentSetting("diagnose_decide_min_categories", value) && !value.IsNull()) {
		auto v = value.GetValue<int64_t>();
		params.min_categories = v < 1 ? 1 : (idx_t)v;
	}
	return params;
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

vector<EscapeRule> CharacterizeEscape(const std::set<idx_t> &escaping, idx_t total_instances,
                                      const vector<ColumnGrouping> &candidates,
                                      double escape_rate_threshold) {
	(void)total_instances; // counts come from instance_to_group; param documents the contract
	vector<EscapeRule> rules;
	for (const auto &cg : candidates) {
		idx_t num_groups = cg.group_value.size();
		if (num_groups == 0) {
			continue;
		}
		vector<idx_t> total_count(num_groups, 0);
		vector<idx_t> esc_count(num_groups, 0);
		for (idx_t i = 0; i < cg.instance_to_group.size(); i++) {
			idx_t g = cg.instance_to_group[i];
			if (g == DConstants::INVALID_INDEX || g >= num_groups) {
				continue; // instance excluded from this column's grouping (e.g. NULL key)
			}
			total_count[g]++;
			if (escaping.count(i)) {
				esc_count[g]++;
			}
		}
		// A group is a "sufficient-direction" rule when (nearly) all of its instances
		// escape: rate = escaping/total ≥ threshold. The a/b count is carried so a
		// threshold < 1 stays honest.
		for (idx_t g = 0; g < num_groups; g++) {
			if (total_count[g] == 0 || esc_count[g] == 0) {
				continue;
			}
			double rate = (double)esc_count[g] / (double)total_count[g];
			if (rate + 1e-12 >= escape_rate_threshold) {
				EscapeRule r;
				r.column = cg.column;
				r.value = cg.group_value[g];
				r.escaping = esc_count[g];
				r.total = total_count[g];
				rules.push_back(std::move(r));
			}
		}
	}
	// Deterministic order: strongest rule first, then column/value.
	std::sort(rules.begin(), rules.end(), [](const EscapeRule &a, const EscapeRule &b) {
		double ra = (double)a.escaping / (double)a.total;
		double rb = (double)b.escaping / (double)b.total;
		if (ra != rb) {
			return ra > rb;
		}
		if (a.column != b.column) {
			return a.column < b.column;
		}
		return a.value < b.value;
	});
	return rules;
}

//! Format one variable's `escaping_instances` cell. Empty => NULL.
static string FormatEscapingInstances(const VarEscape &ve) {
	if (ve.is_aux || ve.total <= 1) {
		// Aux/linearization columns are name-only; a single-instance variable (no
		// scope multiplicity, e.g. the run.sh demo) has nothing to disambiguate.
		return string();
	}
	if (ve.all_escape) {
		return "all " + std::to_string(ve.total) + " instances escape";
	}
	if (!ve.rules.empty()) {
		string cell;
		for (idx_t i = 0; i < ve.rules.size(); i++) {
			const auto &r = ve.rules[i];
			cell += (i == 0 ? "" : "; ") + r.column + "=" + r.value + " (" +
			        std::to_string(r.escaping) + "/" + std::to_string(r.total) + ")";
		}
		return cell;
	}
	// Single escaping instance among many, or a scattered escape no categorical group
	// characterizes: report the bare count.
	return std::to_string(ve.escaping) + " of " + std::to_string(ve.total) + " instances escape";
}

DecideDiagnostic BuildUnboundedDiagnostic(const vector<VarEscape> &escapes,
                                          bool saw_unnamed_global) {
	DecideDiagnostic diag;
	diag.valid = true;
	diag.status = SolverStatus::UNBOUNDED;
	diag.state = "unbounded";

	if (!escapes.empty()) {
		string names;
		for (idx_t i = 0; i < escapes.size(); i++) {
			names += (i == 0 ? "" : ", ") + escapes[i].name;
		}
		diag.summary = "The objective is unbounded: it can improve without limit because " +
		               string(escapes.size() == 1 ? "the variable " : "the variables ") + names +
		               " can grow without bound.";
		for (const auto &ve : escapes) {
			DiagnosticRow row;
			row.variable = ve.name;
			row.direction = ve.direction;
			row.escaping_instances = FormatEscapingInstances(ve);
			// suggested_bound intentionally left empty (=> NULL): DeciDB never picks it.
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
	names = {"query_id", "state", "variable", "direction", "escaping_instances", "suggested_bound"};
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
		output.SetValue(col++, count,
		                row.escaping_instances.empty() ? Value() : Value(row.escaping_instances));
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
