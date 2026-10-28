#pragma once
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace Dimension
{
    // A frame profiler (Window > Profiler): named, nested CPU sections timed on the main thread
    // (DIM_PROFILE_SCOPE("Physics")), and counters (draw calls, triangles), kept for the last few hundred frames.
    namespace Profiler
    {
        struct Section
        {
            const char* Name; // a string literal
            int Depth;        // nesting: 0 = straight inside the frame
            float Ms;         // all its calls this frame
            int Calls;
        };
        struct FrameRecord
        {
            float FrameMs = 0.0f; // from this frame's start to the next's
            std::vector<Section> Sections;
            std::vector<std::pair<std::string, float>> Counters;
        };
        static constexpr size_t kHistory = 300;

        void BeginFrame();
        void EndFrame();
        void SetCounter(const char* name, float value);
        // Recording stops while paused (the history stays to look at).
        void SetPaused(bool paused);
        bool IsPaused();
        void Clear();
        const std::deque<FrameRecord>& History();

        class Scope
        {
        public:
            explicit Scope(const char* name);
            ~Scope();
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;

        private:
            int m_Section = -1;
            long long m_Start = 0;
        };
    }
}

#define DIM_PROFILE_CONCAT_(a, b) a##b
#define DIM_PROFILE_CONCAT(a, b) DIM_PROFILE_CONCAT_(a, b)
#define DIM_PROFILE_SCOPE(name) ::Dimension::Profiler::Scope DIM_PROFILE_CONCAT(dimProfileScope, __LINE__)(name)
