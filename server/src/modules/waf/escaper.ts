// Edge-function dialect string escaper (docs/edge-functions-and-waf-design.md
// §4.2). Security-critical: rule-authored text (regex patterns, project
// names, rule ids, CIDR lists, display strings inside the generated script)
// must land in the generated script as an exactly equivalent string literal.
//
// Strategy: emit a double-quoted ordinary string literal, iterating the
// input's UTF-8 bytes. Every byte is emitted either as itself (printable
// ASCII outside the escape set) or as a two-hex-digit `\xHH` escape. This
// avoids the dialect's hazardous corners entirely: octal escapes (1-3 digits,
// ambiguous), `\a` (produces plain `a`, not bell), `\u{...}` (unsupported),
// and raw newlines (unescaped line terminators are illegal) can never be
// produced, so no authored byte sequence can change the literal's meaning.
// The four escaped metacharacters keep golden scripts readable; `\xHH` is
// used for everything else that is not safe raw.
//
// Precondition: input contains no lone surrogates (validate.ts rejects them;
// Buffer.from would otherwise silently substitute U+FFFD and break the
// round-trip guarantee).

const ESCAPED_METACHARACTERS = new Set([0x22 /* " */, 0x27 /* ' */, 0x5c /* \ */, 0x60 /* ` */])

const HEX_DIGITS = '0123456789abcdef'

export function dialectStringLiteral(text: string): string {
  const bytes = Buffer.from(text, 'utf8')
  let literal = '"'
  for (const byte of bytes) {
    if (byte >= 0x20 && byte <= 0x7e && !ESCAPED_METACHARACTERS.has(byte)) {
      literal += String.fromCharCode(byte)
      continue
    }
    literal += '\\x' + HEX_DIGITS[byte >> 4] + HEX_DIGITS[byte & 0x0f]
  }
  return literal + '"'
}

// Escapes one or more CIDR literals into a `cidr` directive argument: the
// directives join member lists with ';' internally (empty segments are
// ignored by the native parser).
export function cidrDirectiveLiteral(cidrs: readonly string[]): string {
  return dialectStringLiteral(cidrs.join(';'))
}
