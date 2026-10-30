package com.avatarforge.app

import org.junit.Assert.assertEquals
import org.junit.Test

class MainActivityLifecycleTest {
    @Test
    fun `configuration change keeps the process engine alive`() {
        var releaseCount = 0

        releaseEngineOnActivityDestroy(isChangingConfigurations = true) {
            releaseCount++
        }

        assertEquals(0, releaseCount)
    }

    @Test
    fun `final activity destruction releases the engine once`() {
        var releaseCount = 0

        releaseEngineOnActivityDestroy(isChangingConfigurations = false) {
            releaseCount++
        }

        assertEquals(1, releaseCount)
    }
}
