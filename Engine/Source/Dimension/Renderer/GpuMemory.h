#pragma once
#include <cstdint>
#include <vector>

namespace Dimension
{
    // GPU memory in use (the Profiler's Memory view): every Image and Buffer counts its allocation under a kind while
    // it lives, and the device's memory heaps report their use against their budgets.
    namespace GpuMemory
    {
        enum class Kind : int { Textures = 0, RenderTargets, MeshBuffers, OtherBuffers, Count };
        struct Usage
        {
            int64_t Bytes = 0;
            int Count = 0;
        };
        void Add(Kind kind, int64_t bytes);    // an allocation made
        void Remove(Kind kind, int64_t bytes); // and released
        Usage Get(Kind kind);
        const char* Name(Kind kind);

        struct Heap
        {
            uint64_t Used = 0, Budget = 0;
            bool DeviceLocal = false;
        };
        std::vector<Heap> Heaps();
    }
}
