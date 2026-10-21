# 执行与记录

## 可迁移资源包优先

先检查环境变量 `COSMO_JOURNEY_RESOURCE_ROOT`，其次读取 ignored 的 `output/agent-runs/cosmoedge-test-kit/resource-root.local.json`。存在根目录配置时，使用该目录下的 `resources.private.json`，不要退回旧的绝对路径配置。新机器只需在仓库根执行 `python <资源包根目录>/kit.py configure`；包内工具可兼容尚未包含资源根目录功能的仓库版本。

开始或换机续测前执行 `python <资源包根目录>/kit.py check --network`；首次迁移加 `--full` 做 SHA-256 校验。读取 `checks/latest.json`，按资源编号把缺口绑定到依赖用例，端口可达不代表登录或业务验收。`kit.py` 的 `init/preflight/next/record/report` 参数与下列工具一致。包内 `runtime/` 是工具快照，不是另一份用例库；正式用例仍读取当前仓库 XLSX。

新轮次复制私有配置后仅覆盖 DUT；保持 `portableVersion`、`kitId` 和素材相对路径，先配置资源根目录再初始化。不要把远端 `workspace/script/sampleRoot` 改写为本地素材路径。新格式运行已保存本轮私有配置快照，迁移完整 run 后重新配置相同 kitId 的根目录即可读取；旧格式运行要单独核对绝对配置/证据引用，不能声称自动迁移完成。

## 常规命令

以下命令在仓库根执行。`<run>` 必须在 `output/agent-runs/`，例如按时间创建的新目录；不要复用历史run执行另一台设备。私有配置采用 `test/customer-journey/resources.example.json` 的结构，一次配置以后由技能复用。当前用户的既有设备、账号、国标原始字段和素材路径已填入默认私有配置。

```text
python tools/customer_journey.py validate
python tools/customer_journey.py init --run <run> --profile <私有配置>
python tools/customer_journey.py preflight --run <run>
python tools/customer_journey.py next --run <run> --limit 1
python tools/customer_journey.py template --run <run> --case CAM-003 --output <run>/case-input.json
python tools/customer_journey.py record --run <run> --case CAM-003 --result <run>/case-input.json
python tools/customer_journey.py report --run <run>
```

主要功能快测沿用 `next/template/record/report`，初始化时选另一份冻结用例，报告文件名为 `CosmoEdge-主要功能逐用例测试结果.xlsx`：

```text
python tools/customer_journey.py validate --suite test/customer-journey/CosmoEdge-主要功能客户旅程测试用例.xlsx
python tools/customer_journey.py check-resources --suite test/customer-journey/CosmoEdge-主要功能客户旅程测试用例.xlsx
python tools/customer_journey.py init --run <新的快测run> --suite test/customer-journey/CosmoEdge-主要功能客户旅程测试用例.xlsx --profile <本轮私有配置>
python tools/customer_journey.py preflight --run <新的快测run>
python tools/customer_journey.py next --run <新的快测run> --limit 1
python tools/customer_journey.py report --run <新的快测run>
```

快测逐条执行 Q 编号和步骤验收，共 28 条、95 步。`VLM-MODEL` 与 `DINO-MODEL` 独立检查；原始模型文件或模板存在不代表设备可运行。未具备兼容模型时写明 Q26/Q27 的资源阻塞，继续其他用例。快测结果不写回全量用例，也不声称覆盖其设备、参数和接口矩阵。旧资源包自带 `runtime/` 是先前工具快照；快测应使用拉取最新代码后的仓库 `tools/customer_journey.py`，资源包只提供私有配置和素材。

用户补充资源后修改私有配置，运行 `refresh-profile --run <run>` 保存配置哈希变更记录，再 `preflight`。既有失败/阻塞项不会自动变绿；直接按其ID生成模板并重新执行。每次 `record` 保留上一轮尝试，报告展示历史失败。

`init` 默认直接读取正式 XLSX，校验列名、编号、资源和矩阵引用，并保存 `casebook.snapshot.xlsx`、`suite.snapshot.json` 及哈希。继续测试时不重新导入当前主库；主库更新必须新建 run。复制结果工作簿作为主库会被拒绝。

ENV-001 查明版本并保存证据后，执行 `set-version --run <run> --version <实测版本> --evidence evidence/<版本证据文件>`。升级后再次登记；每条新记录保存当时版本，既有记录的版本不会被后续升级覆盖。无法确认则保持 UNVERIFIED。

每次 `record` 更新 HTML/JSON。最终或需要阶段性 XLSX 时执行 `report`，输出 `CosmoEdge-逐用例测试结果.xlsx`；它按编号回填，因此排序不改变结果归属。导出使用 Codex 随附的 `@oai/artifact-tool`；缺少运行时需定位已有依赖，不能把只有 HTML 说成已生成 XLSX。

`init` / `next` / `preflight` 都不是执行完成证明。preflight只检查配置与本地文件，不能证明远端协议、声音、视频真值或连接可用。

## 证据记录

在 `<run>/evidence/` 保存脱敏DOM摘录、截图（工具支持时）、API结果、视频统计、脚本结果、创建对象及清理核对。已有证据不等于已验证全部步骤，逐步注明实际观察。

结果输入字段：

- `status`：PASS / FAIL / BLOCKED / PARTIAL / NOT_EXECUTED / NOT_APPLICABLE。
- `reasonCode`：PRODUCT_DEFECT / RESOURCE_MISSING / RESOURCE_INVALID / CONFIGURATION / AUTHORIZATION / HARDWARE_REQUIRED / GROUND_TRUTH / ACCEPTANCE_UNDEFINED / TOOL_FAILURE / INCOMPLETE / NOT_APPLICABLE / NONE。
- `reason`：具体结论理由；失败写预期与实际差异，阻塞写缺少哪项和谁来补充。
- `actual`：本条实际操作及结果，不写推测。
- `evidence`：run内已存在的相对文件路径。文件不能引用外部或尚未生成的证据。
- `steps` / `assertions`：每项的 `id,status,actual,evidence`。ID以冻结用例为准。
- `cleanup`：`status: VERIFIED | NOT_REQUIRED | PENDING`，以及 `note,evidence`。无写入才可用NOT_REQUIRED；有写入需核对恢复。

通过必须完成所有步骤、所有适用断言且有证据和清理。`NOT_APPLICABLE` 子断言必须说明实际能力依据并留证。批量API矩阵中的写操作必须按接口契约准备隔离对象，不能用同一空请求“扫描通过”。

## 未通过原因与补测

不能只写“未执行”“部分完成”或重复状态。新结果可带 `analysis`；已有记录用 `explain --run <run> --case <ID> --input <分析JSON>` 补充，保留原实测记录和上次分析。字段为 `category`（原因类别）、`cause`（具体原因与已知事实）、`certainty`（CONFIRMED / PARTLY_CONFIRMED / UNKNOWN）、`remaining`（尚缺哪些步骤/参数/样本）、`nextAction`（具体动作）、`owner`（由谁处理）、`evidence`（run 内证据路径）；可另加 `conditions` 和 `caseDesign`。

无历史中止记录时写“未完成执行，停止原因未记录”，责任方是 Codex 补测，不得推断用户缺资源。配置缺项是待核对线索，不能自动作为历史阻塞原因。已出现失败现象但尚未定位代码/模型根因，使用 PARTLY_CONFIRMED 并说明复现动作。

报告的“原因与补测”表覆盖全部未通过项；HTML 同时显示动作与处理方。最终核对 `unexplainedCases`；仍有 UNKNOWN 时明确列出，不宣称原因全部查明。补充原因分析不改变执行状态，也不能替代继续执行可测用例。

## 精度资源

读取私有配置 `PRECISION-SERVER`。先读脚本、核对默认目标、清理行为和报告输出路径；显式指向本轮DUT。用户给出的样本数、采集时长、并发数等作为初始配置，不视为容量或质量验收门槛。选择与设备实际算法语义相符的场景，不能按历史ID猜测。

精度结果绑定 `ACC-*` 和对应 `ALG-*`；首轮与重测分别保留，损坏输入列为资源异常，不计作算法漏检。身份识别、区域、人数、工服、大模型等缺真值时只可验证流程，不能给准确率通过。

## 历史迁移

`import-legacy --run <run> --source <旧执行目录>` 只可用于全新空 run 的历史报告修正。随后运行 `report` 回填历史结果 XLSX。保留旧状态，记录来源标明历史迁移，原记录缺失的逐步骤结果和执行时间留空或标为未记录；新增用例保持未执行。不得用它录入本轮新测试，也不生成任何新的产品通过结论。
