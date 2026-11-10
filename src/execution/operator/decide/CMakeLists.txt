add_library_unity(duckdb_operator_decide OBJECT physical_decide.cpp physical_decide_frames.cpp
                  physical_decide_diagnose.cpp decide_row_store.cpp decide_evaluator.cpp decide_descent.cpp
                  decide_key_map.cpp decide_value_extraction.cpp decide_tree_columns.cpp decide_model_emitter.cpp)
target_link_libraries(duckdb_operator_decide duckdb_highs)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_operator_decide>
    PARENT_SCOPE)
