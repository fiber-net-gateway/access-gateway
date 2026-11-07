// Stored schema-1 policies predate rule groups: one environment-wide mode and
// per-rule project exclusions. Drafts upgrade on read, so a save rewrites them
// as schema 2; release history needs nothing, since it stores wire payloads.

import { dialectStringLiteral } from './escaper.js'
import {
  emptyWafPolicy,
  WAF_POLICY_SCHEMA_VERSION,
  type WafMode,
  type WafPolicy,
  type WafRule,
} from './model.js'

const UPGRADED_GROUP_ID = 'default'
const UPGRADED_GROUP_NAME = '默认规则组'
// The fixed mount priority of the schema-1 generated function, which is where
// handwritten functions were ordered around it.
const UPGRADED_GROUP_PRIORITY = 100

type SchemaOneRule = Omit<WafRule, 'group'> & {
  readonly exclusions?: readonly { readonly project: string; readonly reason: string }[]
}

interface SchemaOnePolicy {
  readonly schemaVersion: 1
  readonly defaultMode: WafMode
  readonly rules: readonly SchemaOneRule[]
  readonly projectModes: Readonly<Record<string, WafMode>>
}

function isSchemaOnePolicy(value: object): value is SchemaOnePolicy {
  const policy = value as Partial<SchemaOnePolicy>
  return (
    policy.schemaVersion === 1 &&
    Array.isArray(policy.rules) &&
    typeof policy.projectModes === 'object' &&
    policy.projectModes !== null
  )
}

// A group cannot exempt a project from one rule, so an exclusion can only stay
// exact as a guard in the rule's own match. Its audit reason has no home there.
function upgradeRule({ exclusions = [], ...rule }: SchemaOneRule): WafRule {
  if (exclusions.length === 0) {
    return { ...rule, group: UPGRADED_GROUP_ID }
  }
  const guards = exclusions.map(({ project }) => `$ctx.project != ${dialectStringLiteral(project)}`)
  const match = rule.match.includes('\n') ? `(\n${rule.match}\n)` : `(${rule.match.trim()})`
  return { ...rule, group: UPGRADED_GROUP_ID, match: [...guards, match].join(' && ') }
}

export function upgradeWafPolicy(stored: object): object {
  if (!isSchemaOnePolicy(stored)) {
    return stored
  }
  if (stored.rules.length === 0) {
    return { ...emptyWafPolicy(), projectModes: stored.projectModes }
  }
  // Default-off used to mean "only the listed projects": that list becomes the
  // group's scope, since an off group now publishes nothing at all.
  const listed = Object.entries(stored.projectModes)
    .filter(([, mode]) => mode !== 'off')
    .map(([project]) => project)
    .sort()
  const scopedToList = stored.defaultMode === 'off' && listed.length > 0
  const policy: WafPolicy = {
    schemaVersion: WAF_POLICY_SCHEMA_VERSION,
    groups: [
      {
        id: UPGRADED_GROUP_ID,
        name: UPGRADED_GROUP_NAME,
        mode: scopedToList ? 'log' : stored.defaultMode,
        priority: UPGRADED_GROUP_PRIORITY,
        projects: { include: scopedToList ? listed : ['*'] },
      },
    ],
    rules: stored.rules.map(upgradeRule),
    projectModes: stored.projectModes,
  }
  return policy
}
