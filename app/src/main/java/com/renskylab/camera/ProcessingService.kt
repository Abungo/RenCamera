package com.renskylab.camera

import android.app.*
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.Canvas
import android.graphics.Paint
import android.media.Image
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.exifinterface.media.ExifInterface
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import java.io.FileOutputStream
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
                        job,
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
                // Run portrait selfie segmentation immediately after native pipeline completes
                if (job.config.debugImagesEnabled && res != null) {
                    try {
                        val options = BitmapFactory.Options().apply {
                            inSampleSize = 4 // Decode at 1/4 size to execute quickly and fit heap limits
                        }
                        val bmp = BitmapFactory.decodeByteArray(res, 0, res.size, options)
                        if (bmp != null) {
                            Log.i(TAG, "Running Selfie Segmentation on fully fused output (${bmp.width}x${bmp.height})")
                            val segmenter = ImageSegmenterHelper(this@ProcessingService)
                            val segResult = segmenter.segmentImage(bmp)
                            if (segResult != null) {
                                // Validate if there is actually a person in the frame.
                                // Count subject pixels (any non-background class in DeepLabv3: 1..20)
                                var subjectPixelCount = 0
                                for (b in segResult.bytes) {
                                    if (b.toInt() and 0xFF in 1..20) {
                                        subjectPixelCount++
                                    }
                                }
                                val totalPixels = segResult.width * segResult.height
                                val subjectRatio = subjectPixelCount.toFloat() / totalPixels.toFloat()
                                Log.i(TAG, "Segmentation foreground ratio: ${String.format("%.4f", subjectRatio)}")

                                if (subjectRatio < 0.005f) {
                                    Log.i(TAG, "No foreground objects detected (foreground ratio < 0.5%). Skipping overlay debug files generation.")
                                    segmenter.close()
                                    bmp.recycle()
                                } else {
                                    // ==========================================
                                    // LEVEL 1: Main Subject Segmentation (Foreground vs Background)
                                    // ==========================================
                                    val subjectMaskBmp = Bitmap.createBitmap(segResult.width, segResult.height, Bitmap.Config.ARGB_8888)
                                val subjectMaskPixels = IntArray(segResult.width * segResult.height)
                                for (i in subjectMaskPixels.indices) {
                                    val classVal = segResult.bytes[i].toInt() and 0xFF
                                    // Any detected object is foreground
                                    val isSubject = (classVal in 1..20)
                                    val grayVal = if (isSubject) 255 else 0
                                    subjectMaskPixels[i] = Color.rgb(grayVal, grayVal, grayVal)
                                }
                                subjectMaskBmp.setPixels(subjectMaskPixels, 0, segResult.width, 0, 0, segResult.width, segResult.height)
                                
                                val subjectMaskFile = java.io.File(rawDir, "segmentation_subject_mask.jpg")
                                FileOutputStream(subjectMaskFile).use { out ->
                                    subjectMaskBmp.compress(Bitmap.CompressFormat.JPEG, 90, out)
                                }
                                subjectMaskBmp.recycle()

                                // Fused visualization for Level 1 (Green overlay for entire subject)
                                val subjectFusedBmp = bmp.copy(Bitmap.Config.ARGB_8888, true)
                                val subjectCanvas = Canvas(subjectFusedBmp)
                                val subjectPaint = Paint().apply {
                                    color = Color.argb(120, 0, 255, 0) // Semi-transparent green
                                    style = Paint.Style.FILL
                                }
                                for (y in 0 until segResult.height) {
                                    for (x in 0 until segResult.width) {
                                        val classVal = segResult.bytes[y * segResult.width + x].toInt() and 0xFF
                                        if (classVal in 1..20) {
                                            subjectCanvas.drawPoint(x.toFloat(), y.toFloat(), subjectPaint)
                                        }
                                    }
                                }
                                
                                // Label for Subject Fused Mask
                                val subjectLabelPaint = Paint().apply {
                                    color = Color.GREEN
                                    textSize = (segResult.height / 32f).coerceAtLeast(16f)
                                    isAntiAlias = true
                                    style = Paint.Style.FILL
                                    typeface = android.graphics.Typeface.DEFAULT_BOLD
                                }
                                val subjectText = "OBJECTS & PETS (Foreground)"
                                val subjectTextBounds = android.graphics.Rect()
                                subjectLabelPaint.getTextBounds(subjectText, 0, subjectText.length, subjectTextBounds)
                                val subjectPadding = 12f
                                val subjectStartX = 24f
                                val subjectStartY = 24f + subjectTextBounds.height()
                                val subjectBgPaint = Paint().apply {
                                    color = Color.BLACK
                                    alpha = 180
                                    style = Paint.Style.FILL
                                }
                                subjectCanvas.drawRect(
                                    subjectStartX - subjectPadding,
                                    subjectStartY - subjectTextBounds.height() - subjectPadding,
                                    subjectStartX + subjectTextBounds.width() + subjectPadding,
                                    subjectStartY + subjectPadding,
                                    subjectBgPaint
                                )
                                subjectCanvas.drawText(subjectText, subjectStartX, subjectStartY, subjectLabelPaint)

                                val subjectFusedFile = java.io.File(rawDir, "segmentation_subject_fused.jpg")
                                FileOutputStream(subjectFusedFile).use { out ->
                                    subjectFusedBmp.compress(Bitmap.CompressFormat.JPEG, 90, out)
                                }
                                subjectFusedBmp.recycle()

                                // ==========================================
                                // LEVEL 2: Finer Multiclass Segmentation (DeepLabv3 Pascal VOC)
                                // ==========================================
                                // Save Category Mask as an Image representation where each class gets a custom pixel value for diagnostics
                                val maskBmp = Bitmap.createBitmap(segResult.width, segResult.height, Bitmap.Config.ARGB_8888)
                                val maskPixels = IntArray(segResult.width * segResult.height)
                                for (i in maskPixels.indices) {
                                    val classVal = segResult.bytes[i].toInt() and 0xFF
                                    // Scale to 255 grayscale range dynamically
                                    val grayVal = (classVal * 12).coerceAtMost(255)
                                    maskPixels[i] = Color.rgb(grayVal, grayVal, grayVal)
                                }
                                maskBmp.setPixels(maskPixels, 0, segResult.width, 0, 0, segResult.width, segResult.height)
                                
                                val maskFile = java.io.File(rawDir, "segmentation_mask.jpg")
                                FileOutputStream(maskFile).use { out ->
                                    maskBmp.compress(Bitmap.CompressFormat.JPEG, 90, out)
                                }
                                maskBmp.recycle()

                                // Create fused mask visualization (overlay mask directly onto the color image with distinct translucent colors)
                                val fusedBmp = bmp.copy(Bitmap.Config.ARGB_8888, true)
                                val canvas = Canvas(fusedBmp)

                                // PASCAL VOC 20 Colors mapping
                                val paints = Array(21) { idx ->
                                    Paint().apply {
                                        color = when (idx) {
                                            15 -> Color.argb(120, 255, 0, 0)      // Person -> Red
                                            8 -> Color.argb(120, 0, 0, 255)       // Cat -> Blue
                                            12 -> Color.argb(120, 0, 255, 255)    // Dog -> Cyan
                                            5 -> Color.argb(120, 255, 255, 0)     // Bottle -> Yellow
                                            9 -> Color.argb(120, 255, 0, 255)     // Chair -> Magenta
                                            20 -> Color.argb(120, 255, 128, 0)    // TV/Monitor -> Orange
                                            else -> {
                                                // Generate distinct values for other object categories
                                                val r = (idx * 37) % 256
                                                val g = (idx * 59) % 256
                                                val b = (idx * 83) % 256
                                                Color.argb(100, r, g, b)
                                            }
                                        }
                                        style = Paint.Style.FILL
                                    }
                                }

                                // Overlay category mask pixels
                                for (y in 0 until segResult.height) {
                                    for (x in 0 until segResult.width) {
                                        val classVal = segResult.bytes[y * segResult.width + x].toInt() and 0xFF
                                        if (classVal in 1..20) {
                                            canvas.drawPoint(x.toFloat(), y.toFloat(), paints[classVal])
                                        }
                                    }
                                }

                                // Draw labels at the top left corner in a legend layout
                                val labelSize = (segResult.height / 38f).coerceAtLeast(14f)
                                // We list key labels of interest on the overlay
                                val legendItems = listOf(
                                    Pair("PERSON (Red)", Color.RED),
                                    Pair("CAT (Blue)", Color.BLUE),
                                    Pair("DOG (Cyan)", Color.CYAN),
                                    Pair("BOTTLE (Yellow)", Color.YELLOW),
                                    Pair("CHAIR (Magenta)", Color.MAGENTA),
                                    Pair("TV (Orange)", Color.rgb(255, 128, 0))
                                )

                                var currentY = 24f
                                for (item in legendItems) {
                                    val labelPaint = Paint().apply {
                                        color = item.second
                                        textSize = labelSize
                                        isAntiAlias = true
                                        style = Paint.Style.FILL
                                        typeface = android.graphics.Typeface.DEFAULT_BOLD
                                    }
                                    val text = item.first
                                    val textBounds = android.graphics.Rect()
                                    labelPaint.getTextBounds(text, 0, text.length, textBounds)

                                    val padding = 6f
                                    val startX = 24f
                                    currentY += textBounds.height() + padding * 2

                                    val bgPaint = Paint().apply {
                                        color = Color.BLACK
                                        alpha = 180
                                        style = Paint.Style.FILL
                                    }
                                    canvas.drawRect(
                                        startX - padding,
                                        currentY - textBounds.height() - padding,
                                        startX + textBounds.width() + padding,
                                        currentY + padding,
                                        bgPaint
                                    )
                                    canvas.drawText(text, startX, currentY, labelPaint)
                                    currentY += padding
                                }

                                val fusedFile = java.io.File(rawDir, "segmentation_fused_mask.jpg")
                                FileOutputStream(fusedFile).use { out ->
                                    fusedBmp.compress(Bitmap.CompressFormat.JPEG, 90, out)
                                }
                                fusedBmp.recycle()
                                    segmenter.close()
                                    Log.i(TAG, "Dumping multi-class segmentation mask debug images completed.")
                                }
                            }
                            bmp.recycle()
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, "Failed running image portrait segmenter callback", e)
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
        job: ProcessingJob,
        debugDir: String,
        progressListener: NativeEngine.ProgressListener
    ): ByteArray? {
        val mergedParams = job.config.toFloatArray().toMutableList().apply {
            add(job.digitalGain) // Index 15
            add(job.appliedEvCompensation) // Index 16

            // Dynamic AWB Gains (Indices 17-19)
            val awb = job.awbGains ?: floatArrayOf(2.1f, 1.0f, 1.9f)
            add(awb.getOrNull(0) ?: 2.1f)
            add(awb.getOrNull(1) ?: 1.0f)
            add(awb.getOrNull(2) ?: 1.9f)

            // Dynamic CCM Matrix (Indices 20-28)
            val ccm = job.colorCorrectionMatrix ?: floatArrayOf(
                1f, 0f, 0f,
                0f, 1f, 0f,
                0f, 0f, 1f
            )
            for (v in ccm) {
                add(v)
            }

            // Dynamic CFA Arrangement Pattern (Index 29)
            add(job.colorFilterArrangement.toFloat())

            // Dynamic Black Level and White Level (Indices 30-31)
            add(job.blackLevel)
            add(job.whiteLevel)
        }.toFloatArray()

        return NativeEngine.processCopiedBurst(
            handle      = job.nativeBurstHandle,
            jpegQuality = job.config.jpegQuality,
            stageFlags  = job.config.toStageFlagArray(),
            nightMode   = job.config.nightMode,
            iso         = job.iso,
            frameIsos   = job.frameIsos,
            frameExposures = job.frameExposures ?: LongArray(job.frameIsos.size),
            frameNoiseProfiles = job.frameNoiseProfiles ?: FloatArray(0),
            configParams = mergedParams,
            debugDir    = debugDir,
            listener    = progressListener
        )
    }
}
