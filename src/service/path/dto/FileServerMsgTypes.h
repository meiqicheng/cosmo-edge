// File-server message types for upload URL management.
#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>

#include "util/MsgBaseTypes.h"
#include "util/dto/FilterTypes.h"
#include "util/dto/OverviewTypes.h"

namespace cosmo {

struct FMsgSendHead {
    int resCode{0};  // 1: success, 0: failure
    std::vector<MsgResBase> resMsg;
};

void to_json(nlohmann::json& j, const FMsgSendHead& v);
void from_json(const nlohmann::json& j, FMsgSendHead& v);

// Async file upload design

// 3.1.1 Get async upload path
struct FMsgRspGetFileUrl {
    bool isHttps{true};
    std::string suffix{"mp4"};
    friend void to_json(nlohmann::json& j, const FMsgRspGetFileUrl& v);
    friend void from_json(const nlohmann::json& j, FMsgRspGetFileUrl& v);
};

// 3.1.1 Get async upload path
// 3.1.2 Standard upload
struct FMsgReqGetFileUrl : public FMsgSendHead {
    struct ResData {
        std::string fileUrl;
        friend void to_json(nlohmann::json& j, const ResData& v);
        friend void from_json(const nlohmann::json& j, ResData& v);
    } resData;
};

void to_json(nlohmann::json& j, const FMsgReqGetFileUrl& v);
void from_json(const nlohmann::json& j, FMsgReqGetFileUrl& v);

}  // namespace cosmo
