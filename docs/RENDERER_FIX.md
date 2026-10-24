# Renderer Recovery and Verification

## Current fault-finding path

For user-facing troubleshooting, start with the [DIY Fault-Finding checklist](DIY_FAULT_FINDING.md). It explains which in-app screen to open and which displayed values to record.

## Black viewer or zero FPS

A healthy engine/native-library status does not prove that the renderer has started. Diagnostics may explicitly say that renderer startup is deferred until the Viewer opens. Open the Viewer before interpreting renderer telemetry.

When investigating a black or stuck viewer, capture the values the app actually displays:

- renderer/backend startup state and any error text;
- FPS and frame interval;
- draw calls, triangles, and vertices;
- selected quality preset/tier;
- memory, storage, and thermal status from Diagnostics.

Interpret the evidence in order:

1. **Renderer startup:** did the Viewer create a renderer, or does it remain on an initialization state / show an error?
2. **Frame execution:** do frames advance and do FPS/frame interval values change?
3. **Geometry submission:** are draw calls, triangles, and vertices non-zero when an avatar should be visible?
4. **Presentation and lifecycle:** is the avatar visible, and does leaving/reopening the Viewer or resizing the surface change the result?

If all frame and geometry counters remain zero, treat the issue as an initialization, frame-loop, submission, or presentation fault before performance tuning. Lowering quality is not evidence of a fix when no frames or geometry are being reported. If the selected preset is Fixed, record that state rather than expecting adaptive quality to change it.

## Verification standard

- **Source/build pass:** code or CI built successfully. This is not device-runtime proof.
- **Renderer runtime verified:** the target phone visibly displays the avatar and reports live frame activity and geometry submission.
- **Recovery verified:** repeat the relevant lifecycle step (for example, leave and reopen the Viewer) and confirm the renderer remains functional.
- **UNVERIFIED:** renderer has not been opened/tested, values are absent, or the evidence does not establish visible geometry.

Do not label the renderer fixed until target-device evidence supports the claim. Record app/build version, phone/Android version, backend, visible error text, and the counters above with each retest.
