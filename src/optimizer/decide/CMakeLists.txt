add_library_unity(
  duckdb_optimizer_decide
  OBJECT
  decide_bound_absorption.cpp
  decide_linear_form.cpp
  decide_optimizer.cpp
  decide_rewrite_abs.cpp
  decide_rewrite_bilinear.cpp
  decide_rewrite_minmax.cpp
  decide_rewrite_norm_in.cpp
  decide_rewrite_notequal_avg.cpp
  decide_solver_gate.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_optimizer_decide>
    PARENT_SCOPE)
