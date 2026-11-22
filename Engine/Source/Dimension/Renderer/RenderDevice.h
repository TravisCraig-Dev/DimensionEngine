#pragma once
#include "Dimension/Renderer/Vulkan.h"
#include "Dimension/Renderer/Buffer.h"
#include <glm/glm.hpp>
#include <deque>

namespace Dimension
{
    class Window;

    constexpr uint32_t kFramesInFlight = 2;
    constexpr uint32_t kMaxBindlessTextures = 4096;
    constexpr uint32_t kPushConstantSize = 128;

    // Slice of the per-frame host-visible ring buffer. Valid until the frame completes.
    struct TransientAllocation
    {
        VkBuffer Buffer = VK_NULL_HANDLE;
        VkDeviceSize Offset = 0;
        void* Data = nullptr;
        VkDeviceAddress Address = 0; // GPU address (buffer_reference in shaders)
        explicit operator bool() const { return Data != nullptr; }
    };

    struct FrameContext
    {
        VkCommandBuffer Cmd = VK_NULL_HANDLE;
        uint32_t FrameIndex = 0;      // 0..kFramesInFlight-1
        uint64_t FrameNumber = 0;     // monotonically increasing
        VkImage SwapchainImage = VK_NULL_HANDLE;
        VkImageView SwapchainView = VK_NULL_HANDLE;
        VkExtent2D SwapchainExtent{};
    };

    struct RenderDeviceSpec
    {
        bool EnableValidation = false;
        bool VSync = true;
    };

    // Owns the Vulkan instance, device, swapchain and frame pacing for the main window.
    // All pipelines share one layout:
    //   set 0: bindless sampler2D array (every texture in the engine)
    //   set 1: per-draw-pass uniform buffer (dynamic offset into the transient ring)
    //   push constants: 128 bytes, all stages
    class RenderDevice
    {
    public:
        RenderDevice(Window& window, const RenderDeviceSpec& spec);
        ~RenderDevice();
        RenderDevice(const RenderDevice&) = delete;
        RenderDevice& operator=(const RenderDevice&) = delete;

        static RenderDevice& Get();
        static bool Exists(); // false in headless tools and tests

        // Frame loop. BeginFrame returns false if the frame must be skipped (e.g. swapchain resized).
        bool BeginFrame();
        void EndFrame();
        const FrameContext& Frame() const { return m_Frame; }
        // Between a successful BeginFrame and EndFrame: Frame().Cmd records, so uploads can go into it (outside passes).
        bool IsRecordingFrame() const { return m_Recording; }
        // GPU timing (the Profiler's GPU sections): spans of this frame's commands timed by timestamp queries, read
        // back once the frame is done. Begin returns a timer to end (-1: no timers left or none supported).
        int BeginGpuTimer(VkCommandBuffer cmd, const char* name);
        void EndGpuTimer(VkCommandBuffer cmd, int timer);
        struct GpuTiming
        {
            const char* Name;
            int Depth;
            float Ms;
        };
        // The newest finished frame's spans, in the order they began ("Frame" first: all of it).
        const std::vector<GpuTiming>& GetGpuTimings() const { return m_GpuTimings; }
        // Draw timestamps (the Frame Debugger's GPU time per draw): MarkDraw writes one before a draw (allowed inside a
        // render pass; up to kMaxDrawStamps a frame) and returns its index, or -1. Once that frame is done on the GPU,
        // GetDrawTimes has the milliseconds from each mark to the next (the last to the end of the frame's work), and
        // GetDrawTimesFrame the frame they were measured in.
        int MarkDraw(VkCommandBuffer cmd);
        const std::vector<float>& GetDrawTimes() const { return m_DrawTimes; }
        uint64_t GetDrawTimesFrame() const { return m_DrawTimesFrame; }
        uint64_t GetCompletedFrameNumber() const { return m_CompletedFrame; }

        void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record);
        void DeferRelease(std::function<void()> release);
        void WaitIdle();

        TransientAllocation AllocateTransient(VkDeviceSize size, VkDeviceSize alignment = 16);
        // Binds the bindless texture table at set 0.
        void BindBindless(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
        // Copies a uniform block (<= kUniformBlockSize) into the transient ring and binds it at set 1.
        void BindUniforms(VkCommandBuffer cmd, const void* data, size_t size, VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
        static constexpr VkDeviceSize kUniformBlockSize = 8192;

        uint32_t RegisterTexture(VkImageView view, VkSampler sampler);
        void UnregisterTexture(uint32_t index);

        void SetVSync(bool vsync);
        bool GetVSync() const { return m_VSync; }

        VkInstance GetInstance() const { return m_Instance; }
        VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
        VkDevice GetDevice() const { return m_Device; }
        VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
        uint32_t GetGraphicsQueueFamily() const { return m_GraphicsFamily; }
        VmaAllocator GetAllocator() const { return m_Allocator; }
        const VkPhysicalDeviceProperties& GetProperties() const { return m_Properties; }
        VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
        VkFormat GetSwapchainFormat() const { return m_SwapchainFormat; }
        uint32_t GetSwapchainImageCount() const { return (uint32_t)m_SwapchainImages.size(); }
        bool SupportsTextureCompressionBC() const { return m_TextureCompressionBC; }
        VkSampler GetLinearSampler() const { return m_LinearSampler; }
        VkSampler GetNearestSampler() const { return m_NearestSampler; }
        VkSampler GetClampSampler() const { return m_ClampSampler; }
        VkSampler GetNearestClampSampler() const { return m_NearestClampSampler; }

    private:
        void CreateInstance(bool validation);
        void PickPhysicalDevice();
        void CreateDevice();
        void CreateSwapchain();
        void DestroySwapchain();
        void CreateFrameResources();
        void CreateBindless();
        void CreateSamplers();
        void FlushReleases(uint64_t completedFrame);

        Window& m_Window;
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_Messenger = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties m_Properties{};
        VkDevice m_Device = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        uint32_t m_GraphicsFamily = 0;
        VmaAllocator m_Allocator = nullptr;

        // Swapchain
        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        VkFormat m_SwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
        VkExtent2D m_SwapchainExtent{};
        std::vector<VkImage> m_SwapchainImages;
        std::vector<VkImageView> m_SwapchainViews;
        std::vector<VkSemaphore> m_RenderFinished; // one per swapchain image
        bool m_VSync = true;
        bool m_TextureCompressionBC = false;
        bool m_SwapchainDirty = false;

        struct PerFrame
        {
            VkCommandPool Pool = VK_NULL_HANDLE;
            VkCommandBuffer Cmd = VK_NULL_HANDLE;
            VkFence Fence = VK_NULL_HANDLE;
            VkSemaphore ImageAvailable = VK_NULL_HANDLE;
            Buffer Transient;
            VkDeviceAddress TransientAddress = 0;
            VkDeviceSize TransientHead = 0;
            VkDescriptorSet UniformSet = VK_NULL_HANDLE;
            uint64_t SubmittedFrame = 0;
            VkQueryPool Timers = VK_NULL_HANDLE;
            struct Timer
            {
                const char* Name;
                int Depth;
                uint32_t Begin, End;
            };
            std::vector<Timer> TimerRecords;
            uint32_t TimerQueries = 0;
            int OpenTimers = 0;
            int FrameTimer = -1;
            VkQueryPool DrawStamps = VK_NULL_HANDLE;
            uint32_t DrawStampCount = 0;
        };
        static constexpr uint32_t kMaxDrawStamps = 8192;
        std::vector<float> m_DrawTimes;
        uint64_t m_DrawTimesFrame = 0;
        static constexpr uint32_t kMaxTimerQueries = 256;
        std::vector<GpuTiming> m_GpuTimings;
        PerFrame m_Frames[kFramesInFlight];
        FrameContext m_Frame;
        bool m_Recording = false;
        uint32_t m_ImageIndex = 0;
        uint64_t m_FrameNumber = 0;
        uint64_t m_CompletedFrame = 0;

        // Immediate submit
        VkCommandPool m_ImmediatePool = VK_NULL_HANDLE;
        VkFence m_ImmediateFence = VK_NULL_HANDLE;

        // Deferred destruction: (frame number when queued, release fn)
        std::deque<std::pair<uint64_t, std::function<void()>>> m_Releases;

        // Descriptors
        VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_BindlessLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_UniformLayout = VK_NULL_HANDLE;
        VkDescriptorSet m_BindlessSet = VK_NULL_HANDLE;
        VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
        std::vector<uint32_t> m_FreeTextureSlots;
        uint32_t m_NextTextureSlot = 0;

        VkSampler m_LinearSampler = VK_NULL_HANDLE, m_NearestSampler = VK_NULL_HANDLE, m_ClampSampler = VK_NULL_HANDLE, m_NearestClampSampler = VK_NULL_HANDLE;
    };
}
