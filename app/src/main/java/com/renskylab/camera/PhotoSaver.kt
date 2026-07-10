package com.renskylab.camera

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.IOException

/**
 * Writes a JPEG [ByteArray] to the shared MediaStore Pictures collection.
 * Outputs to `Pictures/RenCamera/`.
 */
object PhotoSaver {

    /**
     * Saves the provided JPEG byte array as an image file in the shared MediaStore Pictures collection.
     * Writes output to a sub-folder under `Pictures/RenCamera/` (or standard DCIM/Camera path depending on API version).
     *
     * @param context The application or activity context.
     * @param jpegBytes The compressed JPEG image data to be saved.
     * @param filename The target display name for the saved image file.
     * @return The [Uri] pointing to the saved image in the MediaStore, or null if the operation fails.
     */
    suspend fun save(context: Context, jpegBytes: ByteArray, filename: String): Uri? =
        withContext(Dispatchers.IO) {

            val contentValues = ContentValues().apply {
                put(MediaStore.Images.Media.DISPLAY_NAME, filename)
                put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    put(MediaStore.Images.Media.RELATIVE_PATH, "DCIM/Camera")
                    put(MediaStore.Images.Media.IS_PENDING, 1)
                }
            }

            val resolver = context.contentResolver
            val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, contentValues)
                ?: return@withContext null

            try {
                resolver.openOutputStream(uri)?.use { stream ->
                    stream.write(jpegBytes)
                } ?: throw IOException("Failed to open output stream for $uri")

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    contentValues.clear()
                    contentValues.put(MediaStore.Images.Media.IS_PENDING, 0)
                    resolver.update(uri, contentValues, null, null)
                }

                uri
            } catch (e: Exception) {
                resolver.delete(uri, null, null)
                null
            }
        }
}
