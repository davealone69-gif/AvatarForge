/*
 * AvatarForge :: ViewerScreen.kt
 *
 * The result viewer/editor. This is where the app has to be honest about what it can do:
 *
 *   * the surface is the real renderer (Vulkan or GLES 3.1) drawing the real mesh - the same mesh
 *     the .glb contains, deformed by the same rig;
 *   * pose and expression controls drive the kernel (applyPose / applyBlendshapes), which deforms
 *     the vertices the renderer then uploads. There is no separate "preview rig";
 *   * the clip strip plays the baked library clips through the renderer's own clock;
 *   * tapping the model picks the `af_region` under the finger (a real ray/mesh intersection) and
 *     the inspector names it, with the option to highlight it;
 *   * Export writes the .glb through the FileProvider - to a file, or to any app the user picks.
 *
 * Anything that could not be built to standard would not be on this screen.
 */
package com.avatarforge.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import com.avatarforge.sdk.AvatarEngine
import com.avatarforge.sdk.DeviceResourceGovernor
import com.avatarforge.sdk.EngineResult
import com.avatarforge.sdk.summary
import com.avatarforge.sdk.render.CameraSettings
import com.avatarforge.sdk.render.RenderSettings
import com.avatarforge.sdk.render.Quality
import com.avatarforge.sdk.render.RendererInfo
import com.avatarforge.sdk.render.RendererStats

/** Which editing panel is open. Only one at a time: this is a phone, not a workstation. */
enum class ViewerPanel { CAMERA, RENDER, SYSTEM_CHECK, POSE, FACE, INSPECT, EXPORT }

@Composable
fun ViewerScreen(
    result: EngineResult,
    engine: AvatarEngine,
    rendererError: String?,
    onBack: () -> Unit,
    rendererInfo: RendererInfo?,
    stats: RendererStats,
    thermalStatus: Int?,
    camera: CameraSettings,
    onCamera: (CameraSettings) -> Unit,
    settings: RenderSettings,
    onSettings: (RenderSettings) -> Unit,
    panel: ViewerPanel,
    onPanel: (ViewerPanel) -> Unit,
    clipIndex: Int,
    onClip: (Int) -> Unit,
    poseIndex: Int,
    onPose: (Int) -> Unit,
    onExpression: (Int, Float) -> Unit,
    expressionNames: List<String>,
    onExport: () -> Unit,
    onShare: () -> Unit,
    onExportTo: () -> Unit,
    pickedRegion: Int,
    onPickRegion: (Int) -> Unit,
    /** View-space tap, forwarded to the host which performs the pick against the live camera. */
    onTapModel: (Float, Float) -> Unit,
    modifier: Modifier = Modifier,
    /** The SurfaceView, hoisted so this screen stays previewable in Compose tooling. */
    surface: @Composable () -> Unit,
) {
    Box(modifier.fillMaxSize().background(MaterialTheme.colorScheme.surface)) {
        surface()

        /* Model gestures: drag to orbit, pinch to zoom, tap to identify what is under the finger.
           The tap is forwarded to the host, which owns the render surface and turns it into a real
           ray/mesh intersection (af_engine_pick_region) using the same camera the frame was drawn
           with - so what is picked is what is on screen. */
        Box(
            Modifier
                .fillMaxSize()
                .pointerInput(Unit) {
                    detectTransformGestures { _, pan, zoom, _ ->
                        val yaw = camera.yawDeg - pan.x * 0.25f
                        val pitch = (camera.pitchDeg + pan.y * 0.20f).coerceIn(-80f, 80f)
                        val distance = (camera.distance / zoom).coerceIn(0.6f, 8f)
                        onCamera(
                            camera.copy(
                                yawDeg = ((yaw % 360f) + 360f) % 360f,
                                pitchDeg = pitch,
                                distance = distance,
                            )
                        )
                    }
                }
                .pointerInput(onTapModel) {
                    detectTapGestures { offset -> onTapModel(offset.x, offset.y) }
                },
        )

        if (panel != ViewerPanel.SYSTEM_CHECK) Column(
            Modifier
                .align(Alignment.TopStart)
                .statusBarsPadding()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            TextButton(onClick = onBack) { Text("← Back to editor") }
            Header(result, rendererInfo, stats, settings)
            if (rendererError != null) {
                Card(
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                ) {
                    Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text("3D viewer failed to start", fontWeight = FontWeight.SemiBold)
                        Text(rendererError, style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }

        if (panel != ViewerPanel.SYSTEM_CHECK) Row(
            Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .navigationBarsPadding()
                .horizontalScroll(rememberScrollState())
                .padding(8.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            ViewerPanel.entries.forEach { entry ->
                FilterChip(
                    selected = panel == entry,
                    onClick = { onPanel(entry) },
                    label = { Text(entry.name.lowercase().replace("_", " ")) },
                )
            }
        }

        if (panel == ViewerPanel.SYSTEM_CHECK) {
            Card(
                modifier = Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(8.dp),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
            ) {
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    TextButton(onClick = { onPanel(ViewerPanel.CAMERA) }) { Text("← Back to 3D viewer") }
                    SystemCheckPanel(result, rendererError, rendererInfo, stats, thermalStatus, settings)
                }
            }
        } else if (panel != ViewerPanel.CAMERA) {
            Card(
                modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth().navigationBarsPadding().padding(bottom = 56.dp),
                shape = RoundedCornerShape(topStart = 18.dp, topEnd = 18.dp),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
            ) {
                Column(
                    Modifier.heightIn(max = 440.dp).verticalScroll(rememberScrollState()).padding(14.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    when (panel) {
                        ViewerPanel.POSE -> PosePanel(poseIndex, onPose, clipIndex, onClip, engine)
                        ViewerPanel.FACE -> FacePanel(expressionNames, onExpression)
                        ViewerPanel.INSPECT -> InspectPanel(result, pickedRegion, onPickRegion, engine)
                        ViewerPanel.EXPORT -> ExportPanel(onExport, onShare, onExportTo, result)
                        ViewerPanel.RENDER -> RenderPanel(settings, onSettings, stats, rendererInfo, thermalStatus)
                        ViewerPanel.SYSTEM_CHECK, ViewerPanel.CAMERA -> Unit
                    }
                }
            }
        }
    }
}

@Composable
private fun Header(result: EngineResult, info: RendererInfo?, stats: RendererStats, settings: RenderSettings) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f))) {
        Column(Modifier.padding(10.dp)) {
            Text(result.summary(), style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.SemiBold)
            Text(
                "Target stature %.2f m (spec value, not measured mesh bounds)".format(result.spec.heightM),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            val backend = info?.let { "${it.backend} · ${it.deviceName}" } ?: "initialising GPU…"
            Text(
                "$backend · %.0f FPS (%.1f ms frame interval) · %s".format(
                    stats.fps, stats.frameMs,
                    if (stats.meetsTarget) "on target" else if (settings.quality == Quality.AUTO) "below 30 FPS; AUTO can reduce effects" else "below 30 FPS; selected preset is fixed",
                ),
                style = MaterialTheme.typography.labelSmall,
                color = if (stats.meetsTarget) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.error,
            )
            Text(
                "%.0f s to build · %d tris on screen".format(
                    result.timings.values.sum() / 1000f,
                    stats.triangles,
                ),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

internal const val POSE_REST = 0
internal const val POSE_TPOSE = 1
internal const val POSE_APOSE = 2
internal const val POSE_CLIP = 3
internal val POSE_MODES = listOf("Rest", "T-pose", "A-pose", "Clip playback")

/** Authored clip that backs each held pose mode. The bind pose is the library's own "rest" entry,
 *  so every mode is a real clip rather than a special case in the UI. */
internal fun poseClipName(mode: Int): String = when (mode) {
    POSE_TPOSE -> "t_pose"
    POSE_APOSE -> "a_pose"
    else -> "rest"
}

@Composable
private fun PosePanel(poseIndex: Int, onPose: (Int) -> Unit, clipIndex: Int, onClip: (Int) -> Unit, engine: AvatarEngine) {
    Text("Animation", style = MaterialTheme.typography.titleSmall)
    Text(
        "The clip library is authored key data baked into the engine, not a procedural stub. Playback runs on the renderer's clock and deforms the mesh every frame.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    /* Pose source: the three held poses are library entries with playback stopped (speed 0, no
       loop), so what is on screen is the first key of a real clip rather than a separate code
       path that could drift from it. */
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        POSE_MODES.forEachIndexed { i, label ->
            FilterChip(selected = poseIndex == i, onClick = { onPose(i) }, label = { Text(label) })
        }
    }
    if (poseIndex == POSE_CLIP) {
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for (clip in 0 until engine.clipCount) {
                val duration = engine.clipDuration(clip)
                FilterChip(
                    selected = clipIndex == clip,
                    onClick = { onClip(clip) },
                    label = { Text("%s · %.1fs".format(engine.clipName(clip), duration)) },
                )
            }
        }
        Text("Looping playback of the selected clip.", style = MaterialTheme.typography.bodySmall)
    } else {
        /* Hold the first key of a real library clip: playback is stopped (speed 0, no loop), so the
           skeleton is re-deformed through the same skinning path the animated clips use. */
        val held = remember(poseIndex) {
            val wanted = poseClipName(poseIndex)
            engine.clipName(engine.clipIndex(wanted)).ifEmpty { wanted }
        }
        Text(
            "Held pose: %s. Playback is stopped and the skeleton is re-deformed at this key, which is the same skinning path the animated clips use.".format(held),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun FacePanel(names: List<String>, onExpression: (Int, Float) -> Unit) {
    var index by remember { mutableStateOf(0) }
    var value by remember { mutableStateOf(0f) }
    Text("Facial expressions (ARKit blendshapes)", style = MaterialTheme.typography.titleSmall)
    Text(
        "%d channels, all of which move real vertices: the same morph targets the .glb contains.".format(names.size),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        names.forEachIndexed { i, name ->
            FilterChip(selected = index == i, onClick = { index = i; onExpression(i, value) }, label = { Text(name) })
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(names.getOrElse(index) { "" }, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
        Text("%.2f".format(value), style = MaterialTheme.typography.bodySmall)
    }
    Slider(
        value = value,
        onValueChange = { value = it; onExpression(index, it) },
        valueRange = 0f..1f,
    )
    TextButton(onClick = { value = 0f; names.indices.forEach { onExpression(it, 0f) } }) { Text("Reset all channels") }
}

@Composable
private fun InspectPanel(result: EngineResult, picked: Int, onPick: (Int) -> Unit, engine: AvatarEngine) {
    Text("Region inspector", style = MaterialTheme.typography.titleSmall)
    Text(
        "Tap the model to identify what the ray hit. The list below is the engine's own region table, so the names and the ids cannot drift.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Text(
        if (picked < 0) "Nothing picked yet" else "Picked: ${engine.regionName(picked)} (region $picked)",
        style = MaterialTheme.typography.bodyMedium,
        fontWeight = FontWeight.SemiBold,
    )
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        result.regionNames.forEachIndexed { id, name ->
            AssistChip(onClick = { onPick(if (picked == id) -1 else id) }, label = { Text(name) })
        }
    }
}

@Composable
private fun ExportPanel(onExport: () -> Unit, onShare: () -> Unit, onExportTo: () -> Unit, result: EngineResult) {
    Text("Export", style = MaterialTheme.typography.titleSmall)
    Text(
        "%.1f MB binary glTF (2.0): mesh, UV-mapped PBR textures, 71-joint skeleton, skin weights, %d ARKit morph targets and the 15 baked clips - one self-contained file."
            .format(result.glbBytes / 1048576.0, result.blendshapeCount),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Text(result.glbFile.absolutePath, style = MaterialTheme.typography.labelSmall)
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        AssistChip(onClick = onExport, label = { Text("Save to app storage") })
        AssistChip(onClick = onExportTo, label = { Text("Save to…") })
        AssistChip(onClick = onShare, label = { Text("Share") })
    }
}

@Composable
private fun RenderPanel(settings: RenderSettings, onSettings: (RenderSettings) -> Unit, stats: RendererStats, info: RendererInfo?, thermalStatus: Int?) {
    Text("Renderer", style = MaterialTheme.typography.titleSmall)
    Text("Quality presets change MSAA and post-processing costs, not the source mesh.", style = MaterialTheme.typography.bodySmall)
    Row(
        modifier = Modifier.horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        listOf(Quality.AUTO, Quality.LOW, Quality.MEDIUM, Quality.HIGH).forEach { quality ->
            FilterChip(
                selected = settings.quality == quality,
                onClick = { onSettings(settings.copy(quality = quality)) },
                label = { Text(quality.name) },
            )
        }
    }
    if (info == null) {
        Text(
            "No renderer is active here. These settings are saved and will apply when the 3D viewer starts.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    } else {
        Text(
            "Effective tier ${stats.qualityLevel} · ${stats.drawCalls} draw calls · ${stats.triangles} visible triangles · renderer call %.1f ms".format(stats.cpuMs),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    info?.let {
        Text(
            "%s · %s · %s · %s".format(it.backend, it.deviceName, it.apiVersion, it.driver),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "shadow map %d · MSAA %dx · bloom %d levels".format(
                stats.shadowMapSize, stats.msaaSamples, stats.bloomLevels,
            ),
            style = MaterialTheme.typography.bodySmall,
        )
        Text(
            "Avatar texture atlas estimate: %.1f MiB (render targets and driver overhead excluded)".format(stats.textureMiB),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    val thermalLabel = when (thermalStatus) {
        0 -> "none"
        1 -> "light"
        2 -> "moderate"
        3 -> "severe"
        4 -> "critical"
        5 -> "emergency"
        6 -> "shutdown"
        null -> "unavailable on this Android version/device"
        else -> "unknown ($thermalStatus)"
    }
    Text("Android thermal status: $thermalLabel", style = MaterialTheme.typography.bodySmall)
    ToggleRow("Shadows", settings.shadows) { onSettings(settings.copy(shadows = it)) }
    ToggleRow("Bloom", settings.bloom) { onSettings(settings.copy(bloom = it)) }
    ToggleRow("Sub-surface scattering", settings.sss) { onSettings(settings.copy(sss = it)) }
    ToggleRow("Ground plane", settings.groundPlane) { onSettings(settings.copy(groundPlane = it)) }
    ToggleRow("Prefer Vulkan (else GLES 3.1)", settings.preferVulkan) { onSettings(settings.copy(preferVulkan = it)) }
    Text("Exposure %.2f".format(settings.exposure), style = MaterialTheme.typography.bodySmall)
    Slider(value = settings.exposure, onValueChange = { onSettings(settings.copy(exposure = it)) }, valueRange = 0.3f..2.5f)
    Text("Sun azimuth %.0f°".format(settings.sunAzimuthDeg), style = MaterialTheme.typography.bodySmall)
    Slider(value = settings.sunAzimuthDeg, onValueChange = { onSettings(settings.copy(sunAzimuthDeg = it)) }, valueRange = 0f..360f)
    Text("Sun elevation %.0f°".format(settings.sunElevationDeg), style = MaterialTheme.typography.bodySmall)
    Slider(value = settings.sunElevationDeg, onValueChange = { onSettings(settings.copy(sunElevationDeg = it)) }, valueRange = 2f..85f)
    Text("Sun intensity %.2f".format(settings.sunIntensity), style = MaterialTheme.typography.bodySmall)
    Text("Controls direct-light brightness. Higher values brighten lit surfaces and can wash out highlights.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    Slider(value = settings.sunIntensity, onValueChange = { onSettings(settings.copy(sunIntensity = it)) }, valueRange = 0.5f..8f)
    Text("Shadow softness %.1f".format(settings.shadowSoftness), style = MaterialTheme.typography.bodySmall)
    Text("Controls shadow-edge blur, not the sun brightness. Higher values soften edges; shadows must be enabled.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    Slider(value = settings.shadowSoftness, onValueChange = { onSettings(settings.copy(shadowSoftness = it)) }, valueRange = 0.5f..3f)
}

private data class AdaptiveSample(val fps: Float, val frameMs: Float, val cpuMs: Float, val tier: Int, val thermal: Int?, val availableRamBytes: Long, val availableStorageBytes: Long, val resourceState: String)

/** Live diagnostics plus a repeatable observation window for the native AUTO quality controller. */
@Composable
fun RenderSettingsScreen(
    settings: RenderSettings,
    onSettings: (RenderSettings) -> Unit,
    stats: RendererStats,
    rendererInfo: RendererInfo?,
    thermalStatus: Int?,
    onBack: () -> Unit,
) {
    Column(
        Modifier.fillMaxSize().background(MaterialTheme.colorScheme.surface)
            .statusBarsPadding().navigationBarsPadding().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        TextButton(onClick = onBack) { Text("← Back") }
        RenderPanel(settings, onSettings, stats, rendererInfo, thermalStatus)
    }
}
@Composable
fun SystemCheckScreen(
    result: EngineResult?,
    rendererError: String?,
    rendererInfo: RendererInfo?,
    stats: RendererStats,
    thermalStatus: Int?,
    settings: RenderSettings,
    onBack: () -> Unit,
) {
    Column(
        Modifier.fillMaxSize().background(MaterialTheme.colorScheme.surface)
            .statusBarsPadding().navigationBarsPadding().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        TextButton(onClick = onBack) { Text("← Back") }
        SystemCheckPanel(result, rendererError, rendererInfo, stats, thermalStatus, settings)
    }
}

@Composable
private fun SystemCheckPanel(
    result: EngineResult?,
    rendererError: String?,
    info: RendererInfo?,
    stats: RendererStats,
    thermalStatus: Int?,
    settings: RenderSettings,
) {
    val context = LocalContext.current
    val governor = remember(context) { DeviceResourceGovernor(context) }
    var resources by remember { mutableStateOf(governor.snapshot()) }
    val samples = remember { mutableStateListOf<AdaptiveSample>() }
    var running by remember { mutableStateOf(false) }
    val latestStats = rememberUpdatedState(stats)
    val latestThermal = rememberUpdatedState(thermalStatus)
    LaunchedEffect(running) {
        if (running) {
            samples.clear()
            repeat(15) {
                delay(1_000L)
                val s = latestStats.value
                runCatching { governor.snapshot() }.getOrNull()?.let { resources = it }
                val snapshot = resources
                samples.add(AdaptiveSample(s.fps, s.frameMs, s.cpuMs, s.qualityLevel, latestThermal.value ?: snapshot.thermalStatus, snapshot.availableRamBytes, snapshot.availableStorageBytes, snapshot.state.name))
            }
            running = false
        } else {
            while (true) {
                delay(1_000L)
                runCatching { governor.snapshot() }.getOrNull()?.let { resources = it }
            }
        }
    }
    Text("System check + adaptive proof", style = MaterialTheme.typography.titleSmall)
    Text("Live measurements from this renderer session. A green check means the value was observed, not that an unrun test passed.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    val engineHealth = remember { AvatarEngine.health() }
    DiagnosticLine("Native engine", if (engineHealth.usable) "available" else "unavailable: ${engineHealth.problem ?: "unknown reason"}")
    if (result == null) {
        DiagnosticLine("Generated avatar", "no completed avatar in this session")
        DiagnosticLine("GLB export", "not applicable until generation completes")
    } else {
        DiagnosticLine("Generated avatar", "${result.spec.heightM} m requested stature · ${result.blendshapeCount} blendshape channels")
        DiagnosticLine("GLB export", if (result.glbFile.isFile && result.glbFile.length() > 0L) "present · %.1f MiB".format(result.glbFile.length() / 1048576.0) else "MISSING OR EMPTY · export file check failed")
    }
    DiagnosticLine("GPU backend", info?.let { "${it.backend} · ${it.deviceName}" } ?: "not initialised")
    DiagnosticLine("Graphics API / driver", info?.let { "${it.apiVersion} · ${it.driver}" } ?: "not available")
    DiagnosticLine("GPU capabilities", info?.let { "shadows ${if (it.supportsShadows) "yes" else "no"} · MSAA ${if (it.supportsMsaa) "yes" else "no"} · HDR ${if (it.supportsFloatHdr) "yes" else "no"} · anisotropy ${if (it.supportsAnisotropy) "yes" else "no"}" } ?: "not available")
    DiagnosticLine("Available system RAM", "%.0f MiB available · %.1f GiB total".format(resources.availableRamBytes / 1048576.0, resources.totalRamBytes / 1073741824.0))
    DiagnosticLine("Free app storage", "%.0f MiB".format(resources.availableStorageBytes / 1048576.0))
    DiagnosticLine("Resource governor", "${resources.state.name} · ${resources.cpuCores} logical CPU cores · ${if (resources.lowMemory) "Android low-memory flag set" else "no low-memory flag"}")
    DiagnosticLine("Render surface", when {
        rendererError != null -> rendererError
        info != null -> "render loop active; no current reported error"
        result == null -> "not active in this screen; open the 3D viewer to test GPU rendering"
        else -> "waiting for renderer initialisation"
    })
    DiagnosticLine("Current FPS / frame interval", "%.1f FPS · %.2f ms".format(stats.fps, stats.frameMs))
    DiagnosticLine("Renderer-call CPU wall time", "%.2f ms (not a GPU timer)".format(stats.cpuMs))
    DiagnosticLine("Mesh submitted", "${stats.triangles} visible triangles · ${stats.vertices} vertices · ${stats.drawCalls} draw calls")
    DiagnosticLine("Texture memory", "%.1f MiB avatar-atlas estimate only; total GPU memory is not exposed".format(stats.textureMiB))
    val currentThermal = thermalStatus ?: resources.thermalStatus
    DiagnosticLine("Thermal status", when (currentThermal) { 0 -> "none"; 1 -> "light"; 2 -> "moderate"; 3 -> "severe"; 4 -> "critical"; 5 -> "emergency"; 6 -> "shutdown"; null -> "unavailable"; else -> "unknown ($currentThermal)" })
    DiagnosticLine("Requested quality", settings.quality.name)
    DiagnosticLine("Effective quality tier", "${stats.qualityLevel} · MSAA ${stats.msaaSamples}x · shadow map ${stats.shadowMapSize} · bloom ${stats.bloomLevels}")
    rendererError?.let { Text("Renderer error: $it", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
    Text("Proof-of-adaptation loop", style = MaterialTheme.typography.titleSmall)
    Text("Run a 15-second observation. AUTO should move down only after sustained slow frames and move up only after sustained headroom. This is deterministic feedback control, not machine-learning training; samples are session-only, while render settings persist separately.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        AssistChip(onClick = { if (!running) running = true }, label = { Text(if (running) "Sampling… ${samples.size}/15" else "Run 15-second check") }, enabled = !running)
        AssistChip(onClick = { samples.clear() }, label = { Text("Clear evidence") }, enabled = !running && samples.isNotEmpty())
    }
    if (samples.isNotEmpty()) {
        val first = samples.first()
        val last = samples.last()
        val tierChanges = samples.zipWithNext().count { (a, b) -> a.tier != b.tier }
        val averageFps = samples.map { it.fps }.average()
        DiagnosticLine("Samples", "${samples.size}/15 · average %.1f FPS".format(averageFps))
        DiagnosticLine("Start → end", "%.1f → %.1f FPS · tier %d → %d".format(first.fps, last.fps, first.tier, last.tier))
        DiagnosticLine("Observed tier transitions", tierChanges.toString())
        DiagnosticLine("RAM during sample window", "%.0f → %.0f MiB available".format(first.availableRamBytes / 1048576.0, last.availableRamBytes / 1048576.0))
        DiagnosticLine("Storage during sample window", "%.0f → %.0f MiB free".format(first.availableStorageBytes / 1048576.0, last.availableStorageBytes / 1048576.0))
        DiagnosticLine("Resource state start → end", "${first.resourceState} → ${last.resourceState}")
        val hasValidRenderTelemetry = samples.any { sample ->
            sample.fps.isFinite() && sample.fps > 0f &&
                sample.frameMs.isFinite() && sample.frameMs > 0f &&
                sample.cpuMs.isFinite() && sample.cpuMs > 0f
        }
        DiagnosticLine("Evidence verdict", when {
            samples.size < 15 -> "INCOMPLETE · finish the 15-second window"
            info == null -> "SYSTEM ONLY · no active renderer; adaptive behaviour cannot be proven here"
            !hasValidRenderTelemetry -> "INCONCLUSIVE · invalid render telemetry (zero/non-finite FPS, frame interval or renderer-call time); fix frame loop/counters before judging adaptation"
            settings.quality != Quality.AUTO -> "RECORDED ONLY · telemetry observed; adaptive tier changes require AUTO mode"
            tierChanges > 0 -> "ADAPTATION OBSERVED · real effective tier changed during sampling"
            else -> "RECORDED ONLY · valid telemetry but no tier change observed; adaptation was not demonstrated"
        })
        Text("Last sample: ${last.fps} FPS · ${last.frameMs} ms interval · ${last.cpuMs} ms renderer call · thermal ${last.thermal?.toString() ?: "unavailable"}.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    } else {
        Text("No evidence captured this session yet.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun DiagnosticLine(label: String, value: String) {
    Column(verticalArrangement = Arrangement.spacedBy(1.dp)) {
        Text(label, style = MaterialTheme.typography.labelMedium, fontWeight = FontWeight.SemiBold)
        Text(value, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
        Switch(checked = checked, onCheckedChange = onChange)
    }
}
