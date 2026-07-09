package com.renskylab.camera

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.os.Build
import androidx.core.content.ContextCompat
import com.renskylab.camera.ui.RenCameraApp

class MainActivity : ComponentActivity() {

    private val viewModel: CameraViewModel by viewModels()

    // ── Permission launcher ────────────────────────────────────────────────────
    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        permissionGranted.value = permissions[Manifest.permission.CAMERA] ?: false
    }

    private val permissionGranted = mutableStateOf(false)

    // ─────────────────────────────────────────────────────────────────────────
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Check camera permission on launch
        val cameraGranted = ContextCompat.checkSelfPermission(
            this, Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED
        permissionGranted.value = cameraGranted

        if (cameraGranted) {
            // If camera is already granted, request notification permission if missing on Android 13+
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                if (ContextCompat.checkSelfPermission(
                        this, Manifest.permission.POST_NOTIFICATIONS
                    ) != PackageManager.PERMISSION_GRANTED
                ) {
                    permissionLauncher.launch(arrayOf(Manifest.permission.POST_NOTIFICATIONS))
                }
            }
        }

        setContent {
            val hasPermission by permissionGranted

            if (hasPermission) {
                RenCameraApp(viewModel = viewModel)
            } else {
                PermissionRationale(
                    onRequest = {
                        val permissions = mutableListOf(Manifest.permission.CAMERA)
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            permissions.add(Manifest.permission.POST_NOTIFICATIONS)
                        }
                        permissionLauncher.launch(permissions.toTypedArray())
                    }
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Re-check permission in case the user granted it from Settings
        permissionGranted.value = ContextCompat.checkSelfPermission(
            this, Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Permission rationale screen
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun PermissionRationale(onRequest: () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(24.dp),
            modifier = Modifier.padding(40.dp),
        ) {
            Text(
                text       = "📷",
                fontSize   = 64.sp,
            )
            Text(
                text       = "Camera Access Required",
                color      = Color.White,
                fontSize   = 22.sp,
                fontWeight = FontWeight.SemiBold,
                textAlign  = TextAlign.Center,
            )
            Text(
                text       = "RenCamera needs camera access to\ncapture and process HDR+ photos.",
                color      = Color(0x99FFFFFF),
                fontSize   = 15.sp,
                textAlign  = TextAlign.Center,
            )
            Button(
                onClick = onRequest,
                colors  = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF00D4FF),
                    contentColor   = Color.Black,
                ),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    text       = "Grant Permission",
                    fontWeight = FontWeight.Bold,
                    modifier   = Modifier.padding(vertical = 4.dp),
                )
            }
        }
    }
}