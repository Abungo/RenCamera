package com.renskylab.camera

import android.media.Image

/**
 * Wraps a Camera2 [Image] along with its capture metadata (ISO, exposure time).
 */
data class CapturedFrame(
    val image: Image,
    val iso: Int,
    val exposureTimeNs: Long,
    val noiseProfile: FloatArray? = null // size 8 (4 pairs of S and O noise coefficients)
)

/**
 * Thread-safe FIFO ring buffer for [CapturedFrame] frames.
 *
 * Keeps the most recent [maxSize] frames resident in RAM.
 * When the buffer is full and a new frame arrives, the oldest frame
 * is closed immediately to return its buffer slot to the ImageReader pool.
 */
class FrameRingBuffer(private val maxSize: Int = 12) {

    private val lock  = Any()
    private val queue = ArrayDeque<CapturedFrame>(maxSize)

    /**
     * Add a new frame to the buffer.
     * If the buffer is already at capacity, the oldest frame is closed and evicted.
     */
    fun push(frame: CapturedFrame) {
        val evicted: CapturedFrame?
        synchronized(lock) {
            evicted = if (queue.size >= maxSize) queue.removeFirst() else null
            queue.addLast(frame)
        }
        evicted?.image?.close() // close outside the lock to avoid potential deadlock
    }

    /**
     * Drain all frames from the buffer and return them.
     *
     * Unlike a plain copy, this **removes** the images from the queue so that
     * concurrent [push] calls cannot evict and close them while the caller is
     * still processing pixel data. Ownership transfers to the caller; the
     * caller MUST call [Image.close] on every returned image when done.
     */
    fun snapshot(): List<CapturedFrame> {
        synchronized(lock) {
            val drained = queue.toList()
            queue.clear() // transfer ownership
            return drained
        }
    }

    /**
     * Discard all buffered frames, closing each one.
     */
    fun flush() {
        val toClose: List<CapturedFrame>
        synchronized(lock) {
            toClose = queue.toList()
            queue.clear()
        }
        toClose.forEach { runCatching { it.image.close() } }
    }

    val size: Int get() = synchronized(lock) { queue.size }
}
