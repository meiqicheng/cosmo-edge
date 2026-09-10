#pragma once

// Shared, process-wide AX_SYS initialization for the AX650 SoC.
//
// The AXERA SDK's AX_SYS_Init() is NOT idempotent: calling it a second time
// (without an intervening AX_SYS_Deinit()) returns AX_ERR_SYS_ILLEGAL_PARAM
// (0x80060090). The media (VDEC/VENC) and NN (IVPS/engine) subsystems each
// previously guarded their own AX_SYS_Init() with a private std::once_flag,
// so a single process could call AX_SYS_Init() up to four times and the
// later calls would fail. That failure made the hardware VDEC fall back to
// the CPU decoder and the NPU engine fail to load models.
//
// This header centralizes the init behind ONE std::once_flag so AX_SYS_Init()
// runs exactly once per process. Every AXERA subsystem must call
// EnsureAxeraSysInitialized() instead of calling AX_SYS_Init() directly.

#include <cstdlib>
#include <mutex>

#include "ax_sys_api.h"

namespace cosmo::nn {

/// Initializes the AX_SYS layer exactly once per process.
/// Returns true when AX_SYS is ready (either just initialized or already
/// initialized by an earlier caller).
inline bool EnsureAxeraSysInitialized() {
    static std::once_flag flag;
    static bool ok = false;
    std::call_once(flag, []() {
        ok = (AX_SYS_Init() == 0);
    });
    return ok;
}

}  // namespace cosmo::nn
