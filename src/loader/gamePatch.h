#ifndef KYTY_LOADER_GAME_PATCH_H_
#define KYTY_LOADER_GAME_PATCH_H_

#include "common/common.h"

#include <filesystem>
#include <vector>

namespace Loader {

struct Program;

namespace GamePatch {

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs);
bool ApplyPending(Program* program);
// Built-in game fixes that need no patch file (KYTY_ASTRO_CB_SCALE). Main program, before it runs.
void ApplyBuiltin(Program* main_program);
// Before any patch: logs where Astro Bot's fixed-step getters are, for a built-in Decouple
// (KYTY_DECOUPLE_PROBE). Changes nothing.
void ProbeDecouple(Program* main_program);
// Once per guest flip (VideoOut): the built-in bot rescue fix's watchdog (KYTY_ASTRO_RESCUE_FIX).
void OnGuestFlip();
void Clear();

} // namespace GamePatch

} // namespace Loader

#endif // KYTY_LOADER_GAME_PATCH_H_
