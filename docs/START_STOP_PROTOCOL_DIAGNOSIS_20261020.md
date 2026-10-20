# 启动后立即停止：现场诊断（2026-10-20）

## 结论

本次复现任务 `141:62` 的直接原因已经确认：主机在启动前收到从机能力回复后，因从机 build `c0f6b78c` 不在批准名单内，主动以 `PROTOCOL` 终止任务。协议版本 `3` 和功能位 `003f` 均符合要求。任务没有进入开阀或供水阶段。

准备到触发停止仅 **190 ms**；准备到失败结算为 **1190 ms**。这是用户看到“启动后立马停止”的对应过程。这里采用同一主机日志的业务时间 `t=` 计算，未混用两块设备的启动时钟或串口接收时间。

本次问题发生在开阀和供水之前。零脉冲不能作为流量传感器故障的证据；本次采集也不能验收泵、阀门和传感器的实际工作能力。

## 现场身份和状态

| 项目 | 采集结果 |
|---|---|
| 复现时间 | 2026-10-20 19:56:47，Asia/Shanghai |
| 主机 | COM5，地址 `0001`，实际运行 build `eea832c3` |
| 从机 | COM6，地址 `0002`，实际运行 build `c0f6b78c` |
| 主机批准的从机 build | `dd63e1ad`、`7ba339a8` |
| 协议校验 | 收到 `protocol=3 features=003f`，与主从代码要求一致 |
| 主机本地状态 | `pump_command=0 fault=NONE pump_ready=1` |
| 从机终态 | `job=141:62 phase=FAULT reason=PROTOCOL valve_command=0 pulses=0 ml=0` |
| 从机联锁 | `INTERLOCK enabled=0 ready=1` |
| Mesh 状态 | 两端 `healthy=1 faults=0`；从机历史 `expired=1 hello_timeout=9` 在复现前后未增加 |

## 新任务的完整证据链

以下摘录来自第二轮双串口采集；`t` 为主机业务时钟，单位 ms。

```text
schedule: t=860784 job=141:62 phase=1 reason=NONE selected=00000001
cap_rx: t=860974 peer=0002 nonce=1366259795 protocol=3 features=003f build=c0f6b78c applied=1
schedule: t=860974 job=141:62 phase=4 reason=PROTOCOL failed=00000001
mesh_tx: t=860978 ticket=137 peer=0002 op=c3 event=0 job=141:62
FLOW addr=0002 job=141:62 pulses=0 ml=0 phase=FAULT terminal=1 reason=PROTOCOL
schedule: t=861974 job=141:62 phase=7 reason=PROTOCOL done=00000001 failed=00000001 unknown=00000000
```

- `phase=1` 是 Preparing，`phase=4` 是 Draining，`phase=7` 是 Failed。
- `op=c3` 是主机发送 Close。该任务没有 `Prepare`、`Start` 或泵输出开启的日志。
- `cap_rx applied=1` 表示该回复被处理；能力处理函数在拒绝并停止任务时同样返回 true，因此它不代表通过校验。
- APP 收到的 `reply=OK phase=1` 表示请求已受理。随后收到 `phase=4/7 reason=PROTOCOL`，表示任务执行失败；两者语义不同。
- `done=00000001` 表示该路已有终态结果；同时 `failed=00000001` 表示失败，不能将 done 理解为成功抽水。

## 对应代码路径

1. 主机 `include/mesh_deployment.h:9` 仅批准 `dd63e1ad` 和 `7ba339a8`。
2. 主机 `src/master_scheduler.cpp:208` 的 `capabilities()` 同时检查协议、功能位和批准 build。本次收到的 build 不通过 `approvedSlaveBuild()`，调用 `stop(Reason::Protocol, now)`。
3. 主机 `src/master_scheduler.cpp:112` 的 `stop()` 先停止本地泵，再围栏该任务、发送带 PROTOCOL 原因的 Close，并进入 Draining。
4. 从机 `src/application.cpp:129` 收到 Close 后调用 `valve_.close()`。
5. 从机 `src/flow_job_controller.cpp:97` 允许 Close 先于 Prepare 到达：保持关阀、保存取消围栏并记录 Fault。这解释了从机没有开阀、没有脉冲，却已经出现 PROTOCOL 终态。

## 为什么更新了两端固件仍会失败

主从 `tools/mesh_sdk_build.py:14` 都按输入文件的原始字节生成 build，包含完整的 `platformio.ini`、源码和构建脚本。串口配置及 CRLF/LF 差异也会改变 build，批准名单则需要另外维护。

现场只读审计已确认：

- 主机 Git HEAD 为 `96420a742aa462106498a8f6b1768c137f4bfd7e`；当前输入重算 build 为 `eea832c3`，与本地构建清单及设备 HELLO 一致。
- 从机 Git HEAD 为 `f3494d770c7243c1f71433edcc35f8c8c1f40cec`；当前输入重算 build 为 `c0f6b78c`，与本地构建清单及设备 HELLO 一致。
- 两工程当前文件与构建清单逐项 SHA-256 相符。与 Git HEAD 原始 blob 的差异中，`platformio.ini` 包含串口修改，其余差异均为换行差异。没有把串口配置回退，也没有改写用户源码。
- 用 HEAD blob 按当前规则计算，主机为 `4a565883`、从机为 `088f7f04`。这说明当前身份规则依赖本地文件表示；这些 ID 仅用于诊断，不是新的批准名单。

因此，两端都完成烧录不等于部署版本已匹配。实际从机 build 必须属于该主机制品批准的版本集合。

## 可实施的修复方向

1. **固定制品的最小恢复**：审核当前从机源码、SDK 补丁和构建制品，通过后把准确的 `c0f6b78c` 加入主机批准清单，重新构建主机。该办法仅适用于该固定从机制品，后续源码或配置变更仍需重新审核。
2. **稳定构建身份**：统一文本输入换行为 LF；从身份计算中排除 `upload_port`、`monitor_port` 等本机工具配置；仍纳入影响固件行为的源码、编译选项、板型、有效 SDK 配置和补丁。新规则应显式标识版本，并同步构建及打包验证逻辑。
3. **配套发布校验**：先构建并审核从机，再生成明确的主机批准清单并构建主机；打包时验证从机制品 ID 属于主机批准集合，不匹配就阻止发布。现有单元测试使用批准列表中的模拟 ID，不能替代这一配对检查。
4. **细化现场提示**：能力拒绝日志记录具体失败项、收到的 ID 和所需版本；APP 将版本不匹配与传输故障、无流量分别显示。保留协议、功能位和批准版本校验。

本轮完成诊断和证据保存，尚未实施以上固件修改或烧录。

## 修复后验收

- 同一源码和有效固件配置在 LF/CRLF、不同本机串口设置下生成相同身份；影响固件行为的输入变化仍改变身份。
- 打包检查能拒绝未批准的主从组合，同时保留协议和功能位拒绝用例。
- 两端 HELLO 与准备发布的构建清单一致，实际从机 ID 在实际主机制品批准集合中。
- 原操作复测进入 Preparing → Opening → Supplying，观察泵/阀门输出与 HALL 脉冲，任务最终以 TARGET 完成。
- 按此前使用的 500 mL 设置进行实测，并按项目约定的体积误差标准验收；本轮没有完成出水量验收。

## 原始证据位置

- 首轮状态采集：`D:\pump\pump-muti\.pio\diagnostics\start-stop-20261020-194940`。
- 完整复现采集：`D:\pump\pump-muti\.pio\diagnostics\start-stop-20261020-195530`，本地时间 19:55:27 至 19:58:27，包含 `COM5.log`、`COM6.log` 和 `capture.jsonl`；两端采集无串口错误。
- 构建身份审计：第二轮目录内 `build-audit.json`。
- 可复核的精简证据：同目录文档 `START_STOP_PROTOCOL_DIAGNOSIS_20261020.json`。

采集程序仅发送 USB 本地 `hello/local/nodes/hall` 查询，未发起抽水、复位或清除配置；此次启动由用户在 APP 中执行。采集已结束并释放 COM5、COM6。原始日志保存在本机忽略目录中。
