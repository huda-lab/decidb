add_library_unity(
  duckdb_decidb_util
  OBJECT
  decide_diagnostic.cpp
  decide_diagnostic_engines.cpp
  decide_router.cpp
  diagnostic_solves.cpp
  ilp_linearization.cpp
  ilp_model_builder.cpp
  ilp_solver.cpp
  solver_registry.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_util>
    PARENT_SCOPE)
