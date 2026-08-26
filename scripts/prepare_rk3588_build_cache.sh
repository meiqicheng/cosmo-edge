#!/usr/bin/env bash
# Prepare the gitignored RK3588 build cache (.rk3588-build-cache/) from the
# pinned identities in config/rockchip-build/source-lock-rk3588.json.
#
# Subcommands:
#   prepare        (default) clone/verify everything needed by Dockerfile.rk3588-bullseye
#   --verify-only  validate an existing cache against the lock; no network access
#   --preflight    check docker and the pinned base image digest are reachable
#
# The cache is reproducible: MPP is cloned at an exact commit; librga.so and
# the RKLLM runtime are user-supplied drops whose sha256 must match the lock.
# Every mismatch fails fast with actionable instructions.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOCK_FILE="${REPO_ROOT}/config/rockchip-build/source-lock-rk3588.json"

MODE="prepare"
case "${1:-}" in
  "" | prepare) MODE="prepare" ;;
  --verify-only) MODE="verify" ;;
  --preflight) MODE="preflight" ;;
  *)
    echo "usage: $0 [prepare|--verify-only|--preflight]" >&2
    exit 64
    ;;
esac

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "FAIL: required tool '$1' is not installed or not on PATH" >&2
    exit 1
  }
}

lock_get() { # lock_get <python-expression-template using %s placeholders>
  need python3
  python3 -c '
import json, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
node = lock
for key in sys.argv[2].split("."):
    node = node[key]
print(node)
' "${LOCK_FILE}" "$1"
}

die() {
  echo "FAIL: $*" >&2
  exit 1
}

sha256_of() {
  need sha256sum
  sha256sum "$1" | cut -d' ' -f1
}

check_user_drop() { # check_user_drop <file> <expected_sha256> <what> <instructions>
  local file="$1" expected="$2" what="$3" instructions="$4"
  [[ -f "${file}" ]] || die "missing ${what} at ${file}
${instructions}"
  local actual
  actual="$(sha256_of "${file}")"
  [[ "${actual}" == "${expected}" ]] || die "${what} sha256 mismatch at ${file}
  expected ${expected}
  actual   ${actual}
${instructions}"
}

write_cache_manifest() {
  local cache_dir="$1"
  local manifest="${cache_dir}/cache-manifest.json"
  {
    echo "{"
    echo "  \"schema_version\": 1,"
    first=1
    for f in librga.so rkllm-runtime/lib/librkllmrt.so; do
      if [[ -f "${cache_dir}/${f}" ]]; then
        [[ ${first} -eq 0 ]] && echo ","
        printf '  "%s": "%s"' "${f}" "$(sha256_of "${cache_dir}/${f}")"
        first=0
      fi
    done
    echo ""
    echo "}"
  } >"${manifest}"
  echo "wrote ${manifest}"
}

prepare_mpp() {
  local cache_dir="$1" url commit subdir
  url="$(lock_get mpp.git_url)"
  commit="$(lock_get mpp.commit)"
  subdir="$(lock_get mpp.cache_subdir)"
  local dest="${cache_dir}/${subdir}"
  if [[ -d "${dest}/.git" ]]; then
    echo "mpp source already present at ${dest}"
  else
    need git
    echo "cloning ${url} at ${commit} ..."
    git clone "${url}" "${dest}" || die "could not clone ${url}; check network/proxy"
  fi
  git -C "${dest}" fetch origin "${commit}" >/dev/null 2>&1 ||
    git -C "${dest}" fetch origin >/dev/null 2>&1 ||
    die "could not fetch ${commit} from ${url}"
  git -C "${dest}" checkout -q "${commit}" ||
    die "commit ${commit} not reachable in ${dest}"
  local head
  head="$(git -C "${dest}" rev-parse HEAD)"
  [[ "${head}" == "${commit}" ]] || die "checked-out HEAD ${head} != locked ${commit}"
  echo "mpp source verified at ${commit}"
}

verify_user_drops() {
  local cache_dir="$1"
  local llm_sha llm_instructions
  llm_instructions="Place the user-supplied RKLLM v$(lock_get rkllm.version) runtime drop at ${cache_dir}/rkllm-runtime/{include/rkllm.h,lib/librkllmrt.so,LICENSE}; it is not downloadable."
  llm_sha="$(lock_get rkllm.librkllmrt_sha256)"
  check_user_drop "${cache_dir}/rkllm-runtime/lib/librkllmrt.so" "${llm_sha}" \
    "RKLLM runtime library" "${llm_instructions}"
  [[ -f "${cache_dir}/rkllm-runtime/include/rkllm.h" ]] ||
    die "missing rkllm-runtime/include/rkllm.h
${llm_instructions}"
  [[ -f "${cache_dir}/rkllm-runtime/LICENSE" ]] ||
    die "missing rkllm-runtime/LICENSE
${llm_instructions}"

  local rga_path rga_sha
  rga_path="$(lock_get librga.prebuilt_library.path)"
  if python3 -c '
import json, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
sys.exit(0 if lock["librga"]["prebuilt_library"].get("sha256") else 1)
' "${LOCK_FILE}"; then
    rga_sha="$(lock_get librga.prebuilt_library.sha256)"
    check_user_drop "${cache_dir}/${rga_path}" "${rga_sha}" \
      "librga prebuilt library" \
      "Place the official airockchip aarch64 prebuilt at ${cache_dir}/${rga_path}."
  elif [[ -f "${cache_dir}/${rga_path}" ]]; then
    echo "librga.so present ($(sha256_of "${cache_dir}/${rga_path}")); no locked sha256 recorded for it yet"
  else
    die "missing user-provided librga.so at ${cache_dir}/${rga_path}"
  fi
}

preflight() {
  need docker
  echo "docker: $(docker --version)"
  local digest="debian:bullseye@sha256:99cdf7792e25416bd801861ccd8e2fb27fb527b25e8d9a8704ebc3ead2015675"
  if docker manifest inspect "${digest}" >/dev/null 2>&1; then
    echo "base image digest reachable: ${digest}"
  else
    die "cannot inspect base image ${digest}; docker may be offline or the digest unpullable"
  fi
  echo "preflight OK"
}

main() {
  need python3
  [[ -f "${LOCK_FILE}" ]] || die "lock file not found: ${LOCK_FILE}"
  python3 -c 'import json,sys; json.load(open(sys.argv[1], encoding="utf-8"))' \
    "${LOCK_FILE}" || die "lock file is not valid JSON"

  if [[ "${MODE}" == "preflight" ]]; then
    preflight
    return 0
  fi

  local cache_dir="${REPO_ROOT}/$(lock_get cache_dir)"
  mkdir -p "${cache_dir}"

  if [[ "${MODE}" == "prepare" ]]; then
    prepare_mpp "${cache_dir}"
  else
    [[ -d "${cache_dir}/mpp-src/.git" ]] ||
      die "--verify-only: mpp source missing at ${cache_dir}/mpp-src (run without --verify-only)"
  fi

  verify_user_drops "${cache_dir}"
  write_cache_manifest "${cache_dir}"
  echo "RK3588 build cache OK (${MODE})"
}

main
