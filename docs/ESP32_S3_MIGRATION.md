# ESP32-S3 N16R8 主控迁移

日期：2026-10-21。仅主控工程迁移；从机仍为 ESP32-C3，App 和主从 V4 协议沿用当前版本。

## 硬件与接线

| 项目 | 当前配置 |
| --- | --- |
| 主控 | ESP32-S3 N16R8（16MB Flash / 8MB 八线 PSRAM） |
| PlatformIO Board | esp32-s3-devkitc-1，覆盖 Flash 为 16MB |
| Flash | DIO，40MHz；沿用 4MB 范围内的原分区，剩余空间未分配 |
| PSRAM | 不启用；当前控制与蓝牙使用内部 RAM |
| 水泵 PWM | GPIO4，LEDC 低速通道，20kHz / 10bit，启动占空比 0 |
| 维护串口 | UART0，115200；GPIO43=TX，GPIO44=RX |
| 默认业务并发 | 1；五路候选构建仍仅用于台架验证 |

将旧 GPIO25 的水泵驱动信号线改接 GPIO4，主控与驱动共地，保留 PWM 外部下拉。GPIO35/36/37 属于 N16R8 的板载八线 PSRAM 接口，即使软件未启用 PSRAM，也不作为外设信号使用。

串口需连接开发板的 USB-UART 端口或外接 3.3V USB-UART（TX/RX 交叉连接）。原生 USB GPIO19/20 未配置为维护控制台。上传和监视端口按 `platformio.ini` 当前设置，换板后核对实际枚举端口。

## 构建与产物

```powershell
pio run -e pump_master_five_candidate
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

## 换板状态

主控 ProfileId `0x504D0001` 和应用存储布局保持兼容，但旧芯片的 NVS、BLE 绑定、Mesh 凭据不会随更换硬件自动转移。新板需配置六位 PIN、完成手机配对和从机网络登记；旧从机如仍保留旧网络，需按现有受控流程恢复为可配网状态。

已有历史验收与候选包属于旧硬件。此迁移只完成软件构建和回归；实板仍需验证启动输出关闭、20kHz PWM、串口、手机配对与重连、Mesh 配网、定量任务和故障停机。
