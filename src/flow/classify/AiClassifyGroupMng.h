// Instance manager for AiClassifierGroup actions (keyed by taskId + flowActionId).

#pragma once

#include "flow/action/ActionInstMngBase.h"
#include "flow/classify/AiClassifierGroup.h"

namespace cosmo {
class AiClassifyGroupMng : public SimpleMapActionMng<AiClassifierGroup> {
public:
    AiClassifyGroupMng() : SimpleMapActionMng("AiClassifyGroupMng") {}
};
}  // namespace cosmo
