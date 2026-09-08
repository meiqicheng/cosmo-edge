#pragma once

#include "flow/action/ActionInstMngBase.h"
#include "flow/plate/AiPlateRecognition.h"

namespace cosmo {

class AiPlateRecognitionMng : public SimpleMapActionMng<AiPlateRecognition> {
public:
    AiPlateRecognitionMng() : SimpleMapActionMng("AiPlateRecognitionMng") {}
};

}  // namespace cosmo
