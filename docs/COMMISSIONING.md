# 主节点接线与启用

本说明适用于 ESP32-C3 Super Mini、4MB Flash 主节点，PlatformIO Board 为 `esp32-c3-devkitm-1`。按 GPIO 丝印定位，排针顺序以实板为准。2026-10-25 候选尚未实机验收。

## 接线

| 接口 | 用途 |
| --- | --- |
| GPIO4 | 水泵驱动 PWM 输入，3.3V，高有效 |
| GND | 非隔离低压控制侧按驱动要求共地 |
| USB / 板卡规定电源端 | ESP32 电源 |

GPIO4 需外部下拉，复位期间保持关闭。GPIO 通过匹配的驱动控制负载，不能直接驱动泵或线圈。当前频率 20kHz，低/中/高占空比 65/80/100%，0% 停止。启用前核对驱动允许频率、输入极性、最低起转占空比和 0% 停泵行为。GPIO18/19 保留给板载 USB。

主机没有流量、压力、缺水或运行反馈输入。无需为旧方案的输入脚做许可跳线。从节点的接线与标定请使用独立工程 `D:/pump/pump-muti-slave` 的文档。

## 泵参数

`include/board_profile.h` 的 pumpConfig() 当前 commissioned=true。默认 maxRunMs=240000、minOffMs=1000、authorizationMs=1000。上电低档、输出关闭，不强制 BOOT_LOCK；须状态就绪、旧任务已对账且全员关闭/退役证明完成才可显式启动。

基础版禁用新启动独立试泵、交互式标定和逐路不同目标；保留旧会话的查询/停止。命令格式见 `PROTOCOL.md`。本轮未进行实体输出测试。

## 手机配对

USB 115200 波特读取 `hello`、`local`。设备未设有效 PIN 时，通过 USB `pin 六位数字` 保存并重启。手机扫描 PUMP_MASTER_xxxx，输入设备 PIN，等待加密 GATT 通道就绪。已有主机升级保留 NVS 与绑定。

## 主机添加远端节点

1. 读取 `hello` 中的 EPOCH；请求编号 REQ 使用非零整数。节点加入期间主机必须空闲、存储正常且没有未知任务结果。
2. `mesh-state EPOCH REQ` 查询主机配网能力。若主机发现旧 Node 角色，会自动清除旧 Mesh 凭据并重启；手机等待重连，不发送迁移命令。重启后必须看到 `capable=1 phase=IDLE`。
3. `mesh-scan EPOCH REQ` 扫描；读取 CAND 中的 32 位十六进制 UUID。
4. `mesh-join EPOCH REQ UUID` 发起加入。主机通过 PB-ADV 配网，随后获取 Composition、添加 AppKey、绑定模型并设置群组订阅，等待远端 HELLO。
5. `inventory EPOCH REQ` 或 USB `nodes` 查看登记结果。节点 HELLO 的 K 和版本写入主机槽位；新节点必须属于本机 Provisioner 且主机空闲才能自动登记。失败可通过 `mesh-retry EPOCH REQ ADDRhex` 对已配网节点重试模型配置。

若要清空主机全部从机连接，在手机“从机组网”点击“重置从机组网”并确认。主机逐个删除 Provisioner 从机及应用层登记后自动重启；PIN、泵设置和任务记录保留。重连后点击“添加从机”重新扫描。等价维护命令为 `mesh-reset CONFIRM`。

存储兼容 32 槽，产品最多登记 5 台；默认环境并发 1，`pump_master_five_candidate` 并发 5。必要时 USB `node SLOT ADDRhex K VERSION` 维护注册表，但不能绕过容量、维护锁和结果屏障，也不完成配网或自动启用。在线、commissioned、V6 能力和系数版本均满足后才可定量。

本地按钮默认禁用。维护时保持泵关闭，在主机 USB 输入 `maintenance-enter ADDRhex CONFIRM`；全员关闭证明成功后保存维护锁，仅给目标从机本次启动的 60 秒许可。保持按钮释放后再按，只开本路，不启主机泵。结束输入 `maintenance-exit CONFIRM`，全员确认关闭与退役后才解除锁。`maintenance-status` 可查询；许可过期或手机重连均不会自动解锁，失联节点未确认时继续禁止新任务，不能删节点绕过。

连接配置提供 `MESH_BUDGET`：单回调 18 秒、单步骤 75 秒、配置总 330 秒、含配网总 420 秒。缺回调、队列丢事件或无法确定 SDK 上下文状态时进入 `RESTART_REQUIRED` 隔离；重复点击不能解除，重启后先查询清单和旧结果，不自动清网。

先发 PREPARE，收到准备状态后发 START，取得所选节点开阀输出状态才启泵。主动停止和异常先断本地泵输出再发关闭；未知终态不自动补发出水任务。全部状态均为软件命令与远端报告，不能证明机械动作。
