
//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb_python/import_cache/modules/duckdb_module.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_python/import_cache/python_import_cache_item.hpp"

namespace duckdb {

struct DuckdbFilesystemCacheItem : public PythonImportCacheItem {

public:
	static constexpr const char *Name = "decidb.filesystem";

public:
	DuckdbFilesystemCacheItem()
	    : PythonImportCacheItem("decidb.filesystem"), ModifiedMemoryFileSystem("ModifiedMemoryFileSystem", this) {
	}
	~DuckdbFilesystemCacheItem() override {
	}

	PythonImportCacheItem ModifiedMemoryFileSystem;

protected:
	bool IsRequired() const override final {
		return false;
	}
};

struct DuckdbCacheItem : public PythonImportCacheItem {

public:
	static constexpr const char *Name = "decidb";

public:
	DuckdbCacheItem() : PythonImportCacheItem("decidb"), filesystem(), Value("Value", this) {
	}
	~DuckdbCacheItem() override {
	}

	DuckdbFilesystemCacheItem filesystem;
	PythonImportCacheItem Value;
};

} // namespace duckdb
