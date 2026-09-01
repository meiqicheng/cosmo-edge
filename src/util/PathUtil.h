/// @file PathUtil.h
/// @brief Filesystem path helpers for the Cosmo application.
///
/// All path accessors are pure free-functions in the `cosmo::path` namespace.
/// Call cosmo::path::Init() once at application startup (in SwDevicePreInit)
/// to run one-time cleanup of transient directories.
///
/// Replaces the PathServiceImpl / IPathService DI layer — no virtual dispatch,
/// no ServiceRegistry registration needed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cosmo::path {

// ── Lifecycle ────────────────────────────────────────────────────────────────

/// One-time startup initialisation: clears per-request upload/web transient
/// directories while preserving resumable upload sessions, and recreates the
/// CWAI runtime data path. Call once from SwDevicePreInit().
void Init();

/// Override the application data roots before Init(). Both paths must be
/// absolute. This supports isolated side-by-side deployments and containers.
void OverrideRootPaths(const std::string& dataPath, const std::string& appDataPath);

/// Test-only: override the root data paths to isolate unit tests from the
/// real filesystem.  Must be called before any path accessor.
/// @param dataPath    Replaces the package-specific mutable data root
/// @param appDataPath Replaces the default /appfs/cosmo_wander/cwai_data
void OverrideRootPathForTest(const std::string& dataPath, const std::string& appDataPath);

// ── Configuration paths (formerly IPathConfig) ───────────────────────────────

/// Primary configuration directory.
std::string GetCfgPath();

/// Backup configuration directory.
std::string GetBackupCfgPath();

/// Primary configuration directory with @p subPath appended.
std::string GetCfgPath(const std::string& subPath);

/// Backup configuration directory with @p subPath appended.
std::string GetBackupCfgPath(const std::string& subPath);

/// Web file upload staging directory.
std::string GetUploadPath();

/// Per-request temporary upload directory (UUID-named subdirectory).
std::string GetUploadTmpPath();

/// Firmware upgrade package directory.
std::string GetUpgradePath();

/// Application log directory.
std::string GetLogPath();

/// Primary database file directory.
std::string GetDbPath();

/// Database backup directory.
std::string GetDbBackUpPath();

/// Root data directory for the application.
std::string GetBaseDir();

/// Read-only packaged application root (resource, files, font, and binaries).
std::string GetAppBaseDir();

/// True iff @p candidate, after canonicalization, is equal to or nested under
/// @p root (both canonicalized identically). Neither path is required to exist.
///
/// Canonicalization uses weakly_canonical (resolves symlinks and normalizes the
/// non-existent tail) and fails closed on filesystem errors. Comparison is
/// component-aware, not a string prefix: /data/a does NOT contain /data/ab.
/// Symlinks under @p root that resolve outside @p root are rejected because their
/// canonical form is no longer a prefix of @p root's canonical form. Returns false
/// if either argument is empty.
[[nodiscard]] bool IsWithinRoot(const std::string& root, const std::string& candidate);

/// Expected type for a path resolved by ResolveExistingPathWithinRoot().
enum class PathEntryType {
    kAny,
    kRegularFile,
    kDirectory,
};

/// Resolve @p candidate without permitting it to escape @p root.
///
/// Unlike IsWithinRoot(), this security-sensitive variant requires @p root to
/// exist and be a directory, never falls back to lexical-only normalization,
/// and returns the canonical/weakly-canonical path to be used by the caller.
/// The candidate itself need not exist.
[[nodiscard]] bool ResolvePathWithinRoot(const std::string& root, const std::string& candidate,
                                         std::string& resolved);

/// Resolve an existing filesystem entry beneath @p root and validate its type.
/// The final path component must not be a symbolic link. Ancestor symlinks are
/// resolved and accepted only when their target remains beneath @p root.
[[nodiscard]] bool ResolveExistingPathWithinRoot(const std::string& root, const std::string& candidate,
                                                 PathEntryType expected_type, std::string& resolved);

/// Return whether @p value is safe to use as one filesystem path component.
/// Empty values, dot components, separators, NUL/control bytes, and values
/// longer than @p max_length bytes are rejected.
[[nodiscard]] bool IsSafePathComponent(const std::string& value, size_t max_length = 128);

/// Validate every entry in an extracted directory tree without following
/// symbolic links. The root and every descendant must remain beneath @p root;
/// only directories and regular files are accepted. Path components, entry
/// count, per-file size, and aggregate regular-file size are checked. Returns
/// false on any filesystem error or limit violation.
[[nodiscard]] bool ValidateDirectoryTreeWithinRoot(const std::string& root, size_t max_entries,
                                                   std::uintmax_t max_file_bytes,
                                                   std::uintmax_t max_total_bytes);

// ── Event / recording paths (formerly IPathEvent) ────────────────────────────

/// Root directory for event recordings.
/// @param autoCreate Create the directory if absent (default: true).
std::string GetEventRootPath(bool autoCreate = true);

/// Time-partitioned event directory for @p timestamp (ms epoch).
/// @param autoCreate Create the directory if absent.
std::string GetEventPath(uint64_t timestamp, bool autoCreate = true);

/// Web-accessible URL path for event files at @p timestamp.
std::string GetEventWebPath(uint64_t timestamp);

/// Strip the data-path prefix from @p dir to produce a web-relative path.
std::string GetWebDir(const std::string& dir);

/// Root web-accessible temporary file directory (no auto-create).
std::string GetWebRootPath();

/// Local temporary file path partitioned by @p timestamp.
std::string GetWebLocalPath(uint64_t timestamp);

/// Local temporary file path (current-time partition).
std::string GetWebLocalPath();

/// Web-accessible URL path for a temporary file at @p timestamp.
std::string GetWebAcessPath(uint64_t timestamp);

/// Web-accessible URL path for a temporary file (current-time partition).
std::string GetWebAcessPath();

/// Application temporary working directory.
std::string GetTemporaryDirPath();

// ── Model / algorithm paths (formerly IPathModel) ────────────────────────────

/// Primary user model storage directory.
std::string GetModelPath();

/// Preset (built-in, device-bound) model storage directory. Models here ship
/// with the product and are encrypted — they must not be deleted or exported
/// to other machines, but their configuration remains editable. Counterpart to
/// GetModelPath(); GetModelSearchPaths() returns both.
std::string GetPresetModelPath();

/// All directories to search for model files (user + preset).
std::vector<std::string> GetModelSearchPaths();

/// Model template directory (pre-installed default models).
std::string GetModelTemplatePath();

/// Temporary directory for atomic model uploads.
std::string GetModelUploadTmpDir();

/// Algorithm package storage directory.
std::string GetAlgorithmPath();

/// Shared resource directory (fonts, icons, etc.).
std::string GetResourcePath();

/// Algorithm layout configuration directory.
std::string GetLayoutPath();

/// Path to the actions.json definition file.
std::string GetActionsJsonPath();

/// Path to the modelComponents.json definition file.
std::string GetModelComponentsJsonPath();

/// Path to the linkageStorages.json definition file.
std::string GetLinkageStoragesJsonPath();

/// File export staging directory.
std::string GetExportFileDir();

/// Face library photo storage directory.
std::string GetFaceLibPhotoDir();

/// Person library photo storage directory.
std::string GetPersonLibPhotoDir();

/// Web-accessible person library photo directory.
std::string GetPersonLibPhotoWebDir();

/// Body/articles ReID library photo directory.
std::string GetArticlesReidLibPhotoDir();

/// Camera configuration storage directory.
std::string GetCameraPath();

/// System logo file storage directory.
std::string GetLogoPath();

/// Web-accessible system logo directory.
std::string GetLogoWebPath();

// ── Task record paths (formerly IPathService) ─────────────────────────────────

/// Stable root directory containing all date-partitioned task records.
/// @param autoCreate Create the directory if absent (default: true).
std::string GetRecordRootPath(bool autoCreate = true);

/// Path to the daily task record JSON directory.
std::string GetRecordJsonPath();

/// Task overview data directory for @p taskId.
std::string GetTaskOverviewDataPath(const std::string& taskId, bool autoCreate = true);

}  // namespace cosmo::path
