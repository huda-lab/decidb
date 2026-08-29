add_library_unity(
  duckdb_decidb_formulation
  OBJECT
  ilp_linearization.cpp
  ilp_model_builder.cpp
  linearization_bigm.cpp
  linearization_bilinear_abs.cpp
  linearization_minmax.cpp
  linearization_not_equal.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_formulation>
    PARENT_SCOPE)
