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
]
let downloadedKey
const env = {
  STORAGE: {
    list: async ({ prefix }) => ({ objects: keys.filter(k => k.startsWith(prefix)).map(key => ({ key })) }),
    get: async key => {
      downloadedKey = key
      return { body: 'firmware', size: 8, httpEtag: 'test', writeHttpMetadata() {} }
    },
  },
}
const fetch = path => exports.default.fetch(new Request(`https://device.test${path}`), env)
const update = await (await fetch('/firmware/check?device=m5-stopwatch&version=1')).json()
assert.equal(update.available, true)
assert.equal(update.latest_version, 2)
assert.equal(update.download_url, 'https://device.test/firmware/download?device=m5-stopwatch')
await fetch('/firmware/download?device=m5-stopwatch')
assert.equal(downloadedKey, 'chat-stick/firmware/m5-stopwatch/firmware-v2.bin')
assert.equal((await (await fetch('/firmware/check?device=m5-stopwatch&version=2')).json()).available, false)
keys.pop()
const absent = await (await fetch('/firmware/check?device=m5-stopwatch&version=1')).json()
assert.equal(absent.available, false)
assert.equal(absent.download_url, '')
assert.equal((await fetch('/firmware/download?device=m5-stopwatch')).status, 404)
console.log('Firmware routing: StopWatch lineage, version checks, and missing-release isolation passed.')
