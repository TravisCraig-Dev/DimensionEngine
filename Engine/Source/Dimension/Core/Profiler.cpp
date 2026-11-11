#include "Dimension/Core/Profiler.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <cstring>
#include <mutex>
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
            uint64_t Frame = 0;
            std::vector<std::unique_ptr<Scope>> Samples; // BeginSample's open sections, innermost last
            // Worker threads add to Current.Workers (and intern names) under this; the frame changes under it too.
            std::mutex WorkerMutex;
            std::map<std::thread::id, int> WorkerIndex;
            bool Deep = false;
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
        // Samples still open: closed with the frame they began in.
        while (!s.Samples.empty()) s.Samples.pop_back();
        long long now = Now();
        std::lock_guard lock(s.WorkerMutex);
        if (s.Recording && !s.Paused)
        {
            s.Current.FrameMs = (float)((now - s.FrameStart) / 1e6);
            s.History.push_back(std::move(s.Current));
            while (s.History.size() > kHistory) s.History.pop_front();
        }
        s.Current = FrameRecord();
        s.Open.clear();
        ++s.Frame;
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

    long long Timestamp() { return Now(); }

    void AddWorkerSpan(const std::string& name, long long start)
    {
        long long end = Now();
        State& s = S();
        std::lock_guard lock(s.WorkerMutex);
        if (!s.Recording || s.Current.Workers.size() >= kMaxSpans) return;
        auto [it, added] = s.WorkerIndex.try_emplace(std::this_thread::get_id(), (int)s.WorkerIndex.size());
        float startMs = (float)((std::max(start, s.FrameStart) - s.FrameStart) / 1e6);
        s.Current.Workers.push_back({ Intern(name), it->second, startMs, (float)((end - std::max(start, s.FrameStart)) / 1e6) });
    }

    void SetDeepProfiling(bool deep) { S().Deep = deep; }
    bool IsDeepProfiling() { return S().Deep; }

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
        m_Frame = s.Frame;
        if (s.Current.Spans.size() < kMaxSpans)
        {
            m_Span = (int)s.Current.Spans.size();
            s.Current.Spans.push_back({ name, depth, (float)((m_Start - s.FrameStart) / 1e6), 0.0f });
        }
    }

    Scope::~Scope()
    {
        if (m_Section < 0) return;
        State& s = S();
        if (m_Frame != s.Frame) return; // the frame it was in has ended
        if (!s.Open.empty() && s.Open.back() == m_Section) s.Open.pop_back();
        float ms = (float)((Now() - m_Start) / 1e6);
        if (m_Section < (int)s.Current.Sections.size()) s.Current.Sections[m_Section].Ms += ms;
        if (m_Span >= 0 && m_Span < (int)s.Current.Spans.size()) s.Current.Spans[m_Span].Ms = ms;
    }

    void BeginSample(const std::string& name)
    {
        State& s = S();
        if (!OnMainThread(s)) return;
        s.Samples.push_back(std::make_unique<Scope>(Intern(name)));
    }

    void EndSample()
    {
        State& s = S();
        if (!s.Samples.empty()) s.Samples.pop_back();
    }

    const char* Intern(const std::string& name)
    {
        static std::set<std::string> names; // nodes do not move: the pointers stay good
        static std::mutex mutex;              // (worker threads intern job names)
        std::lock_guard lock(mutex);
        return names.insert(name).first->c_str();
    }

    bool SaveRecording(const std::filesystem::path& file, const std::deque<FrameRecord>& frames)
    {
        using json = nlohmann::json;
        auto sections = [](const std::vector<Section>& list) {
            json out = json::array();
            for (const Section& s : list) out.push_back({ s.Name, s.Depth, s.Ms, s.Calls });
            return out;
        };
        json j{ { "dimensionProfile", 1 }, { "frames", json::array() } };
        for (const FrameRecord& f : frames)
        {
            json frame{ { "ms", f.FrameMs }, { "sections", sections(f.Sections) }, { "gpu", sections(f.Gpu) }, { "counters", json::object() },
                        { "spans", json::array() } };
            for (const auto& [name, value] : f.Counters) frame["counters"][name] = value;
            for (const Span& s : f.Spans) frame["spans"].push_back({ s.Name, s.Depth, s.StartMs, s.Ms });
            if (!f.Workers.empty())
            {
                frame["workers"] = json::array();
                for (const WorkerSpan& w : f.Workers) frame["workers"].push_back({ w.Name, w.Worker, w.StartMs, w.Ms });
            }
            j["frames"].push_back(std::move(frame));
        }
        std::ofstream out(file);
        if (!out) return false;
        out << j.dump();
        return (bool)out;
    }

    std::optional<std::deque<FrameRecord>> LoadRecording(const std::filesystem::path& file)
    {
        using json = nlohmann::json;
        std::ifstream in(file);
        if (!in) return std::nullopt;
        json j = json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("dimensionProfile") || !j["frames"].is_array()) return std::nullopt;
        auto sections = [](const json& list) {
            std::vector<Section> out;
            for (const json& s : list)
                if (s.is_array() && s.size() == 4) out.push_back({ Intern(s[0].get<std::string>()), s[1].get<int>(), s[2].get<float>(), s[3].get<int>() });
            return out;
        };
        std::deque<FrameRecord> frames;
        try
        {
            for (const json& f : j["frames"])
            {
                FrameRecord r;
                r.FrameMs = f.value("ms", 0.0f);
                r.Sections = sections(f.value("sections", json::array()));
                r.Gpu = sections(f.value("gpu", json::array()));
                for (auto& [name, value] : f.value("counters", json::object()).items()) r.Counters.push_back({ name, value.get<float>() });
                for (const json& s : f.value("spans", json::array()))
                    if (s.is_array() && s.size() == 4) r.Spans.push_back({ Intern(s[0].get<std::string>()), s[1].get<int>(), s[2].get<float>(), s[3].get<float>() });
                for (const json& w : f.value("workers", json::array()))
                    if (w.is_array() && w.size() == 4) r.Workers.push_back({ Intern(w[0].get<std::string>()), w[1].get<int>(), w[2].get<float>(), w[3].get<float>() });
                frames.push_back(std::move(r));
            }
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
        return frames;
    }

    std::vector<Comparison> Compare(const FrameRecord& a, const FrameRecord& b)
    {
        // Each section's path: its name under its parents' ("Update/Physics").
        auto paths = [](const std::vector<Section>& list) {
            std::vector<std::string> out, stack;
            for (const Section& s : list)
            {
                stack.resize(std::min<size_t>(stack.size(), (size_t)std::max(s.Depth, 0)));
                stack.push_back(s.Name);
                std::string p;
                for (const auto& part : stack) p += (p.empty() ? "" : "/") + part;
                out.push_back(p);
            }
            return out;
        };
        std::vector<std::string> pa = paths(a.Sections), pb = paths(b.Sections);
        std::map<std::string, size_t> inB;
        for (size_t i = 0; i < pb.size(); ++i) inB.emplace(pb[i], i);
        std::vector<Comparison> out;
        std::set<size_t> used;
        for (size_t i = 0; i < pa.size(); ++i)
        {
            const Section& s = a.Sections[i];
            Comparison c{ s.Name, s.Depth, s.Ms, 0.0f, s.Calls, 0, true, false };
            if (auto it = inB.find(pa[i]); it != inB.end())
            {
                const Section& t = b.Sections[it->second];
                c.MsB = t.Ms, c.CallsB = t.Calls, c.InB = true;
                used.insert(it->second);
            }
            out.push_back(c);
        }
        for (size_t i = 0; i < pb.size(); ++i)
            if (!used.contains(i))
            {
                const Section& t = b.Sections[i];
                out.push_back({ t.Name, t.Depth, 0.0f, t.Ms, 0, t.Calls, false, true });
            }
        return out;
    }

    int SpanAt(const FrameRecord& frame, int depth, float ms)
    {
        for (int i = (int)frame.Spans.size() - 1; i >= 0; --i)
        {
            const Span& s = frame.Spans[i];
            if (s.Depth == depth && ms >= s.StartMs && ms <= s.StartMs + s.Ms) return i;
        }
        return -1;
    }
}
