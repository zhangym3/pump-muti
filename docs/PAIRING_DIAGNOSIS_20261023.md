# ESP32-C3 主从机配对失败：串口诊断记录

## 实机结论

2026-10-23 22:03、22:04（北京时间）同步采集主机 COM9、从机 COM6，波特率 115200。主机地址 `0001`、固件 `aa76efa7`，从机地址 `0002`、固件 `e54900b8`。主机已保存从机记录；从机已经获得 Mesh 地址，但尚未绑定业务模型 AppKey，因此显示 `mesh=0 commissioned=0`。

执行两轮 `mesh-retry 16 … 0002`，均停在配置第一步 Composition Data Get。第二轮对应日志：

```text
COM6 W (282848) BLE_MESH: Replay: src 0x0001 dst 0x0002 seq 0x000006
COM6 W (286857) BLE_MESH: Replay: src 0x0001 dst 0x0002 seq 0x000007
COM6 W (290876) BLE_MESH: Replay: src 0x0001 dst 0x0002 seq 0x000008
COM9 W (156966) mesh: management failed: CONFIG_TIMEOUT addr=0002
COM9 MESH epoch=16 … phase=FAILED busy=0 capable=1 reason=CONFIG_TIMEOUT … target=0002
```

已确认的直接原因是从机将主机配置请求判为重放并丢弃，随后主机等待配置应答超时。双方无线收发至少在主机到从机方向有效。

原始双端日志（含时间和下发命令）位于：

- `../.pio/pairing-20261023-220319.jsonl`
- `../.pio/pairing-20261023-220434.jsonl`

日志采集没有清除 NVS、重置网络或下发泵阀动作。接管串口前，切换 VS Code 终端时曾误触重跑上传任务，主机重新烧录当前固件并重启，已向用户说明。主机保留了地址 `0002` 的记录。该次重启可能触发或再次暴露序列号回退问题，不能据此认定它解释了此前所有配对失败。

## 源码缺陷与修复

有效配置为 `BLE_MESH_SETTINGS=y`、`SEQ_STORE_RATE=128`。SDK 的 `bt_mesh_provisioner_net_create()` 初始化时只调用 `bt_mesh_store_iv(true)`，没有保存初始序列号。`bt_mesh_next_seq()` 先递增，再调用 `bt_mesh_store_seq()`；旧逻辑只在递增后的序列号为 128 的倍数时保存。若在初始区间内重启，`seq_set()` 发现缺少 `mesh/seq` 便直接返回，继续从 0 发送，而对端的 RPL 仍保存此前收到的序列号。

这条路径可在原 SDK 函数的主机测试中复现，与实机 `seq=3..8` 被拒绝的现象吻合；没有直接读取设备 NVS 来确认 `mesh/seq` 是否确实缺失，因此将其作为有测试支持的底层原因，保留实机验证步骤。

主、从工程均升级项目本地固定哈希 SDK 补丁到 `pump-sar-3`：

- 首次分配序列号后也安排保存，后续维持每 128 个序列号一次的规则。
- 恢复已有 Mesh 角色但缺少序列号记录时，也跳过初始区间，当前配置下从 127 开始，首次发送安排保存 128。
- 保留原有重放检查，不清除从机 RPL。
- 主机配置日志增加步骤、操作码、重试次数和应答结果。

缺失记录的兼容处理仅覆盖旧版本初始区间未保存的情况，不能恢复任意丢失、损坏或被擦除的序列号历史。保存沿用 SDK 工作队列，测试假定已完成待处理写入，不验证 NVS 写失败或写入完成前突然断电。

## 验证及实机复测

`test/sdk_sequence_harness.c` 执行从 SDK 提取的真实 `seq_set`、`store_pending_seq`、`bt_mesh_store_seq`、`bt_mesh_next_seq` 函数，NVS 与工作队列使用桩。旧补丁下测试在重启后序列号单调性断言失败；新补丁通过初始区间、跨区间、缺少记录、300 次重启及写入频率检查。原有 SDK 发送生命周期与完整组件哈希检查也通过。

复现与验证命令（在主机工程目录执行）：

```powershell
& C:/Users/79622/.platformio/penv/Scripts/python.exe tools/run_sdk_tests.py --framework C:/Users/79622/.platformio/packages/framework-espidf --compiler .pio/host-tools/ziglang/zig.exe
```

本轮主机固件 Build ID 为 `846b086e`，从机为 `c972c64b`。两工程生成的完整 SDK 输出 manifest 一致。主机控制与 Mesh 仿真测试、从机控制与按键测试通过；这些测试不代替无线或带水验收。

本轮仅生成修复固件，不再上传设备。实机验收仍需烧录后，在保留 NVS 的条件下重试地址 `0002` 的配置，确认依次完成 Composition Get、AppKey Add、Model Bind、Subscription Add，并收到 Hello，最终 `mesh=1 commissioned=1`；随后重启主机并再次通信，确认无 Replay。完成前不标记配对已修好。

## 后续：待配网从机无法被搜索到

22:20 起实机已运行上述新固件，主机 Build ID `846b086e`，从机 `c972c64b`。此时从机地址为 `0000`、`mesh=0 commissioned=0`；主机库存为空。当前故障发生在发现阶段，不能用上一节的配置请求 Replay 超时解释。

电脑蓝牙适配器通过 Windows 被动广播监听，在 22:24 收到从机的 AD `0x2B`、类型 `0x00` 待配网信标，UUID 为 `DDDD504D1463939222F6020000000000`，观测 RSSI 为 -53、-53、-58 dBm。前四字节和第 10 字节均满足主机过滤条件。这证明从机当时发出了正确格式的广播；电脑的 RSSI 不能直接代替主机接收强度。

用户明确断开手机蓝牙后，22:25 通过串口成功启动主机独立扫描，完成 15 秒窗口仍 `found=0`。用户随后短按主机 RST；22:30 查询主机 epoch 为 22，再次独立扫描完成仍 `found=0`。启动采集文件为空，未成功截获复位启动日志，不据此推断底层扫描启动成功或失败。现有 `mesh-diag` 的 `healthy=1` 仅反映发送治理队列，不能证明无线扫描有效。

对应原始记录：

- `.pio/radio-discovery-20261023-222407.jsonl`：电脑收到待配网广播。
- `.pio/discovery-20261023-222541.jsonl`：手机断开后的串口扫描。
- `.pio/discovery-20261023-223017.jsonl`：主机复位后的串口扫描。

目前可将问题范围缩到主机接收与过滤链路，尚未确认是控制器扫描、SDK 解析还是应用过滤导致。SDK 在启用 Provisioner 时本身会为 Mesh Beacon 添加重复过滤例外，不能仅凭启用重复过滤就认定缓存是原因。

### 主机诊断固件

新增独立构建环境 `pump_master_discovery_diag`，只为定位问题使用，不替换 `dist/pump_master` 的固件。它编译 SDK DEBUG 日志，仅在主动扫描的 15 秒内打开 `BLE_MESH` 标签 DEBUG；扫描结束、失败或开始配网前恢复原日志级别。SDK 补丁、UUID 过滤规则、候选选择和配网流程保持原行为。

普通主机环境也增加只读发现计数，执行 `mesh-diag` 或扫描开始/结束时通过串口打印 `MESH_DISCOVERY`：

- `received/matched/rejected/dropped`：SDK 发现回调数、符合应用条件数、应用过滤拒绝数、事件入队失败数。
- `outside_scan/duplicate/known/full`：扫描窗口外收到、重复候选、SDK 已配网表命中、候选表满。
- `uuid_filter_error/enable_error`：对应异步完成事件的结果；`-2147483648` 表示尚未收到完成事件。
- `sdk_nodes`：SDK 的全部远端节点数；`trace_build=1` 表示诊断构建。

计数从启动累计，判断一次扫描应比较开始和结束的差值。SDK DEBUG 的接收数据和拒绝原因用于进一步定位 `received=0` 的情况；采集内容可能包含附近设备广播，应保留在本地诊断文件。构建身份已纳入 PlatformIO 环境名，便于区分普通和诊断镜像。

经用户明确授权，已将诊断固件 `b2b0c24e` 烧录 COM9，保留 NVS。第一次上传发生串口异常中断，重试完整写入并校验成功；后续串口 Hello 确认了 Build ID，主机正常运行。应用分区写入地址为 `0x20000`，未擦除 NVS。未重置组网或下发泵阀动作。

### 发现链路的实机结果

22:38:53–22:39:08 的 15 秒扫描首次捕获目标原始广播，SDK 上报未配网设备，应用 `received=1 matched=1 found=1`。这说明 UUID 和候选处理链路曾完整生效，仅增加诊断不能算作修复。

22:39:45–22:40:00 再次扫描却 `found=0`：主机收到其他 Mesh 广播，没有收到目标原始广播；同期电脑收到目标信标，RSSI -50/-52 dBm。这一轮 `rejected=0 dropped=0 sdk_nodes=0`，不是应用 UUID 拒绝、队列溢出或已配网记录造成。

用户将两板放置约 50 厘米后，22:41:18 左右再次扫描，计数维持 `received=6 matched=6 outside_scan=5 found=0`，电脑同期仍收到目标广播（-53 至 -59 dBm）。累计 5 次合法发现发生在扫描窗口外，被当前应用设计丢弃；这是可改善的发现窗口问题，但不能据此解释主机底层漏收。

原始记录：

- `.pio/discovery-trace-20261023-223851.jsonl`：首次发现成功。
- `.pio/discovery-trace-20261023-223943.jsonl`、`.pio/radio-discovery-20261023-223943.jsonl`：随后漏收。
- `.pio/discovery-trace-20261023-224116.jsonl`、`.pio/radio-discovery-20261023-224117.jsonl`：50 厘米摆放后仍漏收。

### 手机连接广播共存对照

诊断构建 `54448547` 增加仅 USB 可用的 `discovery-phone-adv on|off`，仅控制手机连接广播，不影响 Mesh 扫描，不持久化。生产构建不编译此命令；已连接手机或管理流程忙时拒绝操作。停止和启动结果另有异步日志，不将请求受理当作停止成功。

同一次启动（epoch 24）进行 on/off/off/on 四轮，每轮 15 秒；暂停时实际收到 `BLE advertising stopped ... err_code=0`，恢复时收到 active 日志：

| 北京时间扫描开始 | 手机连接广播 | 主机原始广播报告数 | 主机目标报告数 | 电脑目标报告数 | found |
| --- | --- | ---: | ---: | ---: | ---: |
| 22:46:58 | 开启 | 33 | 0 | 1 | 0 |
| 22:47:18 | 暂停 | 9 | 0 | 0 | 0 |
| 22:47:37 | 暂停 | 54 | 0 | 3 | 0 |
| 22:47:57 | 开启 | 56 | 0 | 2 | 0 |

该对照未显示暂停手机连接广播可恢复发现，不据此认定两种广播共存是根因。结束已恢复手机连接广播。记录为 `.pio/discovery-ab-20261023-224654.jsonl` 和 `.pio/radio-discovery-20261023-224654.jsonl`。

静态检查另发现：ESP32-C3 的扫描重复过滤为 `BT_CTRL_SCAN_DUPL_TYPE_DEVICE`，而 SDK 的 `BLE_MESH_USE_DUPLICATE_SCAN` 帮助要求 `BT_CTRL_SCAN_DUPL_TYPE_DATA_DEVICE`。SDK 已为 Mesh Beacon 设置例外，因此配置差异暂不能直接认定为本次漏收根因。下一步使用独立 `pump_master_discovery_no_dup` 构建，仅关闭 Mesh 扫描去重作对照，不将其直接作为生产修复。

### 扫描去重关闭对照

`pump_master_discovery_no_dup` 构建 `4c86aef1` 完整烧录并校验，epoch 25；从机未更换固件、未重启。确认生成配置 `BLE_MESH_USE_DUPLICATE_SCAN` 为关闭。继续相同 on/off/off/on 四轮：

| 北京时间扫描开始 | 手机连接广播 | 主机原始报告数 | 目标有效上报增量 | 电脑目标报告数 | found |
| --- | --- | ---: | ---: | ---: | ---: |
| 22:51:11 | 开启 | 777 | 2 | 3 | 1 |
| 22:51:31 | 暂停 | 963 | 2 | 2 | 1 |
| 22:51:50 | 暂停 | 1037 | 2 | 3 | 1 |
| 22:52:10 | 开启 | 1128 | 0 | 3 | 0 |

前三轮接收改善，第四轮仍漏收，因此不能声称关闭去重就彻底解决；可能同时受无线漏收和应用窗口限制影响。总有效上报为 7，其中 1 次位于扫描窗口外。关闭去重大幅增加主机收到的普通广播，不宜未经验证直接用于生产。

日志：`.pio/discovery-ab-20261023-225107.jsonl`、`.pio/radio-discovery-20261023-225107.jsonl`。继续使用独立 `pump_master_discovery_data_device` 对 SDK 推荐去重模式进行测试。

### SDK 推荐去重模式对照

`6eeba70e`（epoch 26）启用 Mesh 去重，生成配置确认 `BT_CTRL_SCAN_DUPL_TYPE_DATA_DEVICE=y`、数值为 2，保持其他流程相同。四轮结果：

| 北京时间扫描开始 | 手机连接广播 | 主机原始报告数 | 目标有效上报增量 | 电脑目标报告数 | found |
| --- | --- | ---: | ---: | ---: | ---: |
| 22:54:27 | 开启 | 55 | 0 | 2 | 0 |
| 22:54:46 | 暂停 | 26 | 2 | 3 | 1 |
| 22:55:06 | 暂停 | 9 | 0 | 4 | 0 |
| 22:55:25 | 开启 | 8 | 1 | 2 | 1 |

推荐模式单独修改后仍有漏收，不能把原去重模式直接认定为唯一根因。记录：`.pio/discovery-ab-20261023-225423.jsonl`、`.pio/radio-discovery-20261023-225423.jsonl`。所有诊断广播暂停测试结束均恢复广播。

### 发现窗口修复

主机普通构建改为 SDK 要求的地址和内容去重模式，并在应用层保留最近发现：

- 只有已经过 SDK 与应用 UUID/角色过滤、且不在 SDK 已配网表中的候选进入缓存；不绕过过滤。
- 最多 8 台，按照最后实际收到广播的时间保留 30 秒。扫描开始时载入近期候选，然后继续原有 15 秒扫描。
- 重复搜索不刷新缓存年龄；同一 UUID 新广播会更新地址、地址类型和 OOB 信息。容量满时替换最久未见候选。
- 配网完成移除对应 UUID，重置组网清空缓存；每次载入再检查 SDK 已配网表。缓存仅在内存中，不写 NVS。
- `MESH_DISCOVERY seeded` 是累计从近期缓存载入的候选数；`outside_scan` 从此表示窗口外收到的事件数，不再表示必然丢弃。

新增主机测试覆盖窗口外发现、TTL 边界、重复搜索不续期、地址更新、满容量替换、失效移除、清空、输出容量和时钟回绕。完整控制与 Mesh 仿真测试通过，角色边界检查通过。此修复提高间歇接收情况下的发现率，不等于修复无线链路，也不保证接收不到广播时可以发现设备。普通固件仍需实机连续搜索验证。


### 普通固件连续搜索与握手验证

普通固件 `0488cc87`（epoch 27）六轮搜索均 found=1；后五轮 seeded，第五、六轮没有新广播，证明近期缓存有效，不能据此宣称无线漏收消失。记录 `.pio/discovery-cache-20261023-225917.jsonl`。

随后准确 UUID 配网：本地 link opened 后约 30 秒 reason=0x01 超时；从机仍 addr=0000 mesh=0 commissioned=0，无从机 link opened 事件，未进入 CONFIGURING。记录 `.pio/pairing-discovery-fix-20261023-230137.jsonl`。

将独立主机诊断扩展至握手（`2585419b`），记录 `.pio/pairing-handshake-trace-20261023-230551.jsonl` 和 `.pio/radio-pairing-20261023-230552.jsonl`。首个 link ID 的主机 Link Open 空中捕获 30 条，随后 Link Close；没有捕获从机 PB-ADV ACK，主机 SDK 无 link_ack/Invite。只能定位到从机接收/处理请求段，不能由未捕获直接证明从机没有发射。

从机通过 USB 仅重启，日志 `.pio/slave-reboot-20261023-230745.jsonl` 明确 node provisioning enabled 成功。未清 NVS、未更换原从机固件时仍不恢复。

发现第二个独立应用缺陷：START_PROV_NOW 队列条目在配网失败后未删除，后续信标触发 SDK 自动重连，应用却显示 SCAN_DONE found=0。实证第二个 link ID 在未发送新 mesh-join 时出现，记录 `.pio/pairing-handshake-trace-20261023-230802.jsonl`。主机现改为失败先进入 CANCELLING，异步删除该 UUID 的未配网队列条目，完成后再报告原失败原因；同步或异步清理失败保持 busy 和 RESTART_REQUIRED。

从机有效配置仍为 DEVICE 地址去重，违背 SDK 要求 DATA_DEVICE，已加入正确配置并准备实机验证。独立从机 pairing_diag 仅启动后 120 秒 DEBUG、BTC 栈 6144，生产不含运行时追踪。诊断构建也纳入环境身份。注意旧主机 discovery_diag 的生成 sdkconfig 会覆盖后来修改的 defaults，因此 `2585419b` 实际仍 DEVICE；对照引用各构建的实际有效配置，不能将 defaults 当作有效值。

### 从机握手恢复与剩余配置超时

23:18 的双串口复测使用主机诊断 `d539fc2b`（Mesh 去重关闭）及从机诊断 `a463257a`（DATA_DEVICE）。两板经 USB 重启，主机 epoch 33；本轮扫描 found=1，随后主从两端均出现 PB-ADV link opened。主机约 40 秒启动时刻报告 provisioning complete，目标获地址 `0003`，关闭原因 `0x00`。这证明本轮握手和凭据下发成功；同时更换了从机固件、重启两板，不能仅凭这一轮证明去重配置就是握手故障的唯一原因。

后续 Composition Data Get 三次请求均超时。日志明确显示从机收到 `0x8008`，发送 3 段回复并重传；主机第一轮只收到第 2 段，第二轮收到第 0、1 段，第三轮只收到第 2 段。没有拼成完整回复；从机收到对应部分 Seg ACK 后耗尽重传次数。现有证据是分段回复接收不完整，不能推断密钥错误、重放保护错误或硬件损坏。记录 `.pio/pairing-slave-trace-20261023-231816.jsonl`。该诊断原始文件包含 SDK DEBUG 数据，仅留本地，不直接转发；从机诊断代码现已改为 provisioning complete 时恢复 INFO，避免继续输出配置阶段的凭据日志。

### 普通固件恢复与配置重试

两板已恢复普通环境并完整烧录、校验：

| 设备 | 环境 | Build ID | 端口 |
| --- | --- | --- | --- |
| 主机 | pump_master | 56e9a678 | COM9 |
| 从机 | valve_slave_c3 | f0f1957d | COM6 |

两端普通有效配置都是 DATA_DEVICE，Mesh 去重启用，不含运行时 SDK DEBUG 追踪。`dist` 已分别重新打包，manifest 保持 `hardwareValidated=false`。主机构建/上传 92.03 秒，从机 110.37 秒；本轮从机控制器 host 测试通过，主机此前的控制/Mesh 测试及本轮角色边界检查通过。

普通固件下主机 epoch 34，通过已保存地址 `0003` 进行 3 轮 `mesh-retry`，每步最多 3 次 SDK 请求。前两轮 Composition 超时，第三轮在第 3 次请求收到完整 Composition；AppKey Add 第 2 次请求成功，Model App Bind 三次仍超时。普通固件也复现，因此不能将问题仅归因于 DEBUG 日志负担。记录 `.pio/pairing-config-retry-20261023-232221.jsonl`。

上述重试刚结束时，主机清单中该节点 slot=0、online=0。随后只读核对发现后台 HELLO 已到达过：主机为该节点分配 slot=1，从机 epoch 已同步为 34。说明应用消息至少曾通过；此前绑定请求可能已执行而响应丢失，不能把超时等同于从机未执行。管理流程失败后 step 已归零，迟到 HELLO 不会把该次操作改为 READY。只读记录 `.pio/final-pairing-status-20261023-232700.jsonl` 中仍为 phase=FAILED、reason=CONFIG_TIMEOUT、online=0；不能声明稳定在线或组网 READY。

当前从机 addr=0003、mesh=1、commissioned=0，主机清单 slot=1。该从机不再广播未配网信标，搜索候选中不出现符合当前状态；后续用 `mesh-retry 当前epoch 新request 0003` 恢复，不通过清 NVS 或再次 mesh-join 处理。没有启用从机或启动泵阀，主机 pump_command=0，从机 valve_command=0、phase=IDLE。

23:29 最后追加一次有限配置重试（request=980001），Composition 三次请求仍超时，管理状态再次 FAILED / CONFIG_TIMEOUT。结束时清单变为 slot=1、online=1、commissioned=0，主从输出仍为 0，从机 IDLE。记录 `.pio/pairing-final-retry-20261023-232920.jsonl`。这是最后一次状态快照；与前次 online=0 合看，只能确认通信间歇成功，不能把短暂在线当作配置完成或稳定性验收。串口采集已结束并关闭，不继续无限重试。

用户确认两板约 50 厘米、手机蓝牙断开、两板接电脑 USB，但外设仍连接，目前暂时无法断开。仅保留 USB 的隔离对照尚未执行，外设接线/供电干扰与板载无线链路因素仍待排除。未将任何一种硬件因素标为确定根因。

配网失败临时条目清理代码已构建，但本轮 PB-ADV 成功，未进入失败清理分支，故不能标为实机验收通过。旧 discovery_diag 的生成配置已同步改 DATA_DEVICE，历史 `2585419b` 仍按当时实际 DEVICE 记录；当前没有将该诊断镜像留在板上。

### 10 月 24 日现场复现：发现后配置响应不完整

被动串口跟踪期间，主机启动代数已变为 40，从机开机维护查询报告 `addr=0000 mesh=0`，与前一晚 `0003` 状态不同。主机扫描到一个候选；随后日志中出现请求 `605971167` 的 JOINING、目标地址 `0004` 和 CONFIGURING。这些状态由串口查询脚本之外的管理端操作产生；脚本只发送查询，没有发送 mesh-join、mesh-retry、清网或泵阀控制。初始 JOINING 时采集已开始，所以本轮日志没有完整保留 PB-ADV 开链事件，但进入目标 `0004` 的配置阶段说明 Provisioner 已报告配网完成。本轮新的从机地址为 `0004`，不要与前次地址 `0003` 混为一台持续在线的节点。

配置端按日志表现为：

| 阶段 | 现场记录 | 说明 |
| --- | --- | --- |
| Composition Data Get，opcode `0x008008` | 首个请求的三次状态等待均超时；随后两个自动/管理端重试请求也各超时 3 次 | 主机没有拼出有效完整的 Page 0 响应；期间主机报告 `Incomplete timer expired`，从机多次 `Ran out of retransmit attempts` |
| 一次 Composition 重试 | 请求 `605971182` 的第 1 次尝试收到 `error=0 valid=1` | 从机能力数据和预期 Vendor Model 可被正确解析，证明链路并非完全不通 |
| AppKey Add，opcode `0x000000` | 随后的 3 次等待均超时 | 已进入实际绑定 AppKey 的步骤，但没有收到状态回复；之后仍为 `CONFIG_TIMEOUT` |
| 最终设备状态 | 地址 `0004` 在主机清单 slot=0，`online=0 commissioned=0`；失败后 `phase=FAILED reason=CONFIG_TIMEOUT` | 不等于已 READY；没有启动泵阀 |

日志 `.pio/pairing-passive-follow-20261024-174722.jsonl`。主从 `MESH_STATE` 均显示 healthy=1、diagnostic_ready=0、restart_required=0、in_flight=0、pending=0，表明应用传输队列未堆积，但这不是 RF 链路质量测量。`Duplicate SDU from src 0x0004` 表示主机看到了重复网络 SDU；结合 `Incomplete timer expired` 和从机重传耗尽，最有力的定位是 Mesh 分段 PDU/ACK 未能稳定完整往返（可能丢段、丢 ACK，或接收端未能在计时窗内完成重组）。它不是“完全搜不到”或“完全无法建 PB-ADV 链路”。收到的 Composition 状态 `valid=1` 也排除了 UUID/模型不匹配作为这一次超时的直接原因。

AppKey Add 的结果是 timeout（`error=-1`），不是从机明确返回拒绝状态；没有证据表明两端 AppKey 不一致或权限参数错误。从机固件在配网完成后会降回 INFO，因此本轮不能从普通固件串口判定每个 AppKey 分段/状态 ACK具体在哪一跳丢失。此前诊断固件已经直接观察到从机发出三段 Composition 回复、主机只收到不完整段集且从机耗尽重传；这与本轮普通固件的超时现象一致。

`Data for unknown transaction (129 != 0)` 出现在进入配置之前，说明主机还收到不属于当前 PB-ADV 事务号的数据，可能是延迟/重复事务帧；现有日志不足以证明它导致后续配置超时。手机 BLE 加密连接日志发生在本轮 Mesh 配置失败之后，所以不能用它解释失败；外设仍连接，供电/接线影响尚未隔离。此前 USB 供电、50 厘米摆放条件下故障仍复现，尚未做拆除外设的对照，也没有功耗或空中抓包证据区分干扰、天线/供电和控制器接收问题。

界面持续显示“正在绑定密钥和配置 Mesh”是因为主机对每个配置请求等待约 4 秒、最多重试 3 次，状态一直停在 CONFIGURING；当一次 Composition 请求成功后立即进入 AppKey Add，但后者同样超时，最终才转为 FAILED/CONFIG_TIMEOUT。界面提示没有表示配网仍在 PB-ADV 阶段，也不表示密钥已被拒绝。当前应保留已保存节点并针对地址 `0004` 重试配置；反复重新搜索/重新组网不会补齐丢失的配置响应。

### 配置响应超时的更深层根因链

对照实际构建的 SDK 源码（`.pio/build/pump_master/build.ninja` 确认编译 `.mesh-sdk/bt/esp_ble_mesh/core/transport.c`，而非 `transport.enh.c`）后，可以进一步解释重试为什么会使问题恶化：

1. 一个分段配置响应丢失任意一段后，接收端会保留未完成的重组上下文。SDK `incomplete_timeout()` 的下限是 10 秒，另外按 TTL 和已收段数增加；这不是配置客户端 4 秒状态等待超时。
2. 主机配置客户端 `src/mesh_management.cpp` 对请求设置 `msg_timeout=4000`。收到超时回调后立即发送下一次请求，最多 3 次。10 月 23 日诊断日志的请求间隔约 4 秒，符合这条路径。
3. 从机对每个新查询会生成新的分段响应（新的 SeqAuth）。SDK `seg_rx_find()` 遇到同一来源/目的地址、旧上下文仍 `in_use`、但 SeqAuth 已变化时，打印 `Duplicate SDU from src ...`，清除旧的未完成重组并为新消息分配上下文。因此，主机 4 秒时就触发的新查询可能在前一响应的接收状态尚未到 10 秒清理期时打断它。

诊断双端记录为此机制提供了时序佐证：10 月 23 日主机请求 `0x008008` 后，配置等待约 4 秒即发下一次请求；其间主机只记录到响应的 `2/2`，随后在下一响应重传过程中出现 `Duplicate SDU`，再分批收到 `0/2` 和 `1/2`；从机日志同时显示主机的部分 ACK 到达、其余段继续重发，最后耗尽重传。10 月 24 日也重复出现约 4 秒的配置重试、`Incomplete timer expired`、`Duplicate SDU` 和从机重传耗尽。日志没有打印 SeqAuth/ACK 位图，无法逐包证明某一条 `Duplicate SDU` 对应哪个配置请求，因此这是一条有源码和时序支持的最强根因链，不是单凭串口日志即可证明的唯一空中故障原因。

更完整的因果判断是：底层首先存在不稳定的分段收发或 ACK 往返（已由双端分段日志直接观察到）；过短的 4 秒上层请求重试随后可能清除仍在等待补段的旧重组上下文，导致后续响应继续不完整，形成放大/重启循环。它解释了“偶尔一次 Composition 成功、随后 AppKey Add 又失败”和界面长时间停在配置阶段。它不能指出初始丢包究竟发生在发射、空中干扰、接收控制器还是 ACK 返回方向；外设未拆除、无 HCI/空中双端抓包，故 RF、供电、天线、USB 外设干扰仍未区分。也没有证据支持 AppKey 内容错误或 Composition 模型不匹配。

下一步验证应一次只改一个变量：先让配置请求在前一分段 SAR 窗口结束前不发起新的同地址请求（例如将客户端等待/重试间隔提高到覆盖至少 10 秒的不完整重组期，并记录每个请求/响应的 SeqAuth、段号及 ACK 位图），观察 `Duplicate SDU` 与完整状态响应是否消失。若仍失败，再单独做仅 USB 供电、拔除泵阀/传感外设的对照，以区分软件重试放大和硬件/RF 丢包。当前现场仍不具备拆外设条件。本日志在 17:48:40 记录到 `OK mesh_reset_started`、`mesh_reset rebooting`，随后主机重启；这是配置失败之后的外部管理动作，未参与此前故障形成。该记录来自管理端，不是被动采集脚本发出的查询命令。本轮未刷写固件。
