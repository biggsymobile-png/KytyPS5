# KytyPS5 (Jetsku U59) with DLSS frame generation for Astro Bot

For Super Resolution, automatic SDK mode selection, and fixed 4K output, see
[Astro Bot DLSS SR setup](ASTRO-BOT-DLSS-SR.md). SR is enabled separately with
`KYTY_DLSS_SR=on`; this page describes frame generation.

An **experimental, unofficial** test build of [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5)
that adds NVIDIA **DLSS frame generation (DLSS-G)** to **ASTRO BOT (PPSA21564)**. Everything else
is Jetsku's emulator as it was on 22 October 2026.

Frame generation only switches on where it finds Astro Bot's inputs; in other games it stays
off. With the `streamline` folder present Streamline still sits between the emulator and the
driver, so if another game misbehaves with this build, set both `KYTY_FRAMEGEN=0`
and `KYTY_DLSS_SR=off`, or remove the folder.

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
| `KYTY_FRAMEGEN_FPS_CAP=40` | Hold the real frame rate to at most 40: perfectly even 25 ms frames with a 120 Hz vblank (use a multiple of the vblank tick: 40 or 60 at 120 Hz) |
| `KYTY_STREAMLINE_DIR=<folder>` | Use Streamline files from another folder |
| `KYTY_VRAM_BUDGET_MB=12000` | Leave video memory for DLSS-G (helps on 16 GB cards at 4K) |
| `KYTY_SPIRV_BITCAST_FOLD=0` | Give the driver shaders without the redundant-conversion cleanup (exact either way) |
| `KYTY_FLOAT_PROFILE=relaxed` | Simpler shader maths (see below); `precise` is the default |

**Shader maths profile (`KYTY_FLOAT_PROFILE`, experimental):** `precise` (the default) copies the
PS5 GPU's exact floating-point rules, which takes several instructions for things like min/max with
NaNs, tiny (denormal) numbers and rounding modes. `relaxed` maps those to the PC GPU's own
instructions instead: smaller shaders that compile and run a little faster, which can mean less
stutter when new shaders appear. Most games never notice; if you see sparkles, black or white
pixels, flicker or broken effects with it, go back to `precise`. The first start after changing it
compiles the game's shaders again.

**Smoother frames (recommended):** in the launcher's game settings set **Vblank frequency** to
`120`. At the default 60, a frame that misses the 16.7 ms tick by a little is held to 33 ms; at 120
it waits for the next 8.3 ms tick instead (Astro Bot's start: frames at 33 ms or more went from 18%
to under 1%).

**Game threads reading GPU results:** the bundled preset turns on two of Jetsku's experimental
switches, `KYTY_READ_FAULT_PUBLISHED` (simple reads of GPU-written data use the last copy the
emulator made, at most two frames old, instead of waiting for the GPU) and
`KYTY_READBACK_FLUSH_SIDE`. In Astro Bot's first level the copy matched the fresh data for every
read checked, and the frame rate went from about 50 to 54.5 fps at a 60 Hz vblank. Set either to
`"0"` if a game misbehaves.

The relaxed shader profile (`KYTY_FLOAT_PROFILE=relaxed`) is optional and less tested: use
`precise` (the default) if you see crashes or broken effects.

It also sets `KYTY_FAULT_AHEAD_ADAPT=512`, which halves the emulator's write faults (on an
i7-13700K: 6.3 to 5.6 ms per frame). `KYTY_READ_FAULT_LEND` (lets copy loops read GPU-written
pages without waiting) is in the build but off: it removed the waits but Astro Bot ran slightly
slower with it.

**Choppy frame pacing:** in the launcher's game settings set **Present mode** to `Fifo` (or
`Immediate` on a G-Sync/FreeSync display); `Mailbox` throws frames away, generated ones included.
If it is still uneven, cap the real frame rate at a multiple of the vblank tick, e.g.
`"KYTY_FRAMEGEN_FPS_CAP": "40",` with a 120 Hz vblank (a cap of 50 falls between ticks and alternates
16.7 and 25 ms frames).

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
