#include "common/sectionLog.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

namespace SectionLog {

namespace {

struct State {
	FILE*                                 file = nullptr;
	std::chrono::steady_clock::time_point origin;
	std::mutex                            mutex;
	std::unordered_set<std::string>       opens;
	std::unordered_set<std::string>       apr_files;
};

std::atomic<uint64_t> g_flips {0};

State* GetState() {
	static State* state = []() -> State* {
		const auto* path = std::getenv("KYTY_SECTION_LOG");
		if (path == nullptr || path[0] == '\0') {
			return nullptr;
		}
		FILE* file = std::fopen(path, "w");
		if (file == nullptr) {
			std::printf("Section log: could not open %s (KYTY_SECTION_LOG)\n", path);
			std::fflush(stdout);
			return nullptr;
		}
		auto* created   = new State; // lives until exit
		created->file   = file;
		created->origin = std::chrono::steady_clock::now();
		std::fprintf(file, "t_ms,flip,event,detail\n");
		std::fflush(file);
		std::printf("Section log: on, writing %s (KYTY_SECTION_LOG)\n", path);
		std::fflush(stdout);
		return created;
	}();
	return state;
}

// Called with the mutex held. Commas in the detail would split the column, so they become ';'.
void WriteLine(State& state, const char* event, std::string detail) {
	for (auto& c: detail) {
		if (c == ',' || c == '\n' || c == '\r') c = ';';
	}
	const auto t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                      std::chrono::steady_clock::now() - state.origin)
	                      .count();
	std::fprintf(state.file, "%lld,%" PRIu64 ",%s,%s\n", static_cast<long long>(t_ms),
	             g_flips.load(std::memory_order_relaxed), event, detail.c_str());
	std::fflush(state.file);
}

// The last few components of a host path: the part inside the game's folder, without the
// player's own folders.
std::string ShortPath(const std::filesystem::path& host_path) {
	std::string out;
	int         kept = 0;
	for (auto it = host_path.end(); it != host_path.begin() && kept < 5;) {
		--it;
		const auto part = it->generic_string();
		if (part.empty() || part == "/") continue;
		out = out.empty() ? part : part + "/" + out;
		kept++;
	}
	return out;
}

} // namespace

bool Enabled() {
	return GetState() != nullptr;
}

void NoteFlip() {
	if (GetState() != nullptr) {
		g_flips.fetch_add(1, std::memory_order_relaxed);
	}
}

void NoteFileOpen(const char* guest_path) {
	auto* state = GetState();
	if (state == nullptr || guest_path == nullptr) return;
	std::scoped_lock lock(state->mutex);
	if (state->opens.emplace(guest_path).second) {
		WriteLine(*state, "open", guest_path);
	}
}

void NoteAprFile(const std::filesystem::path& host_path) {
	auto* state = GetState();
	if (state == nullptr) return;
	auto             key = host_path.generic_string();
	std::scoped_lock lock(state->mutex);
	if (state->apr_files.emplace(std::move(key)).second) {
		WriteLine(*state, "apr", ShortPath(host_path));
	}
}

void NoteGraphicsPipeline(uint64_t vs_hash, uint64_t ps_hash) {
	auto* state = GetState();
	if (state == nullptr) return;
	char text[64];
	std::snprintf(text, sizeof(text), "%016" PRIx64 " %016" PRIx64, vs_hash, ps_hash);
	std::scoped_lock lock(state->mutex);
	WriteLine(*state, "gfx", text);
}

void NoteComputePipeline(uint64_t cs_hash) {
	auto* state = GetState();
	if (state == nullptr) return;
	char text[32];
	std::snprintf(text, sizeof(text), "%016" PRIx64, cs_hash);
	std::scoped_lock lock(state->mutex);
	WriteLine(*state, "cs", text);
}

void NoteMarker(const char* text) {
	auto* state = GetState();
	if (state == nullptr || text == nullptr) return;
	std::scoped_lock lock(state->mutex);
	WriteLine(*state, "mark", text);
}

} // namespace SectionLog
