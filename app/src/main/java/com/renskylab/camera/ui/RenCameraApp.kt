package com.renskylab.camera.ui

import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.*
import androidx.compose.animation.core.*
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.renskylab.camera.CaptureMode
import com.renskylab.camera.CameraViewModel
import com.renskylab.camera.FlashMode
import com.renskylab.camera.ProcessingManager
import com.renskylab.camera.PipelineConfig

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette — deep space / AMOLED aesthetic
// ─────────────────────────────────────────────────────────────────────────────
private val Black        = Color(0xFF000000)
private val SurfaceGlass = Color(0x99111111)
private val AccentCyan   = Color(0xFF00D4FF)
private val AccentBlue   = Color(0xFF5B6BF8)
private val White90      = Color(0xE6FFFFFF)
private val White60      = Color(0x99FFFFFF)
private val ErrorRed     = Color(0xFFFF4560)

// ─────────────────────────────────────────────────────────────────────────────
// Root composable
// ─────────────────────────────────────────────────────────────────────────────
@Composable
fun RenCameraApp(viewModel: CameraViewModel) {
    val uiState by viewModel.uiState.collectAsState()
    val config by viewModel.pipelineConfig.collectAsState()
    var showSettings by remember { mutableStateOf(false) }

    // Launcher for XML Config Export
    val exportLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("text/xml"),
        onResult = { uri ->
            if (uri != null) {
                viewModel.exportConfigToUri(uri)
            }
        }
    )

    // Launcher for XML Config Import
    val importLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
        onResult = { uri ->
            if (uri != null) {
                viewModel.importConfigFromUri(uri)
            }
        }
    )

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Black)
    ) {
        // ── Camera preview ─────────────────────────────────────────────────────
        CameraPreview(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(3f / 4f)
                .align(Alignment.Center),
            onTextureReady = { st ->
                viewModel.controller.startCamera(st)
            }
        )

        // ── Gradient scrim — bottom controls ──────────────────────────────────
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(280.dp)
                .align(Alignment.BottomCenter)
                .background(
                    Brush.verticalGradient(
                        colors = listOf(Color.Transparent, Color(0xCC000000))
                    )
                )
        )

        // ── Gradient scrim — top controls ─────────────────────────────────────
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(120.dp)
                .align(Alignment.TopCenter)
                .background(
                    Brush.verticalGradient(
                        colors = listOf(Color(0xAA000000), Color.Transparent)
                    )
                )
        )

        // ── Top controls bar ──────────────────────────────────────────────────
        TopControlsBar(
            flashMode    = uiState.flashMode,
            onFlashCycle = { viewModel.setFlashMode(uiState.flashMode.next()) },
            onSettingsClick = { showSettings = true },
            modifier     = Modifier
                .align(Alignment.TopCenter)
                .statusBarsPadding()
                .padding(top = 4.dp, start = 16.dp, end = 16.dp),
        )

        // ── Bottom controls bar ───────────────────────────────────────────────
        Column(
            modifier          = Modifier.align(Alignment.BottomCenter),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            // Mode selector tabs
            ModeSelectorRow(
                selected  = uiState.captureMode,
                onSelect  = viewModel::setCaptureMode,
                modifier  = Modifier.padding(bottom = 24.dp),
            )

            // Main controls: gallery | shutter | (placeholder for front cam)
            BottomControlsRow(
                isProcessing    = uiState.isProcessing,
                lastCapturedUri = uiState.lastCapturedUri,
                onShutter       = viewModel::onShutterPressed,
                modifier        = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 40.dp, vertical = 16.dp),
            )

            Spacer(Modifier.height(32.dp))
        }

        // ── Error snackbar ────────────────────────────────────────────────────
        uiState.errorMessage?.let { msg ->
            ErrorBanner(
                message  = msg,
                onDismiss = viewModel::clearError,
                modifier  = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = 80.dp, start = 24.dp, end = 24.dp),
            )
        }

        // ── Settings Screen Overlay ───────────────────────────────────────────
        if (showSettings) {
            BackHandler {
                showSettings = false
            }
        }

        AnimatedVisibility(
            visible = showSettings,
            enter = fadeIn() + slideInVertically { it },
            exit = fadeOut() + slideOutVertically { it }
        ) {
            SettingsScreen(
                config = config,
                onConfigChange = viewModel::updateConfig,
                onExport = { exportLauncher.launch("ren_camera_config.xml") },
                onImport = { importLauncher.launch(arrayOf("text/xml", "application/xml")) },
                onClose = { showSettings = false }
            )
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera preview — AndroidView wrapping TextureView
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun CameraPreview(
    modifier: Modifier,
    onTextureReady: (SurfaceTexture) -> Unit,
) {
    AndroidView(
        factory = { ctx ->
            TextureView(ctx).apply {
                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                        onTextureReady(st)
                    }
                    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
                    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean = true
                    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
                }
            }
        },
        modifier = modifier,
    )
}

// ─────────────────────────────────────────────────────────────────────────────
// Top controls bar
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun TopControlsBar(
    flashMode: FlashMode,
    onFlashCycle: () -> Unit,
    onSettingsClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier            = modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment   = Alignment.CenterVertically,
    ) {
        // Flash
        IconButton(onClick = onFlashCycle) {
            Icon(
                imageVector = when (flashMode) {
                    FlashMode.OFF  -> Icons.Default.FlashOff
                    FlashMode.AUTO -> Icons.Default.FlashAuto
                    FlashMode.ON   -> Icons.Default.FlashOn
                },
                contentDescription = "Flash: $flashMode",
                tint   = if (flashMode == FlashMode.ON) AccentCyan else White90,
            )
        }

        // Settings
        IconButton(onClick = onSettingsClick) {
            Icon(
                imageVector        = Icons.Default.Settings,
                contentDescription = "Settings",
                tint               = White60,
            )
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mode selector
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun ModeSelectorRow(
    selected: CaptureMode,
    onSelect: (CaptureMode) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier              = modifier,
        horizontalArrangement = Arrangement.spacedBy(24.dp),
        verticalAlignment     = Alignment.CenterVertically,
    ) {
        CaptureMode.entries.forEach { mode ->
            val isSelected = mode == selected
            Text(
                text       = mode.label,
                color      = if (isSelected) AccentCyan else White60,
                fontSize   = if (isSelected) 13.sp else 12.sp,
                fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Normal,
                modifier   = Modifier
                    .clickable { onSelect(mode) }
                    .then(
                        if (isSelected) Modifier.border(
                            width  = 1.dp,
                            color  = AccentCyan.copy(alpha = 0.4f),
                            shape  = RoundedCornerShape(50),
                        ) else Modifier
                    )
                    .padding(horizontal = 12.dp, vertical = 4.dp),
            )
        }
    }
}

private val CaptureMode.label get() = when (this) {
    CaptureMode.PHOTO    -> "PHOTO"
    CaptureMode.NIGHT    -> "NIGHT"
    CaptureMode.PORTRAIT -> "PORTRAIT"
    CaptureMode.VIDEO    -> "VIDEO"
}

// ─────────────────────────────────────────────────────────────────────────────
// Bottom controls row: [gallery thumbnail] [shutter] [front cam]
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun BottomControlsRow(
    isProcessing: Boolean,
    lastCapturedUri: android.net.Uri?,
    onShutter: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier              = modifier,
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment     = Alignment.CenterVertically,
    ) {
        // Gallery thumbnail placeholder with progress spinner
        val isBackgroundProcessing by ProcessingManager.isProcessing.collectAsState()

        Box(
            modifier = Modifier
                .size(52.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(SurfaceGlass)
                .border(1.dp, White60.copy(alpha = 0.3f), RoundedCornerShape(8.dp)),
            contentAlignment = Alignment.Center,
        ) {
            if (isBackgroundProcessing) {
                CircularProgressIndicator(
                    color = AccentCyan,
                    strokeWidth = 2.dp,
                    modifier = Modifier.size(24.dp)
                )
            } else {
                Icon(
                    imageVector        = Icons.Default.PhotoLibrary,
                    contentDescription = "Gallery",
                    tint               = White60,
                    modifier           = Modifier.size(24.dp),
                )
            }
        }

        // Shutter button
        ShutterButton(
            isProcessing = isProcessing,
            onPress      = onShutter,
        )

        // Flip camera (placeholder)
        Box(
            modifier = Modifier
                .size(52.dp)
                .clip(CircleShape)
                .background(SurfaceGlass),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                imageVector        = Icons.Default.Cameraswitch,
                contentDescription = "Flip camera",
                tint               = White60,
                modifier           = Modifier.size(24.dp),
            )
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Shutter button with press animation
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun ShutterButton(
    isProcessing: Boolean,
    onPress: () -> Unit,
) {
    var pressed by remember { mutableStateOf(false) }
    val scale by animateFloatAsState(
        targetValue    = if (pressed || isProcessing) 0.88f else 1f,
        animationSpec  = spring(dampingRatio = 0.5f, stiffness = 400f),
        label          = "shutter_scale",
    )

    // Pulsing ring while processing
    val infiniteTransition = rememberInfiniteTransition(label = "pulse")
    val ringAlpha by infiniteTransition.animateFloat(
        initialValue   = 0.4f,
        targetValue    = 0.9f,
        animationSpec  = infiniteRepeatable(
            animation  = tween(700, easing = EaseInOutSine),
            repeatMode = RepeatMode.Reverse,
        ),
        label = "ring_alpha",
    )

    Box(
        contentAlignment = Alignment.Center,
        modifier = Modifier
            .scale(scale)
            .size(84.dp)
            .pointerInput(isProcessing) {
                if (!isProcessing) {
                    detectTapGestures(
                        onPress = { offset ->
                            pressed = true
                            tryAwaitRelease()
                            pressed = false
                        },
                        onTap = { onPress() },
                    )
                }
            },
    ) {
        // Outer pulsing ring
        Box(
            modifier = Modifier
                .size(84.dp)
                .border(
                    width  = 3.dp,
                    color  = if (isProcessing)
                        AccentCyan.copy(alpha = ringAlpha)
                    else
                        White90.copy(alpha = 0.85f),
                    shape  = CircleShape,
                )
        )

        // Inner filled circle
        Box(
            modifier = Modifier
                .size(68.dp)
                .clip(CircleShape)
                .background(
                    if (isProcessing)
                        Brush.radialGradient(listOf(AccentCyan, AccentBlue))
                    else
                        Brush.radialGradient(listOf(White90, White60))
                )
        )
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Processing overlay
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun ProcessingOverlay() {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0x55000000)),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            CircularProgressIndicator(
                color         = AccentCyan,
                strokeWidth   = 3.dp,
                modifier      = Modifier.size(48.dp),
            )
            Text(
                text       = "HDR+ Processing…",
                color      = White90,
                fontSize   = 14.sp,
                fontWeight = FontWeight.Light,
                letterSpacing = 1.sp,
            )
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Error banner
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun ErrorBanner(
    message: String,
    onDismiss: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier      = modifier.fillMaxWidth(),
        shape         = RoundedCornerShape(12.dp),
        color         = ErrorRed.copy(alpha = 0.92f),
        tonalElevation = 4.dp,
    ) {
        Row(
            modifier              = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment     = Alignment.CenterVertically,
        ) {
            Text(
                text     = message,
                color    = Color.White,
                fontSize = 13.sp,
                modifier = Modifier.weight(1f),
            )
            IconButton(onClick = onDismiss) {
                Icon(
                    imageVector        = Icons.Default.Close,
                    contentDescription = "Dismiss",
                    tint               = Color.White,
                )
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Extension helpers
// ─────────────────────────────────────────────────────────────────────────────
private fun FlashMode.next() = when (this) {
    FlashMode.OFF  -> FlashMode.AUTO
    FlashMode.AUTO -> FlashMode.ON
    FlashMode.ON   -> FlashMode.OFF
}

private val EaseInOutSine = CubicBezierEasing(0.37f, 0f, 0.63f, 1f)

// ─────────────────────────────────────────────────────────────────────────────
// Settings Screen UI Composables
// ─────────────────────────────────────────────────────────────────────────────

@Composable
private fun SettingsScreen(
    config: PipelineConfig,
    onConfigChange: (PipelineConfig) -> Unit,
    onExport: () -> Unit,
    onImport: () -> Unit,
    onClose: () -> Unit
) {
    var infoDialogText by remember { mutableStateOf<Pair<String, String>?>(null) }
    
    // Sub-menu expansion states
    var expandedAcq by remember { mutableStateOf(false) }
    var expandedAlign by remember { mutableStateOf(false) }
    var expandedFuse by remember { mutableStateOf(false) }
    var expandedAwb by remember { mutableStateOf(false) }
    var expandedToneMap by remember { mutableStateOf(false) }
    var expandedDebug by remember { mutableStateOf(false) }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF070708))
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .padding(16.dp)
        ) {
            // Header row
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(bottom = 12.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "Tuning Settings",
                    color = AccentCyan,
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.sp
                )
                IconButton(onClick = onClose) {
                    Icon(Icons.Default.Close, contentDescription = "Close settings", tint = Color.White)
                }
            }

            // Scrollable Category Container
            Column(
                modifier = Modifier
                    .weight(1f)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                // 1. Acquisition Settings Category
                CategoryCard(
                    title = "1. Acquisition & Quality",
                    expanded = expandedAcq,
                    onToggle = { expandedAcq = !expandedAcq },
                    infoText = "Adjusts raw capture parameters like compression ratio and highlight metering bias.",
                    onResetDefaults = {
                        onConfigChange(config.copy(
                            jpegQuality = 95,
                            exposureBias = -1.5f,
                            isoOverride = 0,
                            useRawCapture = true
                        ))
                    }
                ) {
                    SliderSetting(
                        label = "JPEG Quality",
                        value = config.jpegQuality.toFloat(),
                        valueRange = 50f..100f,
                        steps = 50,
                        valueFormatter = { "${it.toInt()}%" },
                        description = "JPEG compression quality. Higher means cleaner details but larger file sizes.",
                        onValueChange = { onConfigChange(config.copy(jpegQuality = it.toInt())) },
                        onShowInfo = { infoDialogText = "JPEG Quality" to "Specifies the quantization quality parameter inside libjpeg-turbo. 95% is the recommended sweet spot between pixel artifacts and compression sizes." }
                    )

                    SliderSetting(
                        label = "Exposure Bias",
                        value = config.exposureBias,
                        valueRange = -3.0f..3.0f,
                        steps = 60,
                        valueFormatter = { String.format("%.1f EV", it) },
                        description = "Exposure compensation in normal modes. Negative values protect highlights from clipping.",
                        onValueChange = { onConfigChange(config.copy(exposureBias = it)) },
                        onShowInfo = { infoDialogText = "Exposure Bias" to "Controls how aggressively the camera hardware underexposes the preview ring buffer. Lower EV values protect the sensor highlight channels from saturation." }
                    )

                    SliderSetting(
                        label = "ISO Override",
                        value = config.isoOverride.toFloat(),
                        valueRange = 0f..6400f,
                        steps = 64,
                        valueFormatter = { if (it.toInt() == 0) "Auto" else "ISO ${it.toInt()}" },
                        description = "Override the ISO used by the noise model. 'Auto' uses the actual capture ISO. Lower values = less aggressive denoising (more texture detail). Set to 200 for daylight shots.",
                        onValueChange = { onConfigChange(config.copy(isoOverride = it.toInt())) },
                        onShowInfo = { infoDialogText = "ISO Override" to "The noise model uses ISO to compute the expected sensor noise floor (sigma). At 'Auto' it reads the actual capture ISO from metadata. For daylight shots (bright scenes), setting this to 100-400 prevents over-aggressive denoising that can wash out fine texture." }
                    )

                    SwitchSetting(
                        label = "Use RAW Capture",
                        checked = config.useRawCapture,
                        description = "Capture 16-bit Bayer RAW sensor data instead of 8-bit YUV. Vastly improves dynamic range, texture clarity, and reduces grain, but increases processing time.",
                        onCheckedChange = { onConfigChange(config.copy(useRawCapture = it)) },
                        onShowInfo = { infoDialogText = "Use RAW Capture" to "Configures the camera sensor to output RAW_SENSOR format instead of YUV_420_888. RAW contains unprocessed 10/12-bit linear values directly from the sensor. Operating on RAW yields cleaner noise model performance and better highlight recovery." }
                    )
                }

                // 2. Alignment Settings Category
                CategoryCard(
                    title = "2. Alignment (Block Matcher)",
                    expanded = expandedAlign,
                    onToggle = { expandedAlign = !expandedAlign },
                    infoText = "Configures the hierarchical block matching search cost regularization.",
                    onResetDefaults = {
                        onConfigChange(config.copy(
                            alignmentRegularization = 5.0f
                        ))
                    }
                ) {
                    SliderSetting(
                        label = "Displacement Regularization",
                        value = config.alignmentRegularization,
                        valueRange = 0.0f..20.0f,
                        steps = 40,
                        valueFormatter = { String.format("%.1f", it) },
                        description = "Regularization cost. High values stabilize alignment in high-noise/low-light zones.",
                        onValueChange = { onConfigChange(config.copy(alignmentRegularization = it)) },
                        onShowInfo = { infoDialogText = "Displacement Regularization" to "Applies a quadratic cost penalty [penalty = reg * (dx^2 + dy^2)] to block displacements. Higher values favor zero-motion, preventing alignment failure on noise." }
                    )
                }

                // 3. Fusion Settings Category
                CategoryCard(
                    title = "3. Fusion (Wiener Blender)",
                    expanded = expandedFuse,
                    onToggle = { expandedFuse = !expandedFuse },
                    infoText = "Controls temporal merging thresholds based on the sensor noise floor.",
                    onResetDefaults = {
                        onConfigChange(config.copy(
                            fusionNoiseMultiplier = 3.0f,
                            chromaDenoiseEnabled = true,
                            spatialDenoiseStrength = 8
                        ))
                    }
                ) {
                    SliderSetting(
                        label = "Noise Blend Multiplier",
                        value = config.fusionNoiseMultiplier,
                        valueRange = 1.0f..6.0f,
                        steps = 50,
                        valueFormatter = { String.format("%.1fx", it) },
                        description = "Rejection envelope multiplier. High values allow blending noisier pixels.",
                        onValueChange = { onConfigChange(config.copy(fusionNoiseMultiplier = it)) },
                        onShowInfo = { infoDialogText = "Noise Blend Multiplier" to "Multiplies the calculated luma noise standard deviation to determine the temporal blending threshold. High multipliers merge more aggressively; low multipliers reject hand movement trails." }
                    )

                    SliderSetting(
                        label = "Spatial Denoise Strength",
                        value = config.spatialDenoiseStrength.toFloat(),
                        valueRange = 0f..20f,
                        steps = 20,
                        valueFormatter = { if (it.toInt() == 0) "Off" else "${it.toInt()}" },
                        description = "NL-Means luma denoising applied after temporal fusion. Removes residual grain while preserving sharp edges. 0 = disabled, 8 = default (daylight), 12-15 = night.",
                        onValueChange = { onConfigChange(config.copy(spatialDenoiseStrength = it.toInt())) },
                        onShowInfo = { infoDialogText = "Spatial Denoise Strength" to "Applies Non-Local Means (NLM) denoising on the fused luma (Y) plane before tonemapping. Unlike a blur, NLM preserves sharp edges by averaging only patches with similar texture. 'h' parameter controls smoothing aggressiveness." }
                    )

                    SwitchSetting(
                        label = "Chroma Denoise",
                        checked = config.chromaDenoiseEnabled,
                        description = "Enables spatial chroma denoising to wipe out colored shadow blotches.",
                        onCheckedChange = { onConfigChange(config.copy(chromaDenoiseEnabled = it)) },
                        onShowInfo = { infoDialogText = "Chroma Denoise" to "Applies an edge-preserving spatial filter on the chrominance (U/V) planes in Night Mode to remove blotchy colored luma artifacts." }
                    )
                }

                // 4. Auto White Balance Category
                CategoryCard(
                    title = "4. Auto White Balance (AWB)",
                    expanded = expandedAwb,
                    onToggle = { expandedAwb = !expandedAwb },
                    infoText = "Tunes the softness gains applied to white balance corrections.",
                    onResetDefaults = {
                        onConfigChange(config.copy(
                            awbSoftnessNormal = 0.60f,
                            awbSoftnessNight = 0.85f
                        ))
                    }
                ) {
                    SliderSetting(
                        label = "AWB Softness (Normal)",
                        value = config.awbSoftnessNormal,
                        valueRange = 0.0f..1.0f,
                        steps = 20,
                        valueFormatter = { String.format("%.2f", it) },
                        description = "AWB softness in daylight. Higher values preserve warm ambient colors.",
                        onValueChange = { onConfigChange(config.copy(awbSoftnessNormal = it)) },
                        onShowInfo = { infoDialogText = "AWB Softness (Normal)" to "Damps computed Grey World correction gains. 1.0 applies complete correction; lower values retain daylight atmosphere." }
                    )

                    SliderSetting(
                        label = "AWB Softness (Night)",
                        value = config.awbSoftnessNight,
                        valueRange = 0.0f..1.0f,
                        steps = 20,
                        valueFormatter = { String.format("%.2f", it) },
                        description = "AWB softness in Night mode. Higher values clear orange street light casts.",
                        onValueChange = { onConfigChange(config.copy(awbSoftnessNight = it)) },
                        onShowInfo = { infoDialogText = "AWB Softness (Night)" to "Damps computed Night mode Grey World gains. Higher softness factors remove sodium yellow casts completely." }
                    )
                }

                // 5. Tone Mapping Category
                CategoryCard(
                    title = "5. Tone Mapping (ACES & Bilateral)",
                    expanded = expandedToneMap,
                    onToggle = { expandedToneMap = !expandedToneMap },
                    infoText = "Configures dynamic range compression, local micro-contrast detail boosting, and black levels.",
                    onResetDefaults = {
                        onConfigChange(config.copy(
                            detailAlpha = 1.15f,
                            saturationBoost = 1.15f,
                            blackPointClamp = 0.08f
                        ))
                    }
                ) {
                    SliderSetting(
                        label = "Detail Boost Alpha",
                        value = config.detailAlpha,
                        valueRange = 0.5f..2.0f,
                        steps = 30,
                        valueFormatter = { String.format("%.2fx", it) },
                        description = "Local micro-contrast texture boost. Higher values sharpen fine details.",
                        onValueChange = { onConfigChange(config.copy(detailAlpha = it)) },
                        onShowInfo = { infoDialogText = "Detail Boost Alpha" to "Specifies the exponent for detail layer reconstruction. Exponents greater than 1.0 amplify fine high-frequency textures without producing halo artifacts." }
                    )

                    SliderSetting(
                        label = "Saturation Boost",
                        value = config.saturationBoost,
                        valueRange = 0.5f..2.0f,
                        steps = 30,
                        valueFormatter = { String.format("%.2fx", it) },
                        description = "Luma-safe color saturation boost. Preserves hues perfectly.",
                        onValueChange = { onConfigChange(config.copy(saturationBoost = it)) },
                        onShowInfo = { infoDialogText = "Saturation Boost" to "Linearly interpolates RGB channel differences from luma. This scales saturation while keeping hues mathematically identical and limiting clipping." }
                    )

                    SliderSetting(
                        label = "Black-Point Clamp",
                        value = config.blackPointClamp,
                        valueRange = 0.0f..0.5f,
                        steps = 50,
                        valueFormatter = { String.format("%.2f", it) },
                        description = "Synthetic Sky Compression threshold. Clamps night sky shadows to clean blacks.",
                        onValueChange = { onConfigChange(config.copy(blackPointClamp = it)) },
                        onShowInfo = { infoDialogText = "Black-Point Clamp" to "Applies quadratic compression to normalized base luminance values below this threshold. Prevents dark night skies from being lifted into noisy gray layers." }
                    )
                }

                // 6. Debug Settings Category
                CategoryCard(
                    title = "6. Debug & Diagnostics",
                    expanded = expandedDebug,
                    onToggle = { expandedDebug = !expandedDebug },
                    infoText = "Developer diagnostics. These options write extra files to disk and may slow down capture.",
                    onResetDefaults = {
                        onConfigChange(config.copy(debugImagesEnabled = true, debugRawDumps = false))
                    }
                ) {
                    // ── Master toggle ──────────────────────────────────────────
                    SwitchSetting(
                        label = "Debug Images",
                        checked = config.debugImagesEnabled,
                        description = "Write per-stage and per-frame JPEG previews to the capture folder for pipeline inspection. Disable for maximum capture speed.",
                        onCheckedChange = { onConfigChange(config.copy(debugImagesEnabled = it)) },
                        onShowInfo = { infoDialogText = "Debug Images" to "When enabled, each pipeline stage writes a JPEG snapshot to disk: ref_frame.jpg, src_frame_1.jpg, diff_before/after_alignment.jpg, denoised_crop.jpg, fused.jpg, debayered.jpg, tonemapped.jpg, final_output.jpg. These are useful for tuning and diagnosing the HDR+ algorithm. Disable to skip all debug writes and maximise capture speed." }
                    )

                    // ── Sub-toggle: raw binary dumps (only meaningful when master is on) ──
                    Box(modifier = Modifier.alpha(if (config.debugImagesEnabled) 1f else 0.38f)) {
                        SwitchSetting(
                            label = "Also Write Raw Binaries",
                            checked = config.debugRawDumps && config.debugImagesEnabled,
                            description = "Additionally write uncompressed binary buffers: fused.yuv (∼19 MB), debayered.ppm (∼38 MB), tonemapped.ppm (∼38 MB). Opens in GIMP / ImageJ / ffplay. Adds ~25 s per capture.",
                            onCheckedChange = { if (config.debugImagesEnabled) onConfigChange(config.copy(debugRawDumps = it)) },
                            onShowInfo = { infoDialogText = "Also Write Raw Binaries" to "When enabled alongside Debug Images, each stage also writes its full-resolution uncompressed buffer: fused.yuv (planar YUV420), debayered.ppm (RGB P6 PPM), tonemapped.ppm (RGB P6 PPM). These contain the exact byte values after each computation and can be loaded in GIMP, ImageJ, or with ffplay -f rawvideo. Requires Debug Images to be on." }
                        )
                    }
                }
            }

            Spacer(Modifier.height(16.dp))

            // Action Buttons Row: Global Reset, Export XML, Import XML
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                // Global Reset
                Button(
                    onClick = { onConfigChange(PipelineConfig()) },
                    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF221111)),
                    modifier = Modifier.weight(1f),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Icon(Icons.Default.Refresh, contentDescription = "Reset", tint = ErrorRed, modifier = Modifier.size(16.dp))
                    Spacer(Modifier.width(4.dp))
                    Text("Reset All", color = ErrorRed, fontSize = 11.sp, fontWeight = FontWeight.SemiBold)
                }

                // Import XML
                Button(
                    onClick = onImport,
                    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF1E293B)),
                    modifier = Modifier.weight(1.1f),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Icon(Icons.Default.ArrowUpward, contentDescription = "Import", tint = Color.White, modifier = Modifier.size(16.dp))
                    Spacer(Modifier.width(4.dp))
                    Text("Import XML", color = Color.White, fontSize = 11.sp, fontWeight = FontWeight.SemiBold)
                }

                // Export XML
                Button(
                    onClick = onExport,
                    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF1E293B)),
                    modifier = Modifier.weight(1.1f),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Icon(Icons.Default.ArrowDownward, contentDescription = "Export", tint = Color.White, modifier = Modifier.size(16.dp))
                    Spacer(Modifier.width(4.dp))
                    Text("Export XML", color = Color.White, fontSize = 11.sp, fontWeight = FontWeight.SemiBold)
                }
            }
        }

        // Info popup dialog
        infoDialogText?.let { (title, description) ->
            AlertDialog(
                onDismissRequest = { infoDialogText = null },
                title = { Text(text = title, color = AccentCyan, fontSize = 16.sp, fontWeight = FontWeight.Bold) },
                text = { Text(text = description, color = Color.White, fontSize = 13.sp) },
                confirmButton = {
                    TextButton(onClick = { infoDialogText = null }) {
                        Text("OK", color = AccentCyan)
                    }
                },
                containerColor = Color(0xFF151518)
            )
        }
    }
}

@Composable
private fun CategoryCard(
    title: String,
    expanded: Boolean,
    onToggle: () -> Unit,
    infoText: String,
    onResetDefaults: () -> Unit,
    content: @Composable ColumnScope.() -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .border(1.dp, Color(0xFF1F1F24), RoundedCornerShape(8.dp)),
        colors = CardDefaults.cardColors(containerColor = Color(0xFF0F0F12)),
        shape = RoundedCornerShape(8.dp)
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            // Header
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onToggle() },
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = title,
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold
                )
                Icon(
                    imageVector = if (expanded) Icons.Default.ArrowUpward else Icons.Default.ArrowDownward,
                    contentDescription = "Expand",
                    tint = White60,
                    modifier = Modifier.size(16.dp)
                )
            }

            AnimatedVisibility(visible = expanded) {
                Column(
                    modifier = Modifier.padding(top = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(16.dp)
                ) {
                    // Category general instruction banner
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(Color(0xFF16161C), RoundedCornerShape(6.dp))
                            .padding(8.dp)
                    ) {
                        Text(
                            text = infoText,
                            color = White60,
                            fontSize = 11.sp,
                            lineHeight = 15.sp
                        )
                    }

                    // Content sliders/toggles
                    content()

                    // Reset Category Defaults Button
                    TextButton(
                        onClick = onResetDefaults,
                        modifier = Modifier.align(Alignment.End)
                    ) {
                        Icon(Icons.Default.Refresh, contentDescription = "Reset category", tint = AccentCyan, modifier = Modifier.size(14.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Reset Defaults", color = AccentCyan, fontSize = 11.sp)
                    }
                }
            }
        }
    }
}

@Composable
private fun SliderSetting(
    label: String,
    value: Float,
    valueRange: ClosedFloatingPointRange<Float>,
    steps: Int,
    valueFormatter: (Float) -> String,
    description: String,
    onValueChange: (Float) -> Unit,
    onShowInfo: () -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(text = label, color = White90, fontSize = 13.sp, fontWeight = FontWeight.Medium)
                Spacer(Modifier.width(4.dp))
                IconButton(onClick = onShowInfo, modifier = Modifier.size(18.dp)) {
                    Icon(Icons.Default.Info, contentDescription = "Info", tint = AccentCyan, modifier = Modifier.size(14.dp))
                }
            }
            Text(text = valueFormatter(value), color = AccentCyan, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        }
        
        Spacer(Modifier.height(4.dp))
        
        Text(text = description, color = White60, fontSize = 11.sp, lineHeight = 14.sp)
        
        Spacer(Modifier.height(4.dp))
        
        Slider(
            value = value,
            onValueChange = onValueChange,
            valueRange = valueRange,
            steps = steps,
            colors = SliderDefaults.colors(
                thumbColor = AccentCyan,
                activeTrackColor = AccentCyan,
                inactiveTrackColor = Color(0xFF27272F)
            ),
            modifier = Modifier.height(24.dp)
        )
    }
}

@Composable
private fun SwitchSetting(
    label: String,
    checked: Boolean,
    description: String,
    onCheckedChange: (Boolean) -> Unit,
    onShowInfo: () -> Unit,
    modifier: Modifier = Modifier
) {
    Column(modifier = modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(text = label, color = White90, fontSize = 13.sp, fontWeight = FontWeight.Medium)
                Spacer(Modifier.width(4.dp))
                IconButton(onClick = onShowInfo, modifier = Modifier.size(18.dp)) {
                    Icon(Icons.Default.Info, contentDescription = "Info", tint = AccentCyan, modifier = Modifier.size(14.dp))
                }
            }
            Switch(
                checked = checked,
                onCheckedChange = onCheckedChange,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = AccentCyan,
                    checkedTrackColor = AccentCyan.copy(alpha = 0.4f),
                    uncheckedThumbColor = White60,
                    uncheckedTrackColor = Color(0xFF27272F)
                )
            )
        }
        Spacer(Modifier.height(2.dp))
        Text(text = description, color = White60, fontSize = 11.sp, lineHeight = 14.sp)
    }
}
