include_directories(../../third_party/fmt/include)
include_directories(../../third_party/fmt)

add_subdirectory(formulation)
add_subdirectory(solver)
add_subdirectory(diagnostics)
add_subdirectory(naive)
add_subdirectory(gurobi)

# Propagate object files from subdirectories to parent scope
set(ALL_OBJECT_FILES ${ALL_OBJECT_FILES} PARENT_SCOPE)
