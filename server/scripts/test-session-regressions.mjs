import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import vm from 'node:vm'
import ts from 'typescript'
import { webcrypto } from 'node:crypto'
const exports = {}
let generateImage
let fetchPage
const source = ts.transpileModule(readFileSync(new URL('../src/live-session.ts', import.meta.url), 'utf8'), { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText
vm.runInNewContext(source, { exports, console, URL, Response, crypto: webcrypto, Date, setTimeout, clearTimeout, require: (id) => id === './image-gen' ? { generateAndProcessImage: (...args) => generateImage(...args), DEFAULT_IMAGE_WIDTH: 232, DEFAULT_IMAGE_HEIGHT: 112 } : id === './web-fetch' ? { fetchWebPage: (...args) => fetchPage(...args) } : {} })
const { LiveSession } = exports
const deferred = () => { let resolve; const promise = new Promise(r => { resolve = r }); return { promise, resolve } }
function session(db = {}) {
  const alarms = []
  const instance = new LiveSession({ storage: { setAlarm: async time => alarms.push(time) }, waitUntil() {} }, { DB: db })
  instance.deviceId = 'device-a'; instance.chatId = 'chat-a'
  const sent = []; instance.deviceWs = { send: data => sent.push(JSON.parse(data)), close() {} }
  return { instance, sent, alarms }
}
// Reject foreign chats before touching the current live session.
{
  const { instance } = session({ prepare: () => ({ bind: () => ({ first: async () => ({ device_id: 'device-b' }) }) }) })
  const socket = instance.deviceWs
  const result = await instance.fetch(new Request('https://example.com/ws?device_id=device-a&chat_id=foreign'))
  assert.equal(result.status, 403); assert.equal(instance.deviceWs, socket); assert.equal(instance.chatId, 'chat-a')
}
// Failed transactions stay queued; overlapping commits preserve turn order and identity.
{
  const rows = new Map(); const logs = []; let fail = true; let active = 0; let peak = 0
  const db = { prepare(sql) { return { bind(...args) { return { sql, args, async first() { const row = rows.get(args[0]); return row?.device === args[1] ? { messages: row.messages } : null } } } } }, async batch(statements) {
    active++; peak = Math.max(peak, active)
    await Promise.resolve(); active--
    if (fail) { fail = false; throw new Error('transient DB failure') }
    const [history, log] = statements; const [chat, device, messages] = history.args
    assert.match(history.sql, /WHERE conversations.device_id = excluded.device_id/)
    const previous = rows.get(chat)
    if (!previous || previous.device === device) { rows.set(chat, { device, messages }); logs.push(log.args) }
  } }
  const { instance } = session(db)
  instance.currentUserText = 'first'; await instance.commitExchange()
  assert.equal(instance.pendingExchanges.length, 1); assert.equal(logs.length, 0)
  instance.currentUserText = 'second'; const a = instance.commitExchange()
  instance.chatId = 'chat-next'; instance.currentAssistantText = 'third'; const b = instance.commitExchange()
  await Promise.all([a, b]); assert.equal(peak, 1); assert.equal(logs.length, 3)
  assert.deepEqual(JSON.parse(rows.get('chat-a').messages).map(m => m.content), ['first', 'second'])
  assert.equal(JSON.parse(rows.get('chat-next').messages)[0].content, 'third')
  await instance.commitExchange(); assert.equal(logs.length, 3)
}
// An idle close leaves the device socket alive; its next input rearms the alarm.
{
  const { instance, alarms } = session(); let closes = 0
  instance.geminiWs = { close() { closes++ } }; instance.lastActivityMs = 0
  await instance.alarm(); assert.equal(closes, 1); assert.equal(instance.geminiWs, null)
  instance.connectGemini = async () => {}; instance.ensureGeminiSession()
  assert.equal(alarms.length, 1); assert.ok(alarms[0] > Date.now())
}
// Generation and persistence both yield: replacing the session during either must
// not send frames, log against the replacement chat, or change its reference image.
for (const boundary of ['generation', 'persistence']) {
  const { instance, sent } = session(); const wait = deferred(); let persisted = 0; let logged = 0
  const result = { data: 'AA==', width: 1, height: 1 }
  generateImage = () => boundary === 'generation' ? wait.promise : Promise.resolve(result)
  instance.persistGeneratedImage = async () => { persisted++; return boundary === 'persistence' ? wait.promise : { imageId: 7 } }
  instance.logToolCall = async () => { logged++ }
  const pending = instance.generateAndSendImage('image', 'show_image', {}, Date.now())
  await new Promise(resolve => setImmediate(resolve))
  instance.sessionGeneration++; instance.chatId = 'replacement'
  wait.resolve(boundary === 'generation' ? result : { imageId: 7 }); await pending
  assert.equal(sent.length, 0); assert.equal(logged, 0); assert.ok(!instance.lastImageId)
  assert.equal(persisted, boundary === 'generation' ? 0 : 1)
}
{
  const { instance, sent } = session(); const wait = deferred()
  instance.persistGeneratedImage = () => wait.promise
  const pending = instance.persistAndSendAnimationFrame({ data: 'AA==', width: 1, height: 1 }, 'frame', 'chat-a', 'group', 0, 2)
  instance.chatId = 'replacement'; wait.resolve({ imageId: 8 }); await pending
  assert.equal(sent.length, 0); assert.ok(!instance.lastImageId)
}
// A long server tool must not respond or log into a replacement Gemini session.
{
  const { instance, sent } = session(); const wait = deferred(); let responses = 0; let logs = 0
  instance.geminiWs = { send() { responses++ } }
  instance.logToolCall = async () => { logs++ }
  fetchPage = () => wait.promise
  const pending = instance.handleGeminiMessage({ toolCall: { functionCalls: [{ name: 'web_fetch', id: 'tool', args: { url: 'https://example.com' } }] } })
  instance.geminiWs = { send() { responses++ } }
  wait.resolve({ content: 'done' }); await pending
  assert.equal(responses, 0); assert.equal(logs, 0); assert.equal(sent.length, 0)
}
// Mode switches must check identity inside the helper, before continuing after
// commit, resumption deletion, or preference persistence yields to a reconnect.
for (const boundary of ['commit', 'clear', 'save']) {
  for (const replacement of ['session', 'gemini']) {
    const { instance, sent } = session(); const wait = deferred()
    let clears = 0; let saves = 0; let reconnects = 0
    instance.currentThinkingLevel = 'minimal'
    instance.geminiWs = { close() {} }
    instance.commitExchange = () => boundary === 'commit' ? wait.promise : Promise.resolve()
    instance.clearSessionResumptionHandle = () => { clears++; return boundary === 'clear' ? wait.promise : Promise.resolve() }
    instance.saveThinkingLevelForChat = () => { saves++; return boundary === 'save' ? wait.promise : Promise.resolve() }
    instance.reconnectGeminiSession = async () => { reconnects++ }
    const pending = instance.switchThinkingLevel('high')
    await new Promise(resolve => setImmediate(resolve))
    if (replacement === 'session') { instance.sessionGeneration++; instance.chatId = 'replacement' }
    instance.geminiWs = { close() {} }; instance.currentThinkingLevel = 'low'
    instance.sessionResumptionHandle = 'replacement-handle'
    wait.resolve(); await pending
    assert.equal(instance.currentThinkingLevel, 'low', `${boundary}/${replacement}: replacement preference`)
    assert.equal(instance.sessionResumptionHandle, 'replacement-handle')
    assert.equal(clears, boundary === 'commit' ? 0 : 1)
    assert.equal(saves, boundary === 'save' ? 1 : 0)
    assert.equal(reconnects, 0); assert.equal(sent.length, 0)
  }
}
// A reconnect waiting for old resumption storage must not close a new socket.
for (const replacement of ['session', 'gemini']) {
  const { instance } = session(); const wait = deferred(); let closes = 0; let connects = 0
  instance.geminiWs = { close() { closes++ } }
  instance.clearSessionResumptionHandle = () => wait.promise
  instance.connectGemini = async () => { connects++ }
  const pending = instance.reconnectGeminiSession({ clearResumptionHandle: true })
  if (replacement === 'session') { instance.sessionGeneration++; instance.chatId = 'replacement' }
  const replacementSocket = { close() { closes++ } }
  instance.geminiWs = replacementSocket; instance.geminiReady = true; instance.geminiConnecting = true
  wait.resolve(); await pending
  assert.equal(instance.geminiWs, replacementSocket); assert.equal(instance.geminiReady, true)
  assert.equal(instance.geminiConnecting, true); assert.equal(closes, 0); assert.equal(connects, 0)
}
console.log('Session regressions: ownership, queued transactional retry, serialization, idle rearm and stale image/animation continuations passed.')
