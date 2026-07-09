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
     * Save [jpegBytes] as a JPEG file in MediaStore.
     * @return the [Uri] of the saved image, or null on failure.
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
