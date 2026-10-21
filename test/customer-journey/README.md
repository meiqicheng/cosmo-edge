# CosmoEdge 逐用例测试套件

在 Codex 中可直接选择两种测试：

> 对 CosmoEdge `<设备IP>`，平台用户名 `<用户名>`、密码 `<密码>`，按仓库 `test/customer-journey/CosmoEdge-客户旅程测试用例.xlsx` 逐条执行全量测试，输出通过、失败、阻塞及原因和证据的 XLSX/HTML 报告。

> 对 CosmoEdge `<设备IP>`，平台用户名 `<用户名>`、密码 `<密码>`，按仓库 `test/customer-journey/CosmoEdge-主要功能客户旅程测试用例.xlsx` 逐条执行主要功能快测，输出通过、失败、阻塞及原因和证据的 XLSX/HTML 报告。

快测使用 [`CosmoEdge-主要功能客户旅程测试用例.xlsx`](CosmoEdge-主要功能客户旅程测试用例.xlsx)，当前 v0.8、29 条核心用例、102 步，设计预算 159–263 分钟（资源已就绪时）。覆盖 RTSP、ONVIF、国标、本地视频、USB、模型和场景任务、底库识别、图片分析、VLM/DINO、大屏预览叠加、事件、事件上报大模型审核、HTTP/MQTT、系统设置及清理。`quick-suite.json` 是由快测 XLSX 生成的 Git 审阅副本，不手工修改。快测的抽样结论不代表全量 332 条及其子矩阵全部通过。

两种模式都由 Codex 实际操作界面、保存逐步证据；工具只管理队列、状态和报告。输入设备 IP/账号/密码后，应先用私有资源包做预检，缺资源只阻塞依赖用例。快测的 VLM 与 DINO 模型分别检查；缺可运行模型时保留明确阻塞原因。资源包仍放在 ignored 私有目录或配置的统一根目录，不提交设备账号、照片、模型或测试报告。

USB 接入为必测（Q25 / CAM-010），每轮核对枚举、预览、算法输出与启停恢复，不沿用历史资源不足结论。下载设备日志已由 Q22、SYS-006 和 REG-008 覆盖，包括真实下载、解包、内容与运行中任务核对。事件上报组件审核由 Q29 和 EVENT-012～014 覆盖：本地与 OpenAI 接口分别验证开关、人物特征正反例、目标 ROI、提示词生效及故障恢复。使用 FACE-VIDEO 的同一视频与人工可见事实，不把接口错误当作模型否定，不用独立 VLM 节点代替上报组件审核。接口基地址、实际 served-model-name、API Key 和并发限制只维护在私有 OPENAI-VLM 资源配置中。

技能负责实际调用浏览器/API/精度工具；`tools/customer_journey.py` 是持久化队列和报告工具，不是无人值守浏览器执行器。修改凭据的用户接管、物理外设确认及尚未授权的高影响操作仍需按实际条件处理，不会因一句全量测试而绕过审批。

[`CosmoEdge-客户旅程测试用例.xlsx`](CosmoEdge-客户旅程测试用例.xlsx) 是全量用例的维护入口，当前版本为 1.7.0。从原 v0.1 工作簿持续保留客户旅程及覆盖表，现有 332 条父用例、1477 条验收检查。修改“详细用例”及设备/参数/组件/接口矩阵，下一轮直接读取 XLSX。额外验收点写在“补充检查”。历史测试报告仍对应各轮冻结的用例库版本，不自动覆盖新增用例。

OBB 旋转框图片检测由 ALG-38874 专项覆盖，检查模板及配置版本的旧字段清理、旋转框输出、直接置信度阈值两侧的保留/过滤、临时参数与持久默认隔离。IMAGE-007 检查图片场景无视频区域/布防及无效置信度偏移，Q14 做相应快测抽查。OBB 样本必须匹配当前模型类别，缺少模型或阈值边界样本时具体记录缺口；这些用例的定义和本地校验不代表设备实测通过。

`suite.json` 由 XLSX 编译生成，提交它是为了在 GitHub 查看文字差异，不手工维护。`coverage.json` 仅为早期源码盘点参考，不参与执行；新增功能需同步工作簿内用例及覆盖表。每轮冻结 `casebook.snapshot.xlsx` 和 `suite.snapshot.json`，并记录 SHA-256。历史结果始终对应当时版本。

`resources.example.json` 是不带凭据的通用资源格式。实际账号、密码和路径只放在 ignored 的 `output/agent-runs/` 私有配置。用户每轮只需给目标设备登录信息；固定摄像机、NVR、服务器和素材配置可复用。默认配置路径见技能。允许明文不代表允许将配置提交到开源仓库。

支持可迁移私有资源包：包内 `resources.private.json` 使用相对路径，`manifest.json` 保存素材大小和 SHA-256。新电脑复制整个包，在仓库根运行一次 `python <资源包根目录>/kit.py configure`，即可保存本机根目录；后续不必逐项修改素材路径。也可用 `COSMO_JOURNEY_RESOURCE_ROOT` 临时覆盖根目录。新电脑需要 Python 3 和本仓库；包内携带配套工具，仍读取仓库内的正式 XLSX，不另建用例主库。

启动前运行 `python <资源包根目录>/kit.py check --full --network`；首次迁移使用完整哈希检查，日常可省略 `--full`，只检查文件存在性和大小。`--network` 仅探测配置中的 TCP 端口，不登录、不修改设备，也不证明协议认证或算法通过。检查列出缺口及受影响用例，保留用户已确认的不适用项；缺口不会把整轮用例统一阻塞。`init` 会自动执行本地资源预检。资源检查报告默认写入包内 `checks/latest.json`；配置损坏返回错误，资源缺口作为报告数据交由 Codex 按用例处理。

新建的可迁移运行保存资源配置快照和相对配置引用。复制该轮完整目录并重新配置同一资源包根目录后可续测；共享配置的后续修改不覆盖历史快照。旧版运行仍兼容，但旧绝对路径不会被自动改写。照片、模型、升级包、账号、根目录配置和实测记录均不提交 GitHub。精度服务器的脚本/样本及外部接收服务仍是远端依赖，资源包不等于离线测试环境。

最终执行 `report --run <run>`，同时生成以下报告：

- `CosmoEdge-逐用例测试结果.xlsx`：在冻结工作簿中按用例 ID 回填状态、实际结果、未通过原因、失败步骤、证据、版本、时间和清理情况；总览自动计数，“步骤与检查结果”逐项列明结果，有重测时附“重测历史”。
- `report.html`：支持编号/模块/状态筛选及“仅看未通过”，并链接结果 XLSX 和证据。
- 未通过原因：XLSX 的“原因与补测”和 HTML 均列明原因、剩余检查、下一步、处理方及证实程度；报告单列未分析项和原因未知项。`explain` 可补充说明，保留原实测状态；不得把潜在资源缺口推断为历史中止原因。
- `results.json` / `summary.json`：持久化结果、重试历史及汇总。每次 `record` 即时更新 HTML/JSON；XLSX 由 `report` 刷新，避免每步重写整个文件。

结果不覆盖主库。保留整个 run 目录才能打开相对路径证据；单独发送 XLSX 仍可看结论，但不附带证据文件。精度样本作为用例证据，不替代功能用例数量。历史导入明确标注“未按此版 XLSX 重测”，不能据此通过新版本验收。

读取/校验/记录使用 Python 3 标准库。XLSX 导出使用 Codex 随附的 Node 和 `@oai/artifact-tool`，不安装仓库依赖。若运行时位于其他路径，先用 `load_workspace_dependencies` 获取路径，再设置 `COSMO_JOURNEY_NODE` 和 `COSMO_JOURNEY_NODE_MODULES`。缺少导出运行时会明确失败，已保存的 HTML/JSON 不丢失。

快测使用仓库最新的 `tools/customer_journey.py`，初始化时指定 `--suite test/customer-journey/CosmoEdge-主要功能客户旅程测试用例.xlsx`。全量测试不指定 `--suite`。运行入口及断点续测见 [执行说明](skill/references/execution.md)。旧资源包内的 `runtime/` 是历史快照，不能替代快测所需的最新仓库工具。

运行和维护方式见 [技能入口](skill/SKILL.md)、[执行说明](skill/references/execution.md) 和 [维护说明](skill/references/maintenance.md)。

修改主库后的检查：

```text
python tools/customer_journey.py compile
python tools/customer_journey.py compile --check
python tools/customer_journey.py validate
python tools/customer_journey.py compile --workbook test/customer-journey/CosmoEdge-主要功能客户旅程测试用例.xlsx --output test/customer-journey/quick-suite.json --check
python tools/customer_journey.py validate --suite test/customer-journey/CosmoEdge-主要功能客户旅程测试用例.xlsx
python -m unittest discover -s test/customer-journey -p "test_*.py"
npm run docs:verify
```

Windows 终端使用 UTF-8（例如 PowerShell 设置 `$env:PYTHONUTF8='1'`）。提交范围为本目录、`tools/customer_journey*.py`、`tools/customer_journey_xlsx.mjs` 及入口说明；不要提交 `output/agent-runs/` 中的账号、个人素材和实测报告。
