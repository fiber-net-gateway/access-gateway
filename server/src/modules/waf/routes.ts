import type { FastifyInstance } from 'fastify'

import { badRequest } from '../../shared/errors.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
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
  required: ['id', 'name', 'severity', 'priority', 'action', 'phase', 'match'],
  properties: {
    id: {
      type: 'string',
      minLength: 1,
      maxLength: 64,
      pattern: '^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$',
    },
    name: { type: 'string', minLength: 1, maxLength: 255 },
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
    exclusions: {
      type: 'array',
      maxItems: 64,
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
} as const

// Coarse shape only: the service re-validates with precise field paths
// (validate.ts) and rejects wire-hostile names the pattern here cannot see.
const wafPolicySchema = {
  type: 'object',
  additionalProperties: false,
  required: ['schemaVersion', 'defaultMode', 'rules', 'projectModes'],
  properties: {
    schemaVersion: { type: 'integer', const: 1 },
    defaultMode: { type: 'string', enum: ['off', 'log', 'block'] },
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

const wafCompilationSchema = {
  anyOf: [
    {
      type: 'object',
      additionalProperties: false,
      required: ['kind', 'reason'],
      properties: {
        kind: { type: 'string', const: 'unmount' },
        reason: { type: 'string', enum: ['no-rules', 'no-mounted-projects'] },
      },
    },
    {
      type: 'object',
      additionalProperties: false,
      required: [
        'kind',
        'script',
        'scriptSha256',
        'functionName',
        'mountPriority',
        'mounts',
        'ruleOrder',
        'directives',
      ],
      properties: {
        kind: { type: 'string', const: 'mounted' },
        script: { type: 'string' },
        scriptSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
        functionName: { type: 'string' },
        mountPriority: { type: 'integer' },
        mounts: {
          type: 'array',
          items: {
            type: 'object',
            additionalProperties: false,
            required: ['include', 'exclude'],
            properties: {
              include: { type: 'array', items: { type: 'string' } },
              exclude: { type: 'array', items: { type: 'string' } },
            },
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
    compilation: wafCompilationSchema,
    scriptValid: { type: ['boolean', 'null'] },
    scriptErrors: {
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

export function registerWafRoutes(app: FastifyInstance, auth: AuthService, waf: WafService): void {
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
        body: wafPolicySchema,
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
}
