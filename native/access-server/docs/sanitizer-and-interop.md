# Sanitizer 与外部互操作门禁

本文记录 T-01 的聚焦 sanitizer 和外部 rnacos 故障注入设计。它们补充常规 Release/CTest，
不替代生产脚本 corpus、Java/C++ 同请求差分、阶段 8 稳定性或切流演练。

## 1. Sanitizer 设计

顶层 native CMake 提供 `ACCESS_SERVER_SANITIZER`，取值为：

- `none`：默认构建，不添加 sanitizer；
- `address`：同时启用 ASAN 和 UBSAN；
- `thread`：启用 TSAN。

sanitizer 标志在引入 pinned Fiber 子目录前设置，因此 Fiber 静态库、access-server 组件和测试
使用同一套插桩；不是只检查测试入口。sanitizer 构建强制关闭 IPO/LTO、保留 frame pointer，
并在首个报告处失败。每种模式使用独立、已忽略的构建树：

```text
native/build-address/
native/build-thread/
```

统一入口为：

```bash
npm run test:native:sanitizers
```

也可以单独运行：

```bash
./native/access-server/scripts/run_sanitizers.sh address
./native/access-server/scripts/run_sanitizers.sh thread
```

`NATIVE_BUILD_JOBS` 控制构建并发数。`ACCESS_SERVER_SANITIZER_REPEAT` 仅用于本地诊断时覆盖
重复次数；正式默认值为 ASAN/UBSAN 5 轮、TSAN 10 轮。

ASAN/UBSAN 集合包含 93 个测试，覆盖：

- runtime/control-plane 启动、失败回滚、取消和重复关闭；
- DNS 异步释放、配置 watcher generation/Closed 竞态和 Nacos status subscriber join；
- route snapshot pin、TLS identity hazard/rotation 和 mTLS 握手；
- 多地址连接、pool lease、HTTP proxy、WebSocket、下游取消和 client metadata 解析。
- worker-sharded service selection、共享 endpoint circuit、权重分布和 canonical fallback。

TSAN 集合包含 34 个测试，集中检查：

- 配置和发现指标的 coherent snapshot 读取；
- service directory owner 发布与 worker 并发选择、单 half-open probe 和跨 worker circuit 状态；
- route/TLS snapshot 发布、pin 和回收；
- compiler/owner EventLoop 交接、Nacos status watch、启动回滚和 shutdown。

## 2. 外部 rnacos 故障注入设计

外部互操作测试默认不进入普通构建或单元测试。只有显式设置
`ACCESS_SERVER_BUILD_EXTERNAL_INTEROP_TESTS=ON` 才生成
`fiber_access_server_external_interop` 并注册 `access-server-interop` CTest label：

```bash
npm run test:native:interop
```

runner 只接受本机已经存在的固定镜像，不会隐式拉取：

```text
qingpan/rnacos@sha256:6c749166929fa565152d26acc344ccdfc437eb6cc57e02a752b5a3cf338edfb2
```

容器使用 `--rm`、`--pull=never`、禁用容器日志、临时 `/io`，并只向 loopback 发布随机端口。
测试不注入生产凭据、不持久化数据。Docker 或固定镜像缺失时 CTest 以 skip code 77 退出；
skip 不是互操作通过证据。

Python TCP fault proxy 位于真实 Fiber Nacos client 与 rnacos gRPC 端口之间，按以下顺序执行：

1. 在连接建立期对前两个 TCP connection 发送 RST，证明初始连接重试；
2. 等待真实 ConfigService 与 NamingService 同时进入 `Ready` 且 `rpc_available=true`；
3. 通过 ConfigService 完成一次缺失 key 的确定性查询；
4. 同时复位两个已就绪 gRPC connection；
5. 要求两个 service 的 disconnect counter 都增长，随后 ready counter 都增长且 RPC 恢复；
6. 再执行一次配置查询，并按 NamingService、ConfigService、status monitor、Nacos client 的
   生命周期顺序关闭；最终状态必须为 `Stopped` 且 RPC 不可用。

runner 还独立检查启动期与就绪后两类 RST 确实发生，避免客户端在未经过故障的情况下误报通过。

## 3. 当前验证记录

2026-08-18，Clang 22.1.6：

- ASAN+UBSAN：P-01 扩展后的 93 个聚焦测试连续 5 轮，共 465 次执行，0 失败、0 sanitizer 报告；
- TSAN：P-01 扩展后的 34 个并发测试连续 10 轮，共 340 次执行，0 失败、0 data-race 报告；
- 外部 rnacos：启动期 RST 2 次、Ready 后 RST 2 次，Config/Naming 均恢复，重连尝试计数
  大于 0（本次为 6），关闭终态正确；
- 常规 Release/ThinLTO 测试仍须单独运行，sanitizer 的无 LTO 构建不能替代生产形状回归。

这些结果完成 T-01 的聚焦 sanitizer 与外部实现故障注入边界。完整生产 corpus 和阶段 8 gate
仍为 `NOT_MET`。
