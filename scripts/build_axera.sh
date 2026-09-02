#!/bin/bash
set -euo pipefail
export LC_ALL=C.UTF-8

# ── rustup/cargo 引导 ─────────────────────────────────────────────────────
# tokenizers-c 通过 cargo 构建 Rust 静态库；rustup 的 shim 位于
# ~/.cargo/bin，只有登录 shell 会通过 ~/.cargo/env 把它加进 PATH。
# 保证无论以何种方式运行本脚本，cargo/rustc 都可达。
if [ -f "${HOME}/.cargo/env" ]; then
    # shellcheck disable=SC1091
    . "${HOME}/.cargo/env"
fi
if ! command -v cargo >/dev/null 2>&1; then
    echo "ERROR: cargo not found in PATH (source ~/.cargo/env or install rustup)" >&2
    exit 1
fi
cargo --version

BUILD_JOBS="${COSMO_BUILD_JOBS:-$(nproc)}"
if ! [[ "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: COSMO_BUILD_JOBS must be a positive integer" >&2
    exit 1
fi

RESOURCE_DIR=""
TARGET_CHIP="${COSMO_TARGET_CHIP:-ax650n}"
AXERA_ROOT_PATH="${AXERA_ROOT:-}"
AXERA_TOOLCHAIN_PATH="${AXERA_TOOLCHAIN:-/opt/axera/toolchain/gcc-arm-9.2}"
DEV_MODE=OFF
BUILD_TESTS_FLAG=OFF
BUILD_DIR_ARG=""
while getopts "m:a:C:b:tT" opt; do
    case ${opt} in
        m) RESOURCE_DIR="${OPTARG}" ;;
        a) AXERA_ROOT_PATH="${OPTARG}" ;;
        C) TARGET_CHIP="${OPTARG}" ;;
        b) BUILD_DIR_ARG="${OPTARG}" ;;
        t) DEV_MODE=ON ;;
        T) BUILD_TESTS_FLAG=ON ;;
        *) echo "Usage: $0 -a <axera-sdk-root> [-C <chip>] [-m <resource-dir>] [-b <build-dir>] [-t] [-T]"; exit 1 ;;
    esac
done

# 配置 CMake 前先校验目标芯片。
case "${TARGET_CHIP}" in
    ax650n) ;;
    *) echo "ERROR: unsupported chip '${TARGET_CHIP}' (expected ax650n)" >&2; exit 1 ;;
esac

if [ -z "${PROJECT_ROOT_PATH:-}" ]; then
    PROJECT_ROOT_PATH=$(cd "$(dirname "$0")/.." && pwd)
fi

if [ -z "${AXERA_ROOT_PATH}" ]; then
    echo "ERROR: pass -a <path> or set AXERA_ROOT (AX650 SDK msp/out directory)" >&2
    exit 1
fi

if [ -z "${RESOURCE_DIR}" ]; then
    RESOURCE_DIR="${PROJECT_ROOT_PATH}/data/resource/aiboxresource_ax650n"
elif [ "${RESOURCE_DIR#/}" = "${RESOURCE_DIR}" ]; then
    RESOURCE_DIR="${PROJECT_ROOT_PATH}/${RESOURCE_DIR}"
fi

# Phase 2：AX650 硬件媒体后端（经 libax_* 的 VDEC/VENC），同时把 CPU
# 解码/编码/frame-proc 编译进去作为容错回退。
# 交叉工具链在生成出的 toolchain 文件里声明（放在构建目录内，不入仓库）：
# ExternalProject 子项目（curl、cryptopp、libevent 等）会用
# ${CMAKE_TOOLCHAIN_FILE} 重新配置，所以整条第三方链必须继承与父工程
# 相同的交叉工具链。AXERA_TOOLCHAIN 指向镜像内的 ARM GNU 工具链根目录。
RESOURCE_MODELS_DIR="${RESOURCE_DIR}/models"
RESOURCE_OVERLAY_DIR="${RESOURCE_DIR}"

if [ -n "${BUILD_DIR_ARG}" ]; then
    # 允许树外构建目录（容器内 ext4 卷），避免把构建产物落在 drvfs 挂载上：
    # drvfs 的时间戳/rename 语义会破坏 OpenSSL 的 Makefile 再生成判断，
    # 且整条编译链路的 I/O 在 drvfs 上显著变慢。
    BUILD_DIR="${BUILD_DIR_ARG}"
else
    # 默认仓库内目录，供裸机 Linux 直跑使用（树内构建才回链 compile_commands.json）。
    BUILD_DIR="${PROJECT_ROOT_PATH}/build_axera"
fi

INSTALL_DIR="${BUILD_DIR}/install"
mkdir -p "${BUILD_DIR}"
rm -rf "${INSTALL_DIR}"

TOOLCHAIN_FILE="${BUILD_DIR}/axera-toolchain.cmake"
cat > "${TOOLCHAIN_FILE}" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-g++")
set(CMAKE_AR "${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-ar")
set(CMAKE_STRIP "${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-strip")
set(CMAKE_RANLIB "${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-ranlib")
set(CMAKE_NM "${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-nm")
set(CMAKE_READELF "${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-readelf")
set(CMAKE_SYSROOT "${AXERA_TOOLCHAIN_PATH}/aarch64-none-linux-gnu/libc")
set(CMAKE_FIND_ROOT_PATH "${AXERA_TOOLCHAIN_PATH}/aarch64-none-linux-gnu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

cmake -S "${PROJECT_ROOT_PATH}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-gcc" \
    -DCMAKE_CXX_COMPILER="${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-g++" \
    -DCMAKE_AR="${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-ar" \
    -DCMAKE_STRIP="${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-strip" \
    -DCMAKE_RANLIB="${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-ranlib" \
    -DCMAKE_NM="${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-nm" \
    -DCMAKE_READELF="${AXERA_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu-readelf" \
    -DCMAKE_SYSROOT="${AXERA_TOOLCHAIN_PATH}/aarch64-none-linux-gnu/libc" \
    -DCMAKE_FIND_ROOT_PATH="${AXERA_TOOLCHAIN_PATH}/aarch64-none-linux-gnu" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_C_FLAGS="-D__ARM_NEON -fPIC" \
    -DCMAKE_CXX_FLAGS="-D__ARM_NEON -fPIC" \
    -DCOSMO_TARGET_ARCH=aarch64 \
    -DCOSMO_TARGET_CHIP="${TARGET_CHIP}" \
    -DCOSMO_NN_USE_SOPHON_BACKEND=OFF \
    -DCOSMO_NN_USE_CPU_BACKEND=OFF \
    -DCOSMO_NN_USE_RKNN_BACKEND=OFF \
    -DCOSMO_NN_USE_AXERA_BACKEND=ON \
    -DCOSMO_MEDIA_USE_SOPHON_BACKEND=OFF \
    -DCOSMO_MEDIA_USE_CPU_BACKEND=OFF \
    -DCOSMO_MEDIA_USE_ROCKCHIP_BACKEND=OFF \
    -DCOSMO_MEDIA_USE_AXERA_BACKEND=ON \
    -DCOSMO_AXERA_ROOT="${AXERA_ROOT_PATH}" \
    -DCOSMO_DEV_MODE="${DEV_MODE}" \
    -DBUILD_TESTS="${BUILD_TESTS_FLAG}" \
    -DRESOURCE_DIR="${RESOURCE_DIR}" \
    -DRESOURCE_OVERLAY_DIR="${RESOURCE_OVERLAY_DIR}" \
    -DRESOURCE_MODELS_DIR="${RESOURCE_MODELS_DIR}"

# 仅当构建目录位于工程内时才把 compile_commands.json 回链到项目根
# （树外构建保持完全私有）。
case "${BUILD_DIR}" in
    "${PROJECT_ROOT_PATH}"/*)
        ln -sf "${BUILD_DIR}/compile_commands.json" "${PROJECT_ROOT_PATH}/compile_commands.json" 2>/dev/null || true
        ;;
esac
cmake --build "${BUILD_DIR}" --target install -j"${BUILD_JOBS}"
if [ "${BUILD_TESTS_FLAG}" = "ON" ]; then
    cmake --build "${BUILD_DIR}" --target cosmo-tests -j"${BUILD_JOBS}"
fi
cmake --build "${BUILD_DIR}" --target package_all
