// Exercise the real Worker routes with an in-memory R2 bucket.
// Run: node scripts/test-firmware-routing.mjs
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import vm from 'node:vm'
import ts from 'typescript'

const source = readFileSync(new URL('../src/index.ts', import.meta.url), 'utf8')
const { outputText } = ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
})
const exports = {}
vm.runInNewContext(outputText, {
  exports, URL, Response, Headers,
  require: () => ({}), // Unused websocket/docs handlers need no external services.
})
const keys = [
  'chat-stick/firmware/firmware-v99.bin',
  'chat-stick/firmware/m5-stick/firmware-v16.bin',
  'chat-stick/firmware/waveshare-v2/firmware-v12.bin',
  'chat-stick/firmware/m5-stopwatch/firmware-v2.bin',
  'chat-stick/firmware/m5-stopwatch/firmware-v10.bin',
  'chat-stick/firmware/m5-stopwatch/nested/firmware-v999.bin',
  'chat-stick/firmware/m5-stopwatch/firmware-v100.bin.backup',
]
let downloadedKey
let missingObject = false
const env = {
  STORAGE: {
    list: async ({ prefix, cursor }) => {
      const matching = keys.filter(k => k.startsWith(prefix))
      const offset = Number(cursor || 0)
      const truncated = offset + 1 < matching.length
      return {
        objects: matching.slice(offset, offset + 1).map(key => ({ key })),
        truncated,
        ...(truncated ? { cursor: String(offset + 1) } : {}),
      }
    },
    get: async key => {
      downloadedKey = key
      return missingObject ? null : { body: 'firmware', size: 8, httpEtag: 'test', writeHttpMetadata() {} }
    },
  },
}
const fetch = (path, bindings = env) => exports.default.fetch(new Request(`https://device.test${path}`), bindings)
const update = await (await fetch('/firmware/check?device=m5-stopwatch&version=1')).json()
assert.equal(update.available, true)
assert.equal(update.latest_version, 10)
assert.equal(update.download_url, 'https://device.test/firmware/download?device=m5-stopwatch')
const download = await fetch('/firmware/download?device=m5-stopwatch')
assert.equal(download.status, 200)
assert.equal(await download.text(), 'firmware')
assert.equal(download.headers.get('content-length'), '8')
assert.equal(download.headers.get('content-type'), 'application/octet-stream')
assert.equal(download.headers.get('content-disposition'), 'attachment; filename="firmware-v10.bin"')
assert.equal(downloadedKey, 'chat-stick/firmware/m5-stopwatch/firmware-v10.bin')
assert.equal((await (await fetch('/firmware/check?device=m5-stopwatch&version=10')).json()).available, false)
for (const query of ['', '?device=unknown', '?device=m5-stick']) {
  const legacy = await (await fetch(`/firmware/check${query}`)).json()
  assert.equal(legacy.latest_version, 99)
  assert.equal(legacy.download_url, 'https://device.test/firmware/download?device=m5-stick')
}
assert.equal((await (await fetch('/firmware/check?device=waveshare-v2')).json()).latest_version, 12)
assert.equal((await fetch('/firmware/download?device=waveshare')).status, 404)
keys.splice(3)
const absent = await (await fetch('/firmware/check?device=m5-stopwatch&version=1')).json()
assert.equal(absent.available, false)
assert.equal(absent.download_url, '')
assert.equal((await fetch('/firmware/download?device=m5-stopwatch')).status, 404)
const noStorage = await (await fetch('/firmware/check?version=7', {})).json()
assert.equal(noStorage.available, false)
assert.equal(noStorage.latest_version, 7)
assert.equal(noStorage.download_url, '')
assert.equal((await fetch('/firmware/download', {})).status, 404)
missingObject = true
assert.equal((await fetch('/firmware/download?device=waveshare-v2')).status, 404)
console.log('Firmware routing: pagination, lineage isolation, legacy fallback, and missing storage/releases passed.')
