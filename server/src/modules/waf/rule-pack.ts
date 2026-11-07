// 起手规则包 (docs/edge-functions-and-waf-design.md §7 P3: 纯数据).
//
// A curated starter rule set the console can import into an empty or sparse
// WAF policy draft. Every rule is plain data in the console-internal rule
// model; the golden fixture pair (fixtures/rule-pack-v1.*) pins the generated
// script byte-for-byte, and the fixture was proven to compile through the
// real native validator binary when it was authored.
//
// Curation policy:
// - Five families from the design doc: SQLi / XSS / 路径穿越 / 扫描器 UA /
//   畸形 URI (+ high-signal RCE probe).
// - Rules default to `block` only where false positives are unlikely; noisy
//   families (scanner UA, sensitive-file probing) stay `log` for review.
// - Groups follow those families, because exempting a project is a group-level
//   decision: a false positive costs one family, not the whole pack.
// - Group modes gate actual blocking (§4.2): every pack group imports as off,
//   so an import changes nothing until an operator turns a group on — 灰度顺序
//   单项目 log → block.
// - Stable ids: operators reference them in metrics labels and logs; never
//   rename an id, retire it and add a new one.
//
// Dialect authoring constraints (match expressions are embedded verbatim as
// Access 脚本语言 source): string literals only accept the dialect escape set
// (single-char escapes, octal, \xHH, \uHHHH) — a regex backslash class like
// `\s` must therefore be written `\\s` (escaped backslash reaches RE2 as one
// backslash). Literal regex metacharacters are written as classes (`[.]`,
// `[$]`, `[{]`) to stay escape-free. Header names use `_` where the wire name
// has `-` (`$header.user_agent`): ConstPackage normalizes `-`→`_` on both the
// script side and the request side, so they meet.

import type { WafGroup, WafRule } from './model.js'

export const WAF_RULE_PACK_VERSION = 'waf-rule-pack-v2'

function packGroup(id: string, name: string, priority: number): WafGroup {
  return { id, name, mode: 'off', priority, projects: { include: ['*'] } }
}

export const WAF_RULE_PACK_GROUPS: readonly WafGroup[] = [
  packGroup('sqli', 'SQL 注入', 100),
  packGroup('rce', '远程代码执行', 120),
  packGroup('xss', 'XSS', 200),
  packGroup('traversal', '路径穿越', 300),
  packGroup('probe', '扫描与探测', 400),
  packGroup('malformed', '畸形请求', 500),
]

export const WAF_RULE_PACK: readonly WafRule[] = [
  {
    id: 'sqli-union-select',
    name: 'union select 注入探测',
    group: 'sqli',
    severity: 'critical',
    priority: 100,
    action: 'block',
    phase: 'request_head',
    match: 'strings.match($req.uri, "(?i)union(?:[+%20]|\\\\s)+(?:all\\\\s+)?select")',
  },
  {
    id: 'sqli-time-function',
    name: 'SQL 延时函数探测',
    group: 'sqli',
    severity: 'high',
    priority: 110,
    action: 'block',
    phase: 'request_head',
    match: 'strings.match($req.query, "(?i)(?:sleep|benchmark|waitfor|pg_sleep)\\\\s*\\\\(")',
  },
  {
    id: 'rce-jndi-probe',
    name: 'JNDI 注入探测（log4shell 形态）',
    group: 'rce',
    severity: 'critical',
    priority: 120,
    action: 'block',
    phase: 'request_head',
    match:
      'strings.match($req.uri, "(?i)[$][{]jndi:") || strings.match($header.user_agent, "(?i)[$][{]jndi:") || strings.match($header.referer, "(?i)[$][{]jndi:")',
  },
  {
    id: 'xss-script-marker',
    name: 'XSS 脚本标记探测',
    group: 'xss',
    severity: 'high',
    priority: 200,
    action: 'block',
    phase: 'request_head',
    match:
      'strings.match($req.uri, "(?i)(?:<\\\\s*script|<\\\\s*iframe|javascript:|on(?:error|load|click)\\\\s*=)")',
  },
  {
    id: 'traversal-encoded-dotdot',
    name: '路径穿越探测（含编码变体）',
    group: 'traversal',
    severity: 'high',
    priority: 300,
    action: 'block',
    phase: 'request_head',
    match: 'strings.match($req.uri, "(?i)(?:[.][.]|%2e%2e|%252e%252e)(?:/|%2f|%5c|%255c)")',
  },
  {
    id: 'scanner-user-agent',
    name: '扫描器 User-Agent',
    group: 'probe',
    severity: 'medium',
    priority: 400,
    action: 'log',
    phase: 'request_head',
    match:
      'strings.match($header.user_agent, "(?i)(?:sqlmap|nikto|nmap|masscan|zgrab|acunetix|nessus|dirbuster|gobuster|wpscan)")',
  },
  {
    id: 'probe-sensitive-file',
    name: '敏感文件与管理端探测',
    group: 'probe',
    severity: 'medium',
    priority: 410,
    action: 'log',
    phase: 'request_head',
    match:
      'strings.match($req.path, "(?i)^/[.](?:env|git|htaccess|htpasswd|ssh|aws)|/(?:phpmyadmin|wp-admin|wp-login[.]php|actuator)(?:/|$)")',
  },
  {
    id: 'malformed-control-chars',
    name: 'URI 控制字符',
    group: 'malformed',
    severity: 'medium',
    priority: 500,
    action: 'block',
    phase: 'request_head',
    match: 'strings.match($req.uri, "[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f]")',
  },
  {
    id: 'malformed-triple-encoding',
    name: '三重百分号编码探测',
    group: 'malformed',
    severity: 'low',
    priority: 510,
    action: 'log',
    phase: 'request_head',
    match: 'strings.match($req.uri, "%[0-9a-f]{2}%[0-9a-f]{2}%[0-9a-f]{2}")',
  },
]

export interface WafRulePackView {
  version: string
  groups: readonly WafGroup[]
  rules: readonly WafRule[]
}

export function wafRulePackView(): WafRulePackView {
  return { version: WAF_RULE_PACK_VERSION, groups: WAF_RULE_PACK_GROUPS, rules: WAF_RULE_PACK }
}
