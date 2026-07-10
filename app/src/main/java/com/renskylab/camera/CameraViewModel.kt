package com.renskylab.camera

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

import kotlinx.coroutines.launch

/**
 * Represents the UI state of the camera application interface.
 *
 * @property isProcessing Indicates if a capture job is currently active/processing in the foreground UI.
 * @property lastCapturedUri The Uri pointing to the most recently saved photo in storage.
 * @property flashMode The current active flash mode setting (OFF, AUTO, ON).
 * @property captureMode The current active capture mode setting (PHOTO, NIGHT, etc.).
 * @property hdrMode The active high dynamic range processing mode setting.
 * @property timerMode The active timer countdown mode setting.
 * @property timerCountdownValue The current remaining seconds for active timer countdowns.
 * @property errorMessage The active error message to display in the UI, or null if there is no error.
 */
data class CameraUiState(
    val isProcessing: Boolean = false,
    val lastCapturedUri: Uri? = null,
    val flashMode: FlashMode = FlashMode.AUTO,
    val captureMode: CaptureMode = CaptureMode.PHOTO,
    val hdrMode: HdrMode = HdrMode.HDR_ON,
    val timerMode: TimerMode = TimerMode.OFF,
    val timerCountdownValue: Int = 0,
    val errorMessage: String? = null,
)

/**
 * Supported hardware flash modes.
 */
enum class FlashMode { OFF, AUTO, ON }

/**
 * Supported camera capture modes.
 */
enum class CaptureMode { PHOTO, NIGHT, PORTRAIT, VIDEO }

/**
 * Supported high dynamic range capture profiles.
 */
enum class HdrMode { OFF, HDR_ON, HDR_ENHANCED }

/**
 * Supported delay timer settings (in seconds).
 */
enum class TimerMode(val seconds: Int) { OFF(0), SEC_3(3), SEC_10(10) }

/**
 * Shared ViewModel that sits between the Jetpack Compose UI and the hardware camera controller.
 * Exposes UI states, manages configuration saving/loading via SharedPreferences, and triggers
 * burst captures based on UI actions.
 */
class CameraViewModel(application: Application) : AndroidViewModel(application) {

    /**
     * The hardware camera controller instance.
     */
    val controller = CameraController(application, viewModelScope)

    private val _uiState = MutableStateFlow(CameraUiState())
    
    /**
     * StateFlow exposing the read-only [CameraUiState] to observers in the UI.
     */
    val uiState: StateFlow<CameraUiState> = _uiState.asStateFlow()

    private val _pipelineConfig = MutableStateFlow(PipelineConfig())
    
    /**
     * StateFlow exposing the active [PipelineConfig] containing tuning settings.
     */
    val pipelineConfig: StateFlow<PipelineConfig> = _pipelineConfig.asStateFlow()

        
    init {
        // Load config and sheet preferences from SharedPreferences
        val prefs = application.getSharedPreferences("RenCameraPrefs", android.content.Context.MODE_PRIVATE)
        val savedXml = prefs.getString("pipeline_config", null)
        if (savedXml != null) {
            runCatching {
                val loaded = PipelineConfig.fromXml(savedXml)
                _pipelineConfig.value = loaded
                controller.setRawCaptureEnabled(loaded.useRawCapture)
                controller.setExposureBias(loaded.exposureBias)
            }
        }
        
        // Restore ui settings state
        try {
            val flashMode = FlashMode.valueOf(prefs.getString("pref_flash_mode", FlashMode.AUTO.name) ?: FlashMode.AUTO.name)
            val hdrMode = HdrMode.valueOf(prefs.getString("pref_hdr_mode", HdrMode.HDR_ON.name) ?: HdrMode.HDR_ON.name)
            val timerMode = TimerMode.valueOf(prefs.getString("pref_timer_mode", TimerMode.OFF.name) ?: TimerMode.OFF.name)
            val captureMode = CaptureMode.valueOf(prefs.getString("pref_capture_mode", CaptureMode.PHOTO.name) ?: CaptureMode.PHOTO.name)
            
            _uiState.value = _uiState.value.copy(
                flashMode = flashMode,
                hdrMode = hdrMode,
                timerMode = timerMode,
                captureMode = captureMode
            )
            controller.setNightModeEnabled(captureMode == CaptureMode.NIGHT)
        } catch (e: Exception) {
            android.util.Log.e("RenCamera/ViewModel", "Failed to restore sheet states", e)
        }
        
        // Load latest photo from MediaStore to populate gallery button on start
        loadLastPhotoUri()
    }

    private fun loadLastPhotoUri() {
        viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            val projection = arrayOf(
                android.provider.MediaStore.Images.Media._ID,
                android.provider.MediaStore.Images.Media.DATE_ADDED
            )
            val sortOrder = "${android.provider.MediaStore.Images.Media.DATE_ADDED} DESC"
            val queryUri = android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI
            
            getApplication<Application>().contentResolver.query(
                queryUri,
                projection,
                null,
                null,
                sortOrder
            )?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val idColumn = cursor.getColumnIndexOrThrow(android.provider.MediaStore.Images.Media._ID)
                    val id = cursor.getLong(idColumn)
                    val uri = android.content.ContentUris.withAppendedId(
                        android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                        id
                    )
                    _uiState.value = _uiState.value.copy(lastCapturedUri = uri)
                }
            }
        }
    }

    /**
     * Updates the active configuration and applies changes (such as raw capture modes)
     * to the camera controller. Persists the configuration to SharedPreferences in XML format.
     *
     * @param config The new [PipelineConfig] to apply.
     */
    fun updateConfig(config: PipelineConfig) {
        _pipelineConfig.value = config
        controller.setRawCaptureEnabled(config.useRawCapture)
        controller.setExposureBias(config.exposureBias)

        // Save config to SharedPreferences
        val prefs = getApplication<Application>().getSharedPreferences("RenCameraPrefs", android.content.Context.MODE_PRIVATE)
        prefs.edit().putString("pipeline_config", config.toXml()).apply()
    }

    /**
     * Exports the current XML configuration to the specified URI.
     *
     * @param uri The destination URI where the XML config will be written.
     */
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

    /**
     * Imports and applies a configuration XML from the specified URI.
     *
     * @param uri The source URI containing the configuration XML to read.
     */
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

    /**
     * Triggered when the shutter button is pressed.
     * Evaluates active capture modes (e.g. Night mode), locks UI states, and delegates
     * to [CameraController] to capture and process a frame burst.
     */
    fun onShutterPressed() {
        if (_uiState.value.isProcessing) return
        _uiState.value = _uiState.value.copy(isProcessing = true, errorMessage = null)

        val activeMode = _uiState.value.captureMode
        val isNight = activeMode == CaptureMode.NIGHT
        // Use the user-configured bias for the active mode — never hardcode EV values here.
        // nightExposureBias and exposureBias are both editable from Settings.
        val config = _pipelineConfig.value.copy(
            nightMode = isNight,
            exposureBias = if (isNight) _pipelineConfig.value.nightExposureBias
                           else _pipelineConfig.value.exposureBias
        )

        val timerSeconds = _uiState.value.timerMode.seconds

        viewModelScope.launch {
            if (timerSeconds > 0) {
                for (s in timerSeconds downTo 1) {
                    _uiState.value = _uiState.value.copy(timerCountdownValue = s)
                    kotlinx.coroutines.delay(1000)
                }
                _uiState.value = _uiState.value.copy(timerCountdownValue = 0)
            }

            val activeHdrMode = if (activeMode == CaptureMode.PHOTO) {
                _uiState.value.hdrMode
            } else {
                HdrMode.OFF
            }

            controller.captureBurst(
                config  = config,
                hdrMode = activeHdrMode,
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
    }

    /**
     * Changes the current flash mode setting.
     *
     * @param mode The new [FlashMode] to apply.
     */
    fun setFlashMode(mode: FlashMode) {
        _uiState.value = _uiState.value.copy(flashMode = mode)
        val prefs = getApplication<Application>().getSharedPreferences("RenCameraPrefs", android.content.Context.MODE_PRIVATE)
        prefs.edit().putString("pref_flash_mode", mode.name).apply()
    }

    /**
     * Changes the current HDR processing mode setting.
     *
     * @param mode The new [HdrMode] to apply.
     */
    fun setHdrMode(mode: HdrMode) {
        _uiState.value = _uiState.value.copy(hdrMode = mode)
        val prefs = getApplication<Application>().getSharedPreferences("RenCameraPrefs", android.content.Context.MODE_PRIVATE)
        prefs.edit().putString("pref_hdr_mode", mode.name).apply()
    }

    /**
     * Changes the current delay timer setting.
     *
     * @param mode The new [TimerMode] to apply.
     */
    fun setTimerMode(mode: TimerMode) {
        _uiState.value = _uiState.value.copy(timerMode = mode)
        val prefs = getApplication<Application>().getSharedPreferences("RenCameraPrefs", android.content.Context.MODE_PRIVATE)
        prefs.edit().putString("pref_timer_mode", mode.name).apply()
    }

    /**
     * Changes the active capture mode and configures the camera controller.
     *
     * @param mode The new [CaptureMode] to apply.
     */
    fun setCaptureMode(mode: CaptureMode) {
        _uiState.value = _uiState.value.copy(captureMode = mode)
        controller.setNightModeEnabled(mode == CaptureMode.NIGHT)
        val prefs = getApplication<Application>().getSharedPreferences("RenCameraPrefs", android.content.Context.MODE_PRIVATE)
        prefs.edit().putString("pref_capture_mode", mode.name).apply()
    }

    /**
     * Clears any active error message from the UI state.
     */
    fun clearError() {
        _uiState.value = _uiState.value.copy(errorMessage = null)
    }

    /**
     * Called when the ViewModel is no longer in use and will be destroyed.
     * Closes camera resources to prevent memory or hardware context leaks.
     */
    override fun onCleared() {
        controller.stopCamera()
        super.onCleared()
    }
}
