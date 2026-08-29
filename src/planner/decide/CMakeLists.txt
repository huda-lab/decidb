add_library_unity(
  duckdb_planner_decide
  OBJECT
  decide_canonicalizer.cpp
  decide_cast_policy.cpp
  decide_source_provenance.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_planner_decide>
    PARENT_SCOPE)
