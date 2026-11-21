// Music Mode's VST3 plug-ins: the installed list (cached in the project's .dimension/MusicPlugins.json and rescanned
// on request), menus to pick one, and the panel for a plug-in in use (its editor window and its parameters).
#include "MusicMode.h"
#include "MusicModeUI.h"
#include "EditorUI.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Platform/FileDialogs.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <fstream>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Music;

    namespace
    {
        fs::path PluginCacheFile() { return Project::GetDirectory() / ".dimension" / "MusicPlugins.json"; }

        std::string Lower(std::string s)
        {
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        }
    }

    void MusicMode::LoadPluginList()
    {
        if (m_PluginListLoaded) return;
        m_PluginListLoaded = true;
        std::ifstream file(PluginCacheFile());
        nlohmann::json j = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
        if (!j.is_object())
        {
            ScanPlugins(); // (first use in this project)
            return;
        }
        m_PluginFolders = j.value("folders", std::vector<std::string>{});
        m_PluginList.clear();
        for (const auto& p : j.value("plugins", nlohmann::json::array()))
        {
            PluginInfo info;
            info.Name = p.value("name", "");
            info.Vendor = p.value("vendor", "");
            info.Version = p.value("version", "");
            info.ClassId = p.value("classId", "");
            info.SubCategories = p.value("subCategories", "");
            info.Module = p.value("module", "");
            info.Instrument = p.value("instrument", false);
            if (!info.ClassId.empty()) m_PluginList.push_back(info);
        }
    }

    void MusicMode::ScanPlugins()
    {
        std::vector<fs::path> folders = Plugins::DefaultFolders();
        for (const auto& f : m_PluginFolders) folders.push_back(f);
        m_PluginErrors.clear();
        m_PluginList = Plugins::Scan(folders, &m_PluginErrors);
        m_PluginListLoaded = true;
        nlohmann::json plugins = nlohmann::json::array();
        for (const PluginInfo& p : m_PluginList)
            plugins.push_back({ { "name", p.Name }, { "vendor", p.Vendor }, { "version", p.Version }, { "classId", p.ClassId },
                                { "subCategories", p.SubCategories }, { "module", p.Module.string() }, { "instrument", p.Instrument } });
        std::error_code ec;
        fs::create_directories(PluginCacheFile().parent_path(), ec);
        std::ofstream(PluginCacheFile()) << nlohmann::json{ { "folders", m_PluginFolders }, { "plugins", plugins } }.dump(1);
        m_Status = "Found " + std::to_string(m_PluginList.size()) + " plug-ins";
        m_StatusTime = 4.0f;
        Log::Info("Music: found {} VST3 plug-ins ({} could not be read)", m_PluginList.size(), m_PluginErrors.size());
    }

    bool MusicMode::PluginMenu(bool instruments, PluginRef& picked)
    {
        // The installed plug-ins of one kind, by vendor; then the Plug-ins window.
        LoadPluginList();
        bool chosen = false;
        std::vector<std::string> vendors;
        for (const PluginInfo& p : m_PluginList)
            if (p.Instrument == instruments && std::find(vendors.begin(), vendors.end(), p.Vendor) == vendors.end()) vendors.push_back(p.Vendor);
        std::sort(vendors.begin(), vendors.end());
        for (const std::string& vendor : vendors)
        {
            if (!ImGui::BeginMenu(vendor.empty() ? "(no vendor)" : vendor.c_str())) continue;
            for (const PluginInfo& p : m_PluginList)
                if (p.Instrument == instruments && p.Vendor == vendor && ImGui::MenuItem(p.Name.c_str()))
                {
                    picked = PluginRef{};
                    picked.Name = p.Name;
                    picked.Module = p.Module.string();
                    picked.ClassId = p.ClassId;
                    picked.Id = NewPluginId();
                    chosen = true;
                }
            ImGui::EndMenu();
        }
        if (vendors.empty()) ImGui::TextDisabled(instruments ? "No VST3 instruments found" : "No VST3 effects found");
        ImGui::Separator();
        if (ImGui::MenuItem("Manage Plug-ins...")) m_ShowPluginManager = true;
        return chosen;
    }

    void MusicMode::DrawPluginManager()
    {
        if (!m_ShowPluginManager) return;
        ImGui::SetNextWindowSize(ImVec2(620.0f, 460.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Plug-ins", &m_ShowPluginManager, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }
        LoadPluginList();
        if (!Plugins::IsSupported()) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "VST3 plug-ins are not supported on this platform.");
        ImGui::TextUnformatted("VST3 folders searched:");
        for (const fs::path& f : Plugins::DefaultFolders()) ImGui::BulletText("%s", f.string().c_str());
        int remove = -1;
        for (int i = 0; i < (int)m_PluginFolders.size(); ++i)
        {
            ImGui::PushID(i);
            ImGui::BulletText("%s", m_PluginFolders[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) remove = i;
            ImGui::PopID();
        }
        if (remove >= 0) m_PluginFolders.erase(m_PluginFolders.begin() + remove);
        if (ImGui::Button("Add Folder..."))
            if (auto folder = FileDialogs::PickFolder()) m_PluginFolders.push_back(folder->string());
        ImGui::SameLine();
        if (ImGui::Button("Scan")) ScanPlugins();
        ImGui::SameLine();
        ImGui::TextDisabled("(scanning loads each plug-in once to read what it offers)");
        ImGui::Separator();
        ImGui::Text("%d plug-ins", (int)m_PluginList.size());
        if (ImGui::BeginTable("##plugins", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, ImGui::GetContentRegionAvail().y - (m_PluginErrors.empty() ? 0.0f : 80.0f))))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Vendor");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableHeadersRow();
            for (const PluginInfo& p : m_PluginList)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.Name.c_str());
                ImGui::SetItemTooltip("%s", p.Module.string().c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.Vendor.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", p.Instrument ? "Instrument" : "Effect");
            }
            ImGui::EndTable();
        }
        if (!m_PluginErrors.empty())
        {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "%d could not be read:", (int)m_PluginErrors.size());
            for (const auto& e : m_PluginErrors) ImGui::TextDisabled("%s", e.c_str());
        }
        ImGui::End();
    }

    void MusicMode::DrawPluginPanel(PluginRef& ref, int index, int slot, float height)
    {
        // A plug-in in use: its editor window, and its parameters (drag to set; right-click to automate).
        ImGui::PushID((int)(ref.Id & 0x7FFFFFFF));
        auto instance = m_Rack ? m_Rack->Get(ref.Id) : nullptr;
        ImGui::TextColored(UI::AccentText(), "%s", ref.Name.c_str());
        if (!instance)
        {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "Not loaded: is it installed?");
            ImGui::TextDisabled("%s", ref.Module.c_str());
            ImGui::PopID();
            return;
        }
        auto hasEditor = m_HasEditorCache.find(ref.Id);
        if (hasEditor == m_HasEditorCache.end()) hasEditor = m_HasEditorCache.emplace(ref.Id, instance->HasEditor()).first;
        if (hasEditor->second)
        {
            bool open = instance->IsEditorOpen();
            if (ImGui::Button(open ? "Close Editor" : "Open Editor"))
            {
                if (open) instance->CloseEditor();
                else if (!instance->OpenEditor(ref.Name + " - " + m_Song.Name)) m_Status = "The plug-in's editor would not open", m_StatusTime = 4.0f;
            }
        }
        else ImGui::TextDisabled("(no editor window here: its parameters below)");

        auto& params = m_ParamCache[ref.Id];
        if (params.empty()) params = instance->Parameters();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", "Filter parameters", &m_ParamFilter);
        std::string filter = Lower(m_ParamFilter);
        (void)height;
        ImGui::BeginChild("##params", ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y)), ImGuiChildFlags_None);
        if (UI::BeginProperties("##pluginParams"))
        {
            for (const PluginParameter& p : params)
            {
                if (p.Hidden || (!filter.empty() && Lower(p.Title).find(filter) == std::string::npos)) continue;
                ImGui::PushID((int)p.Id);
                UI::PropertyLabel(p.Title.c_str());
                float v = (float)instance->GetParameter(p.Id);
                std::string shown = instance->FormatParameter(p.Id, v);
                if (!p.Units.empty()) shown += " " + p.Units;
                ImGui::BeginDisabled(p.ReadOnly);
                if (ImGui::SliderFloat("##v", &v, 0.0f, 1.0f, shown.empty() ? "%.3f" : shown.c_str()))
                {
                    if (p.Steps > 0) v = std::round(v * p.Steps) / p.Steps;
                    instance->SetParameter(p.Id, v);
                    m_Dirty = m_Unsaved = true;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    instance->SetParameter(p.Id, p.Default);
                    m_Dirty = m_Unsaved = true;
                }
                ImGui::EndDisabled();
                if (p.Automatable && ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Create Automation Clip"))
                    {
                        // The clip starts at the parameter's current value.
                        ref.ParamNames[(int)p.Id] = p.Title;
                        ref.Values[(int)p.Id] = v;
                        CreateAutomation({ ParamKind::PluginParam, index, slot, (int)p.Id });
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            UI::EndProperties();
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
}
