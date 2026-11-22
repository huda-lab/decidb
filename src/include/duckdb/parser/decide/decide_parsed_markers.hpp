//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/decide/decide_parsed_markers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! What the parser writes for a DECIDE clause, read through the functions here: a scope in front of a body
//! (`when c:`, `per K:`, `per row:`), a reducer's `by (K)`, a reducer's relation prefix (`sum(D, T: e)`), a
//! declaration and the list of constraints. No other code reads how the parser spells them.

//! How a `per` is written.
enum class DecidePer : uint8_t {
	//! No `per`
	NONE,
	//! `per row`, the default written out
	ROW,
	//! `per ()`: one for the whole query
	ALL,
	//! `per K`: one per distinct key
	KEY
};

//! The type a declaration gives its decision.
enum class DecideVariableType : uint8_t { REAL, INT, BOOL };

//! The elements of a key as written: a `per` key, a `by` key, a relation prefix.
class DecideKeyView {
public:
	DecideKeyView() : first(nullptr), count(0) {
	}
	DecideKeyView(unique_ptr<ParsedExpression> *first, idx_t count) : first(first), count(count) {
	}

	idx_t size() const { // NOLINT: the container's name
		return count;
	}
	bool empty() const { // NOLINT: the container's name
		return count == 0;
	}
	unique_ptr<ParsedExpression> &operator[](idx_t i) const {
		return first[i];
	}
	unique_ptr<ParsedExpression> *begin() const { // NOLINT: the container's name
		return first;
	}
	unique_ptr<ParsedExpression> *end() const { // NOLINT: the container's name
		return first + count;
	}
	//! The elements as SQL, comma-separated.
	string ToString() const;

private:
	unique_ptr<ParsedExpression> *first;
	idx_t count;
};

//! `[when c] [per K]:` and the body it stands in front of.
struct DecideScopeView {
	//! The condition of `when c`; null when none is written
	unique_ptr<ParsedExpression> *when = nullptr;
	//! Where the `when` is written
	optional_idx when_location;
	DecidePer per = DecidePer::NONE;
	//! The elements of `per K`; empty for any other `per`
	DecideKeyView key;
	//! Where the `per` is written
	optional_idx per_location;
	//! What the scope stands in front of
	unique_ptr<ParsedExpression> *body = nullptr;

	//! `WHEN c PER K`, as written before the colon.
	string ToString() const;
};

//! `call by (K)`: the key is empty for `by ()`.
struct DecideByView {
	unique_ptr<ParsedExpression> *call = nullptr;
	DecideKeyView key;
};

//! `sum(D, T: e)`: the call without its prefix, and the relations named in front of the colon.
struct DecideRelationPrefixView {
	unique_ptr<ParsedExpression> *call = nullptr;
	DecideKeyView relations;
};

//! One declaration, `[per K:] name(TYPE)`.
struct DecideDeclarationView {
	string name;
	DecideVariableType type = DecideVariableType::INT;
	//! Its `per`; the scope's body is the name
	DecideScopeView scope;
};

//! A scope written in front of a body: `when c:`, `per K:` or `per row:`.
bool IsDecideScope(const ParsedExpression &expr);
//! The parts of the scope `expr` (IsDecideScope).
DecideScopeView ReadDecideScope(const ParsedExpression &expr);

//! A call with a scope in front of its first argument, `sum(when c per K: e)`. The scope is that argument
//! (ReadDecideScope).
bool IsDecideScopedCall(const ParsedExpression &expr);

//! `call by (K)`.
bool IsDecideBy(const ParsedExpression &expr);
//! The parts of `expr` (IsDecideBy).
DecideByView ReadDecideBy(const ParsedExpression &expr);

//! `sum(D, T: e)`.
bool IsDecideRelationPrefix(const ParsedExpression &expr);
//! The parts of `expr` (IsDecideRelationPrefix).
DecideRelationPrefixView ReadDecideRelationPrefix(const ParsedExpression &expr);

//! The parts of one declaration of a DECIDE.
DecideDeclarationView ReadDecideDeclaration(const ParsedExpression &declaration);

//! The constraints of SUCH THAT, one entry each, in the order written.
vector<reference<unique_ptr<ParsedExpression>>> DecideConstraintList(unique_ptr<ParsedExpression> &constraints);

//! SUM, AVG, MIN or MAX: the reducers a scope may stand inside.
bool IsDecideAggregateName(const string &name);

//! A call to SUM, AVG, MIN or MAX.
bool IsScopedReducerCall(const ParsedExpression &expr);

//! A call a `by` may follow: a reducer, `norm` (lowered onto one, which keeps the key) or
//! `count` (D34, D35).
bool TakesReducerBy(const ParsedExpression &expr);

//! The call under a reducer's `by` and relation prefix.
const ParsedExpression &UnwrapReducerMarkers(const ParsedExpression &expr);

//! `text` without the one pair of parentheses that encloses all of it, else `text` itself.
string WithoutEnclosingParentheses(const string &text);

} // namespace duckdb
