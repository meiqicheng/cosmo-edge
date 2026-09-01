// Concrete IFileService implementation. Manages the file-upload
// thread pool and delegates to network/file utilities
// (HttpFileServerCliThread, HttpFileServerCli, HttpFileDownload).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "network/http/HttpPost.h"
#include "service/path/IFileService.h"
#include "service/path/impl/file/HttpFileServerCliCom.h"
#include "service/path/impl/file/HttpFileServerCliThread.h"
#include "util/PeriodicTimer.h"

namespace cosmo::service {

/// Number of worker threads for file upload operations.
static constexpr int kWorkerThreadCount = 4;

namespace detail {

    [[nodiscard]] std::size_t CalculateImageDownloadBudgetBytes(std::uint64_t total_bytes,
                                                                std::uint64_t available_bytes);

}  // namespace detail

class FileServiceImpl final : public IFileService {
public:
    explicit FileServiceImpl(size_t workerQueueCapacity = 0xffff);
    ~FileServiceImpl();

    // -- IFileService --
    std::string GetFileUrl(FileType type) override;

    void UploadFile(const std::string& taskId, const FileUploadCallback& callback, void* userData,
                    const std::string& ext, const std::string& localPath, const std::string& bucket,
                    const std::string& remoteUrl) override;

    bool DownloadFile(const std::string& url, std::vector<uint8_t>& data) override;

private:
    // --- Lifecycle ---
    void Shutdown();

    // --- Internal dispatch ---
    int DispatchUpload(std::string req_id, const cosmo::service::HFSCallBacK& cb_func, void* req,
                       cosmo::FMsgRspGetFileUrl& req_crt, std::string filepath, std::string bucket,
                       std::string file_url);

    // --- URL fetch and cache ---
    bool ReachLimit(FileType file_type) const;
    void UpdateIpPort(const std::string& ip_port);

    // --- Thread pool ---
    std::vector<std::shared_ptr<cosmo::service::CHttpFileServerCliThread>> worker_threads_;
    std::atomic<size_t> current_img_idx_{0};
    std::mutex worker_mutex_;
    cosmo::network::http::HttpFileServerCli http_file_cli_;
    cosmo::network::http::HttpPost http_post_;

    // --- URL cache ---
    std::map<FileType, std::list<std::string>> url_buffer_;
    std::map<FileType, int> url_list_size_{};
    std::shared_mutex url_mutex_;
    std::string ip_port_;
    std::unique_ptr<cosmo::PeriodicTimer> url_timer_;
};

}  // namespace cosmo::service
