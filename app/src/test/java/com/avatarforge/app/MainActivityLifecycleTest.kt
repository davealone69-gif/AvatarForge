package com.avatarforge.app

import android.content.Context
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.hasScrollAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onFirst
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollToNode
import com.avatarforge.sdk.AvatarEngine
import com.avatarforge.sdk.AvatarSpec
import com.avatarforge.sdk.EngineHealth
import com.avatarforge.sdk.EngineResult
import com.avatarforge.sdk.GenerationRequest
import com.avatarforge.sdk.JobHandle
import com.avatarforge.sdk.render.RenderSurfaceView
import java.io.File
import java.lang.reflect.Field
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.ExternalResource
import org.junit.rules.RuleChain
import org.junit.rules.TestRule
import org.junit.runner.RunWith
import org.mockito.Mockito
import org.mockito.Mockito.never
import org.mockito.Mockito.times
import org.mockito.Mockito.verify
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class MainActivityLifecycleTest {
    private lateinit var engine: AvatarEngine
    private var originalCompanion: Any? = null
    private var companionField: Field? = null

    private val composeRule = createAndroidComposeRule<MainActivity>()
    private val sdkSetupRule = object : ExternalResource() {
        override fun before() = installNativeFreeSdkDoubles()
        override fun after() = restoreSdkCompanion()
    }

    @get:Rule
    val rules: TestRule = RuleChain.outerRule(sdkSetupRule).around(composeRule)

    @Test
    fun `configuration recreation retains viewer engine and rebinds a fresh activity surface`() {
        val scenario = composeRule.activityRule.scenario
        val firstActivity = composeRule.activity
        composeRule.waitForIdle()

        // Use the rendered prompt example and the real Generate button, scrolling via Compose's test API.
        composeRule.onNodeWithText("athletic woman").performClick()
        composeRule.onAllNodes(hasScrollAction()).onFirst()
            .performScrollToNode(hasText("Generate from prompt"))
        composeRule.onNodeWithText("Generate from prompt").performClick()

        composeRule.waitUntil(timeoutMillis = 15_000) {
            composeRule.onAllNodesWithText("← Back to editor").fetchSemanticsNodes().isNotEmpty()
        }
        composeRule.onNodeWithText("← Back to editor").assertIsDisplayed()

        val oldSurface = readField(firstActivity, "surfaceView") as? RenderSurfaceView
        assertNotNull("the production AndroidView factory must create the original surface", oldSurface)
        assertSame("the Activity must own the process engine", engine, readField(firstActivity, "ownedEngine"))
        assertSame("the production factory must bind this engine instance", engine, readField(oldSurface!!, "engine"))
        assertEquals("the surface must attach the engine's native handle", TEST_ENGINE_HANDLE, oldSurface.attachedEngineHandle)

        scenario.recreate()
        composeRule.waitForIdle()

        val recreatedActivity = composeRule.activity
        val retainedEngine = readField(recreatedActivity, "ownedEngine")
        val viewerRestored = composeRule.onAllNodesWithText("← Back to editor").fetchSemanticsNodes().isNotEmpty()
        val newSurface = readField(recreatedActivity, "surfaceView") as? RenderSurfaceView
        val oldSurfaceReleased = readField(oldSurface, "released") as Boolean
        verify(engine, never()).release()

        scenario.close()
        verify(engine, times(1)).release()

        assertSame("the recreated Activity must retain the same engine", engine, retainedEngine)
        assertTrue("the Viewer UI must be restored after configuration recreation", viewerRestored)
        assertTrue("the original Activity-owned SurfaceView must be released", oldSurfaceReleased)
        assertNotNull("the recreated Viewer must create its surface through AndroidView", newSurface)
        assertNotSame("configuration recreation must create a new surface instance", oldSurface, newSurface)
        assertSame("the new AndroidView factory must bind the retained engine", engine, readField(newSurface!!, "engine"))
        assertEquals("the replacement surface must attach the retained native handle", TEST_ENGINE_HANDLE, newSurface.attachedEngineHandle)
    }

    private fun installNativeFreeSdkDoubles() {
        engine = Mockito.mock(AvatarEngine::class.java)
        Mockito.`when`(engine.blendshapeNames()).thenReturn(emptyList())
        Mockito.`when`(engine.nativeHandle).thenReturn(TEST_ENGINE_HANDLE)
        Mockito.`when`(
            engine.parsePrompt(
                Mockito.anyString(),
                Mockito.any(AvatarSpec::class.java) ?: AvatarSpec(),
            ),
        ).thenReturn(AvatarSpec())

        val spec = AvatarSpec()
        val resultFile = File.createTempFile("rb01-avatar", ".glb")
        resultFile.writeBytes(byteArrayOf(1, 2, 3))
        val result = EngineResult(resultFile, spec, 1, 0, 0, 0, 0, 3L, emptyMap(), emptyList())
        val job = completedJob(engine, GenerationRequest(spec, "test avatar"), result)
        Mockito.`when`(
            engine.generate(
                Mockito.any(GenerationRequest::class.java)
                    ?: GenerationRequest(AvatarSpec(), "test"),
            ),
        ).thenReturn(job)

        val field = AvatarEngine::class.java.getDeclaredField("Companion").apply { isAccessible = true }
        companionField = field
        originalCompanion = field.get(null)
        val sdk = Mockito.mock(AvatarEngine.Companion::class.java)
        Mockito.`when`(sdk.get(Mockito.any(Context::class.java) ?: RuntimeEnvironment.getApplication()))
            .thenReturn(engine)
        Mockito.`when`(sdk.health()).thenReturn(EngineHealth(true, AvatarEngine.ABI_VERSION, "Robolectric test double"))
        putStaticFinal(field, sdk)
    }

    private fun restoreSdkCompanion() {
        val field = companionField
        val original = originalCompanion
        if (field != null && original != null) putStaticFinal(field, original)
    }

    private fun completedJob(engine: AvatarEngine, request: GenerationRequest, result: EngineResult): JobHandle {
        val constructor = JobHandle::class.java.getDeclaredConstructor(
            AvatarEngine::class.java,
            GenerationRequest::class.java,
            Job::class.java,
        )
        val job = constructor.newInstance(engine, request, SupervisorJob())
        val deliver = JobHandle::class.java.declaredMethods.single { it.name.startsWith("deliver") }
        deliver.invoke(job, result)
        return job
    }

    private fun readField(target: Any, name: String): Any? =
        target.javaClass.getDeclaredField(name).apply { isAccessible = true }.get(target)

    private fun putStaticFinal(field: Field, value: Any) {
        val unsafeClass = Class.forName("sun.misc.Unsafe")
        val unsafeField = unsafeClass.getDeclaredField("theUnsafe").apply { isAccessible = true }
        val unsafe = unsafeField.get(null)
        val base = unsafeClass.getMethod("staticFieldBase", Field::class.java).invoke(unsafe, field)
        val offset = unsafeClass.getMethod("staticFieldOffset", Field::class.java).invoke(unsafe, field) as Long
        unsafeClass.getMethod("putObjectVolatile", Any::class.java, Long::class.javaPrimitiveType, Any::class.java)
            .invoke(unsafe, base, offset, value)
    }

    private companion object {
        const val TEST_ENGINE_HANDLE = 0xA71L
    }
}
