# 恢复功能：主机身份的可信安装

状态：2026-10-27 的源码候选。此流程建立公开主机身份信任，不授予管理员权限，也不开放从机退网。完整恢复功能尚未验收；`recovery_enabled=0` 必须继续保持。

## 主机 USB 导出

先完成主机镜像、有效配对 PIN、输出关闭与空闲状态的核对。身份初始化须在可信 USB 安装环境中完成，不能让附近第一个 BLE 客户端成为管理员。主机专用的 `master_identity` NVS 命名空间独立于 Mesh 和应用登记。

在主机仓库根目录准备隔离的 Python 环境，安装 `tools/requirements-recovery.txt` 中固定版本的依赖。导出目录必须已经存在，输出文件必须不存在：

```powershell
python tools/master_identity_install.py --port COM7 --out D:\installation\master.pmmaster
```

首次可信安装、确定身份尚未初始化时，显式追加 `--initialize-if-empty`。已有身份时，该参数不会覆盖密钥；存储故障也不会触发重建。

脚本在打开串口前将 DTR、RTS 设为 False，读取公开身份，生成新的 32 字节挑战，并核验设备的 P-256 所有权证明及 SHA-256 截断指纹。通过后以独占新建方式保存文件、同步并读回核对。它不读取主机私钥，不清 Mesh、不修改从机、不发送泵阀命令。若中途失败，先排查串口和状态，不删除 NVS 重试。

对应 USB 原语为 `master-identity`、`master-identity-init CONFIRM`、`master-identity-prove <随机挑战>`。初始化后的私钥不导出。初始化检查由主机既有控制任务处理；软件单测不能代替设备实测。

## APP 导入

在设置页的高级选项中进入“主机身份”，选择安装人员交付的 `.pmmaster` 文件，核对对方可信渠道提供的完整指纹，再确认来源并保存。文件自带的签名只能证明内部身份一致，不能证明它来自可信的主机；不得从附近设备自报的指纹建立首次信任。

APP 严格检查文件长度、版本、曲线点、指纹和 low-S 签名。相同身份重复导入不改写；同一物理 UUID 或指纹发生网络代际、公钥、网络身份冲突时拒绝静默替换。已保存身份的合法更新需要后续明确的维护/迁移流程。

信任库存放于 `noBackupFilesDir/recovery/master-trust.bin`，通过独立 Android Keystore AES-256-GCM 密钥保护，并用 AtomicFile 提交。现有文件损坏或包装密钥丢失时拒绝读取和追加，不自动清库重建。身份文件只有公开信息，可重新通过可信 USB 导出；它不能替代从机恢复凭证备份。

## 格式与验证边界

`.pmmaster` 固定 225 字节，小端整数：

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 8 | ASCII `PMMSTR01` |
| 8 | 2 | 版本 1 |
| 10 | 2 | 长度 225 |
| 12 | 16 | 主机物理 UUID |
| 28 | 4 | 身份 epoch |
| 32 | 16 | network_id |
| 48 | 16 | SHA-256(public_key) 前 16 字节 |
| 64 | 65 | P-256 SEC1 非压缩公钥 |
| 129 | 32 | 本次 USB 挑战 |
| 161 | 64 | low-S `r||s` 签名 |

签名消息：`ASCII PUMP-MASTER-PROOF-V1` + UUID + epochLE32 + network_id + fingerprint + public_key + challenge，共 169 字节。文件证明用于安装；后续 BLE 会话必须用新的挑战验证在线主机，并完成独立管理员认证，不能复用文件里的旧挑战作为在线证明。

当前已完成固件 mbedTLS、Python 与 JVM 的公开合成向量互通。Android provider、Keystore 仪器测试已编译，但主机身份文件相关的新增设备测试尚未在手机执行；主机新镜像也尚未烧录。旧 dist 产物不能视为本次候选的发布包。
