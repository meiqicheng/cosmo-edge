#pragma once

#ifdef COSMO_NN_USE_SOPHON_BACKEND

#include <cosmo_model_guard_v2.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nn/guard/ModelLoadPolicy.h"

namespace cosmo::nn {

static_assert(CMG_V2_ABI_MAJOR == UINT32_C(2), "CosmoEdge requires model-guard ABI major 2");

/// Adapter for the four frozen model-guard v2 entry points. The context is
/// opaque so focused tests can provide deterministic C-ABI mocks without
/// changing the production ABI.
struct CemV2Api {
    using OpenArtifact      = CmgV2Status (*)(void* context, const char* installed_model_path,
                                         CmgV2SourceFormat expected_source_format,
                                         CmgV2Artifact** out_artifact);
    using GetArtifactInfo   = CmgV2Status (*)(void* context, const CmgV2Artifact* artifact,
                                            CmgV2ArtifactInfo* out_info);
    using LoadSophonSegment = CmgV2Status (*)(void* context, CmgV2Artifact* artifact, bm_handle_t bm_handle,
                                              std::uint32_t segment_index,
                                              const CmgV2SophonLoadOptions* options, void** out_bmrt);
    using CloseArtifact     = void (*)(void* context, CmgV2Artifact* artifact) noexcept;
    using CheckLoadMemory   = CmgV2Status (*)(void* context, const char* model_path);

    std::uint32_t abi_major = 0;
    void* context           = nullptr;
    OpenArtifact open_artifact{};
    GetArtifactInfo get_artifact_info{};
    LoadSophonSegment load_sophon_segment{};
    CloseArtifact close_artifact{};
    // Production checks available host RAM; mocks may inject a deterministic
    // budget or leave this empty. This does not change the external guard ABI.
    CheckLoadMemory check_load_memory{};
};

/// Minimal Sophon runtime surface needed for native user-model loading and
/// bmrt ownership cleanup. Protected CEMC loading never calls create/load_file.
struct SophonRuntimeApi {
    using Create   = void* (*)(void* context, bm_handle_t bm_handle);
    using SetFlags = void (*)(void* context, void* bmrt, std::uint32_t flags);
    using LoadFile = bool (*)(void* context, void* bmrt, const char* model_path);
    using Destroy  = void (*)(void* context, void* bmrt) noexcept;

    void* context = nullptr;
    Create create{};
    SetFlags set_flags{};
    LoadFile load_file{};
    Destroy destroy{};
};

struct SophonRuntimeDeleter {
    void* context                     = nullptr;
    SophonRuntimeApi::Destroy destroy = nullptr;

    void operator()(void* runtime) const noexcept;
};

using GuardOwnedBmrt = std::unique_ptr<void, SophonRuntimeDeleter>;

enum class SophonModelLoadError {
    kNone,
    kInvalidArgument,
    kPolicyRejected,
    kAbiUnavailable,
    kArtifactOpenFailed,
    kArtifactInfoFailed,
    kArtifactInfoInvalid,
    kSegmentLoadFailed,
    kAbiContractViolation,
    kNativeRuntimeUnavailable,
    kNativeRuntimeCreateFailed,
    kNativeLoadFailed,
    kNoMemory,
    kMemoryAdmissionFailed,
};

struct SophonModelLoadResult {
    SophonModelLoadError error = SophonModelLoadError::kNone;
    CmgV2Status guard_status   = CMG_V2_OK;
    std::vector<GuardOwnedBmrt> runtimes;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return error == SophonModelLoadError::kNone;
    }

    [[nodiscard]] bool IsOutOfMemory() const noexcept {
        return error == SophonModelLoadError::kNoMemory || guard_status == CMG_V2_RESOURCE_NO_MEMORY;
    }
};

enum class RawBmodelAuthorizationError {
    kNone,
    kPolicyRejected,
};

/// Authorization state resolved before a Sophon device is requested.
struct RawBmodelLoadPlan {
    RawBmodelAuthorizationError error = RawBmodelAuthorizationError::kPolicyRejected;
    ModelLoadDecision decision;

    [[nodiscard]] bool IsAuthorized() const noexcept {
        return error == RawBmodelAuthorizationError::kNone;
    }
};

/// Production adapters. The guard adapter binds directly to the four frozen
/// CmgV2* symbols when model-guard is enabled; no dynamic lookup or fallback is
/// used. The build compatibility gate is responsible for supplying ABI major 2.
[[nodiscard]] const CemV2Api& FrozenCemV2Api() noexcept;
[[nodiscard]] const SophonRuntimeApi& NativeSophonRuntimeApi() noexcept;

/// Resolve raw-bmodel format routing. Call this before acquiring device
/// resources.
[[nodiscard]] RawBmodelLoadPlan PrepareRawBmodelLoad(const ModelLoadPolicy& policy,
                                                     const std::string& model_path);

/// Open one authenticated artifact, validate its immutable info, load every
/// segment through the same handle, then close it exactly once. Partial failure
/// destroys every bmrt already returned by the guard.
[[nodiscard]] SophonModelLoadResult LoadCemV2SophonArtifact(
    const CemV2Api& guard_api, const SophonRuntimeApi& runtime_api, const std::string& model_path,
    CmgV2SourceFormat expected_source_format, bm_handle_t bm_handle, CmgV2SophonLoadFlags flags);

/// Dispatch a raw-bmodel decision already produced by ModelLoadPolicy. CEMC
/// decisions can only use the guard path; any guard failure is final. Native
/// loading is available only for an explicit kNativeRawBmodel decision.
[[nodiscard]] SophonModelLoadResult LoadRawBmodelByPolicy(const ModelLoadDecision& decision,
                                                          const CemV2Api& guard_api,
                                                          const SophonRuntimeApi& runtime_api,
                                                          bm_handle_t bm_handle);

/// Execute a previously authorized plan. An unauthorized plan is rejected
/// without touching either the guard ABI or the native BMRuntime API.
[[nodiscard]] SophonModelLoadResult LoadRawBmodelByPlan(const RawBmodelLoadPlan& plan,
                                                        const CemV2Api& guard_api,
                                                        const SophonRuntimeApi& runtime_api,
                                                        bm_handle_t bm_handle);

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_SOPHON_BACKEND
