# AXERA AX650N cross toolchain (ARM GNU gcc-arm-9.2) for aarch64.
# Host: x86_64 Linux (WSL). Target: aarch64-none-linux-gnu (glibc, self-contained sysroot).
# The toolchain is shared with AXERA ax-pipeline (`.ci/toolchains/`); a copy is
# kept at /opt/axera/toolchain to avoid non-ASCII path issues in third-party
# configure scripts (srs, openssl, mp4v2).
SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_PROCESSOR aarch64)

SET(AXERA_TOOLCHAIN_ROOT "/opt/axera/toolchain/gcc-arm-9.2"
    CACHE PATH "AXERA ARM GNU toolchain root (WSL path)")

SET(CMAKE_C_COMPILER "${AXERA_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-gcc" CACHE PATH "C compiler path" FORCE)
SET(CMAKE_CXX_COMPILER "${AXERA_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-g++" CACHE PATH "C++ compiler path" FORCE)
SET(CMAKE_AR "${AXERA_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-ar" CACHE PATH "ar path" FORCE)
SET(CMAKE_STRIP "${AXERA_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-strip" CACHE PATH "strip path" FORCE)
SET(CMAKE_RANLIB "${AXERA_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-ranlib" CACHE PATH "ranlib path" FORCE)
SET(CMAKE_NM "${AXERA_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-nm" CACHE PATH "nm path" FORCE)
SET(CMAKE_READELF "${AXERA_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-readelf" CACHE PATH "readelf path" FORCE)

# ARM GNU toolchain ships a self-contained glibc sysroot; lock all sub-builds
# (curl ExternalProject etc.) to it so host /usr/include is never probed.
SET(CMAKE_SYSROOT "${AXERA_TOOLCHAIN_ROOT}/aarch64-none-linux-gnu/libc" CACHE PATH "sysroot" FORCE)
SET(CMAKE_FIND_ROOT_PATH "${AXERA_TOOLCHAIN_ROOT}/aarch64-none-linux-gnu" CACHE PATH "find root" FORCE)

# The ARM GNU toolchain carries its own sysroot (aarch64-none-linux-gnu/libc),
# so system headers/libs resolve through the compiler itself.
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

add_definitions(-D__ARM_NEON)
add_compile_options(-fPIC)
