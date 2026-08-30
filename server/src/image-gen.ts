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
	targetHeight: number = DEFAULT_IMAGE_HEIGHT
): Promise<ImageResult | null> {
	const width = clampDimension(targetWidth, DEFAULT_IMAGE_WIDTH)
	const height = clampDimension(targetHeight, DEFAULT_IMAGE_HEIGHT)
	try {
		console.log(`[ImageGen] Generating ${width}x${height} image for: "${prompt}"`)
		const enhancedPrompt = buildEnhancedPrompt(prompt)
		const aspectRatio = pickImagenAspectRatio(width, height)
		const generated = await generateImage(enhancedPrompt, apiKey, aspectRatio)
		if (!generated) return null

		const imageBuffer = base64ToArrayBuffer(generated.base64)
		const decoded = decodeImage(imageBuffer, generated.mimeType)
		if (!decoded) return null
		console.log(
			`[ImageGen] Decoded ${generated.mimeType}: ${decoded.width}x${decoded.height}`
		)

		const resized = resizeImageCover(
			decoded.rgba,
			decoded.width,
			decoded.height,
			width,
			height
		)

		const grayscale = toGrayscale(resized, width, height)
		const dithered = floydSteinbergDither(grayscale, width, height)
		const packed = packBits(dithered)
		const base64 = arrayBufferToBase64(packed)
		console.log(`[ImageGen] Packed to ${packed.length} bytes (${base64.length}b64 chars)`)

		const ditheredRgba = new Uint8Array(width * height * 4)
		for (let i = 0; i < dithered.length; i++) {
			const v = dithered[i] ? 255 : 0
			ditheredRgba[i * 4] = v
			ditheredRgba[i * 4 + 1] = v
			ditheredRgba[i * 4 + 2] = v
			ditheredRgba[i * 4 + 3] = 255
		}
		const ditheredPng = UPNG.encode([ditheredRgba.buffer], width, height, 0)

		return {
			data: base64,
			width,
			height,
			ditheredPng,
			originalImage: imageBuffer,
			originalMimeType: generated.mimeType,
			enhancedPrompt,
		}
	} catch (error) {
		console.error('[ImageGen] Error processing image:', error)
		return null
	}
}

function clampDimension(value: number, fallback: number): number {
	if (!Number.isFinite(value) || value <= 0) return fallback
	const rounded = Math.round(value)
	if (rounded < 16) return fallback
	if (rounded > MAX_IMAGE_WIDTH) return MAX_IMAGE_WIDTH
	return rounded
}

/**
 * The image model only accepts a fixed set of aspect ratio presets. Pick the
 * one whose ratio is closest to the requested target so cover-scaling has the
 * least cropping to do.
 */
function pickImagenAspectRatio(width: number, height: number): string {
	const presets: Array<{ name: string; ratio: number }> = [
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
	const target = width / height
	let best = presets[0]
	let bestDelta = Math.abs(Math.log(target / best.ratio))
	for (const preset of presets.slice(1)) {
		const delta = Math.abs(Math.log(target / preset.ratio))
		if (delta < bestDelta) {
			best = preset
			bestDelta = delta
		}
	}
	return best.name
}

function buildEnhancedPrompt(prompt: string): string {
	// Tuned for monochrome dithered output: bright high-contrast subjects on black.
	return `${prompt}. Style: white artwork on solid black background, high contrast, simple composition, clear silhouettes, dark mode aesthetic with bright white elements against pure black.`
}

async function generateImage(
	enhancedPrompt: string,
	apiKey: string,
	aspectRatio: string
): Promise<{ base64: string; mimeType: string } | null> {
	const controller = new AbortController()
	const timeoutId = setTimeout(() => controller.abort(), IMAGE_GEN_TIMEOUT_MS)

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
					contents: [{ parts: [{ text: enhancedPrompt }] }],
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
	const parts = result.candidates?.[0]?.content?.parts ?? []
	for (const part of parts) {
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
