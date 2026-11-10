//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_function_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/function/function.hpp"

namespace duckdb {

struct FunctionDescription {
	//! Parameter types (if any)
	vector<LogicalType> parameter_types;
	//! Parameter names (if any)
	vector<string> parameter_names;
	//! The description (if any)
	string description;
	//! Examples (if any)
	vector<string> examples;
};

struct CreateFunctionInfo : public CreateInfo {
	explicit CreateFunctionInfo(CatalogType type, string schema = DEFAULT_SCHEMA);

	//! Function name
	string name;
	//! The name of the function this one is an alias of, empty for a primary. An alias is registered with its
	//! primary's implementation, so the two answer alike.
	string primary_name;
	//! Function description
	vector<FunctionDescription> descriptions;

	DUCKDB_API void CopyFunctionProperties(CreateFunctionInfo &other) const;
};

} // namespace duckdb
