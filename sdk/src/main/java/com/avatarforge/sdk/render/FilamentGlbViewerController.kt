/*
 * AvatarForge :: Filament GLB viewer adapter.
 *
 * The UI owns the Surface and calls this controller from its lifecycle callbacks. Filament owns
 * the rendering resources on one controller thread; the file worker only maps GLB/sidecar bytes and
 * never calls Filament. No Compose or View types cross this boundary.
 */
package com.avatarforge.sdk.render

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.view.Surface
import com.google.android.filament.Camera
import com.google.android.filament.Engine
import com.google.android.filament.LightManager
import com.google.android.filament.Renderer
import com.google.android.filament.Scene
import com.google.android.filament.SwapChain
import com.google.android.filament.View
import com.google.android.filament.Viewport
import com.google.android.filament.Filament
import com.google.android.filament.gltfio.AssetLoader
import com.google.android.filament.gltfio.Animator
import com.google.android.filament.gltfio.FilamentAsset
import com.google.android.filament.gltfio.ResourceLoader
import com.google.android.filament.gltfio.UbershaderProvider
import java.io.File
import java.net.URI
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.MappedByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.util.concurrent.CancellationException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Public state used by the UI to gate entry into the viewer on real load success. */
sealed interface FilamentViewerState {
    data object WaitingForSurface : FilamentViewerState
    data object Initializing : FilamentViewerState
    data object WaitingForAsset : FilamentViewerState
    data object LoadingAsset : FilamentViewerState
    data class Ready(val animationCount: Int, val animationNames: List<String>) : FilamentViewerState
    data class Failed(
        val stage: FilamentViewerStage,
        val message: String,
        val outOfMemory: Boolean = false,
    ) : FilamentViewerState
    data object Closed : FilamentViewerState
}

/** Stage labels are stable UI-facing categories; [FilamentViewerState.Failed.message] is diagnostic text. */
enum class FilamentViewerStage {
    FILE,
    INITIALIZATION,
    SURFACE,
    ASSET,
    RESOURCES,
    RENDER,
}

/**
 * File-based Filament viewer controller. All Filament calls and resource destruction are serialized
 * on one owned HandlerThread. The UI retains and releases [Surface]; this class only creates and
 * destroys the associated Filament swap chain.
 *
 * Call [onSurfaceCreated] and [onSurfaceChanged] from Surface callbacks, [loadGlb] with
 * `EngineResult.glbFile`, and [renderFrame] from the host's frame callback. [close] is terminal and
 * idempotent. A load is asynchronous; [state] becomes [FilamentViewerState.Ready] only after the
 * GLB, all required resources, and a live positive-size swap chain are present.
 */
class FilamentGlbViewerController(context: Context) : AutoCloseable {
    @Suppress("unused")
    private val applicationContext = context.applicationContext

    private val renderThread = HandlerThread("filament-glb-viewer", Process.THREAD_PRIORITY_DISPLAY)
        .apply { start() }
    private val renderHandler = Handler(renderThread.looper)
    private val fileExecutor: ExecutorService = Executors.newSingleThreadExecutor { task ->
        Thread(task, "filament-glb-file").apply { isDaemon = true }
    }

    private val mutableState = MutableStateFlow<FilamentViewerState>(FilamentViewerState.WaitingForSurface)
    val state: StateFlow<FilamentViewerState> = mutableState.asStateFlow()

    private val closed = AtomicBoolean(false)
    private val closeStarted = AtomicBoolean(false)
    private val closeComplete = CountDownLatch(1)
    private val loadGeneration = AtomicLong(0L)
    private val framePending = AtomicBoolean(false)
    private val newestFrameTime = AtomicLong(0L)

    @Volatile
    private var latestCamera = CameraGesture.clamped(CameraSettings())

    @Volatile
    private var fileLoadFuture: Future<*>? = null

    // The fields below are render-thread confined.
    private var surface: Surface? = null
    private var surfaceWidth = 0
    private var surfaceHeight = 0
    private var swapChain: SwapChain? = null
    private var engine: Engine? = null
    private var renderer: Renderer? = null
    private var scene: Scene? = null
    private var view: View? = null
    private var camera: Camera? = null
    private var cameraEntity = 0
    private var directionalLightEntity = 0
    private var materialProvider: UbershaderProvider? = null
    private var assetLoader: AssetLoader? = null
    private var resourceLoader: ResourceLoader? = null
    private var asset: FilamentAsset? = null
    private var sourceDataReleased = true
    private var animator: Animator? = null
    private var boundsCenter = floatArrayOf(0f, 0.95f, 0f)
    private var boundsHalfExtent = floatArrayOf(0.45f, 0.95f, 0.35f)
    private var mappedSourceBuffers = mutableListOf<ByteBuffer>()
    private var pendingGlb: Pair<Long, MappedByteBuffer>? = null
    private var resourceLoadGeneration = 0L
    private var resourceLoadInProgress = false
    private var resolvingExternalResources = false
    private var animationNames = emptyList<String>()
    private var activeAnimation = -1
    private var animationPositionSeconds = 0f
    private var animationPlaying = false
    private var previousAnimationFrameNanos = 0L
    private var lastAppliedCamera: CameraSettings? = null

    /** Receives the UI-owned surface. Swap-chain creation waits for a positive width and height. */
    fun onSurfaceCreated(surface: Surface, width: Int, height: Int) = postToRenderThread {
        if (this.surface !== surface) destroySwapChain()
        this.surface = surface
        surfaceWidth = width
        surfaceHeight = height
        synchronizeSurface()
    }

    /** Updates the real surface dimensions and creates/recreates a swap chain only when usable. */
    fun onSurfaceChanged(surface: Surface, width: Int, height: Int) = postToRenderThread {
        if (this.surface !== surface) destroySwapChain()
        this.surface = surface
        surfaceWidth = width
        surfaceHeight = height
        synchronizeSurface()
    }

    /** Drops only this controller's current swap chain and Surface reference; the UI owns Surface. */
    fun onSurfaceDestroyed() = postToRenderThread {
        if (resourceLoadInProgress || resolvingExternalResources) {
            val retry = mappedSourceBuffers.firstOrNull() as? MappedByteBuffer
            val generation = resourceLoadGeneration
            fileLoadFuture?.cancel(true)
            cancelResourceLoad()
            releaseAsset()
            if (retry != null && generation == loadGeneration.get()) {
                pendingGlb = generation to retry
            }
        }
        destroySwapChain()
        surface = null
        surfaceWidth = 0
        surfaceHeight = 0
        previousAnimationFrameNanos = 0L
        mutableState.value = FilamentViewerState.WaitingForSurface
    }

    /**
     * Loads the generated GLB without copying the full file into a ByteArray. The file and any
     * relative sidecar resources are memory-mapped on a worker; parsing, upload, and all Filament
     * resource operations are serialized on the render thread. A newer call supersedes this load.
     */
    fun loadGlb(file: File) {
        if (closed.get()) return
        val generation = loadGeneration.incrementAndGet()
        fileLoadFuture?.cancel(true)
        postToRenderThread {
            if (generation != loadGeneration.get()) return@postToRenderThread
            cancelResourceLoad()
            pendingGlb = null
            releaseAsset()
            mutableState.value = if (hasUsableSurface()) {
                FilamentViewerState.LoadingAsset
            } else {
                FilamentViewerState.WaitingForSurface
            }
        }

        try {
            fileLoadFuture = fileExecutor.submit {
                try {
                    val mapped = mapGlb(file)
                    if (closed.get() || generation != loadGeneration.get()) return@submit
                    renderHandler.post {
                        if (closed.get() || generation != loadGeneration.get()) return@post
                        sourceFileByGeneration = generation to file
                        pendingGlb = generation to mapped
                        if (hasUsableSurface()) {
                            synchronizeSurface()
                        } else {
                            mutableState.value = FilamentViewerState.WaitingForSurface
                        }
                    }
                } catch (failure: OutOfMemoryError) {
                    publishFailure(generation, FilamentViewerStage.FILE, "Not enough memory to map the GLB file.", outOfMemory = true)
                } catch (failure: Exception) {
                    publishFailure(generation, FilamentViewerStage.FILE, failure.message ?: "Could not read the GLB file.")
                }
            }
        } catch (failure: Exception) {
            publishFailure(generation, FilamentViewerStage.FILE, failure.message ?: "Could not start the GLB file load.")
        }
    }

    /** Retains camera state immediately; the next render-thread frame applies it to Filament. */
    fun setCamera(camera: CameraSettings) {
        if (!closed.get()) latestCamera = CameraGesture.clamped(camera)
    }

    /** Starts the selected glTF animation from time zero. Invalid indices are ignored safely. */
    fun playAnimation(index: Int = 0) = postToRenderThread {
        val currentAnimator = animator ?: return@postToRenderThread
        if (index !in 0 until currentAnimator.animationCount) return@postToRenderThread
        activeAnimation = index
        animationPositionSeconds = 0f
        animationPlaying = true
        previousAnimationFrameNanos = 0L
        currentAnimator.applyAnimation(index, 0f)
        currentAnimator.updateBoneMatrices()
    }

    /** Pauses or resumes the selected animation without jumping on resume. */
    fun setAnimationPlaying(playing: Boolean) = postToRenderThread {
        if (animator == null || activeAnimation !in 0 until (animator?.animationCount ?: 0)) {
            return@postToRenderThread
        }
        animationPlaying = playing
        previousAnimationFrameNanos = 0L
    }

    /**
     * Coalesces frame requests: while a tick is queued or running, only its newest timestamp is
     * retained, so a slow upload/render cannot grow an unbounded Handler queue.
     */
    fun renderFrame(frameTimeNanos: Long) {
        if (closed.get()) return
        newestFrameTime.set(frameTimeNanos)
        queueNewestFrame()
    }

    /** Deterministic, idempotent terminal cleanup. All Filament resources die on the render thread. */
    override fun close() {
        if (!closeStarted.compareAndSet(false, true)) {
            if (Looper.myLooper() !== renderThread.looper) closeComplete.await()
            return
        }
        closed.set(true)
        loadGeneration.incrementAndGet()
        fileLoadFuture?.cancel(true)
        fileExecutor.shutdownNow()

        if (Looper.myLooper() === renderThread.looper) {
            cleanupAndStopRenderThread()
            runCatching { fileExecutor.awaitTermination(2, TimeUnit.SECONDS) }
            return
        }
        if (!renderHandler.post { cleanupAndStopRenderThread() }) {
            mutableState.value = FilamentViewerState.Closed
            closeComplete.countDown()
            renderThread.quitSafely()
        }
        closeComplete.await()
        renderThread.join()
        fileExecutor.awaitTermination(2, TimeUnit.SECONDS)
    }

    private fun queueNewestFrame() {
        if (!framePending.compareAndSet(false, true)) return
        if (!renderHandler.post {
                val frameTimeNanos = newestFrameTime.get()
                try {
                    if (!closed.get()) renderFrameOnRenderThread(frameTimeNanos)
                } catch (failure: OutOfMemoryError) {
                    failOnRenderThread(FilamentViewerStage.RENDER, "Not enough memory to render this avatar.", outOfMemory = true)
                } catch (failure: Exception) {
                    failOnRenderThread(FilamentViewerStage.RENDER, failure.message ?: "Filament could not render this frame.")
                } finally {
                    framePending.set(false)
                    if (!closed.get() && newestFrameTime.get() != frameTimeNanos) queueNewestFrame()
                }
            }) {
            framePending.set(false)
        }
    }

    private fun renderFrameOnRenderThread(frameTimeNanos: Long) {
        if (resourceLoadInProgress) updateAsyncResourceLoad()
        val currentRenderer = renderer ?: return
        val currentSwapChain = swapChain ?: return
        if (!hasUsableSurface()) return

        if (asset != null && !resourceLoadInProgress && !resolvingExternalResources) {
            applyCameraIfChanged()
            advanceAnimation(frameTimeNanos)
            val began = currentRenderer.beginFrame(currentSwapChain)
            if (began) {
                try {
                    currentRenderer.render(view ?: return)
                } finally {
                    currentRenderer.endFrame()
                }
            }
        }
    }

    private fun synchronizeSurface() {
        if (!hasUsableSurface()) {
            destroySwapChain()
            mutableState.value = FilamentViewerState.WaitingForSurface
            return
        }
        try {
            if (engine == null) initializeFilament()
            if (swapChain == null) {
                swapChain = engine!!.createSwapChain(surface!!)
                if (swapChain == null) error("Filament did not create a swap chain for the supplied Surface.")
            }
            view?.setViewport(Viewport(0, 0, surfaceWidth, surfaceHeight))
            applyCameraIfChanged(force = true)

            val mapped = pendingGlb
            when {
                resourceLoadInProgress || resolvingExternalResources -> {
                    mutableState.value = FilamentViewerState.LoadingAsset
                }
                mapped != null && mapped.first == loadGeneration.get() -> startAssetLoad(mapped.first, mapped.second)
                asset != null -> publishReadyIfPossible()
                mutableState.value !is FilamentViewerState.Failed -> {
                    mutableState.value = FilamentViewerState.WaitingForAsset
                }
            }
        } catch (failure: OutOfMemoryError) {
            cleanupFilamentObjects()
            failOnRenderThread(FilamentViewerStage.INITIALIZATION, "Not enough memory to initialize Filament.", outOfMemory = true)
        } catch (failure: Exception) {
            val stage = if (engine == null || renderer == null) FilamentViewerStage.INITIALIZATION else FilamentViewerStage.SURFACE
            cleanupFilamentObjects()
            failOnRenderThread(stage, failure.message ?: "Could not initialize the Filament surface.")
        }
    }

    private fun initializeFilament() {
        mutableState.value = FilamentViewerState.Initializing
        try {
            synchronized(FILAMENT_INIT_LOCK) {
                if (!filamentInitialized) {
                    Filament.init()
                    filamentInitialized = true
                }
            }
            val newEngine = Engine.create(Engine.Backend.OPENGL)
            engine = newEngine
            renderer = newEngine.createRenderer()
            val newScene = newEngine.createScene()
            scene = newScene
            view = newEngine.createView().also { it.setScene(newScene) }

            cameraEntity = newEngine.entityManager.create()
            val newCamera = newEngine.createCamera(cameraEntity)
            camera = newCamera
            view?.setCamera(newCamera)

            val newMaterialProvider = UbershaderProvider(newEngine)
            materialProvider = newMaterialProvider
            assetLoader = AssetLoader(newEngine, newMaterialProvider, newEngine.entityManager)
            resourceLoader = ResourceLoader(newEngine, true)

            directionalLightEntity = newEngine.entityManager.create()
            LightManager.Builder(LightManager.Type.DIRECTIONAL)
                .color(1f, 0.96f, 0.90f)
                .intensity(100_000f)
                .direction(-0.3f, -0.8f, 0.4f)
                .castShadows(false)
                .build(newEngine, directionalLightEntity)
            scene?.addEntity(directionalLightEntity)
        } catch (failure: Exception) {
            cleanupFilamentObjects()
            throw failure
        }
    }

    private fun startAssetLoad(generation: Long, mappedGlb: MappedByteBuffer) {
        if (generation != loadGeneration.get() || !hasUsableSurface()) return
        val loader = assetLoader ?: run {
            failOnRenderThread(FilamentViewerStage.INITIALIZATION, "The glTF asset loader is unavailable.")
            return
        }
        try {
            mutableState.value = FilamentViewerState.LoadingAsset
            mappedGlb.position(0)
            val loaded = loader.createAsset(mappedGlb)
            if (loaded == null) {
                failOnRenderThread(FilamentViewerStage.ASSET, "Filament could not parse this GLB asset.")
                pendingGlb = null
                return
            }
            asset = loaded
            sourceDataReleased = false
            pendingGlb = null
            mappedSourceBuffers = mutableListOf(mappedGlb)
            resourceLoadGeneration = generation

            val uris = loaded.resourceUris
            if (uris.isEmpty()) {
                beginAsyncResourceLoad(generation, emptyList())
            } else {
                resolvingExternalResources = true
                mapExternalResourcesAsync(generation, loaded, uris)
            }
        } catch (failure: OutOfMemoryError) {
            pendingGlb = null
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.ASSET, "Not enough memory to parse this GLB asset.", outOfMemory = true)
        } catch (failure: Exception) {
            pendingGlb = null
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.ASSET, failure.message ?: "Filament could not parse this GLB asset.")
        }
    }

    private fun mapExternalResourcesAsync(generation: Long, loadedAsset: FilamentAsset, uris: Array<String>) {
        val sourceFile = currentSourceFileFor(generation)
            ?: run {
                resolvingExternalResources = false
                releaseAsset()
                failOnRenderThread(FilamentViewerStage.FILE, "The source GLB file is no longer available.")
                return
            }
        try {
            fileLoadFuture = fileExecutor.submit {
                try {
                    val mappedResources = mapRelativeResources(sourceFile, uris)
                    if (closed.get() || generation != loadGeneration.get()) return@submit
                    renderHandler.post {
                        if (closed.get() || generation != loadGeneration.get() || asset !== loadedAsset) return@post
                        resolvingExternalResources = false
                        beginAsyncResourceLoad(generation, mappedResources)
                    }
                } catch (cancelled: CancellationException) {
                    // Superseded, closed, or interrupted by surface-loss cleanup: not a load failure.
                } catch (failure: OutOfMemoryError) {
                    publishFailure(generation, FilamentViewerStage.FILE, "Not enough memory to map a GLB sidecar resource.", outOfMemory = true)
                } catch (failure: Exception) {
                    publishFailure(generation, FilamentViewerStage.FILE, failure.message ?: "Could not read a GLB sidecar resource.")
                }
            }
        } catch (failure: Exception) {
            resolvingExternalResources = false
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.FILE, failure.message ?: "Could not start the GLB resource load.")
        }
    }

    /* Kept on the render thread so the worker cannot read mutable controller state. */
    private var sourceFileByGeneration: Pair<Long, File>? = null

    private fun currentSourceFileFor(generation: Long): File? =
        sourceFileByGeneration?.takeIf { it.first == generation }?.second

    private fun beginAsyncResourceLoad(generation: Long, resources: List<Pair<String, MappedByteBuffer>>) {
        if (generation != loadGeneration.get() || asset == null) return
        try {
            val loader = resourceLoader ?: error("The glTF resource loader is unavailable.")
            resources.forEach { (uri, buffer) ->
                buffer.position(0)
                loader.addResourceData(uri, buffer)
                mappedSourceBuffers += buffer
            }
            val currentAsset = asset ?: return
            resourceLoadInProgress = true
            if (!loader.asyncBeginLoad(currentAsset)) {
                resourceLoadInProgress = false
                releaseAsset()
                failOnRenderThread(FilamentViewerStage.RESOURCES, "Filament could not start loading this GLB's buffers and textures.")
                return
            }
            resourceLoadGeneration = generation
            resourceLoadInProgress = true
            mutableState.value = FilamentViewerState.LoadingAsset
        } catch (failure: OutOfMemoryError) {
            cancelResourceLoad()
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.RESOURCES, "Not enough memory to upload this GLB's resources.", outOfMemory = true)
        } catch (failure: Exception) {
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.RESOURCES, failure.message ?: "Could not load this GLB's buffers and textures.")
        }
    }

    private fun updateAsyncResourceLoad() {
        val generation = resourceLoadGeneration
        if (generation != loadGeneration.get()) {
            cancelResourceLoad()
            return
        }
        try {
            val loader = resourceLoader ?: error("The glTF resource loader is unavailable.")
            loader.asyncUpdateLoad()
            if (loader.asyncGetLoadProgress() >= 1f) finishResourceLoad(generation)
        } catch (failure: OutOfMemoryError) {
            cancelResourceLoad()
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.RESOURCES, "Not enough memory to load this GLB's resources.", outOfMemory = true)
        } catch (failure: Exception) {
            cancelResourceLoad()
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.RESOURCES, failure.message ?: "Could not finish loading this GLB's resources.")
        }
    }

    private fun finishResourceLoad(generation: Long) {
        if (generation != loadGeneration.get()) return
        val loaded = asset ?: return
        try {
            loaded.releaseSourceData()
            sourceDataReleased = true
            resourceLoader?.evictResourceData()
            mappedSourceBuffers.clear()
            scene?.addEntities(loaded.entities)
            val box = loaded.boundingBox
            if (!box.isEmpty) {
                boundsCenter = box.center
                boundsHalfExtent = box.halfExtent
            } else {
                boundsCenter = floatArrayOf(0f, 0.95f, 0f)
                boundsHalfExtent = floatArrayOf(0.45f, 0.95f, 0.35f)
            }
            animator = loaded.instance.animator
            val currentAnimator = animator
            animationNames = if (currentAnimator == null) emptyList() else {
                (0 until currentAnimator.animationCount).map { index ->
                    currentAnimator.getAnimationName(index).takeIf { it.isNotBlank() } ?: "Animation ${index + 1}"
                }
            }
            activeAnimation = if (animationNames.isNotEmpty()) 0 else -1
            animationPositionSeconds = 0f
            animationPlaying = activeAnimation >= 0
            previousAnimationFrameNanos = 0L
            currentAnimator?.let {
                if (activeAnimation >= 0) {
                    it.applyAnimation(activeAnimation, 0f)
                    it.updateBoneMatrices()
                }
            }
            resourceLoadInProgress = false
            resolvingExternalResources = false
            lastAppliedCamera = null
            applyCameraIfChanged(force = true)
            publishReadyIfPossible()
        } catch (failure: OutOfMemoryError) {
            cancelResourceLoad()
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.RESOURCES, "Not enough memory to finalize this GLB asset.", outOfMemory = true)
        } catch (failure: Exception) {
            cancelResourceLoad()
            releaseAsset()
            failOnRenderThread(FilamentViewerStage.RESOURCES, failure.message ?: "Could not finalize this GLB asset.")
        }
    }

    private fun advanceAnimation(frameTimeNanos: Long) {
        val currentAnimator = animator ?: return
        val index = activeAnimation
        if (index !in 0 until currentAnimator.animationCount) return
        val previous = previousAnimationFrameNanos
        previousAnimationFrameNanos = frameTimeNanos
        if (!animationPlaying || previous <= 0L || frameTimeNanos <= previous) return

        val duration = currentAnimator.getAnimationDuration(index)
        if (!duration.isFinite() || duration <= 0f) return
        val deltaSeconds = ((frameTimeNanos - previous).toDouble() / NANOS_PER_SECOND)
            .toFloat()
            .coerceAtMost(MAX_ANIMATION_FRAME_DELTA_SECONDS)
        animationPositionSeconds = advanceLoopingAnimationTime(
            animationPositionSeconds,
            deltaSeconds,
            duration,
        )
        currentAnimator.applyAnimation(index, animationPositionSeconds)
        currentAnimator.updateBoneMatrices()
    }

    private fun applyCameraIfChanged(force: Boolean = false) {
        val currentCamera = camera ?: return
        val settings = latestCamera
        if (!force && settings == lastAppliedCamera) return
        val pose = filamentCameraPose(
            settings,
            boundsCenter[0], boundsCenter[1], boundsCenter[2],
            boundsHalfExtent[0], boundsHalfExtent[1], boundsHalfExtent[2],
        )
        val width = surfaceWidth.coerceAtLeast(1)
        val height = surfaceHeight.coerceAtLeast(1)
        currentCamera.setProjection(
            settings.fovDeg.toDouble(),
            width.toDouble() / height.toDouble(),
            pose.near.toDouble(),
            pose.far.toDouble(),
            Camera.Fov.VERTICAL,
        )
        currentCamera.lookAt(
            pose.eyeX.toDouble(), pose.eyeY.toDouble(), pose.eyeZ.toDouble(),
            pose.targetX.toDouble(), pose.targetY.toDouble(), pose.targetZ.toDouble(),
            0.0, 1.0, 0.0,
        )
        lastAppliedCamera = settings
    }

    private fun publishReadyIfPossible() {
        if (!hasUsableSurface() || swapChain == null || asset == null || resourceLoadInProgress || resolvingExternalResources) {
            if (asset == null && mutableState.value !is FilamentViewerState.Failed) {
                mutableState.value = if (hasUsableSurface()) FilamentViewerState.WaitingForAsset else FilamentViewerState.WaitingForSurface
            }
            return
        }
        mutableState.value = FilamentViewerState.Ready(animationNames.size, animationNames.toList())
    }

    private fun cancelResourceLoad() {
        if (resourceLoadInProgress) {
            runCatching { resourceLoader?.asyncCancelLoad() }
            resourceLoadInProgress = false
            runCatching { resourceLoader?.evictResourceData() }
        }
        resolvingExternalResources = false
    }

    private fun releaseAsset() {
        pendingGlb = null
        val oldAsset = asset
        if (oldAsset != null) {
            runCatching { scene?.removeEntities(oldAsset.entities) }
            if (!sourceDataReleased) runCatching { oldAsset.releaseSourceData() }
            runCatching { assetLoader?.destroyAsset(oldAsset) }
        }
        sourceDataReleased = true
        asset = null
        animator = null
        animationNames = emptyList()
        activeAnimation = -1
        animationPositionSeconds = 0f
        animationPlaying = false
        previousAnimationFrameNanos = 0L
        mappedSourceBuffers.clear()
        boundsCenter = floatArrayOf(0f, 0.95f, 0f)
        boundsHalfExtent = floatArrayOf(0.45f, 0.95f, 0.35f)
        lastAppliedCamera = null
    }

    private fun destroySwapChain() {
        val current = swapChain ?: return
        swapChain = null
        runCatching { engine?.destroySwapChain(current) }
    }

    private fun cleanupFilamentObjects() {
        cancelResourceLoad()
        destroySwapChain()
        releaseAsset()
        runCatching { resourceLoader?.destroy() }
        resourceLoader = null
        runCatching { assetLoader?.destroy() }
        assetLoader = null
        runCatching { materialProvider?.destroyMaterials() }
        runCatching { materialProvider?.destroy() }
        materialProvider = null

        val currentEngine = engine
        if (currentEngine != null) {
            runCatching { scene?.removeAllEntities() }
            if (directionalLightEntity != 0) {
                runCatching { currentEngine.lightManager.destroy(directionalLightEntity) }
                runCatching { currentEngine.destroyEntity(directionalLightEntity) }
            }
            if (cameraEntity != 0) {
                runCatching { currentEngine.destroyCameraComponent(cameraEntity) }
                runCatching { currentEngine.destroyEntity(cameraEntity) }
            }
            runCatching { renderer?.let(currentEngine::destroy) }
            runCatching { view?.let(currentEngine::destroy) }
            runCatching { scene?.let(currentEngine::destroy) }
            renderer = null
            view = null
            scene = null
            camera = null
            cameraEntity = 0
            directionalLightEntity = 0
            runCatching { currentEngine.flushAndWait() }
            runCatching { currentEngine.destroy() }
        }
        engine = null
        lastAppliedCamera = null
    }

    private fun cleanupAndStopRenderThread() {
        try {
            renderHandler.removeCallbacksAndMessages(null)
            cleanupFilamentObjects()
            pendingGlb = null
            sourceFileByGeneration = null
            surface = null
            surfaceWidth = 0
            surfaceHeight = 0
            mutableState.value = FilamentViewerState.Closed
        } finally {
            renderThread.quitSafely()
            closeComplete.countDown()
        }
    }

    private fun hasUsableSurface(): Boolean =
        surface?.isValid == true && surfaceWidth > 0 && surfaceHeight > 0

    private fun postToRenderThread(action: () -> Unit) {
        if (closed.get()) return
        renderHandler.post {
            if (closed.get()) return@post
            try {
                action()
            } catch (failure: OutOfMemoryError) {
                failOnRenderThread(FilamentViewerStage.SURFACE, "Not enough memory to initialize the Filament surface.", outOfMemory = true)
            } catch (failure: Exception) {
                failOnRenderThread(FilamentViewerStage.SURFACE, failure.message ?: "The Filament viewer operation failed.")
            }
        }
    }

    private fun publishFailure(
        generation: Long,
        stage: FilamentViewerStage,
        message: String,
        outOfMemory: Boolean = false,
    ) {
        if (closed.get()) return
        renderHandler.post {
            if (!closed.get() && generation == loadGeneration.get()) {
                resolvingExternalResources = false
                if (asset != null && !resourceLoadInProgress) releaseAsset()
                failOnRenderThread(stage, message.take(MAX_ERROR_LENGTH), outOfMemory)
            }
        }
    }

    private fun failOnRenderThread(stage: FilamentViewerStage, message: String, outOfMemory: Boolean = false) {
        if (!closed.get()) mutableState.value = FilamentViewerState.Failed(stage, message.take(MAX_ERROR_LENGTH), outOfMemory)
    }

    private fun mapGlb(file: File): MappedByteBuffer {
        require(file.name.endsWith(".glb", ignoreCase = true)) { "The generated asset must be a .glb file." }
        require(file.isFile && file.canRead()) { "The generated GLB file is missing or unreadable." }
        FileChannel.open(file.toPath(), StandardOpenOption.READ).use { channel ->
            val size = channel.size()
            require(size in GLB_HEADER_BYTES.toLong()..Int.MAX_VALUE.toLong()) { "The GLB file is empty or exceeds Filament's buffer limit." }
            val mapped = channel.map(FileChannel.MapMode.READ_ONLY, 0L, size)
            val header = mapped.duplicate().order(ByteOrder.LITTLE_ENDIAN)
            require(header.int == GLB_MAGIC) { "The file does not have a valid GLB header." }
            require(header.int == GLB_VERSION) { "Only GLB version 2 is supported." }
            require(header.int.toLong() == size) { "The GLB header length does not match the file size." }
            mapped.position(0)
            return mapped
        }
    }

    private fun mapRelativeResources(sourceGlb: File, uris: Array<String>): List<Pair<String, MappedByteBuffer>> {
        val root = sourceGlb.canonicalFile.parentFile ?: error("The GLB file has no parent directory.")
        val rootPath = root.toPath().normalize()
        return uris.distinct().map { uriText ->
            if (Thread.currentThread().isInterrupted) throw CancellationException("GLB sidecar mapping was interrupted.")
            val uri = URI(uriText)
            require(uri.scheme == null && uri.rawAuthority == null && uri.query == null && uri.fragment == null) {
                "Network and absolute GLB resources are not supported: $uriText"
            }
            val relativePath = uri.path?.takeIf { it.isNotBlank() } ?: error("The GLB contains an empty resource URI.")
            val resolved = rootPath.resolve(relativePath).normalize()
            require(resolved.startsWith(rootPath)) { "A GLB resource URI escapes its source directory: $uriText" }
            val resourceFile = resolved.toFile()
            require(resourceFile.isFile && resourceFile.canRead()) { "The GLB resource '$uriText' is missing or unreadable." }
            val realResourcePath = resourceFile.canonicalFile.toPath().normalize()
            require(realResourcePath.startsWith(rootPath)) { "A GLB resource URI escapes its source directory: $uriText" }
            FileChannel.open(resourceFile.toPath(), StandardOpenOption.READ).use { channel ->
                val size = channel.size()
                require(size in 1L..Int.MAX_VALUE.toLong()) { "The GLB resource '$uriText' is empty or too large." }
                uriText to channel.map(FileChannel.MapMode.READ_ONLY, 0L, size).also { it.position(0) }
            }
        }
    }

    companion object {
        private const val GLB_MAGIC = 0x46546C67
        private const val GLB_VERSION = 2
        private const val GLB_HEADER_BYTES = 12
        private const val MAX_ERROR_LENGTH = 300
        private const val NANOS_PER_SECOND = 1_000_000_000.0
        private const val MAX_ANIMATION_FRAME_DELTA_SECONDS = 0.1f
        private val FILAMENT_INIT_LOCK = Any()
        @Volatile private var filamentInitialized = false
    }
}
