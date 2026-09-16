// Instance manager for AiClassifierArea actions (keyed by taskId + flowActionId).

#pragma once

#include "flow/action/ActionInstMngBase.h"
#include "flow/classify/AiClassifierArea.h"

namespace cosmo {
class AiClassifyAreaMng : public SimpleMapActionMng<AiClassifierArea> {
public:
    AiClassifyAreaMng() : SimpleMapActionMng("AiClassifyAreaMng") {}
};
}  // namespace cosmo
