#!/bin/bash
set -euo pipefail
export LC_ALL=C.UTF-8

# ── rustup/cargo bootstrap ────────────────────────────────────────────────
# tokenizers-c builds a Rust staticlib via cargo; rustup's shim lives in
# ~/.cargo/bin, which is only added to PATH by ~/.cargo/env in login shells.
# Make sure cargo/rustc are reachable regardless of how this script is run.
if [ -f "${HOME}/.cargo/env" ]; then
    # shellcheck disable=SC1091
    . "${HOME}/.cargo/env"
fi
if ! command -v cargo >/dev/null 2>&1; then
    echo "ERROR: cargo not found in PATH (source ~/.cargo/env or install rustup)" >&2
    exit 1
fi
cargo --version

RESOURCE_DIR=""
AXERA_ROOT_PATH="${AXERA_ROOT:-}"
TOOLCHAIN_FILE=""
BUILD_DIR_ARG=""
TARGET_CHIP="${COSMO_TARGET_CHIP:-ax650n}"
DEV_MODE=OFF
BUILD_TESTS_FLAG=OFF
while getopts "c:m:a:C:b:tT" opt; do
    case ${opt} in
        c) TOOLCHAIN_FILE="${OPTARG}" ;;
        m) RESOURCE_DIR="${OPTARG}" ;;
        a) AXERA_ROOT_PATH="${OPTARG}" ;;
        C) TARGET_CHIP="${OPTARG}" ;;
        b) BUILD_DIR_ARG="${OPTARG}" ;;
        t) DEV_MODE=ON ;;
        T) BUILD_TESTS_FLAG=ON ;;
        *) echo "Usage: $0 -a <axera-sdk-root> [-C <chip>] [-m <resource-dir>] [-c <toolchain-file>] [-b <build-dir>] [-t] [-T]"; exit 1 ;;
    esac
done

# Validate the target chip before configuring CMake.
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

# Phase 2: AX650 hardware media backend (VDEC/VENC via libax_*) with the CPU
# decoder/encoder/frame-proc compiled in as fault-tolerance fallbacks.
if [ -z "${TOOLCHAIN_FILE}" ]; then
    TOOLCHAIN_FILE="${PROJECT_ROOT_PATH}/toolchains/aarch64-axera.toolchain.cmake"
fi
RESOURCE_MODELS_DIR="${RESOURCE_DIR}/models"
RESOURCE_OVERLAY_DIR="${RESOURCE_DIR}"
if [ -n "${BUILD_DIR_ARG}" ]; then
    # Allow an out-of-tree build dir to avoid filling up the Windows drive
    # with drvfs build artifacts.
    BUILD_DIR="${BUILD_DIR_ARG}"
else
    BUILD_DIR="${PROJECT_ROOT_PATH}/build_axera"
fi
INSTALL_DIR="${BUILD_DIR}/install"
mkdir -p "${BUILD_DIR}"
rm -rf "${INSTALL_DIR}"

cmake -S "${PROJECT_ROOT_PATH}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    ${TOOLCHAIN_FILE:+-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"} \
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

# Only symlink compile_commands.json into the project root when the build
# dir lives inside the project (out-of-tree builds stay fully private).
case "${BUILD_DIR}" in
    "${PROJECT_ROOT_PATH}"/*)
        ln -sf "${BUILD_DIR}/compile_commands.json" "${PROJECT_ROOT_PATH}/compile_commands.json" 2>/dev/null || true
        ;;
esac
cmake --build "${BUILD_DIR}" --target install -j"$(nproc)"
if [ "${BUILD_TESTS_FLAG}" = "ON" ]; then
    cmake --build "${BUILD_DIR}" --target cosmo-tests -j"$(nproc)"
fi
cmake --build "${BUILD_DIR}" --target package_all
