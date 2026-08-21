set(TOKENIZERS_ORIGINAL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd/tokenizers-cpp)
set(TOKENIZERS_SOURCE_DIR ${TOKENIZERS_ORIGINAL_SOURCE_DIR})
set(TOKENIZERS_INSTALL_DIR ${THIRDPARTY_INSTALL_PREFIX}/tokenizers)
set(TOKENIZERS_HEADERS ${TOKENIZERS_INSTALL_DIR}/include)
set(TOKENIZERS_CPP_LIB ${TOKENIZERS_INSTALL_DIR}/lib/libtokenizers_cpp.a)
set(TOKENIZERS_C_LIB ${TOKENIZERS_INSTALL_DIR}/lib/libtokenizers_c.a)

if(COSMO_TARGET_ARCH STREQUAL "x86_64")
    set(TOKENIZERS_SOURCE_DIR ${CMAKE_BINARY_DIR}/tokenizers_source)
    set(TOKENIZERS_DOWNLOAD_COMMAND
        ${CMAKE_COMMAND} -E rm -rf <SOURCE_DIR>
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${TOKENIZERS_ORIGINAL_SOURCE_DIR} <SOURCE_DIR>
    )
    set(TOKENIZERS_PATCH_COMMAND ${CMAKE_COMMAND} -E true)
else()
    set(TOKENIZERS_DOWNLOAD_COMMAND "")
    set(TOKENIZERS_PATCH_COMMAND ${CMAKE_COMMAND} -E true)
endif()

# tokenizers-c builds a Rust staticlib (cargo build --target
# aarch64-unknown-linux-gnu).  Its onig dependency compiles C sources through
# cc-rs, which looks up the compiler as "<target triple>-gcc" (i.e.
# aarch64-linux-gnu-gcc) by default.  Our ARM GNU toolchain is named
# aarch64-none-linux-gnu-*, so we must export the target-prefixed CC/CXX/AR
# and linker variables cargo/cc-rs consult, derived from the active CMake
# toolchain instead of hardcoding a distro prefix.
if(COSMO_TARGET_ARCH STREQUAL "aarch64")
    get_filename_component(TOKENIZERS_TOOLCHAIN_BIN ${CMAKE_C_COMPILER} DIRECTORY)
    get_filename_component(TOKENIZERS_CC_NAME ${CMAKE_C_COMPILER} NAME)
    string(REGEX REPLACE "-gcc$" "" TOKENIZERS_TRIPLE ${TOKENIZERS_CC_NAME})
    set(TOKENIZERS_BUILD_COMMAND
        ${CMAKE_COMMAND} -E env
            "CC_aarch64-unknown-linux-gnu=${CMAKE_C_COMPILER}"
            "CC_aarch64_unknown_linux_gnu=${CMAKE_C_COMPILER}"
            "CXX_aarch64-unknown-linux-gnu=${CMAKE_CXX_COMPILER}"
            "CXX_aarch64_unknown_linux_gnu=${CMAKE_CXX_COMPILER}"
            "AR_aarch64-unknown-linux-gnu=${TOKENIZERS_TOOLCHAIN_BIN}/${TOKENIZERS_TRIPLE}-ar"
            "AR_aarch64_unknown_linux_gnu=${TOKENIZERS_TOOLCHAIN_BIN}/${TOKENIZERS_TRIPLE}-ar"
            "CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=${CMAKE_C_COMPILER}"
            ${CMAKE_COMMAND} --build .
    )
else()
    set(TOKENIZERS_BUILD_COMMAND ${CMAKE_COMMAND} --build .)
endif()

ExternalProject_Add(
    tokenizers_external

    SOURCE_DIR ${TOKENIZERS_SOURCE_DIR}
    DOWNLOAD_COMMAND ${TOKENIZERS_DOWNLOAD_COMMAND}
    PATCH_COMMAND ${TOKENIZERS_PATCH_COMMAND}

    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${TOKENIZERS_INSTALL_DIR}

    BUILD_COMMAND ${TOKENIZERS_BUILD_COMMAND}
    INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install

    UPDATE_COMMAND ""
    BUILD_ALWAYS OFF

    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON
)

add_dependencies(third_build tokenizers_external)

add_library(tokenizers_cpp STATIC IMPORTED)
set_target_properties(tokenizers_cpp PROPERTIES
    IMPORTED_LOCATION ${TOKENIZERS_CPP_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${TOKENIZERS_HEADERS}"
)
add_dependencies(tokenizers_cpp tokenizers_external)

add_library(tokenizers_c STATIC IMPORTED)
set_target_properties(tokenizers_c PROPERTIES
    IMPORTED_LOCATION ${TOKENIZERS_C_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${TOKENIZERS_HEADERS}"
)
add_dependencies(tokenizers_c tokenizers_external)
target_link_libraries(tokenizers_c INTERFACE ${CMAKE_DL_LIBS} pthread)
