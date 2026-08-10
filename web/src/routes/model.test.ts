import assert from 'node:assert/strict'
import test from 'node:test'

import { initialRouteModel, parseRouteModel } from './model.js'

test('creates an empty route model bound to the project domain', () => {
  assert.deepEqual(initialRouteModel('api.example.com'), {
    schemaVersion: 1,
    kind: 'project_route',
    hosts: [{ pattern: 'api.example.com' }],
    routes: [],
  })
})

test('parses a complete project route model without dropping advanced fields', () => {
  const model = parseRouteModel(`{
    "schemaVersion": 1,
    "kind": "project_route",
    "hosts": [{"pattern": "api.example.com"}],
    "routes": [{"path": "/v1/*", "type": "PROXY", "condition": "$req.method == 'GET'"}]
  }`)
  assert.equal((model.routes[0] as { condition: string }).condition, "$req.method == 'GET'")
})

test('rejects malformed JSON and incompatible outer schemas', () => {
  assert.throws(() => parseRouteModel('{'))
  assert.throws(() => parseRouteModel('[]'), /JSON object/u)
  assert.throws(
    () => parseRouteModel('{"schemaVersion":2,"kind":"project_route","hosts":[],"routes":[]}'),
    /schemaVersion=1/u,
  )
})
