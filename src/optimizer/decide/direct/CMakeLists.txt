add_library_unity(
  duckdb_optimizer_decide_direct
  OBJECT
  a1_rule.cpp
  direct_builder.cpp
  direct_rule_driver.cpp
  direct_expression.cpp
  direct_ir.cpp
  direct_registry.cpp
  direct_result_boundary.cpp
  direct_second_read.cpp
  s1_rule.cpp
  s1_support.cpp)
set(ALL_OBJECT_FILES ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_optimizer_decide_direct> PARENT_SCOPE)
