package com.avatarforge.sdk.render

/** Result of starting gltfio's asynchronous resource load, separated for deterministic JVM testing. */
internal sealed interface ResourceLoadStartOutcome {
    data object Started : ResourceLoadStartOutcome
    data object Rejected : ResourceLoadStartOutcome
    data class Failed(val failure: Throwable) : ResourceLoadStartOutcome
}

/**
 * Starts a resource load and invokes [cleanup] exactly once if the loader rejects or throws while
 * starting. Callers supply render-thread cleanup; this helper deliberately preserves the failure.
 */
internal inline fun beginResourceLoadSafely(
    begin: () -> Boolean,
    cleanup: () -> Unit,
): ResourceLoadStartOutcome {
    val started = try {
        begin()
    } catch (failure: OutOfMemoryError) {
        cleanup()
        return ResourceLoadStartOutcome.Failed(failure)
    } catch (failure: Exception) {
        cleanup()
        return ResourceLoadStartOutcome.Failed(failure)
    }

    if (started) return ResourceLoadStartOutcome.Started
    cleanup()
    return ResourceLoadStartOutcome.Rejected
}
