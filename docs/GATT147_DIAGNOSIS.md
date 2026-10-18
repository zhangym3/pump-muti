# 手机连接主机 GATT=147 排查

2026-10-16，PUMP_MASTER_02CE（COM5），三星 SM-F9660 / Android 16。

## 已确认的根因

手机清除旧绑定后，APP 仍报告 GATT=147（0x93，连接超时）。同步串口捕获到：

```text
BT_OSI: osi_alarm_new alarm_cbs exhausted
assert failed: hash_map_set hash_map.c:129 (data != NULL)
rst:0xc (SW_CPU_RESET)
```

设备崩溃 ELF SHA256 为 `4f133c0213a30c4551dbb8c31da32224d5970022da201a1fc98f494d7e6a5d6b`，与现场日志一致。解码调用链为：

```text
bta_gatts_send_service_change_indication
  -> GATT_SendServiceChangeIndication -> gatt_send_srv_chg_ind
  -> GATTS_HandleValueIndication -> gatt_start_conf_timer
  -> btu_start_timer -> hash_map_set(NULL) -> assert
```

ESP-IDF 5.4 的 Mesh k_delayed_work_init 与 Bluedroid 共用 OSI alarm 池。当前配置仅有 50 个槽位，其中 Mesh 分段发送/接收在初始化时就占用 8+16 个；Proxy、Provisioning、存储、心跳及主机协议栈继续占用其余槽位。Mesh 适配层在手机连接时发送 Service Changed indication，申请确认定时器失败，随后 BTU 将空指针传给 hash_map_set，触发断言重启。手机端先看到链路建立，约 5 秒后 reason=0x08 断开，因此收不到连接成功及 PIN 配对流程。

## 修复

主机 sdkconfig.defaults 及 sdkconfig.pump_master 显式设置 CONFIG_BT_ALARM_MAX_NUM=128，为 Mesh、GATT 和 SMP 共存预留容量。新增槽位会增加堆内存占用。128 是本项目的容量余量，不代表任意 Mesh 并发下均已验证，也不能用来断言不存在长期资源泄漏。

保留 PIN 认证和 NVS，不修改全局 ESP-IDF 包，不擦除网络或绑定记录。修复前 ELF 及崩溃日志保存在 D:/pump/pump-muti-archive/master-only-20261017/legacy/pio/gatt147-before-fix/；现场完整串口日志在 D:/pump/pump-muti-archive/master-only-20261017/legacy/pio/gatt147-master-connect.log。

## 验证

- 当时 pump_master 编译成功（95.61 秒），生成的 sdkconfig.h 确认 CONFIG_BT_ALARM_MAX_NUM=128。构建日志：D:/pump/pump-muti-archive/master-only-20261017/legacy/pio/gatt147-fix-build.log。
- 已通过 COM5 在 0x20000 写入主机应用镜像，esptool 数据哈希验证通过；保留 NVS、原分区及引导程序。写入日志：D:/pump/pump-muti-archive/master-only-20261017/legacy/pio/gatt147-fix-flash.log。新 ELF SHA256：a7b8b1da6a1cc33ff8c606b17d672f8c2cc63ba9424df05c28aa155db0e6b60e。
- 更新后串口查询确认 epoch=59、pump_command=0、output=0、test_active=0、BOOT_LOCK。未执行泵阀输出命令。初次采集日志 D:/pump/pump-muti-archive/master-only-20261017/legacy/pio/gatt147-fixed-connect.log 在手机配对前已结束，不包含配对过程。
- 用户确认系统已弹 PIN，输入后连接成功。Android 日志（北京时间 21:32:42—21:32:52）确认：GATT=0/state=2、NONE→BONDING→BONDED、服务发现 GATT=0、加密 CCCD GATT=0、MTU=185/GATT=0、控制通道就绪。证据：Android 工程 build/gatt147-fixed-app.log。
- 配对后只读串口查询仍为 epoch=59，与更新后首次查询一致，本次连接未触发主机重启；pump_command=0、output=0、test_active=0、BOOT_LOCK。证据：D:/pump/pump-muti-archive/master-only-20261017/legacy/pio/gatt147-fixed-state.log（原始 UART 数据有重复片段，以末尾完整响应为准）。该结果验证本次首次正确 PIN 连接，不代表错 PIN、取消、反复重连、长期资源稳定性或实际泵阀已完成验收。
- 当时已打包并记录镜像与源配置 SHA256，只烧录主机。当前工程只生成主机三个镜像，旧产物已移至工程外归档。

## 独立问题

启动仍有 bt_mesh_provisioner_enable, Mismatch role 1，业务诊断为 LEGACY_NETWORK / ENABLE_FAILED。这是持久化 Node 角色与 Master Provisioner 不一致的问题，与上述定时器崩溃分开处理；新版固件启动时自动清除 Mesh 凭据和角色并重启，应用 NVS 仍保留，手机不需要发送迁移命令。
