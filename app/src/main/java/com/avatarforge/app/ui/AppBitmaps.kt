/*
 * AvatarForge :: AppBitmaps.kt
 *
 * Decoding helper for the reference-image pickers.
 *
 * Two things matter here.
 *
 * 1. Memory. A modern phone camera produces 12-50 MP images; decoding one at full size allocates
 *    400 MB of ARGB_8888 and the app's whole budget is 2 GB shared with a kernel that peaks around
 *    1.5 GB during a build. So the image is bounds-decoded first and then downsampled to at most
 *    1600 px on the long edge - which is far more than the analysis (a 256 px working grid) or the
 *    garment's colour statistics can use.
 *
 * 2. Orientation. The kernel expects an upright, sRGB-8 image with a top-left origin (see af_image
 *    in afcore.h). EXIF rotation is applied here, once, so nothing downstream has to guess.
 */
package com.avatarforge.app.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import androidx.exifinterface.media.ExifInterface
import java.io.IOException

object AppBitmaps {

    /** Long-edge cap for anything handed to the engine. */
    private const val MAX_EDGE = 1600

    private var appContext: Context? = null

    /** Called once from the Activity so the pickers can open content:// URIs. */
    fun install(context: Context) {
        appContext = context.applicationContext
    }

    fun decode(uri: Uri): Bitmap? {
        val context = appContext ?: return null
        return try {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
            val sample = sampleSize(bounds.outWidth, bounds.outHeight)
            val options = BitmapFactory.Options().apply {
                inSampleSize = sample
                inPreferredConfig = Bitmap.Config.ARGB_8888
            }
            val decoded = context.contentResolver.openInputStream(uri)?.use {
                BitmapFactory.decodeStream(it, null, options)
            } ?: return null
            val rotated = applyExif(context, uri, decoded)
            /* Final scale: inSampleSize only steps in powers of two, so a 12 MP photo at
               inSampleSize=8 still lands at 4000 px. */
            val scaled = scaleToMaxEdge(rotated, MAX_EDGE)
            if (scaled !== decoded) decoded.recycle()
            scaled
        } catch (t: IOException) {
            null
        } catch (t: OutOfMemoryError) {
            null
        }
    }

    private fun sampleSize(width: Int, height: Int): Int {
        var sample = 1
        var longest = maxOf(width, height)
        while (longest / 2 >= MAX_EDGE) {
            longest /= 2
            sample *= 2
        }
        return sample
    }

    private fun scaleToMaxEdge(bitmap: Bitmap, maxEdge: Int): Bitmap {
        val longest = maxOf(bitmap.width, bitmap.height)
        if (longest <= maxEdge) return bitmap
        val scale = maxEdge.toFloat() / longest.toFloat()
        return Bitmap.createScaledBitmap(
            bitmap,
            (bitmap.width * scale).toInt().coerceAtLeast(1),
            (bitmap.height * scale).toInt().coerceAtLeast(1),
            true,
        )
    }

    private fun applyExif(context: Context, uri: Uri, bitmap: Bitmap): Bitmap {
        val orientation = try {
            context.contentResolver.openInputStream(uri)?.use { ExifInterface(it).getAttributeInt(
                ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL) }
        } catch (t: IOException) {
            null
        } ?: return bitmap
        val matrix = Matrix()
        when (orientation) {
            ExifInterface.ORIENTATION_ROTATE_90 -> matrix.postRotate(90f)
            ExifInterface.ORIENTATION_ROTATE_180 -> matrix.postRotate(180f)
            ExifInterface.ORIENTATION_ROTATE_270 -> matrix.postRotate(270f)
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> matrix.postScale(-1f, 1f)
            ExifInterface.ORIENTATION_FLIP_VERTICAL -> matrix.postScale(1f, -1f)
            else -> return bitmap
        }
        val rotated = Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, matrix, true)
        if (rotated !== bitmap) bitmap.recycle()
        return rotated
    }
}
