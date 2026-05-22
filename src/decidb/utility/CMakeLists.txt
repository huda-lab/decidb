add_library_unity(
  duckdb_decidb_util
  OBJECT
  debug.cpp
  ilp_model_builder.cpp
  ilp_solver.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_util>
    PARENT_SCOPE)