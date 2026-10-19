# Mesh 定量抽水修复：实施与验收交接

日期：2026-10-19。对应[十轮复核实施方案](MESH_FIVE_CHANNEL_IMPLEMENTATION_PLAN_20261019.md)。

**结论：主机、从机和 APP 的软件改造已实现，双端干净构建及下述自动测试通过；尚未刷写本次候选固件，尚未完成射频、实水和真实 8 小时台架验收。生产 `MaxConcurrent=1` 保持。当前不能宣布“最多五路已验收”或“500 mL 精度已达标”。**

## 1. 问题与修复边界

历史现场证据确认 `116:26` 为 500 mL、`FAULT/LEASE`；完整抓取的 `116:27` 实际设置为 200 mL。两端分段发送资源耗尽、状态/授权未及时完成，是本次修复对象。未将所有历史失败归为同一原因，也未把设备上报 0 脉冲等同于量杯绝对无水。

本次没有修改 K=5880 pulses/L、从机 3000 ms 租约、1500 ms 无流保护或 5000 ms 首脉冲等待。500 mL 仍对应 2940 目标脉冲。改造使拥堵有界、授权可追溯、超时可安全停止；不能保证任意无线条件下继续完成出水。

## 2. 已实现的控制行为

| 部分 | 实现及作用 | 主要位置 |
|---|---|---|
| 真实发送终态 | tracked API 传递非零 ticket，区分 API 拒绝、核心接受/拒绝和真实 SAR end；旧 API 保留 ticket=0 语义 | 双端 `tools/mesh_sdk/`、`tools/prepare_mesh_sdk.py` |
| SDK 隔离与身份 | 项目私有 `.mesh-sdk/bt` 覆盖，核对整个 BT 组件 1893 文件及补丁身份；未改全局 SDK | 双端构建脚本、manifest |
| 有界发送 | 主机业务 SAR≤2、普通 SAR≤1，从机 SAR≤1，每目的地址 SAR≤1；BTC 未接受≤4；Pending=64，其中普通意图最多48，保留16个关键槽；Tickets=8 | 双端 `include/mesh_tx_governor.h` |
| 合并与失效 | 待发 Summary/同 revision Status/续租等合并；关键截止不因重复入队移动；关键过期、明确发送错误或缺失终态导致冻结 | 双端 governor/transport |
| 缺失回调 | 从提交起 6000 ms 未得到所需结果则冻结，保留占位，禁止靠计时假装释放；泵仍受更短授权期限保护 | governor、主机 scheduler |
| 单播续租 | 只为本批已确认开阀、仍需供水的节点约每秒单播；seq 在真实提交时分配；每节点保留最近3条提交账本 | 主机 scheduler、transport |
| 启泵门槛 | 当前 job 开阀状态、新鲜有效 LeaseAck、每节点 proof年龄≤1000 ms；首次 Start commit 启动固定2400 ms bootstrap，重试不延长 | `src/master_scheduler.cpp` |
| 运行授权 | 证明从本机 API 调用前 `issuedAt` 计时；`now-issuedAt >= 2400 ms` 先停泵，不按 ACK 收到时重新计时 | 主机 scheduler |
| 停止屏障 | 先本地停泵，锁内 commit 与 fence 排序；屏障确认后不再提交旧任务授权，已提交的仍跟踪 | 双端 transport/governor |
| 任务前检查 | 新增 D0/D1、任务 nonce、protocolRev=2、features=0x000F；主机严格拒绝不在部署清单内的从机构建 | `include/mesh_deployment.h`、双端 protocol/application |
| 遥测减流 | 正常 Hello 不再每次触发完整 Status 查询；首次发现/unknown/必要恢复保留查询 | 主机 application |
| 终态竞态 | 从机已因 Target 关阀时收到在途 Lease，重发真实状态，不续租、不虚构 OK、不以 STALE 覆盖正常结束 | 从机 application |
| 原因保留 | 主机首次停止原因独立于从机关闭原因，迟到 Target 不清除 abort；NVS format 3 持久化 reason，兼容 format1/2 | 主机 storage/scheduler、APP |
| APP 展示 | 兼容旧 R/FLOW；主机原因与从机原因分别显示；历史保存流量快照/原因，重启后可显示；unknown不因普通失败提前清空 | APP protocol/history/viewmodel/UI |

本地泵 authorization=1000 ms、renew=250 ms、控制周期5 ms。保护判定先于 renew。队列、API、SAR 终态均不等于对端开阀或有效授权。

停止语义允许屏障前已经 commit 的调用在屏障后进入 SDK；没有宣称停止按钮之后绝无迟发。无物理阀位反馈，Opening/Running 表示输出命令状态。

## 3. 构建与候选身份

| 项目 | Git 基线（均含本次未提交修改） | 构建 ID | 静态 RAM / Flash |
|---|---|---|---|
| 主机 ESP32 | `ad43f28bcef9ac4477d2c8428f000195e77d383c` | `e0950038` | 58192 / 1000340 bytes |
| 从机 ESP32-C3 | `56821a3fd071347f505d532620e7e4f628ebdbdf` | `495431ba` | 37032 / 969092 bytes |
| Android APP | `4c8ea4a4169dc9805bd27d141e95c96b9c15d16f` | debug、1.2.0 (8) | APK 见候选包 |

主机源码输入 SHA256：`e095003831e85231fdbf5a0d9a4225abce4412725d5bcb664d70c480d3c6f7fe`。

从机源码输入 SHA256：`495431ba17c34a2efac68ac8aebd234a1df80028e5c33c63e5b3647cc2d83053`。

SDK 补丁身份：`1b42ade610119dacd2fad891a49ec4c0d58b2286933f9967c31b5a958fa85467`。环境为 espressif32@6.10.0、framework-espidf 3.50400.0 / IDF5.4.0。

buildId 是源码/默认配置输入清单的32位索引，不是密码学认证，也没有把生成后的 effective sdkconfig 全部纳入该索引。候选包另保存 BIN/ELF 完整SHA256和 effective 配置，才可精确指认本次产物。构建 RAM 为静态链接量，不能代表运行时堆/栈余量。构建仍有 SDK 聚合初始化及既有 volatile 警告。

两端 ELF 均确认有强符号 `T esp_ble_mesh_tracked_send_event` 和 `T esp_ble_mesh_server_model_send_msg_tracked`；应用 BIN 内的 ELF SHA256 与对应 ELF 匹配。两端生成的二进制分区表及 PlatformIO `envdump` 一致：

| 文件 | 主机 ESP32 | 从机 ESP32-C3 |
|---|---:|---:|
| bootloader.bin | 0x1000 | 0x0 |
| partitions.bin | 0x8000 | 0x8000 |
| firmware.bin（factory） | **0x20000** | **0x20000** |

IDF 辅助 `flash_args` / `flasher_args.json` 仍以默认分区写着0x10000，不是 PlatformIO 实际上传配置，不可直接据此手动烧写。本包地址以 `candidate.json`、PlatformIO envdump 和已解析二进制分区表为准。

## 4. 已执行的软件验证

| 验证 | 结果与样本 | 证据/边界 |
|---|---|---|
| 双端干净构建 | PASS；主机100.79s，从机115.53s | `.pio/build-implementation.log`；未上传设备 |
| 主机控制/协议 | PASS，28组 | `.pio/host-implementation.log` |
| Governor边界 | PASS，回调乱序/重复/丢失、错误、窗口、合并、deadline、fence、容量 | 同上；纯状态机测试 |
| 多路模拟 | PASS，1/2/5节点各1000批；每路500mL=2940pulse；重复Start、丢EventAck | 同上；模拟IO与链路，无射频/实水 |
| 多路故障模拟 | PASS，5节点ACK丢失、900ms慢节点、缺失end、人工停止 | 本地停泵/远端保护逻辑；不能代表真实驱动时延 |
| 长稳模拟 | PASS，8小时虚拟时间，3340批，无队列/票据累积 | 不是8小时墙钟台架 |
| 授权与身份 | PASS，ACK900/1100/1800/2500ms、早于accept、核心拒绝、跨轮、重复、计时回绕、精确2400ms；nonce/错源/rev/features/未知构建/旧端超时 | 同上；测试白名单为本次从机构建 |
| 存储与结果 | PASS，NVS1/2→3，忽略旧padding，迟Target保留abort | 同上；未逐个NVS写入点真机断电 |
| 从机控制 | PASS | 从机 `.pio/host-implementation.log` |
| SDK提取桩执行 | PASS，直接提取实际补丁SDK函数：API拒绝/BTC复制与投递/alloc/coreEBUSY/end先到/重复/timeout/reset/legacy，内存平衡 | `.pio/sdk-implementation.log`；依赖为stub，不是完整SDK/RTOS/射频运行 |
| SDK输入篡改拒绝 | PASS，组件缓存新增/修改/缺失，隔离SDK非patch输入修改/新增 | 同上；未修改全局SDK |
| APP | PASS，78项测试、debug APK构建 | APP `implementation-tests.log` 与 JUnit XML；未验收手机UI |
| 日志工具 | PASS，7项：乱序、重启、多设备、截断、earlyACK等 | `.pio/log-tools-implementation.log`；采集器仅编译检查，未打开设备 |

SDK 桩测试执行到 `seg_tx_complete`、`bt_mesh_tx_reset` 和 `bt_mesh_tx_reset_single` 的实际提取函数；reset 确实存在不发end的路径，因此保持冻结策略。它没有覆盖全栈真实调度、射频ACK或硬件故障注入，S0硬件出口仍待验收。

复跑命令（在主机目录，按本机路径调整）：

```powershell
& C:/Users/79622/.platformio/penv/Scripts/platformio.exe run -t clean
& C:/Users/79622/.platformio/penv/Scripts/platformio.exe run
& C:/Users/79622/.platformio/penv/Scripts/python.exe tools/run_host_tests.py --compiler .pio/host-tools/ziglang/zig.exe
& C:/Users/79622/.platformio/penv/Scripts/python.exe tools/run_sdk_tests.py --framework C:/Users/79622/.platformio/packages/framework-espidf --compiler .pio/host-tools/ziglang/zig.exe
& C:/Users/79622/.platformio/penv/Scripts/python.exe test/log_tools_tests.py
```

从机在其项目内执行 clean/build 和 `tools/run_host_tests.py`，编译器使用主机的 Zig 绝对路径。APP 在其项目内执行 `./gradlew.bat :app:testDebugUnitTest :app:assembleDebug --console=plain`。主机跨仓测试依赖相邻目录名 `pump-muti-slave`。

## 5. 验收矩阵与未通过项

| 方案出口 | 当前状态 | 仍需提供的证据 |
|---|---|---|
| S0 固定输入/双芯片构建 | 软件PASS，设备验证PENDING | 真实API/BTC/SAR回调与reset矩阵，设备内存/RTOS故障注入 |
| S1 有界发送 | 软件PASS，设备验证PENDING | 真机同目的/全局占位、队列峰值和运行堆/栈；无水模拟IO台架 |
| S2 授权/停止 | 软件PASS，设备验证PENDING | 泵输出与控制抖动、首脉冲预算、Close丢失/断电/慢节点 |
| S3 身份/结果 | 软件PASS，设备验证PENDING | 串口身份与刷写SHA对应，旧端拒绝，手机重启/重连显示原任务 |
| S4 递进通信/实水/长稳 | **PENDING** | 1→2→5台，分别20批500mL及每路测量，5台8小时墙钟台架 |
| 体积精度 | **PENDING** | 业务先确认容差；目前无量杯/称重数据 |
| 开放生产5路 | **未开放** | 上述门槛通过后单独调整并发，重建、重测、再部署 |

当前只读设备枚举显示1台主机（COM5 CP210x）和1台从机（COM6 ESP32 USB），COM3/4是蓝牙虚拟串口。它只说明连接情况，没有读取现场固件身份，不能证明已部署本次构建。主机工程默认COM3，后续刷写必须核对真实端口并覆盖。

本次尚无现场执行条件的答复。代码构建与离线测试已独立完成；刷机/重启、真实启泵/实水及五路台架需要现场设备、执行安排和容差信息。具体步骤见[现场验收手册与记录](MESH_FIELD_ACCEPTANCE_20261019.md)。

## 6. 交付、恢复与变更管理

候选包由 `tools/package_mesh_candidate.py` 生成至忽略目录 `dist/`。内含双端BIN/ELF/引导/分区、debug APK、源码快照、SDK补丁/完整文件manifest、effective sdkconfig、测试日志和SHA256清单。包是软件候选版，不是硬件验收或生产发布证明。

```powershell
& C:/Users/79622/.platformio/penv/Scripts/python.exe tools/package_mesh_candidate.py --slave D:/pump/pump-muti-slave --app D:/pump/pumpmutiapp --output dist/mesh-candidate-20261019-e0950038-495431ba
```

打包先核对当前构建输入、SDK文件集、编译ID、强回调、BIN/ELF对应、分区/上传地址、从机白名单、生产并发和测试日志；输出已存在则拒绝覆盖。重新构建后必须刷新日志/manifest和产物证据，不可沿用旧包身份。两端另运行PlatformIO `run -t envdump`保存到`.pio/envdump-implementation.log`，运行`run -t compiledb`后将新生成的根`compile_commands.json`移至`.pio/compile_commands-implementation.json`；已有根数据库须先保留，不能覆盖用户文件。IDF辅助数据库不含SCons追加的构建ID，打包使用PlatformIO数据库。

两端及APP修改仍在工作区，未替用户提交、推送或覆盖原有HANDOVER/README修改。源代码基线、当前Git状态和完整打包文件SHA保存在候选包中。

主机现在写NVS format3，旧固件不保证能读取。回退必须先停机、备份并制定迁移/恢复步骤，不能盲目擦除取消fence和防重放账本。掉电前尚未写入的第一停止原因不保证可恢复；重启只能按Reboot/unknown核实，不能声称保存了断电前确切原因。

Mesh Response由46扩展为47字节；本工程主机兼容旧46字节，APP兼容旧文本R/FLOW。工程外固定长度消费者需要同步核对。双端业务协议、governor和SDK脚本为对应版本，不能只刷主机后绕过版本拒绝，也不能单方面增加SAR窗口或拉长保护维持出水。
