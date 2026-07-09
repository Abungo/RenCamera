package com.renskylab.camera

import java.io.Serializable

data class PipelineConfig(
    val stageEnabled: Map<String, Boolean> = DEFAULT_STAGES,
    val jpegQuality: Int = 95,
    val exposureBias: Float = -1.5f,
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
    val detailAlpha: Float = 1.15f,
    val saturationBoost: Float = 1.15f,
    val blackPointClamp: Float = 0.08f,

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
    val normalModeIsoReductionFactor: Float = 2.0f
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

        fun fromXml(xml: String): PipelineConfig {
            val stages = DEFAULT_STAGES.toMutableMap()
            var jpegQuality = 95
            var exposureBias = -1.5f
            var nightMode = false
            var alignmentRegularization = 5.0f
            var fusionNoiseMultiplier = 3.0f
            var chromaDenoiseEnabled = true
            var spatialDenoiseStrength = 8
            var awbSoftnessNormal = 0.60f
            var awbSoftnessNight = 0.85f
            var detailAlpha = 1.15f
            var saturationBoost = 1.15f
            var blackPointClamp = 0.08f
            var isoOverride = 0
            var useRawCapture = true
            var debugRawDumps = false
            var debugImagesEnabled = true
            var normalModeIsoReductionFactor = 2.0f

            val tagRegex = "<([^>]+)>([^<]*)</\\1>".toRegex()
            val matches = tagRegex.findAll(xml)
            for (match in matches) {
                val tag = match.groupValues[1]
                val value = match.groupValues[2].trim()
                when (tag) {
                    "jpegQuality" -> jpegQuality = value.toIntOrNull() ?: 95
                    "exposureBias" -> exposureBias = value.toFloatOrNull() ?: -1.5f
                    "nightMode" -> nightMode = value.toBooleanStrictOrNull() ?: false
                    "alignmentRegularization" -> alignmentRegularization = value.toFloatOrNull() ?: 5.0f
                    "fusionNoiseMultiplier" -> fusionNoiseMultiplier = value.toFloatOrNull() ?: 3.0f
                    "chromaDenoiseEnabled" -> chromaDenoiseEnabled = value.toBooleanStrictOrNull() ?: true
                    "spatialDenoiseStrength" -> spatialDenoiseStrength = value.toIntOrNull() ?: 8
                    "awbSoftnessNormal" -> awbSoftnessNormal = value.toFloatOrNull() ?: 0.60f
                    "awbSoftnessNight" -> awbSoftnessNight = value.toFloatOrNull() ?: 0.85f
                    "detailAlpha" -> detailAlpha = value.toFloatOrNull() ?: 1.15f
                    "saturationBoost" -> saturationBoost = value.toFloatOrNull() ?: 1.15f
                    "blackPointClamp" -> blackPointClamp = value.toFloatOrNull() ?: 0.08f
                    "isoOverride" -> isoOverride = value.toIntOrNull() ?: 0
                    "useRawCapture" -> useRawCapture = value.toBooleanStrictOrNull() ?: false
                    "debugRawDumps" -> debugRawDumps = value.toBooleanStrictOrNull() ?: false
                    "debugImagesEnabled" -> debugImagesEnabled = value.toBooleanStrictOrNull() ?: true
                    "normalModeIsoReductionFactor" -> normalModeIsoReductionFactor = value.toFloatOrNull() ?: 2.0f
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
                normalModeIsoReductionFactor = normalModeIsoReductionFactor
            )
        }
    }

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
            normalModeIsoReductionFactor               // 13
        )
    }

    fun toStageFlagArray(): BooleanArray =
        BooleanArray(stageOrder.size) { i ->
            stageEnabled[stageOrder[i]] ?: true
        }

    fun withStage(name: String, enabled: Boolean): PipelineConfig =
        copy(stageEnabled = stageEnabled + (name to enabled))

    fun toXml(): String {
        val sb = java.lang.StringBuilder()
        sb.append("<RenCameraConfig>\n")
        sb.append("  <jpegQuality>${jpegQuality}</jpegQuality>\n")
        sb.append("  <exposureBias>${exposureBias}</exposureBias>\n")
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
        stageEnabled.forEach { (k, v) ->
            sb.append("  <stage_${k}>${v}</stage_${k}>\n")
        }
        sb.append("</RenCameraConfig>")
        return sb.toString()
    }
}
