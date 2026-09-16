#pragma once

#include "flow/action/ActionInstMngBase.h"
#include "flow/logical/PLogicalJudgment.h"

namespace cosmo {
class PLogicalJudgmentMng : public SimpleMapActionMng<PLogicalJudgment> {
public:
    PLogicalJudgmentMng() : SimpleMapActionMng("PLogicalJudgmentMng") {}
};
}  // namespace cosmo
