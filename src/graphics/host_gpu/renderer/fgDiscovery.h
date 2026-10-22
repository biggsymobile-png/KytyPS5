#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FG_DISCOVERY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FG_DISCOVERY_H_

// KYTY_FG_DISCOVER=1 (default off): records, for a few pairs of consecutive frames, every draw and
// compute dispatch with its shaders, the images it reads and writes (format, size, guest address)
// and the first dwords of its user data and flattened SRT, into fg-discovery.txt next to the
// emulator. It does nothing else and changes no rendering.
//
// Its purpose is to find, in a game without any frame-generation support, the passes DLSS frame
// generation needs: the depth buffer, the per-pixel motion vectors and the camera matrices (the
// way chenxiao07/KytyPS5 found them in Demon's Souls' TAA dispatch). Pairs of frames are recorded
// so that matrices can be told apart from constants: a camera matrix changes between them.
//
// Frames are counted at completed guest flips (VideoOut), from the start of the emulator.
// KYTY_FG_DISCOVER_START=<frame> (default 3600): the first recorded frame.
// KYTY_FG_DISCOVER_EVERY=<frames> (default 600): frames between the starts of two recordings.
// KYTY_FG_DISCOVER_COUNT=<n> (default 8): recordings, each of 2 consecutive frames.

#include <cstdint>
#include <span>
#include <string>

namespace Libs::Graphics {

struct ShaderStageRuntime;
struct PreparedBindings;
struct RenderColorInfo;
struct RenderDepthInfo;

namespace FgDiscovery {

// A completed guest flip: the next frame.
void OnGuestFlip();

// Whether the current frame is recorded (one relaxed load).
[[nodiscard]] bool Recording();

void OnDispatch(const ShaderStageRuntime& stage, const PreparedBindings& bindings,
                uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, bool indirect);

struct DrawStage {
	char                      kind = '?'; // 'V' vertex-side stage, 'P' pixel
	const ShaderStageRuntime* stage    = nullptr;
	const PreparedBindings*   bindings = nullptr;
};

// Shaders whose code (decoded) and bound buffers (first dwords) are also written: Astro Bot's TAA
// (VS ed17fd5a, PS 2936e5ec) and the vertex shaders that draw most into its motion target.
[[nodiscard]] bool Watched(uint64_t shader_hash);
// The decoded guest code of a watched shader, written once to fg-shader-<hash>.txt.
void OnShaderDecoded(uint64_t shader_hash, const char* stage, const std::string& text);

void OnDraw(std::span<const DrawStage> stages, std::span<const RenderColorInfo> colors,
            const RenderDepthInfo& depth);

} // namespace FgDiscovery
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FG_DISCOVERY_H_
