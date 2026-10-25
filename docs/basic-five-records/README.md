# 待执行的现场验收记录

所有行初始为 `NOT_TESTED`，实际观测、地址、量测值、时间和日志路径留空。计划安排不是已执行证据。当前无设备测试条件，本目录不能作为通过报告。

- `node-inventory.csv`：五台地址、别名、实际固件、K/版本和接线。
- `B-communication.csv`：20 系统轮 × 5 台 = 100 节点轮；计划手机连接/断开各 10 轮、两次整组冷启动，实际情况另填。五台全程同时在网，泵和全部阀输出为零。各轮逐台核对配置四步/HELLO及三次只读；替代操作须写明等效分段覆盖，普通查询不能替代配置验收。任一目标 SAR 超时整轮 FAIL，原始失败记录保留。
- `C-tasks.csv`：85 个定量整任务；单路 50、全五路 10、中间组合 25，覆盖全部 31 种非空 mask。每路目标一致，同时出水，独立到量关阀。连续 10 个多路任务可以引用本表。
- `C-branch-measurements.csv`：上述任务的 170 个逐路量测；实际量包含尾流，逐行计算 `signed_error_ml = actual_ml - target_ml_each`，绝对值不得超过 `allowed_abs_error_ml`。实际与上报量分别填写，不以总量抵消单路误差。时间使用同一基准或说明双端时钟对齐方法。
- `D-protection-and-timing.csv`：断连、重启、存储临界点、维护及输出时序清单。50ms 是有效停止/按钮释放到电气输出关闭的时间，手机 BLE 等待和机械动作另计。存储故障项必须在真实 Application 路径执行，helper 测试不能替代。

判定流程与详细次数以 [整体方案第 7 节](../BASIC_FUNCTION_OPTIMIZATION_PLAN_20261025.md) 为准，按 A → B → C → D 推进。记录固件 Build ID、APP/镜像哈希、JobKey、主从日志与示波器证据；每项填 PASS/FAIL/NOT_TESTED 及原因。未完成不得填 PASS，失败复测另追加记录，不覆盖原始失败行。

模板生成入口为 `tools/create_basic_five_records.py`。它拒绝覆盖已有 CSV，以保护后续实际记录；需要新一轮记录时复制本目录并另行编号。
