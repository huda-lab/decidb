if(POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif()

# The vendored tree holds exactly the sources upstream's static library compiles (scripts/upgrade_highs.py removes
# the rest), so the build compiles all of it, as the amalgamation (scripts/package_build.py) does.
file(GLOB_RECURSE HIGHS_SOURCES CONFIGURE_DEPENDS highs/*.c highs/*.cc highs/*.cpp extern/*.c extern/*.cc extern/*.cpp)

add_library(duckdb_highs STATIC ${HIGHS_SOURCES})

target_include_directories(duckdb_highs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extern>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extern/pdqsort>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/interfaces>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/io>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/ipm>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/ipm/ipx>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/ipm/basiclu>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/lp_data>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/mip>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/model>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/parallel>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/pdlp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/pdlp/cupdlp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/presolve>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/qpsolver>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/simplex>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/test_kkt>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/util>
)

set_target_properties(duckdb_highs PROPERTIES EXPORT_NAME duckdb_duckdb_highs)

install(TARGETS duckdb_highs
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_highs)
