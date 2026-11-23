// The browser entry point of DeciDB: one in-memory database, one connection, and a query function that returns the
// result as JSON (columns, types, rows as text) or the error message.
#include "duckdb.hpp"

#include <cstdio>
#include <emscripten.h>

using namespace duckdb;

namespace {

unique_ptr<DuckDB> db;
unique_ptr<Connection> con;
string out;

void Quote(string &target, const string &text) {
	target += '"';
	for (unsigned char c : text) {
		switch (c) {
		case '"':
			target += "\\\"";
			break;
		case '\\':
			target += "\\\\";
			break;
		case '\n':
			target += "\\n";
			break;
		case '\r':
			target += "\\r";
			break;
		case '\t':
			target += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				target += buf;
			} else {
				target += char(c);
			}
		}
	}
	target += '"';
}

void Error(const string &message) {
	out = "{\"ok\":false,\"error\":";
	Quote(out, message);
	out += "}";
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char *decidb_open() {
	try {
		DBConfig config;
		config.options.maximum_threads = 1;
		db = make_uniq<DuckDB>(nullptr, &config);
		con = make_uniq<Connection>(*db);
		auto version = con->Query("SELECT library_version FROM pragma_version()");
		out = "{\"ok\":true,\"version\":";
		Quote(out, version->HasError() ? string("unknown") : version->GetValue(0, 0).ToString());
		out += "}";
	} catch (std::exception &e) {
		Error(e.what());
	}
	return out.c_str();
}

// Runs every statement in sql and returns the last statement's result.
EMSCRIPTEN_KEEPALIVE const char *decidb_query(const char *sql) {
	if (!con) {
		Error("The database is not open.");
		return out.c_str();
	}
	try {
		auto result = con->Query(sql);
		QueryResult *last = result.get();
		for (QueryResult *r = result.get(); r; r = r->next.get()) {
			if (r->HasError()) {
				Error(r->GetError());
				return out.c_str();
			}
			last = r;
		}
		auto &rows = last->Cast<MaterializedQueryResult>();
		out = "{\"ok\":true,\"columns\":[";
		for (idx_t c = 0; c < rows.names.size(); c++) {
			if (c) {
				out += ',';
			}
			Quote(out, rows.names[c]);
		}
		out += "],\"types\":[";
		for (idx_t c = 0; c < rows.types.size(); c++) {
			if (c) {
				out += ',';
			}
			Quote(out, rows.types[c].ToString());
		}
		out += "],\"rows\":[";
		for (idx_t r = 0; r < rows.RowCount(); r++) {
			out += r ? ",[" : "[";
			for (idx_t c = 0; c < rows.names.size(); c++) {
				if (c) {
					out += ',';
				}
				auto value = rows.GetValue(c, r);
				if (value.IsNull()) {
					out += "null";
				} else {
					Quote(out, value.ToString());
				}
			}
			out += ']';
		}
		out += "]}";
	} catch (std::exception &e) {
		Error(ErrorData(e).Message());
	}
	return out.c_str();
}
}
