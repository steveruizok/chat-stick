/**
 * Test the docs keyword search function locally.
 * Runs queries and shows what the model would see.
 */
import docsIndex from '../src/docs-index.json'

interface DocEntry {
	id: string
	title: string
	section: string
	keywords: string[]
	content: string
}

function searchDocs(query: string, limit = 5): DocEntry[] {
	const terms = query.toLowerCase().split(/\s+/).filter(Boolean)
	const scored: { entry: DocEntry; score: number }[] = []

	for (const entry of docsIndex as DocEntry[]) {
		let score = 0
		const haystack = `${entry.title} ${entry.keywords.join(' ')} ${entry.content}`.toLowerCase()
		for (const term of terms) {
			if (entry.title.toLowerCase().includes(term)) score += 10
			if (entry.keywords.some((k: string) => k.includes(term))) score += 5
			if (haystack.includes(term)) score += 1
		}
		if (score > 0) scored.push({ entry, score })
	}

	scored.sort((a, b) => b.score - a.score)
	return scored.slice(0, limit).map((s) => s.entry)
}

// ── Test cases ──

const tests = [
	// Add your own test queries here based on your indexed content
	'getting started',
	'how does it work',
	'configuration options',
]

console.log(`Docs index: ${docsIndex.length} entries\n`)

let pass = 0
let fail = 0

for (const query of tests) {
	const results = searchDocs(query)
	const found = results.length > 0
	const icon = found ? '✓' : '✗'

	console.log(`${icon} "${query}"`)
	if (found) {
		for (const r of results.slice(0, 3)) {
			const preview = r.content.slice(0, 120).replace(/\n/g, ' ')
			console.log(`    ${r.section}/${r.title} — ${preview}...`)
		}
		pass++
	} else {
		console.log(`    (no results)`)
		fail++
	}
	console.log()
}

console.log(`\n${pass}/${tests.length} queries returned results, ${fail} empty`)
