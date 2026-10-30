package com.avatarforge.app

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertNotSame
import org.junit.Test

class ReferenceImageAnalysisDispatcherTest {
    @Test
    fun `reference image work runs off the calling thread`() = runBlocking {
        val caller = Thread.currentThread()
        val worker = withReferenceImageIo { Thread.currentThread() }
        assertNotSame("image work must not run on the caller/UI thread", caller, worker)
    }
}
