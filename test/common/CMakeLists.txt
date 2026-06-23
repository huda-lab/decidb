add_library_unity(
  test_common
  OBJECT
  test_cast.cpp
  test_checksum.cpp
  test_decidb_constraint_provenance.cpp
  test_decidb_diagnostic_solves.cpp
  test_decidb_escape_characterization.cpp
  test_decidb_variable_provenance.cpp
  test_file_system.cpp
  test_hyperlog.cpp
  test_numeric_cast.cpp
  test_parse_logical_type.cpp
  test_utf.cpp
  test_strftime.cpp
  test_string_util.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:test_common>
    PARENT_SCOPE)
