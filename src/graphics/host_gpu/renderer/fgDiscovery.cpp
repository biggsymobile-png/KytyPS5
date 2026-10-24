#include "graphics/host_gpu/renderer/fgDiscovery.h"

#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::FgDiscovery {

namespace {

constexpr uint32_t SrtDwords      = 96; // enough for a few 4x4 matrices at the start of an SRT
constexpr uint32_t UserDataDwords = 32;
constexpr uint32_t BufferDwords   = 256; // per bound buffer of a watched shader
constexpr uint32_t WatchedPerFrame = 3;  // draws per watched shader per frame with buffers

constexpr uint64_t WatchedShaders[] = {
    0x2936e5eca99d75e7ull, // TAA pixel shader
    0xed17fd5af4ea9b3full, // TAA vertex shader
    0xe5398a1c6007f356ull, // vertex shaders drawing the most into the motion target
    0x653419a369aa2c03ull, 0x2e0b0b583b5a02f6ull, 0x8e4a146647cf8029ull,
};

struct Settings {
	bool enabled = false;
	int  start   = 3600;
	int  every   = 600;
	int  count   = 8;
};

int EnvInt(const char* name, int fallback) {
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0') return fallback;
	return std::max(0, std::atoi(value));
}

const Settings& GetSettings() {
	static const Settings settings = [] {
		Settings s;
		const char* value = std::getenv("KYTY_FG_DISCOVER");
		s.enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
		s.start   = EnvInt("KYTY_FG_DISCOVER_START", s.start);
		s.every   = std::max(2, EnvInt("KYTY_FG_DISCOVER_EVERY", s.every));
		s.count   = EnvInt("KYTY_FG_DISCOVER_COUNT", s.count);
		return s;
	}();
	return settings;
}

std::mutex g_mutex;
FILE*      g_file       = nullptr;
bool       g_failed     = false;
int        g_last_frame = -1;
std::unordered_map<uint64_t, uint32_t> g_watched_this_frame;

std::atomic<int>  g_frame {0};
std::atomic<bool> g_recording {false};

bool Decide(int frame) {
	const auto& s = GetSettings();
	if (!s.enabled || frame < s.start) return false;
	const int offset = frame - s.start;
	const int window = offset / s.every;
	return window < s.count && offset % s.every < 2;
}

// Opens the file on first use and starts a frame section when the frame changes. g_mutex held.
FILE* Begin(int frame) {
	if (g_failed) return nullptr;
	if (g_file == nullptr) {
		g_file = std::fopen("fg-discovery.txt", "w");
		if (g_file == nullptr) {
			g_failed = true;
			std::printf("FG discovery: could not create fg-discovery.txt\n");
			return nullptr;
		}
		std::fprintf(g_file,
		             "# KytyPS5 frame-generation discovery v1\n"
		             "# C = dispatch, D = draw. img: slot fmt=VkFormat(guest) WxH @address [d]=depth "
		             "[w]=written [s]=storage\n"
		             "# rt = colour target, ds = depth target. ud = user data, srt = flattened SRT "
		             "(hex dwords)\n");
		std::error_code error;
		const auto      path = std::filesystem::absolute("fg-discovery.txt", error);
		std::printf("FG discovery: recording to %s\n",
		            error ? "fg-discovery.txt" : path.string().c_str());
	}
	if (frame != g_last_frame) {
		g_watched_this_frame.clear();
		std::fprintf(g_file, "\n=== frame %d\n", frame);
		std::fflush(g_file);
		g_last_frame = frame;
	}
	return g_file;
}

void PrintImageInfo(FILE* f, const ImageInfo& info) {
	std::fprintf(f, "fmt=%d(%d) %ux%u", static_cast<int>(info.pixel_format),
	             static_cast<int>(info.guest_format), info.extent.width, info.extent.height);
	if (info.extent.depth > 1 || info.resources.layers > 1) {
		std::fprintf(f, "x%u", std::max(info.extent.depth, info.resources.layers));
	}
	std::fprintf(f, " @%" PRIx64, info.data.address);
	if (info.IsDepth()) std::fputs(" [d]", f);
}

void PrintDwords(FILE* f, const char* label, const std::vector<uint32_t>& words, uint32_t limit) {
	if (words.empty()) return;
	const auto count = std::min<size_t>(words.size(), limit);
	std::fprintf(f, "  %s[%zu]", label, words.size());
	for (size_t i = 0; i < count; ++i) {
		std::fprintf(f, "%s%08" PRIx32, i == 0 ? " " : (i % 4 == 0 ? " | " : " "), words[i]);
	}
	std::fputc('\n', f);
}

void PrintStage(FILE* f, char kind, const ShaderStageRuntime& stage, const PreparedBindings& bindings) {
	const auto& program = *stage.program;
	std::fprintf(f, "  %c %016" PRIx64 "\n", kind, program.shader_hash);
	for (size_t i = 0; i < bindings.images.size(); ++i) {
		const auto& image = bindings.images[i];
		std::fprintf(f, "    img%zu ", i);
		PrintImageInfo(f, image.desc.info);
		if (i < program.info.images.size()) {
			if (program.info.images[i].written) std::fputs(" [w]", f);
			if (program.info.images[i].resource_class ==
			    ShaderRecompiler::IR::ImageResourceClass::Storage) {
				std::fputs(" [s]", f);
			}
		}
		std::fputc('\n', f);
	}
	if (stage.resources != nullptr) {
		PrintDwords(f, "  ud", stage.resources->user_data, UserDataDwords);
		PrintDwords(f, "  srt", stage.resources->flattened_srt, SrtDwords);
	}
	if (Watched(program.shader_hash) && g_watched_this_frame[program.shader_hash]++ < WatchedPerFrame) {
		// Guest memory is mapped at the same addresses in the emulator: the buffers as the CPU last
		// wrote them (constants are written by the CPU).
		for (size_t i = 0; i < bindings.buffer_sources.size(); ++i) {
			const auto& source = bindings.buffer_sources[i];
			std::fprintf(f, "    buf%zu @%" PRIx64 " size %" PRIu64 "\n", i, source.address, source.size);
			if (source.address < 0x10000 || source.size < 4) continue;
			const auto  count = std::min<uint64_t>(source.size / 4, BufferDwords);
			const auto* words = reinterpret_cast<const uint32_t*>(source.address);
			for (uint64_t w = 0; w < count; w += 8) {
				std::fprintf(f, "      %04" PRIx64 ":", w * 4);
				for (uint64_t k = w; k < std::min<uint64_t>(w + 8, count); ++k) {
					std::fprintf(f, " %08" PRIx32, words[k]);
				}
				std::fputc('\n', f);
			}
		}
	}
}

} // namespace

void OnGuestFlip() {
	const auto& settings = GetSettings();
	if (!settings.enabled) return;
	const int frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
	if (frame == 1) {
		std::printf("FG discovery: on, recording starts at frame %d (8 snapshots, %d frames apart)\n",
		            settings.start, settings.every);
	}
	const bool recording = Decide(frame);
	if (recording && !g_recording.load(std::memory_order_relaxed)) {
		std::printf("FG discovery: recording frames %d-%d\n", frame, frame + 1);
	}
	if (frame == settings.start + settings.every * std::max(settings.count - 1, 0) + 2) {
		std::printf("FG discovery: finished, you can close the game\n");
	}
	g_recording.store(recording, std::memory_order_relaxed);
}

bool Recording() {
	return g_recording.load(std::memory_order_relaxed);
}

bool Watched(uint64_t shader_hash) {
	if (!GetSettings().enabled) return false;
	return std::find(std::begin(WatchedShaders), std::end(WatchedShaders), shader_hash) !=
	       std::end(WatchedShaders);
}

void OnShaderDecoded(uint64_t shader_hash, const char* stage, const std::string& text) {
	if (!Watched(shader_hash)) return;
	static std::mutex                   mutex;
	static std::unordered_set<uint64_t> written;
	std::lock_guard lock(mutex);
	if (!written.insert(shader_hash).second) return;
	char name[64];
	std::snprintf(name, sizeof(name), "fg-shader-%016" PRIx64 ".txt", shader_hash);
	if (FILE* f = std::fopen(name, "w")) {
		std::fprintf(f, "# %s shader %016" PRIx64 "\n", stage != nullptr ? stage : "", shader_hash);
		std::fputs(text.c_str(), f);
		std::fclose(f);
		std::printf("FG discovery: wrote %s\n", name);
	}
}

void OnDispatch(const ShaderStageRuntime& stage, const PreparedBindings& bindings,
                uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, bool indirect) {
	if (!stage || !Recording()) return;
	std::lock_guard lock(g_mutex);
	FILE* f = Begin(g_frame.load(std::memory_order_relaxed));
	if (f == nullptr) return;
	if (indirect) {
		std::fputs("C indirect\n", f);
	} else {
		std::fprintf(f, "C %ux%ux%u\n", groups_x, groups_y, groups_z);
	}
	PrintStage(f, 'C', stage, bindings);
}

void OnDraw(std::span<const DrawStage> stages, std::span<const RenderColorInfo> colors,
            const RenderDepthInfo& depth) {
	if (!Recording()) return;
	std::lock_guard lock(g_mutex);
	FILE* f = Begin(g_frame.load(std::memory_order_relaxed));
	if (f == nullptr) return;
	std::fputs("D\n", f);
	for (size_t i = 0; i < colors.size(); ++i) {
		if (!colors[i].image_id) continue;
		std::fprintf(f, "  rt%zu ", i);
		PrintImageInfo(f, colors[i].desc.info);
		std::fputc('\n', f);
	}
	if (!depth.desc.info.data.Empty()) {
		std::fputs("  ds ", f);
		PrintImageInfo(f, depth.desc.info);
		std::fprintf(f, " test=%d write=%d\n", depth.depth_test_enable ? 1 : 0,
		             depth.depth_write_enable ? 1 : 0);
	}
	for (const auto& entry : stages) {
		if (entry.stage == nullptr || entry.bindings == nullptr || !*entry.stage) continue;
		PrintStage(f, entry.kind, *entry.stage, *entry.bindings);
	}
}

} // namespace Libs::Graphics::FgDiscovery
