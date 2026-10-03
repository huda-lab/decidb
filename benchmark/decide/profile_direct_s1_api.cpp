// S1 API timing for materialized and streamed query results.
// Build from the repository root with:
// clang++ -std=c++17 -O2 -I src/include benchmark/decide/profile_direct_s1_api.cpp \
//   -L build/release/src -lduckdb -Wl,-rpath,$PWD/build/release/src \
//   -o benchmark/decide/results/profile_direct_s1_api

#include "duckdb.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace duckdb;

namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point start, Clock::time_point end) {
	return std::chrono::duration<double>(end - start).count();
}

struct MemorySnapshot {
	uint64_t resident_bytes = 0;
	uint64_t process_peak_bytes = 0;
};

MemorySnapshot ReadMemory() {
	MemorySnapshot snapshot;
#if defined(__APPLE__)
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		snapshot.resident_bytes = info.resident_size;
	}
#endif
#ifndef _WIN32
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
		snapshot.process_peak_bytes = static_cast<uint64_t>(usage.ru_maxrss);
#else
		snapshot.process_peak_bytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
	}
#endif
	return snapshot;
}

double MiB(uint64_t bytes) {
	return static_cast<double>(bytes) / (1024 * 1024);
}

void Run(Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "No query result");
	}
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 5 || argc > 12) {
		std::cerr << "usage: profile_direct_s1_api ROWS WIDTH stored|stored_join direct|gurobi|highs|prepare "
		             "[full|aggregate|consume] [materialized|stream] [upper|lower|exact|interval] "
		             "[global|grouped|grouped_local_when] "
		             "[free|fixed] [constant|source|source_double|source_pair|source_coalesce] "
		             "[single|additive]\n";
		return 2;
	}
	try {
		auto rows = std::stoull(argv[1]);
		auto width = std::stoull(argv[2]);
		std::string source_kind(argv[3]);
		std::string mode(argv[4]);
		std::string output_kind(argc >= 6 ? argv[5] : "full");
		std::string delivery(argc >= 7 ? argv[6] : "materialized");
		std::string cardinality(argc >= 8 ? argv[7] : "upper");
		std::string scope(argc >= 9 ? argv[8] : "global");
		std::string pins(argc >= 10 ? argv[9] : "free");
		std::string bound_kind(argc >= 11 ? argv[10] : "constant");
		std::string objective_kind(argc == 12 ? argv[11] : "single");
		if (!rows || (source_kind != "stored" && source_kind != "stored_join") ||
		    (mode != "direct" && mode != "gurobi" && mode != "highs" && mode != "prepare") ||
		    (output_kind != "full" && output_kind != "aggregate" && output_kind != "consume") ||
		    (delivery != "materialized" && delivery != "stream") ||
		    (cardinality != "upper" && cardinality != "lower" && cardinality != "exact" && cardinality != "interval") ||
		    (scope != "global" && scope != "grouped" && scope != "grouped_local_when") ||
		    (scope != "global" && rows < 100) ||
		    (pins != "free" && pins != "fixed") ||
		    (bound_kind != "constant" && bound_kind != "source" && bound_kind != "source_double" &&
		     bound_kind != "source_pair" && bound_kind != "source_coalesce") ||
		    (bound_kind != "constant" && cardinality != "interval") ||
		    (objective_kind != "single" && objective_kind != "additive")) {
			throw std::invalid_argument("Invalid rows, source kind, mode, output kind, delivery, cardinality, scope, or pins");
		}
		// With S1_DB set, the source lives in a database file built once by a "prepare" run. A measured run
		// then opens the file in a fresh process, so its peak memory covers only the query (plus the open).
		auto database_path = std::getenv("S1_DB");
		auto stored_file = database_path && *database_path;
		bool prepare = mode == "prepare";
		if (prepare && !stored_file) {
			throw std::invalid_argument("prepare mode needs S1_DB");
		}
		DuckDB database(stored_file ? database_path : nullptr);
		Connection connection(database);
		Run(connection, "SET threads=4");
		Run(connection, mode == "direct" ? "SET decide_direct_solve='require'" : "SET decide_direct_solve='off'");
		auto grouped = scope != "global";
		auto local_when = scope == "grouped_local_when";
		auto group_size = grouped ? rows / 100 : rows;
		auto lower = (group_size / 10) * 6;
		auto upper = (group_size / 10) * 7;
		std::string payload = width ? ", rpad(i::VARCHAR, " + std::to_string(width) + ", 'p') AS payload" : "";
		std::string group = grouped ? ", (i % 100)::INTEGER AS dept" : "";
		std::string active = local_when ? ", ((i // 100) % 10 != 0) AS active" : "";
		std::string bound_column;
		if (bound_kind != "constant") {
			auto fractional = bound_kind == "source_double" ? ".5" : "";
			auto type = bound_kind == "source_double" ? "DOUBLE" : "INTEGER";
			auto first_cap = bound_kind == "source_coalesce" ? "NULL" : std::to_string(upper) + fractional;
			bound_column = ", (CASE WHEN i % 1000 < 250 THEN " + first_cap + " WHEN i % 1000 < 500 THEN " +
			               std::to_string(upper) + fractional + " ELSE " + std::to_string(upper + 1) + fractional +
			               " END)::" + type + " AS cap";
			if (bound_kind == "source_pair") {
				bound_column += ", (CASE WHEN i % 1000 < 500 THEN " + std::to_string(lower) + " ELSE " +
				                std::to_string(lower ? lower - 1 : 0) + " END)::INTEGER AS lower_cap";
			}
		}
		std::string fixed_columns = pins == "fixed"
		                                ? ", (i % 10000 < 100) AS fixed_one, "
		                                  "(i % 10000 BETWEEN 100 AND 199) AS fixed_zero"
		                                : "";
		auto open_memory = ReadMemory();
		auto setup_start = Clock::now();
		auto create = stored_file ? "CREATE TABLE source AS SELECT i, " : "CREATE TEMP TABLE source AS SELECT i, ";
		if (!stored_file || prepare) {
			Run(connection, std::string(create) + "(((CAST(i AS BIGINT)*37)%10007)-5000)::DOUBLE / 10.0 AS score" +
			                    payload + group + active + bound_column + fixed_columns + " FROM range(" +
			                    std::to_string(rows) + ") t(i)");
		}
		std::string relation = "source";
		if (source_kind == "stored_join") {
			if (!stored_file || prepare) {
				Run(connection, std::string(stored_file ? "CREATE TABLE weights AS SELECT k, "
				                                         : "CREATE TEMP TABLE weights AS SELECT k, ") +
				                    "1.0 + (k % 5)::DOUBLE / 20.0 AS weight FROM range(97) t(k)");
			}
			relation = "(SELECT s.i, s.score * w.weight AS score" + std::string(width ? ", s.payload" : "") +
		           std::string(grouped ? ", s.dept" : "") + std::string(local_when ? ", s.active" : "") +
		           std::string(bound_kind != "constant" ? ", s.cap" : "") +
		           std::string(bound_kind == "source_pair" ? ", s.lower_cap" : "") +
		           std::string(pins == "fixed" ? ", s.fixed_one, s.fixed_zero" : "") +
		           " FROM source s JOIN weights w ON s.i % 97 = w.k) j";
		}
		if (prepare) {
			Run(connection, "CHECKPOINT");
			std::cout << "{\"prepared\":true,\"rows\":" << rows << ",\"width\":" << width
			          << ",\"setup_s\":" << Seconds(setup_start, Clock::now()) << "}\n";
			return 0;
		}
		auto setup_end = Clock::now();
		auto setup_memory = ReadMemory();
		std::string columns = std::string(width ? "i, score, payload" : "i, score") +
		                      (grouped ? ", dept" : "") + (local_when ? ", active" : "") + ", x";
		std::string constraint;
		if (cardinality == "upper") {
			constraint = "SUM(x)<=" + std::to_string(group_size / 10);
		} else if (cardinality == "lower") {
			constraint = "SUM(x)>=" + std::to_string(lower);
		} else if (cardinality == "exact") {
			constraint = "SUM(x)=" + std::to_string(lower);
		} else {
			constraint = "SUM(x)>=" + std::to_string(lower) + " AND SUM(x)<=" + std::to_string(upper);
		}
		if (grouped) {
			if (cardinality == "interval") {
				constraint = local_when ? "SUM(x) WHEN active >=" + std::to_string(lower) +
				                              " PER dept AND SUM(x) WHEN active <=" + std::to_string(upper) + " PER dept"
				                        : "SUM(x)>=" + std::to_string(lower) + " PER dept AND SUM(x)<=" +
				                              std::to_string(upper) + " PER dept";
			} else {
				constraint += " PER dept";
			}
		}
		if (bound_kind != "constant") {
			auto lower_bound = bound_kind == "source_pair" ? "lower_cap" : std::to_string(lower);
			auto upper_bound = bound_kind == "source_coalesce" ? "COALESCE(cap," + std::to_string(upper + 1) + ")"
			                                                    : "cap";
			constraint = local_when ? "SUM(x) WHEN active >=" + lower_bound + " PER dept AND SUM(x) WHEN active <=" +
			                              upper_bound + " PER dept"
			                        : grouped ? "SUM(x)>=" + lower_bound + " PER dept AND SUM(x)<=" + upper_bound +
			                                        " PER dept"
			                                  : "SUM(x)>=" + lower_bound + " AND SUM(x)<=" + upper_bound;
		}
		if (pins == "fixed") {
			constraint = "x=1 WHEN fixed_one AND x=0 WHEN fixed_zero AND " + constraint;
		}
		auto objective_sql = "SUM(score*x)" + std::string(objective_kind == "additive" ? "+SUM(i*x)" : "");
		auto decide = "FROM " + relation + " DECIDE x(BOOL) SUCH THAT " + constraint + " MAXIMIZE " + objective_sql;
		auto sql = output_kind == "full"
		               ? "SELECT " + columns + " FROM (" + decide + ") q"
		               : "SELECT COUNT(*)::BIGINT, SUM(x)::BIGINT, SUM((score" +
		                     std::string(objective_kind == "additive" ? "+i" : "") + ")*x)::DOUBLE" +
		                     std::string(local_when ? ", SUM(x) FILTER (WHERE active)::BIGINT" : "") +
		                     // "consume" reads every column the "full" output returns, but reduces them inside the
		                     // query, so no row-sized result collector runs. Its wall time minus the "full" wall
		                     // time estimates the cost of collecting the result.
		                     std::string(output_kind == "consume" ? ", SUM(i)::BIGINT" : "") +
		                     std::string(output_kind == "consume" && width ? ", SUM(length(payload))::BIGINT" : "") +
		                     std::string(output_kind == "consume" && grouped ? ", SUM(dept)::BIGINT" : "") +
		                     " FROM (" + decide + ") q";
		std::cerr << "DECIDB_PROFILE_QUERY_BEGIN\n" << std::flush;
		auto query_start = Clock::now();
		unique_ptr<QueryResult> result =
		    delivery == "stream" ? connection.SendQuery(sql) : connection.Query(sql);
		auto query_end = Clock::now();
		auto post_submit_memory = ReadMemory();
		if (delivery == "materialized") {
			std::cerr << "DECIDB_PROFILE_QUERY_END\n" << std::flush;
		}
		if (!result || result->HasError()) {
			throw std::runtime_error(result ? result->GetError() : "No DECIDE result");
		}
		if (delivery == "stream" && result->type != QueryResultType::STREAM_RESULT) {
			throw std::runtime_error("DECIDE query did not return a streaming result");
		}
		idx_t result_rows = 0;
		idx_t chosen = 0;
		idx_t active_chosen = 0;
		idx_t payload_bytes = 0;
		idx_t output_rows = 0;
		long double objective = 0;
		auto readback_start = Clock::now();
		while (auto chunk = result->Fetch()) {
			for (idx_t row = 0; row < chunk->size(); row++) {
				output_rows++;
				if (output_kind != "full") {
					result_rows = static_cast<idx_t>(chunk->GetValue(0, row).GetValue<int64_t>());
					chosen = static_cast<idx_t>(chunk->GetValue(1, row).GetValue<int64_t>());
					objective = chunk->GetValue(2, row).GetValue<double>();
					active_chosen = local_when ? static_cast<idx_t>(chunk->GetValue(3, row).GetValue<int64_t>()) : chosen;
					continue;
				}
				auto item_id = chunk->GetValue(0, row).GetValue<int64_t>();
				auto score = chunk->GetValue(1, row).GetValue<double>();
				if (width) {
					payload_bytes += chunk->GetValue(2, row).GetValue<std::string>().size();
				}
				auto x_slot = 2 + (width ? 1 : 0) + (grouped ? 1 : 0) + (local_when ? 1 : 0);
				auto x = chunk->GetValue(x_slot, row).GetValue<int32_t>();
				chosen += static_cast<idx_t>(x);
				if (!local_when || chunk->GetValue(x_slot - 1, row).GetValue<bool>()) {
					active_chosen += static_cast<idx_t>(x);
				}
				objective += (static_cast<long double>(score) +
				              (objective_kind == "additive" ? static_cast<long double>(item_id) : 0.0L)) * x;
				result_rows++;
			}
		}
		auto readback_end = Clock::now();
		auto post_fetch_memory = ReadMemory();
		if (delivery == "stream") {
			std::cerr << "DECIDB_PROFILE_QUERY_END\n" << std::flush;
		}
		if (result->HasError()) {
			throw std::runtime_error(result->GetError());
		}
		auto group_count = grouped ? 100 : 1;
		bool valid_cardinality = cardinality == "upper" ? active_chosen <= (group_size / 10) * group_count
		                         : cardinality == "interval"
		                             ? active_chosen >= lower * group_count && active_chosen <= upper * group_count
		                         : cardinality == "exact" ? active_chosen == lower * group_count
		                                                    : active_chosen >= lower * group_count;
		if (result_rows != rows || !valid_cardinality || output_rows != (output_kind == "full" ? rows : 1)) {
			throw std::runtime_error("Unexpected row count or infeasible assignment");
		}
		std::cout << std::setprecision(12) << "{\"rows\":" << rows << ",\"width\":" << width
		          << ",\"source_kind\":\"" << source_kind << "\",\"mode\":\"" << mode
		          << "\",\"output_kind\":\"" << output_kind << "\",\"delivery\":\"" << delivery
		          << "\",\"cardinality\":\"" << cardinality
		          << "\",\"scope\":\"" << scope << "\",\"pins\":\"" << pins
		          << "\",\"bound_kind\":\"" << bound_kind << "\",\"objective_kind\":\"" << objective_kind
		          << "\",\"setup_s\":" << Seconds(setup_start, setup_end)
		          << ",\"submit_s\":" << Seconds(query_start, query_end)
		          << ",\"fetch_s\":" << Seconds(readback_start, readback_end)
		          << ",\"stored_file\":" << (stored_file ? "true" : "false")
		          << ",\"open_rss_mib\":" << MiB(open_memory.resident_bytes)
		          << ",\"process_peak_through_open_mib\":" << MiB(open_memory.process_peak_bytes)
		          << ",\"setup_rss_mib\":" << MiB(setup_memory.resident_bytes)
		          << ",\"process_peak_through_setup_mib\":" << MiB(setup_memory.process_peak_bytes)
		          << ",\"process_peak_through_submit_mib\":" << MiB(post_submit_memory.process_peak_bytes)
		          << ",\"post_submit_rss_mib\":" << MiB(post_submit_memory.resident_bytes)
		          << ",\"process_peak_through_fetch_mib\":" << MiB(post_fetch_memory.process_peak_bytes)
		          << ",\"post_fetch_rss_mib\":" << MiB(post_fetch_memory.resident_bytes);
		if (delivery == "materialized") {
			std::cout << ",\"query_collect_s\":" << Seconds(query_start, query_end)
			          << ",\"api_readback_s\":" << Seconds(readback_start, readback_end)
			          << ",\"process_peak_through_query_mib\":" << MiB(post_submit_memory.process_peak_bytes)
			          << ",\"post_query_rss_mib\":" << MiB(post_submit_memory.resident_bytes);
		} else {
			std::cout << ",\"query_collect_s\":null,\"api_readback_s\":null,"
			             "\"process_peak_through_query_mib\":null,\"post_query_rss_mib\":null";
		}
		std::cout
		          << ",\"result_rows\":" << result_rows << ",\"output_rows\":" << output_rows
		          << ",\"chosen\":" << chosen
		          << ",\"active_chosen\":" << active_chosen
		          << ",\"objective\":" << static_cast<double>(objective)
		          << ",\"payload_bytes\":" << payload_bytes << "}\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
