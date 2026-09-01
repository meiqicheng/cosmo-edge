#!/usr/bin/env sh
set -eu

expected='3e1c5b97cd5bcc081e47ec631f84c36e72f075c8b9da6a19de3d9705fb887f92'
output=${1:-'controlled-1080p24-sample.mp4'}

if [ ! -f "$output" ]; then
  echo "Provide the licensed sample as $output, then rerun this verifier." >&2
  exit 1
fi

actual=$(sha256sum "$output" | awk '{print $1}')
if [ "$actual" != "$expected" ]; then
  echo "SHA-256 mismatch: expected $expected, got $actual" >&2
  exit 1
fi
echo "Verified $output"
