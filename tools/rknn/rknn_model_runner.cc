#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "rknn_api.h"

namespace {

class ContextGuard {
public:
    ~ContextGuard() {
        if (context_ != 0) {
            rknn_destroy(context_);
        }
    }
    rknn_context* Out() {
        return &context_;
    }
    rknn_context Get() const {
        return context_;
    }

private:
    rknn_context context_{0};
};

template <typename T>
std::vector<T> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("cannot open: " + path.string());
    }
    const auto bytes = stream.tellg();
    if (bytes <= 0 || bytes % static_cast<std::streamoff>(sizeof(T)) != 0) {
        throw std::runtime_error("invalid byte count: " + path.string());
    }
    std::vector<T> data(static_cast<std::size_t>(bytes) / sizeof(T));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(data.data()), bytes);
    if (!stream) {
        throw std::runtime_error("short read: " + path.string());
    }
    return data;
}

void WriteFile(const std::filesystem::path& path, const void* data, std::size_t bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot create: " + path.string());
    }
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    if (!stream) {
        throw std::runtime_error("short write: " + path.string());
    }
}

void Check(int result, const std::string& action) {
    if (result != RKNN_SUCC) {
        throw std::runtime_error(action + " failed with RKNN code " + std::to_string(result));
    }
}

std::optional<rknn_core_mask> ParseCoreMask(const std::string& value) {
    if (value == "auto")
        return std::nullopt;
    if (value == "0")
        return RKNN_NPU_CORE_0;
    if (value == "1")
        return RKNN_NPU_CORE_1;
    if (value == "2")
        return RKNN_NPU_CORE_2;
    if (value == "01")
        return RKNN_NPU_CORE_0_1;
    if (value == "012" || value == "0_1_2")
        return RKNN_NPU_CORE_0_1_2;
    throw std::runtime_error("core mask must be one of: auto, 0, 1, 2, 01, 012");
}

std::size_t ElementCount(const rknn_tensor_attr& attr) {
    if (attr.n_elems != 0) {
        return attr.n_elems;
    }
    return std::accumulate(attr.dims, attr.dims + attr.n_dims, std::size_t{1},
                           [](std::size_t product, std::uint32_t value) { return product * value; });
}

std::string Shape(const rknn_tensor_attr& attr) {
    std::string result;
    for (std::uint32_t index = 0; index < attr.n_dims; ++index) {
        if (!result.empty()) {
            result += 'x';
        }
        result += std::to_string(attr.dims[index]);
    }
    return result;
}

void PrintTiming(const char* name, const std::vector<double>& values) {
    const auto sum    = std::accumulate(values.begin(), values.end(), 0.0);
    const auto minmax = std::minmax_element(values.begin(), values.end());
    std::cout << name << "_mean_ms=" << sum / values.size() << '\n';
    std::cout << name << "_min_ms=" << *minmax.first << '\n';
    std::cout << name << "_max_ms=" << *minmax.second << '\n';
}

std::vector<float> NchwToNhwc(const std::vector<float>& source, const rknn_tensor_attr& attr) {
    if (attr.n_dims != 4) {
        throw std::runtime_error("validation runner requires a four-dimensional image input");
    }
    std::size_t batch    = attr.dims[0];
    std::size_t channels = 0;
    std::size_t height   = 0;
    std::size_t width    = 0;
    if (attr.fmt == RKNN_TENSOR_NCHW) {
        channels = attr.dims[1];
        height   = attr.dims[2];
        width    = attr.dims[3];
    } else if (attr.fmt == RKNN_TENSOR_NHWC) {
        height   = attr.dims[1];
        width    = attr.dims[2];
        channels = attr.dims[3];
    } else {
        throw std::runtime_error("validation runner supports only NCHW/NHWC native inputs");
    }
    std::vector<float> result(source.size());
    for (std::size_t n = 0; n < batch; ++n) {
        for (std::size_t c = 0; c < channels; ++c) {
            for (std::size_t h = 0; h < height; ++h) {
                for (std::size_t w = 0; w < width; ++w) {
                    const auto source_index = ((n * channels + c) * height + h) * width + w;
                    const auto target_index = ((n * height + h) * width + w) * channels + c;
                    result[target_index]    = source[source_index];
                }
            }
        }
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argc > 9) {
        std::cerr << "Usage: " << argv[0]
                  << " <model.rknn> <input.bin> <output-dir> [iterations=1] [warmup=1]"
                     " [auto|0|1|2|01|012] [float32|uint8|int8] [float32|native]\n";
        return 2;
    }

    try {
        const auto model = ReadFile<std::uint8_t>(argv[1]);
        const std::filesystem::path output_dir(argv[3]);
        const int iterations          = argc >= 5 ? std::stoi(argv[4]) : 1;
        const int warmup              = argc >= 6 ? std::stoi(argv[5]) : 1;
        const auto core_mask          = ParseCoreMask(argc >= 7 ? argv[6] : "auto");
        const std::string source_type = argc >= 8 ? argv[7] : "float32";
        const std::string output_type = argc >= 9 ? argv[8] : "float32";
        if (source_type != "float32" && source_type != "uint8" && source_type != "int8") {
            throw std::runtime_error("input source type must be float32, uint8, or int8");
        }
        if (output_type != "float32" && output_type != "native") {
            throw std::runtime_error("output type must be float32 or native");
        }
        const auto input_f32 = source_type == "float32" ? ReadFile<float>(argv[2]) : std::vector<float>{};
        const auto input_u8 =
            source_type == "uint8" ? ReadFile<std::uint8_t>(argv[2]) : std::vector<std::uint8_t>{};
        const auto input_i8 =
            source_type == "int8" ? ReadFile<std::int8_t>(argv[2]) : std::vector<std::int8_t>{};
        if (iterations <= 0 || warmup < 0) {
            throw std::runtime_error("iterations must be positive and warmup must be non-negative");
        }
        std::filesystem::create_directories(output_dir);

        ContextGuard context;
        if (model.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("model exceeds RKNN API size limit");
        }
        Check(rknn_init(context.Out(), const_cast<std::uint8_t*>(model.data()),
                        static_cast<std::uint32_t>(model.size()), 0, nullptr),
              "rknn_init");
        if (core_mask) {
            Check(rknn_set_core_mask(context.Get(), *core_mask), "rknn_set_core_mask");
        }

        rknn_sdk_version version{};
        Check(rknn_query(context.Get(), RKNN_QUERY_SDK_VERSION, &version, sizeof(version)),
              "RKNN_QUERY_SDK_VERSION");
        rknn_input_output_num counts{};
        Check(rknn_query(context.Get(), RKNN_QUERY_IN_OUT_NUM, &counts, sizeof(counts)),
              "RKNN_QUERY_IN_OUT_NUM");
        if (counts.n_input != 1) {
            throw std::runtime_error("validation runner currently requires exactly one input");
        }
        rknn_tensor_attr input_attr{};
        input_attr.index = 0;
        Check(rknn_query(context.Get(), RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr)),
              "RKNN_QUERY_INPUT_ATTR");
        std::vector<rknn_tensor_attr> output_attrs(counts.n_output);
        for (std::uint32_t index = 0; index < counts.n_output; ++index) {
            output_attrs[index].index = index;
            Check(rknn_query(context.Get(), RKNN_QUERY_OUTPUT_ATTR, &output_attrs[index],
                             sizeof(output_attrs[index])),
                  "RKNN_QUERY_OUTPUT_ATTR[" + std::to_string(index) + ']');
        }
        const auto element_count = ElementCount(input_attr);
        std::vector<float> input_nhwc;
        if (source_type == "float32") {
            if (input_f32.size() != element_count) {
                throw std::runtime_error("input float count " + std::to_string(input_f32.size()) +
                                         " does not match model element count " +
                                         std::to_string(element_count));
            }
            input_nhwc = NchwToNhwc(input_f32, input_attr);
        } else {
            const auto input_count = source_type == "uint8" ? input_u8.size() : input_i8.size();
            if (input_count != element_count) {
                throw std::runtime_error("input " + source_type + " count " + std::to_string(input_count) +
                                         " does not match model element count " +
                                         std::to_string(element_count));
            }
        }

        rknn_input input{};
        input.index = 0;
        input.buf =
            source_type == "float32"
                ? static_cast<void*>(input_nhwc.data())
                : (source_type == "uint8" ? static_cast<void*>(const_cast<std::uint8_t*>(input_u8.data()))
                                          : static_cast<void*>(const_cast<std::int8_t*>(input_i8.data())));
        input.size = static_cast<std::uint32_t>(source_type == "float32" ? input_nhwc.size() * sizeof(float)
                                                                         : element_count);
        input.pass_through = source_type == "int8" ? 1 : 0;
        input.type         = source_type == "float32"
                                 ? RKNN_TENSOR_FLOAT32
                                 : (source_type == "uint8" ? RKNN_TENSOR_UINT8 : RKNN_TENSOR_INT8);
        // RKNN Runtime 2.3.2 only accepts NHWC source layout on its input
        // conversion path. CosmoEdge supplies NCHW, so make the boundary copy
        // explicit rather than relying on a silently rejected NCHW request.
        input.fmt = RKNN_TENSOR_NHWC;
        Check(rknn_inputs_set(context.Get(), 1, &input), "rknn_inputs_set");

        const auto collect_outputs = [&](bool write_outputs, double* get_ms, double* release_ms) {
            std::vector<rknn_output> outputs(counts.n_output);
            for (std::uint32_t index = 0; index < counts.n_output; ++index) {
                outputs[index].index       = index;
                outputs[index].want_float  = output_type == "float32" ? 1 : 0;
                outputs[index].is_prealloc = 0;
            }
            const auto get_started = std::chrono::steady_clock::now();
            Check(rknn_outputs_get(context.Get(), counts.n_output, outputs.data(), nullptr),
                  "rknn_outputs_get");
            const auto get_finished = std::chrono::steady_clock::now();
            if (get_ms) {
                *get_ms = std::chrono::duration<double, std::milli>(get_finished - get_started).count();
            }
            if (write_outputs) {
                const auto suffix = output_type == "float32" ? ".f32.bin" : ".native.bin";
                for (std::uint32_t index = 0; index < counts.n_output; ++index) {
                    const auto path = output_dir / ("output-" + std::to_string(index) + suffix);
                    WriteFile(path, outputs[index].buf, outputs[index].size);
                    std::cout << "output_" << index << "_path=" << path << '\n';
                    std::cout << "output_" << index << "_bytes=" << outputs[index].size << '\n';
                }
            }
            const auto release_started = std::chrono::steady_clock::now();
            Check(rknn_outputs_release(context.Get(), counts.n_output, outputs.data()),
                  "rknn_outputs_release");
            const auto release_finished = std::chrono::steady_clock::now();
            if (release_ms) {
                *release_ms =
                    std::chrono::duration<double, std::milli>(release_finished - release_started).count();
            }
        };

        for (int index = 0; index < warmup; ++index) {
            Check(rknn_run(context.Get(), nullptr), "rknn_run warmup");
            collect_outputs(false, nullptr, nullptr);
        }

        std::vector<double> run_ms;
        std::vector<double> outputs_get_ms;
        std::vector<double> outputs_release_ms;
        run_ms.reserve(iterations);
        outputs_get_ms.reserve(iterations);
        outputs_release_ms.reserve(iterations);
        for (int index = 0; index < iterations; ++index) {
            const auto run_started = std::chrono::steady_clock::now();
            Check(rknn_run(context.Get(), nullptr), "rknn_run");
            const auto run_finished = std::chrono::steady_clock::now();
            run_ms.push_back(std::chrono::duration<double, std::milli>(run_finished - run_started).count());
            double get_ms     = 0.0;
            double release_ms = 0.0;
            collect_outputs(index + 1 == iterations, &get_ms, &release_ms);
            outputs_get_ms.push_back(get_ms);
            outputs_release_ms.push_back(release_ms);
        }

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "api_version=" << version.api_version << '\n';
        std::cout << "driver_version=" << version.drv_version << '\n';
        std::cout << "input_native_type=" << get_type_string(input_attr.type) << '\n';
        std::cout << "input_native_format=" << get_format_string(input_attr.fmt) << '\n';
        std::cout << "input_source_type=" << source_type << '\n';
        std::cout << "input_source_format=NHWC\n";
        std::cout << "output_source_type=" << output_type << '\n';
        for (std::uint32_t index = 0; index < output_attrs.size(); ++index) {
            const auto& attr = output_attrs[index];
            std::cout << "output_" << index << "_shape=" << Shape(attr) << '\n';
            std::cout << "output_" << index << "_native_type=" << get_type_string(attr.type) << '\n';
            std::cout << "output_" << index << "_native_format=" << get_format_string(attr.fmt) << '\n';
            std::cout << "output_" << index << "_quant=" << get_qnt_type_string(attr.qnt_type) << '\n';
            std::cout << "output_" << index << "_zero_point=" << attr.zp << '\n';
            std::cout << "output_" << index << "_scale=" << std::setprecision(9) << attr.scale
                      << std::setprecision(4) << '\n';
            std::cout << "output_" << index << "_native_bytes=" << attr.size << '\n';
            std::cout << "output_" << index << "_stride_bytes=" << attr.size_with_stride << '\n';
        }
        std::cout << "iterations=" << iterations << '\n';
        PrintTiming("run", run_ms);
        PrintTiming("outputs_get", outputs_get_ms);
        PrintTiming("outputs_release", outputs_release_ms);
        std::cout << "runner_status=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runner_status=FAIL error=" << error.what() << '\n';
        return 1;
    }
}
