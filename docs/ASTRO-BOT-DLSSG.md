# KytyPS5 (Jetsku U59) with DLSS frame generation for Astro Bot

An **experimental, unofficial** test build of [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5)
that adds NVIDIA **DLSS frame generation (DLSS-G)** and, new and experimental, **DLSS upscaling
(DLSS super resolution)** to **ASTRO BOT (PPSA21564)**. Everything else
is Jetsku's U59 int21 release (24 October 2026).

Frame generation only switches on where it finds Astro Bot's inputs; in other games it stays
off. With the `streamline` folder present Streamline still sits between the emulator and the
driver, so if another game misbehaves with this build, set `KYTY_FRAMEGEN=0` or remove the folder.

## Requirements

- Windows 10/11 with **hardware-accelerated GPU scheduling** on (Settings > System > Display >
  Graphics > Advanced graphics settings; restart after changing it).
- An **NVIDIA RTX 40 or RTX 50** graphics card and a recent driver (tested: RTX 5080, 617.42).
- NVIDIA's **Streamline v2.14.1** runtime files (not included, see below).

## Setup

1. Download this release's zip and unzip it into a new folder.
2. Download NVIDIA Streamline **v2.14.1**:
   https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1 - pick
   `streamline-sdk-v2.14.1.zip` (not the aarch64/arm64ec ones).
3. In the unzipped emulator folder, next to `kyty_emulator.exe`, make a folder named exactly
   `streamline` and copy into it the files from the Streamline zip's `bin\x64` folder (the files
   themselves; the `development` subfolder is not needed).

   ```
   KytyPS5-AstroBot-DLSSG\
   ├── kyty_emulator.exe
   ├── kyty_launcher.exe
   ├── ...
   └── streamline\
       ├── sl.interposer.dll
       ├── sl.common.dll
       ├── sl.dlss_g.dll
       ├── sl.reflex.dll
       ├── sl.pcl.dll
       ├── nvngx_dlssg.dll
       └── ...
   ```
4. Turn **NVIDIA Smooth Motion off** for kyty_emulator.exe in the NVIDIA App (the two clash).
5. Start Astro Bot from the launcher as usual.

## Checking it works

The emulator's console window shows, in order:

```
Frame generation: Streamline loaded from ...\streamline (1 generated frame(s) per frame)
Frame generation: Streamline offers DLSS-G with this GPU, driver and Windows
Frame generation: Vulkan is loaded from sl.interposer.dll (Streamline's interposer, as needed)
Frame generation: DLSS-G ready (1 generated frame(s) per frame); it turns on in the game's 3D view
Frame generation: Astro Bot's inputs found (motion and depth 3840x2160, ...)
Frame generation: DLSS-G on
```

Use the **NVIDIA overlay** (Alt+R) to see the frame rate on screen: it counts the generated
frames. The emulator's own counter shows the game's real frame rate, which DLSS-G does not
change.

## Settings

In the launcher, a game's **Graphics & display** settings have **Frame generation** (Off, 2x, 3x,
4x; 3x and 4x need an RTX 50 card), **Upscaling** (the game's own TAA, NVIDIA DLSS or AMD FSR 3.1) and
**Render resolution** (Astro Bot's; lower is faster). They take precedence over
`u59-preset.json`.

This build is made like Jetsku's U59 releases (profile-guided optimisation trained on Astro Bot,
the emulator's diagnostics compiled out) and ships Jetsku's tuned **`u59-preset.json`** next to
the launcher. While that file is there, the launcher **ignores `KYTY_...` environment variables**
and uses the file instead: add settings to it, one `"NAME": "value",` line each, for example
`"KYTY_FRAMEGEN": "2",` or `"KYTY_VRAM_BUDGET_MB": "12000",`.


| Variable | Effect |
|---|---|
| `KYTY_FRAMEGEN=0` | Frame generation off |
| `KYTY_FRAMEGEN=1` | 2x (default) |
| `KYTY_FRAMEGEN=2` / `3` | 3x / 4x multi frame generation (RTX 50 only) |
| `KYTY_DLSS=1` | DLSS upscaling in place of the game's anti-aliasing (TAA), see below (launcher: Upscaling) |
| `KYTY_ASTRO_RENDER_RES=1080` | Astro Bot's render resolution: 1080, 1368, 1872 or 2160 (launcher: Render resolution) |
| `KYTY_STREAMLINE_DIR=<folder>` | Use Streamline files from another folder |
| `KYTY_VRAM_BUDGET_MB=12000` | Leave video memory for DLSS-G (helps on 16 GB cards at 4K) |

## DLSS upscaling (experimental)

Astro Bot draws its 3D image at a render resolution and its own TAA pass upscales that to the
output resolution. With DLSS upscaling on, NVIDIA's DLSS does that step instead of the game's TAA
(any RTX card), from the render resolution the launcher's **Render resolution** picks:

| Render resolution | DLSS works like |
|---|---|
| 1920x1080 | Performance/Quality (fastest; for lower-end GPUs) |
| 2432x1368 | Quality |
| 3328x1872 | between Ultra Quality and DLAA |
| Game's choice / 3840x2160 | DLAA (native resolution, best image, a little slower than the game's TAA) |

The render resolution needs no patch file: the emulator applies it itself (Astro Bot 01.018, and
other versions where it finds the game's resolution code; the console says which). An enabled
"Fixed render resolution" patch in a patch file takes precedence.

The console shows `DLSS upscaling: 1920x1080 to ...` and `DLSS upscaling: on`, then a status line
every 600 frames. It works with or without frame generation. Your `streamline` folder needs
Streamline's `sl.dlss.dll` and `nvngx_dlss.dll` (both in the SDK's `bin\x64`).

If the image shimmers or looks soft and wobbly, the camera jitter's direction may be wrong: try
`"KYTY_DLSS_JITTER_X": "-1",` and/or `"KYTY_DLSS_JITTER_Y": "-1",`, or
`"KYTY_DLSS_JITTER": "projection",` in `u59-preset.json`; trails behind moving things:
`"KYTY_DLSS_MV_JITTERED": "1",`. Please report which one looked right.

## AMD FSR 3.1 upscaling (experimental)

**Upscaling: AMD FSR 3.1** in the launcher (`KYTY_FSR=1`) does the same as DLSS upscaling with
AMD's FidelityFX Super Resolution 3.1, on **any GPU** (AMD, Intel, older NVIDIA) and without the
Streamline files. Its runtime, `amd_fidelityfx_vk.dll` (FidelityFX SDK v1.1.4, MIT licence), is
included next to the emulator. The console shows `FSR upscaling: 1920x1080 to ...` and
`FSR upscaling: on`. `"KYTY_FSR_SHARPNESS": "0.5",` in `u59-preset.json` adds FSR's sharpening
(0 to 1; default off). The jitter switches of DLSS above apply to FSR too.

## Known issues

- **Game patches are separate and up to you.** Frame generation does not depend on them: it works
  with or without the usual Astro Bot patches (ultrawide, fixed resolution, FPS unlock / decouple
  and so on). Patches that replace the game's renderer or its anti-aliasing could stop it finding
  its inputs; it then just stays off.
- **Tested on version 01.018 (PPSA21564).** Other regions or updates very likely work the same,
  but if the console never shows `Astro Bot's inputs found` in the game's 3D view, your version
  draws differently: please report it with your title ID and version.

- **The HUD can smear slightly** in generated frames: the game image is given to DLSS-G with its
  HUD (no HUD-less image yet).
- **Frame generation does not fix game slowdown.** If the game itself runs below the frame rate
  its patches expect, it still slows down; DLSS-G only adds frames on top of what is rendered.
- Generated frames need a decent base frame rate: about 40 fps or more looks good.
- The bundled `u59-preset.json` turns on `KYTY_CUDA_EARLY_INIT` (CUDA set up before the
  emulator's memory): without it an RTX 5080 refused the Vulkan device with Streamline ("Could not
  create device"). In this build CUDA's memory then sits above 512 GiB, inside the emulator's
  address range; Astro Bot does not use that part of it, but other games might. If the device is
  still refused, set Bottom-up ASLR on for kyty_emulator.exe in Windows Security > App & browser
  control > Exploit protection > Program settings.

## Reporting problems

Please include your GPU, driver version, and all the `Frame generation:` and `Streamline` lines
from the console (or the lines just before a crash).

## How it works

The DLSS-G integration through NVIDIA Streamline (loading `sl.interposer.dll` as the Vulkan loader,
DLSS-G options, constants, resource tags and Reflex/PCL markers at the present) follows
**chenxiao07/KytyPS5**'s `src/local/frame-gen.cpp`, which does this for Demon's Souls - many
thanks. Astro Bot's motion vectors, depth and camera constants were found with a capture tool in
this build (`KYTY_FG_DISCOVER=1`); the details are in
[docs/RE-ASTRO-BOT-FG.md](https://github.com/biggsymobile-png/KytyPS5/blob/fg-discovery/docs/RE-ASTRO-BOT-FG.md), the code in
`src/graphics/host_gpu/renderer/frameGenDlssg.{h,cpp}`.

KytyPS5 is free software under the GPL-2.0 (see LICENSE). NVIDIA Streamline and DLSS are NVIDIA's;
their files are not distributed here. This build is not affiliated with Sony, NVIDIA, the KytyPS5
project or Jetsku.
