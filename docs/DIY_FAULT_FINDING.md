# DIY Fault-Finding

open Diagnostics first. It shows whether AvatarForge's engine and device checks are healthy before you try to fix anything. Then follow the section that matches what you see on screen.

## Status legend

- **OK**: the screen reports the check passed.
- **INFO**: useful context, not a failure by itself.
- **WARN**: a problem or limitation needs attention.
- **FAIL**: the requested operation failed.
- **UNVERIFIED**: the app has not produced evidence that the operation worked. Missing measurements are not proof of success.

Use the exact wording and values shown in the app. Do not treat a healthy engine status as proof that the viewer rendered, or treat a zero measurement as a real measured zero unless the screen explicitly says so.

## 1. Start in Diagnostics

- Open **Diagnostics** first.
- Read the overall status, app/engine version, native-library status, device/ABI, available memory and storage, and thermal status.
- If Diagnostics says the renderer has **not started** because it is created when the viewer opens, that is **INFO**, not a renderer pass or failure. Open the viewer before judging rendering.
- If the native library or engine health reports **FAIL**, capture that text before retrying. Do not jump straight to generation or performance tuning.
- If memory is low or thermal status is elevated, close other heavy apps, let the phone cool, and retry only when the on-screen status improves.

## 2. Black screen or stuck viewer

- Open the **Viewer** and wait briefly for its status to change.
- Read the renderer/backend or startup/error text shown by the app. If it says **initialising GPU**, note whether that message ever changes.
- Check the displayed **FPS**, **frame interval**, **draw calls**, **triangles**, **vertices**, and quality preset/tier if those fields are present.
- A black viewer with FPS and geometry counters all at zero means rendering is **not yet verified**. It does not by itself prove the GPU is too slow.
- If the app shows a renderer startup error, copy the exact error. If it stays on initialising with no error, report that it remains stuck and include the counters.
- Leave the Viewer and reopen it once. If the image returns, report that recovery too. Do not repeatedly reinstall or change several settings at once.

## 3. “Failed to start” card

- Read the whole **Failed to start** card, including any reason, service name, or retry instruction.
- Return to **Diagnostics** and compare the engine/native-library, memory, storage, and thermal values with the card.
- If the card names a missing or unavailable service, use the service status and instructions actually shown in the app. Do not assume an online service is required; AvatarForge is designed to work offline.
- Retry once after addressing the displayed reason. If the same card returns, capture both the card and Diagnostics rather than guessing at a fix.

## 4. Low FPS or stutter

- In the **Viewer**, record FPS and frame interval while the avatar is visible and while the stutter happens.
- Record draw calls, triangles, vertices, selected preset, quality tier, and any thermal or performance warning that the app displays.
- If FPS is low but draw calls and geometry counts are non-zero, this is a performance investigation. Try one lower quality preset only if the in-app selector allows it, then compare the same counters.
- If FPS, draw calls, triangles, and vertices remain zero, treat this as a startup/frame/submission problem first. Lowering quality cannot prove or repair a renderer that is not submitting visible geometry.
- If the selected preset is **Fixed**, do not expect automatic quality changes to step it down. Report the selected preset exactly as shown.

## 5. Generation failures

- On the **Generate** or progress screen, record the stage and exact failure text where generation stopped.
- Check the progress/status shown by the app. A request that failed or stopped partway through is not a successful generation, even if an older avatar remains visible.
- Open **Diagnostics** and note available memory, free app storage, and thermal status. Retry only after resolving a limitation the app actually reports.
- If the app offers retry, use it once with the same input. If it fails again, report the stage, input type (text, one photo, or two images), and error text. Do not include private photos unless you choose to share them.

## 6. Export or share failures

- On the **Export** or **Share** screen, record the selected format, destination/action, and exact error or status shown.
- If the app says export succeeded, check that the resulting file is actually present and has the expected file type and non-zero size before calling it verified.
- If Android shows a file picker or share sheet, note whether it opened, whether you could select a destination, and whether the app returned a success or failure message.
- Retry once to a different user-chosen destination if the UI supports it. Report the format, visible status, and where the flow stopped. Never rename a file extension to make an invalid export appear successful.

## 7. Crashes, freezes, or the app disappearing

- If the app closes, freezes, or returns to its start screen, note what screen you were using and the action immediately before it happened.
- Reopen AvatarForge and open **Diagnostics**. Record the current version, engine/native-library status, memory, storage, and thermal status.
- If the app recovers when you leave and reopen the Viewer, say so. That is useful lifecycle evidence, but it does not prove the underlying fault is fixed.
- If a crash report or error dialog appears, preserve its exact text. Report whether this happens every time or only after a long session, generation, export, screen rotation, or background/foreground transition.

## 8. The offline guarantee

- AvatarForge is designed to run locally and offline. A network connection should not be required for the core on-device engine and viewer.
- If you want to check this, wait until any current operation is finished, enable **Airplane mode** in Android settings, then reopen AvatarForge and test a local feature.
- Report the exact in-app status if a feature refuses to start while offline. Do not enter credentials or configure a remote endpoint just to work around an unexplained failure.
- Offline success is verified only for the feature you actually tested. Passing Diagnostics alone does not prove that generation, rendering, or export works offline.

## 9. Send a useful report

Include the following, copied from the app where possible:

- App version/build and engine version/ABI from **Diagnostics**.
- Phone model, Android version/API, and ABI as displayed.
- Overall Diagnostics status, native-library/engine health, memory, storage, and thermal status.
- The screen where the issue happened and the exact visible error/status text.
- For viewer problems: backend/startup state, FPS, frame interval, draw calls, triangles, vertices, preset and quality tier.
- The steps immediately before the problem, whether it is repeatable, and what happened after leaving/reopening the screen.
- Whether the test was offline, and whether the same operation succeeded on a retry.

A screenshot of **Diagnostics** plus a screenshot of the affected screen is usually more useful than “it doesn't work.” Hide personal information before sharing. Never report a build as fixed solely because it compiled: a renderer fix needs visible geometry and live frame/counter evidence on the target phone.
