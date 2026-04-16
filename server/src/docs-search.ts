import docsIndex from './docs-index.json'

interface DocEntry {
	id: string
	title: string
	section: string
	keywords: string[]
	content: string
}

interface SearchEnv {
	AI: Ai
	VECTORIZE: VectorizeIndex
}

const EMBEDDING_MODEL = '@cf/baai/bge-base-en-v1.5'
const BATCH_SIZE = 20

export function searchDocsKeyword(
	query: string,
	limit = 5
): { title: string; section: string; content: string; score: number }[] {
	const terms = query.toLowerCase().split(/\s+/).filter(Boolean)
	const scored: { entry: DocEntry; score: number }[] = []

	for (const entry of docsIndex as DocEntry[]) {
		let score = 0
		const title = entry.title.toLowerCase()
		const haystack = `${title} ${entry.keywords.join(' ')} ${entry.content}`.toLowerCase()

		for (const term of terms) {
			if (title === term) score += 20
			if (title.includes(term)) score += 10
			if (entry.keywords.some((keyword) => keyword.toLowerCase().includes(term))) score += 5
			const matches = haystack.split(term).length - 1
			if (matches > 0) score += Math.min(matches, 3)
		}

		if (score > 0) score -= entry.content.length / 10000
		if (score > 0) scored.push({ entry, score })
	}

	scored.sort((a, b) => b.score - a.score)
	return scored.slice(0, limit).map(({ entry, score }) => ({
		title: entry.title,
		section: entry.section,
		content: entry.content,
		score,
	}))
}

// ── Index all docs into Vectorize ──

export async function indexDocs(env: SearchEnv): Promise<Response> {
	const docs = docsIndex as DocEntry[]
	const log: string[] = [`Indexing ${docs.length} docs...`]

	let total = 0
	for (let i = 0; i < docs.length; i += BATCH_SIZE) {
		const batch = docs.slice(i, i + BATCH_SIZE)

		const texts = batch.map(
			(d) => `${d.title}\n${d.keywords.join(', ')}\n${d.content.slice(0, 2000)}`
		)

		log.push(`Embedding batch ${Math.floor(i / BATCH_SIZE) + 1}/${Math.ceil(docs.length / BATCH_SIZE)}...`)

		const result = await env.AI.run(EMBEDDING_MODEL, { text: texts })
		const embeddings = (result as any).data as number[][]

		const vectors = batch.map((d, j) => ({
			id: d.id.replace(/[^a-zA-Z0-9_-]/g, '_'),
			values: embeddings[j],
			metadata: {
				title: d.title,
				section: d.section,
				docId: d.id,
			},
		}))

		await env.VECTORIZE.upsert(vectors)
		total += vectors.length
		log.push(`  Upserted ${vectors.length} (total: ${total})`)
	}

	log.push(`\nDone! Indexed ${total} docs.`)
	return new Response(log.join('\n'))
}

// ── Vector search (used by admin endpoint and live session) ──

export async function vectorSearch(query: string, env: SearchEnv): Promise<Response> {
	const results = await searchDocsVector(query, env)
	return new Response(JSON.stringify({ query, results }, null, 2), {
		headers: { 'Content-Type': 'application/json' },
	})
}

export async function searchDocsVector(
	query: string,
	env: SearchEnv,
	topK = 5
): Promise<{ title: string; section: string; content: string; score: number }[]> {
	const queryResult = await env.AI.run(EMBEDDING_MODEL, { text: [query] })
	const queryEmbedding = (queryResult as any).data as number[][]

	const results = await env.VECTORIZE.query(queryEmbedding[0], {
		topK,
		returnMetadata: 'all',
	})

	const docs = docsIndex as DocEntry[]
	return results.matches.map((m) => {
		const doc = docs.find((d) => d.id === m.metadata?.docId)
		return {
			title: (m.metadata?.title as string) || '',
			section: (m.metadata?.section as string) || '',
			content: doc?.content || '(not found)',
			score: m.score,
		}
	})
}
