/// @file HttpFileServerCliCom.h
/// @brief File-server upload task message types for the worker
///        thread pool in FileServiceImpl.
#pragma once

#include <string>

#include "network/msg/MsgTask.h"
#include "service/path/impl/file/HttpFileServerCli.h"

namespace cosmo::service {

using HFSCallBacK = std::function<void(std::string, bool, void* ptr)>;

enum class FileServerMsgId {
    kUploadFile = 0,
};

// Standard upload task
struct CUploadFileTask : cosmo::MsgTask {
    CUploadFileTask(const std::string& req_id, const HFSCallBacK& cb_func, void* user_data,
                    const std::string& bkt, const std::string& url, const FMsgRspGetFileUrl& req_crt,
                    const std::string& fpath)
        : res_get_file_url(req_crt),
          task_id(req_id),
          callback(cb_func),
          bucket(bkt),
          file_url(url),
          req(user_data),
          upload_filepath(fpath) {}
    CUploadFileTask(std::string&& req_id, HFSCallBacK&& cb_func, void* user_data, std::string&& bkt,
                    std::string&& url, FMsgRspGetFileUrl&& req_crt, std::string&& fpath)
        : res_get_file_url(std::move(req_crt)),
          task_id(std::move(req_id)),
          callback(std::move(cb_func)),
          bucket(std::move(bkt)),
          file_url(std::move(url)),
          req(user_data),
          upload_filepath(std::move(fpath)) {}

    ~CUploadFileTask() override = default;

    FMsgRspGetFileUrl res_get_file_url;
    std::string task_id;
    HFSCallBacK callback;
    std::string bucket;
    std::string file_url;
    void* req;
    std::string upload_filepath;
    FMsgReqGetFileUrl req_get_file_url;
};

}  // namespace cosmo::service
