#pragma once
// clang-format off
#include "catch_amalgamated.hpp"
#include "catch2/trompeloeil.hpp"
// clang-format on
#include "service/network/IMqttLifecycle.h"
#include "trompeloeil.hpp"

namespace cosmo::test {

class MockMqttLifecycle : public cosmo::service::IMqttLifecycle {
public:
    MAKE_MOCK0(IsMqttRegistered, bool(), override);
    MAKE_MOCK0(IsMqttEnabled, bool(), override);
    MAKE_MOCK0(MqttStop, void(), override);
    MAKE_MOCK0(MqttStart, void(), override);
    MAKE_MOCK0(MqttShutdown, void(), override);
};

}  // namespace cosmo::test
