/**
 * Build a searchable docs index from the tldraw docs content.
 *
 * Full content for hand-written docs (docs/, sdk-features/, getting-started/, starter-kits/).
 * Title + category + first paragraph for reference docs (too large to embed fully).
 *
 * Output: src/docs-index.json
 */
import { readdir, readFile, writeFile } from 'fs/promises'
import { join, basename } from 'path'

const TLDRAW_DOCS = process.env.TLDRAW_DOCS
	|| '/Users/stephenruiz/Documents/GitHub/tldraw/apps/docs/content'

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
		.replace(/\[([^\]]*)\]\(\?\)/g, '$1') // tldraw doc links [Name](?)
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

	// Full content sections
	for (const section of ['docs', 'sdk-features', 'getting-started', 'starter-kits', 'community']) {
		const entries = await readSection(join(TLDRAW_DOCS, section), section, true)
		all.push(...entries)
		console.log(`${section}: ${entries.length} entries`)
	}

	// Skip reference/ — too large and not useful for voice queries

	const json = JSON.stringify(all)
	const outPath = join(import.meta.dirname, '..', 'src', 'docs-index.json')
	await writeFile(outPath, json)
	console.log(`\nWrote ${all.length} entries (${(json.length / 1024).toFixed(0)}KB) → ${outPath}`)
}

main()
