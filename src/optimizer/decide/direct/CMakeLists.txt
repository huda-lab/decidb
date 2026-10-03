add_library_unity(duckdb_optimizer_decide_direct OBJECT direct_problem.cpp direct_solve.cpp)
set(ALL_OBJECT_FILES ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_optimizer_decide_direct> PARENT_SCOPE)
