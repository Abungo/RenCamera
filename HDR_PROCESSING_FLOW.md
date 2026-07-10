# RenCamera HDR+ Computational Pipeline Architecture

This document provides a highly detailed, step-by-step technical explanation of the **RenCamera** High Dynamic Range (HDR+) processing flow. It traces execution from the moment the application is launched, through real-time frame buffering, user shutter invocation, off-heap data marshalling, and the native JNI C++ stages (parallel alignment, GPU-accelerated temporal fusion, demosaicing, and tone mapping) to the final output file save.

---

## Architecture Topology

![RenCamera Pipeline Topology](/home/abungo/projects/personal/RenCamera/hdr_pipeline_topology.png)

---

## Step 1: App Launch & Viewfinder Setup (`MainActivity` & `ui`)

### 1.1 Permission Guarding
Upon startup, `MainActivity` acts as the system entry point. It checks for standard `Manifest.permission.CAMERA` and, on Android 13+ (API 33+), `Manifest.permission.POST_NOTIFICATIONS`. If permissions are missing, it diverts flow to a Compose `PermissionRationale` screen:

```kotlin
// MainActivity.kt
setContent {
    val hasPermission by permissionGranted
    if (hasPermission) {
        RenCameraApp(viewModel = viewModel)
    } else {
        PermissionRationale(onRequest = {
            val permissions = mutableListOf(Manifest.permission.CAMERA)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                permissions.add(Manifest.permission.POST_NOTIFICATIONS)
            }
            permissionLauncher.launch(permissions.toTypedArray())
        })
    }
}
```

### 1.2 Viewfinder Surface Bind
Once permissions are granted, `RenCameraApp` displays. It mounts a Compose `AndroidView` holding a legacy Android `TextureView`. When the rendering surface is ready, it binds the callback to the shared `CameraViewModel`'s controller:

```kotlin
// ui/RenCameraApp.kt
TextureView(ctx).apply {
    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
        override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
            onTextureReady(st) // Triggers viewModel.controller.startCamera(st)
        }
        // ...
    }
}
```

---

## Step 2: Zero Shutter Lag Viewfinder Repeating Request

Inside `CameraController.kt`, the hardware connection is initialized via `openCamera(...)`:
1. It queries the `CameraManager` for the primary rear camera (`pickBackCamera()`). If RAW capture is enabled, it prioritizes sensors indicating `REQUEST_AVAILABLE_CAPABILITIES_RAW`.
2. It allocates a hardware-backed `ImageReader` configured with the selected size:
   - For **RAW**: `ImageFormat.RAW_SENSOR` (16-bit Bayer patterns).
   - For **YUV**: `ImageFormat.YUV_420_888` (planar YUV space).
3. It sets the default buffer size on the preview `SurfaceTexture` to align aspect ratios and creates a `CameraCaptureSession`.

```kotlin
// CameraController.kt
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
```

### 2.1 Repeating Request Settings
To support zero shutter lag (ZSL), the repeating viewfinder request continuously writes frames to both the preview surface and the `ImageReader` surface.
- **Highlight Protection:** It forces a static exposure compensation bias of **–1.5 EV** (`CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION`) to prevent sensor channels from saturating under normal lighting conditions.
- **Auto Focus:** It enables continuous picture auto focus (`CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE`).

```kotlin
// CameraController.kt
val requestBuilder = cameraDevice!!.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
    addTarget(previewSurface)
    addTarget(imageReader!!.surface)
    set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, evSteps)
    set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
    set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
}
session.setRepeatingRequest(requestBuilder.build(), captureCallback, cameraHandler)
```

### 2.2 ZSL Ring Buffer Pairing
As frames are captured, the JNI hardware pipeline fires async callbacks. Because `Image` availability and `TotalCaptureResult` metadata arrive independently, they are synchronized using their hardware timestamps inside `tryMatchAndPushFrame(...)`:

```kotlin
// CameraController.kt
private fun tryMatchAndPushFrame(timestamp: Long) {
    val img = pendingImages[timestamp]
    val meta = pendingMetadata[timestamp]
    if (img != null && meta != null) {
        pendingImages.remove(timestamp)
        pendingMetadata.remove(timestamp)
        
        // ... Determine ISO, exposure time, and sensor noise profile ...
        ringBuffer.push(CapturedFrame(img, iso, expTime, noiseProfile))
    }
}
```

The thread-safe `FrameRingBuffer` houses frames in a FIFO queue of fixed capacity (default is 15). When the queue overflow limit is hit, the oldest `Image` is evicted and immediately closed outside the monitor lock to prevent deadlocks:

```kotlin
// FrameRingBuffer.kt
fun push(frame: CapturedFrame) {
    val evicted: CapturedFrame?
    synchronized(lock) {
        evicted = if (queue.size >= maxSize) queue.removeFirst() else null
        queue.addLast(frame)
    }
    evicted?.image?.close() // Release buffer slot back to ImageReader pool
}
```

---

## Step 3: Shutter Press & Off-Heap Marshalling

When the user taps the shutter:
1. **CameraViewModel** sets the UI state `isProcessing = true`, halting further shutter interactions.
2. It invokes `controller.captureBurst(...)`.
3. `CameraController` immediately stops repeating capture sessions to lock exposure registers: `captureSession?.stopRepeating()`.
4. **Burst Strategy Selection:**
   - **Low Light / Night Mode:** It flushes the ring buffer and configures a manual hardware burst capture of `captureFrameCount` frames (typically 15). The integration time is locked to **125ms** (`TEMPLATE_MANUAL`) and the camera hardware ISO is divided to capture clean shadow detail, computing a corresponding software/digital gain factor:
     ```kotlin
     val requests = List(numFrames) {
         cameraDevice!!.createCaptureRequest(CameraDevice.TEMPLATE_MANUAL).apply {
             addTarget(imageReader!!.surface)
             set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
             set(CaptureRequest.SENSOR_EXPOSURE_TIME, targetExposureTime)
             set(CaptureRequest.SENSOR_SENSITIVITY, targetIso)
         }.build()
     }
     session.captureBurst(requests, burstMetadataListener, cameraHandler)
     ```
   - **Daylight Mode:** It simply drains the existing high-speed frames from the `FrameRingBuffer` via `ringBuffer.snapshot()`.

### 3.1 Direct JVM-to-Native Memory Copy
To bypass JVM garbage collection latency and prevent memory exhaustion due to multi-plane image references, the pixel byte planes are marshalled into JVM direct ByteBuffers and copied to the C++ heap.
For **RAW Sensor** captures (which can be blocked from user-space address space by vendor drivers), the frame is copied into a pre-allocated JVM direct buffer pool before calling JNI:

```kotlin
// CameraController.kt
val rawBuf = planes[0].buffer
val directBuf = rawBufferPool[idx]
directBuf.clear()
val oldPos = rawBuf.position()
directBuf.put(rawBuf)
rawBuf.position(oldPos)
directBuf.flip()
yPlanes[idx] = directBuf
```

Then, the static JNI bridge `NativeEngine.copyBurst(...)` is called:

```kotlin
// NativeEngine.kt
external fun copyBurst(
    count: Int,
    yPlanes: Array<ByteBuffer>,
    uPlanes: Array<ByteBuffer>,
    vPlanes: Array<ByteBuffer>,
    yRowStrides: IntArray,
    uvRowStrides: IntArray,
    uvPixelStrides: IntArray,
    width: Int, height: Int
): Long
```

In the JNI implementation inside `gcam_core.cpp`, `copyBurst` extracts the raw C++ memory pointers using `GetDirectBufferAddress` and duplicates the allocations on the native heap:

```cpp
// gcam_core.cpp
const uint8_t* yPtr = static_cast<const uint8_t*>(env->GetDirectBufferAddress(yBuf));
// ... Allocation and assign vectors inside a heap-allocated CopiedBurst struct
CopiedFrame& f = burst->frames[i];
f.yData.assign(yPtr, yPtr + copyY);
// ... Returns long handle pointer
return reinterpret_cast<jlong>(burst);
```

Immediately after `copyBurst` returns, the JVM calls `.close()` on all origin `Image` instances. This releases hardware registers back to the camera driver, allowing the viewfinder preview repeating request to be safely restarted on the main UI thread.

---

## Step 4: Background Processing Queue

The native handle is wrapped in a `ProcessingJob` and pushed to the background `ProcessingService.kt`:

```kotlin
// ProcessingService.kt
private val jobChannel = Channel<String>(Channel.UNLIMITED)
// ...
override fun onCreate() {
    super.onCreate()
    serviceScope.launch {
        for (jobId in jobChannel) {
            processJobInternal(jobId) // Runs pipeline sequentially
        }
    }
}
```

The service runs in the foreground (`startForeground`) with a persistent progress notification. It calls the native computational block `NativeEngine.processCopiedBurst(...)` to start the execution.

---

## Step 5: C++ Computational Pipeline Stages

In `gcam_core.cpp`, the JNI wrapper maps the native handles and metadata into a `FrameContext` object, which is then passed to the `Pipeline`:

```cpp
// gcam_core.cpp
Pipeline pipeline = buildPipeline(f_align, f_fuse, f_debayer, f_tonemap, f_encode);
bool ok = pipeline.run(ctx, progressCb);
```

### 5.1 Reference Frame Selection
Before executing the pipeline, the C++ engine determines which frame in the burst is the sharpest. It computes a gradient sharpness score over a subsampled grid:

```cpp
// gcam_core.cpp
static float computeSharpness(const YuvFrame& f) {
    double gradSum = 0;
    int count = 0;
    for (int r = 0; r < f.height - 1; r += 8) {
        const uint8_t* row = f.yPlane + r * f.yRowStride;
        const uint8_t* nextRow = row + f.yRowStride;
        for (int c = 0; c < f.width - 1; c += 8) {
            float diffX = std::abs(static_cast<float>(row[c]) - static_cast<float>(row[c + 1]));
            float diffY = std::abs(static_cast<float>(row[c]) - static_cast<float>(nextRow[c]));
            gradSum += diffX + diffY;
            count++;
        }
    }
    return count > 0 ? static_cast<float>(gradSum / count) : 0.f;
}
```

The frame with the highest sharpness score is swapped to index `0` and serves as the reference coordinate frame for alignment and temporal fusion.

---

### 5.2 Stage 1: AlignStage (Parallel Coarse-to-Fine Alignment)

To handle camera shake and subject motion, `AlignStage::process` aligns all source frames to the selected reference frame in parallel.

1. **Downsampling:** To speed up computation, the high-resolution input frames are downsampled 4x using a fast 4x4 box average filter:
   ```cpp
   // stages/align_stage.cpp
   float sum = 0.0f;
   for (int dy = 0; dy < 4; ++dy) {
       const uint16_t* srcRow = rawBase + (4 * row + dy) * strideElements;
       sum += srcRow[4 * col + 0] + srcRow[4 * col + 1] + srcRow[4 * col + 2] + srcRow[4 * col + 3];
   }
   dst[col] = sum / 16.0f;
   ```
2. **Pyramid Construction:** A 3-level Gaussian Pyramid is constructed for each downsampled frame. Rows and columns are blurred using a separable Gaussian kernel `[1 4 6 4 1]/16` (optimized with Highway SIMD) and then downsampled by 2x:
   ```cpp
   // stages/align_stage.cpp
   tmp[x] = (clamp(x-2) + 4*clamp(x-1) + 6*row[x] + 4*clamp(x+1) + clamp(x+2)) / 16.f;
   ```
3. **Hierarchical Block Matching:**
   - **Level 2 (Coarsest, 1/16 resolution):** Performs a full search within a range of `±16` pixels. Sum of Squared Differences (SSD) is computed using ARM NEON instructions:
     ```cpp
     // stages/align_stage.cpp
     float32x4_t r_val = vld1q_f32(ref + i);
     float32x4_t s_val = vld1q_f32(src + i);
     float32x4_t diff = vsubq_f32(r_val, s_val);
     sum_vec = vmlaq_f32(sum_vec, diff, diff); // Accumulates squared diffs
     ```
     To avoid alignment failures in low-light, high-noise areas, a quadratic motion regularization penalty is added to bias search blocks toward zero displacement:
     ```cpp
     int penalty = static_cast<int>(regFactor * (dx * dx + dy * dy));
     int cost = ssd + penalty;
     ```
   - **Level 1 and 0:** The motion vectors from the coarser level are scaled up by 2x to serve as initial search offsets (hints). The search is then refined within a tighter window of `±4` pixels.
4. **Resolution Restoration:** The final Level 0 motion vectors are scaled up by 4x to match the original input resolution, providing sub-pixel alignment accuracy.

---

### 5.3 Stage 2: FusionStage (Headless OpenGL ES 3.1 Compute Shader)

Once aligned, the frames must be temporally merged. To execute this computationally expensive step in real-time, `FusionStage` offloads the work to the GPU using a headless EGL/GLES context.

1. **GPU Context Setup:** An EGL display is initialized using a virtual pixel buffer:
   ```cpp
   eglGetDisplay(EGL_DEFAULT_DISPLAY);
   eglCreatePbufferSurface(display, config, attribs);
   ```
2. **Resource Uploads:**
   - Input frames are uploaded to a 3D `GL_TEXTURE_2D_ARRAY`.
   - Motion vectors are packed and uploaded to a second `GL_TEXTURE_2D_ARRAY` using floating-point formats (`GL_RG32F`).
   - Noise lookup tables (LUTs) based on sensor ISO parameters are uploaded as a 2D texture.
3. **Temporal Wiener Fusion Shader:** The GPU compute shader processes pixels in 16x16 local workgroups. It computes a patch-based Wiener filter residual relative to the reference frame.

```glsl
// stages/fusion_stage.cpp COMPUTE_SHADER_SRC
void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= u_width || pos.y >= u_height) return;

    float w_inv = 1.0 / float(u_width);
    float h_inv = 1.0 / float(u_height);
    vec2 uv = (vec2(pos) + 0.5) * vec2(w_inv, h_inv);

    // 1. Sample 3x3 patch on reference frame to suppress luma noise
    float ref_val = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 p_uv = (vec2(pos) + vec2(dx, dy) + 0.5) * vec2(w_inv, h_inv);
            ref_val += texture(u_input_frames, vec3(p_uv, 0.0)).r * 255.0;
        }
    ref_val /= 9.0;

    float acc = texture(u_input_frames, vec3(uv, 0.0)).r * 255.0;
    float wgt = 1.0;
    float total_frame_weight = 1.0;

    // 2. Accumulate aligned frames
    for (int f = 1; f < u_num_frames; ++f) {
        float bx = (float(pos.x) - u_block_size * 0.5) / u_block_size;
        float by = (float(pos.y) - u_block_size * 0.5) / u_block_size;
        vec2 mv_uv = vec2((bx + 0.5) / float(u_blocks_wide), (by + 0.5) / float(u_blocks_tall));

        // Sample interpolated motion vector
        vec2 mv = texture(u_motion_fields, vec3(mv_uv, float(f - 1))).rg;

        // Sample 3x3 patch on warped source frame
        float warped_val = 0.0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                vec2 p_uv = (vec2(pos) + mv + vec2(dx, dy) + 0.5) * vec2(w_inv, h_inv);
                warped_val += texture(u_input_frames, vec3(p_uv, float(f))).r * 255.0;
            }
        warped_val /= 9.0;

        float residual = warped_val - ref_val;

        // Read noise variance parameter (sigma) based on local intensity
        int ref_idx = clamp(int(round(ref_val)), 0, 255);
        float inv_2sigma2 = texelFetch(u_noise_luts, ivec2(ref_idx, f), 0).r;

        // Compute Wiener blending weight
        float w_val = exp(-residual * residual * inv_2sigma2);

        // Ghosting Rejection: Zero out weights for moving subjects
        w_val = (w_val < 0.05) ? 0.0 : w_val;

        float center_warped = texture(u_input_frames, vec3((vec2(pos) + mv + 0.5) * vec2(w_inv, h_inv), float(f))).r * 255.0;
        acc += center_warped * w_val;
        wgt += w_val;
        total_frame_weight += w_val;
    }

    uint idx = uint(pos.y * u_width + pos.x);
    outY[idx] = uint(clamp((wgt > 1e-6) ? acc / wgt : acc, 0.0, 255.0));
    outWeights[idx] = total_frame_weight / float(u_num_frames);
}
```

The computed luma weights are passed to the chroma channels during U/V plane fusion. In regions where ghosting is detected (weight < 0.5), the pipeline falls back to the reference frame's chroma to prevent color fringing.

---

### 5.4 Stage 3: DebayerStage (Sabre Multi-Frame Demosaicing)

If RAW capture was selected, the temporally fused frame is debayered using an edge-adaptive multi-frame super-resolution filter called **Sabre Demosaicing**. This stage is also executed on the GPU using a compute shader:

```glsl
// stages/debayer_stage.cpp COMPUTE_SABRE_SRC
float getRawCorrected(ivec2 p, int frameIdx) {
    int cx = clamp(p.x, 0, u_width - 1);
    int cy = clamp(p.y, 0, u_height - 1);
    uint val = texelFetch(u_raw_frames, ivec3(cx, cy, frameIdx), 0).r;
    float cleanVal = max(0.0, (float(val) - u_black_level) * u_scale);

    // Neutralize color filter array sensor layouts (BGGR structure)
    bool isEvenRow = (cy % 2 == 0);
    bool isEvenCol = (cx % 2 == 0);
    if (isEvenRow && isEvenCol) {
        return cleanVal * u_b_gain; // Blue channel
    } else if (!isEvenRow && !isEvenCol) {
        return cleanVal * u_r_gain; // Red channel
    } else {
        return cleanVal * u_g_gain; // Green channel
    }
}

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= u_width || pos.y >= u_height) return;

    // Compute edge gradient directions on reference frame to steer filtering kernel
    float gLeft = getRawCorrected(pos + ivec2(-1, 0), 0);
    float gRight = getRawCorrected(pos + ivec2(1, 0), 0);
    float gUp = getRawCorrected(pos + ivec2(0, -1), 0);
    float gDown = getRawCorrected(pos + ivec2(0, 1), 0);
    float edgeStrength = abs(gLeft - gRight) + abs(gUp - gDown);
    // Smooths flat areas (exponent=2.0) and sharpens high-frequency edges (exponent=8.0)
    float kExponent = mix(2.0, 8.0, clamp(edgeStrength / 15.0, 0.0, 1.0));

    float sumR = 0.0, weightR = 0.0;
    float sumG = 0.0, weightG = 0.0;
    float sumB = 0.0, weightB = 0.0;

    // Accumulate weighted local colors across all non-ghosted frames at subpixel alignment offsets
    for (int f = 0; f < u_num_frames; ++f) {
        vec2 mv = (f == 0) ? vec2(0.0) : texture(u_motion_fields, vec3(pos, float(f - 1))).rg;
        vec2 targetPos = vec2(pos) + mv;
        ivec2 centerIdx = ivec2(round(targetPos.x), round(targetPos.y));

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                ivec2 samplePos = centerIdx + ivec2(dx, dy);
                float val = getRawCorrected(samplePos, f);
                int color = getPixelColor(samplePos);

                vec2 delta = vec2(samplePos) - targetPos;
                float dist2 = dot(delta, delta);
                float spatialW = exp(-dist2 * kExponent);

                if (color == 0) {
                    sumR += val * spatialW; weightR += spatialW;
                } else if (color == 1) {
                    sumG += val * spatialW; weightG += spatialW;
                } else {
                    sumB += val * spatialW; weightB += spatialW;
                }
            }
        }
    }
    
    // Fallback: If a color channel has insufficient samples, apply high-quality local bilinear interpolation
    // ...
}
```

In standard YUV mode, `DebayerStage` bypasses Sabre demosaicing and converts YUV420 to RGB24 in parallel on CPU threads using BT.601 color conversion:

```cpp
// stages/debayer_stage.cpp
static void convertRowScalar(
    const uint8_t* yRow, const uint8_t* uRow, const uint8_t* vRow,
    uint8_t* rgbRow, int width) 
{
    for (int x = 0; x < width; ++x) {
        int Y = yRow[x];
        int U = uRow[x >> 1] - 128;
        int V = vRow[x >> 1] - 128;

        int R = Y + (179 * V + 64) / 128;
        int G = Y - (44 * U + 91 * V + 64) / 128;
        int B = Y + (227 * U + 64) / 128;

        rgbRow[x*3 + 0] = std::clamp(R, 0, 255);
        rgbRow[x*3 + 1] = std::clamp(G, 0, 255);
        rgbRow[x*3 + 2] = std::clamp(B, 0, 255);
    }
}
```

---

### 5.5 Stage 5: ToneMapStage (Dynamic Range Compression & Local Contrast)

The output of the debayer stage is a high dynamic range (HDR) color image. The **ToneMapStage** compresses this range into a standard 8-bit image while preserving local details and contrast.

1. **Auto White Balance:** Applies Grey-World white balance correction. The computed color gains are softened using configuration parameters (`awbSoftness`) to preserve ambient lighting:
   ```cpp
   gainR = 1.0 + alpha * (gainR - 1.0);
   gainB = 1.0 + alpha * (gainB - 1.0);
   ```
2. **Adaptive Gamma Calculation:** Calculates the average scene luminance (key) to adapt the tone mapping curve:
   ```cpp
   // Darker scenes get a lower gamma value to boost shadow details
   float adaptiveGamma = gammaMin + t_mean * (gammaMax - gammaMin);
   ```
3. **Bilateral Filtering (Detail Separation):**
   - A single-pass bilateral filter runs on the GPU using a search radius of `12` pixels to separate the image into a **base layer** (low-frequency luma) and a **detail layer** (high-frequency textures):
     ```glsl
     float w = exp(-dS2 / spatial_sigma2) * exp(-dR2 / range_sigma2);
     sumVal += w * nL;
     ```
   - Separating details in the log-luminance domain prevents halo artifacts around high-contrast edges:
     ```glsl
     float logDetail = logL - logBase;
     ```
4. **Dynamic Range Compression:** The base layer is compressed using a filmic curve approximation (ACES curve fitting):
   ```glsl
   float normBase = baseL / 255.0;
   float boostedBase = pow(normBase, u_adaptive_gamma);
   float compBase = acesFilm(boostedBase) * 255.0;
   ```
5. **Detail Recomposition:** The log details are amplified by `u_detail_alpha` and recombined with the compressed base layer:
   ```glsl
   float compLogL = log2(compBase + 1.0) + logDetail * currentDetailAlpha;
   float compL = clamp(pow(2.0, compLogL) - 1.0, 0.0, 255.0);
   ```
6. **Luma-Safe Saturation Boost:** RGB channels are scaled to match the new local tone-mapped luminance. Color saturation is boosted using a luma-preserving interpolation:
   ```glsl
   oRgb = newL + factor * (oRgb - newL);
   ```
7. **Bilateral Denoising:** A final 5x5 bilateral filter pass is run on the GPU to suppress residual high-frequency luma and chroma noise.

---

### 5.6 Stage 6: EncodeStage (libjpeg-turbo Compression)
The final processed RGB image is converted into a standard JPEG byte stream.
- The pipeline utilizes **libjpeg-turbo** JNI bindings for fast, hardware-accelerated JPEG compression.
- It sets the destination target quality (`jpegQuality`, default 95%) and outputs the compressed byte array back to the Java layer.

---

## Step 6: File Storage and Memory Release

Once `processCopiedBurst` completes:
1. **PhotoSaver.kt** receives the JPEG bytes and writes them to the Android MediaStore under `Pictures/RenCamera/` or `DCIM/Camera`.
2. The UI is notified of the completed URI and updates the gallery thumbnail.
3. The native memory allocations are freed via `NativeEngine.freeBurst(handle)` to prevent heap leaks:

```kotlin
// ProcessingService.kt
finally {
    NativeEngine.freeBurst(job.nativeBurstHandle) // Deletes CopiedBurst heap reference
    ProcessingManager.removeJob(jobId)
}
```

This completes the lifecycle of a single HDR capture in RenCamera.

---

## HDR+ Pipeline Techniques & Tuning Guide

This section explains the core computational photography techniques implemented in RenCamera and how to tweak their parameters (either via `PipelineConfig` or direct code adjustments) to tune the final image quality.

---

### Technique 1: Zero Shutter Lag (ZSL) & Highlight Protection

#### How it works:
Standard cameras suffer from shutter lag because they only start capturing frames after the shutter is pressed. RenCamera implements **Zero Shutter Lag (ZSL)** by running a continuous viewfinder preview loop that streams frames to a rolling **FrameRingBuffer** in RAM. When the user taps shutter, the frames are already in memory.

To preserve highlight details (which clip permanently when oversaturated), the viewfinder runs with a constant underexposure bias (typically **–1.5 EV**). This shifts the dynamic range window upward. The shadow details lost to underexposure are later recovered by temporal fusion and digital gain boosting.

#### How to tweak:
*   **`exposureBias`** (Config: `exposureBias`, range: `-3.0f` to `3.0f` EV):
    *   *Lower values (e.g., -2.0 EV):* Increases highlight protection in high-contrast scenes (e.g., sunset skies) but darkens shadows, requiring more aggressive digital gain.
    *   *Higher values (e.g., -0.5 EV):* Suitable for low-contrast or flat lighting, making shadows cleaner but risking highlight blowout.
*   **`captureFrameCount`** (Config: `captureFrameCount`, range: `5` to `25`):
    *   *More frames (e.g., 20+):* Increases the Signal-to-Noise Ratio (SNR) in shadows, allowing cleaner shadow recovery, but increases processing time and memory usage.

---

### Technique 2: Coarse-to-Fine Parallel Alignment

#### How it works:
To merge multiple hand-held frames without blur, the pipeline must align them. **AlignStage** uses a **3-level Gaussian Pyramid** downsampled 4x:
1. It downsamples the image and blurs it horizontally and verticaly with a `[1 4 6 4 1]/16` filter.
2. **Coarse level (Level 2):** Matches blocks (16x16 pixels scaled) using a full search window (`±16` pixels) to detect large displacements (hand shake).
3. **Refinement levels (Level 1 & 0):** Uses the motion vector from the parent level as a starting point (hint) and refines alignment within a smaller window (`±4` pixels) to capture micro-motion.
4. **ARM NEON Acceleration:** SSD (Sum of Squared Differences) is computed in parallel vector registers on the CPU.
5. **Regularization:** Adds a cost penalty for displacements that deviate from the parent level prediction. This stabilizes search blocks in flat or high-noise areas (preventing alignment on noise patterns).

```cpp
int penalty = static_cast<int>(regFactor * (dx * dx + dy * dy));
```

#### How to tweak:
*   **`alignmentRegularization`** (Config: `alignmentRegularization`, range: `0.0f` to `20.0f`):
    *   *Increase (e.g., 10.0 - 15.0):* Constrains motion vectors to favor zero-displacement. Use this if you see structural warping or alignment artifacts in dark, noisy regions.
    *   *Decrease (e.g., 1.0 - 3.0):* Allows more flexible block matching. Useful for fast-moving subjects but can lead to alignment errors in noisy scenes.

---

### Technique 3: Temporal Wiener Fusion

#### How it works:
Alignment is rarely perfect due to subject motion (e.g., moving cars, branches). Blending all pixels blindly causes "ghosting" artifacts. **FusionStage** uses **Wiener Filtering** to temporally merge frames:
- It estimates the expected sensor noise floor ($\sigma$) based on the current ISO.
- It computes a pixel-by-pixel residual difference relative to the reference frame.
- If the difference is small (within the expected noise floor), it assigns a high weight to blend the pixel and reduce noise.
- If the difference is large (subject motion), it reduces the weight to zero (hard ghost rejection), falling back to the reference frame.

#### How to tweak:
*   **`fusionNoiseMultiplier`** (Config: `fusionNoiseMultiplier`, range: `1.0f` to `6.0f`):
    *   *Increase (e.g., 4.0 - 5.0):* Blends noisier pixels. This increases noise reduction in shadows but may cause motion blur or ghosting trails.
    *   *Decrease (e.g., 1.5 - 2.0):* Rejects motion aggressively. Eliminates ghosting but leaves shadows noisier since fewer frames are merged.

---

### Technique 4: Sabre Multi-Frame Demosaicing (Super-Resolution)

#### How it works:
Bayer sensors capture only one color channel per pixel (Red, Green, or Blue). Standard demosaicing interpolates the missing channels locally. **Sabre Demosaicing** uses sub-pixel motion vectors from multiple frames to reconstruct colors:
- It projects sub-pixel alignment offsets back into the Bayer grid.
- It accumulates actual red, green, and blue values directly from adjacent frames, reducing interpolation artifacts.
- It uses edge-adaptive steering (calculating horizontal/vertical gradients) to dynamically steer the interpolation kernel:

```glsl
float edgeStrength = abs(gLeft - gRight) + abs(gUp - gDown);
float kExponent = mix(2.0, 8.0, clamp(edgeStrength / 15.0, 0.0, 1.0));
float spatialW = exp(-dist2 * kExponent);
```

#### How to tweak:
*   **`useRawCapture`** (Config: `useRawCapture`):
    *   *Enabled:* Bypasses the camera hardware's default demosaicing and runs the C++ Sabre algorithm. Reconstructs finer textures and resolves sub-pixel details but increases JNI copy and processing times.

---

### Technique 5: Bilateral Detail Separation & Tone Mapping

#### How it works:
To render high dynamic range on standard 8-bit displays without looking flat, the pipeline uses **Bilateral Detail Separation**:
1. It applies a **Bilateral Filter** to separate the image into:
   - **Base Layer:** The low-frequency lighting (contrast).
   - **Detail Layer:** The high-frequency textures (pores, hair, fabric).
2. It compresses the base layer using an **ACES Filmic Curve** to fit within [0, 255] while preserving highlights.
3. It amplifies the detail layer (which remains uncompressed) and adds it back to the compressed base layer. This preserves local micro-contrast without creating halos.
4. **Sky Black-Point Clamping:** Compresses dark skies to true black to eliminate noise in dark backgrounds.

#### How to tweak:
*   **`detailAlpha`** (Config: `detailAlpha`, range: `0.5f` to `2.0f`):
    *   *Increase (e.g., 1.4 - 1.8):* Sharpens micro-textures, giving a crisp, highly detailed look. Too high causes a harsh, over-processed appearance.
    *   *Decrease (e.g., 0.8 - 1.0):* Softens details for a smoother, more portrait-friendly look.
*   **`saturationBoost`** (Config: `saturationBoost`, range: `0.5f` to `2.0f`):
    *   *Increase (e.g., 1.2 - 1.4):* Boosts colors using a luma-safe interpolation to prevent color clipping.
*   **`blackPointClamp`** (Config: `blackPointClamp`, range: `0.0f` to `0.5f`):
    *   *Increase (e.g., 0.15 - 0.25):* Clamps dark shadow areas (e.g., night sky) to clean black, hiding sensor grain.

