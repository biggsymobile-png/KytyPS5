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
// KYTY_DLSS_SR=on|off (default off): Astro Bot's TAA colour replacement.
// The game patch sets input resolution; the SDK mode follows the actual input/output
// grids (DLAA for equal sizes). 1/0 are also accepted; explicit legacy dlaa
// retains the native game grid. SDK quality-mode names are not configuration values.
// See docs/ASTRO-BOT-DLSS-SR.md.
// KYTY_DLSS_SR_OUTPUT=4k (default for SR): patch recognized Astro Bot executables
// to use 3840x2160 post-processing independently of the scene resolution patch.
// =game retains the original output grid. Explicit legacy dlaa retains the game grid.
// Windows only, and only in builds with Streamline's headers (KYTY_FRAMEGEN_STREAMLINE).

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
// Streamline is loaded and frame generation or Super Resolution asked for.
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
// After the device: checks DLSS-G and DLSS SR independently.
void OnDevice(vk::PhysicalDevice physical_device);
[[nodiscard]] bool Enabled();

// Opt-in Astro Bot DLSS SR/DLAA at the TAA pass, before post-processing and HUD.
// The original draw has already maintained the game's fallback TAA history.
// Returns true if an external pass was recorded (including a failed evaluation).
bool ResolveTaa(CommandBuffer& command, uint64_t pixel_shader, const PreparedBindings* pixel,
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

// Present thread, around vkQueuePresentKHR (null `inputs`: a frame without them, a menu).
void BeforePresent(const FrameInputs* inputs, vk::Extent2D backbuffer, vk::Format format,
                   uint32_t image_count);
void AfterPresent();
// The swapchain is made again: DLSS-G off until frames with inputs turn it on again.
void SwapchainChanging();

} // namespace FrameGen
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAME_GEN_DLSSG_H_
