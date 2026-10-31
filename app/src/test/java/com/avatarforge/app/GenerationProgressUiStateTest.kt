package com.avatarforge.app

import java.util.concurrent.CancellationException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GenerationProgressUiStateTest {
    @Test
    fun `back during cancellation stays on progress until await completes`() {
        val cancellationStatus = ProgressJobSnapshot(running = false, cancelled = true, failed = false, error = null)
        var cancelRequests = 0
        var transition = progressExitTransition(
            hasJob = true,
            jobRunning = true,
            current = GenerationProgressUiState(),
            cancelJob = { cancelRequests++ },
        )

        assertEquals(ProgressExitAction.WAIT_FOR_COMPLETION, transition.action)
        assertEquals(1, cancelRequests)
        assertTrue(transition.uiState.stopping)
        assertFalse(transition.uiState.isTerminal(cancellationStatus))

        transition = progressExitTransition(
            hasJob = true,
            jobRunning = false,
            current = transition.uiState,
            cancelJob = { cancelRequests++ },
        )
        assertEquals(ProgressExitAction.WAIT_FOR_COMPLETION, transition.action)
        assertEquals("do not issue repeated cancel requests", 1, cancelRequests)
        assertFalse(transition.uiState.isTerminal(cancellationStatus))

        val completed = transition.uiState.completeAwait(
            status = cancellationStatus,
            failure = CancellationException("generation cancelled"),
        )
        assertFalse(completed.stopping)
        assertTrue(completed.awaitCompleted)
        assertTrue(completed.isTerminal(cancellationStatus))
        assertTrue(completed.isCancelled(cancellationStatus))
        assertTrue(completed.canRetry(cancellationStatus))
        assertNull(completed.awaitFailure)

        transition = progressExitTransition(
            hasJob = true,
            jobRunning = false,
            current = completed,
            cancelJob = { cancelRequests++ },
        )
        assertEquals(ProgressExitAction.RETURN_TO_EDITOR, transition.action)
        assertEquals(1, cancelRequests)
    }

    @Test
    fun `unexpected non-cancellation await failure becomes visible retryable failure`() {
        val statusWithoutTerminalDetails = ProgressJobSnapshot(
            running = false,
            cancelled = false,
            failed = false,
            error = null,
        )
        val ui = GenerationProgressUiState().completeAwait(
            status = statusWithoutTerminalDetails,
            failure = IllegalStateException("await completion failed unexpectedly"),
        )

        assertTrue(ui.awaitCompleted)
        assertTrue(ui.isTerminal(statusWithoutTerminalDetails))
        assertTrue(ui.isFailed(statusWithoutTerminalDetails))
        assertTrue(ui.canRetry(statusWithoutTerminalDetails))
        assertEquals("await completion failed unexpectedly", ui.awaitFailure)
    }
}
