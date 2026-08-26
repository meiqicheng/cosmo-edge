// Per-task native-inference capability contract.
//
// Replaces the former "every queued task is AA_00001 detect" string check
// with an explicit, extensible capability table. Unknown action codes resolve
// to the conservative default (fail-closed): they never enable the native-only
// fast path and always keep requiring a materialized host frame.

#pragma once

#include <string_view>
#include <vector>

namespace cosmo {

struct AlgTaskUnit;

struct AlgTaskNativeCapability {
    bool supports_native_input{false};
    bool requires_host_frame{true};
    bool requires_alarm_media{true};
    bool requires_crop_or_classification{true};

    [[nodiscard]] bool NativeOnlyEligible() const {
        return supports_native_input && !requires_host_frame && !requires_alarm_media &&
               !requires_crop_or_classification;
    }
};

AlgTaskNativeCapability ResolveAlgTaskNativeCapability(std::string_view actionId);

/// True only when every task in the list is native-only eligible. An empty
/// list is not eligible.
bool AlgTasksNativeOnlyEligible(const std::vector<AlgTaskUnit>& tasks);

}  // namespace cosmo
