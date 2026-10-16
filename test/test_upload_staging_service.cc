#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include "catch_amalgamated.hpp"
#include "service/path/impl/UploadStagingServiceImpl.h"
#include "util/UuidUtil.h"

namespace cosmo::service {
namespace {

    namespace fs = std::filesystem;

    class TempDirectory {
    public:
        TempDirectory() {
            path_ = fs::path("/tmp") / ("cosmo-upload-staging-" + util::GenerateUUID());
            fs::create_directories(path_);
        }

        ~TempDirectory() {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }

        const fs::path& Path() const {
            return path_;
        }

    private:
        fs::path path_;
    };

    void WriteFile(const fs::path& path, const std::string& content) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.is_open());
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        REQUIRE(output.good());
    }

    std::string ReadFile(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    UploadStagingConfig MakeConfig(const fs::path& root) {
        UploadStagingConfig config;
        config.root_path                  = root.string();
        config.max_total_size             = 64;
        config.max_chunk_size             = 4;
        config.max_chunks                 = 16;
        config.max_sessions_per_principal = 2;
        config.max_sessions               = 4;
        config.max_reserved_bytes         = 64;
        config.reserve_free_bytes         = 0;
        config.reserve_free_percent       = 0;
        config.session_ttl                = std::chrono::minutes(30);
        config.cleanup_interval           = std::chrono::milliseconds::zero();
        return config;
    }

}  // namespace

TEST_CASE("Upload staging binds opaque sessions to owner and consumes once", "[upload-staging]") {
    UploadPurpose parsed_purpose{};
    REQUIRE(ParseUploadPurpose("model-component", parsed_purpose));
    CHECK(parsed_purpose == UploadPurpose::kModelComponent);
    CHECK(UploadPurposeName(UploadPurpose::kFaceImport) == "face-import");
    REQUIRE(ParseUploadPurpose("image", parsed_purpose));
    CHECK(parsed_purpose == UploadPurpose::kImage);
    CHECK(UploadPurposeName(UploadPurpose::kImage) == "image");
    CHECK(ParseUploadPurpose("model-authorization-certificate", parsed_purpose));
    CHECK(parsed_purpose == UploadPurpose::kModelAuthorizationCertificate);
    CHECK(UploadPurposeName(UploadPurpose::kModelAuthorizationCertificate) ==
          "model-authorization-certificate");
    CHECK_FALSE(ParseUploadPurpose("unknown", parsed_purpose));

    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));

    UploadBeginRequest request;
    request.principal     = "owner-token";
    request.purpose       = UploadPurpose::kModelComponent;
    request.original_name = "../model.bin";
    request.total_size    = 11;
    request.total_chunks  = 3;
    request.sha256        = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";

    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    REQUIRE(info.upload_id.size() == 36);
    REQUIRE(info.upload_id != request.original_name);
    REQUIRE(info.original_name == "model.bin");
    REQUIRE(info.max_chunk_size == 4);
    const auto upload_id = info.upload_id;

    const auto chunk0 = temp.Path() / "chunk0";
    const auto chunk1 = temp.Path() / "chunk1";
    const auto chunk2 = temp.Path() / "chunk2";
    WriteFile(chunk0, "hell");
    WriteFile(chunk1, "o wo");
    WriteFile(chunk2, "rld");

    CHECK(service.AppendChunk("other-token", upload_id, 0, chunk0.string(), info) ==
          util::ErrorEnum::AuthFailed);
    CHECK(info.upload_id.empty());
    CHECK(service.AppendChunk(request.principal, upload_id, 1, chunk1.string(), info) ==
          util::ErrorEnum::InvalidParam);
    REQUIRE(service.AppendChunk(request.principal, upload_id, 0, chunk0.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.AppendChunk(request.principal, upload_id, 1, chunk1.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.AppendChunk(request.principal, upload_id, 2, chunk2.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete(request.principal, upload_id, info) == util::ErrorEnum::Success);
    REQUIRE(info.complete);
    REQUIRE(service.Complete(request.principal, upload_id, info) == util::ErrorEnum::Success);

    std::string legacy_path;
    REQUIRE(service.GetLegacyPath(request.principal, upload_id, request.purpose, legacy_path) ==
            util::ErrorEnum::Success);
    REQUIRE(legacy_path == "upload://" + upload_id + "/model.bin");
    REQUIRE(legacy_path.find(temp.Path().string()) == std::string::npos);

    StagedFileLease wrong_purpose;
    CHECK(service.Consume(request.principal, upload_id, UploadPurpose::kVideo, wrong_purpose) ==
          util::ErrorEnum::InvalidParam);

    std::string leased_path;
    {
        StagedFileLease lease;
        REQUIRE(service.Consume(request.principal, upload_id, request.purpose, lease) ==
                util::ErrorEnum::Success);
        REQUIRE(lease.Valid());
        leased_path = lease.Path();
        CHECK(ReadFile(leased_path) == "hello world");
        CHECK(lease.OriginalName() == "model.bin");
        CHECK(lease.Size() == 11);
        CHECK(lease.Sha256() == request.sha256);
        REQUIRE(lease.Revalidate());
        const int verified_fd = lease.OpenVerified();
        REQUIRE(verified_fd >= 0);
        std::array<char, 11> verified_content{};
        REQUIRE(read(verified_fd, verified_content.data(), verified_content.size()) ==
                static_cast<ssize_t>(verified_content.size()));
        CHECK(std::string(verified_content.data(), verified_content.size()) == "hello world");
        REQUIRE(close(verified_fd) == 0);

        StagedFileLease second_lease;
        CHECK(service.Consume(request.principal, upload_id, request.purpose, second_lease) ==
              util::ErrorEnum::NoSuchId);
        CHECK(fs::exists(leased_path));
    }
    CHECK_FALSE(fs::exists(leased_path));
}

TEST_CASE("Upload staging snapshots inputs before resetting aliased outputs", "[upload-staging][api]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "data");

    UploadBeginRequest request{"owner", UploadPurpose::kModelComponent, "model.bin", 4, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    REQUIRE(service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", info.upload_id, info) == util::ErrorEnum::Success);

    std::string identifier_and_path = info.upload_id;
    REQUIRE(service.GetLegacyPath("owner", identifier_and_path, request.purpose, identifier_and_path) ==
            util::ErrorEnum::Success);
    CHECK(identifier_and_path.rfind("upload://", 0) == 0);

    StagedFileLease lease;
    REQUIRE(service.ConsumeLegacyPath("owner", identifier_and_path, request.purpose, lease) ==
            util::ErrorEnum::Success);
    CHECK(lease.Valid());
}

TEST_CASE("Upload staging rejects digest mismatch and unsafe legacy paths", "[upload-staging]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));
    UploadBeginRequest request{"owner", UploadPurpose::kModelArchive, "archive.zip", 3,
                               1,       std::string(64, '0')};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto mismatched_upload_id = info.upload_id;

    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "abc");
    REQUIRE(service.AppendChunk("owner", mismatched_upload_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    CHECK(service.Complete("owner", mismatched_upload_id, info) == util::ErrorEnum::FileAnalysisFailed);
    CHECK(service.Cancel("attacker", mismatched_upload_id) == util::ErrorEnum::AuthFailed);
    REQUIRE(service.Cancel("owner", mismatched_upload_id) == util::ErrorEnum::Success);
    CHECK(service.Cancel("owner", mismatched_upload_id) == util::ErrorEnum::Success);

    request.sha256.clear();
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto valid_upload_id = info.upload_id;
    REQUIRE(service.AppendChunk("owner", valid_upload_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", valid_upload_id, info) == util::ErrorEnum::Success);

    const auto arbitrary = temp.Path() / "arbitrary.zip";
    WriteFile(arbitrary, "abc");
    StagedFileLease lease;
    CHECK(service.ConsumeLegacyPath("owner", arbitrary.string(), request.purpose, lease) ==
          util::ErrorEnum::NoSuchId);

    std::string registered_path;
    REQUIRE(service.GetLegacyPath("owner", valid_upload_id, request.purpose, registered_path) ==
            util::ErrorEnum::Success);
    CHECK(registered_path == "upload://" + valid_upload_id + "/archive.zip");
    const auto actual_path  = temp.Path() / "sessions" / valid_upload_id / "archive.zip";
    const auto symlink_path = temp.Path() / "payload-link";
    fs::create_symlink(actual_path, symlink_path);
    CHECK(service.ConsumeLegacyPath("owner", symlink_path.string(), request.purpose, lease) ==
          util::ErrorEnum::NoSuchId);
    REQUIRE(service.ConsumeLegacyPath("owner", registered_path, request.purpose, lease) ==
            util::ErrorEnum::Success);
}

TEST_CASE("Upload staging detects payload tampering and keeps chunk state retryable", "[upload-staging]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "abc");

    UploadBeginRequest request{"owner", UploadPurpose::kModelArchive, "archive.zip", 3, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto first_upload_id = info.upload_id;
    const auto payload         = temp.Path() / "sessions" / first_upload_id / "archive.zip";
    WriteFile(payload, "tampered");
    CHECK(service.AppendChunk("owner", first_upload_id, 0, chunk.string(), info) ==
          util::ErrorEnum::FileOpenFailed);
    CHECK(info.next_chunk_index == 0);
    REQUIRE(service.Cancel("owner", first_upload_id) == util::ErrorEnum::Success);

    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto incomplete_upload_id = info.upload_id;
    REQUIRE(service.AppendChunk("owner", incomplete_upload_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    const auto completed_payload = temp.Path() / "sessions" / incomplete_upload_id / "archive.zip";
    std::ofstream extra(completed_payload, std::ios::binary | std::ios::app);
    REQUIRE(extra.is_open());
    extra << 'x';
    extra.close();
    REQUIRE(extra.good());
    CHECK(service.Complete("owner", incomplete_upload_id, info) == util::ErrorEnum::FileAnalysisFailed);
    REQUIRE(service.Cancel("owner", incomplete_upload_id) == util::ErrorEnum::Success);

    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto verified_upload_id = info.upload_id;
    REQUIRE(service.AppendChunk("owner", verified_upload_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", verified_upload_id, info) == util::ErrorEnum::Success);
    const auto verified_payload = temp.Path() / "sessions" / verified_upload_id / "archive.zip";
    std::error_code ec;
    fs::permissions(verified_payload, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    REQUIRE_FALSE(static_cast<bool>(ec));
    WriteFile(verified_payload, "abd");
    CHECK(service.Complete("owner", verified_upload_id, info) == util::ErrorEnum::FileAnalysisFailed);
    StagedFileLease tampered_lease;
    CHECK(service.Consume("owner", verified_upload_id, request.purpose, tampered_lease) ==
          util::ErrorEnum::FileAnalysisFailed);
    REQUIRE(service.Cancel("owner", verified_upload_id) == util::ErrorEnum::Success);
}

TEST_CASE("Upload staging consumes legacy paths atomically", "[upload-staging]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");

    auto stage = [&](const std::string& principal, const std::string& name) {
        UploadBeginRequest request{principal, UploadPurpose::kModelComponent, name, 1, 1, {}};
        UploadSessionInfo info;
        REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
        REQUIRE(service.AppendChunk(principal, info.upload_id, 0, chunk.string(), info) ==
                util::ErrorEnum::Success);
        REQUIRE(service.Complete(principal, info.upload_id, info) == util::ErrorEnum::Success);
        return info;
    };
    auto get_path = [&](const std::string& principal, const UploadSessionInfo& info) {
        std::string path;
        REQUIRE(service.GetLegacyPath(principal, info.upload_id, UploadPurpose::kModelComponent, path) ==
                util::ErrorEnum::Success);
        return path;
    };

    const auto first       = stage("owner", "first.bmodel");
    const auto second      = stage("owner", "second.bmodel");
    const auto other       = stage("other", "other.bmodel");
    const auto first_path  = get_path("owner", first);
    const auto second_path = get_path("owner", second);
    const auto other_path  = get_path("other", other);

    std::vector<StagedFileLease> leases;
    CHECK(service.ConsumeLegacyPaths("owner", {first_path, first_path}, UploadPurpose::kModelComponent,
                                     leases) == util::ErrorEnum::InvalidParam);
    CHECK(leases.empty());
    CHECK(get_path("owner", first) == first_path);
    CHECK(get_path("owner", second) == second_path);

    CHECK(service.ConsumeLegacyPaths("owner", {first_path, (temp.Path() / "not-registered").string()},
                                     UploadPurpose::kModelComponent, leases) == util::ErrorEnum::NoSuchId);
    CHECK(leases.empty());
    CHECK(get_path("owner", first) == first_path);
    CHECK(get_path("owner", second) == second_path);

    CHECK(service.ConsumeLegacyPaths("owner", {first_path, other_path}, UploadPurpose::kModelComponent,
                                     leases) == util::ErrorEnum::AuthFailed);
    CHECK(leases.empty());
    CHECK(get_path("owner", first) == first_path);
    CHECK(get_path("owner", second) == second_path);
    CHECK(get_path("other", other) == other_path);

    REQUIRE(service.ConsumeLegacyPaths("owner", {first_path, second_path}, UploadPurpose::kModelComponent,
                                       leases) == util::ErrorEnum::Success);
    REQUIRE(leases.size() == 2);
    CHECK(leases[0].Path() == (temp.Path() / "sessions" / first.upload_id / "first.bmodel").string());
    CHECK(leases[1].Path() == (temp.Path() / "sessions" / second.upload_id / "second.bmodel").string());
    std::string ignored;
    CHECK(service.GetLegacyPath("owner", first.upload_id, UploadPurpose::kModelComponent, ignored) ==
          util::ErrorEnum::NoSuchId);
    CHECK(service.GetLegacyPath("owner", second.upload_id, UploadPurpose::kModelComponent, ignored) ==
          util::ErrorEnum::NoSuchId);
    CHECK(get_path("other", other) == other_path);
    CHECK(service.Cancel("other", other.upload_id) == util::ErrorEnum::Success);
}

TEST_CASE("Upload staging scopes legacy client aliases and keeps canonical responses", "[upload-staging]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "abc");

    UploadBeginRequest request{"owner", UploadPurpose::kModelArchive, "archive.zip", 3, 1, {}};
    request.client_request_id = "1712345678901_a1b2c3";
    UploadSessionInfo first;
    REQUIRE(service.Begin(request, first) == util::ErrorEnum::Success);
    CHECK(first.newly_created);
    REQUIRE(first.upload_id.size() == 36);
    CHECK(first.upload_id != request.client_request_id);

    UploadSessionInfo replay;
    REQUIRE(service.Begin(request, replay) == util::ErrorEnum::Success);
    CHECK_FALSE(replay.newly_created);
    CHECK(replay.upload_id == first.upload_id);
    auto conflict       = request;
    conflict.total_size = 4;
    CHECK(service.Begin(conflict, replay) == util::ErrorEnum::InvalidParam);
    conflict         = request;
    conflict.purpose = UploadPurpose::kAlgorithm;
    CHECK(service.Begin(conflict, replay) == util::ErrorEnum::InvalidParam);

    CHECK(service.AppendChunk("other", request.client_request_id, 0, chunk.string(), replay) ==
          util::ErrorEnum::NoSuchId);
    REQUIRE(service.AppendChunk("owner", request.client_request_id, 0, chunk.string(), replay) ==
            util::ErrorEnum::Success);
    CHECK(replay.upload_id == first.upload_id);
    REQUIRE(service.Complete("owner", request.client_request_id, replay) == util::ErrorEnum::Success);
    CHECK(replay.upload_id == first.upload_id);

    auto other_request      = request;
    other_request.principal = "other";
    UploadSessionInfo other;
    REQUIRE(service.Begin(other_request, other) == util::ErrorEnum::Success);
    CHECK(other.upload_id != first.upload_id);
    REQUIRE(service.AppendChunk("other", request.client_request_id, 0, chunk.string(), other) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("other", request.client_request_id, other) == util::ErrorEnum::Success);
    REQUIRE(service.Cancel("other", request.client_request_id) == util::ErrorEnum::Success);
    CHECK(service.Complete("other", request.client_request_id, other) == util::ErrorEnum::NoSuchId);

    StagedFileLease lease;
    REQUIRE(service.Consume("owner", request.client_request_id, request.purpose, lease) ==
            util::ErrorEnum::Success);
    CHECK(lease.OriginalName() == "archive.zip");
    CHECK(lease.Size() == 3);
    CHECK(lease.Sha256().size() == 64);
    CHECK(service.Complete("owner", request.client_request_id, replay) == util::ErrorEnum::NoSuchId);

    UploadSessionInfo replacement;
    REQUIRE(service.Begin(request, replacement) == util::ErrorEnum::Success);
    CHECK(replacement.upload_id != first.upload_id);
    REQUIRE(service.Cancel("owner", request.client_request_id) == util::ErrorEnum::Success);

    for (const auto& invalid_alias : {"../escape", "contains/slash", "contains\\slash"}) {
        request.client_request_id = invalid_alias;
        CHECK(service.Begin(request, replay) == util::ErrorEnum::InvalidParam);
    }
    request.client_request_id = std::string(129, 'a');
    CHECK(service.Begin(request, replay) == util::ErrorEnum::InvalidParam);
}

TEST_CASE("Upload staging rejects aliases that collide with canonical upload IDs",
          "[upload-staging][security]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");

    UploadBeginRequest first_request{"owner", UploadPurpose::kModelArchive, "first.zip", 1, 1, {}};
    UploadSessionInfo first;
    REQUIRE(service.Begin(first_request, first) == util::ErrorEnum::Success);

    auto colliding_request              = first_request;
    colliding_request.original_name     = "second.zip";
    colliding_request.client_request_id = first.upload_id;
    UploadSessionInfo rejected;
    CHECK(service.Begin(colliding_request, rejected) == util::ErrorEnum::InvalidParam);
    CHECK(rejected.upload_id.empty());

    // Aliases are principal-scoped, but canonical IDs are global. A collision
    // from another principal must also be rejected instead of creating an
    // alias that can only resolve to the other principal's canonical session.
    colliding_request.principal = "other";
    CHECK(service.Begin(colliding_request, rejected) == util::ErrorEnum::InvalidParam);

    REQUIRE(service.AppendChunk("owner", first.upload_id, 0, chunk.string(), first) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", first.upload_id, first) == util::ErrorEnum::Success);
    StagedFileLease lease;
    REQUIRE(service.Consume("owner", first.upload_id, first_request.purpose, lease) ==
            util::ErrorEnum::Success);
    CHECK(ReadFile(lease.Path()) == "x");
}

TEST_CASE("Upload staging resumes authenticated aliases and chunks after restart",
          "[upload-staging][restart]") {
    TempDirectory temp;
    const auto root         = temp.Path() / "sessions";
    const auto first_chunk  = temp.Path() / "first-chunk";
    const auto second_chunk = temp.Path() / "second-chunk";
    WriteFile(first_chunk, "abc");
    WriteFile(second_chunk, "def");

    UploadBeginRequest request{"owner", UploadPurpose::kAudio, "alarm.wav", 6, 2, {}};
    request.client_request_id = "legacy_restart_id";
    std::string first_id;
    {
        UploadStagingServiceImpl service(MakeConfig(root));
        CHECK(service.GetCapabilities().persistent_across_restart);
        UploadSessionInfo info;
        REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
        first_id = info.upload_id;
        REQUIRE(service.AppendChunk("owner", first_id, 0, first_chunk.string(), info) ==
                util::ErrorEnum::Success);
        CHECK(info.next_chunk_index == 1);
        REQUIRE(fs::exists(root / first_id));
    }
    REQUIRE(fs::exists(root / first_id));

    UploadStagingServiceImpl restarted(MakeConfig(root));
    UploadSessionInfo resumed;
    REQUIRE(restarted.Begin(request, resumed) == util::ErrorEnum::Success);
    CHECK_FALSE(resumed.newly_created);
    CHECK(resumed.upload_id == first_id);
    CHECK(resumed.next_chunk_index == 1);
    CHECK(resumed.received_size == 3);
    REQUIRE(restarted.AppendChunk("owner", request.client_request_id, 1, second_chunk.string(), resumed) ==
            util::ErrorEnum::Success);
    REQUIRE(restarted.Complete("owner", first_id, resumed) == util::ErrorEnum::Success);

    StagedFileLease lease;
    REQUIRE(restarted.Consume("owner", first_id, request.purpose, lease) == util::ErrorEnum::Success);
    CHECK(ReadFile(lease.Path()) == "abcdef");
}

TEST_CASE("Upload staging rejects corrupt restart manifests without following links",
          "[upload-staging][restart][security]") {
    TempDirectory temp;
    const auto root     = temp.Path() / "sessions";
    const auto sentinel = temp.Path() / "sentinel";
    WriteFile(sentinel, "keep");

    UploadBeginRequest request{"owner", UploadPurpose::kVideo, "video.mp4", 4, 1, {}};
    std::string upload_id;
    {
        UploadStagingServiceImpl service(MakeConfig(root));
        UploadSessionInfo info;
        REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
        upload_id = info.upload_id;
    }

    const auto manifest = root / upload_id / ".session.json";
    REQUIRE(fs::remove(manifest));
    fs::create_symlink(sentinel, manifest);
    {
        UploadStagingServiceImpl restarted(MakeConfig(root));
        UploadSessionInfo info;
        REQUIRE(restarted.Begin(request, info) == util::ErrorEnum::Success);
        CHECK(info.upload_id != upload_id);
        REQUIRE(restarted.Cancel("owner", info.upload_id) == util::ErrorEnum::Success);
    }
    CHECK(ReadFile(sentinel) == "keep");
    CHECK_FALSE(fs::exists(root / upload_id));
}

TEST_CASE("Upload staging emits opaque references and temporarily accepts exact R1 paths",
          "[upload-staging]") {
    TempDirectory temp;
    const auto root = temp.Path() / "sessions";
    UploadStagingServiceImpl service(MakeConfig(root));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "abc");

    auto stage = [&](const std::string& name) {
        UploadBeginRequest request{"owner", UploadPurpose::kModelArchive, name, 3, 1, {}};
        UploadSessionInfo info;
        REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
        REQUIRE(service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info) ==
                util::ErrorEnum::Success);
        REQUIRE(service.Complete("owner", info.upload_id, info) == util::ErrorEnum::Success);
        return info;
    };

    const auto opaque_info = stage("../model space#.zip");
    std::string reference;
    REQUIRE(service.GetLegacyPath("owner", opaque_info.upload_id, UploadPurpose::kModelArchive, reference) ==
            util::ErrorEnum::Success);
    CHECK(reference == "upload://" + opaque_info.upload_id + "/model%20space%23.zip");
    CHECK(reference.find(root.string()) == std::string::npos);

    StagedFileLease lease;
    CHECK(service.ConsumeLegacyPath("other", reference, UploadPurpose::kModelArchive, lease) ==
          util::ErrorEnum::AuthFailed);
    CHECK(service.ConsumeLegacyPath("owner", reference + ".forged", UploadPurpose::kModelArchive, lease) ==
          util::ErrorEnum::NoSuchId);
    const auto unregistered = root / "unregistered.zip";
    WriteFile(unregistered, "abc");
    CHECK(service.ConsumeLegacyPath("owner", unregistered.string(), UploadPurpose::kModelArchive, lease) ==
          util::ErrorEnum::NoSuchId);
    REQUIRE(service.ConsumeLegacyPath("owner", reference, UploadPurpose::kModelArchive, lease) ==
            util::ErrorEnum::Success);
    CHECK(lease.OriginalName() == "model space#.zip");
    CHECK(lease.Size() == 3);
    CHECK(service.ConsumeLegacyPath("owner", reference, UploadPurpose::kModelArchive, lease) ==
          util::ErrorEnum::NoSuchId);

    const auto path_info     = stage("legacy.zip");
    const auto exact_r1_path = root / path_info.upload_id / "legacy.zip";
    StagedFileLease r1_lease;
    REQUIRE(service.ConsumeLegacyPath("owner", exact_r1_path.string(), UploadPurpose::kModelArchive,
                                      r1_lease) == util::ErrorEnum::Success);
    CHECK(r1_lease.OriginalName() == "legacy.zip");
}

TEST_CASE("Upload staging ConsumeMany is atomic across canonical IDs and aliases", "[upload-staging]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");

    auto stage = [&](const std::string& principal, const std::string& name, const std::string& alias) {
        UploadBeginRequest request{principal, UploadPurpose::kModelComponent, name, 1, 1, {}};
        request.client_request_id = alias;
        UploadSessionInfo info;
        REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
        const auto identifier = alias.empty() ? info.upload_id : alias;
        REQUIRE(service.AppendChunk(principal, identifier, 0, chunk.string(), info) ==
                util::ErrorEnum::Success);
        REQUIRE(service.Complete(principal, identifier, info) == util::ErrorEnum::Success);
        return info;
    };

    const auto first  = stage("owner", "first.bmodel", "legacy_first");
    const auto second = stage("owner", "second.bmodel", {});
    const auto other  = stage("other", "other.bmodel", "legacy_other");
    std::vector<StagedFileLease> leases;

    CHECK(service.ConsumeMany("owner", {first.upload_id, other.upload_id}, UploadPurpose::kModelComponent,
                              leases) == util::ErrorEnum::AuthFailed);
    CHECK(leases.empty());
    std::string still_registered;
    CHECK(service.GetLegacyPath("owner", first.upload_id, UploadPurpose::kModelComponent, still_registered) ==
          util::ErrorEnum::Success);
    CHECK(service.GetLegacyPath("owner", second.upload_id, UploadPurpose::kModelComponent,
                                still_registered) == util::ErrorEnum::Success);

    CHECK(service.ConsumeMany("owner", {first.upload_id, "legacy_first"}, UploadPurpose::kModelComponent,
                              leases) == util::ErrorEnum::InvalidParam);
    CHECK(service.ConsumeMany("owner", {first.upload_id, first.upload_id}, UploadPurpose::kModelComponent,
                              leases) == util::ErrorEnum::InvalidParam);
    CHECK(service.ConsumeMany("owner", {first.upload_id, second.upload_id}, UploadPurpose::kVideo, leases) ==
          util::ErrorEnum::InvalidParam);
    CHECK(leases.empty());

    REQUIRE(service.ConsumeMany("owner", {"legacy_first", second.upload_id}, UploadPurpose::kModelComponent,
                                leases) == util::ErrorEnum::Success);
    REQUIRE(leases.size() == 2);
    CHECK(leases[0].OriginalName() == "first.bmodel");
    CHECK(leases[1].OriginalName() == "second.bmodel");
    CHECK(leases[0].Size() == 1);
    CHECK(leases[1].Size() == 1);
    CHECK(service.ConsumeMany("owner", {"legacy_first"}, UploadPurpose::kModelComponent, leases) ==
          util::ErrorEnum::NoSuchId);
    CHECK(service.Cancel("other", "legacy_other") == util::ErrorEnum::Success);
}

TEST_CASE("Upload staging rolls back partially prepared batch consumption", "[upload-staging][transaction]") {
    TempDirectory temp;
    const auto root                   = temp.Path() / "sessions";
    auto config                       = MakeConfig(root);
    config.max_sessions_per_principal = 2;
    config.max_sessions               = 2;
    config.max_reserved_bytes         = 2;
    UploadStagingServiceImpl service(config);
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");

    const auto stage = [&](const std::string& name, const std::string& alias, UploadBeginRequest& request) {
        request                   = {"owner", UploadPurpose::kModelComponent, name, 1, 1, {}};
        request.client_request_id = alias;
        UploadSessionInfo info;
        REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
        REQUIRE(service.AppendChunk("owner", alias, 0, chunk.string(), info) == util::ErrorEnum::Success);
        REQUIRE(service.Complete("owner", alias, info) == util::ErrorEnum::Success);
        return info;
    };

    UploadBeginRequest first_request;
    UploadBeginRequest second_request;
    const auto first  = stage("first.bmodel", "batch_first", first_request);
    const auto second = stage("second.bmodel", "batch_second", second_request);

    const auto second_payload = root / second.upload_id / second.original_name;
    fs::permissions(second_payload, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    WriteFile(second_payload, "y");
    fs::permissions(second_payload, fs::perms::owner_read, fs::perm_options::replace);

    std::vector<StagedFileLease> leases;
    CHECK(service.ConsumeMany("owner", {"batch_first", "batch_second"}, UploadPurpose::kModelComponent,
                              leases) == util::ErrorEnum::FileAnalysisFailed);
    CHECK(leases.empty());

    // Restore the same inode so both sessions can prove that the failed batch
    // restored kComplete state, canonical registration, aliases, and quotas.
    fs::permissions(second_payload, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    WriteFile(second_payload, "x");
    fs::permissions(second_payload, fs::perms::owner_read, fs::perm_options::replace);

    UploadSessionInfo replay;
    REQUIRE(service.Begin(first_request, replay) == util::ErrorEnum::Success);
    CHECK_FALSE(replay.newly_created);
    CHECK(replay.upload_id == first.upload_id);
    REQUIRE(service.Begin(second_request, replay) == util::ErrorEnum::Success);
    CHECK_FALSE(replay.newly_created);
    CHECK(replay.upload_id == second.upload_id);

    UploadBeginRequest third_request{"owner", UploadPurpose::kModelComponent, "third.bmodel", 1, 1, {}};
    third_request.client_request_id = "batch_third";
    CHECK(service.Begin(third_request, replay) == util::ErrorEnum::ResourceLimit);

    REQUIRE(service.ConsumeMany("owner", {"batch_first", "batch_second"}, UploadPurpose::kModelComponent,
                                leases) == util::ErrorEnum::Success);
    REQUIRE(leases.size() == 2);
    CHECK(leases[0].OriginalName() == "first.bmodel");
    CHECK(leases[1].OriginalName() == "second.bmodel");

    REQUIRE(service.Begin(third_request, replay) == util::ErrorEnum::Success);
    CHECK(replay.newly_created);
    CHECK(service.Cancel("owner", "batch_third") == util::ErrorEnum::Success);
}

TEST_CASE("Upload staging enforces limits and expires abandoned sessions", "[upload-staging]") {
    CHECK(UploadStagingConfig{}.max_total_size == 0);
    CHECK(UploadStagingConfig{}.max_chunk_size == 8ULL * 1024 * 1024);
    CHECK(UploadStagingConfig{}.max_chunks == 0);
    CHECK(UploadStagingConfig{}.max_sessions_per_principal == 0);
    CHECK(UploadStagingConfig{}.max_sessions == 0);
    CHECK(UploadStagingConfig{}.max_reserved_bytes == 0);
    CHECK(UploadStagingConfig{}.reserve_free_bytes == 512ULL * 1024 * 1024);
    CHECK(UploadStagingConfig{}.reserve_free_percent == 0);
    CHECK(UploadStagingConfig{}.session_ttl == std::chrono::minutes(30));
    CHECK(UploadStagingConfig{}.cleanup_interval == std::chrono::minutes(1));
    CHECK(UploadStagingConfig{}.max_session_lifetime == std::chrono::milliseconds::zero());
    CHECK(UploadStagingConfig{}.persist_sessions);

    TempDirectory temp;
    auto config                       = MakeConfig(temp.Path() / "sessions");
    config.max_sessions_per_principal = 1;
    config.max_sessions               = 2;
    config.max_reserved_bytes         = 12;
    config.session_ttl                = std::chrono::milliseconds(50);
    auto now                          = UploadStagingServiceImpl::Clock::now();
    UploadStagingServiceImpl service(config, [&now]() { return now; });

    UploadSessionInfo first;
    UploadBeginRequest request{"owner", UploadPurpose::kVideo, "video.mp4", 8, 2, {}};
    REQUIRE(service.Begin(request, first) == util::ErrorEnum::Success);

    UploadSessionInfo ignored;
    CHECK(service.Begin(request, ignored) == util::ErrorEnum::ResourceLimit);
    CHECK(ignored.admission.failure == UploadAdmissionFailure::kPrincipalSessions);
    CHECK(ignored.admission.actual == 1);
    CHECK(ignored.admission.limit == 1);
    auto other_request         = request;
    other_request.principal    = "other";
    other_request.total_size   = 5;
    other_request.total_chunks = 2;
    CHECK(service.Begin(other_request, ignored) == util::ErrorEnum::ResourceLimit);
    CHECK(ignored.admission.failure == UploadAdmissionFailure::kStorageReserve);
    CHECK(ignored.admission.required_bytes == 5);
    CHECK(ignored.admission.limit == 4);

    now += std::chrono::milliseconds(51);
    service.CleanupExpired();
    const auto chunk = temp.Path() / "expired-chunk";
    WriteFile(chunk, "1234");
    CHECK(service.AppendChunk("owner", first.upload_id, 0, chunk.string(), ignored) ==
          util::ErrorEnum::NoSuchId);
    REQUIRE(service.Begin(request, ignored) == util::ErrorEnum::Success);

    auto impossible         = request;
    impossible.principal    = "third";
    impossible.total_size   = 9;
    impossible.total_chunks = 2;
    CHECK(service.Begin(impossible, first) == util::ErrorEnum::InvalidParam);
}

TEST_CASE("Upload staging has no default product file-size or chunk-count cap", "[upload-staging]") {
    TempDirectory temp;
    auto config                       = MakeConfig(temp.Path() / "sessions");
    config.max_total_size             = 0;
    config.max_chunks                 = 0;
    config.max_sessions_per_principal = 0;
    config.max_sessions               = 0;
    config.max_chunk_size             = 8ULL * 1024 * 1024;
    config.max_reserved_bytes         = 700ULL * 1024 * 1024;
    config.max_session_metadata_bytes = 64ULL * 1024 * 1024;
    UploadStagingServiceImpl service(config);

    UploadBeginRequest request{
        "owner", UploadPurpose::kModelArchive, "large-model.tar.gz", 600ULL * 1024 * 1024, 75, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    CHECK(info.total_size == request.total_size);
    CHECK(info.total_chunks == request.total_chunks);
    CHECK(service.Cancel("owner", info.upload_id) == util::ErrorEnum::Success);
}

TEST_CASE("Upload staging reports metadata budget rejection details", "[upload-staging]") {
    TempDirectory temp;
    auto config                       = MakeConfig(temp.Path() / "sessions");
    config.max_total_size             = 0;
    config.max_chunks                 = 0;
    config.max_session_metadata_bytes = sizeof(std::uint64_t);
    CHECK_THROWS_AS(UploadStagingServiceImpl(config), std::invalid_argument);

    config.max_session_metadata_bytes = 128;
    UploadStagingServiceImpl service(config);
    UploadBeginRequest request{"owner", UploadPurpose::kVideo, "video.mp4", 100, 100, {}};
    UploadSessionInfo info;
    CHECK(service.Begin(request, info) == util::ErrorEnum::ResourceLimit);
    CHECK(info.admission.failure == UploadAdmissionFailure::kMetadataBudget);
    CHECK(info.admission.actual > info.admission.limit);
}

TEST_CASE("Upload staging refreshes idle TTL only for new chunks and enforces absolute lifetime",
          "[upload-staging]") {
    TempDirectory temp;
    auto config                 = MakeConfig(temp.Path() / "sessions");
    config.session_ttl          = std::chrono::milliseconds(50);
    config.max_session_lifetime = std::chrono::milliseconds(120);
    auto now                    = UploadStagingServiceImpl::Clock::now();
    UploadStagingServiceImpl service(config, [&now]() { return now; });
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");

    UploadBeginRequest request{"owner", UploadPurpose::kVideo, "video.mp4", 3, 3, {}};
    request.client_request_id = "ttl_upload";
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto canonical_id = info.upload_id;

    now += std::chrono::milliseconds(40);
    REQUIRE(service.AppendChunk("owner", request.client_request_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    const auto first_refresh = info.expires_at_unix_ms;
    now += std::chrono::milliseconds(40);
    REQUIRE(service.AppendChunk("owner", request.client_request_id, 1, chunk.string(), info) ==
            util::ErrorEnum::Success);
    CHECK(info.upload_id == canonical_id);
    CHECK(info.expires_at_unix_ms >= first_refresh);

    now += std::chrono::milliseconds(30);
    REQUIRE(service.AppendChunk("owner", request.client_request_id, 2, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", request.client_request_id, info) == util::ErrorEnum::Success);
    now += std::chrono::milliseconds(11);
    service.CleanupExpired();
    CHECK(service.Complete("owner", canonical_id, info) == util::ErrorEnum::NoSuchId);
    CHECK(service.Complete("owner", request.client_request_id, info) == util::ErrorEnum::NoSuchId);

    now += std::chrono::milliseconds(100);
    request.total_size        = 1;
    request.total_chunks      = 1;
    request.client_request_id = "retry_does_not_refresh";
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    now += std::chrono::milliseconds(40);
    REQUIRE(service.AppendChunk("owner", request.client_request_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    const auto expiry_after_new_chunk = info.expires_at_unix_ms;
    now += std::chrono::milliseconds(40);
    REQUIRE(service.AppendChunk("owner", request.client_request_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    CHECK(info.expires_at_unix_ms == expiry_after_new_chunk);
    now += std::chrono::milliseconds(11);
    service.CleanupExpired();
    CHECK(service.Complete("owner", request.client_request_id, info) == util::ErrorEnum::NoSuchId);
}

TEST_CASE("Upload staging serializes duplicate chunk appends and rejects symlinks", "[upload-staging]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));
    UploadBeginRequest request{"owner", UploadPurpose::kAudio, "alarm.wav", 4, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto upload_id = info.upload_id;

    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "beep");
    const auto symlink_path = temp.Path() / "chunk-link";
    fs::create_symlink(chunk, symlink_path);
    CHECK(service.AppendChunk("owner", upload_id, 0, symlink_path.string(), info) ==
          util::ErrorEnum::FileOpenFailed);

    util::ErrorEnum first_result  = util::ErrorEnum::Failed;
    util::ErrorEnum second_result = util::ErrorEnum::Failed;
    UploadSessionInfo first_info;
    UploadSessionInfo second_info;
    std::thread first(
        [&]() { first_result = service.AppendChunk("owner", upload_id, 0, chunk.string(), first_info); });
    std::thread second(
        [&]() { second_result = service.AppendChunk("owner", upload_id, 0, chunk.string(), second_info); });
    first.join();
    second.join();

    CHECK(first_result == util::ErrorEnum::Success);
    CHECK(second_result == util::ErrorEnum::Success);
    CHECK(first_info.next_chunk_index == 1);
    CHECK(second_info.next_chunk_index == 1);

    const auto different_chunk = temp.Path() / "different-chunk";
    WriteFile(different_chunk, "boop");
    CHECK(service.AppendChunk("owner", upload_id, 0, different_chunk.string(), info) ==
          util::ErrorEnum::InvalidParam);
    REQUIRE(service.Complete("owner", upload_id, info) == util::ErrorEnum::Success);
}

TEST_CASE("Upload staging pins root identity and never follows replacement links", "[upload-staging]") {
    TempDirectory temp;
    const auto root = temp.Path() / "sessions";
    UploadStagingServiceImpl service(MakeConfig(root));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");

    UploadBeginRequest request{"owner", UploadPurpose::kAudio, "alarm.wav", 1, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    REQUIRE(service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", info.upload_id, info) == util::ErrorEnum::Success);

    const auto victim = temp.Path() / "victim";
    fs::create_directories(victim);
    const auto sentinel = victim / "sentinel";
    WriteFile(sentinel, "keep");
    const auto pinned_root = temp.Path() / "pinned-sessions";
    fs::rename(root, pinned_root);
    fs::create_directory_symlink(victim, root);

    StagedFileLease rejected;
    CHECK(service.Consume("owner", info.upload_id, request.purpose, rejected) ==
          util::ErrorEnum::FileNotExist);
    CHECK(service.Cancel("owner", info.upload_id) == util::ErrorEnum::Success);
    CHECK(fs::exists(sentinel));
    CHECK(ReadFile(sentinel) == "keep");
    CHECK_FALSE(fs::exists(pinned_root / info.upload_id));
}

TEST_CASE("Upload staging accepts a private root inherited from its deployment parent",
          "[upload-staging][root]") {
    if (geteuid() != 0) {
        SUCCEED("owner-transition coverage requires a root test process");
        return;
    }

    constexpr uid_t kDeploymentOwner = 65534;
    TempDirectory temp;
    const auto parent = temp.Path() / "upload";
    const auto root   = parent / "sessions";
    fs::create_directories(root);
    // Pin the deployment parent to 0755 so inherited-owner acceptance does
    // not depend on the test process umask (embedded root shells often run
    // with umask 0, which would leave the parent world-writable).
    fs::permissions(parent, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                fs::perms::others_read | fs::perms::others_exec);
    REQUIRE(chown(parent.c_str(), kDeploymentOwner, kDeploymentOwner) == 0);
    REQUIRE(chown(root.c_str(), kDeploymentOwner, kDeploymentOwner) == 0);

    UploadStagingServiceImpl service(MakeConfig(root));
    UploadBeginRequest request{"owner", UploadPurpose::kAudio, "alarm.wav", 1, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto session = root / info.upload_id;
    struct stat session_status {};
    REQUIRE(lstat(session.c_str(), &session_status) == 0);
    CHECK(session_status.st_uid == geteuid());

    const auto unexpected_parent = temp.Path() / "other-upload";
    const auto unexpected_root   = unexpected_parent / "sessions";
    fs::create_directories(unexpected_root);
    REQUIRE(chown(unexpected_root.c_str(), kDeploymentOwner, kDeploymentOwner) == 0);
    CHECK_THROWS_AS(UploadStagingServiceImpl(MakeConfig(unexpected_root)), std::runtime_error);
}

TEST_CASE("Upload staging lease cleanup remains inside the pinned root", "[upload-staging]") {
    TempDirectory temp;
    const auto root = temp.Path() / "sessions";
    UploadStagingServiceImpl service(MakeConfig(root));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");

    UploadBeginRequest request{"owner", UploadPurpose::kVideo, "video.mp4", 1, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    REQUIRE(service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", info.upload_id, info) == util::ErrorEnum::Success);

    const auto victim = temp.Path() / "victim";
    fs::create_directories(victim);
    const auto sentinel = victim / "sentinel";
    WriteFile(sentinel, "keep");
    const auto pinned_root = temp.Path() / "pinned-sessions";
    {
        StagedFileLease lease;
        REQUIRE(service.Consume("owner", info.upload_id, request.purpose, lease) == util::ErrorEnum::Success);
        fs::rename(root, pinned_root);
        fs::create_directory_symlink(victim, root);
        REQUIRE(fs::exists(pinned_root / info.upload_id));
        CHECK_FALSE(lease.Revalidate());
        CHECK(lease.OpenVerified() == -1);
    }
    CHECK_FALSE(fs::exists(pinned_root / info.upload_id));
    CHECK(fs::exists(sentinel));
    CHECK(ReadFile(sentinel) == "keep");
}

TEST_CASE("Upload staging lease detects payload inode replacement before handoff", "[upload-staging]") {
    TempDirectory temp;
    const auto root = temp.Path() / "sessions";
    UploadStagingServiceImpl service(MakeConfig(root));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "abc");

    UploadBeginRequest request{"owner", UploadPurpose::kModelArchive, "model.zip", 3, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    REQUIRE(service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", info.upload_id, info) == util::ErrorEnum::Success);

    StagedFileLease lease;
    REQUIRE(service.Consume("owner", info.upload_id, request.purpose, lease) == util::ErrorEnum::Success);
    REQUIRE(lease.Revalidate());
    const auto payload = fs::path(lease.Path());
    std::error_code ec;
    fs::permissions(payload, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    REQUIRE_FALSE(static_cast<bool>(ec));
    WriteFile(payload, "abd");
    CHECK_FALSE(lease.Revalidate());
    WriteFile(payload, "abc");
    fs::permissions(payload, fs::perms::owner_read, fs::perm_options::replace, ec);
    REQUIRE_FALSE(static_cast<bool>(ec));
    REQUIRE(lease.Revalidate());

    const auto pinned_inode = payload.parent_path() / "pinned-model.zip";
    fs::rename(payload, pinned_inode);
    WriteFile(payload, "abc");

    CHECK_FALSE(lease.Revalidate());
    CHECK(lease.OpenVerified() == -1);
    CHECK(ReadFile(pinned_inode) == "abc");
}

TEST_CASE("Upload staging rejects replaced session directories and payload symlinks", "[upload-staging]") {
    TempDirectory temp;
    const auto root = temp.Path() / "sessions";
    UploadStagingServiceImpl service(MakeConfig(root));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");
    const auto victim = temp.Path() / "victim";
    fs::create_directories(victim);
    const auto sentinel = victim / "sentinel";
    WriteFile(sentinel, "keep");

    auto stage = [&]() {
        UploadBeginRequest request{"owner", UploadPurpose::kModelArchive, "model.zip", 1, 1, {}};
        UploadSessionInfo info;
        REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
        REQUIRE(service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info) ==
                util::ErrorEnum::Success);
        REQUIRE(service.Complete("owner", info.upload_id, info) == util::ErrorEnum::Success);
        return info;
    };

    auto info                = stage();
    const auto payload       = root / info.upload_id / "model.zip";
    const auto saved_payload = root / info.upload_id / "saved.zip";
    fs::rename(payload, saved_payload);
    fs::create_symlink(sentinel, payload);
    StagedFileLease lease;
    CHECK(service.Consume("owner", info.upload_id, UploadPurpose::kModelArchive, lease) ==
          util::ErrorEnum::FileNotExist);
    REQUIRE(service.Cancel("owner", info.upload_id) == util::ErrorEnum::Success);
    CHECK(ReadFile(sentinel) == "keep");

    info                     = stage();
    const auto session       = root / info.upload_id;
    const auto saved_session = root / (info.upload_id + ".saved");
    fs::rename(session, saved_session);
    fs::create_directory_symlink(victim, session);
    CHECK(service.Consume("owner", info.upload_id, UploadPurpose::kModelArchive, lease) ==
          util::ErrorEnum::FileNotExist);
    REQUIRE(service.Cancel("owner", info.upload_id) == util::ErrorEnum::Success);
    CHECK(ReadFile(sentinel) == "keep");
}

TEST_CASE("Upload staging resolves concurrent consume cancel and expiry safely", "[upload-staging]") {
    TempDirectory temp;
    auto now    = UploadStagingServiceImpl::Clock::now();
    auto config = MakeConfig(temp.Path() / "sessions");
    UploadStagingServiceImpl service(config, [&now]() { return now; });
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");
    UploadBeginRequest request{"owner", UploadPurpose::kAudio, "alarm.wav", 1, 1, {}};
    request.client_request_id = "concurrent_alias";
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    REQUIRE(service.AppendChunk("owner", request.client_request_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", request.client_request_id, info) == util::ErrorEnum::Success);

    StagedFileLease lease;
    util::ErrorEnum consume_result = util::ErrorEnum::Failed;
    util::ErrorEnum cancel_result  = util::ErrorEnum::Failed;
    std::atomic<bool> start{false};
    std::thread consumer([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        consume_result = service.Consume("owner", request.client_request_id, request.purpose, lease);
    });
    std::thread canceller([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        cancel_result = service.Cancel("owner", request.client_request_id);
    });
    start.store(true);
    consumer.join();
    canceller.join();
    CHECK(cancel_result == util::ErrorEnum::Success);
    CHECK((consume_result == util::ErrorEnum::Success || consume_result == util::ErrorEnum::NoSuchId));
    if (lease.Valid()) {
        CHECK(ReadFile(lease.Path()) == "x");
    }

    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    REQUIRE(service.AppendChunk("owner", request.client_request_id, 0, chunk.string(), info) ==
            util::ErrorEnum::Success);
    REQUIRE(service.Complete("owner", request.client_request_id, info) == util::ErrorEnum::Success);
    now += config.session_ttl + std::chrono::milliseconds(1);
    consume_result = util::ErrorEnum::Failed;
    cancel_result  = util::ErrorEnum::Failed;
    start.store(false);
    std::thread expired_consumer([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        StagedFileLease expired_lease;
        consume_result = service.Consume("owner", request.client_request_id, request.purpose, expired_lease);
    });
    std::thread expired_canceller([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        cancel_result = service.Cancel("owner", request.client_request_id);
    });
    std::thread cleaner([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        service.CleanupExpired();
    });
    start.store(true);
    expired_consumer.join();
    expired_canceller.join();
    cleaner.join();
    CHECK(consume_result == util::ErrorEnum::NoSuchId);
    CHECK(cancel_result == util::ErrorEnum::Success);
}

TEST_CASE("Upload staging cleanup skips a session busy with payload I/O", "[upload-staging][thread]") {
    using namespace std::chrono_literals;

    TempDirectory temp;
    auto config = MakeConfig(temp.Path() / "sessions");
    auto now    = UploadStagingServiceImpl::Clock::now();
    std::atomic<int> clock_calls{0};
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool append_holds_session = false;
    bool release_append       = false;

    UploadStagingServiceImpl service(config, [&]() {
        const int call = clock_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        // Begin reads the clock twice. Append reads it once in its pre-cleanup,
        // then refreshes the idle TTL while holding the per-session mutex.
        if (call == 4) {
            std::unique_lock<std::mutex> lock(gate_mutex);
            append_holds_session = true;
            gate_condition.notify_all();
            gate_condition.wait(lock, [&]() { return release_append; });
        }
        return now;
    });

    UploadBeginRequest request{"owner", UploadPurpose::kAudio, "alarm.wav", 1, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");

    util::ErrorEnum append_result = util::ErrorEnum::Failed;
    std::thread append_thread(
        [&]() { append_result = service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info); });

    bool append_entered = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        append_entered = gate_condition.wait_for(lock, 2s, [&]() { return append_holds_session; });
    }
    if (!append_entered) {
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            release_append = true;
        }
        gate_condition.notify_all();
        append_thread.join();
        FAIL("append did not reach the session-locked clock hook");
    }

    auto cleanup              = std::async(std::launch::async, [&]() { service.CleanupExpired(); });
    const auto cleanup_status = cleanup.wait_for(500ms);
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_append = true;
    }
    gate_condition.notify_all();
    append_thread.join();
    cleanup.get();

    CHECK(cleanup_status == std::future_status::ready);
    CHECK(append_result == util::ErrorEnum::Success);
}

TEST_CASE("Upload staging background cleanup expires abandoned sessions", "[upload-staging]") {
    TempDirectory temp;
    auto config             = MakeConfig(temp.Path() / "sessions");
    config.session_ttl      = std::chrono::milliseconds(20);
    config.cleanup_interval = std::chrono::milliseconds(5);
    UploadStagingServiceImpl service(config);
    UploadBeginRequest request{"owner", UploadPurpose::kAudio, "alarm.wav", 1, 1, {}};
    UploadSessionInfo info;
    REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
    const auto session_path = temp.Path() / "sessions" / info.upload_id;

    for (int attempt = 0; attempt < 100 && fs::exists(session_path); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(fs::exists(session_path));
    const auto chunk = temp.Path() / "chunk";
    WriteFile(chunk, "x");
    CHECK(service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info) == util::ErrorEnum::NoSuchId);
}

TEST_CASE("Upload staging preserves sanitized consumer-relevant extensions", "[upload-staging]") {
    TempDirectory temp;
    UploadStagingServiceImpl service(MakeConfig(temp.Path() / "sessions"));

    for (const auto& filename : {"people.zip", "algorithm.tar.gz", "detector.bmodel"}) {
        UploadBeginRequest request{"owner", UploadPurpose::kModelComponent, filename, 1, 1, {}};
        UploadSessionInfo info;
        REQUIRE(service.Begin(request, info) == util::ErrorEnum::Success);
        const auto chunk = temp.Path() / (std::string(filename) + ".chunk");
        WriteFile(chunk, "x");
        REQUIRE(service.AppendChunk("owner", info.upload_id, 0, chunk.string(), info) ==
                util::ErrorEnum::Success);
        REQUIRE(service.Complete("owner", info.upload_id, info) == util::ErrorEnum::Success);
        std::string path;
        REQUIRE(service.GetLegacyPath("owner", info.upload_id, request.purpose, path) ==
                util::ErrorEnum::Success);
        CHECK(path == "upload://" + info.upload_id + "/" + filename);
        StagedFileLease lease;
        REQUIRE(service.Consume("owner", info.upload_id, request.purpose, lease) == util::ErrorEnum::Success);
    }
}

}  // namespace cosmo::service
