package com.avatarforge.app

import android.graphics.Bitmap
import com.avatarforge.sdk.AvatarEngine
import com.avatarforge.sdk.ImageReference
import com.avatarforge.sdk.ImageRole
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext

/** Runs image decode, packing, and native analysis away from the UI thread. */
internal suspend fun <T> withReferenceImageIo(block: suspend CoroutineScope.() -> T): T =
    withContext(Dispatchers.IO, block)

/** Returns null on an ordinary analysis failure; cancellation is always propagated to the caller. */
internal suspend fun analyseReferenceImages(
    engine: AvatarEngine,
    bitmapA: Bitmap,
    bitmapB: Bitmap?,
): List<ImageReference>? = withReferenceImageIo {
    try {
        currentCoroutineContext().ensureActive()
        val refA = ImageReference.fromBitmap(bitmapA, ImageRole.IDENTITY)
        val refB = bitmapB?.let { ImageReference.fromBitmap(it, ImageRole.CLOTHING) }
        currentCoroutineContext().ensureActive()
        engine.analyseImages(listOfNotNull(refA, refB))
    } catch (cancelled: CancellationException) {
        throw cancelled
    } catch (_: Exception) {
        null
    }
}
