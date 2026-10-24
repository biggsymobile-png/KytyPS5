#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_ASTRO_TAA_GEOMETRY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_ASTRO_TAA_GEOMETRY_H_

#include <array>
#include <cmath>
#include <cstdint>

namespace Libs::Graphics::FrameGen {

struct TaaSize {
	uint32_t width, height;
};

struct TaaJitter {
	float x = 0.0f, y = 0.0f;
};

enum class TaaGeometryStatus { Valid, InvalidExtent, InvalidJitter };

// Astro Bot's TAA constants describe its output/history grid, which can differ
// from the current lit-colour and motion grid under a resolution patch or DRS.
// Derive pixel-space jitter from the scene projection on the input grid, using
// the same convention as the existing frame-generation constants.
inline TaaGeometryStatus ResolveAstroTaaGeometry(const std::array<uint32_t, 20>& constants,
                                                 const std::array<float, 16>&    projection,
                                                 TaaSize input, TaaSize output, TaaJitter& jitter) {
	if (input.width == 0 || input.height == 0 || output.width == 0 || output.height == 0 ||
	    constants[4] != output.width || constants[5] != output.height) {
		return TaaGeometryStatus::InvalidExtent;
	}
	jitter.x = projection[8] * static_cast<float>(input.width) * 0.5f;
	jitter.y = -projection[9] * static_cast<float>(input.height) * 0.5f;
	if (!std::isfinite(jitter.x) || !std::isfinite(jitter.y) || std::fabs(jitter.x) > 2.0f ||
	    std::fabs(jitter.y) > 2.0f) {
		return TaaGeometryStatus::InvalidJitter;
	}
	return TaaGeometryStatus::Valid;
}

} // namespace Libs::Graphics::FrameGen

#endif
