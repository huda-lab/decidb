add_library_unity(
  duckdb_optimizer_decide_direct
  OBJECT
  direct_builder.cpp
  direct_coordinator.cpp
  direct_problem.cpp
  direct_result_boundary.cpp
  s1_rule.cpp)
set(ALL_OBJECT_FILES ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_optimizer_decide_direct> PARENT_SCOPE)
