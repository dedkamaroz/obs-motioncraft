# MotionCraft — Scene Zoom, Follow & Handheld Wiggle for OBS Studio

MotionCraft is an OBS Studio plugin that moves your scene like a camera operator would. It **zooms and pans to follow your mouse**, and it can lay a **handheld drift** over the top so a locked-off shot stops looking locked off.

Both effects work at the **scene level** on the sources you tick — no per-source filters, no shader setup.

MotionCraft is a fork of [Zoominator](https://github.com/mmlTools/zoominator) by [MML Tech](https://github.com/mmlTools), which is where everything the zoom does comes from. See [Credits](#credits).

---

## What It Does

### Zoom & Follow

- **Scene-wide zoom and pan** — smoothly transforms the whole scene based on mouse position.
- **Five assignable zoom levels** — each with its own hotkey and its own in / out timing. Pressing a level's key while already zoomed moves straight there, carrying the current speed through rather than restarting.
- **Smart clamping** — the canvas stays fully covered, with no background showing through, even with cropped or rotated sources.
- **Two ways to aim** — follow the desktop cursor across a chosen monitor (right when the scene *is* that screen), or point at the canvas in OBS's own preview panel (right when the scene is video or images). The preview mapping tracks the panel as you resize, zoom or scroll it, so the focal point is always the canvas pixel under the pointer.
- **Full range at any zoom** — optionally maps the pointer's whole travel onto the framing that can actually be reached, so a gentle zoom steers as readily as a deep one. Without it only the fraction `(z-1)/z` of the pointer's range moves anything: about 7% at the small zoom the wiggle asks for, 50% at 2x, 75% at 4x.
- **Holds when you look away** — in preview mode the focal point stays put when the pointer leaves the canvas, and returns to centre — keeping the current zoom level — when OBS is not the active window.
- **Ultrawide edge tracking** — optionally keeps the cursor centred until the captured scene reaches an edge.
- **Idle freeze** — pauses following after a configurable idle timeout, and resumes on movement.
- **Click halo** — optional click-only visual feedback with a configurable ring.

### Wiggle

- **Handheld camera drift** — position, roll and a slow breathe in and out, from smooth value noise rather than per-frame randomness, so it reads as a hand holding a camera rather than as a vibration.
- **Moves as one camera** — every ticked source is panned, rolled and breathed by the same amount about the same point, so a composed scene stays composed.
- **Everything jitters** - position, rotation, scale, speed and the smoothing itself each take a min, a midpoint and a max. Twice a second a fresh value is drawn for each from a bell curve centred on its midpoint, kept inside its range, and eased onto over the smoothing time, which gives the motion the hesitation and hurry of a real hand. Set a min and max equal to hold that parameter steady.
- **No exposed background** — sources are automatically enlarged by exactly what the drift costs, and the drift gives way at a canvas edge rather than pulling the background into view. You do not have to pre-scale anything.
- **Rides on the zoom** — the wiggle runs whether or not the scene is zoomed, and both apply at once.

### Everything Off

- **Off until you switch it on** — MotionCraft starts disabled every time OBS launches. Nothing is captured, no transform is touched and no keyboard hook is installed until you say so.
- **One switch, or one key** — the checkbox on the Target tab takes effect immediately, and an optional hotkey does the same from anywhere. Switching off unwinds any zoom and wiggle and puts every source back exactly as it was; switching on restores whatever you had configured, wiggle included.

Wiggle limits are position 0-20 px, rotation 0-15°, scale 0-5 %, speed 0-20× and smoothing 0.01-1.00 s. The defaults sit near the bottom of each: this is meant to be felt rather than seen, and anything larger is paid for with a bigger safety enlargement.

---

## Usage

1. **Tools → MotionCraft …**
2. Under **Sources**, tick the sources the camera should carry. Anything left unticked stays put — a webcam overlay or a watermark, say.
3. Under **Zoom Levels**, set a zoom factor and a hotkey for each level you want.
4. Under **Wiggle**, set the amounts and tick **Enable wiggle**.
5. **Apply.**
6. Tick **Enable MotionCraft** on the **Target** tab — it takes effect immediately. Bind the **Enable / Disable Hotkey** there too if you want to do this without opening the window.

Everything above is remembered except the on/off state itself: **MotionCraft starts switched off every time OBS launches**. A plugin that installs a system-wide keyboard hook and rewrites scene transforms should be something you turn on, not something you find already running. Switching it on restores everything you configured, including a wiggle that was set up earlier.

---

## Installation

### Windows

1. Download the latest release.
2. Extract the archive and move `motioncraft.dll` into your OBS Studio directory:
   ```
   C:\Program Files\obs-studio\obs-plugins\64bit
   ```
3. Copy the `data\obs-plugins\motioncraft` folder into `C:\Program Files\obs-studio\data\obs-plugins`.
4. Restart OBS.

### macOS

1. Download the `.pkg` from releases.
2. Install and restart OBS.

### Linux (X11)

1. Build from source.
2. Copy the plugin files into `~/.config/obs-studio/plugins/`.
3. Restart OBS.

### Upgrading from Zoominator

Your existing Zoominator settings — zoom levels, hotkeys, the included-source list — are adopted automatically the first time MotionCraft runs. The old config file is only read, never written, so Zoominator still works if you keep it installed. Do not run both at once: they drive the same scene item transforms and will fight over every frame.

---

## Build from Source

### Requirements

- CMake 3.28+
- A C++17 compiler (MSVC 2022, Xcode, or GCC/Clang)
- Qt6

OBS headers and libraries are fetched automatically by the build.

### Steps

```bash
git clone https://github.com/dedkamaroz/obs-motioncraft.git
cd obs-motioncraft
cmake --preset windows-x64        # or macos / ubuntu-x86_64
cmake --build build_x64 --config Release
```

### Tests

The geometry is checked offline, without OBS, so the maths can be verified without a running scene:

```bash
cl /EHsc /O2 tests\wiggle-test.cpp && wiggle-test.exe
cl /EHsc /O2 tests\plugin-toggle-test.cpp && plugin-toggle-test.exe
cl /EHsc /O2 tests\preview-mapping-test.cpp && preview-mapping-test.exe
cl /EHsc /O2 testsollow-range-test.cpp && follow-range-test.exe
cl /EHsc /O2 tests\hotkey-matching-test.cpp && hotkey-matching-test.exe
cl /EHsc /O2 tests\dialog-lifetime-test.cpp && dialog-lifetime-test.exe
cl /EHsc /O2 tests\scene-item-lifetime-test.cpp && scene-item-lifetime-test.exe
cl /EHsc /O2 tests\framing-clamp-test.cpp && framing-clamp-test.exe
cl /EHsc /O2 tests\zoom-geometry-test.cpp && zoom-geometry-test.exe
cl /EHsc /O2 tests\zoom-level-timing-test.cpp && zoom-level-timing-test.exe
```

---

## Compatibility Notes

- **Windows:** full support (global input and smooth tracking).
- **macOS:** requires Accessibility permissions for input tracking.
- **Linux (X11):** supported via XInput2.
- **Wayland:** native sessions are detected and X11 hooks are disabled. The Global Shortcuts portal can support hotkeys, but Wayland currently has no standard passive global cursor-position portal, so full mouse tracking still requires compositor-specific input capture support.

---

## Use Cases

- Tutorials and live coding
- Product demos
- Gameplay and analysis
- Vertical / short-form content
- Documentary-style pieces where a static source needs to feel shot rather than screen-captured

---

## Credits

MotionCraft is a fork of **[Zoominator](https://github.com/mmlTools/zoominator)**, created by **[MML Tech](https://github.com/mmlTools)** ([Marco Maxim](https://github.com/mmlTools)). The zoom, the follow, the framing clamp and the whole scene-level architecture this builds on are theirs — MotionCraft adds the wiggle on top of a design that was already doing the hard part.

If MotionCraft is useful to you, please consider [supporting the original project](https://ko-fi.com/mmltech).

Contributors to the upstream project: Marco Maxim, Derek Ross, Tim Brown.

Licensed under the GNU General Public License v2.0, the same as the original. See [LICENSE](LICENSE).
