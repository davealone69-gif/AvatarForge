/*
 * AvatarForge :: InputScreen.kt
 *
 * Where a generation starts. Three inputs, matching exactly what the engine can consume:
 *
 *   1. a text prompt            -> af_engine_analyze_prompt()  (keyword grammar, measured)
 *   2. Image A: the identity    -> af_engine_analyze_images(role = IDENTITY)
 *   3. Image B: the equipment   -> af_engine_analyze_images(role = CLOTHING)
 *
 * The spec panel below the inputs is not decoration: the engine's image analysis is observational
 * and the app is expected to show the user what it measured (skin tone from CIE L*a*b*, stature from
 * the head-to-body ratio, garment metalness) and let them correct it. A generator that silently
 * builds the wrong person from a good photo is worse than one that says "I read your subject as
 * 1.71 m with medium-brown skin - is that right?".
 */
package com.avatarforge.app.ui

import android.graphics.Bitmap
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
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.avatarforge.sdk.AvatarSpec
import com.avatarforge.sdk.Build
import com.avatarforge.sdk.Footwear
import com.avatarforge.sdk.HairStyle
import com.avatarforge.sdk.ImageRole
import com.avatarforge.sdk.Outfit
import com.avatarforge.sdk.Sex

private val PROMPT_EXAMPLES = listOf(
    "athletic woman, ponytail, full plate armour",
    "tall male bodybuilder in a leather jacket and boots",
    "slim teenage girl, t-shirt and sneakers, short hair",
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
    analysisNote: String?,
    onGeneratePrompt: () -> Unit,
    onGenerateImages: () -> Unit,
    enabled: Boolean,
    modifier: Modifier = Modifier,
) {
    val pickA = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        onImageA(AppBitmaps.decode(uri))
    }
    val pickB = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        onImageB(AppBitmaps.decode(uri))
    }

    LazyColumn(
        modifier = modifier.fillMaxSize().safeDrawingPadding(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            Text("Describe your avatar", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
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
                enabled = enabled,
            )
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PROMPT_EXAMPLES.take(2).forEach { example ->
                    TextButton(onClick = { onPromptChange(example) }, enabled = enabled) {
                        Text(example.split(",").first(), maxLines = 1)
                    }
                }
            }
        }

        item {
            Text("Reference images (optional)", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Text(
                "Image A is the identity: a portrait or half-body shot is enough - the lower body is inferred from the subject's proportions. " +
                    "Image B is the equipment: armour or clothing, segmented from its background and transferred onto the body.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                ImageSlot(
                    label = "A · identity",
                    role = "Portrait or half body",
                    bitmap = imageA,
                    onClick = { pickA.launch("image/*") },
                    onClear = { onImageA(null) },
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                )
                ImageSlot(
                    label = "B · equipment",
                    role = "Armour / clothing",
                    bitmap = imageB,
                    onClick = { pickB.launch("image/*") },
                    onClear = { onImageB(null) },
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                )
            }
        }
        if (analysisNote != null) {
            item {
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
                    Text(
                        analysisNote,
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }

        item { SpecEditor(spec, onSpecChange, enabled) }

        item {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = onGeneratePrompt,
                    enabled = enabled && prompt.isNotBlank(),
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Generate from prompt") }
                Button(
                    onClick = onGenerateImages,
                    enabled = enabled && imageA != null,
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(if (imageB != null) "Generate from both images" else "Generate from photo") }
                if (imageB != null && imageA == null) {
                    Text(
                        "Image B needs an identity image: the garment is transferred onto the body Image A describes.",
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
                EnumRow("Outfit", Outfit.entries, spec.outfit, enabled) { onChange(spec.copy(outfit = it)) }
                EnumRow("Footwear", Footwear.entries, spec.footwear, enabled) { onChange(spec.copy(footwear = it)) }
                LabeledSlider(
                    "Height", spec.heightM.takeIf { it > 0f } ?: 1.75f, 1.40f..2.10f, enabled
                ) { onChange(spec.copy(heightM = it)) }
                LabeledSlider("Body fat", spec.bodyFat, 0.06f..0.45f, enabled) { onChange(spec.copy(bodyFat = it)) }
                LabeledSlider("Muscle", spec.muscle, 0f..1f, enabled) { onChange(spec.copy(muscle = it)) }
                LabeledSlider("Age", spec.ageYears, 16f..70f, enabled) { onChange(spec.copy(ageYears = it)) }
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
                label = { Text(selected.toString().lowercase().replace('_', ' ')) },
            )
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                values.forEach { value ->
                    DropdownMenuItem(
                        text = { Text(value.toString().lowercase().replace('_', ' ')) },
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
    append(spec.outfit.name.lowercase().replace('_', ' '))
    if (spec.footwear != Footwear.NONE) append(" + " + spec.footwear.name.lowercase())
    append(" · melanin %.2f".format(spec.skinMelanin))
    if (spec.prompt.isNotBlank()) append("  —  \"").append(spec.prompt.take(60)).append('"')
}

/** Which role an image slot is analysed as: slot A is the identity, slot B the equipment. */
fun roleFor(slotIsIdentity: Boolean): ImageRole = if (slotIsIdentity) ImageRole.IDENTITY else ImageRole.CLOTHING
