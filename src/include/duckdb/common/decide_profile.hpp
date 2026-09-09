#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace duckdb {

class DecideProfileScope {
public:
	explicit DecideProfileScope(const char *name_p) : enabled(Enabled()), name(name_p) {
		if (enabled) {
			parent = Current();
			Begin();
		}
	}
	~DecideProfileScope() {
		End();
	}
	DecideProfileScope(const DecideProfileScope &) = delete;
	DecideProfileScope &operator=(const DecideProfileScope &) = delete;

	static bool Enabled() {
		return std::getenv("DECIDB_PROFILE") != nullptr;
	}
	static uint64_t Now() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                                std::chrono::steady_clock::now().time_since_epoch())
		                                .count());
	}
	void Next(const char *next_name) {
		if (enabled) {
			End();
			name = next_name;
			Begin();
		}
	}
	static void Counter(const char *key, double value) {
		if (Enabled()) {
			std::fprintf(stderr, "DECIDB_PROFILE: {\"event\":\"counter\",\"parent\":%llu,\"name\":\"%s\",\"value\":%.17g}\n",
			             static_cast<unsigned long long>(Current()), key, value);
		}
	}

private:
	static uint64_t &Current() {
		static thread_local uint64_t current = 0;
		return current;
	}
	static uint64_t NextId() {
		static std::atomic<uint64_t> next {0};
		return ++next;
	}
	static uint64_t ResidentBytes() {
#if defined(__APPLE__)
		mach_task_basic_info_data_t info;
		mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
		if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
			return info.resident_size;
		}
#endif
		return 0;
	}
	static uint64_t PeakBytes() {
#ifndef _WIN32
		struct rusage usage;
		if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
			return static_cast<uint64_t>(usage.ru_maxrss);
#else
			return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
		}
#endif
		return 0;
	}
	void Begin() {
		id = NextId();
		Current() = id;
		start = Now();
		std::fprintf(stderr,
		             "DECIDB_PROFILE: {\"event\":\"begin\",\"id\":%llu,\"parent\":%llu,\"name\":\"%s\",\"start_ns\":%llu,\"rss_bytes\":%llu}\n",
		             static_cast<unsigned long long>(id), static_cast<unsigned long long>(parent), name,
		             static_cast<unsigned long long>(start), static_cast<unsigned long long>(ResidentBytes()));
	}
	void End() {
		if (!enabled) {
			return;
		}
		auto finish = Now();
		std::fprintf(stderr,
		             "DECIDB_PROFILE: {\"event\":\"end\",\"id\":%llu,\"parent\":%llu,\"name\":\"%s\",\"end_ns\":%llu,\"duration_ms\":%.6f,\"rss_bytes\":%llu,\"peak_rss_bytes\":%llu,\"unwinding\":%s}\n",
		             static_cast<unsigned long long>(id), static_cast<unsigned long long>(parent), name,
		             static_cast<unsigned long long>(finish), static_cast<double>(finish - start) / 1e6,
		             static_cast<unsigned long long>(ResidentBytes()), static_cast<unsigned long long>(PeakBytes()),
		             std::uncaught_exception() ? "true" : "false");
		Current() = parent;
	}
	bool enabled;
	const char *name;
	uint64_t id = 0;
	uint64_t parent = 0;
	uint64_t start = 0;
};

class DecideProfileTotals {
public:
	explicit DecideProfileTotals(const char *name_p) : enabled(DecideProfileScope::Enabled()), name(name_p) {
	}
	~DecideProfileTotals() {
		if (enabled) {
			std::fprintf(stderr,
			             "DECIDB_PROFILE: {\"event\":\"aggregate\",\"name\":\"%s\",\"duration_ms\":%.6f,\"calls\":%llu}\n",
			             name, static_cast<double>(nanoseconds) / 1e6, static_cast<unsigned long long>(calls));
		}
	}
	bool enabled;
	const char *name;
	uint64_t nanoseconds = 0;
	uint64_t calls = 0;
};

class DecideProfileSample {
public:
	explicit DecideProfileSample(DecideProfileTotals &totals_p) : totals(totals_p) {
		if (totals.enabled) {
			start = DecideProfileScope::Now();
		}
	}
	~DecideProfileSample() {
		if (totals.enabled) {
			totals.nanoseconds += DecideProfileScope::Now() - start;
			totals.calls++;
		}
	}

private:
	DecideProfileTotals &totals;
	uint64_t start = 0;
};

}
