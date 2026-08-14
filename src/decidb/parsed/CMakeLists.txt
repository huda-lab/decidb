add_library_unity(duckdb_decidb_parsed OBJECT decide_grammar_repair.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_parsed>
    PARENT_SCOPE)
