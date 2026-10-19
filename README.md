# 水泵主节点工程（ESP32）

最新候选及验证结果见 [从机离线修复报告](docs/SLAVE_OFFLINE_FIX_20261019.md)：主机 `4c3a56d9`、从机 `80ba7f64`。故障后可在旧发送真实结束后恢复诊断通信，业务保持停机锁存；固件未刷写，硬件验收待进行。使用报告中的独立候选包，传统 `dist/pump_master` 不包含此次修复。

本工程 `D:/pump/pump-muti` 只包含经典 ESP32 主节点应用，唯一构建环境为 `pump_master`，Board 为 `esp32dev`（4MB Flash），使用 ESP-IDF 5.4 / PlatformIO espressif32@6.10.0。

主节点负责手机 PIN 配对和加密 GATT、水泵 PWM、Mesh 配网、远端节点注册和定量调度。保留与从节点通信所需的协议和状态结构；本地阀门控制、流量计中断、从节点命令和角色分支已移除。从节点由独立工程 `D:/pump/pump-muti-slave` 维护。

当前三工程交接入口为 [HANDOVER.md](HANDOVER.md)（2026-10-19），包含 Git 基线、逐阀水量、接线、命令、NVS 升级/回退、本机测试与待做实机验收。历史文档中的启动锁、存储格式和接线说明有遗留，以该文档及当前源码为准。

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
| bootloader.bin | 0x1000 |
| partitions.bin | 0x8000 |
| firmware.bin | 0x20000 |

NVS 位于 0x9000，长度 64KB。升级保留 NVS。只有确认实板和端口后才手动执行 `pio run -e pump_master -t upload --upload-port COMx`。

## 主机接线与控制

GPIO25 接水泵驱动 PWM 输入，20kHz，低/中/高为 50/75/100%，停止为 0%。参数见 `include/board_profile.h`。信号为 3.3V，需匹配的驱动和外部下拉，频率与极性以实际驱动规格为准。主机不采集流量、压力、缺水或运行反馈。

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
