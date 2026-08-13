add_library_unity(
  duckdb_decidb_util
  OBJECT
  decide_diagnostic.cpp
  decide_diagnostic_engines.cpp
  decide_parse_hints.cpp
  decide_router.cpp
  diagnostic_solves.cpp
  debug.cpp
  ilp_model_builder.cpp
  ilp_solver.cpp
  decide_cast_policy.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_util>
    PARENT_SCOPE)
