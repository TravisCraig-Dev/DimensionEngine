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

    void LanguageService::Send(Question request)
    {
        if (!m_Running) return;
        {
            std::lock_guard lock(m_Mutex);
            int kind = (int)request.Kind;
            m_Pending[kind] = std::move(request);
        }
        m_Wake.notify_one();
    }

    bool LanguageService::Busy(Query kind) const
    {
        std::lock_guard lock(m_Mutex);
        return m_Working == (int)kind || m_Pending[(int)kind].has_value();
    }

    std::optional<LanguageService::Result> LanguageService::Poll(Query kind)
    {
        std::lock_guard lock(m_Mutex);
        std::optional<Result> out = std::move(m_Done[(int)kind]);
        m_Done[(int)kind].reset();
        return out;
    }

    void LanguageService::Worker()
    {
        for (;;)
        {
            Question job;
            {
                std::unique_lock lock(m_Mutex);
                auto any = [&] {
                    for (const auto& p : m_Pending)
                        if (p) return true;
                    return false;
                };
                m_Wake.wait(lock, [&] { return m_Quit || any(); });
                if (m_Quit) return;
                for (auto& p : m_Pending) // (the first kind waiting: completion before the rest)
                    if (p)
                    {
                        job = std::move(*p);
                        p.reset();
                        break;
                    }
                m_Working = (int)job.Kind;
            }
            static const char* kKinds[] = { "completion", "signature", "hover", "definition", "references", "renameCheck", "rename", "format", "diagnostics" };
            static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == (size_t)Query::Count);
            nlohmann::json request;
            request["kind"] = kKinds[(int)job.Kind];
            request["line"] = job.Line;
            request["col"] = job.Col;
            request["file"] = Utf8Path(job.File);
            if (job.Kind == Query::Rename) request["newName"] = job.NewName;
            if (job.Kind == Query::Format)
            {
                request["endLine"] = job.EndLine;
                request["tabs"] = job.Tabs;
                request["crlf"] = job.Crlf;
            }
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
            result.Kind = job.Kind;
            {
                std::lock_guard lock(m_Mutex);
                m_Done[(int)job.Kind] = std::move(result);
                m_Working = -1;
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
        if (j.contains("items") && j["items"].is_array())
            for (const auto& i : j["items"])
                r.Items.push_back({ i.value("label", std::string()), i.value("kind", std::string()), i.value("detail", std::string()),
                                    i.value("doc", std::string()), i.value("insert", std::string()) });
        if (j.contains("signatures") && j["signatures"].is_array())
        {
            for (const auto& s : j["signatures"])
            {
                Signature sig{ s.value("label", std::string()), s.value("doc", std::string()), {} };
                // (the service counts UTF-16 units in the label: as bytes here)
                std::string label = sig.Label;
                if (s.contains("params") && s["params"].is_array())
                    for (const auto& p : s["params"])
                    {
                        int start = Code::Utf16ToByte(label, p.value("start", 0));
                        int end = Code::Utf16ToByte(label, p.value("start", 0) + p.value("length", 0));
                        sig.Params.push_back({ start, end - start, p.value("name", std::string()), p.value("doc", std::string()) });
                    }
                r.Signatures.push_back(std::move(sig));
            }
            r.ActiveSignature = j.value("active", 0);
            r.ActiveParam = j.value("param", 0);
        }
        auto places = [&](const char* key, auto add) {
            if (j.contains(key) && j[key].is_array())
                for (const auto& l : j[key]) add(l);
        };
        places("locations", [&](const nlohmann::json& l) {
            r.Locations.push_back({ PathFromUtf8(l.value("file", std::string())), l.value("line", 0), l.value("col", 0), l.value("endLine", 0), l.value("endCol", 0),
                                    l.value("text", std::string()), l.value("definition", false) });
        });
        places("edits", [&](const nlohmann::json& e) {
            r.Edits.push_back({ PathFromUtf8(e.value("file", std::string())), e.value("line", 0), e.value("col", 0), e.value("endLine", 0), e.value("endCol", 0),
                                e.value("text", std::string()) });
        });
        r.Name = j.value("name", std::string());
        r.Message = j.value("message", std::string());
        r.Warning = j.value("warning", std::string());
        if (j.contains("renameFile") && j["renameFile"].is_object())
        {
            r.RenameFrom = PathFromUtf8(j["renameFile"].value("from", std::string()));
            r.RenameTo = PathFromUtf8(j["renameFile"].value("to", std::string()));
        }
        r.HoverText = j.is_object() && j.contains("text") && j["text"].is_string() ? j["text"].get<std::string>() : std::string();
        r.HoverDoc = j.value("doc", std::string());
        return r;
    }
}
