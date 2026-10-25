# 水泵主节点工程（ESP32-C3 Super Mini）

2026-10-25 基础候选：产品最多一主五从，同一 JobKey 支持多选、统一每路目标量、各路独立到量关阀；五路候选环境 `pump_master_five_candidate` 并发 5，正式默认环境仍为 1。主从协议同步为 revision 6 / RequiredFeatures `0x15FF`，必须配套升级。已完成的离线检查和未验收项见 [基础版本实施报告](docs/BASIC_FIVE_OFFLINE_ACCEPTANCE_20261025.md)。本轮无实机条件，未烧录或操作泵阀，历史 `dist` 不是此次候选包。

2026-10-23：主控改为与从机同款 ESP32-C3 Super Mini（4MB Flash），水泵 PWM 保持 **GPIO4**，板载原生 USB 用于下载、日志和维护，bootloader 地址 **0x0**。见 [ESP32-C3 迁移说明](docs/ESP32_C3_MIGRATION.md)。历史报告中的 ESP32 / S3 产物不适用于本次 C3 主机。

2026-10-21：配套 APP 1.2.2 支持「设备 → 删除 → 确认删除」单台从机，含离线删除、远程退网与重启恢复；需要更新主机固件。新版从机收到 Node Reset 后关闭输出并重启进入待组网。使用、协议和验收边界见 [单台删除从机](docs/SLAVE_REMOVAL.md)，本次未刷写或实机验证。

历史候选及当时验证结果见 [从机离线修复报告](docs/SLAVE_OFFLINE_FIX_20261019.md)：主机 `4c3a56d9`、从机 `80ba7f64`。故障后可在旧发送真实结束后恢复诊断通信，业务保持停机锁存；固件未刷写，硬件验收待进行。报告中的独立候选包保留历史状态；当前 C3 `dist/pump_master` 已按本次源码重新生成，见 C3 迁移说明。

本工程 `D:/pump/pump-muti` 只包含 ESP32-C3 主节点应用，生产构建环境为 `pump_master`，台架五路候选为 `pump_master_five_candidate`。Board 为 `esp32-c3-devkitm-1`，使用 4MB Flash，DIO / 40MHz，不依赖 PSRAM，使用 ESP-IDF 5.4 / PlatformIO espressif32@6.10.0。

主节点负责手机 PIN 配对和加密 GATT、水泵 PWM、Mesh 配网、远端节点注册和定量调度。保留与从节点通信所需的协议和状态结构；本地阀门控制、流量计中断、从节点命令和角色分支已移除。从节点由独立工程 `D:/pump/pump-muti-slave` 维护。

三工程历史交接入口为 [HANDOVER.md](HANDOVER.md)（2026-10-19），包含 Git 基线、逐阀水量、命令、NVS 升级/回退与实机验收。主控芯片、接线和烧录地址以本 README 及 C3 迁移说明为准。

## 构建、测试与打包

```powershell
Set-Location D:/pump/pump-muti
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

NVS 位于 0x9000，长度 64KB。C3 同板升级保留 NVS；更换芯片时旧板 NVS 不会自动转移，见迁移说明。只有确认实板和端口后才手动执行 `pio run -e pump_master -t upload --upload-port COMx`。

## 主机接线与控制

GPIO4 接水泵驱动 PWM 输入，20kHz，低/中/高为 65/80/100%，停止为 0%。C3 没有 GPIO25/43/44，不沿用旧 ESP32 / S3 的这些接线。参数见 `include/board_profile.h`。信号为 3.3V，需匹配的驱动和外部下拉，频率与极性以实际驱动规格为准。主机不采集流量、压力、缺水或运行反馈。

维护命令和日志使用板载原生 USB Serial/JTAG（GPIO18/19），监视器配置 115200；GPIO18/19 不接其他外设。UART0 GPIO20/21 当前不作为维护入口。上传和监视端口按 `platformio.ini` 当前配置，换板后核对实际端口。

当前 `commissioned=true`，上电保持输出关闭、低档和无故障，冷却完成且其他条件满足后可执行定量，不再要求每次 USB `pump-reset`。基础版禁止新启动独立试泵和交互式标定，保留旧会话查询/停止；`pump-reset` 是 USB 故障复位维护命令，不会恢复旧任务。

定量和标定任务先准备从机，再发送 START；从机将常闭电磁阀驱动 GPIO4 置高后回复 START_OK。主机收到本批所有仍需供水从机的有效开阀确认、匹配的新鲜租约确认，并且每路开阀确认已满 **300ms** 后，才启动水泵。等待期间继续续租；确认缺失、超时、取消或全部阀门关闭时不启泵。当前没有阀位传感器，START_OK 表示驱动输出已执行，300ms 为软件预留动作时间，实际开阀时间需按所用电磁阀验证。

主节点通过 Mesh Provisioner 扫描、配网、配置模型并接收 HELLO。存储兼容 32 槽，产品最多登记 5 台，不允许以静默分批替代同时出水。节点已入网不代表已启用执行输出。添加流程和接线见 [COMMISSIONING.md](docs/COMMISSIONING.md)。

2026-10-23 实机诊断已修复扫描窗口外候选丢弃，并增加配网失败临时条目清理；窗口缓存连续六轮可发现目标，但无线漏收未消失。当前现场从机已获地址 `0003`，模型配置仍超时，未 READY；已配网节点应使用配置重试，不能要求它继续出现在未配网候选中。固件版本、证据及验收边界见 [配网诊断](docs/PAIRING_DIAGNOSIS_20261023.md)。

主节点固件启动时会自动识别旧外部工具留下的 Mesh Node 角色，清除 Mesh 凭据后重启并初始化为 Provisioner；手机不提供也不需要“现场迁移”或迁移确认。重启后用 `mesh-state EPOCH REQUEST` 确认 `capable=1 phase=IDLE`，即可在 APP 扫描候选从节点并请求组网。APP“重置从机组网”清除主机本地 Provisioner 节点表和应用登记后重启；当前未显式发送远程 Node Reset，不能保证旧从机自动恢复为可重新添加的未配网设备，恢复边界见交接文档。手机组网控制使用加密 GATT，不要求手机连接 USB。

## 手机连接

扫描 `PUMP_MASTER_xxxx`，首次连接按系统窗口输入设备已配置的六位 PIN。USB `pin NNNNNN` 保存 PIN 后需重启；不要在日常升级中清除绑定或网络。

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
