#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"

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
