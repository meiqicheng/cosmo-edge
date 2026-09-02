#!/usr/bin/env bash
set -euo pipefail

# CosmoEdge AX650N 包构建入口（在 AXERA builder 容器内运行，
# 对标 scripts/build_rockchip_package.sh）。
#
#   用法: build_axera_package.sh [--chip <ax650n>] [--models <include|preserve>]
#
# AX650 SDK 根目录与 ARM GNU 工具链根目录来自 builder 锁文件
# （config/axera-build/builder-lock.json，构建镜像时暂存到
# /opt/cosmo/axera-builder-lock.json）。

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

# Windows Git 检出会把 Linux .so 符号链接物化为小文本文件，并把第三方
# configure 脚本转成 CRLF。构建前先归一化：让链接器正确解析 .so 链、
# configure 脚本以 LF 执行。
"${PROJECT_ROOT_PATH}/scripts/restore-symlinks.sh" || true
find "${PROJECT_ROOT_PATH}/3rd/mp4v2-2.0.0" "${PROJECT_ROOT_PATH}/3rd/openssl-3.5.3" \
     "${PROJECT_ROOT_PATH}/3rd/curl-8.17.0" "${PROJECT_ROOT_PATH}/3rd/srs-6.0-r0" \
    -type f \( -name 'configure' -o -name 'config' -o -name 'Makefile.in' \
               -o -name '*.sh' -o -name '*.pl' -o -name '*.pm' -o -name '*.h' \) \
    -exec sed -i 's/\r$//' {} + 2>/dev/null || true

# 在容器 ext4 层（或持久卷）内构建，而不是 drvfs workspace 挂载：
# CPack 暂存要拷贝整个 install 树，Windows 盘可能空间不足。
# 可用 COSMO_AXERA_BUILD_DIR 覆盖。构建目录本身可能是卷挂载点，
# 因此清空其内容而不是删除挂载点。
build_dir="${COSMO_AXERA_BUILD_DIR:-/opt/axera/build}"
mkdir -p "${build_dir}"
# COSMO_AXERA_KEEP_BUILD=1 resumes an interrupted run: keep the existing build
# tree (externals, objects, install) and only refresh the staged resources.
if [ -z "${COSMO_AXERA_KEEP_BUILD:-}" ]; then
    find "${build_dir}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
fi

# Windows 宿主（GBK ANSI 代码页）上 Docker Desktop 的文件共享会损坏
# 非 ASCII 文件名（正确的 UTF-8 字节被按 GBK 重新解码）。algorithm_template
# 资源带中文文件名，CPack 无法 lstat 损坏后的名字。把资源树拷贝到容器
# ext4，确定性地修复乱码（把名字字节按 GBK 解码即还原原 UTF-8 名），
# 再从修复后的副本构建。
# Stage INSIDE the persistent build volume: the container filesystem layer is
# discarded between `compose run` invocations, so a stage under /opt/axera
# would vanish before any resumed package_all step can use it.
resource_stage="${build_dir}/resource-stage"
rm -rf "${resource_stage}"
python3 - "${PROJECT_ROOT_PATH}/data/resource/aiboxresource_ax650n" "${resource_stage}" <<'PY'
import os
import shutil
import sys

src_root, dst_root = os.fsencode(sys.argv[1]).decode(), os.fsencode(sys.argv[2]).decode()


def repair(name: str) -> str:
    try:
        fixed = name.encode("gbk").decode("utf-8")
    except (UnicodeEncodeError, UnicodeDecodeError):
        return name
    return fixed if fixed != name else name


total = 0
for root, dirs, files in os.walk(src_root):
    rel = os.path.relpath(root, src_root)
    dst_dir = dst_root if rel == "." else os.path.join(dst_root, repair(rel))
    os.makedirs(dst_dir, exist_ok=True)
    for name in dirs + files:
        if os.path.isdir(os.path.join(root, name)):
            continue
        shutil.copy2(os.path.join(root, name), os.path.join(dst_dir, repair(name)))
        total += 1
print(f"staged {total} resource files with repaired names")
PY

COSMO_PACKAGE_MODELS="${package_models}" \
    "${PROJECT_ROOT_PATH}/scripts/build_axera.sh" \
        -a "${sdk_root}" \
        -C "${chip}" \
        -b "${build_dir}" \
        -m "${resource_stage}" \
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
