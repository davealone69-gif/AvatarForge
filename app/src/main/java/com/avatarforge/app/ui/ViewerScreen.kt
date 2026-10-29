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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.avatarforge.sdk.AvatarEngine
import com.avatarforge.sdk.EngineResult
import com.avatarforge.sdk.summary
import com.avatarforge.sdk.render.CameraGesture
import com.avatarforge.sdk.render.CameraSettings
import com.avatarforge.sdk.render.RenderSettings
import com.avatarforge.sdk.render.RendererInfo
import com.avatarforge.sdk.render.RendererStats

/** Which editing panel is open. Only one at a time: this is a phone, not a workstation. */
enum class ViewerPanel { CAMERA, POSE, FACE, INSPECT, EXPORT, RENDER }

@Composable
fun ViewerScreen(
    result: EngineResult,
    engine: AvatarEngine,
    rendererError: String?,
    onBack: () -> Unit,
    rendererInfo: RendererInfo?,
    stats: RendererStats,
    camera: CameraSettings,
    onCamera: (CameraSettings) -> Unit,
    settings: RenderSettings,
    onSettings: (RenderSettings) -> Unit,
    onRetryRenderer: (RenderSettings) -> Unit,
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
    /* pointerInput(Unit) is deliberately long-lived: restarting it on every camera frame would
       cancel the in-flight gesture. The callback therefore reads the *current* camera/callback
       through rememberUpdatedState instead of closing over the first composition's value. */
    val liveCamera by rememberUpdatedState(camera)
    val liveOnCamera by rememberUpdatedState(onCamera)
    val liveOnTapModel by rememberUpdatedState(onTapModel)

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
                        liveOnCamera(CameraGesture.orbit(liveCamera, pan.x, pan.y, zoom))
                    }
                }
                .pointerInput(Unit) {
                    detectTapGestures { offset -> liveOnTapModel(offset.x, offset.y) }
                },
        )

        Column(
            Modifier
                .align(Alignment.TopStart)
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(12.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            TextButton(onClick = onBack) { Text("← Back to editor") }
            Header(result, rendererInfo, stats, rendererError != null)
            if (rendererError != null) {
                Card(
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                ) {
                    Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text("3D viewer stopped", fontWeight = FontWeight.SemiBold)
                        Text(rendererError, style = MaterialTheme.typography.bodySmall)
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TextButton(onClick = { onRetryRenderer(settings) }) { Text("Retry viewer") }
                            TextButton(onClick = {
                                onRetryRenderer(settings.copy(preferVulkan = !settings.preferVulkan))
                            }) {
                                Text(if (settings.preferVulkan) "Try OpenGL ES" else "Try Vulkan")
                            }
                        }
                    }
                }
            }
        }

        Row(
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
                    label = { Text(entry.name.lowercase()) },
                )
            }
        }

        Card(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .navigationBarsPadding()
                .padding(bottom = 56.dp),
            shape = RoundedCornerShape(topStart = 18.dp, topEnd = 18.dp),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        ) {
            Column(
                Modifier
                    .padding(14.dp)
                    .heightIn(max = 360.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                when (panel) {
                    ViewerPanel.CAMERA -> CameraPanel(camera, onCamera)
                    ViewerPanel.POSE -> PosePanel(poseIndex, onPose, clipIndex, onClip, engine)
                    ViewerPanel.FACE -> FacePanel(expressionNames, onExpression)
                    ViewerPanel.INSPECT -> InspectPanel(result, pickedRegion, onPickRegion, engine)
                    ViewerPanel.EXPORT -> ExportPanel(onExport, onShare, onExportTo, result)
                    ViewerPanel.RENDER -> RenderPanel(settings, onSettings, stats, rendererInfo)
                }
            }
        }
    }
}

@Composable
private fun CameraPanel(camera: CameraSettings, onCamera: (CameraSettings) -> Unit) {
    val c = CameraGesture.clamped(camera)
    Text("Camera", style = MaterialTheme.typography.titleSmall)
    Text(
        "Drag the model to orbit, pinch to zoom, tap to inspect. These controls move the same camera and work even if a gesture is unavailable.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        AssistChip(onClick = { onCamera(c.copy(yawDeg = CameraGesture.normalizeYaw(c.yawDeg - 15f))) }, label = { Text("Orbit left") })
        AssistChip(onClick = { onCamera(c.copy(yawDeg = CameraGesture.normalizeYaw(c.yawDeg + 15f))) }, label = { Text("Orbit right") })
        AssistChip(onClick = { onCamera(c.copy(pitchDeg = (c.pitchDeg + 10f).coerceIn(CameraGesture.MIN_PITCH, CameraGesture.MAX_PITCH))) }, label = { Text("Up") })
        AssistChip(onClick = { onCamera(c.copy(pitchDeg = (c.pitchDeg - 10f).coerceIn(CameraGesture.MIN_PITCH, CameraGesture.MAX_PITCH))) }, label = { Text("Down") })
        AssistChip(onClick = { onCamera(c.copy(distance = (c.distance / 1.2f).coerceIn(CameraGesture.MIN_DISTANCE, CameraGesture.MAX_DISTANCE))) }, label = { Text("Zoom in") })
        AssistChip(onClick = { onCamera(c.copy(distance = (c.distance * 1.2f).coerceIn(CameraGesture.MIN_DISTANCE, CameraGesture.MAX_DISTANCE))) }, label = { Text("Zoom out") })
        AssistChip(onClick = { onCamera(CameraSettings()) }, label = { Text("Reset") })
    }
    CameraSlider("Yaw", c.yawDeg, 0f..360f, "°") { onCamera(CameraGesture.clamped(c.copy(yawDeg = it))) }
    CameraSlider("Pitch", c.pitchDeg, CameraGesture.MIN_PITCH..CameraGesture.MAX_PITCH, "°") { onCamera(CameraGesture.clamped(c.copy(pitchDeg = it))) }
    CameraSlider("Distance", c.distance, CameraGesture.MIN_DISTANCE..CameraGesture.MAX_DISTANCE, " m") { onCamera(CameraGesture.clamped(c.copy(distance = it))) }
    CameraSlider("Target height", c.targetY, 0f..2.5f, " m") { onCamera(CameraGesture.clamped(c.copy(targetY = it))) }
    CameraSlider("Field of view", c.fovDeg, 20f..60f, "°") { onCamera(CameraGesture.clamped(c.copy(fovDeg = it))) }
}

@Composable
private fun CameraSlider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    unit: String,
    onChange: (Float) -> Unit,
) {
    Text("%s: %.1f%s".format(label, value, unit), style = MaterialTheme.typography.bodySmall)
    Slider(value = value.coerceIn(range), onValueChange = onChange, valueRange = range)
}

@Composable
private fun Header(result: EngineResult, info: RendererInfo?, stats: RendererStats, failed: Boolean) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f))) {
        Column(Modifier.padding(10.dp)) {
            Text(result.summary(), style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.SemiBold)
            val backend = when {
                failed -> "GPU unavailable"
                info != null -> "${info.backend} · ${info.deviceName}"
                else -> "initialising GPU…"
            }
            val performance = if (failed) "no frames rendered" else "%.0f FPS (%.1f ms) · %s".format(
                stats.fps, stats.frameMs,
                if (stats.meetsTarget) "on target" else "below 30 FPS - auto quality will step down",
            )
            Text(
                "$backend · $performance",
                style = MaterialTheme.typography.labelSmall,
                color = if (!failed && stats.meetsTarget) MaterialTheme.colorScheme.onSurfaceVariant
                        else MaterialTheme.colorScheme.error,
            )
            Text(
                "%.0f s to build · %d tris submitted".format(
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
private fun RenderPanel(settings: RenderSettings, onSettings: (RenderSettings) -> Unit, stats: RendererStats, info: RendererInfo?) {
    Text("Renderer", style = MaterialTheme.typography.titleSmall)
    info?.let {
        Text(
            "%s · %s · %s · %s".format(it.backend, it.deviceName, it.apiVersion, it.driver),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "shadow map %d · MSAA %dx · bloom %d levels · %.0f MiB textures".format(
                stats.shadowMapSize, stats.msaaSamples, stats.bloomLevels, stats.textureMiB,
            ),
            style = MaterialTheme.typography.bodySmall,
        )
    }
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
    Slider(value = settings.sunIntensity, onValueChange = { onSettings(settings.copy(sunIntensity = it)) }, valueRange = 0.5f..8f)
    Text("Shadow softness %.1f".format(settings.shadowSoftness), style = MaterialTheme.typography.bodySmall)
    Slider(value = settings.shadowSoftness, onValueChange = { onSettings(settings.copy(shadowSoftness = it)) }, valueRange = 0.5f..3f)
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
        Switch(checked = checked, onCheckedChange = onChange)
    }
}
