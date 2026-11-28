import assert from 'node:assert/strict'
import test from 'node:test'

import {
  compileGrayRulesWire,
  countEffectiveGrayRules,
  emptyGrayRulesDocument,
  parseGrayRulesWire,
  type GrayRulesDocument,
} from './model.js'
import { isValidGrayCidr, validateGrayRules } from './validate.js'

test('compileGrayRulesWire emits deterministic native wire JSON and {} for empty rules', () => {
  assert.equal(compileGrayRulesWire(emptyGrayRulesDocument()), '{}')

  const doc: GrayRulesDocument = {
    schemaVersion: 1,
    rules: [
      { entry: 'custom', ratio: 10000, cidrs: ['2001:db8::/32'] },
      { entry: 'internet', ratio: 500, cidrs: ['10.0.0.0/8', '192.168.1.0/24'] },
    ],
  }
  const wire = compileGrayRulesWire(doc)
  // canonicalJson sorts object keys deterministically
  assert.equal(
    wire,
    '{"custom":{"cidrs":["2001:db8::/32"],"ratio":10000},"internet":{"cidrs":["10.0.0.0/8","192.168.1.0/24"],"ratio":500}}',
  )

  const parsed = parseGrayRulesWire(wire)
  assert.equal(parsed.parseError, null)
  assert.deepEqual(parsed.document, {
    schemaVersion: 1,
    rules: [
      { entry: 'internet', ratio: 500, cidrs: ['10.0.0.0/8', '192.168.1.0/24'] },
      { entry: 'custom', ratio: 10000, cidrs: ['2001:db8::/32'] },
    ],
  })
})

test('countEffectiveGrayRules mirrors native GrayMatchCompiler skipping inert zero rules', () => {
  const doc: GrayRulesDocument = {
    schemaVersion: 1,
    rules: [
      { entry: 'internet', ratio: 0, cidrs: [] },
      { entry: 'vdi', ratio: 250, cidrs: [] },
      { entry: 'desktop', ratio: 0, cidrs: ['10.0.0.0/8'] },
    ],
  }
  assert.equal(countEffectiveGrayRules(doc), 2)
})

test('parseGrayRulesWire handles null, empty string, and malformed wire content safely', () => {
  assert.deepEqual(parseGrayRulesWire('null'), {
    document: emptyGrayRulesDocument(),
    parseError: null,
  })
  assert.equal(parseGrayRulesWire('').document, null)
  assert.match(parseGrayRulesWire('').parseError ?? '', /empty/u)
  assert.equal(parseGrayRulesWire('{"unknown":{"ratio":10}}').document, null)
  assert.equal(parseGrayRulesWire('{"internet":{"ratio":10001}}').document, null)
  assert.equal(parseGrayRulesWire('[]').document, null)
  assert.equal(parseGrayRulesWire('{bad json').document, null)
})

test('isValidGrayCidr accepts valid IPv4/IPv6 CIDRs and rejects malformed ones', () => {
  for (const valid of [
    '0.0.0.0/0',
    '10.0.0.0/8',
    '192.168.1.1/32',
    '::/0',
    '2001:db8::/32',
    'fe80::1/128',
  ]) {
    assert.equal(isValidGrayCidr(valid), true, `expected valid CIDR: ${valid}`)
  }

  for (const invalid of [
    '',
    '10.0.0.1',
    '10.0.0.0/33',
    '10.0.0.0/-1',
    '10.0.0.0/08',
    ' 10.0.0.0/8',
    '10.0.0.0/8 ',
    '999.0.0.1/8',
    '2001:db8::/129',
    'gggg::/32',
    '10.0.0.0/8/16',
  ]) {
    assert.equal(isValidGrayCidr(invalid), false, `expected invalid CIDR: ${invalid}`)
  }
})

test('validateGrayRules reports precise field paths for invalid entries, ratios, CIDRs, and limits', () => {
  assert.deepEqual(
    validateGrayRules({
      schemaVersion: 1,
      rules: [
        { entry: 'internet', ratio: 500, cidrs: ['10.0.0.0/8', '2001:db8::/32'] },
        { entry: 'vdi', ratio: 0, cidrs: [] },
        { entry: 'desktop', ratio: 10000, cidrs: ['192.168.0.0/16'] },
        { entry: 'custom', ratio: 1, cidrs: ['::1/128'] },
      ],
    }),
    [],
  )

  const issues = validateGrayRules({
    schemaVersion: 1,
    rules: [
      { entry: 'internet', ratio: 10001, cidrs: ['10.0.0.0/33', '10.0.0.0/8', '10.0.0.0/8'] },
      { entry: 'internet', ratio: -1, cidrs: [] },
      { entry: 'unknown', ratio: 1.5, cidrs: [] },
    ],
  })

  assert.deepEqual(
    issues.map((issue) => `${issue.field}:${issue.code}`),
    [
      'rules[0].ratio:out_of_range',
      'rules[0].cidrs[0]:invalid_field',
      'rules[0].cidrs[2]:conflict',
      'rules[1].entry:conflict',
      'rules[1].ratio:out_of_range',
      'rules[2].entry:invalid_field',
      'rules[2].ratio:out_of_range',
    ],
  )

  const limited = validateGrayRules(
    {
      schemaVersion: 1,
      rules: [{ entry: 'internet', ratio: 100, cidrs: ['10.0.0.0/8', '10.1.0.0/16'] }],
    },
    { maxPayloadBytes: 1024, maxRules: 4, maxEntryBytes: 64, maxCidrsPerRule: 1, maxCidrBytes: 64 },
  )
  assert.deepEqual(
    limited.map((issue) => `${issue.field}:${issue.code}`),
    ['rules[0].cidrs:limit_exceeded'],
  )
})
