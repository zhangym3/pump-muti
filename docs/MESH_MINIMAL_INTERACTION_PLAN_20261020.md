# BLE Mesh 最小关键交互方案

日期：2026-10-20
状态：软件实现与主机修复部署已完成；APP 协议兼容层未改；射频和实水验收未完成
适用范围：ESP32 主机 + ESP32-C3 从机，主机直连多个从机的 BLE Mesh Vendor Model

## 1. 设计目标

本方案针对当前现场暴露的分段拥塞和控制链路过于复杂问题，目标是：

1. 正常出水只保留控制闭环和最终结果，不再持续上报流量摘要。
2. 所有出水控制报文使用从机单播地址，不使用群组地址承载授权、租约和关闭。
3. 保留租约和本地超时，主机或 Mesh 失联时从机必须自动关阀。
4. 速度作为任务参数进入授权链路，但速度是主机全局泵速，不是每个从机独立调速。
5. 终态以 JobKey 和结果序号去重；结果丢失时只允许按需查询，不自动重启出水。
6. 把无线传输时延作为显式约束，禁止用固定的 800 ms 队列期限假定 Mesh 一定及时。

本方案不把当前网络称为多跳 Mesh。当前 Relay 关闭、Friend 不支持、TTL 为 3，实际运行模型仍是主机到多个从机的单跳星型控制网络。

## 2. 正常任务保留的交互

正常任务只保留以下五类业务交互：

| 类别 | 方向 | 作用 | 正常任务是否必需 |
| --- | --- | --- | --- |
| PREPARE / COMMAND_ACK | 主机 ↔ 从机 | 校验任务、标定、速度和运行窗口，准备阀门 | 是 |
| START / COMMAND_ACK | 主机 ↔ 从机 | 在全部目标节点准备完成后打开阀门 | 是 |
| LEASE / LEASE_ACK | 主机 ↔ 从机 | 保持授权，防止主机失联后阀门保持打开 | 是，按周期 |
| RESULT / RESULT_ACK | 从机 ↔ 主机 | 目标完成或故障后上报一次终态 | 是，每个节点一次 |
| CLOSE | 主机 → 从机 | 人工停止、通信异常或批次故障时关闭阀门 | 仅停止或异常 |

以下交互从正常任务中删除：

| 删除项 | 处理方式 |
| --- | --- |
| SUMMARY | 完全取消，不做 2 秒周期流量上报 |
| 运行期 STATUS | 完全取消；只发送终态 RESULT |
| EVENT_ACK | 改名为 RESULT_ACK，只确认终态，不确认中间状态 |
| 周期 HELLO | 保留每 10 秒一次在线心跳；携带 build ID，用于在线检测、恢复和能力缓存失效判断，不携带流量遥测 |
| 每批次强制 CAPABILITIES_GET | 使用缓存能力；缓存失效或版本变化时才单播探测 |
| 群组 LEASE / CLOSE | 改为按节点单播，避免无确认广播和同时碰撞 |
| 正常流程 STATUS_GET | 取消；只在结果丢失或主机重启后的恢复流程按需查询 |

STATUS_GET 的按需恢复用途改名为 RESULT_QUERY，它不是实时遥测，也不能触发重新出水。

## 3. 任务身份和速度参数

### 3.1 任务身份

所有任务继续使用：

~~~text
JobKey = epoch:u32 + id:u32
~~~

两者都必须非零。主机在发送任何 PREPARE 之前持久化任务和 JobKey。从机以 JobKey 做幂等判断：

- 重复的 PREPARE 不创建新任务。
- 重复的 START 不重复开阀。
- 旧 JobKey 永远不能覆盖新任务。
- 主机重启后不自动重发 START。

### 3.2 速度因素

当前硬件只有主机全局 PWM，没有每路泵速或压力反馈。因此速度字段定义为本批次全局泵速配置，不表示每个从机有独立速度控制。

speed_factor_permille:u16 使用固定白名单：

| 值 | 含义 | 当前 PWM 参考 |
| ---: | --- | ---: |
| 500 | LOW | 50% |
| 750 | MEDIUM | 75% |
| 1000 | HIGH | 100% |

主机在发出第一条 PREPARE 前设置并锁定本批次速度。任务期间禁止改速。主机只接受白名单值，其他值返回 INVALID。从机把速度字段作为任务一致性和标定校验的一部分，不直接驱动主泵。

每个从机的 k 必须针对所选速度标定。主机不能使用 LOW 的 k 配 HIGH 的 PWM；注册表应保存 (node, speed_factor_permille, k, version) 的对应关系。当前代码只有单一 k，没有按速度档区分的标定表，因此该速度标定表是实施前必须增加的配置，不是现有固件已经具备的能力。

## 4. Mesh 报文格式

协议修订建议提升为 3。所有整数小端编码；源地址和目标地址由 Mesh 上下文提供，不在业务载荷内重复携带。下表只列业务载荷，不含 opcode、网络头和 MIC。

### 4.1 PREPARE，主机 → 从机，单播

~~~text
key.epoch:u32
key.id:u32
target_ml:u32
target_pulses:u32
k:u32
version:u32
max_runtime_ms:u32
speed_factor_permille:u16
lease_ms:u32
flags:u8
~~~

总载荷 35 字节，必然属于 Mesh 分段报文。

约束：

- target_ml 为本节点目标体积，统一任务时每个节点相同，逐路任务时可不同。
- target_pulses 由主机使用该节点、该速度档的 k 计算，从机仍需复核。
- max_runtime_ms 是单次出水硬上限。
- lease_ms 是从机失去续租后关阀的本地期限，建议范围 10000..20000 ms，不得小于本批次已测得的分段时延窗口。
- flags 当前保留为 0；未知位必须拒绝。

从机收到并持久化任务栅栏后，进入 Prepared，但不打开阀门。

### 4.2 COMMAND_ACK，从机 → 主机，单播

~~~text
key.epoch:u32
key.id:u32
command:u8       // PREPARE 或 START
result:u8        // OK 或明确拒绝原因
~~~

总载荷 10 字节，必然属于 Mesh 分段报文。成功和失败都必须回执，不能用“没有拒绝回执”推断成功。

### 4.3 START，主机 → 从机，单播

~~~text
key.epoch:u32
key.id:u32
~~~

总载荷 8 字节。只有对应任务已经 Prepared，且 JobKey、速度档和标定版本匹配时才允许执行。成功后从机打开阀门并开始本地脉冲计量。

### 4.4 LEASE，主机 → 从机，单播

~~~text
key.epoch:u32
key.id:u32
sequence:u32
~~~

总载荷 12 字节。sequence 单调递增，重复序号不重复延长租约。主机按节点轮询发送，不得在同一目的地址上同时存在两条分段报文。

### 4.5 LEASE_ACK，从机 → 主机，单播

~~~text
key.epoch:u32
key.id:u32
sequence:u32
result:u8
~~~

总载荷 13 字节。OK 只表示该序号已被从机接受；它不表示已经达到目标体积。主机必须按 JobKey + sequence 去重，迟到或过期回执不能授权新任务。

### 4.6 RESULT，从机 → 主机，单播

从机完成目标、检测到故障或收到关闭后，只发送终态结果：

~~~text
key.epoch:u32
key.id:u32
pulses:u32
close_pulses:u32
k:u32
result_sequence:u32
phase:u8          // DONE 或 FAULT
reason:u8
flags:u8          // bit0=valve_command, bit1=quiet, bit2=truncated
~~~

总载荷 27 字节，必然属于 Mesh 分段报文。

结果规则：

- pulses 是任务结束时的计量值，close_pulses 是关阀结算值。
- k 必须等于 PREPARE 使用的标定值；不一致时主机标记 UNKNOWN，不计算为可信体积。
- 结果按 JobKey + result_sequence 幂等；从机可最多重发 3 次，间隔建议 500、1000、2000 ms。
- RESULT 发送前从机先关阀，再进入 DONE 或 FAULT。

### 4.7 RESULT_ACK，主机 → 从机，单播

~~~text
key.epoch:u32
key.id:u32
result_sequence:u32
~~~

总载荷 12 字节。收到匹配回执后，从机停止该任务的结果重发。未收到回执不能修改已保存的终态。

### 4.8 CLOSE，主机 → 从机，单播

~~~text
key.epoch:u32
key.id:u32
reason:u8
~~~

总载荷 9 字节。主机必须先关闭本地泵，再按节点发送 CLOSE。从机收到后立即关闭阀门并发送 RESULT(FAULT/MANUAL)；即使 CLOSE 丢失，本地 lease_ms 或 max_runtime_ms 也必须最终关阀。

### 4.9 RESULT_QUERY，主机 → 从机，单播

~~~text
key.epoch:u32
key.id:u32
~~~

总载荷 8 字节。只允许在正常结果超时、主机重启恢复或人工查询时发送。它不能打开阀门，不能延长租约，不能清除故障锁存。

## 5. 正常时序

~~~mermaid
sequenceDiagram
    participant M as 主机
    participant S1 as 从机1
    participant SN as 从机N

    M->>S1: PREPARE(JobKey, volume, k, speed, lease)
    S1-->>M: COMMAND_ACK(PREPARE, OK/ERR)
    M->>SN: PREPARE(JobKey, volume, k, speed, lease)
    SN-->>M: COMMAND_ACK(PREPARE, OK/ERR)
    M->>S1: START(JobKey)
    S1-->>M: COMMAND_ACK(START, OK/ERR)
    M->>SN: START(JobKey)
    SN-->>M: COMMAND_ACK(START, OK/ERR)
    M->>S1: LEASE(seq)
    S1-->>M: LEASE_ACK(seq)
    M->>SN: LEASE(seq)
    SN-->>M: LEASE_ACK(seq)
    M->>M: 全部首租约确认后启动本地泵
    loop 运行期间，按节点轮询
        M->>S1: LEASE(seq+1)
        S1-->>M: LEASE_ACK(seq+1)
        M->>SN: LEASE(seq+1)
        SN-->>M: LEASE_ACK(seq+1)
    end
    S1-->>M: RESULT(DONE/FAULT)
    M-->>S1: RESULT_ACK(result_sequence)
    SN-->>M: RESULT(DONE/FAULT)
    M-->>SN: RESULT_ACK(result_sequence)
~~~

### 5.1 启动阶段

1. 主机确认节点能力缓存仍有效，检查节点在线、标定、速度档和存储状态。
2. 所有目标节点依次接收 PREPARE，每条必须得到 COMMAND_ACK 才继续。
3. 全部 PREPARE 成功后，主机依次发送 START。
4. 全部 START 成功后，主机依次发送首个 LEASE。
5. 所有首个 LEASE_ACK 到达后，主机才启动本地泵。

某个节点在任意步骤失败时，主机先停泵，再向已准备或已启动节点发送 CLOSE。不得只关闭失败节点后继续让其他节点出水，除非产品明确允许部分成功且已经完成水力验证。

### 5.2 运行阶段

从机只做三件事：开阀、脉冲计量、本地保护。它不发送周期 SUMMARY，不发送运行中 STATUS。

主机按单播地址轮询续租。推荐初始参数如下，最终值必须由实测 SAR 时延确定：

| 参数 | 初始建议 | 说明 |
| --- | ---: | --- |
| lease_ms | 12000 ms | 从机失去租约后的本地关阀期限 |
| 续租周期 | 4000 ms | 每个节点独立计时，按轮询发送 |
| LEASE_ACK 等待上限 | 8000 ms | 超时即停止本批次 |
| 同一目的地址分段并发 | 1 | 禁止同节点两条分段消息重叠 |
| 全局业务分段并发 | 1 起步 | 通过台架测试后再评估 2 |

当前现场测得 SAR p99 约 2543 ms、最大约 3232 ms。若真实测试仍接近该水平，5 节点在 4 秒续租周期下可能无法及时轮询完毕，不能直接宣称支持五路。应先修复传输队列和分段终态管理，再重新测量 p50/p95/p99/max。

### 5.3 结束阶段

从机达到目标脉冲后：

1. 进入静默/尾流结算。
2. 关闭阀门。
3. 保存终态。
4. 发送 RESULT，等待 RESULT_ACK。

主机收到所有目标节点的可信 RESULT 后停止本地泵并结束任务。任一节点出现 transport、no-flow、runtime、calibration 或协议故障时，默认停止整批并对其他节点发送 CLOSE。

## 6. 超时、重试和故障处理

### 6.1 传输队列

当前故障表明 LEASE_ACK 在队列等待约 800 ms 就过期，而真实分段发送可超过 2.5 s。新方案必须同时修改传输层参数和调度策略：

~~~text
critical_queue_ttl >= 2 * measured_sar_p99 + 1000 ms
lease_ms >= critical_queue_ttl + measured_sar_p99 + safety_margin
~~~

在没有新的实测数据前，建议暂用 critical_queue_ttl=8000 ms、lease_ms=12000 ms 做台架验证。这个临时参数不是发布结论；如果传输仍出现分段上下文耗尽或重传耗尽，应停机并锁存 Transport Fault。

### 6.2 重试规则

| 报文 | 最大重试 | 建议间隔 | 失败动作 |
| --- | ---: | --- | --- |
| PREPARE | 2 次 | 300、600 ms | 取消整批，关闭已准备节点 |
| START | 2 次 | 300、600 ms | 先停泵，再 CLOSE |
| LEASE | 1 次 | 不滑动原始期限 | 停泵、CLOSE、任务失败 |
| CLOSE | 3 次 | 300、600、1000 ms | 依赖从机本地租约，结果缺失记 UNKNOWN |
| RESULT | 3 次 | 500、1000、2000 ms | 主机可按需 RESULT_QUERY |
| RESULT_QUERY | 2 次 | 1000、2000 ms | 仍缺失则 UNKNOWN，禁止补抽 |

重试不能改变 JobKey、目标体积、速度、标定版本或任务序号。任何超时都不能把一次未确认的启动重发成新任务。

### 6.3 主机重启、从机重启和断电

- 主机重启：上次未完成任务标为 FAILED/UNKNOWN，不重发 START；恢复阶段只发 RESULT_QUERY。
- 从机重启：输出默认关闭；启动后只发送一次 HELLO，上报已保存终态；不能恢复旧任务出水。
- CLOSE 丢失：依赖从机 lease_ms 或 max_runtime_ms 自动关阀。
- 结果缺失：标记 UNKNOWN，不把脉冲为 0 当成 0 mL，也不自动补抽。
- Transport Fault：锁存业务发送器，清理旧任务授权；只有重启或明确诊断恢复后才允许新任务。
- 若从机先于主机启动，启动期 HELLO 的 SAR `ETIMEDOUT` 单独记数并释放已结束 ticket；健康状态保留，等待周期 HELLO 重试。该例外仅适用于 HELLO，PREPARE、START、LEASE、RESULT 等业务报文的 SAR 超时仍锁定发送器并走故障停机。

## 7. 多节点和并发策略

本方案中的“同时运行”与“同时发送”必须分开：

- 控制报文按节点串行发送，避免多个分段报文同时占用 SAR 上下文。
- 多个阀门可以在启动阶段依次打开，但不能承诺严格同步。
- 主机只有全局泵速和全局泵输出，五路水力平衡不能由 Mesh 协议解决。
- 当前生产 MaxConcurrent=1 保持不变。
- 2 路和 5 路只能在独立测试构建中逐级验证；不能把 999、32 或 5 直接当作生产配置。

建议启动顺序按节点地址升序，节点间至少保留 150 ms 调度间隔；如果前一条分段消息尚未完成，必须等待其完成或失败，不能只等待固定间隔。

### 7.1 APP/GATT 边界

- APP 的启动请求仍先由主机持久化并返回 `OK`；这个 `OK` 只表示任务被主机接受，不表示阀门已打开或已经出水。
- Mesh 运行期间不向 APP 推送实时 `FLOW`；APP 只在用户查询、任务结束或恢复查询时读取权威结果。
- APP 断开 GATT 不影响已经接受的任务；主机继续执行租约和终态处理。
- 用户停止请求先由主机停本地泵，再进入 Mesh `CLOSE` 流程；停止响应也不把缺失终态解释为零出水。

## 8. 实施状态和剩余差异

协议修订已提升到 3。主从固件代码现已改用 RESULT / RESULT_ACK、RESULT_QUERY、COMMAND_ACK 和单播 CLOSE；PREPARE 增加速度档、租约与 flags；正常出水期间不再发送 SUMMARY 或运行期 STATUS。CLOSE、租约和结果报文使用关键队列期限及单播调度。APP 的 GATT 请求/响应格式没有变化；APP 端验证仍需单独运行。

周期 HELLO 每 10 秒保留为在线心跳，因为主机的节点在线判定依赖近期 HELLO。HELLO 携带从机 build ID；能力缓存可跨任务复用，节点离线、build 变化或主机重启后必须重新探测。周期 HELLO 不是出水遥测，不会汇报流量。

当前仍有明确差异和发布阻断项：

1. 从机标定仍只有一个 `k`，速度档尚无独立标定表。速度字段目前校验是否属于白名单并参加 PREPARE 幂等比较，但不能据此声称三档流量已分别标定；在建立并验证 `(node, speed, k, version)` 映射前不得将非低档用于真实定量。
2. `MaxConcurrent=1` 生产配置保持不变。五节点 host 仿真通过不证明实际 RF 五路能力或水力平衡。
3. HELLO 增加到 13 字节后，主从操作表、长度校验和协议测试需共同保持一致；主机部署清单必须在从机 clean build 后更新到实际批准的 build ID，再构建主机。
4. 本地 host/SDK 测试与固件构建不能替代真实射频、阀门物理关闭和实水标定验收。

## 9. 实施和验收门槛

### 9.1 软件门槛

- [x] 协议修订号升至 3；PREPARE=35、HELLO=13、RESULT=27 字节，主从操作表已同步。
- [x] 主从运行逻辑删除 SUMMARY；一、二、五节点各 1000 批 host 仿真统计为 0 条 SUMMARY。
- [x] RESULT 终态约束、重复/迟到结果、丢失 ACK 后查询、重复 START 与 JobKey 由控制器和集成仿真覆盖。
- [x] 队列治理有 8 秒关键 TTL、优先级、队列满保护、SAR 结束/错误/丢完成锁定与恢复用例。
- [x] SDK 将 SAR 超时编码为独立事件；HELLO 的启动时序超时可恢复并计数，业务消息超时仍按故障锁定。
- [x] HELLO build ID 能力缓存覆盖同 build 复用、build 变化失效和主机重启重新探测。
- [ ] 继续收集完整真实传输日志并实测 p50/p95/p99/max；单节点测试样本和刷后被动采样见 9.5、9.6，刷后 COM3 采样有不完整日志且 SAR 样本不足，不能作为稳定时延验收。
- [x] 当前源码 Host tests 覆盖 3.2 秒 SAR、HELLO SAR 超时恢复、LeaseAck 丢失、Result/ResultAck 丢失、缺少 SAR 完成、人工停止及重复 JobKey；29 组主机测试、从机测试和 Mesh 多节点仿真通过。
- [x] 当前源码主从 clean 固件构建、APP 单测（26 项 Gradle 任务）、主从 host tests、SDK 补丁测试、日志工具测试和架构检查通过；本轮 Build ID 见 9.4。

### 9.2 射频和实水门槛

1. 单路至少 50 批，验证无误启动、无误关阀和终态完整率。
2. 两路至少 20 批，记录每个节点的启动偏差、租约峰值和结果延迟。
3. 五路至少 20 批，每路独立量杯或称重，记录流量偏差、压力下降和无流次数。
4. 在 900 ms、2.5 s、3.2 s 人工延迟下重复测试，不得出现旧任务复活或阀门无限打开。
5. 任一节点故障时，确认主泵先停、其他节点收到关闭、缺失终态被标记为 UNKNOWN。

发布条件不是“命令发送成功”，而是：无线报文完成、从机物理阀位关闭、脉冲终态可信、主泵停止状态和实水结果同时满足验收要求。

### 9.3 当前验收记录（2026-10-20）

以下是 `pump-sar-1` 基线留下的历史证据，不代表包含 `pump-sar-2` HELLO 超时恢复逻辑的当前源码已通过验收。本轮复验完成后应追加当前结果和新的固件 Build ID。

| 固件 | Build ID | RAM | Flash | 部署状态 |
| --- | --- | ---: | ---: | --- |
| 主机 `pump_master` | `d9685b7c` | 58,648 bytes | 1,003,804 bytes | 旧源码基线；不得刷写为本轮候选；`MaxConcurrent=1` |
| 从机 `valve_slave_c3` | `9e91cbbc` | 37,240 bytes | 972,704 bytes | 旧源码基线；不得刷写为本轮候选 |

上述历史构建与测试证据保存在主从仓库 `.pio/*-candidate-20261020.*`、从机 `.pio/host-candidate-20261020.log` 和 APP `implementation-tests.log`；它们只证明 `pump-sar-1` 基线软件构建和确定性 host 模型通过。不能据此确认当前源码构建通过，也不代表真实 BLE Mesh 调度、阀门机械动作或水路已验证。

发布阻断项仍包括：速度档独立流量标定；单路、两路、五路 RF/实水验收；记录拒绝率、阀门物理关闭时间、最终结果完整率和每路体积偏差；真实 8 小时台架运行及运行期 heap/stack 峰值测量。任一项未完成都不满足第 9 节发布门槛。当前产物只能标记为软件候选，不能标记为硬件验收通过。

### 9.4 当前固件部署与静态链路复核（2026-10-20）

| 设备 | Build ID | RAM | Flash | 镜像 SHA-256 | 刷写结果 |
| --- | --- | ---: | ---: | --- | --- |
| 主机 COM3 / ESP32-D0WD-V3 | `e96b1375` | 58,688 bytes | 1,003,976 bytes | `171E8E743F581E13EF96F42C912468E5A4A2304163FF1D52912F9CACD8E05B7D` | 只写应用分区 `0x20000`，esptool 哈希校验通过 |
| 从机 COM4 / ESP32-C3 | `dd63e1ad` | 37,272 bytes | 972,898 bytes | `882E7258B30F84CF172BFB7B9913203B9087AE71BBA462824D04082136ED89C4` | 只写应用分区 `0x20000`，esptool 哈希校验通过 |

两次刷写均保留 NVS、分区表和引导程序。刷写记录分别为 `.pio/diagnostics/flash-master-pump-sar-2-20261020.log` 和从机仓库 `.pio/diagnostics/flash-slave-pump-sar-2-20261020.log`。生产 `MaxConcurrent=1` 未变。

刷写后进行两次双端被动采样，时长分别 25 秒和 30 秒，均 `commands_sent=0`。两端 `MESH_STATE` 均报告 `healthy=1`、`in_flight=0`、`pending=0`、`restart_required=0`。从机样本有 3 个 SAR 终态，p50=166 ms、p95/p99/max=215 ms；没有新增故障。累计诊断中 `hello_timeout=1`、`sar_max_ms=3407` 是跨采样保留的历史峰值，不能视作本次短样本分布。主机短样本没有足够 SAR 终态用于百分位判断，诊断为 `faults=0`、`trace_lost=0`，最近 `sar_max_ms=772`。这里的 `ended` 只统计分片 SAR 终态，未分片 HELLO 不计入该计数。

日志分析器在两次采样中各发现一条 COM3 `mesh_tx` 行不完整；采集器本身没有串口打开错误，但主机传输样本不完整。采样期间没有发出 `START`、`LEASE` 或 `RESULT`，也没有出水任务；这只验证了空闲链路和队列状态，不能代替阀门动作或实水结果。

本轮只确认一台物理从机（COM4）在线，2 路/5 路能力仍未验证。用户已指定先做单节点低流量测试，但当前电脑没有 ADB 连接的 Android 手机，工作区也没有可用的手机 GATT 控制端；因此本轮尚未触发出水。下一步需由手机 APP 发起单节点低档任务并使用量杯记录实际体积，同时采集完整的双端命令、ACK、阀门关闭和结果日志。500 mL 允许误差尚未给定，不能据此作精度通过判定。

### 9.5 单节点受控低流量测试（2026-10-20）

用户确认 COM3/COM4 设备映射和台架状态，并指定先做单节点低流量测试。主机 COM3 Build ID `e96b1375`、从机 COM4 Build ID `dd63e1ad`；任务 `124:43`，目标 50 mL，低速档（PWM duty 50%、20 kHz），超时 30 秒，`MaxConcurrent=1`。双端原始采集为 `.pio/diagnostics/controlled-single-node-20261020-com3-com4.jsonl`。§9.4 所述“尚未触发出水”仅指此前的空闲链路采样；本节记录后续实际测试。

PREPARE、START、开阀状态和首个 `LEASE_ACK OK` 均已收到，Mesh 发送样本 `err=0`。从机开阀后未检测到流量脉冲，约 5 秒触发本地 `NO_FLOW` 保护并关阀，结果为 `pulses=0`、`ml=0`；因此未达到 50 mL 目标。日志能证明流量输入没有脉冲，但不能单独证明泵电机没有转，也不能区分 PWM 未输出、驱动/供电异常、阀门或水路问题、传感器或接线问题。没有量杯或称重读数，本次不评估计量精度。

从机本地关阀后，主机收到 `LEASE_ACK result=STALE`，并因原代码把所有非 `OK` 租约回复映射为 `TRANSPORT` 而记录为传输故障；此分类与本次 Mesh `healthy=1`、`faults=0`、`trace_lost=0` 和发送 `err=0` 不符。后续主机代码已将 `STALE` 分类为 `LEASE`，仍立即关闭主机泵、围栏任务并进入 `Draining`；其他非 `OK` 回复保持原分类。新增回归用例已验证停泵、围栏、进入 `Draining` 和 `LEASE` 原因；30 组主机核心测试及 Mesh 故障恢复/1、2、5 节点/8 小时仿真测试通过。修复后的主机固件已构建并刷写，详见 §9.6。

从日志计算的主机 11 个 SAR 样本 p50=242 ms、p99=777 ms；从机 14 个样本 p50=210 ms、p99=1281 ms，均未达到建议的 p99 ≤ 200 ms。5 个匹配成功的租约 ACK p50=259 ms、max=400 ms，建议的租约 ACK p99 ≤ 800 ms 达标。主机另有 1 个 `STALE` 回复；采集结束时有 1 个仅存续 3 ms 的 ResultAck SAR 尚未结束，不能据此判为回调丢失。

测试后只读复查显示主机泵输出关闭、`fault=NONE`、`pump_ready=1`，从机阀门命令关闭、互锁就绪；双方 Mesh `healthy=1`、`in_flight=0`、`pending=0`。本轮不再次启动水泵。后续再次实水前，先检查 GPIO25 的 20 kHz / 50% PWM、泵驱动供电和电流、阀门机械动作、水路灌注，以及流量传感器脉冲和接线。2 路/5 路、量杯计量、延迟故障和 8 小时运行验收仍未完成，不能宣告 §9 硬件验收通过。

### 9.6 分类修复后的主机固件部署与复核（2026-10-20）

针对 §9.5 中 `LEASE_ACK STALE` 被误记为 `TRANSPORT` 的问题，主机现将 `STALE` 映射为 `LEASE`。安全动作保持不变：立即停泵、围栏当前任务并进入 `Draining`。修复后主机 Build ID 为 `d43e0a09`；从机仍为 `dd63e1ad`。

| 固件 | Build ID | RAM | Flash | 镜像大小 | 镜像 SHA-256 |
| --- | --- | ---: | ---: | ---: | --- |
| 主机 `pump_master` | `d43e0a09` | 58,688 bytes | 1,003,988 bytes | 1,004,384 bytes | `6BEB6CB23D8FA53C5FB6C62CB7E939B5C60AC506A044F11171745BC24BB6CEF6` |

构建成功。主机只刷写应用分区 `0x20000`，esptool 哈希校验通过；NVS、分区表和引导程序未改动。记录见 `.pio/build-d43e0a09-20261020.log` 与 `.pio/diagnostics/flash-master-d43e0a09-20261020.log`。生产 `MaxConcurrent=1` 未变。

修复回归测试和 Mesh 故障恢复、1/2/5 节点及 8 小时仿真通过。主机测试运行器在本机无法找到 C++ 编译器，未能通过 `run_host_tests.py` 重新编译测试二进制；现有测试二进制运行通过 30 组核心测试，其时间戳与当前测试源码吻合。因此本项记录为已执行测试，但不把它表述为本轮重新编译验证。

刷后进行了约 18 秒的被动双端采样，没有发送任务命令。只读状态确认主机泵输出关闭、`fault=NONE`、`pump_ready=1`，从机阀门命令关闭且互锁就绪；双方 Mesh 均为 `healthy=1`、`in_flight=0`、`pending=0`。采样中 COM3 有不完整/乱码的 `mesh_tx` 行，主机 SAR 样本不足，从机只有 2 个 SAR 样本；这些数据不用于计算或验收 p99。采样记录见 `.pio/diagnostics/post-master-d43e0a09-idle-20261020.jsonl`。

修复后没有再次启动水泵。§9.5 的 `NO_FLOW` 仍未定位到具体硬件原因；再次实水前须先检查 GPIO25 PWM、泵驱动供电与电流、阀门机械动作、水路灌注，以及流量传感器脉冲和接线。单路 50 批、两路/五路、量杯精度、延迟故障和 8 小时运行门槛均未完成，当前不能宣告第 9 节硬件验收通过。

## 10. 结论

推荐的新 Mesh 业务链路为：

~~~text
能力缓存/按需探测
    -> PREPARE + COMMAND_ACK
    -> START + COMMAND_ACK
    -> 首次 LEASE + LEASE_ACK
    -> 本地泵运行，按节点续租
    -> RESULT + RESULT_ACK
    -> 异常时 CLOSE
~~~

它比当前方案减少了周期摘要、运行中状态快照和正常流程查询，降低了 Mesh 分段竞争；同时保留租约、本地超时、终态确认和故障关闭，避免为了减少报文而牺牲安全性。

但它不能绕过当前无线链路的根本限制。只要实测 SAR 延迟仍达到数秒，五路 BLE Mesh 就不能被视为已验证方案；必须先修复传输队列和分段生命周期，再按单路、两路、五路递进验收。
