/// @file ITimerRestartService.h
/// @brief Scheduled restart service interface — manages periodic device
///        reboots based on configured schedules.
#pragma once

namespace cosmo::service {

/// Controls the scheduled device restart timer.
///
/// When started, periodically checks the configured reboot schedule
/// and initiates a device restart at the scheduled time.
class ITimerRestartService {
public:
    virtual ~ITimerRestartService() = default;

    /// Start the scheduled restart timer.
    virtual void Start() = 0;

    /// Stop the scheduled restart timer and wait for an in-flight callback.
    /// Safe to call more than once.
    virtual void Stop() = 0;
};

// Dependency injection methods

}  // namespace cosmo::service
