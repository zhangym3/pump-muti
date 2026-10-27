# ESP32-S3 N16R8 主控迁移

原迁移日期：2026-10-21；2026-10-26 从 C3 主控恢复本配置。从机仍为 ESP32-C3，保留当前 App、主从 revision 6 / RequiredFeatures `0x15FF`、结果持久化和维护互锁。随后整理构建配置，仅保留 `pump_master`；下文中的诊断和五路候选构建记录仅代表整理前的验证结果。

## 硬件与接线

| 项目 | 当前配置 |
| --- | --- |
| 主控 | ESP32-S3 N16R8（16MB Flash / 8MB 八线 PSRAM） |
| PlatformIO Board | esp32-s3-devkitc-1，覆盖 Flash 为 16MB |
| Flash | DIO，40MHz；沿用 4MB 范围内的原分区，剩余空间未分配 |
| PSRAM | 不启用；当前控制与蓝牙使用内部 RAM |
| 水泵 | B24-12 / DC24V，红黑供电、蓝 PWM、黄转速反馈；接口参数待确认 |
| 水泵 PWM | GPIO4，LEDC 低速通道；暂沿用 20kHz / 10bit，启动占空比 0，待新泵规格确认 |
| 速度档位 | 暂沿用低/中/高 65/80/100%；关闭命令为 0%，新泵停转行为未确认 |
| 维护串口 | UART0，115200；GPIO43=TX，GPIO44=RX |
| 默认业务并发 | 1；唯一构建环境为 `pump_master` |

从 C3 主控换回 S3 时，PWM 信号源仍为 GPIO4；若沿用更早 ESP32 的 GPIO25 接线，则需改为 GPIO4。新泵使用独立 24V 电源，蓝线须按商家提供的电气参数选择接口，不能凭线色认定可直连 3.3V GPIO；黄线当前不接入。非隔离接口需主控与泵共地，外部上下拉和极性按泵端接口确定。接线与待确认参数见 [B24-12 水泵说明](PUMP_B24_12_24V.md)。GPIO35/36/37 属于 N16R8 的板载八线 PSRAM 接口，即使软件未启用 PSRAM，也不作为外设信号使用。

串口需连接开发板的 USB-UART 端口或外接 3.3V USB-UART（TX/RX 交叉连接并共地）。原生 USB GPIO19/20 未配置为维护控制台。工程默认上传和串口监视端口为已确认的 `COM8`；端口变化时，上传使用 `--upload-port COMx`，监视使用 `--port COMx` 指定实际 USB-UART 端口。

## 构建与产物

```powershell
pio run -e pump_master
python tools/run_host_tests.py --compiler .pio/host-tools/ziglang/zig.exe
python tools/package_firmware.py
python tools/verify_architecture.py
```

打包产物位于 `dist/pump_master`，身份、偏移和 SHA256 见 `dist/manifest.json`：

| 镜像 | S3 地址 |
| --- | --- |
| bootloader.bin | 0x0 |
| partitions.bin | 0x8000 |
| firmware.bin | 0x20000 |

S3 镜像与旧 ESP32 镜像不能互换。打包工具会拒绝芯片目标不匹配的构建。蓝牙显式选择 BLE 4.2 legacy HCI，保留 Mesh Provisioner、加密手机 GATT、alarm 池 128 和原有控制时序。

## 2026-10-26 恢复 S3 的离线验证（历史构建）

- 正式 `pump_master` 和五路候选 `pump_master_five_candidate` 均构建通过；三个 discovery 诊断环境已重新生成 S3 配置并通过 `envdump`，未全量编译诊断固件。五个环境均为 16MB Flash、UART0、不启用 PSRAM。
- 主机端回归通过：33 个基础测试组及结果持久化、维护互锁、全部 31 种五节点选择、1/2/5 节点各 1000 批、模拟 8 小时等附加测试。角色边界检查通过。
- 已重新打包正式 S3 固件，校验镜像芯片 ID、DIO/40MHz/16MB、ELF 摘要、分区地址和 SHA256。正式构建 ID 为 `053a1462`，固件大小 1,045,680 字节，摘要见 `dist/manifest.json`。

根 CMake 仅构建主控及其声明依赖，并保留 PlatformIO 的语言探测组件，避开固定工具链在未使用的 S3 RGB LCD 驱动中的编译器内部崩溃。切换构建范围时已刷新旧链接脚本缓存。构建和回归日志保存在 `.pio/s3-return-build-final.log`、`.pio/s3-return-release-build.log`、`.pio/s3-return-diagnostics-envdump.log` 和 `.pio/s3-return-host.log`。

## 2026-10-26 开发阶段免 PIN 验证

当前正式包已更新为 `CONFIG_PUMP_DEV_NO_PIN=y`，使用 Just Works 加密配对，不要求配置或输入六位 PIN；系统绑定、加密 GATT 权限和业务互锁继续生效。已保存的 PIN 保留，恢复配置的方法见 README 的「手机连接」。

- 正式和五路候选均编译通过；三个 discovery 环境通过 `envdump`，未全量编译诊断固件。五个环境的实际生成配置均已核对：S3、16MB、UART0、关闭 PSRAM、开启免 PIN。
- 当前正式构建 ID 为 `3f101013`，`firmware.bin` 为 1,045,440 字节，SHA256 为 `a585500a12aa42d049684ac635a0dd7da02112427a7b03b870f1fbaa1f928f8b`；已验证镜像为 S3 / DIO / 40MHz / 16MB，ELF 摘要、分区地址、各镜像 SHA256 和免 PIN 日志字符串匹配。
- `dist/manifest.json` 记录 `developmentNoPin=true`、`phonePairing=just-works`、`phoneEncryptionRequired=true`；打包的三个镜像使用上表地址。角色边界检查通过，检查工具已指向当前 `pump-muti-app` 工程并拒绝缺失的 APP 源码目录。
- APP 配对状态机和控制逻辑保持原流程，仅更新两种配对模式的提示；Debug APK、137 项 JVM 单元测试、Lint 均通过，Lint 为 0 错误、20 警告。

正式构建日志为 `.pio/dev-no-pin-release-build.log`，五路候选构建结果见 `.pio/dev-no-pin-build-final.log`，诊断配置日志为 `.pio/dev-no-pin-diagnostics.log`，APP 结果见其 `docs/VALIDATION.md`。本轮未刷写设备、未安装 APK、未执行实机配对或泵阀测试。

## 换板状态

主控 ProfileId `0x504D0001` 和应用存储布局保持兼容，但旧芯片的 NVS、BLE 绑定、Mesh 凭据不会随更换硬件自动转移。新板需完成手机配对和从机网络登记；当前开发配置 `CONFIG_PUMP_DEV_NO_PIN=y` 使用加密 Just Works 配对，无需输入 PIN，恢复 PIN 模式后才需配置六位 PIN。旧从机如仍保留旧网络，需按现有受控流程恢复为可配网状态。

已有历史验收与候选包记录的是当时硬件和源码，不能作为当前 S3 revision 6 固件的实机验收。恢复配置需重新生成各环境的 `sdkconfig` 并重建固件，旧 C3 配置和本地产物保留在 `.pio/s3-return-backup-*`。实板仍需验证启动输出关闭、20kHz PWM、串口、手机配对与重连、Mesh 配网、定量任务和故障停机。
