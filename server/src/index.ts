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

				// Route to DO by device_id (one session per device)
				const deviceId = url.searchParams.get('device_id') || 'unknown'
				const id = env.LIVE_SESSION.idFromName(deviceId)
				const stub = env.LIVE_SESSION.get(id)
				// Forward the full URL so DO can read device_id and chat_id
				return stub.fetch(request)
			}

			// Admin: index docs into Vectorize
			case '/admin/index':
				return indexDocs(env)

			// Admin: test vector search
			case '/admin/search': {
				const q = url.searchParams.get('q') || 'hello'
				return vectorSearch(q, env)
			}

			case '/health':
				return new Response('ok')

			default: {
				// /history/:deviceId — list recent conversations
				const historyMatch = url.pathname.match(/^\/history\/(.+)$/)
				if (historyMatch) {
					if (!isAuthorizedHistoryRequest(request, env)) {
						return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
					}

					const deviceId = decodeURIComponent(historyMatch[1])
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
					const requestedDeviceId = url.searchParams.get('device_id') ?? ''
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
						(!!requestedDeviceId && requestedDeviceId === row.device_id)
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
		'Access-Control-Allow-Headers': 'Content-Type, X-History-Token',
	}
}

function isAuthorizedHistoryRequest(request: Request, env: Env): boolean {
	const configuredToken = env.HISTORY_API_TOKEN?.trim()
	if (!configuredToken) return false

	const url = new URL(request.url)
	const providedToken =
		request.headers.get('X-History-Token') ??
		url.searchParams.get('token') ??
		''

	return providedToken === configuredToken
}
