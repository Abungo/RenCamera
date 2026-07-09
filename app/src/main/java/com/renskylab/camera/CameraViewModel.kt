package com.renskylab.camera

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

import kotlinx.coroutines.launch

/** Shared state between the UI and the camera pipeline. */
data class CameraUiState(
    val isProcessing: Boolean = false,
    val lastCapturedUri: Uri? = null,
    val flashMode: FlashMode = FlashMode.AUTO,
    val captureMode: CaptureMode = CaptureMode.PHOTO,
    val errorMessage: String? = null,
)

enum class FlashMode { OFF, AUTO, ON }
enum class CaptureMode { PHOTO, NIGHT, PORTRAIT, VIDEO }

class CameraViewModel(application: Application) : AndroidViewModel(application) {

    val controller = CameraController(application, viewModelScope)

    private val _uiState = MutableStateFlow(CameraUiState())
    val uiState: StateFlow<CameraUiState> = _uiState.asStateFlow()

    private val _pipelineConfig = MutableStateFlow(PipelineConfig())
    val pipelineConfig: StateFlow<PipelineConfig> = _pipelineConfig.asStateFlow()

    init {
        // Shutter isProcessing is now controlled dynamically per capture phase.
    }

    fun updateConfig(config: PipelineConfig) {
        _pipelineConfig.value = config
        controller.setRawCaptureEnabled(config.useRawCapture)
    }

    fun exportConfigToUri(uri: Uri) {
        viewModelScope.launch {
            try {
                val context = getApplication<Application>().applicationContext
                context.contentResolver.openOutputStream(uri)?.use { outputStream ->
                    java.io.OutputStreamWriter(outputStream).use { writer ->
                        writer.write(_pipelineConfig.value.toXml())
                    }
                }
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(errorMessage = "Export failed: ${e.localizedMessage}")
            }
        }
    }

    fun importConfigFromUri(uri: Uri) {
        viewModelScope.launch {
            try {
                val context = getApplication<Application>().applicationContext
                context.contentResolver.openInputStream(uri)?.use { inputStream ->
                    java.io.BufferedReader(java.io.InputStreamReader(inputStream)).use { reader ->
                        val xml = reader.readText()
                        val imported = PipelineConfig.fromXml(xml)
                        _pipelineConfig.value = imported
                    }
                }
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(errorMessage = "Import failed: ${e.localizedMessage}")
            }
        }
    }

    fun onShutterPressed() {
        if (_uiState.value.isProcessing) return
        _uiState.value = _uiState.value.copy(isProcessing = true, errorMessage = null)

        val isNight = _uiState.value.captureMode == CaptureMode.NIGHT
        val config = _pipelineConfig.value.copy(
            nightMode = isNight,
            exposureBias = if (isNight) -0.5f else -1.5f
        )

        controller.captureBurst(
            config  = config,
            onDispatched = {
                _uiState.value = _uiState.value.copy(isProcessing = false)
            },
            onSaved = { uri ->
                _uiState.value = _uiState.value.copy(
                    lastCapturedUri = uri,
                )
            },
            onError = { msg ->
                _uiState.value = _uiState.value.copy(
                    errorMessage = msg,
                )
            },
        )
    }

    fun setFlashMode(mode: FlashMode) {
        _uiState.value = _uiState.value.copy(flashMode = mode)
    }

    fun setCaptureMode(mode: CaptureMode) {
        _uiState.value = _uiState.value.copy(captureMode = mode)
        controller.setNightModeEnabled(mode == CaptureMode.NIGHT)
    }

    fun clearError() {
        _uiState.value = _uiState.value.copy(errorMessage = null)
    }

    override fun onCleared() {
        controller.stopCamera()
        super.onCleared()
    }
}
