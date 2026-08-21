set(SRS_ORIGINAL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd/srs-6.0-r0/trunk)
set(SRS_SOURCE_DIR ${SRS_ORIGINAL_SOURCE_DIR})
set(SRS_INSTALL_DIR ${THIRDPARTY_INSTALL_PREFIX}/srs)

if(COSMO_TARGET_ARCH STREQUAL "aarch64")
    # Derive the SRS cross toolchain from the active CMake compiler instead of
    # hardcoding the distro prefix "aarch64-linux-gnu-".  This works with both
    # the ARM GNU Toolchain (aarch64-none-linux-gnu-*) and apt cross packages:
    #   /path/to/bin/aarch64-none-linux-gnu-gcc -> prefix aarch64-none-linux-gnu-
    get_filename_component(SRS_TOOLCHAIN_BIN ${CMAKE_C_COMPILER} DIRECTORY)
    get_filename_component(SRS_CC_NAME ${CMAKE_C_COMPILER} NAME)
    string(REGEX REPLACE "-gcc$" "" SRS_TRIPLE ${SRS_CC_NAME})
    set(SRS_CROSS_PREFIX "${SRS_TOOLCHAIN_BIN}/${SRS_TRIPLE}-")

    set(SRS_DOWNLOAD_COMMAND "")
    set(SRS_PATCH_COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_srs_crossbuild.sh <SOURCE_DIR> ${SRS_CROSS_PREFIX})
    set(SRS_CONFIGURE_ARCH_ARGS
        --cross=on
        --cc=${CMAKE_C_COMPILER}
        --cxx=${CMAKE_CXX_COMPILER}
        --ar=${SRS_CROSS_PREFIX}ar
        --ld=${SRS_CROSS_PREFIX}ld
        --randlib=${SRS_CROSS_PREFIX}ranlib
        --arch=aarch64
        --host=${SRS_TRIPLE}
        --cross-prefix=${SRS_CROSS_PREFIX}
    )
    # SRS sub-builds (srtp2, st, ffmpeg-fit) resolve their cross compiler by
    # looking up "${host}-gcc" etc. on PATH; the ARM GNU Toolchain is usually
    # not on PATH, so export it for every configure/build invocation. Without
    # this the srtp2 sub-build falls back to the host gcc and produces an
    # x86-64 libsrtp2.a, which then fails to link into the aarch64 SRS.
    set(SRS_CROSS_ENV
        ${CMAKE_COMMAND} -E env "PATH=${SRS_TOOLCHAIN_BIN}:$ENV{PATH}"
    )
elseif(COSMO_TARGET_ARCH STREQUAL "x86_64")
    set(SRS_SOURCE_DIR ${CMAKE_BINARY_DIR}/srs_source)
    set(SRS_DOWNLOAD_COMMAND
        ${CMAKE_COMMAND} -E rm -rf <SOURCE_DIR>
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${SRS_ORIGINAL_SOURCE_DIR} <SOURCE_DIR>
    )
    set(SRS_PATCH_COMMAND ${CMAKE_COMMAND} -E true)
    set(SRS_CONFIGURE_ARCH_ARGS
        --cross=off
        --cc=${CMAKE_C_COMPILER}
        --cxx=${CMAKE_CXX_COMPILER}
        --arch=x86_64
    )
    # Native x86 build: no PATH prefix needed.
    set(SRS_CROSS_ENV "")
endif()

# Build the actual command lines. For aarch64 we prefix with
# `cmake -E env PATH=<toolchain-bin>:...` so SRS sub-builds (srtp2, st,
# ffmpeg-fit) find the cross compiler on PATH. For x86 there is no prefix:
# a bare "cmake -E true" would swallow the real command, so build the
# commands explicitly per architecture.
# Parallelize the SRS build: without -j, gmake falls back to -j1 and the
# full SRS compile (incl. bundled ffmpeg) takes tens of minutes.
include(ProcessorCount)
ProcessorCount(SRS_NPROC)
if(NOT SRS_NPROC)
    set(SRS_NPROC 1)
endif()
if(COSMO_TARGET_ARCH STREQUAL "aarch64")
    set(SRS_CONFIGURE_CMD ${SRS_CROSS_ENV} <SOURCE_DIR>/configure)
    # NOTE: use ${CMAKE_MAKE_PROGRAM} instead of $(MAKE) here. ExternalProject
    # only expands a leading "$(MAKE)" (regex ^$(MAKE)); once prefixed by
    # `cmake -E env ...` the literal "$(MAKE)" would be executed as a program
    # name and fail with "No such file or directory".
    set(SRS_BUILD_CMD ${SRS_CROSS_ENV} ${CMAKE_MAKE_PROGRAM} -j${SRS_NPROC})
    set(SRS_INSTALL_CMD ${SRS_CROSS_ENV} ${CMAKE_MAKE_PROGRAM} -j${SRS_NPROC} install)
else()
    set(SRS_CONFIGURE_CMD <SOURCE_DIR>/configure)
    set(SRS_BUILD_CMD ${CMAKE_MAKE_PROGRAM} -j${SRS_NPROC})
    set(SRS_INSTALL_CMD ${CMAKE_MAKE_PROGRAM} -j${SRS_NPROC} install)
endif()

# SRS uses its own ./configure && make build system (not CMake).
# Build artifacts go into ${SRS_SOURCE_DIR}/objs/ as SRS does not reliably
# support true out-of-source builds (internal scripts use relative paths).
# The objs/ directory is already covered by 3rd/srs-6.0-r0/.gitignore.
ExternalProject_Add(
    srs_external

    SOURCE_DIR ${SRS_SOURCE_DIR}
    DOWNLOAD_COMMAND ${SRS_DOWNLOAD_COMMAND}

    # Patch: bypass native tool checks (g++, unzip, pkg-config) that are
    # irrelevant for cross-compilation. The Docker build env only has the
    # aarch64 cross-toolchain, not all native host tools.
    PATCH_COMMAND ${SRS_PATCH_COMMAND}

    CONFIGURE_COMMAND ${SRS_CONFIGURE_CMD}
        --prefix=${SRS_INSTALL_DIR}
        ${SRS_CONFIGURE_ARCH_ARGS}
        --srt=off
        --rtc=on
        --h265=on
        --ffmpeg-fit=on
        --sanitizer=off
        --nasm=off
        --srtp-nasm=off
        --utest=off
        --jobs=4
        COMMAND ${CMAKE_COMMAND}
            "-DSRS_AUTO_HEADERS=<SOURCE_DIR>/objs/srs_auto_headers.hpp"
            "-DSRS_BUILD_EPOCH=${COSMO_REPRODUCIBLE_BUILD_EPOCH}"
            "-DSRS_BUILD_DATE=${COSMO_REPRODUCIBLE_BUILD_UTC}"
            "-DSRS_BUILD_UNAME=${COSMO_REPRODUCIBLE_BUILD_UNAME}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/normalize_srs_build_metadata.cmake

    BUILD_COMMAND ${SRS_BUILD_CMD}
    BUILD_IN_SOURCE ON
    INSTALL_COMMAND ${SRS_INSTALL_CMD}

    UPDATE_COMMAND ""
    BUILD_ALWAYS OFF

    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON
)

add_dependencies(third_build srs_external)

# Install SRS binary
install(PROGRAMS ${SRS_SOURCE_DIR}/objs/srs DESTINATION bin)

# Install a runtime template so SRS logs and its PID follow the package-specific
# mutable data root and any explicit COSMO_DATA_DIR override.
install(FILES ${CMAKE_CURRENT_LIST_DIR}/srs.conf.in
    DESTINATION bin/srs_conf
    RENAME srs.conf)
