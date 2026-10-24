# DIY fault finding

A plain-language checklist for when AvatarForge does not behave. Start at step 0 every time:
the in-app **Diagnostics** screen does most of this work for you and gives you a report to attach
to a bug report.

## 0. Open Diagnostics first

- **In the viewer:** open the bottom panel strip and tap **diag**.
- **Anywhere else:** tap **☰ MENU → Diagnostics** (works before you generate anything).
- The panel shows traffic-light rows: **OK** (green), **WARN** (amber), **FAIL** (red), **INFO**
  (grey). Every WARN/FAIL row has a **Hint** line with the suggested action.
- Tap **Copy report** and paste it somewhere safe. It is plain text, contains no photos or prompts,
  and needs no network or permission.

What the report covers:

| Section | Meaning |
|---|---|
| App | Native library load, engine/ABI version, baked content counts (clips, blendshapes, regions) |
| Device | Model, Android version/API, CPU ABI, RAM, storage, thermal status, CPU cores |
| Renderer | Start-up lifecycle state, every Vulkan/GLES create attempt with its error, backend/driver once running, GPU feature flags, live FPS and quality tier |

## 1. The viewer is black or stuck on "initialising GPU…" / 0 FPS

1. Open **☰ MENU → Diagnostics** (or the **diag** panel) and read the **Renderer start-up
   attempts** card. Each attempt says whether Vulkan was preferred or GLES was forced, how long it
   took, and the exact error on failure.
2. Tap **Retry renderer** (also available on the red "3D viewer failed to start" card). The app
   automatically tries: your settings → a clean GLES 3.1 attempt → one delayed retry.
3. If it still fails, open the **render** panel, switch **Prefer Vulkan (else GLES 3.1) off**, then
   tap **Retry renderer** again. Some ANGLE-based devices reject the Vulkan surface but accept
   OpenGL ES.
4. Still failing? Copy the report. The **Last renderer error** row and the attempt card are the
   bits a developer needs; include them verbatim.

## 2. "3D viewer failed to start" card appears

- Tap the card's **Retry renderer** first.
- Read the error text on the card, then cross-check it against the Diagnostics **FAIL** rows and
  their hints.
- Common meanings:
  - *no GPU backend available* — both Vulkan and GLES refused; the report lists each reason. Try
    the Vulkan-off toggle (step 1.3), then restart the app.
  - *EGL error 0x3009 (BAD_MATCH)* — the window surface format was rejected; update to a build that
    includes the native-visual fix, and confirm the report shows the GLES attempt succeeding.
  - *the render surface is gone* — Android destroyed the Surface (rotation/background). Rotate or
    background/foreground the app once; a new Surface triggers a fresh create sequence.

## 3. It renders, but slowly ("below 30 FPS", stutter)

1. Diagnostics → Renderer → **Frame rate** row. A WARN here means the measured FPS is under 30
   while the renderer is actually running (a 0 FPS reading during start-up is not a performance
   result).
2. Open the **render** panel:
   - set quality to **AUTO** so the budget controller can step MSAA/shadow/bloom down;
   - turn **Bloom** and **Sub-surface scattering** off to compare;
   - drop **Shadows** or the shadow map cost if the device is low-RAM.
3. Check the Device section: a **moderate/severe thermal** row means the SoC is throttling. Let
   the phone cool; the renderer reduces quality automatically under heat.
4. Close background apps if the **Memory** row is WARN (a generation peaks near 1.5 GB native).

## 4. Generation will not start or fails mid-run

1. Diagnostics → App: a red **Native library** or **Engine** row means the build itself is broken
   on this device (missing ABI/ABI mismatch). Reinstall the correct ARM64 APK; nothing in the UI
   can repair this.
2. Device → **Memory** WARN: close other apps and retry. A low-RAM device is forced to the LOW
   render tier and a smaller texture budget by design.
3. Device → **Storage** WARN: free space; an exported avatar needs about 64 MB.
4. Device → **Thermal** FAIL (severe+): the OS may defer heavy work. Wait until the row returns to
   light/none.
5. If the progress screen shows a specific stage failure, note the stage name (Interpreting input,
   Building geometry, Unwrapping UVs, Baking PBR textures, Auto-rigging, Baking facial shapes,
   Baking clips, Writing .glb) and include it with the copied report.

## 5. Export / Save / Share fails

- **Save to…** uses the system file picker and needs no storage permission. If it fails, check
  Diagnostics → Device → Storage (free at least 64 MB) and that the chosen destination is not
  read-only (an SD card locked by another app, a USB drive that was unplugged).
- **Share** needs another app installed that accepts `model/gltf-binary` (Files, Drive, email…).
  "Nothing can share model/gltf-binary here" means no such app is present, not an export failure;
  use **Save to…** instead.
- The avatar always exists first in app-private storage; the Export panel shows its path.

## 6. The app crashes, closes itself, or will not open

1. Reopen it and go straight to **☰ MENU → Diagnostics → Copy report** before doing anything else.
2. Restart the phone (clears a wedged GPU driver and memory pressure).
3. Reinstall the APK if the App section shows a native-library FAIL.
4. Note what you did just before the crash (which screen, which button, rotation, backgrounding)
    and send it with the report.

## 7. Poses, faces or the model look wrong

- Use **inspect** (tap the model) to confirm which region is affected; the region id and name come
  from the engine's own table.
- Switch between Rest / T-pose / A-pose in the **pose** panel. Held poses are the first key of a
  real clip through the same skinning path, so a pose that is wrong everywhere points at the rig,
  while one wrong clip points at that clip.
- Reset all blendshape channels with the **Reset all channels** button in the **face** panel.

## 8. "It needs the internet?" — no

AvatarForge is offline by design and ships without the INTERNET permission. Every feature works in
airplane mode. If anything suggests a download or a server, treat it as a bug and report it.

## 9. Sending a useful report

Paste the copied diagnostics text and add, in your own words:

1. what you did;
2. what you expected;
3. what actually happened (include any red card text);
4. whether **Retry renderer** and the Vulkan-off toggle changed anything;
5. the app version from the top **App version** row and the device model row.

## Status legend

| Status | Meaning | What to do |
|---|---|---|
| OK | Check passed | Nothing |
| INFO | Measured fact, not a pass/fail | Read it for context |
| WARN | Working but constrained (RAM, storage, heat, frame rate) | Follow the row's Hint |
| FAIL | Check failed | Follow the Hint; include the row in a report |
