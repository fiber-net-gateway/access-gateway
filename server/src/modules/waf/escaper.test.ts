import assert from 'node:assert/strict'
import test from 'node:test'

import { cidrDirectiveLiteral, dialectStringLiteral } from './escaper.js'

// Inverse of the escaper over the documented dialect escape set: parses the
// produced double-quoted literal back to bytes, so round-trip assertions
// prove value equivalence rather than just shape.
function parseDialectStringLiteral(literal: string): Buffer {
  assert.ok(literal.startsWith('"') && literal.endsWith('"'), `malformed literal ${literal}`)
  const bytes: number[] = []
  for (let index = 1; index < literal.length - 1; index += 1) {
    const char = literal.charAt(index)
    if (char !== '\\') {
      bytes.push(char.charCodeAt(0))
      continue
    }
    const escape = literal.charAt(index + 1)
    index += 1
    const simple: Record<string, number> = {
      b: 0x08,
      f: 0x0c,
      n: 0x0a,
      r: 0x0d,
      t: 0x09,
      v: 0x0b,
      '"': 0x22,
      "'": 0x27,
      '`': 0x60,
      '\\': 0x5c,
    }
    if (escape === 'x') {
      const hex = literal.slice(index + 1, index + 3)
      assert.ok(/^[0-9a-f]{2}$/u.test(hex), `bad \\x escape in ${literal}`)
      bytes.push(Number.parseInt(hex, 16))
      index += 2
    } else if (escape !== '' && simple[escape] !== undefined) {
      bytes.push(simple[escape])
    } else {
      assert.fail(`escaper produced an unsupported escape '\\${escape}' in ${literal}`)
    }
  }
  return Buffer.from(bytes)
}

const roundTripCorpus: readonly string[] = [
  '',
  'plain-ascii',
  'union select',
  '(?i)union[+%20\\s]+select',
  'quote: " and \' and ` backtick',
  'backslash \\ and double \\\\ and trailing \\',
  'control:\n\r\t\x00\x1b\x7f end',
  'chinese 注入探测 and emoji 🛡️',
  'octal lookalike \\0 \\101 \\8',
  'hex lookalike \\x41 \\xZZ',
  'brace escape lookalike \\u{41} \\u0041',
  'template injection ${secret} \\${secret}',
  'newline injection first line\nsecond line; resp.send(200, "pwned")',
  'quote-semicolon break: "; resp.send(200, "x")',
  'cidr;semicolon;joined',
  '\\a bell lookalike',
]

test('dialectStringLiteral round-trips every corpus entry byte-for-byte', () => {
  for (const value of roundTripCorpus) {
    const literal = dialectStringLiteral(value)
    assert.ok(!literal.includes('\n'), `raw newline leaked into literal ${literal}`)
    assert.deepEqual(
      parseDialectStringLiteral(literal),
      Buffer.from(value, 'utf8'),
      `value: ${value}`,
    )
  }
})

test('dialectStringLiteral never emits raw control or high bytes', () => {
  const literal = dialectStringLiteral('ctrl:\x01\x1f\x7f\x80 lost')
  for (const byte of Buffer.from(literal, 'utf8')) {
    if (byte === 0x5c) continue // backslash introduces an escape
    assert.ok(byte >= 0x20 && byte <= 0x7e, `raw byte 0x${byte.toString(16)} leaked`)
  }
})

test('dialectStringLiteral escapes the string metacharacters uniformly as xHH', () => {
  const literal = dialectStringLiteral('"\'`\\')
  assert.equal(literal, '"\\x22\\x27\\x60\\x5c"')
  assert.deepEqual(parseDialectStringLiteral(literal), Buffer.from('"\'`\\', 'utf8'))
})

test('dialectStringLiteral escapes every byte of a high-bit string as xHH', () => {
  const literal = dialectStringLiteral('中')
  assert.equal(literal, '"\\xe4\\xb8\\xad"')
})

test('cidrDirectiveLiteral joins members with semicolons inside one literal', () => {
  assert.equal(cidrDirectiveLiteral(['10.0.0.0/8', '172.28.0.0/16']), '"10.0.0.0/8;172.28.0.0/16"')
  assert.equal(cidrDirectiveLiteral(['2001:db8::/32']), '"2001:db8::/32"')
})
