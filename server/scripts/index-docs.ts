/**
 * Embed and index tldraw docs into Vectorize.
 *
 * Uses the Cloudflare API directly (not Workers AI binding)
 * so it can run as a standalone script.
 *
 * Usage: CLOUDFLARE_API_TOKEN=xxx npx tsx scripts/index-docs.ts
 */
import { readFile } from 'fs/promises'
import { join } from 'path'

// Load the docs index built by build-docs-index.ts
import docsIndex from '../src/docs-index.json'

const ACCOUNT_ID = process.env.CLOUDFLARE_ACCOUNT_ID || ''
const API_TOKEN = process.env.CLOUDFLARE_API_TOKEN || ''
const INDEX_NAME = 'tldraw-docs'
const EMBEDDING_MODEL = '@cf/baai/bge-base-en-v1.5'
const BATCH_SIZE = 20 // Vectorize upsert batch limit

interface DocEntry {
	id: string
	title: string
	section: string
	keywords: string[]
	content: string
}

async function embed(texts: string[]): Promise<number[][]> {
	const resp = await fetch(
		`https://api.cloudflare.com/client/v4/accounts/${ACCOUNT_ID}/ai/run/${EMBEDDING_MODEL}`,
		{
			method: 'POST',
			headers: {
				Authorization: `Bearer ${API_TOKEN}`,
				'Content-Type': 'application/json',
			},
			body: JSON.stringify({ text: texts }),
		}
	)

	if (!resp.ok) {
		const body = await resp.text()
		throw new Error(`Embedding failed: ${resp.status} ${body}`)
	}

	const json = (await resp.json()) as { result: { data: number[][] } }
	return json.result.data
}

async function upsertVectors(
	vectors: { id: string; values: number[]; metadata: Record<string, string> }[]
) {
	const ndjson = vectors.map((v) => JSON.stringify(v)).join('\n')

	const resp = await fetch(
		`https://api.cloudflare.com/client/v4/accounts/${ACCOUNT_ID}/vectorize/v2/indexes/${INDEX_NAME}/upsert`,
		{
			method: 'POST',
			headers: {
				Authorization: `Bearer ${API_TOKEN}`,
				'Content-Type': 'application/x-ndjson',
			},
			body: ndjson,
		}
	)

	if (!resp.ok) {
		const body = await resp.text()
		throw new Error(`Upsert failed: ${resp.status} ${body}`)
	}

	return (await resp.json()) as { result: { count: number } }
}

async function main() {
	if (!ACCOUNT_ID || !API_TOKEN) {
		console.error('Set CLOUDFLARE_ACCOUNT_ID and CLOUDFLARE_API_TOKEN')
		process.exit(1)
	}

	const docs = docsIndex as DocEntry[]
	console.log(`Indexing ${docs.length} docs...`)

	// Process in batches
	let total = 0
	for (let i = 0; i < docs.length; i += BATCH_SIZE) {
		const batch = docs.slice(i, i + BATCH_SIZE)

		// Prepare text for embedding: title + keywords + content (truncated)
		const texts = batch.map(
			(d) =>
				`${d.title}\n${d.keywords.join(', ')}\n${d.content.slice(0, 2000)}`
		)

		console.log(
			`Embedding batch ${Math.floor(i / BATCH_SIZE) + 1}/${Math.ceil(docs.length / BATCH_SIZE)} (${batch.length} docs)...`
		)
		const embeddings = await embed(texts)

		const vectors = batch.map((d, j) => ({
			id: d.id.replace(/[^a-zA-Z0-9_-]/g, '_'),
			values: embeddings[j],
			metadata: {
				title: d.title,
				section: d.section,
				id: d.id,
			},
		}))

		const result = await upsertVectors(vectors)
		total += vectors.length
		console.log(`  Upserted ${vectors.length} vectors (total: ${total})`)
	}

	console.log(`\nDone! Indexed ${total} docs into Vectorize.`)
}

main().catch(console.error)
