# AvatarForge self-improvement path (no AI)

This is a **deterministic app/renderer stability path**, not an autonomous learning system. It does
not use AI, does not contact a server, does not upload data, and does not rewrite source code. Its job
is to turn a renderer fault into a bounded recovery attempt and a small piece of local evidence that a
human or CI can turn into a regression test.

## The loop

```text
renderer fault
  -> classify + fingerprint locally
  -> try a known-safe configuration (bounded)
  -> journal the attempt privately
  -> probe sustained non-empty frame submission
  -> export a copyable regression report
  -> human/CI adds or tightens a deterministic test
```

### 1. Diagnostics

`MENU -> Self diagnostics` remains available even when the viewer is blank or stopped. The report
includes the device/GPU/backend identity, submitted geometry counts, current settings, last viewer
error, recovery status, and recent journal events. `Copy` puts the report on the clipboard;
`Export report` writes it through Android's Storage Access Framework to a location the user chooses.

### 2. Safe fallback

Startup, frame, and resize failures are classified into a small fault taxonomy (context loss, memory,
shader compile/link, EGL context/surface, framebuffer, invalid GL operation, resize, unknown). The
recovery policy then tries at most **two automatic attempts per failure session**:

1. safe settings on the current/preferred backend;
2. safe settings on the alternate backend.

"Safe" means: `LOW` quality, MSAA 1, shadows/bloom/SSS/ground plane disabled, shadow map 1024, bloom
levels 0. Camera, exposure, and the generated avatar are preserved. A tried-configuration set prevents
the same candidate from being retried in a loop. The user can also press **Safe recovery** manually.

### 3. Fault journal

The journal lives in private app storage:

```text
filesDir/avatarforge/recovery/journal.tsv
```

It is a bounded, tab-separated, URL-encoded log (version `AFRJ1`), capped at 100 records and 64 KiB.
Records contain timestamp, event kind, stage, fault category, a 16-hex SHA-256 fingerprint, backend,
attempt number, and candidate id. They do **not** contain prompts, photos, mesh data, or raw driver
messages. IO failures are reported in diagnostics but never crash recovery. `Clear journal` deletes it.

### 4. Regression check

After a recovery attempt, a frame probe requires **30 consecutive frames** with finite positive FPS and
frame time, non-zero submitted triangles, and non-zero draw calls. It times out after 180 observed
frames and records `PROBE_TIMEOUT` rather than claiming success.

Important: this proves **frame submission**, not visible pixels, likeness, or GPU correctness. A blank
stage can still submit geometry. Visible output must be confirmed by a human/device screenshot or a
future readback-based test.

### 5. Exportable report

The diagnostics report includes a `Next step` recommendation derived only from the local journal.
Export it before changing quality or clearing the journal. A useful regression report contains:

- device/GPU/driver/backend;
- fault category and fingerprint;
- which safe candidate was tried;
- whether frame submission became stable or timed out;
- a screenshot if the question is visibility.

## What this is not

- Not AI, not machine learning, not telemetry.
- Not autonomous code modification.
- Not a guarantee that the avatar is visible.
- Not a replacement for device testing.

## Developer regression gate

The stability path is covered by JVM tests in the SDK module:

```bash
./gradlew :sdk:testDebugUnitTest
```

The renderer host fault-injection suite remains:

```bash
bash tools/test_renderer_host.sh
```

CI runs both before packaging the APK. When a device report identifies a new fault fingerprint, add a
deterministic host/JVM test for that class of failure before changing the recovery ladder. Do not widen
the automatic retry budget to hide a repeatable fault.
