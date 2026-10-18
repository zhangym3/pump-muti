# 主节点接线与启用

本说明适用于经典 ESP32 / ESP32-WROOM-32、4MB Flash 主节点，PlatformIO Board 为 `esp32dev`。按板上 GPIO 丝印定位，排针顺序以实板为准。

## 接线

| 接口 | 用途 |
| --- | --- |
| GPIO25 | 水泵驱动 PWM 输入，3.3V，高有效 |
| GND | 非隔离低压控制侧按驱动要求共地 |
| USB / 板卡规定电源端 | ESP32 电源 |

GPIO25 需外部下拉，复位期间保持关闭。GPIO 通过匹配的驱动控制负载，不能直接驱动泵或线圈。当前频率 20kHz，低/中/高占空比 50/75/100%，0% 停止。启用前核对驱动允许频率、输入极性、最低起转占空比和 0% 停泵行为。

主机没有流量、压力、缺水或运行反馈输入。无需为旧方案的输入脚做许可跳线。从节点的接线与标定请使用独立工程 `D:/pump/pump-muti-slave` 的文档。

## 泵参数

`include/board_profile.h` 的 pumpConfig() 当前 commissioned=true。默认 maxRunMs=240000、minOffMs=1000、authorizationMs=1000。上电选择低档、输出关闭，正式定量保持 BOOT_LOCK；USB `pump-reset` 解除锁定后仍需满足冷却时间。

手机限时试泵独立于定量任务，不自动开阀，不清除定量启动锁定；需要可用水路和符合规格的驱动。命令格式见 `PROTOCOL.md`。本次代码清理未进行任何实体输出测试。

## 手机配对

USB 115200 波特读取 `hello`、`local`。设备未设有效 PIN 时，通过 USB `pin 六位数字` 保存并重启。手机扫描 PUMP_MASTER_xxxx，输入设备 PIN，等待加密 GATT 通道就绪。已有主机升级保留 NVS 与绑定。

## 主机添加远端节点

1. 读取 `hello` 中的 EPOCH；请求编号 REQ 使用非零整数。节点加入期间主机必须空闲、存储正常且没有未知任务结果。
2. `mesh-state EPOCH REQ` 查询主机配网能力。若主机发现旧 Node 角色，会自动清除旧 Mesh 凭据并重启；手机等待重连，不发送迁移命令。重启后必须看到 `capable=1 phase=IDLE`。
3. `mesh-scan EPOCH REQ` 扫描；读取 CAND 中的 32 位十六进制 UUID。
4. `mesh-join EPOCH REQ UUID` 发起加入。主机通过 PB-ADV 配网，随后获取 Composition、添加 AppKey、绑定模型并设置群组订阅，等待远端 HELLO。
5. `inventory EPOCH REQ` 或 USB `nodes` 查看登记结果。节点 HELLO 的 K 和版本写入主机槽位；新节点必须属于本机 Provisioner 且主机空闲才能自动登记。失败可通过 `mesh-retry EPOCH REQ ADDRhex` 对已配网节点重试模型配置。

若要清空主机全部从机连接，在手机“从机组网”点击“重置从机组网”并确认。主机逐个删除 Provisioner 从机及应用层登记后自动重启；PIN、泵设置和任务记录保留。重连后点击“添加从机”重新扫描。等价维护命令为 `mesh-reset CONFIRM`。

最多 32 个逻辑槽位，默认一次一路。必要时 USB `node SLOT ADDRhex K VERSION` 维护注册表，但手工注册本身不完成 Mesh 配网，也不会自动启用远端输出。在线、commissioned 和系数版本均满足后才可进入定量调度。

先发 PREPARE，收到准备状态后发 START，取得所选节点开阀输出状态才启泵。主动停止和异常先断本地泵输出再发关闭；未知终态不自动补发出水任务。全部状态均为软件命令与远端报告，不能证明机械动作。
