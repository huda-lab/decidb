//===----------------------------------------------------------------------===//
//                         PackDB
//
// gurobi_loader.cpp — Runtime dynamic loading of Gurobi via dlopen/dlsym
//
//===----------------------------------------------------------------------===//

#include "gurobi_loader.hpp"

#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Internal state
//===----------------------------------------------------------------------===//

static std::once_flag g_load_flag;
static bool g_loaded = false;
static GurobiAPI g_api = {};

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

//! Try to extract version from a library name like "libgurobi130.so" → (13, 0, 0)
//! or "libgurobi.so.13.0.1" → (13, 0, 1). Returns false if unparseable.
static bool ExtractVersion(const char *name, int &major, int &minor, int &tech) {
	// Pattern 1: libgurobiXYZ.so (e.g., libgurobi130.so)
	const char *p = strstr(name, "libgurobi");
	if (p) {
		p += 9; // skip "libgurobi"
		// Skip non-digit prefix (e.g., "_light")
		if (*p >= '0' && *p <= '9') {
			int ver = 0;
			while (*p >= '0' && *p <= '9') {
				ver = ver * 10 + (*p - '0');
				p++;
			}
			if (ver >= 100) {
				// e.g., 130 → 13.0, 1201 → 12.0.1
				major = ver / 10;
				minor = ver % 10;
				tech = 0;
				return true;
			} else if (ver >= 10) {
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

	// Pattern 3: libgurobiXYZ.dylib (macOS)
	p = strstr(name, "libgurobi");
	if (p && strstr(name, ".dylib")) {
		p += 9; // skip "libgurobi"
		if (*p >= '0' && *p <= '9') {
			int ver = 0;
			while (*p >= '0' && *p <= '9') {
				ver = ver * 10 + (*p - '0');
				p++;
			}
			if (ver >= 10) {
				major = ver / 10;
				minor = ver % 10;
				tech = 0;
				return true;
			}
		}
	}

	// Default: unknown version
	major = 0;
	minor = 0;
	tech = 0;
	return false;
}

//! Try to dlopen a specific path. Returns handle or nullptr.
static void *TryOpen(const char *path) {
	return dlopen(path, RTLD_LAZY);
}

//! Search a directory for libgurobi*.so or libgurobi*.dylib files.
//! Also fills version_str with the matched filename for version extraction.
static void *SearchDir(const std::string &dir, std::string &matched_name) {
	DIR *d = opendir(dir.c_str());
	if (!d) {
		return nullptr;
	}
	void *handle = nullptr;
	struct dirent *entry;
	while ((entry = readdir(d)) != nullptr) {
		const char *name = entry->d_name;
		// Match libgurobi* but skip libgurobi_light and libgurobiXX_light
		if (strncmp(name, "libgurobi", 9) != 0) {
			continue;
		}
		if (strstr(name, "_light")) {
			continue;
		}
		// Must contain ".so" or ".dylib"
		if (!strstr(name, ".so") && !strstr(name, ".dylib")) {
			continue;
		}
		std::string full_path = dir + "/" + name;
		handle = TryOpen(full_path.c_str());
		if (handle) {
			matched_name = name;
			break;
		}
	}
	closedir(d);
	return handle;
}

//! Resolve all 13 function pointers from the loaded library.
//! Returns true if ALL symbols were found.
static bool ResolveSymbols(void *handle, GurobiAPI &api) {
	// Helper macro for cleaner symbol resolution
	#define LOAD_SYM(field, sym_name)                                         \
		api.field = reinterpret_cast<decltype(api.field)>(dlsym(handle, sym_name)); \
		if (!api.field) return false;

	LOAD_SYM(emptyenv_internal, "GRBemptyenvinternal")
	LOAD_SYM(startenv,          "GRBstartenv")
	LOAD_SYM(freeenv,           "GRBfreeenv")
	LOAD_SYM(setintparam,       "GRBsetintparam")
	LOAD_SYM(setdblparam,       "GRBsetdblparam")
	LOAD_SYM(newmodel,          "GRBnewmodel")
	LOAD_SYM(freemodel,         "GRBfreemodel")
	LOAD_SYM(setintattr,        "GRBsetintattr")
	LOAD_SYM(addconstr,         "GRBaddconstr")
	LOAD_SYM(addqpterms,       "GRBaddqpterms")
	LOAD_SYM(optimize,          "GRBoptimize")
	LOAD_SYM(getintattr,        "GRBgetintattr")
	LOAD_SYM(getdblattrarray,   "GRBgetdblattrarray")
	LOAD_SYM(geterrormsg,       "GRBgeterrormsg")

	#undef LOAD_SYM

	// Optional: GRBaddqconstr (quadratic constraints, available in Gurobi 5.0+)
	api.addqconstr = reinterpret_cast<decltype(api.addqconstr)>(dlsym(handle, "GRBaddqconstr"));
	// Not required — bilinear constraints will error if this is missing and needed

	return true;
}

//===----------------------------------------------------------------------===//
// Load implementation
//===----------------------------------------------------------------------===//

static void DoLoad() {
	void *handle = nullptr;
	std::string matched_name;

	// 1. Try $GUROBI_HOME/lib/
	const char *gurobi_home = getenv("GUROBI_HOME");
	if (gurobi_home && gurobi_home[0]) {
		std::string lib_dir = std::string(gurobi_home) + "/lib";
		handle = SearchDir(lib_dir, matched_name);
	}

	// 2. Try well-known versioned names via system search paths (LD_LIBRARY_PATH/DYLD_LIBRARY_PATH etc.)
	if (!handle) {
		static const char *candidates[] = {
			"libgurobi130.so", "libgurobi120.so", "libgurobi110.so",
			"libgurobi100.so", "libgurobi95.so",  "libgurobi.so",
#ifdef __APPLE__
			"libgurobi130.dylib", "libgurobi120.dylib", "libgurobi110.dylib",
			"libgurobi100.dylib", "libgurobi95.dylib",  "libgurobi.dylib",
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

	// 3. Try common install locations
	if (!handle) {
		const char *home = getenv("HOME");
#ifdef __APPLE__
		// macOS: ~/gurobiXXXX/macos_universal2/lib/
		if (home && home[0]) {
			std::string home_str(home);
			DIR *d = opendir(home_str.c_str());
			if (d) {
				struct dirent *entry;
				while ((entry = readdir(d)) != nullptr) {
					if (strncmp(entry->d_name, "gurobi", 6) == 0) {
						std::string lib_dir = home_str + "/" + entry->d_name + "/macos_universal2/lib";
						handle = SearchDir(lib_dir, matched_name);
						if (handle) break;
					}
				}
				closedir(d);
			}
		}
#else
		// Linux: ~/gurobiXXXX/linux64/lib/
		if (home && home[0]) {
			std::string home_str(home);
			DIR *d = opendir(home_str.c_str());
			if (d) {
				struct dirent *entry;
				while ((entry = readdir(d)) != nullptr) {
					if (strncmp(entry->d_name, "gurobi", 6) == 0) {
						std::string lib_dir = home_str + "/" + entry->d_name + "/linux64/lib";
						handle = SearchDir(lib_dir, matched_name);
						if (handle) break;
					}
				}
				closedir(d);
			}
		}
#endif
	}

	if (!handle) {
#ifdef __APPLE__
		// 4a. macOS: /Library/gurobiXXXX/macos_universal2/lib/
		DIR *d = opendir("/Library");
		if (d) {
			struct dirent *entry;
			while ((entry = readdir(d)) != nullptr) {
				if (strncmp(entry->d_name, "gurobi", 6) == 0) {
					std::string lib_dir = std::string("/Library/") + entry->d_name + "/macos_universal2/lib";
					handle = SearchDir(lib_dir, matched_name);
					if (handle) break;
				}
			}
			closedir(d);
		}
		// 4b. macOS: /usr/local/lib/
		if (!handle) {
			handle = SearchDir("/usr/local/lib", matched_name);
		}
#else
		// 4. Linux: /opt/gurobi*/linux64/lib/
		DIR *d = opendir("/opt");
		if (d) {
			struct dirent *entry;
			while ((entry = readdir(d)) != nullptr) {
				if (strncmp(entry->d_name, "gurobi", 6) == 0) {
					std::string lib_dir = std::string("/opt/") + entry->d_name + "/linux64/lib";
					handle = SearchDir(lib_dir, matched_name);
					if (handle) break;
				}
			}
			closedir(d);
		}
#endif
	}

	if (!handle) {
		return; // Gurobi not found anywhere
	}

	// Resolve all symbols
	GurobiAPI api = {};
	if (!ResolveSymbols(handle, api)) {
		dlclose(handle);
		return; // Incomplete library
	}

	// Extract version from the matched library name
	ExtractVersion(matched_name.c_str(), api.version_major, api.version_minor, api.version_tech);

	g_api = api;
	g_loaded = true;
	// Note: we intentionally never dlclose — the library stays loaded for the process lifetime
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
