// Run the real router offline; rejected requests must not reach protected bindings.
// Run: node scripts/test-route-auth.mjs
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import vm from 'node:vm'
import ts from 'typescript'

const { outputText } = ts.transpileModule(
  readFileSync(new URL('../src/index.ts', import.meta.url), 'utf8'),
  { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } },
)
const exports = {}
let queries = 0
let forwards = 0
let storageReads = 0
let adminCalls = 0
const docs = {
  indexDocs: async () => { adminCalls++; return new Response('indexed') },
  vectorSearch: async () => { adminCalls++; return new Response('results') },
}
vm.runInNewContext(outputText, { exports, URL, Response, Headers, require: () => docs })
const env = {
  DEVICE_AUTH_TOKEN: 'test-device-token',
  HISTORY_API_TOKEN: 'test-history-token',
  ADMIN_API_TOKEN: 'test-admin-token',
  LIVE_SESSION: {
    idFromName: name => name,
    get: () => ({ fetch: async () => { forwards++; return new Response('forwarded') } }),
  },
  DB: {
    prepare: () => {
      queries++
      return { bind: id => ({
        all: async () => ({ results: [] }),
        first: async () => id === 'missing' ? null : ({ chat_id: id, device_id: 'device', last_message: 'hello', updated_at: 'now' }),
      }) }
    },
  },
  STORAGE: {
    list: async () => { storageReads++; return { objects: [{ key: 'chat-stick/firmware/waveshare/firmware-v7.bin' }], truncated: false } },
    get: async () => { storageReads++; return { body: 'firmware', size: 8, httpEtag: 'test', writeHttpMetadata() {} } },
  },
}
const fetch = (path, headers = {}, bindings = env) => exports.default.fetch(
  new Request(`https://device.test${path}`, { headers }), bindings,
)
const deviceRoutes = ['/ping', '/firmware/check?device=waveshare', '/firmware/download?device=waveshare', '/ws?device_id=device']
const protectedRoutes = [...deviceRoutes, '/history/device', '/session/existing', '/session/missing', '/admin/index', '/admin/search']
for (const path of protectedRoutes) {
  const base = path.startsWith('/ws') ? { Upgrade: 'websocket' } : {}
  for (const credentials of [{}, { Authorization: 'Bearer wrong-token' }]) {
    assert.equal((await fetch(path, { ...base, ...credentials })).status, 401, path)
  }
}
assert.equal(queries, 0)
assert.equal(storageReads, 0)
assert.equal(forwards, 0)
assert.equal(adminCalls, 0)
for (const path of [...deviceRoutes, '/history/device', '/session/existing']) {
  const base = path.startsWith('/ws') ? { Upgrade: 'websocket' } : {}
  for (const headers of [{ 'X-Device-Token': env.DEVICE_AUTH_TOKEN }, { Authorization: `Bearer ${env.DEVICE_AUTH_TOKEN}` }]) {
    assert.equal((await fetch(path, { ...base, ...headers })).status, 200, path)
  }
  const query = `${path}${path.includes('?') ? '&' : '?'}device_token=${env.DEVICE_AUTH_TOKEN}`
  assert.equal((await fetch(query, base)).status, 200, path)
}
for (const path of ['/history/device', '/session/existing']) {
  assert.equal((await fetch(path, { 'X-History-Token': env.HISTORY_API_TOKEN })).status, 200)
}
assert.equal((await fetch('/session/missing', { 'X-Device-Token': env.DEVICE_AUTH_TOKEN })).status, 404)
for (const path of ['/admin/index', '/admin/search']) {
  assert.equal((await fetch(path, { 'X-Device-Token': env.DEVICE_AUTH_TOKEN })).status, 401)
  assert.equal((await fetch(path, { 'X-Admin-Token': env.ADMIN_API_TOKEN })).status, 200)
}
// Optional-token development compatibility must not silently open history routes.
const optional = { ...env, DEVICE_AUTH_TOKEN: undefined }
for (const path of deviceRoutes) {
  assert.equal((await fetch(path, path.startsWith('/ws') ? { Upgrade: 'websocket' } : {}, optional)).status, 200)
}
for (const path of ['/history/device', '/session/existing']) {
  assert.equal((await fetch(path, {}, optional)).status, 401)
  assert.equal((await fetch(path, { 'X-History-Token': env.HISTORY_API_TOKEN }, optional)).status, 200)
}
console.log('Route auth: rejection before bindings, accepted credentials, and optional-token compatibility passed.')
