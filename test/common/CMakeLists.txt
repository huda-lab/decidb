add_library_unity(
  test_common
  OBJECT
  test_cast.cpp
  test_checksum.cpp
  test_decidb_canonical_verifier.cpp
  test_decidb_constraint_provenance.cpp
  test_decidb_diagnostic_engines.cpp
  test_decidb_direct_decline.cpp
  test_decidb_direct_rule_driver.cpp
  test_decidb_direct_ir.cpp
  test_decidb_numeric_policy.cpp
  test_decidb_probe_models.cpp
  test_decidb_escape_characterization.cpp
  test_decidb_plan_serialization.cpp
  test_decidb_router.cpp
  test_decidb_solver_registry.cpp
  test_decidb_term_split.cpp
  test_decidb_variable_provenance.cpp
  test_file_system.cpp
  test_hyperlog.cpp
  test_numeric_cast.cpp
  test_parse_logical_type.cpp
  test_utf.cpp
  test_strftime.cpp
  test_string_util.cpp)

# HiGHS's headers declare `using std::vector`, which clashes with `using namespace duckdb` in a shared unity file.
add_library_unity(test_decidb_highs OBJECT test_decidb_highs_symmetry.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:test_common>
    $<TARGET_OBJECTS:test_decidb_highs>
    PARENT_SCOPE)
