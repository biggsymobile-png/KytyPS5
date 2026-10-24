# Astro Bot DLSS Super Resolution and DLAA

Set `KYTY_DLSS_SR=on` to enable it or `off` to disable it (the default).
`1` and `0` are also accepted. Frame generation is
independent (`KYTY_FRAMEGEN=0` disables it).

## Fixed 4K SR output

SR modes now default to **3840x2160 output**, independently of the scene's
resolution patch. With the 1920x1080 scene patch, use:

```json
"KYTY_DLSS_SR": "on",
"KYTY_DLSS_SR_OUTPUT": "4k"
```

Add those entries to the launcher's `u59-preset.json` when that file is in use;
otherwise set them as environment variables before launching. `4k` is the
default; `3840x2160` is also accepted. Select the 3840x2160 window/display size
in the launcher to present the resulting image at native 4K. Window size alone
does not select the game's TAA or DLSS output resolution.

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

The old `quality`, `balanced`, `performance`, and `ultra-performance` values remain
accepted as aliases for `on`; they no longer constrain the SDK mode. These labels
never changed the game's input resolution.

To restore the game's output choice, set `KYTY_DLSS_SR_OUTPUT=game`. Unknown
values also leave the executable unchanged. The legacy `KYTY_DLSS_SR=dlaa`
setting explicitly retains the original game grid and requires equal input/output
dimensions; it bypasses the 4K output override. SR off also leaves the original resolution path intact.

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
DLSS SR: patched Astro Bot post-processing output to 3840x2160; scene resolution preset unchanged
DLSS SR: TAA constants: render=3840x2160
DLSS SR: active 1920x1080 -> 3840x2160
```

Game TAA still executes first to maintain fallback history. If DLSS is unavailable,
the patched game continues using its 4K TAA path, which costs more than its original
1368p path. Use `KYTY_DLSS_SR_OUTPUT=game` to opt out. Restart the game after changing
either the SR mode or output policy: resources and the in-memory patch are selected
at startup.

If evaluation reports `eWarnOutOfVRAM` (39), the runtime could not obtain enough
GPU memory. `KYTY_VRAM_BUDGET_MB` caps the renderer's memory budget so it can leave
room for Streamline; avoid running multiple game instances when checking this.

Projection jitter remains measured on the scene's input grid. Temporal history
resets after skipped evaluations, camera cuts, frame discontinuities, swapchain
changes, or input/output dimension changes.
