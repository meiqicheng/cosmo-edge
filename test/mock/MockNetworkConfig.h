#pragma once
// clang-format off
#include "catch_amalgamated.hpp"
#include "catch2/trompeloeil.hpp"
// clang-format on
#include "service/network/INetworkConfig.h"
#include "trompeloeil.hpp"

namespace cosmo::test {

class MockNetworkConfig : public cosmo::service::INetworkConfig {
public:
    MAKE_MOCK0(Init, void(), override);
    MAKE_MOCK1(GetCardRealInfo, cosmo::platform::NetCardInfo(bool), override);
    MAKE_MOCK0(GetCardRealInfos, std::vector<cosmo::platform::NetCardInfo>(), override);
    MAKE_MOCK1(SetCardInfo, bool(const cosmo::platform::NetCardInfo&), override);
    MAKE_MOCK1(ApplyCardInfoAsync, void(const cosmo::platform::NetCardInfo&), override);
    MAKE_MOCK0(StopAsyncApply, void(), override);
    MAKE_MOCK0(GetCfgDns, std::vector<std::string>(), override);
    MAKE_MOCK1(SetDnss, bool(std::vector<std::string>), override);
    MAKE_MOCK3(SearchSetNewInfo, bool(cosmo::platform::NetCardInfo&, const std::string&, const std::string&),
               override);
    MAKE_MOCK0(GetNetCards, std::vector<cosmo::service::NetCardView>(), override);
    MAKE_MOCK0(GetHostIpAddress, std::string(), override);
    MAKE_MOCK2(ProbeNetworkQuality,
               cosmo::service::INetworkConfig::PingQualityResult(const std::string&, int), override);
    MAKE_MOCK1(IsIpAccessible, bool(const std::string&), override);
};

}  // namespace cosmo::test
