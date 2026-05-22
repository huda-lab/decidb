add_library_unity(
  duckdb_decidb_gurobi
  OBJECT
  gurobi_solver.cpp
  gurobi_loader.cpp)

# Runtime detection via dlopen — no compile-time Gurobi dependency
target_link_libraries(duckdb_decidb_gurobi PRIVATE ${CMAKE_DL_LIBS})

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_gurobi>
    PARENT_SCOPE)
