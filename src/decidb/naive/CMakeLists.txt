add_library_unity(
  duckdb_decidb_naive
  OBJECT
  deterministic_naive.cpp)
target_link_libraries(duckdb_decidb_naive duckdb_highs)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_naive>
    PARENT_SCOPE)