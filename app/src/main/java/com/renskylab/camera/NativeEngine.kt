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

    interface ProgressListener {
        fun onProgress(step: String, percentage: Int)
    }

    /**
     * Copy the burst frame planes to C++ native heap memory.
     * Returns a long handle pointing to the CopiedBurst structure in C++.
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
     * Free the memory allocated in C++ native heap for the CopiedBurst handle.
     */
    external fun freeBurst(handle: Long)

    /**
     * Save the raw YUV frames inside the native burst to a directory as planar I420 YUV files.
     */
    external fun saveRawBurst(handle: Long, dirPath: String, useRaw: Boolean, debugRawDumps: Boolean): Boolean

    /**
     * Process the pre-copied native burst using JNI.
     */
    external fun processCopiedBurst(
        handle: Long,
        jpegQuality: Int,
        stageFlags: BooleanArray,
        nightMode: Boolean,
        iso: Int,
        frameIsos: IntArray,
        configParams: FloatArray,
        debugDir: String,
        listener: ProgressListener?
    ): ByteArray?
}
