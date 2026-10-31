package com.avatarforge.sdk.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Test

class FilamentResourceLoadStartTest {
    @Test
    fun successfulBeginDoesNotRunFailureCleanup() {
        var cleanupCalls = 0

        val outcome = beginResourceLoadSafely(begin = { true }) { cleanupCalls++ }

        assertSame(ResourceLoadStartOutcome.Started, outcome)
        assertEquals(0, cleanupCalls)
    }

    @Test
    fun falseBeginRunsCleanupExactlyOnce() {
        var cleanupCalls = 0

        val outcome = beginResourceLoadSafely(begin = { false }) { cleanupCalls++ }

        assertSame(ResourceLoadStartOutcome.Rejected, outcome)
        assertEquals(1, cleanupCalls)
    }

    @Test
    fun thrownBeginRunsCleanupExactlyOnceAndPreservesFailure() {
        var cleanupCalls = 0
        val expectedFailure = IllegalStateException("partial resource start")

        val outcome = beginResourceLoadSafely(begin = { throw expectedFailure }) { cleanupCalls++ }

        assertEquals(1, cleanupCalls)
        assertSame(expectedFailure, (outcome as ResourceLoadStartOutcome.Failed).failure)
    }

    @Test
    fun outOfMemoryBeginRunsCleanupExactlyOnceAndPreservesFailure() {
        var cleanupCalls = 0
        val expectedFailure = OutOfMemoryError("resource start allocation")

        val outcome = beginResourceLoadSafely(begin = { throw expectedFailure }) { cleanupCalls++ }

        assertEquals(1, cleanupCalls)
        assertSame(expectedFailure, (outcome as ResourceLoadStartOutcome.Failed).failure)
    }

    @Test
    fun fatalNonOutOfMemoryErrorRunsCleanupOnceAndRethrowsTheSameError() {
        var cleanupCalls = 0
        val expectedFailure = AssertionError("fatal resource start")
        var actualFailure: AssertionError? = null

        try {
            beginResourceLoadSafely(begin = { throw expectedFailure }) { cleanupCalls++ }
        } catch (failure: AssertionError) {
            actualFailure = failure
        }

        assertEquals(1, cleanupCalls)
        assertSame(expectedFailure, actualFailure)
    }
}
