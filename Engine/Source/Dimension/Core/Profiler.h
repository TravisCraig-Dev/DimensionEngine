#pragma once
#include <deque>
#include <filesystem>
#include <optional>
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
        // One call of a section, where it falls in the frame (the timeline view).
        struct Span
        {
            const char* Name;
            int Depth;
            float StartMs; // from the frame's start
            float Ms;
        };
        struct FrameRecord
        {
            float FrameMs = 0.0f; // from this frame's start to the next's
            std::vector<Section> Sections;
            std::vector<std::pair<std::string, float>> Counters;
            // GPU spans (render passes) of the newest frame the GPU had finished when this one began (a frame or two
            // before it): "Frame" first, the passes inside it.
            std::vector<Section> Gpu;
            // Each call, in the order they started (at most kMaxSpans a frame; Sections still add up the rest).
            std::vector<Span> Spans;
        };
        static constexpr size_t kHistory = 300;
        static constexpr size_t kMaxSpans = 4096;
        // The call on row `depth` that covers `ms` (the one that started last), -1 if none.
        int SpanAt(const FrameRecord& frame, int depth, float ms);

        // Recordings: frames saved to a .dprof file (JSON) and read back (to compare runs). Names read from a file are
        // kept for the rest of the session (Intern), as the live ones are string literals.
        bool SaveRecording(const std::filesystem::path& file, const std::deque<FrameRecord>& frames);
        std::optional<std::deque<FrameRecord>> LoadRecording(const std::filesystem::path& file);
        const char* Intern(const std::string& name);

        // Two frames side by side: their sections matched by where they sit ("Update/Physics"), in the first frame's
        // order with those only the second has after them.
        struct Comparison
        {
            const char* Name;
            int Depth;
            float MsA = 0.0f, MsB = 0.0f; // 0 where the frame does not have it
            int CallsA = 0, CallsB = 0;
            bool InA = false, InB = false;
        };
        std::vector<Comparison> Compare(const FrameRecord& a, const FrameRecord& b);

        void BeginFrame();
        void EndFrame();
        void SetCounter(const char* name, float value);
        void SetGpuSections(std::vector<Section> sections);
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
            int m_Section = -1, m_Span = -1;
            long long m_Start = 0;
        };
    }
}

#define DIM_PROFILE_CONCAT_(a, b) a##b
#define DIM_PROFILE_CONCAT(a, b) DIM_PROFILE_CONCAT_(a, b)
#define DIM_PROFILE_SCOPE(name) ::Dimension::Profiler::Scope DIM_PROFILE_CONCAT(dimProfileScope, __LINE__)(name)
