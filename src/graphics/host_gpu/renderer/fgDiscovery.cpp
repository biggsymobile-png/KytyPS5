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

namespace Libs::Graphics::FgDiscovery {

namespace {

constexpr uint32_t SrtDwords      = 96; // enough for a few 4x4 matrices at the start of an SRT
constexpr uint32_t UserDataDwords = 16;

struct Settings {
	bool enabled = false;
	int  start   = 1200;
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

std::atomic<int>  g_cached_frame {-1};
std::atomic<bool> g_cached_value {false};

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
}

} // namespace

bool Recording(int frame) {
	if (!GetSettings().enabled) return false;
	if (g_cached_frame.load(std::memory_order_relaxed) == frame) {
		return g_cached_value.load(std::memory_order_relaxed);
	}
	const bool value = Decide(frame);
	g_cached_value.store(value, std::memory_order_relaxed);
	g_cached_frame.store(frame, std::memory_order_relaxed);
	return value;
}

void OnDispatch(int frame, const ShaderStageRuntime& stage, const PreparedBindings& bindings,
                uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, bool indirect) {
	if (!stage || !Recording(frame)) return;
	std::lock_guard lock(g_mutex);
	FILE* f = Begin(frame);
	if (f == nullptr) return;
	if (indirect) {
		std::fputs("C indirect\n", f);
	} else {
		std::fprintf(f, "C %ux%ux%u\n", groups_x, groups_y, groups_z);
	}
	PrintStage(f, 'C', stage, bindings);
}

void OnDraw(int frame, std::span<const DrawStage> stages, std::span<const RenderColorInfo> colors,
            const RenderDepthInfo& depth) {
	if (!Recording(frame)) return;
	std::lock_guard lock(g_mutex);
	FILE* f = Begin(frame);
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
