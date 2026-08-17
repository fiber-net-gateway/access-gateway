# 生产差分与切流证据门禁

## 1. 目的和当前状态

`verify_cutover_evidence.py` 把 D-01 的证据要求变成可执行、fail-closed 的本地门禁。它验证私有
corpus、Java/C++ 同请求差分、阶段 8 生命周期/稳定性、逐实例激活、灰度和回滚材料是否属于同一
record 和精确 revision，并逐项核对 SHA-256。

该工具只验证证据结构、关联和完整性，不执行 Java/C++ 差分、生产压测或线上演练，也不为 artifact
提供签名或审批系统。实际证据必须由受控私有环境生成并保管；仓库当前没有这些 artifact，因此当前
切流状态仍为 `NOT_MET`，不能用示例 manifest、单元测试或手写 `passed` 代替。

## 2. 结果和退出码

| 状态 | 退出码 | 含义 |
| --- | ---: | --- |
| `MET` | 0 | schema、revision、corpus 和全部 15 项 gate 均有效且通过 |
| `NOT_MET` | 1 | record 有效，但来源不是 production、corpus 不完整、workload 为空，或 gate 为 `failed`/`not_run` |
| `INVALID` | 2 | 缺字段、未知字段、revision 不符、digest/大小不符、路径越界、symlink、report 矛盾或其他结构错误 |

stdout 只输出 `access-gateway-cutover-result/v1` 的 bounded JSON：record ID、固定 gate ID、状态、
blocker 和字段级错误码。它不回显 artifact 路径、文件内容、项目、Host、Data ID 或自由文本错误。

## 3. 生成稳定 corpus 身份

配置同步工具现在除私有 `manifest.json` 外，还生成：

- `content-manifest.json`：`access-gateway-corpus/v1` canonical JSON，不含导出时间；按相对路径排序，
  记录 source tenant hash、每个配置的 kind/Data ID/group/type、字节数和 SHA-256，以及缺失
  route/gray 状态；
- `content-manifest.sha256`：上述 canonical manifest 的 SHA-256 sidecar；
- stdout 的 `corpus_sha256=<digest>`：可复制到受控的切流记录，不包含业务标识。

相同 tenant、配置字节、相对路径和缺失状态会得到相同 digest；`createdAt` 不进入该 digest。所有 dump
文件保持 `0600`，目录保持 `0700`。最终门禁会再次读取 content manifest，并逐文件验证 digest 和
字节数；只复制总 digest、却不保留对应只读输入集合，不能通过。

## 4. Evidence manifest v1

顶层私有 manifest 只允许以下字段：

```json
{
  "schema": "access-gateway-cutover-evidence/v1",
  "recordId": "production-cutover-YYYY-MM-DD",
  "source": {
    "environment": "production",
    "exportedAt": "YYYY-MM-DDThh:mm:ssZ",
    "redactionRevision": "redactor-v1"
  },
  "revisions": {
    "java": "22c2bf543b96b52c0ccecd4ceb07d4911c502f45",
    "accessGateway": "<40-or-64-lowercase-hex>",
    "fiber": "<40-or-64-lowercase-hex>"
  },
  "corpus": {
    "manifest": "corpus/content-manifest.json",
    "sha256": "<64-lowercase-hex>"
  },
  "workload": {
    "projectConfigs": 1,
    "routeItems": 1,
    "requests": 1,
    "lifecycleScenarios": 1,
    "rolloutInstances": 1
  },
  "gates": [
    { "id": "production_config_compile", "status": "not_run" }
  ],
  "approvedDifferences": []
}
```

示例只展示一个未运行项，因而不是可通过的 manifest。实际文件必须恰好包含下表全部 ID；未知、重复
或缺失 ID 都是 `INVALID`。

| Gate ID | 必需证据边界 |
| --- | --- |
| `production_config_compile` | 完整 production 配置 decode、关系校验和 compiled snapshot |
| `p0_p1_compatibility` | compatibility contract 全部 P0/P1，未通过项只能进入获批差异 |
| `java_cpp_request_differential` | Java/C++ 加载同一配置并执行同一 request corpus |
| `config_hot_update` | same-version、invalid-old-snapshot、remove/unload 和有效热更 |
| `slow_request_body` | 有界慢 body、背压和取消 |
| `connection_disconnect` | downstream/upstream 断开及 WebSocket tunnel 终止 |
| `request_timeout` | connect/request/body/flush timeout 的稳定结果 |
| `ordered_shutdown` | listener、exchange、pool/DNS、watcher/service/client 逆序关闭 |
| `latency_and_allocation` | 同版本生产形状的延迟、吞吐、分配和 CPU 基线 |
| `connection_reuse` | pool hit、失败 lease、重选和跨 worker steal 边界 |
| `memory_stability` | 规定时长/负载下 RSS、retired snapshot、queue 等稳定性 |
| `fd_stability` | 规定时长/负载及故障恢复后的 fd 基线 |
| `activation_evidence` | 目标实例报告匹配 release revision/fingerprint 的 typed active 证据 |
| `canary_rollout` | 最小真实流量灰度、观测窗口和停止条件 |
| `rollback_drill` | 创建新 rollback release、发布、逐实例激活和恢复时间 |

`workload` 五个计数都必须大于零。corpus 必须有且只有一个 project-list；route 配置数加缺失数必须
等于 `projectConfigs`；最终 `MET` 要求缺失 route 为 0，并存在 gray 配置（空规则应发布合法空对象，
不能用 Data ID 缺失代替）。

## 5. Gate report 和差异决定

`passed`/`failed` gate 必须引用一个 hashed `access-gateway-gate-report/v1` JSON：

```json
{
  "schema": "access-gateway-gate-report/v1",
  "recordId": "production-cutover-YYYY-MM-DD",
  "gateId": "java_cpp_request_differential",
  "status": "passed",
  "corpusSha256": "<same-64-lowercase-hex>",
  "revisions": {
    "java": "22c2bf543b96b52c0ccecd4ceb07d4911c502f45",
    "accessGateway": "<same-revision>",
    "fiber": "<same-revision>"
  },
  "startedAt": "YYYY-MM-DDThh:mm:ssZ",
  "completedAt": "YYYY-MM-DDThh:mm:ssZ",
  "checks": {
    "total": 100,
    "passed": 100,
    "failed": 0,
    "approvedDifferences": 0
  },
  "approvedDifferenceIds": [],
  "artifacts": [
    { "path": "artifacts/request-differential.json", "sha256": "<64-lowercase-hex>" }
  ]
}
```

每个 report 的 corpus/revision 必须与顶层 manifest 完全一致，且开始时间不得早于 corpus 导出时间。
checks 必须非空且满足 `total = passed + failed + approvedDifferences`。`passed` report 的 failed
必须为 0；`failed` report 必须至少有一个失败。每个明细 artifact 都必须是 evidence root 下非空、
非 symlink 的普通文件，最大 64 MiB，并匹配 digest。

获批差异在顶层以 `{id, decision, sha256}` 单独记录；report 的 ID 和计数必须精确引用这些决定，未
引用、重复、未知或缺少 hashed decision artifact 都会失败。验证器只确认决定 artifact 存在且未被
篡改；谁有权批准、签名和留存由外部发布治理负责。

## 6. 执行方式

以下 revision 必须来自待验收二进制/发布记录及其 pinned Fiber，而不是从相邻测试推断：

```bash
ACCESS_SERVER_CUTOVER_EVIDENCE_MANIFEST=/private/evidence/cutover-evidence.json \
ACCESS_SERVER_CUTOVER_EVIDENCE_ROOT=/private/evidence \
ACCESS_SERVER_CUTOVER_GATEWAY_REVISION='<exact revision>' \
ACCESS_SERVER_CUTOVER_FIBER_REVISION='<exact gitlink revision>' \
npm run verify:native:cutover
```

也可直接传同名 CLI 参数。相对 artifact 路径必须 canonical，不能使用绝对路径、`..`、反斜杠、
symlink 或超过大小上限的文件。manifest、report 和 corpus JSON 必须为 UTF-8，拒绝重复 key、
非标准数值和未知字段，防止歧义、拼写错误或未来字段被旧验证器静默忽略。

仓库测试只用临时、合成、无业务内容的 artifact 验证 `MET`、`NOT_MET`、`INVALID`、稳定 corpus
digest、失败/未运行 gate、获批差异、revision 漂移、hash 篡改、路径穿越、symlink 和输出脱敏；
它们不计入实际切流证据。
