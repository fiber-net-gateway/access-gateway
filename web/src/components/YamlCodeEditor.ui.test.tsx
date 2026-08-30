import { CompletionContext } from '@codemirror/autocomplete'
import { javascript } from '@codemirror/lang-javascript'
import { EditorState } from '@codemirror/state'
import { cleanup, render, screen, waitFor } from '@testing-library/react'
import { afterEach, expect, test, vi } from 'vitest'

import { completeAccessScript } from './accessScriptEditor'
import { YamlCodeEditor } from './YamlCodeEditor'

afterEach(cleanup)

test('keeps the same focused CodeMirror instance when an invalid value is rendered', async () => {
  const onChange = vi.fn()
  const onSave = vi.fn()
  const { rerender } = render(
    <YamlCodeEditor
      ariaLabel="Route YAML"
      diagnostics={[]}
      value={'path: /health\ntype: RESPONSE'}
      onChange={onChange}
      onSave={onSave}
    />,
  )
  const editor = screen.getByRole('textbox', { name: 'Route YAML' })
  editor.focus()

  rerender(
    <YamlCodeEditor
      ariaLabel="Route YAML contains errors"
      diagnostics={[
        {
          line: 1,
          column: 1,
          path: 'path',
          code: 'INVALID_ROUTE_PATH',
          message: 'path 必须是非空字符串',
        },
      ]}
      value="path: ["
      onChange={onChange}
      onSave={onSave}
    />,
  )

  await waitFor(() => {
    const updatedEditor = screen.getByRole('textbox', { name: 'Route YAML contains errors' })
    expect(updatedEditor).toBe(editor)
    expect(document.activeElement).toBe(editor)
    expect(document.querySelector('.cm-lintRange-error')).toBeTruthy()
    expect(document.querySelector('.cm-lint-marker-error')).toBeTruthy()
  })
})

test('highlights supported script keywords and directive syntax with a dark-editor palette', async () => {
  render(
    <YamlCodeEditor
      ariaLabel="Route script"
      diagnostics={[]}
      language="javascript"
      value={
        'if (true) resp.sendJson(200, "ok"); class Unsupported;\n' +
        'directive google = http "https://www.google.com";'
      }
      onChange={vi.fn()}
      onSave={vi.fn()}
    />,
  )

  await waitFor(() => {
    const spans = Array.from(document.querySelectorAll<HTMLElement>('.cm-content span'))
    const keyword = spans.find((span) => span.textContent === 'if')
    const string = spans.find((span) => span.textContent === '"ok"')
    const directive = spans.find((span) => span.textContent === 'directive')
    const directiveName = spans.find((span) => span.textContent === 'google')
    const directiveType = spans.find((span) => span.textContent === 'http')
    expect(keyword).toBeTruthy()
    expect(string).toBeTruthy()
    expect(keyword?.classList).toContain('cm-access-script-keyword')
    expect(
      spans
        .filter((span) => span.classList.contains('cm-access-script-keyword'))
        .map((span) => span.textContent),
    ).toEqual(['if'])
    expect(directive?.classList).toContain('cm-access-script-directive-keyword')
    expect(directiveName?.classList).toContain('cm-access-script-directive-name')
    expect(directiveType?.classList).toContain('cm-access-script-directive-type')
    expect(string?.className).not.toBe('')
  })
})

test('completes access-server functions and fixed request properties', () => {
  const memberState = EditorState.create({ doc: 'resp.se', extensions: [javascript()] })
  const memberResult = completeAccessScript(
    new CompletionContext(memberState, memberState.doc.length, false),
  )
  expect(memberResult?.from).toBe('resp.'.length)
  expect(memberResult?.options).toEqual(
    expect.arrayContaining([
      expect.objectContaining({ label: 'send', detail: 'send(status, body?)' }),
      expect.objectContaining({ label: 'sendJson', detail: 'sendJson(status, body)' }),
      expect.objectContaining({ label: 'setHeader', detail: 'setHeader(name, value)' }),
    ]),
  )

  const constantState = EditorState.create({ doc: '$req.m', extensions: [javascript()] })
  const constantResult = completeAccessScript(
    new CompletionContext(constantState, constantState.doc.length, false),
  )
  expect(constantResult?.options.map(({ label }) => label)).toEqual([
    'uri',
    'method',
    'path',
    'query',
  ])

  const directiveState = EditorState.create({
    doc: 'directive google = http "https://www.google.com";\ngoogle.re',
    extensions: [javascript()],
  })
  const directiveResult = completeAccessScript(
    new CompletionContext(directiveState, directiveState.doc.length, false),
  )
  expect(directiveResult?.options).toEqual(
    expect.arrayContaining([
      expect.objectContaining({ label: 'request', detail: 'request(options?)' }),
      expect.objectContaining({ label: 'proxyPass', detail: 'proxyPass(options?)' }),
    ]),
  )
})

test('offers standard-library namespaces but not completions inside strings', () => {
  const rootState = EditorState.create({ doc: 'str', extensions: [javascript()] })
  const rootResult = completeAccessScript(
    new CompletionContext(rootState, rootState.doc.length, false),
  )
  expect(rootResult?.options).toEqual(
    expect.arrayContaining([
      expect.objectContaining({ label: 'strings', type: 'namespace' }),
      expect.objectContaining({ label: 'length', type: 'function' }),
      expect.objectContaining({ label: '$path', type: 'constant' }),
    ]),
  )
  const rootLabels = rootResult?.options.map(({ label }) => label)
  expect(rootLabels).not.toEqual(
    expect.arrayContaining(['Array', 'Math', 'Date', 'Promise', 'fetch', 'directive', 'http']),
  )

  const stringState = EditorState.create({ doc: '"resp.se"', extensions: [javascript()] })
  expect(
    completeAccessScript(new CompletionContext(stringState, stringState.doc.length - 1, false)),
  ).toBeNull()
})
