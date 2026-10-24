/*
 * AvatarForge :: DiagnosticsPanel.kt
 *
 * The in-app fault finder. One composable renders the structured DiagnosticsReport both inside
 * the viewer's diagnostics panel and inside the input-screen dialog, so the two can never drift.
 * Every row is a measured fact with a traffic-light status; every WARN/FAIL carries a hint the
 * user can act on, and "Copy report" puts the plain-text version on the clipboard for a bug
 * report - no network, no extra permission.
 */
package com.avatarforge.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clip
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.avatarforge.sdk.DiagItem
import com.avatarforge.sdk.DiagStatus
import com.avatarforge.sdk.DiagnosticsReport
import com.avatarforge.sdk.render.RendererStartupState

private val statusOk = Color(0xFF2E7D32)
private val statusWarn = Color(0xFFF9A825)
private val statusFail = Color(0xFFC62828)
private val statusInfo = Color(0xFF607D8B)

private fun DiagStatus.color(): Color = when (this) {
    DiagStatus.OK -> statusOk
    DiagStatus.WARN -> statusWarn
    DiagStatus.FAIL -> statusFail
    DiagStatus.INFO -> statusInfo
}

private fun DiagStatus.label(): String = when (this) {
    DiagStatus.OK -> "OK"
    DiagStatus.WARN -> "WARN"
    DiagStatus.FAIL -> "FAIL"
    DiagStatus.INFO -> "INFO"
}

@Composable
fun DiagnosticsContent(
    report: DiagnosticsReport,
    onRetryRenderer: (() -> Unit)?,
    onCopy: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier
            .fillMaxWidth()
            .heightIn(max = 460.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        OverallVerdict(report)

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onCopy) { Text("Copy report") }
            val rendererState = report.rendererDiagnostics?.state
            if (onRetryRenderer != null &&
                rendererState != null && rendererState != RendererStartupState.RUNNING) {
                Button(onClick = onRetryRenderer) { Text("Retry renderer") }
            }
        }

        val areas = report.items.map { it.area }.distinct()
        for (area in areas) {
            Text(
                area,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.SemiBold,
            )
            for (item in report.items.filter { it.area == area }) {
                DiagnosticRow(item)
            }
        }

        val d = report.rendererDiagnostics
        if (d != null && d.attempts.isNotEmpty()) {
            Text(
                "Renderer start-up attempts",
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.SemiBold,
            )
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
                Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(
                        "Final state: ${d.state.name} · ${d.framesRendered} frames presented",
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                    for (a in d.attempts) {
                        val backend = if (a.preferVulkan) "Vulkan preferred" else "GLES forced"
                        val outcome = if (a.succeeded) "OK" else "FAIL"
                        Text(
                            "Attempt ${a.attempt} ($backend): $outcome in ${a.elapsedMs} ms",
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = if (a.succeeded) statusOk else statusFail,
                        )
                        a.error?.let {
                            Text(
                                it,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun OverallVerdict(report: DiagnosticsReport) {
    val worst = report.worstStatus
    val container = when (worst) {
        DiagStatus.FAIL -> MaterialTheme.colorScheme.errorContainer
        DiagStatus.WARN -> MaterialTheme.colorScheme.tertiaryContainer
        else -> MaterialTheme.colorScheme.secondaryContainer
    }
    Card(colors = CardDefaults.cardColors(containerColor = container)) {
        Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                "Diagnostics: ${worst.label()}",
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Bold,
            )
            val summary = when (worst) {
                DiagStatus.FAIL -> "At least one check failed. Expand the FAIL rows below and follow the hint."
                DiagStatus.WARN -> "Everything runs, but something is constrained. The WARN rows explain what."
                else -> "All measured checks pass on this device."
            }
            Text(summary, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun DiagnosticRow(item: DiagItem) {
    Row(verticalAlignment = Alignment.Top, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        androidx.compose.foundation.layout.Box(
            Modifier
                .padding(top = 5.dp)
                .size(9.dp)
                .clip(CircleShape)
                .background(item.status.color()),
        )
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(1.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(item.label, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.SemiBold)
                Text(
                    item.status.label(),
                    style = MaterialTheme.typography.labelSmall,
                    color = item.status.color(),
                    fontWeight = FontWeight.Bold,
                )
            }
            Text(item.detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            item.hint?.let {
                Text("Hint: $it", style = MaterialTheme.typography.bodySmall, color = statusWarn)
            }
        }
    }
}

/** Full-screen-dialog wrapper used from the input screen, where no viewer panel exists yet. */
@Composable
fun DiagnosticsDialog(
    report: DiagnosticsReport,
    onDismiss: () -> Unit,
    onCopy: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Diagnostics") },
        text = {
            DiagnosticsContent(
                report = report,
                onRetryRenderer = null,
                onCopy = onCopy,
                modifier = Modifier.fillMaxWidth(),
            )
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } },
    )
}
