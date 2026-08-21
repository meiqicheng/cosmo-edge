set(CURL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd/curl-8.17.0)
set(CURL_INSTALL_DIR ${THIRDPARTY_INSTALL_PREFIX}/curl)
set(CURL_HEADERS ${CURL_INSTALL_DIR}/include)
set(CURL_LIB ${CURL_INSTALL_DIR}/lib/libcurl.so)
set(CURL_EXTERNAL_DEPENDS openssl_external z_external)

ExternalProject_Add(
    curl_external

    SOURCE_DIR ${CURL_SOURCE_DIR}

    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${CURL_INSTALL_DIR}
        -DCMAKE_SYSROOT=${CMAKE_SYSROOT}
        -DCMAKE_FIND_ROOT_PATH=${CMAKE_FIND_ROOT_PATH}
        # Escaped semicolons survive ExternalProject arg splitting; without
        # this, CMAKE_IGNORE_PATH lands as two separate args and the host
        # /usr/include is probed during curl's configure-time checks, which
        # then breaks the glibc sysroot at compile time.
        "-DCMAKE_IGNORE_PATH=/usr/include\\;/usr/local/include"
        # curl's configure-time header checks record the host /usr/include
        # path into flags.make, which then breaks the glibc sysroot during
        # compilation. Pin the sysroot on the compiler flags so the cross
        # sysroot always wins over the recorded host include path.
        "-DCMAKE_C_FLAGS=--sysroot=${CMAKE_SYSROOT}"
        "-DCMAKE_CXX_FLAGS=--sysroot=${CMAKE_SYSROOT}"
        # Cross builds must not pick the host pthread headers (curl's Threads
        # detection records the host /usr/include path, which then breaks the
        # glibc sysroot during compilation). Disabling the threaded resolver
        # skips find_package(Threads) entirely so HAVE_PTHREAD_H stays off and
        # curl compiles cleanly on the sysroot.
        -DENABLE_THREADED_RESOLVER=OFF
        -DOPENSSL_ROOT_DIR=${OPENSSL_INSTALL_DIR}
        -DOPENSSL_INCLUDE_DIR=${OPENSSL_HEADERS}
        -DOPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIB}
        -DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIB}
        -DZLIB_ROOT=${Z_INSTALL_DIR}
        -DZLIB_INCLUDE_DIR=${Z_INSTALL_DIR}/include
        -DZLIB_LIBRARY=${Z_INSTALL_DIR}/lib/libz.so
        -DBUILD_SHARED_LIBS=ON
        -DCURL_USE_LIBPSL=OFF
        # Optional codecs are not bundled for the cross build; disable them so
        # configure does not pick up host libraries that are missing in the
        # ARM GNU sysroot (e.g. brotli/decode.h).
        -DCURL_BROTLI=OFF
        -DCURL_ZSTD=OFF
        -DCURL_ZLIB=ON
        # Cross-compilation skips curl's host CA auto-detection. This path is
        # resolved on the target at runtime and must be provided by the image.
        -DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt
        -DCURL_CA_PATH=none
        -DBUILD_LIBCURL_DOCS=OFF
        -DBUILD_MISC_DOCS=OFF
        -DENABLE_CURL_MANUAL=OFF
        -DBUILD_TESTING=OFF
        -DBUILD_CURL_EXE=OFF
        -DBUILD_EXAMPLES=OFF
    
    INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install

    # curl's configure-time checks record the host /usr/include into
    # flags.make (the ARM GNU sysroot is glibc-based and compiles fine, but a
    # host glibc >= 2.36 stdio.h with __attr_dealloc breaks gcc 9.2). Strip
    # any host include dirs before compiling so only the cross sysroot is used.
    BUILD_COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cmake/curl_build_wrapper.sh
        <BINARY_DIR>

    DEPENDS ${CURL_EXTERNAL_DEPENDS}

    UPDATE_COMMAND ""
    BUILD_ALWAYS OFF

    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON
)

add_dependencies(third_build curl_external)

add_library(curl SHARED IMPORTED)
set_target_properties(curl PROPERTIES
    IMPORTED_LOCATION ${CURL_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${CURL_HEADERS}"
)
add_dependencies(curl curl_external)

install(DIRECTORY ${CURL_INSTALL_DIR}/lib/
    DESTINATION lib
    FILES_MATCHING
        PATTERN "*curl*"
        PATTERN "*so*"
)
