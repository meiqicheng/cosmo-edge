#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 <rknn-runtime-root> [output-dir]" >&2
    exit 2
fi

runtime_root="$(cd "$1" && pwd)"
output_dir="${2:-$PWD/build/rknn-model-runner}"
# Use CXX if given, else auto-detect the aarch64 cross compiler (ARM GNU
# Toolchain first, then the distro apt package).
if [[ -n "${CXX:-}" ]]; then
    cxx="$CXX"
elif command -v aarch64-none-linux-gnu-g++ >/dev/null 2>&1; then
    cxx=aarch64-none-linux-gnu-g++
else
    cxx=aarch64-linux-gnu-g++
fi
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_file="$repo_root/tools/rknn/rknn_model_runner.cc"

for required in "$runtime_root/include/rknn_api.h" "$runtime_root/lib/librknnrt.so" "$source_file"; do
    if [[ ! -f "$required" ]]; then
        echo "Missing build input: $required" >&2
        exit 1
    fi
done
if ! command -v "$cxx" >/dev/null 2>&1; then
    echo "Cross compiler not found: $cxx" >&2
    exit 1
fi

install -d "$output_dir/bin" "$output_dir/lib"
"$cxx" -std=c++17 -O2 -Wall -Wextra -Werror \
    -I"$runtime_root/include" \
    "$source_file" \
    -L"$runtime_root/lib" -lrknnrt \
    -Wl,-rpath,'$ORIGIN/../lib' \
    -o "$output_dir/bin/rknn-model-runner"
cp -p "$runtime_root/lib/librknnrt.so" "$output_dir/lib/librknnrt.so"

file "$output_dir/bin/rknn-model-runner" "$output_dir/lib/librknnrt.so"
sha256sum "$output_dir/bin/rknn-model-runner" "$output_dir/lib/librknnrt.so"
