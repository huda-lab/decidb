add_library_unity(
  duckdb_decidb_diagnostics
  OBJECT
  decide_diagnostic.cpp
  decide_diagnostic_engines.cpp
  decide_diagnostic_render.cpp
  decide_router.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_diagnostics>
    PARENT_SCOPE)
