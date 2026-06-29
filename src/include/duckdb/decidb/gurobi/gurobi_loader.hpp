//===----------------------------------------------------------------------===//
//                         DecidB
//
// gurobi_loader.hpp — Runtime dynamic loading of the Gurobi C API
//
// Loads Gurobi at runtime (dlopen on POSIX, LoadLibrary on Windows) so a single
// binary can use Gurobi when available and fall back to HiGHS otherwise, without
// requiring Gurobi headers or library at build time.
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

//===----------------------------------------------------------------------===//
// Gurobi constants (stable across versions, defined locally to avoid header dep)
//===----------------------------------------------------------------------===//

// Variable types
static constexpr char GRB_CONTINUOUS = 'C';
static constexpr char GRB_BINARY = 'B';
static constexpr char GRB_INTEGER = 'I';

// Objective sense
static constexpr int GRB_MINIMIZE = 1;
static constexpr int GRB_MAXIMIZE = -1;

// Model status codes (must match Gurobi's GRB_* values in gurobi_c.h)
static constexpr int GRB_OPTIMAL = 2;
static constexpr int GRB_INFEASIBLE = 3;
static constexpr int GRB_INF_OR_UNBD = 4;
static constexpr int GRB_UNBOUNDED = 5;
// Values must mirror Gurobi's gurobi_c.h. Drift here is silent: a wrong
// integer here makes the branch in gurobi_solver.cpp unreachable while a
// different real status falls through to the generic error message.
static constexpr int GRB_CUTOFF = 6;
static constexpr int GRB_ITERATION_LIMIT = 7;
static constexpr int GRB_NODE_LIMIT = 8;
static constexpr int GRB_TIME_LIMIT = 9;
static constexpr int GRB_SOLUTION_LIMIT = 10;
static constexpr int GRB_INTERRUPTED = 11;
static constexpr int GRB_NUMERIC = 12;
static constexpr int GRB_SUBOPTIMAL = 13;
static constexpr int GRB_INPROGRESS = 14;
static constexpr int GRB_USER_OBJ_LIMIT = 15;

// Attribute name strings
static constexpr const char *GRB_INT_ATTR_MODELSENSE = "ModelSense";
static constexpr const char *GRB_INT_ATTR_STATUS = "Status";
static constexpr const char *GRB_DBL_ATTR_X = "X";
static constexpr const char *GRB_DBL_ATTR_OBJVAL = "ObjVal";

//===----------------------------------------------------------------------===//
// Function pointer table for the Gurobi C API functions DecidB uses
//===----------------------------------------------------------------------===//

struct GurobiAPI {
	// Environment management
	int (*emptyenv_internal)(void **envP, int major, int minor, int tech);
	int (*startenv)(void *env);
	void (*freeenv)(void *env);
	int (*setintparam)(void *env, const char *paramname, int value);
	int (*setdblparam)(void *env, const char *paramname, double value);
	// Returns the model's own env copy (GRBnewmodel snapshots the env, so
	// post-creation parameter changes must go through this env, not the original).
	void *(*getenv_model)(void *model);

	// Model management
	int (*newmodel)(void *env, void **modelP, const char *name, int numvars,
	                double *obj, double *lb, double *ub, char *vtype, char **varnames);
	int (*freemodel)(void *model);

	// Model building
	int (*setintattr)(void *model, const char *attrname, int newvalue);
	int (*addconstr)(void *model, int numnz, int *cind, double *cval,
	                 char sense, double rhs, const char *constrname);
	int (*addqpterms)(void *model, int numqnz, int *qrow, int *qcol, double *qval);
	int (*addqconstr)(void *model, int numlnz, int *lind, double *lval,
	                  int numqnz, int *qrow, int *qcol, double *qval,
	                  char sense, double rhs, const char *name);

	// Solve and query
	int (*optimize)(void *model);
	int (*getintattr)(void *model, const char *attrname, int *valueP);
	int (*getdblattr)(void *model, const char *attrname, double *valueP);
	int (*getdblattrarray)(void *model, const char *attrname, int start, int len, double *values);

	// Error reporting
	const char *(*geterrormsg)(void *env);

	// Version extracted from loaded library
	int version_major;
	int version_minor;
	int version_tech;
};

//===----------------------------------------------------------------------===//
// Loader: thread-safe singleton that attempts dlopen once
//===----------------------------------------------------------------------===//

class GurobiLoader {
public:
	//! Attempt to load the Gurobi shared library. Thread-safe, runs once.
	static bool Load();

	//! Was Load() successful?
	static bool IsLoaded();

	//! Get the function pointer table (only valid when IsLoaded() == true).
	static const GurobiAPI &API();
};

} // namespace duckdb
