#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "nn/device/axera/axera_net_node.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

#include "ax_engine_api.h"
#include "ax_sys_api.h"

#include "nn/core/status.h"
#include "util/Log.h"

namespace cosmo::nn {
namespace {

    constexpr uint32_t kIoAlignBytes = 128;

    Status AxeraError(const std::string& operation, int code) {
        return Status(COSMO_NN_ERR_NET, operation + " failed with AXERA code 0x" + [&] {
                   char buffer[16];
                   std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned int>(code));
                   return std::string(buffer);
               }());
    }

    size_t BlobElementCount(const BlobDesc& desc) {
        if (desc.dims.empty())
            return 0;
        size_t count = 1;
        for (int dim : desc.dims) {
            if (dim <= 0 || count > std::numeric_limits<size_t>::max() / static_cast<size_t>(dim))
                return 0;
            count *= static_cast<size_t>(dim);
        }
        return count;
    }

    // Engine initialization is process-wide; mirror the official sample which
    // calls AX_ENGINE_Init once with VNPU disabled and deinits at exit.
    void EnsureEngineInitialized() {
        static std::once_flag flag;
        std::call_once(flag, []() {
            AX_ENGINE_NPU_ATTR_T npu_attr;
            std::memset(&npu_attr, 0, sizeof(npu_attr));
            npu_attr.eHardMode = AX_ENGINE_VIRTUAL_NPU_DISABLE;
            if (AX_ENGINE_Init(&npu_attr) == 0)
                std::atexit([]() { AX_ENGINE_Deinit(); });
            else
                LOG_ERRO("[Axera] AX_ENGINE_Init failed; NPU driver may not be loaded");
        });
    }

    bool IsPackedNhwcShape(const std::vector<int>& shape) {
        return shape.size() == 4 && shape[0] == 1;
    }

}  // namespace

AxeraNetNode::AxeraNetNode() = default;

AxeraNetNode::~AxeraNetNode() {
    std::lock_guard<std::mutex> lock(mutex_);
    FreeIoBuffers();
    DestroyHandle();
}

DeviceType AxeraNetNode::GetTopBlobDeviceType() {
    return DEVICE_NAIVE;
}

Status AxeraNetNode::LoadWeight(const char* data, size_t size) {
    if (!data || size == 0)
        return Status(COSMO_NN_ERR_LOAD_MODEL, "AXERA model data is empty");
    if (size > std::numeric_limits<uint32_t>::max())
        return Status(COSMO_NN_ERR_LOAD_MODEL, "AXERA model exceeds the runtime size limit");

    std::lock_guard<std::mutex> lock(mutex_);
    FreeIoBuffers();
    DestroyHandle();
    try {
        model_data_.assign(reinterpret_cast<const unsigned char*>(data),
                           reinterpret_cast<const unsigned char*>(data) + size);
    } catch (const std::bad_alloc&) {
        return Status(COSMO_NN_ERR_OUT_OF_MEMORY, "Not enough memory to retain AXERA model data");
    }

    EnsureEngineInitialized();

    AX_ENGINE_HANDLE handle = nullptr;
    int ret = AX_ENGINE_CreateHandle(&handle, model_data_.data(), static_cast<AX_U32>(model_data_.size()));
    if (ret != 0)
        return AxeraError("AX_ENGINE_CreateHandle", ret);
    engine_handle_ = handle;

    if (AX_ENGINE_CreateContext(handle) != 0) {
        DestroyHandle();
        return AxeraError("AX_ENGINE_CreateContext", -1);
    }

    AX_ENGINE_IO_INFO_T* io_info = nullptr;
    if (AX_ENGINE_GetIOInfo(handle, &io_info) != 0 || io_info == nullptr) {
        DestroyHandle();
        return AxeraError("AX_ENGINE_GetIOInfo", -1);
    }
    io_info_ = io_info;

    auto status = QueryTensorMetadata();
    if (!status) {
        DestroyHandle();
        return status;
    }
    status = ResolveOutputAdapter();
    if (!status) {
        DestroyHandle();
        return status;
    }

    if (network_input_names.size() != input_metas_.size()) {
        DestroyHandle();
        return Status(COSMO_NN_ERR_INVALID_CFG, "AXERA input count does not match config.json");
    }
    const size_t logical_outputs = is_yolov8_ ? 1 : output_metas_.size();
    if (network_output_names.size() != logical_outputs) {
        DestroyHandle();
        return Status(COSMO_NN_ERR_INVALID_CFG, "AXERA logical output count does not match config.json");
    }

    // Allocate physical IO buffers (SoC mode: NPU DMA reads through phyAddr).
    const auto* info = static_cast<AX_ENGINE_IO_INFO_T*>(io_info_);
    io_input_buffers_.resize(input_metas_.size());
    for (size_t i = 0; i < input_metas_.size(); ++i) {
        AX_U64 phy = 0;
        AX_VOID* vir = nullptr;
        if (AX_SYS_MemAlloc(&phy, &vir, info->pInputs[i].nSize, kIoAlignBytes,
                            (const AX_S8*)"cosmo") != 0) {
            FreeIoBuffers();
            DestroyHandle();
            return Status(COSMO_NN_ERR_SOPHON_ALLOC_MEM_FAILED,
                          "AX_SYS_MemAlloc input failed");
        }
        io_input_buffers_[i] = {phy, vir, info->pInputs[i].nSize};
    }
    io_output_buffers_.resize(output_metas_.size());
    for (size_t i = 0; i < output_metas_.size(); ++i) {
        AX_U64 phy = 0;
        AX_VOID* vir = nullptr;
        if (AX_SYS_MemAlloc(&phy, &vir, info->pOutputs[i].nSize, kIoAlignBytes,
                            (const AX_S8*)"cosmo") != 0) {
            FreeIoBuffers();
            DestroyHandle();
            return Status(COSMO_NN_ERR_SOPHON_ALLOC_MEM_FAILED,
                          "AX_SYS_MemAlloc output failed");
        }
        io_output_buffers_[i] = {phy, vir, info->pOutputs[i].nSize};
    }

    const auto& input = input_metas_.front();
    detector_model_   = is_yolov8_ && input.shape.size() == 4 && input.shape[0] == 1 &&
                      input.shape[1] == 640 && input.shape[2] == 640 && input.shape[3] == 3;
    LOG_INFO("AXERA model loaded: inputs={} outputs={} logical_outputs={} yolov8={} class={} points={}",
             input_metas_.size(), output_metas_.size(), logical_outputs, is_yolov8_,
             yolov8_class_count_, yolov8_point_count_);
    return COSMO_NN_OK;
}

Status AxeraNetNode::QueryTensorMetadata() {
    const auto* info = static_cast<AX_ENGINE_IO_INFO_T*>(io_info_);
    if (!info || info->nInputSize == 0 || info->nOutputSize == 0 || info->nOutputSize > 64)
        return Status(COSMO_NN_ERR_UNSUPPORT_NET,
                      "AXERA backend currently requires 1+ inputs and 1-64 outputs");

    input_metas_.clear();
    for (uint32_t i = 0; i < info->nInputSize; ++i) {
        const auto& meta = info->pInputs[i];
        TensorMeta tm;
        tm.name   = meta.pName ? meta.pName : "";
        tm.shape.assign(meta.pShape, meta.pShape + meta.nShapeSize);
        tm.dtype  = static_cast<int>(meta.eDataType);
        tm.layout = static_cast<int>(meta.eLayout);
        tm.size   = meta.nSize;
        input_metas_.push_back(std::move(tm));
    }
    output_metas_.clear();
    for (uint32_t i = 0; i < info->nOutputSize; ++i) {
        const auto& meta = info->pOutputs[i];
        TensorMeta tm;
        tm.name   = meta.pName ? meta.pName : "";
        tm.shape.assign(meta.pShape, meta.pShape + meta.nShapeSize);
        tm.dtype  = static_cast<int>(meta.eDataType);
        tm.layout = static_cast<int>(meta.eLayout);
        tm.size   = meta.nSize;
        output_metas_.push_back(std::move(tm));
    }
    return COSMO_NN_OK;
}

Status AxeraNetNode::ResolveOutputAdapter() {
    std::vector<std::vector<int>> shapes;
    shapes.reserve(output_metas_.size());
    for (const auto& meta : output_metas_)
        shapes.push_back(meta.shape);

    std::string error;
    yolov8_layout_ = AxeraYolov8Layout{};
    is_yolov8_     = DetectAxeraYolov8Layout(shapes, yolov8_layout_, error);
    if (is_yolov8_) {
        yolov8_class_count_ = yolov8_layout_.class_count;
        yolov8_point_count_ = yolov8_layout_.point_count;
        yolov8_heads_.resize(shapes.size());
        for (size_t i = 0; i < shapes.size(); ++i) {
            yolov8_heads_[i].shape = shapes[i];
        }
    } else {
        for (const auto& meta : output_metas_) {
            if (meta.shape.empty() || meta.size == 0)
                return Status(COSMO_NN_ERR_UNSUPPORT_NET,
                              "AXERA model contains an invalid output shape");
        }
    }
    return COSMO_NN_OK;
}

Status AxeraNetNode::InferTopShapes() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_handle_)
        return Status(COSMO_NN_ERR_GRAPH_NOT_INIT, "AXERA handle is not initialized");
    top_blob_shapes.clear();
    top_blob_data_types.clear();
    if (is_yolov8_) {
        top_blob_shapes.push_back({1, 4 + yolov8_class_count_, yolov8_point_count_});
        top_blob_data_types.push_back(DATA_TYPE_FLOAT);
        return COSMO_NN_OK;
    }
    for (const auto& meta : output_metas_) {
        top_blob_shapes.push_back(meta.shape);
        top_blob_data_types.push_back(DATA_TYPE_FLOAT);
    }
    return COSMO_NN_OK;
}

Status AxeraNetNode::PrepareInput(const Blob& blob, std::vector<float>& nhwc, int& height,
                                  int& width) const {
    auto& mutable_blob = const_cast<Blob&>(blob);
    const auto desc    = mutable_blob.GetBlobDesc();
    const auto handle  = mutable_blob.GetHandle();
    if (!handle.base || desc.data_type != DATA_TYPE_FLOAT || desc.data_format != DATA_FORMAT_NCHW ||
        desc.dims.size() != 4 || desc.dims[0] != 1 || desc.dims[1] <= 0 || desc.dims[2] <= 0 ||
        desc.dims[3] <= 0) {
        return Status(COSMO_NN_ERR_INVALID_INPUT,
                      "AXERA input must be a non-empty batch-1 NCHW float blob");
    }
    const int channels = desc.dims[1];
    height             = desc.dims[2];
    width              = desc.dims[3];
    if (channels != 3)
        return Status(COSMO_NN_ERR_UNSUPPORT_NET, "AXERA CV backend currently requires three channels");

    const auto count = BlobElementCount(desc);
    if (count == 0 || count > std::numeric_limits<size_t>::max() / sizeof(float))
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AXERA input element count is invalid");
    try {
        nhwc.resize(count);
    } catch (const std::bad_alloc&) {
        return Status(COSMO_NN_ERR_OUT_OF_MEMORY, "Not enough memory for AXERA NHWC input");
    }

    // NCHW float -> NHWC float.
    const auto* nchw = static_cast<const float*>(handle.base);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < channels; ++c) {
                nhwc[(y * width + x) * channels + c] = nchw[c * height * width + y * width + x];
            }
        }
    }
    return COSMO_NN_OK;
}

void AxeraNetNode::FreeIoBuffers() {
    for (auto& buffer : io_input_buffers_) {
        if (buffer.vir_addr)
            AX_SYS_MemFree(buffer.phy_addr, buffer.vir_addr);
    }
    io_input_buffers_.clear();
    for (auto& buffer : io_output_buffers_) {
        if (buffer.vir_addr)
            AX_SYS_MemFree(buffer.phy_addr, buffer.vir_addr);
    }
    io_output_buffers_.clear();
}

void AxeraNetNode::DestroyHandle() {
    if (engine_handle_) {
        AX_ENGINE_DestroyHandle(static_cast<AX_ENGINE_HANDLE>(engine_handle_));
        engine_handle_ = nullptr;
    }
    io_info_ = nullptr;
}

Status AxeraNetNode::Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                             std::vector<std::shared_ptr<Blob>>& top_blobs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_handle_)
        return Status(COSMO_NN_ERR_GRAPH_NOT_INIT, "AXERA handle is not initialized");
    if (bottom_blobs.size() != 1 || !bottom_blobs[0])
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AXERA backend requires exactly one input blob");
    const size_t logical_outputs = is_yolov8_ ? 1 : output_metas_.size();
    if (top_blobs.size() != logical_outputs)
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AXERA output blob count mismatch");

    int input_height = 0, input_width = 0;
    auto prepare_status = PrepareInput(*bottom_blobs[0], input_nhwc_, input_height, input_width);
    if (!prepare_status)
        return prepare_status;

    auto* info = static_cast<AX_ENGINE_IO_INFO_T*>(io_info_);
    AX_ENGINE_IO_T io_data{};
    io_data.pInputs = new AX_ENGINE_IO_BUFFER_T[input_metas_.size()];
    io_data.pOutputs = new AX_ENGINE_IO_BUFFER_T[output_metas_.size()];

    const size_t input_bytes = input_nhwc_.size() * sizeof(float);
    if (input_bytes > io_input_buffers_[0].size) {
        delete[] io_data.pInputs;
        delete[] io_data.pOutputs;
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AXERA input exceeds the allocated IO buffer");
    }
    std::memcpy(io_input_buffers_[0].vir_addr, input_nhwc_.data(), input_bytes);
    for (size_t i = 0; i < input_metas_.size(); ++i) {
        io_data.pInputs[i].phyAddr  = io_input_buffers_[i].phy_addr;
        io_data.pInputs[i].pVirAddr = io_input_buffers_[i].vir_addr;
        io_data.pInputs[i].nSize    = io_input_buffers_[i].size;
    }
    for (size_t i = 0; i < output_metas_.size(); ++i) {
        io_data.pOutputs[i].phyAddr  = io_output_buffers_[i].phy_addr;
        io_data.pOutputs[i].pVirAddr = io_output_buffers_[i].vir_addr;
        io_data.pOutputs[i].nSize    = io_output_buffers_[i].size;
    }

    const int ret = AX_ENGINE_RunSync(static_cast<AX_ENGINE_HANDLE>(engine_handle_), &io_data);
    if (ret != 0) {
        delete[] io_data.pInputs;
        delete[] io_data.pOutputs;
        return AxeraError("AX_ENGINE_RunSync", ret);
    }

    Status result = COSMO_NN_OK;
    if (is_yolov8_) {
        auto top_desc          = top_blobs[0]->GetBlobDesc();
        const size_t top_count = BlobElementCount(top_desc);
        if (!top_blobs[0]->GetHandle().base || top_desc.data_type != DATA_TYPE_FLOAT) {
            result = Status(COSMO_NN_ERR_INVALID_INPUT, "AXERA YOLOv8 top blob is invalid");
        } else {
            for (size_t i = 0; i < output_metas_.size(); ++i) {
                yolov8_heads_[i].data = static_cast<const float*>(io_output_buffers_[i].vir_addr);
                yolov8_heads_[i].element_count =
                    io_output_buffers_[i].size / sizeof(float);
            }
            std::string adapter_error;
            if (!ReconstructAxeraYolov8(yolov8_heads_, input_height, input_width,
                                        static_cast<float*>(top_blobs[0]->GetHandle().base), top_count,
                                        adapter_error)) {
                result = Status(COSMO_NN_ERR_NET, adapter_error);
            }
        }
    } else {
        for (size_t i = 0; i < output_metas_.size(); ++i) {
            auto desc          = top_blobs[i]->GetBlobDesc();
            const size_t count = BlobElementCount(desc);
            if (!top_blobs[i]->GetHandle().base || desc.data_type != DATA_TYPE_FLOAT ||
                io_output_buffers_[i].size != count * sizeof(float)) {
                result = Status(COSMO_NN_ERR_NET, "AXERA output size does not match the graph blob");
                break;
            }
            std::memcpy(top_blobs[i]->GetHandle().base, io_output_buffers_[i].vir_addr,
                        io_output_buffers_[i].size);
        }
    }

    delete[] io_data.pInputs;
    delete[] io_data.pOutputs;
    return result;
}

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND
