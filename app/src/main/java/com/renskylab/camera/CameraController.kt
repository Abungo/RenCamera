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
import android.view.OrientationEventListener
import java.nio.ByteBuffer
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit
import android.hardware.camera2.params.LensShadingMap

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
    private val _captureProgress = MutableStateFlow(0f)
    val captureProgress: StateFlow<Float> = _captureProgress
    var isNightMode: Boolean = false
    private var activeExposureBias: Float = -0.5f

    // ── Camera hardware ────────────────────────────────────────────────────────
    private val manager by lazy {
        context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    }
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    private var orientationListener: OrientationEventListener? = null
    @Volatile private var deviceOrientation: Int = 0
    var isFrontFacing: Boolean = false
        private set

    // ── Background thread for camera callbacks ─────────────────────────────────
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null

    // ── Ring buffer ────────────────────────────────────────────────────────────
    private val ringBuffer = FrameRingBuffer(RING_BUFFER_SIZE)

    private val pendingImages = java.util.concurrent.ConcurrentHashMap<Long, Image>()
    private val pendingMetadata = java.util.concurrent.ConcurrentHashMap<Long, TotalCaptureResult>()

    private val burstTimestamps = java.util.concurrent.ConcurrentHashMap.newKeySet<Long>()

    @Volatile
    private var lastViewfinderAwbGains: FloatArray? = null
    @Volatile
    private var lastViewfinderCcm: FloatArray? = null
    @Volatile
    private var lastViewfinderLsc: FloatArray? = null // 4-channel R/Gr/Gb/B gain map, row-major
    @Volatile
    private var lastViewfinderLscW: Int = 0
    @Volatile
    private var lastViewfinderLscH: Int = 0

    /**
     * Attempts to pair an incoming [Image] buffer with its matching [TotalCaptureResult] metadata.
     * When both become available for a given hardware sensor timestamp, they are packaged into a
     * [CapturedFrame] and pushed to the FIFO ring buffer. Also implements leak prevention on unmatched buffers.
     *
     * @param timestamp The hardware sensor timestamp of the frame.
     */
    private fun tryMatchAndPushFrame(timestamp: Long) {
        val img = pendingImages[timestamp]
        val meta = pendingMetadata[timestamp]
        if (img != null && meta != null) {
            pendingImages.remove(timestamp)
            pendingMetadata.remove(timestamp)
            
            if (isCapturingPsl && !burstTimestamps.contains(timestamp)) {
                img.close()
                return
            }
            
            if (_isProcessing.value && !isCapturingPsl) {
                img.close()
            } else {
                val iso = meta.get(CaptureResult.SENSOR_SENSITIVITY) ?: 400
                val expTime = meta.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: 30_000_000L
                
                // Run preview frame histogram analysis periodically (every 10th frame) to dynamically adapt AE EV
                if (!isCapturingPsl && analysisFrameCount++ % 10 == 0) {
                    val whiteLvl = cameraWhiteLevel
                    val samples = LumaHistogramAnalyzer.extractLumaSamples(img, whiteLvl)
                    scope.launch(Dispatchers.Default) {
                        val result = LumaHistogramAnalyzer.analyzeSamples(samples)
                        activeDynamicEv = result.targetEv
                        Log.i(TAG, "Luma Analyzer: shadow=${result.shadowLuma}, highlight=${result.highlightLuma}, DR stops=${result.drStops}, targetEv=${result.targetEv}")
                        
                        // Dynamically update the repeating preview request exposure bias if needed
                        val session = captureSession
                        val surface = lastPreviewSurface
                        if (session != null && surface != null) {
                            val diff = Math.abs(activeDynamicEv - appliedEv)
                            if (diff >= 0.33f) {
                                runCatching { startRepeatingRequest(session, surface) }
                            }
                        }
                    }
                }

                val noiseProfile = getNoiseProfileForFrame(meta, iso)
                ringBuffer.push(CapturedFrame(img, iso, expTime, noiseProfile, meta))
            }
        }
        
        // Prevent memory leak by cleaning older frames aggressively (max 3 unmatched images)
        if (pendingImages.size > 3) {
            val sortedKeys = pendingImages.keys.sorted()
            val toEvictCount = pendingImages.size - 2
            for (i in 0 until toEvictCount) {
                val k = sortedKeys.getOrNull(i) ?: break
                if (isCapturingPsl && !burstTimestamps.contains(k)) {
                    pendingImages.remove(k)?.close()
                    pendingMetadata.remove(k)
                    continue
                }
                pendingImages.remove(k)?.close()
                pendingMetadata.remove(k)
            }
        }
    }

    // ── Remembered preview surface (needed to restart repeating request) ───────
    @Volatile private var lastPreviewSurface: Surface? = null
    @Volatile private var lastTexture: android.graphics.SurfaceTexture? = null
    @Volatile private var useRawCapture: Boolean = true  // Always start in RAW mode; YUV path is hardware fallback only
    @Volatile private var currentIso: Int = 400
    @Volatile private var currentExposureTime: Long = 33_333_333L
    @Volatile private var isCapturingPsl = false
    @Volatile private var staticNoiseProfile: FloatArray? = null
    @Volatile @JvmField var activeDynamicEv: Float = 0.0f
    @Volatile private var appliedEv: Float = 0.0f
    @Volatile private var cameraWhiteLevel: Float = 4095.0f
    private var analysisFrameCount = 0
    private val rawBufferPool = ArrayList<ByteBuffer>()

    // ── Semaphore to guard cameraDevice opening (Camera2 requirement) ──────────
    private val cameraOpenLock = Semaphore(1)

    // ─────────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Starts the background handler thread and opens the back-facing camera device to start the preview.
     *
     * @param texture The [android.graphics.SurfaceTexture] to draw the camera preview onto.
     */
    fun startCamera(texture: android.graphics.SurfaceTexture) {
        lastTexture = texture
        startBackgroundThread()
        if (orientationListener == null) {
            orientationListener = object : OrientationEventListener(context) {
                override fun onOrientationChanged(orientation: Int) {
                    if (orientation == ORIENTATION_UNKNOWN) return
                    deviceOrientation = (orientation + 45) / 90 * 90 % 360
                }
            }
        }
        orientationListener?.enable()
        scope.launch(Dispatchers.IO) {
            try {
                openCamera(texture)
            } catch (e: Exception) {
                Log.e(TAG, "startCamera failed", e)
            }
        }
    }

    /**
     * Stops the active capture session, closes the camera device, flushes all buffers, and kills the
     * background thread handler.
     */
    fun stopCamera() {
        orientationListener?.disable()
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

    /**
     * Toggles between front and back camera sensors, restarting the capture session.
     */
    fun toggleCameraFacing() {
        val texture = lastTexture ?: return
        isFrontFacing = !isFrontFacing
        stopCamera()
        startCamera(texture)
    }

    /**
     * Enables or disables Night capture mode. Restarts the repeating capture session request to apply
     * the new EV compensation levels.
     *
     * @param enabled Set to true to optimize exposure settings for low light conditions.
     */
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

    /**
     * Updates the current exposure compensation bias value (in EV steps).
     * Restarts the repeating capture session request to apply the new exposure targets.
     *
     * @param bias The exposure bias to apply.
     */
    fun setExposureBias(bias: Float) {
        if (activeExposureBias == bias) return
        activeExposureBias = bias
        Log.i(TAG, "setExposureBias: $bias")

        val session = captureSession
        val surface = lastPreviewSurface
        if (session != null && surface != null) {
            scope.launch(Dispatchers.IO) {
                runCatching { startRepeatingRequest(session, surface) }
            }
        }
    }

    /**
     * Configures the camera pipeline to capture either raw 16-bit Bayer data or standard 8-bit YUV buffers.
     * Recreates the [ImageReader] and current active capture session with the requested hardware format.
     *
     * @param enabled Set to true to request raw Bayer format; false for standard YUV.
     */
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
                        val cameraId = pickCameraId() ?: return@launch
                        val characteristics = manager.getCameraCharacteristics(cameraId)
                        cameraWhiteLevel = characteristics.get(CameraCharacteristics.SENSOR_INFO_WHITE_LEVEL)?.toFloat() ?: 4095.0f
                        
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

    /**
     * Finds and opens the primary rear-facing camera sensor.
     * Configures the preview texture aspect ratio, initializes the [ImageReader] with target formats,
     * and sets up hardware callbacks.
     *
     * @param texture The drawing surface texture.
     */
    private fun openCamera(texture: android.graphics.SurfaceTexture) {
        val cameraId = pickCameraId() ?: run {
            Log.e(TAG, "No camera found"); return
        }
        val characteristics = manager.getCameraCharacteristics(cameraId)
        cameraWhiteLevel = characteristics.get(CameraCharacteristics.SENSOR_INFO_WHITE_LEVEL)?.toFloat() ?: 4095.0f

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

    /**
     * Builds and configures a dual-output [CameraCaptureSession] with both preview and ImageReader surfaces.
     *
     * @param device The opened hardware camera device.
     * @param previewSurface The surface utilized for live viewfinder previews.
     */
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

    /**
     * Builds and starts a continuous repeating capture request.
     * Sets target exposure bias/compensation parameters, AF modes, and frames-per-second constraints.
     *
     * @param session The active camera capture session.
     * @param previewSurface The rendering surface.
     */
    private fun startRepeatingRequest(session: CameraCaptureSession, previewSurface: Surface) {
        val characteristics = manager.getCameraCharacteristics(
            pickCameraId() ?: return
        )

        // Compute EV compensation steps
        val aeRange = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        val aeStep  = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)
        val stepSize = aeStep?.toFloat() ?: (1f / 3f)
        val bias = if (isNightMode) 1.5f else activeDynamicEv
        val evSteps = (bias / stepSize).toInt()
            .coerceIn(aeRange?.lower ?: -6, aeRange?.upper ?: 0)
        appliedEv = evSteps * stepSize

        val requestBuilder = cameraDevice!!
            .createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                addTarget(previewSurface)
                addTarget(imageReader!!.surface)
                set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, evSteps)
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)

                // Force digital post-RAW gain boost to unity (100) to ensure the driver uses analog sensitivity (ISO) instead of digital gain
                set(CaptureRequest.CONTROL_POST_RAW_SENSITIVITY_BOOST, 100)

                // Enable Lens Shading Map statistics
                set(CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE, CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE_ON)

                // Enable Optical Image Stabilization (OIS) if supported
                val oisModes = characteristics.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION)
                if (oisModes != null && oisModes.contains(CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON)) {
                    set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
                    Log.i(TAG, "OIS enabled for preview")
                }

                // Select target FPS range with lowest minimum to support longer exposure times, preferring 30fps max to avoid capping exposure at 16.6ms
                val fpsRanges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                val bestRange = fpsRanges?.filter { it.upper == 30 }
                    ?.minByOrNull { it.lower }
                    ?: fpsRanges?.filter { it.upper >= 30 }?.minByOrNull { it.lower }
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
                val rawProfile = getNoiseProfileForFrame(result, currentIso)
                if (rawProfile != null && staticNoiseProfile == null) {
                    staticNoiseProfile = rawProfile
                    Log.i(TAG, "Cached static device noise profile from preview frame")
                }
                val iso = result.get(CaptureResult.SENSOR_SENSITIVITY)
                if (iso != null) {
                    currentIso = iso
                }
                val expTime = result.get(CaptureResult.SENSOR_EXPOSURE_TIME)
                if (expTime != null) {
                    currentExposureTime = expTime
                }
                val ts = result.get(CaptureResult.SENSOR_TIMESTAMP)
                if (ts != null) {
                    pendingMetadata[ts] = result
                    tryMatchAndPushFrame(ts)
                }

                // Extract active white balance gains from viewfinder AWB
                val gains = result.get(CaptureResult.COLOR_CORRECTION_GAINS)
                if (gains != null) {
                    val g = (gains.greenEven + gains.greenOdd) / 2.0f
                    val gVal = if (g > 0f) g else 1.0f
                    lastViewfinderAwbGains = floatArrayOf(gains.red / gVal, 1.0f, gains.blue / gVal)
                }

                // Extract active Color Correction Matrix (CCM) mapping sensor RGB to sRGB
                val transform = result.get(CaptureResult.COLOR_CORRECTION_TRANSFORM)
                if (transform != null) {
                    lastViewfinderCcm = FloatArray(9) { idx ->
                       val col = idx % 3
                       val row = idx / 3
                       transform.getElement(col, row).toFloat()
                    }
                }

                // Extract lens shading correction map (4-channel: R, Gr, Gb, B per pixel)
                // LensShadingMap.getGainFactor(ch, column, row): ch 0=R, 1=Gr, 2=Gb, 3=B
                @Suppress("UNCHECKED_CAST")
                val lscMap: LensShadingMap? = result.get(
                    CaptureResult.STATISTICS_LENS_SHADING_CORRECTION_MAP as CaptureResult.Key<LensShadingMap>
                )
                if (lscMap != null) {
                    val mapW = lscMap.columnCount
                    val mapH = lscMap.rowCount
                    val lscData = FloatArray(4 * mapW * mapH)
                    for (r in 0 until mapH) {
                        for (c in 0 until mapW) {
                            val base = (r * mapW + c) * 4
                            lscData[base + 0] = lscMap.getGainFactor(0, c, r)
                            lscData[base + 1] = lscMap.getGainFactor(1, c, r)
                            lscData[base + 2] = lscMap.getGainFactor(2, c, r)
                            lscData[base + 3] = lscMap.getGainFactor(3, c, r)
                        }
                    }
                    lastViewfinderLsc = lscData
                    lastViewfinderLscW = mapW
                    lastViewfinderLscH = mapH
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
        hdrMode: HdrMode = HdrMode.HDR_ON,
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

            var captureIso = currentIso
            var calculatedDigitalGain = 1.0f
            val isNight = config.nightMode
            val isHdrEnhanced = hdrMode == HdrMode.HDR_ENHANCED
            val forceBurst = !isNight && !isHdrEnhanced && hdrMode != HdrMode.OFF && currentIso > 400
            
            // Dynamic burst size based on lighting (current ISO)
            val dynamicFrameCount = when {
                currentIso < 150 -> 6
                currentIso < 800 -> 10
                else -> 15
            }.coerceAtMost(config.captureFrameCount)
            
            val burst = if (isNight || isHdrEnhanced || (forceBurst && hdrMode != HdrMode.OFF)) {
                isCapturingPsl = true
                _captureProgress.value = 0f
                ringBuffer.flush() // Clear preview frames
                burstTimestamps.clear()

                val targetExposureTime: Long
                val targetIso: Int
                val numFrames = dynamicFrameCount

                // exposureBoost: for negative bias (e.g. -1.5 EV), this is 2^(-1.5) = 0.35x target brightness
                val exposureBoost = Math.pow(2.0, config.exposureBias.toDouble()).toFloat()
                
                if (isNight) {
                    // Capped handheld-safe exposure limit: 66.6ms to prevent motion/handshake blur
                    val safeHandheldLimit = 66_666_666L
                    targetExposureTime = currentExposureTime.coerceAtMost(safeHandheldLimit)
                    
                    // Scale ISO to cover target exposure bias and exposure time reduction
                    val neededIso = currentIso.toDouble() * (currentExposureTime.toDouble() / targetExposureTime.toDouble()) * exposureBoost
                    targetIso = neededIso.toInt().coerceIn(50, 6400)
                } else if (isHdrEnhanced) {
                    // HDR+ Enhanced: Base exposure target (normal frames) capped to 50ms
                    targetExposureTime = currentExposureTime.coerceAtMost(50_000_000L)
                    targetIso = (currentIso.toDouble() * exposureBoost).toInt().coerceIn(50, 3200)
                } else {
                    // Normal mode: Cap exposure time at 33.3ms to avoid handshake blur
                    val maxNormalExposure = 33_333_333L
                    targetExposureTime = currentExposureTime.coerceAtMost(maxNormalExposure)
                    
                    // Scale ISO to cover target exposure bias and exposure time reduction
                    val neededIso = currentIso.toDouble() * (currentExposureTime.toDouble() / targetExposureTime.toDouble()) * exposureBoost
                    targetIso = neededIso.toInt().coerceIn(50, 3200)
                }
                captureIso = targetIso
                
                // Calculate digital gain to cover any exposure deficit after maxing out hardware ISO
                val totalExposureProduct = currentIso.toDouble() * currentExposureTime.toDouble() * exposureBoost
                val hardwareExposureProduct = targetIso.toDouble() * targetExposureTime.toDouble()
                calculatedDigitalGain = (totalExposureProduct / hardwareExposureProduct).toFloat()
                
                // Avoid digital gain scaling below 1.0x
                if (calculatedDigitalGain < 1.0f) {
                    calculatedDigitalGain = 1.0f
                }
                Log.i(TAG, "Still capture burst: isNight=$isNight, forceBurst=$forceBurst -> targetIso=$targetIso, targetExp=${targetExposureTime / 1_000_000}ms, digitalGain=${calculatedDigitalGain}x, exposureBoost=$exposureBoost, frames=$numFrames")

                // Create a manual still capture burst using TEMPLATE_MANUAL to guarantee hardware register gains are applied
                val requests = List(numFrames) { idx ->
                    // Alternate between Normal exposure and Short exposure (exposing 4x shorter / -2.0 EV)
                    // Frame 0 is always base normal exposure to act as reference frame
                    val frameExp = if (isHdrEnhanced && idx % 2 == 1) {
                        targetExposureTime / 4
                    } else {
                        targetExposureTime
                    }
                    cameraDevice!!.createCaptureRequest(CameraDevice.TEMPLATE_MANUAL).apply {
                        addTarget(imageReader!!.surface)
                        set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                        set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                        set(CaptureRequest.SENSOR_EXPOSURE_TIME, frameExp)
                        set(CaptureRequest.SENSOR_SENSITIVITY, targetIso)
                        set(CaptureRequest.CONTROL_POST_RAW_SENSITIVITY_BOOST, 100)
                        set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                        
                        // Enable Optical Image Stabilization (OIS) if supported
                        val chars = manager.getCameraCharacteristics(pickCameraId() ?: "")
                        val oisModes = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION)
                        if (oisModes != null && oisModes.contains(CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON)) {
                            set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
                        }

                        // Enable Lens Shading Map statistics
                        set(CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE, CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE_ON)

                        setTag("BURST")
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
                            burstTimestamps.add(ts)
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

                // Wait up to 6 seconds for all frames to arrive
                val timeoutMs = 6000L
                val startTime = System.currentTimeMillis()
                while (ringBuffer.size < numFrames && (System.currentTimeMillis() - startTime) < timeoutMs) {
                    _captureProgress.value = ringBuffer.size.toFloat() / numFrames.toFloat()
                    delay(50)
                }
                _captureProgress.value = 1.0f

                isCapturingPsl = false
                val pslFrames = ringBuffer.snapshot()
                if (pslFrames.size < numFrames) {
                    Log.w(TAG, "Still burst capture timed out, got ${pslFrames.size}/$numFrames frames")
                } else {
                    Log.i(TAG, "Successfully captured all $numFrames still burst frames!")
                }
                pslFrames
            } else {
                _captureProgress.value = 1.0f
                val fullDrained = ringBuffer.snapshot()
                if (fullDrained.isEmpty()) {
                    _isProcessing.value = false
                    if (session != null && surface != null) {
                        runCatching { startRepeatingRequest(session, surface) }
                    }
                    withContext(Dispatchers.Main) {
                        onDispatched()
                        onError("Ring buffer is empty — point camera at subject and try again")
                    }
                    return@launch
                }
                
                calculatedDigitalGain = 1.0f

                // Filter for stable exposure settings (ZSL Exposure Lock Convergence)
                val convergedFrames = if (fullDrained.isNotEmpty() && hdrMode != HdrMode.OFF) {
                    val refFrame = fullDrained.last()
                    fullDrained.filter { frame ->
                        val expDev = Math.abs(frame.exposureTimeNs - refFrame.exposureTimeNs).toDouble() / refFrame.exposureTimeNs.toDouble()
                        val isoDev = Math.abs(frame.iso - refFrame.iso).toDouble() / refFrame.iso.toDouble()
                        expDev <= 0.15 && isoDev <= 0.15
                    }
                } else {
                    fullDrained
                }

                val needed = if (hdrMode == HdrMode.OFF) 1 else dynamicFrameCount
                if (convergedFrames.size > needed) {
                    val discard = convergedFrames.size - needed
                    // Close discarded frames
                    convergedFrames.subList(0, discard).forEach { runCatching { it.image.close() } }
                    // Also close any frames filtered out completely
                    fullDrained.filter { it !in convergedFrames }.forEach { runCatching { it.image.close() } }
                    convergedFrames.subList(discard, convergedFrames.size)
                } else {
                    // Close any frames filtered out completely
                    fullDrained.filter { it !in convergedFrames }.forEach { runCatching { it.image.close() } }
                    convergedFrames
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
            val frameExposures = LongArray(count)
            val frameNoiseProfiles = FloatArray(count * 8)

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
                frameExposures[idx] = frame.exposureTimeNs
                val customProfile = getCustomNoiseProfile(config, frame.iso)
                val profile = customProfile ?: frame.noiseProfile
                if (profile != null && profile.size == 8) {
                    System.arraycopy(profile, 0, frameNoiseProfiles, idx * 8, 8)
                } else {
                    for (i in 0 until 8) {
                        frameNoiseProfiles[idx * 8 + i] = 0f
                    }
                }
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
            
            val characteristics = manager.getCameraCharacteristics(pickCameraId() ?: "")
            val sensorOrientation = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90

            val lensFacing = characteristics.get(CameraCharacteristics.LENS_FACING)
            val isFrontFacing = lensFacing == CameraMetadata.LENS_FACING_FRONT
            val jpegOrientation = if (isFrontFacing) {
                (sensorOrientation - deviceOrientation + 360) % 360
            } else {
                (sensorOrientation + deviceOrientation) % 360
            }

            // Use converged AWB gains and CCM from the viewfinder right before still capture
            val awbGains = lastViewfinderAwbGains ?: floatArrayOf(2.1f, 1.0f, 1.9f)
            val ccm = lastViewfinderCcm ?: floatArrayOf(
                1.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 1.0f
            )

            val cfa = characteristics.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT) ?: 3
            val blackPattern = characteristics.get(CameraCharacteristics.SENSOR_BLACK_LEVEL_PATTERN)
            val blackOffsets = IntArray(4)
            blackPattern?.copyTo(blackOffsets, 0)
            val blackLvl = blackOffsets.getOrNull(0)?.toFloat() ?: 64f
            val whiteLvl = characteristics.get(CameraCharacteristics.SENSOR_INFO_WHITE_LEVEL)?.toFloat() ?: 1023f

            val job = ProcessingJob(
                id = jobId,
                timestamp = timestamp,
                iso = captureIso,
                frameIsos = frameIsos,
                frameExposures = frameExposures,
                frameNoiseProfiles = frameNoiseProfiles,
                nativeBurstHandle = nativeHandle,
                config = config.copy(useRawCapture = isRaw),
                onSaved = onSaved,
                onError = onError,
                awbGains = awbGains,
                colorCorrectionMatrix = ccm,
                blackLevel = blackLvl,
                whiteLevel = whiteLvl,
                digitalGain = calculatedDigitalGain,
                sensorOrientation = jpegOrientation,
                appliedEvCompensation = activeDynamicEv,
                colorFilterArrangement = cfa,
                lensShadingMap = lastViewfinderLsc,
                lscMapWidth = lastViewfinderLscW,
                lscMapHeight = lastViewfinderLscH
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

    private fun pickCameraId(): String? {
        val targetFacing = if (isFrontFacing) CameraMetadata.LENS_FACING_FRONT else CameraMetadata.LENS_FACING_BACK
        for (id in manager.cameraIdList) {
            val characteristics = manager.getCameraCharacteristics(id)
            val facing = characteristics.get(CameraCharacteristics.LENS_FACING)
            if (facing == targetFacing) return id
        }
        return manager.cameraIdList.firstOrNull()
    }

    /**
     * Starts the dedicated background thread for processing camera callbacks to avoid blocking the main UI thread.
     */
    private fun startBackgroundThread() {
        cameraThread = HandlerThread("CameraBackground").also {
            it.start()
            cameraHandler = Handler(it.looper)
        }
    }

    /**
     * Stops the background callback thread safely.
     */
    private fun stopBackgroundThread() {
        cameraThread?.quitSafely()
        cameraThread?.join()
        cameraThread  = null
        cameraHandler = null
    }

    private fun getNoiseProfileForFrame(meta: CaptureResult, iso: Int): FloatArray? {
        val rawProfile: Array<out android.util.Pair<Double, Double>>? = meta.get(CaptureResult.SENSOR_NOISE_PROFILE)
        if (rawProfile != null && rawProfile.size >= 4) {
            return floatArrayOf(
                rawProfile[0].first.toFloat(), rawProfile[0].second.toFloat(),
                rawProfile[1].first.toFloat(), rawProfile[1].second.toFloat(),
                rawProfile[2].first.toFloat(), rawProfile[2].second.toFloat(),
                rawProfile[3].first.toFloat(), rawProfile[3].second.toFloat()
            )
        }
        return staticNoiseProfile
    }

    private fun getCustomNoiseProfile(config: PipelineConfig, iso: Int): FloatArray? {
        val useBack = !isFrontFacing
        val hasCustom = if (useBack) {
            config.backNoiseA != 0f || config.backNoiseB != 0f || config.backNoiseC != 0f || config.backNoiseD != 0f
        } else {
            config.frontNoiseA != 0f || config.frontNoiseB != 0f || config.frontNoiseC != 0f || config.frontNoiseD != 0f
        }
        
        if (!hasCustom) return null
        
        val a = if (useBack) config.backNoiseA else config.frontNoiseA
        val b = if (useBack) config.backNoiseB else config.frontNoiseB
        val c = if (useBack) config.backNoiseC else config.frontNoiseC
        val d = if (useBack) config.backNoiseD else config.frontNoiseD
        
        val sens = iso.toDouble()
        val digitalGain = if (sens / 3200.0 < 1.0) 1.0 else sens / 3200.0
        
        val profile = FloatArray(8)
        for (i in 0 until 4) {
            val sVal = a * sens + b
            val oVal = c * sens * sens + d * digitalGain * digitalGain
            profile[i * 2] = Math.max(0.0, sVal).toFloat()
            profile[i * 2 + 1] = Math.max(0.0, oVal).toFloat()
        }
        return profile
    }
}
