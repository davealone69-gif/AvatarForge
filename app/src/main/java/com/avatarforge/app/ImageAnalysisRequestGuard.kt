package com.avatarforge.app

/** Prevents an older asynchronous image analysis from committing after a newer selection. */
internal class ImageAnalysisRequestGuard {
    private var generation = 0L

    fun begin(): Long {
        generation += 1
        return generation
    }

    fun isCurrent(request: Long): Boolean = request == generation
}
