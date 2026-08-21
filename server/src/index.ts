import { LiveSession } from './live-session'
import { indexDocs, vectorSearch } from './docs-search'

export { LiveSession }

export interface Env {
	LIVE_SESSION: DurableObjectNamespace
	GEMINI_API_KEY: string
	AI: Ai
	VECTORIZE: VectorizeIndex
	DB: D1Database
	HISTORY_API_TOKEN: string
	ADMIN_API_TOKEN?: string
	DEVICE_AUTH_TOKEN?: string
	STORAGE?: R2Bucket
}

const LEGACY_FIRMWARE_PREFIX = 'chat-stick/firmware/'
// `waveshare` is the original SH8601-panel board; `waveshare-v2` is the
// CO5300-panel revision. They share source but need separate binaries.
const FIRMWARE_DEVICE_IDS = new Set(['m5-stick', 'waveshare', 'waveshare-v2'])

async function findLatestFirmware(
	env: Env,
	device: string
): Promise<{ version: number; key: string } | null> {
	if (!env.STORAGE) return null
	const prefixes =
		device === 'm5-stick'
			? [`chat-stick/firmware/${device}/`, LEGACY_FIRMWARE_PREFIX]
			: [`chat-stick/firmware/${device}/`]
	let latest: { version: number; key: string } | null = null
	for (const prefix of prefixes) {
		const list = await env.STORAGE.list({ prefix })
		for (const obj of list.objects) {
			const objectName = obj.key.slice(prefix.length)
			if (objectName.includes('/')) continue
			const match = objectName.match(/^firmware-v(\d+)\.bin$/)
			if (!match) continue
			const version = Number(match[1])
			if (!latest || version > latest.version) {
				latest = { version, key: obj.key }
			}
		}
	}
	return latest
}

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		const url = new URL(request.url)

		if (request.method === 'OPTIONS') {
			return new Response(null, { headers: corsHeaders() })
		}

		switch (url.pathname) {
			case '/ws': {
				const upgrade = request.headers.get('Upgrade')
				if (upgrade !== 'websocket') {
					return new Response('Expected WebSocket', { status: 426 })
				}
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401 })
				}

				// Route to DO by device_id (one session per device)
				const deviceId = url.searchParams.get('device_id') || 'unknown'
				const id = env.LIVE_SESSION.idFromName(deviceId)
				const stub = env.LIVE_SESSION.get(id)
				// Forward the full URL so DO can read device_id and chat_id
				return stub.fetch(request)
			}

			// Admin: index docs into Vectorize
			case '/admin/index':
				if (!isAuthorizedAdminRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				return indexDocs(env)

			// Admin: test vector search
			case '/admin/search': {
				if (!isAuthorizedAdminRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const q = url.searchParams.get('q') || 'hello'
				return vectorSearch(q, env)
			}

			case '/health':
				return new Response('ok')

			case '/ping':
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				return new Response('pong', {
					headers: {
						...corsHeaders(),
						'Cache-Control': 'no-store',
						'Content-Type': 'text/plain',
					},
				})

			case '/firmware/check': {
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const currentVersion = Number(url.searchParams.get('version') || '0')
				const device = resolveFirmwareDevice(url)
				const latest = await findLatestFirmware(env, device)
				const updateAvailable = !!latest && latest.version > currentVersion
				return json({
					available: updateAvailable,
					latest_version: latest?.version ?? currentVersion,
					notes: '',
					download_url: updateAvailable
						? `${url.origin}/firmware/download?device=${encodeURIComponent(device)}`
						: '',
				})
			}

			case '/firmware/download': {
				const device = resolveFirmwareDevice(url)
				const latest = await findLatestFirmware(env, device)
				if (!env.STORAGE || !latest) {
					return new Response('Firmware download unavailable', { status: 404 })
				}

				const object = await env.STORAGE.get(latest.key)
				if (!object) {
					return new Response('Firmware not found', { status: 404 })
				}

				const headers = new Headers()
				object.writeHttpMetadata(headers)
				headers.set('etag', object.httpEtag)
				headers.set('content-length', object.size.toString())
				headers.set('content-type', headers.get('content-type') || 'application/octet-stream')
				headers.set(
					'content-disposition',
					`attachment; filename="${latest.key.split('/').pop()}"`
				)
				return new Response(object.body, { headers })
			}

			default: {
				// /history/:deviceId — list recent conversations
				const historyMatch = url.pathname.match(/^\/history\/(.+)$/)
				if (historyMatch) {
					const deviceId = decodeURIComponent(historyMatch[1])
					const authorized =
						isAuthorizedHistoryRequest(request, env) ||
						isAuthenticatedDeviceRequest(request, env)
					if (!authorized) {
						return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
					}

					const rows = await env.DB.prepare(
						`SELECT chat_id, last_message, updated_at
						 FROM conversations
						 WHERE device_id = ? AND last_message IS NOT NULL
						 ORDER BY updated_at DESC
						 LIMIT 10`
					)
						.bind(deviceId)
						.all()
					return new Response(JSON.stringify(rows.results), {
						headers: { 'Content-Type': 'application/json' },
					})
				}

				const sessionMatch = url.pathname.match(/^\/session\/(.+)$/)
				if (sessionMatch) {
					const chatId = decodeURIComponent(sessionMatch[1])
					const row = await env.DB.prepare(
						`SELECT chat_id, device_id, last_message, updated_at
						 FROM conversations
						 WHERE chat_id = ?
						 LIMIT 1`
					)
						.bind(chatId)
						.first<{
							chat_id: string
							device_id: string
							last_message: string | null
							updated_at: string
						}>()

					if (!row) {
						return new Response('Not found', { status: 404, headers: corsHeaders() })
					}

					const authorized =
						isAuthorizedHistoryRequest(request, env) ||
						isAuthenticatedDeviceRequest(request, env)
					if (!authorized) {
						return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
					}

					return new Response(
						JSON.stringify({
							chat_id: row.chat_id,
							device_id: row.device_id,
							last_message: row.last_message,
							updated_at: row.updated_at,
						}),
						{
							headers: { ...corsHeaders(), 'Content-Type': 'application/json' },
						}
					)
				}
				return new Response('Not found', { status: 404 })
			}
		}
	},
}

function corsHeaders(): HeadersInit {
	return {
		'Access-Control-Allow-Origin': '*',
		'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
		'Access-Control-Allow-Headers': 'Authorization, Content-Type, X-Admin-Token, X-Device-Token, X-History-Token',
	}
}

function json(payload: unknown, init?: ResponseInit): Response {
	return new Response(JSON.stringify(payload), {
		...init,
		headers: {
			...corsHeaders(),
			'Content-Type': 'application/json',
			...(init?.headers || {}),
		},
	})
}

function resolveFirmwareDevice(url: URL): string {
	const requested = url.searchParams.get('device') || 'm5-stick'
	return FIRMWARE_DEVICE_IDS.has(requested) ? requested : 'm5-stick'
}

function isAuthorizedHistoryRequest(request: Request, env: Env): boolean {
	const configuredToken = env.HISTORY_API_TOKEN?.trim()
	if (!configuredToken) return false

	const providedToken = getRequestToken(request, {
		headerNames: ['X-History-Token'],
		queryNames: ['token'],
	})

	return secureTokenEquals(providedToken, configuredToken)
}

function isAuthorizedAdminRequest(request: Request, env: Env): boolean {
	const configuredToken = (env.ADMIN_API_TOKEN || env.HISTORY_API_TOKEN || '').trim()
	if (!configuredToken) return false

	const providedToken = getRequestToken(request, {
		headerNames: ['X-Admin-Token', 'X-History-Token'],
		queryNames: ['admin_token', 'token'],
	})

	return secureTokenEquals(providedToken, configuredToken)
}

function isAuthorizedDeviceRequest(request: Request, env: Env): boolean {
	const configuredToken = env.DEVICE_AUTH_TOKEN?.trim()
	if (!configuredToken) return true

	const providedToken = getRequestToken(request, {
		headerNames: ['X-Device-Token'],
		queryNames: ['device_token'],
	})

	return secureTokenEquals(providedToken, configuredToken)
}

function isAuthenticatedDeviceRequest(request: Request, env: Env): boolean {
	return Boolean(env.DEVICE_AUTH_TOKEN?.trim()) && isAuthorizedDeviceRequest(request, env)
}

function getRequestToken(
	request: Request,
	options: { headerNames: string[]; queryNames: string[] }
): string {
	for (const headerName of options.headerNames) {
		const value = request.headers.get(headerName)
		if (value) return value.trim()
	}

	const auth = request.headers.get('Authorization') || ''
	const bearer = auth.match(/^Bearer\s+(.+)$/i)?.[1]
	if (bearer) return bearer.trim()

	const url = new URL(request.url)
	for (const queryName of options.queryNames) {
		const value = url.searchParams.get(queryName)
		if (value) return value.trim()
	}

	return ''
}

function secureTokenEquals(providedToken: string, configuredToken: string): boolean {
	if (!providedToken || providedToken.length !== configuredToken.length) return false

	let diff = 0
	for (let i = 0; i < configuredToken.length; i++) {
		diff |= configuredToken.charCodeAt(i) ^ providedToken.charCodeAt(i)
	}
	return diff === 0
}
