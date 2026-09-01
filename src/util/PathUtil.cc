// PathUtil — Filesystem path helpers for the Cosmo application.

#include "util/PathUtil.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "RuntimePathsConfig.h"
#include "util/DateTimeFormat.h"
#include "util/FileUtil.h"
#include "util/FormatString.h"
#include "util/Log.h"
#include "util/UuidUtil.h"

namespace fs = std::filesystem;

namespace cosmo::path {

// ── Internal helpers ──────────────────────────────────────────────────────────

namespace {

    constexpr const char* kCwaiRuntimeDir = "cwai";

    // Root paths — mutable so OverrideRootPathForTest() can redirect them.
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    std::string g_dataPath{cosmo::runtime::kDefaultDataDir};
    std::string g_appDataPath{cosmo::runtime::kDefaultAppDataDir};
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    // Derived-path accessors (recomputed on every call so they reflect overrides)
    std::string LogPath() {
        return g_dataPath + "/log";
    }
    std::string MainCfgPath() {
        return g_dataPath + "/conf";
    }
    std::string BackupCfgPath() {
        return g_dataPath + "/confb";
    }
    std::string UploadPath() {
        return g_dataPath + "/upload";
    }
    std::string ModelUploadTmpDir() {
        return g_dataPath + "/tmp/model_upload";
    }
    std::string UpgradePath() {
        return g_dataPath + "/upgrade";
    }
    std::string CameraPath() {
        return g_dataPath + "/camera";
    }
    std::string UserResourcePath() {
        return g_dataPath + "/resource";
    }
    std::string ResourcePath() {
        return g_appDataPath + "/resource";
    }
    std::string LayoutPath() {
        return ResourcePath() + "/layout";
    }
    std::string ModelPath() {
        return UserResourcePath() + "/models";
    }
    std::string PresetModelPath() {
        return ResourcePath() + "/models";
    }
    std::string ModelTemplatePath() {
        return ResourcePath() + "/model_template";
    }
    std::string AlgorithmPath() {
        return ResourcePath() + "/algorithm";
    }
    std::string TemporaryDir() {
        return g_dataPath + "/temporary";
    }
    std::string FileDir() {
        return g_dataPath + "/export";
    }
    std::string FaceLibPhotoDir() {
        return g_dataPath + "/weblibPic/facePicture";
    }
    std::string PersonLibPhotoDir() {
        return g_dataPath + "/weblibPic/personPicture";
    }
    std::string ArticlesReidLibPhotoDir() {
        return g_dataPath + "/weblibPic/articleslib";
    }
    std::string DbPath() {
        return g_dataPath + "/db";
    }
    std::string DbBackupPath() {
        return g_dataPath + "/db2";
    }

    // Relative path tokens used to build event / web paths
    const std::string kAtEvent{"event"};
    const std::string kAtWeb{"web"};
    const std::string kAtLogoWeb{"weblogo"};
    const std::string kAtPersonLibPicWeb{"/weblibPic/personPicture"};

    void EnsureDir(const std::string& path) {
        if (!fs::exists(path)) {
            std::error_code err;
            bool ret = fs::create_directories(path, err);
            LOG_INFO("create dir: {}, ret: {}", path, ret);
        }
    }

    void CleanupTransientUploads() {
        const fs::path upload_root(UploadPath());
        std::error_code ec;
        const auto root_status = fs::symlink_status(upload_root, ec);
        if (ec || fs::is_symlink(root_status) || !fs::is_directory(root_status)) {
            ec.clear();
            fs::remove_all(upload_root, ec);
            if (ec) {
                LOG_WARN("Cannot reset upload root {}: {}", upload_root.string(), ec.message());
                return;
            }
        }
        EnsureDir(upload_root.string());

        fs::directory_iterator iterator(upload_root, ec);
        const fs::directory_iterator end;
        while (!ec && iterator != end) {
            const auto entry_path = iterator->path();
            ++iterator;
            if (entry_path.filename() == "sessions") {
                continue;
            }
            std::error_code cleanup_error;
            fs::remove_all(entry_path, cleanup_error);
            if (cleanup_error) {
                LOG_WARN("Cannot clean transient upload path {}: {}", entry_path.string(),
                         cleanup_error.message());
            }
        }
        if (ec) {
            LOG_WARN("Cannot enumerate upload root {}: {}", upload_root.string(), ec.message());
        }
    }

    std::string RelativePath(uint64_t timestamp) {
        time_t t  = static_cast<time_t>(timestamp / 1000);
        auto date = cosmo::util::YMDDate(t);
        return COSMO_FORMAT("{:04}/{:02}/{:02}", date.Year(), date.Month(), date.Day());
    }

    std::string RecordPath(const std::string& type, uint64_t timestamp, bool autoCreate) {
        std::string p = (fs::path(g_dataPath) / type / RelativePath(timestamp)).string();
        if (autoCreate)
            EnsureDir(p);
        return p;
    }

    std::string RecordPath(const std::string& type, bool autoCreate) {
        std::string p = (fs::path(g_dataPath) / type).string();
        if (autoCreate)
            EnsureDir(p);
        return p;
    }

    std::string WebPath(const std::string& type) {
        return (fs::path("/") / type).string();
    }

    std::string WebPath(const std::string& type, uint64_t timestamp) {
        return (fs::path("/") / type / RelativePath(timestamp)).string();
    }

}  // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void Init() {
    CleanupTransientUploads();
    cosmo::util::RemovePath(RecordPath(kAtWeb, false));

    std::string cwaiRuntimePath = (fs::path(g_dataPath) / kCwaiRuntimeDir).string();
    cosmo::util::RemovePath(cwaiRuntimePath);
    EnsureDir(cwaiRuntimePath);
}

void OverrideRootPaths(const std::string& dataPath, const std::string& appDataPath) {
    const fs::path data_root(dataPath);
    const fs::path app_data_root(appDataPath);
    if (dataPath.empty() || appDataPath.empty() || !data_root.is_absolute() || !app_data_root.is_absolute()) {
        throw std::invalid_argument("Cosmo data roots must be non-empty absolute paths");
    }
    g_dataPath    = data_root.lexically_normal().string();
    g_appDataPath = app_data_root.lexically_normal().string();
}

void OverrideRootPathForTest(const std::string& dataPath, const std::string& appDataPath) {
    OverrideRootPaths(dataPath, appDataPath);
}

// ── Configuration paths ───────────────────────────────────────────────────────

std::string GetCfgPath() {
    auto p = MainCfgPath();
    EnsureDir(p);
    return p;
}

std::string GetBackupCfgPath() {
    auto p = BackupCfgPath();
    EnsureDir(p);
    return p;
}

std::string GetCfgPath(const std::string& subPath) {
    std::string p = (fs::path(MainCfgPath()) / subPath).string();
    EnsureDir(p);
    return p;
}

std::string GetBackupCfgPath(const std::string& subPath) {
    std::string p = (fs::path(BackupCfgPath()) / subPath).string();
    EnsureDir(p);
    return p;
}

std::string GetUploadPath() {
    auto p = UploadPath();
    EnsureDir(p);
    return p;
}

std::string GetUploadTmpPath() {
    const auto upload_root = GetUploadPath();
    std::error_code ec;
    if (!fs::is_directory(upload_root, ec) || ec) {
        LOG_ERRO("Cannot create upload temporary directory: invalid upload root {}", upload_root);
        return {};
    }

    constexpr size_t kMaxCreateAttempts = 4;
    for (size_t attempt = 0; attempt < kMaxCreateAttempts; ++attempt) {
        const auto candidate = fs::path(upload_root) / cosmo::util::GenerateUUID();
        ec.clear();
        if (!fs::create_directory(candidate, ec)) {
            if (ec) {
                LOG_ERRO("Cannot create upload temporary directory {}: {}", candidate.string(), ec.message());
                return {};
            }
            continue;
        }

        fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, ec);
        if (ec) {
            LOG_ERRO("Cannot secure upload temporary directory {}: {}", candidate.string(), ec.message());
            std::error_code cleanup_ec;
            fs::remove(candidate, cleanup_ec);
            return {};
        }

        std::string resolved;
        if (!ResolveExistingPathWithinRoot(upload_root, candidate.string(), PathEntryType::kDirectory,
                                           resolved)) {
            std::error_code cleanup_ec;
            fs::remove(candidate, cleanup_ec);
            return {};
        }
        return resolved;
    }

    LOG_ERRO("Cannot allocate a unique upload temporary directory under {}", upload_root);
    return {};
}

std::string GetUpgradePath() {
    auto p = UpgradePath();
    EnsureDir(p);
    return p;
}

std::string GetLogPath() {
    auto p = LogPath();
    EnsureDir(p);
    return p;
}

std::string GetDbPath() {
    auto p = DbPath();
    EnsureDir(p);
    return p;
}

std::string GetDbBackUpPath() {
    auto p = DbBackupPath();
    EnsureDir(p);
    return p;
}

std::string GetBaseDir() {
    EnsureDir(g_dataPath);
    return g_dataPath;
}

std::string GetAppBaseDir() {
    return g_appDataPath;
}

[[nodiscard]] bool IsWithinRoot(const std::string& root, const std::string& candidate) {
    if (root.empty() || candidate.empty()) {
        return false;
    }
    const auto canonicalize = [](const std::string& s) {
        std::error_code ec;
        auto p = fs::weakly_canonical(fs::path(s), ec);
        if (ec) {
            return fs::path{};
        }
        return p.lexically_normal();  // strip trailing separators / empty tail components
    };
    const fs::path root_path = canonicalize(root);
    const fs::path cand_path = canonicalize(candidate);
    if (root_path.empty() || cand_path.empty()) {
        return false;
    }
    // Must use the 4-arg overload: the 3-arg std::mismatch is UB when cand_path
    // is shorter than root_path.
    const auto mismatch_result =
        std::mismatch(root_path.begin(), root_path.end(), cand_path.begin(), cand_path.end());
    return mismatch_result.first == root_path.end();
}

[[nodiscard]] bool ResolvePathWithinRoot(const std::string& root, const std::string& candidate,
                                         std::string& resolved) {
    resolved.clear();
    if (root.empty() || candidate.empty()) {
        return false;
    }

    std::error_code ec;
    const fs::path root_path = fs::canonical(fs::path(root), ec);
    if (ec || !fs::is_directory(root_path, ec) || ec) {
        return false;
    }

    const fs::path candidate_path = fs::weakly_canonical(fs::path(candidate), ec);
    if (ec || candidate_path.empty()) {
        return false;
    }

    const auto mismatch_result =
        std::mismatch(root_path.begin(), root_path.end(), candidate_path.begin(), candidate_path.end());
    if (mismatch_result.first != root_path.end()) {
        return false;
    }

    resolved = candidate_path.lexically_normal().string();
    return true;
}

[[nodiscard]] bool ResolveExistingPathWithinRoot(const std::string& root, const std::string& candidate,
                                                 PathEntryType expected_type, std::string& resolved) {
    resolved.clear();

    std::error_code ec;
    const fs::file_status link_status = fs::symlink_status(fs::path(candidate), ec);
    if (ec || !fs::exists(link_status) || fs::is_symlink(link_status)) {
        return false;
    }

    if (!ResolvePathWithinRoot(root, candidate, resolved)) {
        return false;
    }

    const fs::file_status status = fs::status(fs::path(resolved), ec);
    if (ec || !fs::exists(status)) {
        resolved.clear();
        return false;
    }
    if ((expected_type == PathEntryType::kRegularFile && !fs::is_regular_file(status)) ||
        (expected_type == PathEntryType::kDirectory && !fs::is_directory(status))) {
        resolved.clear();
        return false;
    }
    return true;
}

[[nodiscard]] bool IsSafePathComponent(const std::string& value, size_t max_length) {
    if (value.empty() || value.size() > max_length || value == "." || value == "..") {
        return false;
    }
    for (const unsigned char ch : value) {
        if (ch == '\0' || ch == '/' || ch == '\\' || ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return fs::path(value).filename() == fs::path(value);
}

[[nodiscard]] bool ValidateDirectoryTreeWithinRoot(const std::string& root, size_t max_entries,
                                                   std::uintmax_t max_file_bytes,
                                                   std::uintmax_t max_total_bytes) {
    std::string resolved_root;
    if (!ResolveExistingPathWithinRoot(root, root, PathEntryType::kDirectory, resolved_root)) {
        return false;
    }

    size_t entry_count        = 0;
    std::uintmax_t total_size = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(resolved_root, fs::directory_options::none, ec);
    const fs::recursive_directory_iterator end;
    if (ec) {
        return false;
    }

    for (; it != end; it.increment(ec)) {
        if (ec || ++entry_count > max_entries) {
            return false;
        }

        const auto link_status = it->symlink_status(ec);
        if (ec) {
            return false;
        }
        if (fs::is_symlink(link_status)) {
            // Permit symlinks whose targets resolve inside the root (the
            // normal shared-library versioning chains). Reject absolute or
            // root-escaping targets so an archive cannot write outside it.
            std::error_code read_ec;
            const fs::path target = fs::read_symlink(it->path(), read_ec);
            if (read_ec || target.empty() || target.is_absolute()) {
                return false;
            }
            fs::path resolved = it->path().parent_path() / target;
            const auto rel    = fs::relative(resolved, resolved_root, ec);
            if (ec || rel.empty()) {
                return false;
            }
            for (const auto& component : rel) {
                if (component == "..") {
                    return false;
                }
            }
            continue;  // a symlink itself contributes no extracted bytes
        }
        if (!fs::is_directory(link_status) && !fs::is_regular_file(link_status)) {
            return false;
        }

        std::string resolved_entry;
        if (!ResolveExistingPathWithinRoot(resolved_root, it->path().string(), PathEntryType::kAny,
                                           resolved_entry)) {
            return false;
        }

        const auto relative = fs::relative(resolved_entry, resolved_root, ec);
        if (ec || relative.empty()) {
            return false;
        }
        for (const auto& component : relative) {
            if (!IsSafePathComponent(component.string())) {
                return false;
            }
        }

        if (fs::is_regular_file(link_status)) {
            const auto file_size = fs::file_size(resolved_entry, ec);
            if (ec || file_size > max_file_bytes || file_size > max_total_bytes ||
                total_size > max_total_bytes - file_size) {
                return false;
            }
            total_size += file_size;
        }
    }
    return !ec;
}

// ── Event / recording paths ───────────────────────────────────────────────────

std::string GetEventRootPath(bool autoCreate) {
    return RecordPath(kAtEvent, autoCreate);
}

std::string GetEventPath(uint64_t timestamp, bool autoCreate) {
    return RecordPath(kAtEvent, timestamp, autoCreate);
}

std::string GetEventWebPath(uint64_t timestamp) {
    return WebPath(kAtEvent, timestamp);
}

std::string GetWebDir(const std::string& dir) {
    if (dir.size() > g_dataPath.size()) {
        return dir.substr(g_dataPath.size());
    }
    return {};
}

std::string GetWebRootPath() {
    return RecordPath(kAtWeb, false);
}

std::string GetWebLocalPath(uint64_t timestamp) {
    return RecordPath(kAtWeb, timestamp, true);
}

std::string GetWebLocalPath() {
    return RecordPath(kAtWeb, true);
}

std::string GetWebAcessPath(uint64_t timestamp) {
    return WebPath(kAtWeb, timestamp);
}

std::string GetWebAcessPath() {
    return WebPath(kAtWeb);
}

std::string GetTemporaryDirPath() {
    auto p = TemporaryDir();
    EnsureDir(p);
    return p;
}

// ── Model / algorithm paths ───────────────────────────────────────────────────

std::string GetModelPath() {
    auto p = ModelPath();
    EnsureDir(p);
    return p;
}

std::string GetPresetModelPath() {
    auto p = PresetModelPath();
    EnsureDir(p);
    return p;
}

std::vector<std::string> GetModelSearchPaths() {
    auto mp = ModelPath();
    auto pp = PresetModelPath();
    EnsureDir(mp);
    EnsureDir(pp);
    return {mp, pp};
}

std::string GetModelTemplatePath() {
    auto p = ModelTemplatePath();
    EnsureDir(p);
    return p;
}

std::string GetModelUploadTmpDir() {
    auto p = ModelUploadTmpDir();
    EnsureDir(p);
    return p;
}

std::string GetAlgorithmPath() {
    auto p = AlgorithmPath();
    EnsureDir(p);
    return p;
}

std::string GetResourcePath() {
    auto p = ResourcePath();
    EnsureDir(p);
    return p;
}

std::string GetLayoutPath() {
    auto p = LayoutPath();
    EnsureDir(p);
    return p;
}

std::string GetActionsJsonPath() {
    return (fs::path(GetLayoutPath()) / "actions.json").string();
}

std::string GetModelComponentsJsonPath() {
    return (fs::path(GetLayoutPath()) / "modelComponents.json").string();
}

std::string GetLinkageStoragesJsonPath() {
    return (fs::path(GetLayoutPath()) / "linkageStorages.json").string();
}

// ── Media paths ───────────────────────────────────────────────────────────────

std::string GetExportFileDir() {
    auto p = FileDir();
    EnsureDir(p);
    return p;
}

std::string GetFaceLibPhotoDir() {
    auto p = FaceLibPhotoDir();
    EnsureDir(p);
    return p;
}

std::string GetPersonLibPhotoDir() {
    auto p = PersonLibPhotoDir();
    EnsureDir(p);
    return p;
}

std::string GetPersonLibPhotoWebDir() {
    return kAtPersonLibPicWeb;
}

std::string GetArticlesReidLibPhotoDir() {
    auto p = ArticlesReidLibPhotoDir();
    EnsureDir(p);
    return p;
}

std::string GetCameraPath() {
    auto p = CameraPath();
    EnsureDir(p);
    return p;
}

std::string GetLogoPath() {
    return RecordPath(kAtLogoWeb, true);
}

std::string GetLogoWebPath() {
    return WebPath(kAtLogoWeb);
}

// ── Task record paths ─────────────────────────────────────────────────────────

std::string GetRecordRootPath(bool autoCreate) {
    std::string p = (fs::path(g_dataPath) / kCwaiRuntimeDir / "record").string();
    if (autoCreate) {
        EnsureDir(p);
    }
    return p;
}

std::string GetRecordJsonPath() {
    auto dataTime        = cosmo::util::GetCurrentDateTime();
    std::string datePath = COSMO_FORMAT("{:04}/{:02}/{:02}", dataTime.Date().Year(), dataTime.Date().Month(),
                                        dataTime.Date().Day());
    std::string p        = (fs::path(GetRecordRootPath()) / datePath).string();
    fs::create_directories(p);
    return p;
}

std::string GetTaskOverviewDataPath(const std::string& taskId, bool autoCreate) {
    if (!IsSafePathComponent(taskId)) {
        return {};
    }

    const fs::path root = fs::path(g_dataPath) / kCwaiRuntimeDir / "overview";
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        return {};
    }

    const fs::path candidate = root / taskId;
    std::string resolved;
    if (!ResolvePathWithinRoot(root.string(), candidate.string(), resolved)) {
        return {};
    }

    const fs::file_status link_status = fs::symlink_status(candidate, ec);
    if (!ec && fs::is_symlink(link_status)) {
        return {};
    }
    ec.clear();
    if (autoCreate) {
        fs::create_directories(candidate, ec);
        if (ec || !ResolveExistingPathWithinRoot(root.string(), candidate.string(), PathEntryType::kDirectory,
                                                 resolved)) {
            return {};
        }
    }
    return resolved;
}

}  // namespace cosmo::path
