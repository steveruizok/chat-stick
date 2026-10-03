/** Local-only device protocol fixture. No model calls, recordings, or remote bindings. */
interface FixtureEnv {
  DEVICE_AUTH_TOKEN: string
  FIXTURE: DurableObjectNamespace
}

const json = (value: unknown, status = 200) => Response.json(value, { status })
const send = (socket: WebSocket, value: unknown) => socket.send(JSON.stringify(value))

function tone(): ArrayBuffer {
  // One second, 24 kHz mono PCM, with a quiet 440 Hz tone and click-free edges.
  const samples = new Int16Array(24000)
  for (let i = 0; i < samples.length; i++) {
    const envelope = Math.min(1, i / 480, (samples.length - 1 - i) / 480)
    samples[i] = Math.round(1800 * envelope * Math.sin(2 * Math.PI * 440 * i / 24000))
  }
  return samples.buffer
}

export class HardwareFixture {
  private sockets = new Set<WebSocket>()
  private observations = {
    connections: 0, starts: 0, stops: 0, audioChunks: 0, audioBytes: 0,
    audioPeak: 0, toneReplies: 0, toolResponses: [] as unknown[],
  }

  constructor(_state: DurableObjectState, private env: FixtureEnv) {}

  private reply(socket: WebSocket) {
    if (!this.sockets.has(socket)) return
    send(socket, { type: 'transcript', source: 'model', text: 'Türkçe: çğıöşü ÇĞİÖŞÜ. Yerel cihaz testi.' })
    const audio = tone()
    for (let offset = 0; offset < audio.byteLength; offset += 4800) {
      socket.send(audio.slice(offset, offset + 4800))
    }
    send(socket, { type: 'turn_complete' })
    this.observations.toneReplies++
  }

  async fetch(request: Request): Promise<Response> {
    const { sockets, observations, env } = this
    const url = new URL(request.url)
    if (!env.DEVICE_AUTH_TOKEN || request.headers.get('X-Device-Token') !== env.DEVICE_AUTH_TOKEN) {
      return new Response('Fixture token required', { status: 401 })
    }
    if (url.pathname === '/ping') return new Response('pong')
    if (url.pathname === '/health') return json({ fixture: true, model: false })
    if (url.pathname === '/firmware/check') return json({ available: false, latest_version: 0, download_url: '' })
    if (url.pathname.startsWith('/history/')) return json([])
    if (url.pathname.startsWith('/session/')) return new Response('Not found', { status: 404 })
    if (url.pathname === '/__test/state') {
      return json({ ...observations, connected: sockets.size, modelTested: false, audioRetained: false })
    }
    if (url.pathname === '/__test/control' && request.method === 'POST') {
      if (Number(request.headers.get('Content-Length') || 0) > 8192) return new Response('Too large', { status: 413 })
      const raw = await request.text()
      if (raw.length > 8192) return new Response('Too large', { status: 413 })
      let command: { action?: string; name?: string; args?: unknown; frame?: Record<string, unknown> }
      try { command = JSON.parse(raw) } catch { return new Response('Invalid JSON', { status: 400 }) }
      if (!command || typeof command !== 'object' || Array.isArray(command)) {
        return new Response('Expected an object', { status: 400 })
      }
      if (command.action === 'disconnect') {
        for (const socket of sockets) socket.close(1000, 'Fixture reconnect test')
        return json({ disconnected: sockets.size })
      }
      const id = crypto.randomUUID()
      const allowed = new Set(['get_device_status', 'set_volume', 'set_brightness', 'show_text', 'play_sound', 'set_timer', 'list_timers', 'cancel_timer', 'extend_timer'])
      if (command.action !== 'tone' && command.action !== 'settings' && !allowed.has(command.name || '')) {
        return new Response('Unsupported test action', { status: 400 })
      }
      for (const socket of sockets) {
        if (command.action === 'tone') this.reply(socket)
        else if (command.action === 'settings') send(socket, { ...command.frame, type: 'settings' })
        else send(socket, { type: 'tool_call', id, name: command.name, args: command.args || {} })
      }
      return json({ sent: sockets.size, id })
    }
    if (url.pathname !== '/ws') return new Response('Not found', { status: 404 })
    if (request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') {
      return new Response('Expected WebSocket', { status: 426 })
    }
    const [client, device] = Object.values(new WebSocketPair())
    device.accept()
    // This fixture exercises one physical device. A hard power cut may leave
    // the old TCP socket apparently open until its network timeout expires.
    for (const previous of sockets) previous.close(1000, 'Device reconnected')
    sockets.clear()
    sockets.add(device)
    observations.connections++
    const remove = () => sockets.delete(device)
    device.addEventListener('close', remove)
    device.addEventListener('error', remove)
    device.addEventListener('message', event => {
      if (typeof event.data !== 'string') {
        const data = new DataView(event.data)
        observations.audioChunks++
        observations.audioBytes += data.byteLength
        for (let i = 0; i + 1 < data.byteLength; i += 2) {
          observations.audioPeak = Math.max(observations.audioPeak, Math.abs(data.getInt16(i, true)))
        }
        return
      }
      let message: { type?: string; [key: string]: unknown }
      try { message = JSON.parse(event.data) } catch { return }
      if (!message || typeof message !== 'object') return
      if (message.type === 'start') {
        observations.starts++
        send(device, { type: 'ready' })
      } else if (message.type === 'stop') {
        observations.stops++
        this.reply(device)
      } else if (message.type === 'tool_response') {
        if (message.name === 'get_device_status' && typeof message.result === 'string') {
          try {
            const status = JSON.parse(message.result)
            delete status.wifi_network
            delete status.server_endpoint
            message.result = JSON.stringify(status)
          } catch { message.result = 'Invalid status JSON (omitted)' }
        }
        observations.toolResponses.push(message)
        observations.toolResponses = observations.toolResponses.slice(-30)
      } else if (message.type === 'set_thinking_level') {
        send(device, { type: 'thinking_changed', level: message.level || 'minimal' })
      }
      // Ignore client_log rather than retaining potentially identifying device logs.
    })
    send(device, { type: 'session', chatId: crypto.randomUUID() })
    send(device, { type: 'server_ready' })
    return new Response(null, { status: 101, webSocket: client })
  }
}

export default {
  fetch(request: Request, env: FixtureEnv): Promise<Response> {
    // A single local Durable Object lets control requests use the same socket
    // context as device requests; a Worker-global socket cannot cross requests.
    return env.FIXTURE.get(env.FIXTURE.idFromName('local-test')).fetch(request)
  },
}
