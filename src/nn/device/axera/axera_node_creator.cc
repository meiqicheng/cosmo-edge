#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "nn/device/axera/axera_node_creator.h"

#include "nn/device/host/host_node_factory.h"

namespace cosmo::nn {

AxeraNodeCreator::AxeraNodeCreator(DeviceType device_type) : NodeCreator(device_type) {}

std::unique_ptr<Node> AxeraNodeCreator::CreateNode(NodeType type) {
    // Phase 1: route all nodes through the host factory so an AXERA build can
    // link and run with the software pipeline. Phase 3 adds AxeraNetNode
    // (AX_ENGINE_*) and AXERA preprocess nodes here.
    (void)type;
    return CreateHostNode(type);
}

NodeCreatorRegister<AxeraNodeCreator> g_axera_node_creator_register(DEVICE_AXERA);

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND
