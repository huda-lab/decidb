//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/parser/decide/decide_declaration.hpp
//
// The parsed representation of a DECIDE declarator and of one objective stage.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! One DECIDE declarator as written: `[PER scope:] name(domain) [bounds]`.
//!
//! The parser retains structure only. `scope_key` holds the key's elements as
//! ColumnRefExpressions -- a one-field ref may name a column or a relation, which the
//! binder resolves (spec §6.1); it is empty unless `scope_kind == KEY`. The bounds are
//! ordinary expressions over columns in scope, bound as constraints later.
struct DecideDeclaration {
	string name;
	DecideScopeKind scope_kind = DecideScopeKind::ROW;
	vector<unique_ptr<ParsedExpression>> scope_key;
	DecideDomain domain = DecideDomain::INT;
	//! `TEXT in ['a', 'b']`: the admissible strings, in written order.
	vector<string> text_values;
	unique_ptr<ParsedExpression> lower_bound;
	unique_ptr<ParsedExpression> upper_bound;

	DecideDeclaration() = default;
	DecideDeclaration(DecideDeclaration &&) = default;
	DecideDeclaration &operator=(DecideDeclaration &&) = default;

	DecideDeclaration Copy() const;
	bool Equals(const DecideDeclaration &other) const;
	//! Renders the declarator in the DECIDE grammar's own spelling.
	string ToString() const;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideDeclaration Deserialize(Deserializer &deserializer);
};

//! One objective stage: `MAXIMIZE e` or `MINIMIZE e`. A DECIDE clause holds a
//! lexicographic list of these, first stage first; an empty list is a feasibility
//! problem (`SATISFY`, or no objective written).
struct DecideObjectiveClause {
	DecideSense sense = DecideSense::MINIMIZE;
	unique_ptr<ParsedExpression> expression;

	DecideObjectiveClause() = default;
	DecideObjectiveClause(DecideSense sense_p, unique_ptr<ParsedExpression> expression_p)
	    : sense(sense_p), expression(std::move(expression_p)) {
	}
	DecideObjectiveClause(DecideObjectiveClause &&) = default;
	DecideObjectiveClause &operator=(DecideObjectiveClause &&) = default;

	DecideObjectiveClause Copy() const;
	bool Equals(const DecideObjectiveClause &other) const;
	string ToString() const;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideObjectiveClause Deserialize(Deserializer &deserializer);
};

//! The spelling of a domain, `INT` / `REAL` / `BOOL` / `SEMIREAL` / `SEMIINT` / `TEXT`.
const char *DecideDomainName(DecideDomain domain);

//! Renders a scope in the grammar's spelling: `PER (): ` for GLOBAL, `PER a, T.b: ` for
//! a key, and nothing for ROW.
string DecideScopePrefixToString(DecideScopeKind kind, const vector<unique_ptr<ParsedExpression>> &key);

} // namespace duckdb
