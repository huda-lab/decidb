//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/function_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"

namespace duckdb {

//! A function in the catalog
class FunctionEntry : public StandardEntry {
public:
	FunctionEntry(CatalogType type, Catalog &catalog, SchemaCatalogEntry &schema, CreateFunctionInfo &info)
	    : StandardEntry(type, schema, catalog, info.name) {
		descriptions = std::move(info.descriptions);
		primary_name = std::move(info.primary_name);
		this->dependencies = info.dependencies;
		this->internal = info.internal;
	}

	vector<FunctionDescription> descriptions;
	//! The name of the function this one is an alias of, empty for a primary.
	string primary_name;
};
} // namespace duckdb
