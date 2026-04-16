/**
 * Build a searchable docs index from a directory of .mdx content files.
 *
 * Set DOCS_PATH to point at your content directory. Each subdirectory
 * becomes a section. Outputs src/docs-index.json for keyword search
 * and Vectorize indexing.
 *
 * Usage: DOCS_PATH=/path/to/content npx tsx scripts/build-docs-index.ts
 */
import { readdir, readFile, writeFile } from 'fs/promises'
import { join, basename } from 'path'

const DOCS_PATH = process.env.DOCS_PATH
if (!DOCS_PATH) {
	console.error('Set DOCS_PATH to your content directory')
	process.exit(1)
}

interface DocEntry {
	id: string
	title: string
	section: string
	keywords: string[]
	content: string // full content for docs, summary for reference
}

function parseFrontmatter(raw: string): { meta: Record<string, any>; body: string } {
	const match = raw.match(/^---\n([\s\S]*?)\n---\n([\s\S]*)$/)
	if (!match) return { meta: {}, body: raw }

	const meta: Record<string, any> = {}
	for (const line of match[1].split('\n')) {
		const m = line.match(/^(\w+):\s*(.*)$/)
		if (m) meta[m[1]] = m[2].trim()
	}

	// Parse keywords array
	if (raw.includes('keywords:')) {
		const kwMatch = raw.match(/keywords:\n((?:\s+-\s+.*\n?)*)/)
		if (kwMatch) {
			meta.keywords = kwMatch[1]
				.split('\n')
				.map(l => l.replace(/^\s+-\s+/, '').trim())
				.filter(Boolean)
		}
	}

	return { meta, body: match[2] }
}

function stripMdx(body: string): string {
	return body
		.replace(/<[^>]+>/g, '') // strip JSX/HTML tags
		.replace(/```[\s\S]*?```/g, '[code block]') // collapse code blocks
		.replace(/\[([^\]]*)\]\(\?\)/g, '$1') // shorthand doc links [Name](?)
		.replace(/\[([^\]]*)\]\([^)]*\)/g, '$1') // markdown links
		.replace(/#{1,6}\s*/g, '') // heading markers
		.replace(/\*\*([^*]+)\*\*/g, '$1') // bold
		.replace(/\*([^*]+)\*/g, '$1') // italic
		.replace(/\n{3,}/g, '\n\n') // collapse multiple newlines
		.trim()
}

async function readSection(dir: string, section: string, fullContent: boolean): Promise<DocEntry[]> {
	const entries: DocEntry[] = []
	let files: string[]
	try {
		files = await readdir(dir)
	} catch {
		return entries
	}

	for (const file of files) {
		if (!file.endsWith('.mdx')) continue
		const raw = await readFile(join(dir, file), 'utf-8')
		const { meta, body } = parseFrontmatter(raw)
		if (meta.status && meta.status !== 'published') continue

		const stripped = stripMdx(body)
		const content = fullContent ? stripped : stripped.slice(0, 300)

		entries.push({
			id: `${section}/${basename(file, '.mdx')}`,
			title: meta.title || basename(file, '.mdx'),
			section,
			keywords: meta.keywords || [],
			content,
		})
	}

	return entries
}

async function main() {
	const all: DocEntry[] = []

	// Read each subdirectory as a section
	const sections = await readdir(DOCS_PATH, { withFileTypes: true })
	for (const entry of sections) {
		if (!entry.isDirectory()) continue
		const entries = await readSection(join(DOCS_PATH, entry.name), entry.name, true)
		all.push(...entries)
		console.log(`${entry.name}: ${entries.length} entries`)
	}

	const json = JSON.stringify(all)
	const outPath = join(import.meta.dirname, '..', 'src', 'docs-index.json')
	await writeFile(outPath, json)
	console.log(`\nWrote ${all.length} entries (${(json.length / 1024).toFixed(0)}KB) → ${outPath}`)
}

main()
