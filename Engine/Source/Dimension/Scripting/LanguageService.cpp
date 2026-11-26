#include "Dimension/Scripting/LanguageService.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Scripting/CSharpProject.h"
#include "Dimension/Scripting/CSharpRuntime.h"
#include <nlohmann/json.hpp>

namespace Dimension
{
    namespace
    {
        // Paths travel as UTF-8 (the service's JSON).
        std::string Utf8Path(const fs::path& p)
        {
            std::u8string s = p.u8string();
            return std::string(s.begin(), s.end());
        }
        fs::path PathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
    }

    LanguageService::~LanguageService()
    {
        {
            std::lock_guard lock(m_Mutex);
            m_Quit = true;
        }
        m_Wake.notify_all();
        if (m_Thread.joinable()) m_Thread.join();
    }

    bool LanguageService::Start(const fs::path& managedDir, const fs::path& projectDir)
    {
        if (m_Running) return true;
        if (!CSharpRuntime::IsAvailable()) return false;
        fs::path assembly = managedDir / "codeservice" / "Dimension.CodeService.dll";
        std::error_code ec;
        if (!fs::exists(assembly, ec))
        {
            Log::Info("Code Mode: no language service at '{}' (live errors off)", assembly.string());
            return false;
        }
        const char* type = "Dimension.CodeService.Service, Dimension.CodeService";
        auto open = (int (*)(const char*))CSharpRuntime::GetFunction(assembly, type, "Open");
        m_Analyze = (AnalyzeFn)CSharpRuntime::GetFunction(assembly, type, "Analyze");
        m_Free = (FreeFn)CSharpRuntime::GetFunction(assembly, type, "Free");
        if (!open || !m_Analyze || !m_Free) return false;
        nlohmann::json config;
        config["scriptsDir"] = Utf8Path(CSharpProject::ScriptsDirectory(projectDir));
        config["managedDir"] = Utf8Path(managedDir);
        config["assembliesDir"] = Utf8Path(CSharpProject::AssemblyPath(projectDir).parent_path());
        if (!open(config.dump().c_str()))
        {
            Log::Error("Code Mode: the language service did not start");
            return false;
        }
        m_Running = true;
        m_Thread = std::thread([this] { Worker(); });
        return true;
    }

    void LanguageService::Request(const fs::path& file, std::vector<Overlay> overlays, uint64_t tag)
    {
        if (!m_Running) return;
        {
            std::lock_guard lock(m_Mutex);
            m_Pending = Pending{ file, std::move(overlays), tag };
        }
        m_Wake.notify_one();
    }

    bool LanguageService::Busy() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Working || m_Pending.has_value();
    }

    std::optional<LanguageService::Result> LanguageService::Poll()
    {
        std::lock_guard lock(m_Mutex);
        std::optional<Result> out = std::move(m_Done);
        m_Done.reset();
        return out;
    }

    void LanguageService::Worker()
    {
        for (;;)
        {
            Pending job;
            {
                std::unique_lock lock(m_Mutex);
                m_Wake.wait(lock, [&] { return m_Quit || m_Pending.has_value(); });
                if (m_Quit) return;
                job = std::move(*m_Pending);
                m_Pending.reset();
                m_Working = true;
            }
            nlohmann::json request;
            request["file"] = Utf8Path(job.File);
            request["overlays"] = nlohmann::json::array();
            for (const Overlay& o : job.Overlays) request["overlays"].push_back({ { "path", Utf8Path(o.Path) }, { "text", o.Text } });
            std::string answer;
            if (char* json = m_Analyze(request.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str()))
            {
                answer = json;
                m_Free(json);
            }
            Result result = ParseResult(answer);
            result.Tag = job.Tag;
            {
                std::lock_guard lock(m_Mutex);
                m_Done = std::move(result);
                m_Working = false;
            }
        }
    }

    LanguageService::Result LanguageService::ParseResult(const std::string& json)
    {
        Result r;
        nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
        if (!j.is_object())
        {
            r.Error = "the language service answered nothing readable";
            return r;
        }
        if (j.contains("error"))
        {
            r.Error = j.value("error", std::string());
            return r;
        }
        r.Assembly = j.value("assembly", std::string());
        r.Milliseconds = j.value("ms", 0.0);
        if (j.contains("files") && j["files"].is_array())
            for (const auto& f : j["files"])
                if (f.is_string()) r.Files.push_back(PathFromUtf8(f.get<std::string>()));
        if (j.contains("diagnostics") && j["diagnostics"].is_array())
            for (const auto& d : j["diagnostics"])
            {
                Code::Diagnostic diag;
                diag.File = d.value("file", std::string());
                diag.Line = d.value("line", 0);
                diag.Col = d.value("col", 0);
                diag.EndLine = d.value("endLine", -1);
                diag.EndCol = d.value("endCol", -1);
                diag.Level = d.value("severity", std::string()) == "warning" ? Code::Severity::Warning : Code::Severity::Error;
                diag.Code = d.value("code", std::string());
                diag.Message = d.value("message", std::string());
                r.Diagnostics.push_back(std::move(diag));
            }
        return r;
    }
}
