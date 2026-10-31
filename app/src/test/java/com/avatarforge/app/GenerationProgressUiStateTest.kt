package com.avatarforge.app

import java.util.concurrent.CancellationException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class GenerationProgressUiStateTest {
    @Test
    fun `stopped status cannot exit before successful await opens viewer with result`() {
        val stoppedStatus = ProgressJobSnapshot(running = false, cancelled = false, failed = false, error = null)
        val beforeAwait = progressExitTransition(
            hasJob = true,
            jobRunning = false,
            current = GenerationProgressUiState(),
            cancelJob = { error("a stopped job must not be cancelled again") },
        )

        assertEquals(ProgressExitAction.WAIT_FOR_COMPLETION, beforeAwait.action)
        assertFalse(beforeAwait.uiState.isTerminal())

        val generatedResult = Any()
        val resolution = resolveProgressAwait(
            current = beforeAwait.uiState,
            status = stoppedStatus,
            outcome = Result.success(generatedResult),
        )

        assertEquals(ProgressDestination.VIEWER, resolution.destination)
        assertSame(generatedResult, resolution.result)
        assertTrue(resolution.uiState.isTerminal())
    }

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
        assertFalse(transition.uiState.isTerminal())

        transition = progressExitTransition(
            hasJob = true,
            jobRunning = false,
            current = transition.uiState,
            cancelJob = { cancelRequests++ },
        )
        assertEquals(ProgressExitAction.WAIT_FOR_COMPLETION, transition.action)
        assertEquals("do not issue repeated cancel requests", 1, cancelRequests)
        assertFalse(transition.uiState.isTerminal())

        val resolution = resolveProgressAwait(
            current = transition.uiState,
            status = cancellationStatus,
            outcome = Result.failure<Any>(CancellationException("generation cancelled")),
        )
        assertEquals(ProgressDestination.PROGRESS, resolution.destination)
        assertTrue(resolution.uiState.isTerminal())
        assertTrue(resolution.uiState.isCancelled(cancellationStatus))
        assertTrue(resolution.uiState.canRetry(cancellationStatus))
        assertNull(resolution.uiState.awaitFailure)

        transition = progressExitTransition(
            hasJob = true,
            jobRunning = false,
            current = resolution.uiState,
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
        val resolution = resolveProgressAwait(
            current = GenerationProgressUiState(),
            status = statusWithoutTerminalDetails,
            outcome = Result.failure<Any>(IllegalStateException("await completion failed unexpectedly")),
        )

        assertEquals(ProgressDestination.PROGRESS, resolution.destination)
        assertTrue(resolution.uiState.isTerminal())
        assertTrue(resolution.uiState.isFailed(statusWithoutTerminalDetails))
        assertTrue(resolution.uiState.canRetry(statusWithoutTerminalDetails))
        assertEquals("await completion failed unexpectedly", resolution.uiState.awaitFailure)
    }
}
