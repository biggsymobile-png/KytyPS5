#include "graphics/host_gpu/renderer/frameGenDlssg.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/render.h"

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

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#endif

// The TAA upscalers' code: DLSS and FSR through AMD's runtime with Streamline (Windows), or FSR
// from the built-in FidelityFX SDK sources (KYTY_FSR_NATIVE: Linux, macOS).
#if defined(KYTY_FRAMEGEN_STREAMLINE) || defined(KYTY_FSR_NATIVE)
#define KYTY_UPSCALER_CODE 1
#include <chrono>
#include <string>
#include <vector>
#endif

#if defined(KYTY_FSR_NATIVE) && !defined(KYTY_FRAMEGEN_STREAMLINE)
#include "common/threads.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include <cfloat>
#include <memory>

#include <stdlib.h> // posix_memalign

// The FidelityFX Vulkan backend calls a few global Vulkan entry points; the emulator loads Vulkan
// at run time, so 3rdparty/fidelityfx/src/ffx_linux_compat.h routes them here, to its dispatcher.
extern "C" {
void kyty_ffx_vkGetPhysicalDeviceFeatures(VkPhysicalDevice device, VkPhysicalDeviceFeatures* features) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures(device, features);
}
void kyty_ffx_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device, VkPhysicalDeviceFeatures2* features) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(device, features);
}
void kyty_ffx_vkGetPhysicalDeviceProperties(VkPhysicalDevice device, VkPhysicalDeviceProperties* properties) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties(device, properties);
}
void kyty_ffx_vkGetPhysicalDeviceProperties2(VkPhysicalDevice device, VkPhysicalDeviceProperties2* properties) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties2(device, properties);
}
void kyty_ffx_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice                  device,
                                                  VkPhysicalDeviceMemoryProperties* properties) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(device, properties);
}
VkResult kyty_ffx_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice device, const char* layer,
                                                       uint32_t* count, VkExtensionProperties* properties) {
	return VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(device, layer, count, properties);
}
PFN_vkVoidFunction kyty_ffx_vkGetDeviceProcAddr(VkDevice device, const char* name) {
	return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, name);
}
}

// The parts of Streamline's types the camera code below uses (sl_consts.h), for builds without it.
namespace sl {
inline constexpr float INVALID_FLOAT = 3.40282346638528859811704183484516925440e38f;
enum class Boolean : char { eFalse, eTrue };
struct float2 {
	float2() = default;
	float2(float x_, float y_): x(x_), y(y_) {}
	float x = 0.0f, y = 0.0f;
};
struct float3 {
	float3() = default;
	float3(float x_, float y_, float z_): x(x_), y(y_), z(z_) {}
	float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct float4 {
	float4() = default;
	float4(float x_, float y_, float z_, float w_): x(x_), y(y_), z(z_), w(w_) {}
	float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};
struct float4x4 {
	float4&       operator[](uint32_t i) { return row[i]; }
	const float4& operator[](uint32_t i) const { return row[i]; }
	float4        row[4];
};
struct Constants {
	float4x4 cameraViewToClip, clipToCameraView, clipToLensClip, clipToPrevClip, prevClipToClip;
	float2   jitterOffset, mvecScale, cameraPinholeOffset;
	float3   cameraPos, cameraUp, cameraRight, cameraFwd;
	float    cameraNear = 0.1f, cameraFar = 1000.0f, cameraFOV = 1.0f, cameraAspectRatio = 1.0f;
	float    motionVectorsInvalidValue = INVALID_FLOAT;
	Boolean  depthInverted = Boolean::eFalse, cameraMotionIncluded = Boolean::eTrue,
	        motionVectors3D = Boolean::eFalse, orthographicProjection = Boolean::eFalse,
	        motionVectorsDilated = Boolean::eFalse, motionVectorsJittered = Boolean::eFalse,
	        reset = Boolean::eFalse;
};
} // namespace sl
#endif

namespace Libs::Graphics::FrameGen {

namespace {

constexpr uint32_t ViewConstantsBytes = 576; // docs/RE-ASTRO-BOT-FG.md

int  g_generate = 0;     // frames to generate per rendered frame (0 = off)
bool g_upscale  = false; // KYTY_DLSS: DLSS super resolution in place of the game's TAA
bool g_fsr      = false; // KYTY_FSR: AMD FSR 3.1 in place of the game's TAA (no Streamline needed)
std::atomic<bool> g_sr_ready {false}; // DLSS-SR available on the device (OnDevice)
std::atomic<uint64_t> g_flips {0};    // guest flips seen (PrepareInputs)
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
// A view (col3.w = 1) at words[at - 16], then a perspective projection at words[at].
bool CameraAt(const uint32_t* words, uint32_t at) {
	const float p00 = AsFloat(words[at]), p11 = AsFloat(words[at + 5]);
	return AsFloat(words[at + 11]) == -1.0f && words[at + 15] == 0 && AsFloat(words[at - 1]) == 1.0f &&
	       p00 > 0.0f && p11 > 0.0f && std::isfinite(p00) && std::isfinite(p11) &&
	       AsFloat(words[at - 13]) == 0.0f && AsFloat(words[at - 9]) == 0.0f && AsFloat(words[at - 5]) == 0.0f;
}

bool ReadCamera(const PreparedBindings& bindings, Camera& camera) {
	const auto take = [&](const uint32_t* words, uint32_t at) {
		for (uint32_t i = 0; i < 16; ++i) {
			camera.view[i]       = AsFloat(words[at - 16 + i]);
			camera.projection[i] = AsFloat(words[at + i]);
		}
		camera.valid = true;
		return true;
	};
	for (const auto& source: bindings.buffer_sources) {
		if (source.size != ViewConstantsBytes || source.address < 0x10000) continue;
		// Guest memory is mapped at the same addresses in the emulator; the CPU writes these.
		const auto* words = reinterpret_cast<const uint32_t*>(source.address);
		const float p00 = AsFloat(words[32]), p11 = AsFloat(words[37]);
		if (AsFloat(words[43]) != -1.0f || words[47] != 0 || AsFloat(words[31]) != 1.0f ||
		    !(p00 > 0.0f) || !(p11 > 0.0f) || !std::isfinite(p00) || !std::isfinite(p11)) {
			continue;
		}
		return take(words, 32);
	}
	// Other layouts (Astro's Playroom: 496 bytes): the first view-then-projection pair, on 16-byte
	// boundaries, in a per-view constants buffer.
	for (const auto& source: bindings.buffer_sources) {
		if (source.size < 128 || source.size > 1024 || source.size == ViewConstantsBytes ||
		    source.address < 0x10000) {
			continue;
		}
		const auto*    words = reinterpret_cast<const uint32_t*>(source.address);
		const uint32_t count = static_cast<uint32_t>(source.size / 4);
		for (uint32_t at = 16; at + 16 <= count; at += 4) {
			if (CameraAt(words, at)) return take(words, at);
		}
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
// Streamline entry points (sl.interposer.dll, loaded at run time) and feature functions.
struct Api {
	PFun_slInit*               init                 = nullptr;
	PFun_slIsFeatureSupported* is_feature_supported = nullptr;
	PFun_slIsFeatureLoaded*    is_feature_loaded    = nullptr;
	PFun_slGetFeatureFunction* get_feature_function = nullptr;
	PFun_slGetNewFrameToken*   get_new_frame_token  = nullptr;
	PFun_slSetConstants*       set_constants        = nullptr;
	PFun_slSetTagForFrame*     set_tag_for_frame    = nullptr;
	PFun_slDLSSGSetOptions*    dlssg_set_options    = nullptr;
	PFun_slDLSSGGetState*      dlssg_get_state      = nullptr;
	PFun_slReflexSetOptions*   reflex_set_options   = nullptr;
	PFun_slPCLSetMarker*       pcl_set_marker       = nullptr;
	PFun_slReflexSleep*        reflex_sleep         = nullptr;
	PFun_slEvaluateFeature*        evaluate_feature      = nullptr;
	PFun_slDLSSSetOptions*         dlss_set_options      = nullptr;
	PFun_slDLSSGetOptimalSettings* dlss_optimal_settings = nullptr;
};
// Streamline's calls are not thread safe: the GPU thread's DLSS-SR evaluation and the present
// thread's DLSS-G constants and tags take turns (not around vkQueuePresentKHR itself).
std::mutex g_sl_mutex;
Api             g_api;
sl::FrameToken* g_token       = nullptr; // present thread: the frame being presented
uint32_t        g_frame_index = 0;
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
#endif

#ifdef KYTY_UPSCALER_CODE
// Matrices in Streamline's convention: row-major, row vectors (p' = p * M). A column-major
// column-vector matrix in memory reads as exactly that.
using Mat4 = sl::float4x4;
// The previous presented frame's world-to-clip (unjittered), for clipToPrevClip.
[[maybe_unused]] Mat4 g_previous_view_proj;
[[maybe_unused]] bool g_previous_valid = false;

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

// Streamline's constants without a camera (identity matrices, Astro Bot's depth convention).
void DefaultConstants(sl::Constants& constants) {
	const Mat4 identity = Identity();
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
	constants.reset                  = sl::Boolean::eFalse;
}

// The motion vector sign: the game's vectors are current minus previous, in UV units (its TAA
// reads the history at uv - mv); Streamline wants previous = current + mv * scale.
float MotionVectorSign() {
	static const float sign = [] {
		const char* value = std::getenv("KYTY_FRAMEGEN_MV_SIGN");
		return value != nullptr && std::atof(value) > 0 ? 1.0f : -1.0f;
	}();
	return sign;
}

// The camera's constants from its view and jittered projection, `extent` the render size;
// `previous` is the previous call's world-to-clip (unjittered). False: no previous camera (reset).
bool CameraConstants(const Camera& camera, vk::Extent2D extent, Mat4& previous, bool& previous_valid,
                     sl::Constants& constants) {
	bool        continuous = true;
	const auto& v          = camera.view;
	auto        p          = camera.projection;
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
	if (previous_valid && Inverted(view_proj, clip_to_world)) {
		constants.clipToPrevClip = Multiply(clip_to_world, previous);
		Inverted(constants.clipToPrevClip, constants.prevClipToClip);
	} else {
		continuous = false;
	}
	previous       = view_proj;
	previous_valid = true;
	const float width      = static_cast<float>(extent.width);
	const float height     = static_cast<float>(extent.height);
	constants.jitterOffset = sl::float2(jitter_x * width * 0.5f, -jitter_y * height * 0.5f);
	constants.mvecScale    = sl::float2(MotionVectorSign(), MotionVectorSign());
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
	return continuous;
}
#endif

#ifdef KYTY_FRAMEGEN_STREAMLINE
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
	// KYTY_FRAMEGEN_FPS_CAP=<fps> (the launcher's Frame rate limit): Reflex holds the rendered
	// (base) frames to at most this rate, so DLSS-G paces evenly spaced frames instead of the
	// emulator's uneven ones. 0 or unset: no cap.
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

#ifdef KYTY_UPSCALER_CODE
bool LoadFsr(); // (with FSR's state, below)
#endif
#ifdef KYTY_FRAMEGEN_STREAMLINE
bool LoadFsrModule(const char* who);
#endif
// AMD FSR 3.1 frame generation (KYTY_FRAMEGEN_BACKEND=fsr): asked for, and ready (runtime and queues).
bool              g_fsr_fg = false;
std::atomic<bool> g_fsr_fg_ready {false};
// Built in (KYTY_FSR_NATIVE: Linux, macOS): asked for; the presenter shows the generated frames.
bool g_fsr_fg_native = false;
std::atomic<bool> g_fsr_fg_native_reset {false}; // the swapchain was made again (SwapchainChanging)

} // namespace

void Initialize() {
	const char* value = std::getenv("KYTY_FRAMEGEN");
	g_generate        = value != nullptr && value[0] != '\0' ? std::clamp(std::atoi(value), 0, 3) : 1;
	const char* dlss  = std::getenv("KYTY_DLSS");
	g_upscale         = dlss != nullptr && dlss[0] != '\0' && dlss[0] != '0';
	const char* fsr   = std::getenv("KYTY_FSR");
	g_fsr             = fsr != nullptr && fsr[0] != '\0' && fsr[0] != '0';
	if (g_fsr) {
		g_upscale = false; // (one upscaler: FSR, which needs no Streamline)
	}
	// KYTY_FRAMEGEN_BACKEND=fsr: AMD FSR 3.1 frame generation (2x) in place of DLSS-G.
	if (const char* backend = std::getenv("KYTY_FRAMEGEN_BACKEND");
	    backend != nullptr && std::strcmp(backend, "fsr") == 0 && g_generate > 0) {
		g_generate = 0; // (no Streamline frame generation)
#ifdef KYTY_FRAMEGEN_STREAMLINE
		if (g_upscale) {
			// (DLSS upscaling makes Streamline the Vulkan loader; FSR's swapchain goes to the system's.)
			std::printf("Frame generation: FSR frame generation does not run with DLSS upscaling; use FSR "
			            "upscaling or the game's. Frame generation off\n");
		} else if (LoadFsrModule("FSR frame generation")) {
			g_fsr_fg = true;
			std::printf("Frame generation: AMD FSR 3.1 frame generation asked for (2x)\n");
		}
#elif defined(KYTY_FSR_NATIVE)
		g_fsr_fg_native = true;
		std::printf("Frame generation: AMD FSR 3.1 frame generation asked for (2x, built in)\n");
#else
		std::printf("Frame generation: FSR frame generation is not in this build\n");
#endif
		std::fflush(stdout);
	}
	if (g_generate == 0 && !g_upscale) {
		return;
	}
#ifdef KYTY_FRAMEGEN_STREAMLINE
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
	    !load(g_api.set_tag_for_frame, "slSetTagForFrame") ||
	    !load(g_api.evaluate_feature, "slEvaluateFeature")) {
		std::printf("Frame generation: sl.interposer.dll lacks an entry point\n");
		std::fflush(stdout);
		return;
	}
	static const std::wstring    plugin_dir = dir.wstring();
	static const wchar_t*        plugin_paths[] {plugin_dir.c_str()};
	static std::vector<sl::Feature> features;
	if (g_generate > 0) {
		features.insert(features.end(), {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL});
	}
	if (g_upscale) {
		features.push_back(sl::kFeatureDLSS);
	}
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
	LOGF("frame generation: Streamline loaded from %s, %d generated frame(s), DLSS upscaling %s\n",
	     dir.string().c_str(), g_generate, g_upscale ? "on" : "off");
	std::printf("Frame generation: Streamline loaded from %s (%d generated frame(s) per frame; DLSS "
	            "upscaling %s)\n",
	            dir.string().c_str(), g_generate, g_upscale ? "asked for" : "off");
	std::fflush(stdout);
#else
	LOGF("frame generation: this build has no Streamline support\n");
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
	return g_loaded && g_generate > 0;
}

bool Enabled() {
	return g_ready.load(std::memory_order_relaxed);
}

bool Gathering() {
	return g_ready.load(std::memory_order_relaxed) || g_sr_ready.load(std::memory_order_relaxed) ||
	       g_fsr_fg_ready.load(std::memory_order_relaxed);
}

bool UpscaleEnabled() {
	return g_sr_ready.load(std::memory_order_relaxed);
}

std::string SupportProblem(vk::PhysicalDevice physical_device) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!Requested()) return {};
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
	if (g_fsr) {
		// FSR's runtime looks these up under their extension names (vkGetBufferMemoryRequirements2KHR)
		// when the GPU offers dedicated allocations, and calls them unchecked: the extensions,
		// promoted to Vulkan 1.1, are enabled so the names exist on the device.
		for (const char* name: {VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
		                        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME}) {
			const bool offered = std::any_of(available.begin(), available.end(), [&](const vk::ExtensionProperties& item) {
				return std::strcmp(item.extensionName.data(), name) == 0;
			});
			const bool added = std::any_of(extensions.begin(), extensions.end(),
			                               [&](const char* have) { return std::strcmp(have, name) == 0; });
			if (offered && !added) {
				extensions.push_back(name);
			}
		}
	}
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
	if (!g_loaded || std::getenv("KYTY_FRAMEGEN_HOOKS") != nullptr) return;
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
#ifdef KYTY_UPSCALER_CODE
	if (g_fsr) {
		(void)LoadFsr();
	}
#endif
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_loaded) return;
	sl::AdapterInfo adapter {};
	adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
	if (g_upscale) {
		bool       loaded    = false;
		const auto supported = g_api.is_feature_supported(sl::kFeatureDLSS, adapter);
		void*      set_options = nullptr;
		void*      optimal     = nullptr;
		if (supported != sl::Result::eOk ||
		    g_api.is_feature_loaded(sl::kFeatureDLSS, loaded) != sl::Result::eOk || !loaded ||
		    g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", set_options) !=
		        sl::Result::eOk ||
		    set_options == nullptr ||
		    g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", optimal) !=
		        sl::Result::eOk ||
		    optimal == nullptr) {
			std::printf("DLSS upscaling: not available on this device (%s)\n",
			            sl::getResultAsStr(supported));
		} else {
			g_api.dlss_set_options      = reinterpret_cast<PFun_slDLSSSetOptions*>(set_options);
			g_api.dlss_optimal_settings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(optimal);
			g_sr_ready.store(true, std::memory_order_relaxed);
			std::printf("DLSS upscaling: ready; it replaces the game's TAA (use a \"Fixed render "
			            "resolution\" patch for a lower render resolution)\n");
		}
		std::fflush(stdout);
	}
	if (!Requested()) return;
	bool loaded = false;
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
	if (!Gathering()) return;
	// The TAA pass: its first image is the motion vectors.
	if (IsTaaPixelShader(pixel_shader)) {
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
	if (!Gathering()) return;
	g_flips.fetch_add(1, std::memory_order_relaxed);
	Gathered gathered;
	{
		std::lock_guard lock(g_mutex);
		gathered   = g_gathered;
		g_gathered = {};
	}
	if (!g_ready && !g_fsr_fg_ready.load(std::memory_order_relaxed)) return;
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
		std::printf("Frame generation: the game's inputs found (motion and depth %ux%u, vertical FOV "
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
	inputs.valid = false;
}

#ifdef KYTY_UPSCALER_CODE
namespace {

// DLSS super resolution's own state (GPU thread; Streamline's viewport 1).
struct Upscaler {
	VulkanImage   color, depth, motion, output;
	vk::ImageView color_view = nullptr, depth_view = nullptr, motion_view = nullptr,
	              output_view = nullptr;
	vk::Extent2D  options_in {}, options_out {};
	bool          options_set   = false;
	Mat4          previous_view_proj;
	bool          previous_valid = false;
	uint64_t      last_flip      = 0;
	uint32_t      frames         = 0;
	uint32_t      failures       = 0; // in a row
	uint64_t      evaluated      = 0;
	bool          disabled       = false;
	bool          reported       = false;
	float         jitter_min_x = 1e9f, jitter_max_x = -1e9f, jitter_min_y = 1e9f, jitter_max_y = -1e9f;
	uint32_t      resets       = 0;
};
Upscaler g_sr;
[[maybe_unused]] constexpr uint32_t UpscaleViewport = 1;

float EnvSign(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr && std::atof(value) < 0 ? -1.0f : 1.0f;
}

// (Re)creates the DLSS output: a storage image of the TAA output's size and format.
bool ConfigureOutput(GraphicContext& graphics, vk::Extent2D size, vk::Format format) {
	auto& image = g_sr.output;
	if (image.image != nullptr && image.format == format && image.extent.width == size.width &&
	    image.extent.height == size.height) {
		return true;
	}
	if (g_sr.output_view != nullptr) {
		graphics.device.destroyImageView(g_sr.output_view, nullptr);
		g_sr.output_view = nullptr;
	}
	if (image.image != nullptr) {
		graphics.DeleteImage(image);
	}
	vk::ImageCreateInfo create {};
	create.imageType     = vk::ImageType::e2D;
	create.extent        = vk::Extent3D {size.width, size.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
	               vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, image)) {
		std::printf("DLSS upscaling: cannot make a %ux%u output image (format %d)\n", size.width,
		            size.height, static_cast<int>(format));
		return false;
	}
	vk::ImageViewCreateInfo view_info {};
	view_info.image            = image.image;
	view_info.viewType         = vk::ImageViewType::e2D;
	view_info.format           = format;
	view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	if (graphics.device.createImageView(&view_info, nullptr, &g_sr.output_view) != vk::Result::eSuccess) {
		g_sr.output_view = nullptr;
		graphics.DeleteImage(image);
		return false;
	}
	return true;
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
const char* ModeName(sl::DLSSMode mode) {
	switch (mode) {
		case sl::DLSSMode::eDLAA: return "DLAA";
		case sl::DLSSMode::eUltraQuality: return "Ultra Quality";
		case sl::DLSSMode::eMaxQuality: return "Quality";
		case sl::DLSSMode::eBalanced: return "Balanced";
		case sl::DLSSMode::eMaxPerformance: return "Performance";
		case sl::DLSSMode::eUltraPerformance: return "Ultra Performance";
		default: return "?";
	}
}

// The DLSS mode whose render size range holds the game's render size (the game decides it).
sl::DLSSMode PickMode(vk::Extent2D in, vk::Extent2D out) {
	constexpr sl::DLSSMode modes[] {sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality,
	                                sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance,
	                                sl::DLSSMode::eUltraPerformance};
	sl::DLSSMode best      = sl::DLSSMode::eMaxQuality;
	uint64_t     best_diff = UINT64_MAX;
	for (const auto mode: modes) {
		sl::DLSSOptions options {};
		options.mode         = mode;
		options.outputWidth  = out.width;
		options.outputHeight = out.height;
		sl::DLSSOptimalSettings settings {};
		if (g_api.dlss_optimal_settings(options, settings) != sl::Result::eOk) continue;
		const bool min_ok = settings.renderWidthMin == 0 ||
		                    (in.width >= settings.renderWidthMin && in.height >= settings.renderHeightMin);
		const bool max_ok = settings.renderWidthMax == 0 ||
		                    (in.width <= settings.renderWidthMax && in.height <= settings.renderHeightMax);
		if (min_ok && max_ok && settings.renderWidthMin != 0) return mode;
		const uint64_t diff = static_cast<uint64_t>(std::abs(static_cast<int64_t>(settings.optimalRenderWidth) -
		                                                     static_cast<int64_t>(in.width)));
		if (diff < best_diff) {
			best_diff = diff;
			best      = mode;
		}
	}
	return best;
}
#endif

void UpscaleFailed(const char* what, const char* detail) {
	const char* name = g_fsr ? "FSR upscaling" : "DLSS upscaling";
	++g_sr.failures;
	if (g_sr.failures <= 3) {
		std::printf("%s: %s failed (%s); the game's TAA is used\n", name, what, detail);
		std::fflush(stdout);
	}
	if (g_sr.failures >= 60 && !g_sr.disabled) {
		g_sr.disabled = true;
		std::printf("%s: off after repeated failures; the game's TAA is used\n", name);
		std::fflush(stdout);
	}
}

// vkGetDeviceProcAddr for FSR's runtime: an entry point the device does not give under its
// extension name (a KHR/EXT name of a core function whose extension is not enabled, e.g.
// vkGetBufferMemoryRequirements2KHR) is looked up under its core name; one missing even so is
// logged (FSR's runtime calls some without checking).
VkInstance g_fsr_instance = VK_NULL_HANDLE; // (FsrGetDeviceProcAddr's fallback)

PFN_vkVoidFunction VKAPI_CALL FsrGetDeviceProcAddr(VkDevice device, const char* name);

// What FsrDispatch needs of the frame (the images are g_sr's).
struct FsrFrame {
	vk::CommandBuffer command;
	vk::Extent2D      in, out;
	float             jitter_x = 0.0f, jitter_y = 0.0f;
	bool              reset      = false;
	float             near_plane = 0.1f, far_plane = 1000.0f, fov = 1.0f;
};

void FsrFrameTime(std::chrono::steady_clock::time_point& last, uint64_t dispatches, float& frame_ms) {
	const auto now = std::chrono::steady_clock::now();
	frame_ms       = dispatches == 0
	                     ? 16.7f
	                     : std::clamp(std::chrono::duration<float, std::milli>(now - last).count(), 1.0f, 100.0f);
	last = now;
}

float FsrSharpnessFromEnv() {
	if (const char* value = std::getenv("KYTY_FSR_SHARPNESS"); value != nullptr && value[0] != '\0') {
		return std::clamp(static_cast<float>(std::atof(value)), 0.0f, 1.0f);
	}
	return 0.0f;
}

const char* FsrQualityName(vk::Extent2D in, vk::Extent2D out) {
	const float ratio = static_cast<float>(out.height) / static_cast<float>(std::max(in.height, 1u));
	return ratio < 1.05f   ? "native AA"
	       : ratio < 1.6f  ? "Quality"
	       : ratio < 1.85f ? "Balanced"
	       : ratio < 2.5f  ? "Performance"
	                       : "Ultra Performance";
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
// AMD FidelityFX FSR 3.1 (KYTY_FSR=1): the FidelityFX API's Vulkan runtime, amd_fidelityfx_vk.dll
// next to the emulator (FidelityFX SDK v1.1.4, MIT), loaded at the device (LoadFsr).
struct FsrApi {
	HMODULE              module   = nullptr;
	PfnFfxCreateContext  create   = nullptr;
	PfnFfxDestroyContext destroy  = nullptr;
	PfnFfxDispatch       dispatch = nullptr;
	PfnFfxConfigure      configure = nullptr;
	PfnFfxQuery          query     = nullptr;
	ffxContext           context  = nullptr;
	vk::Extent2D         in {}, out {};
	// Contexts of an earlier size, destroyed once frames in flight can no longer use them.
	std::vector<std::pair<ffxContext, uint64_t>> retired;
	uint64_t                                     dispatches = 0;
	std::chrono::steady_clock::time_point        last {};
	float                                        sharpness = 0.0f;
};
FsrApi g_fsr_api;

void FsrMessage(uint32_t type, const wchar_t* message) {
	static std::atomic<uint32_t> count {0};
	if (message == nullptr || count.fetch_add(1, std::memory_order_relaxed) >= 20) return;
	std::printf("FSR %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
	std::fflush(stdout);
}

void RetireFsrContexts(bool all) {
	auto& retired = g_fsr_api.retired;
	for (auto it = retired.begin(); it != retired.end();) {
		if (all || g_fsr_api.dispatches >= it->second + 30) {
			(void)g_fsr_api.destroy(&it->first, nullptr);
			it = retired.erase(it);
		} else {
			++it;
		}
	}
}

#endif

PFN_vkVoidFunction VKAPI_CALL FsrGetDeviceProcAddr(VkDevice device, const char* name) {
	if (name == nullptr) return nullptr;
	auto&      dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
	const auto lookup     = [&](const char* entry) -> PFN_vkVoidFunction {
		if (dispatcher.vkGetDeviceProcAddr != nullptr) {
			if (auto* function = dispatcher.vkGetDeviceProcAddr(device, entry); function != nullptr) {
				return function;
			}
		}
		// The loader's trampolines dispatch any device's call too.
		if (dispatcher.vkGetInstanceProcAddr != nullptr && g_fsr_instance != VK_NULL_HANDLE) {
			return dispatcher.vkGetInstanceProcAddr(g_fsr_instance, entry);
		}
		return nullptr;
	};
	if (auto* function = lookup(name); function != nullptr) return function;
	std::string core(name);
	for (const char* suffix: {"KHR", "EXT"}) {
		if (core.size() > 3 && core.compare(core.size() - 3, 3, suffix) == 0) {
			core.resize(core.size() - 3);
			if (auto* function = lookup(core.c_str()); function != nullptr) {
				std::printf("FSR upscaling: %s taken as %s\n", name, core.c_str());
				std::fflush(stdout);
				return function;
			}
			break;
		}
	}
	static std::atomic<uint32_t> missing {0};
	if (missing.fetch_add(1, std::memory_order_relaxed) < 40) {
		std::printf("FSR upscaling: the device has no %s (getDeviceProcAddr %p)\n", name,
		            reinterpret_cast<void*>(dispatcher.vkGetDeviceProcAddr));
		std::fflush(stdout);
	}
	return nullptr;
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
// The FSR context for this render and output size (made again when either changes).
bool EnsureFsrContext(GraphicContext& graphics, vk::Extent2D in, vk::Extent2D out, bool mv_jittered) {
	if (g_fsr_api.context != nullptr && g_fsr_api.in == in && g_fsr_api.out == out) {
		return true;
	}
	if (g_fsr_api.context != nullptr) {
		g_fsr_api.retired.emplace_back(g_fsr_api.context, g_fsr_api.dispatches);
		g_fsr_api.context = nullptr;
	}
	ffxCreateBackendVKDesc backend {};
	backend.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	backend.vkDevice         = static_cast<VkDevice>(graphics.device);
	backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	backend.vkDeviceProcAddr = FsrGetDeviceProcAddr;
	g_fsr_instance           = static_cast<VkInstance>(graphics.instance);
	ffxCreateContextDescUpscale desc {};
	desc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	desc.header.pNext = &backend.header;
	desc.flags        = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
	             (mv_jittered ? FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0u);
	desc.maxRenderSize  = {in.width, in.height};
	desc.maxUpscaleSize = {out.width, out.height};
	desc.fpMessage      = FsrMessage;
	ffxContext context  = nullptr;
	std::printf("FSR upscaling: making its context (%ux%u to %ux%u)\n", in.width, in.height, out.width,
	            out.height);
	std::fflush(stdout);
	if (const auto code = g_fsr_api.create(&context, &desc.header, nullptr);
	    code != FFX_API_RETURN_OK || context == nullptr) {
		UpscaleFailed("ffxCreateContext", ("error " + std::to_string(code)).c_str());
		return false;
	}
	g_fsr_api.context = context;
	g_fsr_api.in      = in;
	g_fsr_api.out     = out;
	g_sr.previous_valid = false;
	std::printf("FSR upscaling: %ux%u to %ux%u (%.2fx, %s)\n", in.width, in.height, out.width,
	            out.height, static_cast<float>(out.height) / static_cast<float>(std::max(in.height, 1u)),
	            FsrQualityName(in, out));
	std::fflush(stdout);
	return true;
}

FfxApiResource FsrResource(const VulkanImage& image, uint32_t state) {
	VkImageCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType   = VK_IMAGE_TYPE_2D;
	info.format      = static_cast<VkFormat>(image.format);
	info.extent      = {image.extent.width, image.extent.height, 1};
	info.mipLevels   = 1;
	info.arrayLayers = 1;
	info.usage       = static_cast<VkImageUsageFlags>(image.usage);
	const auto vk_image = static_cast<VkImage>(image.image);
	return ffxApiGetResourceVK(reinterpret_cast<void*>(vk_image),
	                           ffxApiGetImageResourceDescriptionVK(vk_image, info, 0), state);
}

// AMD's runtime, once (FSR upscaling and FSR frame generation share it). `who`: for the messages.
bool LoadFsrModule(const char* who) {
	if (g_fsr_api.module != nullptr) return true;
	wchar_t exe[MAX_PATH] {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const auto path   = std::filesystem::path(exe).parent_path() / "amd_fidelityfx_vk.dll";
	HMODULE    module = LoadLibraryW(path.c_str());
	if (module == nullptr) {
		std::printf("%s: amd_fidelityfx_vk.dll was not found next to kyty_emulator.exe; off\n", who);
		std::fflush(stdout);
		return false;
	}
	const auto load = [&](auto& function, const char* function_name) {
		function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
		    reinterpret_cast<void*>(GetProcAddress(module, function_name)));
		return function != nullptr;
	};
	if (!load(g_fsr_api.create, "ffxCreateContext") || !load(g_fsr_api.destroy, "ffxDestroyContext") ||
	    !load(g_fsr_api.dispatch, "ffxDispatch") || !load(g_fsr_api.configure, "ffxConfigure") ||
	    !load(g_fsr_api.query, "ffxQuery")) {
		std::printf("%s: amd_fidelityfx_vk.dll lacks an entry point; off\n", who);
		std::fflush(stdout);
		return false;
	}
	g_fsr_api.module = module;
	return true;
}

bool LoadFsr() {
	if (!LoadFsrModule("FSR upscaling")) return false;
	g_fsr_api.sharpness = FsrSharpnessFromEnv();
	g_sr_ready.store(true, std::memory_order_relaxed);
	std::printf("FSR upscaling: ready (AMD FidelityFX FSR 3.1); it replaces the game's TAA (sharpening "
	            "%.2f, KYTY_FSR_SHARPNESS)\n",
	            g_fsr_api.sharpness);
	std::fflush(stdout);
	return true;
}

bool FsrDispatch(const FsrFrame& frame, std::string& failure) {
	float frame_ms = 16.7f;
	FsrFrameTime(g_fsr_api.last, g_fsr_api.dispatches, frame_ms);
	ffxDispatchDescUpscale dispatch {};
	dispatch.header.type       = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	dispatch.commandList       = static_cast<VkCommandBuffer>(frame.command);
	dispatch.color             = FsrResource(g_sr.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch.depth             = FsrResource(g_sr.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch.motionVectors     = FsrResource(g_sr.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch.output            = FsrResource(g_sr.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	dispatch.jitterOffset      = {frame.jitter_x, frame.jitter_y};
	// The game's UV-unit vectors (current minus previous) to FSR's render-size pixels.
	dispatch.motionVectorScale = {MotionVectorSign() * static_cast<float>(frame.in.width),
	                              MotionVectorSign() * static_cast<float>(frame.in.height)};
	dispatch.renderSize        = {frame.in.width, frame.in.height};
	dispatch.upscaleSize       = {frame.out.width, frame.out.height};
	dispatch.enableSharpening  = g_fsr_api.sharpness > 0.0f;
	dispatch.sharpness         = g_fsr_api.sharpness;
	dispatch.frameTimeDelta    = frame_ms;
	dispatch.preExposure       = 1.0f;
	dispatch.reset             = frame.reset;
	dispatch.cameraNear        = frame.near_plane;
	dispatch.cameraFar         = frame.far_plane;
	dispatch.cameraFovAngleVertical  = frame.fov;
	dispatch.viewSpaceToMetersFactor = 1.0f;
	dispatch.flags                   = 0;
	const auto code = g_fsr_api.dispatch(&g_fsr_api.context, &dispatch.header);
	g_fsr_api.dispatches++;
	RetireFsrContexts(false);
	if (code != FFX_API_RETURN_OK) {
		failure = "ffxDispatch error " + std::to_string(code);
		return false;
	}
	return true;
}
#else
// AMD FidelityFX FSR 3 (KYTY_FSR=1) built in from the FidelityFX SDK 1.1.4 sources
// (3rdparty/fidelityfx, MIT; from chenxiao07/KytyPS5 PR #117): its native upscaler API on the
// SDK's Vulkan backend. No runtime file is needed.
struct FsrNative {
	VkDeviceContext                       device_context {};
	void*                                 scratch = nullptr;
	FfxInterface                          backend {};
	alignas(64) FfxFsr3UpscalerContext    context {};
	uint32_t                              shared_context = 0;
	FfxResourceInternal                   previous_depth {}, dilated_depth {}, dilated_motion {};
	bool                                  created = false;
	bool                                  failed  = false;
	vk::Extent2D                          in {}, out {};
	std::chrono::steady_clock::time_point last {};
	uint64_t                              dispatches = 0;
	float                                 sharpness  = 0.0f;
	GraphicContext*                       graphics   = nullptr;
};
FsrNative g_fsr_native;

void FsrNativeMessage(FfxMsgType type, const wchar_t* message) {
	static std::atomic<uint32_t> count {0};
	if (message == nullptr || count.fetch_add(1, std::memory_order_relaxed) >= 20) return;
	std::printf("FSR %s: %ls\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
	std::fflush(stdout);
}

// Destroying the contexts needs the GPU done with what referenced them (a size change: rare).
void DestroyFsrNative() {
	auto& f = g_fsr_native;
	if (!f.created) return;
	if (f.graphics != nullptr) {
		Common::LockGuard lock(f.graphics->queue_mutex);
		(void)f.graphics->queue.waitIdle();
	}
	auto& b = f.backend;
	b.fpDestroyResource(&b, f.previous_depth, f.shared_context);
	b.fpDestroyResource(&b, f.dilated_depth, f.shared_context);
	b.fpDestroyResource(&b, f.dilated_motion, f.shared_context);
	b.fpDestroyBackendContext(&b, f.shared_context);
	ffxFsr3UpscalerContextDestroy(&f.context);
	std::free(f.scratch);
	f.scratch = nullptr;
	f.created = false;
}

bool EnsureFsrContext(GraphicContext& graphics, vk::Extent2D in, vk::Extent2D out, bool mv_jittered) {
	auto& f = g_fsr_native;
	if (f.failed) return false;
	if (f.created && f.in == in && f.out == out) return true;
	DestroyFsrNative();
	f.graphics = &graphics;
	g_fsr_instance = static_cast<VkInstance>(graphics.instance);
	f.device_context.vkDevice         = static_cast<VkDevice>(graphics.device);
	f.device_context.vkPhysicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	f.device_context.vkDeviceProcAddr = FsrGetDeviceProcAddr;
	std::printf("FSR upscaling: making its context (%ux%u to %ux%u)\n", in.width, in.height, out.width,
	            out.height);
	std::fflush(stdout);
	const auto fail = [&](const char* what, FfxErrorCode code) {
		f.failed = true; // (no retry each frame: the game's TAA from now on)
		UpscaleFailed(what, ("error 0x" + [&] {
			char hex[16];
			std::snprintf(hex, sizeof(hex), "%x", static_cast<unsigned>(code));
			return std::string(hex);
		}()).c_str());
		return false;
	};
	// (The SDK keeps structures with SIMD members in its scratch memory: more than its declared
	// alignment.)
	const size_t size = (ffxGetScratchMemorySizeVK(f.device_context.vkPhysicalDevice, 2) + 63) & ~size_t {63};
	f.scratch         = nullptr;
	if (posix_memalign(&f.scratch, 64, size) != 0 || f.scratch == nullptr) {
		f.scratch = nullptr;
		return fail("scratch memory", FFX_ERROR_OUT_OF_MEMORY);
	}
	std::memset(f.scratch, 0, size);
	if (const auto code = ffxGetInterfaceVK(&f.backend, ffxGetDeviceVK(&f.device_context), f.scratch, size, 2);
	    code != FFX_OK) {
		return fail("ffxGetInterfaceVK", code);
	}
	FfxFsr3UpscalerContextDescription description {};
	// The game's linear HDR scene; OpenGL-style depth, not reversed (docs/RE-ASTRO-BOT-FG.md).
	description.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE |
	                    (mv_jittered ? FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0u);
	description.maxRenderSize    = {in.width, in.height};
	description.maxUpscaleSize   = {out.width, out.height};
	description.backendInterface = f.backend;
	description.fpMessage        = FsrNativeMessage;
	if (const auto code = ffxFsr3UpscalerContextCreate(&f.context, &description); code != FFX_OK) {
		return fail("ffxFsr3UpscalerContextCreate", code);
	}
	auto& b = f.backend;
	if (const auto code = b.fpCreateBackendContext(&b, FFX_EFFECT_SHAREDRESOURCES, nullptr, &f.shared_context);
	    code != FFX_OK) {
		ffxFsr3UpscalerContextDestroy(&f.context);
		return fail("its shared resources", code);
	}
	FfxFsr3UpscalerSharedResourceDescriptions shared {};
	FfxErrorCode code = ffxFsr3UpscalerGetSharedResourceDescriptions(&f.context, &shared);
	if (code == FFX_OK) code = b.fpCreateResource(&b, &shared.reconstructedPrevNearestDepth, f.shared_context, &f.previous_depth);
	if (code == FFX_OK) code = b.fpCreateResource(&b, &shared.dilatedDepth, f.shared_context, &f.dilated_depth);
	if (code == FFX_OK) code = b.fpCreateResource(&b, &shared.dilatedMotionVectors, f.shared_context, &f.dilated_motion);
	if (code != FFX_OK) {
		b.fpDestroyBackendContext(&b, f.shared_context);
		ffxFsr3UpscalerContextDestroy(&f.context);
		return fail("its shared resources", code);
	}
	f.created           = true;
	f.in                = in;
	f.out               = out;
	f.dispatches        = 0;
	g_sr.previous_valid = false;
	std::printf("FSR upscaling: %ux%u to %ux%u (%.2fx, %s)\n", in.width, in.height, out.width,
	            out.height, static_cast<float>(out.height) / static_cast<float>(std::max(in.height, 1u)),
	            FsrQualityName(in, out));
	std::fflush(stdout);
	return true;
}

FfxResource FsrNativeResource(const VulkanImage& image, const wchar_t* name, bool written) {
	VkImageCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType   = VK_IMAGE_TYPE_2D;
	info.format      = static_cast<VkFormat>(image.format);
	info.extent      = {image.extent.width, image.extent.height, 1};
	info.mipLevels   = 1;
	info.arrayLayers = 1;
	info.samples     = VK_SAMPLE_COUNT_1_BIT;
	info.usage       = static_cast<VkImageUsageFlags>(image.usage);
	const auto vk_image    = static_cast<VkImage>(image.image);
	const auto description = ffxGetImageResourceDescriptionVK(
	    vk_image, info, written ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY);
	return ffxGetResourceVK(reinterpret_cast<void*>(vk_image), description, name,
	                        written ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
}

bool LoadFsr() {
	g_fsr_native.sharpness = FsrSharpnessFromEnv();
	g_sr_ready.store(true, std::memory_order_relaxed);
	std::printf("FSR upscaling: ready (AMD FidelityFX FSR 3, built in); it replaces the game's TAA "
	            "(sharpening %.2f, KYTY_FSR_SHARPNESS)\n",
	            g_fsr_native.sharpness);
	std::fflush(stdout);
	return true;
}

bool FsrDispatch(const FsrFrame& frame, std::string& failure) {
	auto& f        = g_fsr_native;
	float frame_ms = 16.7f;
	FsrFrameTime(f.last, f.dispatches, frame_ms);
	auto&                              b = f.backend;
	FfxFsr3UpscalerDispatchDescription dispatch {};
	dispatch.commandList   = ffxGetCommandListVK(static_cast<VkCommandBuffer>(frame.command));
	dispatch.color         = FsrNativeResource(g_sr.color, L"KytyFsrColor", false);
	dispatch.depth         = FsrNativeResource(g_sr.depth, L"KytyFsrDepth", false);
	dispatch.motionVectors = FsrNativeResource(g_sr.motion, L"KytyFsrMotion", false);
	dispatch.output        = FsrNativeResource(g_sr.output, L"KytyFsrOutput", true);
	dispatch.jitterOffset  = {frame.jitter_x, frame.jitter_y};
	// The game's UV-unit vectors (current minus previous) to FSR's render-size pixels.
	dispatch.motionVectorScale = {MotionVectorSign() * static_cast<float>(frame.in.width),
	                              MotionVectorSign() * static_cast<float>(frame.in.height)};
	dispatch.renderSize        = {frame.in.width, frame.in.height};
	dispatch.upscaleSize       = {frame.out.width, frame.out.height};
	dispatch.enableSharpening  = f.sharpness > 0.0f;
	dispatch.sharpness         = f.sharpness;
	dispatch.frameTimeDelta    = frame_ms;
	dispatch.preExposure       = 1.0f;
	dispatch.reset             = frame.reset;
	dispatch.cameraNear        = frame.near_plane;
	dispatch.cameraFar         = frame.far_plane;
	dispatch.cameraFovAngleVertical        = frame.fov;
	dispatch.viewSpaceToMetersFactor       = 1.0f;
	dispatch.dilatedDepth                  = b.fpGetResource(&b, f.dilated_depth);
	dispatch.dilatedMotionVectors          = b.fpGetResource(&b, f.dilated_motion);
	dispatch.reconstructedPrevNearestDepth = b.fpGetResource(&b, f.previous_depth);
	const auto code = ffxFsr3UpscalerContextDispatch(&f.context, &dispatch);
	f.dispatches++;
	if (code != FFX_OK) {
		char text[48];
		std::snprintf(text, sizeof(text), "ffxFsr3UpscalerContextDispatch error 0x%x", static_cast<unsigned>(code));
		failure = text;
		return false;
	}
	return true;
}
#endif

uint64_t DescriptorAddress(std::span<const uint32_t> user_data, size_t dword) {
	if (dword + 1 >= user_data.size()) return 0;
	// T# / V# words 0-1: the base address, 256-byte units.
	return ((static_cast<uint64_t>(user_data[dword + 1] & 0xffu) << 32) | user_data[dword]) << 8;
}

} // namespace
#endif

bool UpscaleTaa(GraphicContext& graphics, TextureCache& cache, CommandBuffer& command,
                const PreparedBindings& pixel, std::span<const uint32_t> user_data,
                std::span<const RenderColorInfo> colors) {
#ifdef KYTY_UPSCALER_CODE
	if (!g_sr_ready.load(std::memory_order_relaxed) || g_sr.disabled) return false;
	// Astro Bot's TAA pass (docs/RE-ASTRO-BOT-FG.md): the current colour's T# at user data 0, the
	// history's at 8, the motion vectors' at 16, its constants buffer (80 bytes: the jitter in UV
	// at 0, the output size at 16); colour target 0 is its output, 1 the next frame's history.
	if (colors.size() < 2 || !colors[0].image_id || !colors[1].image_id ||
	    colors[0].guest_mip_level != 0 || colors[1].guest_mip_level != 0 ||
	    colors[0].guest_array_layer != 0 || colors[1].guest_array_layer != 0) {
		return false;
	}
	const uint64_t color_address  = DescriptorAddress(user_data, 0);
	const uint64_t motion_address = DescriptorAddress(user_data, 16);
	const TextureBinding* color_binding  = nullptr;
	const TextureBinding* motion_binding = nullptr;
	for (const auto& image: pixel.images) {
		const auto address = image.desc.info.data.address;
		if (address == color_address && color_binding == nullptr) color_binding = &image;
		if (address == motion_address && motion_binding == nullptr) motion_binding = &image;
	}
	if (color_binding == nullptr || motion_binding == nullptr || !color_binding->image_id ||
	    !motion_binding->image_id || color_address == 0 || motion_address == 0) {
		return false;
	}
	const uint32_t* taa = nullptr;
	for (const auto& source: pixel.buffer_sources) {
		if (source.address >= 0x10000 && source.size >= 64 && source.size <= 256) {
			// Guest memory is mapped at the same addresses in the emulator; the CPU writes these.
			taa = reinterpret_cast<const uint32_t*>(source.address);
			break;
		}
	}
	if (taa == nullptr) return false;
	Gathered gathered;
	{
		std::lock_guard lock(g_mutex);
		gathered = g_gathered;
	}
	if (!gathered.gbuffer || !gathered.camera.valid || gathered.depth_address == 0) return false;

	auto& color  = cache.GetImage(color_binding->image_id);
	auto& motion = cache.GetImage(motion_binding->image_id);
	auto& out0   = cache.GetImage(colors[0].image_id);
	auto& out1   = cache.GetImage(colors[1].image_id);
	const auto depth_id = cache.FindImageFromRange(gathered.depth_address, gathered.depth_size, false);
	if (!depth_id) return false;
	auto& depth = cache.GetImage(depth_id);
	const auto same_extent = [](const Image& a, const Image& b) {
		return a.backing.extent.width == b.backing.extent.width &&
		       a.backing.extent.height == b.backing.extent.height;
	};
	if (color.backing.image == nullptr || motion.backing.image == nullptr ||
	    depth.backing.image == nullptr || out0.backing.image == nullptr ||
	    out1.backing.image == nullptr || color.info.data.address != color_address ||
	    motion.info.data.address != motion_address ||
	    depth.info.data.address != gathered.depth_address || !IsDepthFormat(depth.backing.format) ||
	    !same_extent(color, motion) || !same_extent(color, depth) || !same_extent(out0, out1) ||
	    out0.backing.format != out1.backing.format || &out0 == &out1 ||
	    color.backing.extent.width > out0.backing.extent.width ||
	    color.backing.extent.height > out0.backing.extent.height) {
		if (!g_sr.reported) {
			g_sr.reported = true;
			std::printf("%s: the TAA pass's images are not as expected (colour %ux%u, "
			            "motion %ux%u, depth %ux%u, output %ux%u); the game's TAA is used\n",
			            g_fsr ? "FSR upscaling" : "DLSS upscaling", color.backing.extent.width, color.backing.extent.height,
			            motion.backing.extent.width, motion.backing.extent.height,
			            depth.backing.extent.width, depth.backing.extent.height,
			            out0.backing.extent.width, out0.backing.extent.height);
			std::fflush(stdout);
		}
		return false;
	}
	const vk::Extent2D in {color.backing.extent.width, color.backing.extent.height};
	const vk::Extent2D out {out0.backing.extent.width, out0.backing.extent.height};
	if (!ConfigureOutput(graphics, out, out0.backing.format)) {
		g_sr.disabled = true;
		return false;
	}
	Configure(graphics, g_sr.color, g_sr.color_view, color.backing);
	Configure(graphics, g_sr.depth, g_sr.depth_view, depth.backing);
	Configure(graphics, g_sr.motion, g_sr.motion_view, motion.backing);

	const bool  fsr  = g_fsr;
	const char* name = fsr ? "FSR upscaling" : "DLSS upscaling";

	// The camera: Streamline's constants, also FSR's source (near, far, field of view, jitter).
	sl::Constants constants {};
	DefaultConstants(constants);
	const uint64_t flip = g_flips.load(std::memory_order_relaxed);
	bool reset = !CameraConstants(gathered.camera, in, g_sr.previous_view_proj, g_sr.previous_valid,
	                              constants);
	// Frames without the TAA pass (a menu, a loading screen) since the last one: the history is stale.
	reset = reset || flip > g_sr.last_flip + 4;
	g_sr.last_flip = flip;
	// The TAA's own unjitter offset (UV, added to the sample position): the jitter in pixels is
	// minus it (it agrees with the projection's). DLSS and FSR take the same convention: the
	// projection's offset in pixels (NDC x * width / 2, -NDC y * height / 2).
	float taa_jitter_x = 0.0f, taa_jitter_y = 0.0f;
	std::memcpy(&taa_jitter_x, &taa[0], sizeof(float));
	std::memcpy(&taa_jitter_y, &taa[1], sizeof(float));
	const float projection_x = constants.jitterOffset.x, projection_y = constants.jitterOffset.y;
	static const bool use_projection = [] {
		const char* value = std::getenv("KYTY_DLSS_JITTER");
		return value != nullptr && std::strcmp(value, "projection") == 0;
	}();
	static const float sign_x = EnvSign("KYTY_DLSS_JITTER_X"), sign_y = EnvSign("KYTY_DLSS_JITTER_Y");
	// (Astro's Playroom: its TAA constants hold no jitter, the projection does; a TAA jitter of
	// exactly zero with a jittered projection takes the projection's.)
	// (Under a thousandth of a pixel counts as none: Playroom's words there are tiny, not zero.)
	const float taa_px_x = std::fabs(taa_jitter_x * static_cast<float>(in.width));
	const float taa_px_y = std::fabs(taa_jitter_y * static_cast<float>(in.height));
	const bool  from_projection =
	    use_projection || (taa_px_x < 1e-3f && taa_px_y < 1e-3f &&
	                       (std::fabs(projection_x) >= 1e-3f || std::fabs(projection_y) >= 1e-3f));
	const float jitter_x = from_projection ? projection_x : -taa_jitter_x * static_cast<float>(in.width);
	const float jitter_y = from_projection ? projection_y : -taa_jitter_y * static_cast<float>(in.height);
	constants.jitterOffset = sl::float2(jitter_x * sign_x, jitter_y * sign_y);
	static const bool mv_jittered = [] {
		const char* value = std::getenv("KYTY_DLSS_MV_JITTERED");
		return value != nullptr && value[0] == '1';
	}();
	constants.motionVectorsJittered = mv_jittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	constants.reset                 = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

#ifdef KYTY_FRAMEGEN_STREAMLINE
	std::unique_lock sl_lock(g_sl_mutex, std::defer_lock);
	const sl::ViewportHandle viewport {UpscaleViewport};
	sl::FrameToken*          token = nullptr;
#endif
	if (fsr) {
		if (!EnsureFsrContext(graphics, in, out, mv_jittered)) {
			return false;
		}
	} else {
#ifdef KYTY_FRAMEGEN_STREAMLINE
		sl_lock.lock();
		if (!g_sr.options_set || g_sr.options_in != in || g_sr.options_out != out) {
			sl::DLSSOptions options {};
			options.mode            = PickMode(in, out);
			options.outputWidth     = out.width;
			options.outputHeight    = out.height;
			options.colorBuffersHDR = sl::Boolean::eTrue;
			options.useAutoExposure = sl::Boolean::eTrue;
			if (const auto result = g_api.dlss_set_options(viewport, options); result != sl::Result::eOk) {
				UpscaleFailed("slDLSSSetOptions", sl::getResultAsStr(result));
				return false;
			}
			g_sr.options_set    = true;
			g_sr.options_in     = in;
			g_sr.options_out    = out;
			std::printf("DLSS upscaling: %ux%u to %ux%u (%s)\n", in.width, in.height, out.width,
			            out.height, ModeName(options.mode));
			std::fflush(stdout);
		}
		// (Indices apart from the presents' DLSS-G frames, on their own viewport.)
		uint32_t index = 0x40000000u + ++g_sr.frames;
		if (const auto result = g_api.get_new_frame_token(token, &index);
		    result != sl::Result::eOk || token == nullptr) {
			UpscaleFailed("slGetNewFrameToken", sl::getResultAsStr(result));
			return false;
		}
		if (const auto result = g_api.set_constants(constants, *token, viewport); result != sl::Result::eOk) {
			UpscaleFailed("slSetConstants", sl::getResultAsStr(result));
			return false;
		}
#else
		return false; // (DLSS needs Streamline: Windows)
#endif
	}

	// Outside rendering, natively at this point of the frame.
	command.EndRendering();
	auto       vk_command = command.Handle();
	const auto keep_color = color.backing.state, keep_motion = motion.backing.state,
	           keep_depth = depth.backing.state;
	CopyInto(vk_command, color, g_sr.color);
	CopyInto(vk_command, depth, g_sr.depth);
	CopyInto(vk_command, motion, g_sr.motion);
	Transition(vk_command, g_sr.output, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
	           vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
	           vk::PipelineStageFlagBits2::eComputeShader);
	bool        ok = false;
	std::string failure;
	if (fsr) {
		FsrFrame frame;
		frame.command    = vk_command;
		frame.in         = in;
		frame.out        = out;
		frame.jitter_x   = constants.jitterOffset.x;
		frame.jitter_y   = constants.jitterOffset.y;
		frame.reset      = reset;
		frame.near_plane = constants.cameraNear;
		frame.far_plane  = constants.cameraFar;
		frame.fov        = constants.cameraFOV;
		ok               = FsrDispatch(frame, failure);
	} else {
#ifdef KYTY_FRAMEGEN_STREAMLINE
		const auto resource = [](VulkanImage& image, vk::ImageView view, vk::ImageLayout layout) {
			sl::Resource r(sl::ResourceType::eTex2d, static_cast<VkImage>(image.image), nullptr,
			               static_cast<VkImageView>(view), static_cast<uint32_t>(layout));
			r.width        = image.extent.width;
			r.height       = image.extent.height;
			r.nativeFormat = static_cast<uint32_t>(image.format);
			r.mipLevels    = 1;
			r.arrayLayers  = 1;
			r.flags        = 0;
			r.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(image.usage));
			return r;
		};
		sl::Resource color_resource  = resource(g_sr.color, g_sr.color_view, vk::ImageLayout::eShaderReadOnlyOptimal);
		sl::Resource depth_resource  = resource(g_sr.depth, g_sr.depth_view, vk::ImageLayout::eShaderReadOnlyOptimal);
		sl::Resource motion_resource = resource(g_sr.motion, g_sr.motion_view, vk::ImageLayout::eShaderReadOnlyOptimal);
		sl::Resource output_resource = resource(g_sr.output, g_sr.output_view, vk::ImageLayout::eGeneral);
		const sl::Extent in_extent {0, 0, in.width, in.height};
		const sl::Extent out_extent {0, 0, out.width, out.height};
		const sl::ResourceTag color_tag(&color_resource, sl::kBufferTypeScalingInputColor,
		                                sl::ResourceLifecycle::eValidUntilEvaluate, &in_extent);
		const sl::ResourceTag depth_tag(&depth_resource, sl::kBufferTypeDepth,
		                                sl::ResourceLifecycle::eValidUntilEvaluate, &in_extent);
		const sl::ResourceTag motion_tag(&motion_resource, sl::kBufferTypeMotionVectors,
		                                 sl::ResourceLifecycle::eValidUntilEvaluate, &in_extent);
		const sl::ResourceTag output_tag(&output_resource, sl::kBufferTypeScalingOutputColor,
		                                 sl::ResourceLifecycle::eValidUntilEvaluate, &out_extent);
		const sl::BaseStructure* inputs[] {&viewport, &color_tag, &depth_tag, &motion_tag, &output_tag};
		const auto result = g_api.evaluate_feature(sl::kFeatureDLSS, *token, inputs,
		                                           static_cast<uint32_t>(std::size(inputs)),
		                                           static_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(vk_command)));
		ok = result == sl::Result::eOk;
		if (!ok) failure = std::string("slEvaluateFeature ") + sl::getResultAsStr(result);
#endif
	}
	// (Streamline and FSR leave the command buffer's bindings as their passes left them.)
	command.ForgetBoundState();
	if (!ok) {
		// The game's TAA draws after all: its textures as its bindings expect them.
		color.Transit(keep_color.layout, keep_color.access_mask, {}, vk_command);
		motion.Transit(keep_motion.layout, keep_motion.access_mask, {}, vk_command);
		depth.Transit(keep_depth.layout, keep_depth.access_mask, {}, vk_command);
		UpscaleFailed("evaluation", failure.c_str());
		return false;
	}
	g_sr.failures = 0;
	// The output and the next frame's history (read by the game's reprojection passes too).
	Transition(vk_command, g_sr.output, vk::ImageAspectFlagBits::eColor,
	           vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
	           vk::PipelineStageFlagBits2::eTransfer);
	for (auto* target: {&out0, &out1}) {
		target->Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
		                vk_command);
		vk::ImageCopy copy {};
		copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		copy.extent         = vk::Extent3D {out.width, out.height, 1};
		vk_command.copyImage(g_sr.output.image, vk::ImageLayout::eTransferSrcOptimal,
		                     target->backing.image, vk::ImageLayout::eTransferDstOptimal, copy);
		target->NoteContentWrite();
	}
	// The jitter's spread over the last status interval: a still camera jitter (0) means the
	// upscaler has no sub-pixel samples to accumulate.
	g_sr.jitter_min_x = std::min(g_sr.jitter_min_x, constants.jitterOffset.x);
	g_sr.jitter_max_x = std::max(g_sr.jitter_max_x, constants.jitterOffset.x);
	g_sr.jitter_min_y = std::min(g_sr.jitter_min_y, constants.jitterOffset.y);
	g_sr.jitter_max_y = std::max(g_sr.jitter_max_y, constants.jitterOffset.y);
	g_sr.resets += reset ? 1u : 0u;
	if (++g_sr.evaluated == 1) {
		std::printf("%s: on (jitter %.3f, %.3f px; projection's %.3f, %.3f)\n", name,
		            constants.jitterOffset.x, constants.jitterOffset.y, projection_x, projection_y);
		std::fflush(stdout);
	} else if (g_sr.evaluated % 600 == 0) {
		std::printf("%s: %ux%u to %ux%u, last 600 frames: jitter x %.3f..%.3f, y %.3f..%.3f "
		            "px, %u history reset(s)\n",
		            name, in.width, in.height, out.width, out.height, g_sr.jitter_min_x, g_sr.jitter_max_x,
		            g_sr.jitter_min_y, g_sr.jitter_max_y, g_sr.resets);
		std::fflush(stdout);
		g_sr.jitter_min_x = g_sr.jitter_min_y = 1e9f;
		g_sr.jitter_max_x = g_sr.jitter_max_y = -1e9f;
		g_sr.resets = 0;
	}
	return true;
#else
	(void)graphics;
	(void)cache;
	(void)command;
	(void)pixel;
	(void)user_data;
	(void)colors;
	return false;
#endif
}

void BeforePresent(const FrameInputs* inputs, vk::Extent2D backbuffer, vk::Format format,
                   uint32_t image_count) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
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

	++g_frame_index;
	std::unique_lock sl_lock(g_sl_mutex);
	if (g_api.get_new_frame_token(g_token, &g_frame_index) != sl::Result::eOk || g_token == nullptr) {
		g_token = nullptr;
		return;
	}
	auto& token = *g_token;
	// The frame's Reflex sleep, then its markers (all at its present: the game's frame was made before).
	sl_lock.unlock();
	if (g_api.reflex_sleep != nullptr) g_api.reflex_sleep(token);
	sl_lock.lock();
	g_api.pcl_set_marker(sl::PCLMarker::eSimulationStart, token);
	g_api.pcl_set_marker(sl::PCLMarker::eSimulationEnd, token);
	g_api.pcl_set_marker(sl::PCLMarker::eRenderSubmitStart, token);

	sl::Constants constants {};
	DefaultConstants(constants);
	bool reset = !usable;
	if (usable) {
		reset = !CameraConstants(inputs->camera, inputs->render_extent, g_previous_view_proj,
		                         g_previous_valid, constants) ||
		        reset;
	} else {
		g_previous_valid = false;
	}
	constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	if (const auto result = g_api.set_constants(constants, token, viewport);
	    result != sl::Result::eOk && g_presents < 8) {
		LOGF("frame generation: slSetConstants failed (%d)\n", static_cast<int>(result));
	}

	if (usable) {
		sl::Resource depth(sl::ResourceType::eTex2d, static_cast<VkImage>(inputs->depth.image), nullptr,
		                   static_cast<VkImageView>(inputs->depth_view),
		                   static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		depth.width        = inputs->depth.extent.width;
		depth.height       = inputs->depth.extent.height;
		depth.nativeFormat = static_cast<uint32_t>(inputs->depth.format);
		depth.mipLevels    = 1;
		depth.arrayLayers  = 1;
		depth.flags        = 0;
		depth.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(inputs->depth.usage));
		sl::Resource motion(sl::ResourceType::eTex2d, static_cast<VkImage>(inputs->motion.image), nullptr,
		                    static_cast<VkImageView>(inputs->motion_view),
		                    static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		motion.width        = inputs->motion.extent.width;
		motion.height       = inputs->motion.extent.height;
		motion.nativeFormat = static_cast<uint32_t>(inputs->motion.format);
		motion.mipLevels    = 1;
		motion.arrayLayers  = 1;
		motion.flags        = 0;
		motion.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(inputs->motion.usage));
		const sl::Extent full {0, 0, inputs->render_extent.width, inputs->render_extent.height};
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
		++g_with_inputs;
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
	// Off before the swapchain is made again (DLSS-G guide: before a resize or a full screen change).
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
	g_fsr_fg_native_reset.store(true, std::memory_order_relaxed);
}


#ifdef KYTY_FRAMEGEN_STREAMLINE
namespace {

// FSR 3.1 frame generation's state (present thread; the swapchain's calls come from it too).
struct FsrFg {
	ffxContext                                  swapchain_context = nullptr; // FSR's swapchain
	ffxContext                                  fg_context        = nullptr; // frame generation
	ffxQueryDescSwapchainReplacementFunctionsVK functions {};
	vk::SwapchainKHR                            swapchain = nullptr;
	vk::Extent2D                                display {};
	vk::Format                                  format = vk::Format::eUndefined;
	bool                                        hdr    = false;
	vk::Extent2D                                max_render {};
	uint64_t                                    frame_id = 0;
	bool                                        mode_on  = false;
	bool                                        reset    = false;
	uint32_t                                    run_with = 0, run_without = 0;
	std::chrono::steady_clock::time_point       last {};
	Mat4                                        previous_view_proj;
	bool                                        previous_valid = false;
	GraphicContext*                             graphics       = nullptr;
};
FsrFg g_fsr_fg_state;
// FSR's HUD-less images (RecordFsrHudless): two in turn (a generated frame may still read the
// previous one while this frame's is copied).
struct FsrHudless {
	std::array<VulkanImage, 2> images {};
	uint32_t                   index = 0;
	bool                       ready = false; // this present's copy was made
	bool                       reported = false;
};
FsrHudless g_fsr_hudless;
// (DLSS-G's: on after a run of frames with inputs, off after a run without.)
constexpr uint32_t FsrTurnOnAfter = 10, FsrTurnOffAfter = 30;

// The present thread holds the graphics queue's lock in QueuePresent; FSR submits to that queue
// from its present (generated frame) and may from other calls, which then take the lock.
thread_local bool t_fsr_queue_locked = false;

VkResult FsrGameQueueSubmit(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
	auto& graphics = *g_fsr_fg_state.graphics;
	if (t_fsr_queue_locked) {
		return VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(static_cast<VkQueue>(graphics.queue), count, submits, fence);
	}
	Common::LockGuard lock(graphics.queue_mutex);
	return VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(static_cast<VkQueue>(graphics.queue), count, submits, fence);
}

bool FsrFgOk(ffxReturnCode_t code, const char* what) {
	if (code == FFX_API_RETURN_OK) return true;
	std::printf("Frame generation: FSR %s failed (%u)\n", what, static_cast<uint32_t>(code));
	std::fflush(stdout);
	return false;
}

void FsrFgDestroyContext() {
	auto& s = g_fsr_fg_state;
	if (s.fg_context == nullptr) return;
	if (s.swapchain != nullptr) {
		// (Off before its context goes: the swapchain then presents the frames as they are.)
		ffxConfigureDescFrameGeneration config {};
		config.header.type            = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		config.swapChain              = static_cast<VkSwapchainKHR>(s.swapchain);
		config.frameGenerationEnabled = false;
		config.frameID                = ++s.frame_id;
		(void)g_fsr_api.configure(&s.fg_context, &config.header);
	}
	(void)g_fsr_api.destroy(&s.fg_context, nullptr);
	s.fg_context = nullptr;
	s.mode_on    = false;
}

// The frame generation context for render sizes up to `render` (made again for a larger one).
bool FsrFgEnsureContext(vk::Extent2D render) {
	auto& s = g_fsr_fg_state;
	if (s.fg_context != nullptr && render.width <= s.max_render.width && render.height <= s.max_render.height) {
		return true;
	}
	FsrFgDestroyContext();
	ffxCreateBackendVKDesc backend {};
	backend.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	backend.vkDevice         = static_cast<VkDevice>(s.graphics->device);
	backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(s.graphics->physical_device);
	backend.vkDeviceProcAddr = FsrGetDeviceProcAddr;
	g_fsr_instance           = static_cast<VkInstance>(s.graphics->instance);
	ffxCreateContextDescFrameGeneration create {};
	create.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	create.header.pNext = &backend.header;
	// Astro Bot's depth is OpenGL-style (not reversed, a finite far plane); vectors at render size.
	create.flags = std::getenv("KYTY_FRAMEGEN_DEBUG") != nullptr ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0u;
	create.displaySize      = {s.display.width, s.display.height};
	s.max_render            = {std::max(render.width, s.display.width), std::max(render.height, s.display.height)};
	create.maxRenderSize    = {s.max_render.width, s.max_render.height};
	create.backBufferFormat = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(s.format));
	if (!FsrFgOk(g_fsr_api.create(&s.fg_context, &create.header, nullptr), "frame generation context")) {
		s.fg_context = nullptr;
		return false;
	}
	std::printf("Frame generation: FSR 3.1 ready (%ux%u, render up to %ux%u)\n", s.display.width, s.display.height,
	            s.max_render.width, s.max_render.height);
	std::fflush(stdout);
	return true;
}

// The swapchain calls it in its present (async workloads off: on the game queue): the generated frame.
ffxReturnCode_t FsrFgGenerate(ffxDispatchDescFrameGeneration* params, void* context) {
	if (std::exchange(g_fsr_fg_state.reset, false)) params->reset = true;
	if (g_fsr_fg_state.hdr) {
		params->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
		params->minMaxLuminance[0]         = 0.0f;
		params->minMaxLuminance[1]         = 1000.0f;
	}
	return g_fsr_api.dispatch(static_cast<ffxContext*>(context), &params->header);
}

} // namespace
#endif

#if defined(KYTY_FSR_NATIVE) && !defined(KYTY_FRAMEGEN_STREAMLINE)
namespace {

// FSR 3.1 frame generation built in (Linux, macOS): FidelityFX's optical flow and frame
// interpolation from the SDK sources, as chenxiao07/KytyPS5's src/local/fsr-native.cpp does for
// Demon's Souls. AMD's frame generation swapchain is Windows only, so the presenter shows the
// frames itself: at a new game frame the swapchain image the frame was drawn into gets the frame
// generated between the last one and it, which is presented; the frame itself (`composed`) is
// presented at the next vblank tick (Presenter::PresentPendingFrame).
struct FsrFgNative {
	GraphicContext*                    graphics = nullptr;
	VkDeviceContext                    device_context {};
	void*                              scratch = nullptr;
	FfxInterface                       backend {};
	alignas(64) FfxOpticalflowContext        optical_flow {};
	alignas(64) FfxFrameInterpolationContext interpolation {};
	uint32_t                           shared_context = 0;
	FfxResourceInternal                flow_vectors {}, scene_change {}, dilated_depth {}, dilated_motion {},
	    previous_depth {};
	bool                               created = false, failed = false;
	vk::Extent2D                       display {}, max_render {};
	VulkanImage                        composed, generated; // R8G8B8A8 at the display size
	uint64_t                           frame_id = 0;
	bool                               mode_on = false, reset = true, format_reported = false;
	uint32_t                           run_with = 0, run_without = 0;
	std::chrono::steady_clock::time_point last {};
	Mat4                               previous_view_proj;
	bool                               previous_valid = false;
};
FsrFgNative g_fsr_fg_native_state;
constexpr uint32_t FsrNativeTurnOnAfter = 10, FsrNativeTurnOffAfter = 30;
constexpr vk::Format FsrNativeFormat = vk::Format::eR8G8B8A8Unorm;

void FsrFgNativeWaitIdle() {
	auto& s = g_fsr_fg_native_state;
	if (s.graphics == nullptr) return;
	Common::LockGuard lock(s.graphics->queue_mutex);
	(void)s.graphics->queue.waitIdle();
}

void FsrFgNativeDestroyContext() {
	auto& s = g_fsr_fg_native_state;
	if (!s.created) return;
	FsrFgNativeWaitIdle();
	auto& b = s.backend;
	for (auto* resource: {&s.flow_vectors, &s.scene_change, &s.dilated_depth, &s.dilated_motion, &s.previous_depth}) {
		b.fpDestroyResource(&b, *resource, s.shared_context);
	}
	b.fpDestroyBackendContext(&b, s.shared_context);
	ffxFrameInterpolationContextDestroy(&s.interpolation);
	ffxOpticalflowContextDestroy(&s.optical_flow);
	std::free(s.scratch);
	s.scratch = nullptr;
	s.created = false;
}

bool FsrFgNativeFail(const char* what, FfxErrorCode code) {
	g_fsr_fg_native_state.failed = true;
	std::printf("Frame generation: FSR %s failed (error 0x%x); frame generation off\n", what,
	            static_cast<unsigned>(code));
	std::fflush(stdout);
	return false;
}

// The contexts for the display size and render sizes up to `render` (made again for others).
bool FsrFgNativeEnsureContext(vk::Extent2D display, vk::Extent2D render) {
	auto& s = g_fsr_fg_native_state;
	if (s.failed) return false;
	if (s.created && s.display == display && render.width <= s.max_render.width &&
	    render.height <= s.max_render.height) {
		return true;
	}
	FsrFgNativeDestroyContext();
	g_fsr_instance                    = static_cast<VkInstance>(s.graphics->instance);
	s.device_context.vkDevice         = static_cast<VkDevice>(s.graphics->device);
	s.device_context.vkPhysicalDevice = static_cast<VkPhysicalDevice>(s.graphics->physical_device);
	s.device_context.vkDeviceProcAddr = FsrGetDeviceProcAddr;
	// (Optical flow, frame interpolation and their shared resources. The SDK keeps structures with
	// SIMD members in its scratch memory: more than its declared alignment.)
	const size_t size = (ffxGetScratchMemorySizeVK(s.device_context.vkPhysicalDevice, 3) + 63) & ~size_t {63};
	if (posix_memalign(&s.scratch, 64, size) != 0 || s.scratch == nullptr) {
		s.scratch = nullptr;
		return FsrFgNativeFail("scratch memory", FFX_ERROR_OUT_OF_MEMORY);
	}
	std::memset(s.scratch, 0, size);
	if (const auto code = ffxGetInterfaceVK(&s.backend, ffxGetDeviceVK(&s.device_context), s.scratch, size, 3);
	    code != FFX_OK) {
		return FsrFgNativeFail("ffxGetInterfaceVK", code);
	}
	auto& b = s.backend;
	FfxOpticalflowContextDescription flow {};
	flow.backendInterface = b;
	flow.resolution       = {display.width, display.height};
	if (const auto code = ffxOpticalflowContextCreate(&s.optical_flow, &flow); code != FFX_OK) {
		return FsrFgNativeFail("optical flow context", code);
	}
	s.max_render = {std::max(render.width, display.width), std::max(render.height, display.height)};
	FfxFrameInterpolationContextDescription description {};
	// Astro Bot's depth is OpenGL-style (not reversed, a finite far plane); vectors at render size.
	description.flags            = std::getenv("KYTY_FRAMEGEN_DEBUG") != nullptr ? FFX_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING : 0u;
	description.maxRenderSize    = {s.max_render.width, s.max_render.height};
	description.displaySize      = {display.width, display.height};
	description.backBufferFormat = ffxGetSurfaceFormatVK(static_cast<VkFormat>(FsrNativeFormat));
	description.previousInterpolationSourceFormat = description.backBufferFormat;
	description.backendInterface                  = b;
	if (const auto code = ffxFrameInterpolationContextCreate(&s.interpolation, &description); code != FFX_OK) {
		ffxOpticalflowContextDestroy(&s.optical_flow);
		return FsrFgNativeFail("frame interpolation context", code);
	}
	FfxErrorCode code = b.fpCreateBackendContext(&b, FFX_EFFECT_SHAREDRESOURCES, nullptr, &s.shared_context);
	FfxOpticalflowSharedResourceDescriptions flow_resources {};
	FfxFrameInterpolationSharedResourceDescriptions resources {};
	if (code == FFX_OK) code = ffxOpticalflowGetSharedResourceDescriptions(&s.optical_flow, &flow_resources);
	if (code == FFX_OK) code = b.fpCreateResource(&b, &flow_resources.opticalFlowVector, s.shared_context, &s.flow_vectors);
	if (code == FFX_OK) code = b.fpCreateResource(&b, &flow_resources.opticalFlowSCD, s.shared_context, &s.scene_change);
	if (code == FFX_OK) code = ffxFrameInterpolationGetSharedResourceDescriptions(&s.interpolation, &resources);
	if (code == FFX_OK) code = b.fpCreateResource(&b, &resources.dilatedDepth, s.shared_context, &s.dilated_depth);
	if (code == FFX_OK) code = b.fpCreateResource(&b, &resources.dilatedMotionVectors, s.shared_context, &s.dilated_motion);
	if (code == FFX_OK) {
		code = b.fpCreateResource(&b, &resources.reconstructedPrevNearestDepth, s.shared_context, &s.previous_depth);
	}
	if (code != FFX_OK) {
		ffxFrameInterpolationContextDestroy(&s.interpolation);
		ffxOpticalflowContextDestroy(&s.optical_flow);
		return FsrFgNativeFail("shared resources", code);
	}
	s.created = true;
	s.display = display;
	s.reset   = true;
	std::printf("Frame generation: FSR 3.1 ready (%ux%u, render up to %ux%u, built in)\n", display.width,
	            display.height, s.max_render.width, s.max_render.height);
	std::fflush(stdout);
	return true;
}

// `image` at the display size (made again for another), `usage` its usage.
bool FsrFgNativeImage(VulkanImage& image, vk::Extent2D extent, vk::ImageUsageFlags usage) {
	auto& graphics = *g_fsr_fg_native_state.graphics;
	if (image.image != nullptr && image.extent.width == extent.width && image.extent.height == extent.height) {
		return true;
	}
	if (image.image != nullptr) {
		FsrFgNativeWaitIdle();
		graphics.DeleteImage(image);
	}
	vk::ImageCreateInfo create {};
	create.imageType     = vk::ImageType::e2D;
	create.extent        = vk::Extent3D {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = FsrNativeFormat;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage         = usage;
	create.sharingMode   = vk::SharingMode::eExclusive;
	create.samples       = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, image)) {
		return false;
	}
	image.state.layout      = vk::ImageLayout::eUndefined;
	image.state.access_mask = {};
	image.state.pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
	return true;
}

// A swapchain image's layout change (its contents kept unless from undefined).
void SwapchainBarrier(vk::CommandBuffer command, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                      vk::AccessFlags2 access) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask       = access;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
}

void Blit(vk::CommandBuffer command, vk::Image source, vk::Image target, vk::Extent2D extent) {
	vk::ImageBlit region {};
	region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	region.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
	region.dstOffsets[1]  = region.srcOffsets[1];
	command.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, target, vk::ImageLayout::eTransferDstOptimal, 1,
	                  &region, vk::Filter::eNearest);
}

} // namespace
#endif

bool NativeFrameGenOn() {
#if defined(KYTY_FSR_NATIVE) && !defined(KYTY_FRAMEGEN_STREAMLINE)
	return g_fsr_fg_native && g_fsr_fg_ready.load(std::memory_order_relaxed) && !g_fsr_fg_native_state.failed;
#else
	return false;
#endif
}

bool RecordNativeFrameGen(vk::CommandBuffer command, const FrameInputs* inputs, vk::Image backbuffer,
                          vk::Format format, vk::Extent2D extent, bool show) {
#if defined(KYTY_FSR_NATIVE) && !defined(KYTY_FRAMEGEN_STREAMLINE)
	auto& s = g_fsr_fg_native_state;
	if (!NativeFrameGenOn() || s.graphics == nullptr) return false;
	if (g_fsr_fg_native_reset.exchange(false, std::memory_order_relaxed)) s.reset = true;
	const bool usable = inputs != nullptr && inputs->valid && inputs->camera.valid;
	s.run_with        = usable ? s.run_with + 1 : 0;
	s.run_without     = usable ? 0 : s.run_without + 1;
	const bool mode_on = s.mode_on ? s.run_without < FsrNativeTurnOffAfter : s.run_with >= FsrNativeTurnOnAfter;
	if (mode_on != s.mode_on) {
		std::printf("Frame generation: FSR %s\n", mode_on ? "on" : "off (no inputs)");
		std::fflush(stdout);
		s.mode_on        = mode_on;
		s.reset          = true;
		s.previous_valid = false;
	}
	if (!mode_on || !usable) {
		s.reset = true;
		return false;
	}
	// (The generated frame is made in 8-bit RGBA: an SDR swapchain's. HDR output is not interpolated.)
	if (format != vk::Format::eB8G8R8A8Unorm && format != vk::Format::eR8G8B8A8Unorm) {
		if (!std::exchange(s.format_reported, true)) {
			std::printf("Frame generation: FSR (built in) needs SDR output (swapchain format %s); not generating\n",
			            vk::to_string(format).c_str());
			std::fflush(stdout);
		}
		return false;
	}
	const auto render = inputs->render_extent;
	if (!FsrFgNativeEnsureContext(extent, render)) return false;
	using Usage = vk::ImageUsageFlagBits;
	if (!FsrFgNativeImage(s.composed, extent, Usage::eTransferDst | Usage::eTransferSrc | Usage::eSampled) ||
	    !FsrFgNativeImage(s.generated, extent, Usage::eStorage | Usage::eTransferSrc | Usage::eSampled)) {
		std::printf("Frame generation: no memory for FSR's %ux%u frames; frame generation off\n", extent.width,
		            extent.height);
		std::fflush(stdout);
		s.failed = true;
		return false;
	}

	// The camera as DLSS-G gets it (Streamline's conventions are FSR's).
	sl::Constants constants {};
	DefaultConstants(constants);
	if (!CameraConstants(inputs->camera, render, s.previous_view_proj, s.previous_valid, constants)) s.reset = true;
	const auto  now   = std::chrono::steady_clock::now();
	const float delta = s.last.time_since_epoch().count() == 0
	                        ? 16.7f
	                        : std::clamp(std::chrono::duration<float, std::milli>(now - s.last).count(), 1.0f, 200.0f);
	s.last = now;
	if (delta >= 200.0f) s.reset = true; // (a gap: a menu, a load, a pause)
	static const uint32_t debug_flags = [] {
		const char* value = std::getenv("KYTY_FRAMEGEN_DEBUG");
		const int   level = value != nullptr ? std::atoi(value) : 0;
		return level >= 2   ? uint32_t(FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW)
		       : level == 1 ? uint32_t(FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES |
		                               FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS)
		                    : 0u;
	}();

	// This frame (with its overlays) copied out of the swapchain image: FSR's input, and the frame
	// presented after the generated one.
	SwapchainBarrier(command, backbuffer, vk::ImageLayout::ePresentSrcKHR, vk::ImageLayout::eTransferSrcOptimal,
	                 vk::AccessFlagBits2::eTransferRead);
	Transition(command, s.composed, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
	           vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eTransfer);
	Blit(command, backbuffer, s.composed.image, extent);
	Transition(command, s.composed, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal,
	           vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eAllCommands);
	Transition(command, s.generated, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
	           vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
	           vk::PipelineStageFlagBits2::eAllCommands);

	auto&      b              = s.backend;
	const auto list           = ffxGetCommandListVK(static_cast<VkCommandBuffer>(command));
	const auto color          = FsrNativeResource(s.composed, L"KytyFgColor", false);
	const auto target         = FsrNativeResource(s.generated, L"KytyFgOutput", true);
	const auto flow_vector    = b.fpGetResource(&b, s.flow_vectors);
	const auto flow_scd       = b.fpGetResource(&b, s.scene_change);
	const auto depth_dilated  = b.fpGetResource(&b, s.dilated_depth);
	const auto motion_dilated = b.fpGetResource(&b, s.dilated_motion);
	const auto depth_previous = b.fpGetResource(&b, s.previous_depth);
	const uint64_t id         = ++s.frame_id;
	const bool     reset      = std::exchange(s.reset, false);

	FfxFrameInterpolationPrepareDescription prepare {};
	prepare.flags                   = debug_flags;
	prepare.commandList             = list;
	prepare.renderSize              = {render.width, render.height};
	prepare.jitterOffset            = {constants.jitterOffset.x, constants.jitterOffset.y};
	prepare.motionVectorScale       = {MotionVectorSign() * static_cast<float>(render.width),
	                                   MotionVectorSign() * static_cast<float>(render.height)};
	prepare.frameTimeDelta          = delta;
	prepare.cameraNear              = constants.cameraNear;
	prepare.cameraFar               = constants.cameraFar;
	prepare.viewSpaceToMetersFactor = 1.0f;
	prepare.cameraFovAngleVertical  = constants.cameraFOV;
	prepare.depth                   = FsrNativeResource(inputs->depth, L"KytyFgDepth", false);
	prepare.motionVectors           = FsrNativeResource(inputs->motion, L"KytyFgMotion", false);
	prepare.frameID                 = id;
	prepare.dilatedDepth            = depth_dilated;
	prepare.dilatedMotionVectors    = motion_dilated;
	prepare.reconstructedPrevDepth  = depth_previous;
	prepare.cameraPosition[0] = constants.cameraPos.x;
	prepare.cameraPosition[1] = constants.cameraPos.y;
	prepare.cameraPosition[2] = constants.cameraPos.z;
	prepare.cameraUp[0]       = constants.cameraUp.x;
	prepare.cameraUp[1]       = constants.cameraUp.y;
	prepare.cameraUp[2]       = constants.cameraUp.z;
	prepare.cameraRight[0]    = constants.cameraRight.x;
	prepare.cameraRight[1]    = constants.cameraRight.y;
	prepare.cameraRight[2]    = constants.cameraRight.z;
	prepare.cameraForward[0]  = constants.cameraFwd.x;
	prepare.cameraForward[1]  = constants.cameraFwd.y;
	prepare.cameraForward[2]  = constants.cameraFwd.z;
	bool ok = true;
	if (const auto code = ffxFrameInterpolationPrepare(&s.interpolation, &prepare); code != FFX_OK) {
		ok = FsrFgNativeFail("prepare", code);
	}

	if (ok) {
		FfxOpticalflowDispatchDescription flow {};
		flow.commandList                = list;
		flow.color                      = color;
		flow.opticalFlowVector          = flow_vector;
		flow.opticalFlowSCD             = flow_scd;
		flow.reset                      = reset;
		flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		flow.minMaxLuminance            = {0.0f, 1.0f};
		if (const auto code = ffxOpticalflowContextDispatch(&s.optical_flow, &flow); code != FFX_OK) {
			ok = FsrFgNativeFail("optical flow", code);
		}
	}
	if (ok) {
		FfxFrameInterpolationDispatchDescription dispatch {};
		dispatch.flags                           = debug_flags;
		dispatch.commandList                     = list;
		dispatch.displaySize                     = {extent.width, extent.height};
		dispatch.renderSize                      = {render.width, render.height};
		dispatch.currentBackBuffer               = color;
		dispatch.output                          = target;
		dispatch.interpolationRect               = {0, 0, static_cast<int32_t>(extent.width),
		                                            static_cast<int32_t>(extent.height)};
		dispatch.opticalFlowVector               = flow_vector;
		dispatch.opticalFlowSceneChangeDetection = flow_scd;
		dispatch.opticalFlowBufferSize           = {flow_vector.description.width, flow_vector.description.height};
		dispatch.opticalFlowScale = {1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};
		dispatch.opticalFlowBlockSize       = 8;
		dispatch.cameraNear                 = constants.cameraNear;
		dispatch.cameraFar                  = constants.cameraFar;
		dispatch.cameraFovAngleVertical     = constants.cameraFOV;
		dispatch.viewSpaceToMetersFactor    = 1.0f;
		dispatch.frameTimeDelta             = delta;
		dispatch.reset                      = reset;
		dispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		dispatch.minMaxLuminance[0]         = 0.0f;
		dispatch.minMaxLuminance[1]         = 1.0f;
		dispatch.frameID                    = id;
		dispatch.dilatedDepth               = depth_dilated;
		dispatch.dilatedMotionVectors       = motion_dilated;
		dispatch.reconstructedPrevDepth     = depth_previous;
		if (const auto code = ffxFrameInterpolationDispatch(&s.interpolation, &dispatch); code != FFX_OK) {
			ok = FsrFgNativeFail("frame interpolation", code);
		}
	}
	// (The first frame after a reset has no frame before it: shown as it is.)
	const bool generated = ok && !reset && show;
	if (generated) {
		Transition(command, s.generated, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferSrcOptimal,
		           vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eTransfer);
		SwapchainBarrier(command, backbuffer, vk::ImageLayout::eTransferSrcOptimal,
		                 vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
		Blit(command, s.generated.image, backbuffer, extent);
		SwapchainBarrier(command, backbuffer, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR,
		                 vk::AccessFlagBits2::eMemoryRead);
		static bool reported = false;
		if (!std::exchange(reported, true)) {
			std::printf("Frame generation: FSR 2x on (render %ux%u, display %ux%u, jitter %.3f, %.3f px; built in, "
			            "shown at the vblank ticks)\n",
			            render.width, render.height, extent.width, extent.height, constants.jitterOffset.x,
			            constants.jitterOffset.y);
			std::fflush(stdout);
		}
	} else {
		SwapchainBarrier(command, backbuffer, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::ePresentSrcKHR,
		                 vk::AccessFlagBits2::eMemoryRead);
	}
	return generated;
#else
	(void)command, (void)inputs, (void)backbuffer, (void)format, (void)extent, (void)show;
	return false;
#endif
}

void RecordNativeRealFrame(vk::CommandBuffer command, vk::Image backbuffer, vk::Extent2D extent) {
#if defined(KYTY_FSR_NATIVE) && !defined(KYTY_FRAMEGEN_STREAMLINE)
	auto& s = g_fsr_fg_native_state;
	SwapchainBarrier(command, backbuffer, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
	                 vk::AccessFlagBits2::eTransferWrite);
	if (s.composed.image != nullptr && s.composed.extent.width == extent.width &&
	    s.composed.extent.height == extent.height) {
		Transition(command, s.composed, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferSrcOptimal,
		           vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eTransfer);
		Blit(command, s.composed.image, backbuffer, extent);
	} else {
		const vk::ClearColorValue black {};
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		command.clearColorImage(backbuffer, vk::ImageLayout::eTransferDstOptimal, &black, 1, &range);
	}
	SwapchainBarrier(command, backbuffer, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR,
	                 vk::AccessFlagBits2::eMemoryRead);
#else
	(void)command, (void)backbuffer, (void)extent;
#endif
}

bool FsrFrameGenRequested() {
	return g_fsr_fg;
}

void OnFsrQueues(GraphicContext& graphics) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_fsr_fg) return;
	if (graphics.fsr_present_queue == nullptr || graphics.fsr_compute_queue == nullptr ||
	    graphics.fsr_acquire_queue == nullptr) {
		g_fsr_fg = false;
		return;
	}
	g_fsr_fg_state.graphics = &graphics;
	g_fsr_fg_ready.store(true, std::memory_order_relaxed);
#elif defined(KYTY_FSR_NATIVE)
	if (!g_fsr_fg_native) return;
	g_fsr_fg_native_state.graphics = &graphics;
	g_fsr_fg_ready.store(true, std::memory_order_relaxed);
#else
	(void)graphics;
#endif
}

bool OwnsSwapchain() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return g_fsr_fg_ready.load(std::memory_order_relaxed);
#else
	return false; // (built in: the emulator's own swapchain, the presenter shows the frames)
#endif
}

vk::Result CreateSwapchain(GraphicContext& graphics, const vk::SwapchainCreateInfoKHR& info, bool hdr,
                           vk::SwapchainKHR* swapchain) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	auto& s = g_fsr_fg_state;
	ffxCreateContextDescFrameGenerationSwapChainVK create {};
	create.header.type    = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
	create.physicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	create.device         = static_cast<VkDevice>(graphics.device);
	VkSwapchainKHR handle = VK_NULL_HANDLE;
	create.swapchain      = &handle;
	create.allocator      = nullptr;
	create.createInfo     = static_cast<VkSwapchainCreateInfoKHR>(info);
	create.gameQueue      = {static_cast<VkQueue>(graphics.queue), graphics.queue_family, FsrGameQueueSubmit};
	create.asyncComputeQueue = {static_cast<VkQueue>(graphics.fsr_compute_queue), graphics.fsr_compute_family, nullptr};
	create.presentQueue      = {static_cast<VkQueue>(graphics.fsr_present_queue), graphics.fsr_present_family, nullptr};
	create.imageAcquireQueue = {static_cast<VkQueue>(graphics.fsr_acquire_queue), graphics.fsr_compute_family, nullptr};
	if (!FsrFgOk(g_fsr_api.create(&s.swapchain_context, &create.header, nullptr), "swapchain")) {
		s.swapchain_context = nullptr;
		g_fsr_fg_ready.store(false, std::memory_order_relaxed);
		return vk::Result::eErrorInitializationFailed;
	}
	s.functions             = {};
	s.functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
	if (!FsrFgOk(g_fsr_api.query(&s.swapchain_context, &s.functions.header), "swapchain functions") ||
	    s.functions.pOutAcquireNextImageKHR == nullptr || s.functions.pOutQueuePresentKHR == nullptr ||
	    s.functions.pOutGetSwapchainImagesKHR == nullptr) {
		(void)g_fsr_api.destroy(&s.swapchain_context, nullptr);
		s.swapchain_context = nullptr;
		g_fsr_fg_ready.store(false, std::memory_order_relaxed);
		return vk::Result::eErrorInitializationFailed;
	}
	s.swapchain      = handle;
	s.display        = info.imageExtent;
	s.format         = info.imageFormat;
	s.hdr            = hdr;
	s.run_with       = 0;
	s.run_without    = 0;
	s.previous_valid = false;
	*swapchain       = handle;
	std::printf("Frame generation: FSR swapchain %ux%u\n", s.display.width, s.display.height);
	std::fflush(stdout);
	return vk::Result::eSuccess;
#else
	(void)graphics, (void)info, (void)hdr, (void)swapchain;
	return vk::Result::eErrorInitializationFailed;
#endif
}

void DestroySwapchain() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	auto& s = g_fsr_fg_state;
	// (The frame generation context is made for the display size: again with the next swapchain.)
	FsrFgDestroyContext();
	if (s.swapchain_context != nullptr) {
		(void)g_fsr_api.destroy(&s.swapchain_context, nullptr); // (and its swapchain)
		s.swapchain_context = nullptr;
	}
	s.swapchain = nullptr;
	s.functions = {};
#endif
}

vk::Result GetSwapchainImages(vk::Device device, vk::SwapchainKHR swapchain, uint32_t* count, vk::Image* images) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return static_cast<vk::Result>(g_fsr_fg_state.functions.pOutGetSwapchainImagesKHR(
	    static_cast<VkDevice>(device), static_cast<VkSwapchainKHR>(swapchain), count, reinterpret_cast<VkImage*>(images)));
#else
	(void)device, (void)swapchain, (void)count, (void)images;
	return vk::Result::eErrorInitializationFailed;
#endif
}

vk::Result AcquireNextImage(vk::Device device, vk::SwapchainKHR swapchain, uint64_t timeout, vk::Semaphore semaphore,
                            uint32_t* index) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return static_cast<vk::Result>(g_fsr_fg_state.functions.pOutAcquireNextImageKHR(
	    static_cast<VkDevice>(device), static_cast<VkSwapchainKHR>(swapchain), timeout, static_cast<VkSemaphore>(semaphore),
	    VK_NULL_HANDLE, index));
#else
	(void)device, (void)swapchain, (void)timeout, (void)semaphore, (void)index;
	return vk::Result::eErrorInitializationFailed;
#endif
}

vk::Result QueuePresent(vk::Queue queue, const vk::PresentInfoKHR& present) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	const VkPresentInfoKHR& info = present;
	t_fsr_queue_locked           = true;
	const auto result = g_fsr_fg_state.functions.pOutQueuePresentKHR(static_cast<VkQueue>(queue), &info);
	t_fsr_queue_locked = false;
	return static_cast<vk::Result>(result);
#else
	(void)queue, (void)present;
	return vk::Result::eErrorInitializationFailed;
#endif
}

void RecordFsrHudless(GraphicContext& graphics, vk::CommandBuffer command, vk::Image backbuffer,
                      vk::ImageLayout layout, vk::Format format, vk::Extent2D extent) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	auto& h = g_fsr_hudless;
	h.ready = false;
	// KYTY_FSR_HUDLESS=0: no HUD-less image (FSR interpolates the HUD with the scene).
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_FSR_HUDLESS");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	if (!enabled || !OwnsSwapchain() || g_fsr_fg_state.fg_context == nullptr || !g_fsr_fg_state.mode_on) return;
	h.index     = (h.index + 1u) % static_cast<uint32_t>(h.images.size());
	auto& image = h.images[h.index];
	if (image.image == nullptr || image.format != format || image.extent.width != extent.width ||
	    image.extent.height != extent.height) {
		if (image.image != nullptr) {
			graphics.DeleteImage(image);
		}
		vk::ImageCreateInfo create {};
		create.imageType     = vk::ImageType::e2D;
		create.extent        = vk::Extent3D {extent.width, extent.height, 1};
		create.mipLevels     = 1;
		create.arrayLayers   = 1;
		create.format        = format;
		create.tiling        = vk::ImageTiling::eOptimal;
		create.initialLayout = vk::ImageLayout::eUndefined;
		create.usage         = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
		create.sharingMode = vk::SharingMode::eExclusive;
		create.samples     = vk::SampleCountFlagBits::e1;
		if (!graphics.CreateImage(create, image)) {
			std::printf("Frame generation: cannot make FSR's %ux%u HUD-less image; the HUD is interpolated\n",
			            extent.width, extent.height);
			std::fflush(stdout);
			return;
		}
	}
	// The backbuffer as its blit left it (`layout`), copied, then back in `layout`.
	vk::ImageMemoryBarrier2 barriers[2] {};
	barriers[0].srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barriers[0].srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	barriers[0].dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barriers[0].dstAccessMask       = vk::AccessFlagBits2::eTransferRead;
	barriers[0].oldLayout           = layout;
	barriers[0].newLayout           = vk::ImageLayout::eTransferSrcOptimal;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image               = backbuffer;
	barriers[0].subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	barriers[1]                     = barriers[0];
	barriers[1].srcAccessMask       = image.state.access_mask;
	barriers[1].srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barriers[1].dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	barriers[1].oldLayout           = vk::ImageLayout::eUndefined;
	barriers[1].newLayout           = vk::ImageLayout::eTransferDstOptimal;
	barriers[1].image               = image.image;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 2;
	dependency.pImageMemoryBarriers    = barriers;
	command.pipelineBarrier2(dependency);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.extent         = vk::Extent3D {extent.width, extent.height, 1};
	command.copyImage(backbuffer, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
	vk::ImageMemoryBarrier2 back {};
	back.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	back.srcAccessMask       = vk::AccessFlagBits2::eTransferRead;
	back.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	back.dstAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	back.oldLayout           = vk::ImageLayout::eTransferSrcOptimal;
	back.newLayout           = layout;
	back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	back.image               = backbuffer;
	back.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	// (The copy for FSR's compute passes: its "compute read" state.)
	vk::ImageMemoryBarrier2 ready {};
	ready.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	ready.srcAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	ready.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	ready.dstAccessMask       = vk::AccessFlagBits2::eShaderRead;
	ready.oldLayout           = vk::ImageLayout::eTransferDstOptimal;
	ready.newLayout           = vk::ImageLayout::eShaderReadOnlyOptimal;
	ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ready.image               = image.image;
	ready.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::ImageMemoryBarrier2 after[2] {back, ready};
	dependency.imageMemoryBarrierCount = 2;
	dependency.pImageMemoryBarriers    = after;
	command.pipelineBarrier2(dependency);
	image.state.layout      = vk::ImageLayout::eShaderReadOnlyOptimal;
	image.state.access_mask = vk::AccessFlagBits2::eShaderRead;
	image.state.pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
	h.ready                 = true;
	if (!std::exchange(h.reported, true)) {
		std::printf("Frame generation: the game draws a HUD layer; FSR gets the frame without it (HUD-less, "
		            "KYTY_FSR_HUDLESS=0 off)\n");
		std::fflush(stdout);
	}
#else
	(void)graphics, (void)command, (void)backbuffer, (void)layout, (void)format, (void)extent;
#endif
}

void RecordFsrPrepare(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs,
                      vk::Extent2D backbuffer) {
	(void)graphics;
#ifdef KYTY_FRAMEGEN_STREAMLINE
	auto& s = g_fsr_fg_state;
	if (!OwnsSwapchain() || s.swapchain == nullptr) return;
	const bool usable = inputs != nullptr && inputs->valid && inputs->camera.valid;
	s.run_with        = usable ? s.run_with + 1 : 0;
	s.run_without     = usable ? 0 : s.run_without + 1;
	bool mode_on      = s.mode_on ? s.run_without < FsrTurnOffAfter : s.run_with >= FsrTurnOnAfter;
	if (mode_on && usable && !FsrFgEnsureContext(inputs->render_extent)) mode_on = false;
	if (s.fg_context == nullptr) return; // (no frame with inputs yet: the swapchain presents as it is)
	if (mode_on != s.mode_on) {
		std::printf("Frame generation: FSR %s\n", mode_on ? "on" : "off (no inputs)");
		std::fflush(stdout);
		s.mode_on        = mode_on;
		s.reset          = true;
		s.previous_valid = false;
	}
	const auto  now   = std::chrono::steady_clock::now();
	const float delta = s.last.time_since_epoch().count() == 0
	                        ? 16.7f
	                        : std::clamp(std::chrono::duration<float, std::milli>(now - s.last).count(), 1.0f, 200.0f);
	s.last            = now;
	const uint64_t id = ++s.frame_id;
	// KYTY_FRAMEGEN_DEBUG=1: tear and reset lines, 2: FSR's debug view.
	static const uint32_t debug_flags = [] {
		const char* value = std::getenv("KYTY_FRAMEGEN_DEBUG");
		const int   level = value != nullptr ? std::atoi(value) : 0;
		return level >= 2   ? uint32_t(FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW)
		       : level == 1 ? uint32_t(FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES |
		                               FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS)
		                    : 0u;
	}();
	ffxConfigureDescFrameGeneration config {};
	config.header.type                        = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	config.swapChain                          = static_cast<VkSwapchainKHR>(s.swapchain);
	config.presentCallback                    = nullptr;
	config.frameGenerationCallback            = FsrFgGenerate;
	config.frameGenerationCallbackUserContext = &s.fg_context;
	config.frameGenerationEnabled             = mode_on;
	config.allowAsyncWorkloads                = false;
	config.flags                              = debug_flags;
	config.onlyPresentGenerated               = false;
	config.generationRect                     = {0, 0, static_cast<int32_t>(backbuffer.width),
	                                             static_cast<int32_t>(backbuffer.height)};
	// (This present's backbuffer before the HUD layer was drawn over it, RecordFsrHudless: FSR takes
	// the HUD from the frame and keeps it out of the interpolation.)
	if (std::exchange(g_fsr_hudless.ready, false)) {
		config.HUDLessColor =
		    FsrResource(g_fsr_hudless.images[g_fsr_hudless.index], FFX_API_RESOURCE_STATE_COMPUTE_READ);
	}
	config.frameID                            = id;
	if (!FsrFgOk(g_fsr_api.configure(&s.fg_context, &config.header), "configure") || !mode_on || !usable) return;

	// The camera as DLSS-G gets it (Streamline's conventions are FSR's: jitter in render pixels,
	// vectors scaled to render pixels towards the previous position).
	sl::Constants constants {};
	DefaultConstants(constants);
	const auto render = inputs->render_extent;
	if (!CameraConstants(inputs->camera, render, s.previous_view_proj, s.previous_valid, constants)) {
		s.reset = true;
	}
	ffxDispatchDescFrameGenerationPrepareCameraInfo camera_info {};
	camera_info.header.type   = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
	camera_info.cameraPosition[0] = constants.cameraPos.x;
	camera_info.cameraPosition[1] = constants.cameraPos.y;
	camera_info.cameraPosition[2] = constants.cameraPos.z;
	camera_info.cameraUp[0]       = constants.cameraUp.x;
	camera_info.cameraUp[1]       = constants.cameraUp.y;
	camera_info.cameraUp[2]       = constants.cameraUp.z;
	camera_info.cameraRight[0]    = constants.cameraRight.x;
	camera_info.cameraRight[1]    = constants.cameraRight.y;
	camera_info.cameraRight[2]    = constants.cameraRight.z;
	camera_info.cameraForward[0]  = constants.cameraFwd.x;
	camera_info.cameraForward[1]  = constants.cameraFwd.y;
	camera_info.cameraForward[2]  = constants.cameraFwd.z;
	ffxDispatchDescFrameGenerationPrepare prepare {};
	prepare.header.type       = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
	prepare.header.pNext      = &camera_info.header;
	prepare.frameID           = id;
	prepare.flags             = debug_flags;
	prepare.commandList       = static_cast<VkCommandBuffer>(command);
	prepare.renderSize        = {render.width, render.height};
	prepare.jitterOffset      = {constants.jitterOffset.x, constants.jitterOffset.y};
	prepare.motionVectorScale = {MotionVectorSign() * static_cast<float>(render.width),
	                             MotionVectorSign() * static_cast<float>(render.height)};
	prepare.frameTimeDelta          = delta;
	prepare.cameraNear              = constants.cameraNear;
	prepare.cameraFar               = constants.cameraFar;
	prepare.cameraFovAngleVertical  = constants.cameraFOV;
	prepare.viewSpaceToMetersFactor = 1.0f;
	prepare.depth                   = FsrResource(inputs->depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	prepare.motionVectors           = FsrResource(inputs->motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	(void)FsrFgOk(g_fsr_api.dispatch(&s.fg_context, &prepare.header), "prepare");
	static bool reported = false;
	if (!reported) {
		reported = true;
		std::printf("Frame generation: FSR 2x on (render %ux%u, display %ux%u, jitter %.3f, %.3f px)\n", render.width,
		            render.height, s.display.width, s.display.height, constants.jitterOffset.x,
		            constants.jitterOffset.y);
		std::fflush(stdout);
	}
#else
	(void)command, (void)inputs, (void)backbuffer;
#endif
}

} // namespace Libs::Graphics::FrameGen
