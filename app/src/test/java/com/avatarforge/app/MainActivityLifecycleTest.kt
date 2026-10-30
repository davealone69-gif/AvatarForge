package com.avatarforge.app

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import com.avatarforge.sdk.JobHandle
import com.avatarforge.sdk.JobStatus
import com.avatarforge.sdk.Stage
import kotlinx.coroutines.flow.MutableStateFlow
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import org.mockito.Mockito

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

    @Test
    fun `configuration recreation reuses active generation and progress state`() {
        val retainedStore = ViewModelStore()
        val originalViewModel = sessionProvider(retainedStore)[GenerationSessionViewModel::class.java]
        val original = originalViewModel.state
        original.screen.value = Screen.PROGRESS
        original.status.value = JobStatus(
            stage = Stage.GEOMETRY,
            overallProgress = 0.42f,
            running = true,
        )

        /* A recreated Activity asks the same retained ViewModelStore for its session ViewModel. */
        val recreatedViewModel = sessionProvider(retainedStore)[GenerationSessionViewModel::class.java]
        val recreated = recreatedViewModel.state

        assertSame(originalViewModel, recreatedViewModel)
        assertSame(original, recreated)
        assertSame("the retained session keeps the active-job state holder", original.job, recreated.job)
        assertEquals(Screen.PROGRESS, recreated.screen.value)
        assertEquals(Stage.GEOMETRY, recreated.status.value.stage)
        assertEquals(0.42f, recreated.status.value.overallProgress, 0f)
        assertEquals(true, recreated.status.value.running)
        retainedStore.clear()
    }

    @Test
    fun `configuration recreation retains non-null active JobHandle`() {
        val retainedStore = ViewModelStore()
        val original = sessionProvider(retainedStore)[GenerationSessionViewModel::class.java].state
        val activeJob = Mockito.mock(JobHandle::class.java)
        Mockito.`when`(activeJob.status).thenReturn(MutableStateFlow(JobStatus(running = true)))
        original.job.value = activeJob
        original.screen.value = Screen.PROGRESS

        val reattached = sessionProvider(retainedStore)[GenerationSessionViewModel::class.java].state

        assertNotNull(reattached.job.value)
        assertSame(activeJob, reattached.job.value)
        assertTrue(reattached.job.value!!.status.value.running)
        assertEquals(Screen.PROGRESS, reattached.screen.value)
        retainedStore.clear()
    }

    @Test
    fun `a fresh process session starts with empty generation state`() {
        val firstStore = ViewModelStore()
        val previous = sessionProvider(firstStore)[GenerationSessionViewModel::class.java].state
        previous.screen.value = Screen.PROGRESS
        previous.status.value = JobStatus(running = true)
        firstStore.clear()

        val newProcessStore = ViewModelStore()
        val fresh = sessionProvider(newProcessStore)[GenerationSessionViewModel::class.java].state

        assertNotSame(previous, fresh)
        assertEquals(Screen.INPUT, fresh.screen.value)
        assertEquals(null, fresh.job.value)
        assertEquals(false, fresh.status.value.running)
        assertEquals(null, fresh.result.value)
        newProcessStore.clear()
    }

    private fun sessionProvider(store: ViewModelStore) = ViewModelProvider(
        store,
        object : ViewModelProvider.Factory {
            override fun <T : ViewModel> create(modelClass: Class<T>): T {
                @Suppress("UNCHECKED_CAST")
                return GenerationSessionViewModel() as T
            }
        },
    )
}
