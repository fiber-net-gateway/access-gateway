# 网络策略与证书配置需求及详细设计

- 状态：Implemented v5（控制面发布 + native Nacos 热更新）
- 日期：2026-08-12
- 适用范围：`server/`、`web/`、配置版本编译器和既有 native `allows` 兼容能力
- 明确未完成：逐实例证书/配置激活证据，以及生产脚本语料差分和最终切流门槛

## 1. 目标与边界

本增量补齐两个此前只有占位页的能力：

1. 在 Project 中配置可版本化的 CIDR 网络访问策略，并确定性编译到现有 route wire
   `allows` 字段；
2. 上传、校验和加密保存 TLS 证书链/私钥，以稳定逻辑证书管理不可变版本，并根据当前 leaf
   证书的 DNS SAN 自动生成 ClientHello SNI 选择索引。

网络策略属于 Route 配置事实，必须随 Configuration Version 冻结，历史版本发布时使用历史策略，
不能在 Release 创建时读取一个会漂移的 Project 当前设置。证书具有独立于 Route 的安全生命周期：
SAN 派生索引不属于 Project、不进入 route payload；Console 通过独立 TLS Release 将完整加密存储的
证书材料编译为单一 Nacos 快照。TLS
握手阶段使用 ClientHello SNI 选择证书；HTTP header 完整后才使用 Host 或 `:authority` 选择
Project，两个输入允许不同且不得在控制面中隐式绑定。

access-server 已监听独立 TLS 快照并原子热切换。Nacos 写入/readback 只证明 Release Published；
逐实例证据仍未实现，所以 UI 必须把 activation 显示为 unknown。证书版本保存、TLS Release
Published 和实例 Active 继续是三个不同事实。

## 2. 细化需求

### 2.1 网络策略

| ID          | 优先级 | 需求                                                                       |
| ----------- | ------ | -------------------------------------------------------------------------- |
| CON-NET-001 | P0     | Project 可选择由每条 Route 自行配置，或由 Project 统一提供 CIDR 策略       |
| CON-NET-002 | P0     | Project 策略分别录入允许 CIDR 和拒绝 CIDR，支持标准 IPv4/IPv6 与可选前缀   |
| CON-NET-003 | P0     | Project 策略确定性注入所有 Route；拒绝项编译为 native `!CIDR`              |
| CON-NET-004 | P0     | Project 策略生效时禁止 Route YAML 同时声明 `allows`，避免不明确的覆盖/合并 |
| CON-NET-005 | P0     | 策略作为 Configuration Version schema v3 的一部分保存、恢复、校验和发布    |
| CON-NET-006 | P0     | 无效、重复、超过 256 项或单项超过 64 字节的 CIDR 在保存/发布前失败         |
| CON-NET-007 | P0     | 空允许列表表示不启用 allowlist；空允许和拒绝列表表示公开访问               |
| CON-NET-008 | P0     | UI 对未保存、已保存、已发布和激活未知使用独立文案并提供离开保护            |
| CON-NET-009 | P1     | 在可信入口与 access-server 间定义并验证客户端地址来源，消除伪造/缺失头风险 |

`allows` 的运行时语义保持 Java 兼容：非空允许集合要求至少匹配一项，拒绝集合优先。当前 native
从 `X-Real-Ip` 取得地址，并在该头缺失或不可解析时跳过 CIDR 检查。部署必须由受信任入口清洗并
规范设置该头；在 CON-NET-009 完成前，Console 不应把该策略描述为不依赖部署前提的网络防火墙。
本次不改变这一 wire/runtime 兼容行为。

### 2.2 证书配置

| ID           | 优先级 | 需求                                                                            |
| ------------ | ------ | ------------------------------------------------------------------------------- |
| CON-CERT-001 | P0     | 接收 leaf-first PEM 证书链和一把未加密 PEM 私钥，限制请求大小                   |
| CON-CERT-002 | P0     | 校验 PEM、链顺序/签名、leaf 与私钥匹配、当前有效期和 DNS SAN                    |
| CON-CERT-003 | P0     | exact SAN 精确匹配；`*.example.com` 只覆盖一层子域名                            |
| CON-CERT-004 | P0     | 证书链与私钥分别 envelope-encrypted；API 永不返回 PEM、文档 ID 或密钥定位信息   |
| CON-CERT-005 | P0     | 逻辑证书具有稳定 ID 和名称；其 PEM/私钥版本不可变，并以环境内 SHA-256 指纹去重  |
| CON-CERT-006 | P0     | 当前 leaf DNS SAN 自动形成只读 SNI selector；不维护第二套人工绑定               |
| CON-CERT-007 | P0     | ClientHello SNI 按 exact 优先、单层 wildcard selector 选择逻辑证书              |
| CON-CERT-008 | P0     | 环境内相同 SAN selector 只能属于一个活动逻辑证书；冲突时整个写事务失败          |
| CON-CERT-009 | P0     | 更新创建新版本并原子切换 current version 和 SAN 索引；范围变化必须显式确认      |
| CON-CERT-010 | P0     | 事实状态区分 valid、30 天内 expiring、expired、superseded 和 activation unknown |
| CON-CERT-011 | P0     | 创建逻辑证书和新增版本写入脱敏审计，审计不保存 PEM 或私钥                       |
| CON-CERT-012 | P0/P1  | Nacos 完整快照原子热更新已完成；逐实例指纹证据仍为 P1                           |
| CON-CERT-013 | P0     | 证书内容校验不使用业务流量灰度；运行时部署灰度与证书是否合法是两个独立问题      |

上传不接受尚未生效或已经过期的证书；当前版本随时间可变为 `expiring`/`expired`。续期在同一
逻辑证书下创建新版本，旧版本标记为 `superseded` 并保留指纹与审计。新版本可以包含额外 SAN，
当前版本 DNS SAN 是控制面的权威 SNI 范围。普通续期 SAN 不变时直接切换；如果增加或删除 SAN，
API 返回影响明细并要求 `confirmSniCoverageChange=true` 后才能原子更新。私钥销毁期限仍需在运行时
交付设计中确定。

上述上传校验能在接收证书时确定 PEM 可解析、所提供链条的相邻签名关系、leaf/key 匹配、当时的
有效期和 DNS SAN，不依赖真实业务流量。到期状态仍随时间推进，必须持续计算和提醒；公共 CA 信任、
吊销状态或企业私有信任策略也不能仅由这组本地检查证明，后续交付设计应定义独立的 trust policy。
证书内容不做按请求灰度；未来若为降低运行时发布风险进行实例批次部署，应作为 deployment rollout
记录和验证，不能反向改变证书版本或 SAN selector 的事实状态。

## 3. 配置模型与编译

Configuration Version 模型升级为 schema v3：

```json
{
  "schemaVersion": 3,
  "kind": "project_routes_yaml",
  "networkPolicy": {
    "source": "project",
    "allowedCidrs": ["10.0.0.0/8", "2001:db8::/32"],
    "deniedCidrs": ["10.1.0.0/16"]
  },
  "routes": []
}
```

- `source=route`：逐条 YAML 的 `allows` 原样进入 wire；Project 两个列表不参与编译。
- `source=project`：任何 Route 自带 `allows` 都返回 `ROUTE_NETWORK_POLICY_CONFLICT`；编译器把
  `allowedCidrs` 后接带 `!` 的 `deniedCidrs`，注入每条 Route。
- CIDR 先做控制面标准 IPv4/IPv6 语法和重复检查，再由 Native Validator 使用仓库自有 codec/
  compiled route model 权威校验。
- schema v1 whole-project JSON 和 schema v2 YAML 读取时确定性升级到 schema v3，并设为
  `source=route`，从而保持既有 route `allows` 行为。旧加密文档不被原地重写。
- 编译器 revision 为 `project-routes-yaml-v3-network-policy`；Release 继续冻结输入摘要、编译器
  revision、wire version、精确 payload 和 Native Validator revision。

切换到 Project 策略不自动删除 YAML 中的 `allows`，而是 fail closed 并要求用户确认修改对应
Route。这样不会因 UI 切换静默放宽或覆盖已有策略。

## 4. 证书存储、SNI 解析与 API

Migration `0006_network_policies_and_certificates` 最初新增不可变证书和 Project 绑定。
Migration `0007_certificate_series_and_automatic_resolution` 在不改写既有密文和审计的前提下引入：

- `certificate_series`：稳定逻辑证书、名称、当前版本和乐观锁；
- `certificates`：不可变版本、公有元数据、证书链/私钥加密文档外键、版本号和事实生命周期；
- `certificate_dns_names`：最初由证书 SAN 自动产生的 exact/wildcard 选择器；
- `certificate_bindings`：只保留迁移前的历史证据；新代码不再读取或写入。

Migration `0008_tls_sni_rules` 曾为 `certificate_dns_names` 增加显式规则元数据。为保持已经执行的
checksum 历史不变，Migration `0009_certificate_san_selectors` 新建
`certificate_san_selectors`，从每个逻辑证书当前版本的 DNS SAN 重建只读 selector，并以
`(environment_id, dns_name)` 唯一约束拒绝歧义。`certificate_dns_names` 自此只作为兼容历史保留，
新代码不再读取或写入。

证书链与私钥分别写入 `config_documents`，purpose 为 `certificate_chain` 和
`certificate_private_key`。上传事务、元数据和审计要么全部提交，要么全部回滚。服务端只在解析
与加密所需的短生命周期内持有 PEM；没有私钥读取或下载 API。

| Method | Path                                        | 权限             | 行为                                |
| ------ | ------------------------------------------- | ---------------- | ----------------------------------- |
| GET    | `/api/certificates`                         | read             | 返回独立逻辑证书及当前版本          |
| POST   | `/api/certificates`                         | admin/maintainer | 创建 V1 并自动生成 SAN selector     |
| GET    | `/api/certificates/:certificateId/versions` | read             | 返回不可变版本历史                  |
| POST   | `/api/certificates/:certificateId/versions` | admin/maintainer | 原子更新版本和 SAN selector         |
| GET    | `/api/tls/sni-resolution?serverName=...`    | read             | 预览 ClientHello SNI 控制面解析结果 |
| GET    | `/api/tls/releases`                         | read             | 返回不可变 TLS Release 历史         |
| POST   | `/api/tls/releases`                         | admin/publisher  | 冻结当前证书并选择默认证书          |
| POST   | `/api/tls/releases/:id/publications`        | admin/publisher  | 排队写入 Nacos 并执行摘要 readback  |

新增版本请求必须用 `If-Match: "<lockVersion>"` 提交逻辑证书当前锁版本。证书创建和版本更新都先
锁定环境，再锁定目标对象，并在同一事务内检查 selector 唯一性、写入不可变版本、替换 SAN 索引和
切换 current version。锁冲突返回 409，客户端必须重新读取后再决定。

稳定业务错误包括 `INVALID_CERTIFICATE`、`CERTIFICATE_ALREADY_EXISTS`、
`CERTIFICATE_VERSION_CONFLICT`、`CERTIFICATE_SNI_NAME_CONFLICT` 和
`CERTIFICATE_SNI_COVERAGE_CONFIRMATION_REQUIRED`。请求 schema、Fastify 错误与日志不得回显
body。跨 workspace 或无权对象继续返回 404，防止枚举。

## 5. Web 交互

- 顶层 TLS 页面展示独立逻辑证书、当前版本 DNS SAN、当前/历史版本和 activation unknown，并提供
  新建、版本更新、默认证书选择和 TLS 快照发布入口。
- 同一页面提供只读 SNI 解析预览，不提供规则创建、切换或删除。SAN 范围变化时展示新增/停止覆盖
  的域名并要求确认。
- Project 列表、详情 DTO、导航和页面不再包含证书字段或证书入口。
- Project / Network Policy 页面编辑策略所有权、允许/拒绝 CIDR 和版本说明；保存复用
  Configuration Version 乐观锁与幂等写入，并阻止有未保存修改时离开。
- 私钥输入成功后立即从 React state 清空；页面、响应、列表和错误中都不显示私钥。
- 状态均带文字，键盘可操作；窄屏下表单和元数据布局降为单列。

## 6. 安全、故障与后续工作

- PEM、私钥和 CIDR 都是不可信输入；解析失败 fail closed，不调用 shell/OpenSSL，不发出网络请求。
- 证书 fingerprint 是公有标识，不代替私钥保密；数据库备份必须和 KEK 分权保护。
- 当前本地 KEK 实现仍是部署级能力；生产应接入 Secret Provider、轮换和销毁证明。
- 逻辑证书创建或版本更新不自动创建 Release、不写 Nacos，也不证明任何 access-server 已加载证书。
- TLS Release 固定写入 `ploto.unified-access.tls-certificates` / `ACCESS-SERVER`。payload 在数据库中
  加密，Nacos readback 和 API 只暴露摘要；Nacos 本身持有握手所需明文私钥，因此生产必须启用受控
  网络、服务端鉴权和传输加密，并限制读取权限。
- native 在 Nacos owner loop 上限制 4 MiB/128 证书/8192 SAN，解析链与私钥、检查当前有效期、
  构建 TCP/QUIC context 和排序索引后一次原子发布。空值、NotFound、非法候选、低 version 与
  same-version/different-content 都保留旧快照。
- ClientHello 热路径只做原子读、ASCII 无分配比较与有序数组二分查找；每 worker 预分配 hazard
  slot 延长旧快照到 Fiber 完成 `SSL_set_SSL_CTX`，不使用 mutex、shared_ptr refcount 或字符串分配。
- 逐实例 fingerprint 回执仍未实现，因此 Release Published 后 activation 仍为 unknown。
- 网络策略仍受当前 `X-Real-Ip` Java 兼容语义约束。可信代理边界、直接连接行为和 header
  spoofing 防护必须作为后续跨组件安全增量处理。

## 7. 验收与验证

最低自动化覆盖：

- schema v1/v2 到 v3 的确定性升级；
- Project 策略注入每条 Route、deny 编译、无效/重复 CIDR、Route 冲突；
- Native Validator 对最终 wire payload 的既有 CIDR 校验；
- PEM/key 匹配、mismatch、过期拒绝、exact 优先和单层 wildcard；
- 相同 SAN 续期直接成功；SAN 范围变化未确认时失败且旧版本、旧索引保持不变；
- SNI exact 优先于单层 wildcard，重复 selector fail closed，不按顺序选择；
- 私钥不出现在响应模型、审计 summary、日志或 readback 证据；只存在于加密文档和专用 Nacos payload；
- Web 保存网络策略时创建新版本；TLS 页面把库存预览、Release Published 与 activation unknown 分开显示，
  Project 页面没有证书耦合。

本增量必须执行 console 全矩阵以及 native configure/build/focused CTest，并验证 Compose 配置渲染
和 TLS 握手。上述测试不能替代生产脚本语料差分与最终切流门槛。
