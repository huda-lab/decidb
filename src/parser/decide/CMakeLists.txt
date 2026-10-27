add_library_unity(
  duckdb_parser_decide
  OBJECT
  decide_parsed_markers.cpp
  decide_removed_spellings.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_parser_decide>
    PARENT_SCOPE)
