import {
  autocompletion,
  closeBrackets,
  closeBracketsKeymap,
  completionKeymap,
  type Completion,
  type CompletionContext,
  type CompletionResult,
} from '@codemirror/autocomplete'
import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands'
import {
  bracketMatching,
  defaultHighlightStyle,
  foldGutter,
  foldKeymap,
  indentOnInput,
  syntaxHighlighting,
} from '@codemirror/language'
import { yaml } from '@codemirror/lang-yaml'
import { highlightSelectionMatches, searchKeymap } from '@codemirror/search'
import { Compartment, EditorState } from '@codemirror/state'
import {
  crosshairCursor,
  drawSelection,
  dropCursor,
  EditorView,
  highlightActiveLine,
  highlightActiveLineGutter,
  highlightSpecialChars,
  keymap,
  lineNumbers,
  rectangularSelection,
} from '@codemirror/view'
import { useEffect, useRef } from 'react'

interface YamlCodeEditorProps {
  ariaLabel: string
  value: string
  onChange(value: string): void
  onSave(): void
}

const routeCompletions: readonly Completion[] = [
  { label: 'path', type: 'property', apply: 'path: /', detail: '请求路径' },
  { label: 'type', type: 'property', apply: 'type: PROXY', detail: 'PROXY / RESPONSE' },
  { label: 'service', type: 'property', apply: 'service: ', detail: 'NamingService 服务' },
  { label: 'cluster', type: 'property', apply: 'cluster: ', detail: '上游集群' },
  { label: 'addresses', type: 'property', apply: 'addresses:\n  - ', detail: '静态上游地址' },
  { label: 'condition', type: 'property', apply: 'condition: ', detail: '同步条件表达式' },
  { label: 'rewrite', type: 'property', apply: 'rewrite: /', detail: '上游路径模板' },
  { label: 'timeout', type: 'property', apply: 'timeout: 30s', detail: '代理超时' },
  { label: 'status', type: 'property', apply: 'status: 200', detail: 'RESPONSE 状态码' },
  {
    label: 'body',
    type: 'property',
    apply: 'body:\n  type: TEXT\n  content: ',
    detail: 'RESPONSE body',
  },
  {
    label: 'proxy_headers',
    type: 'property',
    apply: 'proxy_headers:\n  X-Header: value',
    detail: '上游请求头模板',
  },
  {
    label: 'response_headers',
    type: 'property',
    apply: 'response_headers:\n  X-Header: value',
    detail: '下游响应头模板',
  },
  { label: 'context', type: 'property', apply: 'context:\n  cluster: ', detail: '追踪上下文' },
  {
    label: 'max_client_body_size',
    type: 'property',
    apply: 'max_client_body_size: 4m',
    detail: '请求体限制',
  },
  {
    label: 'max_proxy_body_size',
    type: 'property',
    apply: 'max_proxy_body_size: 20m',
    detail: '上游响应体限制',
  },
  {
    label: 'websocket_timeout',
    type: 'property',
    apply: 'websocket_timeout: 300s',
    detail: 'WebSocket 超时',
  },
  { label: 'flush', type: 'property', apply: 'flush: false', detail: '流式刷新' },
  { label: 'allows', type: 'property', apply: 'allows:\n  - 10.0.0.0/8', detail: 'CIDR 规则' },
]

function completeRouteField(context: CompletionContext): CompletionResult | null {
  const line = context.state.doc.lineAt(context.pos)
  const prefix = line.text.slice(0, context.pos - line.from)
  if (/^\s+/u.test(prefix)) return null
  const word = context.matchBefore(/[a-z_]*/u)
  if (!word || (word.from === word.to && !context.explicit)) return null
  return { from: word.from, options: routeCompletions }
}

const editorTheme = EditorView.theme({
  '&': {
    minHeight: '210px',
    backgroundColor: '#12201a',
    color: '#e8eee9',
    fontSize: '13px',
  },
  '&.cm-focused': { outline: 'none' },
  '.cm-scroller': {
    overflow: 'auto',
    fontFamily: "'SFMono-Regular', Consolas, 'Liberation Mono', monospace",
    lineHeight: '1.65',
  },
  '.cm-content': { padding: '14px 0', caretColor: '#d8f36c' },
  '.cm-line': { padding: '0 16px' },
  '.cm-gutters': {
    border: '0',
    borderRight: '1px solid rgba(255, 255, 255, 0.08)',
    backgroundColor: '#12201a',
    color: '#657a70',
  },
  '.cm-activeLine, .cm-activeLineGutter': { backgroundColor: 'rgba(216, 243, 108, 0.06)' },
  '.cm-selectionBackground, &.cm-focused .cm-selectionBackground': {
    backgroundColor: 'rgba(126, 164, 143, 0.32)',
  },
  '.cm-cursor, .cm-dropCursor': { borderLeftColor: '#d8f36c' },
  '.cm-foldPlaceholder': { backgroundColor: '#294238', border: 0, color: '#b8c9c0' },
})

export function YamlCodeEditor({ ariaLabel, value, onChange, onSave }: YamlCodeEditorProps) {
  const hostRef = useRef<HTMLDivElement | null>(null)
  const editorRef = useRef<EditorView | null>(null)
  const ariaLabelCompartmentRef = useRef<Compartment | null>(null)
  ariaLabelCompartmentRef.current ??= new Compartment()
  const ariaLabelCompartment = ariaLabelCompartmentRef.current
  const changeRef = useRef(onChange)
  const saveRef = useRef(onSave)
  changeRef.current = onChange
  saveRef.current = onSave

  useEffect(() => {
    if (!hostRef.current) return
    const view = new EditorView({
      parent: hostRef.current,
      state: EditorState.create({
        doc: value,
        extensions: [
          lineNumbers(),
          highlightActiveLineGutter(),
          highlightSpecialChars(),
          history(),
          foldGutter(),
          drawSelection(),
          dropCursor(),
          EditorState.allowMultipleSelections.of(true),
          indentOnInput(),
          syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
          bracketMatching(),
          closeBrackets(),
          autocompletion({ override: [completeRouteField] }),
          rectangularSelection(),
          crosshairCursor(),
          highlightActiveLine(),
          highlightSelectionMatches(),
          yaml(),
          editorTheme,
          ariaLabelCompartment.of(EditorView.contentAttributes.of({ 'aria-label': ariaLabel })),
          EditorView.updateListener.of((update) => {
            if (update.docChanged) changeRef.current(update.state.doc.toString())
          }),
          keymap.of([
            {
              key: 'Mod-s',
              run: () => {
                saveRef.current()
                return true
              },
            },
            indentWithTab,
            ...closeBracketsKeymap,
            ...completionKeymap,
            ...defaultKeymap,
            ...searchKeymap,
            ...historyKeymap,
            ...foldKeymap,
          ]),
        ],
      }),
    })
    editorRef.current = view
    return () => {
      editorRef.current = null
      view.destroy()
    }
  }, [])

  useEffect(() => {
    const view = editorRef.current
    if (!view) return
    view.dispatch({
      effects: ariaLabelCompartment.reconfigure(
        EditorView.contentAttributes.of({ 'aria-label': ariaLabel }),
      ),
    })
  }, [ariaLabel, ariaLabelCompartment])

  useEffect(() => {
    const view = editorRef.current
    if (!view || view.state.doc.toString() === value) return
    view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: value } })
  }, [value])

  return <div className="yaml-code-editor" ref={hostRef} />
}
