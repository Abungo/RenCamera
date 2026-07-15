package com.renskylab.camera

import java.io.Serializable

/**
 * Configuration parameters for the computational camera pipeline.
 * Contains tuning values for frame capture, alignment, fusion, denoising, auto white balance,
 * tone mapping, and diagnostics. Implements [Serializable] to allow state saving.
 *
 * @property stageEnabled Map indicating whether specific pipeline stages (e.g., "align", "fuse") are enabled.
 * @property jpegQuality Target compression quality for output JPEGs (50 to 100).
 * @property exposureBias EV compensation value applied on capture request (e.g. -1.5f).
 * @property nightExposureBias EV compensation value applied in night mode capture requests (e.g. -0.5f).
 * @property nightMode Indicates if night processing mode optimizations should be used.
 * @property alignmentRegularization Penalty scaling factor to constrain block displacements in alignment.
 * @property fusionNoiseMultiplier Standard deviation multiplier threshold for temporal fusion merging.
 * @property chromaDenoiseEnabled Toggle for spatial chroma denoising filter.
 * @property spatialDenoiseStrength Value representing Non-Local Means luma denoiser strength (0 = disabled).
 * @property awbSoftnessNormal Damping factor for Auto White Balance gains in normal daylight mode.
 * @property awbSoftnessNight Damping factor for Auto White Balance gains in night mode.
 * @property detailAlpha Local micro-contrast detail boost exponent (greater than 1.0 amplifies texture).
 * @property saturationBoost Linear scaling factor for color saturation.
 * @property blackPointClamp Threshold below which luminance values are quadratically clamped to dark.
 * @property isoOverride Manual ISO override used for the noise model calibration (0 = use capture metadata).
 * @property useRawCapture Enables raw Bayer 16-bit format captures instead of 8-bit YUV images.
 * @property debugRawDumps Enables writing heavy raw intermediate binary dumps per stage.
 * @property debugImagesEnabled Master toggle for exporting stage-wise debugging JPEGs.
 * @property normalModeIsoReductionFactor Factor used to shift ISO lower and shutter longer in normal low-light modes.
 * @property captureFrameCount The total number of burst frames captured.
 */
data class PipelineConfig(
    val stageEnabled: Map<String, Boolean> = DEFAULT_STAGES,
    val jpegQuality: Int = 95,
    val exposureBias: Float = -1.5f,
    val nightExposureBias: Float = -0.5f,
    val nightMode: Boolean = false,

    // Alignment parameters
    val alignmentRegularization: Float = 5.0f,

    // Fusion parameters
    val fusionNoiseMultiplier: Float = 3.0f,
    val chromaDenoiseEnabled: Boolean = true,

    // Spatial denoising (NL-Means on fused luma plane, 0 = disabled)
    val spatialDenoiseStrength: Int = 8,

    // AWB parameters
    val awbSoftnessNormal: Float = 0.60f,
    val awbSoftnessNight: Float = 0.85f,

    // Tone mapping parameters
    val detailAlpha: Float = 1.00f,
    val saturationBoost: Float = 1.00f,
    val blackPointClamp: Float = 0.008f, // Reduced from 0.03f to 0.008f so dark shadow details are fully visible

    // ISO override for noise model (0 = use actual capture ISO from EXIF/metadata)
    val isoOverride: Int = 0,

    // Captures 16-bit RAW Bayer sensor data instead of 8-bit YUV
    val useRawCapture: Boolean = true,

    // Debug: also write raw binary dumps (YUV / PPM) per stage alongside JPEGs.
    // Disabled by default — these blobs are large (~94 MB) and slow down capture.
    // Enable temporarily when you need to inspect pixel-exact intermediate buffers.
    val debugRawDumps: Boolean = false,

    // Debug: master switch — enables all per-stage debug image writing (frame JPEGs +
    // pipeline-step JPEGs). When off, no debug images are written at all (fastest path).
    val debugImagesEnabled: Boolean = true,

    // Normal mode ISO reduction factor (e.g. 2.0f halves ISO and doubles exposure time)
    val normalModeIsoReductionFactor: Float = 2.0f,

    // Dynamic capture burst frame count (default is 15)
    val captureFrameCount: Int = 15,

    // Custom noise model parameters per lens (0.0f = use system noise profile)
    val backNoiseA: Float = 0f,
    val backNoiseB: Float = 0f,
    val backNoiseC: Float = 0f,
    val backNoiseD: Float = 0f,
    val frontNoiseA: Float = 0f,
    val frontNoiseB: Float = 0f,
    val frontNoiseC: Float = 0f,
    val frontNoiseD: Float = 0f,

    // Post-tonemap luma passes switches and strengths
    val denoisePass1Enabled: Boolean = true,
    val denoisePass1Strength: Float = 1.0f,
    val denoisePass2Enabled: Boolean = true,
    val denoisePass2Strength: Float = 0.5f,
    val tonemapExposureBoost: Float = 1.30f
) : Serializable {

    companion object {
        val stageOrder = listOf("align", "fuse", "debayer", "tonemap", "encode")

        val DEFAULT_STAGES: Map<String, Boolean> = mapOf(
            "align"   to true,
            "fuse"    to true,
            "debayer" to true,
            "denoise" to false,
            "sharpen" to false,
            "tonemap" to true,
            "encode"  to true,
        )

        /**
         * Parses a PipelineConfig object from an XML string representation.
         *
         * @param xml The XML formatted configuration string.
         * @return A parsed [PipelineConfig] instance with settings extracted from XML.
         */
        fun fromXml(xml: String): PipelineConfig {
            val stages = DEFAULT_STAGES.toMutableMap()
            var jpegQuality = 95
            var exposureBias = -1.5f
            var nightExposureBias = -0.5f
            var nightMode = false
            var alignmentRegularization = 5.0f
            var fusionNoiseMultiplier = 3.0f
            var chromaDenoiseEnabled = true
            var spatialDenoiseStrength = 8
            var awbSoftnessNormal = 0.60f
            var awbSoftnessNight = 0.85f
            var detailAlpha = 1.00f
            var saturationBoost = 1.00f
            var blackPointClamp = 0.008f
            var isoOverride = 0
            var useRawCapture = true
            var debugRawDumps = false
            var debugImagesEnabled = true
            var normalModeIsoReductionFactor = 2.0f
            var captureFrameCount = 15
            var backNoiseA = 0f
            var backNoiseB = 0f
            var backNoiseC = 0f
            var backNoiseD = 0f
            var frontNoiseA = 0f
            var frontNoiseB = 0f
            var frontNoiseC = 0f
            var frontNoiseD = 0f
            var denoisePass1Enabled = true
            var denoisePass1Strength = 1.0f
            var denoisePass2Enabled = true
            var denoisePass2Strength = 0.5f
            var tonemapExposureBoost = 1.30f

            val tagRegex = "<([^>]+)>([^<]*)</\\1>".toRegex()
            val matches = tagRegex.findAll(xml)
            for (match in matches) {
                val tag = match.groupValues[1]
                val value = match.groupValues[2].trim()
                when (tag) {
                    "jpegQuality" -> jpegQuality = value.toIntOrNull() ?: 95
                    "exposureBias" -> exposureBias = value.toFloatOrNull() ?: -1.5f
                    "nightExposureBias" -> nightExposureBias = value.toFloatOrNull() ?: -0.5f
                    "nightMode" -> nightMode = value.toBooleanStrictOrNull() ?: false
                    "alignmentRegularization" -> alignmentRegularization = value.toFloatOrNull() ?: 5.0f
                    "fusionNoiseMultiplier" -> fusionNoiseMultiplier = value.toFloatOrNull() ?: 3.0f
                    "chromaDenoiseEnabled" -> chromaDenoiseEnabled = value.toBooleanStrictOrNull() ?: true
                    "spatialDenoiseStrength" -> spatialDenoiseStrength = value.toIntOrNull() ?: 8
                    "awbSoftnessNormal" -> awbSoftnessNormal = value.toFloatOrNull() ?: 0.60f
                    "awbSoftnessNight" -> awbSoftnessNight = value.toFloatOrNull() ?: 0.85f
                    "detailAlpha" -> detailAlpha = value.toFloatOrNull() ?: 1.15f
                    "saturationBoost" -> saturationBoost = value.toFloatOrNull() ?: 1.15f
                    "blackPointClamp" -> blackPointClamp = value.toFloatOrNull() ?: 0.03f
                    "isoOverride" -> isoOverride = value.toIntOrNull() ?: 0
                    "useRawCapture" -> useRawCapture = value.toBooleanStrictOrNull() ?: true
                    "debugRawDumps" -> debugRawDumps = value.toBooleanStrictOrNull() ?: false
                    "debugImagesEnabled" -> debugImagesEnabled = value.toBooleanStrictOrNull() ?: true
                    "normalModeIsoReductionFactor" -> normalModeIsoReductionFactor = value.toFloatOrNull() ?: 2.0f
                    "captureFrameCount" -> captureFrameCount = value.toIntOrNull() ?: 15
                    "backNoiseA" -> backNoiseA = value.toFloatOrNull() ?: 0f
                    "backNoiseB" -> backNoiseB = value.toFloatOrNull() ?: 0f
                    "backNoiseC" -> backNoiseC = value.toFloatOrNull() ?: 0f
                    "backNoiseD" -> backNoiseD = value.toFloatOrNull() ?: 0f
                    "frontNoiseA" -> frontNoiseA = value.toFloatOrNull() ?: 0f
                    "frontNoiseB" -> frontNoiseB = value.toFloatOrNull() ?: 0f
                    "frontNoiseC" -> frontNoiseC = value.toFloatOrNull() ?: 0f
                    "frontNoiseD" -> frontNoiseD = value.toFloatOrNull() ?: 0f
                    "denoisePass1Enabled" -> denoisePass1Enabled = value.toBooleanStrictOrNull() ?: true
                    "denoisePass1Strength" -> denoisePass1Strength = value.toFloatOrNull() ?: 1.0f
                    "denoisePass2Enabled" -> denoisePass2Enabled = value.toBooleanStrictOrNull() ?: true
                    "denoisePass2Strength" -> denoisePass2Strength = value.toFloatOrNull() ?: 0.5f
                    "tonemapExposureBoost" -> tonemapExposureBoost = value.toFloatOrNull() ?: 1.30f
                    else -> {
                        if (tag.startsWith("stage_")) {
                            val stageName = tag.substringAfter("stage_")
                            stages[stageName] = value.toBooleanStrictOrNull() ?: true
                        }
                    }
                }
            }
            return PipelineConfig(
                stageEnabled = stages,
                jpegQuality = jpegQuality,
                exposureBias = exposureBias,
                nightExposureBias = nightExposureBias,
                nightMode = nightMode,
                alignmentRegularization = alignmentRegularization,
                fusionNoiseMultiplier = fusionNoiseMultiplier,
                chromaDenoiseEnabled = chromaDenoiseEnabled,
                spatialDenoiseStrength = spatialDenoiseStrength,
                awbSoftnessNormal = awbSoftnessNormal,
                awbSoftnessNight = awbSoftnessNight,
                detailAlpha = detailAlpha,
                saturationBoost = saturationBoost,
                blackPointClamp = blackPointClamp,
                isoOverride = isoOverride,
                useRawCapture = useRawCapture,
                debugRawDumps = debugRawDumps,
                debugImagesEnabled = debugImagesEnabled,
                normalModeIsoReductionFactor = normalModeIsoReductionFactor,
                captureFrameCount = captureFrameCount,
                backNoiseA = backNoiseA,
                backNoiseB = backNoiseB,
                backNoiseC = backNoiseC,
                backNoiseD = backNoiseD,
                frontNoiseA = frontNoiseA,
                frontNoiseB = frontNoiseB,
                frontNoiseC = frontNoiseC,
                frontNoiseD = frontNoiseD,
                denoisePass1Enabled = denoisePass1Enabled,
                denoisePass1Strength = denoisePass1Strength,
                denoisePass2Enabled = denoisePass2Enabled,
                denoisePass2Strength = denoisePass2Strength,
                tonemapExposureBoost = tonemapExposureBoost
            )
        }
    }

    /**
     * Converts configuration parameters to a flat float array.
     * This format is easily digestible by JNI / C++ processing engines.
     *
     * @return FloatArray mapping all relevant configuration settings.
     */
    fun toFloatArray(): FloatArray {
        return floatArrayOf(
            alignmentRegularization,                   // 0
            fusionNoiseMultiplier,                     // 1
            if (chromaDenoiseEnabled) 1.0f else 0.0f, // 2
            awbSoftnessNormal,                         // 3
            awbSoftnessNight,                          // 4
            detailAlpha,                               // 5
            saturationBoost,                           // 6
            blackPointClamp,                           // 7
            spatialDenoiseStrength.toFloat(),          // 8
            isoOverride.toFloat(),                     // 9
            if (useRawCapture) 1.0f else 0.0f,   // 10
            if (debugRawDumps) 1.0f else 0.0f,    // 11
            if (debugImagesEnabled) 1.0f else 0.0f, // 12
            normalModeIsoReductionFactor,              // 13
            captureFrameCount.toFloat(),               // 14
            if (denoisePass1Enabled) 1.0f else 0.0f,   // 15
            denoisePass1Strength,                      // 16
            if (denoisePass2Enabled) 1.0f else 0.0f,   // 17
            denoisePass2Strength,                      // 18
            tonemapExposureBoost                       // 19
        )
    }

    /**
     * Maps the enabled stages to a BooleanArray ordered by execution priority.
     *
     * @return BooleanArray representing execution states of pipeline stages.
     */
    fun toStageFlagArray(): BooleanArray =
        BooleanArray(stageOrder.size) { i ->
            stageEnabled[stageOrder[i]] ?: true
        }

    /**
     * Helper to duplicate config with a specific pipeline stage altered.
     *
     * @param name The stage name.
     * @param enabled Set to true if the stage should be run.
     * @return A new duplicate [PipelineConfig] with modified stage settings.
     */
    fun withStage(name: String, enabled: Boolean): PipelineConfig =
        copy(stageEnabled = stageEnabled + (name to enabled))

    /**
     * Serializes this config object into an XML structure.
     *
     * @return The XML formatted configuration string.
     */
    fun toXml(): String {
        val sb = java.lang.StringBuilder()
        sb.append("<RenCameraConfig>\n")
        sb.append("  <jpegQuality>${jpegQuality}</jpegQuality>\n")
        sb.append("  <exposureBias>${exposureBias}</exposureBias>\n")
        sb.append("  <nightExposureBias>${nightExposureBias}</nightExposureBias>\n")
        sb.append("  <nightMode>${nightMode}</nightMode>\n")
        sb.append("  <alignmentRegularization>${alignmentRegularization}</alignmentRegularization>\n")
        sb.append("  <fusionNoiseMultiplier>${fusionNoiseMultiplier}</fusionNoiseMultiplier>\n")
        sb.append("  <chromaDenoiseEnabled>${chromaDenoiseEnabled}</chromaDenoiseEnabled>\n")
        sb.append("  <spatialDenoiseStrength>${spatialDenoiseStrength}</spatialDenoiseStrength>\n")
        sb.append("  <awbSoftnessNormal>${awbSoftnessNormal}</awbSoftnessNormal>\n")
        sb.append("  <awbSoftnessNight>${awbSoftnessNight}</awbSoftnessNight>\n")
        sb.append("  <detailAlpha>${detailAlpha}</detailAlpha>\n")
        sb.append("  <saturationBoost>${saturationBoost}</saturationBoost>\n")
        sb.append("  <blackPointClamp>${blackPointClamp}</blackPointClamp>\n")
        sb.append("  <isoOverride>${isoOverride}</isoOverride>\n")
        sb.append("  <useRawCapture>${useRawCapture}</useRawCapture>\n")
        sb.append("  <debugRawDumps>${debugRawDumps}</debugRawDumps>\n")
        sb.append("  <debugImagesEnabled>${debugImagesEnabled}</debugImagesEnabled>\n")
        sb.append("  <normalModeIsoReductionFactor>${normalModeIsoReductionFactor}</normalModeIsoReductionFactor>\n")
        sb.append("  <captureFrameCount>${captureFrameCount}</captureFrameCount>\n")
        sb.append("  <backNoiseA>${backNoiseA}</backNoiseA>\n")
        sb.append("  <backNoiseB>${backNoiseB}</backNoiseB>\n")
        sb.append("  <backNoiseC>${backNoiseC}</backNoiseC>\n")
        sb.append("  <backNoiseD>${backNoiseD}</backNoiseD>\n")
        sb.append("  <frontNoiseA>${frontNoiseA}</frontNoiseA>\n")
        sb.append("  <frontNoiseB>${frontNoiseB}</frontNoiseB>\n")
        sb.append("  <frontNoiseC>${frontNoiseC}</frontNoiseC>\n")
        sb.append("  <frontNoiseD>${frontNoiseD}</frontNoiseD>\n")
        sb.append("  <denoisePass1Enabled>${denoisePass1Enabled}</denoisePass1Enabled>\n")
        sb.append("  <denoisePass1Strength>${denoisePass1Strength}</denoisePass1Strength>\n")
        sb.append("  <denoisePass2Enabled>${denoisePass2Enabled}</denoisePass2Enabled>\n")
        sb.append("  <denoisePass2Strength>${denoisePass2Strength}</denoisePass2Strength>\n")
        sb.append("  <tonemapExposureBoost>${tonemapExposureBoost}</tonemapExposureBoost>\n")
        stageEnabled.forEach { (k, v) ->
            sb.append("  <stage_${k}>${v}</stage_${k}>\n")
        }
        sb.append("</RenCameraConfig>")
        return sb.toString()
    }
}
