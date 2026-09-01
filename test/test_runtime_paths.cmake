if(NOT DEFINED TEST_TARGET_CHIP OR NOT DEFINED TEST_OUTPUT_DIR)
    message(FATAL_ERROR "TEST_TARGET_CHIP and TEST_OUTPUT_DIR are required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CosmoRuntimePaths.cmake")
cosmo_configure_runtime_paths("${TEST_TARGET_CHIP}" "${TEST_OUTPUT_DIR}")
