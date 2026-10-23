#include "graphics/host_gpu/renderer/frameGenDlssg.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/astroTaaGeometry.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef KYTY_FRAMEGEN_STREAMLINE
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// (After windows.h.)
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

#include <filesystem>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif

namespace Libs::Graphics::FrameGen {

namespace {

constexpr uint32_t ViewConstantsBytes = 576; // docs/RE-ASTRO-BOT-FG.md

int  g_generate = 0;     // frames to generate per rendered frame (0 = off)
bool              g_super_resolution = false;
std::atomic<bool> g_sr_ready {false};
bool g_loaded   = false; // Streamline loaded (Initialize)
std::atomic<bool> g_ready {false}; // ... and DLSS-G's functions found (OnDevice or the first present)
bool g_device_ok = false;           // DLSS-G supported and loaded on the device (OnDevice)
std::string g_loader_path;          // Streamline's interposer, UTF-8 (VulkanLoaderPath)

// GPU thread: what the current guest frame has drawn so far.
struct Gathered {
	Camera   camera;                         // from a G-buffer draw's per-view constants
	uint64_t gbuffer_motion_address = 0;     // that draw's second colour target
	uint64_t depth_address = 0, depth_size = 0; // and its depth target
	uint32_t depth_width = 0, depth_height = 0;
	ImageId  motion;                         // the TAA pass's motion vectors
	uint64_t motion_address = 0;
	bool     gbuffer = false, taa = false;
};
std::mutex g_mutex;
Gathered   g_gathered;
// The motion vectors' address the TAA pass last read (the same image every frame): the G-buffer
// draws writing it are found by it (it is the fifth colour target of the main G-buffer draws,
// the second of the transparent ones).
std::atomic<uint64_t> g_known_motion_address {0};

float AsFloat(uint32_t word) {
	float value = 0.0f;
	std::memcpy(&value, &word, sizeof(value));
	return value;
}

bool IsDepthFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32Sfloat:
		case vk::Format::eD32SfloatS8Uint:
		case vk::Format::eX8D24UnormPack32: return true;
		default: return false;
	}
}

// Astro Bot's per-view constants (576 bytes): view at byte 64, jittered projection at 128, both
// column-major. A projection has col2.w = -1 and col3.w = 0; a view has col3.w = 1.
bool ReadCamera(const PreparedBindings& bindings, Camera& camera) {
	for (const auto& source: bindings.buffer_sources) {
		if (source.size != ViewConstantsBytes || source.address < 0x10000) continue;
		// Guest memory is mapped at the same addresses in the emulator; the CPU writes these.
		const auto* words = reinterpret_cast<const uint32_t*>(source.address);
		const float p00 = AsFloat(words[32]), p11 = AsFloat(words[37]);
		if (AsFloat(words[43]) != -1.0f || words[47] != 0 || AsFloat(words[31]) != 1.0f ||
		    !(p00 > 0.0f) || !(p11 > 0.0f) || !std::isfinite(p00) || !std::isfinite(p11)) {
			continue;
		}
		for (uint32_t i = 0; i < 16; ++i) {
			camera.view[i]       = AsFloat(words[16 + i]);
			camera.projection[i] = AsFloat(words[32 + i]);
		}
		camera.valid = true;
		return true;
	}
	return false;
}

void Transition(vk::CommandBuffer command, VulkanImage& image, vk::ImageAspectFlags aspect,
                vk::ImageLayout layout, vk::AccessFlags2 access, vk::PipelineStageFlags2 stage) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = image.state.pl_stage;
	barrier.srcAccessMask       = image.state.access_mask;
	barrier.dstStageMask        = stage;
	barrier.dstAccessMask       = access;
	barrier.oldLayout           = image.state.layout;
	barrier.newLayout           = layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image.image;
	barrier.subresourceRange    = {aspect, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state.pl_stage    = stage;
	image.state.access_mask = access;
	image.state.layout      = layout;
}

// (Re)creates `image` as a sampled copy target like `source`.
void Configure(GraphicContext& graphics, VulkanImage& image, vk::ImageView& view,
               const VulkanImage& source) {
	const vk::Extent2D size {source.extent.width, source.extent.height};
	if (image.image != nullptr && image.format == source.format &&
	    image.extent.width == size.width && image.extent.height == size.height) {
		return;
	}
	if (view != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
	}
	if (image.image != nullptr) {
		graphics.DeleteImage(image);
	}
	vk::ImageCreateInfo create {};
	create.imageType     = vk::ImageType::e2D;
	create.extent        = vk::Extent3D {size.width, size.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = source.format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage         = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
	create.sharingMode   = vk::SharingMode::eExclusive;
	create.samples       = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, image)) {
		EXIT("frame generation: cannot allocate a %ux%u input image\n", size.width, size.height);
	}
	vk::ImageViewCreateInfo view_info {};
	view_info.image            = image.image;
	view_info.viewType         = vk::ImageViewType::e2D;
	view_info.format           = source.format;
	view_info.subresourceRange = {IsDepthFormat(source.format) ? vk::ImageAspectFlagBits::eDepth
	                                                           : vk::ImageAspectFlagBits::eColor,
	                              0, 1, 0, 1};
	RequireVulkanSuccess(graphics.device.createImageView(&view_info, nullptr, &view),
	                     "frame generation input view");
}

void CopyInto(vk::CommandBuffer command, Image& source, VulkanImage& target) {
	const auto aspect = IsDepthFormat(source.backing.format) ? vk::ImageAspectFlagBits::eDepth
	                                                         : vk::ImageAspectFlagBits::eColor;
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transition(command, target, aspect, vk::ImageLayout::eTransferDstOptimal,
	           vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eTransfer);
	vk::ImageCopy copy {};
	copy.srcSubresource = {aspect, 0, 0, 1};
	copy.dstSubresource = {aspect, 0, 0, 1};
	copy.extent         = target.extent;
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, target.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
	Transition(command, target, aspect, vk::ImageLayout::eShaderReadOnlyOptimal,
	           vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eAllCommands);
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
// Streamline's feature entry points are not thread safe. SR runs on the GPU thread,
// whereas frame generation runs on the presenter thread.
std::mutex g_streamline_mutex;
// Streamline entry points (sl.interposer.dll, loaded at run time) and feature functions.
struct Api {
	PFun_slInit*               init                 = nullptr;
	PFun_slIsFeatureSupported* is_feature_supported = nullptr;
	PFun_slIsFeatureLoaded*    is_feature_loaded    = nullptr;
	PFun_slGetFeatureFunction* get_feature_function = nullptr;
	PFun_slGetNewFrameToken*   get_new_frame_token  = nullptr;
	PFun_slSetConstants*       set_constants        = nullptr;
	PFun_slSetTagForFrame*     set_tag_for_frame    = nullptr;
	PFun_slEvaluateFeature*        evaluate_feature      = nullptr;
	PFun_slDLSSSetOptions*         dlss_set_options      = nullptr;
	PFun_slDLSSGetOptimalSettings* dlss_optimal_settings = nullptr;
	PFun_slDLSSGSetOptions*    dlssg_set_options    = nullptr;
	PFun_slDLSSGGetState*      dlssg_get_state      = nullptr;
	PFun_slReflexSetOptions*   reflex_set_options   = nullptr;
	PFun_slPCLSetMarker*       pcl_set_marker       = nullptr;
	PFun_slReflexSleep*        reflex_sleep         = nullptr;
};
Api             g_api;
sl::FrameToken* g_token       = nullptr; // present thread: the frame being presented
std::atomic<uint32_t> g_frame_index {0};
sl::DLSSMode          g_sr_mode = sl::DLSSMode::eOff;
const char*           SrModeName() {
    switch (g_sr_mode) {
        case sl::DLSSMode::eMaxQuality: return "quality";
        case sl::DLSSMode::eBalanced: return "balanced";
        case sl::DLSSMode::eMaxPerformance: return "performance";
        case sl::DLSSMode::eUltraPerformance: return "ultra-performance";
        case sl::DLSSMode::eDLAA: return "dlaa";
        default: return "off";
    }
}
bool         g_sr_history_valid = false;
uint32_t     g_sr_last_frame    = 0;
vk::Extent2D g_sr_input {}, g_sr_output {};
// GPU thread: bounded diagnostics, including when no recognized TAA draw executes.
uint64_t g_sr_taa_calls = 0, g_sr_guest_flips = 0;
enum class SrSkip : size_t {
	Bindings,
	Camera,
	CameraValues,
	Constants,
	Images,
	Extent,
	MissingImage,
	Aliasing,
	Format,
	MotionIdentity,
	MotionSize,
	Subresources,
	Jitter,
	OptimalSettings,
	Options,
	Projection,
	View,
	Reprojection,
	ClipPlanes,
	FrameToken,
	SetConstants,
	Count
};
std::array<uint64_t, static_cast<size_t>(SrSkip::Count)> g_sr_skip_counts {};

template <typename... Args>
bool SkipSr(SrSkip reason, const char* format, Args... args) {
	const auto count = ++g_sr_skip_counts[static_cast<size_t>(reason)];
	if (count == 1 || count == 300) {
		std::printf("DLSS SR: skipped: ");
		if constexpr (sizeof...(Args) == 0) {
			std::printf("%s", format);
		} else {
			std::printf(format, args...);
		}
		std::printf(" (occurrences=%" PRIu64 "; keeping game TAA)\n", count);
		std::fflush(stdout);
	}
	return false;
}

void ReportSrLayout(TextureCache& cache, const PreparedBindings* pixel,
                    std::span<const RenderColorInfo> colors) {
	std::array<uint64_t, 36> signature {};
	signature[0]      = pixel != nullptr ? pixel->images.size() : 0;
	signature[1]      = colors.size();
	signature[2]      = pixel != nullptr ? pixel->buffer_sources.size() : 0;
	const auto record = [&](size_t slot, ImageId id, const ImageInfo& guest) {
		if (!id) return;
		const auto& image       = cache.GetImage(id).backing;
		signature[3 + slot * 5] = image.extent.width;
		signature[4 + slot * 5] = image.extent.height;
		signature[5 + slot * 5] = static_cast<uint32_t>(image.format);
		signature[6 + slot * 5] = guest.extent.width;
		signature[7 + slot * 5] = guest.extent.height;
	};
	if (pixel != nullptr) {
		for (size_t i = 0; i < std::min<size_t>(3, pixel->images.size()); ++i)
			record(i, pixel->images[i].image_id, pixel->images[i].desc.info);
		for (size_t i = 0; i < std::min<size_t>(8, pixel->buffer_sources.size()); ++i)
			signature[28 + i] = pixel->buffer_sources[i].size;
	}
	for (size_t i = 0; i < std::min<size_t>(2, colors.size()); ++i)
		record(3 + i, colors[i].image_id, colors[i].desc.info);
	static std::array<uint64_t, 36> previous {};
	static uint32_t                 reports = 0;
	if (signature == previous || reports >= 16) return;
	previous = signature;
	++reports;
	std::printf("DLSS SR: TAA observed: %" PRIu64 " sampled images, %" PRIu64 " colour targets\n",
	            signature[0], signature[1]);
	constexpr const char* names[] {"motion input[0]", "lit colour input[1]", "history input[2]",
	                               "TAA output[0]", "history output[1]"};
	for (size_t i = 0; i < 5; ++i) {
		std::printf("DLSS SR: %s: image=%" PRIu64 "x%" PRIu64 " guest=%" PRIu64 "x%" PRIu64
		            " format=%s\n",
		            names[i], signature[3 + i * 5], signature[4 + i * 5], signature[6 + i * 5],
		            signature[7 + i * 5],
		            vk::to_string(static_cast<vk::Format>(signature[5 + i * 5])).c_str());
	}
	std::printf("DLSS SR: TAA buffer sizes:");
	for (size_t i = 0; i < std::min<uint64_t>(8, signature[2]); ++i)
		std::printf(" %" PRIu64, signature[28 + i]);
	std::printf("\n");
	std::fflush(stdout);
}
bool            g_options_set = false;
vk::Extent2D    g_options_backbuffer {};
vk::Extent2D    g_options_render {};
vk::Format      g_options_format = vk::Format::eUndefined;
uint64_t        g_presents       = 0;
uint64_t        g_with_inputs    = 0;
// On after TurnOnAfter presents in a row with inputs, off after TurnOffAfter without (menus,
// loading), so a fade or a cut does not flap it.
constexpr uint32_t TurnOffAfter = 30, TurnOnAfter = 10;
bool               g_mode_on    = false;
uint32_t           g_run_with = 0, g_run_without = 0;

// Matrices in Streamline's convention: row-major, row vectors (p' = p * M). A column-major
// column-vector matrix in memory reads as exactly that.
using Mat4 = sl::float4x4;
Mat4 g_sr_previous_view_proj;
// The previous presented frame's world-to-clip (unjittered), for clipToPrevClip.
Mat4 g_previous_view_proj;
bool g_previous_valid = false;
// The last presented frame's inputs (owned by its presenter frame, which outlives this pointer:
// DestroyInputs and SwapchainChanging clear it): a present without inputs while DLSS-G is on (a
// repeated or held guest frame) tags them again with zero motion instead of none, which made
// Streamline log "Failed to find global tag" and reset DLSS-G.
const FrameInputs* g_last_inputs = nullptr;

Mat4 FromColumnMajor(const std::array<float, 16>& m) {
	Mat4 r;
	for (uint32_t i = 0; i < 4; ++i) {
		r[i] = sl::float4(m[4 * i], m[4 * i + 1], m[4 * i + 2], m[4 * i + 3]);
	}
	return r;
}

float At(const sl::float4& v, uint32_t i) {
	return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
	Mat4 r;
	for (uint32_t i = 0; i < 4; ++i) {
		float row[4] {};
		for (uint32_t j = 0; j < 4; ++j) {
			for (uint32_t k = 0; k < 4; ++k) row[j] += At(a[i], k) * At(b[k], j);
		}
		r[i] = sl::float4(row[0], row[1], row[2], row[3]);
	}
	return r;
}

bool Inverted(const Mat4& m, Mat4& out) {
	double a[4][8] {};
	for (uint32_t i = 0; i < 4; ++i) {
		for (uint32_t j = 0; j < 4; ++j) a[i][j] = At(m[i], j);
		a[i][4 + i] = 1.0;
	}
	for (uint32_t c = 0; c < 4; ++c) {
		uint32_t pivot = c;
		for (uint32_t r = c + 1; r < 4; ++r) {
			if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
		}
		if (std::fabs(a[pivot][c]) < 1e-12) return false;
		for (uint32_t j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
		const double scale = 1.0 / a[c][c];
		for (uint32_t j = 0; j < 8; ++j) a[c][j] *= scale;
		for (uint32_t r = 0; r < 4; ++r) {
			if (r == c) continue;
			const double f = a[r][c];
			for (uint32_t j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
		}
	}
	for (uint32_t i = 0; i < 4; ++i) {
		out[i] = sl::float4(static_cast<float>(a[i][4]), static_cast<float>(a[i][5]),
		                    static_cast<float>(a[i][6]), static_cast<float>(a[i][7]));
	}
	return true;
}

Mat4 Identity() {
	Mat4 identity;
	for (uint32_t i = 0; i < 4; ++i) {
		identity[i] = sl::float4(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f,
		                         i == 3 ? 1.0f : 0.0f);
	}
	return identity;
}

void LogMessage(sl::LogType type, const char* message) {
	static const bool verbose = std::getenv("KYTY_FRAMEGEN_VERBOSE") != nullptr;
	// (Asked again at the first presents until DLSS-G is set up: its "not initialized yet" shown twice.)
	static std::atomic<uint32_t> not_ready {0};
	if (message != nullptr && std::strstr(message, "has not been initialized yet") != nullptr &&
	    not_ready.fetch_add(1, std::memory_order_relaxed) >= 2 && !verbose) {
		LOGF("Streamline: %s", message);
		return;
	}
	if (type == sl::LogType::eError || verbose) {
		std::printf("Streamline %s: %s", type == sl::LogType::eError ? "error" : "log", message);
		std::fflush(stdout);
	}
	LOGF("Streamline: %s", message);
}

// The file is signed by NVIDIA (Streamline then checks its plugins' signatures itself).
bool IsNvidiaSigned(const std::wstring& path) {
	WINTRUST_FILE_INFO file {};
	file.cbStruct      = sizeof(file);
	file.pcwszFilePath = path.c_str();
	WINTRUST_DATA data {};
	data.cbStruct            = sizeof(data);
	data.dwUIChoice          = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwUnionChoice       = WTD_CHOICE_FILE;
	data.pFile               = &file;
	data.dwStateAction       = WTD_STATEACTION_VERIFY;
	data.dwProvFlags         = WTD_CACHE_ONLY_URL_RETRIEVAL;
	GUID       action        = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	const LONG status        = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
	bool       nvidia        = false;
	if (status == ERROR_SUCCESS) {
		auto* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
		auto* signer   = provider != nullptr ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
		auto* cert     = signer != nullptr ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
		wchar_t name[256] {};
		if (cert != nullptr && cert->pCert != nullptr &&
		    CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256) > 1) {
			nvidia = std::wstring(name) == L"NVIDIA Corporation";
		}
	}
	data.dwStateAction = WTD_STATEACTION_CLOSE;
	(void)WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
	return nvidia;
}

// DLSS-G's, Reflex's and PCL's functions, then DLSS-G ready. `report`: say what is missing.
bool FindFunctions(bool report) {
	if (g_ready.load(std::memory_order_relaxed)) return true;
	if (!g_device_ok) return false;
	const auto find = [&](sl::Feature feature, const char* name, auto*& function) {
		void* pointer = nullptr;
		if (g_api.get_feature_function(feature, name, pointer) != sl::Result::eOk || pointer == nullptr) {
			if (report) {
				std::printf("Frame generation: Streamline has no %s\n", name);
				std::fflush(stdout);
			}
			return false;
		}
		function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(pointer);
		return true;
	};
	if (!find(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_api.dlssg_set_options) ||
	    !find(sl::kFeatureDLSS_G, "slDLSSGGetState", g_api.dlssg_get_state) ||
	    !find(sl::kFeatureReflex, "slReflexSetOptions", g_api.reflex_set_options) ||
	    !find(sl::kFeaturePCL, "slPCLSetMarker", g_api.pcl_set_marker)) {
		return false;
	}
	(void)find(sl::kFeatureReflex, "slReflexSleep", g_api.reflex_sleep);
	// (Required with DLSS-G, whatever the Reflex mode: Streamline's DLSS-G and Reflex guides.)
	sl::ReflexOptions reflex {};
	reflex.mode = sl::ReflexMode::eLowLatency;
	// KYTY_FRAMEGEN_FPS_CAP=<fps>: Reflex holds the rendered (base) frames to at most this rate, so
	// DLSS-G paces evenly spaced frames instead of the emulator's uneven ones. 0 or unset: no cap.
	if (const char* cap = std::getenv("KYTY_FRAMEGEN_FPS_CAP"); cap != nullptr && std::atoi(cap) > 0) {
		reflex.frameLimitUs = static_cast<uint32_t>(1'000'000 / std::atoi(cap));
		std::printf("Frame generation: base frame rate capped at %d fps (KYTY_FRAMEGEN_FPS_CAP)\n", std::atoi(cap));
	}
	if (const auto result = g_api.reflex_set_options(reflex); result != sl::Result::eOk) {
		LOGF("frame generation: slReflexSetOptions failed (%d)\n", static_cast<int>(result));
	}
	g_ready.store(true, std::memory_order_relaxed);
	std::printf("Frame generation: DLSS-G ready (%d generated frame(s) per frame); it turns on in "
	            "the game's 3D view\n",
	            g_generate);
	std::fflush(stdout);
	return true;
}
#endif

} // namespace

void Initialize() {
	const char* value = std::getenv("KYTY_FRAMEGEN");
	g_generate        = value != nullptr && value[0] != '\0' ? std::clamp(std::atoi(value), 0, 3) : 1;
	const char* sr    = std::getenv("KYTY_DLSS_SR");
	g_super_resolution =
	    sr != nullptr && sr[0] != '\0' && std::strcmp(sr, "off") != 0 && std::strcmp(sr, "0") != 0;
	if (g_generate == 0 && !g_super_resolution) {
		return;
	}
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (g_super_resolution) {
		if (std::strcmp(sr, "quality") == 0)
			g_sr_mode = sl::DLSSMode::eMaxQuality;
		else if (std::strcmp(sr, "balanced") == 0)
			g_sr_mode = sl::DLSSMode::eBalanced;
		else if (std::strcmp(sr, "performance") == 0)
			g_sr_mode = sl::DLSSMode::eMaxPerformance;
		else if (std::strcmp(sr, "ultra-performance") == 0)
			g_sr_mode = sl::DLSSMode::eUltraPerformance;
		else if (std::strcmp(sr, "dlaa") == 0)
			g_sr_mode = sl::DLSSMode::eDLAA;
		else {
			std::printf("DLSS SR: unknown KYTY_DLSS_SR=%s; using the game's TAA\n", sr);
			g_super_resolution = false;
		}
	}
	if (g_generate == 0 && !g_super_resolution) return;
	// NVIDIA's Streamline runtime, the user's own copy (the Streamline SDK's bin\x64):
	// KYTY_STREAMLINE_DIR, else a "streamline" folder next to the emulator. A folder may be the
	// SDK's own (its bin\x64 is taken).
	std::vector<std::filesystem::path> candidates;
	wchar_t                            exe[MAX_PATH] {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const auto exe_dir = std::filesystem::path(exe).parent_path();
	if (const wchar_t* env = _wgetenv(L"KYTY_STREAMLINE_DIR"); env != nullptr && *env != L'\0') {
		candidates.emplace_back(env);
	} else {
		candidates.push_back(exe_dir / "streamline");
	}
	std::filesystem::path dir;
	for (const auto& candidate: candidates) {
		for (const auto& folder: {candidate, candidate / "bin" / "x64"}) {
			std::error_code error;
			if (dir.empty() && std::filesystem::exists(folder / "sl.interposer.dll", error)) dir = folder;
		}
	}
	if (dir.empty()) {
		LOGF("frame generation: no sl.interposer.dll\n");
		std::printf("Frame generation: NVIDIA's Streamline runtime was not found (put the Streamline "
		            "SDK's bin\\x64 files in a \"streamline\" folder next to kyty_emulator.exe)\n");
		std::fflush(stdout);
		return;
	}
	const auto interposer = dir / "sl.interposer.dll";
	if (!IsNvidiaSigned(interposer.wstring())) {
		LOGF("frame generation: %s is not signed by NVIDIA: not loaded\n", interposer.string().c_str());
		std::printf("Frame generation: %s is not signed by NVIDIA, not loaded\n",
		            interposer.string().c_str());
		std::fflush(stdout);
		return;
	}
	HMODULE module = LoadLibraryW(interposer.c_str());
	if (module == nullptr) {
		std::printf("Frame generation: cannot load %s\n", interposer.string().c_str());
		std::fflush(stdout);
		return;
	}
	const auto load = [&](auto& function, const char* name) {
		function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
		    reinterpret_cast<void*>(GetProcAddress(module, name)));
		return function != nullptr;
	};
	if (!load(g_api.init, "slInit") || !load(g_api.is_feature_supported, "slIsFeatureSupported") ||
	    !load(g_api.is_feature_loaded, "slIsFeatureLoaded") ||
	    !load(g_api.get_feature_function, "slGetFeatureFunction") ||
	    !load(g_api.get_new_frame_token, "slGetNewFrameToken") ||
	    !load(g_api.set_constants, "slSetConstants") ||
	    !load(g_api.set_tag_for_frame, "slSetTagForFrame")) {
		std::printf("Frame generation: sl.interposer.dll lacks an entry point\n");
		std::fflush(stdout);
		return;
	}
	static const std::wstring    plugin_dir = dir.wstring();
	static const wchar_t*        plugin_paths[] {plugin_dir.c_str()};
	static std::vector<sl::Feature> features;
	if (g_generate > 0) features = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
	if (g_super_resolution) features.push_back(sl::kFeatureDLSS);
	sl::Preferences preferences {};
	preferences.showConsole        = false;
	preferences.logLevel           = std::getenv("KYTY_FRAMEGEN_VERBOSE") != nullptr ? sl::LogLevel::eVerbose
	                                                                                : sl::LogLevel::eDefault;
	preferences.pathsToPlugins     = plugin_paths;
	preferences.numPathsToPlugins  = 1;
	preferences.pathToLogsAndData  = nullptr;
	preferences.logMessageCallback = LogMessage;
	preferences.flags              = sl::PreferenceFlags::eDisableCLStateTracking |
	                    sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	preferences.featuresToLoad    = features.data();
	preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
	preferences.engine            = sl::EngineType::eCustom;
	preferences.engineVersion     = "1.0";
	preferences.projectId         = "6a3f1d5e-4b7c-4f2a-9d8e-1c2b3a4d5e6f";
	preferences.renderAPI         = sl::RenderAPI::eVulkan;
	if (const auto result = g_api.init(preferences, sl::kSDKVersion); result != sl::Result::eOk) {
		std::printf("Frame generation: slInit failed (%d)\n", static_cast<int>(result));
		std::fflush(stdout);
		return;
	}
	// SDL loads the interposer as the Vulkan loader: every instance, device and swapchain call
	// then goes through Streamline, which adds what DLSS-G needs.
	SetEnvironmentVariableW(L"SDL_VULKAN_LIBRARY", interposer.c_str());
	_putenv_s("SDL_VULKAN_LIBRARY", interposer.string().c_str());
	{
		const auto utf8 = interposer.u8string();
		g_loader_path.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
	}
	g_loaded = true;
	LOGF("frame generation: Streamline loaded from %s, %d generated frame(s)\n", dir.string().c_str(),
	     g_generate);
	std::printf("Frame generation: Streamline loaded from %s (%d generated frame(s) per frame)\n",
	            dir.string().c_str(), g_generate);
	std::fflush(stdout);
#else
	LOGF("frame generation: this build has no Streamline support\n");
	if (g_super_resolution) {
		std::printf("DLSS SR: this build has no Streamline support; using the game's TAA\n");
	}
#endif
}

const char* VulkanLoaderPath() {
	return g_loaded && !g_loader_path.empty() ? g_loader_path.c_str() : nullptr;
}

void NoteVulkanLoader(void* get_instance_proc_addr) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_loaded || get_instance_proc_addr == nullptr) return;
	HMODULE module = nullptr;
	wchar_t name[MAX_PATH] {};
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       static_cast<LPCWSTR>(get_instance_proc_addr), &module) &&
	    GetModuleFileNameW(module, name, MAX_PATH) != 0) {
		const auto file = std::filesystem::path(name).filename();
		const bool streamline = _wcsicmp(file.c_str(), L"sl.interposer.dll") == 0;
		std::printf("Frame generation: Vulkan is loaded from %s%s\n", file.string().c_str(),
		            streamline ? " (Streamline's interposer, as needed)"
		                       : " - not Streamline's interposer: DLSS-G cannot start");
		std::fflush(stdout);
	}
#else
	(void)get_instance_proc_addr;
#endif
}

bool Requested() {
	return g_loaded && (g_generate > 0 || g_super_resolution);
}

bool Enabled() {
	return g_ready.load(std::memory_order_relaxed) || g_sr_ready.load(std::memory_order_relaxed);
}

std::string SupportProblem(vk::PhysicalDevice physical_device) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!Requested() || g_generate == 0) return {};
	sl::AdapterInfo adapter {};
	adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
	const auto  result       = g_api.is_feature_supported(sl::kFeatureDLSS_G, adapter);
	std::string problem;
	switch (result) {
		case sl::Result::eOk: break;
		case sl::Result::eErrorOSDisabledHWS:
			problem = "Windows' hardware-accelerated GPU scheduling is off (Settings > System > Display > "
			          "Graphics > Advanced graphics settings; then restart the PC)";
			break;
		case sl::Result::eErrorOSOutOfDate: problem = "this Windows version is too old for DLSS frame generation"; break;
		case sl::Result::eErrorDriverOutOfDate: problem = "the NVIDIA driver is too old for DLSS frame generation"; break;
		case sl::Result::eErrorNoSupportedAdapterFound:
		case sl::Result::eErrorAdapterNotSupported:
			problem = "this graphics card has no DLSS frame generation (NVIDIA RTX 40 or newer)";
			break;
		default:
			problem = std::string("Streamline does not offer DLSS frame generation here (") +
			          sl::getResultAsStr(result) + ")";
			break;
	}
	std::printf("Frame generation: %s\n",
	            problem.empty() ? "Streamline offers DLSS-G with this GPU, driver and Windows" : problem.c_str());
	std::fflush(stdout);
	return problem;
#else
	(void)physical_device;
	return {};
#endif
}

std::string RefusedDeviceAdvice() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	// (chenxiao07/KytyPS5 issue #45: an RTX 5080's driver refused the device with Streamline
	// until Exploit protection forced bottom-up ASLR on for the emulator.)
	PROCESS_MITIGATION_ASLR_POLICY aslr {};
	if (Requested() &&
	    GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof(aslr)) &&
	    aslr.EnableBottomUpRandomization == 0) {
		return " If it keeps failing, in Windows Security: App & browser control > Exploit protection > "
		       "Program settings > add kyty_emulator.exe > Randomize memory allocations (Bottom-up ASLR): "
		       "Override system settings, On. Or set KYTY_FRAMEGEN=0.";
	}
#endif
	return {};
}

void AddDeviceExtensions(const std::vector<vk::ExtensionProperties>& available,
                         std::vector<const char*>&                   extensions) {
	if (!Requested()) return;
	// (Streamline adds what DLSS-G needs; VK_KHR_present_id it relies on for VK_NV_low_latency2.)
	const auto has = [&](const char* name) {
		return std::any_of(available.begin(), available.end(), [&](const vk::ExtensionProperties& item) {
			return std::strcmp(item.extensionName.data(), name) == 0;
		});
	};
	const auto added = [&](const char* name) {
		return std::any_of(extensions.begin(), extensions.end(),
		                   [&](const char* have) { return std::strcmp(have, name) == 0; });
	};
	if (has(VK_KHR_PRESENT_ID_EXTENSION_NAME) && !added(VK_KHR_PRESENT_ID_EXTENSION_NAME)) {
		extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
	}
}

void BypassCommandHooks(vk::Device device) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!Requested() || std::getenv("KYTY_FRAMEGEN_HOOKS") != nullptr) return;
	// The real loader: Streamline itself forwards to it.
	HMODULE loader          = GetModuleHandleW(L"vulkan-1.dll");
	auto*   get_device_proc = loader != nullptr
	                            ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
	                                  reinterpret_cast<void*>(GetProcAddress(loader, "vkGetDeviceProcAddr")))
	                            : nullptr;
	if (get_device_proc == nullptr) return;
	auto&      dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
	const auto native     = static_cast<VkDevice>(device);
	uint32_t   replaced   = 0;
	const auto direct     = [&](auto& function, const char* name) {
		if (auto* pointer = get_device_proc(native, name); pointer != nullptr) {
			function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(pointer);
			++replaced;
		}
	};
	direct(dispatcher.vkBeginCommandBuffer, "vkBeginCommandBuffer");
	direct(dispatcher.vkCmdBindPipeline, "vkCmdBindPipeline");
	direct(dispatcher.vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets");
	direct(dispatcher.vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
	std::printf("Frame generation: %u command-buffer calls bypass Streamline\n", replaced);
	std::fflush(stdout);
#else
	(void)device;
#endif
}

void OnDevice(vk::PhysicalDevice physical_device) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!Requested()) return;
	sl::AdapterInfo adapter {};
	adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
	if (g_super_resolution) {
		bool  sr_loaded   = false;
		void* set_options = nullptr;
		void* optimal     = nullptr;
		if (g_api.is_feature_supported(sl::kFeatureDLSS, adapter) == sl::Result::eOk &&
		    g_api.is_feature_loaded(sl::kFeatureDLSS, sr_loaded) == sl::Result::eOk && sr_loaded &&
		    g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", set_options) ==
		        sl::Result::eOk &&
		    g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", optimal) ==
		        sl::Result::eOk) {
			g_api.dlss_set_options      = reinterpret_cast<PFun_slDLSSSetOptions*>(set_options);
			g_api.dlss_optimal_settings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(optimal);
			HMODULE module              = GetModuleHandleW(L"sl.interposer.dll");
			g_api.evaluate_feature      = reinterpret_cast<PFun_slEvaluateFeature*>(
                reinterpret_cast<void*>(GetProcAddress(module, "slEvaluateFeature")));
			g_sr_ready =
			    set_options != nullptr && optimal != nullptr && g_api.evaluate_feature != nullptr;
		}
		std::printf("DLSS SR: %s (mode=%s)\n",
		            g_sr_ready ? "ready for Astro Bot's TAA pass"
		                       : "unavailable on this device/runtime; using the game's TAA",
		            SrModeName());
	}
	if (g_generate == 0) return;
	bool loaded              = false;
	if (g_api.is_feature_supported(sl::kFeatureDLSS_G, adapter) != sl::Result::eOk ||
	    g_api.is_feature_loaded(sl::kFeatureDLSS_G, loaded) != sl::Result::eOk || !loaded) {
		std::printf("Frame generation: DLSS-G is not supported on this device\n");
		std::fflush(stdout);
		return;
	}
	g_device_ok = true;
	// (Streamline's feature functions exist once DLSS-G is set up on the device; if not yet, the
	// first present asks again, its swapchain made by then.)
	(void)FindFunctions(false);
#else
	(void)physical_device;
#endif
}

void OnDraw(uint64_t pixel_shader, const PreparedBindings* pixel,
            std::span<const PreparedBindings* const> vertex, std::span<const RenderColorInfo> colors,
            const RenderDepthInfo& depth) {
	if (!Enabled()) return;
	// The TAA pass: its first image is the motion vectors.
	if (pixel_shader == TaaPixelShader) {
		if (pixel == nullptr || pixel->images.empty()) return;
		const auto&       motion = pixel->images[0];
		std::lock_guard lock(g_mutex);
		g_gathered.motion         = motion.image_id;
		g_gathered.motion_address = motion.desc.info.data.address;
		g_gathered.taa            = true;
		g_known_motion_address.store(motion.desc.info.data.address, std::memory_order_relaxed);
		return;
	}
	// A G-buffer draw: writes depth and the motion vectors (a colour target of the depth's size);
	// its vertex shader has the per-view constants.
	if (colors.size() < 2 || !depth.depth_write_enable || depth.desc.info.data.Empty()) return;
	const auto&    info = depth.desc.info;
	const uint64_t known = g_known_motion_address.load(std::memory_order_relaxed);
	if (known == 0) return; // (from the first TAA pass on)
	const ImageInfo* motion_info = nullptr;
	for (size_t i = 1; i < colors.size(); ++i) {
		const auto& candidate = colors[i].desc.info;
		if (colors[i].image_id && candidate.data.address == known) {
			motion_info = &candidate;
			break;
		}
	}
	if (motion_info == nullptr || motion_info->extent.width != info.extent.width ||
	    motion_info->extent.height != info.extent.height) {
		return;
	}
	const auto& motion = *motion_info;
	std::lock_guard lock(g_mutex);
	if (g_gathered.gbuffer && g_gathered.camera.valid) return; // (the frame's first one is enough)
	Camera camera;
	for (const auto* stage: vertex) {
		if (stage != nullptr && ReadCamera(*stage, camera)) break;
	}
	if (!camera.valid) return;
	g_gathered.camera                 = camera;
	g_gathered.gbuffer_motion_address = motion.data.address;
	g_gathered.depth_address          = info.data.address;
	g_gathered.depth_size             = info.data.size;
	g_gathered.depth_width            = info.extent.width;
	g_gathered.depth_height           = info.extent.height;
	g_gathered.gbuffer                = true;
}

void PrepareInputs(GraphicContext& graphics, TextureCache& cache, CommandBuffer& command,
                   FrameInputs& inputs) {
	inputs.valid = false;
	if (!Enabled()) return;
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (g_sr_ready && ++g_sr_guest_flips == 120 && g_sr_taa_calls == 0) {
		std::printf("DLSS SR: waiting: no executed Astro Bot TAA shader (0x%" PRIx64
		            ") after 120 guest flips; enter gameplay or capture KYTY_FG_DISCOVER=1 "
		            "to check this game/patch's shader path\n",
		            TaaPixelShader);
		std::fflush(stdout);
	}
#endif
	Gathered gathered;
	{
		std::lock_guard lock(g_mutex);
		gathered   = g_gathered;
		g_gathered = {};
	}
	// SR consumes the original images at TAA. It does not need the presenter's
	// persistent depth/motion copies when frame generation is unavailable or off.
	if (!g_ready) return;
	if (!gathered.gbuffer || !gathered.taa || !gathered.camera.valid ||
	    gathered.motion_address != gathered.gbuffer_motion_address) {
		return;
	}
	auto& motion = cache.GetImage(gathered.motion);
	if (motion.info.data.address != gathered.motion_address || motion.backing.image == nullptr ||
	    motion.backing.extent.width != gathered.depth_width ||
	    motion.backing.extent.height != gathered.depth_height) {
		return; // (replaced after the TAA pass)
	}
	const auto depth_id = cache.FindImageFromRange(gathered.depth_address, gathered.depth_size, false);
	if (!depth_id) return;
	auto& depth = cache.GetImage(depth_id);
	if (depth.backing.image == nullptr || !IsDepthFormat(depth.backing.format) ||
	    depth.info.data.address != gathered.depth_address ||
	    depth.backing.extent.width != motion.backing.extent.width ||
	    depth.backing.extent.height != motion.backing.extent.height) {
		return;
	}
	Configure(graphics, inputs.depth, inputs.depth_view, depth.backing);
	Configure(graphics, inputs.motion, inputs.motion_view, motion.backing);
	command.EndRendering();
	auto vk_command = command.Handle();
	CopyInto(vk_command, depth, inputs.depth);
	CopyInto(vk_command, motion, inputs.motion);
	inputs.render_extent = {motion.backing.extent.width, motion.backing.extent.height};
	inputs.camera        = gathered.camera;
	inputs.valid         = true;
	static bool reported = false;
	if (!reported) {
		reported = true;
		const auto& p = gathered.camera.projection;
		std::printf("Frame generation: Astro Bot's inputs found (motion and depth %ux%u, vertical FOV "
		            "%.1f degrees)\n",
		            inputs.render_extent.width, inputs.render_extent.height,
		            2.0 * std::atan(1.0 / p[5]) * 180.0 / 3.14159265358979);
		std::fflush(stdout);
	}
}

void DestroyInputs(GraphicContext& graphics, FrameInputs& inputs) {
	for (auto* view: {&inputs.depth_view, &inputs.motion_view}) {
		if (*view != nullptr) {
			graphics.device.destroyImageView(*view, nullptr);
			*view = nullptr;
		}
	}
	for (auto* image: {&inputs.depth, &inputs.motion}) {
		if (image->image != nullptr) {
			graphics.DeleteImage(*image);
		}
	}
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (g_last_inputs == &inputs) {
		g_last_inputs = nullptr;
	}
#endif
	inputs.valid = false;
}

bool ResolveTaa(CommandBuffer& command, uint64_t pixel_shader, const PreparedBindings* pixel,
                std::span<const RenderColorInfo> colors) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_sr_ready || pixel_shader != TaaPixelShader) return false;
	std::lock_guard api_lock(g_streamline_mutex);
	++g_sr_taa_calls;
	auto& cache = command.GetContext().GetTextureCache();
	ReportSrLayout(cache, pixel, colors);
	// Run after the original TAA draw: its second target remains a valid fallback
	// history. Only replace rt0, from the unfiltered lit colour (image 1).
	const bool previous_valid = g_sr_history_valid;
	g_sr_history_valid        = false;
	if (pixel == nullptr || pixel->images.size() != 3 || colors.size() != 2 ||
	    !colors[0].image_id || colors[0].target_slot != 0 || colors[0].guest_mip_level != 0 ||
	    colors[0].guest_array_layer != 0)
		return SkipSr(SrSkip::Bindings,
		              "unexpected TAA bindings/target subresource (images=%zu targets=%zu)",
		              pixel != nullptr ? pixel->images.size() : size_t {0}, colors.size());
	Gathered gathered;
	{
		std::lock_guard lock(g_mutex);
		gathered = g_gathered;
	}
	std::array<uint32_t, 20> taa {};
	bool                     have_constants = false;
	for (const auto& source: pixel->buffer_sources) {
		if (source.size == sizeof(taa) &&
		    LibKernel::Memory::TryReadGpuCleanBacking(source.address, taa.data(), sizeof(taa))) {
			have_constants = true;
			break;
		}
	}
	if (!have_constants)
		return SkipSr(SrSkip::Constants, "no readable 80-byte TAA constant buffer");
	static std::array<uint32_t, 2> reported_constants {};
	static uint32_t                constant_reports = 0;
	if (reported_constants != std::array<uint32_t, 2> {taa[4], taa[5]} && constant_reports < 16) {
		reported_constants = {taa[4], taa[5]};
		++constant_reports;
		std::printf("DLSS SR: TAA constants: render=%ux%u\n", taa[4], taa[5]);
		std::fflush(stdout);
	}
	if (!gathered.gbuffer || !gathered.camera.valid || !gathered.taa ||
	    gathered.motion_address != gathered.gbuffer_motion_address)
		return SkipSr(
		    SrSkip::Camera,
		    "incomplete G-buffer/camera capture (gbuffer=%d camera=%d taa=%d motion_match=%d)",
		    int(gathered.gbuffer), int(gathered.camera.valid), int(gathered.taa),
		    int(gathered.motion_address == gathered.gbuffer_motion_address));
	const auto finite = [](float value) { return std::isfinite(value); };
	if (!std::all_of(gathered.camera.view.begin(), gathered.camera.view.end(), finite) ||
	    !std::all_of(gathered.camera.projection.begin(), gathered.camera.projection.end(), finite))
		return SkipSr(SrSkip::CameraValues, "non-finite camera constants");
	const auto depth_id =
	    cache.FindImageFromRange(gathered.depth_address, gathered.depth_size, false);
	if (!depth_id || !pixel->images[0].image_id || !pixel->images[1].image_id)
		return SkipSr(SrSkip::Images, "depth/motion/colour image missing from texture cache");
	auto&              color  = cache.GetImage(pixel->images[1].image_id);
	auto&              motion = cache.GetImage(pixel->images[0].image_id);
	auto&              depth  = cache.GetImage(depth_id);
	auto&              output = cache.GetImage(colors[0].image_id);
	const vk::Extent2D input_size {color.backing.extent.width, color.backing.extent.height};
	const vk::Extent2D output_size {output.backing.extent.width, output.backing.extent.height};
	TaaJitter          jitter;
	const auto         geometry = ResolveAstroTaaGeometry(taa, gathered.camera.projection,
	                                                      {input_size.width, input_size.height},
	                                                      {output_size.width, output_size.height}, jitter);
	if (geometry == TaaGeometryStatus::InvalidExtent || output_size != colors[0].Extent())
		return SkipSr(
		    SrSkip::Extent,
		    "render area/image mismatch: constants=%ux%u lit=%ux%u TAA=%ux%u target=%ux%u", taa[4],
		    taa[5], input_size.width, input_size.height, output_size.width, output_size.height,
		    colors[0].Extent().width, colors[0].Extent().height);
	if (color.backing.image == nullptr || motion.backing.image == nullptr ||
	    depth.backing.image == nullptr || output.backing.image == nullptr)
		return SkipSr(SrSkip::MissingImage, "a TAA resource has no Vulkan image");
	if (color.backing.image == output.backing.image ||
	    motion.backing.image == output.backing.image || depth.backing.image == output.backing.image)
		return SkipSr(SrSkip::Aliasing, "DLSS input aliases the TAA output");
	if (color.backing.format != vk::Format::eR16G16B16A16Sfloat ||
	    output.backing.format != color.backing.format || !IsDepthFormat(depth.backing.format) ||
	    !(output.backing.usage & vk::ImageUsageFlagBits::eStorage))
		return SkipSr(SrSkip::Format,
		              "unsupported colour/depth format or output storage usage (lit=%s TAA=%s "
		              "depth=%s storage=%d)",
		              vk::to_string(color.backing.format).c_str(),
		              vk::to_string(output.backing.format).c_str(),
		              vk::to_string(depth.backing.format).c_str(),
		              int(bool(output.backing.usage & vk::ImageUsageFlagBits::eStorage)));
	if (motion.info.data.address != gathered.motion_address ||
	    depth.info.data.address != gathered.depth_address)
		return SkipSr(SrSkip::MotionIdentity, "captured depth/motion image was replaced");
	if (motion.backing.extent.width != input_size.width ||
	    motion.backing.extent.height != input_size.height ||
	    depth.backing.extent.width != input_size.width ||
	    depth.backing.extent.height != input_size.height)
		return SkipSr(SrSkip::MotionSize, "input size mismatch: lit=%ux%u motion=%ux%u depth=%ux%u",
		              input_size.width, input_size.height, motion.backing.extent.width,
		              motion.backing.extent.height, depth.backing.extent.width,
		              depth.backing.extent.height);
	for (const auto* image: {&color, &motion, &depth, &output}) {
		if (image->info.samples != 1 || image->backing.extent.depth != 1)
			return SkipSr(SrSkip::Subresources, "multisampled or non-2D TAA resource");
	}
	if (geometry == TaaGeometryStatus::InvalidJitter)
		return SkipSr(SrSkip::Jitter, "invalid projection jitter (%g, %g input pixels)", jitter.x,
		              jitter.y);
	const sl::ViewportHandle viewport {1}; // distinct constants/tags from presenter-side DLSS-G
	sl::DLSSOptions          options {};
	options.mode            = g_sr_mode;
	options.outputWidth     = output_size.width;
	options.outputHeight    = output_size.height;
	options.colorBuffersHDR = sl::Boolean::eTrue; // pre-tonemap RGBA16F, regardless of display HDR
	options.useAutoExposure = sl::Boolean::eTrue;
	options.alphaUpscalingEnabled = sl::Boolean::eTrue;
	sl::DLSSOptimalSettings optimal {};
	if (const auto result = g_api.dlss_optimal_settings(options, optimal);
	    result != sl::Result::eOk)
		return SkipSr(SrSkip::OptimalSettings, "slDLSSGetOptimalSettings failed (%d)", int(result));
	if ((g_sr_mode == sl::DLSSMode::eDLAA && input_size != output_size) ||
	    (g_sr_mode != sl::DLSSMode::eDLAA &&
	     (input_size.width >= output_size.width || input_size.height >= output_size.height)) ||
	    input_size.width < optimal.renderWidthMin || input_size.height < optimal.renderHeightMin ||
	    input_size.width > optimal.renderWidthMax || input_size.height > optimal.renderHeightMax) {
		static vk::Extent2D reported_input {}, reported_output {};
		if (reported_input != input_size || reported_output != output_size) {
			std::printf("DLSS SR: %ux%u -> %ux%u is outside mode=%s range "
			            "(allowed input %ux%u to %ux%u, recommended %ux%u); "
			            "using the game's TAA. SR needs lower-resolution game input; "
			            "KYTY_DLSS_SR=dlaa uses native-resolution input.\n",
			            input_size.width, input_size.height, output_size.width, output_size.height,
			            SrModeName(), optimal.renderWidthMin, optimal.renderHeightMin,
			            optimal.renderWidthMax, optimal.renderHeightMax, optimal.optimalRenderWidth,
			            optimal.optimalRenderHeight);
			reported_input  = input_size;
			reported_output = output_size;
			std::fflush(stdout);
		}
		return false;
	}
	if (const auto result = g_api.dlss_set_options(viewport, options); result != sl::Result::eOk)
		return SkipSr(SrSkip::Options, "slDLSSSetOptions failed (%d)", int(result));
	const bool reset = !previous_valid || g_sr_input != input_size || g_sr_output != output_size ||
	                   taa[11] != g_sr_last_frame + 1u || taa[13] == 0;
	sl::Constants constants {};
	auto          projection = gathered.camera.projection;
	projection[8] = projection[9] = 0.0f;
	constants.cameraViewToClip    = FromColumnMajor(projection);
	if (!Inverted(constants.cameraViewToClip, constants.clipToCameraView))
		return SkipSr(SrSkip::Projection, "projection matrix cannot be inverted");
	const auto view_proj =
	    Multiply(FromColumnMajor(gathered.camera.view), constants.cameraViewToClip);
	Mat4 clip_to_world;
	if (!Inverted(view_proj, clip_to_world))
		return SkipSr(SrSkip::View, "view-projection matrix cannot be inverted");
	constants.clipToLensClip = Identity();
	constants.clipToPrevClip =
	    reset ? Identity() : Multiply(clip_to_world, g_sr_previous_view_proj);
	if (!Inverted(constants.clipToPrevClip, constants.prevClipToClip))
		return SkipSr(SrSkip::Reprojection, "reprojection matrix cannot be inverted");
	constants.jitterOffset        = sl::float2(jitter.x, jitter.y);
	constants.mvecScale           = sl::float2(-1.0f, -1.0f);
	constants.cameraPinholeOffset = sl::float2(0, 0);
	const auto& v                 = gathered.camera.view;
	constants.cameraRight         = sl::float3(v[0], v[4], v[8]);
	constants.cameraUp            = sl::float3(v[1], v[5], v[9]);
	constants.cameraFwd           = sl::float3(-v[2], -v[6], -v[10]);
	constants.cameraPos           = sl::float3(-(v[0] * v[12] + v[1] * v[13] + v[2] * v[14]),
	                                           -(v[4] * v[12] + v[5] * v[13] + v[6] * v[14]),
	                                           -(v[8] * v[12] + v[9] * v[13] + v[10] * v[14]));
	constants.cameraNear          = std::fabs(projection[14] / (projection[10] - 1.0f));
	constants.cameraFar =
	    projection[10] == -1.0f ? 500.0f : std::fabs(projection[14] / (projection[10] + 1.0f));
	if (!std::isfinite(constants.cameraNear) || !std::isfinite(constants.cameraFar) ||
	    constants.cameraNear <= 0.0f || constants.cameraFar <= constants.cameraNear)
		return SkipSr(SrSkip::ClipPlanes, "invalid near/far clip planes (%g, %g)",
		              constants.cameraNear, constants.cameraFar);
	constants.cameraFOV              = 2.0f * std::atan(1.0f / std::fabs(projection[5]));
	constants.cameraAspectRatio      = std::fabs(projection[5] / projection[0]);
	constants.depthInverted          = sl::Boolean::eFalse;
	constants.cameraMotionIncluded   = sl::Boolean::eTrue;
	constants.motionVectors3D        = sl::Boolean::eFalse;
	constants.orthographicProjection = sl::Boolean::eFalse;
	constants.reset                  = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	sl::FrameToken* token            = nullptr;
	const uint32_t  frame_index      = ++g_frame_index;
	if (const auto result = g_api.get_new_frame_token(token, &frame_index);
	    result != sl::Result::eOk || token == nullptr)
		return SkipSr(SrSkip::FrameToken, "slGetNewFrameToken failed (%d, token=%d)", int(result),
		              int(token != nullptr));
	if (const auto result = g_api.set_constants(constants, *token, viewport);
	    result != sl::Result::eOk)
		return SkipSr(SrSkip::SetConstants, "slSetConstants failed (%d)", int(result));
	command.EndRendering();
	auto native = command.Handle(); // drains the CP recorder before SDK commands
	for (auto* image: {&color, &motion, &depth}) {
		image->Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
		               {}, native);
	}
	output.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite, {}, native);
	const auto resource = [&](Image& image, vk::ImageAspectFlags aspect,
	                          vk::ImageUsageFlags usage) {
		ImageViewInfo view {};
		view.format = image.backing.format;
		view.aspect = aspect;
		view.usage  = usage;
		sl::Resource r(sl::ResourceType::eTex2d, static_cast<VkImage>(image.backing.image), nullptr,
		               static_cast<VkImageView>(image.FindView(view)),
		               static_cast<uint32_t>(image.backing.state.layout));
		r.width        = image.backing.extent.width;
		r.height       = image.backing.extent.height;
		r.nativeFormat = static_cast<uint32_t>(image.backing.format);
		r.mipLevels    = 1;
		r.arrayLayers  = 1;
		r.flags        = 0;
		r.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(image.backing.usage));
		return r;
	};
	auto color_resource =
	    resource(color, vk::ImageAspectFlagBits::eColor, vk::ImageUsageFlagBits::eSampled);
	auto motion_resource =
	    resource(motion, vk::ImageAspectFlagBits::eColor, vk::ImageUsageFlagBits::eSampled);
	auto depth_resource =
	    resource(depth, vk::ImageAspectFlagBits::eDepth, vk::ImageUsageFlagBits::eSampled);
	auto output_resource =
	    resource(output, vk::ImageAspectFlagBits::eColor, vk::ImageUsageFlagBits::eStorage);
	// NGX otherwise assumes COLOR for Vulkan resources, including depth/stencil images.
	sl::SubresourceRange color_range {};
	color_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	color_range.levelCount = 1;
	color_range.layerCount = 1;
	sl::SubresourceRange depth_range {};
	depth_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depth_range.levelCount = 1;
	depth_range.layerCount = 1;
	color_resource.next    = &color_range;
	motion_resource.next   = &color_range;
	output_resource.next   = &color_range;
	depth_resource.next    = &depth_range;
	const sl::Extent input_extent {0, 0, input_size.width, input_size.height};
	const sl::Extent output_extent {0, 0, output_size.width, output_size.height};
	sl::ResourceTag  tags[] {
        {&color_resource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow,
	      &input_extent},
        {&motion_resource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow,
	      &input_extent},
        {&depth_resource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow,
	      &input_extent},
        {&output_resource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow,
	      &output_extent},
    };
	auto result =
	    g_api.set_tag_for_frame(*token, viewport, tags, static_cast<uint32_t>(std::size(tags)),
	                            static_cast<VkCommandBuffer>(native));
	if (result == sl::Result::eOk) {
		const sl::BaseStructure* inputs[] {&viewport};
		result = g_api.evaluate_feature(sl::kFeatureDLSS, *token, inputs, 1,
		                                static_cast<VkCommandBuffer>(native));
	}
	command.InvalidateExternalState();
	if (result != sl::Result::eOk) {
		std::printf("DLSS SR: evaluation failed (%d); disabled for this session\n",
		            static_cast<int>(result));
		g_sr_ready = false;
	} else {
		g_sr_history_valid      = true;
		g_sr_last_frame         = taa[11];
		g_sr_previous_view_proj = view_proj;
		if (g_sr_input != input_size || g_sr_output != output_size) {
			std::printf("DLSS SR: active %ux%u -> %ux%u%s\n", input_size.width, input_size.height,
			            output_size.width, output_size.height,
			            g_sr_mode == sl::DLSSMode::eDLAA ? " (DLAA)" : "");
		}
		g_sr_input  = input_size;
		g_sr_output = output_size;
	}
	return true;
#else
	(void)command;
	(void)pixel_shader;
	(void)pixel;
	(void)colors;
	return false;
#endif
}

void BeforePresent(const FrameInputs* inputs, vk::Extent2D backbuffer, vk::Format format,
                   uint32_t image_count) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	std::lock_guard api_lock(g_streamline_mutex);
	if (g_sr_ready) {
		static vk::Extent2D reported_backbuffer {};
		if (reported_backbuffer != backbuffer) {
			reported_backbuffer = backbuffer;
			std::printf("DLSS SR: presentation=%ux%u (swapchain; DLSS output is the TAA target)\n",
			            backbuffer.width, backbuffer.height);
			std::fflush(stdout);
		}
	}
	if (!g_ready.load(std::memory_order_relaxed)) {
		// (The swapchain exists now; asked once more, then reported.)
		static uint32_t attempts = 0;
		if (!g_device_ok || attempts > 300 || !FindFunctions(++attempts > 300)) return;
	}
	const sl::ViewportHandle viewport {0};
	const bool               usable = inputs != nullptr && inputs->valid;
	g_run_with    = usable ? g_run_with + 1 : 0;
	g_run_without = usable ? 0 : g_run_without + 1;
	const bool mode_on = g_mode_on ? g_run_without < TurnOffAfter : g_run_with >= TurnOnAfter;
	if (!g_options_set || g_options_format != format || mode_on != g_mode_on ||
	    (usable && (g_options_backbuffer != backbuffer || g_options_render != inputs->render_extent))) {
		sl::DLSSGOptions options {};
		options.mode                = mode_on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
		options.flags               = sl::DLSSGFlags::eRetainResourcesWhenOff;
		options.numFramesToGenerate = static_cast<uint32_t>(g_generate);
		options.numBackBuffers      = image_count;
		options.colorWidth          = backbuffer.width;
		options.colorHeight         = backbuffer.height;
		options.colorBufferFormat   = static_cast<uint32_t>(format);
		if (usable) {
			options.mvecDepthWidth    = inputs->render_extent.width;
			options.mvecDepthHeight   = inputs->render_extent.height;
			options.mvecBufferFormat  = static_cast<uint32_t>(inputs->motion.format);
			options.depthBufferFormat = static_cast<uint32_t>(inputs->depth.format);
			g_options_render          = inputs->render_extent;
		}
		if (const auto result = g_api.dlssg_set_options(viewport, options); result != sl::Result::eOk) {
			LOGF("frame generation: slDLSSGSetOptions failed (%d)\n", static_cast<int>(result));
			std::printf("Frame generation: slDLSSGSetOptions failed (%d)\n", static_cast<int>(result));
		}
		g_options_set        = true;
		g_options_backbuffer = backbuffer;
		g_options_format     = format;
		if (mode_on != g_mode_on) {
			std::printf("Frame generation: DLSS-G %s\n", mode_on ? "on" : "off (no 3D view)");
			std::fflush(stdout);
			g_mode_on = mode_on;
		}
	}

	const uint32_t frame_index = ++g_frame_index;
	if (g_api.get_new_frame_token(g_token, &frame_index) != sl::Result::eOk || g_token == nullptr) {
		g_token = nullptr;
		return;
	}
	auto& token = *g_token;
	// The frame's Reflex sleep, then its markers (all at its present: the game's frame was made before).
	if (g_api.reflex_sleep != nullptr) g_api.reflex_sleep(token);
	g_api.pcl_set_marker(sl::PCLMarker::eSimulationStart, token);
	g_api.pcl_set_marker(sl::PCLMarker::eSimulationEnd, token);
	g_api.pcl_set_marker(sl::PCLMarker::eRenderSubmitStart, token);

	sl::Constants constants {};
	const Mat4    identity = Identity();
	constants.cameraViewToClip          = identity;
	constants.clipToCameraView          = identity;
	constants.clipToLensClip            = identity;
	constants.clipToPrevClip            = identity;
	constants.prevClipToClip            = identity;
	constants.jitterOffset              = sl::float2(0.0f, 0.0f);
	constants.mvecScale                 = sl::float2(1.0f, 1.0f);
	constants.cameraPinholeOffset       = sl::float2(0.0f, 0.0f);
	constants.cameraPos                 = sl::float3(0, 0, 0);
	constants.cameraUp                  = sl::float3(0, 1, 0);
	constants.cameraRight               = sl::float3(1, 0, 0);
	constants.cameraFwd                 = sl::float3(0, 0, -1);
	constants.cameraNear                = 0.005f;
	constants.cameraFar                 = 500.0f;
	constants.cameraFOV                 = 0.4f;
	constants.cameraAspectRatio         = 16.0f / 9.0f;
	constants.motionVectorsInvalidValue = sl::INVALID_FLOAT;
	// Astro Bot's projection is OpenGL-style, not reversed (docs/RE-ASTRO-BOT-FG.md).
	constants.depthInverted          = sl::Boolean::eFalse;
	constants.cameraMotionIncluded   = sl::Boolean::eTrue;
	constants.motionVectors3D        = sl::Boolean::eFalse;
	constants.orthographicProjection = sl::Boolean::eFalse;
	constants.motionVectorsDilated   = sl::Boolean::eFalse;
	constants.motionVectorsJittered  = sl::Boolean::eFalse;
	// A frame without inputs while DLSS-G is on: the last inputs again, as a still frame.
	const bool held = !usable && g_mode_on && g_last_inputs != nullptr &&
	                  g_last_inputs->depth.image != nullptr && g_last_inputs->motion.image != nullptr;
	const FrameInputs* tagged = usable ? inputs : held ? g_last_inputs : nullptr;
	bool               reset  = tagged == nullptr;
	if (usable) {
		g_last_inputs = inputs;
	}
	if (tagged != nullptr) {
		const auto& v = tagged->camera.view;
		auto        p = tagged->camera.projection;
		// The projection's jitter (col2.x, col2.y in NDC), then without it.
		const float jitter_x = p[8], jitter_y = p[9];
		p[8] = 0.0f;
		p[9] = 0.0f;
		const Mat4 projection = FromColumnMajor(p);
		constants.cameraViewToClip = projection;
		Inverted(projection, constants.clipToCameraView);
		// World to clip, row vectors: p * V * P.
		const Mat4 view_proj = Multiply(FromColumnMajor(v), projection);
		Mat4       clip_to_world;
		if (held) {
			// Nothing moved since the last frame: identity reprojection (set above).
		} else if (g_previous_valid && Inverted(view_proj, clip_to_world)) {
			constants.clipToPrevClip = Multiply(clip_to_world, g_previous_view_proj);
			Inverted(constants.clipToPrevClip, constants.prevClipToClip);
		} else {
			reset = true;
		}
		if (!held) {
			g_previous_view_proj = view_proj;
			g_previous_valid     = true;
		}
		const float width    = static_cast<float>(tagged->render_extent.width);
		const float height   = static_cast<float>(tagged->render_extent.height);
		constants.jitterOffset = sl::float2(jitter_x * width * 0.5f, -jitter_y * height * 0.5f);
		// The game's vectors are current minus previous, in UV units (its TAA reads the history
		// at uv - mv); Streamline wants previous = current + mv * scale.
		static const float mv_sign = [] {
			const char* sign = std::getenv("KYTY_FRAMEGEN_MV_SIGN");
			return sign != nullptr && std::atof(sign) > 0 ? 1.0f : -1.0f;
		}();
		constants.mvecScale = held ? sl::float2(0.0f, 0.0f) : sl::float2(mv_sign, mv_sign);
		// The view's rows are the camera's axes in the world (column-major: v[0], v[4], v[8] ...);
		// the camera looks down its -Z; its position is -R^T t.
		const sl::float3 right(v[0], v[4], v[8]), up(v[1], v[5], v[9]), back(v[2], v[6], v[10]);
		const float      tx = v[12], ty = v[13], tz = v[14];
		constants.cameraRight = right;
		constants.cameraUp    = up;
		constants.cameraFwd   = sl::float3(-back.x, -back.y, -back.z);
		constants.cameraPos   = sl::float3(-(right.x * tx + up.x * ty + back.x * tz),
		                                   -(right.y * tx + up.y * ty + back.y * tz),
		                                   -(right.z * tx + up.z * ty + back.z * tz));
		// OpenGL-style terms: z = p10 * zv + p14, w = -zv.
		if (p[10] != 1.0f && p[10] != -1.0f) {
			constants.cameraNear = std::fabs(p[14] / (p[10] - 1.0f));
			constants.cameraFar  = std::fabs(p[14] / (p[10] + 1.0f));
		}
		constants.cameraFOV         = 2.0f * std::atan(1.0f / std::fabs(p[5]));
		constants.cameraAspectRatio = std::fabs(p[5] / p[0]);
	} else {
		g_previous_valid = false;
	}
	constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	if (const auto result = g_api.set_constants(constants, token, viewport);
	    result != sl::Result::eOk && g_presents < 8) {
		LOGF("frame generation: slSetConstants failed (%d)\n", static_cast<int>(result));
	}

	if (tagged != nullptr) {
		sl::Resource depth(sl::ResourceType::eTex2d, static_cast<VkImage>(tagged->depth.image), nullptr,
		                   static_cast<VkImageView>(tagged->depth_view),
		                   static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		depth.width        = tagged->depth.extent.width;
		depth.height       = tagged->depth.extent.height;
		depth.nativeFormat = static_cast<uint32_t>(tagged->depth.format);
		depth.mipLevels    = 1;
		depth.arrayLayers  = 1;
		depth.flags        = 0;
		depth.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(tagged->depth.usage));
		sl::Resource motion(sl::ResourceType::eTex2d, static_cast<VkImage>(tagged->motion.image), nullptr,
		                    static_cast<VkImageView>(tagged->motion_view),
		                    static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		motion.width        = tagged->motion.extent.width;
		motion.height       = tagged->motion.extent.height;
		motion.nativeFormat = static_cast<uint32_t>(tagged->motion.format);
		motion.mipLevels    = 1;
		motion.arrayLayers  = 1;
		motion.flags        = 0;
		motion.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(tagged->motion.usage));
		const sl::Extent full {0, 0, tagged->render_extent.width, tagged->render_extent.height};
		sl::ResourceTag  tags[] {
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &full),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent,
                            &full),
        };
		if (const auto result =
		        g_api.set_tag_for_frame(token, viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr);
		    result != sl::Result::eOk && g_presents < 8) {
			LOGF("frame generation: slSetTagForFrame failed (%d)\n", static_cast<int>(result));
		}
		g_with_inputs += usable ? 1u : 0u;
	} else {
		// Menus, movies, loading: no depth and motion (DLSS-G then presents the frame as is).
		sl::ResourceTag tags[] {
		    sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
		    sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
		};
		g_api.set_tag_for_frame(token, viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr);
	}
	g_api.pcl_set_marker(sl::PCLMarker::eRenderSubmitEnd, token);
	g_api.pcl_set_marker(sl::PCLMarker::ePresentStart, token);
#else
	(void)inputs;
	(void)backbuffer;
	(void)format;
	(void)image_count;
#endif
}

void AfterPresent() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	std::lock_guard api_lock(g_streamline_mutex);
	if (!g_ready.load(std::memory_order_relaxed) || g_token == nullptr) return;
	g_api.pcl_set_marker(sl::PCLMarker::ePresentEnd, *g_token);
	const auto presents = ++g_presents;
	if (presents % 600 != 1) return;
	sl::DLSSGState state {};
	if (g_api.dlssg_get_state(sl::ViewportHandle {0}, state, nullptr) != sl::Result::eOk) return;
	// More generated frames asked for than the GPU makes: as many as it makes.
	if (state.numFramesToGenerateMax != 0 && static_cast<uint32_t>(g_generate) > state.numFramesToGenerateMax) {
		std::printf("Frame generation: %d generated frame(s) asked for, the GPU makes up to %u\n", g_generate,
		            state.numFramesToGenerateMax);
		g_generate    = static_cast<int>(state.numFramesToGenerateMax);
		g_options_set = false;
	}
	std::printf("Frame generation: status=0x%x, %u frame(s) shown per rendered frame, %" PRIu64
	            " of the last presents had inputs\n",
	            static_cast<uint32_t>(state.status), state.numFramesActuallyPresented, g_with_inputs);
	std::fflush(stdout);
	g_with_inputs = 0;
#endif
}

void SwapchainChanging() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	std::lock_guard api_lock(g_streamline_mutex);
	g_sr_history_valid = false;
	// Off before the swapchain is made again (DLSS-G guide: before a resize or a full screen change).
	g_last_inputs = nullptr;
	if (!g_ready || !g_mode_on) return;
	sl::DLSSGOptions options {};
	options.mode                = sl::DLSSGMode::eOff;
	options.flags               = sl::DLSSGFlags::eRetainResourcesWhenOff;
	options.numFramesToGenerate = static_cast<uint32_t>(g_generate);
	if (const auto result = g_api.dlssg_set_options(sl::ViewportHandle {0}, options); result != sl::Result::eOk) {
		LOGF("frame generation: slDLSSGSetOptions (off) failed (%d)\n", static_cast<int>(result));
	}
	g_mode_on        = false;
	g_run_with       = 0;
	g_options_set    = false;
	g_previous_valid = false;
#endif
}

} // namespace Libs::Graphics::FrameGen
