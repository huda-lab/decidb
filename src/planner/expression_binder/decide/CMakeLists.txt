add_library_unity(
  duckdb_expression_binder_decide
  OBJECT
  decide_binder.cpp
  decide_constraints_binder.cpp
  decide_degree.cpp
  decide_objective_binder.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_expression_binder_decide>
    PARENT_SCOPE)
