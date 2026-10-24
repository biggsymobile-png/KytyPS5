#ifndef KYTY_LOADER_ASTRO_DLSS_OUTPUT_H_
#define KYTY_LOADER_ASTRO_DLSS_OUTPUT_H_

#include "common/dlssSrSettings.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Loader::AstroDlssOutput {

// The scene preset remains untouched. Only its separate post-processing grid
// changes: presets 0/1 normally use 2432x1368, presets 2..4 use 3840x2160.
// These instruction sequences were identified in PPSA21567 01.018.000.
inline constexpr std::array<uint8_t, 26> ResourceSignature {
    0x89, 0x99, 0x48, 0x01, 0x00, 0x00, 0x89, 0x81, 0x4c, 0x01, 0x00, 0x00, 0x0f,
    0x9c, 0x81, 0x50, 0x01, 0x00, 0x00, 0x44, 0x89, 0xa1, 0x54, 0x01, 0x00, 0x00};
inline constexpr std::array<uint8_t, 64> CameraSignature {
    0x41, 0xba, 0x58, 0x05, 0x00, 0x00, 0x4c, 0x0f, 0xaf, 0xe8, 0xb8, 0x70, 0x08, 0x00, 0x00, 0x49,
    0xc1, 0xe9, 0x21, 0x49, 0xc1, 0xe8, 0x21, 0x48, 0xc1, 0xef, 0x21, 0xc5, 0xf3, 0x58, 0xc6, 0xc4,
    0xc1, 0x39, 0xc6, 0xc8, 0x01, 0x49, 0xc1, 0xed, 0x21, 0x41, 0x83, 0xfb, 0x02, 0x41, 0xbb, 0x80,
    0x09, 0x00, 0x00, 0x44, 0x0f, 0x4d, 0xd0, 0xb8, 0x00, 0x0f, 0x00, 0x00, 0x44, 0x0f, 0x4d, 0xd8};

inline bool Requested(std::string_view mode, std::string_view output) {
	const bool sr = Common::ParseDlssSrRequest(mode) == Common::DlssSrRequest::Automatic;
	return sr && (output.empty() || output == "4k" || output == "3840x2160");
}

enum class Result { Applied, MissingSignature, AmbiguousSignature };

// Validate both complete sequences, including uniqueness, before modifying any
// byte. No offsets, code caves, guest allocations, or on-disk executable writes.
inline Result Apply(std::span<uint8_t> code) {
	const auto locate = [&](auto& signature, bool& ambiguous) {
		const auto found =
		    std::search(code.begin(), code.end(), signature.begin(), signature.end());
		if (found != code.end() &&
		    std::search(found + 1, code.end(), signature.begin(), signature.end()) != code.end()) {
			ambiguous = true;
		}
		return found;
	};
	bool       ambiguous = false;
	const auto resource  = locate(ResourceSignature, ambiguous);
	const auto camera    = locate(CameraSignature, ambiguous);
	if (ambiguous) return Result::AmbiguousSignature;
	if (resource == code.end() || camera == code.end()) return Result::MissingSignature;

	// setl byte [rcx+0x150] -> mov byte [rcx+0x150],0 (same length,
	// preserves flags). The width/height at +0x148/+0x14c and preset +0x154
	// still describe the original scene. All consumers of the low-output flag
	// now allocate/select the game's existing 4K post-processing resources.
	constexpr std::array<uint8_t, 7> high_output {0xc6, 0x81, 0x50, 0x01, 0x00, 0x00, 0x00};
	std::copy(high_output.begin(), high_output.end(), resource + 12);
	// The camera/viewport calculation independently derives the output grid
	// from the preset. Keep it in agreement with those resources, including
	// preset 0/1; its high-preset cmov values already specify 4K.
	camera[2]  = 0x70;
	camera[3]  = 0x08;
	camera[47] = 0x00;
	camera[48] = 0x0f;
	return Result::Applied;
}

} // namespace Loader::AstroDlssOutput

#endif
