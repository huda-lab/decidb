add_library_unity(
  duckdb_decidb_formulation
  OBJECT
  ilp_linearization.cpp
  ilp_model_builder.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_decidb_formulation>
    PARENT_SCOPE)
