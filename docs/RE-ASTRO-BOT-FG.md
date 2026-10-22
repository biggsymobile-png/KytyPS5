# Astro Bot (PPSA21564): inputs for DLSS frame generation

Found with the fg-discovery logger (src/graphics/host_gpu/renderer/fgDiscovery.h) on 2026-10-22,
Sky Garden, RTX 5080, internal render 3840x2160 (dynamic resolution not seen in these captures).

## TAA pass (one per frame, menus and gameplay)

Full-screen draw: VS `ed17fd5af4ea9b3f`, PS `2936e5eca99d75e7`.

- Outputs: rt0 = TAA result (RGBA16F 3840x2160), rt1 = new history (ping-pongs between two
  RGBA16F 3840x2160 images every frame).
- Images, in order of first use in the shader: img0 = **motion vectors**, img1 = current lit
  colour (sampled 3x3), img2 = previous history.
- Constant buffer (80 bytes), dwords:
  - 0..1: jitter offset in UV, added to the pixel's UV before sampling (e.g. +0.25 / -0.25 px)
  - 2..3: UV offset subtracted together with the motion vector for the history lookup
  - 4..5: render width / height (uint), 6..7 the same, 8..9 width-1 / height-1
  - 11: frame counter
  - 13: motion vectors used (1) or not (0)
  - 14: history blend (0.8), 17..19 other tuning constants

## Motion vectors

- RGBA16F 3840x2160, the second colour target of the G-buffer draws (and of transparent draws).
- **.xy, in UV units, current minus previous**: the TAA reads history at `uv - mv - cb[2..3]`.
  So `prev_uv = uv - mv`: for Streamline (prev = cur + mvec * mvecScale) mvecScale = (-1, -1).
- Camera and object motion are both included (objects write them in their own draws).

## Depth

- D32_SFLOAT_S8 3840x2160, depth target of the G-buffer draws; read later as R32F.
- Not inverted (see the projection below: OpenGL-style, z/w = -1 at the near plane).

## Camera constants (per-view constant buffer, 576 bytes)

Bound to the G-buffer vertex shaders (`e5398a1c6007f356`: `s[12:15]`, `653419a369aa2c03`: `s[8:11]`).
Column-major 4x4 float matrices, byte offsets:

| Offset | Content |
|---|---|
| 0   | view-projection (jittered) |
| 64  | view (world to camera, right-handed, camera looks down -Z) |
| 128 | projection, jittered: col2 = (jx, jy, -1.00002, -1), col3 = (0, 0, -0.01, 0); near 0.005, far effectively infinite |
| 192 | inverse view (camera to world) |
| 320 | previous frame's view-projection |
| 384 | negated jitter in UV (2 floats) |
| 400 | camera position (3 floats) |
| 416 | 1.00002, -0.01, 0.01, 500.0 (projection terms; 500 likely the far distance used elsewhere) |
| 432 | render width, height (floats), 2.0 |
| 512 | previous frame's view |

Example (frame 6000): proj[0][0] = 2.77778, proj[1][1] = 4.93828 (16:9, vertical FOV about 23
degrees), jitter -0.25 px in both axes.

Identifying the buffer at run time: a 576-byte buffer of a draw writing the motion-vector target
whose dwords 43 and 47 read -1.0 and 0.0 (projection col2.w and col3.w).

## Using it (frameGenDlssg.h)

Windows, an NVIDIA RTX 40/50 GPU, hardware-accelerated GPU scheduling on.

1. Download NVIDIA Streamline **v2.14.1** (the version the build's headers are from):
   https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1
2. Copy the files of its `bin\x64` folder into a folder named `streamline` next to
   kyty_emulator.exe (or point KYTY_STREAMLINE_DIR at it).
3. Start the game. The console shows `Frame generation: DLSS-G ready`, then
   `Astro Bot's inputs found` and `DLSS-G on` once the 3D view runs.

KYTY_FRAMEGEN=0 turns it off; 2 or 3 asks for multi frame generation (RTX 50). Turn NVIDIA Smooth
Motion off for kyty_emulator.exe while using it. The HUD is not separated yet (no HUD-less
image), so it can smear slightly in generated frames.
