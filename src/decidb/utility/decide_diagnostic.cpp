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
	return mode == "off" || mode == "auto";
}

static void DiagnoseDecideSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	string mode = StringUtil::Lower(parameter.ToString());
	if (!IsValidDiagnoseMode(mode)) {
		throw InvalidInputException(
		    "Invalid diagnose_decide mode '" + parameter.ToString() +
		    "'. Valid modes: off, auto.");
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
	    "DECIDE failure-diagnosis mode: auto (default) or off. Under auto, a failed solve "
	    "(infeasible / unbounded / time-limit) is automatically diagnosed where an engine exists; "
	    "off suppresses diagnosis and reproduces the plain static solver error.",
	    LogicalType::VARCHAR, Value("auto"), DiagnoseDecideSetCallback);
	// Unbounded characterization knobs (see decide_diagnostics() affected_rows).
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
	return "auto";
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
		       status == SolverStatus::INF_OR_UNBD ||
		       status == SolverStatus::TIME_LIMIT;
	}
	return false; // "off" or unrecognized
}

bool DiagnoseModeWantsUnboundedRay(const string &mode) {
	return mode == "auto";
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

//! Format one variable's `affected_rows` / `affected_entities` cell. Empty => NULL.
static string FormatEscapingInstances(const VarEscape &ve) {
	if (ve.is_aux || ve.total <= 1) {
		// Aux/linearization columns are name-only; a single-instance variable (no scope
		// multiplicity — e.g. a DECIDE over a single-row input) has nothing to
		// disambiguate.
		return string();
	}
	// Plain-language, self-describing cell: "rows"/"entities" per the variable's scope,
	// so no legend is needed to read it.
	const char *noun = ve.is_entity_scoped ? " entities" : " rows";
	if (ve.all_escape) {
		return "all " + std::to_string(ve.total) + noun;
	}
	if (!ve.rules.empty()) {
		string cell;
		for (idx_t i = 0; i < ve.rules.size(); i++) {
			const auto &r = ve.rules[i];
			cell += (i == 0 ? "" : "; ") + std::to_string(r.escaping) + " of " +
			        std::to_string(r.total) + noun + " where " + r.column + " = '" + r.value + "'";
		}
		return cell;
	}
	// Scattered escape that no categorical group characterizes: report the bare count.
	return std::to_string(ve.escaping) + " of " + std::to_string(ve.total) + noun;
}

DecideDiagnostic BuildUnboundedDiagnostic(const vector<VarEscape> &escapes) {
	// Precondition: at least one named escaping variable. The caller falls through to
	// the rich static error when the ray names nothing (a quadratic model attaches no
	// ray, or only internal auxiliaries escaped), so this never builds a content-free
	// diagnosis.
	D_ASSERT(!escapes.empty());
	DecideDiagnostic diag;
	diag.valid = true;
	diag.status = SolverStatus::UNBOUNDED;
	diag.state = "unbounded";

	string names;
	for (idx_t i = 0; i < escapes.size(); i++) {
		names += (i == 0 ? "" : ", ") + escapes[i].name;
	}
	// Prescribe the forced remedy (add a finite bound) without inventing the cap
	// value — DeciDB names what to change, the user supplies the magnitude.
	if (escapes.size() == 1) {
		diag.summary = "variable " + names + " can grow without bound. Add an upper bound, e.g. SUCH THAT " +
		               escapes[0].name + " <= <cap>.";
	} else {
		string bounds;
		for (idx_t i = 0; i < escapes.size(); i++) {
			bounds += (i == 0 ? "" : " AND ") + escapes[i].name + " <= <cap>";
		}
		diag.summary = "variables " + names + " can grow without bound. Add upper bounds, e.g. SUCH THAT " +
		               bounds + ".";
	}
	for (const auto &ve : escapes) {
		DiagnosticRow row;
		row.subject_kind = "variable";
		row.subject = ve.name;
		row.attribute = "grows_toward";
		row.value = ve.direction;
		diag.rows.push_back(std::move(row));

		auto escaping_instances = FormatEscapingInstances(ve);
		if (!escaping_instances.empty()) {
			DiagnosticRow instances_row;
			instances_row.subject_kind = "variable";
			instances_row.subject = ve.name;
			instances_row.attribute = ve.is_entity_scoped ? "affected_entities" : "affected_rows";
			instances_row.value = std::move(escaping_instances);
			diag.rows.push_back(std::move(instances_row));
		}
	}
	return diag;
}

DecideDiagnostic BuildInfeasibleDiagnostic(const vector<ClauseEdit> &edits) {
	// Precondition: at least one edit. The engine returns an invalid diagnosis (so the
	// caller falls through to the static error) when nothing is loosenable or every
	// slack came back zero, so this never builds a content-free edit list.
	D_ASSERT(!edits.empty());
	DecideDiagnostic diag;
	diag.valid = true;
	diag.status = SolverStatus::INFEASIBLE;
	diag.state = "infeasible";

	// Summary: name the smallest change(s) that restore feasibility. Tell the user
	// the fix, not the math — no slacks, no "elastic", just the edited constraint(s).
	if (edits.size() == 1) {
		diag.summary = "the constraints cannot all be satisfied at once. Loosen " + edits[0].label +
		               " to " + edits[0].suggestion + ".";
	} else {
		string changes;
		for (idx_t i = 0; i < edits.size(); i++) {
			changes += (i == 0 ? "" : "; ") + edits[i].label + " to " + edits[i].suggestion;
		}
		diag.summary = "the constraints cannot all be satisfied at once. Loosen " + changes + ".";
	}

	for (const auto &e : edits) {
		DiagnosticRow change_row;
		change_row.subject_kind = "clause";
		change_row.subject = e.label;
		change_row.attribute = "suggested_change";
		change_row.value = e.suggestion;
		diag.rows.push_back(std::move(change_row));

		DiagnosticRow amount_row;
		amount_row.subject_kind = "clause";
		amount_row.subject = e.label;
		amount_row.attribute = "amount";
		amount_row.value = e.amount;
		diag.rows.push_back(std::move(amount_row));
	}
	return diag;
}

DecideDiagnostic BuildElasticInfeasibleDiagnostic() {
	DecideDiagnostic diag;
	diag.valid = true;
	diag.status = SolverStatus::INFEASIBLE;
	diag.state = "infeasible";
	diag.summary = "the constraints cannot all be satisfied at once, and loosening your SUCH THAT "
	               "limits cannot fix it — the conflict involves a fixed part of the query.";
	DiagnosticRow row;
	row.subject_kind = "model";
	row.attribute = "elastic_infeasible";
	row.value = "true";
	diag.rows.push_back(std::move(row));
	return diag;
}

void StashDecideDiagnostic(ClientContext &context, DecideDiagnostic diag) {
	auto state =
	    context.registered_state->GetOrCreate<DecideDiagnosticState>(DECIDE_DIAGNOSTIC_STATE_KEY);
	// Stamp a per-connection id so every row of this diagnosis shares one diagnosis_id
	// (and a later diagnosis on the same connection gets a distinct one).
	diag.diagnosis_id = state->next_diagnosis_id++;
	state->latest = std::move(diag);
}

void ClearDecideDiagnostic(ClientContext &context) {
	// Only clear if a stash exists — a successful solve on a connection that never
	// diagnosed anything has nothing to invalidate (and no reason to create state).
	auto state = context.registered_state->Get<DecideDiagnosticState>(DECIDE_DIAGNOSTIC_STATE_KEY);
	if (state) {
		state->latest = DecideDiagnostic(); // valid=false => decide_diagnostics() returns 0 rows
	}
}

void ThrowDecideDiagnosisReady(const DecideDiagnostic &diag) {
	ThrowDecideDiagnosisReady(diag, string());
}

void ThrowDecideDiagnosisReady(const DecideDiagnostic &diag, const string &extra_message) {
	string msg = "DECIDE optimization is " + diag.state + ": " + diag.summary;
	if (!extra_message.empty()) {
		msg += " " + extra_message;
	}
	msg += "\nDetails: SELECT * FROM decide_diagnostics();";
	throw InvalidInputException(msg);
}

void ThrowUnboundedDiagnosisUnavailable(const string &reason) {
	throw InvalidInputException(
	    "DECIDE optimization is unbounded: " + reason +
	    " Add an upper bound, e.g. SUCH THAT x <= <cap>.");
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
	names = {"diagnosis_id", "state", "subject_kind", "subject", "attribute", "value"};
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
		output.SetValue(col++, count, Value::BIGINT(data.diag.diagnosis_id));
		output.SetValue(col++, count, Value(data.diag.state));
		output.SetValue(col++, count, row.subject_kind.empty() ? Value() : Value(row.subject_kind));
		output.SetValue(col++, count, row.subject.empty() ? Value() : Value(row.subject));
		output.SetValue(col++, count, row.attribute.empty() ? Value() : Value(row.attribute));
		output.SetValue(col, count, row.value.empty() ? Value() : Value(row.value));
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
