/**
 * Test the tldraw docs search function locally.
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
	// Basic concepts
	'What is the Editor class?',
	'How do I create shapes?',
	'What are tools in tldraw?',

	// Features
	'How does collaboration work?',
	'How do I add custom shapes?',
	'How do I handle assets and images?',

	// Specific topics
	'How do I use the store?',
	'What persistence options are available?',
	'How do I customize the user interface?',
	'How do handles work?',

	// Getting started
	'How do I install tldraw?',
	'What starter kits are available?',

	// Edge cases — should still find something relevant
	'How do I zoom to fit?',
	'Can I use tldraw with Next.js?',
	'How do I export shapes as SVG?',
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
