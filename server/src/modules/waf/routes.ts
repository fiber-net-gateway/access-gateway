import type { FastifyInstance } from 'fastify'

import { badRequest } from '../../shared/errors.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import type { EdgeFunctionService } from './edge-function-service.js'
import { isWafHitsWindow, type WafHitsService } from './hits-service.js'
import type { WafReleaseService } from './release-service.js'
import { wafRulePackView } from './rule-pack.js'
import type { WafCompilePreviewView, WafService } from './service.js'

function parseIfMatch(value: string | undefined): string {
  if (!value) {
    throw badRequest('IF_MATCH_REQUIRED', 'If-Match is required when saving the WAF policy')
  }
  const match = /^(?:W\/)?"(0|[1-9][0-9]*)"$/u.exec(value.trim())
  if (!match?.[1]) {
    throw badRequest('INVALID_IF_MATCH', 'If-Match must contain the WAF policy lock version ETag')
  }
  return match[1]
}

const wafRuleSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['id', 'name', 'group', 'severity', 'priority', 'action', 'phase', 'match'],
  properties: {
    id: {
      type: 'string',
      minLength: 1,
      maxLength: 64,
      pattern: '^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$',
    },
    name: { type: 'string', minLength: 1, maxLength: 255 },
    group: { type: 'string', minLength: 1, maxLength: 60 },
    severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low'] },
    priority: { type: 'integer', minimum: 0, maximum: 1000000 },
    action: { type: 'string', enum: ['log', 'block'] },
    phase: { type: 'string', const: 'request_head' },
    match: { type: 'string', minLength: 1, maxLength: 8192 },
    scope: {
      type: 'object',
      additionalProperties: false,
      properties: {
        methods: {
          type: 'array',
          maxItems: 16,
          items: { type: 'string', maxLength: 16, pattern: '^[A-Z][A-Z0-9]{0,15}$' },
        },
        paths: {
          type: 'array',
          maxItems: 64,
          items: { type: 'string', minLength: 1, maxLength: 255 },
        },
        sourceCidrs: {
          type: 'array',
          maxItems: 64,
          items: { type: 'string', minLength: 1, maxLength: 64 },
        },
      },
    },
  },
} as const

const wafGroupSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['id', 'name', 'mode', 'priority', 'projects'],
  properties: {
    id: {
      type: 'string',
      minLength: 1,
      maxLength: 60,
      pattern: '^[a-z0-9](?:[a-z0-9-]{0,58}[a-z0-9])?$',
    },
    name: { type: 'string', minLength: 1, maxLength: 255 },
    mode: { type: 'string', enum: ['off', 'log', 'block'] },
    priority: { type: 'integer', minimum: 0, maximum: 1000000 },
    projects: {
      type: 'object',
      additionalProperties: false,
      required: ['include'],
      properties: {
        include: {
          type: 'array',
          minItems: 1,
          maxItems: 256,
          items: { type: 'string', minLength: 1, maxLength: 255 },
        },
        exempt: {
          type: 'array',
          maxItems: 256,
          items: {
            type: 'object',
            additionalProperties: false,
            required: ['project', 'reason'],
            properties: {
              project: { type: 'string', minLength: 1, maxLength: 255 },
              reason: { type: 'string', minLength: 1, maxLength: 255 },
            },
          },
        },
      },
    },
  },
} as const

// Coarse shape only: the service re-validates with precise field paths
// (validate.ts) and rejects wire-hostile names the pattern here cannot see.
const wafPolicySchema = {
  type: 'object',
  additionalProperties: false,
  required: ['schemaVersion', 'groups', 'rules', 'projectModes'],
  properties: {
    schemaVersion: { type: 'integer', const: 2 },
    groups: { type: 'array', maxItems: 32, items: wafGroupSchema },
    rules: { type: 'array', maxItems: 256, items: wafRuleSchema },
    projectModes: {
      type: 'object',
      maxProperties: 1024,
      additionalProperties: false,
      patternProperties: {
        '^[^*;]{1,255}$': { type: 'string', enum: ['off', 'log', 'block'] },
      },
    },
  },
} as const

// compile-preview is the authoring feedback loop: value-level mistakes have to
// come back as structured validateWafPolicy issues with field paths, not as a
// bare 400. Its body therefore keeps only the structural skeleton the validator
// relies on (object/array/scalar types and required keys).
const valueConstraintKeywords = new Set([
  'const',
  'enum',
  'maxItems',
  'maxLength',
  'maxProperties',
  'maximum',
  'minItems',
  'minLength',
  'minimum',
  'pattern',
])

function structuralSchema(schema: unknown): unknown {
  if (Array.isArray(schema)) {
    return schema.map(structuralSchema)
  }
  if (typeof schema !== 'object' || schema === null) {
    return schema
  }
  return Object.fromEntries(
    Object.entries(schema)
      .filter(([keyword]) => !valueConstraintKeywords.has(keyword))
      .map(([keyword, value]) => [keyword, structuralSchema(value)]),
  )
}

const wafPolicyDraftResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['id', 'policy', 'policySha256', 'lockVersion', 'createdAt', 'updatedAt'],
  properties: {
    id: { type: 'string', format: 'uuid' },
    policy: wafPolicySchema,
    policySha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    createdAt: { type: 'string', format: 'date-time' },
    updatedAt: { type: 'string', format: 'date-time' },
  },
} as const

const wafRulePackResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['version', 'groups', 'rules'],
  properties: {
    version: { type: 'string', minLength: 1, maxLength: 64 },
    groups: { type: 'array', items: wafGroupSchema },
    rules: { type: 'array', items: wafRuleSchema },
  },
} as const

const wafHitsResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['window', 'queriedAt', 'rows'],
  properties: {
    window: { type: 'string', enum: ['1h', '24h', '7d'] },
    queriedAt: { type: 'string', format: 'date-time' },
    rows: {
      type: 'array',
      maxItems: 512,
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['ruleId', 'event', 'count'],
        properties: {
          ruleId: { type: 'string', minLength: 1, maxLength: 64 },
          event: { type: 'string', minLength: 1, maxLength: 32 },
          count: { type: 'number', minimum: 0 },
        },
      },
    },
  },
} as const

const wafSkippedGroupsSchema = {
  type: 'array',
  items: {
    type: 'object',
    additionalProperties: false,
    required: ['groupId', 'reason'],
    properties: {
      groupId: { type: 'string' },
      reason: { type: 'string', enum: ['off', 'no-rules'] },
    },
  },
} as const

const wafCompilationSchema = {
  anyOf: [
    {
      type: 'object',
      additionalProperties: false,
      required: ['kind', 'reason', 'skipped'],
      properties: {
        kind: { type: 'string', const: 'unmount' },
        reason: { type: 'string', enum: ['no-rules', 'no-mounted-groups'] },
        skipped: wafSkippedGroupsSchema,
      },
    },
    {
      type: 'object',
      additionalProperties: false,
      required: ['kind', 'functions', 'skipped'],
      properties: {
        kind: { type: 'string', const: 'mounted' },
        functions: {
          type: 'array',
          items: {
            type: 'object',
            additionalProperties: false,
            required: [
              'groupId',
              'functionName',
              'script',
              'scriptSha256',
              'priority',
              'projects',
              'ruleOrder',
              'directives',
            ],
            properties: {
              groupId: { type: 'string' },
              functionName: { type: 'string' },
              script: { type: 'string' },
              scriptSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
              priority: { type: 'integer' },
              projects: {
                type: 'object',
                additionalProperties: false,
                required: ['include', 'exclude'],
                properties: {
                  include: { type: 'array', items: { type: 'string' } },
                  exclude: { type: 'array', items: { type: 'string' } },
                },
              },
              ruleOrder: { type: 'array', items: { type: 'string' } },
              directives: {
                type: 'array',
                items: {
                  type: 'object',
                  additionalProperties: false,
                  required: ['name', 'cidrs'],
                  properties: {
                    name: { type: 'string' },
                    cidrs: { type: 'array', items: { type: 'string' } },
                  },
                },
              },
            },
          },
        },
        skipped: wafSkippedGroupsSchema,
      },
    },
  ],
} as const

const wafCompilePreviewResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['valid', 'issues', 'compilation', 'scriptValid', 'scriptErrors', 'validator'],
  properties: {
    valid: { type: 'boolean' },
    issues: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['field', 'code', 'message'],
        properties: {
          field: { type: 'string' },
          code: { type: 'string' },
          message: { type: 'string' },
        },
      },
    },
    // Null when the policy has field-level issues and nothing was compiled.
    compilation: { anyOf: [...wafCompilationSchema.anyOf, { type: 'null' }] },
    scriptValid: { type: ['boolean', 'null'] },
    scriptErrors: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['code', 'message', 'functionName'],
        properties: {
          code: { type: 'string' },
          functionName: { type: 'string' },
          field: { type: 'string' },
          offset: { type: 'integer' },
          line: { type: 'integer' },
          column: { type: 'integer' },
          message: { type: 'string' },
        },
      },
    },
    validator: {
      anyOf: [
        { type: 'null' },
        {
          type: 'object',
          additionalProperties: false,
          required: ['contractVersion', 'revision'],
          properties: {
            contractVersion: { type: 'integer' },
            revision: { type: 'string' },
          },
        },
      ],
    },
  },
} as const

interface SaveWafPolicyBody {
  policy: unknown
}

// Coarse shape only: the service re-validates with precise field paths
// (edge-function-validate.ts) and the native validator compiles each script.
const edgeFunctionNameSchema = {
  type: 'string',
  minLength: 1,
  maxLength: 64,
  pattern: '^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$',
} as const

const edgeFunctionDraftFunctionSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['name', 'enabled', 'script', 'projects'],
  properties: {
    name: edgeFunctionNameSchema,
    description: { type: 'string', minLength: 1, maxLength: 255 },
    enabled: { type: 'boolean' },
    phase: { type: 'string', enum: ['request_head', 'request_body'] },
    script: { type: 'string', minLength: 1, maxLength: 1048576 },
    projects: {
      type: 'object',
      additionalProperties: false,
      required: ['include'],
      properties: {
        include: {
          type: 'array',
          minItems: 1,
          maxItems: 256,
          items: { type: 'string', minLength: 1, maxLength: 255 },
        },
        exclude: {
          type: 'array',
          maxItems: 256,
          items: { type: 'string', minLength: 1, maxLength: 255 },
        },
      },
    },
    priority: { type: 'integer', minimum: 0, maximum: 1000000 },
    ratio: { type: 'integer', minimum: 0, maximum: 10000 },
    replaces: edgeFunctionNameSchema,
  },
} as const

const edgeFunctionsDraftSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['schemaVersion', 'functions'],
  properties: {
    schemaVersion: { type: 'integer', const: 1 },
    functions: { type: 'array', maxItems: 64, items: edgeFunctionDraftFunctionSchema },
  },
} as const

const edgeFunctionsDraftResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['id', 'draft', 'draftSha256', 'lockVersion', 'createdAt', 'updatedAt'],
  properties: {
    id: { type: 'string', format: 'uuid' },
    draft: edgeFunctionsDraftSchema,
    draftSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    createdAt: { type: 'string', format: 'date-time' },
    updatedAt: { type: 'string', format: 'date-time' },
  },
} as const

const edgeFunctionCompilePreviewResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['valid', 'issues', 'functions', 'validator'],
  properties: {
    valid: { type: 'boolean' },
    issues: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['field', 'code', 'message'],
        properties: {
          field: { type: 'string' },
          code: { type: 'string' },
          message: { type: 'string' },
        },
      },
    },
    functions: {
      type: 'array',
      maxItems: 64,
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['name', 'phase', 'scriptSha256', 'scriptValid', 'errors'],
        properties: {
          name: { type: 'string' },
          phase: { type: 'string', enum: ['request_head', 'request_body'] },
          scriptSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
          scriptValid: { type: 'boolean' },
          errors: {
            type: 'array',
            items: {
              type: 'object',
              additionalProperties: false,
              required: ['code', 'message'],
              properties: {
                code: { type: 'string' },
                field: { type: 'string' },
                offset: { type: 'integer' },
                message: { type: 'string' },
              },
            },
          },
        },
      },
    },
    validator: {
      anyOf: [
        { type: 'null' },
        {
          type: 'object',
          additionalProperties: false,
          required: ['contractVersion', 'revision'],
          properties: {
            contractVersion: { type: 'integer' },
            revision: { type: 'string' },
          },
        },
      ],
    },
  },
} as const

interface SaveEdgeFunctionsDraftBody {
  draft: unknown
}

export function registerWafRoutes(
  app: FastifyInstance,
  auth: AuthService,
  waf: WafService,
  wafReleases: WafReleaseService,
  wafHits: WafHitsService,
  edgeFunctions: EdgeFunctionService,
): void {
  app.get(
    '/api/waf/policy',
    { schema: { response: { 200: wafPolicyDraftResponseSchema } } },
    async (_request, reply) => {
      const result = await waf.getPolicyDraft(await requireActor(auth))
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )

  app.post(
    '/api/waf/policy',
    { schema: { response: { 201: wafPolicyDraftResponseSchema } } },
    async (request, reply) => {
      const result = await waf.createPolicyDraft(await requireActor(auth), String(request.id))
      reply.header('etag', `"${result.lockVersion}"`)
      return reply.code(201).send(result)
    },
  )

  app.put<{ Headers: { 'if-match'?: string }; Body: SaveWafPolicyBody }>(
    '/api/waf/policy',
    {
      schema: {
        headers: {
          type: 'object',
          required: ['if-match'],
          properties: { 'if-match': { type: 'string' } },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['policy'],
          properties: {
            policy: wafPolicySchema,
          },
        },
        response: { 200: wafPolicyDraftResponseSchema },
      },
    },
    async (request, reply) => {
      const result = await waf.savePolicyDraft(
        await requireActor(auth),
        {
          lockVersion: parseIfMatch(request.headers['if-match']),
          policy: request.body.policy,
        },
        String(request.id),
      )
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )

  app.post<{ Body: unknown }>(
    '/api/waf/policy/compile-preview',
    {
      schema: {
        body: structuralSchema(wafPolicySchema),
        response: { 200: wafCompilePreviewResponseSchema },
      },
    },
    async (request, reply) => {
      const result: WafCompilePreviewView = await waf.compilePreview(
        await requireActor(auth),
        request.body,
        String(request.id),
      )
      reply.header('cache-control', 'no-store')
      return result
    },
  )

  app.get(
    '/api/waf/rule-pack',
    { schema: { response: { 200: wafRulePackResponseSchema } } },
    async () => {
      // Static curated data shipped with the console build; viewer-readable.
      await requireActor(auth)
      return wafRulePackView()
    },
  )

  app.get(
    '/api/waf/edge-functions/draft',
    { schema: { response: { 200: edgeFunctionsDraftResponseSchema } } },
    async (_request, reply) => {
      const result = await edgeFunctions.getDraft(await requireActor(auth))
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )

  app.post(
    '/api/waf/edge-functions/draft',
    { schema: { response: { 201: edgeFunctionsDraftResponseSchema } } },
    async (request, reply) => {
      const result = await edgeFunctions.createDraft(await requireActor(auth), String(request.id))
      reply.header('etag', `"${result.lockVersion}"`)
      return reply.code(201).send(result)
    },
  )

  app.put<{ Headers: { 'if-match'?: string }; Body: SaveEdgeFunctionsDraftBody }>(
    '/api/waf/edge-functions/draft',
    {
      schema: {
        headers: {
          type: 'object',
          required: ['if-match'],
          properties: { 'if-match': { type: 'string' } },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['draft'],
          properties: {
            draft: edgeFunctionsDraftSchema,
          },
        },
        response: { 200: edgeFunctionsDraftResponseSchema },
      },
    },
    async (request, reply) => {
      const result = await edgeFunctions.saveDraft(
        await requireActor(auth),
        {
          lockVersion: parseIfMatch(request.headers['if-match']),
          draft: request.body.draft,
        },
        String(request.id),
      )
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )

  app.post<{ Body: unknown }>(
    '/api/waf/edge-functions/compile-preview',
    {
      schema: {
        body: edgeFunctionsDraftSchema,
        response: { 200: edgeFunctionCompilePreviewResponseSchema },
      },
    },
    async (request, reply) => {
      const result = await edgeFunctions.compilePreview(
        await requireActor(auth),
        request.body,
        String(request.id),
      )
      reply.header('cache-control', 'no-store')
      return result
    },
  )

  app.get<{ Querystring: { window?: string } }>(
    '/api/waf/hits',
    {
      schema: {
        querystring: {
          type: 'object',
          additionalProperties: false,
          properties: { window: { type: 'string', enum: ['1h', '24h', '7d'] } },
        },
        response: { 200: wafHitsResponseSchema },
      },
    },
    async (request, reply) => {
      // The querystring schema already constrains the value; the guard keeps
      // the service input honest if the schema ever loosens.
      const window = request.query.window ?? '24h'
      if (!isWafHitsWindow(window)) {
        throw badRequest('INVALID_WINDOW', 'window must be one of 1h, 24h, 7d')
      }
      const result = await wafHits.hits(await requireActor(auth), window)
      reply.header('cache-control', 'no-store')
      return result
    },
  )

  registerWafReleaseRoutes(app, auth, wafReleases)
}

const wafReleaseResourceSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['id', 'kind', 'dataId', 'group', 'status', 'verifiedSha256', 'verifiedAt'],
  properties: {
    id: { type: 'string', format: 'uuid' },
    kind: { type: 'string', enum: ['edge_function_script', 'edge_function_index'] },
    dataId: { type: 'string' },
    group: { type: 'string' },
    status: {
      type: 'string',
      enum: ['pending', 'running', 'verified', 'failed', 'conflict', 'conflict_after_partial'],
    },
    verifiedSha256: { type: ['string', 'null'], pattern: '^[0-9a-f]{64}$' },
    verifiedAt: { type: ['string', 'null'], format: 'date-time' },
  },
} as const

const activationSummarySchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'status',
    'targetCount',
    'activeCount',
    'pendingCount',
    'degradedCount',
    'unknownCount',
    'evaluatedAt',
  ],
  properties: {
    status: { type: 'string', enum: ['unknown', 'pending', 'active', 'degraded'] },
    targetCount: { type: 'integer', minimum: 0 },
    activeCount: { type: 'integer', minimum: 0 },
    pendingCount: { type: 'integer', minimum: 0 },
    degradedCount: { type: 'integer', minimum: 0 },
    unknownCount: { type: 'integer', minimum: 0 },
    evaluatedAt: { type: ['string', 'null'], format: 'date-time' },
  },
} as const

const wafReleaseResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'sequence',
    'status',
    'wafFunctions',
    'mode',
    'ruleCount',
    'indexVersion',
    'description',
    'resources',
    'publication',
    'activationStatus',
    'activation',
    'createdAt',
    'publishedAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    sequence: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    status: {
      type: 'string',
      enum: [
        'creating',
        'validating',
        'validation_failed',
        'ready',
        'queued',
        'publishing',
        'published',
        'partially_published',
        'publish_failed',
        'canceled',
        'superseded',
        'abandoned',
      ],
    },
    wafFunctions: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['name', 'scriptSha256'],
        properties: {
          name: { type: 'string' },
          scriptSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
        },
      },
    },
    mode: { type: 'string', enum: ['mounted', 'unmount'] },
    ruleCount: { type: 'integer', minimum: 0 },
    indexVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    description: { type: 'string' },
    resources: { type: 'array', items: wafReleaseResourceSchema },
    publication: {
      type: 'object',
      additionalProperties: false,
      required: ['jobId', 'state'],
      properties: {
        jobId: { type: ['string', 'null'], format: 'uuid' },
        state: { type: ['string', 'null'] },
      },
    },
    activationStatus: { type: 'string' },
    activation: activationSummarySchema,
    createdAt: { type: 'string', format: 'date-time' },
    publishedAt: { type: ['string', 'null'], format: 'date-time' },
  },
} as const

function parseIdempotencyKey(value: string | undefined): string {
  const key = value?.trim() ?? ''
  if (key.length < 1 || key.length > 128 || !/^[\x21-\x7e]+$/u.test(key)) {
    throw badRequest(
      'INVALID_IDEMPOTENCY_KEY',
      'Idempotency-Key must contain 1-128 printable ASCII characters',
    )
  }
  return key
}

const releaseParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['releaseId'],
  properties: { releaseId: { type: 'string', format: 'uuid' } },
} as const

const idempotencyHeaders = {
  type: 'object',
  required: ['idempotency-key'],
  properties: { 'idempotency-key': { type: 'string', minLength: 1, maxLength: 128 } },
} as const

function registerWafReleaseRoutes(
  app: FastifyInstance,
  auth: AuthService,
  wafReleases: WafReleaseService,
): void {
  app.get(
    '/api/waf/edge-functions',
    {
      schema: {
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['index', 'scripts'],
            properties: {
              index: {
                type: 'object',
                additionalProperties: false,
                required: ['exists', 'sha256', 'md5', 'version', 'functions', 'parseError'],
                properties: {
                  exists: { type: 'boolean' },
                  sha256: { type: ['string', 'null'] },
                  md5: { type: ['string', 'null'] },
                  version: { type: ['integer', 'null'] },
                  functions: {
                    type: 'array',
                    items: {
                      type: 'object',
                      additionalProperties: false,
                      required: [
                        'name',
                        'sha256',
                        'phase',
                        'projects',
                        'priority',
                        'ratio',
                        'replaces',
                      ],
                      properties: {
                        name: { type: 'string' },
                        sha256: { type: ['string', 'null'] },
                        phase: {
                          type: ['string', 'null'],
                          enum: ['request_head', 'request_body', null],
                        },
                        projects: {
                          type: 'object',
                          additionalProperties: false,
                          required: ['include', 'exclude'],
                          properties: {
                            include: { type: 'array', items: { type: 'string' } },
                            exclude: { type: 'array', items: { type: 'string' } },
                          },
                        },
                        priority: { type: ['integer', 'null'] },
                        ratio: { type: ['integer', 'null'], minimum: 0, maximum: 10000 },
                        replaces: { type: ['string', 'null'] },
                      },
                    },
                  },
                  parseError: { type: ['string', 'null'] },
                },
              },
              scripts: {
                type: 'array',
                items: {
                  type: 'object',
                  additionalProperties: false,
                  required: ['name', 'exists', 'sha256', 'content'],
                  properties: {
                    name: { type: 'string' },
                    exists: { type: 'boolean' },
                    sha256: { type: ['string', 'null'] },
                    content: { type: ['string', 'null'] },
                  },
                },
              },
            },
          },
        },
      },
    },
    async (_request, reply) => {
      const view = await wafReleases.currentEdgeFunctions(await requireActor(auth))
      reply.header('cache-control', 'no-store')
      return view
    },
  )

  app.get(
    '/api/waf/releases',
    {
      schema: {
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items'],
            properties: { items: { type: 'array', items: wafReleaseResponseSchema } },
          },
        },
      },
    },
    async () => wafReleases.list(await requireActor(auth)),
  )

  app.get<{ Params: { releaseId: string } }>(
    '/api/waf/releases/:releaseId',
    { schema: { params: releaseParameters, response: { 200: wafReleaseResponseSchema } } },
    async (request) => wafReleases.get(await requireActor(auth), request.params.releaseId),
  )

  app.post<{ Headers: { 'idempotency-key'?: string } }>(
    '/api/waf/releases',
    { schema: { headers: idempotencyHeaders, response: { 201: wafReleaseResponseSchema } } },
    async (request, reply) => {
      const release = await wafReleases.create(
        await requireActor(auth),
        { idempotencyKey: parseIdempotencyKey(request.headers['idempotency-key']) },
        String(request.id),
      )
      return reply.code(201).send(release)
    },
  )

  app.post<{ Params: { releaseId: string }; Headers: { 'idempotency-key'?: string } }>(
    '/api/waf/releases/:releaseId/rollback',
    {
      schema: {
        params: releaseParameters,
        headers: idempotencyHeaders,
        response: { 201: wafReleaseResponseSchema },
      },
    },
    async (request, reply) => {
      const release = await wafReleases.rollback(
        await requireActor(auth),
        request.params.releaseId,
        { idempotencyKey: parseIdempotencyKey(request.headers['idempotency-key']) },
        String(request.id),
      )
      return reply.code(201).send(release)
    },
  )

  app.post<{ Params: { releaseId: string }; Headers: { 'idempotency-key'?: string } }>(
    '/api/waf/releases/:releaseId/publication',
    {
      schema: {
        params: releaseParameters,
        headers: idempotencyHeaders,
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['jobId', 'state', 'release'],
            properties: {
              jobId: { type: 'string', format: 'uuid' },
              state: { type: 'string' },
              release: wafReleaseResponseSchema,
            },
          },
        },
      },
    },
    async (request) =>
      wafReleases.queuePublication(
        await requireActor(auth),
        request.params.releaseId,
        parseIdempotencyKey(request.headers['idempotency-key']),
        String(request.id),
      ),
  )
}
