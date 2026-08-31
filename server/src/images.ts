// Device-scoped image history in D1.
// Every query is filtered by device_id so a device only ever sees its own images.

export interface ImageSummary {
	id: number
	chat_id: string | null
	prompt: string
	dithered_key: string | null
	original_key: string | null
	width: number
	height: number
	created_at: string
	// Set on frames of a flipbook animation (see show_animation): frames of one
	// animation share animation_group and are ordered by frame_index (0-based).
	animation_group: string | null
	frame_index: number | null
	// Total frames in this row's animation group (1 for plain images). Computed
	// at query time, not stored.
	frame_count: number
}

export interface ImageRecord extends ImageSummary {
	enhanced_prompt: string | null
	packed_bits: string
}

export interface ImageSearchHit extends ImageSummary {
	enhanced_prompt: string | null
	snippet: string
	match_count: number
}

const SUMMARY_COLUMNS =
	'id, chat_id, prompt, dithered_key, original_key, width, height, created_at, animation_group, frame_index'

// Frames in the same animation group, counted per row (1 for plain images).
const FRAME_COUNT_COLUMN = `CASE WHEN images.animation_group IS NULL THEN 1
     ELSE (SELECT COUNT(*) FROM images g
           WHERE g.device_id = images.device_id
             AND g.animation_group = images.animation_group)
     END AS frame_count`

// Listing/search should show one entry per animation, anchored on frame 0.
const FIRST_FRAME_ONLY = '(frame_index IS NULL OR frame_index = 0)'

export async function recordImage(
	db: D1Database,
	params: {
		deviceId: string
		chatId: string | null
		prompt: string
		enhancedPrompt: string | null
		ditheredKey: string | null
		originalKey: string | null
		packedBits: string
		width: number
		height: number
		animationGroup?: string | null
		frameIndex?: number | null
	}
): Promise<number> {
	const result = await db
		.prepare(
			`INSERT INTO images
        (device_id, chat_id, prompt, enhanced_prompt, dithered_key, original_key, packed_bits, width, height, animation_group, frame_index, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'))`
		)
		.bind(
			params.deviceId,
			params.chatId,
			params.prompt,
			params.enhancedPrompt,
			params.ditheredKey,
			params.originalKey,
			params.packedBits,
			params.width,
			params.height,
			params.animationGroup ?? null,
			params.frameIndex ?? null
		)
		.run()
	const id = (result.meta as { last_row_id?: number } | undefined)?.last_row_id
	return typeof id === 'number' ? id : 0
}

export async function listRecentImages(
	db: D1Database,
	deviceId: string,
	limit = 10
): Promise<ImageSummary[]> {
	const capped = Math.max(1, Math.min(50, Math.floor(limit)))
	const rows = await db
		.prepare(
			`SELECT ${SUMMARY_COLUMNS}, ${FRAME_COUNT_COLUMN} FROM images
       WHERE device_id = ? AND ${FIRST_FRAME_ONLY}
       ORDER BY created_at DESC, id DESC
       LIMIT ?`
		)
		.bind(deviceId, capped)
		.all<ImageSummary>()
	return rows.results ?? []
}

// Substring search across prompt + enhanced_prompt for a device. Case-insensitive.
// Returns up to `limit` images mentioning `query`, with a short snippet.
export async function searchImages(
	db: D1Database,
	deviceId: string,
	query: string,
	limit = 10
): Promise<ImageSearchHit[]> {
	const q = query.trim()
	if (!q) return []

	const capped = Math.max(1, Math.min(50, Math.floor(limit)))
	const like = `%${q.replace(/[\\%_]/g, (c) => '\\' + c)}%`
	const rows = await db
		.prepare(
			`SELECT ${SUMMARY_COLUMNS}, ${FRAME_COUNT_COLUMN}, enhanced_prompt FROM images
       WHERE device_id = ? AND ${FIRST_FRAME_ONLY}
         AND (LOWER(prompt) LIKE LOWER(?) ESCAPE '\\'
              OR LOWER(IFNULL(enhanced_prompt, '')) LIKE LOWER(?) ESCAPE '\\')
       ORDER BY created_at DESC, id DESC
       LIMIT ?`
		)
		.bind(deviceId, like, like, capped)
		.all<ImageSummary & { enhanced_prompt: string | null }>()

	const needle = q.toLowerCase()
	const SNIPPET_PAD = 60

	return (rows.results ?? []).map((row) => {
		const haystack = `${row.prompt}\n${row.enhanced_prompt ?? ''}`
		const lower = haystack.toLowerCase()
		const idx = lower.indexOf(needle)
		const start = idx < 0 ? 0 : Math.max(0, idx - SNIPPET_PAD)
		const end =
			idx < 0
				? Math.min(haystack.length, SNIPPET_PAD * 2)
				: Math.min(haystack.length, idx + needle.length + SNIPPET_PAD)
		const prefix = start > 0 ? '…' : ''
		const suffix = end < haystack.length ? '…' : ''
		const snippet = prefix + haystack.slice(start, end) + suffix

		let count = 0
		let from = 0
		while (true) {
			const next = lower.indexOf(needle, from)
			if (next === -1) break
			count++
			from = next + needle.length
		}

		return { ...row, snippet, match_count: count }
	})
}

export async function getImageById(
	db: D1Database,
	deviceId: string,
	id: number
): Promise<ImageRecord | null> {
	const row = await db
		.prepare(
			`SELECT ${SUMMARY_COLUMNS}, ${FRAME_COUNT_COLUMN}, enhanced_prompt, packed_bits FROM images
       WHERE device_id = ? AND id = ?
       LIMIT 1`
		)
		.bind(deviceId, id)
		.first<ImageRecord>()
	return row ?? null
}

// All frames of one animation, in playback order. Includes packed_bits so the
// caller can re-send every frame to the device without touching R2.
export async function getAnimationFrames(
	db: D1Database,
	deviceId: string,
	animationGroup: string
): Promise<ImageRecord[]> {
	const rows = await db
		.prepare(
			`SELECT ${SUMMARY_COLUMNS}, ${FRAME_COUNT_COLUMN}, enhanced_prompt, packed_bits FROM images
       WHERE device_id = ? AND animation_group = ?
       ORDER BY frame_index ASC, id ASC`
		)
		.bind(deviceId, animationGroup)
		.all<ImageRecord>()
	return rows.results ?? []
}
