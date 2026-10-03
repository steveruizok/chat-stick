import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import vm from 'node:vm'
import ts from 'typescript'
const exports = {}
let fakeFetch
const source = ts.transpileModule(readFileSync(new URL('../src/web-fetch.ts', import.meta.url), 'utf8'), { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText
vm.runInNewContext(source, { exports, URL, AbortController, TextDecoder, Error, setTimeout, clearTimeout, fetch: (...args) => fakeFetch(...args) })
const { fetchWebPage } = exports
let calls = []
fakeFetch = async (url, options) => { calls.push(url); assert.equal(options.redirect, 'manual'); return new Response(null, { status: 302, headers: { location: 'http://127.0.0.1/admin' } }) }
assert.match((await fetchWebPage('https://example.com')).content, /not allowed/)
assert.equal(calls.length, 1)
for (const url of ['http://localhost./', 'http://[::ffff:127.0.0.1]/', 'file:///etc/passwd', 'http://user:pass@example.com']) {
  assert.equal((await fetchWebPage(url)).status, 0)
}
calls = []
fakeFetch = async (url) => { calls.push(url); return calls.length === 1 ? new Response(null, { status: 302, headers: { location: '/next' } }) : new Response('<title>Test</title><script>bad</script>Hello', { headers: { 'content-type': 'text/html' } }) }
const success = await fetchWebPage('https://example.com/start')
assert.equal(success.title, 'Test'); assert.equal(success.content, 'Test Hello'); assert.equal(calls[1], 'https://example.com/next')
calls = []
fakeFetch = async (url) => { calls.push(url); return new Response(null, { status: 301, headers: { location: '/loop' } }) }
assert.match((await fetchWebPage('https://example.com')).content, /too many redirects/); assert.equal(calls.length, 6)
fakeFetch = async () => new Response(new ReadableStream({ start() {} }))
assert.match((await fetchWebPage('https://example.com', 4000, 20)).content, /timed out/)
fakeFetch = async () => new Promise(() => {})
assert.match((await fetchWebPage('https://example.com', 4000, 20)).content, /timed out/)
fakeFetch = async () => new Response('x'.repeat(210000))
assert.equal((await fetchWebPage('https://example.com')).truncated, true)
fakeFetch = async () => new Response('x', { headers: { 'content-length': '210000' } })
assert.match((await fetchWebPage('https://example.com')).content, /too large/)
console.log('Web fetch: private redirects, relative redirects, loops, body/header timeouts and size limits passed.')
