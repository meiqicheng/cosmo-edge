# Custom ARM GNU toolchain (gcc-arm-10.2-2020.11) cross toolchain for aarch64
# Host: x86_64 Linux (WSL). Target: aarch64-none-linux-gnu (glibc, self-contained sysroot).
SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_PROCESSOR aarch64)

SET(TOOLCHAIN_ROOT "/mnt/e/Gdsc/projects/dev/tools/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu"
    CACHE PATH "ARM GNU toolchain root (WSL path)")

SET(CMAKE_C_COMPILER "${TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-gcc" CACHE PATH "C compiler path" FORCE)
SET(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-g++" CACHE PATH "C++ compiler path" FORCE)
SET(CMAKE_AR "${TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-ar" CACHE PATH "ar path" FORCE)
SET(CMAKE_STRIP "${TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-strip" CACHE PATH "strip path" FORCE)
SET(CMAKE_RANLIB "${TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-ranlib" CACHE PATH "ranlib path" FORCE)

# The ARM GNU toolchain carries its own sysroot (aarch64-none-linux-gnu/libc),
# so system headers/libs resolve through the compiler itself.
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

add_definitions(-D__ARM_NEON)
add_compile_options(-fPIC)