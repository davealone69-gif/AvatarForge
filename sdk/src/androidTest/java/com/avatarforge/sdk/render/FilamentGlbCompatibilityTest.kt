package com.avatarforge.sdk.render

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.google.android.filament.Engine
import com.google.android.filament.Filament
import com.google.android.filament.gltfio.AssetLoader
import com.google.android.filament.gltfio.FilamentAsset
import com.google.android.filament.gltfio.ResourceLoader
import com.google.android.filament.gltfio.UbershaderProvider
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.security.MessageDigest
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Real Android/JNI gltfio smoke test for the exact Agent 1 output. The GLB is copied into this test
 * APK's private files directory by the test runner; it is deliberately not checked into the repo.
 * This test exercises parsing, embedded-resource upload, renderable creation, and animation data,
 * but does not create a surface or claim visual correctness.
 */
@RunWith(AndroidJUnit4::class)
class FilamentGlbCompatibilityTest {
    @Test
    fun generatedAvatarGlbLoadsWithGltfio1751() {
        val context = InstrumentationRegistry.getInstrumentation().context
        val suppliedPath = InstrumentationRegistry.getArguments().getString("glb_path")
        val file = suppliedPath?.let(::File) ?: File(context.filesDir, EXPECTED_FILE_NAME)

        assertTrue("Missing test GLB at ${file.absolutePath}", file.isFile)
        assertEquals(EXPECTED_FILE_LENGTH, file.length())
        assertEquals(EXPECTED_SHA256, sha256(file))

        val glbBuffer = FileChannel.open(file.toPath(), StandardOpenOption.READ).use { channel ->
            channel.map(FileChannel.MapMode.READ_ONLY, 0L, channel.size())
        }
        assertEmbeddedImageMetadata(glbBuffer)

        Filament.init()
        var engine: Engine? = null
        var materialProvider: UbershaderProvider? = null
        var assetLoader: AssetLoader? = null
        var resourceLoader: ResourceLoader? = null
        var asset: FilamentAsset? = null
        var resourceLoadActive = false
        var sourceDataReleased = false
        try {
            val createdEngine = Engine.create(Engine.Backend.OPENGL)
            engine = createdEngine
            val createdProvider = UbershaderProvider(createdEngine)
            materialProvider = createdProvider
            val createdAssetLoader = AssetLoader(
                createdEngine,
                createdProvider,
                createdEngine.entityManager,
            )
            assetLoader = createdAssetLoader
            val createdResourceLoader = ResourceLoader(createdEngine, true)
            resourceLoader = createdResourceLoader

            val loadedAsset = createdAssetLoader.createAsset(glbBuffer)
            assertTrue("gltfio could not create an asset from the generated GLB", loadedAsset != null)
            asset = loadedAsset ?: throw AssertionError("gltfio returned no asset")

            assertTrue("GLB produced no renderable entities", asset!!.renderableEntities.isNotEmpty())
            assertTrue(
                "GLB should not require external sidecar resources: ${asset!!.resourceUris.contentToString()}",
                asset!!.resourceUris.isEmpty(),
            )

            val animator = asset!!.instance.animator
            assertEquals(EXPECTED_ANIMATION_NAMES, (0 until animator.animationCount).map(animator::getAnimationName))

            resourceLoadActive = true
            assertTrue("gltfio failed to begin loading embedded image resources", createdResourceLoader.asyncBeginLoad(asset!!))
            val deadlineMillis = System.currentTimeMillis() + RESOURCE_LOAD_TIMEOUT_MILLIS
            var loadProgress = createdResourceLoader.asyncGetLoadProgress()
            while (loadProgress < 1f && System.currentTimeMillis() < deadlineMillis) {
                createdResourceLoader.asyncUpdateLoad()
                loadProgress = createdResourceLoader.asyncGetLoadProgress()
                if (loadProgress < 1f) Thread.sleep(10L)
            }
            assertTrue("gltfio resource load timed out at $loadProgress", loadProgress >= 1f)
            resourceLoadActive = false

            asset!!.releaseSourceData()
            sourceDataReleased = true
            createdResourceLoader.evictResourceData()

            for (index in EXPECTED_ANIMATION_NAMES.indices) {
                animator.applyAnimation(index, 0f)
                animator.updateBoneMatrices()
            }
        } finally {
            val currentResourceLoader = resourceLoader
            if (resourceLoadActive) runCatching { currentResourceLoader?.asyncCancelLoad() }
            val currentAsset = asset
            if (currentAsset != null) {
                if (!sourceDataReleased) runCatching { currentAsset.releaseSourceData() }
                runCatching { assetLoader?.destroyAsset(currentAsset) }
            }
            runCatching { currentResourceLoader?.evictResourceData() }
            runCatching { currentResourceLoader?.destroy() }
            runCatching { assetLoader?.destroy() }
            runCatching { materialProvider?.destroyMaterials() }
            runCatching { materialProvider?.destroy() }
            engine?.let { currentEngine ->
                runCatching { currentEngine.flushAndWait() }
                runCatching { currentEngine.destroy() }
            }
        }
    }

    private fun assertEmbeddedImageMetadata(glb: ByteBuffer) {
        val header = glb.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(GLB_MAGIC, header.getInt(0))
        assertEquals(GLB_VERSION, header.getInt(4))
        assertEquals(EXPECTED_FILE_LENGTH.toInt(), header.getInt(8))

        val jsonChunkLength = header.getInt(GLB_HEADER_BYTES)
        assertEquals(JSON_CHUNK_TYPE, header.getInt(GLB_HEADER_BYTES + Int.SIZE_BYTES))
        assertTrue(
            "Invalid GLB JSON chunk length: $jsonChunkLength",
            jsonChunkLength > 0 && GLB_JSON_OFFSET.toLong() + jsonChunkLength <= glb.capacity().toLong(),
        )
        val jsonBytes = ByteArray(jsonChunkLength)
        header.position(GLB_JSON_OFFSET)
        header.limit(GLB_JSON_OFFSET + jsonChunkLength)
        header.get(jsonBytes)

        val document = JSONObject(String(jsonBytes, Charsets.UTF_8).trimEnd(' ', '\t', '\r', '\n', '\u0000'))
        val images = document.getJSONArray("images")
        assertEquals(EXPECTED_EMBEDDED_IMAGES, images.length())
        for (index in 0 until images.length()) {
            val image = images.getJSONObject(index)
            assertFalse("Image $index must not declare an external URI", image.has("uri"))
            assertTrue("Image $index must be embedded in a bufferView", image.has("bufferView"))
            assertEquals("image/png", image.getString("mimeType"))
        }

        val textures = document.getJSONArray("textures")
        assertEquals(EXPECTED_EMBEDDED_TEXTURES, textures.length())
        for (index in 0 until textures.length()) {
            val sourceIndex = textures.getJSONObject(index).getInt("source")
            assertTrue("Texture $index references missing image $sourceIndex", sourceIndex in 0 until images.length())
        }
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString(separator = "") { byte ->
            "%02x".format(byte.toInt() and 0xff)
        }
    }

    private companion object {
        const val GLB_MAGIC = 0x46546C67
        const val GLB_VERSION = 2
        const val GLB_HEADER_BYTES = 12
        const val JSON_CHUNK_TYPE = 0x4E4F534A
        const val GLB_JSON_OFFSET = 20
        const val EXPECTED_FILE_NAME = "agent-af-native-generated.glb"
        const val EXPECTED_FILE_LENGTH = 40_782_696L
        const val EXPECTED_SHA256 = "84069e147a703a8ca6aae887c79508bececbd472fced56b67a98c067efac1809"
        const val EXPECTED_EMBEDDED_IMAGES = 5
        const val EXPECTED_EMBEDDED_TEXTURES = 5
        const val RESOURCE_LOAD_TIMEOUT_MILLIS = 60_000L
        val EXPECTED_ANIMATION_NAMES = listOf(
            "idle", "breathe", "walk", "jog", "run", "squat", "jump", "wave", "punch", "kick",
            "pushup", "t_pose", "a_pose", "sit", "turn_360",
        )
    }
}
