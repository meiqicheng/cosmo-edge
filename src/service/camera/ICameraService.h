// Aggregate camera service interface — inherits all narrow camera
// sub-interfaces (ICameraDeviceCrud, ICameraTaskConfig, ICameraChannelQuery).
// New code should prefer the narrow sub-interfaces; this aggregate is
// kept for backward compatibility.
#pragma once

#include "service/camera/ICameraChannelQuery.h"
#include "service/camera/ICameraDeviceCrud.h"
#include "service/camera/ICameraTaskConfig.h"

namespace cosmo::service {

/// Aggregate interface — inherits all narrow camera sub-interfaces.
/// New code should prefer narrow interfaces; this aggregate is kept for backward compatibility.
class ICameraService : public ICameraDeviceCrud, public ICameraTaskConfig, public ICameraChannelQuery {
public:
    virtual ~ICameraService() = default;

    /// Permanently stop camera monitor/switch workers and destroy all camera
    /// algorithm and channel tasks. Safe to call more than once.
    virtual void Stop() = 0;
};

}  // namespace cosmo::service
