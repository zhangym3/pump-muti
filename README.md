# 水泵主节点工程（ESP32-S3 N16R8）

2026-10-21：主控迁移至 ESP32-S3 N16R8，水泵 PWM 改为 **GPIO4**，bootloader 写入地址改为 **0x0**。接线和迁移说明见 [ESP32-S3 迁移说明](docs/ESP32_S3_MIGRATION.md)。以下历史候选报告记录旧 ESP32 产物。

2026-10-21：配套 APP 1.2.2 支持「设备 → 删除 → 确认删除」单台从机，含离线删除、远程退网与重启恢复；需要更新主机固件。新版从机收到 Node Reset 后关闭输出并重启进入待组网。使用、协议和验收边界见 [单台删除从机](docs/SLAVE_REMOVAL.md)，本次未刷写或实机验证。

最新候选及验证结果见 [从机离线修复报告](docs/SLAVE_OFFLINE_FIX_20261019.md)：主机 `4c3a56d9`、从机 `80ba7f64`。故障后可在旧发送真实结束后恢复诊断通信，业务保持停机锁存；固件未刷写，硬件验收待进行。使用报告中的独立候选包，传统 `dist/pump_master` 不包含此次修复。

本工程 `D:/pump/pump-muti` 只包含 ESP32-S3 主节点应用，生产构建环境为 `pump_master`，台架五路候选为 `pump_master_five_candidate`。Board 为 `esp32-s3-devkitc-1`，覆盖为 N16R8 的 16MB Flash，DIO / 40MHz，不依赖 PSRAM，使用 ESP-IDF 5.4 / PlatformIO espressif32@6.10.0。

主节点负责手机 PIN 配对和加密 GATT、水泵 PWM、Mesh 配网、远端节点注册和定量调度。保留与从节点通信所需的协议和状态结构；本地阀门控制、流量计中断、从节点命令和角色分支已移除。从节点由独立工程 `D:/pump/pump-muti-slave` 维护。

三工程历史交接入口为 [HANDOVER.md](HANDOVER.md)（2026-10-19），包含 Git 基线、逐阀水量、命令、NVS 升级/回退与实机验收。主控芯片、接线和烧录地址以本 README 及 S3 迁移说明为准。

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

NVS 位于 0x9000，长度 64KB。S3 同板升级保留 NVS；更换芯片时旧板 NVS 不会自动转移，见迁移说明。只有确认实板和端口后才手动执行 `pio run -e pump_master -t upload --upload-port COMx`。

## 主机接线与控制

GPIO4 接水泵驱动 PWM 输入，20kHz，低/中/高为 50/75/100%，停止为 0%。N16R8 的 GPIO35/36/37 连接板载八线 PSRAM，不能作为水泵 PWM；S3 没有 GPIO25。参数见 `include/board_profile.h`。信号为 3.3V，需匹配的驱动和外部下拉，频率与极性以实际驱动规格为准。主机不采集流量、压力、缺水或运行反馈。

维护命令和日志使用 UART0：GPIO43=TX、GPIO44=RX、115200。使用开发板的 USB-UART 接口；原生 USB（GPIO19/20）未配置为维护控制台。上传和监视端口按 `platformio.ini` 当前配置，换板后核对实际端口。

当前 `commissioned=true`，上电保持输出关闭、低档和无故障，冷却完成且其他条件满足后可执行定量，不再要求每次 USB `pump-reset`。独立限时试泵不会自动开阀；`pump-reset` 仍是 USB 故障复位维护命令，不会恢复旧任务。

主节点通过 Mesh Provisioner 扫描、配网、配置模型并接收 HELLO，最多 32 个逻辑槽位，默认逐路调度。节点已入网不代表其已启用执行输出。添加流程和接线见 [COMMISSIONING.md](docs/COMMISSIONING.md)。

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

保留主机 ProfileId `0x504D0001`。当前 NVS format 2 保留格式 1 完整布局并追加逐客户端水量计划，支持启动时导入旧记录；升级前备份 NVS，降级需匹配的旧固件与升级前备份。旧本地阀门字段仍作为兼容保留，不能直接删除或重排。`ValveStatus` 是主机接收远端结果的数据类型，位于 `include/valve_status.h`。

APP 新任务每路 50～2000mL、步进 50mL，支持统一和单独设置；主机通过 `individual=1` 声明逐阀计划能力。任务在主机完整受理后自主逐路执行。从机入网后还需显式启用，当前从机 GPIO6 外部联锁禁用。现有本地 dist 镜像早于最新源码，发布前必须重新构建打包，不能将历史验收视为最新实机验收。

最近主机清理验证见 [VALIDATION.md](docs/VALIDATION.md)，维护状态见 [HANDOVER.md](HANDOVER.md)，报文协议见 [PROTOCOL.md](docs/PROTOCOL.md)。旧源码、从节点文档和混合历史产物已移至 `D:/pump/pump-muti-archive/master-only-20261017`。
