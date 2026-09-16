# Cosmo Model Guard v2 SDK

This directory contains the complete Sophon Model Guard SDK delivered in the
explicitly authorized CosmoEdge fork, following the RK3576 delivery model:

- `include/cosmo_model_guard_v2.h`
- `lib/libcosmo_model_guard.so*`
- `bin/cosmo-model-provision`
- `SDK-MANIFEST.json`

The checked-in AArch64 shared library has the `v2-only` runtime compatibility
profile. It does not expose the legacy Model Guard ABI. The manifest records
the paired SDK artifacts and their SHA-256 hashes.

The default Sophon SDK root is this directory for both `public-runtime` and
`production-release`. Only an explicit `COSMO_MODEL_GUARD_SDK_ROOT` overrides
that choice; a directory under `build_output/model-guard-sdk-production/` is
not selected automatically. An override must pass the same admission checks.

The default build profile remains `public-runtime` (Open). Open packages
contain the runtime library but exclude `bin/cosmo-model-provision`; checking
in the complete SDK does not change the package profile. Protected
(`production-release`) packages include the matching provisioning tool.
A configured device needs only its Guard device certificate to authorize
current and future preset models published under the product model key.
There are no per-model licenses.

This complete SDK may be checked into the explicitly authorized CosmoEdge fork
to make its Sophon builds reproducible. This task-scoped authorization is not a
general relicensing or permission to redistribute the SDK elsewhere. This README
does not grant or alter artifact licensing or redistribution rights
beyond that authorization. Upstream or other
public distribution still requires separately approved terms from the artifact
owner. The historical approvals below remain limited to their original scope.

The SDK contains no private Guard source, production signing keys, device
secrets, device certificates, or model-encryption secrets. These must not be
committed. Selecting `production-release` does not create or recover signing
keys, authorize a blank device, or grant device-deployment authority.

## Distribution approval record

The separately approved CosmoEdge distribution record is public
[Issue #59](https://github.com/cosmo-wander-ai/cosmo-edge/issues/59), which
states that CosmoEdge may distribute the approved Model Guard runtime library
and public header. The implementation containing this exact runtime was then
approved and merged through
[PR #101](https://github.com/cosmo-wander-ai/cosmo-edge/pull/101).

The runtime originally integrated by PR #101 had SHA-256
`74ff8b456548e615882e5c9ee6dd18a51a2caf8124d761d7243dad014310042c`.
This project-specific approval record is not a general relicensing of the
runtime, its private implementation, provisioning inputs, or signing material.

## Integrated runtime update

The currently integrated `libcosmo_model_guard.so.2.0.0` SHA-256 is
`568bbc836180f5528fc343a5fd969c7b754d7f86f87be02ca5c4c1f1912bea3e`.
This build includes the current `release/v2.3` changes and the Sophon serial
fix from private source commit `4a46ba754781115ace624cca4be556063b2840c0`.
The matching provisioner supports `status --store-dir`.
It adds Sophon SoC serial normalization: after trimming trailing NUL and
whitespace, valid hexadecimal text shorter than 32 characters is right-padded
with ASCII `0`, and longer text is truncated to its first 32 characters.
The existing 64-byte input limit and full-input character validation remain.
Existing valid 32-character identities and the public ABI are unchanged.
The independent RKNN OTP implementation is unchanged.

The matching provisioning tool SHA-256 is
`5a2f80e14359421626eccd88d476aef2a2d84d00383ffc63957baf082ea3e41a`.
Production builds use this matching checked-in tool at
`bin/cosmo-model-provision`. The runtime and tool identities above are retained
from the existing integration; this delivery change does not claim a new build.
The original distribution references above describe the original integration,
not a separate upstream review of this runtime update.

## Verification

Sophon builds invoke `scripts/verify_model_guard_v2_sdk.py` before compilation
for SDK admission. Production admission verifies the complete manifest and
matching runtime/provisioner identities. Package auditing checks the packaged
artifacts against the same SDK manifest. Open package auditing still rejects
a bundled provisioning tool.

For the default Open profile:

```bash
/usr/bin/python3 -I -B scripts/verify_model_guard_v2_sdk.py \
  --admission-profile public-runtime \
  --sdk-root "$MODEL_GUARD_SDK_ROOT" \
  --readelf "$AARCH64_READELF" \
  --nm "$AARCH64_NM"
```
