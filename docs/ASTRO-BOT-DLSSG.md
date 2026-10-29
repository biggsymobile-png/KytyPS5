# KytyPS5 (Jetsku U59) with DLSS and FSR for Astro Bot and Astro's Playroom

An **experimental, unofficial** test build of [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5)
that adds frame generation and modern upscaling to **ASTRO BOT (PPSA21564)** and
**Astro's Playroom (PPSA01325)**:

| | What it does | GPU | Platforms |
|---|---|---|---|
| **NVIDIA DLSS frame generation (DLSS-G)** | 2x, 3x, 4x generated frames, with Reflex | RTX 40 / 50 (3x/4x: RTX 50) | Windows |
| **AMD FSR 3.1 frame generation** | 2x generated frames | any GPU (AMD, Intel, RTX 20/30, ...) | Windows, Linux, macOS |
| **NVIDIA DLSS upscaling (super resolution)** | replaces the game's TAA | any RTX | Windows |
| **AMD FSR 3.1 upscaling** | replaces the game's TAA | any Vulkan GPU | Windows, Linux, macOS |
| **Built-in render resolution** | 540p / 720p / 1080p / 1368p / 1872p / 2160p, no patch file needed | any | all |

Everything else is Jetsku's U59 int21 release (24 October 2026).

All of this only switches on where it finds Astro Bot's or Playroom's inputs (depth, motion vectors
and camera); in other games it stays off. With the `streamline` folder present Streamline still sits
between the emulator and the driver, so if another game misbehaves with this build, set
`KYTY_FRAMEGEN=0` or remove the folder.

## Requirements

- **Windows 10/11** with **hardware-accelerated GPU scheduling** on (Settings > System > Display >
  Graphics > Advanced graphics settings; restart after changing it), for frame generation.
- **DLSS-G and DLSS upscaling:** an NVIDIA RTX card (RTX 40 or 50 for frame generation) with a
  recent driver (tested: RTX 5080, 617.42), and NVIDIA's **Streamline v2.14.1** runtime files (not
  included, see below).
- **FSR upscaling and FSR frame generation:** any GPU. Nothing to download: AMD's
  `amd_fidelityfx_vk.dll` (FidelityFX SDK v1.1.4, MIT licence) is included next to the emulator.
- **Linux / macOS:** see below (FSR upscaling, FSR frame generation and the render resolution).

## Setup

1. Download this release's zip and unzip it into a new folder.
2. **For DLSS only:** download NVIDIA Streamline **v2.14.1**:
   https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1 - pick
   `streamline-sdk-v2.14.1.zip` (not the aarch64/arm64ec ones).
3. **For DLSS only:** in the unzipped emulator folder, next to `kyty_emulator.exe`, make a folder
   named exactly `streamline` and copy into it the files from the Streamline zip's `bin\x64` folder
   (the files themselves; the `development` subfolder is not needed).

   ```
   KytyPS5-AstroBot-DLSSG\
   ├── kyty_emulator.exe
   ├── kyty_launcher.exe
   ├── amd_fidelityfx_vk.dll
   ├── ...
   └── streamline\
       ├── sl.interposer.dll
       ├── sl.common.dll
       ├── sl.dlss_g.dll
       ├── sl.dlss.dll
       ├── sl.reflex.dll
       ├── sl.pcl.dll
       ├── nvngx_dlssg.dll
       ├── nvngx_dlss.dll
       └── ...
   ```
4. Turn **NVIDIA Smooth Motion off** for kyty_emulator.exe in the NVIDIA App (it clashes with
   frame generation).
5. Pick your settings in the launcher (below) and start the game as usual.

## Settings

In the launcher, a game's **Graphics & display** settings have:

- **Frame generation:** Off, 2x, 3x, 4x (NVIDIA DLSS-G; 3x and 4x need an RTX 50 card), or
  **2x (AMD FSR 3.1, any GPU)**.
- **Upscaling:** the game's own TAA, NVIDIA DLSS or AMD FSR 3.1.
- **Render resolution:** the game's 3D resolution, for both Astro Bot and Playroom (lower is faster).
- **Relaxed shader maths:** simpler shader maths, see below.
- **Smooth frame pacing** (`KYTY_PRESENT_ABS_VBLANK`): after a slow frame a vblank is skipped
  instead of the next frames being shown back to back. Only the display skips it: the game still
  counts the vblank, so its speed stays right.
- **Frame rate limit** (`KYTY_FRAMEGEN_FPS_CAP`): with DLSS-G, NVIDIA Reflex holds the rendered
  frames to that rate so generated frames are evenly spaced; pick one your PC holds, e.g. 40 or 60
  with Vblank frequency 120.

They take precedence over `u59-preset.json`.

### Recommended on a G-Sync / FreeSync display: the smoothest way to play

With these settings the frame times are **remarkably even**: no judder and no hitching, just a
steady, fluid picture. In the tester's words, "ultra smooth because of the perfect frame times".

1. **Present mode: Immediate**, with **Smooth frame pacing** ticked. A frame that misses its vblank
   is shown the moment it is ready, instead of a whole vblank later, and the variable refresh shows
   it evenly. Measured in a busy Astro Bot level (Sky Garden) against FIFO: the visible hitches
   (doubled 33 ms frames) dropped from **97 a minute to 7**, and the slowest 1% of frames went from
   **33 ms to 23 ms**.
2. **Vblank frequency 60**, with **frame generation on top** (2x: 120 fps on screen) at a matching
   **Frame rate limit**. Lower the render resolution if your PC cannot hold 60 in the busiest
   levels: evenly timed frames feel far smoother than a rate that keeps dipping.

**Below 60** (40 or 45 gives beautifully even frames) Astro Bot keeps its speed with **Decouple
built in** (since build 23): no patch file or button combination needed, any version of the game
(US and EU). The console shows `Decouple: built-in one on`. If it says `not applied` instead, a
"Decouple gameplay timers from render FPS" mod in the patch file is used as before; please report
that line with your game version. The community "[Fix] Bot rescue" mod is skipped: it slowed the
game down (see Known issues). If you still see slow motion below 60, go back to 60 and please
report the level.

Without a variable-refresh display keep FIFO. Astro's Playroom: keep the Vblank frequency at 60
(see below).

This build is made like Jetsku's U59 releases (profile-guided optimisation trained on Astro Bot,
the emulator's diagnostics compiled out) and ships Jetsku's tuned **`u59-preset.json`** next to
the launcher. While that file is there, the launcher **ignores `KYTY_...` environment variables**
and uses the file instead: add settings to it, one `"NAME": "value",` line each, for example
`"KYTY_FRAMEGEN": "2",` or `"KYTY_VRAM_BUDGET_MB": "12000",`.

| Variable | Effect |
|---|---|
| `KYTY_FRAMEGEN=0` | Frame generation off |
| `KYTY_FRAMEGEN=1` | 2x (default) |
| `KYTY_FRAMEGEN=2` / `3` | 3x / 4x multi frame generation (DLSS-G, RTX 50 only) |
| `KYTY_FRAMEGEN_BACKEND=fsr` | AMD FSR 3.1 frame generation instead of DLSS-G (launcher: 2x AMD FSR 3.1) |
| `KYTY_DLSS=1` | DLSS upscaling in place of the game's TAA (launcher: Upscaling) |
| `KYTY_FSR=1` | FSR 3.1 upscaling in place of the game's TAA (launcher: Upscaling) |
| `KYTY_FSR_SHARPNESS=0.5` | FSR upscaling's sharpening, 0 to 1 (default off) |
| `KYTY_ASTRO_RENDER_RES=1080` | Render resolution: 540, 720, 1080, 1368, 1872 or 2160, both games (launcher: Render resolution) |
| `KYTY_FRAMEGEN_FPS_CAP=40` | With DLSS-G: Reflex limits the rendered frames to 40 fps (launcher: Frame rate limit) |
| `KYTY_FLOAT_PROFILE=relaxed` | Simpler shader maths (launcher: Relaxed shader maths) |
| `KYTY_STREAMLINE_DIR=<folder>` | Use Streamline files from another folder |
| `KYTY_VRAM_BUDGET_MB=12000` | Leave video memory for DLSS-G (helps on 16 GB cards at 4K) |
| `KYTY_PORTABLE=0` | Use the shared `Kyty.ini` instead of this build's own (see below) |

## NVIDIA DLSS frame generation (DLSS-G)

The emulator's console window shows, in order:

```
Frame generation: Streamline loaded from ...\streamline (1 generated frame(s) per frame)
Frame generation: Streamline offers DLSS-G with this GPU, driver and Windows
Frame generation: Vulkan is loaded from sl.interposer.dll (Streamline's interposer, as needed)
Frame generation: DLSS-G ready (1 generated frame(s) per frame); it turns on in the game's 3D view
Frame generation: the game's inputs found (motion and depth 3840x2160, ...)
Frame generation: DLSS-G on
```

Use the **NVIDIA overlay** (Alt+R) to see the frame rate on screen: it counts the generated
frames. The emulator's own counter shows the game's real frame rate, which frame generation does
not change.

## AMD FSR 3.1 frame generation (experimental)

**Frame generation: 2x (AMD FSR 3.1, any GPU)** generates frames with AMD's FSR 3.1 instead of
DLSS-G, on **any GPU**, from the same depth, motion vectors and camera. No Streamline files are
needed. Use it with **FSR upscaling or the game's own TAA** (not with DLSS upscaling). The console
shows `Frame generation: FSR swapchain ...`, `FSR 3.1 ready ...`, then `FSR on` and
`FSR 2x on (render ..., display ...)` in the 3D view; it switches off during videos and menus and
back on in the 3D view. `"KYTY_FRAMEGEN_DEBUG": "1",` draws FSR's tear and reset lines, `"2"` its
debug view. Tested working on Windows in both Astro Bot and Playroom.

**On Linux and macOS** (new in this build, untested by us) FSR frame generation is built into the
emulator from AMD's FidelityFX SDK 1.1.4 sources, so the same launcher choice (or
`"KYTY_FRAMEGEN": "1",` with `"KYTY_FRAMEGEN_BACKEND": "fsr",` in `u59-preset.json`) works on any
Vulkan GPU, no extra file needed. AMD's frame generation swapchain is Windows only, so there the
emulator shows the frames itself, on its vblank ticks: the generated frame at the tick a new game
frame arrives, the game's frame at the next tick. So set **Vblank frequency to twice the game's
frame rate** (e.g. 120 for 60 fps, with a display that shows 120 Hz); if the game reaches the
vblank rate itself, no frame is generated. The console shows `Frame generation: AMD FSR 3.1 frame
generation asked for (2x, built in)`, `FSR 3.1 ready (..., built in)`, `FSR on` and
`FSR 2x on (...; built in, shown at the vblank ticks)`. It needs SDR output (not HDR). Please report
whether it works, with those lines.

## DLSS and FSR upscaling (experimental)

Astro Bot draws its 3D image at a render resolution and its own TAA pass upscales that to the
output resolution. With **DLSS** (any RTX card) or **FSR 3.1** (any GPU) upscaling on, that step
is done by DLSS or FSR instead of the game's TAA, from the render resolution the launcher's
**Render resolution** picks:

| Render resolution | Works like |
|---|---|
| 960x540 / 1280x720 | Ultra Performance / Performance (fastest; for low-end GPUs and handhelds) |
| 1920x1080 | Performance/Quality (for lower-end GPUs) |
| 2432x1368 | Quality |
| 3328x1872 | between Ultra Quality and DLAA |
| Game's choice / 3840x2160 | DLAA / native AA (best image, a little slower than the game's TAA) |

The console shows `DLSS upscaling: 1920x1080 to ...` / `FSR upscaling: 1920x1080 to ...`, then
`... upscaling: on` and a status line every 600 frames. Both work with or without frame generation.
For DLSS your `streamline` folder needs Streamline's `sl.dlss.dll` and `nvngx_dlss.dll`.

If the image shimmers or looks soft and wobbly, the camera jitter's direction may be wrong: try
`"KYTY_DLSS_JITTER_X": "-1",` and/or `"KYTY_DLSS_JITTER_Y": "-1",`, or
`"KYTY_DLSS_JITTER": "projection",` in `u59-preset.json`; trails behind moving things:
`"KYTY_DLSS_MV_JITTERED": "1",`. These apply to FSR too. Please report which one looked right.

## Built-in render resolution

The launcher's **Render resolution** (`KYTY_ASTRO_RENDER_RES`) needs no patch file: the emulator
finds the game's resolution code itself, in **Astro Bot** (01.018 and other versions where it
matches) and **Astro's Playroom** (found by its instructions, so other versions should match too).
The console says whether it was applied, and why not if it was not. An enabled "Fixed render
resolution" patch in a patch file takes precedence.

## Astro's Playroom (PPSA01325)

Playroom uses the same kind of TAA pass as Astro Bot, so everything above works in it: **DLSS-G**,
**FSR frame generation**, **DLSS and FSR upscaling** and the **render resolution** (tested on
version 01.905). One difference: Playroom's TAA runs at the render resolution and a later pass of
its own does the upscale, so DLSS and FSR work there as **anti-aliasing at the render resolution
(DLAA / native AA)**, not as the upscaler. The console's lines are the same as for Astro Bot.

**Work in progress: real upscaling in Playroom.** The plan is to upscale Playroom's scene just before
its last pass, so that the game's own HUD is drawn sharp over it. The idea of a late upscale in
Playroom comes from [iska0702/DLSSKytyPS5](https://github.com/iska0702/DLSSKytyPS5). This build only
adds a probe that changes nothing on screen. In Playroom it prints up to 24 console lines starting
`Composite probe:`, listing the image and target sizes and formats of the TAA pass and of the last
pass. `KYTY_COMPOSITE_PROBE=0` turns it off.

**Playroom runs at double speed with a Vblank frequency above 60** unless a "Decouple gameplay
timers from render FPS" patch is applied (its game logic steps once per frame). Keep Playroom's
**Vblank frequency at 60** (launcher: the game's settings, Graphics & display; frontends:
`--vblank-frequency 60`). DLSS-G and FSR frame generation on Windows do not need more. With the
decouple patch (the console shows `Successfully applied cheat: Decouple gameplay timers from render
FPS`) a higher rate is fine.

## Shader maths profile (experimental)

**Relaxed shader maths** (`KYTY_FLOAT_PROFILE`): `precise` (the default) copies the PS5 GPU's exact
floating-point rules, which takes several instructions for things like min/max with NaNs, tiny
(denormal) numbers and rounding modes. `relaxed` maps those to the PC GPU's own instructions
instead: smaller shaders that compile and run a little faster, which can mean less stutter when new
shaders appear. Most games never notice; if you see sparkles, black or white pixels, flicker or
broken effects with it, go back to `precise`. The first start after changing it compiles the
game's shaders again.

## Linux, Batocera / Steam Deck and macOS

The release also has a **Linux build** (`...-Linux-x86_64.tar.gz`) and a **macOS build**
(`...-macOS-x86_64.zip`, Intel Macs; Apple Silicon through Rosetta, with MoltenVK). DLSS, frame
generation (DLSS-G) and Reflex are Windows only and not in them. **FSR upscaling and FSR frame
generation are**: there they are built into the emulator from AMD's FidelityFX SDK 1.1.4 sources
(no extra file), so **Upscaling: AMD FSR** and **Frame generation: 2x (AMD FSR 3.1)** in the
launcher, or `"KYTY_FSR": "1",` and `"KYTY_FRAMEGEN_BACKEND": "fsr",` in `u59-preset.json`, work on
any Vulkan GPU (frame generation: see above, Vblank frequency at twice the game's frame rate). The built-in **render resolution**, the **shader maths profile** and everything else
from Jetsku's int21 are in them too. The macOS build is new and untested by us: please report
whether it works (and the console's `FSR upscaling:` and `Frame generation:` lines if it does not).

Started without the launcher (as frontends do), the emulator applies `u59-preset.json` from its
folder itself, so add settings there, e.g. `"KYTY_ASTRO_RENDER_RES": "1080",` and
`"KYTY_FSR": "1",`. On macOS, keep `u59-preset.json` next to `KytyPS5.app`.

Batocera: run the community `install_ps5_kyty.sh` with this release's Linux file:
`KYTY_URL=<the .tar.gz's download link> bash install_ps5_kyty.sh`.

## Separate settings per build (portable)

These builds keep their own `Kyty.ini` (game folders, per-game settings) next to the launcher
(on macOS next to `KytyPS5.app`), so another Kyty build no longer overwrites them. The first start
copies the shared one, so nothing has to be set up again. `KYTY_PORTABLE=0` goes back to the
shared file; a folder the launcher cannot write to (e.g. Program Files) does too.

## Known issues

- **The HUD can smear a little with FSR frame generation.** The game draws its HUD into the same
  image as the 3D scene, so FSR generates it along with the scene. With DLSS-G it is steady.
- **FSR frame generation on Linux and macOS is new and untested**: it needs the Vblank frequency
  at about twice the game's frame rate and a display that refreshes that fast.
- **Astro's Playroom at double speed:** a Vblank frequency above 60 without the decouple patch.
  Set it to 60 for Playroom (see Astro's Playroom above).
- **New in this build: DualSense gyro no longer drifts.** Every gyro reads a small rotation even
  at rest, and passed straight to the game it made aiming and tilt creep sideways during play (the
  accelerometer correction already fixed tilt, but nothing can correct heading that way). The
  emulator now measures that resting offset whenever the controller is held still for a moment and
  removes it from every reading (the console shows `Gyro calibration: resting offset ...`;
  `"KYTY_GYRO_CALIBRATION": "0",` turns it off). The launcher's render resolutions are now listed
  smallest to largest (saved choices are kept).
- **Fixed in build 19: slow motion for a whole level below 60 fps with the decouple patch**
  (Pumpkin Peril at Vblank 40). The community "[Fix] Bot rescue plays with the game's own 60 fps
  timing" mod switches Decouple off when a rescue starts and only back on when it ends; some levels
  start one without ending it. The emulator now applies that fix itself, automatically (no button
  combination), in place of the mod (it is skipped, enabled or not), with a safety net: if Decouple
  stays off for 6 s (15 s before build 22), it is switched back on (the console then shows `Game
  patch: bot rescue fix: Decouple was off ...`). Settings: `"KYTY_ASTRO_RESCUE_TIMEOUT_S": "6",`
  (2 to 600), and `"KYTY_ASTRO_RESCUE_FIX": "0",` to use the patch file's mod as before.
- **Build 23b (test): Decouple built in for Astro's Playroom too.** Playroom uses the same timing
  code as Astro Bot, so the built-in Decouple now covers it (01.905.000; other versions: the patch
  file's mod as before). Its patch file's Decouple passed the raw frame time to the game, which
  made Playroom less smooth than Astro Bot at the same settings; the built-in one smooths it the
  same way, and Playroom no longer needs the patch file for a Vblank frequency above 60. The
  console shows `Decouple: built-in one on (Astro's Playroom ...)`; the patch file's Decouple mod
  is skipped (its physics and ultrawide mods still apply).
- **Build 23d (test): finding Playroom's render size groups, for real upscaling.** The render
  resolution changes five groups of sizes in Playroom; some size the 3D scene, others probably its
  TAA outputs and HUD, which should keep their full size so that DLSS/FSR upscales for real and the
  HUD stays sharp. `"KYTY_PLAYROOM_RES_KEEP": "3",` (in `u59-preset.json`) keeps group 3 at the
  game's own sizes, and so on for 1 to 5, and logs `Composite probe:` lines. Testers only.
  Also two fixes from [moooohung](https://github.com/moooohung) (pull request #2): Korean and
  Japanese text in games no longer shows as `?` (every installed CJK font is tried, not only the
  first), and FSR frame generation enables the Vulkan extensions FSR needs without FSR upscaling.
- **Build 23c (test): the built-in Decouple finds the game's frame update by its shape**, so it
  should work in other versions of both games too (the console says `agrees with the known one`
  on 01.018.000 and 01.905.000; on another version, `built-in one on` or, if it is not sure,
  `not applied` and the patch file's Decouple as before). Please report your game version and
  that line.
- **New in build 23: Decouple built in, the rescue mod's slow motion gone, gyro re-centre key.**
  - **Decouple built in.** The emulator now finds Astro Bot's timing code itself, in any version
    and region, and lets game time follow the real frame time, with the community v3 mod's
    smoothing and its "delta time capped at 1/20 s" safety (`"KYTY_ASTRO_DECOUPLE_DT_CAP": "30",`
    for 1/30 s). The patch file's Decouple mod and the mods that need it (frame-limiter sleep,
    precise frame caps, delta-time caps) are skipped while it is on; use the launcher's Frame
    rate limit instead of the precise caps. `"KYTY_ASTRO_DECOUPLE": "0",` goes back to the patch
    file's mod.
  - **The "Bot rescue" mod is skipped.** Testing showed that its two hooks do not belong to
    rescues at all: they mark entering and leaving a level (and some areas), and a real rescue
    calls neither. So the mod switched Decouple off when a level started, and only the end of the
    level or a safety timer switched it back on: the slow motion at level starts and in the middle
    of stages. Rescues work normally without it. `"KYTY_ASTRO_RESCUE_FIX": "1",` still turns on the
    built-in version with its 6 s safety timer; if a rescue goes wrong without it, please report
    it with your log.
  - **Gyro re-centre key.** Hold the controller the way you play and press the key in the game
    window (launcher: the game's settings, Graphics & display, "Gyro re-centre key"; default F9): that becomes its
    neutral position. Useful if the game started while the controller was held at an angle.
  - The gyro offset is saved between sessions (from build 22b), so the view is centred from the
    start; the test logs from builds 22 and 22b are off again.
- **Build 22b (test): the gyro is centred from the start, and a deeper rescue log.** The gyro
  calibration only corrected once the pad had rested for a moment, so in the first minutes of a
  session the view could creep off centre and stay there. The learned offset is now saved per
  controller in `gyro-calibration.txt` next to the emulator and loaded at the next start (the
  console shows `Gyro calibration: saved resting offset ... loaded`), and still refined during
  play. The first session still needs the pad to rest once: put it down for 3 seconds at start-up.
  The rescue log now also prints each call's chain (`bot rescue log:   chain ...`), to tell a real
  rescue apart from the level launches and the gameplay moments that use the same hook.
- **Build 22 (test): shorter slow-down at a level's start, and two logs.** The rescue fix's hook
  also fires at the start of some levels, not only for rescues, so those levels began with up to
  15 s of slow motion; the safety net now steps in after 6 s. To find a proper fix, the console
  shows a `Game patch: bot rescue log:` line for each rescue start and end, with the calling code
  and the time (`"KYTY_ASTRO_RESCUE_LOG": "0",` hides them). For a built-in Decouple that works in
  every version of the game without a patch file, the console also shows `Decouple probe:` lines at
  start-up, saying where it finds the game's timing code (`"KYTY_DECOUPLE_PROBE": "0",` hides
  them). Neither log changes the game.
- **Fixed in build 18: slow motion with frame generation and Smooth frame pacing.** When a
  frame took longer to present than one vblank (DLSS-G pacing its generated frames, more often at
  3x than 2x), the skipped vblank was also lost to the game, which then ran slower, in Astro Bot
  until the next load. The display still skips it, but the game now receives every vblank on time
  (the console shows `Vblank catch-up: ...` when it happens; `"KYTY_VBLANK_CATCH_UP": "0",` restores
  the old behaviour). With older builds: untick Smooth frame pacing when using frame generation.
- **Fixed in build 17: Astro's Playroom too fast with Present mode Immediate and Smooth frame
  pacing.** That combination showed every frame as soon as it was ready, so the Vblank frequency
  no longer capped the frame rate, and Playroom's game logic (one step per frame) sped up with it.
  It now shows at most one frame per vblank (`"KYTY_PRESENT_IMMEDIATE_UNCAPPED": "1",` restores the
  old behaviour). With older builds: use Present mode FIFO or untick Smooth frame pacing.
- **FSR frame generation does not work together with DLSS upscaling.** Use FSR upscaling or the
  game's TAA with it.
- **Game patches are separate and up to you.** Frame generation and upscaling do not depend on
  them: they work with or without the usual patches (ultrawide, fixed resolution, FPS unlock /
  decouple and so on). Patches that replace the game's renderer or its anti-aliasing could stop it
  finding its inputs; it then just stays off.
- **Tested on Astro Bot 01.018 (PPSA21564) and Astro's Playroom 01.905 (PPSA01325).** Other regions
  or updates very likely work the same, but if the console never shows `inputs found` in the game's
  3D view, your version draws differently: please report it with your title ID and version.
- **Frame generation does not fix game slowdown.** If the game itself runs below the frame rate
  its patches expect, it still slows down; frame generation only adds frames on top of what is
  rendered.
- Generated frames need a decent base frame rate: about 40 fps or more looks good.
- The bundled `u59-preset.json` turns on `KYTY_CUDA_EARLY_INIT` (CUDA set up before the
  emulator's memory): without it an RTX 5080 refused the Vulkan device with Streamline ("Could not
  create device"). In this build CUDA's memory then sits above 512 GiB, inside the emulator's
  address range; Astro Bot does not use that part of it, but other games might. If the device is
  still refused, set Bottom-up ASLR on for kyty_emulator.exe in Windows Security > App & browser
  control > Exploit protection > Program settings.
- A "DLSSG SDK - DO NOT DISTRIBUTE" watermark means the Streamline files came from the SDK's
  `development` folder: use the ones from `bin\x64` instead.

## Reporting problems

Report problems on the fork's [Issues page](https://github.com/biggsymobile-png/KytyPS5/issues/new?template=bug_report.yml):
the form asks for the build, game version, GPU and settings, and the console log. Please include your GPU, driver version, game and version, and all the `Frame generation:`,
`DLSS upscaling:`, `FSR upscaling:`, `Game patch:` and `Streamline` lines from the console (or the
lines just before a crash).

## How it works

The DLSS-G integration through NVIDIA Streamline (loading `sl.interposer.dll` as the Vulkan loader,
DLSS-G options, constants, resource tags and Reflex/PCL markers at the present) follows
**chenxiao07/KytyPS5**'s `src/local/frame-gen.cpp`, which does this for Demon's Souls, and the
Linux/macOS FSR build uses the FidelityFX source integration from chenxiao07's work too - many
thanks. FSR 3.1 upscaling and frame generation use AMD's FidelityFX SDK 1.1.4 (MIT licence). Astro
Bot's and Playroom's motion vectors, depth and camera constants were found with a capture tool in
this build (`KYTY_FG_DISCOVER=1`); the details are in
[docs/RE-ASTRO-BOT-FG.md](https://github.com/biggsymobile-png/KytyPS5/blob/fg-discovery/docs/RE-ASTRO-BOT-FG.md), the code in
`src/graphics/host_gpu/renderer/frameGenDlssg.{h,cpp}`.

KytyPS5 is free software under the GPL-2.0 (see LICENSE). NVIDIA Streamline and DLSS are NVIDIA's;
their files are not distributed here. AMD FidelityFX is AMD's, under the MIT licence. This build is
not affiliated with Sony, NVIDIA, AMD, the KytyPS5 project or Jetsku.
