const MAX_WEB_FETCH_BYTES = 200_000

export async function fetchWebPage(
	url: string,
	maxChars = 4000,
	timeoutMs = 10000,
): Promise<{
	url: string
	status: number
	content_type: string
	title: string | null
	content: string
	truncated: boolean
}> {
	const normalizedMaxChars = Math.max(500, Math.min(maxChars || 4000, 10000))
	const validated = validateWebFetchUrl(url)

	const controller = new AbortController()
	const timeout = setTimeout(() => controller.abort(), timeoutMs)
	try {
		if (!validated.ok) {
			return {
				url,
				status: 0,
				content_type: 'error',
				title: null,
				content: `Error: ${validated.error}`,
				truncated: false,
			}
		}

		let target = validated.url
		let resp: Response
		for (let redirects = 0; ; redirects++) {
			resp = await abortable(fetch(target, {
				redirect: 'manual',
				headers: { 'User-Agent': 'm5-live-assistant/1.0', Accept: 'text/html,application/json,text/plain,*/*' },
				signal: controller.signal,
			}), controller.signal)
			if (![301, 302, 303, 307, 308].includes(resp.status)) break
			void resp.body?.cancel().catch(() => {})
			if (redirects >= 5) throw new Error('too many redirects')
			const location = resp.headers.get('location')
			if (!location) throw new Error('redirect missing Location')
			const next = validateWebFetchUrl(new URL(location, target).href)
			if (!next.ok) throw new Error(next.error)
			target = next.url
		}
		const contentLength = Number(resp.headers.get('content-length') || '0')
		if (contentLength > MAX_WEB_FETCH_BYTES) {
			void resp.body?.cancel().catch(() => {})
			throw new Error(`response too large (${contentLength} bytes, max ${MAX_WEB_FETCH_BYTES})`)
		}

		const contentType = resp.headers.get('content-type') || ''
		const read = await readResponseTextWithLimit(resp, MAX_WEB_FETCH_BYTES, controller.signal)
		let body = read.text
		const title = extractHtmlTitle(body, contentType)

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

		const truncated = read.truncated || body.length > normalizedMaxChars
		if (body.length > normalizedMaxChars) {
			body = body.slice(0, normalizedMaxChars) + '... [truncated]'
		}

		if (!resp.ok) {
			body = body || `Error: HTTP ${resp.status} ${resp.statusText}`
		}

		return {
			url: resp.url || target,
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
	} finally {
		clearTimeout(timeout)
		controller.abort()
	}

}

function validateWebFetchUrl(
	rawUrl: string,
): { ok: true; url: string } | { ok: false; error: string } {
	let parsed: URL
	try {
		parsed = new URL(rawUrl)
	} catch {
		return { ok: false, error: 'URL is invalid' }
	}

	if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
		return { ok: false, error: 'URL must start with http:// or https://' }
	}

	if (parsed.username || parsed.password) return { ok: false, error: 'URL credentials are not allowed' }
	const rawHostname = parsed.hostname.toLowerCase().replace(/\.$/, '')
	const hostname =
		rawHostname.startsWith('[') && rawHostname.endsWith(']')
			? rawHostname.slice(1, -1)
			: rawHostname
	const isIpv6Literal = hostname.includes(':')
	if (
		hostname === 'localhost' ||
		hostname.endsWith('.localhost') ||
		hostname.endsWith('.local') ||
		hostname.endsWith('.internal') ||
		(isIpv6Literal &&
			(hostname === '::' || hostname === '::1' || hostname.startsWith('::ffff:') ||
				hostname.startsWith('fe80:') ||
				hostname.startsWith('fc') ||
				hostname.startsWith('fd')))
	) {
		return { ok: false, error: 'local or private network URLs are not allowed' }
	}

	const ipv4 = hostname.match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/)
	if (ipv4) {
		const octets = ipv4.slice(1).map(Number)
		if (octets.some((n) => n < 0 || n > 255)) {
			return { ok: false, error: 'IP address is invalid' }
		}

		const [a, b] = octets
		const privateOrReserved =
			a === 0 ||
			a === 10 ||
			a === 127 ||
			(a === 100 && b >= 64 && b <= 127) ||
			(a === 169 && b === 254) ||
			(a === 172 && b >= 16 && b <= 31) ||
			(a === 192 && b === 168) ||
			a >= 224
		if (privateOrReserved) {
			return { ok: false, error: 'local or private network URLs are not allowed' }
		}
	}

	return { ok: true, url: parsed.toString() }
}

async function readResponseTextWithLimit(
	resp: Response,
	maxBytes: number,
	signal: AbortSignal,
): Promise<{ text: string; truncated: boolean }> {
	if (!resp.body) {
		return { text: await resp.text(), truncated: false }
	}

	const reader = resp.body.getReader()
	const decoder = new TextDecoder()
	let total = 0
	let text = ''
	let truncated = false

	try {
		while (true) {
			const { done, value } = await abortable(reader.read(), signal)
			if (done) break
			if (!value) continue

			total += value.byteLength
			if (total > maxBytes) {
				const allowed = Math.max(0, value.byteLength - (total - maxBytes))
				if (allowed > 0) {
					text += decoder.decode(value.slice(0, allowed), { stream: true })
				}
				truncated = true
				try {
					void reader.cancel().catch(() => {})
				} catch {
					// ignore
				}
				break
			}

			text += decoder.decode(value, { stream: true })
		}
	} finally {
		void reader.cancel().catch(() => {})
		reader.releaseLock()
	}

	text += decoder.decode()
	return { text, truncated }
}

function extractHtmlTitle(body: string, contentType: string): string | null {
	if (!contentType.includes('html')) return null

	const match = body.match(/<title[^>]*>([\s\S]*?)<\/title>/i)
	if (!match) return null

	return match[1].replace(/\s+/g, ' ').trim() || null
}


// Race the body read too: a peer may send headers and then stop indefinitely.
function abortable<T>(promise: Promise<T>, signal: AbortSignal): Promise<T> {
	return new Promise((resolve, reject) => {
		const abort = () => reject(new Error('request timed out'))
		if (signal.aborted) { abort(); return }
		signal.addEventListener('abort', abort, { once: true })
		promise.then(resolve, reject).finally(() => signal.removeEventListener('abort', abort))
	})
}
