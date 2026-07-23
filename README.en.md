# OBS Studio Custom Build — Region Recording Enhancements

[中文 README](README.md)

A customized build of [OBS Studio](https://obsproject.com) (master branch) adding **Draw Region capture**, **scene-following canvas**, and a **floating quick-record widget** for recording any screen area fast.

---

## Feature 1: Draw Region Source

A new source type (plugin: `win-capture`) that captures an **arbitrary rectangular region** of the screen via DXGI Desktop Duplication.

### Usage

1. In the **Sources** dock, click `+` and choose **Draw Region**.
2. In the properties window click **Select Region...**, then drag a rectangle on the fullscreen overlay (right-click or Esc to cancel).
3. The selection takes effect immediately:
   - Canvas and output resolution change to the region size **without restarting OBS**;
   - The source is fitted to fill the canvas;
   - A **Desktop Audio** source (default output device) is added to the current scene if missing — handy when you're in a hurry.
4. You can also type X / Y / Width / Height manually, then click **Apply as Output Size** (audio is added here too).

### Properties

| Property | Description |
| --- | --- |
| Display | Monitor the region is captured from |
| X / Y / Width / Height | Region coordinates relative to the selected display |
| Select Region... | Fullscreen drag selection |
| Apply as Output Size | Set output resolution to the current region size |
| Capture Cursor | Draw the mouse cursor in the video |
| Force SDR | Capture HDR displays as SDR |

Output size is aligned automatically for encoders: width rounded down to a multiple of 4, height to even, minimum 32×32.

---

## Feature 2: Scene-Following Canvas

The canvas (output) resolution **follows the current scene** automatically:

- Switch to a scene containing a Draw Region source → the canvas becomes that region's size;
- Switch to a scene without one → the previous ("normal") resolution is restored.

Details:

- Tracking state is persisted in the user config and restored correctly across restarts;
- The resolution never changes while recording / streaming / replay buffer / virtual camera is active — it is re-evaluated when recording stops;
- Switching profiles resets the tracking state, so another profile's resolution is never "restored" into the current one;
- The region scene item is always fitted 1:1 (the relative-coordinate scaling pitfall is handled by applying transforms after the video reset).

---

## Feature 3: Floating Widget

A small always-on-top floating widget for quick recording control anywhere.

### Toggle

**Settings → General → Floating Widget → Enable**. Takes effect immediately; you can also hide it from its right-click menu.

### The widget

- **Drag**: move it (position is remembered);
- **Click**: start / stop recording;
- Shows `REC` when idle; turns red and **shows the elapsed recording time** while recording.

### Right-click menu

| Item | Description |
| --- | --- |
| Start/Stop Recording | Same as clicking |
| Draw Region | Region recording (below) |
| Scene | Switch the current scene (regular scenes are still managed in the main window) |
| Hide Floating Widget | Hide it (re-enable in Settings) |

### Draw Region recording flow

1. Right-click the widget → **Draw Region**, drag an initial region on the fullscreen overlay;
2. An **adjustable region frame** appears:
   - Drag **anywhere inside** to move it;
   - Drag the corners/edges (or anywhere within ±10px of the border) to **resize**;
   - The toolbar shows the live size with **Start Recording / Cancel** buttons; Esc cancels;
3. Click **Start Recording** and it automatically:
   - Creates a temporary scene (region source + default desktop audio);
   - Sets canvas/output to the region size;
   - Shows a red boundary **just outside** the captured area (so it is not included in the video);
   - Starts recording.
4. When recording stops it automatically: switches back to the previous scene, deletes the temporary scene, and restores the canvas.

Failure handling: if recording fails to start, the session is cleaned up; if OBS is closed mid-session, leftover temporary scenes are removed and the canvas is healed on next startup.

---

## Building

Same as stock OBS (CMake presets):

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

Output: `build_x64/rundir/RelWithDebInfo/bin/64bit/obs64.exe`.

> Note: `frontend-tools` and `win-dshow` are excluded from the build in `plugins/CMakeLists.txt` (a local change unrelated to these features).

## Main files added/changed

| File | Description |
| --- | --- |
| `plugins/win-capture/region-capture.c` | Draw Region source + proc handlers |
| `plugins/win-capture/region-picker.{c,h}` | Fullscreen region picker overlay |
| `plugins/win-capture/plugin-main.c` | Source/proc registration |
| `frontend/widgets/FloatingBall.{hpp,cpp}` | Floating widget, adjustable region frame, region recording flow |
| `frontend/widgets/OBSBasic.{hpp,cpp}` | Scene-following canvas manager |
| `frontend/settings/OBSBasicSettings.cpp` + `frontend/forms/OBSBasicSettings.ui` | Floating widget settings toggle |
| `plugins/win-capture/data/locale/*.ini`, `frontend/data/locale/*.ini` | EN/CN strings |

### Proc handlers exposed by the plugin

- `win_capture_region_picker_select` — shows the fullscreen picker, returns monitor id + region rect;
- `win_capture_region_clamp_to_monitor` — maps an absolute rect to its monitor and clamps it.

## License

Same as OBS Studio (GPLv2). See the original project.
