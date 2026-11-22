add_library_unity(duckdb_parser_decide OBJECT parsed_decide.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_parser_decide>
    PARENT_SCOPE)
