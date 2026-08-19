#!/bin/sh
# Called by build_sophon_package.ps1 inside a Docker container to populate the
# Sophon build volume from a Windows-host bind mount.
set -eu

cp -a /src/. /workspace/

# Normalize CRLF -> LF for shell scripts and Python files.
# Git for Windows (core.autocrlf) can check out files that .gitattributes pins
# to LF with CRLF endings; the container then fails on the shebang or script
# parsing (e.g. "No such file or directory" running 3rd/srs-6.0-r0/trunk/configure).
# This covers:
#   - well-known names: configure/config/Configure, *.sh, *.pl, *.py
#   - any extensionless file whose first line is a shebang (#!): autotools
#     helpers like config.sub/config.guess/missing/install-sh/depcomp, etc.
# Only text files are touched; binary blobs (models, .so) are left untouched
# because they never start with a shebang.
# NOTE: this script runs in an Alpine (busybox) container, so use `tr -d '\r'`
# instead of `sed 's/\r$//'` (busybox sed treats \r as a literal, not a CR).
normalize_lf() {
    mode=$(stat -c %a "$1" 2>/dev/null || stat -f %Lp "$1")
    tr -d '\r' < "$1" > "$1.tmp" && mv "$1.tmp" "$1"
    chmod "$mode" "$1"
}

find /workspace/3rd /workspace/prebuild \
    -type f -size +0c -print0 2>/dev/null | while IFS= read -r -d '' f; do
    case "$f" in
        */configure|*/config|*/Configure|*.sh|*.pl|*.py) normalize_lf "$f" ;;
        *)
            # Extensionless: normalize only if the first line is a shebang.
            first=$(head -c 2 "$f" 2>/dev/null || true)
            if [ "$first" = "#!" ]; then
                normalize_lf "$f"
            fi
            ;;
    esac
done

# Remove directories we never need inside the build container, especially
# node_modules/ which contains Windows-native binaries (esbuild etc.) that
# would break the Linux builds.
for dir in build build_cpu build_cpu_verify build_cpu_windows build_output cmake-build-debug cmake-build-release .git node_modules src/web/node_modules; do
    rm -rf /workspace/$dir 2>/dev/null || true
done

echo "Source sync complete"
