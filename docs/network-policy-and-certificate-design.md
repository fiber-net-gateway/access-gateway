# 网络策略与证书配置需求及详细设计

- 状态：Implemented v1（控制面 P0）
- 日期：2026-08-12
- 适用范围：`server/`、`web/`、配置版本编译器和既有 native `allows` 兼容能力
- 明确未完成：逐域名 SNI 证书热更新、证书安全交付、逐实例证书/配置激活证据

## 1. 目标与边界

本增量补齐两个此前只有占位页的能力：

1. 在 Project 中配置可版本化的 CIDR 网络访问策略，并确定性编译到现有 route wire
   `allows` 字段；
2. 上传、校验和加密保存 TLS 证书链/私钥，维护证书库存，并把 DNS SAN 覆盖的证书绑定到
   Project。

网络策略属于 Route 配置事实，必须随 Configuration Version 冻结，历史版本发布时使用历史策略，
不能在 Release 创建时读取一个会漂移的 Project 当前设置。证书具有独立于 Route/rnacos 的安全
生命周期：证书绑定不会进入 route payload，私钥绝不通过 rnacos 分发。

本次不增加 access-server 动态 SNI 能力。因此 UI 必须分别显示“证书已绑定 / SAN 已覆盖”和
“运行时部署未接入”，不能显示“已部署”或“已激活”。配置保存、Release Published 和实例 Active
继续是三个不同事实。

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

| ID           | 优先级 | 需求                                                                             |
| ------------ | ------ | -------------------------------------------------------------------------------- |
| CON-CERT-001 | P0     | 接收 leaf-first PEM 证书链和一把未加密 PEM 私钥，限制请求大小                    |
| CON-CERT-002 | P0     | 校验 PEM、链顺序/签名、leaf 与私钥匹配、当前有效期和 DNS SAN                     |
| CON-CERT-003 | P0     | exact SAN 精确匹配；`*.example.com` 只覆盖一层子域名                             |
| CON-CERT-004 | P0     | 证书链与私钥分别 envelope-encrypted；API 永不返回 PEM、文档 ID 或密钥定位信息    |
| CON-CERT-005 | P0     | 证书版本不可变，以环境内 SHA-256 指纹去重，展示 subject/issuer/SAN/有效期/指纹   |
| CON-CERT-006 | P0     | 一个证书可绑定多个 Project；一个 Project 同时只有一个当前绑定，并保留绑定历史    |
| CON-CERT-007 | P0     | 仅 `valid`/`expiring` 且覆盖 Project domain 的证书可以绑定                       |
| CON-CERT-008 | P0     | 事实状态区分 valid、30 天内 expiring、expired、superseded 和 runtime unsupported |
| CON-CERT-009 | P0     | 创建、绑定、替换绑定和解绑写入脱敏审计，审计不保存 PEM 或私钥                    |
| CON-CERT-010 | P1     | 专用双向鉴权交付通道向逐域名 SNI store 原子热更新，并收集实例指纹证据            |

上传不接受尚未生效或已经过期的证书；已入库证书随时间可变为 `expiring`/`expired`。替换通过上传
新的不可变证书并改变当前绑定完成，旧证书和旧绑定证据不被改写。私钥销毁期限和
`superseded` 自动化策略仍需在运行时交付设计中确定。

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

## 4. 证书存储与 API

Migration `0006_network_policies_and_certificates` 新增：

- `certificates`：不可变公有元数据、证书链/私钥加密文档外键、创建人和事实生命周期；
- `certificate_bindings`：Project/Certificate 绑定历史，`unbound_at IS NULL` 表示当前绑定；生成列
  `active_project_id` 的唯一索引保证一个 Project 只有一个当前绑定。

证书链与私钥分别写入 `config_documents`，purpose 为 `certificate_chain` 和
`certificate_private_key`。上传事务、元数据和审计要么全部提交，要么全部回滚。服务端只在解析
与加密所需的短生命周期内持有 PEM；没有私钥读取或下载 API。

| Method | Path                                   | 权限             | 行为                                     |
| ------ | -------------------------------------- | ---------------- | ---------------------------------------- |
| GET    | `/api/certificates`                    | read             | 返回固定 workspace 的公有证书库存        |
| POST   | `/api/certificates`                    | admin/maintainer | 校验并创建不可变证书版本                 |
| GET    | `/api/projects/:projectId/certificate` | read             | 返回当前绑定、覆盖与 runtime unsupported |
| PUT    | `/api/projects/:projectId/certificate` | admin/maintainer | 校验 SAN/状态后原子替换当前绑定          |
| DELETE | `/api/projects/:projectId/certificate` | admin/maintainer | 关闭当前绑定并保留历史                   |

稳定业务错误包括 `INVALID_CERTIFICATE`、`CERTIFICATE_ALREADY_EXISTS`、
`CERTIFICATE_NOT_BINDABLE` 和 `CERTIFICATE_DOMAIN_NOT_COVERED`。请求 schema、Fastify 错误与日志
不得回显 body。跨 workspace 或无权对象继续返回 404，防止枚举。

## 5. Web 交互

- 顶层 Certificates 页面展示库存、有效期、SAN、指纹摘要、绑定数和运行时未接入状态，并提供
  PEM 上传。
- Project / Certificate 页面展示当前绑定、SAN 覆盖、有效期和完整指纹；可从库存绑定、解绑，
  或上传新版本后绑定。
- Project / Network Policy 页面编辑策略所有权、允许/拒绝 CIDR 和版本说明；保存复用
  Configuration Version 乐观锁与幂等写入，并阻止有未保存修改时离开。
- 私钥输入成功后立即从 React state 清空；页面、响应、列表和错误中都不显示私钥。
- 状态均带文字，键盘可操作；窄屏下表单和元数据布局降为单列。

## 6. 安全、故障与后续工作

- PEM、私钥和 CIDR 都是不可信输入；解析失败 fail closed，不调用 shell/OpenSSL，不发出网络请求。
- 证书 fingerprint 是公有标识，不代替私钥保密；数据库备份必须和 KEK 分权保护。
- 当前本地 KEK 实现仍是部署级能力；生产应接入 Secret Provider、轮换和销毁证明。
- 证书上传/绑定成功不创建 Release，不写 rnacos，也不证明任何 access-server 已加载证书。
- 动态 SNI 需要 native 的不可变证书快照、原子替换、失败保留旧证书、有界安全 API、逐实例
  fingerprint 回执和有序关闭；该工作必须单独完成 native/Console/部署测试矩阵。
- 网络策略仍受当前 `X-Real-Ip` Java 兼容语义约束。可信代理边界、直接连接行为和 header
  spoofing 防护必须作为后续跨组件安全增量处理。

## 7. 验收与验证

最低自动化覆盖：

- schema v1/v2 到 v3 的确定性升级；
- Project 策略注入每条 Route、deny 编译、无效/重复 CIDR、Route 冲突；
- Native Validator 对最终 wire payload 的既有 CIDR 校验；
- PEM/key 匹配、mismatch、过期拒绝、exact/单层 wildcard SAN；
- 私钥不出现在响应模型、审计 summary 或 rnacos payload；
- Web 保存网络策略时创建新版本；证书页面把 SAN 覆盖与 runtime unsupported 分开显示。

本增量的 TypeScript 变更必须执行 `npm run typecheck`、`npm test`、`npm run format:check` 和
`npm run build`。由于未改变 native wire codec/runtime，本次不以控制面测试宣称生产兼容；生产脚本
语料差分与最终切流门槛仍未完成。
