package com.renskylab.camera

import android.graphics.ImageFormat
import android.media.Image
import android.util.Log

/**
 * Real-time luminance histogram analyzer for dynamic range estimation
 * and dynamic highlight-priority auto exposure.
 */
object LumaHistogramAnalyzer {
    private const val TAG = "RenCamera/LumaHist"
    private const val BINS = 64

    data class AnalysisResult(
        val shadowLuma: Float,
        val highlightLuma: Float,
        val drStops: Float,
        val targetEv: Float
    )

    /**
     * Builds a fast, downsampled luminance histogram from a preview/captured frame.
     * Computes shadow/highlight percentiles and recommends a target EV offset to prevent highlight clipping.
     */
    fun analyze(image: Image): AnalysisResult {
        try {
            val width = image.width
            val height = image.height
            val format = image.format

            val hist = IntArray(BINS)
            var totalPixels = 0

            if (format == ImageFormat.YUV_420_888) {
                // YUV: analyze Y-plane directly
                val plane = image.planes[0]
                val buffer = plane.buffer
                val rowStride = plane.rowStride
                val pixelStride = plane.pixelStride

                // Highly downsampled (step by 16 in row & col) to keep computation instant (~0.5ms)
                val step = 16
                val rowData = ByteArray(width)
                for (y in 0 until height step step) {
                    buffer.position(y * rowStride)
                    val remaining = buffer.remaining()
                    val toRead = Math.min(width, remaining)
                    buffer.get(rowData, 0, toRead)

                    for (x in 0 until toRead step step) {
                        val pixelIdx = x * pixelStride
                        if (pixelIdx < toRead) {
                            val yVal = rowData[pixelIdx].toInt() and 0xFF
                            val bin = (yVal * (BINS - 1)) / 255
                            hist[bin.coerceIn(0, BINS - 1)]++
                            totalPixels++
                        }
                    }
                }
            } else if (format == ImageFormat.RAW_SENSOR) {
                // RAW: analyze Green pixels (standard Bayer layout)
                val plane = image.planes[0]
                val buffer = plane.buffer
                val rowStride = plane.rowStride
                val pixelStride = plane.pixelStride

                // Raw values are usually 10/12/14/16-bit. We normalize to 12-bit (4095) for consistency.
                val step = 16
                buffer.position(0)
                // Read sample pixels
                for (y in 0 until height step step) {
                    val rowOffset = y * rowStride
                    for (x in 0 until width step step) {
                        val pos = rowOffset + x * pixelStride
                        if (pos + 1 < buffer.capacity()) {
                            val valLow = buffer.get(pos).toInt() and 0xFF
                            val valHigh = buffer.get(pos + 1).toInt() and 0xFF
                            val rawVal = (valHigh shl 8) or valLow
                            // Max green range fallback estimation
                            // Max green range fallback estimation
                            val normVal = Math.min(1.0f, Math.max(0.0f, rawVal.toFloat() / 4095.0f))
                            val bin = (normVal * (BINS - 1)).toInt()
                            hist[bin.coerceIn(0, BINS - 1)]++
                            totalPixels++
                        }
                    }
                }
            }

            if (totalPixels == 0) {
                return AnalysisResult(10.0f, 240.0f, 4.5f, 0.0f)
            }

            // Compute cumulative percentiles
            // Shadows: 2nd percentile
            // Highlights: 98th percentile
            val shadowCount = (totalPixels * 0.02f).toInt()
            val highlightCount = (totalPixels * 0.98f).toInt()

            var cumSum = 0
            var shadowBin = 0
            var highlightBin = BINS - 1

            var shadowFound = false
            for (i in 0 until BINS) {
                cumSum += hist[i]
                if (!shadowFound && cumSum >= shadowCount) {
                    shadowBin = i
                    shadowFound = true
                }
                if (cumSum >= highlightCount) {
                    highlightBin = i
                    break
                }
            }

            val shadowLuma = (shadowBin.toFloat() / (BINS - 1).toFloat()) * 255.0f
            val highlightLuma = (highlightBin.toFloat() / (BINS - 1).toFloat()) * 255.0f

            // Compute Dynamic Range (DR) in stops
            val maxRatio = Math.max(1.0f, highlightLuma) / Math.max(1.0f, shadowLuma)
            val drStops = (Math.log(maxRatio.toDouble()) / Math.log(2.0)).toFloat()

            // Recommend Target EV offset to protect highlights from clipping
            // Highlight clipping threshold: ~245 in 8-bit luma
            val targetHighlight = 245.0f
            val targetEv = if (highlightLuma > targetHighlight) {
                // Highlight is clipping; compute how much EV underexposure we need to pull it down to targetHighlight
                val evRatio = targetHighlight / highlightLuma
                val computedEv = (Math.log(evRatio.toDouble()) / Math.log(2.0)).toFloat()
                Math.min(0.0f, Math.max(-2.5f, computedEv))
            } else if (highlightLuma < 180.0f) {
                // Scene has low dynamic range and highlights are dim; we can safely raise EV compensation
                0.0f
            } else {
                // Highlights are close but safe; slight negative bias to guarantee protection
                val computedEv = (highlightLuma - targetHighlight) / targetHighlight
                Math.min(0.0f, Math.max(-0.5f, computedEv))
            }

            return AnalysisResult(shadowLuma, highlightLuma, drStops, targetEv)
        } catch (e: Exception) {
            Log.e(TAG, "LumaHistogramAnalyzer failed", e)
            return AnalysisResult(10.0f, 240.0f, 4.5f, 0.0f)
        }
    }
}
