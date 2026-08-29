add_library_unity(
  duckdb_decidb_solver
  OBJECT
  ilp_solver.cpp
  probe_models.cpp
  solver_registry.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_solver>
    PARENT_SCOPE)
