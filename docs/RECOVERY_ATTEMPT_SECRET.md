# 每次恢复尝试的 OOB 存储

状态：2026-10-27，主从共享的持久化基础代码，尚未接入运行时、GATT 或 Mesh SDK。`AttemptSecretStore` 不授权清网，不签署主机回执，不开放恢复功能；`recovery_enabled=0` 继续保持。

## 调用边界

协调器先验证恢复会话、当前凭证、目标主机信任和新鲜签名回执，再保存本次 16 字节 Static OOB。只有保存成功且 `resolve(reference, receipt, out)` 验证完整上下文后，才可将引用交给 `ResetIntentStore`。不能把签名有效或秘密已保存等同于安全准入通过。

`load(credential)` 必须使用当前从机 UUID、credential_epoch 和 recovery_key。首次无 selector 为 Empty；已提交记录损坏、凭证不匹配、读失败或 AEAD 验证失败为 Fault，不自动重建。底层 journal 以 selector 为提交点，不提升孤立的新 slot。

`install` 只接受承诺值匹配的规范回执与非全零 OOB。相同完整回执和 OOB 返回 Existing，参数冲突返回 Rejected，不覆盖。写入或读回不确定返回 StorageFault，实例清除内存秘密并进入 Fault。`resolve` 必须同时提供引用和完整回执，任一失败将输出清零。

当前只支持单次尝试；没有退休、重新武装、凭证轮换或跨 journal 回收协议。不能通过删除本命名空间来重试：旧清网意图可能仍引用该秘密。后续生命周期必须先持久化终态/替代关系，再使旧引用不可用；掉电后须能解释两份 journal 的组合。

## 固定编码

记录为 272 字节；整数小端；保留字节必须为 0。外层使用独立 NVS 命名空间中的 A/B journal。

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 | magic `0x31534150` |
| 4 | 2 | schema 1 |
| 6 | 2 | 长度 272 |
| 8 | 16 | 从机物理 UUID |
| 24 | 4 | credential_epoch |
| 28 | 4 | 保留 |
| 32 | 16 | 非零随机秘密引用 |
| 48 | 12 | AES-GCM 非零随机 nonce |
| 60 | 4 | 保留 |
| 64 | 192 | 加密的规范 PreparedReceipt176 + OOB16 |
| 256 | 16 | GCM tag |

包装密钥采用 HKDF-SHA256：IKM 为 recovery_key32，salt 为 UUID16 + epochLE32，info 为 ASCII `PUMP-ATTEMPT-STORE-KEY-V1`，输出 32 字节。AAD 为 ASCII `PUMP-ATTEMPT-STORE-V1` + 完整 64 字节头部。nonce 由设备密码学 RNG 生成；RNG 不可用时拒绝安装。

OOB 承诺为 SHA256，输入依次为 ASCII `PUMP-ATTEMPT-OOB-V1`、operation16、attemptLE32、device16、epochLE32、master16、network16、binding16、generationLE64、OOB16。不得只比较引用或 operation，必须同时校验整个回执。APP 的 `RecoveryAttemptOob` 使用相同编码；调用方负责在使用后清除可变秘密缓冲区。

两份 slot（各含 24 字节 journal 头）加 selector 的净长度为 `2 × (24 + 272) + 24 = 616` 字节，不含 NVS namespace、blob 索引、条目、页面、旧版本和 GC 开销。不能以该数字替代 64 KiB 分区最坏容量验收。

包装密钥源自同一设备 Flash 内的恢复凭证；本设计不能抵抗取得该凭证后的物理解密，也不提供对恶意 Flash 回滚的硬件防护。

## 验证

`tools/run_recovery_tests.py` 检查主从共享源一致，并使用固定 ESP-IDF 的真实 mbedTLS 执行独立 Python 向量、全部上下文、OOB 变异、幂等冲突、坏 RNG、重启、8 个掉电/模糊提交边界、272 字节 envelope 及 320 字节 journal 损坏测试。另验证它与清网意图的最小引用关系。APP 对应 4 项 JVM 测试已通过。

上述为软件证据；没有代表真实 NVS 满、设备掉电、强制 OOB 空口拒绝降级或完整恢复事务通过验收。
