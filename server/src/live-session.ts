import { searchDocsKeyword, searchDocsVector } from './docs-search'

interface Env {
	GEMINI_API_KEY: string
	AI: Ai
	VECTORIZE: VectorizeIndex
	DB: D1Database
}

interface ConversationMessage {
	role: 'user' | 'assistant'
	content: string
}

interface GeminiMessage {
	setupComplete?: Record<string, unknown>
	serverContent?: {
		modelTurn?: {
			parts?: Array<{
				inlineData?: { mimeType: string; data: string }
				text?: string
			}>
		}
		turnComplete?: boolean
		inputTranscription?: { text: string }
		outputTranscription?: { text: string }
	}
	toolCall?: {
		functionCalls: Array<{
			name: string
			id: string
			args: Record<string, unknown>
		}>
	}
}

interface WebFetchArgs {
	url?: string
	max_chars?: number
}

export class LiveSession {
	private state: DurableObjectState
	private env: Env
	private deviceWs: WebSocket | null = null
	private geminiWs: WebSocket | null = null
	private geminiReady = false
	private deviceId = 'unknown'
	private chatId = ''
	private currentUserText = ''
	private currentAssistantText = ''
	private sessionGeneration = 0
	// Pending device-side tool calls keyed by call id → { name, args, startMs }
	private pendingDeviceCalls = new Map<string, { name: string; args: unknown; startMs: number }>()

	constructor(state: DurableObjectState, env: Env) {
		this.state = state
		this.env = env
	}

	async fetch(request: Request): Promise<Response> {
		const sessionGeneration = ++this.sessionGeneration
		await this.saveConversation()
		this.cleanup()

		// Extract device_id and chat_id from URL
		const url = new URL(request.url)
		this.deviceId = url.searchParams.get('device_id') || 'unknown'
		this.chatId = url.searchParams.get('chat_id') || crypto.randomUUID()

		console.log(`[Device] Connected: device=${this.deviceId} chat=${this.chatId}`)

		const pair = new WebSocketPair()
		const [client, server] = Object.values(pair)

		server.accept()
		this.deviceWs = server

		// Send chat_id to device (in case it was server-generated)
		this.sendToDevice({ type: 'session', chatId: this.chatId })

		server.addEventListener('message', (event) => {
			if (sessionGeneration !== this.sessionGeneration) return
			this.onDeviceMessage(event.data)
		})

		server.addEventListener('close', async () => {
			if (sessionGeneration !== this.sessionGeneration) return
			console.log('[Device] Disconnected')
			await this.saveConversation()
			this.cleanup()
		})

		server.addEventListener('error', (event) => {
			if (sessionGeneration !== this.sessionGeneration) return
			console.error('[Device] WebSocket error:', event)
		})

		// Connect to Gemini Live API
		await this.connectGemini(sessionGeneration)

		return new Response(null, { status: 101, webSocket: client })
	}

	private async connectGemini(sessionGeneration = this.sessionGeneration) {
		const url =
			'https://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent' +
			`?key=${this.env.GEMINI_API_KEY}`

		try {
			const resp = await fetch(url, {
				headers: { Upgrade: 'websocket' },
			})

			const ws = resp.webSocket
			if (!ws) {
				console.error('[Gemini] WebSocket upgrade failed')
				this.sendToDevice({ type: 'error', message: 'Failed to connect to AI' })
				return
			}
			if (sessionGeneration !== this.sessionGeneration) {
				try {
					ws.close()
				} catch {
					// ignore
				}
				return
			}

			ws.accept()
			this.geminiWs = ws

			ws.addEventListener('message', (event) => {
				if (sessionGeneration !== this.sessionGeneration) return
				const raw = event.data
				const text =
					typeof raw === 'string'
						? raw
						: new TextDecoder().decode(raw as ArrayBuffer)
				this.onGeminiMessage(text).catch((err) => {
					console.error('[Gemini] Message handler error:', err)
				})
			})

			ws.addEventListener('close', (event) => {
				if (sessionGeneration !== this.sessionGeneration) return
				console.log(`[Gemini] Disconnected: code=${event.code} reason="${event.reason}" wasReady=${this.geminiReady}`)
				this.geminiWs = null
				this.geminiReady = false
				// Don't send error if we haven't set up yet — connectGemini will retry
			})

			ws.addEventListener('error', (event) => {
				if (sessionGeneration !== this.sessionGeneration) return
				console.error('[Gemini] WebSocket error:', event)
			})

			// Send session setup
			ws.send(
				JSON.stringify({
					setup: {
						model: 'models/gemini-3.1-flash-live-preview',
						generationConfig: {
							responseModalities: ['AUDIO'],
						},
						systemInstruction: {
							parts: [
								{
									text: [
										'You are a voice assistant running on an M5StickS3 — a tiny handheld ESP32-S3 device.',
										'',
										'Device specs:',
										'- Display: 135×240 pixel color LCD (ST7789)',
										'- Speaker: 8Ω 1W cavity speaker with AW8737 amplifier',
										'- Microphone: MEMS mic (SPM1423)',
										'- Battery: 250mAh rechargeable',
										'- Connectivity: WiFi 2.4GHz',
										'- Size: 48×24×15mm — fits in a palm',
										'',
										'The user holds a button to talk and releases to hear your response.',
										'Keep responses concise — the speaker is tiny. Be helpful, warm, and conversational.',
										'',
										'You can control the device using the available tools:',
										'- set_brightness: adjust display backlight',
										'- set_volume: adjust speaker volume',
										'- show_text: display a message on the screen',
										'- play_sound: play a named device sound effect',
										'- play_melody: play a short note sequence on the device speaker',
										'- power_off: shut the device down',
										'- get_device_status: check battery, volume, brightness, etc.',
										'- search_docs: search the indexed knowledge base',
										'- web_fetch: fetch a specific URL and read its text content',
										'- google_search: search the web for current information (news, facts, recent events)',
										'- new_conversation: reset the chat (say goodbye first)',
										'- new_chat: reset the chat (alias for new_conversation)',
										'',
										'You have a search_docs tool that searches an indexed knowledge base.',
										'Use it when the user asks about topics that may be covered in the indexed documents.',
										'',
										'Use tools when the user asks to change device settings or needs information.',
										'When you don\'t understand the audio, say so briefly rather than guessing.',
									].join('\n'),
								},
							],
						},
						tools: [
							{ googleSearch: {} },
							{
								functionDeclarations: [
									{
										name: 'set_brightness',
										description: 'Set the display backlight brightness',
										parameters: {
											type: 'OBJECT',
											properties: {
												level: {
													type: 'INTEGER',
													description:
														'Brightness level from 0 (off) to 255 (maximum)',
												},
											},
											required: ['level'],
										},
									},
									{
										name: 'set_volume',
										description: 'Set the speaker volume',
										parameters: {
											type: 'OBJECT',
											properties: {
												level: {
													type: 'INTEGER',
													description:
														'Volume level from 0 (mute) to 255 (maximum)',
												},
											},
											required: ['level'],
										},
									},
										{
											name: 'show_text',
										description:
											'Display a short text message on the device screen. Max ~20 characters per line, 7 lines.',
										parameters: {
											type: 'OBJECT',
											properties: {
												text: {
													type: 'STRING',
													description: 'Text to display',
												},
											},
											required: ['text'],
										},
										},
										{
											name: 'play_sound',
											description:
												'Play a named sound effect on the device speaker.',
											parameters: {
												type: 'OBJECT',
												properties: {
													sound: {
														type: 'STRING',
														description:
															'One of: beep, success, error, alert, fanfare',
													},
												},
												required: ['sound'],
											},
										},
										{
											name: 'play_melody',
											description:
												'Play a short melody on the device speaker using note tokens like "C4:200 E4:200 G4:400". Use R for rests.',
											parameters: {
												type: 'OBJECT',
												properties: {
													notes: {
														type: 'STRING',
														description:
															'Space-separated note tokens with durations in milliseconds, for example "C4:200 E4:200 G4:400"',
													},
												},
												required: ['notes'],
											},
										},
										{
											name: 'power_off',
											description: 'Power the device off immediately.',
											parameters: {
												type: 'OBJECT',
												properties: {},
											},
										},
										{
											name: 'get_device_status',
										description:
											'Get current device status including battery level, volume, brightness, WiFi network, and uptime.',
										parameters: {
											type: 'OBJECT',
											properties: {},
										},
									},
									{
										name: 'search_docs',
										description:
											'Search the indexed knowledge base. Returns relevant documentation entries matching the query.',
										parameters: {
											type: 'OBJECT',
											properties: {
												query: {
													type: 'STRING',
													description:
														'Search query — keywords or a question',
												},
											},
											required: ['query'],
										},
									},
									{
										name: 'web_fetch',
										description:
											'Fetch a web page or API endpoint and return cleaned text content. Use this to look up current information, read articles, or access APIs.',
										parameters: {
											type: 'OBJECT',
											properties: {
												url: {
													type: 'STRING',
													description:
														'The full URL to fetch (must include https:// or http://)',
												},
												max_chars: {
													type: 'INTEGER',
													description:
														'Maximum characters of cleaned text to return, from 500 to 10000. Defaults to 4000.',
												},
											},
											required: ['url'],
										},
									},
										{
											name: 'new_conversation',
										description:
											'Start a fresh conversation. Call this when the user wants to reset the chat or change topics completely. Say a brief goodbye before calling this tool.',
										parameters: {
											type: 'OBJECT',
											properties: {},
											},
										},
										{
											name: 'new_chat',
											description:
												'Start a fresh conversation. Alias for new_conversation.',
											parameters: {
												type: 'OBJECT',
												properties: {},
											},
										},
									],
								},
							],
					},
				})
			)

			console.log('[Gemini] Setup message sent')
		} catch (err) {
			console.error('[Gemini] Connection error:', err)
			this.sendToDevice({
				type: 'error',
				message: `AI connection failed: ${err}`,
			})
		}
	}

	private audioChunkCount = 0

	private onDeviceMessage(data: string | ArrayBuffer) {
		if (data instanceof ArrayBuffer) {
			// Binary frame = raw PCM audio from device mic
			if (!this.geminiWs) {
				console.warn('[Bridge] Audio dropped — Gemini WS is null, reconnecting...')
				this.connectGemini()
				return
			}
			if (!this.geminiReady) {
				console.warn('[Bridge] Audio dropped — Gemini not ready yet')
				return
			}

			this.audioChunkCount++
			if (this.audioChunkCount <= 3 || this.audioChunkCount % 10 === 0) {
				// Log first few samples as int16 for debugging
				const view = new Int16Array(data)
				const firstSamples = Array.from(view.slice(0, 4))
				console.log(`[Bridge] Audio chunk #${this.audioChunkCount}: ${data.byteLength} bytes → Gemini (samples: ${firstSamples})`)
			}

			const base64 = arrayBufferToBase64(data)
			this.geminiWs.send(
				JSON.stringify({
					realtimeInput: {
						audio: {
							data: base64,
							mimeType: 'audio/pcm;rate=16000',
						},
					},
				})
			)
		} else {
			// Text frame = control message
			try {
				const msg = JSON.parse(data)
				console.log('[Device]', msg.type)

				// Forward tool response to Gemini
				if (msg.type === 'tool_response' && this.geminiWs && this.geminiReady) {
					console.log(`[Bridge] Tool response: ${msg.name} → Gemini`)
					this.geminiWs.send(
						JSON.stringify({
							toolResponse: {
								functionResponses: [
									{
										name: msg.name,
										id: msg.id,
										response: { result: msg.result },
									},
								],
							},
						})
					)
					const pending = this.pendingDeviceCalls.get(msg.id)
					if (pending) {
						this.pendingDeviceCalls.delete(msg.id)
						this.logToolCall({
							name: pending.name,
							args: pending.args,
							result: msg.result,
							handledBy: 'device',
							durationMs: Date.now() - pending.startMs,
						}).catch(() => {})
					}
				}

				// Forward text input to Gemini
				if (msg.type === 'text' && msg.content && this.geminiWs && this.geminiReady) {
					this.geminiWs.send(
						JSON.stringify({
							realtimeInput: { text: msg.content },
						})
					)
				}

				// Send trailing silence so Gemini's VAD detects end-of-speech
				if (msg.type === 'stop' && this.geminiWs && this.geminiReady) {
					this.audioChunkCount = 0
					this.sendTrailingSilence()
				}
			} catch {
				console.warn('[Device] Unparseable message:', data)
			}
		}
	}

	private async onGeminiMessage(data: string) {
		let msg: GeminiMessage
		try {
			msg = JSON.parse(data)
		} catch {
			console.warn('[Gemini] Unparseable message:', typeof data, data.substring(0, 200))
			return
		}

		try {
			await this.handleGeminiMessage(msg)
		} catch (err) {
			console.error('[Gemini] Error handling message:', err)
		}
	}

	private async handleGeminiMessage(msg: GeminiMessage) {

		// Session ready
		if (msg.setupComplete) {
			console.log('[Gemini] Setup complete')
			this.geminiReady = true
			this.sendToDevice({ type: 'ready' })
			return
		}

		if (msg.serverContent) {
			const sc = msg.serverContent

			// Model audio — decode base64 and forward as raw binary
			if (sc.modelTurn?.parts) {
				for (const part of sc.modelTurn.parts) {
					if (part.inlineData?.data) {
						const raw = base64ToArrayBuffer(part.inlineData.data)
						this.deviceWs?.send(raw)
					}
				}
			}

			// Turn complete — save exchange to D1
			if (sc.turnComplete) {
				this.sendToDevice({ type: 'turn_complete' })
				await this.commitExchange()
			}

			// Transcriptions — accumulate for DB storage
			if (sc.inputTranscription?.text) {
				this.currentUserText += sc.inputTranscription.text
				this.sendToDevice({
					type: 'transcript',
					source: 'user',
					text: sc.inputTranscription.text,
				})
			}
			if (sc.outputTranscription?.text) {
				this.currentAssistantText += sc.outputTranscription.text
				this.sendToDevice({
					type: 'transcript',
					source: 'model',
					text: sc.outputTranscription.text,
				})
			}
		}

		if (msg.toolCall) {
			for (const call of msg.toolCall.functionCalls) {
				console.log(`[Gemini] Tool call: ${call.name}(${JSON.stringify(call.args)})`)
				const startMs = Date.now()

				if (call.name === 'search_docs') {
					const query = (call.args as { query?: string }).query || ''
					console.log(`[Gemini] Docs search: "${query}"`)

					let results: { title: string; section: string; content: string; score: number }[] = []
					let searchMode: 'vector' | 'keyword' = 'vector'

					try {
						results = await searchDocsVector(query, this.env, 3)
					} catch (err) {
						console.warn('[Gemini] Vector search failed, falling back to keyword search:', err)
					}

					if (results.length === 0) {
						searchMode = 'keyword'
						results = searchDocsKeyword(query, 3)
					}

					console.log(
						`[Gemini] Found ${results.length} ${searchMode} results (top: ${results[0]?.title})`
					)
					const searchResults = results.map((r) => ({
						title: r.title,
						section: r.section,
						content: r.content.slice(0, 1000),
						score: r.score,
					}))
					const payload = JSON.stringify({
						toolResponse: {
							functionResponses: [
								{
									name: call.name,
									id: call.id,
									response: { results: searchResults },
								},
							],
						},
					})
					console.log(`[Gemini] Sending tool response: ${payload.length} bytes`)

					if (this.geminiWs) {
						this.geminiWs.send(payload)
						console.log(`[Gemini] Tool response sent`)
					}
					await this.logToolCall({
						name: call.name,
						args: call.args,
						result: {
							mode: searchMode,
							count: searchResults.length,
							titles: searchResults.map((r) => r.title),
						},
						handledBy: 'server',
						durationMs: Date.now() - startMs,
					})
				} else if (call.name === 'web_fetch' || call.name === 'fetch_url') {
						const args = call.args as WebFetchArgs
						const url = args.url || ''
						console.log(`[Gemini] Fetching: ${url}`)
						const result = await fetchWebPage(url, args.max_chars)
						const payload = JSON.stringify({
							toolResponse: {
								functionResponses: [
									{
										name: call.name,
										id: call.id,
										response: result,
									},
								],
							},
						})
						if (this.geminiWs) {
							this.geminiWs.send(payload)
							console.log(`[Gemini] ${call.name} response sent (${result.content.length} chars)`)
						}
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: { url, chars: result.content.length },
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
				} else if (call.name === 'new_conversation' || call.name === 'new_chat') {
						// Handle server-side: close Gemini session and open a fresh one
						console.log('[Gemini] Resetting conversation')
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: 'conversation reset',
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
						await this.commitExchange()
						this.chatId = crypto.randomUUID()
						this.currentUserText = ''
						this.currentAssistantText = ''
						this.sendToDevice({ type: 'session', chatId: this.chatId })
						if (this.geminiWs) {
							try { this.geminiWs.close() } catch { /* ignore */ }
							this.geminiWs = null
							this.geminiReady = false
						}
					await this.connectGemini()
				} else {
					// Forward tool call to device for execution — log when response arrives
					this.pendingDeviceCalls.set(call.id, {
						name: call.name,
						args: call.args,
						startMs,
					})
					this.sendToDevice({
						type: 'tool_call',
						name: call.name,
						id: call.id,
						args: call.args,
					})
				}
			}
		}
	}

	private sendToDevice(msg: Record<string, unknown>) {
		this.deviceWs?.send(JSON.stringify(msg))
	}

	private async logToolCall(entry: {
		name: string
		args: unknown
		result?: unknown
		handledBy: 'server' | 'device'
		status?: 'ok' | 'error'
		error?: string
		durationMs?: number
	}) {
		try {
			const argsStr = entry.args === undefined ? null : JSON.stringify(entry.args)
			const resultStr =
				entry.result === undefined
					? null
					: typeof entry.result === 'string'
						? entry.result
						: JSON.stringify(entry.result)
			// Truncate oversized payloads so we don't blow up D1
			const trim = (s: string | null) =>
				s && s.length > 8000 ? s.slice(0, 8000) + '…[truncated]' : s

			await this.env.DB.prepare(
				`INSERT INTO tool_log (device_id, chat_id, tool_name, args, result, handled_by, status, error, duration_ms)
				 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
			)
				.bind(
					this.deviceId,
					this.chatId,
					entry.name,
					trim(argsStr),
					trim(resultStr),
					entry.handledBy,
					entry.status ?? 'ok',
					entry.error ?? null,
					entry.durationMs ?? null
				)
				.run()
			console.log(
				`[ToolLog] ${entry.name} by=${entry.handledBy} status=${entry.status ?? 'ok'}` +
					(entry.durationMs !== undefined ? ` ${entry.durationMs}ms` : '')
			)
		} catch (err) {
			console.error('[ToolLog] Failed to insert:', err)
		}
	}

	private async commitExchange() {
		const user = this.currentUserText.trim()
		const assistant = this.currentAssistantText.trim()
		this.currentUserText = ''
		this.currentAssistantText = ''

		if (!user && !assistant) return

		try {
			// Get existing messages
			const row = await this.env.DB.prepare(
				'SELECT messages FROM conversations WHERE chat_id = ?'
			)
				.bind(this.chatId)
				.first<{ messages: string }>()

			const messages: ConversationMessage[] = row?.messages
				? JSON.parse(row.messages)
				: []

			if (user) messages.push({ role: 'user', content: user })
			if (assistant) messages.push({ role: 'assistant', content: assistant })

			// Keep last 20 messages
			const trimmed = messages.slice(-20)

			await this.env.DB.prepare(
				`INSERT INTO conversations (chat_id, device_id, messages, last_message, updated_at)
				 VALUES (?, ?, ?, ?, datetime('now'))
				 ON CONFLICT(chat_id) DO UPDATE SET
				   messages = excluded.messages,
				   last_message = excluded.last_message,
				   updated_at = excluded.updated_at`
			)
				.bind(this.chatId, this.deviceId, JSON.stringify(trimmed), assistant || null)
				.run()

			// Log the exchange
			await this.env.DB.prepare(
				`INSERT INTO message_log (device_id, chat_id, user_text, assistant_text)
				 VALUES (?, ?, ?, ?)`
			)
				.bind(this.deviceId, this.chatId, user || null, assistant || null)
				.run()

			console.log(`[DB] Saved exchange: chat=${this.chatId} (${trimmed.length} messages)`)
		} catch (err) {
			console.error('[DB] Failed to save exchange:', err)
		}
	}

	private async saveConversation() {
		// Final save on disconnect — commit any pending text
		await this.commitExchange()
	}

	private sendTrailingSilence() {
		if (!this.geminiWs) return
		// 1s of silence at 16kHz 16-bit mono = 32000 bytes
		const silence = new ArrayBuffer(32000)
		const base64 = arrayBufferToBase64(silence)
		this.geminiWs.send(
			JSON.stringify({
				realtimeInput: {
					audio: { data: base64, mimeType: 'audio/pcm;rate=16000' },
				},
			})
		)
		console.log('[Bridge] Sent 1s trailing silence')
	}

	private cleanup() {
		// Orphaned device tool calls (device disconnected before responding)
		for (const [id, pending] of this.pendingDeviceCalls) {
			this.logToolCall({
				name: pending.name,
				args: pending.args,
				handledBy: 'device',
				status: 'error',
				error: 'device disconnected before responding',
				durationMs: Date.now() - pending.startMs,
			}).catch(() => {})
		}
		this.pendingDeviceCalls.clear()

		if (this.geminiWs) {
			try {
				this.geminiWs.close()
			} catch {
				// ignore
			}
			this.geminiWs = null
			this.geminiReady = false
		}
		if (this.deviceWs) {
			try {
				this.deviceWs.close()
			} catch {
				// ignore
			}
			this.deviceWs = null
		}
		this.audioChunkCount = 0
	}
}

// ─── Utilities ───

function arrayBufferToBase64(buffer: ArrayBuffer): string {
	const bytes = new Uint8Array(buffer)
	const CHUNK = 8192
	let binary = ''
	for (let i = 0; i < bytes.length; i += CHUNK) {
		const slice = bytes.subarray(i, i + CHUNK)
		binary += String.fromCharCode(...slice)
	}
	return btoa(binary)
}

async function fetchWebPage(url: string, maxChars = 4000): Promise<{
	url: string
	status: number
	content_type: string
	title: string | null
	content: string
	truncated: boolean
}> {
	const normalizedMaxChars = Math.max(500, Math.min(maxChars || 4000, 10000))

	try {
		if (!/^https?:\/\//i.test(url)) {
			return {
				url,
				status: 0,
				content_type: 'error',
				title: null,
				content: 'Error: URL must start with http:// or https://',
				truncated: false,
			}
		}
		const controller = new AbortController()
		const timeout = setTimeout(() => controller.abort(), 10000)
		const resp = await fetch(url, {
			headers: {
				'User-Agent': 'm5-live-assistant/1.0',
				Accept: 'text/html,application/json,text/plain,*/*',
			},
			signal: controller.signal,
		})
		clearTimeout(timeout)
		const contentType = resp.headers.get('content-type') || ''
		const title = extractHtmlTitle(await resp.clone().text(), contentType)
		let body = await resp.text()

		if (contentType.includes('html')) {
			// Strip scripts/styles and tags; collapse whitespace
			body = body
				.replace(/<script[\s\S]*?<\/script>/gi, ' ')
				.replace(/<style[\s\S]*?<\/style>/gi, ' ')
				.replace(/<noscript[\s\S]*?<\/noscript>/gi, ' ')
				.replace(/<[^>]+>/g, ' ')
				.replace(/&nbsp;/g, ' ')
				.replace(/&amp;/g, '&')
				.replace(/&lt;/g, '<')
				.replace(/&gt;/g, '>')
				.replace(/&quot;/g, '"')
				.replace(/&#39;/g, "'")
				.replace(/\s+/g, ' ')
				.trim()
		} else {
			body = body.replace(/\s+/g, ' ').trim()
		}

		const truncated = body.length > normalizedMaxChars
		if (truncated) {
			body = body.slice(0, normalizedMaxChars) + '... [truncated]'
		}

		if (!resp.ok) {
			body = body || `Error: HTTP ${resp.status} ${resp.statusText}`
		}

		return {
			url: resp.url,
			status: resp.status,
			content_type: contentType || 'unknown',
			title,
			content: body || '(empty response)',
			truncated,
		}
	} catch (err) {
		return {
			url,
			status: 0,
			content_type: 'error',
			title: null,
			content: `Error fetching URL: ${err instanceof Error ? err.message : String(err)}`,
			truncated: false,
		}
	}
}

function extractHtmlTitle(body: string, contentType: string): string | null {
	if (!contentType.includes('html')) return null

	const match = body.match(/<title[^>]*>([\s\S]*?)<\/title>/i)
	if (!match) return null

	return match[1].replace(/\s+/g, ' ').trim() || null
}

function base64ToArrayBuffer(base64: string): ArrayBuffer {
	const binary = atob(base64)
	const bytes = new Uint8Array(binary.length)
	for (let i = 0; i < binary.length; i++) {
		bytes[i] = binary.charCodeAt(i)
	}
	return bytes.buffer
}
