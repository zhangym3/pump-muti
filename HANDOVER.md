# 主节点工程交接（2026-10-17）

## 最新补充：从机搜索实板排查

COM5 / COM6 已烧录本轮修复。C3 从机改用 Bluedroid BLE 4.2 功能配置后，PB-ADV 启用成功；主机补齐“残留 Node 角色但尚未配网”的自动恢复，启动和重启均为 `capable=1`，两次扫描均找到 `PUMP_SLAVE_22F6` 的 UUID。新版 APP APK 已生成，手机实际点击组网仍待验证。详细原因、变更及证据见 [SLAVE_DISCOVERY_DIAGNOSIS.md](docs/SLAVE_DISCOVERY_DIAGNOSIS.md)。下文“本次未烧录”等描述为先前工程清理阶段的记录。

## 当前范围

`D:/pump/pump-muti` 仅维护 ESP32 主节点，环境 `pump_master` / Board `esp32dev`。ESP32-C3 从节点独立工程为 `D:/pump/pump-muti-slave`，只构建 `valve_slave_c3`；从机端口为 COM6，已在从机 PlatformIO 设置 upload_port / monitor_port。

本次移除了本地 FlowJobController、ValveIO、流量计中断、阀门/联锁引脚、从节点维护命令和事件发送、双角色代码及 7 组从节点控制测试。主机保留 17 组泵控制、请求去重、调度和远端重启结果测试。远端状态独立到 `include/valve_status.h`，Mesh 协议、配网管理、节点注册和调度保留。

## 兼容性

- 主机 ProfileId 仍为 `0x504D0001`。NVS format 1、字段顺序和默认值保持不变；旧本地阀门字段作为保留区，不能直接删除或重排。
- ESP32 交叉编译比较清理前后的存储类型，大小、对齐、各字段偏移及大小一致；PersistentState=2040 字节、ValveStatus=32 字节。
- `protocol.h`、`protocol.cpp`、`control_request.h` 保持与清理前及独立从机工程一致。
- 保留手机六位 PIN / 加密 GATT、alarm 池 128 和 Mesh 初始化后 MTU=185 的修复。

## 现场已知状态

COM5 是此前连接的主节点。此前 GATT=147 原因为 OSI alarm 池耗尽引起主机重启；修复后用户已确认弹出 PIN 并连接成功，APP 记录 BONDED、加密 CCCD 成功和 MTU185。该结论来自此前现场验证，本次清理尚未烧录。

旧主机持久化的 Node 角色由新版 Master 固件自动处理：启动时清除旧 Mesh 凭据和角色，保留应用 NVS（PIN、泵设置、任务记录），然后自动重启并进入 Provisioner。手机不发送迁移命令，也不需要现场迁移按钮；重启后确认 `mesh-state` 为 `capable=1 phase=IDLE`，即可从手机添加从节点。本次代码验证仍未烧录或擦除现场设备。

主机 GPIO25 输出为泵 PWM：20kHz、50/75/100%，停止 0%；当前 commissioned=true。启动仍保持 BOOT_LOCK，主机无流量或运行传感器。软件输出状态不代表机械设备反馈。

## 构建与产物

操作命令见 README，验证结果见 `docs/VALIDATION.md`。本次没有烧录设备、擦除 NVS 或执行泵阀输出。后续部署前应核对芯片和端口，部署后复测手机 PIN/GATT、Mesh 添加节点与调度。

手机“从机组网”提供“添加设备”和“重置从机组网”。手机通过已配对加密 GATT 直接控制主机；重置需二次确认，主机逐个删除 Provisioner 节点表中的全部远端节点（包括外部工具遗留节点），清空 `PersistentState.nodes[]` 后自动重启；PIN、泵设置和任务记录保留。APP 收到 `OK mesh_reset_started` 显示处理中，收到 `OK mesh_reset rebooting` 后清空本地从机/候选状态，重连后即可重新添加。

## 工程外归档

`D:/pump/pump-muti-archive/master-only-20261017` 保存清理前 src/include/test/tools/docs/dist、配置和原 HANDOVER。原 backups 目录、历史混合日志和旧产物另存该目录的 `legacy` 子目录。ABI 检查脚本与对象文件也在归档中，可追溯清理前布局。
