#pragma once
#include "Dimension/Renderer/Vulkan.h"

namespace Dimension
{
    enum class MemoryUsage { GpuOnly, CpuToGpu, GpuToCpu };

    // RAII GPU buffer. Destruction is deferred until in-flight frames are done with it.
    class Buffer
    {
    public:
        Buffer() = default;
        Buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory, const char* debugName = nullptr);
        ~Buffer();
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        // Creates a device-local buffer and uploads data through a staging buffer.
        static Buffer CreateWithData(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName = nullptr);

        VkBuffer GetHandle() const { return m_Buffer; }
        VkDeviceSize GetSize() const { return m_Size; }
        void* GetMapped() const { return m_Mapped; }
        bool IsValid() const { return m_Buffer != VK_NULL_HANDLE; }

    private:
        void Release();

        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        VkDeviceSize m_Size = 0;
        void* m_Mapped = nullptr;
    };
}
