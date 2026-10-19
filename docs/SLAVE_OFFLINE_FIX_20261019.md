# 从机离线修复与软件验收（2026-10-19）

本次已修复“发送器发生一次保护故障后，连 Hello 和终态也永久停发”的代码路径。**故障后业务继续锁存停机；确认旧发送已真实结束后，仅恢复诊断通信。** 主从固件构建、电脑回归及故障仿真通过，尚未刷写设备或完成实机验收。

本报告取代旧实施报告中的候选版本信息。此前现场诊断保留为历史证据：[离线诊断](SLAVE_OFFLINE_DIAGNOSIS_20261019.md)。现场主从机均执行过更新是用户已确认的事实；现场主机回应格式与此前候选不一致的问题，仍须通过新加的 build 查询核实，不能据此声称用户未更新。

## 1. 解决的问题与保留的边界

原调度器把任务保护与通信永久冻结绑定：真实 SAR end 错误、发送拒绝或关键报文排队过期均会使 `frozen_` 永久生效；即使占位已全部安全结束，也无法再发 Hello。主机超过 15 秒无有效在线消息后判离线。

本次修复这一持续离线机制，并保留首次故障快照，便于下一次查清触发错误。历史首次故障发生在抓日志之前，仍不能确认究竟是 SAR 错误、API/核心拒绝还是 LeaseAck 过期。本修复不证明现场射频与资源问题已消失，也不自动补抽未完成的水量。

## 2. 故障后的实际行为

1. SDK 错误回调在发送锁内立即锁存故障，记录首因；清空故障前待发送队列，禁止继续派发 Prepare、Start、Lease。已提交的发送继续保留真实 ticket 跟踪。
2. 控制循环观察到故障后停止本地输出。主机现有调度停止逻辑继续生效；从机新增独立 `transportFault_` 锁存，prepare/start/lease/commission 均返回 NotReady。
3. 只有所有旧 ticket 经真实拒绝/接受和 end 流程结清后，才允许恢复诊断发送；不是仅比较 submitted/ended 计数，也不按超时伪造回收。
4. 首次退避 1 秒，诊断发送再次失败后依次为 2、4、8 秒，封顶 8 秒。恢复期间最多一个诊断发送在途，Hello 提升优先级，避免终态重试一直挤掉心跳。
5. 只接受新构造的白名单报文：Hello/HelloReply、能力查询/回复、状态查询、Close、EventAck、明确拒绝的应答，以及阀命令关闭的 Idle/Settling/Done/Fault 状态。禁止开放状态、OK 授权证明和业务报文。
6. 等待核心完成或 SAR end 达到 6000 ms 时，设置不可自动清除的 `restart_required=1`；即使之后迟到回调释放 ticket，仍不恢复发送。ticket/序号耗尽同样要求维护重启。控制输出按原有更短授权期限保护，不等待 6000 ms 才停机。
7. 从机故障模式每 3 秒尝试 Hello，正常模式仍为 10 秒；只有入队成功才推进发送时间和状态重试次数。恢复 Hello 成功入队后重新开放终态上报预算，避免长时间冻结后用尽 6 次重试。
8. **任何通信恢复都不清除业务故障。** 两端 governor 的 `healthy()` 本运行期保持 false；从机能力回复 features=0，独立控制锁存也继续拒绝开阀。`healthy=0` 与心跳恢复可以同时成立。

这里的“关闭”指 GPIO/驱动命令关闭，没有新增机械阀位传感器；真实漏水和尾流仍须现场验证。首次故障快照保存在 RAM，重启会丢失，应在受控重启前保存日志。

## 3. 新增诊断入口

主从 USB `hello` 均增加精确源码 build ID 和 `tx_healthy`。`mesh-diag` 为只读命令，`local` 也附带同样信息；每 10 秒日志重复输出保存的证据。

```text
MESH_STATE healthy=0 diagnostic_ready=1 restart_required=0 in_flight=0 pending=0 backoff_ms=1000
MESH_FAULT first=1 reason=SAR_END t=... ticket=... peer=0001 op=... err=-110 job=...:...
MESH_FAULT_TIMING first=1 issued=... queued=... in_flight=... pending=... seq=... rev=...
```

`diagnostic_ready=1` 仅表示此刻允许诊断派发；正常健康时或有诊断报文在途时可为 0，不能当作总体在线状态。`restart_required=1` 表示占位完成过程出现不确定性或身份计数耗尽，禁止自动恢复发送。它为 0 也不表示业务已解锁。

`first=1` 是本次启动首次故障，后续错误不覆盖；最新不同故障另列 `first=0`。`faults` 现在统计不同故障事件，诊断重试的失败也会增加，不能当作掉线次数。正常 trace 被读走或环形缓冲溢出后，首因快照仍可查询。单行不超过本地 192 字节缓冲区。

## 4. 软件验收结果

| 检查 | 本轮结果与范围 |
|---|---|
| 主机/从机 PlatformIO 固件构建 | PASS；ESP-IDF 5.4.0；未连接串口 |
| 主机控制/协议回归 | PASS，28 组 |
| 从机业务锁存 | PASS，Idle/Prepared/Opening/终态均验证；配置、Close 和手动停止不清除锁存；拒绝授权不额外写 fence |
| 拒绝与真实 end 错误 | PASS，包含 end 先于 accepted；首因不被覆盖、旧队列清空、不能重放授权 |
| 2777 ms SAR 与 800 ms LeaseAck 过期 | PASS；必须等待原 SAR 真实结束后才恢复 Hello |
| 缺失 accepted/end | PASS；6000 ms 后维护锁存，迟到回调不自动解锁 |
| 恢复退避、计时回绕、报文白名单 | PASS，限制单个在途，拒绝开放状态与 OK 租约证明 |
| 主/从分别故障的联动仿真 | PASS；泵阀命令关闭、心跳更新、终态对账，新授权被拒绝，无额外启泵/开阀 |
| 1/2/5 节点，各 1000 批次 | PASS；500 mL/2940 脉冲模型，含重复 Start/丢 EventAck |
| 5 节点错误/虚拟 8 小时 | PASS；3340 批次，无累计队列/ticket；是逻辑仿真，不是射频或实水 |
| SDK 提取函数桩及组件锁定 | PASS；实际补丁函数在 host stubs 执行，包含拒绝、真实 end、乱序及 reset 不回调；不等于完整 RTOS/空口测试 |
| 日志工具 | PASS，7 项 |
| Android | 本轮未改；APK 取自旧候选并核对清单哈希，当前源码/配置和此前 78 项测试日志/XML 与旧快照相同；复用证据，未重跑 |

持久证据：主机 `.pio/{build,host,sdk,log-tools}-offline-fix.log`；从机 `.pio/{build,host}-offline-fix.log`；双方 `.pio/compile_commands-offline-fix.json` 与 `.pio/envdump-offline-fix.log`。候选包内保存其副本；源码、镜像、ELF 和证据 SHA256 见[机器可读验收](SLAVE_OFFLINE_FIX_20261019.json)及包内 `candidate.json` / `SHA256SUMS`。

现有 SDK 模型初始化等编译警告仍存在，构建无错误；电脑 C++ 测试使用 `-Wall -Wextra -Werror`。本轮没有修改 SDK 补丁或协议线格式。

## 5. 配套固件与交付

| 角色 | 本轮 build ID | 静态 RAM | Flash 程序大小 |
|---|---|---|---|
| ESP32 主机 | `4c3a56d9` | 58392 B | 1003424 B |
| ESP32-C3 从机 | `80ba7f64` | 37224 B | 972578 B |

主机白名单仅接受本次从机 `80ba7f64`。生产并发保持 **1**；最多 5 路顺序调度的 host 仿真已通过，5 台真实无线节点与真实水路仍待验收。

新候选目录：`D:/pump/pump-muti/dist/mesh-offline-fix-20261019-4c3a56d9-80ba7f64/`，旁边有同名 ZIP 和 SHA256。主机、从机分别位于 `master/firmware.bin`、`slave/firmware.bin`。传统角色目录 `dist/pump_master` / `dist/valve_slave_c3` 与旧候选没有被替换，不能将其中旧 BIN 当作本修复。

两端应用偏移均为 **0x20000**；主机引导为 0x1000，从机引导为 0x0，分区表均为 0x8000。以包内 `candidate.json.flash_files` 为准，不用 IDF 辅助文件中的 0x10000。最近识别主机 COM5、从机 COM6；主机工程默认仍是 COM3，上传前须显式核对端口。

本轮没有刷写、重启设备、清除 NVS 或控制泵阀。已有配网、取消 fence、任务结果和防重放记录须保留；主机 NVS format 3 的回退需要既有迁移方案。

## 6. 尚待进行的设备验收

按[现场步骤](MESH_FIELD_ACCEPTANCE_20261019.md)执行，先隔离执行器进行无水台架验证，再进行实水计量。每项保留从启动到故障、恢复后的连续双端原始日志。

1. 核对 `hello` 主机 `build=4c3a56d9`、从机 `build=80ba7f64`，记录地址、端口、BIN SHA256；确认输出关闭、配网信息及旧任务结果。
2. 在独立测试构建注入一次核心拒绝/已结束 SAR 错误：首因可重复查询；输出关闭；全部旧 ticket 结清且退避完成后，Hello/关闭状态能实际到达；连续至少 60 秒主机在线时间更新；任务为失败终态，不产生新开阀。
3. 复现 Status 长 SAR 阻塞 LeaseAck：过期仍停止业务，真实 end 之前不恢复发送；之后只恢复诊断，不能自动续租或补抽。
4. 抑制 accepted 或 end：6000 ms 后 `restart_required=1`，不伪造回收；即使补发迟到回调也不得恢复。USB 诊断仍可用，泵阀命令维持关闭。硬件注入项未执行前不能标为 PASS。
5. 保存首因、旧任务终态/unknown 后受控重启；确认取消 fence 未清、旧任务不能重放。核对身份、能力及任务状态，之后才用新请求/新 JobKey 开始新业务。重启不会替代未完成水量的人工确认。
6. 1/2/5 台射频、每种规模各 20 批 500 mL、丢包/断电保护、8 小时真实台架、运行堆/栈峰值仍为 PENDING。500 mL 允许误差尚须明确，不能用脉冲模型宣称实际出水精度达标。

## 7. 代码入口与复现命令

- 两仓 `include/mesh_tx_governor.h`：故障快照、清队列、诊断白名单、退避及不可自动清除的维护状态；host 入口强制校验两份共享头文件相同。
- 两仓 `include/mesh_tx_diagnostics.h`、`src/mesh_transport.cpp`：带锁快照和可重复读取的日志格式。
- 从机 `src/flow_job_controller.cpp`、`src/application.cpp`：业务锁存、能力降级、故障心跳和终态重试。
- 主机 `src/application.cpp`、`include/mesh_deployment.h`：版本查询、故障查询、配套从机身份。
- 主机 `test/mesh_tests.cpp`、从机 `test/core_tests.cpp`：故障恢复矩阵与主从联动保护。
- 主机 `tools/package_mesh_candidate.py`：本轮证据标签、报告归档、未改变 APP 的复用校验。

```powershell
# 在主机根目录执行；从机 host 测试在相邻从机目录执行同名脚本
& C:/Users/79622/.platformio/penv/Scripts/python.exe tools/run_host_tests.py --compiler .pio/host-tools/ziglang/zig.exe
& C:/Users/79622/.platformio/penv/Scripts/python.exe tools/run_sdk_tests.py --framework C:/Users/79622/.platformio/packages/framework-espidf --compiler .pio/host-tools/ziglang/zig.exe
& C:/Users/79622/.platformio/penv/Scripts/python.exe test/log_tools_tests.py
```

构建先从机后主机；从机源码变化会改变 build ID，须审查并更新主机白名单。重新打包使用 `--evidence-label offline-fix`，并附带本报告与 JSON；独立候选目录不得覆盖历史证据。
