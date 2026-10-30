/*
 * AvatarForge :: InputScreen.kt
 *
 * Where a generation starts. Three inputs, matching exactly what the engine can consume:
 *
 *   1. a text prompt              -> af_engine_analyze_prompt()  (keyword grammar, measured)
 *   2. Image A: appearance hint   -> af_engine_analyze_images(role = IDENTITY, ABI-stable name)
 *   3. Image B: outfit hint       -> af_engine_analyze_images(role = CLOTHING)
 *
 * The spec panel below the inputs is not decoration: the current deterministic image analyser is
 * deliberately coarse and reports framing, optional head-count stature, median colours and garment
 * metalness for correction. It does not reconstruct a face, preserve identity, copy a garment, or
 * project source pixels into textures, and the UI must never imply that it does.
 */
package com.avatarforge.app.ui

import android.graphics.Bitmap
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.avatarforge.app.ImageAnalysisRequestGuard
import com.avatarforge.app.withReferenceImageIo
import com.avatarforge.sdk.AvatarSpec
import com.avatarforge.sdk.Build
import com.avatarforge.sdk.Footwear
import com.avatarforge.sdk.HairStyle
import com.avatarforge.sdk.ImageRole
import com.avatarforge.sdk.Outfit
import com.avatarforge.sdk.Sex
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch

private val PROMPT_EXAMPLES = listOf(
    "athletic woman, ponytail, full plate armour",
    "tall male bodybuilder in a leather jacket and boots",
    "slim adult woman, t-shirt and sneakers, short hair",
    "spacesuit explorer, heavy build, buzz cut",
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun InputScreen(
    prompt: String,
    onPromptChange: (String) -> Unit,
    spec: AvatarSpec,
    onSpecChange: (AvatarSpec) -> Unit,
    imageA: Bitmap?,
    imageB: Bitmap?,
    onImageA: (Bitmap?) -> Unit,
    onImageB: (Bitmap?) -> Unit,
    analysisInProgress: Boolean,
    analysisNote: String?,
    onGeneratePrompt: () -> Unit,
    onGenerateImages: () -> Unit,
    enabled: Boolean,
    modifier: Modifier = Modifier,
) {
    val decodeScope = rememberCoroutineScope()
    val decodeGuardA = remember { ImageAnalysisRequestGuard() }
    val decodeGuardB = remember { ImageAnalysisRequestGuard() }
    var decodeJobA by remember { mutableStateOf<Job?>(null) }
    var decodeJobB by remember { mutableStateOf<Job?>(null) }
    var decodingA by remember { mutableStateOf(false) }
    var decodingB by remember { mutableStateOf(false) }
    var decodeNoteA by remember { mutableStateOf<String?>(null) }
    var decodeNoteB by remember { mutableStateOf<String?>(null) }

    fun clearA() {
        decodeGuardA.begin()
        decodeJobA?.cancel()
        decodeJobA = null
        decodingA = false
        decodeNoteA = null
        onImageA(null)
    }

    fun clearB() {
        decodeGuardB.begin()
        decodeJobB?.cancel()
        decodeJobB = null
        decodingB = false
        decodeNoteB = null
        onImageB(null)
    }

    fun decodeA(uri: Uri?) {
        if (uri == null) return
        val request = decodeGuardA.begin()
        decodeJobA?.cancel()
        decodingA = true
        decodeNoteA = null
        onImageA(null)
        decodeJobA = decodeScope.launch {
            val bitmap = try {
                withReferenceImageIo {
                    val decoded = AppBitmaps.decode(uri)
                    try {
                        currentCoroutineContext().ensureActive()
                        decoded
                    } catch (cancelled: CancellationException) {
                        decoded?.recycle()
                        throw cancelled
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                null
            }
            if (!decodeGuardA.isCurrent(request)) {
                bitmap?.recycle()
                return@launch
            }
            decodeJobA = null
            decodingA = false
            if (bitmap == null) decodeNoteA = "Image A could not be decoded. Choose another image or generate from the prompt."
            else onImageA(bitmap)
        }
    }

    fun decodeB(uri: Uri?) {
        if (uri == null) return
        val request = decodeGuardB.begin()
        decodeJobB?.cancel()
        decodingB = true
        decodeNoteB = null
        onImageB(null)
        decodeJobB = decodeScope.launch {
            val bitmap = try {
                withReferenceImageIo {
                    val decoded = AppBitmaps.decode(uri)
                    try {
                        currentCoroutineContext().ensureActive()
                        decoded
                    } catch (cancelled: CancellationException) {
                        decoded?.recycle()
                        throw cancelled
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                null
            }
            if (!decodeGuardB.isCurrent(request)) {
                bitmap?.recycle()
                return@launch
            }
            decodeJobB = null
            decodingB = false
            if (bitmap == null) decodeNoteB = "Image B could not be decoded. Choose another image or continue without it."
            else onImageB(bitmap)
        }
    }

    val pickA = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        decodeA(uri)
    }
    val pickB = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        decodeB(uri)
    }
    val canEdit = enabled && !analysisInProgress && !decodingA && !decodingB
    val visibleAnalysisNote = listOfNotNull(decodeNoteA, decodeNoteB, analysisNote)
        .joinToString("\n").takeIf { it.isNotBlank() }

    LazyColumn(
        modifier = modifier.fillMaxSize().safeDrawingPadding(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            Text("Grok Faces", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
            Text("Describe your avatar", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Text(
                "The prompt is parsed on-device into a specification: build, stature, hair, outfit, footwear and colours are all real fields you can correct below.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        item {
            OutlinedTextField(
                value = prompt,
                onValueChange = onPromptChange,
                modifier = Modifier.fillMaxWidth(),
                minLines = 2,
                label = { Text("Prompt") },
                placeholder = { Text(PROMPT_EXAMPLES.first()) },
                enabled = canEdit,
            )
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PROMPT_EXAMPLES.take(2).forEach { example ->
                    TextButton(onClick = { onPromptChange(example) }, enabled = canEdit) {
                        Text(example.split(",").first(), maxLines = 1)
                    }
                }
            }
        }

        item {
            Text("Reference images (optional)", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Text(
                "Image A samples coarse framing, optional full-body stature, and skin/hair colours. It does not copy or preserve the face. " +
                    "Image B supplies garment colour, metal confidence, and a generic outfit hint; it does not transfer garment geometry.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                ImageSlot(
                    label = "A · appearance hint",
                    role = "Portrait or full body",
                    bitmap = imageA,
                    onClick = { pickA.launch("image/*") },
                    onClear = ::clearA,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                )
                ImageSlot(
                    label = "B · outfit hint",
                    role = "Garment / material",
                    bitmap = imageB,
                    onClick = { pickB.launch("image/*") },
                    onClear = ::clearB,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                )
            }
        }
        if (decodingA || decodingB || analysisInProgress) {
            item {
                Text(
                    if (decodingA || decodingB) "Preparing reference image…" else "Analysing reference images…",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (visibleAnalysisNote != null) {
            item {
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
                    Text(
                        visibleAnalysisNote,
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }

        item { SpecEditor(spec, onSpecChange, canEdit) }

        item {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = onGeneratePrompt,
                    enabled = canEdit && prompt.isNotBlank(),
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Generate from prompt") }
                Button(
                    onClick = onGenerateImages,
                    enabled = canEdit && imageA != null,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(
                        when {
                            decodingA || decodingB -> "Preparing image…"
                            analysisInProgress -> "Analysing references…"
                            imageB != null -> "Generate using both hints"
                            else -> "Generate using image measurements"
                        },
                    )
                }
                if (imageB != null && imageA == null) {
                    Text(
                        "Image B needs Image A because the coarse analysis flow starts from the primary appearance/proportion reference.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        }
    }
}

@Composable
private fun ImageSlot(
    label: String,
    role: String,
    bitmap: Bitmap?,
    onClick: () -> Unit,
    onClear: () -> Unit,
    enabled: Boolean,
    modifier: Modifier = Modifier,
) {
    Column(modifier) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(0.75f)
                .clip(RoundedCornerShape(12.dp))
                .background(MaterialTheme.colorScheme.surfaceVariant)
                .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(12.dp))
                .clickable(enabled = enabled, onClick = onClick),
            contentAlignment = Alignment.Center,
        ) {
            if (bitmap != null) {
                Image(
                    bitmap = bitmap.asImageBitmap(),
                    contentDescription = label,
                    contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize(),
                )
            } else {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("+", style = MaterialTheme.typography.headlineMedium)
                    Text(role, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, style = MaterialTheme.typography.labelLarge, modifier = Modifier.weight(1f))
            if (bitmap != null) {
                TextButton(onClick = onClear, enabled = enabled) { Text("Clear") }
            }
        }
    }
}

/** The spec panel: every field the engine takes, with the defaults the prompt/images filled in. */
@Composable
private fun SpecEditor(spec: AvatarSpec, onChange: (AvatarSpec) -> Unit, enabled: Boolean) {
    var expanded by remember { mutableStateOf(false) }
    Card {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Specification", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                TextButton(onClick = { expanded = !expanded }) { Text(if (expanded) "Hide" else "Edit") }
            }
            Text(
                summaryLine(spec),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (expanded) {
                EnumRow("Sex", Sex.entries, spec.sex, enabled) { onChange(spec.copy(sex = it)) }
                EnumRow("Build", Build.entries, spec.build, enabled) { onChange(spec.copy(build = it)) }
                EnumRow("Hair", HairStyle.entries, spec.hairStyle, enabled) { onChange(spec.copy(hairStyle = it)) }
                EnumRow("Outfit", Outfit.entries, spec.outfit, enabled) {
                    onChange(
                        if (it == Outfit.NONE) {
                            spec.copy(outfit = it, footwear = Footwear.NONE, ageYears = maxOf(18f, spec.ageYears))
                        } else {
                            spec.copy(outfit = it)
                        },
                    )
                }
                if (spec.outfit == Outfit.NONE) {
                    Text(
                        "Adult-only nude anatomical base selected. This is the real unclothed base surface. " +
                            "Detailed sexual-anatomy controls stay hidden until each one changes real geometry or material output.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.primary,
                    )
                }
                EnumRow("Footwear", Footwear.entries, spec.footwear, enabled) { onChange(spec.copy(footwear = it)) }
                LabeledSlider(
                    "Height", spec.heightM, AvatarSpec.MIN_HEIGHT_M..AvatarSpec.MAX_HEIGHT_M, enabled
                ) { onChange(spec.copy(heightM = it)) }
                LabeledSlider("Body fat", spec.bodyFat, 0.06f..0.45f, enabled) { onChange(spec.copy(bodyFat = it)) }
                LabeledSlider("Muscle", spec.muscle, 0f..1f, enabled) { onChange(spec.copy(muscle = it)) }
                LabeledSlider(
                    "Age",
                    spec.ageYears,
                    (if (spec.outfit == Outfit.NONE) 18f else 16f)..70f,
                    enabled,
                ) { onChange(spec.copy(ageYears = it)) }
                LabeledSlider("Skin melanin", spec.skinMelanin, 0.02f..0.98f, enabled) { onChange(spec.copy(skinMelanin = it)) }
                LabeledSlider("Cloth roughness", if (spec.clothRoughness < 0f) 0.6f else spec.clothRoughness, 0.2f..1f, enabled) {
                    onChange(spec.copy(clothRoughness = it))
                }
                LabeledSlider("Gear metalness", if (spec.gearMetalness < 0f) 0.9f else spec.gearMetalness, 0f..1f, enabled) {
                    onChange(spec.copy(gearMetalness = it))
                }
            }
        }
    }
}

@Composable
private fun <T> EnumRow(label: String, values: List<T>, selected: T, enabled: Boolean, onSelect: (T) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, modifier = Modifier.width(96.dp), style = MaterialTheme.typography.bodyMedium)
        Box {
            FilterChip(
                selected = true,
                onClick = { open = true },
                enabled = enabled,
                label = { Text(enumLabel(selected)) },
            )
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                values.forEach { value ->
                    DropdownMenuItem(
                        text = { Text(enumLabel(value)) },
                        onClick = { onSelect(value); open = false },
                    )
                }
            }
        }
    }
}

@Composable
private fun LabeledSlider(label: String, value: Float, range: ClosedFloatingPointRange<Float>, enabled: Boolean, onChange: (Float) -> Unit) {
    Column {
        Row {
            Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            Text(formatValue(label, value), style = MaterialTheme.typography.bodySmall)
        }
        Slider(
            value = value.coerceIn(range.start, range.endInclusive),
            onValueChange = { if (enabled) onChange(it) },
            valueRange = range,
            enabled = enabled,
        )
    }
}

private fun enumLabel(value: Any?): String = when (value) {
    Outfit.NONE -> "nude anatomical base (18+)"
    else -> value.toString().lowercase().replace('_', ' ')
}

private fun formatValue(label: String, value: Float): String = when (label) {
    "Height" -> "%.2f m".format(value)
    "Age" -> "%.0f".format(value)
    else -> "%.2f".format(value)
}

private fun summaryLine(spec: AvatarSpec): String = buildString {
    append(spec.sex.name.lowercase().replace('_', ' '))
    append(" · ")
    append(spec.build.name.lowercase())
    append(" · ")
    append("%.2f m".format(spec.heightM))
    append(" · ")
    append(spec.hairStyle.name.lowercase())
    append(" · ")
    append(enumLabel(spec.outfit))
    if (spec.footwear != Footwear.NONE) append(" + " + spec.footwear.name.lowercase())
    append(" · melanin %.2f".format(spec.skinMelanin))
    if (spec.prompt.isNotBlank()) append("  —  \"").append(spec.prompt.take(60)).append('"')
}

/** ABI role mapping: slot A is the primary appearance hint; slot B is the outfit/material hint. */
fun roleFor(slotIsPrimary: Boolean): ImageRole = if (slotIsPrimary) ImageRole.IDENTITY else ImageRole.CLOTHING
