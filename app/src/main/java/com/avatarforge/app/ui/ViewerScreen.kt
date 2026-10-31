/*
 * AvatarForge :: ViewerScreen.kt
 *
 * The viewer owns the Android SurfaceView and the UI-facing lifecycle of the real GLB viewer
 * controller. The model is revealed only after that controller reports Ready; initialization,
 * loading and failure are explicit states, never a simulated render result.
 */
package com.avatarforge.app.ui

import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.Choreographer
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.runtime.key
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.avatarforge.sdk.EngineResult
import com.avatarforge.sdk.render.CameraGesture
import com.avatarforge.sdk.render.CameraSettings
import com.avatarforge.sdk.render.FilamentGlbViewerController
import com.avatarforge.sdk.render.FilamentViewerState
import com.avatarforge.sdk.summary
import java.util.concurrent.atomic.AtomicBoolean

/** The UI panels supported by the GLB viewer contract. */
enum class ViewerPanel { CAMERA, ANIMATION, EXPORT }

@Composable
fun ViewerScreen(
    result: EngineResult,
    camera: CameraSettings,
    onCamera: (CameraSettings) -> Unit,
    onBack: () -> Unit,
    onExport: () -> Unit,
    onShare: () -> Unit,
    onExportTo: () -> Unit,
    panel: ViewerPanel,
    onPanel: (ViewerPanel) -> Unit,
    onViewerState: (FilamentViewerState) -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current.applicationContext
    var retryCount by remember(result.glbFile) { mutableIntStateOf(0) }
    val controller = remember(result.glbFile, retryCount) {
        FilamentGlbViewerController(context)
    }
    val viewerState by controller.state.collectAsStateWithLifecycle()
    val lifecycleOwner = LocalLifecycleOwner.current
    val choreographer = remember { Choreographer.getInstance() }
    val frameLoopRunning = remember(controller) { AtomicBoolean(false) }
    val frameCallback = remember(controller) {
        object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                controller.renderFrame(frameTimeNanos)
                if (frameLoopRunning.get()) choreographer.postFrameCallback(this)
            }
        }
    }
    val ready = viewerState as? FilamentViewerState.Ready
    var cameraInput by remember(controller) { mutableStateOf(CameraGesture.clamped(camera)) }
    val updateCamera by rememberUpdatedState<(CameraSettings) -> Unit> { requested ->
        val next = CameraGesture.clamped(requested)
        cameraInput = next
        onCamera(next)
        controller.setCamera(next)
    }

    LaunchedEffect(controller, result.glbFile) {
        controller.loadGlb(result.glbFile)
    }
    LaunchedEffect(controller, camera) {
        cameraInput = CameraGesture.clamped(camera)
        controller.setCamera(cameraInput)
    }
    LaunchedEffect(viewerState) {
        onViewerState(viewerState)
    }
    DisposableEffect(controller, lifecycleOwner, frameCallback) {
        fun startFrames() {
            if (frameLoopRunning.compareAndSet(false, true)) {
                choreographer.postFrameCallback(frameCallback)
            }
        }
        fun stopFrames() {
            if (frameLoopRunning.getAndSet(false)) {
                choreographer.removeFrameCallback(frameCallback)
            }
        }
        val lifecycleObserver = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> startFrames()
                Lifecycle.Event.ON_PAUSE, Lifecycle.Event.ON_STOP, Lifecycle.Event.ON_DESTROY -> stopFrames()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(lifecycleObserver)
        if (lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) startFrames()
        onDispose {
            stopFrames()
            lifecycleOwner.lifecycle.removeObserver(lifecycleObserver)
            controller.close()
        }
    }

    val surfaceCallback = remember(controller) {
        object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                val frame = holder.surfaceFrame
                val width = frame.width().takeIf { it > 0 } ?: 1
                val height = frame.height().takeIf { it > 0 } ?: 1
                controller.onSurfaceCreated(holder.surface, width, height)
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                controller.onSurfaceChanged(holder.surface, width, height)
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                controller.onSurfaceDestroyed()
            }
        }
    }

    Box(modifier.fillMaxSize().background(MaterialTheme.colorScheme.surface)) {
        key(controller) {
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { viewContext ->
                    SurfaceView(viewContext).also { view ->
                        view.holder.addCallback(surfaceCallback)
                    }
                },
                onRelease = { view ->
                    view.holder.removeCallback(surfaceCallback)
                },
            )
        }

        if (ready != null) {
            Box(
                Modifier
                        .fillMaxSize()
                        .pointerInput(controller) {
                            detectTransformGestures { _, pan, zoom, _ ->
                            updateCamera(CameraGesture.orbit(cameraInput, pan.x, pan.y, zoom))
                        }
                    },
            )
        }

        Column(
            Modifier
                .align(Alignment.TopStart)
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                TextButton(onClick = onBack) { Text("← Back to editor") }
                Text(
                    "%.2f m · %,d triangles".format(result.spec.heightM, result.triangleCount),
                    modifier = Modifier.padding(start = 8.dp),
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.SemiBold,
                )
            }
            if (ready != null) {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.86f),
                    ),
                ) {
                    Text(
                        result.summary(),
                        Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }

        if (ready != null) {
            Column(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .navigationBarsPadding()
                    .padding(8.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    ViewerPanel.entries.forEach { entry ->
                        FilterChip(
                            selected = panel == entry,
                            onClick = { onPanel(entry) },
                            label = { Text(entry.name.lowercase().replaceFirstChar { it.uppercase() }) },
                        )
                    }
                }
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(18.dp),
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
                ) {
                    Column(
                        Modifier
                            .padding(14.dp)
                            .heightIn(max = 260.dp)
                            .verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        when (panel) {
                            ViewerPanel.CAMERA -> CameraPanel(cameraInput, updateCamera)
                            ViewerPanel.ANIMATION -> AnimationPanel(controller, ready)
                            ViewerPanel.EXPORT -> ExportPanel(result, onExport, onShare, onExportTo)
                        }
                    }
                }
            }
        }

        if (ready == null) {
            ViewerLoadingOverlay(
                state = viewerState,
                onBack = onBack,
                onRetry = { retryCount += 1 },
            )
        }
    }
}

@Composable
private fun ViewerLoadingOverlay(
    state: FilamentViewerState,
    onBack: () -> Unit,
    onRetry: () -> Unit,
) {
    val failure = state as? FilamentViewerState.Failed
    val closed = state == FilamentViewerState.Closed
    Box(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.97f)),
        contentAlignment = Alignment.Center,
    ) {
        Card(
            modifier = Modifier
                .fillMaxWidth()
                .padding(24.dp),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        ) {
            Column(
                Modifier.padding(20.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                if (failure == null) {
                    if (!closed) CircularProgressIndicator()
                    Text(
                        if (closed) "Viewer closed" else "Opening generated GLB",
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(
                        when (state) {
                            FilamentViewerState.WaitingForSurface -> "Waiting for the viewer surface…"
                            FilamentViewerState.Initializing -> "Starting the 3D viewer…"
                            FilamentViewerState.WaitingForAsset -> "Waiting for the generated GLB…"
                            FilamentViewerState.LoadingAsset -> "Loading the generated avatar…"
                            FilamentViewerState.Closed -> "Viewer closed. Return to the editor to continue."
                            is FilamentViewerState.Ready -> "Avatar ready"
                            is FilamentViewerState.Failed -> "Viewer failed"
                        },
                        style = MaterialTheme.typography.bodyMedium,
                    )
                } else {
                    Text("Couldn’t open the 3D viewer", style = MaterialTheme.typography.titleMedium)
                    Text("Stage: ${failure.stage}", style = MaterialTheme.typography.labelMedium)
                    Text(failure.message, style = MaterialTheme.typography.bodyMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = onRetry) { Text("Retry viewer") }
                        TextButton(onClick = onBack) { Text("Back to editor") }
                    }
                }
                if (failure == null) {
                    TextButton(onClick = onBack) { Text("Back to editor") }
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
        "Drag to rotate and pinch to zoom. These controls move the same live camera.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        AssistChip(
            onClick = { onCamera(CameraGesture.clamped(c.copy(yawDeg = CameraGesture.normalizeYaw(c.yawDeg - 15f)))) },
            label = { Text("Rotate left") },
        )
        AssistChip(
            onClick = { onCamera(CameraGesture.clamped(c.copy(yawDeg = CameraGesture.normalizeYaw(c.yawDeg + 15f)))) },
            label = { Text("Rotate right") },
        )
        AssistChip(
            onClick = { onCamera(CameraGesture.orbit(c, 0f, 0f, zoom = 1.2f)) },
            label = { Text("Zoom in") },
        )
        AssistChip(
            onClick = { onCamera(CameraGesture.orbit(c, 0f, 0f, zoom = 1f / 1.2f)) },
            label = { Text("Zoom out") },
        )
        AssistChip(onClick = { onCamera(CameraSettings()) }, label = { Text("Reset") })
    }
    Text("Distance: %.2f m".format(c.distance), style = MaterialTheme.typography.bodySmall)
    Slider(
        value = c.distance,
        onValueChange = { onCamera(CameraGesture.clamped(c.copy(distance = it))) },
        valueRange = CameraGesture.MIN_DISTANCE..CameraGesture.MAX_DISTANCE,
    )
}

@Composable
private fun AnimationPanel(controller: FilamentGlbViewerController, ready: FilamentViewerState.Ready) {
    var playing by remember(controller) { mutableStateOf(false) }
    var started by remember(controller) { mutableStateOf(false) }
    var selectedIndex by remember(controller) { mutableIntStateOf(0) }
    Text("GLB animations", style = MaterialTheme.typography.titleSmall)
    if (ready.animationCount <= 0) {
        Text("This GLB has no animations.", style = MaterialTheme.typography.bodySmall)
        return
    }
    Text("${ready.animationCount} animation${if (ready.animationCount == 1) "" else "s"} available", style = MaterialTheme.typography.bodySmall)
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        (0 until ready.animationCount).forEach { index ->
            val label = ready.animationNames.getOrNull(index)?.takeIf { it.isNotBlank() } ?: "Animation ${index + 1}"
            FilterChip(
                selected = selectedIndex == index,
                onClick = {
                    selectedIndex = index
                    controller.playAnimation(index)
                    controller.setAnimationPlaying(true)
                    playing = true
                    started = true
                },
                label = { Text(label) },
            )
        }
    }
    TextButton(onClick = {
        if (playing) {
            playing = false
        } else {
            if (!started) {
                controller.playAnimation(selectedIndex)
                started = true
            }
            playing = true
        }
        controller.setAnimationPlaying(playing)
    }) {
        Text(if (playing) "Pause animation" else if (started) "Resume animation" else "Play animation")
    }
}

@Composable
private fun ExportPanel(
    result: EngineResult,
    onExport: () -> Unit,
    onShare: () -> Unit,
    onExportTo: () -> Unit,
) {
    Text("Generated GLB", style = MaterialTheme.typography.titleSmall)
    Text(
        "%.1f MB · %,d triangles · %d joints · %d morph targets"
            .format(result.glbBytes / 1048576.0, result.triangleCount, result.jointCount, result.blendshapeCount),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Text(result.glbFile.absolutePath, style = MaterialTheme.typography.labelSmall)
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        AssistChip(onClick = onExport, label = { Text("Show stored file") })
        AssistChip(onClick = onExportTo, label = { Text("Save to…") })
        AssistChip(onClick = onShare, label = { Text("Share") })
    }
}
