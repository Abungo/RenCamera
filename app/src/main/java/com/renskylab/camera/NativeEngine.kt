package com.renskylab.camera

import java.nio.ByteBuffer

/**
 * Singleton JNI bridge to the native gcam_core library.
 *
 * The native function receives direct ByteBuffer references to the Camera2
 * image planes — zero-copy into the JNI layer.
 */
object NativeEngine {

    init {
        System.loadLibrary("gcam_core")
    }

    /**
     * Interface definition for receiving progress notifications during image processing stages.
     */
    interface ProgressListener {
        /**
         * Invoked periodically to report progress of a specific pipeline stage.
         *
         * @param step The name of the current processing stage (e.g., "align", "fuse", "debayer").
         * @param percentage The progress percentage completed for the current stage (0 to 100).
         */
        fun onProgress(step: String, percentage: Int)
    }

    /**
     * Copies the planes of multiple burst frames from the Android JVM Heap to C++ native heap memory.
     * This prepares the frame buffers for zero-copy operations during alignment and fusion.
     *
     * @param count The number of frames in the burst capture.
     * @param yPlanes Array of ByteBuffers referencing the Y (luma) channel of each frame.
     * @param uPlanes Array of ByteBuffers referencing the U (chroma) channel of each frame.
     * @param vPlanes Array of ByteBuffers referencing the V (chroma) channel of each frame.
     * @param yRowStrides Integer array representing the row strides of the Y plane of each frame.
     * @param uvRowStrides Integer array representing the row strides of the U/V planes of each frame.
     * @param uvPixelStrides Integer array representing the pixel strides of the U/V planes of each frame.
     * @param width The image width in pixels.
     * @param height The image height in pixels.
     * @return A long handle (pointer address) referencing the allocated CopiedBurst structure in C++.
     */
    external fun copyBurst(
        count: Int,
        yPlanes: Array<ByteBuffer>,
        uPlanes: Array<ByteBuffer>,
        vPlanes: Array<ByteBuffer>,
        yRowStrides: IntArray,
        uvRowStrides: IntArray,
        uvPixelStrides: IntArray,
        width: Int,
        height: Int
    ): Long

    /**
     * Frees the native heap memory allocated for a burst capture.
     * Must be called once processing completes or fails to prevent memory leaks.
     *
     * @param handle The C++ pointer address returned by [copyBurst].
     */
    external fun freeBurst(handle: Long)

    /**
     * Saves the raw YUV or Bayer frames inside a native burst to a target directory.
     * Primarily used for debugging and diagnostics.
     *
     * @param handle The C++ pointer address returned by [copyBurst].
     * @param dirPath The absolute directory path where raw files will be saved.
     * @param useRaw If true, dumps RAW Bayer data; otherwise, dumps YUV420 planar data.
     * @param debugRawDumps If true, enables additional raw intermediate diagnostics.
     * @return True if files were successfully written; false otherwise.
     */
    external fun saveRawBurst(handle: Long, dirPath: String, useRaw: Boolean, debugRawDumps: Boolean): Boolean

    /**
     * Executes the computational camera pipeline on the pre-copied native burst frames.
     * Handles alignment, fusion, debayering, denoising, sharpening, and tonemapping, returning a JPEG.
     *
     * @param handle The C++ pointer address returned by [copyBurst].
     * @param jpegQuality The target compression quality for the output JPEG (range 1-100).
     * @param stageFlags Boolean array mapping configuration status for each pipeline stage.
     * @param nightMode Set to true to optimize alignment and fusion parameters for low light conditions.
     * @param iso The target ISO sensitivity of the overall capture.
     * @param frameIsos Integer array representing the individual ISO speed of each captured frame.
     * @param frameNoiseProfiles Flat float array of size N * 8 containing noise model sensor coefficients (S, O).
     * @param configParams Float array containing configuration tuning parameters (alignment, fusion, etc.).
     * @param debugDir Absolute directory path where per-stage debugging images are written. If empty, debugging is skipped.
     * @param listener Callback interface to monitor progress updates of the execution.
     * @return The final processed JPEG compressed image bytes, or null if processing fails.
     */
    external fun processCopiedBurst(
        handle: Long,
        jpegQuality: Int,
        stageFlags: BooleanArray,
        nightMode: Boolean,
        iso: Int,
        frameIsos: IntArray,
        frameExposures: LongArray,
        frameNoiseProfiles: FloatArray,
        configParams: FloatArray,
        debugDir: String,
        listener: ProgressListener?
    ): ByteArray?
}
