package com.avatarforge.sdk.internal

import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

class KernelAccessTest {
    @Test fun isolatedBuildFenceDoesNotBlockActiveRenderFence() {
        val buildEntered = CountDownLatch(1)
        val letBuildFinish = CountDownLatch(1)
        val renderEntered = CountDownLatch(1)
        val build = thread(name = "isolated-build-test") {
            KernelAccess.withKernel(101L) {
                buildEntered.countDown()
                letBuildFinish.await(2, TimeUnit.SECONDS)
            }
        }
        assertTrue("candidate build did not acquire its engine fence", buildEntered.await(2, TimeUnit.SECONDS))

        val render = thread(name = "active-render-test") {
            KernelAccess.withKernel(202L) { renderEntered.countDown() }
        }
        val renderedWhileBuildWasHeld = renderEntered.await(1, TimeUnit.SECONDS)
        letBuildFinish.countDown()
        build.join(2_000)
        render.join(2_000)

        assertTrue("active rendering waited on an unrelated candidate engine", renderedWhileBuildWasHeld)
        assertTrue("test build thread did not finish", !build.isAlive)
        assertTrue("test render thread did not finish", !render.isAlive)
    }

    @Test fun sameEngineOperationsShareOneFence() {
        val firstEntered = CountDownLatch(1)
        val releaseFirst = CountDownLatch(1)
        val secondEntered = CountDownLatch(1)
        val first = thread(name = "engine-writer-test") {
            KernelAccess.withKernel(303L) {
                firstEntered.countDown()
                releaseFirst.await(2, TimeUnit.SECONDS)
            }
        }
        assertTrue(firstEntered.await(2, TimeUnit.SECONDS))

        val second = thread(name = "engine-reader-test") {
            KernelAccess.withKernel(303L) { secondEntered.countDown() }
        }
        val blockedUntilRelease = !secondEntered.await(100, TimeUnit.MILLISECONDS)
        releaseFirst.countDown()
        first.join(2_000)
        second.join(2_000)

        assertTrue("same-engine operation bypassed its fence", blockedUntilRelease)
        assertTrue(!first.isAlive)
        assertTrue(!second.isAlive)
    }
}
