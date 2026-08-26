set(OPENSSL_ORIGINAL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd/openssl-3.5.3)
set(OPENSSL_SOURCE_DIR ${OPENSSL_ORIGINAL_SOURCE_DIR})
set(OPENSSL_BUILD_INSTALL_DIR ${THIRDPARTY_INSTALL_PREFIX}/openssl)
set(OPENSSL_INSTALL_DIR ${OPENSSL_BUILD_INSTALL_DIR})
set(OPENSSL_HEADERS ${OPENSSL_INSTALL_DIR}/include)
if(COSMO_MODEL_GUARD)
    set(OPENSSL_SSL_LIB ${OPENSSL_INSTALL_DIR}/lib/libssl.so.3)
    set(OPENSSL_CRYPTO_LIB ${OPENSSL_INSTALL_DIR}/lib/libcrypto.so.3)
else()
    set(OPENSSL_SSL_LIB ${OPENSSL_INSTALL_DIR}/lib/libssl.so)
    set(OPENSSL_CRYPTO_LIB ${OPENSSL_INSTALL_DIR}/lib/libcrypto.so)
endif()
set(OPENSSL_DOWNLOAD_COMMAND "")
set(OPENSSL_PATCH_COMMAND ${CMAKE_COMMAND} -E true)

# OpenSSL embeds its build time in libcrypto. Keep the dependency byte-for-byte
# compatible with the formally admitted Guard SDK instead of inheriting the
# wall clock of each clean build.
set(OPENSSL_REPRODUCIBLE_ENV
    ${CMAKE_COMMAND} -E env
    SOURCE_DATE_EPOCH=${COSMO_REPRODUCIBLE_BUILD_EPOCH}
)
set(OPENSSL_REPRODUCIBLE_MAKE_ARGS
    SOURCE_DATE_EPOCH=${COSMO_REPRODUCIBLE_BUILD_EPOCH}
)

if(COSMO_TARGET_ARCH STREQUAL "x86_64")
    set(OPENSSL_SOURCE_DIR ${CMAKE_BINARY_DIR}/openssl_source)
    set(OPENSSL_DOWNLOAD_COMMAND
        ${CMAKE_COMMAND} -E rm -rf <SOURCE_DIR>
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${OPENSSL_ORIGINAL_SOURCE_DIR} <SOURCE_DIR>
    )
    set(OPENSSL_PATCH_COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clean_openssl_source.sh <SOURCE_DIR>)
endif()

set(OPENSSL_COMMON_CONFIGURE_ARGS
    --prefix=${OPENSSL_BUILD_INSTALL_DIR}
    --openssldir=/usr/local/ssl
    --libdir=lib
    --release
    --api=1.0.2
    shared
    zlib-dynamic
    no-apps
    no-docs
    no-tests
)

if(COSMO_TARGET_ARCH STREQUAL "aarch64")
    # Derive the OpenSSL cross-compile prefix from the active CMake compiler
    # instead of hardcoding a distro package name. Example:
    #   /path/to/bin/aarch64-none-linux-gnu-gcc
    #     -> /path/to/bin/aarch64-none-linux-gnu-
    # This keeps OpenSSL building with the same toolchain selected by the
    # CMAKE_TOOLCHAIN_FILE (ARM GNU Toolchain, apt cross packages, etc.).
    get_filename_component(OPENSSL_TOOLCHAIN_BIN_DIR ${CMAKE_C_COMPILER} DIRECTORY)
    get_filename_component(OPENSSL_TOOLCHAIN_COMPILER_NAME ${CMAKE_C_COMPILER} NAME)
    string(REGEX REPLACE "-gcc$" "-" OPENSSL_TOOLCHAIN_PREFIX_NAME ${OPENSSL_TOOLCHAIN_COMPILER_NAME})
    set(OPENSSL_CROSS_COMPILE_PREFIX "${OPENSSL_TOOLCHAIN_BIN_DIR}/${OPENSSL_TOOLCHAIN_PREFIX_NAME}")

    set(OPENSSL_CONFIGURE_COMMAND
        <SOURCE_DIR>/config linux-aarch64
        ${OPENSSL_COMMON_CONFIGURE_ARGS}
        --cross-compile-prefix=${OPENSSL_CROSS_COMPILE_PREFIX}
        --with-zlib-include=${THIRDPARTY_INSTALL_PREFIX}/zlib/include
        --with-zlib-lib=${THIRDPARTY_INSTALL_PREFIX}/zlib/lib
    )
elseif(COSMO_TARGET_ARCH STREQUAL "x86_64")
    set(OPENSSL_CONFIGURE_COMMAND
        <SOURCE_DIR>/config linux-x86_64
        ${OPENSSL_COMMON_CONFIGURE_ARGS}
    )
endif()

ExternalProject_Add(
    openssl_external
    
    SOURCE_DIR ${OPENSSL_SOURCE_DIR}
    DOWNLOAD_COMMAND ${OPENSSL_DOWNLOAD_COMMAND}
    PATCH_COMMAND ${OPENSSL_PATCH_COMMAND}

    CONFIGURE_COMMAND ${OPENSSL_REPRODUCIBLE_ENV}
        ${OPENSSL_CONFIGURE_COMMAND}
    
    # Keep $(MAKE) literal so the generated parent recipe forwards GNU Make's
    # jobserver to OpenSSL's recursive _build_sw invocation. Passing the epoch
    # as a make variable preserves the reproducible-build environment without
    # inserting a process wrapper that closes the jobserver file descriptors.
    # -j1 is mandatory for the aarch64 in-source build: the parent -jN jobserver
    # penetrates OpenSSL's internal make and races header regeneration against
    # compilation (see docs/development/rk3588-support-plan.md).
    BUILD_COMMAND $(MAKE) -j1 ${OPENSSL_REPRODUCIBLE_MAKE_ARGS}
    INSTALL_COMMAND $(MAKE) -j1 ${OPENSSL_REPRODUCIBLE_MAKE_ARGS} install_sw
    
    UPDATE_COMMAND ""
    BUILD_ALWAYS OFF

    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON
)
add_dependencies(third_build openssl_external)
add_dependencies(openssl_external z_external)

add_library(openssl_ssl SHARED IMPORTED)
set_target_properties(openssl_ssl PROPERTIES
    IMPORTED_LOCATION ${OPENSSL_SSL_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_HEADERS}"
)
add_dependencies(openssl_ssl openssl_external)

add_library(openssl_crypto SHARED IMPORTED)
set_target_properties(openssl_crypto PROPERTIES
    IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_HEADERS}"
)
add_dependencies(openssl_crypto openssl_external)

if(COSMO_MODEL_GUARD)
    install(FILES
        ${OPENSSL_INSTALL_DIR}/lib/libcrypto.so.3
        ${OPENSSL_INSTALL_DIR}/lib/libssl.so.3
        DESTINATION lib)
else()
    install(DIRECTORY ${OPENSSL_INSTALL_DIR}/lib/
        DESTINATION lib
        FILES_MATCHING
            PATTERN "*so*"
    )
endif()
