#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/Core/Window.h"
#include "Dimension/Core/Log.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>

namespace Dimension
{
    namespace
    {
        RenderDevice* s_Instance = nullptr;
        constexpr VkDeviceSize kTransientSize = 32ull * 1024 * 1024;

        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                Log::Error("[Vulkan] {}", data->pMessage);
            else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                Log::Warn("[Vulkan] {}", data->pMessage);
            return VK_FALSE;
        }

        bool HasInstanceExtension(const char* name)
        {
            uint32_t count = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> props(count);
            vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
            return std::any_of(props.begin(), props.end(), [&](auto& p) { return std::strcmp(p.extensionName, name) == 0; });
        }

        bool HasLayer(const char* name)
        {
            uint32_t count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> props(count);
            vkEnumerateInstanceLayerProperties(&count, props.data());
            return std::any_of(props.begin(), props.end(), [&](auto& p) { return std::strcmp(p.layerName, name) == 0; });
        }
    }

    bool RenderDevice::Exists() { return s_Instance != nullptr; }

    RenderDevice& RenderDevice::Get()
    {
        DIM_ASSERT(s_Instance, "RenderDevice not created");
        return *s_Instance;
    }

    RenderDevice::RenderDevice(Window& window, const RenderDeviceSpec& spec) : m_Window(window), m_VSync(spec.VSync)
    {
        DIM_ASSERT(!s_Instance, "Only one RenderDevice may exist");
        s_Instance = this;

        CreateInstance(spec.EnableValidation);
        VK_CHECK(glfwCreateWindowSurface(m_Instance, window.GetNative(), nullptr, &m_Surface));
        PickPhysicalDevice();
        CreateDevice();
        CreateSamplers();
        CreateBindless();
        CreateFrameResources();
        CreateSwapchain();
    }

    RenderDevice::~RenderDevice()
    {
        WaitIdle();
        FlushReleases(UINT64_MAX);
        DestroySwapchain();
        for (auto& f : m_Frames)
        {
            f.Transient = Buffer();
            vkDestroyCommandPool(m_Device, f.Pool, nullptr);
            vkDestroyFence(m_Device, f.Fence, nullptr);
            vkDestroySemaphore(m_Device, f.ImageAvailable, nullptr);
            if (f.Timers) vkDestroyQueryPool(m_Device, f.Timers, nullptr);
            if (f.DrawStamps) vkDestroyQueryPool(m_Device, f.DrawStamps, nullptr);
        }
        FlushReleases(UINT64_MAX);
        vkDestroyCommandPool(m_Device, m_ImmediatePool, nullptr);
        vkDestroyFence(m_Device, m_ImmediateFence, nullptr);
        vkDestroySampler(m_Device, m_LinearSampler, nullptr);
        vkDestroySampler(m_Device, m_NearestSampler, nullptr);
        vkDestroySampler(m_Device, m_ClampSampler, nullptr);
        vkDestroySampler(m_Device, m_NearestClampSampler, nullptr);
        vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_Device, m_BindlessLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_Device, m_UniformLayout, nullptr);
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
        vmaDestroyAllocator(m_Allocator);
        vkDestroyDevice(m_Device, nullptr);
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        if (m_Messenger) vkDestroyDebugUtilsMessengerEXT(m_Instance, m_Messenger, nullptr);
        vkDestroyInstance(m_Instance, nullptr);
        s_Instance = nullptr;
    }

    void RenderDevice::CreateInstance(bool validation)
    {
        VK_CHECK(volkInitialize());
        DIM_ASSERT(volkGetInstanceVersion() >= VK_API_VERSION_1_3, "Vulkan 1.3 loader is required");

        uint32_t glfwCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwCount);
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwCount);
        bool debugUtils = HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (debugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        std::vector<const char*> layers;
        if (validation)
        {
            if (HasLayer("VK_LAYER_KHRONOS_validation"))
                layers.push_back("VK_LAYER_KHRONOS_validation");
            else
                Log::Info("Validation layer not installed (install the Vulkan SDK or set VK_ADD_LAYER_PATH); running without it");
        }

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "Dimension";
        app.pEngineName = "Dimension Engine";
        app.engineVersion = VK_MAKE_VERSION(0, 2, 0);
        app.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = (uint32_t)extensions.size();
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledLayerCount = (uint32_t)layers.size();
        info.ppEnabledLayerNames = layers.data();
        VK_CHECK(vkCreateInstance(&info, nullptr, &m_Instance));
        volkLoadInstance(m_Instance);

        if (debugUtils && !layers.empty())
        {
            VkDebugUtilsMessengerCreateInfoEXT m{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            m.pfnUserCallback = DebugCallback;
            VK_CHECK(vkCreateDebugUtilsMessengerEXT(m_Instance, &m, nullptr, &m_Messenger));
            Log::Info("Vulkan validation layers enabled");
        }
    }

    void RenderDevice::PickPhysicalDevice()
    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(m_Instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_Instance, &count, devices.data());

        int bestScore = -1;
        for (VkPhysicalDevice pd : devices)
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(pd, &props);
            if (props.apiVersion < VK_API_VERSION_1_3) continue;

            uint32_t qCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(qCount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, families.data());
            int family = -1;
            for (uint32_t i = 0; i < qCount; ++i)
            {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, m_Surface, &present);
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { family = (int)i; break; }
            }
            if (family < 0) continue;

            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 100;
            if (score > bestScore)
            {
                bestScore = score;
                m_PhysicalDevice = pd;
                m_GraphicsFamily = (uint32_t)family;
                m_Properties = props;
            }
        }
        DIM_ASSERT(m_PhysicalDevice, "No GPU with Vulkan 1.3 and presentation support found");
        Log::Info("GPU: {} (Vulkan {}.{}.{})", m_Properties.deviceName, VK_API_VERSION_MAJOR(m_Properties.apiVersion),
                  VK_API_VERSION_MINOR(m_Properties.apiVersion), VK_API_VERSION_PATCH(m_Properties.apiVersion));
    }

    void RenderDevice::CreateDevice()
    {
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supported);

        VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        f13.shaderDemoteToHelperInvocation = VK_TRUE; // glslang emits OpDemoteToHelperInvocation for discard

        VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        f12.pNext = &f13;
        f12.descriptorIndexing = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f12.bufferDeviceAddress = VK_TRUE;
        f12.scalarBlockLayout = VK_TRUE;

        VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        features.pNext = &f12;
        features.features.samplerAnisotropy = supported.samplerAnisotropy;
        features.features.fillModeNonSolid = supported.fillModeNonSolid;
        features.features.independentBlend = VK_TRUE;
        features.features.wideLines = supported.wideLines;
        features.features.textureCompressionBC = supported.textureCompressionBC; // (compressed textures when there)
        m_TextureCompressionBC = supported.textureCompressionBC == VK_TRUE;

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queue.queueFamilyIndex = m_GraphicsFamily;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;

        const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        info.pNext = &features;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
        VK_CHECK(vkCreateDevice(m_PhysicalDevice, &info, nullptr, &m_Device));
        volkLoadDevice(m_Device);
        vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);

        VmaVulkanFunctions funcs{};
        funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo alloc{};
        alloc.vulkanApiVersion = VK_API_VERSION_1_3;
        alloc.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        alloc.physicalDevice = m_PhysicalDevice;
        alloc.device = m_Device;
        alloc.instance = m_Instance;
        alloc.pVulkanFunctions = &funcs;
        VK_CHECK(vmaCreateAllocator(&alloc, &m_Allocator));

        VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = m_GraphicsFamily;
        VK_CHECK(vkCreateCommandPool(m_Device, &pool, nullptr, &m_ImmediatePool));
        VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_CHECK(vkCreateFence(m_Device, &fence, nullptr, &m_ImmediateFence));
    }

    void RenderDevice::CreateSamplers()
    {
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supported);

        VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.maxLod = VK_LOD_CLAMP_NONE;
        info.anisotropyEnable = supported.samplerAnisotropy;
        info.maxAnisotropy = supported.samplerAnisotropy ? std::min(8.0f, m_Properties.limits.maxSamplerAnisotropy) : 1.0f;
        VK_CHECK(vkCreateSampler(m_Device, &info, nullptr, &m_LinearSampler));

        info.magFilter = info.minFilter = VK_FILTER_NEAREST;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.anisotropyEnable = VK_FALSE;
        info.maxAnisotropy = 1.0f;
        VK_CHECK(vkCreateSampler(m_Device, &info, nullptr, &m_NearestSampler));

        info.magFilter = info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.0f;
        VK_CHECK(vkCreateSampler(m_Device, &info, nullptr, &m_ClampSampler));

        info.magFilter = info.minFilter = VK_FILTER_NEAREST;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        VK_CHECK(vkCreateSampler(m_Device, &info, nullptr, &m_NearestClampSampler));
    }

    void RenderDevice::CreateBindless()
    {
        VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBindlessTextures },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFramesInFlight },
        };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool.maxSets = 1 + kFramesInFlight;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(m_Device, &pool, nullptr, &m_DescriptorPool));

        VkDescriptorSetLayoutBinding bindless{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBindlessTextures, VK_SHADER_STAGE_ALL, nullptr };
        VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                         VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        flagsInfo.bindingCount = 1;
        flagsInfo.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout.pNext = &flagsInfo;
        layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout.bindingCount = 1;
        layout.pBindings = &bindless;
        VK_CHECK(vkCreateDescriptorSetLayout(m_Device, &layout, nullptr, &m_BindlessLayout));

        VkDescriptorSetLayoutBinding ubo{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr };
        VkDescriptorSetLayoutCreateInfo uboLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        uboLayout.bindingCount = 1;
        uboLayout.pBindings = &ubo;
        VK_CHECK(vkCreateDescriptorSetLayout(m_Device, &uboLayout, nullptr, &m_UniformLayout));

        VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = m_DescriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &m_BindlessLayout;
        VK_CHECK(vkAllocateDescriptorSets(m_Device, &alloc, &m_BindlessSet));

        VkDescriptorSetLayout setLayouts[] = { m_BindlessLayout, m_UniformLayout };
        VkPushConstantRange push{ VK_SHADER_STAGE_ALL, 0, kPushConstantSize };
        VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pl.setLayoutCount = 2;
        pl.pSetLayouts = setLayouts;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        VK_CHECK(vkCreatePipelineLayout(m_Device, &pl, nullptr, &m_PipelineLayout));
    }

    void RenderDevice::CreateFrameResources()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            PerFrame& f = m_Frames[i];
            VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool.queueFamilyIndex = m_GraphicsFamily;
            VK_CHECK(vkCreateCommandPool(m_Device, &pool, nullptr, &f.Pool));

            VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            alloc.commandPool = f.Pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(m_Device, &alloc, &f.Cmd));

            VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_CHECK(vkCreateFence(m_Device, &fence, nullptr, &f.Fence));
            VkSemaphoreCreateInfo sem{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VK_CHECK(vkCreateSemaphore(m_Device, &sem, nullptr, &f.ImageAvailable));
            if (m_Properties.limits.timestampComputeAndGraphics)
            {
                VkQueryPoolCreateInfo timers{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                timers.queryType = VK_QUERY_TYPE_TIMESTAMP;
                timers.queryCount = kMaxTimerQueries;
                VK_CHECK(vkCreateQueryPool(m_Device, &timers, nullptr, &f.Timers));
                timers.queryCount = kMaxDrawStamps;
                VK_CHECK(vkCreateQueryPool(m_Device, &timers, nullptr, &f.DrawStamps));
            }

            f.Transient = Buffer(kTransientSize,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // (uploads to images)
                                 MemoryUsage::CpuToGpu, "TransientRing");
            VkBufferDeviceAddressInfo addressInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            addressInfo.buffer = f.Transient.GetHandle();
            f.TransientAddress = vkGetBufferDeviceAddress(m_Device, &addressInfo);

            VkDescriptorSetAllocateInfo setAlloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            setAlloc.descriptorPool = m_DescriptorPool;
            setAlloc.descriptorSetCount = 1;
            setAlloc.pSetLayouts = &m_UniformLayout;
            VK_CHECK(vkAllocateDescriptorSets(m_Device, &setAlloc, &f.UniformSet));

            VkDescriptorBufferInfo bufferInfo{ f.Transient.GetHandle(), 0, kUniformBlockSize };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = f.UniformSet;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
        }
    }

    void RenderDevice::CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &caps));

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (auto& f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
        m_SwapchainFormat = chosen.format;

        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &modeCount, modes.data());
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (!m_VSync)
        {
            for (auto pref : { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR })
                if (std::find(modes.begin(), modes.end(), pref) != modes.end()) { presentMode = pref; break; }
        }

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX)
        {
            auto size = m_Window.GetFramebufferSize();
            extent.width = std::clamp(size.x, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(size.y, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        m_SwapchainExtent = extent;

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

        VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        info.surface = m_Surface;
        info.minImageCount = imageCount;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = presentMode;
        info.clipped = VK_TRUE;
        VK_CHECK(vkCreateSwapchainKHR(m_Device, &info, nullptr, &m_Swapchain));

        uint32_t count = 0;
        vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &count, nullptr);
        m_SwapchainImages.resize(count);
        vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &count, m_SwapchainImages.data());
        m_SwapchainViews.resize(count);
        m_RenderFinished.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            m_SwapchainViews[i] = Vk::CreateImageView(m_SwapchainImages[i], m_SwapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT);
            VkSemaphoreCreateInfo sem{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VK_CHECK(vkCreateSemaphore(m_Device, &sem, nullptr, &m_RenderFinished[i]));
        }
        m_SwapchainDirty = false;
    }

    void RenderDevice::DestroySwapchain()
    {
        for (auto v : m_SwapchainViews) vkDestroyImageView(m_Device, v, nullptr);
        for (auto s : m_RenderFinished) vkDestroySemaphore(m_Device, s, nullptr);
        m_SwapchainViews.clear();
        m_RenderFinished.clear();
        m_SwapchainImages.clear();
        if (m_Swapchain) vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }

    void RenderDevice::SetVSync(bool vsync)
    {
        if (m_VSync == vsync) return;
        m_VSync = vsync;
        m_SwapchainDirty = true;
    }

    bool RenderDevice::BeginFrame()
    {
        auto size = m_Window.GetFramebufferSize();
        if (size.x == 0 || size.y == 0) return false;
        if (m_SwapchainDirty || size.x != m_SwapchainExtent.width || size.y != m_SwapchainExtent.height)
        {
            WaitIdle();
            DestroySwapchain();
            CreateSwapchain();
        }

        uint64_t next = m_FrameNumber + 1;
        PerFrame& f = m_Frames[next % kFramesInFlight];
        VK_CHECK(vkWaitForFences(m_Device, 1, &f.Fence, VK_TRUE, UINT64_MAX));
        m_CompletedFrame = std::max(m_CompletedFrame, f.SubmittedFrame);
        FlushReleases(m_CompletedFrame);

        VkResult acquire = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX, f.ImageAvailable, VK_NULL_HANDLE, &m_ImageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            m_SwapchainDirty = true;
            return false;
        }
        if (acquire == VK_SUBOPTIMAL_KHR) m_SwapchainDirty = true;
        else if (acquire != VK_SUCCESS) VK_CHECK(acquire);

        m_FrameNumber = next;
        // That frame's GPU timers are done: their spans become the newest timings.
        if (f.Timers && f.TimerQueries > 0)
        {
            std::vector<uint64_t> stamps(f.TimerQueries);
            if (vkGetQueryPoolResults(m_Device, f.Timers, 0, f.TimerQueries, stamps.size() * sizeof(uint64_t), stamps.data(), sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                m_GpuTimings.clear();
                float period = m_Properties.limits.timestampPeriod; // ns per tick
                for (const auto& t : f.TimerRecords)
                    if (t.End != UINT32_MAX && stamps[t.End] >= stamps[t.Begin])
                        m_GpuTimings.push_back({ t.Name, t.Depth, (float)((double)(stamps[t.End] - stamps[t.Begin]) * period / 1e6) });
            }
        }
        // And its draw marks (one more stamp closes the last draw's span: the frame's end).
        if (f.DrawStamps && f.DrawStampCount > 1)
        {
            std::vector<uint64_t> stamps(f.DrawStampCount);
            if (vkGetQueryPoolResults(m_Device, f.DrawStamps, 0, f.DrawStampCount, stamps.size() * sizeof(uint64_t), stamps.data(), sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                double period = m_Properties.limits.timestampPeriod;
                m_DrawTimes.resize(stamps.size() - 1);
                for (size_t i = 0; i + 1 < stamps.size(); ++i)
                    m_DrawTimes[i] = stamps[i + 1] >= stamps[i] ? (float)((double)(stamps[i + 1] - stamps[i]) * period / 1e6) : 0.0f;
                m_DrawTimesFrame = f.SubmittedFrame;
            }
        }
        f.DrawStampCount = 0;
        f.TimerRecords.clear();
        f.TimerQueries = 0;
        f.OpenTimers = 0;
        VK_CHECK(vkResetFences(m_Device, 1, &f.Fence));
        VK_CHECK(vkResetCommandPool(m_Device, f.Pool, 0));
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(f.Cmd, &begin));
        f.TransientHead = 0;
        if (f.Timers)
        {
            vkCmdResetQueryPool(f.Cmd, f.Timers, 0, kMaxTimerQueries);
            vkCmdResetQueryPool(f.Cmd, f.DrawStamps, 0, kMaxDrawStamps);
            f.FrameTimer = BeginGpuTimer(f.Cmd, "Frame");
        }

        m_Frame.Cmd = f.Cmd;
        m_Frame.FrameIndex = uint32_t(m_FrameNumber % kFramesInFlight);
        m_Frame.FrameNumber = m_FrameNumber;
        m_Frame.SwapchainImage = m_SwapchainImages[m_ImageIndex];
        m_Frame.SwapchainView = m_SwapchainViews[m_ImageIndex];
        m_Frame.SwapchainExtent = m_SwapchainExtent;
        m_Recording = true;
        return true;
    }

    int RenderDevice::BeginGpuTimer(VkCommandBuffer cmd, const char* name)
    {
        PerFrame& f = m_Frames[m_FrameNumber % kFramesInFlight];
        if (!f.Timers || cmd != f.Cmd || f.TimerQueries + 2 > kMaxTimerQueries) return -1;
        uint32_t q = f.TimerQueries;
        f.TimerQueries += 2;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.Timers, q);
        f.TimerRecords.push_back({ name, f.OpenTimers, q, UINT32_MAX });
        ++f.OpenTimers;
        return (int)f.TimerRecords.size() - 1;
    }

    int RenderDevice::MarkDraw(VkCommandBuffer cmd)
    {
        PerFrame& f = m_Frames[m_FrameNumber % kFramesInFlight];
        if (!f.DrawStamps || cmd != f.Cmd || f.DrawStampCount >= kMaxDrawStamps) return -1;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.DrawStamps, f.DrawStampCount);
        return (int)f.DrawStampCount++;
    }

    void RenderDevice::EndGpuTimer(VkCommandBuffer cmd, int timer)
    {
        PerFrame& f = m_Frames[m_FrameNumber % kFramesInFlight];
        if (timer < 0 || timer >= (int)f.TimerRecords.size() || cmd != f.Cmd) return;
        auto& t = f.TimerRecords[timer];
        if (t.End != UINT32_MAX) return;
        t.End = t.Begin + 1;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.Timers, t.End);
        f.OpenTimers = std::max(f.OpenTimers - 1, 0);
    }

    void RenderDevice::EndFrame()
    {
        m_Recording = false;
        PerFrame& f = m_Frames[m_FrameNumber % kFramesInFlight];
        if (f.FrameTimer >= 0) EndGpuTimer(f.Cmd, f.FrameTimer);
        f.FrameTimer = -1;
        VK_CHECK(vkEndCommandBuffer(f.Cmd));

        VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        wait.semaphore = f.ImageAvailable;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signal.semaphore = m_RenderFinished[m_ImageIndex];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cmd{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmd.commandBuffer = f.Cmd;

        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, f.Fence));
        f.SubmittedFrame = m_FrameNumber;

        VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &m_RenderFinished[m_ImageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &m_Swapchain;
        present.pImageIndices = &m_ImageIndex;
        VkResult result = vkQueuePresentKHR(m_GraphicsQueue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) m_SwapchainDirty = true;
        else VK_CHECK(result);
    }

    void RenderDevice::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record)
    {
        VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = m_ImmediatePool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cmd;
        VK_CHECK(vkAllocateCommandBuffers(m_Device, &alloc, &cmd));

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
        record(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, m_ImmediateFence));
        VK_CHECK(vkWaitForFences(m_Device, 1, &m_ImmediateFence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(m_Device, 1, &m_ImmediateFence));
        VK_CHECK(vkResetCommandPool(m_Device, m_ImmediatePool, 0));
    }

    void RenderDevice::DeferRelease(std::function<void()> release)
    {
        m_Releases.emplace_back(m_FrameNumber, std::move(release));
    }

    void RenderDevice::FlushReleases(uint64_t completedFrame)
    {
        // Release callbacks may queue further releases; process in FIFO order.
        while (!m_Releases.empty() && m_Releases.front().first <= completedFrame)
        {
            auto fn = std::move(m_Releases.front().second);
            m_Releases.pop_front();
            fn();
        }
    }

    void RenderDevice::WaitIdle()
    {
        if (m_Device) vkDeviceWaitIdle(m_Device);
        m_CompletedFrame = m_FrameNumber;
    }

    TransientAllocation RenderDevice::AllocateTransient(VkDeviceSize size, VkDeviceSize alignment)
    {
        PerFrame& f = m_Frames[m_FrameNumber % kFramesInFlight];
        VkDeviceSize offset = (f.TransientHead + alignment - 1) & ~(alignment - 1);
        if (offset + size > f.Transient.GetSize())
        {
            static bool warned = false;
            if (!warned) Log::Error("Transient ring buffer exhausted ({} bytes requested)", size);
            warned = true;
            return {};
        }
        f.TransientHead = offset + size;
        return { f.Transient.GetHandle(), offset, (uint8_t*)f.Transient.GetMapped() + offset, f.TransientAddress + offset };
    }

    void RenderDevice::BindBindless(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint)
    {
        vkCmdBindDescriptorSets(cmd, bindPoint, m_PipelineLayout, 0, 1, &m_BindlessSet, 0, nullptr);
    }

    void RenderDevice::BindUniforms(VkCommandBuffer cmd, const void* data, size_t size, VkPipelineBindPoint bindPoint)
    {
        DIM_ASSERT(size <= kUniformBlockSize, "Uniform block too large");
        VkDeviceSize align = std::max<VkDeviceSize>(m_Properties.limits.minUniformBufferOffsetAlignment, 16);
        TransientAllocation a = AllocateTransient(kUniformBlockSize, align);
        if (!a) return;
        std::memcpy(a.Data, data, size);
        uint32_t dynamicOffset = (uint32_t)a.Offset;
        PerFrame& f = m_Frames[m_FrameNumber % kFramesInFlight];
        vkCmdBindDescriptorSets(cmd, bindPoint, m_PipelineLayout, 1, 1, &f.UniformSet, 1, &dynamicOffset);
    }

    uint32_t RenderDevice::RegisterTexture(VkImageView view, VkSampler sampler)
    {
        uint32_t slot;
        if (!m_FreeTextureSlots.empty())
        {
            slot = m_FreeTextureSlots.back();
            m_FreeTextureSlots.pop_back();
        }
        else
        {
            DIM_ASSERT(m_NextTextureSlot < kMaxBindlessTextures, "Bindless texture table is full");
            slot = m_NextTextureSlot++;
        }
        VkDescriptorImageInfo image{ sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = m_BindlessSet;
        write.dstBinding = 0;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
        return slot;
    }

    void RenderDevice::UnregisterTexture(uint32_t index)
    {
        DeferRelease([this, index] { m_FreeTextureSlots.push_back(index); });
    }
}
