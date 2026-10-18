# 独立工程边界

主节点位于 `D:/pump/pump-muti`，只维护 ESP32 / esp32dev / pump_master。主节点负责 GPIO25 泵 PWM、手机 PIN/GATT、Mesh Provisioner、节点注册和任务调度。

从节点独立位于 `D:/pump/pump-muti-slave`，其板型、接线、构建和维护说明由该工程提供。从机源码已完成固定 C3 Node、阀门、流量和联锁边界清理；用户随后指定 COM6，已将其设为从机 PlatformIO 的上传和串口监视端口。

两边不共享 include/src、测试或构建目录。保持 `include/protocol.h`、`src/protocol.cpp`、`include/control_request.h` 的报文兼容；后续协议变更需协调两边。主机 `ValveStatus` 只描述远端结果和存储格式，不执行本地阀门控制。

主机保留 IDF Mesh Provisioner 功能。启动时若发现旧外部工具留下的 Mesh Node 角色，会自动清除 Mesh 凭据并重启接管为 Provisioner；手机不参与迁移。删除从机应用代码与主机自动接管流程相互独立。

主机清理前快照和更早的 backups 已归档至 `D:/pump/pump-muti-archive/master-only-20261017`。本工程打包只生成 dist/pump_master 的 3 个镜像。当前验证见 VALIDATION.md。
