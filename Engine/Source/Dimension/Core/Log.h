#pragma once
#include "Dimension/Core/Base.h"
#include <format>

namespace Dimension
{
    enum class LogLevel : uint8_t { Trace, Info, Warn, Error };

    struct LogEntry
    {
        LogLevel Level;
        std::string Message;
        double Time; // seconds since startup
    };

    // Thread-safe logger with pluggable sinks. Output always goes to stdout/stderr;
    // the editor console registers an extra sink.
    class Log
    {
    public:
        using Sink = std::function<void(const LogEntry&)>;

        static void Write(LogLevel level, std::string message);
        static int AddSink(Sink sink);
        static void RemoveSink(int id);

        template <class... A> static void Trace(std::format_string<A...> f, A&&... a) { Write(LogLevel::Trace, std::format(f, std::forward<A>(a)...)); }
        template <class... A> static void Info(std::format_string<A...> f, A&&... a) { Write(LogLevel::Info, std::format(f, std::forward<A>(a)...)); }
        template <class... A> static void Warn(std::format_string<A...> f, A&&... a) { Write(LogLevel::Warn, std::format(f, std::forward<A>(a)...)); }
        template <class... A> static void Error(std::format_string<A...> f, A&&... a) { Write(LogLevel::Error, std::format(f, std::forward<A>(a)...)); }
    };
}
