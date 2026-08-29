//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/verification/parsed_statement_verifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/verification/statement_verifier.hpp"

namespace duckdb {

class ParsedStatementVerifier : public StatementVerifier {
public:
	explicit ParsedStatementVerifier(unique_ptr<SQLStatement> statement_p,
	                                 optional_ptr<case_insensitive_map_t<BoundParameterData>> parameters,
	                                 bool contains_decide);
	static unique_ptr<StatementVerifier> Create(const SQLStatement &statement,
	                                            optional_ptr<case_insensitive_map_t<BoundParameterData>> parameters);

	bool RequireEquality() const override {
		return false;
	}

	bool ShouldExecute() const override {
		// Creating this verifier already proves ToString() reparses. Do not solve a
		// DECIDE query again merely to compare rows: equally optimal assignments and
		// interactive continuation are intentionally not deterministic that way.
		return !contains_decide;
	}

private:
	bool contains_decide;
};

} // namespace duckdb
