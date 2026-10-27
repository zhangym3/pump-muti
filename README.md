# 水泵主节点工程（ESP32-S3 N16R8）

2026-10-26：主控恢复为 ESP32-S3-WROOM-1 N16R8（16MB Flash，PSRAM 不启用），保留近期 revision 6 协议、结果持久化和维护互锁。构建环境统一为 `pump_master`，默认业务并发为 1。水泵改选 **B24-12 / DC24V 四线无刷 PWM 泵**，图片已确认线色用途，电气接口参数待商家提供；固件暂沿用 GPIO4 / 20kHz / 65、80、100%，尚未验证适配新泵。日志和维护恢复为 USB-UART（UART0：TX=GPIO43，RX=GPIO44）。从机仍为 ESP32-C3。新泵接线见 [B24-12 水泵说明](docs/PUMP_B24_12_24V.md)，主控接线和构建见 [ESP32-S3 迁移说明](docs/ESP32_S3_MIGRATION.md)。

历史：2026-10-25 基础候选支持产品最多一主五从，同一 JobKey 多选、统一每路目标量、各路独立到量关阀；当时五路候选环境 `pump_master_five_candidate` 并发 5，该独立环境现已移除，当前 `pump_master` 默认并发仍为 1。主从协议同步为 revision 6 / RequiredFeatures `0x15FF`，必须配套升级。已完成的离线检查和未验收项见 [基础版本实施报告](docs/BASIC_FIVE_OFFLINE_ACCEPTANCE_20261025.md)。当时无实机条件，未烧录或操作泵阀，历史 `dist` 不是此次候选包。

历史：2026-10-23 曾迁移为 ESP32-C3 Super Mini；[ESP32-C3 迁移说明](docs/ESP32_C3_MIGRATION.md) 仅记录当时配置与验收，不适用于当前 S3 主控。

2026-10-21：配套 APP 1.2.2 支持「设备 → 删除 → 确认删除」单台从机，含离线删除、远程退网与重启恢复；需要更新主机固件。新版从机收到 Node Reset 后关闭输出并重启进入待组网。使用、协议和验收边界见 [单台删除从机](docs/SLAVE_REMOVAL.md)，本次未刷写或实机验证。

历史候选及当时验证结果见 [从机离线修复报告](docs/SLAVE_OFFLINE_FIX_20261019.md)：主机 `4c3a56d9`、从机 `80ba7f64`。故障后可在旧发送真实结束后恢复诊断通信，业务保持停机锁存；报告中的独立候选包保留历史状态。当前镜像目标和源码摘要以重新构建打包后的 `dist/manifest.json` 为准。

本工程只包含 ESP32-S3 主节点应用，唯一构建环境为 `pump_master`；独立诊断和五路候选环境已移除。Board 为 `esp32-s3-devkitc-1`，覆盖 Flash 为 16MB，DIO / 40MHz，不启用 PSRAM，使用 ESP-IDF 5.4 / PlatformIO espressif32@6.10.0。

主节点负责手机蓝牙配对和加密 GATT、水泵 PWM、Mesh 配网、远端节点注册和定量调度。默认要求 PIN 配对，安装方法见下方「手机连接」。保留与从节点通信所需的协议和状态结构；本地阀门控制、流量计中断、从节点命令和角色分支已移除。从节点由独立工程 `D:/pump/pump-muti-slave` 维护。

三工程历史交接入口为 [HANDOVER.md](HANDOVER.md)（2026-10-19），包含 Git 基线、逐阀水量、命令、NVS 升级/回退与实机验收。主控芯片、接线和烧录地址以本 README 及 S3 迁移说明为准。

## 构建、测试与打包

```powershell
Set-Location D:/pump-muti/pump-muti
pio run -e pump_master
python tools/run_host_tests.py --compiler .pio/host-tools/ziglang/zig.exe
python tools/package_firmware.py
```

若命令不在 PATH，使用 `$env:USERPROFILE/.platformio/penv/Scripts/platformio.exe` 及同目录的 `python.exe`。打包输出为 `dist/pump_master` 的三个镜像，校验信息见 `dist/manifest.json`。

| 镜像 | 写入偏移 |
| --- | --- |
| bootloader.bin | 0x0 |
| partitions.bin | 0x8000 |
| firmware.bin | 0x20000 |

NVS 位于 0x9000，长度 64KB，沿用原分区布局，4MB 以上 Flash 暂未分配。S3 同板升级保留 NVS；更换芯片时旧板 NVS 不会自动转移，见迁移说明。工程默认上传和串口监视端口为 `COM8`；端口变化时，上传使用 `pio run -e pump_master -t upload --upload-port COMx`，监视使用 `pio device monitor --port COMx` 指定实际端口。

## 主机接线与控制

当前选用 B24-12 / DC24V 四线无刷泵：红线接独立 24V 电源正极，黑线接负极；蓝线是内置驱动的 PWM 调速输入，黄线是转速反馈。ESP32 用 USB 或合规降压电源供电；不能从 3V3 经 MT3608 给新泵供电，也不能将 24V 接入 ESP32 引脚。

GPIO4 作为蓝线的 PWM 信号源；需先确认泵端输入电压、内部上拉和驱动形式，再决定能否直连或采用电平转换/开漏接口。非隔离接口需 ESP32 GND 与黑线共地。黄线当前绝缘悬空，未接入采样；转速反馈不能代替从节点流量计。详细参数确认项见 [B24-12 水泵说明](docs/PUMP_B24_12_24V.md)。

固件暂保留 20kHz、低/中/高 65/80/100%；关闭时输出 0%（GPIO 低）。这些是旧控制设置，不是新泵的已确认规格，尤其需要确认 0% 是否真正停泵。N16R8 的 GPIO35/36/37 为板载八线 PSRAM 保留，不接外设。参数入口为 `include/board_profile.h` 和 `include/pump_safety_controller.h`，主机当前不采集泵的流量、压力、缺水或转速反馈。

维护命令和日志使用开发板的 USB-UART 端口，UART0 GPIO43=TX、GPIO44=RX，115200。外接 3.3V USB-UART 时 TX/RX 交叉并共地。原生 USB GPIO19/20 未配置为维护入口，换板后核对实际 USB-UART 端口。

当前 `commissioned=true` 是既有软件就绪配置，不代表新泵已完成硬件适配。上电先将 PWM 输出置低并选择低档，冷却完成且其他条件满足后软件可执行定量，不再要求每次 USB `pump-reset`；输出低电平能否使新泵停止仍待确认。基础版禁止新启动独立试泵和交互式标定，保留旧会话查询/停止；`pump-reset` 是 USB 故障复位维护命令，不会恢复旧任务。

定量和标定任务先准备从机，再发送 START；从机将常闭电磁阀驱动 GPIO4 置高后回复 START_OK。主机收到本批所有仍需供水从机的有效开阀确认、匹配的新鲜租约确认，并且每路开阀确认已满 **300ms** 后，才启动水泵。等待期间继续续租；确认缺失、超时、取消或全部阀门关闭时不启泵。当前没有阀位传感器，START_OK 表示驱动输出已执行，300ms 为软件预留动作时间，实际开阀时间需按所用电磁阀验证。

主节点通过 Mesh Provisioner 扫描、配网、配置模型并接收 HELLO。存储兼容 32 槽，产品最多登记 5 台，不允许以静默分批替代同时出水。节点已入网不代表已启用执行输出。添加流程和接线见 [COMMISSIONING.md](docs/COMMISSIONING.md)。

2026-10-23 实机诊断已修复扫描窗口外候选丢弃，并增加配网失败临时条目清理；窗口缓存连续六轮可发现目标，但无线漏收未消失。当前现场从机已获地址 `0003`，模型配置仍超时，未 READY；已配网节点应使用配置重试，不能要求它继续出现在未配网候选中。固件版本、证据及验收边界见 [配网诊断](docs/PAIRING_DIAGNOSIS_20261023.md)。

主节点固件启动时会自动识别旧外部工具留下的 Mesh Node 角色，清除 Mesh 凭据后重启并初始化为 Provisioner；手机不提供也不需要“现场迁移”或迁移确认。重启后用 `mesh-state EPOCH REQUEST` 确认 `capable=1 phase=IDLE`，即可在 APP 扫描候选从节点并请求组网。APP 高级维护中的“仅清主机登记”（原“重置从机组网”）清除主机本地 Provisioner 节点表、应用登记和分档标定后重启；不保证旧从机已退网或可被“添加新设备”发现。手机组网控制使用加密 GATT，不要求手机连接 USB。

## 手机连接

默认关闭 `CONFIG_PUMP_DEV_NO_PIN`，扫描 `PUMP_MASTER_xxxx` 后要求经过 PIN 认证的加密配对。先通过可信 USB `pin NNNNNN` 保存安装人员设置的六位 PIN 并重启；未设置时手机控制服务关闭。禁止在日志中输出实际 PIN。Mesh 密钥、USB 维护权限和业务互锁保持原流程。

升级前若设备使用开发免 PIN 镜像，须核对有效 PIN 已保存。切换配对方式后如旧绑定失效，在手机上忘记这个 Master 后重新配对，不清空设备 NVS 或 Mesh 网络。`dist/manifest.json` 记录实际打包的配对模式；既有 dist 产物不会随源码配置自动更新，发布前须重新打包核验。

恢复功能的主机身份使用独立 P-256 密钥和 NVS journal。可信 USB 下、空闲且无待处理任务时执行 `master-identity-init CONFIRM`，已初始化时拒绝覆盖；`master-identity` 只导出公钥、指纹、网络标识和版本，`master-identity-prove <64位小写十六进制随机挑战>` 提供签名证明。私钥不导出、不打印，存储损坏不自动重建。APP 必须通过可信安装保存公钥，不能信任蓝牙自报指纹或第一个连接者。当前仅有身份与准备回执基础，尚未接入经过独立管理员认证的恢复协调器，`recovery_enabled=0`；PIN 配对成功不等于获得恢复管理权限。

USB 导出 `.pmmaster`、APP 导入确认和存储边界见 [主机身份安装](docs/RECOVERY_INSTALLATION.md)。清网前的意图 journal 已补齐基础代码及故障注入测试，尚未接入运行时，见 [清网意图记录](docs/RECOVERY_RESET_INTENT.md)。

| GATT 项目 | UUID |
| --- | --- |
| 服务 | A64D0001-2F9C-4B6E-8DB7-A570530E1000 |
| RX，写入 | A64D0002-2F9C-4B6E-8DB7-A570530E1000 |
| TX，读取/通知 | A64D0003-2F9C-4B6E-8DB7-A570530E1000 |

GATT=147 修复继续保留：`CONFIG_BT_ALARM_MAX_NUM=128`，Mesh 初始化后恢复本地 MTU=185。现场记录见 [GATT147_DIAGNOSIS.md](docs/GATT147_DIAGNOSIS.md)。USB 可先读 `hello`、`local`、`nodes`。

## 存储与交接

保留主机 ProfileId `0x504D0001`。当前 NVS format 3，可读取格式 1/2 并迁移，保留旧布局和逐客户端计划；维护锁独立存储并读回验证。升级前备份 NVS，降级需匹配固件与状态迁移。旧字段不能直接删除或重排。`ValveStatus` 是主机接收远端结果的数据类型，位于 `include/valve_status.h`。

基础版 APP 每路 50～2000mL、步进 50mL，统一每路目标，同任务自主同时供水，各路按自己的 K 和脉冲关阀。新启动单独设置、交互标定和试泵禁用，旧任务仍可查询/停止。默认禁本地按钮开阀；现场 USB 取得维护许可才允许按住开本路，主机泵始终禁止。维护退出须核对全部登记节点关闭与许可退役，不能靠超时解锁。从机入网仍需显式启用，GPIO6 外部联锁禁用。

最近主机清理验证见 [VALIDATION.md](docs/VALIDATION.md)，维护状态见 [HANDOVER.md](HANDOVER.md)，报文协议见 [PROTOCOL.md](docs/PROTOCOL.md)。旧源码、从节点文档和混合历史产物已移至 `D:/pump/pump-muti-archive/master-only-20261017`。
