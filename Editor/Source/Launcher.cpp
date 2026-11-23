#include "Launcher.h"
#include <Dimension/Core/Application.h>
#include <Dimension/Core/Log.h>
#include <Dimension/ImGui/ImGuiLayer.h>
#include <Dimension/Platform/FileDialogs.h>
#include <Dimension/Platform/Platform.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Project/ProjectHub.h>
#include <Dimension/Scene/SceneTemplates.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <chrono>
#include <cmath>

#ifndef DIM_ENGINE_VERSION
#define DIM_ENGINE_VERSION "0.1.0"
#endif

namespace Editor::Launcher
{
    using namespace Dimension;

    fs::path DefaultProjectsFolder() { return Platform::DocumentsDirectory() / "Dimension Projects"; }

    fs::path ShippedSample()
    {
#ifdef DIM_SAMPLE_PROJECT
        // (a development build: the source tree's own)
        if (ProjectHub::IsProject(DIM_SAMPLE_PROJECT)) return fs::path(DIM_SAMPLE_PROJECT);
#endif
        // Installs: <prefix>/bin/DimensionEditor and <prefix>/Samples/Siege (Windows and Linux alike).
        fs::path exe = Platform::ExecutableDirectory();
        for (const fs::path& p : { exe.parent_path() / "Samples" / "Siege", exe / "Samples" / "Siege" })
            if (ProjectHub::IsProject(p)) return p;
        return {};
    }

    namespace
    {
        // A project folder copied without what the editor makes in it (caches, build output, generated files).
        bool CopyProject(const fs::path& from, const fs::path& to, std::string& error)
        {
            std::error_code ec;
            fs::create_directories(to, ec);
            for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const fs::path& src = it->path();
                std::string name = src.filename().string();
                fs::path rel = fs::relative(src, from, ec);
                bool generated = name == ".dimension" || ((name == "bin" || name == "obj") && rel.begin()->string() == "Scripts") ||
                                 name == "Directory.Build.props";
                if (generated)
                {
                    if (it->is_directory()) it.disable_recursion_pending();
                    continue;
                }
                fs::path dst = to / rel;
                if (it->is_directory()) fs::create_directories(dst, ec);
                else fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    error = "Could not copy '" + src.string() + "': " + ec.message();
                    return false;
                }
            }
            if (ec)
            {
                error = "Could not read '" + from.string() + "': " + ec.message();
                return false;
            }
            // Its scripts compiled when it was packaged (an install's sample), so it plays without the .NET SDK: copied
            // too, and dated now, after the sources just copied (a copy may not keep the times, and an older assembly
            // than its sources is built again).
            fs::path built = from / ".dimension" / "ScriptAssemblies";
            if (fs::exists(built / "GameScripts.dll", ec))
            {
                fs::path out = to / ".dimension" / "ScriptAssemblies";
                fs::create_directories(out, ec);
                auto now = fs::file_time_type::clock::now();
                for (const auto& entry : fs::directory_iterator(built, ec))
                {
                    if (!entry.is_regular_file()) continue;
                    fs::path dst = out / entry.path().filename();
                    fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
                    if (ec)
                    {
                        error = "Could not copy '" + entry.path().string() + "': " + ec.message();
                        return false;
                    }
                    fs::last_write_time(dst, now, ec);
                }
                ec.clear();
            }
            return true;
        }
    }

    fs::path SampleProject(std::string* error)
    {
        fs::path sample = ShippedSample();
        if (sample.empty())
        {
            if (error) *error = "The Siege sample isn't installed.";
            return {};
        }
#ifdef DIM_SAMPLE_PROJECT
        if (sample == fs::path(DIM_SAMPLE_PROJECT)) return sample;
#endif
        // From an install: the user's own copy (made once; opened as it is after that).
        fs::path copy = DefaultProjectsFolder() / "Siege";
        if (ProjectHub::IsProject(copy)) return copy;
        std::string why;
        if (!CopyProject(sample, copy, why))
        {
            if (error) *error = why;
            return {};
        }
        Log::Info("Copied the Siege sample to '{}'", copy.string());
        return copy;
    }

    namespace
    {
        class LauncherLayer : public Layer
        {
        public:
            explicit LauncherLayer(std::optional<Choice>& result) : m_Result(result) {}

            void OnAttach() override
            {
                ImGuiLayer::ApplyEditorStyle(ImGuiLayer::ThemeDark);
                ImGui::GetIO().IniFilename = nullptr; // (nothing to remember about this window's layout)
                m_Location = DefaultProjectsFolder().string();
                for (const auto& t : SceneTemplates::Builtins()) m_Starters.push_back({ t.Name, t.Description }); // (no project yet)
                m_Tab = ProjectHub::Recent().empty() ? 1 : 0;
                if (m_Tab == 1 && !ShippedSample().empty()) m_Tab = 0; // (first run: the sample is the obvious start)
            }

            void OnImGui() override
            {
                const ImGuiViewport* vp = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(vp->WorkPos);
                ImGui::SetNextWindowSize(vp->WorkSize);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
                ImGui::Begin("##launcher", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
                ImGui::PopStyleVar(3);
                DrawSidebar();
                ImGui::SameLine(0.0f, 0.0f);
                DrawContent();
                ImGui::End();
            }

        private:
            std::optional<Choice>& m_Result;
            int m_Tab = 0; // 0 projects, 1 new project
            std::string m_Name, m_Location, m_Error, m_Filter;
            int m_Starter = 0;
            std::vector<std::pair<std::string, std::string>> m_Starters;

            void Finish(Choice c)
            {
                m_Result = std::move(c);
                Application::Get().Close();
            }

            static ImU32 Accent(float alpha = 1.0f)
            {
                uint32_t a = ImGuiLayer::kDefaultAccent;
                return IM_COL32((a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, (int)(alpha * 255.0f));
            }

            // The engine's mark: a lit isometric cube on a rounded indigo tile (the app icon).
            static void DrawLogo(ImDrawList* dl, ImVec2 c, float size)
            {
                float h = size * 0.5f;
                dl->AddRectFilledMultiColor(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), IM_COL32(58, 72, 140, 255), IM_COL32(58, 72, 140, 255),
                                            IM_COL32(24, 22, 58, 255), IM_COL32(24, 22, 58, 255));
                dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), IM_COL32(255, 255, 255, 30), size * 0.2f, 0, 1.0f);
                float r = size * 0.3f, k = r * 0.866f, y = c.y + size * 0.03f;
                ImVec2 top(c.x, y - r), right(c.x + k, y - r * 0.5f), mid(c.x, y), left(c.x - k, y - r * 0.5f);
                ImVec2 lowL(c.x - k, y + r * 0.5f), lowR(c.x + k, y + r * 0.5f), bottom(c.x, y + r);
                dl->AddQuadFilled(top, right, mid, left, IM_COL32(150, 220, 255, 255));
                dl->AddQuadFilled(left, mid, bottom, lowL, IM_COL32(70, 140, 235, 255));
                dl->AddQuadFilled(right, lowR, bottom, mid, IM_COL32(120, 90, 230, 255));
            }

            bool SideButton(const char* label, bool selected, const char* tip = nullptr)
            {
                ImVec2 size(ImGui::GetContentRegionAvail().x, 34.0f);
                ImVec2 p = ImGui::GetCursorScreenPos();
                bool pressed = ImGui::InvisibleButton(label, size);
                bool hot = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (selected) dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Accent(0.35f), 5.0f);
                else if (hot) dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(255, 255, 255, 18), 5.0f);
                if (selected) dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + size.y), Accent(), 2.0f);
                dl->AddText(ImVec2(p.x + 14.0f, p.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(235, 235, 240, 255), label);
                if (tip && hot) ImGui::SetTooltip("%s", tip);
                return pressed;
            }

            void DrawSidebar()
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(28, 29, 34, 255));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 22.0f));
                ImGui::BeginChild("##side", ImVec2(270.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetCursorScreenPos();
                DrawLogo(dl, ImVec2(p.x + 26.0f, p.y + 26.0f), 52.0f);
                ImGui::SetCursorScreenPos(ImVec2(p.x + 66.0f, p.y + 2.0f));
                ImGui::PushFont(nullptr, 26.0f);
                ImGui::TextUnformatted("Dimension");
                ImGui::PopFont();
                ImGui::SetCursorScreenPos(ImVec2(p.x + 67.0f, p.y + 34.0f));
                ImGui::TextDisabled("Engine " DIM_ENGINE_VERSION);
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 76.0f));

                if (SideButton("Projects", m_Tab == 0)) m_Tab = 0, m_Error.clear();
                if (SideButton("New Project", m_Tab == 1)) m_Tab = 1, m_Error.clear();
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                bool sample = !ShippedSample().empty();
                ImGui::BeginDisabled(!sample);
                if (SideButton("Open the Siege sample", false,
                               sample ? "The sample project: a castle on its crag above a battle of hundreds, catapults, cavalry and a "
                                        "cinematic camera (from an install, your own copy is made in Documents/Dimension Projects)"
                                      : "The Siege sample isn't installed"))
                {
                    std::string error;
                    fs::path project = SampleProject(&error);
                    if (project.empty()) m_Error = error;
                    else Finish({ project });
                }
                ImGui::EndDisabled();
                if (SideButton("Open Project Folder...", false, "Any folder with a Project.dproj (or one to make a project of)"))
                {
                    fs::path start = m_Location.empty() ? DefaultProjectsFolder() : fs::path(m_Location);
                    if (auto dir = FileDialogs::PickFolder(start))
                    {
                        if (ProjectHub::IsProject(*dir)) Finish({ *dir });
                        else if (FileDialogs::Confirm("Open Project", "'" + dir->string() + "' has no project yet. Make it one?"))
                            Finish({ *dir, true, dir->filename().string(), m_Starters.empty() ? "Basic 3D" : m_Starters.front().first });
                    }
                }
                // (at the bottom: a hint)
                const char* hint = "Starting the editor with a project folder (DimensionEditor <folder>) skips this screen.";
                float wrap = ImGui::GetContentRegionAvail().x;
                float hintH = ImGui::CalcTextSize(hint, nullptr, false, wrap).y;
                float bottom = ImGui::GetWindowHeight() - 18.0f - hintH;
                if (bottom > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(bottom);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
                ImGui::TextDisabled("%s", hint);
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }

            void DrawContent()
            {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
                ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
                if (m_Tab == 0) DrawProjects();
                else DrawNewProject();
                if (!m_Error.empty())
                {
                    ImGui::Spacing();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", m_Error.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }

            static void Heading(const char* text)
            {
                ImGui::PushFont(nullptr, 22.0f);
                ImGui::TextUnformatted(text);
                ImGui::PopFont();
                ImGui::Spacing();
            }

            void DrawProjects()
            {
                Heading("Projects");
                auto recent = ProjectHub::Recent();
                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputTextWithHint("##filter", "Search", &m_Filter);
                ImGui::SameLine();
                ImGui::TextDisabled("%zu recent", recent.size());
                ImGui::Spacing();
                if (recent.empty())
                {
                    ImGui::Dummy(ImVec2(0.0f, 30.0f));
                    ImGui::TextDisabled("No projects yet.");
                    ImGui::TextDisabled("Open the Siege sample to look around, or make a New Project.");
                    return;
                }
                auto lower = [](std::string s) {
                    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    return s;
                };
                std::string filter = lower(m_Filter);
                int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                ImGui::BeginChild("##list", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - (m_Error.empty() ? 0.0f : 30.0f)));
                std::optional<fs::path> forget;
                for (size_t i = 0; i < recent.size(); ++i)
                {
                    const auto& p = recent[i];
                    if (!filter.empty() && lower(p.Name).find(filter) == std::string::npos && lower(p.Path.string()).find(filter) == std::string::npos)
                        continue;
                    ImGui::PushID((int)i);
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImVec2 size(ImGui::GetContentRegionAvail().x, 56.0f);
                    ImGui::SetNextItemAllowOverlap();
                    bool clicked = ImGui::InvisibleButton("##card", size);
                    bool hot = ImGui::IsItemHovered();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), hot && p.Exists ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 8), 6.0f);
                    // A tile with the project's initial.
                    ImVec2 tile(pos.x + 10.0f, pos.y + 10.0f);
                    dl->AddRectFilled(tile, ImVec2(tile.x + 36.0f, tile.y + 36.0f), p.Exists ? Accent(0.8f) : IM_COL32(80, 80, 80, 255), 6.0f);
                    char initial[2] = { p.Name.empty() ? '?' : (char)std::toupper((unsigned char)p.Name[0]), 0 };
                    ImGui::PushFont(nullptr, 20.0f);
                    ImVec2 is = ImGui::CalcTextSize(initial);
                    dl->AddText(ImVec2(tile.x + (36.0f - is.x) * 0.5f, tile.y + (36.0f - is.y) * 0.5f), IM_COL32_WHITE, initial);
                    ImGui::PopFont();
                    ImU32 text = p.Exists ? IM_COL32(235, 235, 240, 255) : IM_COL32(140, 140, 140, 255);
                    std::string name = p.Name + (p.Exists ? "" : "   (missing)");
                    dl->AddText(ImVec2(pos.x + 58.0f, pos.y + 10.0f), text, name.c_str());
                    // (a long path keeps its end: the folder names that tell projects apart)
                    std::string path = p.Path.string();
                    float room = size.x - 58.0f - 44.0f;
                    if (ImGui::CalcTextSize(path.c_str()).x > room)
                    {
                        while (path.size() > 4 && ImGui::CalcTextSize(("..." + path).c_str()).x > room) path.erase(0, 1);
                        path = "..." + path;
                    }
                    dl->AddText(ImVec2(pos.x + 58.0f, pos.y + 30.0f), IM_COL32(150, 150, 160, 255), path.c_str());
                    if (hot && path != p.Path.string()) ImGui::SetTooltip("%s", p.Path.string().c_str());
                    int64_t ago = std::max<int64_t>(now - p.LastOpened, 0);
                    std::string when = p.LastOpened == 0 ? "" : ago < 3600 ? "opened just now" : ago < 86400 ? "opened " + std::to_string(ago / 3600) + " h ago"
                                                                                                  : "opened " + std::to_string(ago / 86400) + " days ago";
                    ImVec2 ws = ImGui::CalcTextSize(when.c_str());
                    dl->AddText(ImVec2(pos.x + size.x - ws.x - 44.0f, pos.y + 10.0f), IM_COL32(150, 150, 160, 255), when.c_str());
                    // Remove from the list (the project stays on disk).
                    ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - 32.0f, pos.y + 16.0f));
                    if (hot || ImGui::IsItemHovered() || !p.Exists)
                    {
                        if (ImGui::SmallButton("x")) forget = p.Path;
                        ImGui::SetItemTooltip("Take it off the list (the project stays on disk)");
                    }
                    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 6.0f));
                    if (clicked && p.Exists && !forget) Finish({ p.Path });
                    ImGui::PopID();
                }
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ImGui::EndChild();
                if (forget) ProjectHub::Forget(*forget);
                ImGui::TextDisabled("Click a project to open it.");
            }

            void DrawNewProject()
            {
                Heading("New Project");
                float w = std::min(ImGui::GetContentRegionAvail().x, 560.0f);
                ImGui::TextUnformatted("Name");
                ImGui::SetNextItemWidth(w);
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                ImGui::InputTextWithHint("##name", "My Game", &m_Name);
                ImGui::Spacing();
                ImGui::TextUnformatted("Location");
                ImGui::SetNextItemWidth(w - 90.0f);
                ImGui::InputText("##location", &m_Location);
                ImGui::SameLine();
                if (ImGui::Button("Browse...", ImVec2(82.0f, 0.0f)))
                    if (auto dir = FileDialogs::PickFolder(m_Location)) m_Location = dir->string();
                ImGui::Spacing();
                ImGui::TextUnformatted("Start with");
                for (int i = 0; i < (int)m_Starters.size(); ++i)
                {
                    ImGui::PushID(i);
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImVec2 size(w, 42.0f);
                    if (ImGui::InvisibleButton("##starter", size)) m_Starter = i;
                    bool hot = ImGui::IsItemHovered(), on = m_Starter == i;
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), on ? Accent(0.3f) : hot ? IM_COL32(255, 255, 255, 20) : IM_COL32(255, 255, 255, 8), 5.0f);
                    if (on) dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), Accent(), 5.0f, 0, 1.5f);
                    dl->AddText(ImVec2(pos.x + 12.0f, pos.y + 5.0f), IM_COL32(235, 235, 240, 255), m_Starters[(size_t)i].first.c_str());
                    std::string desc = m_Starters[(size_t)i].second;
                    if (ImGui::CalcTextSize(desc.c_str()).x > size.x - 24.0f)
                    {
                        while (desc.size() > 4 && ImGui::CalcTextSize((desc + "...").c_str()).x > size.x - 24.0f) desc.pop_back();
                        desc += "...";
                    }
                    dl->AddText(ImVec2(pos.x + 12.0f, pos.y + 22.0f), IM_COL32(150, 150, 160, 255), desc.c_str());
                    if (hot) ImGui::SetTooltip("%s", m_Starters[(size_t)i].second.c_str());
                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(0.0f, 2.0f));
                }
                ImGui::Spacing();
                std::string name = m_Name;
                name.erase(0, name.find_first_not_of(' '));
                while (!name.empty() && name.back() == ' ') name.pop_back();
                bool bad = name.find_first_of("\\/:*?\"<>|") != std::string::npos;
                fs::path target = fs::path(m_Location) / name;
                bool taken = !name.empty() && !bad && (ProjectHub::IsProject(target) || (fs::exists(target) && !fs::is_empty(target)));
                if (!name.empty()) ImGui::TextDisabled("%s", target.string().c_str());
                if (bad) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "A name can't have \\ / : * ? \" < > |");
                else if (taken) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "That folder is already in use: pick another name or place.");
                ImGui::Spacing();
                ImGui::BeginDisabled(name.empty() || bad || taken || m_Location.empty() || m_Starters.empty());
                ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent());
                if (ImGui::Button("Create Project", ImVec2(160.0f, 32.0f)))
                    Finish({ target, true, name, m_Starters[(size_t)std::clamp(m_Starter, 0, (int)m_Starters.size() - 1)].first });
                ImGui::PopStyleColor(2);
                ImGui::EndDisabled();
            }
        };
    }

    std::optional<Choice> Run(uint64_t maxFrames, const std::string& screenshot)
    {
        std::optional<Choice> result;
        ApplicationSpec spec;
        spec.Name = "Dimension Launcher";
        spec.Window.Title = "Dimension";
        spec.Window.Width = 1000;
        spec.Window.Height = 620;
        spec.Docking = false;
        spec.MaxFrames = maxFrames;
        spec.ScreenshotPath = screenshot;
        {
            Application app(spec);
            app.PushLayer(MakeScope<LauncherLayer>(result));
            app.Run();
            app.PopAllLayers();
        }
        return result;
    }
}
