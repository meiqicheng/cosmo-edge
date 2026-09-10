#ifdef COSMO_NN_USE_AXERA_BACKEND

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "service/system/impl/AcceleratorMetricsProvider.h"
#include "util/NnBackendConstants.h"

namespace cosmo::service::detail {
namespace {

    // AX650 SDK V3.10.2 真机（ax_npu V3.6.2）实测的 procfs 节点：
    //   /proc/ax_proc/npu/top          —— 各 vNPU core 的利用率块
    //   /proc/ax_proc/npu/clk          —— 当前 NPU 核心频率（如 "800M"）
    //   /proc/ax_proc/npu/enable       —— 利用率统计开关（默认关闭，需写 1）
    //   /proc/ax_proc/mem_cmm_info     —— CMM 媒体内存池总量/占用汇总
    constexpr const char* kNpuTopPath    = "/proc/ax_proc/npu/top";
    constexpr const char* kNpuClkPath    = "/proc/ax_proc/npu/clk";
    constexpr const char* kNpuEnablePath = "/proc/ax_proc/npu/enable";
    constexpr const char* kCmmInfoPath   = "/proc/ax_proc/mem_cmm_info";

    // NPU 负载快照：aggregate 取所有 core 中最忙者作为设备健康信号，
    // cores 保留全部逐 core 数值用于遥测。
    struct NpuLoadSnapshot {
        double aggregate{0.0};
        std::vector<double> cores;
    };

    // CMM 内存快照（MB）。AX650N 的 NPU 使用独立的 CMM 媒体内存池，
    // 而非共享系统内存，因此不能像 RKNN 那样读 /proc/meminfo。
    struct CmmSnapshot {
        int64_t total_mb{0};
        int64_t available_mb{0};
    };

    std::optional<unsigned int> ParseUnsigned(const std::ssub_match& match) {
        const auto text = match.str();
        unsigned int value{0};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size())
            return std::nullopt;
        return value;
    }

    std::string ReadFile(const char* path) {
        std::ifstream stream(path);
        if (!stream)
            return {};
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    // NPU perf 驱动按 vNPU core 逐块上报利用率（真机采样）：
    //   core:vnpu-Non
    //   time:2237
    //   period:1000000
    //   utilization:98%
    // 空载时输出 "nputop info is empty!"；统计未开启时输出
    // "nputop info is not enabled!"（需先 echo 1 > /proc/ax_proc/npu/enable）。
    // 两种状态都解析不出任何块，统一返回 nullopt，由调用方按不可用处理。
    std::optional<NpuLoadSnapshot> ParseNpuTop(const std::string& text) {
        static const std::regex core_pattern(
            R"(core:\s*(\S+)\s*\n\s*time:\s*([0-9]+)\s*\n\s*period:\s*([0-9]+)\s*\n\s*utilization:\s*([0-9]+)\s*%)");
        NpuLoadSnapshot result;
        bool found{false};
        for (auto it = std::sregex_iterator(text.begin(), text.end(), core_pattern);
             it != std::sregex_iterator(); ++it) {
            const auto percent = ParseUnsigned((*it)[4]);
            if (!percent || *percent > 100)
                return std::nullopt;
            const auto load = static_cast<double>(*percent) / 100.0;
            (void)(*it)[1];
            result.cores.push_back(load);
            result.aggregate = std::max(result.aggregate, load);
            found            = true;
        }
        if (!found)
            return std::nullopt;
        return result;
    }

    std::optional<NpuLoadSnapshot> ReadNpuTop() {
        const std::string text = []() {
            if (const char* configured_path = std::getenv("COSMO_AXNPU_TOP_PATH");
                configured_path && *configured_path) {
                return ReadFile(configured_path);
            }
            return ReadFile(kNpuTopPath);
        }();
        if (text.empty())
            return std::nullopt;
        return ParseNpuTop(text);
    }

    // NPU 使用独立的 CMM 媒体内存分区而非共享系统内存；
    // /proc/ax_proc/mem_cmm_info 末尾的汇总行给出用量：
    //   ---CMM_USE_INFO:
    //    total size=4194304KB(4096MB),used=283076KB(...),remain=3911228KB(...),...
    std::optional<CmmSnapshot> ParseCmmInfo(const std::string& text) {
        static const std::regex info_pattern(R"(total size=([0-9]+)KB.*used=([0-9]+)KB.*remain=([0-9]+)KB)");
        std::smatch match;
        if (!std::regex_search(text, match, info_pattern))
            return std::nullopt;
        const auto total_kb  = ParseUnsigned(match[1]);
        const auto used_kb   = ParseUnsigned(match[2]);
        const auto remain_kb = ParseUnsigned(match[3]);
        if (!total_kb || !used_kb || !remain_kb || *used_kb > *total_kb || *remain_kb > *total_kb)
            return std::nullopt;
        CmmSnapshot result;
        result.total_mb     = static_cast<int64_t>(*total_kb / 1024);
        result.available_mb = static_cast<int64_t>(*remain_kb / 1024);
        return result;
    }

    CmmSnapshot ReadCmmInfo() {
        CmmSnapshot result;
        std::string text;
        if (const char* configured_path = std::getenv("COSMO_AXNPU_CMM_INFO_PATH");
            configured_path && *configured_path) {
            text = ReadFile(configured_path);
        } else {
            text = ReadFile(kCmmInfoPath);
        }
        if (const auto snapshot = ParseCmmInfo(text))
            return *snapshot;
        return result;
    }

    // clk 节点暴露当前 NPU 核心频率，实测形如 "800M"（也兼容 "1.6G"/裸数字）。
    std::string ReadNpuFrequency() {
        const auto text = ReadFile(kNpuClkPath);
        static const std::regex frequency_pattern(R"(([0-9]+)\s*([MG]?)Hz?)", std::regex::icase);
        std::smatch match;
        if (std::regex_search(text, match, frequency_pattern)) {
            if (const auto value = ParseUnsigned(match[1]); value && *value > 0) {
                const auto unit = match[2].str();
                const auto mhz  = !unit.empty() && std::toupper(static_cast<unsigned char>(unit[0])) == 'G'
                                      ? *value * 1000
                                      : *value;
                std::ostringstream frequency;
                frequency << mhz << " MHz";
                return frequency.str();
            }
        }
        std::ostringstream fallback;
        fallback << cosmo::util::kEngineType << " NPU";
        return fallback.str();
    }

    bool NpuDevicePresent(const CmmSnapshot& cmm) {
        // AX650 exposes a valid clock node even when npu/top accounting is
        // disabled. CMM is an additional presence signal for the AX NPU.
        return cmm.total_mb > 0 || ReadNpuFrequency() != std::string(cosmo::util::kEngineType) + " NPU";
    }

    // 尽力开启利用率统计：enable 节点不写 1 时 top 恒为
    // "nputop info is not enabled!"。失败（权限/节点缺失）时静默忽略，
    // provider 仍可用，只是利用率按不可用上报。
    void EnableNpuPerfCounters() {
        std::ofstream enable_stream(kNpuEnablePath, std::ios::trunc);
        if (!enable_stream)
            return;
        enable_stream << 1;
    }

    class AxeraAcceleratorMetricsProvider final : public AcceleratorMetricsProvider {
    public:
        AxeraAcceleratorMetricsProvider() {
            // 利用率统计默认关闭，构造时开启一次。
            EnableNpuPerfCounters();
        }

        cosmo::MsgGpuInfo QueryUtilization() override {
            const auto cmm = ReadCmmInfo();
            // The enable bit may be reset by the kernel after a service
            // restart. Retry on every poll rather than permanently caching an
            // unavailable utilization state.
            EnableNpuPerfCounters();
            cosmo::MsgGpuInfo result;
            result.utilizationMetric = "busy-time-load";
            result.memoryDomain      = "cmm";
            if (const auto load = ReadNpuTop()) {
                // 仪表盘以最忙的 vNPU core 作为设备健康信号，
                // 同时保留全部逐 core 数值。
                result.gpuusage          = load->aggregate;
                result.gpuusageAvailable = true;
                result.coreUtilizations  = load->cores;
            } else {
                result.gpuusage          = 0.0;
                // Distinguish "NPU present, utilization counter disabled"
                // from an absent accelerator. The Web UI should not report
                // the former as NPU unavailable.
                result.gpuusageAvailable = NpuDevicePresent(cmm);
            }
            result.gpumemtotal     = cmm.total_mb;
            result.gpumemavailable = cmm.available_mb;
            result.gpumemusage     = cmm.total_mb > 0 ? static_cast<double>(cmm.total_mb - cmm.available_mb) /
                                                        static_cast<double>(cmm.total_mb)
                                                      : 0.0;
            result.gpuCapacity     = ReadNpuFrequency();

            cosmo::MsgGpuDevUsage shared;
            shared.gpuusage        = result.gpuusage;
            shared.gpumemtotal     = result.gpumemtotal;
            shared.gpumemavailable = result.gpumemavailable;
            shared.gpumemusage     = result.gpumemusage;
            result.gpudevusage.push_back(shared);
            return result;
        }

        int64_t QueryAvailableMemoryMB() override {
            return ReadCmmInfo().available_mb;
        }
    };

}  // namespace

std::unique_ptr<AcceleratorMetricsProvider> CreateAcceleratorMetricsProvider() {
    return std::make_unique<AxeraAcceleratorMetricsProvider>();
}

}  // namespace cosmo::service::detail

#endif  // COSMO_NN_USE_AXERA_BACKEND
