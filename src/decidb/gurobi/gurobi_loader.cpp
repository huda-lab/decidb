//===----------------------------------------------------------------------===//
//                         DecidB
//
// gurobi_loader.cpp — Runtime dynamic loading of the Gurobi C API
//
// POSIX (Linux/macOS): dlopen/dlsym + opendir/readdir.
// Windows:             LoadLibraryEx/GetProcAddress + FindFirstFile/FindNextFile.
//
// The high-level search/resolve logic is shared; only the platform primitives
// and the well-known install locations differ per OS.
//
//===----------------------------------------------------------------------===//

#include "duckdb/decidb/gurobi/gurobi_loader.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

namespace duckdb {

//===----------------------------------------------------------------------===//
// Internal state
//===----------------------------------------------------------------------===//

static std::once_flag g_load_flag;
static bool g_loaded = false;
static GurobiAPI g_api = {};

//===----------------------------------------------------------------------===//
// Platform abstraction
//
// Handles are stored as opaque void* on every platform (HMODULE is a pointer).
//===----------------------------------------------------------------------===//

#ifdef _WIN32
static constexpr char PATH_SEP = '\\';

//! Open a shared library by full path or bare name. Returns a handle or nullptr.
static void *TryOpen(const char *path) {
	// LOAD_WITH_ALTERED_SEARCH_PATH: when `path` is fully qualified, search its
	// own directory for dependent DLLs. Ignored for bare names (which use PATH).
	return reinterpret_cast<void *>(LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

//! Resolve a symbol from an open handle.
static void *GetSym(void *handle, const char *name) {
	FARPROC proc = GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
	return reinterpret_cast<void *>(proc);
}

//! Close an open handle.
static void CloseLib(void *handle) {
	FreeLibrary(reinterpret_cast<HMODULE>(handle));
}

//! True if `name` is the Gurobi C API DLL (gurobiXYZ.dll) — not the JNI, C++,
//! or *_light variants. We require a digit right after "gurobi" so we match
//! gurobi130.dll but skip GurobiJni130.dll and gurobi_c++md.lib.
static bool IsGurobiLib(const char *name) {
	if (_strnicmp(name, "gurobi", 6) != 0) {
		return false;
	}
	if (name[6] < '0' || name[6] > '9') {
		return false;
	}
	size_t len = strlen(name);
	return len > 4 && _stricmp(name + len - 4, ".dll") == 0;
}
#else
static constexpr char PATH_SEP = '/';

static void *TryOpen(const char *path) {
	return dlopen(path, RTLD_LAZY);
}

static void *GetSym(void *handle, const char *name) {
	return dlsym(handle, name);
}

static void CloseLib(void *handle) {
	dlclose(handle);
}

//! True if `name` is the Gurobi C API shared object (libgurobi*.so / .dylib),
//! excluding the *_light reduced-functionality variant.
static bool IsGurobiLib(const char *name) {
	if (strncmp(name, "libgurobi", 9) != 0) {
		return false;
	}
	if (strstr(name, "_light")) {
		return false;
	}
	return strstr(name, ".so") || strstr(name, ".dylib");
}
#endif

//! List a directory's entries (filenames only, no path). Empty vector on failure.
static std::vector<std::string> ListDir(const std::string &dir) {
	std::vector<std::string> names;
#ifdef _WIN32
	std::string pattern = dir + PATH_SEP + "*";
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return names;
	}
	do {
		names.emplace_back(fd.cFileName);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir.c_str());
	if (!d) {
		return names;
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != nullptr) {
		names.emplace_back(entry->d_name);
	}
	closedir(d);
#endif
	return names;
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

//! Extract version from a library name: "libgurobi130.so" / "gurobi130.dll" →
//! (13, 0, 0), or "libgurobi.so.13.0.1" → (13, 0, 1). Returns false if
//! unparseable (callers fall back to version 0.0.0).
static bool ExtractVersion(const char *name, int &major, int &minor, int &tech) {
	// Pattern 1: [lib]gurobiXYZ.{so,dylib,dll}  (e.g., libgurobi130.so, gurobi130.dll)
	const char *p = strstr(name, "gurobi");
	if (p) {
		p += 6; // skip "gurobi"
		if (*p >= '0' && *p <= '9') {
			int ver = 0;
			while (*p >= '0' && *p <= '9') {
				ver = ver * 10 + (*p - '0');
				p++;
			}
			if (ver >= 10) {
				// e.g., 130 → 13.0, 95 → 9.5
				major = ver / 10;
				minor = ver % 10;
				tech = 0;
				return true;
			}
		}
	}

	// Pattern 2: libgurobi.so.X.Y.Z
	p = strstr(name, ".so.");
	if (p) {
		p += 4; // skip ".so."
		if (sscanf(p, "%d.%d.%d", &major, &minor, &tech) >= 2) {
			return true;
		}
	}

	// Default: unknown version
	major = 0;
	minor = 0;
	tech = 0;
	return false;
}

//! Search a directory for the Gurobi C API library and open the first match.
//! Fills `matched_name` with the filename (for version extraction).
static void *SearchDir(const std::string &dir, std::string &matched_name) {
	for (const auto &name : ListDir(dir)) {
		if (!IsGurobiLib(name.c_str())) {
			continue;
		}
		void *handle = TryOpen((dir + PATH_SEP + name).c_str());
		if (handle) {
			matched_name = name;
			return handle;
		}
	}
	return nullptr;
}

//! Resolve all required function pointers from the loaded library.
//! Returns true only if ALL required symbols were found.
static bool ResolveSymbols(void *handle, GurobiAPI &api) {
	// Helper macro for cleaner symbol resolution
	#define LOAD_SYM(field, sym_name)                                              \
		api.field = reinterpret_cast<decltype(api.field)>(GetSym(handle, sym_name)); \
		if (!api.field) return false;

	LOAD_SYM(emptyenv_internal, "GRBemptyenvinternal")
	LOAD_SYM(startenv,          "GRBstartenv")
	LOAD_SYM(freeenv,           "GRBfreeenv")
	LOAD_SYM(setintparam,       "GRBsetintparam")
	LOAD_SYM(setdblparam,       "GRBsetdblparam")
	LOAD_SYM(getenv_model,      "GRBgetenv")
	LOAD_SYM(newmodel,          "GRBnewmodel")
	LOAD_SYM(freemodel,         "GRBfreemodel")
	LOAD_SYM(setintattr,        "GRBsetintattr")
	LOAD_SYM(addconstr,         "GRBaddconstr")
	LOAD_SYM(addqpterms,       "GRBaddqpterms")
	LOAD_SYM(optimize,          "GRBoptimize")
	LOAD_SYM(getintattr,        "GRBgetintattr")
	LOAD_SYM(getdblattr,        "GRBgetdblattr")
	LOAD_SYM(getdblattrarray,   "GRBgetdblattrarray")
	LOAD_SYM(geterrormsg,       "GRBgeterrormsg")

	#undef LOAD_SYM

	// Optional: GRBaddqconstr (quadratic constraints, available in Gurobi 5.0+)
	api.addqconstr = reinterpret_cast<decltype(api.addqconstr)>(GetSym(handle, "GRBaddqconstr"));
	// Not required — bilinear constraints will error if this is missing and needed

	// Optional: GRBaddgenconstrAbs (native absolute value, Gurobi 8.0+). Loaded
	// best-effort: a missing symbol turns off the `abs` capability flag, and stage 05
	// lowers ABS into its Big-M envelope exactly as it always has.
	api.addgenconstrAbs =
	    reinterpret_cast<decltype(api.addgenconstrAbs)>(GetSym(handle, "GRBaddgenconstrAbs"));
	api.addgenconstrMin =
	    reinterpret_cast<decltype(api.addgenconstrMin)>(GetSym(handle, "GRBaddgenconstrMin"));
	api.addgenconstrMax =
	    reinterpret_cast<decltype(api.addgenconstrMax)>(GetSym(handle, "GRBaddgenconstrMax"));

	// Optional: GRBterminate (mid-solve interrupt). Present in all modern Gurobi, but
	// loaded best-effort so a missing symbol never breaks solving — the mid-chunk Ctrl-C
	// feature just falls back to interrupting at the next chunk boundary.
	api.terminate = reinterpret_cast<decltype(api.terminate)>(GetSym(handle, "GRBterminate"));

	return true;
}

//===----------------------------------------------------------------------===//
// Load implementation
//===----------------------------------------------------------------------===//

static void DoLoad() {
	void *handle = nullptr;
	std::string matched_name;

	// 1. $GUROBI_HOME — DLLs live in bin\ on Windows, lib/ on POSIX.
	const char *gurobi_home = getenv("GUROBI_HOME");
	if (gurobi_home && gurobi_home[0]) {
#ifdef _WIN32
		handle = SearchDir(std::string(gurobi_home) + "\\bin", matched_name);
#else
		handle = SearchDir(std::string(gurobi_home) + "/lib", matched_name);
#endif
	}

	// 2. Well-known versioned names via the system library search path
	//    (PATH on Windows, LD_LIBRARY_PATH / DYLD_LIBRARY_PATH on POSIX).
	if (!handle) {
		static const char *candidates[] = {
#ifdef _WIN32
			"gurobi130.dll", "gurobi120.dll", "gurobi110.dll",
			"gurobi100.dll", "gurobi95.dll",
#elif defined(__APPLE__)
			"libgurobi130.dylib", "libgurobi120.dylib", "libgurobi110.dylib",
			"libgurobi100.dylib", "libgurobi95.dylib",  "libgurobi.dylib",
#else
			"libgurobi130.so", "libgurobi120.so", "libgurobi110.so",
			"libgurobi100.so", "libgurobi95.so",  "libgurobi.so",
#endif
			nullptr
		};
		for (const char **c = candidates; *c; c++) {
			handle = TryOpen(*c);
			if (handle) {
				matched_name = *c;
				break;
			}
		}
	}

	// 3. Common install locations
	if (!handle) {
#ifdef _WIN32
		// Windows: C:\gurobiXXX\win64\bin  (installer's default layout)
		for (const auto &entry : ListDir("C:\\")) {
			if (_strnicmp(entry.c_str(), "gurobi", 6) != 0) {
				continue;
			}
			handle = SearchDir("C:\\" + entry + "\\win64\\bin", matched_name);
			if (handle) {
				break;
			}
			handle = SearchDir("C:\\" + entry + "\\win32\\bin", matched_name);
			if (handle) {
				break;
			}
		}
#elif defined(__APPLE__)
		// macOS: ~/gurobiXXXX/macos_universal2/lib, /Library/gurobi*/..., /usr/local/lib
		const char *home = getenv("HOME");
		if (home && home[0]) {
			std::string home_str(home);
			for (const auto &entry : ListDir(home_str)) {
				if (strncmp(entry.c_str(), "gurobi", 6) != 0) {
					continue;
				}
				handle = SearchDir(home_str + "/" + entry + "/macos_universal2/lib", matched_name);
				if (handle) {
					break;
				}
			}
		}
		if (!handle) {
			for (const auto &entry : ListDir("/Library")) {
				if (strncmp(entry.c_str(), "gurobi", 6) != 0) {
					continue;
				}
				handle = SearchDir("/Library/" + entry + "/macos_universal2/lib", matched_name);
				if (handle) {
					break;
				}
			}
		}
		if (!handle) {
			handle = SearchDir("/usr/local/lib", matched_name);
		}
#else
		// Linux: ~/gurobiXXXX/linux64/lib, /opt/gurobi*/linux64/lib
		const char *home = getenv("HOME");
		if (home && home[0]) {
			std::string home_str(home);
			for (const auto &entry : ListDir(home_str)) {
				if (strncmp(entry.c_str(), "gurobi", 6) != 0) {
					continue;
				}
				handle = SearchDir(home_str + "/" + entry + "/linux64/lib", matched_name);
				if (handle) {
					break;
				}
			}
		}
		if (!handle) {
			for (const auto &entry : ListDir("/opt")) {
				if (strncmp(entry.c_str(), "gurobi", 6) != 0) {
					continue;
				}
				handle = SearchDir("/opt/" + entry + "/linux64/lib", matched_name);
				if (handle) {
					break;
				}
			}
		}
#endif
	}

	if (!handle) {
		return; // Gurobi not found anywhere
	}

	// Resolve all symbols
	GurobiAPI api = {};
	if (!ResolveSymbols(handle, api)) {
		CloseLib(handle);
		return; // Incomplete library
	}

	// Extract version from the matched library name
	ExtractVersion(matched_name.c_str(), api.version_major, api.version_minor, api.version_tech);

	g_api = api;
	g_loaded = true;
	// Note: we intentionally never close the handle — the library stays loaded
	// for the process lifetime.
}

//===----------------------------------------------------------------------===//
// Public interface
//===----------------------------------------------------------------------===//

bool GurobiLoader::Load() {
	std::call_once(g_load_flag, DoLoad);
	return g_loaded;
}

bool GurobiLoader::IsLoaded() {
	return g_loaded;
}

const GurobiAPI &GurobiLoader::API() {
	return g_api;
}

} // namespace duckdb
