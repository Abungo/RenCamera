package com.renskylab.camera

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.*
import android.media.Image
import android.media.ImageReader
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.nio.ByteBuffer
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

private const val TAG = "RenCamera/CameraCtrl"
private const val RING_BUFFER_SIZE = 15

/**
 * Camera2-based controller.
 *
 * Manages the full capture pipeline:
 *   1. Open [CameraDevice] for the primary back-facing camera.
 *   2. Create a dual-output [CameraCaptureSession]:
 *        - Low-res surface → live viewfinder preview.
 *        - Max-res [ImageReader] (YUV_420_888) → ring buffer.
 *   3. Apply –1.5 EV exposure compensation on every repeating request.
 *   4. On shutter press: freeze 12 frames, invoke native engine, close images.
 */
@SuppressLint("MissingPermission")
class CameraController(
    private val context: Context,
    private val scope: CoroutineScope,
) {
    // ── Public state ───────────────────────────────────────────────────────────
    private val _isProcessing = MutableStateFlow(false)
    val isProcessing: StateFlow<Boolean> = _isProcessing
    var isNightMode: Boolean = false

    // ── Camera hardware ────────────────────────────────────────────────────────
    private val manager by lazy {
        context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    }
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null

    // ── Background thread for camera callbacks ─────────────────────────────────
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null

    // ── Ring buffer ────────────────────────────────────────────────────────────
    private val ringBuffer = FrameRingBuffer(RING_BUFFER_SIZE)

    private val pendingImages = java.util.concurrent.ConcurrentHashMap<Long, Image>()
    private val pendingMetadata = java.util.concurrent.ConcurrentHashMap<Long, TotalCaptureResult>()

    private fun tryMatchAndPushFrame(timestamp: Long) {
        val img = pendingImages[timestamp]
        val meta = pendingMetadata[timestamp]
        if (img != null && meta != null) {
            pendingImages.remove(timestamp)
            pendingMetadata.remove(timestamp)
            
            if (_isProcessing.value && !isCapturingPsl) {
                img.close()
            } else {
                val iso = meta.get(CaptureResult.SENSOR_SENSITIVITY) ?: 400
                val expTime = meta.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: 30_000_000L
                ringBuffer.push(CapturedFrame(img, iso, expTime))
            }
        }
        
        // Prevent memory leak by cleaning older frames aggressively (max 3 unmatched images)
        if (pendingImages.size > 3) {
            val sortedKeys = pendingImages.keys.sorted()
            val toEvictCount = pendingImages.size - 2
            for (i in 0 until toEvictCount) {
                val k = sortedKeys.getOrNull(i) ?: break
                pendingImages.remove(k)?.close()
                pendingMetadata.remove(k)
            }
        }
    }

    // ── Remembered preview surface (needed to restart repeating request) ───────
    @Volatile private var lastPreviewSurface: Surface? = null
    @Volatile private var lastTexture: android.graphics.SurfaceTexture? = null
    @Volatile private var useRawCapture: Boolean = false
    @Volatile private var currentIso: Int = 400
    @Volatile private var isCapturingPsl = false
    private val rawBufferPool = ArrayList<ByteBuffer>()

    // ── Semaphore to guard cameraDevice opening (Camera2 requirement) ──────────
    private val cameraOpenLock = Semaphore(1)

    // ─────────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────────

    fun startCamera(texture: android.graphics.SurfaceTexture) {
        lastTexture = texture
        startBackgroundThread()
        scope.launch(Dispatchers.IO) {
            try {
                openCamera(texture)
            } catch (e: Exception) {
                Log.e(TAG, "startCamera failed", e)
            }
        }
    }

    fun stopCamera() {
        lastPreviewSurface = null
        lastTexture = null
        ringBuffer.flush()
        pendingImages.values.forEach { runCatching { it.close() } }
        pendingImages.clear()
        pendingMetadata.clear()
        captureSession?.close()
        captureSession = null
        cameraDevice?.close()
        cameraDevice = null
        imageReader?.close()
        imageReader = null
        stopBackgroundThread()
    }

    fun setNightModeEnabled(enabled: Boolean) {
        if (isNightMode == enabled) return
        isNightMode = enabled
        Log.i(TAG, "setNightModeEnabled: $enabled")

        val session = captureSession
        val surface = lastPreviewSurface
        if (session != null && surface != null) {
            scope.launch(Dispatchers.IO) {
                runCatching { startRepeatingRequest(session, surface) }
            }
        }
    }

    fun setRawCaptureEnabled(enabled: Boolean) {
        if (useRawCapture == enabled) return
        useRawCapture = enabled
        Log.i(TAG, "setRawCaptureEnabled: $enabled")

        val texture = lastTexture
        if (texture != null) {
            scope.launch(Dispatchers.IO) {
                try {
                    if (!cameraOpenLock.tryAcquire(2500, TimeUnit.MILLISECONDS)) {
                        Log.e(TAG, "Timed out waiting to recreate camera session"); return@launch
                    }
                    try {
                        // Close current capture session and ImageReader
                        captureSession?.close()
                        captureSession = null
                        imageReader?.close()
                        imageReader = null

                        // Flush all acquired images to prevent hardware buffer slot leaks
                        ringBuffer.flush()
                        pendingImages.values.forEach { runCatching { it.close() } }
                        pendingImages.clear()
                        pendingMetadata.clear()
                        
                        // Recreate ImageReader and capture session using the new format
                        val cameraId = pickBackCamera() ?: return@launch
                        val characteristics = manager.getCameraCharacteristics(cameraId)
                        
                        var format = ImageFormat.YUV_420_888
                        if (useRawCapture) {
                            val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                            val rawSizes = map?.getOutputSizes(ImageFormat.RAW_SENSOR)
                            if (!rawSizes.isNullOrEmpty()) {
                                format = ImageFormat.RAW_SENSOR
                            } else {
                                Log.w(TAG, "RAW_SENSOR not supported. Falling back to YUV.")
                            }
                        }

                        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                        val sizes = map?.getOutputSizes(format)
                            ?.sortedByDescending { it.width * it.height }
                            ?: emptyList()
                        val captureSize = sizes.firstOrNull() ?: return@launch

                        imageReader = ImageReader.newInstance(
                            captureSize.width, captureSize.height,
                            format,
                            RING_BUFFER_SIZE + 15
                        ).also { reader ->
                            reader.setOnImageAvailableListener({ r ->
                                val img = r.acquireLatestImage() ?: return@setOnImageAvailableListener
                                pendingImages[img.timestamp] = img
                                tryMatchAndPushFrame(img.timestamp)
                            }, cameraHandler)
                        }

                        val device = cameraDevice ?: return@launch
                        val previewSurface = lastPreviewSurface ?: return@launch
                        val targets = listOf(previewSurface, imageReader!!.surface)
                        device.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
                            override fun onConfigured(session: CameraCaptureSession) {
                                captureSession = session
                                startRepeatingRequest(session, previewSurface)
                            }
                            override fun onConfigureFailed(session: CameraCaptureSession) {
                                Log.e(TAG, "Recreate CaptureSession configure failed")
                            }
                        }, cameraHandler)
                    } finally {
                        cameraOpenLock.release()
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to switch capture format", e)
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Open camera + create session
    // ─────────────────────────────────────────────────────────────────────────

    private fun openCamera(texture: android.graphics.SurfaceTexture) {
        val cameraId = pickBackCamera() ?: run {
            Log.e(TAG, "No back camera found"); return
        }
        val characteristics = manager.getCameraCharacteristics(cameraId)

        var format = ImageFormat.YUV_420_888
        if (useRawCapture) {
            val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val rawSizes = map?.getOutputSizes(ImageFormat.RAW_SENSOR)
            if (!rawSizes.isNullOrEmpty()) {
                format = ImageFormat.RAW_SENSOR
            } else {
                Log.w(TAG, "RAW_SENSOR format not supported! Falling back to YUV.")
            }
        }

        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val sizes = map?.getOutputSizes(format)
            ?.sortedByDescending { it.width * it.height }
            ?: emptyList()
        val captureSize = sizes.firstOrNull() ?: run {
            Log.e(TAG, "No capture sizes found for format $format"); return
        }

        Log.i(TAG, "Using format $format with capture size: ${captureSize.width}×${captureSize.height}")

        val previewSizes = characteristics
            .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes<android.graphics.SurfaceTexture>(android.graphics.SurfaceTexture::class.java)
            ?.toList()
            ?: emptyList()

        val captureRatio = captureSize.width.toDouble() / captureSize.height.toDouble()
        // Select preview size with same aspect ratio, capped at 1080p width
        val optimalPreviewSize = previewSizes
            .filter { Math.abs((it.width.toDouble() / it.height.toDouble()) - captureRatio) < 0.05 }
            .sortedByDescending { it.width * it.height }
            .firstOrNull { it.width <= 1920 }
            ?: previewSizes.firstOrNull()
            ?: captureSize

        Log.i(TAG, "Selected preview size: ${optimalPreviewSize.width}x${optimalPreviewSize.height}")
        texture.setDefaultBufferSize(optimalPreviewSize.width, optimalPreviewSize.height)
        val previewSurface = Surface(texture)
        lastPreviewSurface = previewSurface

        imageReader = ImageReader.newInstance(
            captureSize.width, captureSize.height,
            format,
            RING_BUFFER_SIZE + 15 // extra slots to avoid stalls
        ).also { reader ->
            reader.setOnImageAvailableListener({ r ->
                val img = r.acquireLatestImage() ?: return@setOnImageAvailableListener
                pendingImages[img.timestamp] = img
                tryMatchAndPushFrame(img.timestamp)
            }, cameraHandler)
        }

        if (!cameraOpenLock.tryAcquire(2500, TimeUnit.MILLISECONDS)) {
            Log.e(TAG, "Timed out waiting to open camera"); return
        }

        manager.openCamera(cameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                cameraOpenLock.release()
                cameraDevice = device
                createCaptureSession(device, previewSurface)
            }
            override fun onDisconnected(device: CameraDevice) {
                cameraOpenLock.release()
                device.close()
                cameraDevice = null
            }
            override fun onError(device: CameraDevice, error: Int) {
                cameraOpenLock.release()
                device.close()
                cameraDevice = null
                Log.e(TAG, "Camera error $error")
            }
        }, cameraHandler)
    }

    private fun createCaptureSession(device: CameraDevice, previewSurface: Surface) {
        val targets = listOf(previewSurface, imageReader!!.surface)
        device.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(session: CameraCaptureSession) {
                captureSession = session
                startRepeatingRequest(session, previewSurface)
            }
            override fun onConfigureFailed(session: CameraCaptureSession) {
                Log.e(TAG, "CaptureSession configure failed")
            }
        }, cameraHandler)
    }

    private fun startRepeatingRequest(session: CameraCaptureSession, previewSurface: Surface) {
        val characteristics = manager.getCameraCharacteristics(
            pickBackCamera() ?: return
        )

        // Compute EV compensation steps
        val aeRange = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        val aeStep  = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)
        val stepSize = aeStep?.toFloat() ?: (1f / 3f)
        val bias = if (isNightMode) 1.5f else -1.5f
        val evSteps = (bias / stepSize).toInt()
            .coerceIn(aeRange?.lower ?: -6, aeRange?.upper ?: 0)

        val requestBuilder = cameraDevice!!
            .createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                addTarget(previewSurface)
                addTarget(imageReader!!.surface)
                set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, evSteps)
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)

                // Select target FPS range with lowest minimum to support longer exposure times
                val fpsRanges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                val bestRange = fpsRanges?.filter { it.upper >= 30 }
                    ?.minByOrNull { it.lower }
                if (bestRange != null) {
                    set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, bestRange)
                    Log.i(TAG, "Selected AE FPS range for preview: $bestRange")
                }
            }

        session.setRepeatingRequest(requestBuilder.build(), object : CameraCaptureSession.CaptureCallback() {
            override fun onCaptureCompleted(
                session: CameraCaptureSession,
                request: CaptureRequest,
                result: TotalCaptureResult
            ) {
                val iso = result.get(CaptureResult.SENSOR_SENSITIVITY)
                if (iso != null) {
                    currentIso = iso
                }
                val ts = result.get(CaptureResult.SENSOR_TIMESTAMP)
                if (ts != null) {
                    pendingMetadata[ts] = result
                    tryMatchAndPushFrame(ts)
                }
            }
        }, cameraHandler)
        Log.i(TAG, "Repeating request started (EV steps = $evSteps, isNightMode = $isNightMode)")
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Burst capture
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Freeze the current ring buffer, copy YUV buffers off-heap, and launch the background processing service.
     */
    fun captureBurst(
        config: PipelineConfig = PipelineConfig(),
        onDispatched: () -> Unit,
        onSaved: (android.net.Uri) -> Unit,
        onError: (String) -> Unit,
    ) {
        if (_isProcessing.value) return
        _isProcessing.value = true

        scope.launch(Dispatchers.Default) {
            // Stop new frames from racing into the buffer while we drain it.
            val session = captureSession
            val surface = lastPreviewSurface
            runCatching { session?.stopRepeating() }

            val isNight = config.nightMode
            val burst = if (isNight) {
                isCapturingPsl = true
                ringBuffer.flush() // Clear preview frames

                // Create a manual still capture burst to collect massive light
                val requests = List(15) {
                    cameraDevice!!.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE).apply {
                        addTarget(imageReader!!.surface)
                        set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                        // Request 125ms per-frame exposure time (1/8s)
                        set(CaptureRequest.SENSOR_EXPOSURE_TIME, 125_000_000L)
                        // Target a high ISO range [1600, 3200]
                        set(CaptureRequest.SENSOR_SENSITIVITY, currentIso.coerceIn(1600, 3200))
                        set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                    }.build()
                }

                // Temporary CaptureCallback to capture manual burst metadata
                val burstMetadataListener = object : CameraCaptureSession.CaptureCallback() {
                    override fun onCaptureCompleted(
                        session: CameraCaptureSession,
                        request: CaptureRequest,
                        result: TotalCaptureResult
                    ) {
                        val ts = result.get(CaptureResult.SENSOR_TIMESTAMP)
                        if (ts != null) {
                            pendingMetadata[ts] = result
                            tryMatchAndPushFrame(ts)
                        }
                    }
                }

                if (session != null) {
                    session.captureBurst(requests, burstMetadataListener, cameraHandler)
                } else {
                    isCapturingPsl = false
                    _isProcessing.value = false
                    withContext(Dispatchers.Main) {
                        onDispatched()
                        onError("Capture session is inactive")
                    }
                    return@launch
                }

                // Wait up to 6 seconds for all 15 long-exposure frames to arrive
                val timeoutMs = 6000L
                val startTime = System.currentTimeMillis()
                while (ringBuffer.size < 15 && (System.currentTimeMillis() - startTime) < timeoutMs) {
                    delay(50)
                }

                isCapturingPsl = false
                val pslFrames = ringBuffer.snapshot()
                if (pslFrames.size < 15) {
                    Log.w(TAG, "PSL capture timed out, got ${pslFrames.size}/15 frames")
                } else {
                    Log.i(TAG, "Successfully captured all 15 PSL frames!")
                }
                pslFrames
            } else {
                val fullDrained = ringBuffer.snapshot()
                if (fullDrained.isEmpty()) {
                    _isProcessing.value = false
                    if (session != null && surface != null) {
                        runCatching { startRepeatingRequest(session, surface) }
                    }
                    withContext(Dispatchers.Main) {
                        onDispatched()
                        onError("Ring buffer is empty — point camera at subject")
                    }
                    return@launch
                }

                val needed = 12
                if (fullDrained.size > needed) {
                    val discard = fullDrained.size - needed
                    fullDrained.subList(0, discard).forEach { runCatching { it.image.close() } }
                    fullDrained.subList(discard, fullDrained.size)
                } else {
                    fullDrained
                }
            }

            // Extract array parameters for copyBurst
            val count = burst.size
            val isRaw = burst[0].image.format == ImageFormat.RAW_SENSOR
            Log.i(TAG, "Captured burst format: ${burst[0].image.format} (isRaw = $isRaw)")

            val yPlanes = arrayOfNulls<ByteBuffer>(count)
            val uPlanes = arrayOfNulls<ByteBuffer>(count)
            val vPlanes = arrayOfNulls<ByteBuffer>(count)
            val yStrides = IntArray(count)
            val uvStrides = IntArray(count)
            val uvPixelStrides = IntArray(count)
            val width = burst[0].image.width
            val height = burst[0].image.height
            val frameIsos = IntArray(count)

            burst.forEachIndexed { idx, frame ->
                val planes = frame.image.planes
                
                yStrides[idx] = planes[0].rowStride
                Log.i(TAG, "Frame $idx: format=${frame.image.format}, capacity=${planes[0].buffer.capacity()}, stride=${planes[0].rowStride}, w=${frame.image.width}, h=${frame.image.height}")
                
                if (isRaw) {
                    // Qualcomm RAW buffers may not be virtual-address mapable by user-space JNI.
                    // Copying to a JVM direct ByteBuffer guarantees a valid address in GetDirectBufferAddress.
                    val rawBuf = planes[0].buffer
                    val capacity = rawBuf.remaining()
                    
                    // Re-use or allocate buffer in the pool
                    synchronized(rawBufferPool) {
                        while (rawBufferPool.size <= idx) {
                            rawBufferPool.add(ByteBuffer.allocateDirect(capacity))
                        }
                        if (rawBufferPool[idx].capacity() < capacity) {
                            rawBufferPool[idx] = ByteBuffer.allocateDirect(capacity)
                        }
                    }
                    val directBuf = rawBufferPool[idx]
                    directBuf.clear()
                    
                    val oldPos = rawBuf.position()
                    directBuf.put(rawBuf)
                    rawBuf.position(oldPos) // restore position
                    directBuf.flip()
                    
                    yPlanes[idx] = directBuf

                    // Create dummy single-byte buffers to avoid nulls in C++ JNI bridge
                    uPlanes[idx] = ByteBuffer.allocateDirect(1)
                    vPlanes[idx] = ByteBuffer.allocateDirect(1)
                    uvStrides[idx] = 0
                    uvPixelStrides[idx] = 0
                } else {
                    yPlanes[idx] = planes[0].buffer
                    uPlanes[idx] = planes[1].buffer
                    vPlanes[idx] = planes[2].buffer
                    uvStrides[idx] = planes[1].rowStride
                    uvPixelStrides[idx] = planes[1].pixelStride
                }
                frameIsos[idx] = frame.iso
            }

            var nativeHandle: Long = 0
            try {
                nativeHandle = NativeEngine.copyBurst(
                    count = count,
                    yPlanes = yPlanes.requireNoNulls(),
                    uPlanes = uPlanes.requireNoNulls(),
                    vPlanes = vPlanes.requireNoNulls(),
                    yRowStrides = yStrides,
                    uvRowStrides = uvStrides,
                    uvPixelStrides = uvPixelStrides,
                    width = width,
                    height = height
                )
            } catch (e: Exception) {
                Log.e(TAG, "Failed to copy burst native memory", e)
            } finally {
                // IMMEDIATELY close all hardware Images to release Camera2 slots
                burst.forEach { runCatching { it.image.close() } }
            }

            if (nativeHandle == 0L) {
                _isProcessing.value = false
                if (session != null && surface != null) {
                    runCatching { startRepeatingRequest(session, surface) }
                }
                withContext(Dispatchers.Main) {
                    onDispatched()
                    onError("Failed to copy frame buffers to native memory")
                }
                return@launch
            }

            // Create a unique job ID and delegate to ProcessingService
            val timestamp = System.currentTimeMillis()
            val jobId = "job_$timestamp"
            
            val job = ProcessingJob(
                id = jobId,
                timestamp = timestamp,
                iso = currentIso,
                frameIsos = frameIsos,
                nativeBurstHandle = nativeHandle,
                config = config.copy(useRawCapture = isRaw),
                onSaved = onSaved,
                onError = onError
            )
            
            ProcessingManager.addJob(job)
            
            // Start the foreground service
            val serviceIntent = Intent(context, ProcessingService::class.java).apply {
                action = ProcessingService.ACTION_PROCESS
                putExtra(ProcessingService.EXTRA_JOB_ID, jobId)
            }
            
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    context.startForegroundService(serviceIntent)
                } else {
                    context.startService(serviceIntent)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start ProcessingService", e)
            }

            // Clear camera controller processing state immediately and restart preview
            _isProcessing.value = false
            if (session != null && surface != null) {
                runCatching { startRepeatingRequest(session, surface) }
            }
            
            // Notify UI that the capture and dispatch is complete
            withContext(Dispatchers.Main) {
                onDispatched()
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Native engine call — marshals Image planes into JNI-friendly arrays
    // ─────────────────────────────────────────────────────────────────────────

    // ─────────────────────────────────────────────────────────────────────────
    // Helpers
    // ─────────────────────────────────────────────────────────────────────────

    private fun pickBackCamera(): String? {
        val backCameras = manager.cameraIdList.filter { id ->
            manager.getCameraCharacteristics(id)
                .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        }
        
        // If we want RAW capture, look for a back camera that supports the RAW capability
        if (useRawCapture) {
            val rawCamera = backCameras.firstOrNull { id ->
                val characteristics = manager.getCameraCharacteristics(id)
                val capabilities = characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
                capabilities?.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_RAW) == true
            }
            if (rawCamera != null) {
                Log.i(TAG, "Selected camera $rawCamera because it explicitly supports RAW")
                return rawCamera
            }
        }
        
        val defaultCamera = backCameras.firstOrNull()
        Log.i(TAG, "Selected default camera $defaultCamera (RAW requested=$useRawCapture)")
        return defaultCamera
    }

    private fun startBackgroundThread() {
        cameraThread = HandlerThread("CameraBackground").also {
            it.start()
            cameraHandler = Handler(it.looper)
        }
    }

    private fun stopBackgroundThread() {
        cameraThread?.quitSafely()
        cameraThread?.join()
        cameraThread  = null
        cameraHandler = null
    }
}
