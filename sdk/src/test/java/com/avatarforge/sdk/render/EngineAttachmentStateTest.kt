package com.avatarforge.sdk.render

import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class EngineAttachmentStateTest {
    @Test
    fun `retiring A after rapid A to B replacement leaves B current`() {
        val state = EngineAttachmentState()
        val attachmentA = state.attach(101L)
        val attachmentB = state.attach(202L)

        assertFalse(state.isCurrent(attachmentA))
        assertFalse(state.retire(attachmentA))
        assertTrue(state.isCurrent(attachmentB))
    }

    @Test
    fun `queued render callbacks skip stale A and attach current B`() {
        val state = EngineAttachmentState()
        val attachedHandles = mutableListOf<Long>()
        val queuedCallbacks = mutableListOf<() -> Unit>()

        val attachmentA = state.attach(101L)
        queuedCallbacks += {
            state.attachIfCurrent(attachmentA) { handle -> attachedHandles += handle }
        }
        val attachmentB = state.attach(202L)
        queuedCallbacks += {
            state.attachIfCurrent(attachmentB) { handle -> attachedHandles += handle }
        }

        queuedCallbacks.forEach { it() }

        assertEquals(listOf(202L), attachedHandles)
        assertTrue(state.isCurrent(attachmentB))
    }

    @Test
    fun `replacement cannot become current between callback validation and renderer attach`() {
        val state = EngineAttachmentState()
        val attachmentA = state.attach(101L)
        val rendererAttachEntered = CountDownLatch(1)
        val allowRendererAttach = CountDownLatch(1)
        val replacementAttempting = CountDownLatch(1)
        val replacementFinished = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())
        val threadFailure = AtomicReference<Throwable?>(null)

        val callbackThread = Thread {
            try {
                assertTrue(state.attachIfCurrent(attachmentA) { handle ->
                    rendererAttachEntered.countDown()
                    if (!allowRendererAttach.await(2, TimeUnit.SECONDS)) {
                        throw AssertionError("Timed out before simulated renderer attachment")
                    }
                    events += "renderer:$handle"
                })
            } catch (failure: Throwable) {
                threadFailure.compareAndSet(null, failure)
            }
        }
        callbackThread.start()
        assertTrue(rendererAttachEntered.await(2, TimeUnit.SECONDS))

        val replacementThread = Thread {
            replacementAttempting.countDown()
            try {
                val attachmentB = state.attach(202L)
                events += "current:${attachmentB.engineHandle}"
            } catch (failure: Throwable) {
                threadFailure.compareAndSet(null, failure)
            } finally {
                replacementFinished.countDown()
            }
        }
        replacementThread.start()

        try {
            assertTrue(replacementAttempting.await(2, TimeUnit.SECONDS))
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2)
            while (replacementThread.state != Thread.State.BLOCKED &&
                replacementFinished.count != 0L && System.nanoTime() < deadline
            ) {
                Thread.yield()
            }
            assertEquals(Thread.State.BLOCKED, replacementThread.state)
        } finally {
            allowRendererAttach.countDown()
        }

        assertTrue(replacementFinished.await(2, TimeUnit.SECONDS))
        callbackThread.join(2_000)
        replacementThread.join(2_000)
        assertFalse(callbackThread.isAlive)
        assertFalse(replacementThread.isAlive)
        threadFailure.get()?.let { throw AssertionError("Attachment callback failed", it) }
        assertEquals(listOf("renderer:101", "current:202"), events)
    }
}
