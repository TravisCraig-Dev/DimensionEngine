#include "Dimension/Core/Profiler.h"
#include <chrono>
#include <cstring>
#include <thread>

namespace Dimension::Profiler
{
    namespace
    {
        struct State
        {
            std::deque<FrameRecord> History;
            FrameRecord Current;
            bool Recording = false, Paused = false;
            int Depth = 0;
            std::vector<int> Open; // the sections open now, innermost last
            long long FrameStart = 0;
            std::thread::id Main;
        };
        State& S()
        {
            static State s;
            return s;
        }
        long long Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        bool OnMainThread(State& s) { return s.Recording && std::this_thread::get_id() == s.Main; }
    }

    void BeginFrame()
    {
        State& s = S();
        long long now = Now();
        if (s.Recording && !s.Paused)
        {
            s.Current.FrameMs = (float)((now - s.FrameStart) / 1e6);
            s.History.push_back(std::move(s.Current));
            while (s.History.size() > kHistory) s.History.pop_front();
        }
        s.Current = FrameRecord();
        s.Open.clear();
        s.FrameStart = now;
        s.Main = std::this_thread::get_id();
        s.Recording = !s.Paused;
    }

    void EndFrame() {} // the frame lasts until the next begins (waiting included)

    void SetCounter(const char* name, float value)
    {
        State& s = S();
        if (!OnMainThread(s)) return;
        for (auto& [n, v] : s.Current.Counters)
            if (n == name)
            {
                v = value;
                return;
            }
        s.Current.Counters.push_back({ name, value });
    }

    void SetGpuSections(std::vector<Section> sections)
    {
        State& s = S();
        if (s.Recording) s.Current.Gpu = std::move(sections);
    }

    void SetPaused(bool paused) { S().Paused = paused; }
    bool IsPaused() { return S().Paused; }
    void Clear() { S().History.clear(); }
    const std::deque<FrameRecord>& History() { return S().History; }

    Scope::Scope(const char* name)
    {
        State& s = S();
        if (!OnMainThread(s)) return;
        // The same name at the same depth under the same parent this frame adds up (fixed steps run several times).
        int depth = (int)s.Open.size(), parent = s.Open.empty() ? -1 : s.Open.back();
        auto& sections = s.Current.Sections;
        for (int i = (int)sections.size() - 1; i > parent; --i)
            if (sections[i].Depth == depth && std::strcmp(sections[i].Name, name) == 0)
            {
                m_Section = i;
                break;
            }
            else if (sections[i].Depth < depth)
                break;
        if (m_Section < 0)
        {
            m_Section = (int)sections.size();
            sections.push_back({ name, depth, 0.0f, 0 });
        }
        sections[m_Section].Calls++;
        s.Open.push_back(m_Section);
        m_Start = Now();
    }

    Scope::~Scope()
    {
        if (m_Section < 0) return;
        State& s = S();
        if (!s.Open.empty() && s.Open.back() == m_Section) s.Open.pop_back();
        if (m_Section < (int)s.Current.Sections.size()) s.Current.Sections[m_Section].Ms += (float)((Now() - m_Start) / 1e6);
    }
}
