# 从节点搜索排查与实板验证（2026-10-17）

## 名称与发现路径

COM6 的 ESP32-C3 从机身份标签示例为 **PUMP_SLAVE_ABCD**。文中设备标识已替换为非现场示例。

从机仅启用 PB-ADV，不开放手机业务 GATT、PB-GATT 或 GATT Proxy。PB-ADV 广播携带 Mesh UUID，不携带普通 GAP 名称，因此手机系统蓝牙列表搜不到 `PUMP_SLAVE_22F6` 属于预期行为。

使用路径：手机连接 `PUMP_MASTER_02CE` → “从机组网 / 添加设备” → 主机搜索 PB-ADV UUID → APP 显示 `PUMP_SLAVE_22F6 · 连接组网` → 用户点击后主机配网。APP 的名字由候选 UUID 第 8、9 字节生成，与从机固件一致。

## 阻断点与修复

1. **从机扫描初始化失败。** ESP-IDF 5.4 的 C3 默认启用 `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y`、关闭 BLE 4.2 功能，而当前 Bluedroid Mesh adapter 调用传统 LE 扫描接口。实板报 `CC evt: op=0x200b, status=0xc`、`starting scan failed`、`node provisioning enable failed`。延迟 750ms 和重试无效。将从机的 sdkconfig.defaults 与现有 sdkconfig.valve_slave_c3 同步改为 BLE 5.0 功能关闭、BLE 4.2 功能启用后，实板扫描与配网启用成功。`CONFIG_BLE_MESH_HCI_5_0` 保留原值，该选项控制 20ms 广播支持，并非本次修复的 Bluedroid 功能选项。没有修改 ESP-IDF 安装目录。
2. **主机残留未配网 Node 角色。** COM5 报 `bt_mesh_provisioner_enable, Mismatch role 1`，此时 `addr=0000 mesh=0`。旧判断只检查 `esp_ble_mesh_node_is_provisioned()`，漏掉“已保存 Node 角色但尚未配网”的状态。现在 Provisioner 启用回调返回 `-EIO`，且尚未配网、主地址为 0 时，通过 SDK 停用 Node 配网释放角色，等待完成回调后重试 Provisioner 一次。失败仍报告 FAILED，避免无限重试；已正常启用的 Provisioner 不走此恢复路径。此分支不擦除应用 NVS，不需要手机执行迁移。

移除了从机上无效的延迟重试任务；启动日志改为 `Mesh identity`，避免误认为从机有手机 GAP 广播名称。

## 已执行验证

- 主从 PlatformIO 构建成功，分别烧录到 COM5 / COM6，正常更新保留 NVS。
- 从机无延迟启动成功：`I (434) mesh: node provisioning enabled (PB-ADV)`；不再出现扫描初始化失败。`HELLO` 返回 VALVE_SLAVE，`LOCAL` 为 `valve_command=0`。
- 主机修复前：epoch=91、addr=0000、mesh=0、Mismatch role 1；烧录修复后：epoch=92、addr=0001、mesh=1、`phase=IDLE capable=1`。
- 主机 15 秒扫描结束：`phase=SCAN_DONE found=1`，返回下列候选，UUID 与从机日志完全一致。

```text
MESH epoch=92 req=6003 op=6002 scan=6002 phase=SCAN_DONE busy=0 capable=1 reason=NONE found=1 target=0000
CAND epoch=92 scan=6002 uuid=dddd504d00000000abcd020000000000
```

- 再次重启主机：epoch=93，直接恢复 `phase=IDLE capable=1`，没有 Mismatch role；重新扫描仍找到同一个 UUID。
- Android 名称映射单元测试已通过，`:app:assembleDebug` 成功，安装包在 `D:/pump/pumpmutiapp/app/build/outputs/apk/debug/app-debug.apk`。两处候选列表均显示 PUMP_SLAVE 名称。
- 两工程已重新运行 tools/package_firmware.py 更新 dist 镜像与 manifest。

本轮仅发送 hello、local、mesh-state、mesh-scan 并重启验证；没有执行 mesh-join、清空节点表或泵阀输出命令。从机保持 commissioned=false。手机实机安装新版 APK 后的显示、点击组网及后续水路验收尚未在本轮验证。

## 证据文件

- 从机 `.pio/ble-legacy-final-build.log`、`.pio/ble-legacy-final-upload.log`、`.pio/ble-legacy-final-boot.log`。
- 主机 `.pio/master-discovery-boot.log`（修复前）、`.pio/discovery-fix-build.log`、`.pio/discovery-fix-upload.log`、`.pio/discovery-fix-scan.log`、`.pio/discovery-fix-reboot.log`。
- APP `build/slave-discovery-apk.log`。
