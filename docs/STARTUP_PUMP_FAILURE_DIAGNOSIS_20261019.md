# 启动后无法抽水与阀门异常诊断（2026-10-19）

## 结论

现场没有证据指向泵电机故障、标定参数错误或外部联锁阻止启动。直接原因是从机 Mesh 发送调度器在启动后约 31 秒锁存了传输故障：`LeaseAck`（操作码 `ce`）在待发送队列中超过 800 ms 后过期。过期时已有 1 条发送在途、队列另有 3 条消息。该故障触发从机 `Reason::Transport` 业务锁存，关阀并拒绝新的 Prepare/Start/Lease。

这解释了“启动后提示异常、无法抽水”：主机仍能发现从机并接受手机提交的任务，但任务停在 Preparing，随后进入失败结算；没有进入 Opening 或 Supplying，故泵没有获得启动授权。现场显示的“部分阀门执行异常”对应已选节点的失败位，不代表已抽出一部分水。记录中的 `total_ml=0`、从机 `pulses=0 ml=0` 与此一致。

高置信度诱因是 Mesh 分段发送拥塞与关键确认报文期限不匹配。现场后续测得从机分段发送 SAR p99 为 2543 ms、固件观测峰值 3232 ms，而 LeaseAck 队列 TTL 只有 800 ms；主机日志同期明确出现多段消息上下文不足和重传耗尽。日志足以说明传输链路无法满足当前关键报文时限，但不能单独确定射频干扰、节点距离、SDK 缓冲配置或具体在途消息中哪一项是最初的硬件/空口诱因。

## 现场证据

| 项目 | 记录 | 含义 |
|---|---|---|
| 主/从 USB | COM5 主机 `0001`；COM6 从机 `0002` | 两端 USB 控制台在线；USB 在线不等于 Mesh 健康 |
| 从机身份 | `build=80ba7f64 tx_healthy=0 mesh=1 commissioned=1` | 从机运行已知新版本且仍可响应本地诊断；Mesh 发送器业务健康锁存已触发 |
| 首个故障 | `reason=QUEUE_EXPIRED t=31436 ticket=0 peer=0001 op=ce err=0 job=116:29` | 启动后 31.436 秒，LeaseAck 尚未发出便从队列过期（ticket=0）；不是“ACK 已发出但主机未收到”的直接证据 |
| 故障瞬间队列 | `issued=0 queued=30635 in_flight=1 pending=3 seq=2` | 消息排队约 801 ms 后过期；当时已有一个发送在途及三条待发消息 |
| 故障后从机 | `healthy=0 diagnostic_ready=1 restart_required=0`；`LOCAL ... reason=TRANSPORT valve_command=0 pulses=0 ml=0` | 可继续发送诊断，但业务保持锁定；阀门驱动命令为关，当前任务没有脉冲 |
| 联锁 | `INTERLOCK enabled=0 ready=1 pin=6` | 当前未配置为阻止启动的互锁，也没有报告未就绪 |
| 主机 | `pump_command=0 pump_phase=0 fault=NONE pump_ready=1` | 泵控制器自报就绪且无本地故障，但调度器因节点/通信失败未发出泵启动命令 |
| 从机可见性 | `NODE ... online=1 commissioned=1 phase=FAULT`，`age_ms` 周期刷新 | 本次采集中从机在线，不是当前离线；在线心跳与业务健康是不同状态 |
| 任务 `116:36` | `phase=1 mask=1` 后 `phase=4 failed=1`，终态 `phase=7 done=1 failed=1`；`V ... total_ml=0` | Preparing 后直接进入 Draining/Failed，没有 Opening/Supplying；该节点没有形成有效抽水 |
| 主机 Mesh 错误 | 第一段采集发现 5 次 `No multi-segment message contexts available`、3 次 `Ran out of retransmit attempts` | 与发送上下文耗尽及 Mesh 传输拥塞相符 |
| SAR 时延 | 103 个已结束分段发送；p99 2543 ms、样本最大 3115 ms；固件汇总峰值 3232 ms；`trace_lost=0` | 大多数慢发送远超从机 800 ms 队列期限；分析见同目录 `timing.json` |
| 后续错误 | `reason=SAR_END ... op=c5 err=-116`；第二次采集故障计数达到 6 | 故障锁存后仍可恢复诊断发送，但 Mesh 分段状态上仍出现 SAR 错误；`-116` 是底层返回值，当前日志没有足够信息把它归因到某种物理故障 |

故障后继续提交的任务不会解除锁存。第一次采集记录 job `116:36`，后续至少 `116:37` 仍是零脉冲终态；反复点击只增加失败任务并产生更多 Mesh 流量。手机/主机返回 `reply=OK` 表示主机接受了任务请求，不是阀门已打开或已抽到水。

## 代码路径核对

- 从机 [`mesh_tx_governor.h`](../../pump-muti-slave/include/mesh_tx_governor.h) 将 `LeaseAck` 队列 TTL 设为 800 ms，并把关键报文过期作为传输故障锁存。
- 从机 [`application.cpp`](../../pump-muti-slave/src/application.cpp) 在 Mesh 不健康时调用 `valve_.emergency(Reason::Transport, now)`，并拒绝新的 Start。
- 从机 [`flow_job_controller.cpp`](../../pump-muti-slave/src/flow_job_controller.cpp) 将 transport fault 独立锁存；随后 `prepare/start/lease` 返回 `NotReady`，阀门命令保持关闭。
- 主机 [`master_scheduler.h`](../include/master_scheduler.h) 的 LeaseAck 接收期限为 2400 ms；本次首个故障发生在从机 ACK 尚未出队阶段，因此仅放宽主机接收期限不会修复 800 ms 队列过期，也不能作为单独安全修复。
- 主机 [`master_scheduler.cpp`](../src/master_scheduler.cpp) 只有节点 Prepare/Start 状态确认后才会进入 Opening/Supplying；现场报告未到达这两个阶段。
- 手机端 [`MasterProtocol.kt`](../../pumpmutiapp/app/src/main/java/com/cczb/app/MasterProtocol.kt) 将 `TRANSPORT` 显示为 Mesh 通信异常、保护停止；任务的 failed 位显示为阀门执行异常。

## 固件身份边界

从机明确报告 `80ba7f64`，与本轮修复候选中的从机版本一致。主机 COM5 的 `hello` 回应只有 `role/address/master/epoch/mesh/commissioned/pump_fault`，没有该候选固件应有的 `individual=1 build=... tx_healthy=...` 字段，因此无法从本次现场记录证明主机运行的是候选 `4c3a56d9`。用户已确认主从都执行过更新；这里反映的是现场主机身份仍未核实，不能据此断言没有执行更新，也不能把版本差异认定为唯一故障原因。

本次库存只看到 slot 1、地址 `0002`，且失败任务 mask 为 `00000001`。因此诊断证据覆盖这一条从机/阀门路径，不能据此判断其它阀门的独立状态。

## 建议的修复与验收顺序

1. 保留本报告和原始串口文件；停止反复提交任务。当前任务均为终态且命令输出关闭，但这是软件命令状态，不是机械阀位或实际无流量的传感器证明。
2. 在现场通过只读 `hello` 核验两端 build。确认主机是否为 `4c3a56d9`、从机是否为 `80ba7f64`；核对 COM5 确实连接正在控制现场设备的主机。
3. 修复重点放在关键 LeaseAck 的传输设计：优先把租约确认设计成不依赖同一节点的分段发送资源、且仍能绑定当前任务和序号的短报文；同时检查 Mesh 重传、分段上下文和业务遥测/状态消息占用。不能仅把 800 ms 放大到数秒或去掉故障锁存，因为主机授权时限和从机租约安全需要重新做端到端时序证明。
4. 修复固件先做至少 1/2/5 节点的拥塞与丢包测试，重点验证最坏 LeaseAck 时延、旧 ACK 不会授权新任务、分段上下文不耗尽，以及在故障下泵阀保持关闭并可继续上报诊断。
5. 现场先保存首因并检查管路、泵阀物理安全，再按受控步骤重启/恢复故障锁存。先做单路低风险空载检查，确认主从版本和能力匹配、开启/关闭命令与状态一致，再做带计量的 500 mL 实水验收；连续多次成功且目标量误差达产品标准后，才扩展到多路。

本轮没有刷写、重启、清除存储或操作泵阀。因现场输出保持关闭且任务没有进入供水阶段，本次是故障取证，不是实水验收。

## 原始采集

- 第一轮双端采集：`D:\pump\pump-muti\.pio\diagnostics\log-follow-20261019-220044`，COM5/COM6 各约 180 秒；其中 `timing.json` 汇总分段发送时延。
- 启动失败复核采集：`D:\pump\pump-muti\.pio\diagnostics\start-failure-20261019-2207`，2026-10-19 22:13:15 至 22:16:16（本地时间），主机 13,921 行、从机 494 行，采集结束无串口错误。
- 复核时 COM6 `mesh_diag` 的 `submitted/ended` 从 623/623 增至 629/629，说明诊断发送仍在工作；首个 `QUEUE_EXPIRED` 仍为 `t=31436`，`healthy=0` 和本地 `TRANSPORT` 终态没有清除。
