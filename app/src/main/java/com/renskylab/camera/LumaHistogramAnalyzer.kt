package com.renskylab.camera

import android.graphics.ImageFormat
import android.media.Image
import android.util.Log
import java.nio.ByteBuffer

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
     * Synchronously extracts downsampled luma samples from the image buffer.
     * Run this on the camera handler thread BEFORE the image is closed or recycled.
     */
    fun extractLumaSamples(image: Image, whiteLevel: Float): FloatArray {
        try {
            val width = image.width
            val height = image.height
            val format = image.format

            val step = 16
            val sampleW = (width + step - 1) / step
            val sampleH = (height + step - 1) / step
            val samples = FloatArray(sampleW * sampleH)
            var count = 0

            if (format == ImageFormat.YUV_420_888) {
                val plane = image.planes[0]
                val buffer = plane.buffer
                val rowStride = plane.rowStride
                val pixelStride = plane.pixelStride

                val oldPos = buffer.position()
                val rowData = ByteArray(width)

                for (y in 0 until height step step) {
                    buffer.position(oldPos + y * rowStride)
                    val remaining = buffer.remaining()
                    val toRead = Math.min(width, remaining)
                    buffer.get(rowData, 0, toRead)

                    for (x in 0 until toRead step step) {
                        val pixelIdx = x * pixelStride
                        if (pixelIdx < toRead && count < samples.size) {
                            val yVal = rowData[pixelIdx].toInt() and 0xFF
                            samples[count++] = yVal / 255.0f
                        }
                    }
                }
                buffer.position(oldPos) // restore original position
            } else if (format == ImageFormat.RAW_SENSOR) {
                val plane = image.planes[0]
                val buffer = plane.buffer
                val rowStride = plane.rowStride
                val pixelStride = plane.pixelStride

                val oldPos = buffer.position()
                buffer.position(0)

                for (y in 0 until height step step) {
                    val rowOffset = y * rowStride
                    for (x in 0 until width step step) {
                        val pos = rowOffset + x * pixelStride
                        if (pos + 1 < buffer.capacity() && count < samples.size) {
                            val valLow = buffer.get(pos).toInt() and 0xFF
                            val valHigh = buffer.get(pos + 1).toInt() and 0xFF
                            val rawVal = (valHigh shl 8) or valLow
                            val normVal = Math.min(1.0f, Math.max(0.0f, rawVal.toFloat() / whiteLevel))
                            samples[count++] = normVal
                        }
                    }
                }
                buffer.position(oldPos) // restore original position
            }

            return if (count == 0) FloatArray(0) else samples.copyOf(count)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to extract luma samples", e)
            return FloatArray(0)
        }
    }

    /**
     * Recommend a target EV offset to prevent highlight clipping based on extracted samples.
     * Safe to run asynchronously on any background dispatcher.
     */
    fun analyzeSamples(samples: FloatArray): AnalysisResult {
        try {
            val totalPixels = samples.size
            if (totalPixels == 0) {
                return AnalysisResult(10.0f, 240.0f, 4.5f, 0.0f)
            }

            val hist = IntArray(BINS)
            for (i in 0 until totalPixels) {
                val bin = (samples[i] * (BINS - 1)).toInt()
                hist[bin.coerceIn(0, BINS - 1)]++
            }

            // Compute cumulative percentiles (2nd and 98th)
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

            // Highlight clipping threshold: ~245 in 8-bit luma
            val targetHighlight = 245.0f
            val targetEv = if (highlightLuma > targetHighlight) {
                val evRatio = targetHighlight / highlightLuma
                val computedEv = (Math.log(evRatio.toDouble()) / Math.log(2.0)).toFloat()
                Math.min(0.0f, Math.max(-2.5f, computedEv))
            } else if (highlightLuma < 180.0f) {
                0.0f
            } else {
                val computedEv = (highlightLuma - targetHighlight) / targetHighlight
                Math.min(0.0f, Math.max(-0.5f, computedEv))
            }

            return AnalysisResult(shadowLuma, highlightLuma, drStops, targetEv)
        } catch (e: Exception) {
            Log.e(TAG, "LumaHistogramAnalyzer analyzeSamples failed", e)
            return AnalysisResult(10.0f, 240.0f, 4.5f, 0.0f)
        }
    }
}
