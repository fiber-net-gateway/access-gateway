# Project Settings 需求文档

## 1. 背景与目标

Project Settings 是域名 Project 的低频生命周期操作入口，不是通用配置页。Project 主域名不可变，
关联域名在 Routes 页面维护；Route、HTTPS redirect
和 CIDR 策略已经分别由 Routes 与 Network Policy 管理；rnacos 连接、Data ID、成员和系统能力属于
部署级 System。Settings 首个业务可用版本聚焦于安全、可审计地将一个 Project（及其全部 Host）从运行配置中下线并归档。

本增量目标：

1. 展示不可编辑的 Project 身份和三类独立状态；
2. 由管理员创建不可变的下线 Release；
3. 由管理员或发布者把下线 Release 加入既有发布队列；
4. 先从 rnacos Project List 移除主域名并回读，再把 Project 标记为 archived；route resource 中的全部 Host 随之停止服务；
5. 保留配置版本、Release、发布证据和审计历史；
6. 只有逐实例精确证据可证明 activation；没有目标、没有证据或证据过期时报告 unknown。

## 2. 非目标

- 不在 Settings 中编辑主域名或关联域名。主域名是 exact Host 和 rnacos project key；改名应通过复制到
  新主域名并单独下线旧 Project 完成。关联域名在 Routes 页面与配置版本一起保存。
- 不在 Settings 中编辑 Route、HTTPS、CIDR、证书、rnacos endpoint、namespace、tenant、Data ID
  或凭据。
- 不通过空 Route、空 Host 或空 YAML 表达下线。
- 不把 rnacos 写入、回读、进程存活或请求成功当作实例激活证据。
- 本阶段不恢复 archived Project，也不删除历史记录。
- 本阶段不删除 per-project route Data ID。Project List 移除并回读是必需下线资源；route Data ID
  清理是后续独立、可选且需要单独证据的维护动作。
- 不补做通用 Release retry/cancel 或 publication worker 崩溃恢复；失败后允许创建新的下线 Release，
  且新 Release 必须基于新的 rnacos 回读事实。

## 3. 用户与权限

| 操作                        | Admin | Maintainer | Publisher | Auditor |
| --------------------------- | ----- | ---------- | --------- | ------- |
| 查看 Settings               | 是    | 是         | 是        | 是      |
| 创建下线 Release            | 是    | 否         | 否        | 否      |
| 发布已创建的下线 Release    | 是    | 否         | 是        | 否      |
| 查看 Release 和脱敏发布证据 | 是    | 是         | 是        | 是      |

服务端每次操作重新查询角色。前端隐藏或禁用按钮不能替代服务端授权。OIDC 和重新认证尚未实现时，
页面不得模拟重新认证已经发生。

## 4. 页面需求

### 4.1 Project 信息

Settings 展示：

- 规范化域名；
- Project UUID，可复制；
- 生命周期状态 `active` 或 `decommissioning`；
- 当前 Configuration Version；
- 最近 rnacos 已发布来源版本；
- 实例激活状态为 unknown、pending、active 或 degraded；
- 创建时间、最后更新时间。

域名以只读形式展示，并解释为什么不能原地修改。

### 4.2 下线状态

当最新下线 Release 存在时，页面展示 Release 序号、状态和 Project List 资源状态：

- `ready`：Release 已冻结但未排队，允许管理员或发布者继续发布；
- `queued` / `publishing`：发布进行中，禁止创建另一个下线 Release；
- `published`：rnacos Project List 已回读一致，Project 归档；
- `publish_failed` / `partially_published` / `abandoned`：展示事实并允许管理员基于新回读创建新的
  下线 Release；
- `activationStatus` 独立于 Release 发布状态展示；无有效实例证据时为 unknown。

### 4.3 Danger Zone

`active` Project 显示“下线并归档 Project”。`decommissioning` Project 若没有可继续发布的 ready
Release，且上一 Release 已终止失败，则显示“重新创建下线 Release”。

确认对话框必须：

1. 说明主域名将从 `ploto.unified-access.projects`（或部署配置的 Project List Data ID）移除，
   并说明 route resource 中的关联域名也会随 Project 下线；
2. 说明 access-server 应据此取消订阅并卸载运行快照，但没有实例证据时仍无法证明每个实例已执行；
3. 说明历史版本、Release 和审计不会删除；
4. 说明浏览器中的未保存内容不会进入下线 Release；
5. 要求输入完整、规范化主域名；
6. 要求填写 1 至 1000 个字符的下线原因；
7. 最终按钮明确标为“创建并发布下线 Release”。

发布能力未配置时按钮禁用并展示原因，不能模拟成功。

## 5. 生命周期与事实语义

```text
active
  -> decommissioning     创建 ready 下线 Release
  -> decommissioning     queued / publishing / failed / partially published
  -> archived            Project List 移除写入并回读一致
```

- `decommissioning` 是控制面操作状态，不是运行时下线证据。
- `archived` 表示控制面已完成必需 rnacos 资源发布并把 Project 从默认业务列表隐藏。
- `published` 只证明 Project List 目标内容已写入或本来一致，并完成摘要回读。
- `activation unknown` 表示没有目标、尚无或已经过期的具体 access-server 实例类型化证据。
- 发布部分成功或失败时 Project 保持 `decommissioning`，不得回滚已经发生的外部写入，也不得标记为
  archived。

## 6. 发布语义

创建 Release 前只读回读 Project List，冻结以下事实：

- Data ID、Group；
- base 是否存在、rnacos MD5、Console SHA-256 和加密 base payload；
- 移除主域名后的精确 target payload 与 SHA-256；route resource 中的全部 Host 会随 Project
  订阅移除；
- Project ID、主域名、下线原因和预期 Project lock version。

下线 Release 只有一个 P0 必需资源：

| 顺序 | kind           | operation | 说明                                 |
| ---- | -------------- | --------- | ------------------------------------ |
| 10   | `project_list` | `upsert`  | 写入移除主域名后的分号分隔列表并回读 |

如果主域名在 base 中已经不存在，仍生成 Release。target 保留原始 base 内容，worker 通过
`already_at_target` 完成幂等验证；Project route resource 中的主域名和全部 alias 由同一次
Project 订阅移除。rnacos base 在创建后发生变化时按既有外部冲突规则 fail closed。

## 7. API 需求

### 7.1 创建下线 Release

`POST /api/projects/:projectId/decommission-releases`

Headers：

- `If-Match: "<project lockVersion>"`
- `Idempotency-Key: <1..128 printable ASCII>`

Body：

```json
{
  "confirmationDomain": "api.example.com",
  "reason": "域名已迁移到新入口"
}
```

成功返回 `201` 和 `ProjectReleaseView`，其中：

- `kind=project_decommission`；
- route 配置来源、wire version 和 Native Validator 为 `null`；
- `sourceModelSha256` 是不可变下线计划文档摘要；
- `resources` 包含 Project List 资源；
- `activationStatus` 由逐实例证据独立计算，缺失时为 unknown。

稳定错误：

| code                               | HTTP | 含义                         |
| ---------------------------------- | ---- | ---------------------------- |
| `PROJECT_CONFIRMATION_MISMATCH`    | 422  | 确认域名不完全匹配规范化域名 |
| `INVALID_DECOMMISSION_REASON`      | 422  | 原因为空或超过上限           |
| `PROJECT_VERSION_CONFLICT`         | 409  | Project lock 已变化          |
| `PROJECT_DECOMMISSION_IN_PROGRESS` | 409  | 已有非终态下线/配置 Release  |
| `PROJECT_ARCHIVED`                 | 409  | Project 已归档               |
| `PUBLICATION_UNCONFIGURED`         | 503  | rnacos publication 未配置    |
| `NACOS_PREFLIGHT_FAILED`           | 503  | Project List 只读回读失败    |

### 7.2 发布

继续使用 `POST /api/releases/:releaseId/publications`。既有环境级 lease、逐资源 attempt、base 冲突
检测、回读和审计语义不变。

### 7.3 查询

`GET /api/projects/:projectId/releases` 同时返回 `project_route` 与 `project_decommission` Release。
Release 查询按环境 membership 授权，并允许读取已归档 Project 的历史 Release；普通 Project 编辑
接口仍拒绝已归档资源。

## 8. 审计与安全

必须记录：

- `project.decommission_requested`：Project、Release、域名、原因摘要和 lock version；
- `publication.queued` 与 `publication.completed`：沿用既有事件；
- `project.archived`：仅在 Project List 资源 verified 后记录。

原因可进入审计，但必须限制长度；事件不得包含 rnacos 凭据、Authorization、Cookie、Route body、
敏感 header 或任何解密配置正文。base/target payload 继续加密存储，只在 publication adapter 所需的
短生命周期中解密。

## 9. 验收场景

1. active Project 的 Settings 显示只读身份、三种独立状态和 Danger Zone。
2. 域名或原因不合法时前后端均阻止创建，下线 Release 和 rnacos 均无变化。
3. 非管理员不能创建下线 Release；非管理员/发布者不能排队发布。
4. 正确确认后创建 immutable `project_decommission` Release，Project 进入 decommissioning。
5. worker 只写部署配置的 Project List Data ID；目标内容不包含该域名，且不写空 route payload。
6. Project List 写入并回读一致后 Release 为 Published、Project 为 archived、默认列表不再返回它。
7. 上一步之后 UI 在 collector 收到精确卸载证据前显示 pending/unknown，不提前显示“已生效”。
8. rnacos base 外部变化时不覆盖，Release 失败且 Project 保持 decommissioning。
9. 请求在创建后、排队前中断时，Settings 展示 ready Release 并允许继续发布。
10. 失败后重新创建会重新回读 rnacos，并生成新 Release；旧 Release 不修改。
11. 同一 Idempotency-Key 和同一请求返回原 Release；不同请求复用 key 返回冲突。
12. 配置版本、旧 Release、发布 attempt 和审计外键在归档后完整保留。
