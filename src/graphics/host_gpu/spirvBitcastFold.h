#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_SPIRVBITCASTFOLD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_SPIRVBITCASTFOLD_H_

#include <cstdint>
#include <span>
#include <vector>

// Redundant OpBitcast removal in a module given to the driver (KYTY_SPIRV_BITCAST_FOLD, default 1).
//
// The recompiler keeps guest registers as 32-bit integers and its emitter helpers reinterpret
// values at every float or integer use, so about a quarter of the instructions in Astro Bot's
// translated shaders are OpBitcast, and more than half of those are redundant (offline, 536
// permutations of its shader journal):
// - a bitcast of a bitcast back to the original type (u32 -> f32 -> u32): the original value;
// - a second bitcast of the same value to the same type in the same block: the first one.
// Fold() removes both kinds and renames their uses; the bits every instruction sees are the same,
// so the module computes exactly what it did. Decorations and names of removed ids are dropped.
// Like SpirvLocalArrays, only the module given to the driver changes (the program cache keeps the
// emitted words): less for the driver to parse and optimise.
namespace Libs::Graphics::SpirvBitcastFold {

struct Result {
	bool     changed      = false;
	uint32_t removed      = 0; // OpBitcast instructions removed
	uint32_t roundtrips   = 0; // of them, bitcasts back to the original type
	uint32_t instructions = 0; // instructions in the module before
};

// `out` receives the rewritten module when Result::changed, else it is left empty. A module the
// parser rejects is left unchanged.
Result Fold(std::span<const uint32_t> module, std::vector<uint32_t>& out);

} // namespace Libs::Graphics::SpirvBitcastFold

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_SPIRVBITCASTFOLD_H_ */
