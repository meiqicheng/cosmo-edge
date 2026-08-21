#!/usr/bin/env bash
set -euo pipefail

# CosmoEdge AX650N package build entrypoint (runs inside the AXERA builder
# container, mirrors scripts/build_rockchip_package.sh).
#
#   Usage: build_axera_package.sh [--chip <ax650n>] [--models <include|preserve>]
#
# The AX650 SDK root and the ARM GNU toolchain root come from the builder
# lock (config/axera-build/builder-lock.json, staged into the image at
# /opt/cosmo/axera-builder-lock.json).

chip="ax650n"
package_models="include"

while (($#)); do
    case "$1" in
        --chip)
            if (($# < 2)); then
                echo "ERROR: --chip requires ax650n" >&2
                exit 2
            fi
            chip="$2"
            shift 2
            ;;
        --models)
            if (($# < 2)); then
                echo "ERROR: --models requires include or preserve" >&2
                exit 2
            fi
            package_models="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--chip <ax650n>] [--models <include|preserve>]"
            exit 0
            ;;
        *)
            echo "ERROR: unsupported argument: $1" >&2
            exit 2
            ;;
    esac
done

case "${chip}" in
    ax650n) ;;
    *)
        echo "ERROR: unsupported AXERA target '${chip}'; expected ax650n" >&2
        exit 2
        ;;
esac
case "${package_models}" in
    include|preserve) ;;
    *)
        echo "ERROR: unsupported model policy '${package_models}'; expected include or preserve" >&2
        exit 2
        ;;
esac

if [ -z "${PROJECT_ROOT_PATH:-}" ]; then
    PROJECT_ROOT_PATH=$(cd "$(dirname "$0")/.." && pwd -P)
fi
builder_lock="${PROJECT_ROOT_PATH}/config/axera-build/builder-lock.json"
image_lock="${COSMO_AXERA_BUILDER_LOCK:-/opt/cosmo/axera-builder-lock.json}"
if [ ! -f "${builder_lock}" ]; then
    echo "ERROR: AXERA builder lock is missing: ${builder_lock}" >&2
    exit 1
fi
if [ ! -f "${image_lock}" ]; then
    echo "ERROR: this container is not a locked CosmoEdge AXERA builder" >&2
    exit 1
fi
if ! cmp -s "${builder_lock}" "${image_lock}"; then
    echo "ERROR: builder image lock does not match this source checkout" >&2
    exit 1
fi

IFS=$'\t' read -r sdk_root toolchain_root < <(
    python3 - "${builder_lock}" "${chip}" <<'PY'
import json
import pathlib
import sys

lock = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
chip = sys.argv[2]
target = lock["targets"][chip]
values = (target["sdk_root"], target["toolchain_root"])
if any("\t" in str(value) or "\n" in str(value) for value in values):
    raise SystemExit("builder lock values must be single-line fields")
print("\t".join(str(value) for value in values))
PY
)

test -f "${sdk_root}/include/ax_engine_api.h"
test -f "${sdk_root}/lib/libax_engine.so"
test -x "${toolchain_root}/bin/aarch64-none-linux-gnu-gcc"

# Windows Git checkouts materialize Linux .so symlinks as tiny text files and
# convert third-party configure scripts to CRLF. Normalize before building so
# the linker resolves .so chains and configure scripts execute with LF.
"${PROJECT_ROOT_PATH}/scripts/restore-symlinks.sh" || true
find "${PROJECT_ROOT_PATH}/3rd/mp4v2-2.0.0" "${PROJECT_ROOT_PATH}/3rd/openssl-3.5.3" \
     "${PROJECT_ROOT_PATH}/3rd/curl-8.17.0" "${PROJECT_ROOT_PATH}/3rd/srs-6.0-r0" \
    -type f \( -name 'configure' -o -name 'config' -o -name 'Makefile.in' \
               -o -name '*.sh' -o -name '*.pl' -o -name '*.pm' -o -name '*.h' \) \
    -exec sed -i 's/\r$//' {} + 2>/dev/null || true

# Build inside the container's ext4 layer (or a persistent volume) instead
# of the drvfs workspace mount: CPack staging copies the whole install tree
# and the Windows drive may lack space. Override via COSMO_AXERA_BUILD_DIR.
# The build dir may itself be a volume mount point, so clear its contents
# rather than removing the mount.
build_dir="${COSMO_AXERA_BUILD_DIR:-/opt/axera/build}"
mkdir -p "${build_dir}"
find "${build_dir}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
COSMO_PACKAGE_MODELS="${package_models}" \
    "${PROJECT_ROOT_PATH}/scripts/build_axera.sh" \
        -a "${sdk_root}" \
        -c "${PROJECT_ROOT_PATH}/toolchains/aarch64-axera.toolchain.cmake" \
        -C "${chip}" \
        -b "${build_dir}" \
        -T

shopt -s nullglob
packages=("${build_dir}"/packages/*.tar.gz)
if ((${#packages[@]} != 1)) || [ ! -f "${packages[0]:-}" ] || [ -L "${packages[0]:-}" ]; then
    echo "ERROR: expected exactly one regular ${chip} package artifact" >&2
    exit 1
fi

package="${packages[0]}"
python3 "${PROJECT_ROOT_PATH}/scripts/verify_package_contents.py" \
    --archive "$(cd "$(dirname "${package}")" && pwd -P)/$(basename "${package}")" \
    --build-profile public-runtime \
    --target-chip "${chip}" \
    --target-policy-lock "${builder_lock}"

output_root="${COSMO_BUILD_OUTPUT_ROOT:-/build_output}"
output_dir="${output_root}/${chip}"
rm -rf "${output_dir}"
mkdir -p "${output_dir}"
cp -f -- "${package}" "${output_dir}/"
printf '%s\n' "${chip}" > "${output_dir}/TARGET_CHIP"
(
    cd "${output_dir}"
    sha256sum -- "$(basename "${package}")" > SHA256SUMS
    sha256sum -c SHA256SUMS
)
ls -lh "${output_dir}"
