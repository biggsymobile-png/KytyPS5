#include "common/assert.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_fsr_hudless_spv.h"
#include "gpu_blit_shaders/gpu_video_out_downscale_spv.h"
#include "gpu_blit_shaders/gpu_video_out_overlay_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/frameGenDlssg.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/openxrHeadset.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <limits>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

namespace Libs::Graphics {

struct Presenter::Frame {
	VulkanImage   image;
	vk::ImageView view         = nullptr;
	uint64_t      present_tick = 0;
	bool          busy         = false;
	bool          hdr          = false;
	bool          blank        = false;
	// DLSS frame generation's inputs for this frame (frameGenDlssg.h), taken at its guest flip.
	FrameGen::FrameInputs fg;

	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		for (auto& frame: m_frames) {
			FrameGen::DestroyInputs(m_window.graphic_ctx, frame->fg);
			m_window.graphic_ctx.device.destroyImageView(frame->view, nullptr);
			if (frame->image.image != nullptr) {
				m_window.graphic_ctx.DeleteImage(frame->image);
			}
		}
	}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count, vk::Format format) {
		if (count == 0 || format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires at least one frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!m_frames.empty()) {
			EXIT("prepared-frame pool was initialized twice\n");
		}
		m_format = format;
		m_frames.reserve(count);
		m_free.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	void SetFormat(vk::Format format) {
		if (format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires a presentation format\n");
		}
		Common::LockGuard lock(m_mutex);
		m_format = format;
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		if (m_format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool has no presentation format\n");
		}
		return m_format;
	}

	Presenter::Frame* Acquire(vk::Extent2D extent, vk::Format format) {
		m_mutex.Lock();
		if (m_frames.empty()) {
			EXIT("prepared-frame pool was used before swapchain initialization\n");
		}
		// A synchronized flip may need more frames than the swapchain has images.
		// Queue capacity bounds growth; waiting here can prevent its master from recording.
		if (m_free.empty()) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
		const auto compatible = std::ranges::find_if(m_free, [&](const auto* frame) {
			return frame->image.extent.width == extent.width &&
			       frame->image.extent.height == extent.height && frame->image.format == format;
		});
		auto*      frame      = compatible == m_free.end() ? m_free.back() : *compatible;
		if (compatible != m_free.end()) {
			*compatible = m_free.back();
		}
		m_free.pop_back();
		if (frame->busy) {
			EXIT("prepared-frame pool returned an invalid frame\n");
		}
		frame->busy = true;
		m_mutex.Unlock();

		m_scheduler.Wait(frame->present_tick);
		return frame;
	}

	void ValidateForPresent(Presenter::Frame* frame) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
	}

private:
	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::vector<Presenter::Frame*>                 m_free;
	vk::Format                                     m_format = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	auto&      dst        = image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format;
	if (compatible) {
		return;
	}

	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferSrc |
	    vk::FormatFeatureFlagBits::eTransferDst;
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	if (dst.image != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
		graphics.DeleteImage(dst);
	}

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
	               vk::ImageUsageFlagBits::eSampled;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     extent.width, extent.height, static_cast<int>(format));
	}
}

void Presenter::Frame::Transit(vk::CommandBuffer command, vk::ImageLayout layout,
                               vk::AccessFlags2 access) {
	const auto     stage  = access == vk::AccessFlagBits2::eTransferRead ||
	                                access == vk::AccessFlagBits2::eTransferWrite
	                            ? vk::PipelineStageFlagBits2::eTransfer
	                            : vk::PipelineStageFlagBits2::eAllCommands;
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite;
	if (image.state.layout == layout && image.state.access_mask == access &&
	    !static_cast<bool>(image.state.access_mask & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = image.state.pl_stage;
	barrier.srcAccessMask                   = image.state.access_mask;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
	image.subresource_states.clear();
}

void Presenter::Frame::CopyFrom(CommandBuffer& command_buffer, Image& source) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, source.backing.layers};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, image.layers};
	copy.extent         = {std::min(source.backing.extent.width, image.extent.width),
	                       std::min(source.backing.extent.height, image.extent.height), 1};
	EXIT_IF(copy.srcSubresource.layerCount != copy.dstSubresource.layerCount);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
}

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost, Minimized };

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	/// Creates the Vulkan swapchain; sets m_minimized when the window or surface extent is zero.
	void                 Create();
	/// Destroys then re-creates the swapchain, optionally recreating the Vulkan surface first.
	void                 Recreate(bool surface_lost = false);
	/// Returns true when the swapchain must be recreated before the next present.
	[[nodiscard]] bool   NeedsResize() const;
	[[nodiscard]] Status AcquireNextImage(CommandScheduler& scheduler);
	[[nodiscard]] bool   PrepareSystemOverlay();
	void     RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
	                               const Presenter::Layer& overlay, bool draw_system_overlay);
	uint64_t Submit(CommandScheduler& scheduler);
	[[nodiscard]] Status Present();

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }
	[[nodiscard]] vk::Extent2D Extent() const noexcept { return m_extent; }
	// AMD FSR 3.1 frame generation's swapchain in use (FrameGen::OwnsSwapchain at Create).
	[[nodiscard]] bool UsesFsrSwapchain() const noexcept { return m_fsr_swapchain && m_handle != nullptr; }
	// Built-in FSR frame generation (Linux, macOS): the images can be read (transfer source).
	[[nodiscard]] bool ImagesReadable() const noexcept { return m_images_readable; }
	// The image acquired last.
	[[nodiscard]] vk::Image CurrentImage() const { return m_images[m_image_index]; }
	// AMD FSR frame generation: after RecordPresentCommands (the image in the present layout), the
	// finished frame with the pixels of the game's HUD layer (inputs.hud) changed, as FSR's HUD-less
	// image (shader-read layout, `format`, the swapchain's size); null when there is none to make.
	[[nodiscard]] const VulkanImage* RecordHudless(vk::CommandBuffer command, const FrameGen::FrameInputs& inputs,
	                                               vk::Format format);
	[[nodiscard]] bool       IsMinimized() const noexcept { return m_minimized; }
	[[nodiscard]] bool IsHdrSupported() const noexcept { return m_hdr_supported.load(); }
	bool SetHdr(bool hdr) {
		const bool changed = m_hdr_active != (hdr && IsHdrSupported());
		m_hdr_requested = hdr;
		return changed;
	}

private:
	vk::SurfaceFormatKHR SelectFormat();
	/// Destroys the swapchain Vulkan objects and resets all members.
	void Destroy();
	void DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer);

	WindowContext&   m_window;
	vk::SwapchainKHR m_handle = nullptr;
	vk::Format       m_format = vk::Format::eUndefined;
	std::atomic_bool m_hdr_supported {false};
	bool             m_hdr_requested = false;
	bool             m_hdr_active    = false;
	vk::Extent2D     m_extent {};
	// Drawable pixel size observed when this swapchain was created.
	vk::Extent2D                   m_window_extent {};
	bool                           m_minimized         = false;
	/// True when m_minimized was set because the surface/drawable extent was {0,0} but
	/// WindowContext::minimized is false (e.g. compositor not ready). Cleared on successful
	/// swapchain creation or Destroy().
	bool                           m_surface_extent_zero = false;
	bool                           m_suboptimal          = false;
	bool                           m_fsr_swapchain       = false;
	bool                           m_images_readable     = false;
	// RecordHudless: a copy of the frame, and its two output images in turn (FSR may still read the
	// previous one).
	VulkanImage                    m_hud_frame;
	vk::ImageView                  m_hud_frame_view = nullptr;
	std::array<VulkanImage, 2>     m_hudless {};
	std::array<vk::ImageView, 2>   m_hudless_views {};
	uint32_t                       m_hudless_index       = 0;
	vk::Sampler                    m_hudless_sampler     = nullptr;
	vk::DescriptorSetLayout        m_hudless_descriptors = nullptr;
	vk::PipelineLayout             m_hudless_layout      = nullptr;
	vk::Pipeline                   m_hudless_pipeline    = nullptr;
	vk::Format                     m_hudless_format      = vk::Format::eUndefined;
	void                           ReleaseHudless();
	std::vector<vk::Image>         m_images;
	std::vector<vk::ImageView>     m_image_views;
	std::vector<vk::Semaphore>     m_image_acquired;
	std::vector<vk::Semaphore>     m_render_complete;
	std::vector<uint64_t>          m_frame_ticks;
	std::unique_ptr<SystemOverlay> m_system_overlay;
	vk::DescriptorSetLayout        m_overlay_descriptors = nullptr;
	vk::PipelineLayout             m_overlay_layout      = nullptr;
	vk::Pipeline                   m_overlay_pipeline    = nullptr;
	vk::Sampler                    m_overlay_sampler     = nullptr;
	PresentFilter                  m_filter; // KYTY_PRESENT_BOX_DOWNSCALE
	uint32_t                       m_image_index         = static_cast<uint32_t>(-1);
	uint32_t                       m_frame_index         = 0;
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx, CommandScheduler::Role::Presenter),
	      frames(owner, present_scheduler) {
		EXIT_IF(owner.render_context == nullptr);
		swapchain.Create();
		const uint32_t image_count = swapchain.ImageCount() > 0 ? swapchain.ImageCount() : 3;
		frames.Initialize(image_count, swapchain.Format());
	}

	// Built-in FSR frame generation: a generated frame was presented, the game's frame waits for
	// the next vblank tick (PresentPendingFrame).
	bool fg_pending = false;

	void RecoverSwapchain(Swapchain::Status status) {
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		Common::DebugCounters::Add(Common::DebugCounters::Counter::SwapchainRecreations);
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		if (!swapchain.IsMinimized()) {
			frames.SetFormat(swapchain.Format());
		}
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info                  = info;
		desc.view_info.format      = info.pixel_format;
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		desc.view_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;
		desc.type                  = TextureCache::BindingType::VideoOut;

		auto&      cache      = renderer.GetTextureCache();
		const auto image_id   = cache.FindImage(desc);
		auto&      image      = cache.GetImage(image_id);
		image.usage.video_out = true;
		cache.UpdateImage(image_id);
		return image;
	}
	void Present();

	RenderContext&        renderer;
	WindowContext&        window;
	Swapchain             swapchain;
	CommandScheduler      present_scheduler;
	FramePool             frames;
	Common::Mutex         present_mutex;
	std::array<Layer, 2>  layers {};
	std::atomic<uint64_t> presented_overlay_revision {0};
};

vk::SurfaceFormatKHR Swapchain::SelectFormat() {
	const auto& formats    = m_window.surface_capabilities.formats;
	auto&       graphics   = m_window.graphic_ctx;
	const auto  hdr_format = std::ranges::find_if(formats, [&](const auto& candidate) {
		return candidate.colorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT &&
		       IsHdrSwapchainFormat(candidate.format) &&
		       static_cast<bool>(
		           graphics.GetFormatProperties(candidate.format).optimalTilingFeatures &
		           vk::FormatFeatureFlagBits::eBlitDst);
	});
	m_hdr_supported        = Config::HdrEnabled() && hdr_format != formats.end();
	m_hdr_active           = m_hdr_requested && IsHdrSupported();
	if (m_hdr_active) {
		return *hdr_format;
	}
	if (Config::HdrEnabled() && !IsHdrSupported()) {
		LOGF("HDR10 surface unavailable; using SDR output\n");
	}
	if (formats.size() == 1 && formats.front().format == vk::Format::eUndefined) {
		return {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
	}
	for (const auto& candidate: formats) {
		if (candidate.colorSpace != vk::ColorSpaceKHR::eSrgbNonlinear) {
			continue;
		}
		switch (candidate.format) {
			case vk::Format::eB8G8R8A8Unorm:
			case vk::Format::eR8G8B8A8Unorm: return candidate;
			default: break;
		}
	}
	EXIT("no supported UNORM swapchain format\n");
}

/// Creates the Vulkan swapchain for the current window surface.
/// Sets m_minimized and returns early when the window or surface extent is zero.
/// Sets m_surface_extent_zero when the early return is due to a zero surface/drawable extent
/// and WindowContext::minimized is false, so NeedsResize() can poll instead of spinning.
void Swapchain::Create() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	EXIT_IF(m_window.surface == nullptr);

	m_window.RefreshSurfaceCapabilities();
	const auto& surface = m_window.surface_capabilities;
	EXIT_NOT_IMPLEMENTED(surface.formats.empty());

	const auto format = SelectFormat();
	m_format                      = format.format;
	const auto swapchain_features = graphics.GetFormatProperties(m_format).optimalTilingFeatures;
	if (!static_cast<bool>(swapchain_features & vk::FormatFeatureFlagBits::eBlitDst)) {
		EXIT("swapchain format cannot be a blit destination: format=%d\n",
		     static_cast<int>(m_format));
	}

	if (m_window.minimized.load(std::memory_order_acquire)) {
		// Window is genuinely minimized; m_surface_extent_zero is not the cause.
		m_minimized           = true;
		m_surface_extent_zero = false;
		return;
	}

	// Capture the size before querying surface capabilities. If the window changes
	// during creation, the next presentation will detect the newer size.
	{
		Common::LockGuard lock(m_window.mutex);
		m_window_extent = {graphics.screen_width, graphics.screen_height};
	}
	if (m_window_extent.width == 0 || m_window_extent.height == 0) {
		LOGF("Swapchain::Create(): drawable extent is zero while window is not minimized; "
		     "waiting for surface to become ready\n");
		m_minimized           = true;
		m_surface_extent_zero = true;
		return;
	}

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(m_window_extent.width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(m_window_extent.height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	if (m_extent.width == 0 || m_extent.height == 0) {
		LOGF("Swapchain::Create(): surface currentExtent is zero while window is not minimized; "
		     "waiting for surface to become ready\n");
		m_minimized           = true;
		m_surface_extent_zero = true;
		return;
	}

	m_minimized           = false;
	m_surface_extent_zero = false;
	m_suboptimal          = false;

	uint32_t image_count = surface.capabilities.minImageCount + 1;
	if (surface.capabilities.maxImageCount != 0) {
		image_count = std::min(image_count, surface.capabilities.maxImageCount);
	}
	const auto transform =
	    surface.capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
	        ? vk::SurfaceTransformFlagBitsKHR::eIdentity
	        : surface.capabilities.currentTransform;
	const auto composite =
	    surface.capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
	        ? vk::CompositeAlphaFlagBitsKHR::eOpaque
	        : vk::CompositeAlphaFlagBitsKHR::eInherit;

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType            = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.surface          = m_window.surface;
	create_info.minImageCount    = image_count;
	create_info.imageFormat      = format.format;
	create_info.imageColorSpace  = format.colorSpace;
	create_info.imageExtent      = m_extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	// FSR frame generation copies each frame out of the swapchain image (built in: its input; both:
	// the HUD-less image, RecordHudless).
	m_images_readable = (FrameGen::NativeFrameGenOn() || FrameGen::OwnsSwapchain()) &&
	                    static_cast<bool>(surface.capabilities.supportedUsageFlags &
	                                      vk::ImageUsageFlagBits::eTransferSrc) &&
	                    static_cast<bool>(graphics.GetFormatProperties(m_format).optimalTilingFeatures &
	                                      vk::FormatFeatureFlagBits::eBlitSrc);
	if (m_images_readable) {
		create_info.imageUsage |= vk::ImageUsageFlagBits::eTransferSrc;
	}
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = transform;
	create_info.compositeAlpha   = composite;
	switch (Config::GetPresentMode()) {
		case Config::PresentMode::Mailbox:
			create_info.presentMode = vk::PresentModeKHR::eMailbox;
			break;
		case Config::PresentMode::Immediate:
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
			break;
		case Config::PresentMode::Fifo:
		default: create_info.presentMode = vk::PresentModeKHR::eFifo; break;
	}
	if (std::find(surface.present_modes.begin(), surface.present_modes.end(),
	              create_info.presentMode) == surface.present_modes.end()) {
		LOGF("warning: requested present mode is unavailable; falling back to Fifo\n");
		create_info.presentMode = vk::PresentModeKHR::eFifo;
	}
	create_info.clipped = VK_TRUE;
	// AMD FSR 3.1 frame generation: its swapchain in place of ours (frameGenDlssg.h); if it cannot
	// make one, FSR frame generation is off and ours is made.
	m_fsr_swapchain = false;
	if (FrameGen::OwnsSwapchain()) {
		m_fsr_swapchain =
		    FrameGen::CreateSwapchain(graphics, create_info, m_hdr_active, &m_handle) == vk::Result::eSuccess;
	}
	if (!m_fsr_swapchain) {
		RequireVulkanSuccess(graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle),
		                     "vkCreateSwapchainKHR");
	}
	EXIT_IF(m_handle == nullptr);
	LOGF("Vulkan presentation: %s\n", m_hdr_active ? "HDR10 (PQ)" : "SDR");

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return m_fsr_swapchain ? FrameGen::GetSwapchainImages(graphics.device, m_handle, count, images)
		                           : graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());
	// Freeze reports depend on it (RTX 50 PCs froze in Mailbox, not in Fifo).
	LOGF("Swapchain: %ux%u, %zu images, present mode %s\n", m_extent.width, m_extent.height,
	     m_images.size(), vk::to_string(create_info.presentMode).c_str());

	m_image_views.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		vk::ImageViewCreateInfo view {};
		view.sType                           = vk::StructureType::eImageViewCreateInfo;
		view.image                           = m_images[i];
		view.viewType                        = vk::ImageViewType::e2D;
		view.format                          = m_format;
		view.components                      = {};
		view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.baseMipLevel   = 0;
		view.subresourceRange.layerCount     = 1;
		view.subresourceRange.levelCount     = 1;
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_image_views[i]),
		                     "vkCreateImageView");
		EXIT_IF(m_image_views[i] == nullptr);
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_render_complete.resize(m_images.size());
	m_frame_ticks.assign(m_images.size(), 0);
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
		    "create swapchain image-acquired semaphore");
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
		    "create swapchain render-complete semaphore");
	}
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
}

Swapchain::~Swapchain() {
	Destroy();
}

/// Destroys all Vulkan swapchain objects and resets all members to their default state.
void Swapchain::Destroy() {
	m_minimized           = false;
	m_surface_extent_zero = false;
	m_suboptimal          = false;
	if (m_handle == nullptr && m_image_acquired.empty() && m_render_complete.empty() &&
	    m_image_views.empty()) {
		return;
	}
	auto& graphics = m_window.graphic_ctx;

	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		graphics.submission_queue.DrainPendingLocked();
		RequireVulkanSuccess(graphics.queue.waitIdle(), "wait for swapchain queue");
	}
	if (m_system_overlay != nullptr) {
		m_system_overlay->ReleaseVulkan();
	}
	graphics.device.destroyPipeline(m_overlay_pipeline, nullptr);
	graphics.device.destroyPipelineLayout(m_overlay_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(m_overlay_descriptors, nullptr);
	graphics.device.destroySampler(m_overlay_sampler, nullptr);
	m_overlay_pipeline    = nullptr;
	m_overlay_layout      = nullptr;
	m_overlay_descriptors = nullptr;
	m_overlay_sampler     = nullptr;
	m_filter.Release(graphics.device);
	ReleaseHudless();

	for (const auto semaphore: m_image_acquired) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto semaphore: m_render_complete) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	if (m_handle != nullptr) {
		if (m_fsr_swapchain) {
			FrameGen::DestroySwapchain();
		} else {
			graphics.device.destroySwapchainKHR(m_handle, nullptr);
		}
	}
	m_fsr_swapchain = false;

	m_handle        = nullptr;
	m_format        = vk::Format::eUndefined;
	m_extent        = {};
	m_window_extent = {};
	m_image_index   = static_cast<uint32_t>(-1);
	m_frame_index   = 0;
	m_images.clear();
	m_image_views.clear();
	m_image_acquired.clear();
	m_render_complete.clear();
	m_frame_ticks.clear();
}

/// Destroys and re-creates the swapchain, optionally recreating the Vulkan surface first.
void Swapchain::Recreate(bool surface_lost) {
	FrameGen::SwapchainChanging();
	Destroy();
	if (surface_lost) {
#if defined(__APPLE__)
		// Surface recreation goes through SDL_Vulkan_CreateSurface, which touches the
		// window's view/layer and must run on the main thread on macOS.
		EXIT_IF(!SDL_RunOnMainThread(
		    [](void* window) { static_cast<WindowContext*>(window)->RecreateSurface(); }, &m_window,
		    true));
#else
		m_window.RecreateSurface();
#endif
	}
	Create();
}

/// Returns true when the swapchain must be recreated before the next present.
/// In the m_surface_extent_zero case (compositor not ready, window not minimized) this
/// re-queries the surface capabilities and only returns true when the extent is usable,
/// preventing a recreate-every-frame busy-loop.
bool Swapchain::NeedsResize() const {
	const bool window_minimized = m_window.minimized.load(std::memory_order_acquire);
	if (m_minimized) {
		if (window_minimized) {
			// OS window is minimized; nothing to do yet.
			return false;
		}
		if (!m_surface_extent_zero) {
			// Swapchain was marked minimized because WindowContext::minimized was true at
			// creation time, but it has since been cleared -> recreate now.
			return true;
		}
		// m_surface_extent_zero: the OS window exists but the compositor has not yet
		// assigned a real extent. Re-query and only trigger a recreate once the extent
		// is non-zero; otherwise return false to avoid a spinning recreate loop.
		m_window.RefreshSurfaceCapabilities();
		const auto& caps   = m_window.surface_capabilities.capabilities;
		const auto& extent = caps.currentExtent;
		// Extent 0xFFFFFFFF means the surface size is determined by the swapchain;
		// in that case read the cached drawable size under the window mutex.
		if (extent.width == std::numeric_limits<uint32_t>::max()) {
			Common::LockGuard lock(m_window.mutex);
			const bool drawable_ready = m_window.graphic_ctx.screen_width > 0 &&
			                            m_window.graphic_ctx.screen_height > 0;
			return drawable_ready;
		}
		Common::LockGuard lock(m_window.mutex);
		const bool drawable_ready = m_window.graphic_ctx.screen_width > 0 &&
		                            m_window.graphic_ctx.screen_height > 0;
		return drawable_ready && extent.width > 0 && extent.height > 0;
	}
	if (window_minimized) {
		return true;
	}
	Common::LockGuard lock(m_window.mutex);
	return m_window_extent.width != m_window.graphic_ctx.screen_width ||
	       m_window_extent.height != m_window.graphic_ctx.screen_height;
}

Swapchain::Status Swapchain::AcquireNextImage(CommandScheduler& scheduler) {
	if (m_minimized || m_handle == nullptr) {
		return Status::Minimized;
	}
	EXIT_IF(m_frame_index >= m_image_acquired.size());
	if (m_frame_index < m_frame_ticks.size() && m_frame_ticks[m_frame_index] != 0) {
		scheduler.Wait(m_frame_ticks[m_frame_index]);
		m_frame_ticks[m_frame_index] = 0;
	}
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result =
	    m_fsr_swapchain
	        ? FrameGen::AcquireNextImage(m_window.graphic_ctx.device, m_handle, std::numeric_limits<uint64_t>::max(),
	                                     m_image_acquired[m_frame_index], &m_image_index)
	        : m_window.graphic_ctx.device.acquireNextImageKHR(m_handle, std::numeric_limits<uint64_t>::max(),
	                                                          m_image_acquired[m_frame_index], nullptr,
	                                                          &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			m_suboptimal = true;
			break;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

bool Swapchain::PrepareSystemOverlay() {
	if (m_system_overlay == nullptr) {
		m_system_overlay = std::make_unique<SystemOverlay>(m_window.graphic_ctx);
	}
	return m_system_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

// KYTY_PRESENT_BOX_DOWNSCALE=1: present 1-2x downscales with PresentFilter (windowInternal.h).
static bool PresentBoxDownscaleEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_PRESENT_BOX_DOWNSCALE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

// A pipeline that draws the fullscreen triangle with `fragment_code` into one `format` attachment.
static vk::Pipeline CreateFullscreenPipeline(vk::Device device, vk::PipelineLayout layout,
                                             std::span<const uint32_t> fragment_code,
                                             vk::Format format) {
	const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
	const auto fragment = CompileSPV(fragment_code, device);
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = vertex;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = fragment;
	stages[1].pName  = "main";
	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo assembly {};
	assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
	vk::PipelineColorBlendAttachmentState attachment {};
	attachment.blendEnable    = VK_FALSE;
	attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blend {};
	blend.attachmentCount = 1;
	blend.pAttachments    = &attachment;
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();
	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &format;
	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pColorBlendState    = &blend;
	create.pDynamicState       = &dynamic;
	create.layout              = layout;
	vk::Pipeline pipeline      = nullptr;
	RequireVulkanSuccess(device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	                     "create video-out downscale pipeline");
	device.destroyShaderModule(fragment, nullptr);
	device.destroyShaderModule(vertex, nullptr);
	return pipeline;
}

bool PresentNeedsFilter(vk::Extent2D source, vk::Extent2D target) noexcept {
	const bool shrinks = source.width > target.width || source.height > target.height;
	const bool halves =
	    source.width == 2ull * target.width && source.height == 2ull * target.height;
	return shrinks && !halves && source.width <= 2ull * target.width &&
	       source.height <= 2ull * target.height;
}

void PresentFilter::Record(vk::Device device, vk::CommandBuffer command, vk::ImageView source_view,
                           uint32_t level, vk::Extent2D source, vk::ImageView target_view,
                           vk::Format target_format, vk::Extent2D target) {
	struct Constants {
		int32_t  size[2];
		float    scale[2];
		uint32_t level;
	};
	static_assert(sizeof(Constants) == 20);
	if (m_layout == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.mipmapMode   = vk::SamplerMipmapMode::eNearest;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		sampler.maxLod       = VK_LOD_CLAMP_NONE;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_sampler),
		                     "create video-out downscale sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(device.createDescriptorSetLayout(&descriptors, nullptr, &m_descriptors),
		                     "create video-out downscale descriptor layout");
		const vk::PushConstantRange  constants {vk::ShaderStageFlagBits::eFragment, 0,
		                                        sizeof(Constants)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &constants;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_layout),
		                     "create video-out downscale pipeline layout");
	}
	if (m_pipeline != nullptr && m_format != target_format) {
		device.destroyPipeline(m_pipeline, nullptr);
		m_pipeline = nullptr;
	}
	if (m_pipeline == nullptr) {
		m_pipeline = CreateFullscreenPipeline(device, m_layout, GPU_VIDEO_OUT_DOWNSCALE_SPV, target_format);
		m_format   = target_format;
	}

	const vk::DescriptorImageInfo image {m_sampler, source_view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_layout, 0, 1, &write);
	const Constants constants {
	    {static_cast<int32_t>(source.width), static_cast<int32_t>(source.height)},
	    {static_cast<float>(source.width) / static_cast<float>(target.width),
	     static_cast<float>(source.height) / static_cast<float>(target.height)},
	    level};
	command.pushConstants(m_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants),
	                      &constants);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = target_view;
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eDontCare;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = target;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {
	    0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f};
	const vk::Rect2D scissor {{0, 0}, target};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void PresentFilter::Release(vk::Device device) {
	device.destroyPipeline(m_pipeline, nullptr);
	device.destroyPipelineLayout(m_layout, nullptr);
	device.destroyDescriptorSetLayout(m_descriptors, nullptr);
	device.destroySampler(m_sampler, nullptr);
	m_pipeline    = nullptr;
	m_layout      = nullptr;
	m_descriptors = nullptr;
	m_sampler     = nullptr;
	m_format      = vk::Format::eUndefined;
}

void Swapchain::ReleaseHudless() {
	auto device = m_window.graphic_ctx.device;
	for (size_t i = 0; i <= m_hudless.size(); ++i) {
		auto& image = i < m_hudless.size() ? m_hudless[i] : m_hud_frame;
		auto& view  = i < m_hudless.size() ? m_hudless_views[i] : m_hud_frame_view;
		if (view != nullptr) device.destroyImageView(view, nullptr);
		view = nullptr;
		if (image.image != nullptr) m_window.graphic_ctx.DeleteImage(image);
	}
	device.destroyPipeline(m_hudless_pipeline, nullptr);
	device.destroyPipelineLayout(m_hudless_layout, nullptr);
	device.destroyDescriptorSetLayout(m_hudless_descriptors, nullptr);
	device.destroySampler(m_hudless_sampler, nullptr);
	m_hudless_pipeline    = nullptr;
	m_hudless_layout      = nullptr;
	m_hudless_descriptors = nullptr;
	m_hudless_sampler     = nullptr;
	m_hudless_format      = vk::Format::eUndefined;
}

const VulkanImage* Swapchain::RecordHudless(vk::CommandBuffer command, const FrameGen::FrameInputs& inputs,
                                            vk::Format format) {
	if (!m_images_readable || !inputs.has_hud || inputs.hud_view == nullptr || m_handle == nullptr ||
	    m_image_index >= m_images.size()) {
		return nullptr;
	}
	auto& graphics = m_window.graphic_ctx;
	auto  device   = graphics.device;
	// (Images at the swapchain's size; made again for another size or format, which only a new
	// swapchain brings, after the GPU is done with the old ones.)
	const auto make = [&](VulkanImage& image, vk::ImageView& view, vk::Format image_format,
	                      vk::ImageUsageFlags usage) {
		if (image.image != nullptr && image.format == image_format && image.extent.width == m_extent.width &&
		    image.extent.height == m_extent.height) {
			return true;
		}
		if (view != nullptr) device.destroyImageView(view, nullptr);
		view = nullptr;
		if (image.image != nullptr) {
			{
				Common::LockGuard lock(graphics.queue_mutex);
				(void)graphics.queue.waitIdle();
			}
			graphics.DeleteImage(image);
		}
		vk::ImageCreateInfo create {};
		create.imageType     = vk::ImageType::e2D;
		create.extent        = vk::Extent3D {m_extent.width, m_extent.height, 1};
		create.mipLevels     = 1;
		create.arrayLayers   = 1;
		create.format        = image_format;
		create.tiling        = vk::ImageTiling::eOptimal;
		create.initialLayout = vk::ImageLayout::eUndefined;
		create.usage         = usage;
		create.sharingMode   = vk::SharingMode::eExclusive;
		create.samples       = vk::SampleCountFlagBits::e1;
		if (!graphics.CreateImage(create, image)) return false;
		image.state.layout      = vk::ImageLayout::eUndefined;
		image.state.access_mask = {};
		image.state.pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
		vk::ImageViewCreateInfo info {};
		info.image            = image.image;
		info.viewType         = vk::ImageViewType::e2D;
		info.format           = image_format;
		info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		return device.createImageView(&info, nullptr, &view) == vk::Result::eSuccess;
	};
	using Usage       = vk::ImageUsageFlagBits;
	const auto  index = m_hudless_index;
	if (!make(m_hud_frame, m_hud_frame_view, m_format, Usage::eTransferDst | Usage::eSampled) ||
	    !make(m_hudless[index], m_hudless_views[index], format,
	          Usage::eColorAttachment | Usage::eSampled | Usage::eTransferSrc)) {
		static bool reported = false;
		if (!std::exchange(reported, true)) {
			std::printf("Frame generation: no memory for FSR's HUD-less image; the HUD is interpolated\n");
			std::fflush(stdout);
		}
		return nullptr;
	}
	m_hudless_index = (m_hudless_index + 1u) % static_cast<uint32_t>(m_hudless.size());
	auto& output    = m_hudless[index];

	struct Constants {
		float inv_size[2];
		float hud_texel[2];
		float threshold;
	};
	static_assert(sizeof(Constants) == 20);
	if (m_hudless_layout == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_hudless_sampler),
		                     "create FSR HUD-less sampler");
		const std::array<vk::DescriptorSetLayoutBinding, 2> bindings {
		    vk::DescriptorSetLayoutBinding {0, vk::DescriptorType::eCombinedImageSampler, 1,
		                                    vk::ShaderStageFlagBits::eFragment},
		    vk::DescriptorSetLayoutBinding {1, vk::DescriptorType::eCombinedImageSampler, 1,
		                                    vk::ShaderStageFlagBits::eFragment}};
		vk::DescriptorSetLayoutCreateInfo descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = static_cast<uint32_t>(bindings.size());
		descriptors.pBindings    = bindings.data();
		RequireVulkanSuccess(device.createDescriptorSetLayout(&descriptors, nullptr, &m_hudless_descriptors),
		                     "create FSR HUD-less descriptor layout");
		const vk::PushConstantRange  constants {vk::ShaderStageFlagBits::eFragment, 0, sizeof(Constants)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_hudless_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &constants;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_hudless_layout),
		                     "create FSR HUD-less pipeline layout");
	}
	if (m_hudless_pipeline != nullptr && m_hudless_format != format) {
		device.destroyPipeline(m_hudless_pipeline, nullptr);
		m_hudless_pipeline = nullptr;
	}
	if (m_hudless_pipeline == nullptr) {
		m_hudless_pipeline = CreateFullscreenPipeline(device, m_hudless_layout, GPU_FSR_HUDLESS_SPV, format);
		m_hudless_format   = format;
	}

	const auto barrier = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags2 src,
	                         vk::AccessFlags2 dst) {
		vk::ImageMemoryBarrier2 b {};
		b.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
		b.srcAccessMask       = src;
		b.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
		b.dstAccessMask       = dst;
		b.oldLayout           = from;
		b.newLayout           = to;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image               = image;
		b.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		vk::DependencyInfo dependency {};
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers    = &b;
		command.pipelineBarrier2(dependency);
	};
	const auto track = [](VulkanImage& image, vk::ImageLayout layout, vk::AccessFlags2 access) {
		image.state.layout      = layout;
		image.state.access_mask = access;
		image.state.pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
	};
	// The finished frame copied out (the swapchain image back in the present layout).
	const auto backbuffer = m_images[m_image_index];
	barrier(backbuffer, vk::ImageLayout::ePresentSrcKHR, vk::ImageLayout::eTransferSrcOptimal,
	        vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferRead);
	barrier(m_hud_frame.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
	        vk::AccessFlagBits2::eMemoryRead, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.extent         = vk::Extent3D {m_extent.width, m_extent.height, 1};
	command.copyImage(backbuffer, vk::ImageLayout::eTransferSrcOptimal, m_hud_frame.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
	barrier(backbuffer, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::ePresentSrcKHR,
	        vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eMemoryRead);
	barrier(m_hud_frame.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
	        vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderRead);
	track(m_hud_frame, vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead);
	barrier(output.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
	        vk::AccessFlagBits2::eMemoryRead, vk::AccessFlagBits2::eColorAttachmentWrite);

	// The frame with the HUD's pixels changed.
	const std::array<vk::DescriptorImageInfo, 2> images {
	    vk::DescriptorImageInfo {m_hudless_sampler, m_hud_frame_view, vk::ImageLayout::eShaderReadOnlyOptimal},
	    vk::DescriptorImageInfo {m_hudless_sampler, inputs.hud_view, vk::ImageLayout::eShaderReadOnlyOptimal}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); ++i) {
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		writes[i].pImageInfo      = &images[i];
	}
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_hudless_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	// KYTY_FSR_HUD_THRESHOLD: how far a HUD-layer texel must be from the empty one to be HUD.
	static const float threshold = [] {
		const char* value = std::getenv("KYTY_FSR_HUD_THRESHOLD");
		return value != nullptr && value[0] != '\0' ? static_cast<float>(std::atof(value)) : 0.02f;
	}();
	const Constants constants {
	    {1.0f / static_cast<float>(m_extent.width), 1.0f / static_cast<float>(m_extent.height)},
	    {1.0f / static_cast<float>(std::max(inputs.hud.extent.width, 1u)),
	     1.0f / static_cast<float>(std::max(inputs.hud.extent.height, 1u))},
	    threshold};
	command.pushConstants(m_hudless_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants), &constants);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_hudless_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_hudless_views[index];
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eDontCare;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = m_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {0.0f, 0.0f, static_cast<float>(m_extent.width),
	                             static_cast<float>(m_extent.height), 0.0f, 1.0f};
	const vk::Rect2D   scissor {{0, 0}, m_extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
	// (FSR's "compute read" state.)
	barrier(output.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
	        vk::AccessFlagBits2::eColorAttachmentWrite, vk::AccessFlagBits2::eShaderRead);
	track(output, vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead);
	static bool reported = false;
	if (!std::exchange(reported, true)) {
		std::printf("Frame generation: FSR gets a HUD-less image (%ux%u) made from the game's HUD layer\n",
		            m_extent.width, m_extent.height);
		std::fflush(stdout);
	}
	return &output;
}

void Swapchain::DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer) {
	auto device = m_window.graphic_ctx.device;
	if (m_overlay_pipeline == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_overlay_sampler),
		                     "create video-out overlay sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&descriptors, nullptr, &m_overlay_descriptors),
		    "create video-out overlay descriptor layout");
		const vk::PushConstantRange alpha {vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_overlay_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &alpha;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_overlay_layout),
		                     "create video-out overlay pipeline layout");
		const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
		const auto fragment = CompileSPV(GPU_VIDEO_OUT_OVERLAY_SPV, device);
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = vertex;
		stages[0].pName  = "main";
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = fragment;
		stages[1].pName  = "main";
		vk::PipelineVertexInputStateCreateInfo   vertex_input {};
		vk::PipelineInputAssemblyStateCreateInfo assembly {};
		assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;
		vk::PipelineRasterizationStateCreateInfo rasterization {};
		rasterization.lineWidth = 1.0f;
		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
		vk::PipelineColorBlendAttachmentState attachment {};
		attachment.blendEnable         = VK_TRUE;
		attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
		attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
		                            vk::ColorComponentFlagBits::eG |
		                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		vk::PipelineColorBlendStateCreateInfo blend {};
		blend.attachmentCount = 1;
		blend.pAttachments    = &attachment;
		const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
		dynamic.pDynamicStates    = dynamic_states.data();
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.colorAttachmentCount    = 1;
		rendering.pColorAttachmentFormats = &m_format;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = static_cast<uint32_t>(stages.size());
		create.pStages             = stages.data();
		create.pVertexInputState   = &vertex_input;
		create.pInputAssemblyState = &assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &rasterization;
		create.pMultisampleState   = &multisample;
		create.pColorBlendState    = &blend;
		create.pDynamicState       = &dynamic;
		create.layout              = m_overlay_layout;
		RequireVulkanSuccess(
		    device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_overlay_pipeline),
		    "create video-out overlay pipeline");
		device.destroyShaderModule(fragment, nullptr);
		device.destroyShaderModule(vertex, nullptr);
	}
	auto& frame = *layer.frame;
	if (frame.view == nullptr) {
		vk::ImageViewCreateInfo view {};
		view.image            = frame.image.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = frame.image.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(device.createImageView(&view, nullptr, &frame.view),
		                     "create video-out overlay image view");
	}
	const vk::DescriptorImageInfo image {m_overlay_sampler, frame.view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_overlay_layout, 0, 1, &write);
	const uint32_t flags = (layer.premultiplied_alpha ? 1u : 0u) |
	                       (m_hdr_active ? 2u : 0u) | (frame.hdr ? 4u : 0u) |
	                       (frame.image.format == vk::Format::eR16G16B16A16Sfloat ? 8u : 0u);
	command.pushConstants(m_overlay_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(flags), &flags);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_overlay_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_image_views[m_image_index];
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = m_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {
	    0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height),
	    0.0f, 1.0f};
	const vk::Rect2D scissor {{0, 0}, m_extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void Swapchain::RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
                                      const Presenter::Layer& overlay, bool draw_system_overlay) {
	EXIT_IF(m_image_index >= m_images.size());
	auto       vk_command      = command.Handle();
	const bool draw_overlay    = overlay.frame != nullptr;
	const bool draw_attachment = draw_overlay || draw_system_overlay;
	// KYTY_PRESENT_BOX_DOWNSCALE: shrinking by less than 2x draws with PresentFilter instead of
	// the blit (windowInternal.h).
	const vk::Extent2D source_extent =
	    source != nullptr ? vk::Extent2D {source->image.extent.width, source->image.extent.height}
	                      : vk::Extent2D {};
	const bool filtered = source != nullptr && PresentBoxDownscaleEnabled() &&
	                      PresentNeedsFilter(source_extent, m_extent);
	if (filtered) {
		if (source->view == nullptr) {
			vk::ImageViewCreateInfo view {};
			view.image            = source->image.image;
			view.viewType         = vk::ImageViewType::e2D;
			view.format           = source->image.format;
			view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			RequireVulkanSuccess(
			    m_window.graphic_ctx.device.createImageView(&view, nullptr, &source->view),
			    "create presentation source view");
		}
		source->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                vk::AccessFlagBits2::eShaderRead);
	} else if (source != nullptr) {
		source->Transit(vk_command, vk::ImageLayout::eTransferSrcOptimal,
		                vk::AccessFlagBits2::eTransferRead);
	}
	if (draw_overlay) {
		overlay.frame->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                       vk::AccessFlagBits2::eShaderRead);
	}

	vk::ImageMemoryBarrier to_transfer {};
	to_transfer.sType = vk::StructureType::eImageMemoryBarrier;
	to_transfer.dstAccessMask =
	    filtered ? vk::AccessFlagBits::eColorAttachmentWrite : vk::AccessFlagBits::eTransferWrite;
	to_transfer.oldLayout = vk::ImageLayout::eUndefined;
	to_transfer.newLayout =
	    filtered ? vk::ImageLayout::eColorAttachmentOptimal : vk::ImageLayout::eTransferDstOptimal;
	to_transfer.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image                           = m_images[m_image_index];
	to_transfer.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_transfer.subresourceRange.baseMipLevel   = 0;
	to_transfer.subresourceRange.levelCount     = 1;
	to_transfer.subresourceRange.baseArrayLayer = 0;
	to_transfer.subresourceRange.layerCount     = 1;
	// Match the acquire wait stage so the layout transition cannot precede acquisition.
	const auto write_stage = filtered ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                  : vk::PipelineStageFlagBits::eTransfer;
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, write_stage,
	                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &to_transfer);

	if (filtered) {
		m_filter.Record(m_window.graphic_ctx.device, vk_command, source->view, 0, source_extent,
		                m_image_views[m_image_index], m_format, m_extent);
	} else if (source != nullptr) {
		vk::ImageBlit region {};
		region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.srcSubresource.mipLevel       = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount     = 1;
		region.srcOffsets[1].x               = static_cast<int>(source->image.extent.width);
		region.srcOffsets[1].y               = static_cast<int>(source->image.extent.height);
		region.srcOffsets[1].z               = 1;
		region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.dstSubresource.mipLevel       = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount     = 1;
		region.dstOffsets[1].x               = static_cast<int>(m_extent.width);
		region.dstOffsets[1].y               = static_cast<int>(m_extent.height);
		region.dstOffsets[1].z               = 1;
		vk_command.blitImage(source->image.image, vk::ImageLayout::eTransferSrcOptimal,
		                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1,
		                     &region, vk::Filter::eLinear);
	} else {
		const vk::ClearColorValue black {};
		vk_command.clearColorImage(m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal,
		                           &black, 1, &to_transfer.subresourceRange);
	}

	vk::ImageMemoryBarrier to_present {};
	to_present.sType               = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask =
	    filtered ? vk::AccessFlagBits::eColorAttachmentWrite : vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask       = draw_attachment ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                       vk::AccessFlagBits::eColorAttachmentWrite
	                                                 : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout           = to_transfer.newLayout;
	to_present.newLayout           = draw_attachment ? vk::ImageLayout::eColorAttachmentOptimal
	                                                 : vk::ImageLayout::ePresentSrcKHR;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image               = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(write_stage,
	                           draw_attachment ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                           : vk::PipelineStageFlagBits::eAllCommands,
	                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
	                           &to_present);
	if (draw_attachment) {
		if (draw_overlay) {
			// AMD FSR frame generation: the frame before the game's HUD layer is drawn over it, FSR's
			// HUD-less image (it then keeps the HUD out of the generated frames).
			if (m_fsr_swapchain) {
				FrameGen::RecordFsrHudless(m_window.graphic_ctx, vk_command, m_images[m_image_index],
				                           vk::ImageLayout::eColorAttachmentOptimal, m_format, m_extent);
			}
			DrawOverlay(vk_command, overlay);
			if (draw_system_overlay) {
				to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
				to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
				vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0,
				                           nullptr, 1, &to_present);
			}
		}
		if (draw_system_overlay) {
			m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
		}
		to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
		to_present.newLayout     = vk::ImageLayout::ePresentSrcKHR;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	const auto tick = scheduler.Submit(submit);
	if (m_frame_index < m_frame_ticks.size()) {
		m_frame_ticks[m_frame_index] = tick;
	}
	return tick;
}

Swapchain::Status Swapchain::Present() {
	if (m_minimized || m_handle == nullptr) {
		return Status::Minimized;
	}
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		// The blit this present waits for goes ahead of game batches still waiting for their
		// texture copies: the present never waits for them, nor for a signal not yet submitted.
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		m_window.graphic_ctx.submission_queue.DrainReadyForPresentLocked();
		result = m_fsr_swapchain ? FrameGen::QueuePresent(m_window.graphic_ctx.queue, present)
		                         : m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	if (m_suboptimal) {
		m_suboptimal = false;
		return Status::Recreate;
	}
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

// KYTY_PRESENT_UNLOCKED (default on; =0 keeps the renderer's lock; chenxiao07/KytyPS5 a30144267): the
// present thread records and submits the swapchain blit without the renderer's lock. What it uses is
// the presenter's own: present_scheduler (its pool, timeline and command buffers; only the present
// thread records on it), the presenter's frames (the flip queue hands a frame over only once the GPU
// completed its copy, FlipQueue::Complete) and the swapchain images, and the graphics queue, which
// has its own lock (queue_mutex, also taken by the RenderDoc capture boundaries). Under the
// renderer's lock the GPU thread waited for that whole recording and submission once a frame. (This
// tree has no frame generation, which shares the renderer's state and keeps the lock in theirs.)
static bool PresentUnlocked() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_PRESENT_UNLOCKED");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

Presenter::~Presenter() = default;

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info, bool hdr,
                                          bool headset) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto frame_format = info.pixel_format;
	switch (frame_format) {
		case vk::Format::eR8G8B8A8Srgb: frame_format = vk::Format::eR8G8B8A8Unorm; break;
		case vk::Format::eB8G8R8A8Srgb: frame_format = vk::Format::eB8G8R8A8Unorm; break;
		default: break;
	}
	auto* frame = m_impl->frames.Acquire({info.extent.width, info.extent.height}, frame_format);
	frame->hdr   = hdr;
	frame->blank = false;
	RenderLockGuard   render_lock(m_impl->renderer.GetMutex());
	auto&             image = m_impl->ResolveSurface(info);
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}
	frame->Configure(m_impl->window.graphic_ctx,
	                 {image.backing.extent.width, image.backing.extent.height}, frame_format);
	frame->CopyFrom(buffer, image);
	if (headset) {
		// The frame holds the raw display-encoded pixels the headset needs.
		frame->Transit(buffer.Handle(), vk::ImageLayout::eTransferSrcOptimal,
		               vk::AccessFlagBits2::eTransferRead);
		Vr::OpenXrQueueEyeSource(buffer, frame->image);
	}
	// DLSS frame generation: this guest frame's depth, motion and camera, when it had them.
	frame->fg.valid = false;
	if (FrameGen::Gathering()) {
		FrameGen::PrepareInputs(m_impl->window.graphic_ctx, m_impl->renderer.GetTextureCache(), buffer,
		                        frame->fg);
	}
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
	auto              format = m_impl->frames.GetFormat();
	auto*             frame  = m_impl->frames.Acquire({width, height}, format);
	frame->hdr   = false;
	frame->blank = true;
	// Into the GPU thread's command buffer (its flip) under the renderer's lock; the present
	// thread's blank frame (no producer) uses only the presenter's scheduler and frame.
	std::optional<RenderLockGuard> render_lock;
	if (producer != nullptr || !PresentUnlocked()) {
		render_lock.emplace(m_impl->renderer.GetMutex());
	}
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	if (producer != nullptr) {
		EXIT_IF(producer->IsInvalid());
		frame->Clear(*producer, clear);
	} else {
		auto& command = m_impl->present_scheduler.BeginCommand();
		frame->Clear(command, clear);
		frame->present_tick = m_impl->present_scheduler.Submit();
	}
	return *frame;
}

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (m_impl->layers[0].frame == nullptr && m_impl->layers[1].frame == nullptr) {
		return false;
	}
	m_impl->Present();
	return true;
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.loop.paused.load(std::memory_order_acquire);
}

bool Presenter::IsHdrSupported() const noexcept {
	return m_impl->swapchain.IsHdrSupported();
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	const auto visual = GetSystemOverlayVisualState();
	return visual.active ||
	       visual.revision != m_impl->presented_overlay_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::Present(Frame& frame) {
	const Layer layer {&frame, 0, false};
	Present(std::span(&layer, 1));
}

void Presenter::Present(std::span<const Layer> layers) {
	Common::LockGuard lock(m_impl->present_mutex);
	for (const auto& layer: layers) {
		EXIT_IF(layer.bus < 0 || layer.bus >= static_cast<int>(m_impl->layers.size()));
		m_impl->frames.ValidateForPresent(layer.frame);
		auto& previous = m_impl->layers[layer.bus];
		if (previous.frame != nullptr) {
			m_impl->frames.Release(previous.frame);
		}
		previous = layer;
	}
	m_impl->Present();
}

void Presenter::ClearLayer(int bus) {
	if (static_cast<size_t>(bus) >= m_impl->layers.size()) {
		return;
	}
	Common::LockGuard lock(m_impl->present_mutex);
	auto& layer = m_impl->layers[bus];
	if (layer.frame != nullptr) {
		m_impl->frames.Release(layer.frame);
		layer = {};
	}
}

void Presenter::Impl::Present() {
	KYTY_PROFILER_FUNCTION();

	const auto overlay_visual = GetSystemOverlayVisualState();
	// Keep black/blank flips in the current color space; only real main-bus frames switch it.
	const auto* main_frame = layers[0].frame;
	const bool hdr_changed = main_frame != nullptr && !main_frame->blank &&
	                         swapchain.SetHdr(main_frame->hdr);
	// Some window systems keep presenting an old swapchain after a resize.
	if (hdr_changed || swapchain.NeedsResize()) {
		RecoverSwapchain(Swapchain::Status::Recreate);
	}
	if (swapchain.IsMinimized()) {
		return;
	}
	bool generated = false;
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		auto status = swapchain.AcquireNextImage(present_scheduler);
		if (status == Swapchain::Status::Minimized) {
			return;
		}
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			if (swapchain.IsMinimized()) {
				return;
			}
			continue;
		}
		{
			std::optional<RenderLockGuard> render_lock;
			if (!PresentUnlocked()) {
				render_lock.emplace(renderer.GetMutex());
			}
			auto&             command = present_scheduler.BeginCommand();
			const bool        draw_system_overlay =
			    (overlay_visual.active || PerformanceOverlayEnabled()) && swapchain.PrepareSystemOverlay();
			swapchain.RecordPresentCommands(command, layers[0].frame, layers[1],
			                                draw_system_overlay);
			// AMD FSR 3.1 frame generation: its settings for this present and its preparation from
			// the frame's depth, motion and camera (a repeat or a menu has none).
			if (swapchain.UsesFsrSwapchain()) {
				auto*       fg_frame = layers[0].frame;
				const auto* inputs   = fg_frame != nullptr && fg_frame->fg.valid ? &fg_frame->fg : nullptr;
				// (FSR's HUD-less image, from the game's HUD layer, when the frame has one.)
				const VulkanImage* hudless =
				    inputs != nullptr && inputs->has_hud && FrameGen::HudlessWanted()
				        ? swapchain.RecordHudless(command.Handle(), *inputs, swapchain.Format())
				        : nullptr;
				FrameGen::RecordFsrPrepare(window.graphic_ctx, command.Handle(), inputs, swapchain.Extent(),
				                           hudless);
				if (fg_frame != nullptr) fg_frame->fg.valid = false;
			}
			// Built-in FSR frame generation (Linux, macOS): a new game frame (one with inputs) puts
			// the generated frame in this image; the game's frame follows at the next vblank tick
			// (PresentPendingFrame). One still waiting there means the game outpaces the ticks: that
			// one is dropped, and this frame is shown as it is.
			generated = false;
			if (FrameGen::NativeFrameGenOn() && swapchain.ImagesReadable()) {
				auto*      fg_frame = layers[0].frame;
				const bool waiting  = std::exchange(fg_pending, false);
				const bool fresh    = fg_frame != nullptr && fg_frame->fg.valid;
				// (Only new game frames: a repeat of the last one carries no new inputs.)
				if (fresh) {
					// (FSR's HUD-less image, in the format of its copy of the frame.)
					const VulkanImage* hudless =
					    fg_frame->fg.has_hud && FrameGen::HudlessWanted()
					        ? swapchain.RecordHudless(command.Handle(), fg_frame->fg, vk::Format::eR8G8B8A8Unorm)
					        : nullptr;
					generated = FrameGen::RecordNativeFrameGen(command.Handle(), &fg_frame->fg,
					                                           swapchain.CurrentImage(), swapchain.Format(),
					                                           swapchain.Extent(), !waiting, hudless);
				}
				if (fg_frame != nullptr) fg_frame->fg.valid = false;
			}
			const auto tick = swapchain.Submit(present_scheduler);
			for (const auto& layer: layers) {
				if (layer.frame != nullptr) {
					layer.frame->present_tick = tick;
				}
			}
		}
		// DLSS frame generation: its constants, tags and markers around the present (Streamline
		// makes and shows the generated frames inside vkQueuePresentKHR). A frame shown again
		// (a repeat) has no inputs.
		// (Requested, not Enabled: BeforePresent itself finishes DLSS-G's setup once the
		// swapchain exists.)
		const bool frame_gen = FrameGen::Requested();
		if (frame_gen) {
			auto* fg_frame = layers[0].frame;
			FrameGen::BeforePresent(fg_frame != nullptr && fg_frame->fg.valid ? &fg_frame->fg : nullptr,
			                        swapchain.Extent(), swapchain.Format(), swapchain.ImageCount());
			if (fg_frame != nullptr) fg_frame->fg.valid = false;
		}
		status = swapchain.Present();
		if (frame_gen) {
			FrameGen::AfterPresent();
		}
		if (status == Swapchain::Status::Minimized) {
			return;
		}
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			if (swapchain.IsMinimized()) {
				return;
			}
			continue;
		}

		presented_overlay_revision.store(overlay_visual.revision, std::memory_order_release);
		window.loop.presented_frames.fetch_add(1, std::memory_order_relaxed);
		fg_pending = generated;
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
	Common::DebugCounters::Add(Common::DebugCounters::Counter::DroppedPresents);
}

bool Presenter::PresentPendingFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (!std::exchange(m_impl->fg_pending, false) || m_impl->swapchain.IsMinimized()) {
		return false;
	}
	auto& impl   = *m_impl;
	auto  status = impl.swapchain.AcquireNextImage(impl.present_scheduler);
	if (status != Swapchain::Status::Success) {
		if (status != Swapchain::Status::Minimized) impl.RecoverSwapchain(status);
		return false;
	}
	auto& command = impl.present_scheduler.BeginCommand();
	FrameGen::RecordNativeRealFrame(command.Handle(), impl.swapchain.CurrentImage(), impl.swapchain.Extent());
	(void)impl.swapchain.Submit(impl.present_scheduler);
	status = impl.swapchain.Present();
	if (status != Swapchain::Status::Success) {
		if (status != Swapchain::Status::Minimized) impl.RecoverSwapchain(status);
		return false;
	}
	impl.window.loop.presented_frames.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
