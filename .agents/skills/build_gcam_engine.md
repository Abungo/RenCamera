# Rule Profile: `build_gcam_engine`

**Mode:** Always On

**Description:** Architectural specification to build a high-performance Zero Shutter Lag (ZSL) computational photography pipeline from an empty shell.

---

# Environment Scaffolding

Initialize an Android project structure targeting **Compile SDK 34+**.

Link the Android Native Development Kit (NDK) by creating:

```
src/main/cpp/CMakeLists.txt
```

Declare it in the app module's `build.gradle.kts`:

```kotlin
android {
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}
```

Generate a **single-activity Jetpack Compose** application that contains:

- Full-screen preview UI
- `AndroidView`
- `SurfaceView` or `TextureView`
- Hardware camera preview rendering

---

# Low-Level Capture Pipeline (Kotlin Layer)

Use **android.hardware.camera2** exclusively.

**Do not generate CameraX references.**

Configure a `CameraCaptureSession` with two simultaneous output targets:

1. Low-resolution surface for the interactive viewfinder.
2. High-resolution `ImageReader` configured as:

```
ImageFormat.YUV_420_888
```

using the maximum physical resolution of the primary camera sensor.

---

## Ring Buffer

Create a thread-safe FIFO ring buffer:

- Type: `ArrayDeque`
- Maximum history: **12 frames**

---

## Exposure

Apply a persistent exposure compensation:

```kotlin
CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION
```

Value:

```
-1.5 EV
```

to reduce highlight clipping.

---

## Burst Capture

When the shutter button is pressed:

- Freeze the latest 12 frame references
- Keep them resident in RAM
- Pass the frozen burst to the JNI layer

---

# Unidirectional Data & Memory Pipeline (JNI Bridge)

Create a JNI wrapper:

```kotlin
package com.camera.app

object NativeEngine {

    init {
        System.loadLibrary("gcam_core")
    }

    external fun processBurst(images: Array<*>): Int
}
```

Within the native layer:

- Receive the image array
- Access image data using NDK image APIs
- Retrieve hardware buffers
- Lock buffers
- Obtain raw pointers for:
  - Y plane
  - U plane
  - V plane

using:

- `AImage_getHardwareBuffer`
- `AHardwareBuffer_lock`

---

# Native Math Core Execution (C++ / Halide)

Execute the processing pipeline in the following order.

---

## 1. Pyramidal Block Alignment (Anti-Ghosting)

- Divide each frame into **16×16 blocks**
- Build a **3-level Gaussian Pyramid**
- Estimate block displacement
- Align all frames at macro and micro scales

---

## 2. Temporal Frame Fusion

Implement a vectorized Halide pipeline that:

- Processes all 12 aligned frames
- Computes temporal pixel averages
- Uses parallel execution
- Reduces random shadow noise

---

## 3. De-Bayer / Color Reconstruction

Reconstruct full RGB data from the fused luminance and chroma information.

---

## 4. Bilateral Grid Tone Mapping

Separate the image into:

- Base illumination layer
- High-frequency detail layer

Process as follows:

1. Compress the base layer.
2. Recover shadow detail.
3. Reapply the texture layer.
4. Preserve local contrast.

---

# Output Encoding

Compress the processed RGB image into JPEG using:

- **libjpeg-turbo**

---

# Cleanup & Lifecycle

Before returning from JNI:

- Unlock every hardware buffer:

```
AHardwareBuffer_unlock(...)
```

for all 12 input images.

Return:

```
0
```

to Kotlin.

---

## Kotlin Cleanup

After successful native processing:

Loop through the captured images:

```kotlin
image.close()
```

Close every `Image` explicitly to prevent Camera2 resource lockups.
