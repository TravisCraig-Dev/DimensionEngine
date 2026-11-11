#include "Panels.h"
#include <Dimension/Project/Project.h>
#include <Dimension/Scripting/CSharpProject.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

namespace Editor
{
    void PackagesPanel::Run(int kind, const std::string& label, const std::string& command)
    {
        if (m_Job.valid()) return;
        m_JobKind = kind;
        m_Status = label;
        m_Job = std::async(std::launch::async, [command] {
            std::string output;
            int code = CSharpProject::RunCommand(command, output);
            return std::make_pair(code, output);
        });
    }

    void PackagesPanel::Search(const std::string& term)
    {
        m_Query = term;
        if (!term.empty()) Run(1, "Searching nuget.org for \"" + term + "\"...", CSharpProject::SearchCommand(term));
    }

    void PackagesPanel::OnImGui(bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Packages", open))
        {
            ImGui::End();
            return;
        }
        fs::path project = Project::GetDirectory();
        // A command finished: its results, or what went wrong.
        if (m_Job.valid() && m_Job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto [code, output] = m_Job.get();
            if (m_JobKind == 1)
            {
                m_Results = CSharpProject::ParseSearchResults(output);
                m_Status = code == 0 ? std::to_string(m_Results.size()) + " package(s) found" : "Search failed: " + output.substr(0, 300);
            }
            else
            {
                m_Status = code == 0 ? m_Status.substr(0, m_Status.size() - 3) + ": done (the scripts build again)" : "Failed: " + output.substr(0, 300);
                if (code != 0) Log::Error("Packages: {}", output);
            }
        }
        bool busy = m_Job.valid();
        if (!fs::exists(CSharpProject::ProjectFile(project)))
        {
            ImGui::TextDisabled("This project has no C# scripts project yet (add a C# script first).");
            ImGui::End();
            return;
        }

        // The packages the scripts use.
        ImGui::SeparatorText("Installed");
        auto installed = CSharpProject::ListPackages(project);
        if (installed.empty()) ImGui::TextDisabled("No NuGet packages yet: search below and Add one.");
        else if (ImGui::BeginTable("##installed", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Package");
            ImGui::TableSetupColumn("Version");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            for (const auto& p : installed)
            {
                ImGui::PushID(p.Id.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.Id.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.Version.empty() ? "(any)" : p.Version.c_str());
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(busy);
                if (ImGui::SmallButton("Remove")) Run(2, "Removing " + p.Id + "...", CSharpProject::RemovePackageCommand(project, p.Id));
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // nuget.org.
        ImGui::SeparatorText("nuget.org");
        ImGui::SetNextItemWidth(320);
        bool enter = ImGui::InputTextWithHint("##query", "Search packages (e.g. Newtonsoft.Json)", &m_Query, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || m_Query.empty());
        if (ImGui::Button("Search") || (enter && !busy)) Search(m_Query);
        ImGui::EndDisabled();
        if (!m_Status.empty()) ImGui::TextDisabled("%s", m_Status.c_str());
        if (!m_Results.empty() && ImGui::BeginTable("##results", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Package");
            ImGui::TableSetupColumn("Latest");
            ImGui::TableSetupColumn("Downloads");
            ImGui::TableSetupColumn("Owners");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (const auto& r : m_Results)
            {
                ImGui::PushID(r.Id.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.Id.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.LatestVersion.c_str());
                ImGui::TableNextColumn();
                ImGui::Text(r.Downloads >= 1000000 ? "%.1fM" : "%.0fk", r.Downloads >= 1000000 ? r.Downloads / 1e6 : r.Downloads / 1e3);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.Owners.c_str());
                ImGui::TableNextColumn();
                auto have = std::find_if(installed.begin(), installed.end(), [&](const auto& p) { return p.Id == r.Id; });
                ImGui::BeginDisabled(busy || (have != installed.end() && have->Version == r.LatestVersion));
                const char* label = have == installed.end() ? "Add" : have->Version == r.LatestVersion ? "Installed" : "Update";
                if (ImGui::SmallButton(label))
                    Run(2, std::string(have == installed.end() ? "Adding " : "Updating ") + r.Id + " " + r.LatestVersion + "...",
                        CSharpProject::AddPackageCommand(project, r.Id, r.LatestVersion));
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
}
