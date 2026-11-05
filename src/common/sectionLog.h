#ifndef COMMON_INCLUDE_SECTIONLOG_H_
#define COMMON_INCLUDE_SECTIONLOG_H_

#include <cstdint>
#include <filesystem>

// KYTY_SECTION_LOG=<file> (default off; temporary, test builds only): one CSV line per "first time
// this session" event, to find a signal for a stretch of a game (Astro Bot's final space section):
//   t_ms,flip,event,detail
//   t_ms    milliseconds since the log was opened
//   flip    guest flips so far
//   event   open      a guest file opened for the first time (detail: guest path)
//           apr       a file read through APR for the first time (detail: path inside the game)
//           gfx       a graphics pipeline built for the first time (detail: VS hash PS hash)
//           cs        a compute pipeline built for the first time (detail: CS hash)
//           mark      a player action worth a timestamp (detail: e.g. the Decouple key)
// Every line is flushed at once, so the log survives a crash or a closed window.
namespace SectionLog {

[[nodiscard]] bool Enabled();

void NoteFlip();
void NoteFileOpen(const char* guest_path);
void NoteAprFile(const std::filesystem::path& host_path);
void NoteGraphicsPipeline(uint64_t vs_hash, uint64_t ps_hash);
void NoteComputePipeline(uint64_t cs_hash);
void NoteMarker(const char* text);

} // namespace SectionLog

#endif // COMMON_INCLUDE_SECTIONLOG_H_
