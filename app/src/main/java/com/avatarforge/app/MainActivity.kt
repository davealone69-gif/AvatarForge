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
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.avatarforge.app.ui.AppBitmaps
import com.avatarforge.app.ui.DiagnosticsDialog
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
import com.avatarforge.sdk.render.RenderThermalPolicy
import com.avatarforge.sdk.render.SettingsStore
import com.avatarforge.sdk.render.RendererInfo
import com.avatarforge.sdk.render.RendererStats
import com.avatarforge.sdk.recovery.RecoveryAdvisor
import com.avatarforge.sdk.recovery.RecoveryBackend
import com.avatarforge.sdk.recovery.RecoveryJournal
import com.avatarforge.sdk.recovery.RecoveryRecordKind
import com.avatarforge.sdk.recovery.RendererRecoveryPolicy
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private enum class Screen { INPUT, PROGRESS, VIEWER }

/** Zero-valued frame stats used before a renderer exists; never presented as a real measurement. */
private val EMPTY_RENDERER_STATS = RendererStats(0f, 0f, 0f, 0f, 0, 0, 0, 0, 1, 0, 0)

/** One analysed reference image, rendered as the card the user sees before pressing Generate. */
private fun describeAnalysis(ref: ImageReference): String {
    val an = ref.analysis
    if (an == null) return "no analysis available"
    return buildString {
        append(if (ref.role == ImageRole.IDENTITY) "A: " else "B: ")
        append("subject %.0f%% of frame".format(an.subjectCoverage * 100f))
        append(" · ")
        append(if (an.isFullBody) "full body detected" else "upper body: lower body inferred")
        append(" · skin L* %.0f".format(an.skinLab[0]))
        append(" · hair L* %.0f".format(an.hairLab[0]))
        append(" · garment metalness %.2f".format(an.metalConfidence))
        append(" · key light %.2f".format(an.keyLightIntensity))
    }
}

class MainActivity : ComponentActivity() {

    /** The render surface, owned by the activity so it survives recomposition. */
    private var surfaceView: RenderSurfaceView? = null

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
        super.onDestroy()
    }

    @Composable
    private fun App() {
        val context = LocalContext.current
        val engine = remember { AvatarEngine.get(context) }
        val scope = rememberCoroutineScope()
        val health = remember { AvatarEngine.health() }

        /* Stable stand-ins for the viewer's flows. The surface view is created during layout, so
           the first viewer composition has no view to read from; these keep one of each
           collectAsStateWithLifecycle() call site instead of a composable call behind a safe call. */
        val noRendererInfo = remember { MutableStateFlow<RendererInfo?>(null) }
        val noRendererStats = remember { MutableStateFlow(EMPTY_RENDERER_STATS) }
        val noRendererError = remember { MutableStateFlow<String?>(null) }

        var surfaceViewState by remember { mutableStateOf<RenderSurfaceView?>(null) }
        var screen by remember { mutableStateOf(Screen.INPUT) }
        var prompt by remember { mutableStateOf("") }
        var spec by remember { mutableStateOf(AvatarSpec()) }
        var imageA by remember { mutableStateOf<Bitmap?>(null) }
        var imageB by remember { mutableStateOf<Bitmap?>(null) }
        var analysisNote by remember { mutableStateOf<String?>(null) }
        /* The measurements the last successful reference-image analysis produced, kept so the
           spec the user sees and approves is exactly the spec that gets built. */
        var analysedImages by remember { mutableStateOf<List<ImageReference>>(emptyList()) }
        var job by remember { mutableStateOf<JobHandle?>(null) }
        var status by remember { mutableStateOf(JobStatus()) }
        var result by remember { mutableStateOf<EngineResult?>(null) }

        var camera by remember { mutableStateOf(CameraSettings()) }
        val settingsStore = remember { SettingsStore(context) }
        var settings by remember { mutableStateOf(settingsStore.loadRenderSettings() ?: AvatarForge.recommendedSettings(context)) }
        var effectiveSettings by remember { mutableStateOf(settings) }
        var panel by remember { mutableStateOf(ViewerPanel.CAMERA) }
        var clipIndex by remember { mutableStateOf(0) }
        var poseIndex by remember { mutableStateOf(POSE_CLIP) }
        var pickedRegion by remember { mutableStateOf(-1) }
        var glbToSave by remember { mutableStateOf<EngineResult?>(null) }
        var menuExpanded by remember { mutableStateOf(false) }
        var showDiagnostics by remember { mutableStateOf(false) }
        val recoveryJournal = remember { RecoveryJournal(context.filesDir) }
        var recoveryReportToSave by remember { mutableStateOf<String?>(null) }

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
            /* One authoritative copy of the ARKit channel weights. Every slider writes here and the
               whole array is re-applied, so channels compose instead of overwriting each other. */
            var expressionWeights by remember { mutableStateOf(FloatArray(expressionNames.size)) }

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

            val recoveryReportLauncher = rememberLauncherForActivityResult(
                ActivityResultContracts.CreateDocument("text/plain"),
            ) { uri: Uri? ->
                val pending = recoveryReportToSave
                if (uri != null && pending != null) {
                    val written = runCatching {
                        context.contentResolver.openOutputStream(uri, "wt")?.use { out ->
                            out.write(pending.toByteArray(Charsets.UTF_8))
                        } ?: error("the chosen destination could not be opened")
                    }
                    Toast.makeText(
                        context,
                        if (written.isSuccess) "Recovery report saved" else "Export failed: ${written.exceptionOrNull()?.message}",
                        Toast.LENGTH_LONG,
                    ).show()
                }
                recoveryReportToSave = null
            }

            /* The export is 33-40 MB; starting a two-minute build that cannot be written is worse
               than refusing it up front. The check is the SDK's own, not a guess. */
            fun hasStorageForExport(): Boolean {
                if (AvatarForge.hasRoomForExport(context)) return true
                Toast.makeText(
                    context,
                    "Not enough free storage for the ~40 MB export. Free some space and try again.",
                    Toast.LENGTH_LONG,
                ).show()
                return false
            }

            fun begin(started: JobHandle) {
                job = started
                screen = Screen.PROGRESS
                scope.launch {
                    val outcome = runCatching { started.await() }
                    outcome.onSuccess {
                        result = it
                        /* A new avatar starts with a neutral face; the previous channel weights
                           belong to the previous mesh. */
                        expressionWeights = FloatArray(expressionNames.size)
                        surfaceView?.attachEngine(engine.nativeHandle)
                        screen = Screen.VIEWER
                    }.onFailure { error ->
                        Toast.makeText(
                            context,
                            "Avatar generation failed: ${error.message ?: error.javaClass.simpleName}",
                            Toast.LENGTH_LONG,
                        ).show()
                        screen = Screen.INPUT
                    }
                }
            }

            /* Off the main thread: the native analyser downsamples and histograms a 1600 px image,
               which is long enough to drop frames if it ran inside composition. */
            fun analyseReferences(bitmapA: Bitmap?, bitmapB: Bitmap?) {
                if (bitmapA == null) {
                    analysedImages = emptyList()
                    analysisNote = null
                    return
                }
                /* The pixel copy into a direct buffer happens on the IO thread too: a 1600 px
                   ARGB image is up to 10 MB, and this is not work for the frame the user tapped on. */
                scope.launch {
                    val analysed = withContext(Dispatchers.IO) {
                        runCatching {
                            val refA = ImageReference.fromBitmap(bitmapA, ImageRole.IDENTITY)
                            val refB = bitmapB?.let { ImageReference.fromBitmap(it, ImageRole.CLOTHING) }
                            engine.analyseImages(listOfNotNull(refA, refB))
                        }.getOrNull()
                    }
                    if (analysed == null) {
                        analysedImages = emptyList()
                        analysisNote =
                            "Reference analysis failed on this device; the prompt and the specification below are used unchanged."
                    } else {
                        analysedImages = analysed
                        spec = engine.specFromImages(analysed, spec)
                        analysisNote = analysed.joinToString("\n", transform = ::describeAnalysis)
                    }
                }
            }

        LaunchedEffect(job) { job?.status?.collect { status = it } }
        LaunchedEffect(surfaceViewState) {
            surfaceViewState?.effectiveSettings?.collect { effectiveSettings = it }
        }
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
                /* Analysis runs when the image is chosen, not when Generate is pressed: the card
                   above the spec editor then shows the measurements the kernel actually extracted,
                   and the user can correct them before a build starts. */
                onImageA = { picked ->
                    imageA = picked
                    analyseReferences(picked, imageB)
                },
                onImageB = { picked ->
                    imageB = picked
                    analyseReferences(imageA, picked)
                },
                analysisNote = analysisNote,
                enabled = health.usable,
                onGeneratePrompt = {
                    if (hasStorageForExport()) {
                        /* The prompt parser is run first so the spec the user sees is the spec that
                           gets built - including the measurements the prompt implied. */
                        val parsed = runCatching { engine.parsePrompt(prompt, spec) }.getOrDefault(spec)
                        spec = parsed
                        begin(engine.generate(GenerationRequest(parsed, prompt)))
                    }
                },
                onGenerateImages = {
                    val a = imageA
                    if (a == null) {
                        Toast.makeText(context, "Choose Image A first.", Toast.LENGTH_SHORT).show()
                    } else if (hasStorageForExport()) {
                        /* Reuse the analysis the user already saw. Only analyse here when that step
                           has not run yet (for example after a process restart). */
                        val analysed = analysedImages.ifEmpty {
                            val refA = ImageReference.fromBitmap(a, ImageRole.IDENTITY)
                            val refB = imageB?.let { ImageReference.fromBitmap(it, ImageRole.CLOTHING) }
                            runCatching { engine.analyseImages(listOfNotNull(refA, refB)) }
                                .getOrDefault(emptyList())
                        }
                        val resolved = if (analysed.isNotEmpty()) engine.specFromImages(analysed, spec) else spec
                        spec = resolved
                        begin(
                            engine.generate(
                                GenerationRequest(
                                    spec = resolved,
                                    prompt = prompt,
                                    imageA = analysed.firstOrNull(),
                                    imageB = analysed.getOrNull(1),
                                ),
                            ),
                        )
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
                    /* Never write state during composition: with no result there is nothing to
                       show, so route back on the next frame instead of mutating here. */
                    LaunchedEffect(Unit) { screen = Screen.INPUT }
                } else {
                    /* Lifecycle-aware: while the activity is stopped the 60 Hz stats stream is not
                       collected, so a backgrounded viewer does no Compose work at all. */
                    val rendererInfo by (surfaceViewState?.rendererInfo ?: noRendererInfo)
                        .collectAsStateWithLifecycle()
                    val stats by (surfaceViewState?.stats ?: noRendererStats)
                        .collectAsStateWithLifecycle()
                    val rendererError by (surfaceViewState?.error ?: noRendererError)
                        .collectAsStateWithLifecycle()
                    ViewerScreen(
                        result = finished,
                        engine = engine,
                        rendererError = rendererError,
                        onBack = { screen = Screen.INPUT },
                        rendererInfo = rendererInfo,
                        stats = stats,
                        camera = camera,
                        onCamera = { camera = it; surfaceView?.applyCamera(it) },
                        settings = settings,
                        onSettings = { settings = it; settingsStore.saveRenderSettings(it); surfaceView?.applySettings(it) },
                        onRetryRenderer = { requested ->
                            settings = requested
                            settingsStore.saveRenderSettings(requested)
                            surfaceView?.retryRenderer(requested)
                        },
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
                        expressionWeights = expressionWeights,
                        onExpression = { index, value ->
                            if (index in expressionWeights.indices) {
                                /* One array per edit, applied as a unit, on the render thread:
                                   channels accumulate instead of resetting each other. */
                                val next = expressionWeights.copyOf()
                                next[index] = value.coerceIn(0f, 1f)
                                expressionWeights = next
                                surfaceView?.applyBlendshapes(next)
                            }
                        },
                        onResetExpressions = {
                            val neutral = FloatArray(expressionWeights.size)
                            expressionWeights = neutral
                            surfaceView?.applyBlendshapes(neutral)
                        },
                        onExport = {
                            /* The build already wrote the .glb; this action re-checks that it is
                               really there and reports its measured size instead of pretending to
                               save something a second time. */
                            val file = finished.glbFile
                            val message = if (file.isFile) {
                                "Stored: ${file.absolutePath} (%.1f MB)".format(file.length() / 1048576.0)
                            } else {
                                "The exported file is missing at ${file.absolutePath}. Export again with Save to…."
                            }
                            Toast.makeText(context, message, Toast.LENGTH_LONG).show()
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
                            val pick = surfaceView?.pick(x, y)
                            pickedRegion = pick?.region ?: -1
                            surfaceView?.setOverlays(false, false, pickedRegion)
                            panel = ViewerPanel.INSPECT
                        },
                        surface = {
                            /* The generated avatar exists before the surface does, so the attach
                               happens here, once per result - not on every recomposition. */
                            LaunchedEffect(finished) { surfaceView?.attachEngine(engine.nativeHandle) }
                            AndroidView(
                                modifier = Modifier.fillMaxSize(),
                                factory = { ctx ->
                                    RenderSurfaceView(ctx).also { view ->
                                        surfaceView = view
                                        surfaceViewState = view
                                        view.bind(engine)
                                        view.attachEngine(engine.nativeHandle)
                                        view.applySettings(settings)
                                        view.applyCamera(camera)
                                        view.setFrameLoopEnabled(true)
                                    }
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
                        text = { Text("Self diagnostics") },
                        onClick = {
                            menuExpanded = false
                            showDiagnostics = true
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
        if (showDiagnostics) {
            val info = surfaceViewState?.rendererInfo?.value
            val frameStats = surfaceViewState?.stats?.value
            val viewerError = surfaceViewState?.error?.value
            val recoveryStatus = surfaceViewState?.recoveryStatus?.value
            val records = recoveryJournal.recent(20)
            val report = buildString {
                append(AvatarForge.deviceReport(context))
                appendLine()
                appendLine("Viewer")
                if (info == null) {
                    appendLine("Renderer    : not initialised")
                } else {
                    appendLine("Backend     : ${info.backend}")
                    appendLine("GPU         : ${info.deviceName}")
                    appendLine("API         : ${info.apiVersion}")
                    appendLine("Driver      : ${info.driver}")
                    appendLine("HDR / MSAA  : ${info.supportsFloatHdr} / ${info.supportsMsaa}")
                    appendLine("Anisotropy  : ${info.supportsAnisotropy} (capability; atlas requests up to 4x)")
                }
                if (frameStats != null) {
                    appendLine("FPS         : %.1f (%.2f ms presented-frame interval)".format(frameStats.fps, frameStats.frameMs))
                    appendLine("CPU submit  : %.2f ms; GPU time: %s".format(frameStats.cpuMs,
                        if (frameStats.gpuMs < 0f) "unknown (no timestamp query)" else "%.2f ms".format(frameStats.gpuMs)))
                    appendLine("Frames      : %d attempts, %d submissions, %d accepted presents".format(
                        frameStats.frameAttempts, frameStats.frameSubmissions, frameStats.presentRequestsAccepted))
                    appendLine("Submitted   : %d tris, %d draw calls".format(frameStats.triangles, frameStats.drawCalls))
                    appendLine("Textures    : %.1f MiB".format(frameStats.textureMiB))
                    appendLine("Quality     : level %d, MSAA %dx, shadow %d, bloom %d".format(
                        frameStats.qualityLevel, frameStats.msaaSamples, frameStats.shadowMapSize, frameStats.bloomLevels))
                }
                appendLine("Last error  : ${viewerError ?: "none"}")
                appendLine("Settings    : requested backend=GLES, quality=${settings.quality}")
                appendLine("Effective   : backend=GLES, quality=${effectiveSettings.quality}")
                appendLine("Thermal     : ${RenderThermalPolicy.describe(surfaceViewState?.thermalStatusForDiagnostics)}")
                appendLine("Camera      : yaw %.1f°, pitch %.1f°, distance %.2f m, targetY %.2f m, fov %.1f°".format(
                    camera.yawDeg, camera.pitchDeg, camera.distance, camera.targetY, camera.fovDeg))
                appendLine()
                appendLine("Self-improvement path (deterministic, no AI, no network)")
                appendLine("Recovery    : ${recoveryStatus?.let { "${it.phase}: ${it.message}" } ?: "idle"}")
                appendLine("Journal     : ${if (recoveryJournal.isAvailable) "available" else "unavailable (${recoveryJournal.lastStorageError})"}")
                appendLine("Next step   : ${RecoveryAdvisor.recommendation(records)}")
                appendLine("Recent events:")
                if (records.isEmpty()) appendLine("  none") else records.takeLast(12).forEach { appendLine("  ${it.toReportLine()}") }
            }
            DiagnosticsDialog(
                report = report,
                recoveryStatus = recoveryStatus?.let { "${it.phase}: ${it.message}" },
                onSafeRecovery = {
                    val view = surfaceView
                    if (view != null) {
                        view.runSafeRecovery()
                    } else {
                        val backend = RecoveryBackend.GLES
                        settings = RendererRecoveryPolicy.safeSettings(settings)
                        settingsStore.saveRenderSettings(settings)
                        recoveryJournal.record(
                            RecoveryRecordKind.MANUAL_SAFE_RECOVERY,
                            backend = backend.name,
                            candidate = "safe-${backend.name.lowercase()}",
                        )
                        Toast.makeText(context, "Safe settings armed for the next viewer", Toast.LENGTH_SHORT).show()
                    }
                },
                onExportReport = {
                    recoveryReportToSave = report
                    recoveryReportLauncher.launch("avatarforge-recovery-${System.currentTimeMillis()}.txt")
                },
                onClearJournal = {
                    surfaceView?.clearRecoveryJournal() ?: recoveryJournal.clear()
                },
                onDismiss = { showDiagnostics = false },
            )
        }
    }
}
}
