add_library_unity(
  duckdb_planner_operator_decide
  OBJECT
  logical_decide.cpp
  logical_decide_diagnose.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_planner_operator_decide>
    PARENT_SCOPE)
