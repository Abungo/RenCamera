package com.renskylab.camera

import android.app.*
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Color
import android.media.Image
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.exifinterface.media.ExifInterface
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import java.nio.ByteBuffer

/**
 * Foreground Service that handles the CPU/GPU-intensive image processing pipeline sequentially.
 * Running in a foreground service ensures that the OS does not terminate the process when the user
 * backgrounds the application during long-running JNI HDR+ computations.
 */
class ProcessingService : Service() {

    companion object {
        private const val TAG = "RenCamera/ProcessingSvc"
        private const val CHANNEL_ID = "processing_channel"
        private const val NOTIFICATION_ID = 1001
        
        const val ACTION_PROCESS = "com.renskylab.camera.ACTION_PROCESS"
        const val EXTRA_JOB_ID = "extra_job_id"
    }

    private val serviceJob = SupervisorJob()
    private val serviceScope = CoroutineScope(Dispatchers.Default + serviceJob)
    
    // Channel for sequential processing
    private val jobChannel = Channel<String>(Channel.UNLIMITED)

    /**
     * Initializes the service, creates the system notification channel, and starts the sequential worker
     * coroutine loop to process incoming job requests.
     */
    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "ProcessingService created")
        createNotificationChannel()
        startForegroundServiceNotification()
        
        // Start sequential worker
        serviceScope.launch {
            for (jobId in jobChannel) {
                processJobInternal(jobId)
            }
        }
    }

    /**
     * Handles service start intents. Reads the job ID from the intent extras and sends it to the
     * sequential job queue channel.
     */
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_PROCESS) {
            val jobId = intent.getStringExtra(EXTRA_JOB_ID)
            if (jobId != null) {
                serviceScope.launch {
                    jobChannel.send(jobId)
                }
            }
        }
        return START_NOT_STICKY
    }

    /**
     * Binds the service to an activity. Since this service operates as a start service without binding,
     * it returns null.
     */
    override fun onBind(intent: Intent?): IBinder? = null

    /**
     * Called when the service is destroyed. Cancels the coroutine job hierarchy to stop active execution.
     */
    override fun onDestroy() {
        Log.i(TAG, "ProcessingService destroyed")
        serviceJob.cancel()
        super.onDestroy()
    }

    /**
     * Starts the foreground service by displaying a sticky notification explaining the background processing state.
     */
    private fun startForegroundServiceNotification() {
        val intent = Intent(this, MainActivity::class.java)
        val pendingIntent = PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Processing Photo")
            .setContentText("Developing HDR+ photo in background...")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    /**
     * Configures the system notification channel for Android O and above.
     */
    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Photo Processing",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Shows notification when processing camera pictures"
                enableLights(false)
                enableVibration(false)
            }
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            manager.createNotificationChannel(channel)
        }
    }

    /**
     * Updates the persistent notification with the current pipeline execution step and progress percentage.
     *
     * @param step The active pipeline stage name.
     * @param percentage The completion progress (0 to 100).
     */
    private fun updateNotification(step: String, percentage: Int) {
        val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        
        val intent = Intent(this, MainActivity::class.java)
        val pendingIntent = PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        val displayStep = when (step) {
            "align" -> "Aligning frames"
            "fuse" -> "Fusing exposures"
            "debayer" -> "Demosaicing colors"
            "tonemap" -> "Applying tone mapping"
            "encode" -> "Encoding JPEG"
            "complete" -> "Processing complete"
            else -> "Processing step: $step"
        }

        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Processing Photo")
            .setContentText("$displayStep… ($percentage%)")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentIntent(pendingIntent)
            .setProgress(100, percentage, false)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()

        notificationManager.notify(NOTIFICATION_ID, notification)
    }

    /**
     * Retrieves the specified job parameters, runs JNI processing stages sequentially,
     * saves output, updates progress notifications, and stops service when no jobs remain.
     *
     * @param jobId The unique ID of the job to process.
     */
    private suspend fun processJobInternal(jobId: String) {
        val job = ProcessingManager.getJob(jobId) ?: return
        Log.i(TAG, "Starting JNI processing for job: $jobId")
        
        try {
            val timestamp = job.timestamp
            val filename = "REN_$timestamp.jpg"
            val rawDirName = "REN_$timestamp"
            val baseDir = externalMediaDirs.firstOrNull() ?: filesDir
            val rawDir = java.io.File(baseDir, rawDirName)
            if (!rawDir.exists()) {
                rawDir.mkdirs()
            }
            // Create subdirectories for intermediate stage outputs
            java.io.File(rawDir, "stage_1_fusion").mkdirs()
            java.io.File(rawDir, "stage_2_debayer").mkdirs()
            java.io.File(rawDir, "stage_3_tonemap").mkdirs()
            val jpegBytes = coroutineScope {
                val saveDeferred = if (job.config.debugImagesEnabled) {
                    async(Dispatchers.IO) {
                        Log.i(TAG, "Saving raw frames in background to ${rawDir.absolutePath}")
                        NativeEngine.saveRawBurst(job.nativeBurstHandle, rawDir.absolutePath, job.config.useRawCapture, job.config.debugRawDumps)
                    }
                } else {
                    null
                }

                val pipelineDeferred = async(Dispatchers.Default) {
                    runNativeEngine(
                        job.nativeBurstHandle,
                        job.config,
                        job.iso,
                        job.frameIsos,
                        job.frameExposures ?: LongArray(job.frameIsos.size),
                        job.frameNoiseProfiles ?: FloatArray(0),
                        job.digitalGain,
                        if (job.config.debugImagesEnabled) rawDir.absolutePath else "", // master debug toggle
                        object : NativeEngine.ProgressListener {
                            override fun onProgress(step: String, percentage: Int) {
                                updateNotification(step, percentage)
                                ProcessingManager.updateProgress(percentage)
                            }
                        }
                    )
                }

                saveDeferred?.await()
                val res = pipelineDeferred.await()
                
                // Write EXIF orientation tags to intermediate debug preview files if enabled
                if (job.config.debugImagesEnabled) {
                    try {
                        val exifOrientation = when (job.sensorOrientation) {
                            90 -> ExifInterface.ORIENTATION_ROTATE_90
                            180 -> ExifInterface.ORIENTATION_ROTATE_180
                            270 -> ExifInterface.ORIENTATION_ROTATE_270
                            else -> ExifInterface.ORIENTATION_NORMAL
                        }
                        val debugPaths = listOf("stage_1_fusion", "stage_2_debayer", "stage_3_tonemap")
                        for (dirName in debugPaths) {
                            val dir = java.io.File(rawDir, dirName)
                            if (dir.exists()) {
                                dir.listFiles()?.forEach { file ->
                                    if (file.name.endsWith(".jpg") || file.name.endsWith(".jpeg")) {
                                        try {
                                            val ex = ExifInterface(file.absolutePath)
                                            ex.setAttribute(ExifInterface.TAG_ORIENTATION, exifOrientation.toString())
                                            ex.saveAttributes()
                                        } catch (e: Exception) {
                                            Log.w(TAG, "Failed writing EXIF to debug file ${file.name}", e)
                                        }
                                    }
                                }
                            }
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, "Error writing debug image EXIF orientations", e)
                    }
                }

                // Append exact capture parameters to pipeline timing log
                if (job.config.debugImagesEnabled && res != null) {
                    try {
                        val logFile = java.io.File(rawDir, "pipeline_timing_log.txt")
                        val averageExposure = job.frameExposures?.average() ?: 0.0
                        val averageExposureMs = averageExposure / 1_000_000.0
                        val details = """
                            
                            === CAPTURE PARAMETERS ===
                            Night Mode: ${job.config.nightMode}
                            Use RAW: ${job.config.useRawCapture}
                            JPEG Quality: ${job.config.jpegQuality}
                            Target ISO: ${job.iso}
                            Frame ISOs: ${job.frameIsos.joinToString()}
                            Frame Exposure Times (ms): ${job.frameExposures?.map { String.format("%.2f", it / 1_000_000.0) }?.joinToString() ?: ""}
                            Average Exposure Time: ${String.format("%.2f", averageExposureMs)} ms
                            Digital Gain: ${String.format("%.2f", job.digitalGain)}x
                            
                        """.trimIndent()
                        logFile.appendText(details)
                    } catch (e: Exception) {
                        Log.e(TAG, "Failed to write details to log file", e)
                    }
                }
                res
            }
            
            if (jpegBytes != null) {
                val uri = PhotoSaver.save(this, jpegBytes, filename, job.sensorOrientation)
                if (uri != null) {
                    Log.i(TAG, "Job $jobId completed. Saved URI: $uri")
                    withContext(Dispatchers.Main) {
                        job.onSaved(uri)
                    }
                } else {
                    Log.e(TAG, "Job $jobId failed to save JPEG")
                    withContext(Dispatchers.Main) {
                        job.onError("Failed to save JPEG to media store")
                    }
                }
            } else {
                Log.e(TAG, "Job $jobId native engine returned null")
                withContext(Dispatchers.Main) {
                    job.onError("Native pipeline failed to process frames")
                }
            }
            ProcessingManager.removeJob(jobId)
            
            // If no more jobs are in queue, stop the service
            if (!ProcessingManager.hasJobs()) {
                Log.i(TAG, "All jobs complete. Stopping service.")
                stopSelf()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error processing job $jobId", e)
            withContext(Dispatchers.Main) {
                job.onError(e.message ?: "Unknown processing error")
            }
        } finally {
            // Free the memory allocated in C++ native heap immediately
            try {
                NativeEngine.freeBurst(job.nativeBurstHandle)
            } catch (e: Exception) {
                Log.e(TAG, "Failed to free native burst memory", e)
            }
            ProcessingManager.removeJob(jobId)
            
            // If no more jobs are in queue, stop the service
            if (!ProcessingManager.hasJobs()) {
                Log.i(TAG, "All jobs complete. Stopping service.")
                stopSelf()
            }
        }
    }

    /**
     * Configures tuning float array parameters and triggers JNI processing execution.
     *
     * @return Processed JPEG ByteArray, or null if native execution fails.
     */
    private fun runNativeEngine(
        nativeBurstHandle: Long,
        config: PipelineConfig,
        iso: Int,
        frameIsos: IntArray,
        frameExposures: LongArray,
        frameNoiseProfiles: FloatArray,
        digitalGain: Float,
        debugDir: String,
        progressListener: NativeEngine.ProgressListener
    ): ByteArray? {
        val mergedParams = config.toFloatArray().toMutableList().apply {
            add(digitalGain) // Index 15
        }.toFloatArray()

        return NativeEngine.processCopiedBurst(
            handle      = nativeBurstHandle,
            jpegQuality = config.jpegQuality,
            stageFlags  = config.toStageFlagArray(),
            nightMode   = config.nightMode,
            iso         = iso,
            frameIsos   = frameIsos,
            frameExposures = frameExposures,
            frameNoiseProfiles = frameNoiseProfiles,
            configParams = mergedParams,
            debugDir    = debugDir,
            listener    = progressListener
        )
    }
}
