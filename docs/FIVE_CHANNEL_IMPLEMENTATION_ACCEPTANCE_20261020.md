# 五路 V4 实施与验收台账

日期：2026-10-20。对应方案：[FIVE_CHANNEL_REPAIR_PLAN_20261020.md](FIVE_CHANNEL_REPAIR_PLAN_20261020.md)。

**状态：V4 实施与软件回归完成；实机验收未通过，不能发布五路。** 现场只有 1 台从机，用户已确认水路、水源和计量工具具备连续测试条件。首批 100 mL 实水完成正常启停和结算，但主机控制 tick 达到 66 ms，超过 50 ms 门槛；量杯实测值和 APP 核对仍缺失。随后串口优化版出现 BTC_TASK 栈溢出重启，已增栈修订并重新刷入，尚需新版出水复测。单台测试不能代替五台 RF、真实并发、轮换节点及 8 小时实水验收。详见第 6 节和同名 JSON 台账。

## 1. 已落实的行为

- V4 revision=4、features=0x01FF。Prepare 39 字节携带 leaseFloor；Start、Lease、成功 LeaseAck、PrepareOK、StartOK、TargetClosed、ClosedAck 正常业务载荷均为 8 字节。异常拒绝保留专门格式，旧长度和 CF=OK 不被接受。
- 完整 JobKey 通过 Prepare 与本地 Packet.context 关联。LeaseScope 建立和全局序号分配共用传输锁；旧 pending 授权被撤销，真实 SDK ticket 保留。epoch/floor/sequence 确定性隔离任务，序号耗尽拒绝且不回绕。
- Prepare 阶段 15 秒，从机 Prepared 固定 20 秒。首 Start 未提交保护 1 秒，首次实际提交起固定 4 秒启动窗口；启泵要求全部必要成功确认及年龄严格小于 2 秒的有效租约证明。运行证明保护 8 秒，从机租约 12 秒；无流和尾流保护未放宽。
- Prepare 采用滚动准入；同 peer 普通 SAR 不能同时堆积。每次实际提交至少间隔 1500 ms、最多 3 次，暂忙不计次数；首发优先、公平轮询。命令和成功 ACK 的旧阶段副本被清理，在途重复成功 ACK 被合并，真实丢失后允许重发。
- 每节点 10 条实际 Lease 提交记录，cursor 模 10；五路相位按本批选中节点的序数分配，覆盖不连续注册槽位。每路实际提交间隔至少 1000 ms，错过周期不突发补发。
- ACK 早于 SDK accepted 时只缓存；accepted 成功后才应用。SDK 拒绝、取消和旧任务迟到 ACK 均不能启泵。SAR end 与业务确认独立。
- TargetClosed 只锁存阀输出已关闭事实，不能冒充最终量和成功。一路关闭不停止其他仍需要供水的节点；最后关闭事实到达同次处理停泵。收尾先等待 3500 ms，再全局每 1000 ms 查询一个缺失节点，总期限 10 秒。
- 两端每 10 秒记录空闲堆、历史最小堆、最大空闲块、控制及发送任务栈低水位。增加 start_guard/start_proof、命令 reply/applied、租约和关闭诊断。
- 主机维护回复与 ESP_LOG 共用中断驱动 UART 和 stdout 锁，避免直接写 FIFO 与驱动写入混用；BTC_TASK 从 3072 增至 6144 字节，并记录真实任务栈低水位。此修订的实水控制时序仍待复测。
- 保留 COM5/COM6、既有配网/标定/NVS，以及用户要求移除 build hash 白名单的行为。能力、来源、任务隔离校验继续生效。APP 手机协议和业务代码未改。

## 2. 构建及可复现入口

| 项目 | 环境 | 并发 | build_id | 静态 RAM / Flash（字节） | 状态 |
| --- | --- | --- | --- | --- | --- |
| 主机生产默认 | pump_master | 1 | 5939b648 | 62688 / 1009628 | 构建成功，未刷入本轮台架 |
| 主机台架候选 | pump_master_five_candidate | 5 | 5939b648 | 62136 / 1009620 | 构建成功，已刷主机，仅空闲/启动验证 |
| 从机 | valve_slave_c3 | 单阀 | 144be637 | 37960 / 977268 | 构建成功 |
| APP | debug | 随主机计划 | Git 24ddb0b 的业务源码 | 79 项单元测试 | 测试、APK 构建成功 |

build_id 目前表示源码及默认配置输入，未覆盖环境的有效宏，故两个主机环境可以同 ID。必须同时核对 ELF/bin SHA256、compiledb 中的 `PUMP_MAX_CONCURRENT`、有效 sdkconfig 和包清单，不可仅凭 build_id 区分并发。真实构建采用固定版本平台 espressif32@6.10.0 和项目 SDK 补丁；不修改全局 SDK，不扩大 SAR 上限。

最新主机源码摘要为 `5939b6482e16873dfd805f5aa29e652c4192ffa59484d6ba245587a1ac70b901`。固件 SHA256：

- 生产单路：`a214d90ccfe8a5213f449f3a9aafe196a44c876839a101ec0b9635f0d1d9d2da`。
- 台架五路：`d0981e6c7284f6dd0a98f857e14f5749f30dbacfdf4807f8c568153f5c0481d5`。
- 从机：`0a712a9da00dcf6951540f5b5c0663dcedd5e91757d3b9d2552f21529f1ed5cb`。

主机两环境的本轮构建/compiledb/envdump 证据分别使用 `.pio/*-v4-btc-production.*` 和 `.pio/*-v4-btc-five.*`。从机没有因主机 BTC 修订重刷。上述 Flash 数值是构建工具统计，不是 bin 文件长度。

在对应工程执行：

```powershell
# 主机；生产默认与台架五路分别构建
platformio run -e pump_master
platformio run -e pump_master_five_candidate
python tools/run_host_tests.py --compiler .pio/host-tools/ziglang/zig.exe
python tools/run_sdk_tests.py --framework C:/Users/79622/.platformio/packages/framework-espidf --compiler .pio/host-tools/ziglang/zig.exe
# 从机工程
platformio run -e valve_slave_c3
python tools/run_host_tests.py --compiler ../pump-muti/.pio/host-tools/ziglang/zig.exe
# APP 工程；JAVA_HOME 使用 Android Studio jbr
.\gradlew.bat testDebugUnitTest assembleDebug
```

实际测试日志和输入摘要另存于软件证据目录及候选包。SDK 回归执行结果以 `.pio/sdk-v4.log` 为准。

## 3. 第 9.1 节逐项软件验收

以下“通过”仅指 host/SDK harness 指定范围。没有将 host stub、模拟时钟或人工脉冲当作真实无线或实水证据。

| 序号 | 方案场景 | 结果及可定位证据 |
| --- | --- | --- |
| S01 | 旧 V3：2680 ms 启动、1283 ms 证明年龄、重复确认 | 通过。用实施前保存的原始 scheduler/headers 独立编译 `.pio/diagnostics/v3-field-replay.cpp`，2400 ms 后旧实现拒绝迟 ACK、未启泵；`.pio/v3-field-replay.log`。新实现 `v4_exact_boundaries_and_ledger` 同时序启泵成功。治理器矩阵覆盖在途重复 OK 合并和旧阶段清理。原始现场时间见近期故障诊断，未声称重新现场运行旧版。 |
| S02 | 五节点、646/1116/1514 ms 等 SAR 占用 | 通过。`v4_timing_link` 覆盖这三个值及 3200 ms；五个 Prepare 只实际发五次、SAR 峰值不超上限、所有有效证明后一次启泵。`governor_matrix` 检查 compact SAR bypass；实机 seg=0 另验。 |
| S03 | 1999/2000、3999/4000 ms 边界 | 通过。core `lease_proof_age_and_boundary_matrix`、`v4_exact_boundaries_and_ledger`：等于期限不能启泵，重试不滑动期限。 |
| S04 | ACK/accepted/拒绝/SAR end 独立顺序 | 通过。`early_ack_commit_and_rejection_matrix`、V4 early StartOK/rejection 与 acceptedDelay=300；拒绝 Start/Lease 不启泵。SDK harness 覆盖真实提取函数的回调先后、late/no-end，不伪造完成。 |
| S05 | 关闭、取消后迟 Start/Lease ACK、关闭事件 | 通过。core V4 cancel late proofs、`rejected_commands_and_late_terminal_reconciliation`；关闭可补事实但不恢复授权、不制造 final。 |
| S06 | 同 epoch 上任务 Lease、跨 epoch | 通过。`v4_wire_and_session`、从机 `prepared_deadline_and_floor`；低于 floor、epoch 不符、改 floor 重复 Prepare 拒绝。 |
| S07 | scope/worker 竞争、全局 sequence | 通过。`v4_sequence_scope` 穷举建立 scope 前后提交两种锁内顺序；五节点联合仿真使用实际分配器。非线程竞争压力测试，依赖生产同一传输锁的实现。 |
| S08 | 五个长 Prepare、首发/重试竞争 | 通过。V4 联合仿真及 governor 准入测试；rolling admission、防饿死、暂忙不计次数和阶段失败保护均覆盖。 |
| S09 | 大于 2³¹、UINT32_MAX、重启 epoch | 通过（软件范围）。V4 scope/session 大序号及耗尽不回绕；既有 capability/master restart、持久化迁移及重放测试。实际设备升级重启 epoch 记录另见第 6 节。 |
| S10 | 同序号、旧序号、过期、新包重复 | 通过。session 和从控制器测试：重复不续期，过期不能复活；Prepared 19999/20000 ms、重复 Prepare 不延时。 |
| S11 | 4～8 秒有效记录、cursor 跨 255 | 通过。ACK 延迟 4500 ms 仍匹配，300 次提交跨旧 8 位边界，最后 10 条均在。8 秒保护边界由 core 测试独立检查。 |
| S12 | 先关一路、全关、D2/D3/Result/ResultAck 丢失 | 通过。`v4_timing_link` 四类独立丢失、一路先关其余继续、全闭停泵；closed 不替代 final/量。 |
| S13 | 拒绝、掉电/失联、SAR 无 end、队列满 | 通过（软件注入）。governor/recovery/integrated fault、五路 lease ACK loss/missing SAR end/manual stop：本地先停、远端关闭、真实 ticket 保留、缺结果 UNKNOWN。物理断电仍待验收。 |
| S14 | V3/V4、旧长度、未知能力、来源及 build 改变 | 通过。capability nonce/source/revision/features/cache/restart、wire strict length；build 改变触发重新能力核验，本身不白名单拒绝。 |
| S15 | 随机五路至少 1000 批 | 通过。1000 批：交付 jitter 0～150 ms、SAR 646/1081/1516 ms、重复 Start、每批至多丢一个包；全部完成且一次启泵、队列/ticket 清理。任意多次连续丢包不在成功包络内，应允许有界保护失败。 |

共同证据：主机 `.pio/host-v4-btc.log`（打包时同步为 `.pio/host-v4.log`），从机 `.pio/host-v4.log`，主机 `.pio/sdk-v4.log`、`.pio/architecture-v4-btc.log`、`.pio/log-tools-v4-btc.log`，APP `v4-tests.log` 和 XML。主机 30 组既有测试及 V4 扩展、从机控制器、SDK harness、角色边界、10 项日志工具测试均通过。APP 79 项，失败/错误/跳过均为 0。BTC 修订后重跑主机 host、架构检查和日志工具；从机、SDK 补丁和 APP 本轮未改，复用既有相同输入的证据。

日志解析器新增 V4 epoch 字段识别、设备栈溢出/崩溃/看门狗及 reset banner 保留。设备故障与采集错误分栏；不能用 `capture_errors=[]` 表示设备从未崩溃，也不能仅看最后一次稳定启动而忽略之前崩溃。

额外：1/2/5 节点各 1000 批既有联合回归；模拟 8 小时/五节点 3601 批，无累计 ticket/queue。**模拟 8 小时不满足真实连续 8 小时验收。**

## 4. 第 9.2 节实机批组台账

| 批组 | 最低门槛 | 当前结论 | 剩余条件 |
| --- | --- | --- | --- |
| 单路基线 | ≥50 批，轮换 5 节点 | B001 已执行但未通过整批验收，暂停累计 | 66 ms tick 需新版复测；量杯/APP 待核对；还缺 4 台轮换 |
| 两路并发 | ≥50 批，轮换组合，真实重叠 | 待验收 | ≥2 台、水路与计量 |
| 五路并发 | ≥200 批，同量/不等量、100/200/500 mL、冷/热缓存和泵速 | 待验收 | 5 台完整台架 |
| 五路持续运行 | ≥8 小时真实混合任务、遵守泵占空比 | 待验收 | 五台、足够水源及回收、时钟与内存趋势 |
| 故障注入 | 每类 ≥10 次、轮换节点：断电/丢 ACK/人工停止 | 待验收 | 有监督的完整台架；host 注入不抵扣物理次数 |

正常工况任何启动失败、零脉冲、尾流异常或超差均暂停该组并保存证据，不能继续累计为正常通过。

## 5. 六项定量门槛与测量口径

| 门槛 | 当前状态 | 证据/测量要求 |
| --- | --- | --- |
| 首 Start→启泵最大 <4000 ms、P99 目标 ≤3000 ms、证明年龄 <2000 ms | B001 为 197 ms / 46 ms；新版及分布待验 | 单个样本不作 P99 验收；见 6.3 |
| 五路 Start 最大偏差 ≤500 ms、泵至首脉冲 ≤600 ms、控制 tick ≤50 ms | 未通过：B001 主机 tick_max=66 ms | 修订版空闲主机 28 ms 不能抵扣实水；从机后续历史 tick_max=55 ms。首脉冲和五路偏差仍缺统一测量 |
| 最后输出关阀→停泵 P99 ≤300 ms/最大 ≤500 ms；收到最后关闭证明→停泵 ≤50 ms | B001 主机收到证明→停泵同钟 0 ms；物理分布待验 | 串口观测约 71 ms 仅供参考，不能当物理延迟或直接相减两端 uptime |
| 正常 C1/C2/CE/D2/D3/D4/D5 trace seg=0，无新增业务/SDK/冻结错误 | B001 所列正常包均 seg=0；完整矩阵待验 | 随后的 3507fa72 出现启动崩溃，已单独记为失败；最新修订版只有空闲/启动观察 |
| 全部 selected 最终可信、failed/unknown=0、Target/quiet=1/truncated=0，三端一致 | B001 主从一致；APP 未核查 | 主从均为 600 脉冲、折算 102 mL，DONE/TARGET、quiet=1、truncated=0；不能代替 APP 或量杯 |
| 每种水量/泵速/并发实际计量 | 等待 B001 量杯值；其余待验 | 建议 ±max(5 mL,目标×5%)，仍需产品确认；量杯或净水重及量具精度由现场人员提供，脉冲换算不能冒充实测 |

## 6. 本轮设备与实水记录

### 6.1 刷机前

原始日志 `.pio/diagnostics/v4-before-device-check.jsonl`（被动 20 秒）及 `v4-preflash-query.jsonl`（只发 hello/local/nodes 等查询）。COM5 主机 0001、build=18ed7b18、epoch=142；COM6 从机 0002、build=c0f6b78c。查询时泵输出=0、阀输出=0；上一任务 142:74 的从机结果为 TRANSPORT/0 脉冲。主机错误累计 0，从机 expired=3 属历史累计。上述不是新 V4 部署或新任务通过证据。

### 6.2 备份和首次 V4 部署

备份在 `.pio/diagnostics/v4-device-backup/`，两端各读取 `0x120000` 字节，**不是完整 4 MB flash**。`backup.json` 保存 SHA256 和提取程序摘要；含配网/标定信息的原始数据仅作本地取证。`nvs_restore_allowed=false`，禁止整体回写旧 NVS 导致 epoch/fence 回退。

首次功能版：主机 `cf3ba5e2`（五路候选，epoch=144），从机 `144be637`，查询标定 k=5880/version=1。两端按实际 PlatformIO app 地址 `0x20000` 写入，保留 NVS、配网和标定。

### 6.3 B001：100 mL 真实出水，未通过整批验收

串口 `.pio/diagnostics/v4-bench-live.jsonl`，摘要 `v4-bench-analysis.json`；实际任务段 UTC 14:09:28～14:09:46（北京时间 22:09:28～22:09:46）。主机 cf3ba5e2、从机 144be637、节点 0002；PWM level=2/duty=75，目标 100 mL、任务上限 30000 ms。client=`563442454e4348`、seq=1、任务 144:75 已持久化，后续新任务不得复用该 client/seq。

| 项目 | 实际记录与结论 |
| --- | --- |
| 首 Start 实际提交 / StartACK / 启泵（主机同钟） | 303969 / 304116 / 304166 ms；首提交→启泵 197 ms |
| 首个启泵证明 | Lease issued=304120、接收/启泵=304166，年龄 46 ms、accepted=1 |
| 阀输出（从机同钟） | 299371 ms 开、314544 ms 关，非主机 uptime |
| 收到关闭证明 / 停泵（主机同钟） | 均为 319281 ms，处理延迟 0 ms |
| 关阀→停泵的串口接收时间差 | 约 71 ms，只是双串口接收观测；未校时，不能充当物理验收 |
| 前 9 条匹配 LeaseACK | 最大 215 ms；样本不足以作完整 P99 结论 |
| 正常紧凑业务 | C1/C2/CE/D2/D3/D4/D5 所见 trace 均 seg=0 |
| 最终结果 | 主从 600 pulse、折算 102 mL；DONE/TARGET、quiet=1、truncated=0；主机 done=1、failed=0、unknown=0、measured=1 |
| 控制间隔 | 主机 tick_max=66 ms，**超过 50 ms，整批验收未通过**；该次从机记录 39 ms |
| 物理水量和 APP | 量杯读数、量具最小刻度及清空确认待用户回复；APP 三端一致待核查。102 mL 是脉冲折算值 |

该组已暂停，不将这次完成出水记作“50 批基线通过”。原日志起始还有 80895 条重复残行 `it=3 seg=0 err=0`；真实任务段之后可定位，部分旧维护回复与 ESP_LOG 交错，完整 FLOW 通过查询重发取得。重复残行不能当作空口包数或发送速率证据。

### 6.4 串口优化版 3507fa72：发现启动回归，拒绝验收

为处理 66 ms 与回复交错，主机 UART 发送统一到中断驱动和 stdout 锁。该变更加深日志调用栈，原 3072 字节 BTC 栈在 GATT 注册阶段发生 `A stack overflow in task BTC_TASK has been detected`。`.pio/diagnostics/v4-console-bench-analysis.json` 保留 **137 条栈溢出、139 条 reset banner**；这是该采集可见条数，不是完整生命周期重启总次数。未发新抽水任务。

3507fa72 随后偶然启动并升到 epoch=302，不代表该版本可用。不得把最后一段 `healthy=1` 当作整段启动稳定。同期从机 hello_timeout 和 tick_max 是累计值，不能直接归因于某次水量任务。

### 6.5 BTC 修订版 5939b648：已部署，空闲验证范围

主机 defaults 及有效 sdkconfig 将 BTC 任务栈改为 6144 字节；GATT 回调保存真实任务 handle，周期记录 `btc_stack_min_bytes`。新栈观测低水位 2904 字节，即该已覆盖路径约用 3240 字节，超过旧 3072，支持栈不足诊断。尚未覆盖手机配对、重连和全部 BLE 业务路径。

仅重刷主机五路候选 app，`.pio/diagnostics/v4-btc-upload-master.log` 为 Hash verified/exit 0；未擦 NVS、未写 bootloader/partition、未重刷从机。部署后实际查询 epoch=303，PWM 恢复启动默认 level=1/duty=50/rev=0、泵输出=0；从机仍保留 144:75 DONE、阀输出=0。nodes 显示 0002 online=1、commissioned=1。

`.pio/diagnostics/v4-btc-bench-analysis.json` 的稳定空闲观察未见设备失败/reset，采集错误为空，无观测到未闭合 ticket；主机 tick_max=28 ms、btc_stack_min=2904、heap_min=80644、mesh_stack_min=1724、app_stack_min=16356 字节，pending/in_flight=0、restart_required=0。从机 hello_timeout=31/tick_max=55 是之前崩溃期及运行的历史累计值。**此处不是新版出水时序通过证据。**

该空闲采集开头另有 140186 条重复 `_stack_min_bytes=2904` 残行，约 7.5 秒接收窗口内远超 115200 串口实际字节速率，来源未定；原始保留，不据此计算事件率或声称完整启动已被采集。后续受控启动采集在开串口后明确清空输入缓存，再开始计划重启记录。

### 6.6 受控重复启动观察

本轮完成不启泵的 **5 次主机 RTS 计划重启**：每次仅 1 条对应 reset banner，无观测到栈溢出、其他设备失败或计划外重启；每次查询泵阀均关闭、Mesh 健康、pending/in_flight 均为 0。五次 build 均为 5939b648/max_concurrent=5，epoch 依次为 304～308；BTC 栈最低剩余范围 2900～3008 字节，主机本轮空闲 tick 最大 40 ms。末次节点 0002 online=1/commissioned=1；主机 epoch=308、PWM level=1/duty=50/rev=0/output=0，从机旧任务 144:75 保持 DONE、阀输出=0。两路串口均已关闭释放。

首轮前置查询正好捕获从机 HELLO 分段在途（in_flight=1，随后正常 event=3），在重启前终止；修正采集程序为有界等待真实完成，不把周期 HELLO 的瞬时在途判为故障，也不修改设备状态。成功观察的结果在 `.pio/diagnostics/v4-btc-boot-check-2-summary.json`，原始 `v4-btc-boot-check-2-live.jsonl` 约 3 分钟，无之前大段重复残行。分析结果设备失败/采集错误均为空，5 条 reset 与计划逐一对应。**此项只覆盖 5 次启动和短时空闲，不替代手机 BLE 业务、实水时序或 8 小时稳定性验收。**

### 6.7 继续测试的入口

待 B001 实际读数、量具精度和清空确认后，先在 5939b648 上复测 100 mL；查询真实 epoch、PWM revision 与主从输出，再使用新 seq。保留正常工况保护，若 tick 超限、启动失败、零脉冲、尾流异常或水量超差即停该组。单路测试通过后仍需补足其余 4 台，才可开展第 9.2 节的轮换、并发、8 小时和故障注入；不能以模拟节点代替。

## 7. 交付和回退

候选环境 `pump_master_five_candidate` 是明确的五路台架入口；`pump_master` 生产默认保持 1。五台、定量水路、真实 8 小时等门槛通过后才能把生产默认改为 5。当前不能标记“第 9 节全部完成”或正式发布五路。

打包工具校验源码摘要、两端共享头、SDK 完整文件清单、强 tracked-send 符号、ELF/bin 对应、有效并发宏、APP 测试和分区地址。包包含源码、固件/ELF、有效配置、测试日志、报告、APK、SHA256SUMS 和 zip 摘要。包工具本身不打开串口或启泵。

原不可变包 `dist/mesh-v4-five-candidate-20261020.zip` 是 cf3ba5e2 历史候选（ZIP SHA256=`98c40e1f98d658d5ec6666ff7a53d0d6f9d50d08f82cc8bb044187173b391f80`），不覆盖其中任何文件。最新 BTC 修订使用独立 `mesh-v4-five-btc-review-20261020-r2` 包；无 r2 后缀的同日文件是打包检查草稿，以 r2 为本轮交付。它仍是待验收的软件候选，不能因打包成功升级验收结论。原始串口、PIN 和 flash/NVS 备份不进入交付包。

实际 PlatformIO 地址：主 bootloader=0x1000，从 bootloader=0x0；两端 partition=0x8000，app=0x20000。不用 IDF 辅助文件中的 0x10000，不擦 NVS。

主机既有 NVS format 3，V4 未把 Packet/Dose 原始结构持久化、未增加持久化计数器格式。回退须使用配套主从且能识别当前 NVS 的组合；保留配网、标定、任务 fence 和客户端重放记录。旧 flash 中程序与配置区域备份用于取证和提取旧程序，**不能把旧 NVS 整体回写造成 epoch/fence 回退**。旧固件是否能读 format 3 必须核对。

本轮基于已有未提交改动继续实施，原始三仓差异与源码快照位于 `.pio/diagnostics/v4-implementation-baseline/`。没有将先前白名单移除、诊断和方案文件统称本轮新增。未执行 Git 提交或推送。
