import {
  type Completion,
  type CompletionContext,
  type CompletionResult,
} from '@codemirror/autocomplete'
import { HighlightStyle, syntaxTree } from '@codemirror/language'
import { type Extension } from '@codemirror/state'
import { Decoration, EditorView } from '@codemirror/view'
import { tags } from '@lezer/highlight'

type ScriptFunction = readonly [name: string, signature: string, description: string]

const scriptFunctions = {
  array: [
    ['join', 'join(values, separator?)', '连接数组元素'],
    ['pop', 'pop(values)', '删除并返回数组最后一个元素'],
    ['push', 'push(values, ...items)', '向数组追加元素'],
  ],
  strings: [
    ['hasPrefix', 'hasPrefix(text, prefix)', '检查字符串前缀'],
    ['hasSuffix', 'hasSuffix(text, suffix)', '检查字符串后缀'],
    ['toLower', 'toLower(text)', '转换为 ASCII 小写'],
    ['toUpper', 'toUpper(text)', '转换为 ASCII 大写'],
    ['trim', 'trim(text, cutset?)', '删除字符串两端内容'],
    ['trimLeft', 'trimLeft(text, cutset?)', '删除字符串左侧内容'],
    ['trimRight', 'trimRight(text, cutset?)', '删除字符串右侧内容'],
    ['split', 'split(text, separators?)', '拆分字符串'],
    ['contains', 'contains(text, value)', '检查字符串是否包含子串'],
    ['contains_any', 'contains_any(text, chars)', '检查字符串是否包含任一字符'],
    ['index', 'index(text, value)', '查找子串位置'],
    ['indexAny', 'indexAny(text, chars)', '查找任一字符的位置'],
    ['lastIndex', 'lastIndex(text, value)', '反向查找子串位置'],
    ['lastIndexAny', 'lastIndexAny(text, chars)', '反向查找任一字符的位置'],
    ['repeat', 'repeat(text, count)', '重复字符串'],
    ['substring', 'substring(text, start?, end?)', '截取字符串'],
    ['toString', 'toString(value?)', '转换为兼容文本'],
  ],
  binary: [
    ['base64Encode', 'base64Encode(value)', '将 Binary 编码为 Base64'],
    ['base64Decode', 'base64Decode(value)', '将 Base64 解码为 Binary'],
    ['hex', 'hex(value)', '将 Binary 编码为十六进制'],
    ['fromHex', 'fromHex(value)', '将十六进制解码为 Binary'],
    ['getUtf8Bytes', 'getUtf8Bytes(value)', '取得值的 UTF-8 字节'],
  ],
  hash: [
    ['crc32', 'crc32(value)', '计算 CRC-32'],
    ['md5', 'md5(value)', '计算 MD5 摘要'],
    ['sha1', 'sha1(value)', '计算 SHA-1 摘要'],
    ['sha256', 'sha256(value)', '计算 SHA-256 摘要'],
  ],
  math: [
    ['floor', 'floor(value)', '向负无穷取整'],
    ['abs', 'abs(value)', '取得绝对值'],
  ],
  rand: [
    ['random', 'random(max?)', '生成随机整数'],
    ['canary', 'canary(ratio, ...keys)', '按比例或稳定键选择流量'],
  ],
  JSON: [
    ['parse', 'parse(text)', '解析 JSON 文本'],
    ['stringify', 'stringify(value)', '序列化为 JSON 文本'],
  ],
  Object: [
    ['assign', 'assign(target, source, ...sources)', '复制对象属性'],
    ['keys', 'keys(value)', '返回对象属性名'],
    ['values', 'values(value)', '返回对象属性值'],
    ['deleteProperties', 'deleteProperties(target, key, ...keys)', '删除对象属性'],
  ],
  URL: [
    ['encodeComponent', 'encodeComponent(value)', '编码 form-urlencoded 组件'],
    ['decodeComponent', 'decodeComponent(value)', '解码 form-urlencoded 组件'],
    ['parseQuery', 'parseQuery(value)', '解析 query string'],
    ['buildQuery', 'buildQuery(value?)', '构造 query string'],
  ],
  req: [
    ['getHeader', 'getHeader(name?)', '读取一个或全部请求头'],
    ['getQuery', 'getQuery(name?)', '读取一个或全部 query 参数'],
    ['getCookie', 'getCookie(name?)', '读取一个或全部 Cookie'],
    ['getUri', 'getUri()', '读取原始 request target'],
    ['getPath', 'getPath()', '读取请求路径'],
    ['getQueryStr', 'getQueryStr()', '读取原始 query string'],
    ['getMethod', 'getMethod()', '读取 HTTP method'],
    ['readJson', 'readJson()', '读取并解析 JSON 请求体'],
    ['readBinary', 'readBinary()', '读取 Binary 请求体'],
    ['discardBody', 'discardBody()', '消费并丢弃请求体'],
  ],
  resp: [
    ['setHeader', 'setHeader(name, value)', '设置响应头'],
    ['addHeader', 'addHeader(name, value)', '追加响应头'],
    ['addCookie', 'addCookie(cookie)', '追加 Set-Cookie'],
    ['sendJson', 'sendJson(status, body)', '发送 JSON 响应'],
    ['send', 'send(status, body?)', '发送空、文本、Binary 或 JSON 响应'],
  ],
} as const satisfies Record<string, readonly ScriptFunction[]>

const constantProperties = {
  $req: ['uri', 'method', 'path', 'query'],
  $conn: ['remote_addr', 'remote_port', 'http_version', 'scheme', 'tls'],
} as const

const directiveFunctions = [
  ['request', 'request(options?)', '发送上游 HTTP 请求并返回 status/body'],
  ['proxyPass', 'proxyPass(options?)', '将当前请求和上游响应流式转发'],
] as const satisfies readonly ScriptFunction[]

const globalCompletions: readonly Completion[] = [
  {
    label: 'length',
    type: 'function',
    detail: 'length(value?)',
    info: '返回 string、Binary、array 或 object 的长度',
  },
  {
    label: 'includes',
    type: 'function',
    detail: 'includes(container, ...items)',
    info: '检查 string 或 array 是否包含全部指定值',
  },
  ...Object.keys(scriptFunctions).map((label) => ({
    label,
    type: label === 'req' || label === 'resp' ? 'variable' : 'namespace',
    detail: label === 'req' ? '请求 API' : label === 'resp' ? '响应 API' : '脚本标准库',
  })),
  ...['$path', '$query', '$header', '$cookie', '$context', '$req', '$conn'].map((label) => ({
    label,
    type: 'constant',
    detail: '请求期常量',
  })),
]

function functionCompletions(functions: readonly ScriptFunction[]): Completion[] {
  return functions.map(([name, signature, description]) => ({
    label: name,
    type: 'function',
    detail: signature,
    info: description,
  }))
}

function isIgnoredSyntax(context: CompletionContext): boolean {
  const node = syntaxTree(context.state).resolveInner(context.pos, -1)
  return /Comment|String|TemplateString/u.test(node.name)
}

function hasHttpDirective(context: CompletionContext, owner: string): boolean {
  const source = context.state.doc.toString()
  directivePattern.lastIndex = 0
  return Array.from(source.matchAll(directivePattern)).some((match) => match[1] === owner)
}

export function completeAccessScript(context: CompletionContext): CompletionResult | null {
  if (isIgnoredSyntax(context)) return null

  const member = context.matchBefore(/[$A-Za-z_][$\w]*\.[$\w]*$/u)
  if (member) {
    const [owner, prefix = ''] = member.text.split('.')
    const functions = scriptFunctions[owner as keyof typeof scriptFunctions]
    if (functions) {
      return {
        from: member.to - prefix.length,
        options: functionCompletions(functions),
        validFor: /^[$\w]*$/u,
      }
    }
    if (owner && hasHttpDirective(context, owner)) {
      return {
        from: member.to - prefix.length,
        options: functionCompletions(directiveFunctions),
        validFor: /^[$\w]*$/u,
      }
    }
    const properties = constantProperties[owner as keyof typeof constantProperties]
    if (properties) {
      return {
        from: member.to - prefix.length,
        options: properties.map((label) => ({ label, type: 'property' })),
        validFor: /^[$\w]*$/u,
      }
    }
    return null
  }

  const word = context.matchBefore(/[$A-Za-z_][$\w]*$/u)
  if (!word && !context.explicit) return null
  return {
    from: word?.from ?? context.pos,
    options: globalCompletions,
    validFor: /^[$\w]*$/u,
  }
}

export const accessScriptHighlightStyle = HighlightStyle.define([
  { tag: [tags.bool, tags.null, tags.atom], color: '#ffcb6b' },
  { tag: tags.number, color: '#f78c6c' },
  { tag: [tags.string, tags.special(tags.string)], color: '#c3e88d' },
  { tag: tags.comment, color: '#8ca59a', fontStyle: 'italic' },
  { tag: [tags.function(tags.variableName), tags.definition(tags.variableName)], color: '#82d7ff' },
  { tag: tags.propertyName, color: '#c7b6ff' },
  { tag: tags.variableName, color: '#e8eee9' },
  { tag: [tags.operator, tags.punctuation], color: '#89ddff' },
  { tag: tags.bracket, color: '#d6e5dd' },
])

const supportedKeywordPattern =
  /\b(?:break|catch|continue|else|for|if|in|let|of|return|throw|try|typeof)\b/gu
const supportedAtomPattern = /\bundefined\b/gu
const directivePattern = /\bdirective\s+([$A-Za-z_][$\w]*)\s*=\s*([$A-Za-z_][$\w]*)\s+(?=["'])/gu

function isInsideTextOrComment(state: Parameters<typeof syntaxTree>[0], position: number): boolean {
  let node: ReturnType<typeof syntaxTree>['topNode'] | null = syntaxTree(state).resolveInner(
    position,
    1,
  )
  while (node) {
    if (/Comment|String|TemplateString/u.test(node.name)) return true
    node = node.parent
  }
  return false
}

function markSupportedTokens(
  state: Parameters<typeof syntaxTree>[0],
  pattern: RegExp,
  decoration: Decoration,
) {
  const ranges = []
  const source = state.doc.toString()
  pattern.lastIndex = 0
  for (const match of source.matchAll(pattern)) {
    const from = match.index
    if (!isInsideTextOrComment(state, from))
      ranges.push(decoration.range(from, from + match[0].length))
  }
  return ranges
}

const supportedKeywordMark = Decoration.mark({ class: 'cm-access-script-keyword' })
const supportedAtomMark = Decoration.mark({ class: 'cm-access-script-atom' })
const directiveKeywordMark = Decoration.mark({ class: 'cm-access-script-directive-keyword' })
const directiveNameMark = Decoration.mark({ class: 'cm-access-script-directive-name' })
const directiveTypeMark = Decoration.mark({ class: 'cm-access-script-directive-type' })

function markDirectives(state: Parameters<typeof syntaxTree>[0]) {
  const ranges = []
  const source = state.doc.toString()
  directivePattern.lastIndex = 0
  for (const match of source.matchAll(directivePattern)) {
    const from = match.index
    const name = match[1]
    const type = match[2]
    if (!name || !type) continue
    if (isInsideTextOrComment(state, from)) continue
    const nameFrom = from + match[0].indexOf(name, 'directive'.length)
    const typeFrom = from + match[0].lastIndexOf(type)
    ranges.push(
      directiveKeywordMark.range(from, from + 'directive'.length),
      directiveNameMark.range(nameFrom, nameFrom + name.length),
      directiveTypeMark.range(typeFrom, typeFrom + type.length),
    )
  }
  return ranges
}

export const accessScriptTokenHighlighting: Extension = [
  EditorView.decorations.compute(['doc'], (state) =>
    Decoration.set(
      [
        ...markSupportedTokens(state, supportedKeywordPattern, supportedKeywordMark),
        ...markSupportedTokens(state, supportedAtomPattern, supportedAtomMark),
        ...markDirectives(state),
      ].sort((left, right) => left.from - right.from),
    ),
  ),
  EditorView.baseTheme({
    '.cm-access-script-keyword': { color: '#ff9dca', fontWeight: '600' },
    '.cm-access-script-atom': { color: '#ffcb6b' },
    '.cm-access-script-directive-keyword': { color: '#ff9dca', fontWeight: '700' },
    '.cm-access-script-directive-name': { color: '#82d7ff', fontWeight: '600' },
    '.cm-access-script-directive-type': { color: '#c7b6ff' },
  }),
]
