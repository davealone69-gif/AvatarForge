/*
 * AvatarForge :: MainActivity.kt
 *
 * The app shell. It owns exactly three things:
 *
 *   * the Compose navigation between input -> progress -> viewer;
 *   * the RenderSurfaceView, which owns the render thread and the GPU context;
 *   * the session state: the confirmed spec, the running job, the finished result.
 *
 * There is no generation logic here on purpose. Everything that builds, rigs, animates or renders
 * lives in the C++ kernel and renderer behind the SDK; this file is glue, and it is short enough to
 * read in one sitting - which is the point.
 *
 * The merged manifest declares no INTERNET permission, so "works in airplane mode" is enforced by
 * the platform rather than by good intentions.
 */
package com.avatarforge.app

import android.graphics.Bitmap
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.avatarforge.app.ui.AppBitmaps
import com.avatarforge.app.ui.InputScreen
import com.avatarforge.app.ui.ProgressScreen
import com.avatarforge.app.ui.ViewerPanel
import com.avatarforge.app.ui.POSE_CLIP
import com.avatarforge.app.ui.ViewerScreen
import com.avatarforge.app.ui.poseClipName
import com.avatarforge.sdk.AvatarEngine
import com.avatarforge.sdk.AvatarForge
import com.avatarforge.sdk.AvatarSpec
import com.avatarforge.sdk.EngineResult
import com.avatarforge.sdk.GenerationRequest
import com.avatarforge.sdk.ImageReference
import com.avatarforge.sdk.ImageRole
import com.avatarforge.sdk.JobHandle
import com.avatarforge.sdk.JobStatus
import com.avatarforge.sdk.render.CameraSettings
import com.avatarforge.sdk.render.RenderSurfaceView
import com.avatarforge.sdk.render.RenderSettings
import com.avatarforge.sdk.render.RenderProfileStore
import com.avatarforge.sdk.render.RendererStats
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private enum class Screen { INPUT, PROGRESS, VIEWER }

class MainActivity : ComponentActivity() {

    /** The render surface, owned by the activity so it survives recomposition. */
    private var surfaceView: RenderSurfaceView? = null
    private var engineForRelease: AvatarEngine? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        AppBitmaps.install(this)
        setContent {
            MaterialTheme {
                Surface(Modifier.fillMaxSize()) { App() }
            }
        }
    }

    override fun onDestroy() {
        surfaceView?.release()
        surfaceView = null
        if (isFinishing && !isChangingConfigurations) {
            engineForRelease?.release()
            engineForRelease = null
        }
        super.onDestroy()
    }

    @Composable
    private fun App() {
        val context = LocalContext.current
        val engine = remember {
            AvatarEngine.get(context).also { this@MainActivity.engineForRelease = it }
        }
        val scope = rememberCoroutineScope()
        val health = remember { AvatarEngine.health() }

        var surfaceViewState by remember { mutableStateOf<RenderSurfaceView?>(null) }
        var screen by remember { mutableStateOf(Screen.INPUT) }
        var prompt by remember { mutableStateOf("") }
        var spec by remember { mutableStateOf(AvatarSpec()) }
        var imageA by remember { mutableStateOf<Bitmap?>(null) }
        var imageB by remember { mutableStateOf<Bitmap?>(null) }
        var analysisNote by remember { mutableStateOf<String?>(null) }
        var generationError by remember { mutableStateOf<String?>(null) }
        var preparingInput by remember { mutableStateOf(false) }
        var job by remember { mutableStateOf<JobHandle?>(null) }
        var status by remember { mutableStateOf(JobStatus()) }
        var result by remember { mutableStateOf<EngineResult?>(null) }

        var camera by remember { mutableStateOf(CameraSettings()) }
        var settings by remember { mutableStateOf(RenderProfileStore.load(context, AvatarForge.recommendedSettings(context))) }
        var panel by remember { mutableStateOf(ViewerPanel.CAMERA) }
        var clipIndex by remember { mutableStateOf(0) }
        var poseIndex by remember { mutableStateOf(POSE_CLIP) }
        var pickedRegion by remember { mutableStateOf(-1) }
        var glbToSave by remember { mutableStateOf<EngineResult?>(null) }
        var menuExpanded by remember { mutableStateOf(false) }

        Box(Modifier.fillMaxSize()) {
            BackHandler(enabled = screen != Screen.INPUT) {
                when (screen) {
                    Screen.VIEWER -> screen = Screen.INPUT
                    Screen.PROGRESS -> {
                        job?.cancel()
                        screen = Screen.INPUT
                    }
                    Screen.INPUT -> Unit
                }
            }

            val expressionNames = remember { engine.blendshapeNames() }

        /* Storage Access Framework export: registered at composition scope, launched from the
           export panel. The app writes through the returned URI, so it needs no storage
           permission and no manifest entry. */
            val saveLauncher = rememberLauncherForActivityResult(
                ActivityResultContracts.CreateDocument("model/gltf-binary"),
            ) { uri: Uri? ->
                val pending = glbToSave
                if (uri != null && pending != null) {
                    val written = runCatching { AvatarForge.copyToDestination(context, pending.glbFile, uri) }
                    Toast.makeText(
                        context,
                        written.fold({ "Saved %.1f MB".format(it / 1048576.0) }, { "Export failed: ${it.message}" }),
                        Toast.LENGTH_LONG,
                    ).show()
                }
                glbToSave = null
            }

            fun begin(started: JobHandle) {
                job = started
                screen = Screen.PROGRESS
                scope.launch {
                    val outcome = runCatching { started.await() }
                    outcome.onSuccess {
                        result = it
                        spec = it.spec
                        surfaceView?.attachEngine(engine)
                        screen = Screen.VIEWER
                    }.onFailure { error ->
                        generationError = buildString {
                            append(error.javaClass.simpleName)
                            error.message?.takeIf { it.isNotBlank() }?.let { append(": ").append(it) }
                        }
                        screen = Screen.INPUT
                    }
                }
            }

        LaunchedEffect(job) { job?.status?.collect { status = it } }
        LaunchedEffect(Unit) {
            if (!health.usable) Toast.makeText(context, health.problem ?: "engine unavailable", Toast.LENGTH_LONG).show()
        }

        when (screen) {
            Screen.INPUT -> InputScreen(
                prompt = prompt,
                onPromptChange = { prompt = it },
                spec = spec,
                onSpecChange = { spec = it },
                imageA = imageA,
                imageB = imageB,
                onImageA = { imageA = it },
                onImageB = { imageB = it },
                analysisNote = analysisNote,
                generationError = generationError,
                enabled = health.usable && !preparingInput,
                onGeneratePrompt = {
                    /* Prompt parsing touches the native handle; keep even this short operation off
                       the UI thread and build the exact spec the user saw. */
                    val requestedPrompt = prompt
                    val requestedSpec = spec
                    generationError = null
                    preparingInput = true
                    scope.launch {
                        try {
                            val parsed = withContext(Dispatchers.IO) {
                                engine.parsePrompt(requestedPrompt, requestedSpec)
                            }
                            spec = parsed
                            begin(engine.generate(GenerationRequest(parsed, requestedPrompt)))
                        } catch (cancelled: kotlinx.coroutines.CancellationException) {
                            throw cancelled
                        } catch (error: Throwable) {
                            generationError = buildString {
                                append("Prompt analysis failed · ").append(error.javaClass.simpleName)
                                error.message?.takeIf { it.isNotBlank() }?.let { append(": ").append(it) }
                            }
                        } finally {
                            preparingInput = false
                        }
                    }
                },
                onGenerateImages = {
                    val a = imageA ?: return@InputScreen
                    val b = imageB
                    val requestedSpec = spec
                    val requestedPrompt = prompt
                    generationError = null
                    preparingInput = true
                    scope.launch {
                        try {
                            val (refA, refB) = withContext(Dispatchers.IO) {
                                ImageReference.fromBitmap(a, ImageRole.IDENTITY) to
                                    b?.let { ImageReference.fromBitmap(it, ImageRole.CLOTHING) }
                            }
                            val analysed = withContext(Dispatchers.IO) {
                                runCatching { engine.analyseImages(listOfNotNull(refA, refB)) }.getOrNull()
                            }
                            analysisNote = analysed?.joinToString("\n") { ref ->
                                val an = ref.analysis
                                if (an == null) "no analysis available" else buildString {
                                    append("subject %.0f%% of frame".format(an.subjectCoverage * 100f))
                                    append(" · ")
                                    append(if (an.isFullBody) "full body detected" else "upper body: lower body inferred")
                                    append(" · skin L* %.0f".format(an.skinLab[0]))
                                    append(" · hair L* %.0f".format(an.hairLab[0]))
                                    append(" · garment metalness %.2f".format(an.metalConfidence))
                                    append(" · key light %.2f".format(an.keyLightIntensity))
                                }
                            }
                            val resolved = withContext(Dispatchers.IO) {
                                val fromImages = analysed?.let { engine.specFromImages(it, requestedSpec) } ?: requestedSpec
                                if (requestedPrompt.isBlank()) fromImages else engine.parsePrompt(requestedPrompt, fromImages)
                            }
                            spec = resolved
                            begin(
                                engine.generate(
                                    GenerationRequest(
                                        spec = resolved,
                                        prompt = requestedPrompt,
                                        imageA = analysed?.firstOrNull() ?: refA,
                                        imageB = analysed?.getOrNull(1) ?: refB,
                                    ),
                                ),
                            )
                        } catch (cancelled: kotlinx.coroutines.CancellationException) {
                            throw cancelled
                        } catch (error: Throwable) {
                            generationError = buildString {
                                append("Image analysis failed · ").append(error.javaClass.simpleName)
                                error.message?.takeIf { it.isNotBlank() }?.let { append(": ").append(it) }
                            }
                        } finally {
                            preparingInput = false
                        }
                    }
                },
            )

            Screen.PROGRESS -> ProgressScreen(
                status = status,
                onCancel = {
                    job?.cancel()
                    screen = Screen.INPUT
                },
            )

            Screen.VIEWER -> {
                val finished = result
                if (finished == null) {
                    screen = Screen.INPUT
                } else {
                    val rendererInfo by surfaceViewState?.rendererInfo?.collectAsState() ?: remember { mutableStateOf(null) }
                    val stats by surfaceViewState?.stats?.collectAsState()
                        ?: remember { mutableStateOf(RendererStats(0f, 0f, 0f, 0f, 0, 0, 0, 0, 1, 0, 0)) }
                    val rendererError by surfaceViewState?.error?.collectAsState() ?: remember { mutableStateOf(null) }
                    val thermalStatus by surfaceViewState?.thermalStatusFlow?.collectAsState()
                        ?: remember { mutableStateOf<Int?>(null) }
                    ViewerScreen(
                        result = finished,
                        engine = engine,
                        rendererError = rendererError,
                        onBack = { screen = Screen.INPUT },
                        rendererInfo = rendererInfo,
                        stats = stats,
                        thermalStatus = thermalStatus,
                        camera = camera,
                        onCamera = { camera = it; surfaceView?.applyCamera(it) },
                        settings = settings,
                        onSettings = { settings = it; RenderProfileStore.save(context, it); surfaceView?.applySettings(it) },
                        panel = panel,
                        onPanel = { panel = it },
                        clipIndex = clipIndex,
                        onClip = {
                            clipIndex = it
                            poseIndex = POSE_CLIP
                            surfaceView?.setClip(it, 1f, true)
                        },
                        poseIndex = poseIndex,
                        onPose = { mode ->
                            /* Every pose mode is a clip the kernel already has: playback is simply
                               stopped (speed 0, no loop) when the pose is meant to be held. */
                            poseIndex = mode
                            val clip = if (mode == POSE_CLIP) clipIndex else engine.clipIndex(poseClipName(mode))
                            if (clip < 0) {
                                Toast.makeText(context, "clip '${poseClipName(mode)}' is not in the library", Toast.LENGTH_SHORT).show()
                            } else {
                                surfaceView?.setClip(clip, if (mode == POSE_CLIP) 1f else 0f, mode == POSE_CLIP)
                            }
                        },
                        expressionNames = expressionNames,
                        onExpression = { index, value ->
                            val weights = FloatArray(expressionNames.size)
                            weights[index] = value
                            surfaceView?.applyBlendshapes(weights)
                        },
                        onExport = {
                            Toast.makeText(
                                context,
                                "Stored in ${finished.glbFile.parentFile?.absolutePath}",
                                Toast.LENGTH_LONG,
                            ).show()
                        },
                        onShare = {
                            runCatching {
                                startActivity(
                                    android.content.Intent.createChooser(
                                        AvatarForge.shareIntent(context, finished),
                                        "Share avatar",
                                    ),
                                )
                            }.onFailure {
                                Toast.makeText(context, "Nothing can share model/gltf-binary here", Toast.LENGTH_SHORT).show()
                            }
                        },
                        onExportTo = {
                            glbToSave = finished
                            saveLauncher.launch("avatar-${finished.spec.seed}.glb")
                        },
                        pickedRegion = pickedRegion,
                        onPickRegion = { pickedRegion = it; surfaceView?.setOverlays(false, false, it) },
                        onTapModel = { x, y ->
                            scope.launch {
                                val pick = surfaceView?.pickAsync(x, y)
                                pickedRegion = pick?.region ?: -1
                                surfaceView?.setOverlays(false, false, pickedRegion)
                                panel = ViewerPanel.INSPECT
                            }
                        },
                        surface = {
                            /* The generated avatar exists before the surface does, so the attach
                               happens here, once per result - not on every recomposition. */
                            LaunchedEffect(finished) { surfaceView?.attachEngine(engine) }
                            AndroidView(
                                modifier = Modifier.fillMaxSize(),
                                factory = { ctx ->
                                    RenderSurfaceView(ctx).also { view ->
                                        surfaceView = view
                                        surfaceViewState = view
                                        view.bind(engine)
                                        view.attachEngine(engine)
                                        view.applySettings(settings)
                                        view.applyCamera(camera)
                                        view.setFrameLoopEnabled(true)
                                    }
                                },
                                onRelease = { view ->
                                    if (surfaceView === view) surfaceView = null
                                    if (surfaceViewState === view) surfaceViewState = null
                                    view.release()
                                },
                            )
                        },
                    )
                }
            }
        }
        // Grok-Girls Compose navigation pattern, adapted to destinations that actually exist
        // in AvatarForge. No placeholder routes are exposed.
        if (screen != Screen.PROGRESS) {
            Box(
                Modifier
                    .align(Alignment.TopEnd)
                    .safeDrawingPadding()
                    .padding(8.dp),
            ) {
                TextButton(onClick = { menuExpanded = true }) {
                    Text("☰ MENU")
                }
                DropdownMenu(
                    expanded = menuExpanded,
                    onDismissRequest = { menuExpanded = false },
                ) {
                    DropdownMenuItem(
                        text = { Text("Builder") },
                        onClick = {
                            menuExpanded = false
                            screen = Screen.INPUT
                        },
                    )
                    DropdownMenuItem(
                        text = { Text("3D Viewer") },
                        enabled = result != null,
                        onClick = {
                            menuExpanded = false
                            if (result != null) screen = Screen.VIEWER
                        },
                    )
                    DropdownMenuItem(
                        text = { Text("Render settings") },
                        enabled = result != null,
                        onClick = {
                            menuExpanded = false
                            if (result != null) {
                                panel = ViewerPanel.RENDER
                                screen = Screen.VIEWER
                            }
                        },
                    )
                }
            }
        }
    }
}
}
