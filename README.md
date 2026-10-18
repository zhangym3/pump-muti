# 水泵主节点工程（ESP32）

本工程 `D:/pump/pump-muti` 只包含经典 ESP32 主节点应用，唯一构建环境为 `pump_master`，Board 为 `esp32dev`（4MB Flash），使用 ESP-IDF 5.4 / PlatformIO espressif32@6.10.0。

主节点负责手机 PIN 配对和加密 GATT、水泵 PWM、Mesh 配网、远端节点注册和定量调度。保留与从节点通信所需的协议和状态结构；本地阀门控制、流量计中断、从节点命令和角色分支已移除。从节点由独立工程 `D:/pump/pump-muti-slave` 维护。

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

当前 `commissioned=true`，上电保持输出关闭、低档和 `BOOT_LOCK`，正式定量仍需 USB `pump-reset`。独立限时试泵不会自动开阀，结束后保留原来的定量锁定。

主节点通过 Mesh Provisioner 扫描、配网、配置模型并接收 HELLO，最多 32 个逻辑槽位，默认逐路调度。节点已入网不代表其已启用执行输出。添加流程和接线见 [COMMISSIONING.md](docs/COMMISSIONING.md)。

主节点固件启动时会自动识别旧外部工具留下的 Mesh Node 角色，清除 Mesh 凭据后重启并初始化为 Provisioner；手机不提供也不需要“现场迁移”或迁移确认。重启后确认 `mesh-state` 为 `capable=1 phase=IDLE`，即可在 APP 扫描候选从节点并请求组网。已组网从机可在 APP 点击“重置从机组网”，清除 Provisioner 节点表中的全部远端连接和应用登记后重启；完成后点击“添加设备”重新组网。整个组网、重置和添加流程通过手机加密 GATT 控制主机，不要求手机连接 USB。

## 手机连接

扫描 `PUMP_MASTER_xxxx`，首次连接按系统窗口输入设备已配置的六位 PIN。USB `pin NNNNNN` 保存 PIN 后需重启；不要在日常升级中清除绑定或网络。

| GATT 项目 | UUID |
| --- | --- |
| 服务 | A64D0001-2F9C-4B6E-8DB7-A570530E1000 |
| RX，写入 | A64D0002-2F9C-4B6E-8DB7-A570530E1000 |
| TX，读取/通知 | A64D0003-2F9C-4B6E-8DB7-A570530E1000 |

GATT=147 修复继续保留：`CONFIG_BT_ALARM_MAX_NUM=128`，Mesh 初始化后恢复本地 MTU=185。现场记录见 [GATT147_DIAGNOSIS.md](docs/GATT147_DIAGNOSIS.md)。USB 可先读 `hello`、`local`、`nodes`。

## 存储与交接

保留主机 ProfileId `0x504D0001` 和 NVS format 1 布局。`PersistentState` 中一组旧本地阀门字段只作为兼容保留，不再调用从节点控制逻辑。`ValveStatus` 是主机接收远端结果的数据类型，位于 `include/valve_status.h`。

最近主机清理验证见 [VALIDATION.md](docs/VALIDATION.md)，维护状态见 [HANDOVER.md](HANDOVER.md)，报文协议见 [PROTOCOL.md](docs/PROTOCOL.md)。旧源码、从节点文档和混合历史产物已移至 `D:/pump/pump-muti-archive/master-only-20261017`。
