add_library_unity(duckdb_decidb_symbolic OBJECT decide_symbolic.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_symbolic>
    PARENT_SCOPE)

