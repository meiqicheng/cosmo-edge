// FileServiceImpl — Concrete IFileService implementation. Manages the file-upload

#include "service/path/impl/FileServiceImpl.h"

#include <fmt/format.h>
#include <sys/sysinfo.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

#include "media/EncodedImageInfo.h"
#include "network/http/HttpRequest.h"
#include "network/http/HttpRequestHandler.h"
#include "service/path/dto/FileServerMsgTypes.h"
#include "util/DateTimeFormat.h"
#include "util/Log.h"
#include "util/PathUtil.h"
#include "util/PeriodicTimer.h"
#include "util/StringUtil.h"
#include "util/UuidUtil.h"

namespace cosmo::service {

static constexpr const char* kTag = "FileServiceImpl";

constexpr int kCheckUrlTimeMsec = 3000;
constexpr int kMaxUrlSize       = 512;

namespace {
    constexpr std::uint64_t kImageMemoryReserveBytes   = 256ULL * 1024 * 1024;
    constexpr std::uint32_t kImageMemoryReservePercent = 10;
    constexpr std::uint64_t kBytesPerKibibyte          = 1024;

    struct MemoryAvailability {
        bool valid{false};
        std::uint64_t total_bytes{0};
        std::uint64_t available_bytes{0};
    };

    MemoryAvailability ReadProcMemoryAvailability() {
        std::ifstream file("/proc/meminfo");
        std::string line;
        MemoryAvailability result;
        bool has_total     = false;
        bool has_available = false;
        while (std::getline(file, line)) {
            std::istringstream stream(line);
            std::string key;
            std::string unit;
            std::uint64_t value_kib = 0;
            if (!(stream >> key >> value_kib >> unit) || unit != "kB" ||
                value_kib > std::numeric_limits<std::uint64_t>::max() / kBytesPerKibibyte) {
                continue;
            }
            if (key == "MemTotal:") {
                result.total_bytes = value_kib * kBytesPerKibibyte;
                has_total          = true;
            } else if (key == "MemAvailable:") {
                result.available_bytes = value_kib * kBytesPerKibibyte;
                has_available          = true;
            }
        }
        result.valid = has_total && has_available;
        return result;
    }

    MemoryAvailability ReadSysinfoMemoryAvailability() {
        struct sysinfo status {};
        if (sysinfo(&status) != 0 || status.mem_unit == 0) {
            return {};
        }
        const auto multiply = [unit = static_cast<std::uint64_t>(status.mem_unit)](unsigned long value) {
            const auto max_value = std::numeric_limits<std::uint64_t>::max();
            return static_cast<std::uint64_t>(value) > max_value / unit
                       ? max_value
                       : static_cast<std::uint64_t>(value) * unit;
        };
        const auto free    = multiply(status.freeram);
        const auto buffers = multiply(status.bufferram);
        return {true, multiply(status.totalram),
                free > std::numeric_limits<std::uint64_t>::max() - buffers
                    ? std::numeric_limits<std::uint64_t>::max()
                    : free + buffers};
    }

    std::size_t ImageDownloadBudgetBytes() {
        auto memory = ReadProcMemoryAvailability();
        if (!memory.valid) {
            memory = ReadSysinfoMemoryAvailability();
        }
        return memory.valid
                   ? detail::CalculateImageDownloadBudgetBytes(memory.total_bytes, memory.available_bytes)
                   : 0;
    }

    bool ContainsControlCharacter(std::string_view value) {
        return std::any_of(value.begin(), value.end(),
                           [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
    }

    bool IsAbsoluteHttpUrl(std::string_view value) {
        return value.compare(0, 7, "http://") == 0 || value.compare(0, 8, "https://") == 0;
    }

    bool IsSupportedRemoteUrl(std::string_view value) {
        if (value.empty()) {
            return false;
        }
        return value.front() == '/' || IsAbsoluteHttpUrl(value);
    }

    bool IsSupportedUploadMetadata(const std::string& task_id, const std::string& extension,
                                   const std::string& bucket, const std::string& remote_url) {
        static constexpr std::array<std::string_view, 4> kExtensions{"jpg", "json", "mp4", "feature"};
        static constexpr std::array<std::string_view, 3> kBuckets{"gaf_commodity", "gaf_commodity_video",
                                                                  "gaf_feature"};
        const auto normalized_extension = cosmo::util::ToLower(extension);
        return !task_id.empty() && task_id.size() <= 128 && !ContainsControlCharacter(task_id) &&
               std::find(kExtensions.begin(), kExtensions.end(), std::string_view(normalized_extension)) !=
                   kExtensions.end() &&
               std::find(kBuckets.begin(), kBuckets.end(), std::string_view(bucket)) != kBuckets.end() &&
               !remote_url.empty() && remote_url.size() <= 2048 && !ContainsControlCharacter(remote_url) &&
               IsSupportedRemoteUrl(remote_url);
    }

    bool ResolveManagedPlatformUpload(const std::string& local_path, const std::string& extension,
                                      std::string& resolved) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto status = fs::symlink_status(local_path, ec);
        if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)) {
            return false;
        }

        const std::array<std::string, 2> roots{cosmo::path::GetEventRootPath(false),
                                               cosmo::path::GetRecordRootPath(false)};
        bool managed = false;
        for (const auto& root : roots) {
            if (cosmo::path::ResolveExistingPathWithinRoot(
                    root, local_path, cosmo::path::PathEntryType::kRegularFile, resolved)) {
                managed = true;
                break;
            }
        }
        if (!managed) {
            return false;
        }

        const auto size = fs::file_size(resolved, ec);
        if (ec || size == 0) {
            return false;
        }
        auto actual_extension = cosmo::util::ToLower(fs::path(resolved).extension().string());
        if (!actual_extension.empty() && actual_extension.front() == '.') {
            actual_extension.erase(actual_extension.begin());
        }
        return actual_extension == cosmo::util::ToLower(extension);
    }
}  // namespace

std::size_t detail::CalculateImageDownloadBudgetBytes(std::uint64_t total_bytes,
                                                      std::uint64_t available_bytes) {
    const auto percentage_reserve =
        total_bytes / 100 * kImageMemoryReservePercent + total_bytes % 100 * kImageMemoryReservePercent / 100;
    const auto reserve = std::max(kImageMemoryReserveBytes, percentage_reserve);
    const auto usable  = available_bytes > reserve ? available_bytes - reserve : 0;
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        {usable, media::kVideoFrameMaxSize, std::numeric_limits<std::size_t>::max()}));
}

FileServiceImpl::FileServiceImpl(size_t worker_queue_capacity) {
    worker_threads_.reserve(kWorkerThreadCount);
    for (int index = 0; index < kWorkerThreadCount; ++index) {
        auto worker = std::make_shared<cosmo::service::CHttpFileServerCliThread>(
            fmt::format("FileUpload_{}", index), http_file_cli_, http_post_,
            [this](std::string ip_port) { UpdateIpPort(ip_port); }, worker_queue_capacity);
        if (!worker->start()) {
            LOG_ERRO("{} failed to start upload worker {}", kTag, index);
            Shutdown();
            return;
        }
        worker_threads_.push_back(std::move(worker));
    }
}

// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------
FileServiceImpl::~FileServiceImpl() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void FileServiceImpl::Shutdown() {
    if (url_timer_) {
        url_timer_->Destroy();
        url_timer_.reset();
    }
    std::vector<std::shared_ptr<cosmo::service::CHttpFileServerCliThread>> workers;
    {
        std::lock_guard<std::mutex> worker_lock(worker_mutex_);
        workers.swap(worker_threads_);
    }
    for (auto& thread : workers) {
        thread->Stop();
    }
}

// ---------------------------------------------------------------------------
// URL management internals
// ---------------------------------------------------------------------------
void FileServiceImpl::UpdateIpPort(const std::string& ip_port) {
    if (ip_port.empty()) {
        return;
    }

    // Build the updated map outside the lock to minimise hold time.
    std::unique_lock<std::shared_mutex> lck(url_mutex_);
    if (ip_port_ == ip_port) {
        return;
    }

    for (auto& [type, urls] : url_buffer_) {
        for (auto& url : urls) {
            url.replace(0, ip_port_.length(), ip_port);
        }
    }

    ip_port_ = ip_port;

    for (auto& [type, urls] : url_buffer_) {
        for (auto& item : urls) {
            const std::string kHttpPrefix = "http";
            if (item.substr(0, kHttpPrefix.length()) != kHttpPrefix) {
                item = ip_port_ + item;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Internal dispatch
// ---------------------------------------------------------------------------
int FileServiceImpl::DispatchUpload(std::string req_id, const cosmo::service::HFSCallBacK& cb_func, void* req,
                                    cosmo::FMsgRspGetFileUrl& req_crt, std::string filepath,
                                    std::string bucket, std::string file_url) {
    std::shared_ptr<cosmo::service::CHttpFileServerCliThread> worker;
    {
        std::lock_guard<std::mutex> worker_lock(worker_mutex_);
        if (worker_threads_.empty()) {
            // No task exists yet, so no worker-side ClearMsg callback can
            // report this rejection to the caller.
            return -2;
        }
        const size_t idx = current_img_idx_.fetch_add(1, std::memory_order_relaxed) % worker_threads_.size();
        worker           = worker_threads_[idx];
    }

    cosmo::MsgEnvelope msg(static_cast<int>(cosmo::service::FileServerMsgId::kUploadFile),
                           std::make_unique<cosmo::service::CUploadFileTask>(
                               std::move(req_id), std::move(cb_func), req, std::move(bucket),
                               std::move(file_url), std::move(req_crt), std::move(filepath)));

    // Put may synchronously reject a message and invoke its callback.  Keep it
    // outside worker_mutex_ so callbacks may safely re-enter UploadFile.
    return worker->Put(std::move(msg));
}

// ---------------------------------------------------------------------------
// IFileService overrides
// ---------------------------------------------------------------------------
bool FileServiceImpl::ReachLimit(FileType file_type) const {
    auto it = url_list_size_.find(file_type);
    if (it == url_list_size_.end()) {
        return false;
    }
    return file_type == FileType::Image ? it->second < kMaxUrlSize : it->second < kMaxUrlSize / 3;
}

std::string FileServiceImpl::GetFileUrl(FileType type) {
    std::unique_lock<std::shared_mutex> lck(url_mutex_);
    if (url_buffer_[type].empty()) {
        LOG_ERRO("{} url is empty!", kTag);
        return "";
    }

    auto date_time = cosmo::util::GetCurrentDateTime();
    auto year      = date_time.Date().Year();
    auto month     = date_time.Date().Month();
    auto date_str  = fmt::format("{}{:02d}", year, month);

    std::string url = url_buffer_[type].front();
    size_t pos      = url.rfind("/");
    if (pos != std::string::npos) {
        std::string suffix = url.substr(pos + 1);
        suffix.replace(0, date_str.length(), date_str);
        url.replace(pos + 1, suffix.length(), suffix);
    }

    url_buffer_[type].pop_front();
    url_list_size_[type] = static_cast<int>(url_buffer_[type].size());
    if (ReachLimit(type)) {
        LOG_WARN("{} url used too fast, maybe need get new url!", kTag);
    }
    return url;
}

void FileServiceImpl::UploadFile(const std::string& taskId, const FileUploadCallback& callback,
                                 void* userData, const std::string& ext, const std::string& localPath,
                                 const std::string& bucket, const std::string& remoteUrl) {
    std::string managed_path;
    if (!IsSupportedUploadMetadata(taskId, ext, bucket, remoteUrl) ||
        !ResolveManagedPlatformUpload(localPath, ext, managed_path)) {
        LOG_WARN("{} rejected invalid platform upload request", kTag);
        if (callback) {
            callback(taskId, false, userData);
        }
        return;
    }

    cosmo::FMsgRspGetFileUrl req_crt{true, ext};
    const auto dispatch_result = DispatchUpload(
        taskId,
        [callback](const std::string& id, bool finished, void* ptr) {
            if (callback) {
                callback(id, finished, ptr);
            }
        },
        userData, req_crt, managed_path, bucket, remoteUrl);
    // A bounded-queue rejection (-1) is synchronously reported by the
    // worker's ClearMsg callback. Only the no-worker case needs reporting here.
    if (dispatch_result == -2 && callback) {
        callback(taskId, false, userData);
    }
}

bool FileServiceImpl::DownloadFile(const std::string& url, std::vector<uint8_t>& data) {
    data.clear();
    if (url.empty() || url.size() > 2048 || ContainsControlCharacter(url) || !IsAbsoluteHttpUrl(url)) {
        LOG_WARN("{} rejected invalid download URL", kTag);
        return false;
    }
    const auto memory_budget = ImageDownloadBudgetBytes();
    if (memory_budget == 0) {
        LOG_WARN("{} cannot reserve memory for image download", kTag);
        return false;
    }
    cosmo::network::http::HttpImageHandler http_hnd(memory_budget);
    cosmo::network::http::HttpRequest http_req(url, &http_hnd);
    http_req.SetTimeout(200);
    auto ret_code = static_cast<int>(http_req.Submit(cosmo::network::http::HttpRequestMethod::kGet));
    if (ret_code != 200) {
        LOG_ERRO("{} Download failed, status:{}", kTag, ret_code);
        return false;
    }

    data = http_hnd.GetImageData();
    if (data.empty()) {
        LOG_WARN("{} Download returned an empty body", kTag);
        return false;
    }
    return true;
}

}  // namespace cosmo::service
