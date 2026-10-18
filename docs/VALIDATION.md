# 主节点清理验证（2026-10-17）

验证对象：D:/pump/pump-muti，仅 pump_master / esp32dev。清理移除本地从节点应用实现及其测试；保留主机的远端协议、配网管理、注册和调度。

| 检查 | 结果 |
| --- | --- |
| PlatformIO 构建 | `pump_master` / `esp32dev` SUCCESS；主机镜像可生成，目标芯片为 ESP32 |
| 主机 host 测试 | 18 组通过；涵盖泵输出/PWM/授权、限时试泵、去重与取消、32 节点逐路调度、消息乱序及远端重启未知量处理 |
| Master 自动接管旧 Mesh | `mesh_management.cpp` 启动路径自动清除旧 Node 凭据并重启；手机无需迁移命令，重启后应为 `capable=1 phase=IDLE` |
| Android 单测与协议 | `:app:testDebugUnitTest` SUCCESS；覆盖 CAND/UUID 校验、兼容 `LEGACY_NETWORK` 状态、扫描/加入阶段权限和无迁移提示 |
| Android 编译与 lint | 本轮仅执行单元测试；Android instrumentation/lint 需在完整 Android SDK 环境复核 |
| 独立从节点构建 | `valve_slave_c3` / `esp32-c3-devkitm-1` clean build SUCCESS；RAM 8.9%、Flash 24.9%，配置端口 COM6 |
| 从节点实现清理 | src/include/test/tools 无 FlowJobController、ValveIO、FlowReading、ValveConfig、从机引脚或 IsMaster 分支 |
| ELF 符号 | 无 FlowJobController 和 Hardware::valve |
| ESP32 存储 ABI | 与清理前类型比较：大小、对齐、各字段偏移及大小一致；PersistentState=2040、ValveStatus=32 字节 |
| 协议一致性 | 两工程 `protocol.h`、`protocol.cpp`、`control_request.h`、固定向量文档一致；主机 18 组、从机固定向量 host tests SUCCESS |
| 主机 sdkconfig | ESP32、Mesh Provisioner、BT_ALARM_MAX_NUM=128 保留 |
| 镜像 | 仅主机三个镜像；芯片头为 ESP32，偏移为 0x1000 / 0x8000 / 0x20000 |
| 实际分区表 | NVS=0x9000 / 64KB，应用=0x20000 |
| Manifest | 主机和从机均已 clean build，并分别运行 `tools/package_firmware.py`；role/chip/board、镜像路径和 SHA256 已更新 |
| 从机工程变化 | 从机 BLE 控制器枚举已修正为 `ESP_BT_MODE_BLE`；COM6 已设置为上传与监视端口，配置解析确认有效 |
| 实板 | 本次未烧录、未擦 NVS、未执行泵阀输出；需现场用 COM5 刷入新版 Master、用 COM6 从节点验证 Mesh 自动接管、重启恢复、手机添加设备和水路 |

当前打包固件位于主机 `dist/pump_master` 和从机 `D:/pump/pump-muti-slave/dist/valve_slave_c3`；manifest 分别记录目标芯片、偏移、镜像 SHA256 和源码配置哈希。

构建保留 ESP-IDF Mesh 模型宏产生的成员缺省初始化警告，以及从机流量计数器的 volatile 增量弃用警告；这些警告不影响本次成功链接，但不将构建成功解释为无警告。

主机测试入口为 tools/run_host_tests.py，编译器为 .pio/host-tools/ziglang/zig.exe。ABI 比较和综合核对脚本及清理前头文件位于工程外归档 D:/pump/pump-muti-archive/master-only-20261017。

## 第 7 项现场验收

以下步骤需要接入真实设备后执行；本轮只完成了代码、编译和协议验证，没有烧录或擦除 NVS。

1. 主机接 COM5，发送 `hello` 和 `mesh-state EPOCH REQ`。若检测到旧 Node 角色，Master 自动清理 Mesh 凭据并重启；手机和 USB 均不发送迁移命令。重启后必须是 `capable=1 phase=IDLE`。
2. 从节点接 COM6 并上电。手机只连接 `PUMP_MASTER`，点击“添加设备”，确认候选 UUID 来自主机的 `CAND` 响应。
3. 点击候选从机，确认主机依次完成配对、Composition/AppKey/模型绑定、C001 订阅和从机 HELLO，最终 `READY`，清单刷新出 slot/address。
4. 断电重启主从后再次刷新，确认 Mesh 和从机清单恢复；若主机仍有旧 Node 凭据，确认其自行重启并进入 `capable=1 phase=IDLE`。
5. 在确认水路安全、GPIO25 驱动和输出锁定后，再按现场 SOP 做单路低风险出水；软件状态不能替代机械流量验证。

## 从机组网重置验收

1. 手机已配对主机后，在“从机组网”点击“重置从机组网”，确认弹窗说明会保留 PIN、泵设置和任务记录。
2. 主机输出 `OK mesh_reset_started`，完成逐个删除后输出 `OK mesh_reset rebooting` 并重启；重连后 `inventory` 为空，`mesh-state` 为 `capable=1 phase=IDLE`。
3. 点击“添加从机”，确认 APP 只展示主节点扫描返回的 `CAND`；点击候选后 `MESH` 最终进入 `READY`，清单出现新地址。
4. 忙状态返回 `BUSY mesh_reset_requires_idle`，非法确认返回 `INVALID mesh_reset_confirmation`，两者均不清除数据。

此前 COM5 主机已验证 PIN 配对成功；刷入新版 Master 后，旧网络角色由启动流程自动清理。当前源码和从机镜像已重新编译打包；仍需现场在 COM5/COM6 烧录后执行双板和水路验收。
