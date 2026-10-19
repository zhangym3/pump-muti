# 从设备离线现场诊断（2026-10-19）

## 结论与证据边界

**本次离线的直接原因已经定位：从机应用仍在运行，但 Mesh 发送调度器进入故障冻结，连周期 Hello 也不再发送；主机超过 15 秒收不到有效在线消息后，将地址 `0002` 判为离线。** 这是现场通信故障持续不恢复的原因。

首次触发冻结的具体错误发生在开始抓日志之前。现有计数不能区分“某个真实 SAR end 返回错误”“API/核心发送拒绝”或“关键 LeaseAck 排队过期”等路径，不能把其中任意一种写成已抓到的首因。`expired=4` 不是“确认有 4 次 LeaseAck 超时”。

用户确认主机和从机均执行过更新；但**实际主机回应仍不符合本次候选固件的行为特征**。需核实更新是否作用于实际运行固件，不能仅凭执行过上传或更新就认定版本一致。没有证据证明版本差异就是首次冻结的唯一触发因素。

本轮仅采集串口、发送 `hello` / `local` / `nodes` 查询、读取 Android BLE 日志和执行电脑上的离线复现。没有刷机、重启、清空 NVS、修改标定或发送泵阀控制命令。采集已结束，串口已释放。没有宣告硬件验收通过。

## 现场状态

时间均为 PC 接收时间。JSONL 保存 UTC，下表换算为 Asia/Shanghai。

| 项目 | 现场记录 | 判断 |
|---|---|---|
| 设备 | COM5 主机 `0001`，COM6 从机 `0002` | USB 枚举正常；不等同于 Mesh 正常 |
| 从机身份查询 | `addr=0002 master=0001 epoch=116 mesh=1 commissioned=1` | 地址及配网信息仍在，未退回未配网状态 |
| 从机控制状态 | `job=116:28 phase=FAULT reason=LEASE valve_command=0 pulses=0 ml=0 k=5880 version=1` | 租约故障终态，当前阀输出关闭 |
| 从机联锁 | `INTERLOCK enabled=0 ready=1 pin=6` | 本次查询未见外部联锁阻止 |
| 主机状态 | `pump_command=0 pump_phase=0 fault=NONE ... pump_ready=0` | 当前泵输出关闭，本地泵控制器未报故障；这不代表任务可继续 |
| 主机节点 | `online=0 commissioned=1 phase=OPEN_COMMAND_SENT pulses=0` | 主机保留了过时的开阀状态，尚未收到从机最终故障状态 |
| 主机任务查询（由现场 APP 发起） | `job=116:28 phase=7 unknown=00000001` | 主机尚未确认最终结果，不能直接清掉 unknown 或自动补抽 |
| 从机发送器 | `healthy=0 submitted=16 ended=16 merged=39 expired=4 faults=1 peak_sar=1 peak_pending=4 sar_max_ms=2777 tick_max_ms=36 trace_lost=0` | 故障冻结后计数不再增长；应用和本地串口仍正常响应 |

### 同步观察时序

1. **21:22:25—21:23:55**：双端被动采集 90 秒，不发送命令。从机每 10 秒继续输出 `mesh_diag`，但上述发送计数全部不变。
2. **21:24:21—21:25:26**：再次用块读取方式采集，并仅发送本地状态查询。从机继续每 10 秒输出同一组计数，`hello` 和 `local` 均正常回应。
3. 主机连续四次 `nodes` 的 `age_ms` 分别为 **349390、364345、379350、394345**，间隔约 15 秒，未被任何在线消息刷新。
4. 用查询接收时间减 `age_ms`，四次都得到约 **21:18:36.22** 的最后在线时间。这是近似推算，含串口接收延迟；不是冻结瞬间的直接录波。首次异常比本轮抓取早约 4 分钟。

原始从机日志示例：

```text
I (300542) mesh_diag: healthy=0 submitted=16 ended=16 merged=39 expired=4 faults=1 peak_sar=1 peak_pending=4 sar_max_ms=2777 tick_max_ms=36 trace_lost=0
I (470579) mesh_diag: healthy=0 submitted=16 ended=16 merged=39 expired=4 faults=1 peak_sar=1 peak_pending=4 sar_max_ms=2777 tick_max_ms=36 trace_lost=0
HELLO role=VALVE_SLAVE addr=0002 master=0001 epoch=116 mesh=1 commissioned=1
LOCAL job=116:28 phase=FAULT reason=LEASE valve_command=0 pulses=0 ml=0 k=5880 version=1 quiet=1 truncated=0
```

以上直接排除了“整个从机应用已经停止响应”的解释；不能据此排除历史电源波动或早先射频丢包。观察期没有看到从机 uptime 回退、panic、watchdog 或 brownout 日志。

## 代码机制与离线复现

核对从机 `include/mesh_tx_governor.h`、`src/mesh_transport.cpp` 和 `src/application.cpp`：

- `fault()` 将 `frozen_` 永久置为 true；当前类没有运行期恢复接口。
- API/核心发送拒绝、真实分段结束错误、缺失完成事件超过 6000 ms、关键队列失败/过期等均可进入该状态。
- `enqueue()` 与 `next()` 在冻结时直接拒绝，**没有为 Hello 单独保留可发送路径**。
- 从机虽然仍每 10000 ms 构造 Hello，但未检查 `mesh_.send(hello)` 的失败，仍推进 `helloAt_`。因此应用继续运行，主机却持续收不到在线消息。
- 从机的 `mesh_.ready()` 只检查地址和应用密钥绑定；它与 `mesh_.healthy()` 是两个概念。故障时出现 `mesh=1` 与 `healthy=0` 完全可能。
- 主机通过 Hello / Status / Summary 刷新 `seenAt`，以 15000 ms 判断离线。当前持续数分钟不刷新，不能靠把 15 秒阈值放宽解决。

故障冻结最初是为防止未知 SDK 占位被错误释放、旧授权继续发送而设置的保守保护。现场暴露的问题是：**任务停止与整个设备永久失去在线通信共用同一个不可恢复状态。** 这项限制必须在后续修复中明确处理，不能把它包装为射频资源问题已完全解决。

用当前从机真实头文件编译了最小复现：[复现源码](analysis/offline_governor_repro_20261019.cpp)。结果如下，全部为电脑上的逻辑复现，不是向设备注入故障：

| 输入场景 | 复现结果 |
|---|---|
| 一笔已被核心接受的 SAR，真实 end 返回 `-110` | 上下文释放、inFlight=0，但 healthy=false；随后 60 秒内 6 个 Hello 全部被拒绝 |
| Status 占用同目的 SAR，LeaseAck 在队列内等待达到 800 ms | 进入故障；即使 Status 后来成功结束、inFlight=0，随后 6 个 Hello 仍全部被拒绝 |
| 仅普通 Hello 排队达到 2000 ms 过期 | healthy 保持 true，下一轮 Hello 可以发送 |

第二个场景说明：从机的在途 SAR 不可抢占，即使 LeaseAck 队列优先级高，也可能被已经在途的普通 Status 阻挡。现场 `sar_max_ms=2777` 已超过 800 ms 的排队期限，但缺少首因时序，**不能断言本次恰好发生的是这一场景**。

复现命令（主机工程根目录）：

```powershell
& .pio/host-tools/ziglang/zig.exe c++ -std=c++17 -Wall -Wextra -Werror -I D:/pump/pump-muti-slave/include docs/analysis/offline_governor_repro_20261019.cpp -o .pio/diagnostics/offline-20261019/repro.exe
& .pio/diagnostics/offline-20261019/repro.exe
```

当前计数 `submitted=ended=16`、`peak_sar=1`、`peak_pending=4` 不支持“采集时从机还在持续耗尽发送上下文”的说法。它支持“发生过一次保护故障，发送器随后停止工作”。此前旧日志中的 `No multi-segment message contexts available` 不能作为本次首次冻结错误的替代证据。

`LOCAL reason=LEASE` 是任务终态原因，`faults=1` 是发送器故障计数，两者不能直接等同。缺少首因日志，无法确定两者谁先发生。

## 两端更新是否实际生效

用户反馈两端均已更新。现场仍有以下差异：

1. COM5 两次 USB `hello` 均返回：

   ```text
   HELLO role=PUMP_MASTER addr=0001 master=0001 epoch=116 mesh=1 commissioned=1 pump_fault=NONE
   ```

   当前源码及新主机 BIN 均包含末尾 `individual=1`；实际返回没有该字段。后续 65 秒块读取期间主机也没有新版每 10 秒 `mesh_diag`，从机有。
2. 历史 19:20:35 日志主机 epoch 也是 116。当前主机存储实现每次正常启动递增 epoch，因此现有证据还没有显示本次更新后新主机完成启动；确切启动/烧录结果仍需启动日志或上传记录核对。
3. 本机传统 `dist/manifest.json` 的日期在两工程中都仍是 **2026-10-17 23:37:26**；对应传统固件与本次候选不同。

| 文件 | 大小 | `individual=1` | `mesh_diag` |
|---|---:|---|---|
| 主机 `dist/pump_master/firmware.bin` | 988864 | 无 | 无 |
| 主机 `.pio/build/pump_master/firmware.bin` | 1000736 | 有 | 有 |
| 从机 `dist/valve_slave_c3/firmware.bin` | 962368 | 不适用 | 无 |
| 从机 `.pio/build/valve_slave_c3/firmware.bin` | 969488 | 不适用 | 有 |

新版候选位于主机工程：`dist/mesh-candidate-20261019-e0950038-495431ba/`，主机在 `master/firmware.bin`，从机在 `slave/firmware.bin`。传统 dist 目录没有随着本次候选打包自动替换。

用于核验的固件 SHA256：

```text
旧主机 dist: 3fa717cb8335180dcd2e6755eb9097ddccda74d441c38749fc735912a6488b78
新主机:      e3942813c65ce2f52f8cb3cf11383f4c59391c953c56a43a37c37307f0d47880
旧从机 dist: f21dc259663095715e1d951a3947a6d9bd9db647951a6245121eb1ec004db46c
新从机:      9e4fff0eceb6addf1ac72ea969be2d5186dd7ba050f768e7b00b4463d698b26e
```

另外，主机 `platformio.ini` 默认上传端口仍是 COM3，而当前物理主机为 COM5；应用实际分区是 `0x20000`，IDF 辅助 `flasher_args.json` 的 `0x10000` 不能用于本项目应用烧写。这些是已核实的部署隐患，**没有本次上传命令或记录，不能断言用户具体踩中了其中哪一项**。

从机已具有新版 governor 诊断行为，但没有本轮启动 buildId，不能只凭这些计数宣称实机精确身份已经核对为 `495431ba`。主机也不能仅凭回应格式反推出完整旧版 BIN SHA256。

## 修复和复验顺序

1. **先核对实际运行身份。** 在现场安排停机后，从正确物理端口和候选文件核对烧录结果、应用地址、启动日志。目标主机 `e0950038`、从机 `495431ba`。保留配网、fence 和任务结果记录。不能以清空 NVS 代替诊断。
2. **补充不会丢失的首因记录。** 发送器第一次 fault 时保留故障类型、ticket、op、peer、SDK error、queued/issued/failed 时间、在途/待发送数量；后续周期状态和本地只读命令能再次读取。记录放在内存即可用于这类持续运行的故障，不应高频写 NVS。`trace_lost=0` 仅表示设备 trace 队列没丢，不能补回 PC 尚未开始采集的历史。
3. **拆开业务安全锁存和通信恢复。** 当前任务故障后保持停泵/关阀及取消屏障，禁止自动恢复旧任务。对于已经明确收到拒绝或真实 end、全部相关占位已安全结束的情况，可设计有退避的诊断通信恢复，允许心跳和终态回报。恢复通信不代表允许供水。
4. **未知占位继续保守处理。** 缺失 end 的 ticket 不得按时间假释放；不应直接 `frozen_=false`，也不能单靠 `submitted==ended` 判断整个 Mesh/授权状态已安全。未能证明占位结束时，走明确的传输重建/人工维护流程，并保留业务故障锁存。
5. **空闲台架先复现，再做故障恢复验收。** 在隔离执行器的设备捕获从启动起的连续双端日志，分别注入一次已完成 SAR 错误、LeaseAck 排队过期、缺失 end。验收要求：输出保持关闭、不重放旧授权、首因可重复查询；可恢复类故障能重新报告心跳和原任务终态，缺失 end 不假释放；业务重新开放须满足既有身份/新鲜证明/新任务规则。

第 3—5 项是后续修复设计与验收要求，本轮没有更改生产发送器，也没有擅自部署或执行故障注入。正确版本对齐后仍须验收冻结恢复问题，不能把“重新刷写后暂时在线”当作修复完成。

## 其他现象与证据文件

- 初次主机采集中出现约 125 万条以 `d=1` 为主的重复短行。重新打开端口、改用块读取后，65 秒内主机查询输出正常。尚未确定重复数据来源，不将其当作从机首次故障的证据；保留原始数据供复核。
- Android `MasterBle` 记录有短暂 GATT=133，随后 **21:20:15 控制通道就绪、MTU=185**；也有 21:25:44 写超时。手机链路问题可能同时存在，但 USB `nodes` 本身已显示 offline，不能仅归因于 APP 缓存。
- 本轮现场采集不包含首次故障前的 `mesh_tx event=... err=...`。继续抓当前冻结后的空闲日志，无法补回首次错误。

证据均在主机工程：

- `dist/offline-20261019-live-01.jsonl`：双端被动采集，90 秒、0 命令、采集结束错误列表为空。
- `.pio/diagnostics/offline-20261019/COM5-query.jsonl`、`COM6-query.jsonl`：65 秒只读查询；旁边 `.log` 为按换行重建的人类可读输出。
- `.pio/diagnostics/offline-20261019/query_readonly.py`：实际使用的查询脚本。
- `.pio/diagnostics/offline-20261019/android-ble.log`：Android BLE 日志快照。
- `.pio/diagnostics/offline-20261019/repro-results.jsonl`：逻辑复现结果。
- [结构化诊断证据及 SHA256](SLAVE_OFFLINE_DIAGNOSIS_20261019.json)：采集路径、查询记录、状态、最后在线时间推算和证据校验值。
