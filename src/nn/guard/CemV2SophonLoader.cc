#include "nn/guard/CemV2SophonLoader.h"

#ifdef COSMO_NN_USE_SOPHON_BACKEND

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <new>
#include <utility>

#include "bmruntime_interface.h"
#include "nn/guard/HostModelLoadMemory.h"

namespace cosmo::nn {
namespace {

    class ArtifactOwner final {
    public:
        ArtifactOwner(CmgV2Artifact* artifact, const CemV2Api& api) noexcept
            : artifact_(artifact), api_(api) {}

        ~ArtifactOwner() {
            Reset();
        }

        ArtifactOwner(const ArtifactOwner&)            = delete;
        ArtifactOwner& operator=(const ArtifactOwner&) = delete;

        ArtifactOwner(ArtifactOwner&& other) noexcept
            : artifact_(std::exchange(other.artifact_, nullptr)), api_(other.api_) {}

        ArtifactOwner& operator=(ArtifactOwner&& other) noexcept {
            if (this != &other) {
                Reset();
                artifact_ = std::exchange(other.artifact_, nullptr);
                api_      = other.api_;
            }
            return *this;
        }

        [[nodiscard]] CmgV2Artifact* Get() const noexcept {
            return artifact_;
        }

    private:
        void Reset() noexcept {
            if (artifact_ == nullptr) {
                return;
            }
            api_.close_artifact(api_.context, artifact_);
            artifact_ = nullptr;
        }

        CmgV2Artifact* artifact_ = nullptr;
        CemV2Api api_{};
    };

    bool IsGuardApiAvailable(const CemV2Api& api) noexcept {
        return api.abi_major == CMG_V2_ABI_MAJOR && api.open_artifact != nullptr &&
               api.get_artifact_info != nullptr && api.load_sophon_segment != nullptr &&
               api.close_artifact != nullptr;
    }

    bool IsRuntimeDestroyAvailable(const SophonRuntimeApi& api) noexcept {
        return api.destroy != nullptr;
    }

    bool IsValidArtifactInfo(const CmgV2ArtifactInfo& info,
                             CmgV2SourceFormat expected_source_format) noexcept {
        return info.struct_size == CMG_V2_ARTIFACT_INFO_SIZE && info.reserved == 0 &&
               info.source_format == expected_source_format && info.segment_count > 0;
    }

    SophonModelLoadResult Failure(SophonModelLoadError error, CmgV2Status guard_status) {
        SophonModelLoadResult result;
        result.error        = error;
        result.guard_status = guard_status;
        return result;
    }

#ifdef COSMO_HAS_MODEL_GUARD
    CmgV2Status CheckFrozenLoadMemory(void*, const char* path) {
        std::error_code error;
        const auto artifact_bytes = std::filesystem::file_size(path, error);
        if (error) {
            return CMG_V2_RESOURCE_IO;
        }
        std::ifstream meminfo("/proc/meminfo");
        const auto available_bytes = ReadHostAvailableMemory(meminfo);
        if (!available_bytes) {
            std::fputs("[ModelLoader] Cannot read available host memory; protected load refused\n", stderr);
            return CMG_V2_RESOURCE_IO;
        }
        if (!HasHostModelLoadMemory(artifact_bytes, *available_bytes)) {
            std::fprintf(stderr,
                         "[ModelLoader] Insufficient host memory for protected model: "
                         "artifact_bytes=%llu available_bytes=%llu headroom_bytes=%llu\n",
                         static_cast<unsigned long long>(artifact_bytes),
                         static_cast<unsigned long long>(*available_bytes),
                         static_cast<unsigned long long>(kModelLoadHeadroomBytes));
            return CMG_V2_RESOURCE_NO_MEMORY;
        }
        return CMG_V2_OK;
    }

    CmgV2Status OpenFrozenArtifact(void*, const char* path, CmgV2SourceFormat source_format,
                                   CmgV2Artifact** out_artifact) {
        return CmgV2OpenArtifact(path, source_format, out_artifact);
    }

    CmgV2Status GetFrozenArtifactInfo(void*, const CmgV2Artifact* artifact, CmgV2ArtifactInfo* out_info) {
        return CmgV2GetArtifactInfo(artifact, out_info);
    }

    CmgV2Status LoadFrozenSophonSegment(void*, CmgV2Artifact* artifact, bm_handle_t bm_handle,
                                        std::uint32_t segment_index, const CmgV2SophonLoadOptions* options,
                                        void** out_bmrt) {
        return CmgV2LoadSophonSegment(artifact, bm_handle, segment_index, options, out_bmrt);
    }

    void CloseFrozenArtifact(void*, CmgV2Artifact* artifact) noexcept {
        CmgV2CloseArtifact(artifact);
    }
#endif

    void* CreateNativeRuntime(void*, bm_handle_t bm_handle) {
        return bmrt_create(bm_handle);
    }

    void SetNativeRuntimeFlags(void*, void* runtime, std::uint32_t flags) {
        bmrt_set_flags(runtime, flags);
    }

    bool LoadNativeRuntimeFile(void*, void* runtime, const char* model_path) {
        return bmrt_load_bmodel(runtime, model_path);
    }

    void DestroyNativeRuntime(void*, void* runtime) noexcept {
        if (runtime == nullptr) {
            return;
        }
        try {
            bmrt_destroy(runtime);
        } catch (...) {
            std::fputs("[ModelLoader] bmrt_destroy failed during cleanup\n", stderr);
        }
    }

    bool IsProtectedRawDecision(const ModelLoadDecision& decision) noexcept {
        return decision.action == ModelLoadAction::kGuardV2 && decision.magic == ModelMagic::kCemc &&
               !decision.model_path.empty();
    }

    bool IsNativeRawDecision(const ModelLoadDecision& decision) noexcept {
        return decision.action == ModelLoadAction::kNativeRawBmodel &&
               decision.magic == ModelMagic::kUnknown && !decision.model_path.empty();
    }

}  // namespace

void SophonRuntimeDeleter::operator()(void* runtime) const noexcept {
    if (runtime != nullptr && destroy != nullptr) {
        destroy(context, runtime);
    }
}

const CemV2Api& FrozenCemV2Api() noexcept {
#ifdef COSMO_HAS_MODEL_GUARD
    static const CemV2Api api{CMG_V2_ABI_MAJOR,        nullptr,
                              OpenFrozenArtifact,      GetFrozenArtifactInfo,
                              LoadFrozenSophonSegment, CloseFrozenArtifact,
                              CheckFrozenLoadMemory};
#else
    static const CemV2Api api{CMG_V2_ABI_MAJOR, nullptr, nullptr, nullptr, nullptr, nullptr};
#endif
    return api;
}

const SophonRuntimeApi& NativeSophonRuntimeApi() noexcept {
    static const SophonRuntimeApi api{nullptr, CreateNativeRuntime, SetNativeRuntimeFlags,
                                      LoadNativeRuntimeFile, DestroyNativeRuntime};
    return api;
}

RawBmodelLoadPlan PrepareRawBmodelLoad(const ModelLoadPolicy& policy, const std::string& model_path) {
    RawBmodelLoadPlan plan;
    plan.decision = policy.Evaluate(model_path, ModelLoadIntent::kRawBmodel);
    if (!plan.decision.IsAllowed() || (plan.decision.action != ModelLoadAction::kGuardV2 &&
                                       plan.decision.action != ModelLoadAction::kNativeRawBmodel)) {
        return plan;
    }

    plan.error = RawBmodelAuthorizationError::kNone;
    return plan;
}

SophonModelLoadResult LoadCemV2SophonArtifact(const CemV2Api& guard_api, const SophonRuntimeApi& runtime_api,
                                              const std::string& model_path,
                                              CmgV2SourceFormat expected_source_format, bm_handle_t bm_handle,
                                              CmgV2SophonLoadFlags flags) {
    if (model_path.empty() || bm_handle == nullptr || (flags & ~CMG_V2_SOPHON_SHARE_MEM) != 0) {
        return Failure(SophonModelLoadError::kInvalidArgument, CMG_V2_RESOURCE_INVALID_ARGUMENT);
    }
    if (expected_source_format != CMG_V2_SOURCE_COSMO_NN_V1 &&
        expected_source_format != CMG_V2_SOURCE_RAW_BMODEL) {
        return Failure(SophonModelLoadError::kInvalidArgument, CMG_V2_RESOURCE_INVALID_ARGUMENT);
    }
    if (!IsGuardApiAvailable(guard_api)) {
        return Failure(SophonModelLoadError::kAbiUnavailable, CMG_V2_RESOURCE_ABI_MISMATCH);
    }
    if (!IsRuntimeDestroyAvailable(runtime_api)) {
        return Failure(SophonModelLoadError::kNativeRuntimeUnavailable, CMG_V2_RESOURCE_INVALID_STATE);
    }
    if (guard_api.check_load_memory != nullptr) {
        const auto status = guard_api.check_load_memory(guard_api.context, model_path.c_str());
        if (status != CMG_V2_OK) {
            return Failure(SophonModelLoadError::kMemoryAdmissionFailed, status);
        }
    }

    CmgV2Artifact* raw_artifact = nullptr;
    const CmgV2Status open_status =
        guard_api.open_artifact(guard_api.context, model_path.c_str(), expected_source_format, &raw_artifact);
    if (open_status != CMG_V2_OK) {
        if (raw_artifact != nullptr) {
            ArtifactOwner invalid_artifact(raw_artifact, guard_api);
            return Failure(SophonModelLoadError::kAbiContractViolation, CMG_V2_RESOURCE_INTERNAL);
        }
        return Failure(SophonModelLoadError::kArtifactOpenFailed, open_status);
    }
    if (raw_artifact == nullptr) {
        return Failure(SophonModelLoadError::kAbiContractViolation, CMG_V2_RESOURCE_INTERNAL);
    }
    ArtifactOwner artifact(raw_artifact, guard_api);

    CmgV2ArtifactInfo info{};
    info.struct_size              = CMG_V2_ARTIFACT_INFO_SIZE;
    const CmgV2Status info_status = guard_api.get_artifact_info(guard_api.context, artifact.Get(), &info);
    if (info_status != CMG_V2_OK) {
        return Failure(SophonModelLoadError::kArtifactInfoFailed, info_status);
    }
    if (!IsValidArtifactInfo(info, expected_source_format)) {
        return Failure(SophonModelLoadError::kArtifactInfoInvalid, CMG_V2_RESOURCE_INTERNAL);
    }
    SophonModelLoadResult result;
    try {
        result.runtimes.reserve(info.segment_count);
    } catch (const std::bad_alloc&) {
        return Failure(SophonModelLoadError::kNoMemory, CMG_V2_RESOURCE_NO_MEMORY);
    }

    CmgV2SophonLoadOptions options{};
    options.struct_size = CMG_V2_SOPHON_LOAD_OPTIONS_SIZE;
    options.flags       = flags;

    for (std::uint32_t index = 0; index < info.segment_count; ++index) {
        // Authentication can take time, and earlier segments can retain memory.
        // Recheck immediately before the guard's large plaintext allocation.
        if (guard_api.check_load_memory != nullptr) {
            const auto status = guard_api.check_load_memory(guard_api.context, model_path.c_str());
            if (status != CMG_V2_OK) {
                return Failure(SophonModelLoadError::kMemoryAdmissionFailed, status);
            }
        }
        void* raw_runtime             = nullptr;
        const CmgV2Status load_status = guard_api.load_sophon_segment(
            guard_api.context, artifact.Get(), bm_handle, index, &options, &raw_runtime);
        GuardOwnedBmrt owned_runtime(raw_runtime, {runtime_api.context, runtime_api.destroy});
        if (load_status != CMG_V2_OK) {
            if (raw_runtime != nullptr) {
                return Failure(SophonModelLoadError::kAbiContractViolation, CMG_V2_RESOURCE_INTERNAL);
            }
            return Failure(SophonModelLoadError::kSegmentLoadFailed, load_status);
        }
        if (raw_runtime == nullptr) {
            return Failure(SophonModelLoadError::kAbiContractViolation, CMG_V2_RESOURCE_INTERNAL);
        }
        try {
            result.runtimes.push_back(std::move(owned_runtime));
        } catch (const std::bad_alloc&) {
            return Failure(SophonModelLoadError::kNoMemory, CMG_V2_RESOURCE_NO_MEMORY);
        }
    }

    return result;
}

SophonModelLoadResult LoadRawBmodelByPolicy(const ModelLoadDecision& decision, const CemV2Api& guard_api,
                                            const SophonRuntimeApi& runtime_api, bm_handle_t bm_handle) {
    if (bm_handle == nullptr) {
        return Failure(SophonModelLoadError::kInvalidArgument, CMG_V2_RESOURCE_INVALID_ARGUMENT);
    }
    if (IsProtectedRawDecision(decision)) {
        return LoadCemV2SophonArtifact(guard_api, runtime_api, decision.model_path, CMG_V2_SOURCE_RAW_BMODEL,
                                       bm_handle, CMG_V2_SOPHON_SHARE_MEM);
    }
    if (!IsNativeRawDecision(decision)) {
        return Failure(SophonModelLoadError::kPolicyRejected, CMG_V2_FORMAT_SOURCE_MISMATCH);
    }
    if (runtime_api.create == nullptr || runtime_api.set_flags == nullptr ||
        runtime_api.load_file == nullptr || runtime_api.destroy == nullptr) {
        return Failure(SophonModelLoadError::kNativeRuntimeUnavailable, CMG_V2_RESOURCE_INVALID_STATE);
    }

    void* raw_runtime = runtime_api.create(runtime_api.context, bm_handle);
    GuardOwnedBmrt owned_runtime(raw_runtime, {runtime_api.context, runtime_api.destroy});
    if (raw_runtime == nullptr) {
        return Failure(SophonModelLoadError::kNativeRuntimeCreateFailed, CMG_V2_BACKEND_FAILED);
    }
    try {
        runtime_api.set_flags(runtime_api.context, raw_runtime, BM_RUNTIME_SHARE_MEM);
        if (!runtime_api.load_file(runtime_api.context, raw_runtime, decision.model_path.c_str())) {
            return Failure(SophonModelLoadError::kNativeLoadFailed, CMG_V2_BACKEND_FAILED);
        }
    } catch (...) {
        return Failure(SophonModelLoadError::kNativeLoadFailed, CMG_V2_BACKEND_FAILED);
    }

    SophonModelLoadResult result;
    try {
        result.runtimes.push_back(std::move(owned_runtime));
    } catch (const std::bad_alloc&) {
        return Failure(SophonModelLoadError::kNoMemory, CMG_V2_RESOURCE_NO_MEMORY);
    }
    return result;
}

SophonModelLoadResult LoadRawBmodelByPlan(const RawBmodelLoadPlan& plan, const CemV2Api& guard_api,
                                          const SophonRuntimeApi& runtime_api, bm_handle_t bm_handle) {
    if (!plan.IsAuthorized()) {
        return Failure(SophonModelLoadError::kPolicyRejected, CMG_V2_FORMAT_SOURCE_MISMATCH);
    }
    return LoadRawBmodelByPolicy(plan.decision, guard_api, runtime_api, bm_handle);
}

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_SOPHON_BACKEND
