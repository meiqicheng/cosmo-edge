// Instance manager for PClassifier actions (keyed by taskId + flowActionId).

#pragma once

#include "flow/action/ActionInstMngBase.h"
#include "flow/classify/PClassifier.h"

namespace cosmo {
class PClassifierMng : public SimpleMapActionMng<PClassifier> {
public:
    PClassifierMng() : SimpleMapActionMng("PClassifierMng") {}
};
}  // namespace cosmo
