# Astro Bot DLSS Super Resolution and DLAA

Set `KYTY_DLSS_SR=on` to enable it or `off` to disable it (the default).
`1` and `0` are also accepted. Frame generation is
independent (`KYTY_FRAMEGEN=0` disables it).

## Enable DLSS SR

1. Use a Windows emulator build compiled with Streamline headers and an NVIDIA
   RTX GPU supported by the DLSS SR runtime. This integration uses Streamline
   v2.14.1 ([download and folder layout](ASTRO-BOT-DLSSG.md#setup)). Copy the
   runtime's complete `bin/x64` contents into a `streamline`
   folder next to `kyty_emulator.exe`, including `sl.interposer.dll`,
   `sl.common.dll`, `sl.dlss.dll`, and `nvngx_dlss.dll`. Alternatively point
   `KYTY_STREAMLINE_DIR` at the runtime folder. Frame-generation files alone
   do not provide Super Resolution.
2. Choose the Astro Bot resolution patch in the launcher's game-patch selection
   using the table below. Enable **only one** fixed-resolution patch at a time.
   For SR upscaling, use one of the three patches below 3840x2160. For DLAA,
   leave the resolution patches disabled and use native 4K rendering, or select
   `Fixed render resolution 3840x2160` to lock native resolution. DLSS reads the
   game's actual scene colour, motion, and depth images; it does not set scene
   resolution itself.
3. In `u59-preset.json` next to the launcher, add or update these entries inside
   its existing JSON object, preserving your other settings:

   ```json
   "KYTY_DLSS_SR": "on",
   "KYTY_DLSS_SR_OUTPUT": "4k",
   "KYTY_FRAMEGEN": "0"
   ```

   The output entry is optional because `4k` is the default. `KYTY_FRAMEGEN=0`
   requests SR alone; use your preferred frame-generation value to combine them.
   The launcher uses the preset file when present, so put these settings in that
   file instead of relying on inherited environment variables.
4. Select a 3840x2160 window/display size in the launcher, then restart the game.
   Check for the `ready`, `automatic SDK mode`, and `active` messages shown below.

### Astro Bot resolution patches

With `KYTY_DLSS_SR=on` and the default 4K output:

| Game-patch selection | Scene input | DLSS behavior |
|---|---|---|
| `Fixed render resolution 1920x1080` | 1920x1080 | SR upscaling to 3840x2160. |
| `Fixed render resolution 2432x1368` | 2432x1368 | SR upscaling to 3840x2160. |
| `Fixed render resolution 3328x1872` | 3328x1872 | SR upscaling to 3840x2160, if accepted by the SDK's supported input ranges. |
| `Fixed render resolution 3840x2160` | 3840x2160 | DLAA at native 4K; no upscaling. |
| No fixed-resolution patch, native 4K rendering | 3840x2160 | DLAA at native 4K; no upscaling. |

The same `on` setting enables both SR and automatic DLAA; no SDK quality-mode
setting is needed. Without a fixed-resolution patch, any dynamic-resolution
changes still follow the actual input grid: equal input/output selects DLAA,
while lower input selects a supported SR mode. Use the 3840x2160 patch to keep
DLAA fixed at 4K.

### Direct command-line launch

For direct command-line launch, set environment variables in the same terminal:

```powershell
$env:KYTY_DLSS_SR = "on"
$env:KYTY_FRAMEGEN = "0"
& .\kyty_emulator.exe --game "D:\Games\ASTRO BOT" --game-patch ".\_Patches\PPSA21567.json" --screen-width 3840 --screen-height 2160
```

Replace the example paths with your game and patch file. Enable the desired scene
resolution in that patch file first. To disable SR, change the preset entry or
environment variable to `off` and restart the game; frame generation is independent.

## Accepted settings

| Setting | Behavior |
|---|---|
| `KYTY_DLSS_SR=on` or `1` | Enable SR and choose the SDK mode from actual image dimensions; use DLAA when input and output are equal. |
| `KYTY_DLSS_SR=off`, `0`, empty, or unset | Disable SR (default). |
| `KYTY_DLSS_SR=dlaa` | Retained explicit native-grid mode; requires equal input/output and bypasses the 4K output patch. |
| `KYTY_DLSS_SR_OUTPUT=4k`, `3840x2160`, empty, or unset | With SR enabled via `on`/`1`, apply the guarded 4K game-output patch (default). |
| `KYTY_DLSS_SR_OUTPUT=game` | Retain the game's original output grid while enabling SR. |

`quality`, `balanced`, `performance`, and `ultra-performance` are **not accepted**
as `KYTY_DLSS_SR` values. Replace any of those old values with `on`. Unknown SR
values print `unknown KYTY_DLSS_SR=...; using the game's TAA` and disable SR.
Unknown output-policy values leave the output-grid patch disabled. SDK mode names
in console diagnostics describe the automatic choice, not a user setting.

## Fixed 4K SR output

SR enabled with `on` defaults to **3840x2160 output**, independently of the scene's
resolution patch. A 3840x2160 window/display presents the result at native 4K;
window size alone does not select the game's TAA or DLSS output resolution.

The rendering path is:

```
patched scene resolution (e.g. 1920x1080)
  -> DLSS SR at 3840x2160
  -> game post-processing and HUD composition
  -> presentation
```

The game patch sets scene resolution. The emulator queries the SDK for modes
that support the actual input grid, then chooses the closest recommended input
resolution. For example, 1920x1080 -> 3840x2160 normally selects the SDK's
Performance mode. Changing the game patch changes the input, and the SDK mode
follows automatically. At equal input/output sizes, it selects DLAA. SDK queries
are cached until the dimensions change. If no mode accepts the dimensions,
the game retains TAA and the console reports the unsupported grid.

To restore the game's output choice, set `KYTY_DLSS_SR_OUTPUT=game`. Unknown
values also leave the executable unchanged. The legacy `KYTY_DLSS_SR=dlaa`
setting explicitly retains the original game grid and requires equal input/output
dimensions; it bypasses the 4K output override. SR off also leaves the original
resolution path intact.

## Implementation and compatibility

Astro Bot separates its scene preset from its post-processing grid. Presets 0/1
normally select 2432x1368 post-processing, while presets 2..4 select 3840x2160.
The loader changes the low-output flag in the render-resource constructor and
the matching output-grid dimensions in the camera/viewport calculation. It
preserves the scene dimensions and preset. Consequently the game allocates
its own 4K TAA/history and downstream resources, with consistent constants and
viewports; DLSS writes directly into that existing 4K colour target. No 4K image
is downsampled back into a 1368p target.

The signatures were derived from **PPSA21567, version 01.018.000**. The patch is
restricted to Astro Bot title IDs PPSA21564/PPSA21567 and only applies when both
complete instruction signatures occur exactly once in the executable segment.
Other versions are supported only if both sequences match. Missing or ambiguous
signatures leave all bytes unchanged and log that the game output grid is retained.
This is an in-memory patch; it never modifies `eboot.bin` or the user's cheat file.
It is compiled only in Windows builds with Streamline headers.

Expected console messages with the 1080p scene patch:

```
DLSS SR: ready for Astro Bot's TAA pass (mode=automatic (game input resolution))
DLSS SR: patched Astro Bot post-processing output to 3840x2160; scene resolution preset unchanged
DLSS SR: TAA constants: render=3840x2160
DLSS SR: automatic SDK mode=performance for game input 1920x1080 -> 3840x2160
DLSS SR: active 1920x1080 -> 3840x2160
```

Game TAA still executes first to maintain fallback history. If DLSS is unavailable,
the patched game continues using its 4K TAA path, which costs more than its original
1368p path. Use `KYTY_DLSS_SR_OUTPUT=game` to opt out. Restart the game after changing
either the SR setting or output policy: resources and the in-memory patch are selected
at startup.

If evaluation reports `eWarnOutOfVRAM` (39), the runtime could not obtain enough
GPU memory. `KYTY_VRAM_BUDGET_MB` caps the renderer's memory budget so it can leave
room for Streamline; avoid running multiple game instances when checking this.

Projection jitter remains measured on the scene's input grid. Temporal history
resets after skipped evaluations, camera cuts, frame discontinuities, swapchain
changes, or input/output dimension changes.
