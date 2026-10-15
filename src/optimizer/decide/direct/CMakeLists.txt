add_library_unity(
  duckdb_optimizer_decide_direct
  OBJECT
  a1_separable_box_rule.cpp
  direct_plan_factory.cpp
  direct_rule_driver.cpp
  direct_expression.cpp
  direct_ir.cpp
  direct_registry.cpp
  direct_result_adapter.cpp
  direct_late_materialization.cpp
  s1_uniform_matroid_rule.cpp
  s1_uniform_matroid_helpers.cpp)
set(ALL_OBJECT_FILES ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_optimizer_decide_direct> PARENT_SCOPE)
