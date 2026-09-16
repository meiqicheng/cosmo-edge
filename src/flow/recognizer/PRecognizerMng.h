#pragma once

#include "flow/action/ActionInstMngBase.h"
#include "flow/recognizer/PRecognizer.h"

namespace cosmo {
class PRecognizerMng : public SimpleMapActionMng<PRecognizer> {
public:
    PRecognizerMng() : SimpleMapActionMng("PRecognizerMng") {}
};
}  // namespace cosmo
