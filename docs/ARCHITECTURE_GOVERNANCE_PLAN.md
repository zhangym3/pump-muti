# 主从固件彻底解耦与历史代码治理方案

## 1. 目标与范围

本方案目标是让主机 ESP32 与从机 ESP32-C3 成为两个职责单一、可独立构建、测试、发布和升级的固件工程。主从之间只通过已定义的 BLE Mesh 协议交互；不通过共享应用代码、运行时角色开关或交叉构建依赖实现协同。

治理覆盖：

- 主机工程：`D:/pump/pump-muti`
- 从机工程：`D:/pump/pump-muti-slave`
- 必要的协议兼容面：`D:/pump/pumpmutiapp` 中的主机 GATT 命令/状态解析及 Mesh 页面
- 构建配置、主从 NVS、设备身份、Mesh 组网和发布产物

本方案中的源码边界治理已执行到从机独立化和 APP 提示清理阶段；仍不代表已完成设备擦除、固件烧录或 COM5/COM6 双板现场验收。

## 2. 目标架构

| 边界 | 主机 ESP32 | 从机 ESP32-C3 | 手机 APP |
|---|---|---|---|
| 用户连接 | 唯一手机 GATT/PIN 入口 | 不提供手机业务 GATT | 仅连接主机 |
| Mesh 角色 | Provisioner、候选扫描、从机加入和节点清单管理 | Provisioned Node、接收主机任务并回报状态 | 经主机命令发起扫描/加入/清单查询 |
| 业务职责 | 任务编排、安全门控、节点调度、本机水泵 PWM | 本地阀门执行、流量累计、联锁、到量关阀、租约保护 | 展示状态、提交/停止任务、管理组网 |
| 板级硬件 | ESP32 GPIO25 泵 PWM | C3 GPIO4 阀门、GPIO5 流量输入、GPIO6 联锁 | 无硬件控制责任 |
| 本地配置 | 主机 PIN、泵档案、任务和节点登记 | 阀门档案、流量系数、主机绑定/任务状态 | 不直接改硬件档案或 Mesh 密钥 |

工程之间允许协议和行为兼容，不允许共享 `src/`、`include/`、构建输出目录，也不允许从机工程依赖主机工程路径。协议规范是接口契约，不是可被两个固件共同编译的“万能实现库”。

## 3. 当前发现和治理判定

### 3.1 明确的角色混杂（基线问题与当前结果）

基线版本曾通过 `PUMP_ROLE_MASTER=0` 和 `board::IsMaster` 在共用应用中保留主机分支：

- `D:/pump/pump-muti-slave/src/CMakeLists.txt` 仍将 `master_scheduler.cpp` 纳入从机目标。
- 从机 `src/application.cpp` 仍处理主机 `pump-test`、任务调度、`pump-reset`、节点注册等分支，并在运行时检查 `board::IsMaster`。
- 从机 `src/mesh_management.cpp` 仍包含 Provisioner 扫描/加入、配置客户端、远端节点清理等主机管理逻辑，只靠宏裁剪部分代码。
- 从机 `src/mesh_transport.cpp`、`src/hardware.cpp`、`src/main.cpp` 也含有主机/从机条件分支。
- 从机 `board_profile.h` 同时定义阀门配置和主机泵配置，说明板级接口仍为通用双角色形态。

这些内容已从当前从机目标移除，并由静态角色边界脚本检查；真实 C3 clean build、链接映射和双板行为仍需在具备 PlatformIO/ESP-IDF 的环境中完成。

### 3.2 两边重复源码不是共享契约

当前两工程存在同名文件，部分内容相同、部分内容不同：

- `protocol.cpp`、`pump_safety_controller.cpp` 等存在同内容副本。
- `application.cpp`、`mesh_management.cpp`、`mesh_transport.cpp`、`storage.cpp` 等具有角色差异。
- 这类复制本身不一定是缺陷；若主从必须独立构建，复制后的内部实现可以独立演化。风险在于没有清晰标注哪些复制实现必须满足同一协议契约，哪些只是碰巧同名。

治理原则是：共享协议事实，不共享角色实现；协议数据通过规范、固定测试向量和版本校验保持兼容，不以同步复制整份 C++ 源码作为保证。

### 3.3 必须保护的兼容/安全内容

以下内容不能按“重复或历史”直接删除：

- 主机和从机 `protocol.h` 中的 Vendor Company ID、Model ID、操作码、字段长度和枚举值。
- Mesh UUID 角色标记、Provisioner/Node 行为、组地址和模型绑定流程。
- 两端持久化格式：`magic`、`format`、`ProfileId`、字段顺序/类型、已部署设备升级语义。
- 从机 `commissioned=false` 安全默认、输出低电平启动、流量/联锁保护、租约和本地关阀逻辑。
- 主机 `BOOT_LOCK`、任务持久化、未知结果保护、泵输出限时和失败安全行为。
- 已验证仍需兼容旧设备的 Mesh Node 自动清理流程；但它必须与普通 Provisioner 启动失败使用不同状态码和 APP 提示。

特别注意：从机当前 `commissioned=false` 没有发现运行时启用入口，不能通过直接改成 `true` 来消除“未配置”。必须先确定安全的现场投产流程，再开发配置机制或正式生成已验收板级配置。

### 3.4 APP 和文档对固件拆分有外部影响

APP 只连接主机，但依赖主机 GATT 命令及 `HELLO`、`MESH`、`INV_NODE`、`LOCAL`、任务/测试状态文本。固件拆分不得改变这些语义，除非同时升级 APP 并定义版本兼容策略。APP 仍兼容解析 `LEGACY_NETWORK`，但只显示不可用/重连提示，不再显示迁移步骤；USB 仅作为固件现场维护接口，不属于手机组网流程。

### 3.5 当前架构的数据流与耦合点

当前主机启动链路是 `app_main → Storage/Hardware → PumpSafetyController → MeshTransport/ControlGateway → MasterScheduler → Application`。主机的 `Application` 集中处理手机 GATT 文本命令、USB 命令、Mesh 二进制消息、节点注册、泵保护、任务结果持久化、状态发布和清单分页，已经承担多个应用服务和协调器职责。

从机入口则构造 `FlowJobController`、`PumpSafetyController`、`MasterScheduler`、`MeshTransport`、`ControlGateway` 和同名 `Application`。从机的 `Application` 不是精简后的从机应用，而是主机应用的大量副本加上阀门协议分支。`if constexpr (board::IsMaster)` 与运行时 `board::IsMaster` 检查把角色区别埋在实现内部，构建成功只能证明所选宏组合可以编译，不等于工程职责已经隔离。

当前主要耦合可以归纳为：

1. **应用层角色耦合：**主从共用 `Application` 名称、命令解析和状态字段；从机必须在多个入口拒绝主机业务。
2. **Mesh 管理与业务调度耦合：**`MeshTransport` 头文件包含 `master_scheduler.h`；同一 transport API 同时暴露从机需要的 Node 通信和主机专属的扫描、Provisioner、清除远端节点操作。
3. **持久状态模型耦合：**主机状态保留旧本地阀门字段；从机的 `PersistentState` 也包含 `NodeConfig nodes[]`、手机 client 记录、主机 `JobReport` 和所有节点结果等主机字段。源码引用显示，主机启动时使用 `nodes[]` 恢复调度器；从机运行代码主要需要主机地址、从机 K 系数/版本、当前防重 fence、阀门结果和 revision 保留值。
4. **物理存储格式耦合：**两边都将 C++ `PersistentState` 整体按 `sizeof(state)` 读写为单个 NVS blob，并以 `format == 1`、profile 和精确 blob 长度判定兼容。字段删除、排序、类型、对齐或编译器 ABI 变化都可能使旧设备启动拒绝；这比普通结构体重构风险更高。
5. **设备 I/O 边界不对称：**主机 `ControlGateway` 同时负责 PIN/GATT 和 USB；从机 `ControlGateway` 已固定为 USB Serial/JTAG，没有手机业务 GATT。这一差异符合产品职责，应当成为独立接口，而不是再强行统一实现。
6. **构建边界比源码边界更强：**两个目录已有独立 PlatformIO/IDF 项目，但从机 CMake 仍编译主机调度源码，公共宏和宽泛头文件使错误职责进入从机目标。

### 3.6 推荐的优化后结构

建议按“两个独立固件 + 一个协议契约”组织，而不是抽成一个双角色固件或共享业务库：

```text
Master ESP32
  MasterComposition / app_main
    MasterControlService (GATT/USB 命令、授权、状态快照)
    MasterProvisioningService (扫描、加入、模型配置、远端节点重置)
    MasterJobCoordinator (任务账本、调度、节点结果汇总)
    MasterPumpController + MasterBoardProfile
    MasterStorage (PIN、节点表、任务账本、泵状态)
    MasterMeshPort (Mesh 收发队列和 Node/Provisioner 底层适配)

Slave ESP32-C3
  SlaveComposition / app_main
    SlaveMaintenanceService (仅本地维护命令)
    SlaveJobService (主机任务协议、去重、结果确认)
    SlaveMeshNode (Provisioned Node 收发/恢复，不负责发现其他节点)
    ValveJobController (本地阀门、流量、联锁、租约)
    SlaveBoardProfile + SlaveHardware
    SlaveStorage (主机绑定、K 系数、任务 fence、阀门结果)

Protocol contract (不参与固件链接)
  Vendor Mesh 报文规范 + 状态/命令规范 + golden vectors + 版本规则
```

`MasterControlService` 与 `SlaveJobService` 不共享应用类层次或角色宏。Mesh 控制面和数据面分开：主机 Provisioner 扫描/入网是 `MasterProvisioningService` 的职责；可靠的 Mesh 收发队列由角色适配层提供；从机只实现 Node 生命周期和业务报文。调度器不得被底层 transport 头文件反向包含。必要的小型 `PacketSender`/`MeshPort` 接口可在两个工程各自定义，避免为了共享一个接口又引入构建依赖。

双方可以各自拥有小型编码/解码实现，但都必须通过同一套协议固定向量。若未来确实需要减少编码器重复，应先证明该库不引入交叉板型依赖、构建路径依赖或发布耦合，再单独评审；当前阶段不建议先抽共同 C++ runtime library。服务拆分也不应过度细化：按独立状态、权限边界、重启/超时生命周期拆分即可，不要求每个函数新建一个类。

### 3.7 架构优化选项比较

| 方案 | 优点 | 代价/风险 | 结论 |
|---|---|---|---|
| 保持一套双角色 Application/Hardware，通过宏切换 | 短期改动最少 | 无效分支、误编译和主从回归风险持续；职责不清 | 不继续扩展，逐步退出 |
| 提取共享 C++ 固件库，主从工程链接同一份业务代码 | 可减少文本重复 | 形成新的共同构建/版本依赖，容易再次引入双角色抽象；公共 API 可能拉入错误平台 | 当前不推荐 |
| 主从业务源码彻底分开，协议以规范和固定向量协调 | 构建隔离清楚，角色错误难进入镜像，版本可独立 | 少量编码器/测试辅助代码可能重复，需要协议治理 | **推荐作为近期目标** |
| 再建立独立协议仓/代码生成器 | 单一 schema、可自动生成多端向量或 parser | 增加仓库、工具链、发布和版本治理成本 | 只有协议复杂度/多客户端数量增长后再评估 |

因此更优路径不是把所有重复消灭，而是消除**职责重复和错误依赖**：角色业务代码分别所有；稳定的 wire contract 单一管理；小型重复 codec 可接受但必须由一致性测试约束。

### 3.8 持久化架构优化建议

当前 `format 1` 是整块 C++ 结构体 blob。近期解耦不应直接换格式；应分两步处理：

1. **兼容阶段：**主从应用拆分时保持各自现有 `format 1` 解码器和 ProfileId；从机启动仍能读老 C3 blob，主机升级仍能读老 ESP32 blob。新运行时只读取各自实际需要的字段，不因源码层移除字段而马上缩短旧 blob 结构。
2. **格式演进阶段：**设计显式字段版本或独立 NVS key，提供 `format 1 → format 2` 单向迁移、迁移后校验、失败保留旧数据和回滚策略。只有在备份/恢复与老镜像回退方案明确后，才删除主机遗留阀门字段和从机遗留主机 ledger 字段。

若为短期拆分保留一个仅用于旧 blob 解析的兼容结构，应命名为 `LegacyPersistentStateV1`，禁止业务代码继续依赖它；新状态模型使用 `MasterPersistentState` 和 `SlavePersistentState`，让“兼容格式”与“当前业务状态”明确分层。

## 4. 文件级处置建议

### 4.1 从机工程

| 文件/区域 | 建议动作 | 主要风险/验证 |
|---|---|---|
| `src/application.cpp` | 拆为从机专用应用入口，移除主机命令解析和调度状态 | Mesh Vendor 消息收发、USB 维护命令、NVS 更新及故障行为必须保持 |
| `src/master_scheduler.cpp`、`include/master_scheduler.h` | 首选从机目标中移除；确认无从机引用后再删除从机副本 | 不得误删主机工程自己的调度器；检查构建图和符号引用 |
| `src/pump_safety_controller.cpp`、`include/pump_safety_controller.h` | 检查从机是否仅为通用构造/HELLO 保留；拆分后若无从机调用则从从机工程移除 | 不要触及主机泵安全模块；保留从机阀门安全控制器 |
| `src/mesh_management.cpp` | 改为从机节点生命周期管理，移除 Provisioner、候选扫描、节点配置客户端和主机 reset 管理 | 确保从机仍可被发现、Provision、恢复、接收模型消息 |
| `src/mesh_transport.cpp` | 固定 C3 Node 初始化路径，移除主机 Provisioner/GATT 服务路径 | 检查 Mesh Proxy 是否为业务所需；不可因删 GATT 而误删 Mesh PB-GATT 必需项 |
| `src/hardware.cpp`、`include/hardware.h` | 只保留阀门输出、流量计数和联锁接口 | C3 GPIO 映射、中断、上电输出低、故障关阀实板验收 |
| `include/board_profile.h` | 移除主机 `pumpConfig` 和 `IsMaster`；只保留 C3 阀门档案 | ProfileId 与现有 C3 NVS 兼容，不得静默更改 |
| `src/main.cpp`、`src/CMakeLists.txt`、`platformio.ini` | 固定 `valve_slave_c3`；构建仅列从机实际模块，不再传 `PUMP_ROLE_MASTER=0` 作为角色选择机制 | Clean build、sdkconfig 和链接 map 检查；保留 COM6 本地维护设置 |
| 从机测试 | 只覆盖从机控制器、协议解析和从机 Mesh 行为 | 测试必须证明泵主机 API 不进入从机镜像 |

### 4.2 主机工程

| 文件/区域 | 建议动作 | 主要风险/验证 |
|---|---|---|
| `src/application.cpp` | 固定为手机 GATT/USB 命令到主机业务的入口；移除从机本地阀门路径和主从角色判定 | 保持 APP 命令、重放保护、任务/测试终态和 NVS 记录兼容 |
| `src/master_scheduler.cpp` | 保留为主机唯一的从机任务调度模块 | 验证多节点顺序、部分失败、停止和未知结果处理 |
| `src/mesh_management.cpp` | 保留 Provisioner 职责；区分旧 Node 清理、普通 enable/config 失败和用户主动 `mesh-reset` | Mesh 本地 reset 与清空远端节点属于高影响操作，分别验收并确认保留的 NVS 范围 |
| `src/mesh_transport.cpp` | 固定 Provisioner/主机模型行为及手机 GATT 入口 | 验证手机 PIN/加密、Mesh 并行与 GATT 稳定性、旧设备角色恢复 |
| `include/board_profile.h`、`src/hardware.cpp` | 只保留主机 ESP32 GPIO25 水泵硬件和主机泵保护 | 输出参数、BOOT_LOCK、限时测试、上电关闭状态均需回归 |
| `src/CMakeLists.txt`、`platformio.ini` | 固定 `esp32dev`/ESP32 主机构建，不接受 C3 目标 | Clean build、目标宏、sdkconfig、镜像目标芯片检查 |
| USB 维护协议 | 清查 `mesh-migrate`、`pump-reset`、`node` 等命令，按现场仍需功能决定保留/废弃 | APP 当前不应依赖 USB 入口；移除前确认不会切断部署和救援能力 |

### 4.3 协议与状态合同

建立主从和 APP 均可消费的协议基准材料，建议包括：

- `docs/protocol/` 下唯一版本化协议说明，逐条定义命令/响应方向、字节长度、字段范围、超时、重试、幂等性、错误码。
- 固定二进制报文向量，供主机和从机各自的 host 测试验证解析与编码，不引入共同 C++ 头文件依赖。
- 固定文本 GATT 状态的字段定义和 APP parser 测试向量，标注兼容的 APP/固件最低版本。
- 协议变更流程：先兼容扩展、后切换消费者、最后才考虑删除旧字段；不可在两个工程各自“顺手调整”枚举值或长度。

## 5. 实施阶段与阶段门

### 阶段 0：冻结基线和影响清单

1. 记录两个工程当前源码版本、构建参数、SDK 配置、产物哈希、板卡型号和 NVS Profile/格式。
2. 对每个从机目标源文件做 include/符号调用图，标注 master-only、slave-only、协议公共事实、待确认项。
3. 保存当前 ESP32 与 C3 可刷写镜像及其 manifest；记录现场 COM5/COM6 和固件版本。
4. 在此阶段不擦除 NVS、不改 `commissioned`、不改现场 Mesh 密钥。

**阶段门：**有可回退的镜像/源码基线；已确认 NVS Profile 和当前 APP 协议版本。

### 阶段 1：建立边界测试，再动生产逻辑

1. 增加独立的协议固定向量测试，覆盖主发从收、从发主收、错误长度、未知版本、范围边界和字节序。
2. 增加主机/从机目标特征检查：目标板型、核心业务类、命令入口、Mesh 角色和输出引脚。
3. 记录当前关键行为基线：手机 PIN/GATT、扫描候选、Mesh 加入和模型配置、清单同步、任务启动/停止/结果、从机流量停止和联锁故障。

**阶段门：**拆分前测试通过，且失败时可定位到行为变化而不是协议基线缺失。

### 阶段 2：先拆从机，限制单一角色

1. 从 `Application` 中提取只属于从机的命令和 Vendor Model 消息处理。
2. 将 Mesh 管理拆成从机 Node 初始化与恢复逻辑，不编译 Provisioner 管理状态机。
3. 固定 C3 hardware profile，删除 `board::IsMaster` 选择和主机 `PumpConfig` 路径。
4. 从从机 CMake 源清单移除主机调度器/主机泵控制；根据未引用证据删除从机副本。
5. 去掉从机不使用的主机 GATT/命令能力，保留 BLE Mesh Provisioning 实际依赖的 bearer。
6. 每次完成一个模块就做 C3 clean build、host tests、链接符号审计，避免一次性大删导致故障难定位。

**阶段门：**从机独立构建通过；从机镜像不包含主机业务符号；Mesh 入网和从机控制器测试通过。

### 阶段 3：固化主机工程并治理旧状态

1. 从主机应用移除从机本地硬件路径和运行时角色选择，确保主机只负责本地泵与远端节点调度。
2. 将“旧 Node 凭据恢复”“Provisioner 启用失败”“用户主动清空所有远端节点”定义成独立状态及操作。
3. 评估 `mesh-migrate` 兼容入口和旧 Mesh UI 状态：确认无已部署旧 APP/维护工具依赖后，再按弃用周期移除。
4. 主机 NVS 中保留格式 1 的旧字段时添加明确兼容注释；只有完成迁移读取方案和升级测试后才允许格式演进。
5. 验证 PIN、泵设置、任务记录、节点登记和重启 epoch 的升级行为。

**阶段门：**主机工程只构建 ESP32；手机添加/重置从机流程可用；旧设备状态不再被普通启动错误误报。

### 阶段 4：清理维护入口、配置和文档

1. 按命令用途逐项审核 USB：保留真实救援/生产维护命令；删除没有实现、无调用或角色错误的命令。
2. 对从机 commissioning 机制单独决策；未实现前，文档清楚说明无法手机或 USB 动态启用，而不是声称已可配置。
3. 清理旧迁移说明、旧协议示例和不再适用的 USB 步骤；分别更新两个工程 README、HANDOVER、验收文档和 APP 设置提示。
4. 检查构建配置、工具脚本、CI、打包 manifest 和历史产物路径，确认任何工程都不会取用对方的 `.pio` 或镜像。
5. 归档而不是直接删除现有发布镜像、NVS 迁移资料和现场日志；待新版本验收后再决定归档保留期。

**阶段门：**文档与代码命令一致；镜像 manifest 能识别角色和芯片；没有跨目录构建或发布依赖。

### 阶段 5：双板和手机联合验收

按以下顺序验证，任何安全失败都停止放量：

1. 空白构建环境下分别 clean build 两个工程，确认互不读取源码、include、SDK 配置或输出目录。
2. 审核 `.map`/符号表和镜像元数据：从机没有主机 GATT/Provisioner/泵 PWM/任务编排符号；主机没有 C3 阀门驱动/流量 ISR/联锁路径。
3. 主机接 COM5，从机接 COM6；确认各镜像芯片目标、引脚配置和镜像哈希。
4. 手机连接主机并完成 PIN/GATT 加密；主机扫描、展示、加入从机，确认模型绑定、从机握手和清单完整提交。
5. 测试主机重启、从机重启、Mesh 恢复、重复加入、离线从机、配置失败、节点重置及 APP 重连。
6. 在安全水路/负载条件下测试从机关闭状态启动、联锁断开、无流量、到量关闭、租约过期和 Mesh 断链行为。
7. 测试主机 BOOT_LOCK、手动试泵、定量任务、停止、未知结果保护和 NVS 升级兼容。

## 6. 影响范围与风险控制

| 影响面 | 风险 | 控制方式 |
|---|---|---|
| Vendor Mesh 协议 | 两边独立修改后字段或操作码不一致，节点不可控 | 固定向量测试、协议版本和双端回归 |
| NVS/设备升级 | 字段变更使已部署设备启动失败或丢失任务/节点状态 | 保留旧 Profile/format，先迁移测试，明确回滚镜像 |
| Mesh 持久角色/密钥 | 清理 Node/Provisioner 状态会使设备退出现有 Mesh | 先区分主机本地角色清理与远端节点重置；现场操作有备份和双确认 |
| 从机安全门控 | commissioning 变更可能允许未验证阀门动作 | 默认保持禁用，硬件验收后才配置；不通过“清理”放宽保护 |
| 手机 APP | 固件文本状态或命令变化导致界面误判/按钮不可用 | APP parser、UI state tests 和最低版本策略同步更新 |
| USB 维护 | 移除命令可能失去现场救援或配置能力 | 命令逐项盘点，任何删改先确认对应替代路径 |
| 发布/烧录 | 主从镜像误刷，或使用过期产物 | manifest 标芯片、角色、版本和哈希；烧录前核对芯片与端口 |
| 双工程维护 | 复制协议实现发生漂移 | 明确协议唯一事实来源，不复制完整角色业务代码 |

## 7. 删除判定规则

代码只有满足以下条件才列入“移除”：

1. 不属于当前产品职责或目标芯片；
2. 无直接调用、回调注册、命令入口、构建引用或对外协议用途；
3. 不承担旧设备 NVS/Mesh/APP 的兼容职责，或兼容周期已正式结束；
4. 删除后由目标工程测试、协议向量、链接符号检查和实板用例覆盖；
5. 删除不会绕过安全门控、数据完整性检查或现场恢复能力。

命中 `#if PUMP_ROLE_MASTER`、文件名含 `master` 或源码重复不能单独作为删除依据。最终判断以依赖图、实际目标源清单、链接结果、行为测试和设备兼容需求为准。

## 8. 完成验收标准

- 主机与从机分别能在干净目录独立构建、测试、打包；一个工程断开/改名时另一个工程仍能构建。
- 从机源清单不包含主机调度器、主机泵安全控制、手机 GATT 业务和 Mesh Provisioner 管理；主机源清单不包含从机阀门、流量和联锁实现。
- 产品业务源码不再依赖 `PUMP_ROLE_MASTER`/`board::IsMaster` 在两个角色间切换。
- 两个工程通过同一组协议向量测试；APP 的 GATT 状态 parser/UI tests 与固件状态兼容。
- 主从 NVS Profile/format 及升级/回退方案有文档和测试证据。
- 编译镜像和发布 manifest 明确显示 ESP32 Master 或 ESP32-C3 Slave，固件不交叉引用对方工程产物。
- 手机只连接主机即可完成约定范围内的从机扫描、组网、控制和状态查看；从机没有手机业务入口。
- 未通过硬件安全验收的阀门配置仍保持禁用，Mesh 故障不会导致未授权输出。
- COM5/COM6 双板联合验收记录完整，所有失败项有明确处理结果。

## 9. 建议执行顺序

执行时采用小步提交：**冻结基线 → 协议/NVS 测试 → 从机独立化 → 主机边界固化 → APP/文档同步 → 双板验收**。先以从机为主清理目标，因为它当前混入的主机角色分支最多；主机侧涉及旧设备 Mesh 持久状态和 NVS，必须在兼容策略明确后处理。

验收期间不应同时改协议格式、NVS 格式和 commissioning 策略。每项改变独立评审、独立测试、独立记录镜像哈希，便于发现回归时准确回退。
