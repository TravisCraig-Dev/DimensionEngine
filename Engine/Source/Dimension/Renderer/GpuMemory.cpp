#include "Dimension/Renderer/GpuMemory.h"
#include "Dimension/Renderer/RenderDevice.h"
#include <atomic>

namespace Dimension::GpuMemory
{
    namespace
    {
        std::atomic<int64_t> s_Bytes[(int)Kind::Count];
        std::atomic<int> s_Count[(int)Kind::Count];
    }

    void Add(Kind kind, int64_t bytes)
    {
        s_Bytes[(int)kind] += bytes;
        ++s_Count[(int)kind];
    }

    void Remove(Kind kind, int64_t bytes)
    {
        s_Bytes[(int)kind] -= bytes;
        --s_Count[(int)kind];
    }

    Usage Get(Kind kind) { return { s_Bytes[(int)kind].load(), s_Count[(int)kind].load() }; }

    const char* Name(Kind kind)
    {
        switch (kind)
        {
        case Kind::Textures: return "Textures";
        case Kind::RenderTargets: return "Render targets";
        case Kind::MeshBuffers: return "Mesh buffers";
        default: return "Other buffers";
        }
    }

    std::vector<Heap> Heaps()
    {
        std::vector<Heap> out;
        VmaAllocator allocator = RenderDevice::Get().GetAllocator();
        if (!allocator) return out;
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(allocator, &props);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(allocator, budgets);
        for (uint32_t i = 0; i < props->memoryHeapCount; ++i)
            out.push_back({ budgets[i].usage, budgets[i].budget, (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 });
        return out;
    }
}
