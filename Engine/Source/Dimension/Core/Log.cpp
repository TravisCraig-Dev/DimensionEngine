#include "Dimension/Core/Log.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace Dimension
{
    namespace
    {
        std::recursive_mutex& Mutex() { static std::recursive_mutex m; return m; }
        std::map<int, Log::Sink>& Sinks() { static std::map<int, Log::Sink> s; return s; }
        const auto kStart = std::chrono::steady_clock::now();
        int s_NextSinkId = 1;
    }

    void Log::Write(LogLevel level, std::string message)
    {
        LogEntry entry{ level, std::move(message),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - kStart).count() };
        static constexpr const char* kTags[] = { "trace", "info", "warn", "error" };
        static constexpr const char* kColors[] = { "\x1b[90m", "\x1b[0m", "\x1b[33m", "\x1b[31m" };

        std::lock_guard lock(Mutex());
        std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
        std::fprintf(out, "%s[%8.3f] [%s] %s\x1b[0m\n", kColors[(int)level], entry.Time, kTags[(int)level], entry.Message.c_str());
        std::fflush(out);
        for (auto& [id, sink] : Sinks()) sink(entry);
    }

    int Log::AddSink(Sink sink)
    {
        std::lock_guard lock(Mutex());
        int id = s_NextSinkId++;
        Sinks()[id] = std::move(sink);
        return id;
    }

    void Log::RemoveSink(int id)
    {
        std::lock_guard lock(Mutex());
        Sinks().erase(id);
    }

    void FatalError(std::string_view message, const char* file, int line)
    {
        Log::Error("FATAL: {} ({}:{})", message, file, line);
#if defined(_MSC_VER) && defined(DIM_DEBUG)
        __debugbreak();
#endif
        std::abort();
    }
}
