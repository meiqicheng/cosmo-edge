// PDetectorMng.h — Manager for per-task PDetector instances.

#pragma once

#include "flow/action/ActionInstMngBase.h"
#include "flow/detect/PDetector.h"

namespace cosmo {
class PDetectorMng : public SimpleMapActionMng<PDetector> {
public:
    PDetectorMng() : SimpleMapActionMng("PDetectorMng") {}
};
}  // namespace cosmo
