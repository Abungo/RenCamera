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
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import java.nio.ByteBuffer

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

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        Log.i(TAG, "ProcessingService destroyed")
        serviceJob.cancel()
        super.onDestroy()
    }

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
                        if (job.config.debugImagesEnabled) rawDir.absolutePath else "", // master debug toggle
                        object : NativeEngine.ProgressListener {
                            override fun onProgress(step: String, percentage: Int) {
                                updateNotification(step, percentage)
                            }
                        }
                    )
                }

                saveDeferred?.await()
                val res = pipelineDeferred.await()
                
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
                            Frame ISOs: ${job.frameIsos.joinToString(", ")}
                            Frame Exposure Times (ms): ${job.frameExposures?.map { String.format("%.2f", it / 1_000_000.0) }?.joinToString(", ")}
                            Average Exposure Time: ${String.format("%.2f", averageExposureMs)} ms
                            
                        """.trimIndent()
                        logFile.appendText(details)
                    } catch (e: Exception) {
                        Log.e(TAG, "Failed to write capture parameters to pipeline log", e)
                    }
                }
                res
            }
            if (jpegBytes != null) {
                val uri = PhotoSaver.save(this, jpegBytes, filename)
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

    private fun runNativeEngine(
        nativeBurstHandle: Long,
        config: PipelineConfig,
        iso: Int,
        frameIsos: IntArray,
        debugDir: String,
        progressListener: NativeEngine.ProgressListener
    ): ByteArray? {
        return NativeEngine.processCopiedBurst(
            handle      = nativeBurstHandle,
            jpegQuality = config.jpegQuality,
            stageFlags  = config.toStageFlagArray(),
            nightMode   = config.nightMode,
            iso         = iso,
            frameIsos   = frameIsos,
            configParams = config.toFloatArray(),
            debugDir    = debugDir,
            listener    = progressListener
        )
    }
}
