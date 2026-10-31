/*
 * AvatarForge :: MainActivity.kt
 *
 * The app shell. It owns the real Compose navigation and the generation session:
 *
 *   * navigation between input -> progress -> GLB viewer;
 *   * the confirmed spec and running generation job;
 *   * the completed EngineResult whose GLB is handed to the viewer.
 *
 * The viewer composable owns its SurfaceView and controller; generation, export and rendering
 * implementations stay behind the SDK boundary.
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
import com.avatarforge.app.ui.AppBitmaps
import com.avatarforge.app.ui.DiagnosticsDialog
import com.avatarforge.app.ui.InputScreen
import com.avatarforge.app.ui.ProgressScreen
import com.avatarforge.app.ui.ViewerPanel
import com.avatarforge.app.ui.ViewerScreen
import com.avatarforge.sdk.AvatarEngine
import com.avatarforge.sdk.AvatarForge
import com.avatarforge.sdk.AvatarSpec
import com.avatarforge.sdk.EngineResult
import com.avatarforge.sdk.GenerationRequest
import com.avatarforge.sdk.ImageReference
import com.avatarforge.sdk.ImageRole
import com.avatarforge.sdk.JobHandle
import com.avatarforge.sdk.JobStatus
import com.avatarforge.sdk.render.CameraGesture
import com.avatarforge.sdk.render.CameraSettings
import com.avatarforge.sdk.render.DiagnosticRung
import com.avatarforge.sdk.render.FilamentViewerState
import com.avatarforge.sdk.recovery.RecoveryAdvisor
import com.avatarforge.sdk.recovery.RecoveryJournal
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

private enum class Screen { INPUT, PROGRESS, VIEWER }

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

/** Preserve the process-wide engine while Android replaces this Activity for a configuration change. */
internal fun releaseEngineOnActivityDestroy(isChangingConfigurations: Boolean, release: () -> Unit) {
    if (!isChangingConfigurations) release()
}

class MainActivity : ComponentActivity() {

    private var ownedEngine: AvatarEngine? = null

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
        val engine = ownedEngine
        ownedEngine = null
        releaseEngineOnActivityDestroy(isChangingConfigurations) { engine?.release() }
        super.onDestroy()
    }

    @Composable
    private fun App() {
        val context = LocalContext.current
        val engine = remember { AvatarEngine.get(context).also { ownedEngine = it } }
        val scope = rememberCoroutineScope()
        val analysisRequestGuard = remember { ImageAnalysisRequestGuard() }
        val health = remember { AvatarEngine.health() }

        var screen by remember { mutableStateOf(Screen.INPUT) }
        var prompt by remember { mutableStateOf("") }
        var spec by remember { mutableStateOf(AvatarSpec()) }
        var imageA by remember { mutableStateOf<Bitmap?>(null) }
        var imageB by remember { mutableStateOf<Bitmap?>(null) }
        var analysisNote by remember { mutableStateOf<String?>(null) }
        var imageAnalysisInProgress by remember { mutableStateOf(false) }
        /* The measurements the last successful reference-image analysis produced, kept so the
           spec the user sees and approves is exactly the spec that gets built. */
        var analysedImages by remember { mutableStateOf<List<ImageReference>>(emptyList()) }
        var job by remember { mutableStateOf<JobHandle?>(null) }
        var status by remember { mutableStateOf(JobStatus()) }
        var result by remember { mutableStateOf<EngineResult?>(null) }
        var retryRequest by remember { mutableStateOf<GenerationRequest?>(null) }
        var viewerState by remember { mutableStateOf<FilamentViewerState>(FilamentViewerState.WaitingForSurface) }

        var camera by remember { mutableStateOf(CameraSettings()) }
        var panel by remember { mutableStateOf(ViewerPanel.CAMERA) }
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

            fun begin(request: GenerationRequest) {
                retryRequest = request
                val started = engine.generate(request)
                job = started
                status = started.status.value
                screen = Screen.PROGRESS
                scope.launch {
                    val outcome = runCatching { started.await() }
                    if (job?.id != started.id) return@launch
                    if (started.isCancelled || screen != Screen.PROGRESS) {
                        status = started.status.value
                        return@launch
                    }
                    outcome.fold(
                        onSuccess = { generated ->
                            result = generated
                            screen = Screen.VIEWER
                        },
                        onFailure = {
                            /* Keep the terminal status on the progress screen. It carries the
                               engine's failure details and offers retry/back actions. */
                            status = started.status.value
                            screen = Screen.PROGRESS
                        },
                    )
                }
            }

            /* Off the main thread: the native analyser downsamples and histograms a 1600 px image,
               which is long enough to drop frames if it ran inside composition. */
            fun analyseReferences(bitmapA: Bitmap?, bitmapB: Bitmap?) {
                val request = analysisRequestGuard.begin()
                /* Do not let Generate reuse measurements from the previous selection while the
                   current pair is being analysed. */
                analysedImages = emptyList()
                analysisNote = null
                if (bitmapA == null) {
                    imageAnalysisInProgress = false
                    return
                }
                imageAnalysisInProgress = true
                /* The pixel copy into a direct buffer happens on the IO thread too: a 1600 px
                   ARGB image is up to 10 MB, and this is not work for the frame the user tapped on. */
                scope.launch {
                    val analysed = analyseReferenceImages(engine, bitmapA, bitmapB)
                    if (!analysisRequestGuard.isCurrent(request)) return@launch
                    imageAnalysisInProgress = false
                    if (analysed.isNullOrEmpty()) {
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
                analysisInProgress = imageAnalysisInProgress,
                analysisNote = analysisNote,
                enabled = health.usable,
                onGeneratePrompt = {
                    if (hasStorageForExport()) {
                        analysisRequestGuard.begin()
                        imageAnalysisInProgress = false
                        /* The prompt parser is run first so the spec the user sees is the spec that
                           gets built - including the measurements the prompt implied. */
                        val parsed = runCatching { engine.parsePrompt(prompt, spec) }.getOrDefault(spec)
                        spec = parsed
                        begin(GenerationRequest(parsed, prompt))
                    }
                },
                onGenerateImages = {
                    val a = imageA
                    if (a == null) {
                        Toast.makeText(context, "Choose Image A first.", Toast.LENGTH_SHORT).show()
                    } else if (hasStorageForExport()) {
                        val request = analysisRequestGuard.begin()
                        val selectedB = imageB
                        val cachedAnalysis = analysedImages
                        imageAnalysisInProgress = true
                        scope.launch {
                            /* Reuse the measurements already shown to the user. If absent, decode
                               packing and native analysis still run off-main; failure never silently
                               falls through to an image-free build. */
                            val analysed = if (cachedAnalysis.isNotEmpty()) cachedAnalysis
                                else analyseReferenceImages(engine, a, selectedB)
                            if (!analysisRequestGuard.isCurrent(request)) return@launch
                            imageAnalysisInProgress = false
                            if (analysed.isNullOrEmpty()) {
                                analysedImages = emptyList()
                                analysisNote =
                                    "Reference analysis failed; no image hints were applied and nothing was generated. Retry the image or use Generate from prompt."
                                return@launch
                            }
                            analysedImages = analysed
                            val resolved = engine.specFromImages(analysed, spec)
                            spec = resolved
                            analysisNote = analysed.joinToString("\n", transform = ::describeAnalysis)
                            begin(
                                GenerationRequest(
                                    spec = resolved,
                                    prompt = prompt,
                                    imageA = analysed.firstOrNull(),
                                    imageB = analysed.getOrNull(1),
                                ),
                            )
                        }
                    }
                },
            )

            Screen.PROGRESS -> ProgressScreen(
                status = status,
                onCancel = {
                    job?.takeIf { it.status.value.running }?.cancel()
                    screen = Screen.INPUT
                },
                onRetry = {
                    retryRequest?.let { request ->
                        if (hasStorageForExport()) begin(request)
                    }
                },
            )

            Screen.VIEWER -> {
                val finished = result
                if (finished == null) {
                    /* Never show a success path without a completed engine result. */
                    LaunchedEffect(Unit) { screen = Screen.INPUT }
                } else {
                    ViewerScreen(
                        result = finished,
                        camera = camera,
                        onCamera = { camera = CameraGesture.clamped(it) },
                        onBack = { screen = Screen.INPUT },
                        onExport = {
                            val file = finished.glbFile
                            val message = if (file.isFile) {
                                "Stored: ${file.absolutePath} (%.1f MB)".format(file.length() / 1048576.0)
                            } else {
                                "The exported file is missing at ${file.absolutePath}. Export again with Save to… ."
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
                        panel = panel,
                        onPanel = { panel = it },
                        onViewerState = { viewerState = it },
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
                        text = { Text("Camera controls") },
                        enabled = result != null,
                        onClick = {
                            menuExpanded = false
                            if (result != null) {
                                panel = ViewerPanel.CAMERA
                                screen = Screen.VIEWER
                            }
                        },
                    )
                }
            }
        }
        if (showDiagnostics) {
            val records = recoveryJournal.recent(20)
            val report = buildString {
                append(AvatarForge.deviceReport(context))
                appendLine()
                appendLine("GLB viewer")
                appendLine("Last controller state: $viewerState")
                val controllerError = (viewerState as? FilamentViewerState.Failed)?.message
                appendLine("Last error  : ${controllerError ?: "none"}")
                appendLine("GPU timing, renderer settings, and recovery controls are not exposed for this GLB viewer")
                appendLine("Camera      : yaw %.1f°, pitch %.1f°, distance %.2f m, targetY %.2f m, fov %.1f°".format(
                    camera.yawDeg, camera.pitchDeg, camera.distance, camera.targetY, camera.fovDeg))
                appendLine()
                appendLine("Journal     : ${if (recoveryJournal.isAvailable) "available" else "unavailable (${recoveryJournal.lastStorageError})"}")
                appendLine("Previous journal recommendation: ${RecoveryAdvisor.recommendation(records)}")
                appendLine("Recent events:")
                if (records.isEmpty()) appendLine("  none") else records.takeLast(12).forEach { appendLine("  ${it.toReportLine()}") }
            }
            DiagnosticsDialog(
                report = report,
                recoveryStatus = "not available for the GLB viewer",
                selectedDiagnosticRung = DiagnosticRung.PRODUCTION_AVATAR,
                onDiagnosticRungSelected = {},
                onSafeRecovery = {},
                onExportReport = {
                    recoveryReportToSave = report
                    recoveryReportLauncher.launch("avatarforge-recovery-${System.currentTimeMillis()}.txt")
                },
                onClearJournal = { recoveryJournal.clear() },
                onDismiss = { showDiagnostics = false },
                rendererControlsAvailable = false,
            )
        }
    }
}
}
