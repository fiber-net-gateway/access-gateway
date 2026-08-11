import { cleanup, render, screen, waitFor } from '@testing-library/react'
import { afterEach, expect, test, vi } from 'vitest'

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
