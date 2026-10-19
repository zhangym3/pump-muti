# 2026-10-19 定量出水不足现场诊断

## 结论

已确认最近一笔 500 mL 任务 `116:26` 的从机最终状态为 `FAULT / LEASE`，累计 0 脉冲、计量 0 mL。它因通信租约保护停止，未达到目标水量。

随后完整抓到任务 `116:27` 的失败过程：主从两端出现大量 Mesh 分段发送资源耗尽、旧分段丢弃、重传次数耗尽；主机从准备进入等待开阀确认后，转入收尾，未在采样日志中出现供水阶段；从机最终同样报告 `LEASE`、0 脉冲。APP 本地记录确认这笔任务实际设置为 **200 mL**，不能把它写成 500 mL 复现。

当前首要故障定位为 **主从 Mesh 消息发送拥堵，状态确认/续租未及时完成，任务被保护逻辑中止**。发送重试策略存在放大拥堵的路径；具体最初由射频丢包、并发发送还是现场旧固件行为触发，现有日志无法单独定论。

## 采集条件与边界

- 主机 COM5，地址 `0001`；从机 COM6，地址 `0002`，APP 槽位 1。
- 采集自 2026-10-19 19:20:35 起；文件时间为 PC 接收串口行的本地时间（Asia/Shanghai），不是设备动作发生时间。
- 使用 115200 波特率，打开端口前关闭 DTR/RTS；仅发送 `hello`、`local`、`nodes` 只读查询，每 15 秒一次。
- 同时只读提取已连接 Android 手机的 APP 本地历史及 `MasterBle` 日志。
- 未下发出水命令、未刷写固件、未重启设备、未改标定/NVS。19:21:42 的出水请求来自现场操作。
- 已停止本次串口抓取并释放端口；证据已保存。后续任务需重新开启采集。
- 计量 0 mL 指设备上报结果，尚无量杯实测记录，不能据此断言物理出水绝对为零。

## 500 mL 历史记录

手机保存的请求格式：`start CLIENT SEQ EPOCH MASK ML MAX_MS`。

| 任务 | 目标 mL | 上报 mL | 主机结果 |
|---|---:|---:|---|
| 116:22 | 500 | 3 | Failed |
| 116:23 | 500 | 9 | Failed |
| 116:24 | 500 | 0 | Failed |
| 116:25 | 500 | 0 | Failed |
| 116:26 | 500 | 0 | Failed |
| 116:27 | 200 | 0 | Failed |

历史记录不包含每笔任务的从机停止原因。只有 `116:26`、`116:27` 在本次串口采集中直接确认 `reason=LEASE`，不能把该原因追溯套用到其他历史任务。

500 mL 原始请求（手机历史）：

```text
start 2a703339fccac1cd 8 116 00000001 500 60000
```

对应从机状态：

```text
2026-10-19T19:20:35.180 LOCAL job=116:26 phase=FAULT reason=LEASE valve_command=0 pulses=0 ml=0 k=5880 version=1 quiet=1 truncated=0
```

## 完整抓到的 116:27 时序

| PC 时间 | 证据 | 含义 |
|---|---|---|
| 19:21:42.147 | 主机 `R ... job=116:27 phase=1` | 已接收请求，准备任务 |
| 19:21:43.357 | 从机 `No multi-segment message contexts available` | 分段发送上下文已耗尽 |
| 19:21:43.509 | 主机同样出现上述错误 | 双向发送均受影响 |
| 19:21:43.711 | 主机 `phase=2` | 等待开阀确认 |
| 19:21:45.280 | 主机 `FLOW ... phase=PREPARED revision=961` | 主机仍持有准备状态，未收到有效开阀状态推进 |
| 19:21:46.950 | 主机 `pump_command=0 pump_phase=0 fault=NONE` | 当时泵输出关闭，无本地泵故障 |
| 19:21:47.708 | 主机 `phase=4` | 已转入收尾，约为等待开阀确认 4 秒超时 |
| 19:21:47.859 起 | 从机重复 `JOB_REJECT STALE` | 后续命令到达时任务已不再可启动，不能重新打开 |
| 19:21:50.240 | 从机 `FAULT reason=LEASE ... pulses=0 ml=0` | 从机租约保护停止 |
| 19:21:51.403 | 主机 `phase=7 ... done=1 failed=1 unknown=0` | 任务结算为失败 |

上述时间段还反复出现：

```text
BLE_MESH: Ignoring old SeqAuth, src 0x0002, dst 0x0001
BLE_MESH: Duplicate SDU from src 0x0002
BLE_MESH: Ran out of retransmit attempts
```

日志中本次任务无 `phase=3`（供水阶段）记录。结合等待开阀确认约 4 秒后转收尾、主机看到的状态一直是 PREPARED，可定位为开阀确认链路失败。串口轮询不是高频 GPIO 录波，无法给出阀动作或每一个租约包的精确时间。

## 源码核对

1. 主机 `src/master_scheduler.cpp` 的 Preparing / Opening 每 150 ms 重发未确认命令；Opening 超时为 4000 ms。
2. 从机 `src/application.cpp` 收到重复 PREPARE / START 且返回 OK 后，都会调用 `status(true)`。状态载荷 27 字节，加 vendor opcode 后需 Mesh 分段；周期性状态重试、Summary 与 EventAck 又会增加双向消息量。
3. 双端 `src/mesh_transport.cpp` 工作线程每次调用异步发送 API 后仅延迟 20 ms，没有等待同一目的地址前一条分段消息完成，也没有对相同状态做合并。业务队列优先级不能回收已经占用的 Mesh 分段上下文。
4. 本机安装的 ESP-IDF `components/bt/esp_ble_mesh/core/transport.c` 中，`No multi-segment message contexts available` 来自 `send_seg()` 无可用 `seg_tx` 时返回 `-EBUSY`。这是发送分段上下文耗尽，不能误写成普通堆内存不足或接收上下文耗尽。
5. 接收端 `trans_seg()` 会丢弃比已跟踪会话更旧的 SeqAuth。现场旧分段丢弃与重传失败支持“分段消息重叠/积压”的判断，但不足以确认最初丢包原因。
6. 从机 `src/flow_job_controller.cpp` 在开阀时开始 3000 ms 租约；租约超时检查早于初始无流量 5000 ms 检查。因此 0 脉冲任务首先显示 LEASE，并不能直接判定霍尔传感器损坏。
7. 当前 K=5880，按协议换算为 5880 pulses/L，500 mL 对应 2940 脉冲。当前任务以 FAULT/LEASE 停止，没有证据表明它因达到 2940 脉冲而正常停止，暂不应据此调整 K。
8. 从机现场 `INTERLOCK enabled=0 ready=1`，未显示外部联锁故障；主机 `fault=NONE`，无本地泵故障证据。

### 版本限制

本次 Git 更新只更新了工作区，未部署。现场主机 HELLO 没有当前源码输出的 `individual=1`，提示主机固件落后于工作区；不能把当前源码当作现场精确版本。

例如当前主机源码仅在 Supplying 阶段发送 Lease，并增加 LeaseAck 监控；较早提交 `6c0a5b4` 在非 Draining 阶段就发送 Lease。未获得设备固件哈希，不能断言现场一定运行哪种续租策略。上述 150 ms 重试、重复状态发送及分段异步并发路径在可见代码中有依据，但修复部署前仍需锁定现场主从固件版本。

## 建议处理顺序

1. 先记录主从实际固件构建版本/哈希，补充任务阶段、停止原因、Lease 序号与收发结果日志，避免仅凭 FLOW 的 FAULT 猜测原因。
2. 对同一目的地址的分段消息控制并发，跟踪发送完成/失败；相同 job/revision 的状态合并，增加有界重试和退避。避免 150 ms 业务重试持续生成新分段状态。仅扩大上下文数量不足以解决持续积压。
3. 核对开阀确认和初始租约的配合，保证发生通信失败时按既定时限停泵关阀；保留现有失联保护，不能通过取消保护或无限延长超时掩盖问题。
4. 改动完成并部署后，由现场操作复测 500 mL，完整捕获 Preparing → Opening → Supplying → Target → Done，记录脉冲及量杯实际水量。
5. 若通信稳定后仍提前停止，再按明确的 NoFlow/Runtime 等原因检查供水、泵驱动、阀、流量输入；只有正常 Target 结束但量杯偏差稳定时，才开展 K 标定。

本次仅完成诊断与证据归档，没有修改控制代码或部署修复，因此不能视为问题已修复。

## 原始证据位置

目录：`D:\pump\pump-muti\.pio\diagnostics\volume-20261019-192035\`

- `master.log`：主机带 PC 时间戳原始串口日志。
- `slave.log`：从机带 PC 时间戳原始串口日志。
- `android-master-history.properties`：APP 原始本地历史快照。
- `android-task-history.json`：按任务整理的历史记录。
- `android-snapshot.log`、`android-snapshot-final.log`：APP BLE 连接日志；未记录业务请求内容，业务请求以 APP 本地历史为依据。
- `capture.py`：本次使用的串口采集逻辑。启动时须直接传脚本绝对路径，避免通过 stdin 执行导致 `__file__` 指向错误目录。

`.pio` 属于本地忽略目录，原始证据不会随普通 Git 提交同步；长期交接时需单独保留。本诊断文档位于 `docs/`。

## 深入优化方案

后续已完成 [深入分析与优化实施方案](D:/pump/pump-muti/docs/MESH_VOLUME_OPTIMIZATION_20261019.md)，补充了真实 SAR 终态与 SDK send-comp 的区别、报文分段成本、当前默认单路与未来并发预算、Opening 续租/ACK 新鲜度、消息合并和停止屏障，以及实施批次和验收矩阵。配套有可复算的离线预算脚本与 JSON。

该方案细化了本诊断的建议；尚未修改控制代码或部署修复，不能视为修复验证结果。
