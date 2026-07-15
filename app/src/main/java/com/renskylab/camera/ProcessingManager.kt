package com.renskylab.camera

import android.media.Image
import android.net.Uri
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.ConcurrentHashMap

import java.nio.ByteBuffer

/**
 * Data class representing a pending or active image processing job.
 * Encapsulates the hardware configuration, frame reference, capture parameters,
 * and callbacks required for processing a burst of frames.
 *
 * @property id Unique identifier for the processing job.
 * @property timestamp The Epoch time at which the capture job was initialized.
 * @property iso The target ISO sensitivity computed for the overall capture.
 * @property frameIsos Array containing the captured ISO value of each frame.
 * @property frameExposures Optional array containing exposure times (in nanoseconds) of each frame.
 * @property frameNoiseProfiles Optional array containing sensor noise floor model coefficients.
 * @property nativeBurstHandle C++ pointer referencing the allocated CopiedBurst memory.
 * @property config Configuration params tuning each pipeline step.
 * @property onSaved Callback function invoked when final JPEG is saved successfully.
 * @property onError Callback function invoked when the pipeline encounters a terminal error.
 * @property awbGains White balance gains computed from color sensors.
 * @property blackLevel The baseline raw black level value.
 * @property whiteLevel The maximum raw white level (saturation threshold).
 * @property digitalGain Computational/digital scaling factor applied post-capture.
 */
data class ProcessingJob(
    val id: String,
    val timestamp: Long,
    val iso: Int,
    val frameIsos: IntArray,
    val frameExposures: LongArray? = null,
    val frameNoiseProfiles: FloatArray? = null, // N * 8 float values (S, O coefficients per channel)
    val nativeBurstHandle: Long,
    val config: PipelineConfig,
    val onSaved: (Uri) -> Unit,
    val onError: (String) -> Unit,
    val awbGains: FloatArray? = null,
    val colorCorrectionMatrix: FloatArray? = null,
    val blackLevel: Float = 1024f,
    val whiteLevel: Float = 4095f,
    val digitalGain: Float = 1.0f,
    val sensorOrientation: Int = 90,
    val appliedEvCompensation: Float = 0.0f,
    val colorFilterArrangement: Int = 3
)

/**
 * Thread-safe singleton manager that coordinates concurrent and queued processing jobs.
 * Manages active states and serves as the source of truth for the background [ProcessingService].
 */
object ProcessingManager {
    private val jobs = ConcurrentHashMap<String, ProcessingJob>()
    
    private val _isProcessing = MutableStateFlow(false)
    
    /**
     * StateFlow indicating whether there are one or more image processing jobs currently active.
     */
    val isProcessing: StateFlow<Boolean> = _isProcessing.asStateFlow()

    private val _processingProgress = MutableStateFlow(0)
    
    /**
     * StateFlow indicating the percentage progress of the active processing job (0 to 100).
     */
    val processingProgress: StateFlow<Int> = _processingProgress.asStateFlow()

    /**
     * Updates the active processing progress percentage.
     */
    fun updateProgress(percentage: Int) {
        _processingProgress.value = percentage
    }

    /**
     * Registers a new processing job and triggers the processing active state.
     *
     * @param job The job configuration to add.
     */
    fun addJob(job: ProcessingJob) {
        jobs[job.id] = job
        _processingProgress.value = 0
        _isProcessing.value = true
    }

    /**
     * Retrieves a job by its unique identifier.
     *
     * @param id The unique job identifier.
     * @return The matching [ProcessingJob], or null if it does not exist.
     */
    fun getJob(id: String): ProcessingJob? = jobs[id]

    /**
     * Removes a job from the active jobs list.
     * Updates the processing active state based on whether other jobs are remaining in the queue.
     *
     * @param id The unique job identifier to remove.
     */
    fun removeJob(id: String) {
        jobs.remove(id)
        if (jobs.isEmpty()) {
            _processingProgress.value = 0
        }
        _isProcessing.value = jobs.isNotEmpty()
    }
    
    /**
     * Checks if there are any jobs currently registered in the manager.
     *
     * @return True if jobs map is not empty; false otherwise.
     */
    fun hasJobs(): Boolean = jobs.isNotEmpty()
}
