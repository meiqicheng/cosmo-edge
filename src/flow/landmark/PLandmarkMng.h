// Instance manager for PLandmark picture-mode actions.

#pragma once

#include "flow/action/ActionInstMngBase.h"
#include "flow/landmark/PLandmark.h"

namespace cosmo {
class PLandmarkMng : public SimpleMapActionMng<PLandmark> {
public:
    PLandmarkMng() : SimpleMapActionMng("PLandmarkMng") {}
};
}  // namespace cosmo
