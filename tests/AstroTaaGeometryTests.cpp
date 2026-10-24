// Replays the reported 1080p scene / 1368p TAA configuration without Vulkan or
// Streamline. The scene projection supplies jitter on the scene's input grid.
#include "graphics/host_gpu/renderer/astroTaaGeometry.h"

#include <bit>
#include <cstdio>
#include <limits>

using namespace Libs::Graphics::FrameGen;

int main() {
  int failures = 0;
  const auto check = [&](bool value, const char *message) {
    if (!value) {
      std::printf("AstroTaaGeometryTests: failed: %s\n", message);
      ++failures;
    }
  };
  std::array<uint32_t, 20> constants{};
  constants[4] = 2432;
  constants[5] = 1368;
  // The legacy implementation scaled these output-grid UV offsets by the input
  // dimensions. Use a different input projection phase to detect that mistake.
  constants[0] = std::bit_cast<uint32_t>(0.25f / 2432.0f);
  constants[1] = std::bit_cast<uint32_t>(-0.25f / 1368.0f);
  std::array<float, 16> projection{};
  projection[8] = 2.0f * 0.25f / 1920.0f;
  projection[9] = -2.0f * -0.25f / 1080.0f;
  TaaJitter jitter;
  check(ResolveAstroTaaGeometry(constants, projection, {1920, 1080},
                                {2432, 1368},
                                jitter) == TaaGeometryStatus::Valid,
        "reported 1080p -> 1368p geometry must be accepted");
  check(std::fabs(jitter.x - 0.25f) < 1e-6f &&
            std::fabs(jitter.y + 0.25f) < 1e-6f,
        "jitter must retain its quarter-pixel input-grid phase");
  check(ResolveAstroTaaGeometry(constants, projection, {1920, 1080},
                                {3840, 2160},
                                jitter) == TaaGeometryStatus::InvalidExtent,
        "a 4K swapchain must not substitute for the TAA output");
  check(ResolveAstroTaaGeometry(constants, projection, {0, 1080}, {2432, 1368},
                                jitter) == TaaGeometryStatus::InvalidExtent,
        "a missing input grid must be rejected");
  constants[4] = 3840;
  constants[5] = 2160;
  check(ResolveAstroTaaGeometry(constants, projection, {1920, 1080},
                                {3840, 2160}, jitter) == TaaGeometryStatus::Valid,
        "1080p scene with patched 4K TAA output must be accepted");
  check(std::fabs(jitter.x - 0.25f) < 1e-6f &&
            std::fabs(jitter.y + 0.25f) < 1e-6f,
        "4K output must preserve the 1080p projection jitter phase");
  projection[8] = 2.0f * 0.25f / 3840.0f;
  projection[9] = -2.0f * -0.25f / 2160.0f;
  check(ResolveAstroTaaGeometry(constants, projection, {3840, 2160},
                                {3840, 2160},
                                jitter) == TaaGeometryStatus::Valid,
        "native 4K/DLAA geometry must remain valid");
  check(std::fabs(jitter.x - 0.25f) < 1e-6f &&
            std::fabs(jitter.y + 0.25f) < 1e-6f,
        "native-resolution jitter must remain unchanged");
  projection[8] = std::numeric_limits<float>::quiet_NaN();
  check(ResolveAstroTaaGeometry(constants, projection, {3840, 2160},
                                {3840, 2160},
                                jitter) == TaaGeometryStatus::InvalidJitter,
        "non-finite projection jitter must be rejected");
  projection[8] = 1.0f;
  check(ResolveAstroTaaGeometry(constants, projection, {3840, 2160},
                                {3840, 2160},
                                jitter) == TaaGeometryStatus::InvalidJitter,
        "an invalid projection offset must be rejected");
  std::printf("AstroTaaGeometryTests: %s\n",
              failures == 0 ? "passed" : "failed");
  return failures == 0 ? 0 : 1;
}
