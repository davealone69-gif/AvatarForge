/*
 * AvatarForge :: ProgressScreen.kt
 *
 * The generation screen. It shows the kernel's own stage progress (not a fake spinner): the job's
 * StateFlow carries the stage, the fraction within it, a weighted overall fraction and the message
 * the kernel emitted, and every one of those numbers comes from the engine's progress callback.
 *
 * The elapsed clock and the per-stage timings are the honest answer to "is it stuck?" - a mid-range
 * phone spends about two thirds of the wall clock in the geometry stage, and the weighted bar is
 * built on that measurement so it does not lie in either direction.
 */
package com.avatarforge.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.avatarforge.sdk.JobStatus
import com.avatarforge.sdk.Stage

@Composable
fun ProgressScreen(
    status: JobStatus,
    onCancel: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        Text("Building your avatar", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
        Text(
            "Everything below happens on this device: no server, no upload, no network. You can turn the radio off and it will still finish.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(status.stage.label, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                    Text("%.0f%%".format(status.overallProgress * 100f), style = MaterialTheme.typography.titleMedium)
                }
                LinearProgressIndicator(
                    progress = { status.stageProgress.coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth(),
                )
                LinearProgressIndicator(
                    progress = { status.overallProgress.coerceIn(0f, 1f) },
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(4.dp),
                )
                if (status.message.isNotBlank()) {
                    Text(status.message, style = MaterialTheme.typography.bodySmall)
                }
                Text("Elapsed %.1f s".format(status.elapsedMs / 1000f), style = MaterialTheme.typography.bodySmall)
            }
        }

        StageList(status.stage)

        val failure = status.error
        if (failure != null) {
            Card {
                Column(Modifier.padding(16.dp)) {
                    Text("Generation failed", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.error)
                    Text(failure, style = MaterialTheme.typography.bodySmall)
                }
            }
        }

        Spacer(Modifier.weight(1f))
        OutlinedButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) {
            Text(if (status.failed) "Back" else "Cancel")
        }
    }
}

/**
 * The stage list with the measured share of the wall clock next to each stage. These are the same
 * weights the progress bar uses; showing them makes the bar explicable instead of mysterious.
 */
@Composable
private fun StageList(current: Stage) {
    val weights = mapOf(
        Stage.ANALYZE_INPUTS to "2%",
        Stage.FIT_ANATOMY to "1%",
        Stage.GEOMETRY to "66%",
        Stage.UV_UNWRAP to "3%",
        Stage.BAKE_TEXTURES to "4%",
        Stage.RIG to "20%",
        Stage.BLENDSHAPES to "3%",
        Stage.ANIMATION to "0.5%",
        Stage.EXPORT to "0.5%",
    )
    LazyColumn(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        items(Stage.ALL) { stage ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    when {
                        stage.ordinal < current.ordinal -> "done"
                        stage == current -> "now"
                        else -> "waiting"
                    },
                    style = MaterialTheme.typography.labelSmall,
                    color = when {
                        stage == current -> MaterialTheme.colorScheme.primary
                        stage.ordinal < current.ordinal -> MaterialTheme.colorScheme.onSurfaceVariant
                        else -> MaterialTheme.colorScheme.outline
                    },
                    modifier = Modifier
                        .fillMaxWidth(0.22f),
                )
                Text(stage.label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                Text(weights[stage] ?: "", style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}
