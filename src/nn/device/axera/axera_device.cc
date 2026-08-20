#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "nn/device/naive/naive_device.h"

namespace cosmo::nn {

// Graph metadata and compatibility tensors retain the graph-owned NaiveDevice
// lifecycle. Native input allocation and AX_ENGINE_* IO binding are owned by
// AxeraNetNode/AXERA preprocess (Phase 3), so this registration does not imply
// host-copy inference on the AX650 SoC memory path.
TypeDeviceRegister<NaiveDevice> g_axera_device_register(DEVICE_AXERA);

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND
