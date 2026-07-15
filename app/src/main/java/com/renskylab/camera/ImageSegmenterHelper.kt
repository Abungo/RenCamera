package com.renskylab.camera

import android.content.Context
import android.graphics.Bitmap
import com.google.mediapipe.framework.image.BitmapImageBuilder
import com.google.mediapipe.tasks.core.BaseOptions
import com.google.mediapipe.tasks.vision.imagesegmenter.ImageSegmenter
import java.nio.ByteBuffer

/**
 * Result representation of multiclass segmentation.
 * Contains the raw category byte array and its dimensions.
 */
data class MultiClassSegmentationResult(
    val bytes: ByteArray,
    val width: Int,
    val height: Int
)

/**
 * Wrapper for MediaPipe Multiclass Selfie Segmentation.
 * Segments an image into 6 classes:
 * - 0: Background
 * - 1: Hair
 * - 2: Body (Skin)
 * - 3: Face (Skin)
 * - 4: Clothes
 * - 5: Others (Accessories)
 */
class ImageSegmenterHelper(private val context: Context) {

    private var segmenter: ImageSegmenter? = null

    init {
        try {
            val baseOptions = BaseOptions.builder()
                .setModelAssetPath("deeplabv3.tflite")
                .build()

            val options = ImageSegmenter.ImageSegmenterOptions.builder()
                .setBaseOptions(baseOptions)
                .setRunningMode(com.google.mediapipe.tasks.vision.core.RunningMode.IMAGE)
                .setOutputCategoryMask(true)
                .setOutputConfidenceMasks(false)
                .build()

            segmenter = ImageSegmenter.createFromOptions(context, options)
        } catch (e: Exception) {
            android.util.Log.e("RenCamera/Segmenter", "Failed to initialize MediaPipe ImageSegmenter", e)
        }
    }

    /**
     * Segments the input bitmap and returns multiclass category bytes.
     */
    fun segmentImage(bitmap: Bitmap): MultiClassSegmentationResult? {
        val activeSegmenter = segmenter ?: return null
        return try {
            val mpImage = BitmapImageBuilder(bitmap).build()
            val result = activeSegmenter.segment(mpImage)
            val categoryMask = result.categoryMask().orElse(null) ?: return null

            val width = categoryMask.width
            val height = categoryMask.height
            val byteBuffer = com.google.mediapipe.framework.image.ByteBufferExtractor.extract(categoryMask)

            val outputBytes = ByteArray(width * height)
            byteBuffer.rewind()
            byteBuffer.get(outputBytes, 0, outputBytes.size)

            MultiClassSegmentationResult(outputBytes, width, height)
        } catch (e: Exception) {
            android.util.Log.e("RenCamera/Segmenter", "MediaPipe segmentation failed", e)
            null
        }
    }

    fun close() {
        segmenter?.close()
    }
}
