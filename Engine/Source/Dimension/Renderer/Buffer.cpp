#include "Dimension/Renderer/Buffer.h"
#include "Dimension/Renderer/RenderDevice.h"
#include "Dimension/Renderer/GpuMemory.h"
#include <cstring>

namespace Dimension
{
    Buffer::Buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory, const char* debugName)
        : m_Size(size)
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        if (memory == MemoryUsage::CpuToGpu)
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        else if (memory == MemoryUsage::GpuToCpu)
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfo{};
        VK_CHECK(vmaCreateBuffer(RenderDevice::Get().GetAllocator(), &info, &alloc, &m_Buffer, &m_Allocation, &allocInfo));
        m_Mapped = allocInfo.pMappedData;
        m_MemoryKind = (int)(usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT) && memory == MemoryUsage::GpuOnly
                                 ? GpuMemory::Kind::MeshBuffers : GpuMemory::Kind::OtherBuffers);
        m_MemoryBytes = (int64_t)allocInfo.size;
        GpuMemory::Add((GpuMemory::Kind)m_MemoryKind, m_MemoryBytes);
        if (debugName) Vk::SetDebugName(VK_OBJECT_TYPE_BUFFER, (uint64_t)m_Buffer, debugName);
    }

    Buffer::~Buffer() { Release(); }

    Buffer::Buffer(Buffer&& o) noexcept
        : m_Buffer(o.m_Buffer), m_Allocation(o.m_Allocation), m_Size(o.m_Size), m_Mapped(o.m_Mapped), m_MemoryKind(o.m_MemoryKind),
          m_MemoryBytes(o.m_MemoryBytes)
    {
        o.m_Buffer = VK_NULL_HANDLE;
        o.m_Allocation = nullptr;
        o.m_Mapped = nullptr;
        o.m_Size = 0;
    }

    Buffer& Buffer::operator=(Buffer&& o) noexcept
    {
        if (this != &o)
        {
            Release();
            std::swap(m_Buffer, o.m_Buffer);
            std::swap(m_Allocation, o.m_Allocation);
            std::swap(m_Size, o.m_Size);
            std::swap(m_Mapped, o.m_Mapped);
            std::swap(m_MemoryKind, o.m_MemoryKind);
            std::swap(m_MemoryBytes, o.m_MemoryBytes);
        }
        return *this;
    }

    void Buffer::Release()
    {
        if (!m_Buffer) return;
        GpuMemory::Remove((GpuMemory::Kind)m_MemoryKind, m_MemoryBytes);
        VkBuffer buffer = m_Buffer;
        VmaAllocation allocation = m_Allocation;
        RenderDevice::Get().DeferRelease([buffer, allocation] {
            vmaDestroyBuffer(RenderDevice::Get().GetAllocator(), buffer, allocation);
        });
        m_Buffer = VK_NULL_HANDLE;
        m_Allocation = nullptr;
        m_Mapped = nullptr;
    }

    Buffer Buffer::CreateWithData(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName)
    {
        Buffer staging(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
        std::memcpy(staging.GetMapped(), data, size);
        Buffer result(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, debugName);
        RenderDevice::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy region{ 0, 0, size };
            vkCmdCopyBuffer(cmd, staging.GetHandle(), result.GetHandle(), 1, &region);
        });
        return result;
    }
}
