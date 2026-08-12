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
- **Ultrawide edge tracking** — optionally keeps the cursor centred until the captured scene reaches an edge.
- **Idle freeze** — pauses following after a configurable idle timeout, and resumes on movement.
- **Click halo** — optional click-only visual feedback with a configurable ring.

### Wiggle

- **Handheld camera drift** — position, roll and a slow breathe in and out, from smooth value noise rather than per-frame randomness, so it reads as a hand holding a camera rather than as a vibration.
- **Moves as one camera** — every ticked source is panned, rolled and breathed by the same amount about the same point, so a composed scene stays composed.
- **Speed jitter** — set a min and a max and the speed is redrawn from that range twice a second and eased onto, which gives the motion the hesitation and hurry of a real hand. Set them equal for a steady drift.
- **No exposed background** — sources are automatically enlarged by exactly what the drift costs, and the drift gives way at a canvas edge rather than pulling the background into view. You do not have to pre-scale anything.
- **Rides on the zoom** — the wiggle runs whether or not the scene is zoomed, and both apply at once.

Wiggle ranges are deliberately narrow — position 0–5 px, rotation 0–1°, scale 0–2 %, speed 0–5× — because this is meant to be felt rather than seen.

---

## Usage

1. **Tools → MotionCraft …**
2. Under **Sources**, tick the sources the camera should carry. Anything left unticked stays put — a webcam overlay or a watermark, say.
3. Under **Zoom Levels**, set a zoom factor and a hotkey for each level you want.
4. Under **Wiggle**, set the amounts and tick **Enable wiggle**.
5. **Apply.**

The wiggle starts as soon as you apply it and stays on until you untick it. If it was running when you closed OBS, it resumes once your scenes have loaded.

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
