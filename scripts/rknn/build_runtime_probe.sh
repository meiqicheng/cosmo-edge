#!/usr/bin/env bash

set -euo pipefail

usage() {
    echo "Usage: $0 <rknn-runtime-root> [output-dir]" >&2
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage
    exit 2
fi

runtime_root="$(cd "$1" && pwd)"
output_dir="${2:-$PWD/build/rknn-runtime-probe}"
cxx="${CXX:-aarch64-linux-gnu-g++}"

header="$runtime_root/include/rknn_api.h"
library="$runtime_root/lib/librknnrt.so"
source_file="$(cd "$(dirname "$0")/../.." && pwd)/tools/rknn/rknn_runtime_probe.cc"

for required in "$header" "$library" "$source_file"; do
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
    -o "$output_dir/bin/rknn-runtime-probe"
cp -p "$library" "$output_dir/lib/librknnrt.so"

file "$output_dir/bin/rknn-runtime-probe" "$output_dir/lib/librknnrt.so"
sha256sum "$output_dir/bin/rknn-runtime-probe" "$output_dir/lib/librknnrt.so"
