#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

using namespace duckdb;

namespace {

//! The DECIDE node inside an extracted plan, wherever the projection put it.
LogicalDecide *FindDecide(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE) {
		return &op.Cast<LogicalDecide>();
	}
	for (auto &child : op.children) {
		if (auto *found = FindDecide(*child)) {
			return found;
		}
	}
	return nullptr;
}

//! A bound (unoptimized) plan for `query`, plus a copy of it made by round-tripping
//! through serialization. `LogicalOperator::Copy` is the round trip: it serializes to
//! a stream and deserializes back, so anything the wire format drops is missing from
//! the copy. Extraction runs the optimizer unless it is switched off, and stage 05's
//! output is deliberately not serializable, so it must be off here.
void SetUp(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers TO 'decide_optimizer'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE region(r_key INTEGER, r_name VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO region VALUES (1, 'east'), (2, 'west')"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE site(s_key INTEGER, r_key INTEGER, cap INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO site VALUES (10, 1, 40), (11, 1, 25), (12, 2, 60)"));
}

//! The DIAGNOSE wrapper inside a plan, wherever the projection put it.
LogicalOperator *FindDiagnose(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DECIDE_DIAGNOSE) {
		return &op;
	}
	for (auto &child : op.children) {
		if (auto *found = FindDiagnose(*child)) {
			return found;
		}
	}
	return nullptr;
}

//! `op` copied by round-tripping through serialization.
//!
//! Deserializing an expression tree resolves catalog references, so it needs a live
//! transaction; `ExtractPlan` runs in one of its own and closes it behind itself.
duckdb::unique_ptr<LogicalOperator> RoundTrip(Connection &con, LogicalOperator &op) {
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	auto copied = op.Copy(*con.context);
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	return copied;
}

//! Parse exactly one SELECT-shaped statement. DIAGNOSE is represented as a SELECT
//! over ShowRef too, so it intentionally belongs here.
duckdb::unique_ptr<SelectStatement> ParseSelect(const string &sql) {
	Parser parser;
	parser.ParseQuery(sql);
	REQUIRE(parser.statements.size() == 1);
	REQUIRE(parser.statements[0]->type == StatementType::SELECT_STATEMENT);
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
}

//! Parsed-statement serialization is separate from bound logical-plan serialization.
//! Exercise its generated wire format directly so a lost parser-only DECIDE tag cannot
//! hide behind ParsedExpression::Equals (which does not consider every display flag).
duckdb::unique_ptr<SelectStatement> RoundTripStatement(const SelectStatement &statement) {
	Allocator allocator;
	MemoryStream stream(allocator);
	BinarySerializer::Serialize(statement, stream);
	stream.Rewind();
	return BinaryDeserializer::Deserialize<SelectStatement>(stream);
}

} // namespace

TEST_CASE("Parsed DECIDE statements survive ToString and binary round trips", "[decidb]") {
	const duckdb::vector<string> queries = {
	    // All public type markers, including several declarations.
	    "SELECT a, x, y, flag FROM t "
	    "DECIDE x(INT), y(REAL), flag(BOOL) "
	    "SUCH THAT x >= 0 AND y <= 2.5 AND flag IN (0, 1)",
	    // Table and scalar scopes, and the split clause order. ToString canonicalizes
	    // both accepted orders to the single block after FROM.
	    "SELECT ship, cap DECIDE D.ship(BOOL), scalar cap(REAL) FROM data D "
	    "SUCH THAT ship <= cap AND cap >= 0 MINIMIZE cap - SUM(ship)",
	    // Whole-constraint WHEN plus multi-column PER.
	    "SELECT a, b, x FROM t DECIDE x(INT) "
	    "SUCH THAT SUM(x) <= 2 WHEN (a > 0) PER (a, t.b) AND x BETWEEN 0 AND 2",
	    // Aggregate-local WHEN and one- and many-relation qualified reducers.
	    "SELECT a, b, x FROM data D CROSS JOIN other T DECIDE D.x(INT) "
	    "SUCH THAT SUM(D: x) WHEN (a > 0) + AVG(D, T: x) <= 10 "
	    "MAXIMIZE SUM(D: x) WHEN (b = 2)",
	    // Objective WHEN + PER, nested aggregates, and NORM.
	    "SELECT a, b, x FROM t DECIDE x(REAL) "
	    "SUCH THAT norm(x - a, 1) <= (SELECT 3) "
	    "MAXIMIZE MAX(SUM(x)) WHEN (a = 1) PER (a, b)",
	    // Quoted identifiers and the statement-level DIAGNOSE wrapper.
	    "DIAGNOSE SELECT \"from\", \"Choice\", \"limit\" FROM data AS \"select\" "
	    "DECIDE \"select\".\"Choice\"(BOOL), scalar \"limit\"(INT) "
	    "SUCH THAT \"Choice\" <= \"limit\" AND \"limit\" <= 1",
	};

	for (auto &query : queries) {
		INFO(query);
		auto original = ParseSelect(query);
		auto rendered = original->ToString();
		INFO(rendered);

		auto reparsed = ParseSelect(rendered);
		REQUIRE(original->Equals(*reparsed));
		// A canonical rendering is stable, not merely parseable once.
		REQUIRE(reparsed->ToString() == rendered);

		auto deserialized = RoundTripStatement(*original);
		REQUIRE(original->Equals(*deserialized));
		// This pins FunctionExpression::is_operator and every private DECIDE tag:
		// losing one changes the rendered SQL even where Equals remains permissive.
		REQUIRE(deserialized->ToString() == rendered);
	}
}

TEST_CASE("Parsed DECIDE scope spellings survive ToString and binary round trips", "[decidb]") {
	// Round trips as above; nothing here is bound, so the tables need not exist.
	const duckdb::vector<string> queries = {
	    // Keyed, whole-query and per-row declarations, the last leaving no marker.
	    "SELECT a FROM t DECIDE per t.a, t: x(INT), per (): cap(REAL), per row: y(BOOL) "
	    "SUCH THAT x <= cap AND y <= 1",
	    // Scopes in front of constraints: a bare condition, when + per, per (), and the
	    // next constraint unscoped.
	    "SELECT a FROM t DECIDE x(INT) "
	    "SUCH THAT when a > 0 and b IS NOT NULL per a, t.b: SUM(x) BY (a) <= 2 "
	    "AND per (): SUM(x) <= 9 AND when a = 1: x <= 1 AND x >= 0",
	    // Scopes inside reducers, BY with an expression, the colon form with BY, and a
	    // scoped objective.
	    "SELECT a FROM t DECIDE x(INT) "
	    "SUCH THAT SUM(when a > 0 per t: x) BY (a, b + 1) <= 2 AND AVG(D: x) BY () <= 3 "
	    "MAXIMIZE when a = 1 per (): SUM(x) BY ()",
	    // The split clause order, DIAGNOSE, and names that are keywords inside DECIDE.
	    "DIAGNOSE SELECT a DECIDE per \"per\": x(INT) FROM t "
	    "SUCH THAT per \"row\", \"by\": SUM(x) BY (\"per\") <= \"row\"",
	    // The deck examples 1-8 of the behaviour spec, as written and fully explicit.
	    "SELECT S.shipmentID FROM Shipment S JOIN Depot D USING (depotID) "
	    "DECIDE ship(INT), per D.depotID: reserve(REAL) "
	    "SUCH THAT ship <= 0.20 * sum(ship) "
	    "AND per (): sum(ship) <= networkCapacity "
	    "AND per D.depotID: reserve >= D.networkReserveShare * sum(S.demand) "
	    "AND per D.depotID: sum(ship) by (D.depotID) <= D.capacity "
	    "AND ship <= D.maxShipmentShare * sum(ship) by (D.depotID) "
	    "AND per D.depotID: reserve >= 0.10 * sum(S.demand) by (D.region) + 0.02 * sum(S.demand) by (D.country) "
	    "AND per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity "
	    "AND per S.shipmentID: ship <= 0.25 * sum(ship) by (D.depotID, S.dispatchDay + S.transitDays)",
	    "SELECT S.shipmentID FROM Shipment S JOIN Depot D USING (depotID) "
	    "DECIDE ship(INT), per D.depotID: reserve(REAL) "
	    "SUCH THAT per row: ship <= 0.20 * sum(per row: ship) by () "
	    "AND per (): sum(per row: ship) by () <= networkCapacity "
	    "AND per D.depotID: reserve >= D.networkReserveShare * sum(per row: S.demand) by () "
	    "AND per D.depotID: sum(per row: ship) by (D.depotID) <= D.capacity "
	    "AND per row: ship <= D.maxShipmentShare * sum(per row: ship) by (D.depotID) "
	    "AND per D.depotID: reserve >= 0.10 * sum(per row: S.demand) by (D.region) "
	    "+ 0.02 * sum(per row: S.demand) by (D.country) "
	    "AND per D.depotID: sum(when S.priority per row: ship) by (D.depotID) <= D.priorityCapacity "
	    "AND per S.shipmentID: ship <= 0.25 * sum(per row: ship) by (D.depotID, S.dispatchDay + S.transitDays)",
	};

	for (auto &query : queries) {
		INFO(query);
		auto original = ParseSelect(query);
		auto rendered = original->ToString();
		INFO(rendered);

		auto reparsed = ParseSelect(rendered);
		REQUIRE(original->Equals(*reparsed));
		REQUIRE(reparsed->ToString() == rendered);

		auto deserialized = RoundTripStatement(*original);
		REQUIRE(original->Equals(*deserialized));
		REQUIRE(deserialized->ToString() == rendered);
	}

	// A scope covers exactly one constraint: x >= 0 stays outside the WHEN.
	REQUIRE(ParseSelect("SELECT a FROM t DECIDE x(INT) SUCH THAT when a = 1: x <= 1 AND x >= 0")->ToString() ==
	        "SELECT a FROM t DECIDE x(INT) SUCH THAT WHEN (a = 1): (x <= 1) AND (x >= 0)");

	// PER ROW is the default written out: it parses to the same statement.
	auto spelled_out = ParseSelect("SELECT a FROM t DECIDE per row: x(INT) "
	                               "SUCH THAT per row: SUM(per row: x) <= 1 AND per row: x <= 1");
	auto implied = ParseSelect("SELECT a FROM t DECIDE x(INT) SUCH THAT SUM(x) <= 1 AND x <= 1");
	REQUIRE(spelled_out->Equals(*implied));
	REQUIRE(spelled_out->ToString() == implied->ToString());

	// The three exact parse errors, and the generic one. The exception text is JSON,
	// so a quoted token is matched on its own rather than inside the message.
	auto rejects = [](const string &sql, const string &message, const string &token = "") {
		INFO(sql);
		Parser parser;
		REQUIRE_THROWS_WITH(parser.ParseQuery(sql), Catch::Contains(message) && Catch::Contains(token));
	};
	const string once = "an objective is produced once: write per (): or leave per out";
	rejects("SELECT a FROM t DECIDE x(INT) SUCH THAT x <= 1 MAXIMIZE per a: SUM(x)", once);
	rejects("SELECT a FROM t DECIDE x(INT) SUCH THAT x <= 1 MINIMIZE when a = 1 per row: SUM(x)", once);
	const string parens = "write per a, b: without parentheses; parentheses are only for per ()";
	rejects("SELECT a FROM t DECIDE x(INT) SUCH THAT per (a, b): SUM(x) <= 1", parens);
	rejects("SELECT a FROM t DECIDE per (a): x(INT) SUCH THAT x <= 1", parens);
	rejects("SELECT a FROM t DECIDE x(INT) SUCH THAT SUM(per (a): x) <= 1", parens);
	const string names = "a per key lists columns or relations; put expressions in by (...)";
	rejects("SELECT a FROM t DECIDE x(INT) SUCH THAT per a + 1: SUM(x) <= 1", names);
	rejects("SELECT a FROM t DECIDE per lower(a): x(INT) SUCH THAT x <= 1", names);
	rejects("SELECT a FROM t DECIDE x(INT) SUCH THAT SUM(per 5: x) <= 1", names);
	// A scoped constraint takes no postfix modifier, and outside DECIDE nothing changed.
	rejects("SELECT a FROM t DECIDE x(INT) SUCH THAT per a: x <= 1 WHEN b", "syntax error at or near", "WHEN");
	rejects("SELECT SUM(x) BY (a) FROM t", "syntax error at or near", "BY");
	rejects("SELECT SUM(when a: x) FROM t", "syntax error at or near", "when");
}

TEST_CASE("Bound DECIDE plans survive a serialization round trip", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	SetUp(con);

	// `r_name` is referenced ONLY by the entity-scoped declaration. Its column ref
	// lives on entity_key_expressions purely so column pruning keeps it alive, so a
	// round trip that loses that vector loses the entity's identity.
	// `ship <= cap * open` is the one shape canonicalization has to move -- a bound that
	// CONTAINS a decision -- so it is the only thing that populates source_lhs/source_rhs.
	auto plan = con.ExtractPlan("SELECT s_key, ship FROM site JOIN region USING (r_key) "
	                            "DECIDE region.ship(INT), open(BOOL) "
	                            "SUCH THAT ship <= cap * open AND SUM(ship) <= 100 "
	                            "MAXIMIZE SUM(ship) + 5");
	auto *before = FindDecide(*plan);
	REQUIRE(before != nullptr);
	REQUIRE_FALSE(before->optimized);

	auto copied = RoundTrip(con, *plan);
	auto *after = FindDecide(*copied);
	REQUIRE(after != nullptr);

	// Entity scopes, including the key column types and bindings that the previous
	// hand-written serializer dropped entirely.
	REQUIRE(after->entity_scopes.size() == before->entity_scopes.size());
	REQUIRE(after->entity_scopes.size() == 1);
	REQUIRE(after->entity_scopes[0].table_alias == before->entity_scopes[0].table_alias);
	REQUIRE(after->entity_scopes[0].source_table_indices == before->entity_scopes[0].source_table_indices);
	REQUIRE(after->entity_scopes[0].entity_key_column_types.size() ==
	        before->entity_scopes[0].entity_key_column_types.size());
	REQUIRE(after->entity_scopes[0].entity_key_bindings.size() == before->entity_scopes[0].entity_key_bindings.size());
	REQUIRE(after->entity_scopes[0].scoped_variable_indices == before->entity_scopes[0].scoped_variable_indices);
	REQUIRE(after->entity_key_expressions.size() == before->entity_key_expressions.size());
	REQUIRE(!after->entity_key_expressions.empty());

	// Per-variable scope assignment.
	REQUIRE(after->variable_scopes.size() == before->variable_scopes.size());
	for (idx_t i = 0; i < before->variable_scopes.size(); i++) {
		REQUIRE(after->variable_scopes[i].scope == before->variable_scopes[i].scope);
		REQUIRE(after->variable_scopes[i].entity_scope_idx == before->variable_scopes[i].entity_scope_idx);
	}

	// Display provenance, including the source_lhs/source_rhs pair the old serializer
	// never wrote at all.
	REQUIRE(after->constraint_sources.size() == before->constraint_sources.size());
	REQUIRE(!after->constraint_sources.empty());
	bool saw_written_form = false;
	for (idx_t i = 0; i < before->constraint_sources.size(); i++) {
		auto &b = before->constraint_sources[i];
		auto &a = after->constraint_sources[i];
		REQUIRE(a.canonical_lhs == b.canonical_lhs);
		REQUIRE(a.canonical_rhs == b.canonical_rhs);
		REQUIRE(a.canonical_cmp == b.canonical_cmp);
		REQUIRE(a.qualifier == b.qualifier);
		REQUIRE(a.rhs_kind == b.rhs_kind);
		REQUIRE(a.source_lhs == b.source_lhs);
		REQUIRE(a.source_rhs == b.source_rhs);
		REQUIRE(a.written_lhs == b.written_lhs);
		REQUIRE(a.written_rhs == b.written_rhs);
		REQUIRE(a.written_cmp == b.written_cmp);
		saw_written_form = saw_written_form || !b.source_rhs.empty();
	}
	REQUIRE(saw_written_form);

	REQUIRE(after->written_objective == before->written_objective);
	REQUIRE(after->canonical_objective == before->canonical_objective);
	REQUIRE(after->source_fragments == before->source_fragments);
	REQUIRE(after->objective_constant_offset == before->objective_constant_offset);
	REQUIRE(after->decide_sense == before->decide_sense);
	REQUIRE(after->is_boolean_var == before->is_boolean_var);
	REQUIRE(after->num_auxiliary_vars == before->num_auxiliary_vars);
	REQUIRE(after->decide_variables.size() == before->decide_variables.size());
	REQUIRE(after->decide_constraints != nullptr);
	REQUIRE(after->decide_objective != nullptr);
}

TEST_CASE("Keyed declarations share one scope per key and survive a serialization round trip", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	SetUp(con);

	// a and b write one key two ways; d's old table scope comes first, then c's relation
	// key with the same columns; e is query-wide and f is per row.
	auto plan = con.ExtractPlan("SELECT s_key FROM site JOIN region USING (r_key) "
	                            "DECIDE per region.r_name, site.cap: a(INT), per site.cap, r_name, r_name: b(INT), "
	                            "region.d(INT), per region: c(INT), per (): e(INT), f(INT) "
	                            "SUCH THAT a + b + c + d + e + f <= 10 MAXIMIZE SUM(a)");
	auto *before = FindDecide(*plan);
	REQUIRE(before != nullptr);

	auto &scopes = before->entity_scopes;
	auto &vars = before->variable_scopes;
	REQUIRE(scopes.size() == 3);
	REQUIRE(vars.size() == 6);
	// Equal keys share one scope, however they are written.
	REQUIRE(vars[0].IsKeyed());
	REQUIRE(vars[0].declared_key == "region.r_name, site.cap");
	REQUIRE(vars[1].declared_key == "site.cap, r_name, r_name");
	REQUIRE(vars[0].entity_scope_idx == vars[1].entity_scope_idx);
	auto &shared = scopes[vars[0].entity_scope_idx];
	REQUIRE(shared.exact_key);
	REQUIRE(shared.entity_key_bindings.size() == 2);
	REQUIRE(shared.source_table_indices.size() == 2);
	// A per key never reuses a T.x scope on the same columns: only the old table scope
	// drops the columns its clause reads as data.
	REQUIRE_FALSE(vars[2].IsKeyed());
	REQUIRE(vars[2].IsEntity());
	REQUIRE(vars[3].IsKeyed());
	REQUIRE(vars[2].entity_scope_idx != vars[3].entity_scope_idx);
	REQUIRE_FALSE(scopes[vars[2].entity_scope_idx].exact_key);
	REQUIRE(scopes[vars[3].entity_scope_idx].exact_key);
	REQUIRE(scopes[vars[2].entity_scope_idx].entity_key_bindings ==
	        scopes[vars[3].entity_scope_idx].entity_key_bindings);
	REQUIRE(vars[4].IsScalar());
	REQUIRE(vars[5].scope == DecideVarScope::ROW);

	auto copied = RoundTrip(con, *plan);
	auto *after = FindDecide(*copied);
	REQUIRE(after != nullptr);
	REQUIRE(after->entity_scopes.size() == scopes.size());
	for (idx_t i = 0; i < scopes.size(); i++) {
		REQUIRE(after->entity_scopes[i].exact_key == scopes[i].exact_key);
		REQUIRE(after->entity_scopes[i].table_alias == scopes[i].table_alias);
		REQUIRE(after->entity_scopes[i].entity_key_bindings == scopes[i].entity_key_bindings);
		REQUIRE(after->entity_scopes[i].source_table_indices == scopes[i].source_table_indices);
	}
	REQUIRE(after->variable_scopes.size() == vars.size());
	for (idx_t i = 0; i < vars.size(); i++) {
		REQUIRE(after->variable_scopes[i].scope == vars[i].scope);
		REQUIRE(after->variable_scopes[i].entity_scope_idx == vars[i].entity_scope_idx);
		REQUIRE(after->variable_scopes[i].declared_key == vars[i].declared_key);
	}
	REQUIRE(after->entity_key_expressions.size() == before->entity_key_expressions.size());
}

TEST_CASE("DIAGNOSE survives a serialization round trip", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	SetUp(con);

	// The flag arms the diagnosis engines and nothing else reads it back out of a
	// session setting, so a round trip that drops it turns DIAGNOSE into a plain solve.
	auto plan = con.ExtractPlan("DIAGNOSE SELECT s_key, ship FROM site "
	                            "DECIDE ship(INT) SUCH THAT SUM(ship) <= 5 AND SUM(ship) >= 9 "
	                            "MAXIMIZE SUM(ship)");
	auto *before = FindDecide(*plan);
	REQUIRE(before != nullptr);
	REQUIRE(before->diagnose);

	auto copied = RoundTrip(con, *plan);
	auto *after = FindDecide(*copied);
	REQUIRE(after != nullptr);
	REQUIRE(after->diagnose);
	// The DIAGNOSE wrapper node has its own serialization, which it had none of before.
	auto *diagnose_before = FindDiagnose(*plan);
	auto *diagnose_after = FindDiagnose(*copied);
	REQUIRE(diagnose_before != nullptr);
	REQUIRE(diagnose_after != nullptr);
	REQUIRE(diagnose_after->GetTableIndex() == diagnose_before->GetTableIndex());
}

TEST_CASE("A user's own column names survive a serialization round trip", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	SetUp(con);

	// An alias list over VALUES is the case with no catalog entry to fall back on:
	// the plan's projection carries the binder's positional placeholders, and
	// source_columns is the only record of what the user actually wrote.
	auto plan = con.ExtractPlan("SELECT lbl, pick FROM (VALUES ('a', 3), ('b', 4)) t(lbl, wt) "
	                            "DECIDE pick(BOOL) SUCH THAT SUM(wt * pick) <= 5 "
	                            "MAXIMIZE SUM(pick)");
	auto *before = FindDecide(*plan);
	REQUIRE(before != nullptr);
	REQUIRE(!before->source_columns.empty());

	auto copied = RoundTrip(con, *plan);
	auto *after = FindDecide(*copied);
	REQUIRE(after != nullptr);
	REQUIRE(after->source_columns.size() == before->source_columns.size());
	bool saw_alias = false;
	for (idx_t i = 0; i < before->source_columns.size(); i++) {
		REQUIRE(after->source_columns[i].binding == before->source_columns[i].binding);
		REQUIRE(after->source_columns[i].name == before->source_columns[i].name);
		saw_alias = saw_alias || after->source_columns[i].name == "lbl";
	}
	// The alias list is the point: `lbl` is written nowhere else in the plan.
	REQUIRE(saw_alias);
}

TEST_CASE("An optimized DECIDE plan refuses to be copied", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE site(s_key INTEGER, cap INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO site VALUES (10, 40), (11, 25)"));

	// With the DECIDE optimizer left on, the plan carries a formulation chosen from
	// this host's solver -- the prepared linear form, the absorbed variable box, the
	// composed MIN/MAX terms -- none of which is serialized. Copying it would hand
	// back a plan missing all of it, so it raises instead.
	auto plan = con.ExtractPlan("SELECT s_key, ship FROM site "
	                            "DECIDE ship(INT) SUCH THAT SUM(ship) <= 50 MAXIMIZE SUM(ship)");
	auto *decide = FindDecide(*plan);
	REQUIRE(decide != nullptr);
	REQUIRE(decide->optimized);
	REQUIRE_THROWS_AS(plan->Copy(*con.context), NotImplementedException);
}
