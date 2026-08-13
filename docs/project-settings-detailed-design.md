# Project Settings 详细设计

## 1. 设计摘要

本设计在现有 Project Release 与 publication worker 上增加 `project_decommission` Release。创建阶段
先回读 rnacos Project List，再用一个短 MySQL 事务冻结下线计划、base observation、target payload
和审计事件。发布阶段复用现有环境级 lease、冲突判断、写入及回读。worker 只有在全部必需资源
verified 后，才把 Project 从 `decommissioning` 改为 `archived`。

数据流：

```text
Settings confirmation
  -> ReleaseService preflight reads Project List
  -> ReleaseRepository transaction
       release(kind=project_decommission, ready)
       release_item(decommission plan)
       observation(base Project List)
       release_resource(target Project List)
       project(status=decommissioning)
       audit(project.decommission_requested)
  -> existing publication queue
  -> worker base/target decision, write, readback
  -> worker transaction
       release(status=published)
       project(status=archived, archived_at=now)
       audit(publication.completed, project.archived)
```

rnacos 与 MySQL 不组成事务。任何外部变化均按 observation/attempt 记录，不能用数据库回滚伪造外部
回滚。

## 2. 数据模型

新增 migration `0010_project_decommission_releases.sql`：

- `releases.native_validator_contract` 改为 nullable；
- `releases.native_validator_revision` 改为 nullable。

原因：下线 Release 不编译 Route，也不调用 Native Validator。继续写入虚假的 validator revision 会
污染证据。现有 `project_route` 和 `tls_certificates` Release 仍写入非空值。

不新增 Project 表列。`projects.status` 是现有字符串列，增加以下合法应用值：

- `active`；
- `decommissioning`；
- `archived`。

`projects.archived_at` 只在最终 published 事务内设置。Project 创建下线 Release 时只改变 status，因而
仍可在默认列表和详情页看到发布进度。

### 2.1 Release

下线 Release 写入：

| 字段                              | 值                        |
| --------------------------------- | ------------------------- |
| `kind`                            | `project_decommission`    |
| `status`                          | `ready`                   |
| `compiler_revision`               | `project-decommission-v1` |
| `current_revision_id_at_creation` | 当前版本 ID，可为空       |
| `native_validator_*`              | `NULL`                    |
| `validation_errors_json`          | `[]`                      |

`release_items.kind=project_decommission`，`draft_revision_id` 和
`allocated_project_version` 均可为空。`model_document_id` 指向加密的不可变计划：

```json
{
  "schemaVersion": 1,
  "kind": "project_decommission",
  "projectId": "uuid",
  "domain": "api.example.com",
  "reason": "域名已迁移到新入口"
}
```

列表 API 不返回计划正文，只返回其 SHA-256。原因使用现有 Release description 返回，长度受限。

### 2.2 Release resource

`release_resources` 写入一个必需资源：

- `kind=project_list`；
- `operation=upsert`；
- `publish_order=10`；
- `required_resource=true`；
- `payload_document_id` 指向加密 target 文本；
- `base_observation_id` 指向创建前回读；
- `target_sha256` 是精确 target 文本摘要；
- `allocated_project_version=NULL`。

P0 不写 `project_route/remove` 资源，因此无需扩展 Nacos client 的删除操作。后续 route Data ID 清理
必须作为独立维护 Release 或独立可重试资源设计，不能修改已发布下线 Release。

## 3. Project List 编译

`compileDecommissionProjectList(baseContent, domain)`：

1. base 不存在时按空列表处理；
2. 按现有 Console 兼容逻辑以分号切分、trim、丢弃空项并去重；
3. 如果列表包含 domain，移除后按字典序输出分号分隔文本；
4. 如果列表不包含 domain，返回原始 base content，避免幂等操作重排无关外部内容；
5. target 永不包含被下线 domain。

Project List 不是 JSON。函数不得把它序列化为 JSON 数组。

## 4. 服务与仓储边界

### 4.1 ReleaseService

新增：

```ts
createDecommission(
  actor,
  projectId,
  { confirmationDomain, reason, expectedLockVersion, idempotencyKey },
  requestId,
): Promise<ProjectReleaseView>
```

顺序：

1. 查找 active/decommissioning Project 和环境 membership；
2. 仅允许 admin；
3. 严格校验确认域名和原因；
4. 检查 Nacos client available；
5. 取得部署环境的 rnacos target 和 Data ID；
6. 在 MySQL 事务外回读 Project List；
7. 编译 target；
8. 调用 repository 原子创建 Release。

步骤 6 失败不打开写事务，不改变 Project。步骤 6 与最终写入之间的外部竞态由 worker 对 frozen base
的冲突检测处理。

### 4.2 ReleaseRepository

`beginDecommission` 在一个 READ COMMITTED 事务中：

1. 先按 `(environment_id, created_by, idempotency_key)` 查询重放；digest 不同返回
   `IDEMPOTENCY_KEY_REUSED`；
2. `SELECT ... FOR UPDATE` 锁 Project；
3. 比较 `lock_version`，只接受 active/decommissioning；
4. 查询该 Project 是否存在 `ready/queued/publishing/creating/validating` Release；存在则返回
   `PROJECT_DECOMMISSION_IN_PROGRESS`；
5. 锁环境并分配单调 Release sequence；
6. 加密并插入计划、base observation 和 target payload；
7. 插入 Release、item、resource；
8. 更新 Project 为 decommissioning、递增 lock；
9. 写 `project.decommission_requested` 审计。

事务失败不会留下 decommissioning 状态或不完整 Release。

### 4.3 ProjectRepository

读取分成两种意图：

- operational identity：只返回 `status=active`，供 Route、Version 和普通 Release 写操作使用；
- historical identity：返回 active、decommissioning 和 archived，供 Release 历史读取及下线发布授权
  使用。

Project list/detail 返回 archived_at 为空的 active/decommissioning Project。默认 Projects 列表不返回
archived。

## 5. Publication worker

资源写入和 readback 沿用现有逻辑。完成事务增加 Release kind 分支：

1. 聚合状态为 published；
2. 若 `release.kind=project_decommission`：
   - 找到其 `release_items.kind=project_decommission` 的 Project；
   - 将此前该 Project 的 published project route Release 标记为 superseded；
   - `UPDATE projects SET status='archived', archived_at=now, lock_version=lock_version+1`；
   - 写 `project.archived` 审计；
3. 再完成 `publication.completed` 和 lease 清理。

这些数据库更新位于同一事务。只有资源全部 verified 才执行。`publish_failed` 或
`partially_published` 不改 archived_at。

Project List 回读并不证明实例卸载。worker 不写 activation observations，也不改变
`activationStatus=unknown`。

## 6. API schema

`ProjectReleaseView` 增加：

```ts
kind: 'project_route' | 'project_decommission'
```

并把只适用于 route Release 的字段改为 nullable：

- `sourceConfigurationVersion`；
- `currentConfigurationVersionAtCreation`；
- `allocatedWireVersion`；
- `nativeValidator`。

`sourceModelSha256` 对 route Release 表示配置模型摘要，对 decommission Release 表示下线计划摘要。
`resources.operation` 继续显式返回，UI 不根据 kind 推断写入还是删除。

创建接口同时要求 `If-Match` 和 `Idempotency-Key`。`If-Match` 只接受带双引号的非负十进制整数，
与 Project `lockVersion` 比较。

## 7. 前端设计

新增 `ProjectSettingsPage` 替换 unavailable 占位页。

页面布局：

1. header：Settings、用途说明、生命周期 chip；
2. Project identity card：域名、UUID、创建/更新时间、不可改名说明；
3. lifecycle evidence card：当前版本、rnacos 来源、实例激活 unknown；
4. 最新下线 Release card：kind、序号、Release 状态、Project List 资源状态和“继续发布”；
5. Danger Zone：影响列表、能力提示和确认按钮；
6. dialog：原因、完整域名确认、取消和提交。

提交步骤：

1. 前端做非权威必填与域名匹配检查；
2. POST 创建 Release；
3. POST queue publication；
4. 成功后导航到 Project Releases 页观察资源证据；
5. 若步骤 2 成功而步骤 3 失败，保留错误。重新进入 Settings 时从 Release 列表发现 ready Release 并
   提供“继续发布”，不得重复创建。

publicationWorker capability 不是 ready 时禁用创建/继续发布按钮。activationCollector 不可用只影响
证据文案，不阻止 rnacos 下线发布。

Release timeline 对 decommission Release 显示“下线 Project”，不显示来源 V 或 wire version；资源
同时显示 `operation`。

## 8. 并发、幂等与故障

- 两个管理员同时创建：Project row lock 和非终态 Release 查询只允许一个成功；
- 普通 Configuration Version、旧 Draft 写入口和 route Release 创建也先锁同一 Project row 并再次
  检查 `status=active`，避免在 Service 预检后与下线事务并发穿越；
- Project lock 已变化：返回 `PROJECT_VERSION_CONFLICT`，客户端刷新后重新确认；
- 相同请求重放：返回相同 Release，不重复分配 sequence；
- 创建成功、排队失败：Release 保持 ready，页面可继续排队；
- worker 写入前外部改变 base：resource conflict，Project 保持 decommissioning；
- worker 写入后回读不一致：保留 attempt 事实，Project 不归档；
- 失败后重试：创建新的下线 Release并重新回读 base；不修改旧 Release；
- worker 崩溃恢复仍属于仓库已知开放项，文档和 UI 不宣称已完成。

## 9. 测试设计

后端：

- Project List target 编译：存在、已不存在、空列表、去重；
- 确认域名和原因校验；
- admin 创建与 publisher/maintainer 拒绝；
- Nacos unavailable/preflight failure fail closed；
- repository lock、幂等、非终态冲突、事务回滚和审计；
- API schema、If-Match、Idempotency-Key 与 unavailable service；
- worker published 分支归档 Project，失败/部分发布不归档。

前端：

- Settings 不再显示 unavailable；
- 只读身份和 activation unknown；
- 域名不匹配时按钮禁用；
- 创建请求携带 If-Match，随后排队并导航 Releases；
- ready 下线 Release 可以继续发布；
- publication worker 未配置时 fail closed；
- 键盘可访问 dialog，关闭后焦点回到触发按钮。

最小验证：`npm run typecheck`、`npm test`、`npm run format:check`、`npm run build`。
