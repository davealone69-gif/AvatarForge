package com.avatarforge.app.ui

import android.widget.Toast
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.avatarforge.sdk.render.DiagnosticRung
import com.avatarforge.sdk.render.RenderedPixelCheck

/** Offline self-diagnostics and deterministic recovery. Nothing is uploaded and no AI is used. */
@Composable
fun DiagnosticsDialog(
    report: String,
    recoveryStatus: String?,
    renderedPixelCheck: RenderedPixelCheck?,
    selectedDiagnosticRung: DiagnosticRung,
    onDiagnosticRungSelected: (DiagnosticRung) -> Unit,
    onCheckRenderedPixels: () -> Unit,
    onSafeRecovery: () -> Unit,
    onExportReport: () -> Unit,
    onShareReport: () -> Unit,
    onClearJournal: () -> Unit,
    onDismiss: () -> Unit,
) {
    val clipboard = LocalClipboardManager.current
    val context = LocalContext.current
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Self diagnostics & recovery") },
        text = {
            Column(
                Modifier
                    .fillMaxWidth()
                    .heightIn(max = 480.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                Text(
                    "Recovery: ${recoveryStatus ?: "idle"}",
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(bottom = 8.dp),
                )
                Text(
                    "Diagnostic ladder",
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.padding(bottom = 4.dp),
                )
                DiagnosticRung.entries.forEach { rung ->
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clickable { onDiagnosticRungSelected(rung) }
                            .padding(vertical = 2.dp),
                    ) {
                        RadioButton(
                            selected = rung == selectedDiagnosticRung,
                            onClick = { onDiagnosticRungSelected(rung) },
                        )
                        Column(Modifier.padding(start = 8.dp, top = 7.dp, bottom = 7.dp)) {
                            Text(rung.displayName, style = MaterialTheme.typography.bodyMedium)
                            Text(rung.passCriterion, style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
                Row(Modifier.horizontalScroll(rememberScrollState())) {
                    TextButton(onClick = onCheckRenderedPixels) { Text("Check visible pixels") }
                    TextButton(onClick = onSafeRecovery) { Text("Safe recovery") }
                    TextButton(onClick = onShareReport) { Text("Share report") }
                    TextButton(onClick = onExportReport) { Text("Export report") }
                    TextButton(onClick = {
                        onClearJournal()
                        Toast.makeText(context, "Recovery journal cleared", Toast.LENGTH_SHORT).show()
                    }) { Text("Clear journal") }
                }
                Text(
                    renderedPixelCheck?.summary ?: "Visible output check: not run",
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(vertical = 4.dp),
                )
                Text(
                    "The check renders one fresh diagnostic frame and reads its full GLES scene framebuffer on demand; Vulkan currently reports unsupported.",
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(bottom = 8.dp),
                )
                Text(
                    report,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.padding(bottom = 8.dp),
                )
            }
        },
        confirmButton = {
            TextButton(onClick = {
                clipboard.setText(AnnotatedString(report))
                Toast.makeText(context, "Diagnostics copied", Toast.LENGTH_SHORT).show()
            }) { Text("Copy") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Close") }
        },
    )
}
