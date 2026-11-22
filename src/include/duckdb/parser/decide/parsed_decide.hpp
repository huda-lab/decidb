//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/decide/parsed_decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! The elements of a `per` or `by` key as SQL, comma-separated.
string DecideKeyToString(const vector<unique_ptr<ParsedExpression>> &key);

//! `[when c] [per K]`, written in front of a constraint, the objective, a declaration or a reducer's argument.
struct DecideScope {
	//! The condition of `when c`; null when none is written
	unique_ptr<ParsedExpression> when;
	DecidePer per = DecidePer::NONE;
	//! The elements of `per K`; empty for any other `per`
	vector<unique_ptr<ParsedExpression>> key;
	//! Where the scope is written: its `per`, else its `when`
	optional_idx location;

	bool IsEmpty() const {
		return !when && per == DecidePer::NONE;
	}
	//! `WHEN c PER K`, as written before the colon.
	string ToString() const;
	DecideScope Copy() const;
	bool Equals(const DecideScope &other) const;
	void EnumerateExpressions(const std::function<void(unique_ptr<ParsedExpression> &child)> &callback);

	void Serialize(Serializer &serializer) const;
	static DecideScope Deserialize(Deserializer &deserializer);
};

//! One declaration, `[per K:] name(TYPE)`.
struct DecideDeclaration {
	string name;
	DecideVariableType type = DecideVariableType::INT;
	//! Its `per`; a declaration takes no `when`
	DecideScope scope;
	optional_idx location;

	string ToString() const;
	DecideDeclaration Copy() const;
	bool Equals(const DecideDeclaration &other) const;

	void Serialize(Serializer &serializer) const;
	static DecideDeclaration Deserialize(Deserializer &deserializer);
};

//! One constraint of SUCH THAT, or the objective: `[scope:] body`.
struct DecideClause {
	DecideScope scope;
	unique_ptr<ParsedExpression> body;

	string ToString() const;
	DecideClause Copy() const;
	bool Equals(const DecideClause &other) const;

	void Serialize(Serializer &serializer) const;
	static DecideClause Deserialize(Deserializer &deserializer);
};

//! The DECIDE part of a SELECT: `DECIDE variables SUCH THAT constraints [MAXIMIZE | MINIMIZE objective]`.
struct ParsedDecide {
	vector<DecideDeclaration> variables;
	vector<DecideClause> constraints;
	//! The body is null when no objective is written
	DecideClause objective;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! The text of the DECIDE tail as the user wrote it, with the spans the binders cut from it
	DecideSourceText text;

	//! From `DECIDE` through the objective.
	string ToString() const;
	unique_ptr<ParsedDecide> Copy() const;
	bool Equals(const ParsedDecide &other) const;
	//! Every expression of the DECIDE part: the keys of the declarations, and each clause's scope and body.
	void EnumerateExpressions(const std::function<void(unique_ptr<ParsedExpression> &child)> &callback);

	void Serialize(Serializer &serializer) const;
	static unique_ptr<ParsedDecide> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
