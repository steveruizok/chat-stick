/**
 * Indexing worker — run via:
 *   wrangler dev scripts/index-docs-worker.ts --port 8799
 *   curl http://localhost:8799/index
 *
 * This runs inside the Workers runtime so it has access to AI and Vectorize bindings.
 */
import docsIndex from '../src/docs-index.json'

interface Env {
	AI: Ai
	VECTORIZE: VectorizeIndex
}

interface DocEntry {
	id: string
	title: string
	section: string
	keywords: string[]
	content: string
}

const BATCH_SIZE = 20

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		const url = new URL(request.url)

		if (url.pathname === '/index') {
			return indexDocs(env)
		}

		if (url.pathname === '/search') {
			const q = url.searchParams.get('q') || 'what is tldraw'
			return searchDocs(q, env)
		}

		return new Response('GET /index to index docs, GET /search?q=query to test search')
	},
}

async function indexDocs(env: Env): Promise<Response> {
	const docs = docsIndex as DocEntry[]
	const log: string[] = [`Indexing ${docs.length} docs...`]

	let total = 0
	for (let i = 0; i < docs.length; i += BATCH_SIZE) {
		const batch = docs.slice(i, i + BATCH_SIZE)

		const texts = batch.map(
			(d) => `${d.title}\n${d.keywords.join(', ')}\n${d.content.slice(0, 2000)}`
		)

		log.push(`Embedding batch ${Math.floor(i / BATCH_SIZE) + 1}...`)
		const { data: embeddings } = await env.AI.run('@cf/baai/bge-base-en-v1.5', {
			text: texts,
		})

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

async function searchDocs(query: string, env: Env): Promise<Response> {
	const { data: queryEmbedding } = await env.AI.run('@cf/baai/bge-base-en-v1.5', {
		text: [query],
	})

	const results = await env.VECTORIZE.query(queryEmbedding[0], {
		topK: 5,
		returnMetadata: 'all',
	})

	// Look up full content for matches
	const docs = docsIndex as DocEntry[]
	const enriched = results.matches.map((m) => {
		const doc = docs.find((d) => d.id === m.metadata?.docId)
		return {
			score: m.score,
			title: m.metadata?.title,
			section: m.metadata?.section,
			content: doc?.content.slice(0, 300) || '(not found)',
		}
	})

	return new Response(JSON.stringify({ query, results: enriched }, null, 2), {
		headers: { 'Content-Type': 'application/json' },
	})
}
