/**
 * Image generation and processing for the on-device display.
 *
 * Generates images via Google's Gemini image model (the Imagen predict API
 * was retired), then processes them to a 1-bit dithered bitmap sized to
 * whatever the connected device requested (see IMAGE_TARGET_* in each
 * device's Config.h). The M5StickS3 still asks for the 232x112 chat-area box
 * from designs.md; the Waveshare asks for its full 368x448 panel.
 */

import UPNG from 'upng-js'
import jpeg from 'jpeg-js'

// Fallback target dimensions for devices that don't declare a size in the
// connect URL. Matches the M5StickS3 chat-area bounding box (designs.md).
export const DEFAULT_IMAGE_WIDTH = 232
export const DEFAULT_IMAGE_HEIGHT = 112

// Hard caps so a misbehaving client can't ask the Worker to allocate tens of
// megabytes of pixels. Generous enough for any plausible display.
const MAX_IMAGE_WIDTH = 1024
const MAX_IMAGE_HEIGHT = 1024

// Generation calls take ~5–15s; cap at 30s.
const IMAGE_GEN_TIMEOUT_MS = 30000

// Current Gemini image-generation model (successor to imagen-4.0-fast, which
// now 404s). Uses generateContent with IMAGE response modality. The lite tier
// is plenty for a 1-bit dithered device display.
const IMAGE_MODEL = 'gemini-3.1-flash-lite-image'

export interface ReferenceImage {
	data: Uint8Array // encoded image bytes (PNG or JPEG)
	mimeType: string
}

// Set when generating one frame of a flipbook animation (show_animation).
// index is 0-based; frames after the first are edits of the previous frame.
export interface FrameContext {
	index: number
	total: number
}

export interface ImageResult {
	data: string // base64 1-bit packed pixels for device
	width: number
	height: number
	ditheredPng: ArrayBuffer // dithered PNG for storage (matches what device shows)
	originalImage: ArrayBuffer // full-color model output (pre-dither) for archival
	originalMimeType: string // mime type of originalImage (model returns JPEG today)
	enhancedPrompt: string // the prompt actually sent to the model (with style suffix)
}

/**
 * Generate an image from a prompt and process it for the device display.
 * Returns null on any failure; the caller decides how to surface that.
 */
export async function generateAndProcessImage(
	prompt: string,
	apiKey: string,
	targetWidth: number = DEFAULT_IMAGE_WIDTH,
	targetHeight: number = DEFAULT_IMAGE_HEIGHT,
	referenceImage?: ReferenceImage,
	frameContext?: FrameContext
): Promise<ImageResult | null> {
	const width = clampDimension(targetWidth, DEFAULT_IMAGE_WIDTH)
	const height = clampDimension(targetHeight, DEFAULT_IMAGE_HEIGHT)
	try {
		console.log(
			`[ImageGen] Generating ${width}x${height} image for: "${prompt}"` +
				(referenceImage ? ' (with reference image)' : '') +
				(frameContext ? ` (frame ${frameContext.index + 1}/${frameContext.total})` : '')
		)
		const enhancedPrompt = buildEnhancedPrompt(prompt, !!referenceImage, frameContext)
		const aspectRatio = pickImagenAspectRatio(width, height)
		const generated = await generateImage(enhancedPrompt, apiKey, aspectRatio, referenceImage)
		if (!generated) return null

		const imageBuffer = base64ToArrayBuffer(generated.base64)
		const decoded = decodeImage(imageBuffer, generated.mimeType)
		if (!decoded) return null
		console.log(
			`[ImageGen] Decoded ${generated.mimeType}: ${decoded.width}x${decoded.height}`
		)

		return finishImageResult(
			decoded.rgba,
			decoded.width,
			decoded.height,
			width,
			height,
			enhancedPrompt,
			imageBuffer,
			generated.mimeType
		)
	} catch (error) {
		console.error('[ImageGen] Error processing image:', error)
		return null
	}
}

/**
 * Run source RGBA pixels through the device pipeline (cover-resize, grayscale,
 * dither, pack) and assemble an ImageResult with the given archival original.
 */
function finishImageResult(
	rgba: Uint8Array,
	srcW: number,
	srcH: number,
	dstW: number,
	dstH: number,
	enhancedPrompt: string,
	originalImage: ArrayBuffer,
	originalMimeType: string
): ImageResult {
	const resized = resizeImageCover(rgba, srcW, srcH, dstW, dstH)
	const grayscale = toGrayscale(resized, dstW, dstH)
	const dithered = floydSteinbergDither(grayscale, dstW, dstH)
	const packed = packBits(dithered)
	const base64 = arrayBufferToBase64(packed)
	console.log(`[ImageGen] Packed to ${packed.length} bytes (${base64.length}b64 chars)`)

	const ditheredRgba = new Uint8Array(dstW * dstH * 4)
	for (let i = 0; i < dithered.length; i++) {
		const v = dithered[i] ? 255 : 0
		ditheredRgba[i * 4] = v
		ditheredRgba[i * 4 + 1] = v
		ditheredRgba[i * 4 + 2] = v
		ditheredRgba[i * 4 + 3] = 255
	}
	const ditheredPng = UPNG.encode([ditheredRgba.buffer], dstW, dstH, 0)

	return {
		data: base64,
		width: dstW,
		height: dstH,
		ditheredPng,
		originalImage,
		originalMimeType,
		enhancedPrompt,
	}
}

/**
 * Generate all frames of a flipbook animation in ONE model call: the model
 * draws a grid of equal-sized sections ("contact sheet") showing successive
 * instants of the same scene, and we slice the grid into per-frame images.
 * A single generation keeps subject/style perfectly consistent across frames
 * and takes one generation's latency instead of one per frame.
 * Returns one ImageResult per frame prompt, or null on any failure (the
 * caller can fall back to chained per-frame generation).
 */
export async function generateAnimationStrip(
	framePrompts: string[],
	apiKey: string,
	targetWidth: number = DEFAULT_IMAGE_WIDTH,
	targetHeight: number = DEFAULT_IMAGE_HEIGHT,
	referenceImage?: ReferenceImage
): Promise<ImageResult[] | null> {
	const width = clampDimension(targetWidth, DEFAULT_IMAGE_WIDTH)
	const height = clampDimension(targetHeight, DEFAULT_IMAGE_HEIGHT)
	const count = framePrompts.length
	if (count < 2) return null
	try {
		const { rows, cols } = pickStripLayout(width / height, count)
		const enhancedPrompt = buildStripPrompt(framePrompts, rows, cols, !!referenceImage)
		// Pick the preset closest to the whole grid's aspect so each section's
		// aspect lands near the device's, minimizing per-cell cover-cropping.
		const aspectRatio = pickImagenAspectRatio(width * cols, height * rows)
		console.log(
			`[ImageGen] Generating ${rows}x${cols} strip (${count} sections, ${aspectRatio}) for: "${framePrompts[0]}"` +
				(referenceImage ? ' (with reference image)' : '')
		)
		const generated = await generateImage(enhancedPrompt, apiKey, aspectRatio, referenceImage)
		if (!generated) return null

		const imageBuffer = base64ToArrayBuffer(generated.base64)
		const decoded = decodeImage(imageBuffer, generated.mimeType)
		if (!decoded) return null
		console.log(
			`[ImageGen] Decoded strip ${generated.mimeType}: ${decoded.width}x${decoded.height}`
		)

		const regions = locateStripCells(decoded.rgba, decoded.width, decoded.height, rows, cols)
		const results: ImageResult[] = []
		for (let i = 0; i < count; i++) {
			const region = regions[i]
			if (region.w < 16 || region.h < 16) {
				console.error(`[ImageGen] Strip cell ${i} too small: ${region.w}x${region.h}`)
				return null
			}
			const cell = extractRegion(
				decoded.rgba,
				decoded.width,
				region.x,
				region.y,
				region.w,
				region.h
			)
			// Any leftover magenta fringe (anti-aliased separator edges) would
			// dither into gray speckle; force it to background black.
			suppressMagenta(cell)
			// Archive each frame's full-color cell as its own PNG so per-frame
			// recall and reference-image edits work exactly like plain images.
			const cellPng = UPNG.encode([cell.buffer], region.w, region.h, 0)
			results.push(
				finishImageResult(cell, region.w, region.h, width, height, enhancedPrompt, cellPng, 'image/png')
			)
		}
		return results
	} catch (error) {
		console.error('[ImageGen] Error generating animation strip:', error)
		return null
	}
}

interface CellRegion {
	x: number
	y: number
	w: number
	h: number
}

/**
 * Whether a pixel is (close to) the pure-magenta separator color. The
 * artwork itself is strictly monochrome (r ≈ g ≈ b), so anything strongly
 * magenta can only be a grid line — the thresholds just need to survive JPEG
 * compression around the separators.
 */
function isMagentaPixel(r: number, g: number, b: number): boolean {
	return r > 140 && b > 140 && g < 110 && r - g > 60 && b - g > 60
}

/** Fraction of magenta pixels per column ('col') or per row ('row'). */
function magentaFractions(
	rgba: Uint8Array,
	imgW: number,
	imgH: number,
	axis: 'col' | 'row'
): number[] {
	const counts = new Array(axis === 'col' ? imgW : imgH).fill(0)
	for (let y = 0; y < imgH; y++) {
		for (let x = 0; x < imgW; x++) {
			const i = (y * imgW + x) * 4
			if (isMagentaPixel(rgba[i], rgba[i + 1], rgba[i + 2])) {
				counts[axis === 'col' ? x : y]++
			}
		}
	}
	const denom = axis === 'col' ? imgH : imgW
	return counts.map((c) => c / denom)
}

/**
 * Split an axis into content spans delimited by magenta separator lines
 * (columns/rows that are mostly magenta) and keep the `expected` widest spans
 * — dropping outer margins and letterbox bars, which sit outside the magenta
 * grid and are narrower than real cells. Returns null when the axis doesn't
 * decompose into at least `expected` plausible spans.
 */
function findContentSpans(
	fractions: number[],
	expected: number,
	total: number
): Array<{ start: number; end: number }> | null {
	const spans: Array<{ start: number; end: number }> = []
	let start = -1
	for (let i = 0; i <= total; i++) {
		const separator = i === total || fractions[i] > 0.5
		if (!separator && start < 0) start = i
		if (separator && start >= 0) {
			spans.push({ start, end: i })
			start = -1
		}
	}
	if (spans.length < expected) return null
	spans.sort((a, b) => b.end - b.start - (a.end - a.start))
	const kept = spans.slice(0, expected).sort((a, b) => a.start - b.start)
	// Every kept span must look like a real cell, not a sliver.
	const minSpan = Math.max(16, Math.floor(total / (expected * 4)))
	if (kept.some((s) => s.end - s.start < minSpan)) return null
	// Inset a couple of pixels to shave anti-aliased separator fringe.
	return kept.map((s) => ({ start: s.start + 2, end: s.end - 2 }))
}

/**
 * Locate the actual cell rectangles in a generated strip. Preferred: find the
 * magenta separator lines the prompt asked for and use the content spans
 * between them (robust to margins, letterboxing, and uneven grids). Fallback:
 * equal division of the canvas.
 */
function locateStripCells(
	rgba: Uint8Array,
	imgW: number,
	imgH: number,
	rows: number,
	cols: number
): CellRegion[] {
	const colSpans = findContentSpans(magentaFractions(rgba, imgW, imgH, 'col'), cols, imgW)
	const rowSpans = findContentSpans(magentaFractions(rgba, imgW, imgH, 'row'), rows, imgH)
	if (colSpans && rowSpans) {
		console.log('[ImageGen] Strip grid located via magenta separators')
		const regions: CellRegion[] = []
		for (const rs of rowSpans) {
			for (const cs of colSpans) {
				regions.push({ x: cs.start, y: rs.start, w: cs.end - cs.start, h: rs.end - rs.start })
			}
		}
		return regions
	}

	console.log('[ImageGen] Magenta grid not detected; slicing into equal cells')
	const cellW = Math.floor(imgW / cols)
	const cellH = Math.floor(imgH / rows)
	const regions: CellRegion[] = []
	for (let r = 0; r < rows; r++) {
		for (let c = 0; c < cols; c++) {
			regions.push({ x: c * cellW, y: r * cellH, w: cellW, h: cellH })
		}
	}
	return regions
}

/** Replace magenta separator remnants with background black, in place. */
function suppressMagenta(rgba: Uint8Array): void {
	for (let i = 0; i < rgba.length; i += 4) {
		if (isMagentaPixel(rgba[i], rgba[i + 1], rgba[i + 2])) {
			rgba[i] = 0
			rgba[i + 1] = 0
			rgba[i + 2] = 0
		}
	}
}

/** Copy a rectangular RGBA region out of a larger RGBA buffer. */
function extractRegion(
	rgba: Uint8Array,
	srcW: number,
	x: number,
	y: number,
	w: number,
	h: number
): Uint8Array<ArrayBuffer> {
	const out = new Uint8Array(w * h * 4)
	for (let row = 0; row < h; row++) {
		const srcStart = ((y + row) * srcW + x) * 4
		out.set(rgba.subarray(srcStart, srcStart + w * 4), row * w * 4)
	}
	return out
}

/**
 * Choose a rows x cols grid for `count` animation sections whose overall
 * aspect ratio (cellAspect * cols / rows) sits closest to one of the image
 * model's aspect presets — that preset is what the strip is generated at, so
 * a close match means minimal cover-cropping per section.
 */
function pickStripLayout(cellAspect: number, count: number): { rows: number; cols: number } {
	let best = { rows: 1, cols: count }
	let bestDelta = Infinity
	for (let rows = 1; rows <= count; rows++) {
		if (count % rows !== 0) continue
		const cols = count / rows
		const gridAspect = (cellAspect * cols) / rows
		const delta = closestAspectPreset(gridAspect).delta
		if (delta < bestDelta) {
			best = { rows, cols }
			bestDelta = delta
		}
	}
	return best
}

function clampDimension(value: number, fallback: number): number {
	if (!Number.isFinite(value) || value <= 0) return fallback
	const rounded = Math.round(value)
	if (rounded < 16) return fallback
	if (rounded > MAX_IMAGE_WIDTH) return MAX_IMAGE_WIDTH
	return rounded
}

// The image model only accepts a fixed set of aspect ratio presets.
const ASPECT_PRESETS: Array<{ name: string; ratio: number }> = [
	{ name: '1:1', ratio: 1 },
	{ name: '4:3', ratio: 4 / 3 },
	{ name: '3:4', ratio: 3 / 4 },
	{ name: '4:5', ratio: 4 / 5 },
	{ name: '5:4', ratio: 5 / 4 },
	{ name: '3:2', ratio: 3 / 2 },
	{ name: '2:3', ratio: 2 / 3 },
	{ name: '16:9', ratio: 16 / 9 },
	{ name: '9:16', ratio: 9 / 16 },
]

/** Find the preset closest (in log-ratio distance) to the given aspect. */
function closestAspectPreset(target: number): { name: string; delta: number } {
	let best = ASPECT_PRESETS[0]
	let bestDelta = Math.abs(Math.log(target / best.ratio))
	for (const preset of ASPECT_PRESETS.slice(1)) {
		const delta = Math.abs(Math.log(target / preset.ratio))
		if (delta < bestDelta) {
			best = preset
			bestDelta = delta
		}
	}
	return { name: best.name, delta: bestDelta }
}

/**
 * Pick the aspect preset closest to the requested target so cover-scaling has
 * the least cropping to do.
 */
function pickImagenAspectRatio(width: number, height: number): string {
	return closestAspectPreset(width / height).name
}

// Tuned for monochrome dithered output: bright high-contrast subjects on black.
const STYLE_SUFFIX =
	'Style: white artwork on solid black background, high contrast, simple composition, clear silhouettes, dark mode aesthetic with bright white elements against pure black.'

function buildEnhancedPrompt(
	prompt: string,
	hasReference: boolean,
	frameContext?: FrameContext
): string {
	const styleSuffix = STYLE_SUFFIX
	// NOTE: never mention "animation", "flipbook", or "frame" in the prompt —
	// the image model draws the words literally (a request for "the first frame
	// of a flipbook animation" produces a picture of a flipbook). The animation
	// is our concern; the model only ever sees a scene or an edit instruction.
	if (frameContext && frameContext.index > 0 && hasReference) {
		// Later animation frame: the reference is the previous frame. Consistency
		// matters more than creativity — everything must stay identical except
		// the stated change, or the animation flickers instead of moving.
		return (
			`Reproduce the attached image EXACTLY — same subject, same style, same composition, ` +
			`same camera angle, same line weight, same background — with only this one difference: ` +
			`${prompt}. The difference should read as one small instant of motion later in the same scene. ` +
			styleSuffix
		)
	}
	// The first animation frame is deliberately identical to a plain image
	// request; it falls through to the branches below.
	if (hasReference) {
		// Edit mode: the request describes a change to the attached image, not a
		// scene from scratch. Anchor the model to the reference so "zoom in",
		// "rotate", "make it darker" etc. keep the same subject and composition.
		return `Using the attached image as the starting point, make this modification: ${prompt}. Preserve the subject and overall composition of the attached image except where the modification says otherwise. ${styleSuffix}`
	}
	return `${prompt}. ${styleSuffix}`
}

/**
 * Prompt for a grid of animation sections in one image. Unlike the
 * buildEnhancedPrompt frame branch, the grid vocabulary here is meant
 * literally — we want the model to draw the divided layout — so the language
 * is as exact as possible about section count, size, and position, and
 * forbids the decorations (borders, gutters, labels) that "contact sheet"
 * style images usually carry.
 */
function buildStripPrompt(
	framePrompts: string[],
	rows: number,
	cols: number,
	hasReference: boolean
): string {
	const layout =
		rows === 1
			? `a single row of ${cols} sections side by side`
			: cols === 1
				? `a single column of ${rows} sections stacked vertically`
				: `${rows} rows of ${cols} sections each`
	const readingOrder =
		rows === 1
			? 'left to right'
			: cols === 1
				? 'top to bottom'
				: 'left to right, then top to bottom'
	const parts: string[] = [
		`A single image divided into exactly ${framePrompts.length} equal rectangular sections: ${layout}, all EXACTLY the same size, filling the entire canvas edge to edge.`,
		// Magenta grid lines are a machine-readable sentinel: the artwork is
		// strictly monochrome, so slicing scans for magenta rows/columns to find
		// the real section boundaries (and discards letterbox bars outside them).
		'CRITICAL: the sections are separated from each other and surrounded on the outside by straight, solid, PURE MAGENTA (#FF00FF) dividing lines about 8 pixels thick, forming a precise rectangular grid. Magenta appears ONLY in these dividing lines. Apart from the magenta grid lines there are NO other borders, NO gutters, NO margins, NO numbers, NO labels, and NO text anywhere; each section\'s artwork is strictly black-and-white on a solid black background.',
		`Read ${readingOrder}, the sections show the SAME scene at successive instants of one motion: identical subject, identical style, identical camera angle, identical composition, identical line weight in every section — only the described motion differs. Keep the subject centered within its own section, well away from the section edges.`,
	]
	if (hasReference) {
		parts.push(
			'Every section depicts the subject of the attached image, in the style and composition of the attached image.'
		)
	}
	parts.push(`Section 1: ${framePrompts[0]}.`)
	for (let i = 1; i < framePrompts.length; i++) {
		parts.push(`Section ${i + 1}: identical to section ${i}, except: ${framePrompts[i]}.`)
	}
	parts.push(STYLE_SUFFIX)
	return parts.join(' ')
}

async function generateImage(
	enhancedPrompt: string,
	apiKey: string,
	aspectRatio: string,
	referenceImage?: ReferenceImage
): Promise<{ base64: string; mimeType: string } | null> {
	const controller = new AbortController()
	const timeoutId = setTimeout(() => controller.abort(), IMAGE_GEN_TIMEOUT_MS)

	// For modifications, the reference image goes first so the text reads as an
	// instruction about the attached image.
	const requestParts: Array<Record<string, unknown>> = []
	if (referenceImage) {
		requestParts.push({
			inlineData: {
				mimeType: referenceImage.mimeType,
				data: arrayBufferToBase64(referenceImage.data),
			},
		})
	}
	requestParts.push({ text: enhancedPrompt })

	let response: Response
	try {
		response = await fetch(
			`https://generativelanguage.googleapis.com/v1beta/models/${IMAGE_MODEL}:generateContent`,
			{
				method: 'POST',
				headers: {
					'Content-Type': 'application/json',
					'x-goog-api-key': apiKey,
				},
				body: JSON.stringify({
					contents: [{ parts: requestParts }],
					generationConfig: {
						responseModalities: ['IMAGE'],
						// Cover scaling later crops the long axis as needed; this just
						// picks whichever preset is closest to the device's display.
						imageConfig: { aspectRatio },
					},
				}),
				signal: controller.signal,
			}
		)
	} catch (error) {
		if (error instanceof Error && error.name === 'AbortError') {
			console.error(`[ImageGen] Image API timed out after ${IMAGE_GEN_TIMEOUT_MS}ms`)
			return null
		}
		throw error
	} finally {
		clearTimeout(timeoutId)
	}

	if (!response.ok) {
		const errorText = await response.text()
		console.error(`[ImageGen] Image API error: ${response.status} ${errorText}`)
		return null
	}

	const result = (await response.json()) as {
		candidates?: Array<{
			content?: { parts?: Array<{ inlineData?: { mimeType?: string; data?: string } }> }
		}>
	}
	const responseParts = result.candidates?.[0]?.content?.parts ?? []
	for (const part of responseParts) {
		if (part.inlineData?.data) {
			return {
				base64: part.inlineData.data,
				mimeType: part.inlineData.mimeType ?? 'image/jpeg',
			}
		}
	}
	console.error('[ImageGen] No image data in response')
	return null
}

/**
 * Decode PNG or JPEG bytes to RGBA. The Gemini image model returns JPEG (the
 * API has no output-format option); PNG support is kept for safety.
 */
function decodeImage(
	buffer: ArrayBuffer,
	mimeType: string
): { rgba: Uint8Array; width: number; height: number } | null {
	if (mimeType === 'image/png') {
		const decoded = UPNG.decode(buffer)
		return {
			rgba: new Uint8Array(UPNG.toRGBA8(decoded)[0]),
			width: decoded.width,
			height: decoded.height,
		}
	}
	if (mimeType === 'image/jpeg') {
		const decoded = jpeg.decode(new Uint8Array(buffer), { useTArray: true })
		return {
			rgba: new Uint8Array(decoded.data.buffer, decoded.data.byteOffset, decoded.data.length),
			width: decoded.width,
			height: decoded.height,
		}
	}
	console.error(`[ImageGen] Unsupported image mime type: ${mimeType}`)
	return null
}

/**
 * Resize with cover semantics: image fills the target box, centered, overflow
 * clipped on the long axis. Matches the "cover" rule in designs.md.
 */
function resizeImageCover(
	pixels: Uint8Array,
	srcW: number,
	srcH: number,
	dstW: number,
	dstH: number
): Uint8Array {
	const dst = new Uint8Array(dstW * dstH * 4)
	// Cover: pick the SMALLER scale ratio so the image is large enough to fill
	// both dimensions; the larger axis overflows and gets clipped.
	const scale = Math.min(srcW / dstW, srcH / dstH)
	const cropOffsetX = (srcW - dstW * scale) / 2
	const cropOffsetY = (srcH - dstH * scale) / 2

	for (let y = 0; y < dstH; y++) {
		for (let x = 0; x < dstW; x++) {
			const dstIdx = (y * dstW + x) * 4
			const srcX = cropOffsetX + x * scale
			const srcY = cropOffsetY + y * scale

			const x0 = Math.max(0, Math.min(srcW - 1, Math.floor(srcX)))
			const y0 = Math.max(0, Math.min(srcH - 1, Math.floor(srcY)))
			const x1 = Math.min(x0 + 1, srcW - 1)
			const y1 = Math.min(y0 + 1, srcH - 1)
			const xw = srcX - Math.floor(srcX)
			const yw = srcY - Math.floor(srcY)

			for (let c = 0; c < 4; c++) {
				const v00 = pixels[(y0 * srcW + x0) * 4 + c]
				const v10 = pixels[(y0 * srcW + x1) * 4 + c]
				const v01 = pixels[(y1 * srcW + x0) * 4 + c]
				const v11 = pixels[(y1 * srcW + x1) * 4 + c]
				dst[dstIdx + c] = Math.round(
					v00 * (1 - xw) * (1 - yw) +
						v10 * xw * (1 - yw) +
						v01 * (1 - xw) * yw +
						v11 * xw * yw
				)
			}
		}
	}
	return dst
}

function toGrayscale(pixels: Uint8Array, width: number, height: number): Float32Array {
	const gray = new Float32Array(width * height)
	for (let i = 0; i < width * height; i++) {
		// ITU-R BT.601 luminance
		gray[i] = 0.299 * pixels[i * 4] + 0.587 * pixels[i * 4 + 1] + 0.114 * pixels[i * 4 + 2]
	}
	return gray
}

function floydSteinbergDither(
	gray: Float32Array,
	width: number,
	height: number
): Uint8Array {
	const result = new Uint8Array(width * height)
	const buffer = Float32Array.from(gray)
	for (let y = 0; y < height; y++) {
		for (let x = 0; x < width; x++) {
			const idx = y * width + x
			const oldPixel = Math.max(0, Math.min(255, buffer[idx]))
			const newPixel = oldPixel < 128 ? 0 : 255
			result[idx] = newPixel === 255 ? 1 : 0
			const error = oldPixel - newPixel
			if (x + 1 < width) buffer[idx + 1] += (error * 7) / 16
			if (y + 1 < height) {
				if (x > 0) buffer[idx + width - 1] += (error * 3) / 16
				buffer[idx + width] += (error * 5) / 16
				if (x + 1 < width) buffer[idx + width + 1] += (error * 1) / 16
			}
		}
	}
	return result
}

/** Pack 1-bit pixels into bytes, MSB first (bit 7 = first pixel of each byte). */
function packBits(pixels: Uint8Array): Uint8Array {
	const packed = new Uint8Array(Math.ceil(pixels.length / 8))
	for (let i = 0; i < pixels.length; i++) {
		if (pixels[i]) packed[Math.floor(i / 8)] |= 1 << (7 - (i % 8))
	}
	return packed
}

function base64ToArrayBuffer(base64: string): ArrayBuffer {
	const binary = atob(base64)
	const bytes = new Uint8Array(binary.length)
	for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
	return bytes.buffer
}

function arrayBufferToBase64(buffer: Uint8Array): string {
	let binary = ''
	for (let i = 0; i < buffer.length; i++) binary += String.fromCharCode(buffer[i])
	return btoa(binary)
}
