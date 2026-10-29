#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAME_GEN_DLSSG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAME_GEN_DLSSG_H_

// DLSS frame generation (DLSS-G) through NVIDIA Streamline, for Astro Bot (PPSA21564).
//
// The Streamline integration (loading sl.interposer.dll as SDL's Vulkan loader, DLSS-G options,
// constants, tags, Reflex/PCL markers at the present) follows chenxiao07/KytyPS5's
// src/local/frame-gen.cpp, which does this for Demon's Souls. The inputs are Astro Bot's,
// found with the fg-discovery logger (docs/RE-ASTRO-BOT-FG.md): the motion vectors its TAA
// pass reads, the G-buffer's depth and the per-view constant buffer of its G-buffer draws.
//
// KYTY_FRAMEGEN=<n> (default 1 in this branch, 0 off): frames generated per rendered frame
// (1 = 2x; 2 and 3 are multi frame generation, RTX 50).
// KYTY_STREAMLINE_DIR=<folder>: NVIDIA's Streamline runtime (sl.interposer.dll and its plugins,
// the SDK's bin\x64). Default: a "streamline" folder next to kyty_emulator.exe.
// KYTY_FRAMEGEN_MV_SIGN=1 keeps the game's motion vector sign (a comparison; default negated).
// KYTY_DLSS=1: DLSS super resolution (DLSS-SR, Streamline's sl.dlss) in place of the game's TAA
// pass, from the render size the game draws at (a "Fixed render resolution" patch lowers it) to
// the TAA's output size (DLAA when they are equal); with or without frame generation.
// KYTY_FSR=1: AMD FSR 3.1 upscaling the same way (any GPU, no Streamline needed; the FidelityFX
// API's amd_fidelityfx_vk.dll next to the emulator); KYTY_FSR_SHARPNESS=0..1 adds its sharpening.
// KYTY_DLSS_JITTER=projection takes the jitter from the camera's projection instead of the TAA's
// constants; KYTY_DLSS_JITTER_X=-1 / KYTY_DLSS_JITTER_Y=-1 flip it; KYTY_DLSS_MV_JITTERED=1 tells
// DLSS the motion vectors include the jitter.
// DLSS: Windows only, and only in builds with Streamline's headers (KYTY_FRAMEGEN_STREAMLINE).
// FSR: Windows through AMD's runtime; Linux and macOS built in (KYTY_FSR_NATIVE).

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
class TextureCache;
struct PreparedBindings;
struct RenderColorInfo;
struct RenderDepthInfo;

namespace FrameGen {

// Astro Bot's TAA pixel shader (docs/RE-ASTRO-BOT-FG.md): its image 0 is the motion vectors.
constexpr uint64_t TaaPixelShader = 0x2936e5eca99d75e7ull;
// Astro's Playroom's (PPSA01325 01.905): the same pass (colour at user data 0, history at 8, motion
// at 16 and image 0, output and history in colour targets 0 and 1), with 64 bytes of constants.
constexpr uint64_t TaaPixelShaderPlayroom = 0xeb87b8b7c526169dull;

constexpr bool IsTaaPixelShader(uint64_t hash) {
	return hash == TaaPixelShader || hash == TaaPixelShaderPlayroom;
}

struct Camera {
	std::array<float, 16> view {};       // column-major, world to camera
	std::array<float, 16> projection {}; // column-major, jittered
	bool                  valid = false;
};

// One presented frame's inputs, owned by the presenter's frame.
struct FrameInputs {
	VulkanImage   depth, motion;
	vk::ImageView depth_view = nullptr, motion_view = nullptr;
	vk::Extent2D  render_extent {};
	Camera        camera;
	bool          valid = false;
};

// Process start, before SDL makes the Vulkan window: loads Streamline (it then becomes SDL's
// Vulkan loader, and so the proxy of the instance, device and swapchain calls).
void Initialize();
// After Initialize: the Vulkan loader SDL must load (Streamline's interposer, UTF-8), or null.
// SDL3 reads its hints from an environment copy made at its start, so the window code sets it
// with SDL_SetHint (SDL_HINT_VULKAN_LIBRARY) before the window loads Vulkan.
[[nodiscard]] const char* VulkanLoaderPath();
// The vkGetInstanceProcAddr SDL loaded: logs which DLL it is (Streamline's interposer, or not).
void NoteVulkanLoader(void* get_instance_proc_addr);
// Streamline is loaded and frame generation asked for.
[[nodiscard]] bool Requested();
// Before the device: why DLSS-G cannot run with this GPU, driver and Windows (logged); empty
// when it can or when frame generation is off.
[[nodiscard]] std::string SupportProblem(vk::PhysicalDevice physical_device);
// A refused device's advice (bottom-up ASLR, chenxiao07/KytyPS5 issue #45, an RTX 5080).
[[nodiscard]] std::string RefusedDeviceAdvice();
// Before the device: the extensions Streamline's Reflex relies on but does not add itself.
void AddDeviceExtensions(const std::vector<vk::ExtensionProperties>& available,
                         std::vector<const char*>&                   extensions);
// Right after the dispatcher is loaded for the device: the command-buffer calls Streamline
// wraps only for its state tracking (disabled) go straight to the driver.
void BypassCommandHooks(vk::Device device);
// After the device: DLSS-G available on it.
void OnDevice(vk::PhysicalDevice physical_device);
[[nodiscard]] bool Enabled();
// Frame generation or DLSS-SR is on: the draws are watched for their inputs (OnDraw) and the
// guest flips end each frame's gathering (PrepareInputs).
[[nodiscard]] bool Gathering();
// An upscaler is on: DLSS-SR (KYTY_DLSS=1) or FSR 3.1 (KYTY_FSR=1), available on the device.
[[nodiscard]] bool UpscaleEnabled();

// KYTY_COMPOSITE_PROBE (default 1; 0 = off): the games' last pass (CompositePixelShader) and their
// TAA pass, logged once per new combination of their images' and targets' sizes and formats
// ("Composite probe: ..."), to plan an upscale before the last pass (Astro's Playroom).
// GPU thread, every draw after its bindings are prepared.
constexpr uint64_t CompositePixelShader = 0x67008a703cf06422ull;
void ProbeDraw(uint64_t pixel_shader, const PreparedBindings* pixel,
               std::span<const RenderColorInfo> colors);

// GPU thread, every draw after its bindings are prepared (while Enabled()).
void OnDraw(uint64_t pixel_shader, const PreparedBindings* pixel,
            std::span<const PreparedBindings* const> vertex, std::span<const RenderColorInfo> colors,
            const RenderDepthInfo& depth);
// At the guest flip, in the frame's command buffer (render lock held): copies the frame's
// gathered depth and motion into `inputs`, with its camera; inputs.valid tells whether it had
// them. Resets the gathering for the next guest frame.
void PrepareInputs(GraphicContext& graphics, TextureCache& cache, CommandBuffer& command,
                   FrameInputs& inputs);
void DestroyInputs(GraphicContext& graphics, FrameInputs& inputs);
// GPU thread, at Astro Bot's TAA draw (TaaPixelShader) with its colour targets acquired, before
// anything of the draw is recorded: DLSS-SR writes the TAA's two outputs (the frame and the next
// frame's history) from its colour, depth and motion vectors. True: done, the draw is skipped;
// false: the game's TAA draws (DLSS-SR off or unavailable, unexpected images, a failure).
// `user_data`: the pixel shader's user data (its T#s).
[[nodiscard]] bool UpscaleTaa(GraphicContext& graphics, TextureCache& cache, CommandBuffer& command,
                              const PreparedBindings& pixel, std::span<const uint32_t> user_data,
                              std::span<const RenderColorInfo> colors);

// Present thread, around vkQueuePresentKHR (null `inputs`: a frame without them, a menu).
void BeforePresent(const FrameInputs* inputs, vk::Extent2D backbuffer, vk::Format format,
                   uint32_t image_count);
void AfterPresent();
// The swapchain is made again: DLSS-G off until frames with inputs turn it on again.
void SwapchainChanging();

// AMD FSR 3.1 frame generation (KYTY_FRAMEGEN_BACKEND=fsr with KYTY_FRAMEGEN=1; Windows, AMD's
// amd_fidelityfx_vk.dll): any GPU, 2x. Its swapchain replaces the emulator's: the swapchain calls
// below go through it, and it shows a generated frame before each real one inside the present.
// Before the device: asked for (its queues are then requested, GraphicContext::fsr_*).
[[nodiscard]] bool FsrFrameGenRequested();
// After the device's queues are fetched: FSR frame generation off if it has none.
void OnFsrQueues(GraphicContext& graphics);
// FSR's swapchain is in use: Swapchain makes, uses and destroys it through the calls below.
[[nodiscard]] bool OwnsSwapchain();
[[nodiscard]] vk::Result CreateSwapchain(GraphicContext& graphics, const vk::SwapchainCreateInfoKHR& info,
                                         bool hdr, vk::SwapchainKHR* swapchain);
void                     DestroySwapchain();
[[nodiscard]] vk::Result GetSwapchainImages(vk::Device device, vk::SwapchainKHR swapchain, uint32_t* count,
                                            vk::Image* images);
[[nodiscard]] vk::Result AcquireNextImage(vk::Device device, vk::SwapchainKHR swapchain, uint64_t timeout,
                                          vk::Semaphore semaphore, uint32_t* index);
// With the graphics queue's lock held (the generated frame's work goes on that queue).
[[nodiscard]] vk::Result QueuePresent(vk::Queue queue, const vk::PresentInfoKHR& present);
// Present thread, into the present's command buffer before it is submitted: FSR's settings for
// this present and, with inputs, its preparation from the frame's depth, motion and camera.
void RecordFsrPrepare(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs,
                      vk::Extent2D backbuffer);
// Present thread, in RecordPresentCommands when an overlay layer (the game's HUD on PS5's overlay
// bus) is drawn over the frame: the backbuffer as it is before the overlay, `layout` its layout
// (left in it), copied as FSR's HUD-less image, so FSR keeps the HUD out of its interpolation.
void RecordFsrHudless(GraphicContext& graphics, vk::CommandBuffer command, vk::Image backbuffer,
                      vk::ImageLayout layout, vk::Format format, vk::Extent2D extent);

// AMD FSR 3.1 frame generation built in (KYTY_FRAMEGEN_BACKEND=fsr on Linux and macOS, from the
// FidelityFX SDK sources): AMD's frame generation swapchain is Windows only, so the presenter
// shows the generated frames itself.
// On (asked for, the device ready, not failed): the swapchain's images can then be read
// (transfer source) and the presenter calls the two below.
[[nodiscard]] bool NativeFrameGenOn();
// Present thread, after RecordPresentCommands (`backbuffer` the swapchain image, in the present
// layout): copies the frame out and, with inputs, generates the frame between the last one and it
// into `backbuffer` (left in the present layout). True: `backbuffer` now holds the generated frame,
// and RecordNativeRealFrame shows the game's frame at the next present. `show` false: FSR's
// history only (the generated frame is not put in `backbuffer`).
[[nodiscard]] bool RecordNativeFrameGen(vk::CommandBuffer command, const FrameInputs* inputs,
                                        vk::Image backbuffer, vk::Format format, vk::Extent2D extent,
                                        bool show);
// Into a newly acquired swapchain image: the game's frame RecordNativeFrameGen kept.
void RecordNativeRealFrame(vk::CommandBuffer command, vk::Image backbuffer, vk::Extent2D extent);

} // namespace FrameGen
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAME_GEN_DLSSG_H_
