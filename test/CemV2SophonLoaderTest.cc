#include "catch_amalgamated.hpp"

#ifdef COSMO_NN_USE_SOPHON_BACKEND

#include <bmruntime_interface.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nn/device/sophon/qwen_runtime_safety.h"
#include "nn/guard/CemV2SophonLoader.h"

namespace cosmo::nn {
namespace {

    struct LoaderMockState {
        CmgV2Status open_status              = CMG_V2_OK;
        CmgV2Status info_status              = CMG_V2_OK;
        CmgV2Status load_status              = CMG_V2_OK;
        std::uint32_t segment_count          = 1;
        std::uint32_t fail_segment           = UINT32_MAX;
        std::uint32_t info_source            = CMG_V2_SOURCE_COSMO_NN_V1;
        bool return_artifact_on_open_failure = false;
        bool return_runtime_on_load_failure  = false;
        bool native_create_result            = true;
        bool native_load_result              = true;

        int open_calls             = 0;
        int info_calls             = 0;
        int close_calls            = 0;
        int native_create_calls    = 0;
        int native_set_calls       = 0;
        int native_load_calls      = 0;
        std::uint32_t native_flags = 0;
        std::string opened_path;
        std::string native_path;
        CmgV2SourceFormat opened_source = 0;
        std::vector<std::uint32_t> loaded_segments;
        std::vector<std::uint32_t> load_flags;
        std::vector<std::uintptr_t> destroyed_runtimes;
    };

    CmgV2Artifact* MockArtifact() {
        return reinterpret_cast<CmgV2Artifact*>(static_cast<std::uintptr_t>(0x7000));
    }

    void* MockRuntime(std::uint32_t index) {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x8000U + index));
    }

    bm_handle_t MockBmHandle() {
        return reinterpret_cast<bm_handle_t>(static_cast<std::uintptr_t>(0x9000));
    }

    CmgV2Status MockOpen(void* context, const char* path, CmgV2SourceFormat source,
                         CmgV2Artifact** out_artifact) {
        auto& state = *static_cast<LoaderMockState*>(context);
        ++state.open_calls;
        state.opened_path   = path == nullptr ? std::string() : path;
        state.opened_source = source;
        *out_artifact       = nullptr;
        if (state.open_status != CMG_V2_OK) {
            if (state.return_artifact_on_open_failure) {
                *out_artifact = MockArtifact();
            }
            return state.open_status;
        }
        *out_artifact = MockArtifact();
        return CMG_V2_OK;
    }

    CmgV2Status MockInfo(void* context, const CmgV2Artifact* artifact, CmgV2ArtifactInfo* out_info) {
        auto& state = *static_cast<LoaderMockState*>(context);
        ++state.info_calls;
        REQUIRE(artifact == MockArtifact());
        REQUIRE(out_info != nullptr);
        REQUIRE(out_info->struct_size == CMG_V2_ARTIFACT_INFO_SIZE);
        if (state.info_status != CMG_V2_OK) {
            return state.info_status;
        }
        out_info->struct_size   = CMG_V2_ARTIFACT_INFO_SIZE;
        out_info->source_format = state.info_source;
        out_info->segment_count = state.segment_count;
        out_info->reserved      = 0;
        return CMG_V2_OK;
    }

    CmgV2Status MockLoad(void* context, CmgV2Artifact* artifact, bm_handle_t bm_handle,
                         std::uint32_t segment_index, const CmgV2SophonLoadOptions* options,
                         void** out_bmrt) {
        auto& state = *static_cast<LoaderMockState*>(context);
        REQUIRE(artifact == MockArtifact());
        REQUIRE(bm_handle == MockBmHandle());
        REQUIRE(options != nullptr);
        REQUIRE(options->struct_size == CMG_V2_SOPHON_LOAD_OPTIONS_SIZE);
        REQUIRE(options->reserved[0] == 0);
        REQUIRE(options->reserved[1] == 0);
        state.loaded_segments.push_back(segment_index);
        state.load_flags.push_back(options->flags);
        *out_bmrt = nullptr;
        if (segment_index == state.fail_segment) {
            if (state.return_runtime_on_load_failure) {
                *out_bmrt = MockRuntime(segment_index);
            }
            return state.load_status == CMG_V2_OK ? CMG_V2_BACKEND_FAILED : state.load_status;
        }
        *out_bmrt = MockRuntime(segment_index);
        return CMG_V2_OK;
    }

    void MockClose(void* context, CmgV2Artifact* artifact) noexcept {
        auto& state = *static_cast<LoaderMockState*>(context);
        CHECK(artifact == MockArtifact());
        ++state.close_calls;
    }

    void* MockNativeCreate(void* context, bm_handle_t bm_handle) {
        auto& state = *static_cast<LoaderMockState*>(context);
        REQUIRE(bm_handle == MockBmHandle());
        ++state.native_create_calls;
        return state.native_create_result ? MockRuntime(100) : nullptr;
    }

    void MockNativeSetFlags(void* context, void* runtime, std::uint32_t flags) {
        auto& state = *static_cast<LoaderMockState*>(context);
        REQUIRE(runtime == MockRuntime(100));
        ++state.native_set_calls;
        state.native_flags = flags;
    }

    bool MockNativeLoad(void* context, void* runtime, const char* path) {
        auto& state = *static_cast<LoaderMockState*>(context);
        REQUIRE(runtime == MockRuntime(100));
        ++state.native_load_calls;
        state.native_path = path == nullptr ? std::string() : path;
        return state.native_load_result;
    }

    void MockDestroy(void* context, void* runtime) noexcept {
        auto& state = *static_cast<LoaderMockState*>(context);
        state.destroyed_runtimes.push_back(reinterpret_cast<std::uintptr_t>(runtime));
    }

    CemV2Api GuardApi(LoaderMockState& state) {
        return {CMG_V2_ABI_MAJOR, &state, MockOpen, MockInfo, MockLoad, MockClose};
    }

    SophonRuntimeApi RuntimeApi(LoaderMockState& state) {
        return {&state, MockNativeCreate, MockNativeSetFlags, MockNativeLoad, MockDestroy};
    }

    ModelLoadDecision ProtectedRawDecision() {
        ModelLoadDecision decision;
        decision.magic      = ModelMagic::kCemc;
        decision.action     = ModelLoadAction::kGuardV2;
        decision.model_path = "/models/protected/model.nn";
        return decision;
    }

    ModelLoadDecision NativeRawDecision() {
        ModelLoadDecision decision;
        decision.magic      = ModelMagic::kUnknown;
        decision.action     = ModelLoadAction::kNativeRawBmodel;
        decision.model_path = "/models/plain/model.nn";
        return decision;
    }

    RawBmodelLoadPlan ProtectedRawPlan() {
        RawBmodelLoadPlan plan;
        plan.error    = RawBmodelAuthorizationError::kNone;
        plan.decision = ProtectedRawDecision();
        return plan;
    }

    RawBmodelLoadPlan NativeRawPlan() {
        RawBmodelLoadPlan plan;
        plan.error    = RawBmodelAuthorizationError::kNone;
        plan.decision = NativeRawDecision();
        return plan;
    }

    TEST_CASE("CEM v2 loader refuses an unsafe host allocation before loading a segment",
              "[nn][model-guard-v2][memory]") {
        LoaderMockState state;
        auto api              = GuardApi(state);
        api.check_load_memory = [](void*, const char*) { return CMG_V2_RESOURCE_NO_MEMORY; };
        auto result           = LoadCemV2SophonArtifact(api, RuntimeApi(state), "/models/protected/model.nn",
                                                        CMG_V2_SOURCE_COSMO_NN_V1, MockBmHandle(), 0);
        CHECK_FALSE(result.IsSuccess());
        CHECK(result.IsOutOfMemory());
        CHECK(state.loaded_segments.empty());
        CHECK(state.close_calls == state.open_calls);
    }

    TEST_CASE("CEM v2 loader rechecks RAM and releases partial model loads", "[nn][model-guard-v2][memory]") {
        LoaderMockState state;
        state.segment_count = 2;
        auto api            = GuardApi(state);
        SECTION("memory exhausted during authentication") {
            api.check_load_memory = [](void* context, const char*) {
                return static_cast<LoaderMockState*>(context)->open_calls == 0 ? CMG_V2_OK
                                                                               : CMG_V2_RESOURCE_NO_MEMORY;
            };
        }
        SECTION("memory exhausted after the first segment") {
            api.check_load_memory = [](void* context, const char*) {
                return static_cast<LoaderMockState*>(context)->loaded_segments.empty()
                           ? CMG_V2_OK
                           : CMG_V2_RESOURCE_NO_MEMORY;
            };
        }
        auto result = LoadCemV2SophonArtifact(api, RuntimeApi(state), "/models/protected/model.nn",
                                              CMG_V2_SOURCE_COSMO_NN_V1, MockBmHandle(), 0);
        REQUIRE(result.IsOutOfMemory());
        CHECK(result.runtimes.empty());
        CHECK(state.loaded_segments.size() < 2);
        CHECK(state.destroyed_runtimes.size() == state.loaded_segments.size());
        CHECK(state.close_calls == 1);
    }

    TEST_CASE("CEM v2 loader keeps one artifact handle through 31 segments",
              "[nn][model-guard-v2][ownership]") {
        for (const std::uint32_t segment_count : {1U, 2U, 8U, 31U}) {
            DYNAMIC_SECTION("segment count " << segment_count) {
                LoaderMockState state;
                state.segment_count = segment_count;

                auto result =
                    LoadCemV2SophonArtifact(GuardApi(state), RuntimeApi(state), "/managed/preset/model.nn",
                                            CMG_V2_SOURCE_COSMO_NN_V1, MockBmHandle(), 0);

                REQUIRE(result.IsSuccess());
                REQUIRE(result.runtimes.size() == segment_count);
                REQUIRE(state.open_calls == 1);
                REQUIRE(state.info_calls == 1);
                REQUIRE(state.close_calls == 1);
                REQUIRE(state.opened_source == CMG_V2_SOURCE_COSMO_NN_V1);
                REQUIRE(state.loaded_segments.size() == segment_count);
                for (std::uint32_t index = 0; index < segment_count; ++index) {
                    REQUIRE(state.loaded_segments[index] == index);
                    REQUIRE(state.load_flags[index] == 0);
                }

                result.runtimes.clear();
                REQUIRE(state.destroyed_runtimes.size() == segment_count);
            }
        }
    }

    TEST_CASE("CEM v2 partial segment failure destroys prior runtimes and closes once",
              "[nn][model-guard-v2][ownership][security]") {
        LoaderMockState state;
        state.segment_count = 8;
        state.fail_segment  = 3;
        state.load_status   = CMG_V2_BACKEND_FAILED;

        const auto result =
            LoadCemV2SophonArtifact(GuardApi(state), RuntimeApi(state), "/managed/preset/model.nn",
                                    CMG_V2_SOURCE_COSMO_NN_V1, MockBmHandle(), 0);

        REQUIRE_FALSE(result.IsSuccess());
        REQUIRE(result.error == SophonModelLoadError::kSegmentLoadFailed);
        const std::vector<std::uint32_t> expected_segments{0, 1, 2, 3};
        REQUIRE(state.loaded_segments == expected_segments);
        REQUIRE(state.destroyed_runtimes.size() == 3);
        REQUIRE(state.close_calls == 1);
    }

    TEST_CASE("protected raw path delegates authorization directly to Guard",
              "[nn][model-guard-v2][qwen][security]") {
        LoaderMockState state;
        state.info_source = CMG_V2_SOURCE_RAW_BMODEL;

        auto result =
            LoadRawBmodelByPolicy(ProtectedRawDecision(), GuardApi(state), RuntimeApi(state), MockBmHandle());

        REQUIRE(result.IsSuccess());
        REQUIRE(result.runtimes.size() == 1);
        REQUIRE(state.open_calls == 1);
        REQUIRE(state.loaded_segments == std::vector<std::uint32_t>{0});
        REQUIRE(state.native_create_calls == 0);
        REQUIRE(state.native_load_calls == 0);
    }

    TEST_CASE("CEM v2 contract violations reclaim unexpected resources",
              "[nn][model-guard-v2][ownership][security]") {
        SECTION("failed open returned an artifact") {
            LoaderMockState state;
            state.open_status                     = CMG_V2_FORMAT_INVALID;
            state.return_artifact_on_open_failure = true;

            const auto result =
                LoadCemV2SophonArtifact(GuardApi(state), RuntimeApi(state), "/managed/preset/model.nn",
                                        CMG_V2_SOURCE_COSMO_NN_V1, MockBmHandle(), 0);

            REQUIRE(result.error == SophonModelLoadError::kAbiContractViolation);
            REQUIRE(state.close_calls == 1);
        }

        SECTION("failed load returned a runtime") {
            LoaderMockState state;
            state.fail_segment                   = 0;
            state.load_status                    = CMG_V2_CRYPTO_FAILED;
            state.return_runtime_on_load_failure = true;

            const auto result =
                LoadCemV2SophonArtifact(GuardApi(state), RuntimeApi(state), "/managed/preset/model.nn",
                                        CMG_V2_SOURCE_COSMO_NN_V1, MockBmHandle(), 0);

            REQUIRE(result.error == SophonModelLoadError::kAbiContractViolation);
            REQUIRE(state.destroyed_runtimes.size() == 1);
            REQUIRE(state.close_calls == 1);
        }
    }

    TEST_CASE("protected raw bmodel failures never invoke plaintext native loading",
              "[nn][model-guard-v2][qwen][security]") {
        for (const CmgV2Status failure : {CMG_V2_FORMAT_INVALID, CMG_V2_LICENSE_UNAVAILABLE,
                                          CMG_V2_LICENSE_REJECTED, CMG_V2_CRYPTO_FAILED}) {
            DYNAMIC_SECTION("guard failure " << failure) {
                LoaderMockState state;
                state.open_status = failure;

                const auto result = LoadRawBmodelByPolicy(ProtectedRawDecision(), GuardApi(state),
                                                          RuntimeApi(state), MockBmHandle());

                REQUIRE_FALSE(result.IsSuccess());
                REQUIRE(result.error == SophonModelLoadError::kArtifactOpenFailed);
                REQUIRE(result.guard_status == failure);
                REQUIRE(state.native_create_calls == 0);
                REQUIRE(state.native_load_calls == 0);
            }
        }

        SECTION("ABI unavailable") {
            LoaderMockState state;
            CemV2Api unavailable{CMG_V2_ABI_MAJOR, &state, nullptr, nullptr, nullptr, nullptr};

            const auto result =
                LoadRawBmodelByPolicy(ProtectedRawDecision(), unavailable, RuntimeApi(state), MockBmHandle());

            REQUIRE(result.error == SophonModelLoadError::kAbiUnavailable);
            REQUIRE(state.native_create_calls == 0);
            REQUIRE(state.native_load_calls == 0);
        }

        SECTION("guard resource exhaustion remains distinguishable internally") {
            LoaderMockState state;
            state.open_status = CMG_V2_RESOURCE_NO_MEMORY;

            const auto result = LoadRawBmodelByPolicy(ProtectedRawDecision(), GuardApi(state),
                                                      RuntimeApi(state), MockBmHandle());

            REQUIRE_FALSE(result.IsSuccess());
            REQUIRE(result.IsOutOfMemory());
            REQUIRE(state.native_create_calls == 0);
            REQUIRE(state.native_load_calls == 0);
        }
    }

    TEST_CASE("protected Qwen path uses v2 raw source and guard share-memory option",
              "[nn][model-guard-v2][qwen][ownership]") {
        LoaderMockState state;
        state.info_source = CMG_V2_SOURCE_RAW_BMODEL;

        auto result =
            LoadRawBmodelByPlan(ProtectedRawPlan(), GuardApi(state), RuntimeApi(state), MockBmHandle());

        REQUIRE(result.IsSuccess());
        REQUIRE(result.runtimes.size() == 1);
        REQUIRE(state.opened_source == CMG_V2_SOURCE_RAW_BMODEL);
        const std::vector<std::uint32_t> expected_flags{CMG_V2_SOPHON_SHARE_MEM};
        REQUIRE(state.load_flags == expected_flags);
        REQUIRE(state.native_create_calls == 0);
        REQUIRE(state.native_load_calls == 0);
        REQUIRE(state.close_calls == 1);
    }

    TEST_CASE("explicit raw format decision may use native Qwen loader",
              "[nn][model-guard-v2][qwen][security]") {
        LoaderMockState state;

        auto result =
            LoadRawBmodelByPlan(NativeRawPlan(), GuardApi(state), RuntimeApi(state), MockBmHandle());

        REQUIRE(result.IsSuccess());
        REQUIRE(result.runtimes.size() == 1);
        REQUIRE(state.open_calls == 0);
        REQUIRE(state.native_create_calls == 1);
        REQUIRE(state.native_set_calls == 1);
        REQUIRE(state.native_load_calls == 1);
        REQUIRE(state.native_flags == BM_RUNTIME_SHARE_MEM);
        REQUIRE(state.native_path == "/models/plain/model.nn");

        ModelLoadDecision rejected = NativeRawDecision();
        rejected.action            = ModelLoadAction::kReject;
        const auto rejected_result =
            LoadRawBmodelByPolicy(rejected, GuardApi(state), RuntimeApi(state), MockBmHandle());
        REQUIRE(rejected_result.error == SophonModelLoadError::kPolicyRejected);
        REQUIRE(state.native_create_calls == 1);
        REQUIRE(state.native_load_calls == 1);
    }

    TEST_CASE("native user-model failure destroys its runtime without touching guard",
              "[nn][model-guard-v2][qwen][ownership]") {
        LoaderMockState state;
        state.native_load_result = false;

        const auto result =
            LoadRawBmodelByPolicy(NativeRawDecision(), GuardApi(state), RuntimeApi(state), MockBmHandle());

        REQUIRE(result.error == SophonModelLoadError::kNativeLoadFailed);
        REQUIRE(state.open_calls == 0);
        REQUIRE(state.native_create_calls == 1);
        REQUIRE(state.native_load_calls == 1);
        REQUIRE(state.destroyed_runtimes.size() == 1);
    }

    TEST_CASE("shared Qwen helpers preserve authorization and load error mapping",
              "[nn][model-guard-v2][qwen][errors]") {
        SECTION("one authorized runtime is transferred to the Qwen owner") {
            LoaderMockState state;
            state.info_source = CMG_V2_SOURCE_RAW_BMODEL;

            void* runtime = qwen_runtime_safety::LoadSingleAuthorizedRawBmodel(
                ProtectedRawPlan(), GuardApi(state), RuntimeApi(state), MockBmHandle());

            REQUIRE(runtime == MockRuntime(0));
            REQUIRE(state.destroyed_runtimes.empty());
            MockDestroy(&state, runtime);
        }

        SECTION("policy rejection keeps the public Qwen error text") {
            RawBmodelLoadPlan plan;
            plan.error = RawBmodelAuthorizationError::kPolicyRejected;

            REQUIRE_THROWS_WITH(qwen_runtime_safety::RequireAuthorizedRawBmodel(plan),
                                "Qwen Sophon runtime operation failed: authorize model (format rejected)");
        }

        SECTION("guard resource exhaustion remains bad_alloc") {
            LoaderMockState state;
            state.open_status = CMG_V2_RESOURCE_NO_MEMORY;

            REQUIRE_THROWS_AS(qwen_runtime_safety::LoadSingleAuthorizedRawBmodel(
                                  ProtectedRawPlan(), GuardApi(state), RuntimeApi(state), MockBmHandle()),
                              std::bad_alloc);
            REQUIRE(state.native_create_calls == 0);
            REQUIRE(state.native_load_calls == 0);
        }

        SECTION("other load failures keep the public Qwen error text") {
            LoaderMockState state;
            state.open_status = CMG_V2_LICENSE_REJECTED;

            REQUIRE_THROWS_WITH(qwen_runtime_safety::LoadSingleAuthorizedRawBmodel(
                                    ProtectedRawPlan(), GuardApi(state), RuntimeApi(state), MockBmHandle()),
                                "Qwen Sophon runtime operation failed: load authorized model");
            REQUIRE(state.native_create_calls == 0);
            REQUIRE(state.native_load_calls == 0);
        }

        SECTION("native runtime creation failure keeps the public Qwen error text") {
            LoaderMockState state;
            state.native_create_result = false;

            REQUIRE_THROWS_WITH(qwen_runtime_safety::LoadSingleAuthorizedRawBmodel(
                                    NativeRawPlan(), GuardApi(state), RuntimeApi(state), MockBmHandle()),
                                "Qwen Sophon runtime operation failed: create runtime");
        }

        SECTION("native model load failure keeps the public Qwen error text and path") {
            LoaderMockState state;
            state.native_load_result = false;

            REQUIRE_THROWS_WITH(qwen_runtime_safety::LoadSingleAuthorizedRawBmodel(
                                    NativeRawPlan(), GuardApi(state), RuntimeApi(state), MockBmHandle()),
                                "Qwen Sophon runtime operation failed: load model (/models/plain/model.nn)");
        }
    }

}  // namespace
}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_SOPHON_BACKEND
