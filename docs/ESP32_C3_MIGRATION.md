# ESP32-C3 主控迁移（2026-10-23）

主机与从机均使用 ESP32-C3 Super Mini，按 4MB Flash / DIO / 40MHz 构建。两者仍是独立固件：主机 `pump_master`，从机 `valve_slave_c3`，不能互刷。主机 Board 使用 PlatformIO 的 `esp32-c3-devkitm-1` 通用定义。芯片是单核 RISC-V，不使用 PSRAM。

## 主机接线

| 接口 | 接法 |
| --- | --- |
| GPIO4 | 水泵驱动的 3.3V 兼容 PWM 输入，驱动端加外部下拉 |
| GND | 与水泵驱动信号地共地，可用端子分线 |
| 板载 USB | 下载、日志、维护命令；GPIO18/19 为 USB 专用 |
| GPIO20/21 | UART0 当前未用于维护 |

泵使用匹配的外部功率驱动和电源；GPIO 不能直接供电给泵。PWM 20kHz，低/中/高 65/80/100%，停止 0%。主机没有霍尔输入或本地阀门。旧 GPIO25、GPIO43/44 不适用于 C3。

手机仍连接 `PUMP_MASTER_xxxx` 的 PIN 加密 GATT，由主机完成 Mesh Provisioner、32 个逻辑槽位登记和定量调度。生产默认并发 1；`pump_master_five_candidate` 仅是并发 5 的台架候选。保留阀门 START_OK、匹配的新鲜租约 ACK，以及每路确认至少 300ms 后启泵的条件。无实际阀位传感器。

USB 维护改用 USB Serial/JTAG 驱动。USB 回复等待限时 5ms，未连接电脑时继续处理手机通知；控制任务与控制台任务分离，单核任务亲和性兼容 C3。保留 Bluedroid legacy LE 配置、Mesh 缓冲与 128 个 OSI alarm。

## 构建与更换设备

```powershell
pio run -e pump_master
pio run -e pump_master_five_candidate
python tools/run_host_tests.py --compiler .pio/host-tools/ziglang/zig.exe
python tools/package_firmware.py
python tools/verify_architecture.py
```

主机镜像在 `dist/pump_master/`，清单 `dist/manifest.json`。bootloader=0x0，分区表=0x8000，应用=0x20000；NVS=0x9000，长度 0x10000；factory 分区至 4MB 边界。旧 S3 配置已在本机 `.pio/c3-migration-backup/` 备份，两个主机构建环境的 sdkconfig 从当前 defaults 重新生成。

更换实体主板不会自动迁移旧板 NVS、PIN、手机绑定和 Mesh 凭据。新板需配置六位 PIN 后重启、手机重新配对；已有从机若仍绑定旧 Mesh 网络，需按从机恢复说明退网后重新添加和启用。保持应用存储结构与 ProfileId 不变不等于凭据会自动复制。未执行刷写、擦除或泵阀输出命令。COM9 是原工程配置，换板需核对实际端口。

## 从机按住开阀

四脚轻触开关只取两组触点各一脚，接 GPIO3 与 GND，使用内部上拉。用万用表确认：未按下不通，按下才导通；不要取同组始终相通的两脚。霍尔 GND 和按键 GND 可以通过端子共用同一个板上 GND。

按住稳定 30ms 后开阀，松开在下一控制周期关阀；上电时按住不会开，需先松开再按。只控制本从机阀门，不向主机申请开泵。首次未启用、存储/传输故障、联锁不许可或远程任务忙时拒绝开阀。保留无流量保护（首次 5 秒、断流 1.5 秒）、单次最长 60 秒、关阀尾流结算；故障关阀后继续按住不会重开，需释放后再按。不改动已完成远程定量结果，也不把本地流量作为定量任务结果发送。

USB `local` 附带 `BUTTON` 状态；接线和详细行为见从机 `docs/PINOUT.md`。默认常闭、高有效驱动需与硬件一致。

## 验证记录

本机验证完成：

| 项目 | 结果 |
| --- | --- |
| `pump_master` | 构建通过；静态 RAM 54,376B，程序 1,128,072B |
| `pump_master_five_candidate` | 构建通过；静态 RAM 54,376B，程序 1,128,070B；仅台架候选 |
| `valve_slave_c3` | 构建通过；静态 RAM 38,048B，程序 980,144B |
| 主机 host tests | 33 组核心测试及 Mesh 集成回归通过，含 300ms 开阀等待、丢包、故障恢复和模拟 8h |
| 从机 host tests | 按键启动锁/消抖/释放、远程互斥、历史结果、无流/时限/尾流/故障测试通过 |
| 两端打包 | `dist/manifest.json` 及三镜像已更新，镜像头均为 ESP32-C3 / 4MB |
| 分区与清单 | 0x0/0x8000/0x20000、4MB 边界、SHA256 和源码摘要核对通过 |
| 角色边界 | `tools/verify_architecture.py` 通过；同芯片仍分主机/从机固件 |

打包仅纳入本构建环境的有效 sdkconfig 及 defaults，避免台架候选配置的生成时间影响生产包的新旧判定。编译日志位于各工程 `.pio/`。SDK 保留原有的聚合初始化等编译警告，本轮无编译错误。

编译通过只确认代码、链接、静态占用和镜像目标；静态 RAM 报告不包含所有运行时分配。C3 运行时堆、手机配对、Mesh 射频、阀门动作时间及真实水路仍需实机验证，历史 S3 验收不作为 C3 验收。
