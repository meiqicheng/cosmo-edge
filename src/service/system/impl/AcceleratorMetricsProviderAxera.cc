#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "service/system/impl/AcceleratorMetricsProvider.h"

namespace cosmo::service::detail {
namespace {

    // AX650 NPU utilization queries are not part of the public ax_engine API
    // surface yet; report a neutral "no accelerator" state until the runtime
    // exposes npu_perf counters (mirrors the CPU backend fallback).
    class AxeraAcceleratorMetricsProvider final : public AcceleratorMetricsProvider {
    public:
        cosmo::MsgGpuInfo QueryUtilization() override {
            cosmo::MsgGpuInfo result;
            result.memoryDomain = "none";
            return result;
        }

        int64_t QueryAvailableMemoryMB() override {
            return 0;
        }
    };

}  // namespace

std::unique_ptr<AcceleratorMetricsProvider> CreateAcceleratorMetricsProvider() {
    return std::make_unique<AxeraAcceleratorMetricsProvider>();
}

}  // namespace cosmo::service::detail

#endif  // COSMO_NN_USE_AXERA_BACKEND
