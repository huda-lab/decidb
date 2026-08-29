#include "duckdb/decidb/decide_diagnostic.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Engine tuning settings. None of these starts a diagnosis: only the DIAGNOSE
// statement prefix does that.
//===----------------------------------------------------------------------===//

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

static void RemovalBigMSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	double v = parameter.GetValue<double>();
	if (!(v >= 0.0)) {
		throw InvalidInputException(
		    "diagnose_decide_removal_bigm must be >= 0 (0 = auto); got " + parameter.ToString() + ".");
	}
}

static bool IsValidSlackScope(const string &scope) {
	return scope == "query" || scope == "expanded";
}

static void SlackScopeSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	string mode = StringUtil::Lower(parameter.ToString());
	if (!IsValidSlackScope(mode)) {
		throw InvalidInputException(
		    "Invalid diagnose_decide_infeasible_slack_scope '" + parameter.ToString() +
		    "'. Valid scopes: query, expanded.");
	}
	parameter = Value(mode); // normalize to lowercase
}

// L0 (norm(e, 0)) nonzero threshold. The reverse indicator link `ABS(e) >= tol*z`
// is violated by exactly `tol` at the boundary (e = 0, z = 1); if `tol` were at or
// below the solver feasibility tolerance (~1e-6) the solver would accept it and the
// indicator would not bite, silently reintroducing the over-count. Enforce a floor
// comfortably above it.
static constexpr double DECIDE_L0_TOLERANCE_DEFAULT = 1e-4;
static constexpr double DECIDE_L0_TOLERANCE_FLOOR = 1e-5;

static void L0ToleranceSetCallback(ClientContext &context, SetScope scope, Value &parameter) {
	double v = parameter.GetValue<double>();
	if (!(v >= DECIDE_L0_TOLERANCE_FLOOR)) {
		throw InvalidInputException(
		    "decide_l0_tolerance must be >= 1e-5 (it must exceed the solver feasibility "
		    "tolerance ~1e-6 to be enforced); got " + parameter.ToString() + ".");
	}
}

void RegisterDecideDiagnosticOptions(DBConfig &config) {
	// Unbounded characterization knobs: they shape the `group` / `amount` columns a
	// runaway variable reports under DIAGNOSE.
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
	// Infeasible diagnosis: removal Big-M for dropping a `<>` (I4 L0 / removal dial).
	config.AddExtensionOption(
	    "diagnose_decide_removal_bigm",
	    "Infeasible diagnosis: the Big-M used to neutralize a dropped `<>` constraint when "
	    "diagnosing which clause to remove. 0 (default) auto-derives a sufficient value per "
	    "clause from its existing formulation; set a positive value only to override. >= 0.",
	    LogicalType::DOUBLE, Value::DOUBLE(0.0), RemovalBigMSetCallback);
	config.AddExtensionOption(
	    "diagnose_decide_infeasible_slack_scope",
	    "Infeasible diagnosis: slack granularity. query (default): one edit per SQL-level knob "
	    "— a data-backed RHS (`x <= col`) reports a virtual query offset (`x <= col + delta`) "
	    "plus a conflict profile. expanded: one edit per emitted relaxable row/group — a "
	    "diagnostic profile of which generated constraints are tight, not a directly pasteable "
	    "SQL edit.",
	    LogicalType::VARCHAR, Value("query"), SlackScopeSetCallback);
	// L0 nonzero threshold (expressivity knob, registered here with the other DECIDE
	// session options). norm(e, 0) counts a row as nonzero when |e| >= this value.
	config.AddExtensionOption(
	    "decide_l0_tolerance",
	    "norm(expr, 0) (L0 count of nonzeros): a row counts as nonzero when |expr| is at "
	    "least this value. Must exceed the solver feasibility tolerance (~1e-6) to be "
	    "enforced, so >= 1e-5. Default 1e-4. Lower it toward the floor for small-magnitude "
	    "data; raise it to treat larger residuals as zero.",
	    LogicalType::DOUBLE, Value::DOUBLE(DECIDE_L0_TOLERANCE_DEFAULT), L0ToleranceSetCallback);
}

double GetDecideL0Tolerance(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting("decide_l0_tolerance", value) && !value.IsNull()) {
		double v = value.GetValue<double>();
		// The set-callback enforces the floor, but a direct settings write could bypass
		// it; clamp defensively so the indicator link always bites.
		return v < DECIDE_L0_TOLERANCE_FLOOR ? DECIDE_L0_TOLERANCE_FLOOR : v;
	}
	return DECIDE_L0_TOLERANCE_DEFAULT;
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
	if (context.TryGetCurrentSetting("diagnose_decide_removal_bigm", value) && !value.IsNull()) {
		double v = value.GetValue<double>();
		params.removal_bigm = v < 0.0 ? 0.0 : v;
	}
	if (context.TryGetCurrentSetting("diagnose_decide_infeasible_slack_scope", value) && !value.IsNull()) {
		// The set-callback validates/normalizes; default to "query" defensively on any
		// unrecognized value written directly to the settings.
		string scope = StringUtil::Lower(value.ToString());
		params.slack_scope = IsValidSlackScope(scope) ? scope : "query";
	}
	return params;
}

bool DiagnosisApplies(bool armed, SolverStatus status) {
	if (!armed) {
		return false; // no DIAGNOSE prefix: the query never pays for a diagnosis
	}
	// TIME_LIMIT is deliberately absent. A slow solve is ordinary execution behaviour —
	// it reports its checkpoint and offers to continue on the normal path, with or
	// without the prefix — not a state for an engine to explain.
	return status == SolverStatus::INFEASIBLE || status == SolverStatus::UNBOUNDED ||
	       status == SolverStatus::INF_OR_UNBD;
}

//===----------------------------------------------------------------------===//
// Diagnosis construction + statement-scoped handoff
//===----------------------------------------------------------------------===//

vector<EscapeRule> CharacterizeEscape(const std::set<idx_t> &escaping, idx_t total_instances,
                                      const vector<ColumnGrouping> &candidates,
                                      double escape_rate_threshold) {
	(void)total_instances; // counts come from instance_to_group; param documents the contract
	vector<EscapeRule> rules;
	// Which escaping instances each kept rule covers, and whether the rule's column was
	// named in the DECIDE clause. Two columns that pick out exactly the same escaping
	// instances state the same fact, so only one of them is reported (see below).
	// Bounded by the escaping-instance count, not the table: a rule records only its
	// own escapers, and every group of one column partitions that same set.
	vector<bool> kept_clause_referenced;
	vector<vector<idx_t>> rule_members; //!< aligned with `rules`
	std::map<vector<idx_t>, idx_t> rule_by_members;
	// Candidates arrive in column order, so the first rule reaching a given
	// escaping-instance set is the leftmost column describing it.
	for (const auto &cg : candidates) {
		idx_t num_groups = cg.group_value.size();
		if (num_groups == 0) {
			continue;
		}
		vector<idx_t> total_count(num_groups, 0);
		vector<vector<idx_t>> esc_members(num_groups);
		for (idx_t i = 0; i < cg.instance_to_group.size(); i++) {
			idx_t g = cg.instance_to_group[i];
			if (g == DConstants::INVALID_INDEX || g >= num_groups) {
				continue; // instance excluded from this column's grouping (e.g. NULL key)
			}
			total_count[g]++;
			if (escaping.count(i)) {
				esc_members[g].push_back(i); // ascending: `i` walks the instances in order
			}
		}
		// A group is a "sufficient-direction" rule when (nearly) all of its instances
		// escape: rate = escaping/total ≥ threshold. The a/b count is carried so a
		// threshold < 1 stays honest.
		for (idx_t g = 0; g < num_groups; g++) {
			if (total_count[g] == 0 || esc_members[g].empty()) {
				continue;
			}
			double rate = (double)esc_members[g].size() / (double)total_count[g];
			if (rate + 1e-12 < escape_rate_threshold) {
				continue;
			}
			EscapeRule r;
			r.column = cg.column;
			r.value = cg.group_value[g];
			r.escaping = esc_members[g].size();
			r.total = total_count[g];
			// On a narrow input many columns correlate perfectly with the escape by
			// coincidence, and reporting each one buries the rule that explains it.
			// Collapse rules covering an identical escaping-instance set to one
			// representative: a column the DECIDE clause references wins over one that
			// only rides along in the SELECT, and otherwise the leftmost column wins.
			auto found = rule_by_members.find(esc_members[g]);
			if (found == rule_by_members.end()) {
				rule_by_members.emplace(esc_members[g], rules.size());
				rule_members.push_back(std::move(esc_members[g]));
				kept_clause_referenced.push_back(cg.clause_referenced);
				rules.push_back(std::move(r));
			} else if (cg.clause_referenced && !kept_clause_referenced[found->second]) {
				kept_clause_referenced[found->second] = true;
				rules[found->second] = std::move(r);
			}
		}
	}
	// Deterministic order: strongest rule first, then column/value. Permute an index
	// so `rule_members` stays aligned while the cover below is chosen.
	vector<idx_t> order(rules.size());
	for (idx_t i = 0; i < order.size(); i++) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&rules](idx_t ia, idx_t ib) {
		const EscapeRule &a = rules[ia];
		const EscapeRule &b = rules[ib];
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

	// Can the reported rules account for every escaping instance? If so the caller may
	// scope the prescribed cap to just those rows. Only whole-group rules (rate 1.0)
	// are eligible: a partial group would cap rows that were never running away.
	//
	// Greedy, widest rule first, so the condition the user pastes stays short: one
	// `region = 'B'` covering three rows beats three `id = ...` equalities covering one
	// each. The report order above cannot serve here — it ranks by rate, which every
	// eligible rule ties on at 1.0, leaving alphabetical order to pick the cover.
	// Finding a provably smallest cover is not worth it; the alternative is only a
	// longer, equally correct condition.
	vector<idx_t> cover_order = order;
	std::sort(cover_order.begin(), cover_order.end(), [&rules](idx_t ia, idx_t ib) {
		const EscapeRule &a = rules[ia];
		const EscapeRule &b = rules[ib];
		if (a.escaping != b.escaping) {
			return a.escaping > b.escaping;
		}
		if (a.column != b.column) {
			return a.column < b.column;
		}
		return a.value < b.value;
	});
	std::set<idx_t> covered;
	vector<idx_t> cover;
	for (idx_t oi : cover_order) {
		if (rules[oi].escaping != rules[oi].total) {
			continue; // rate < 1.0
		}
		bool adds = false;
		for (idx_t m : rule_members[oi]) {
			if (!covered.count(m)) {
				adds = true;
				break;
			}
		}
		if (!adds) {
			continue; // wholly implied by rules already chosen
		}
		covered.insert(rule_members[oi].begin(), rule_members[oi].end());
		cover.push_back(oi);
	}
	if (covered.size() == escaping.size()) {
		for (idx_t oi : cover) {
			rules[oi].covers_scope = true;
		}
	}

	vector<EscapeRule> sorted;
	sorted.reserve(rules.size());
	for (idx_t oi : order) {
		sorted.push_back(std::move(rules[oi]));
	}
	return sorted;
}

//! The prescribed remedy for a runaway variable: name what to add, never invent the
//! cap. DeciDB knows the variable needs a ceiling; only the user knows how high.
//!
//! When the reported rules account for every escaping instance (EscapeRule::covers_scope)
//! the cap is scoped to exactly those rows, so pasting it back does not also restrict
//! rows that were already bounded. Rendered as a `SUCH THAT` conjunct the user can paste:
//! `buy <= <cap> WHEN region = 'B'`, or over several rules the disjunction they form.
//! Anything less than full coverage keeps the global form — capping extra rows is merely
//! over-restrictive, but missing an escaper would leave the query unbounded.
//!
//! The parentheses are required: DeciQL's `WHEN` takes a bare comparison or a
//! parenthesized condition, and an unbracketed `OR` is a parse error.
static string PrescribeCap(const VarEscape &ve) {
	string cap = ve.name + " <= <cap>";
	vector<string> scope;
	for (const auto &r : ve.rules) {
		if (r.covers_scope) {
			scope.push_back(r.column + " = '" + r.value + "'");
		}
	}
	if (scope.empty()) {
		return cap;
	}
	if (scope.size() == 1) {
		return cap + " WHEN " + scope[0];
	}
	return cap + " WHEN (" + StringUtil::Join(scope, " OR ") + ")";
}

DecideDiagnostic BuildUnboundedDiagnostic(const vector<VarEscape> &escapes) {
	// Precondition: at least one named escaping variable. The caller reports the bare
	// `undiagnosed` finding when the ray names nothing (a quadratic model attaches no
	// ray, or only internal auxiliaries escaped), so this never builds a content-free
	// diagnosis.
	D_ASSERT(!escapes.empty());
	DecideDiagnostic diag;
	diag.valid = true;
	diag.state = "unbounded";

	for (const auto &ve : escapes) {
		DiagnosticFinding base;
		base.clause = ve.name;
		base.suggested_change = PrescribeCap(ve);
		base.edit_source =
		    ve.direction == EscapeDirection::NEGATIVE ? "runaway_-inf" : "runaway_+inf";

		// An aux/linearization column, or a variable with a single instance, has no
		// slice to break out: one finding, no count worth reporting.
		if (ve.is_aux || ve.total <= 1) {
			diag.findings.push_back(std::move(base));
			continue;
		}
		const char *instance_scope = ve.is_entity_scoped ? "entity" : "row";
		if (ve.all_escape) {
			// Every instance escapes, so no categorical slice explains anything the
			// whole-variable count does not: one finding covering all of them.
			DiagnosticFinding f = base;
			f.has_amount = true;
			f.amount = static_cast<double>(ve.total);
			f.has_total = true;
			f.total = static_cast<int64_t>(ve.total);
			f.scope = instance_scope;
			diag.findings.push_back(std::move(f));
			continue;
		}
		if (!ve.rules.empty()) {
			// One finding per categorical slice that cleared the escape-rate threshold,
			// so `WHERE group IS NOT NULL` is the "which slice" question and `amount`
			// answers "how many instances in it".
			for (const auto &r : ve.rules) {
				DiagnosticFinding f = base;
				f.group = r.column + " = '" + r.value + "'";
				f.has_amount = true;
				f.amount = static_cast<double>(r.escaping);
				f.has_total = true;
				f.total = static_cast<int64_t>(r.total);
				f.scope = instance_scope;
				diag.findings.push_back(std::move(f));
			}
			continue;
		}
		// Scattered escape that no categorical group characterizes: report the count
		// alone, with no slice to name.
		DiagnosticFinding f = base;
		f.has_amount = true;
		f.amount = static_cast<double>(ve.escaping);
		f.has_total = true;
		f.total = static_cast<int64_t>(ve.total);
		f.scope = instance_scope;
		diag.findings.push_back(std::move(f));
	}
	return diag;
}

DecideDiagnostic BuildInfeasibleDiagnostic(const vector<ClauseEdit> &edits,
                                           const string &achievable_objective,
                                           bool unbounded_after_fix) {
	// Precondition: at least one edit. The engine returns an invalid diagnosis (so the
	// caller emits an `undiagnosed` finding) when nothing is loosenable or every slack
	// came back zero, so this never builds a content-free edit list.
	D_ASSERT(!edits.empty());
	DecideDiagnostic diag;
	diag.valid = true;
	diag.state = "infeasible";

	for (const auto &e : edits) {
		DiagnosticFinding f;
		f.clause = e.label;
		f.group = e.group;
		if (e.row != DConstants::INVALID_INDEX) {
			f.has_row = true;
			f.row = static_cast<int64_t>(e.row);
		}
		if (e.kind == ClauseEditKind::DROP) {
			// A remove-only clause (`<>`) cannot be loosened by any amount, so there is
			// no re-quotable text and no magnitude — the change itself is the deletion.
			f.suggested_change = "remove this clause";
			f.edit_source = e.edit_source.empty() ? "remove_only" : e.edit_source;
			diag.findings.push_back(std::move(f));
			continue;
		}
		f.suggested_change = e.suggestion;
		f.has_amount = e.has_amount;
		f.amount = e.amount;
		f.edit_source = e.edit_source;
		diag.findings.push_back(std::move(f));
	}
	// I3: what the objective reaches once the edits above are applied. A model-level
	// fact, so it names no clause.
	if (unbounded_after_fix) {
		DiagnosticFinding f;
		f.edit_source = "unbounded_after_fix";
		f.suggested_change = "this fix satisfies your constraints, but the objective can then "
		                     "grow without limit — bound the terms in it too";
		diag.findings.push_back(std::move(f));
	} else if (!achievable_objective.empty()) {
		DiagnosticFinding f;
		f.edit_source = "achievable_objective";
		try {
			f.amount = std::stod(achievable_objective);
			f.has_amount = true;
		} catch (const std::exception &) {
			// Not a number we can put in a DOUBLE column: keep the text instead of
			// dropping the finding.
			f.suggested_change = achievable_objective;
		}
		diag.findings.push_back(std::move(f));
	}
	return diag;
}

DecideDiagnostic BuildUnreachableBoundDiagnostic(const vector<UnreachableClause> &clauses) {
	D_ASSERT(!clauses.empty());
	DecideDiagnostic diag;
	diag.valid = true;
	diag.state = "infeasible";

	for (const auto &c : clauses) {
		// No slack to read and no finite loosening that reaches this bound, so the
		// finding names the clause and says what is wrong with it rather than quoting
		// the user's own text back as a suggested change.
		DiagnosticFinding f;
		f.clause = c.label;
		f.group = c.group;
		f.suggested_change = "lower this bound — no assignment can reach it";
		f.edit_source = "unreachable_bound";
		diag.findings.push_back(std::move(f));
	}
	return diag;
}

DecideDiagnostic BuildElasticInfeasibleDiagnostic() {
	DecideDiagnostic diag;
	diag.valid = true;
	diag.state = "infeasible";
	DiagnosticFinding f;
	f.suggested_change = "loosening your SUCH THAT limits cannot fix this — the conflict "
	                     "involves a fixed part of the query";
	f.edit_source = "rigid_conflict";
	diag.findings.push_back(std::move(f));
	return diag;
}

DecideDiagnostic BuildUndiagnosedDiagnostic(const string &state, const string &reason) {
	DecideDiagnostic diag;
	diag.valid = true;
	diag.state = state;
	DiagnosticFinding f;
	f.suggested_change = reason;
	f.edit_source = "undiagnosed";
	diag.findings.push_back(std::move(f));
	return diag;
}

DecideDiagnostic BuildFeasibleDiagnostic() {
	DecideDiagnostic diag;
	diag.valid = true;
	diag.state = "feasible";
	diag.findings.push_back(DiagnosticFinding());
	return diag;
}

string BuildUnboundedDiagnosisUnavailableReason(bool diagnostic_timed_out, bool ray_empty,
                                                bool has_nonlinear_terms) {
	if (diagnostic_timed_out) {
		return "diagnosis ran out of time before it could identify the runaway variable.";
	}
	if (ray_empty && has_nonlinear_terms) {
		return "a non-linear term prevents naming the variable.";
	}
	if (ray_empty) {
		return "the runaway variable could not be identified.";
	}
	return "the runaway is an internal helper variable.";
}

void GetDecideDiagnoseSchema(vector<string> &names, vector<LogicalType> &types) {
	names = {"state", "clause", "suggested_change", "amount", "total",
	         "scope", "edit_source", "group", "row"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE,
	         LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::BIGINT};
}

void RenderDecideDiagnostic(const DecideDiagnostic &diag, idx_t &offset, DataChunk &output) {
	idx_t count = 0;
	auto text = [](const string &v) { return v.empty() ? Value() : Value(v); };
	while (offset < diag.findings.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &f = diag.findings[offset++];
		idx_t col = 0;
		output.SetValue(col++, count, Value(diag.state));
		output.SetValue(col++, count, text(f.clause));
		output.SetValue(col++, count, text(f.suggested_change));
		output.SetValue(col++, count, f.has_amount ? Value::DOUBLE(f.amount) : Value());
		output.SetValue(col++, count, f.has_total ? Value::BIGINT(f.total) : Value());
		output.SetValue(col++, count, text(f.scope));
		output.SetValue(col++, count, text(f.edit_source));
		output.SetValue(col++, count, text(f.group));
		output.SetValue(col, count, f.has_row ? Value::BIGINT(f.row) : Value());
		count++;
	}
	output.SetCardinality(count);
}

void StashDecideDiagnostic(ClientContext &context, DecideDiagnostic diag) {
	auto state =
	    context.registered_state->GetOrCreate<DecideDiagnosticState>(DECIDE_DIAGNOSTIC_STATE_KEY);
	state->latest = std::move(diag);
}

void ClearDecideDiagnostic(ClientContext &context) {
	// Only clear if the handoff exists — a solve that never diagnosed anything has nothing
	// to invalidate, and no reason to create the state.
	auto state = context.registered_state->Get<DecideDiagnosticState>(DECIDE_DIAGNOSTIC_STATE_KEY);
	if (state) {
		state->latest = DecideDiagnostic();
	}
}

DecideDiagnostic TakeDecideDiagnostic(ClientContext &context) {
	auto state = context.registered_state->Get<DecideDiagnosticState>(DECIDE_DIAGNOSTIC_STATE_KEY);
	if (!state) {
		return DecideDiagnostic();
	}
	// Consume it: the handoff exists only to cross from the DECIDE operator to the
	// DIAGNOSE operator above it within one statement, so nothing may read it twice.
	DecideDiagnostic taken = std::move(state->latest);
	state->latest = DecideDiagnostic();
	return taken;
}

} // namespace duckdb
