# 一主五从基础候选：实施与离线验证记录

日期：2026-10-25。结论：三端代码已形成可构建的软件候选，本文列出的离线检查通过；**不能判定基本功能整体验收通过**。用户本轮明确暂无真实设备测试条件，未连接串口、未烧录、未安装 APK、未操作泵阀。B/C/D 全部未测。

## 1. 对应版本

| 项目 | 本次候选 |
|---|---|
| 主机 | ESP32-C3，`pump_master_five_candidate`，Build ID `182ab692`，并发 5 |
| 从机 | ESP32-C3，`valve_slave_c3`，Build ID `60d8ad44` |
| 产品容量 / 存储 ABI | 最多 5 台 / 保留 32 槽 |
| 正式默认并发 | 1；未以候选替换正式默认或历史 dist |
| Mesh 能力 | revision 6，RequiredFeatures `0x15FF` |
| SDK 补丁 | `pump-sar-4`，SHA256 `d11924460a21f0c47c8594f398c8effebba60ec11b517641d950479e6d0f40c8` |
| 主机静态 RAM / Flash | 55,048 / 1,137,724 字节 |
| 从机静态 RAM / Flash | 38,192 / 982,860 字节 |
| APP | debug APK；18 个测试类、137 项 JVM 测试，失败/错误/跳过均为 0 |

完整源文件哈希、有效 sdkconfig、编译命令、镜像哈希、分区及写入偏移由候选包 `candidate.json` 和 `SHA256SUMS` 记录。Build ID 仅索引源码与默认配置，不单独证明有效配置或实机运行身份。

原有三仓未提交改动保留；实施前快照为 `artifacts/basic-five-20261025/baseline.zip`，未覆盖。主从旧 SDK 缓存可逆保留为 `.mesh-sdk-before-basic-20261025`，不属于交付源码。

打包前发现主从 `patch.json` 的独立替换步骤顺序不同，完整生成组件文件哈希原本一致。已统一文件及补丁身份，从机缓存另保留为 `.mesh-sdk-before-identity-sync-20261025`，重建成功并重新导出编译参数；主从生成 SDK 全部文件及身份均一致。从机 Build ID 因输入文件统一由 `4fb2308f` 变为上表值，未改变 SDK 输出逻辑。

## 2. 已落实的行为

- 单个 JobKey、多位选择、统一每路目标；各路使用自己的 K 和脉冲，独立到量关闭，其他路继续。全员 START、有效租约与 300ms 提前量满足后才启共享泵，最后一路关闭证明触发停泵。停止/故障先停泵再关闭本任务各路。
- 所有登记入口限制产品五台，不改变已有 32 槽 ABI；超过候选并发的请求拒绝，不静默串行分批。
- 从机终态写成功才发送 RESULT；主机真实结果保存成功才发 FLOW/RESULT_ACK。保存失败锁定新任务，重复终态不重复写；未保存结果不能被新 CLOSE/PREPARE 覆盖。
- 相同 revision 的冲突结果拒绝；从机重启升 revision 只接受相同计量。合成 Reboot 未知可由真实终态对账补齐，但保留本轮故障。新增测试曾发现活动任务补回终态未清 unknown 的缺陷，现已修复并通过回归。
- 自动运行前向全部登记节点取得关闭、健康、静流、许可退役证明。默认禁止本地按钮开阀；现场 USB 维护许可绑定从机 boot、最长 60 秒、重复不续租、退役后不可复活。先持久化主机维护锁再授予许可；全员退出证明后才持久化解锁，失联/超时不解锁。
- 配置增加固定截止：回调 18 秒、步骤 75 秒、配置总 330 秒、配网总 420 秒；上下文不确定、丢事件、缺终止回调进入 RESTART_REQUIRED 隔离，迟到事件不能恢复。APP 显示最近预算和恢复动作，绑定本次 epoch/operation，不伪装本地实时倒计时。
- APP 使用当前启动的并发能力，维护状态未知禁启动；未获 JobKey 即点击停止时，先保存停止意图、查询原任务，获原 JobKey 后停止，进程重启仍保留，不自动重复启动。LIVE 使用冻结的任务地址映射。
- 基础版禁新启动逐路不同目标、交互式标定和独立试泵，保留旧会话恢复/查询/停止。文档同步 C3 GPIO4、65/80/100% PWM、12 秒从机租约及维护许可。

## 3. 本轮实际执行的离线检查

| 检查 | 结果与证据 |
|---|---|
| 主机 host 核心 | 33 个原测试组，加结果持久化/对账、维护 token 两组，全部通过；`.pio/host-basic-five.log` |
| 多节点模拟 | 全部 31 种非空 mask；各路不同脉冲速率、独立到量、未选输出保持关、重复包/丢结果 ACK、运行期整批停止均通过 |
| 既有压力模拟 | 1/2/5 节点各 1000 批、1000 个随机五节点批次、模拟 8 小时 3500 批通过；均为进程内模拟，无 RF/水路 |
| 从机 host | 未保存终态与新 CLOSE/PREPARE、维护过期/持续按住、按钮/计量/Prepared/传输故障等通过；从机 `.pio/host-basic-five.log` |
| 固定 SDK harness | 真实 SDK 提取路径的 accepted/end 顺序、分配/队列失败、无回调冻结、序号持久化及组件篡改拒绝通过；`.pio/sdk-basic-five.log` |
| 日志工具 | 10 项通过；`.pio/log-tools-basic-five.log` |
| Android JVM | 137 项通过；`basic-five-tests.log` 与 TEST XML；含停止意图重启恢复、旧账本、五路能力、维护门禁及预算跨操作隔离 |
| 角色/共享契约 | 主从角色分离、协议与维护/结果共享头一致；`.pio/architecture-basic-five.log` |
| 构建 | 主机五路候选、从机及 APK 成功；固件 envdump/compiledb 已导出，无上传操作 |
| 文本检查 | 三仓 `git diff --check` 无空白错误；换行转换提示不属于测试失败 |

测试调用了共享结果提交门禁及真实 Valve/调度/Mesh 会话逻辑；存储失败通过注入返回值覆盖。**未在 host 环境完整运行 ESP-IDF Application、RTOS、NVS 与 BLE 回调组合**，不能把 helper/controller 测试扩大为板上应用级故障注入已经通过。NVS 读回失败、Flash 阻塞、重启临界点和真实关闭时序仍须补证。因此 A 的已列离线检查通过，但完整验收矩阵仍有上述应用层覆盖缺口，不能写成 A/B/C/D 全部通过。

## 4. 当前参数与使用边界

主机 GPIO4 PWM=20kHz、65/80/100%；从机阀 GPIO4、霍尔 GPIO5、按钮 GPIO3-GND，GPIO6 联锁禁用。GPIO18/19 留原生 USB。GND 可分线共用；GPIO 不能直接驱动线圈。

主机运行租约证明 8 秒，从机租约 12 秒；首次无流 5 秒、断流 1.5 秒、尾流静默 500ms、结算上限 3 秒、运行最长 240 秒。本轮未修改保护以掩盖通信故障。

维护：主机 USB `maintenance-enter ADDRhex CONFIRM` → 核实 `MAINT locked=1 pending=0` → 目标从机先释放再按；仅本路开阀，主机泵不运行。`maintenance-exit CONFIRM` 需全员证明后才解锁。许可到期不等于维护锁解除。旧结果不可恢复时保持未知/冻结，首版无人工结案解锁入口。

## 5. 待验收与不可宣称的结论

| 门槛 | 状态 | 后续证据 |
|---|---|---|
| A 应用层覆盖补充 | 部分未覆盖 | 实际 Application + NVS/RTOS 的故障注入与关键顺序证据 |
| B 五台只读通信 | 未测 | 五台同时在线，20 系统轮/100 节点轮；手机连/断各至少 10 轮、至少两次整组冷启动，全程输出 0 |
| C 同时定量 | 未测 | 全 31 组合、85 个整任务、170 个支路量测；每路误差 ≤max(10mL,5%)；不分批 |
| C 输出时序 | 未测 | 主机收到有效停止至 PWM 关、按钮释放至阀 GPIO 关目标 ≤50ms；最后一路关阀至停泵延迟 |
| D 保护与重启 | 未测 | 手机/链路断连、主从重启、无流/断流/运行时限、维护许可乱序/退役丢包、存储临界点 |
| SDK/RF 根因 | 未闭环 | 配置幂等回读、真实旧回调归属、SeqAuth/SAR 双端日志、外设/供电干扰排查 |

固定 SDK 缺请求 token 的旧回调归属问题不能仅凭隔离逻辑宣称彻底解决；本候选提供有界退出、禁止不可信恢复与可追踪状态。真实搜索/配对成功率、五台无线并发、硬件输出、机械阀位、精度及 50ms 都未验收。

现场记录模板位于 `docs/basic-five-records/`，所有初始结果为 NOT_TESTED。详细步骤和判定沿用 [整体优化方案](BASIC_FUNCTION_OPTIMIZATION_PLAN_20261025.md) 第 7 节。只有完成全部缺口并通过 B/C/D 才能作为一主五从基本功能正式交付。

## 6. 复现入口

```powershell
python tools/run_host_tests.py --compiler .pio/host-tools/ziglang/zig.exe
python tools/verify_architecture.py
python tools/run_sdk_tests.py --framework C:/Users/79622/.platformio/packages/framework-espidf --compiler .pio/host-tools/ziglang/zig.exe
python test/log_tools_tests.py
pio run -e pump_master_five_candidate
# 相邻 pump-muti-slave：host tests + pio run -e valve_slave_c3
# 相邻 pumpmutiapp：gradlew.bat :app:testDebugUnitTest :app:assembleDebug
```

候选包包含匹配主从镜像/ELF、APK、测试日志、SDK身份、源文件快照和 SHA256；包标记 `software_candidate_not_hardware_accepted`。不覆盖历史 dist，不自动擦除 NVS，不执行上传。
