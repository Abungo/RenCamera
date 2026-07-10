package com.renskylab.camera

import android.media.Image
import android.net.Uri
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.ConcurrentHashMap

import java.nio.ByteBuffer

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
    val blackLevel: Float = 1024f,
    val whiteLevel: Float = 4095f,
    val digitalGain: Float = 1.0f
)

object ProcessingManager {
    private val jobs = ConcurrentHashMap<String, ProcessingJob>()
    
    private val _isProcessing = MutableStateFlow(false)
    val isProcessing: StateFlow<Boolean> = _isProcessing.asStateFlow()

    fun addJob(job: ProcessingJob) {
        jobs[job.id] = job
        _isProcessing.value = true
    }

    fun getJob(id: String): ProcessingJob? = jobs[id]

    fun removeJob(id: String) {
        jobs.remove(id)
        _isProcessing.value = jobs.isNotEmpty()
    }
    
    fun hasJobs(): Boolean = jobs.isNotEmpty()
}
