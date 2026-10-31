package com.avatarforge.app

import java.util.concurrent.CancellationException

/** What the progress screen's back/cancel action should do while a job is still awaiting completion. */
internal enum class ProgressExitAction { WAIT_FOR_COMPLETION, RETURN_TO_EDITOR }

internal enum class ProgressDestination { PROGRESS, VIEWER }

internal data class ProgressJobSnapshot(
    val running: Boolean,
    val cancelled: Boolean,
    val failed: Boolean,
    val error: String?,
)

internal data class ProgressExitTransition(
    val action: ProgressExitAction,
    val uiState: GenerationProgressUiState,
)

internal sealed interface ProgressAwaitResolution<out T> {
    val uiState: GenerationProgressUiState
    val destination: ProgressDestination
    val result: T?

    data class Success<T>(
        override val result: T,
        override val uiState: GenerationProgressUiState,
    ) : ProgressAwaitResolution<T> {
        override val destination = ProgressDestination.VIEWER
    }

    data class Failure(
        override val uiState: GenerationProgressUiState,
    ) : ProgressAwaitResolution<Nothing> {
        override val destination = ProgressDestination.PROGRESS
        override val result: Nothing? = null
    }
}

internal fun progressExitTransition(
    hasJob: Boolean,
    jobRunning: Boolean,
    current: GenerationProgressUiState,
    cancelJob: () -> Unit,
): ProgressExitTransition {
    /* A finished status is not enough: await() may still be about to return a valid EngineResult. */
    val action = if (!hasJob || current.awaitCompleted) {
        ProgressExitAction.RETURN_TO_EDITOR
    } else {
        ProgressExitAction.WAIT_FOR_COMPLETION
    }
    val next = if (action == ProgressExitAction.WAIT_FOR_COMPLETION && !current.stopping && jobRunning) {
        cancelJob()
        current.requestStop()
    } else {
        current
    }
    return ProgressExitTransition(action, next)
}

internal fun <T> resolveProgressAwait(
    current: GenerationProgressUiState,
    status: ProgressJobSnapshot,
    outcome: Result<T>,
): ProgressAwaitResolution<T> = outcome.fold(
    onSuccess = { generated ->
        ProgressAwaitResolution.Success(generated, current.completeAwait(status, failure = null))
    },
    onFailure = { failure ->
        ProgressAwaitResolution.Failure(current.completeAwait(status, failure))
    },
)

/** UI-only terminal details for cases where JobStatus alone is not enough to describe await(). */
internal data class GenerationProgressUiState(
    val stopping: Boolean = false,
    val awaitCompleted: Boolean = false,
    val awaitFailure: String? = null,
    val cancellationObservedByAwait: Boolean = false,
) {
    fun requestStop(): GenerationProgressUiState = copy(
        stopping = true,
        awaitCompleted = false,
        awaitFailure = null,
        cancellationObservedByAwait = false,
    )

    fun completeAwait(status: ProgressJobSnapshot, failure: Throwable?): GenerationProgressUiState {
        val nonCancellationFailure = failure?.takeUnless { it is CancellationException }
        val detail = nonCancellationFailure?.let { throwable ->
            throwable.message?.trim()?.takeIf(String::isNotEmpty)
                ?: throwable.javaClass.simpleName.takeIf(String::isNotEmpty)
                ?: "Unexpected generation failure"
        }
        return copy(
            stopping = false,
            awaitCompleted = true,
            awaitFailure = detail?.takeIf { !status.failed || status.error.isNullOrBlank() },
            cancellationObservedByAwait = failure is CancellationException && !status.cancelled,
        )
    }

    fun isTerminal(): Boolean = awaitCompleted && !stopping

    fun isCancelled(status: ProgressJobSnapshot): Boolean =
        awaitCompleted && (status.cancelled || cancellationObservedByAwait)

    fun isFailed(status: ProgressJobSnapshot): Boolean =
        awaitCompleted && (status.failed || awaitFailure != null)

    fun canRetry(status: ProgressJobSnapshot): Boolean = isCancelled(status) || isFailed(status)
}
