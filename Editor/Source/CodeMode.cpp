#include "CodeMode.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Platform/Platform.h>
#include <Dimension/Scripting/CSharpRuntime.h>
#include <Dimension/ImGui/ImGuiLayer.h>
#include <Dimension/Platform/FileDialogs.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scripting/CSharpProject.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace Dimension;
using Dimension::Code::Diagnostic;
using Dimension::Code::LexState;
using Dimension::Code::Severity;
using Dimension::Code::TextBuffer;
using Dimension::Code::TextPos;
using Dimension::Code::Token;
using Dimension::Code::TokenKind;

namespace Editor
{
    namespace
    {
        // Colours of the token kinds (dark and light themes).
        ImU32 TokenColor(TokenKind kind)
        {
            bool light = ImGuiLayer::IsLightTheme();
            switch (kind)
            {
                case TokenKind::Keyword: return light ? IM_COL32(0, 0, 255, 255) : IM_COL32(86, 156, 214, 255);
                case TokenKind::ControlKeyword: return light ? IM_COL32(143, 8, 196, 255) : IM_COL32(197, 134, 192, 255);
                case TokenKind::TypeKeyword: return light ? IM_COL32(0, 0, 255, 255) : IM_COL32(86, 156, 214, 255);
                case TokenKind::TypeName: return light ? IM_COL32(38, 127, 153, 255) : IM_COL32(78, 201, 176, 255);
                case TokenKind::Method: return light ? IM_COL32(121, 94, 38, 255) : IM_COL32(220, 220, 170, 255);
                case TokenKind::Identifier: return light ? IM_COL32(0, 16, 128, 255) : IM_COL32(156, 220, 254, 255);
                case TokenKind::Number: return light ? IM_COL32(9, 134, 88, 255) : IM_COL32(181, 206, 168, 255);
                case TokenKind::String:
                case TokenKind::Char: return light ? IM_COL32(163, 21, 21, 255) : IM_COL32(206, 145, 120, 255);
                case TokenKind::Comment: return light ? IM_COL32(0, 128, 0, 255) : IM_COL32(106, 153, 85, 255);
                case TokenKind::DocComment: return light ? IM_COL32(0, 128, 0, 255) : IM_COL32(96, 139, 78, 255);
                case TokenKind::Preprocessor: return light ? IM_COL32(128, 128, 128, 255) : IM_COL32(155, 155, 155, 255);
                case TokenKind::Operator:
                case TokenKind::Punctuation: return light ? IM_COL32(40, 40, 40, 255) : IM_COL32(212, 212, 212, 255);
                default: return light ? IM_COL32(30, 30, 30, 255) : IM_COL32(212, 212, 212, 255);
            }
        }

        std::string ToUtf8(unsigned int c)
        {
            std::string out;
            if (c < 0x80) out += (char)c;
            else if (c < 0x800)
            {
                out += (char)(0xC0 | (c >> 6));
                out += (char)(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000)
            {
                out += (char)(0xE0 | (c >> 12));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }
            else
            {
                out += (char)(0xF0 | (c >> 18));
                out += (char)(0x80 | ((c >> 12) & 0x3F));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }
            return out;
        }

        bool ReadFile(const fs::path& path, std::string& text)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::stringstream ss;
            ss << in.rdbuf();
            text = ss.str();
            if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
                text.erase(0, 3); // (a UTF-8 byte order mark)
            return true;
        }

        // A C# identifier from a file or project name ("my script" -> MyScript).
        std::string Identifier(const std::string& name)
        {
            std::string out;
            bool upper = true;
            for (char c : name)
            {
                if (std::isalnum((unsigned char)c) || c == '_')
                {
                    out += upper ? (char)std::toupper((unsigned char)c) : c;
                    upper = false;
                }
                else upper = true;
            }
            if (out.empty() || std::isdigit((unsigned char)out[0])) out = "_" + out;
            return out;
        }

        // The pair a typed opener closes with ('\0': none).
        // A path to compare files by (absolute, normalised, one case).
        std::string PathKey(const fs::path& p)
        {
            std::error_code ec;
            fs::path abs = fs::absolute(p, ec);
            std::string key = (ec ? p : abs).lexically_normal().generic_string();
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return key;
        }

        bool IsWordByte(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

        ImU32 SeverityColor(Severity level)
        {
            bool light = ImGuiLayer::IsLightTheme();
            return level == Severity::Error ? (light ? IM_COL32(220, 30, 30, 255) : IM_COL32(244, 71, 71, 255))
                                            : (light ? IM_COL32(190, 130, 0, 255) : IM_COL32(220, 180, 60, 255));
        }

        char Closer(char c)
        {
            switch (c)
            {
                case '(': return ')';
                case '[': return ']';
                case '{': return '}';
                case '"': return '"';
                case '\'': return '\'';
                default: return '\0';
            }
        }
    }

    CodeMode::~CodeMode() = default;

    fs::path CodeMode::ScriptsDir() const { return CSharpProject::ScriptsDirectory(Project::GetDirectory()); }

    // ---- Entering and leaving

    bool CodeMode::Open(const fs::path& file)
    {
        if (!m_Active)
        {
            m_Active = true;
            if (m_Docs.empty()) LoadSession();
        }
        if (!file.empty() && !OpenFile(file)) return false;
        if (m_Docs.empty())
        {
            // Nothing open yet: the first script there is.
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(ScriptsDir(), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_directory() && (it->path().filename() == "bin" || it->path().filename() == "obj")) it.disable_recursion_pending();
                else if (it->path().extension() == ".cs")
                {
                    OpenFile(it->path());
                    break;
                }
            }
        }
        m_FocusCode = true;
        return true;
    }

    void CodeMode::Close()
    {
        if (!m_Active) return;
        SaveAll();
        SaveSession();
        m_Active = false;
        m_Dragging = false;
    }

    bool CodeMode::SaveAll()
    {
        bool ok = true;
        for (auto& doc : m_Docs)
            if (doc->Buffer.IsDirty()) ok &= Save(*doc);
        return ok;
    }

    // ---- Files

    bool CodeMode::OpenFile(const fs::path& file)
    {
        std::error_code ec;
        fs::path path = fs::weakly_canonical(file, ec);
        if (ec) path = file;
        for (size_t i = 0; i < m_Docs.size(); ++i)
            if (fs::equivalent(m_Docs[i]->Path, path, ec))
            {
                m_Current = (int)i;
                m_SelectTab = (int)i;
                m_FocusCode = true;
                return true;
            }
        std::string text;
        if (!ReadFile(path, text))
        {
            Log::Error("Could not open '{}'", path.string());
            return false;
        }
        auto doc = std::make_unique<Document>();
        doc->Path = path;
        doc->Buffer.SetText(text);
        doc->DiskTime = fs::last_write_time(path, ec);
        AttachMarks(*doc);
        m_Docs.push_back(std::move(doc));
        m_Current = (int)m_Docs.size() - 1;
        m_SelectTab = m_Current;
        m_FocusCode = true;
        return true;
    }

    bool CodeMode::Save(Document& doc)
    {
        std::ofstream out(doc.Path, std::ios::binary);
        out << doc.Buffer.GetText();
        out.close();
        if (!out)
        {
            Log::Error("Could not save '{}'", doc.Path.string());
            return false;
        }
        doc.Buffer.MarkSaved();
        std::error_code ec;
        doc.DiskTime = fs::last_write_time(doc.Path, ec);
        doc.ChangedOnDisk = false;
        return true;
    }

    void CodeMode::ReloadFromDisk(Document& doc)
    {
        std::string text;
        if (!ReadFile(doc.Path, text)) return;
        TextPos cursor = doc.Buffer.Cursor();
        DetachMarks(doc);
        doc.Buffer.SetText(text);
        doc.Buffer.SetCursor(cursor);
        AttachMarks(doc);
        std::error_code ec;
        doc.DiskTime = fs::last_write_time(doc.Path, ec);
        doc.ChangedOnDisk = false;
    }

    void CodeMode::CloseTab(int index)
    {
        if (index < 0 || index >= (int)m_Docs.size()) return;
        DetachMarks(*m_Docs[(size_t)index]);
        m_Docs.erase(m_Docs.begin() + index);
        if (m_Current >= (int)m_Docs.size()) m_Current = (int)m_Docs.size() - 1;
        else if (m_Current > index) --m_Current;
        m_SelectTab = m_Current;
    }

    void CodeMode::CheckDisk(float dt)
    {
        // Changed by something else (another editor, version control): taken in when not edited here, else asked.
        m_DiskTimer += dt;
        if (m_DiskTimer < 1.0f) return;
        m_DiskTimer = 0.0f;
        for (size_t i = 0; i < m_Docs.size();)
        {
            Document& doc = *m_Docs[i];
            std::error_code ec;
            if (!fs::exists(doc.Path, ec))
            {
                if (!doc.Buffer.IsDirty())
                {
                    CloseTab((int)i); // (deleted)
                    continue;
                }
                ++i;
                continue;
            }
            auto time = fs::last_write_time(doc.Path, ec);
            if (!ec && time != doc.DiskTime)
            {
                if (doc.Buffer.IsDirty()) doc.ChangedOnDisk = true;
                else ReloadFromDisk(doc);
            }
            ++i;
        }
    }

    void CodeMode::SaveSession()
    {
        // The open files (relative to the project) and the one in front, for next time.
        nlohmann::json j;
        fs::path project = Project::GetDirectory();
        for (auto& doc : m_Docs)
        {
            std::error_code ec;
            fs::path rel = fs::relative(doc->Path, project, ec);
            j["files"].push_back((ec || rel.empty() ? doc->Path : rel).generic_string());
        }
        j["current"] = m_Current;
        std::error_code ec;
        fs::create_directories(project / ".dimension", ec);
        std::ofstream(project / ".dimension" / "CodeMode.json") << j.dump(2);
    }

    void CodeMode::LoadSession()
    {
        std::ifstream in(Project::GetDirectory() / ".dimension" / "CodeMode.json");
        if (!in) return;
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) return;
        for (const auto& f : j.value("files", nlohmann::json::array()))
        {
            fs::path p = f.get<std::string>();
            if (p.is_relative()) p = Project::GetDirectory() / p;
            std::error_code ec;
            if (fs::exists(p, ec)) OpenFile(p);
        }
        int current = j.value("current", 0);
        if (current >= 0 && current < (int)m_Docs.size()) m_Current = m_SelectTab = current;
    }

    // ---- The window

    void CodeMode::OnImGui()
    {
        CheckDisk(ImGui::GetIO().DeltaTime);
        UpdateLive();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking;
        ImGui::Begin("##CodeMode", nullptr, flags);
        ImGui::PopStyleVar(3);

        DrawTopBar();
        float status = ImGui::GetFrameHeight() + 2.0f;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        avail.y -= status;
        float problems = m_ShowProblems ? std::round(std::clamp(avail.y * 0.25f, ImGui::GetTextLineHeightWithSpacing() * 6.0f, 260.0f)) : 0.0f;
        avail.y -= problems;
        float explorer = std::clamp(avail.x * 0.18f, 180.0f, 320.0f);
        DrawExplorer(ImVec2(explorer, avail.y));
        ImGui::SameLine();
        DrawDocuments(ImVec2(0.0f, avail.y));
        if (m_ShowProblems) DrawProblems(ImVec2(0.0f, problems - ImGui::GetStyle().ItemSpacing.y));
        DrawStatusBar();
        DrawPopups();
        ImGui::End();
    }

    void CodeMode::DrawTopBar()
    {
        if (ImGui::Button("< Editor")) m_Ctx.RequestCloseCode = true;
        ImGui::SetItemTooltip("Back to the scene (saves your scripts)");
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        Document* doc = Current();
        if (ImGui::Button("Save") && doc) Save(*doc);
        ImGui::SetItemTooltip("Ctrl+S");
        ImGui::SameLine();
        if (ImGui::Button("Save All")) SaveAll();
        ImGui::SetItemTooltip("Ctrl+Shift+S");
        ImGui::SameLine();
        int state = BuildState ? BuildState() : 0;
        ImGui::BeginDisabled(state == 1 || !BuildScripts);
        if (ImGui::Button("Build"))
        {
            SaveAll();
            BuildScripts();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Save everything and compile the scripts (Ctrl+B). Saving builds them too.");
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Button("Find")) m_FindOpen = m_FocusFind = true;
        ImGui::SetItemTooltip("Ctrl+F (Ctrl+H to replace)");
        ImGui::SameLine();
        ImGui::TextDisabled("  Code Mode: %s", Project::GetConfig().Name.c_str());
    }

    // ---- Explorer

    void CodeMode::DrawExplorer(const ImVec2& size)
    {
        ImGui::BeginChild("##explorer", size, ImGuiChildFlags_Borders);
        ImGui::TextDisabled("SCRIPTS");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("+").x - 8.0f);
        if (ImGui::SmallButton("+"))
        {
            m_FileAction = FileAction::NewScript;
            m_ActionPath = ScriptsDir();
            m_ActionName = "NewScript";
            m_OpenActionPopup = true;
        }
        ImGui::SetItemTooltip("New C# script");
        ImGui::Separator();
        fs::path dir = ScriptsDir();
        std::error_code ec;
        if (!fs::exists(dir, ec)) ImGui::TextDisabled("No Scripts folder yet.\nAdd a script with +.");
        else DrawTree(dir);
        // Right-click on empty space: new things in the Scripts folder.
        if (ImGui::BeginPopupContextWindow("##explorerctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::MenuItem("New C# Script..."))
            {
                m_FileAction = FileAction::NewScript;
                m_ActionPath = dir;
                m_ActionName = "NewScript";
                m_OpenActionPopup = true;
            }
            if (ImGui::MenuItem("New Folder..."))
            {
                m_FileAction = FileAction::NewFolder;
                m_ActionPath = dir;
                m_ActionName = "NewFolder";
                m_OpenActionPopup = true;
            }
            if (ImGui::MenuItem("Show in Explorer")) FileDialogs::OpenWithDefaultApp(dir);
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    void CodeMode::DrawTree(const fs::path& dir)
    {
        // Folders first, then .cs files, by name (build output left out).
        std::vector<fs::path> folders, files;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            std::string name = entry.path().filename().string();
            if (entry.is_directory())
            {
                if (name != "bin" && name != "obj" && name[0] != '.') folders.push_back(entry.path());
            }
            else if (entry.path().extension() == ".cs") files.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end());
        std::sort(files.begin(), files.end());
        Document* current = Current();
        auto itemMenu = [&](const fs::path& path, bool folder) {
            if (!ImGui::BeginPopupContextItem()) return;
            if (folder)
            {
                if (ImGui::MenuItem("New C# Script..."))
                {
                    m_FileAction = FileAction::NewScript;
                    m_ActionPath = path;
                    m_ActionName = "NewScript";
                    m_OpenActionPopup = true;
                }
                if (ImGui::MenuItem("New Folder..."))
                {
                    m_FileAction = FileAction::NewFolder;
                    m_ActionPath = path;
                    m_ActionName = "NewFolder";
                    m_OpenActionPopup = true;
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Rename..."))
            {
                m_FileAction = FileAction::Rename;
                m_ActionPath = path;
                m_ActionName = folder ? path.filename().string() : path.stem().string();
                m_OpenActionPopup = true;
            }
            if (ImGui::MenuItem("Delete..."))
            {
                m_FileAction = FileAction::Delete;
                m_ActionPath = path;
                m_OpenActionPopup = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Show in Explorer")) FileDialogs::OpenWithDefaultApp(folder ? path : path.parent_path());
            ImGui::EndPopup();
        };
        for (const fs::path& f : folders)
        {
            bool open = ImGui::TreeNodeEx(f.string().c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen,
                                          "%s", f.filename().string().c_str());
            itemMenu(f, true);
            if (open)
            {
                DrawTree(f);
                ImGui::TreePop();
            }
        }
        for (const fs::path& f : files)
        {
            bool selected = current && fs::equivalent(current->Path, f, ec);
            bool dirty = false;
            for (auto& d : m_Docs)
                if (fs::equivalent(d->Path, f, ec)) dirty = d->Buffer.IsDirty();
            std::string label = f.filename().string() + (dirty ? " *" : "");
            bool errors = std::find(m_ErrorFiles.begin(), m_ErrorFiles.end(), PathKey(f)) != m_ErrorFiles.end();
            if (errors) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(SeverityColor(Severity::Error)));
            ImGui::TreeNodeEx(f.string().c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                      (selected ? ImGuiTreeNodeFlags_Selected : 0),
                              "%s", label.c_str());
            if (errors)
            {
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Has build errors");
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) OpenFile(f);
            itemMenu(f, false);
        }
    }

    // ---- Tabs

    void CodeMode::DrawDocuments(const ImVec2& size)
    {
        ImGui::BeginChild("##documents", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        if (m_Docs.empty())
        {
            ImGui::Dummy(ImVec2(0, 40));
            ImGui::Indent(30);
            ImGui::TextDisabled("No script open.");
            ImGui::TextDisabled("Pick one on the left, or make one with +.");
            ImGui::EndChild();
            return;
        }
        int close = -1;
        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_AutoSelectNewTabs))
        {
            for (size_t i = 0; i < m_Docs.size(); ++i)
            {
                Document& doc = *m_Docs[i];
                bool open = true;
                ImGuiTabItemFlags flags = doc.Buffer.IsDirty() ? ImGuiTabItemFlags_UnsavedDocument : 0;
                if (m_SelectTab == (int)i) flags |= ImGuiTabItemFlags_SetSelected;
                ImGui::PushID(doc.Path.string().c_str());
                bool visible = ImGui::BeginTabItem(doc.Path.filename().string().c_str(), &open, flags);
                ImGui::SetItemTooltip("%s", doc.Path.string().c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) open = false;
                if (visible)
                {
                    if (m_SelectTab < 0 || m_SelectTab == (int)i)
                    {
                        if (m_Current != (int)i) m_FocusCode = true;
                        m_Current = (int)i;
                    }
                    ImGui::EndTabItem();
                }
                ImGui::PopID();
                if (!open) close = (int)i;
            }
            ImGui::EndTabBar();
        }
        m_SelectTab = -1;
        if (close >= 0)
        {
            if (m_Docs[(size_t)close]->Buffer.IsDirty()) m_ConfirmClose = close;
            else CloseTab(close);
        }
        if (Document* doc = Current())
        {
            if (doc->ChangedOnDisk)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
                ImGui::TextUnformatted("This file changed on disk while you were editing it.");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::SmallButton("Reload (lose my changes)")) ReloadFromDisk(*doc);
                ImGui::SameLine();
                if (ImGui::SmallButton("Keep mine")) doc->ChangedOnDisk = false, doc->DiskTime = fs::last_write_time(doc->Path);
            }
            if (m_FindOpen) DrawFindBar(*doc);
            DrawCode(*doc, ImGui::GetContentRegionAvail());
        }
        ImGui::EndChild();
    }

    // ---- Find and replace

    void CodeMode::DrawFindBar(Document& doc)
    {
        ImGui::PushID("find");
        float width = 260.0f;
        ImGui::SetNextItemWidth(width);
        if (m_FocusFind)
        {
            ImGui::SetKeyboardFocusHere();
            m_FocusFind = false;
        }
        bool enter = ImGui::InputTextWithHint("##find", "Find", &m_Find, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter)
        {
            FindNext(doc, ImGui::GetIO().KeyShift);
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("<")) FindNext(doc, true);
        ImGui::SetItemTooltip("Previous (Shift+F3)");
        ImGui::SameLine();
        if (ImGui::SmallButton(">")) FindNext(doc, false);
        ImGui::SetItemTooltip("Next (F3)");
        ImGui::SameLine();
        ImGui::Checkbox("Aa", &m_FindOptions.CaseSensitive);
        ImGui::SetItemTooltip("Match case");
        ImGui::SameLine();
        ImGui::Checkbox("Word", &m_FindOptions.WholeWord);
        ImGui::SetItemTooltip("Whole words only");
        ImGui::SameLine();
        if (ImGui::SmallButton(m_ReplaceOpen ? "Hide Replace" : "Replace...")) m_ReplaceOpen = !m_ReplaceOpen;
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) m_FindOpen = false, m_FocusCode = true;
        if (!m_FindMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_FindMessage.c_str());
        }
        if (m_ReplaceOpen)
        {
            ImGui::SetNextItemWidth(width);
            ImGui::InputTextWithHint("##replace", "Replace with", &m_Replace);
            ImGui::SameLine();
            if (ImGui::SmallButton("Replace"))
            {
                // The selection when it is the match, then on to the next.
                std::string selected = doc.Buffer.SelectedText();
                bool same = m_FindOptions.CaseSensitive ? selected == m_Find
                                                        : selected.size() == m_Find.size() &&
                                                              std::equal(selected.begin(), selected.end(), m_Find.begin(), [](char a, char b) {
                                                                  return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
                                                              });
                if (doc.Buffer.HasSelection() && same) doc.Buffer.Insert(m_Replace);
                FindNext(doc, false);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Replace All"))
            {
                int n = doc.Buffer.ReplaceAll(m_Find, m_Replace, m_FindOptions);
                m_FindMessage = n ? std::to_string(n) + " replaced" : "No matches";
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        {
            m_FindOpen = false;
            m_FocusCode = true;
        }
        ImGui::PopID();
    }

    void CodeMode::FindNext(Document& doc, bool backwards)
    {
        TextPos a, b;
        TextPos from = backwards ? doc.Buffer.SelStart() : doc.Buffer.SelEnd();
        if (doc.Buffer.Find(m_Find, m_FindOptions, from, backwards, a, b))
        {
            doc.Buffer.SetSelection(a, b);
            doc.ScrollToCursor = true;
            m_FindMessage.clear();
        }
        else m_FindMessage = m_Find.empty() ? "" : "No matches";
    }

    // ---- Status bar and popups

    void CodeMode::DrawStatusBar()
    {
        ImGui::Separator();
        Document* doc = Current();
        if (doc)
        {
            const TextBuffer& b = doc->Buffer;
            TextPos c = b.Cursor();
            ImGui::Text("Ln %d, Col %d", c.Line + 1, b.VisualColumn(c.Line, c.Col) + 1);
            if (b.HasSelection())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%d selected)", (int)b.SelectedText().size());
            }
            ImGui::SameLine(0, 24);
            ImGui::TextDisabled("%s", b.IndentUnit == "\t" ? "Tabs" : "Spaces: 4");
            ImGui::SameLine(0, 24);
            ImGui::TextDisabled("%s", b.UsesCrlf() ? "CRLF" : "LF");
            ImGui::SameLine(0, 24);
            ImGui::TextDisabled("UTF-8");
            ImGui::SameLine(0, 24);
            ImGui::TextDisabled("%d%%", (int)std::lround(m_Zoom * 100.0f));
        }
        // The problem counts (click: the Problems list) and how the build went.
        int errors = 0, warnings = 0;
        for (const Problem& p : m_Problems) (p.D.Level == Severity::Error ? errors : warnings)++;
        char counts[64];
        std::snprintf(counts, sizeof(counts), "%d error%s, %d warning%s", errors, errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s");
        int state = BuildState ? BuildState() : 0;
        if (state == 0 && m_HaveBuildResult) state = m_LastBuildOk ? 2 : 3; // (the editor forgets it once taken in)
        const char* text = state == 1 ? "Building..." : state == 2 ? "Build succeeded" : state == 3 ? "Build failed" : "";
        if (m_Language && state != 1)
        {
            ImGui::SameLine(0, 24);
            if (m_Language->Busy()) ImGui::TextDisabled("Checking...");
            else if (!m_LiveError.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Live checking failed");
                ImGui::SetItemTooltip("%s", m_LiveError.c_str());
            }
            else if (m_LiveMs >= 0.0)
            {
                ImGui::TextDisabled("Checked live");
                ImGui::SetItemTooltip("Errors are found as you type (the last check took %.0f ms)", m_LiveMs);
            }
        }
        float right = ImGui::GetWindowContentRegionMax().x;
        float textW = *text ? ImGui::CalcTextSize(text).x : 0.0f;
        float countsW = ImGui::CalcTextSize(counts).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(right - textW - (*text ? 24.0f : 0.0f) - countsW);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (errors) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(SeverityColor(Severity::Error)));
        if (ImGui::SmallButton(counts)) m_ShowProblems = !m_ShowProblems;
        if (errors) ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Problems from the last build (F8: next, Shift+F8: previous)");
        if (*text)
        {
            ImGui::SameLine(right - textW);
            ImVec4 col = state == 3 ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : state == 2 ? ImVec4(0.5f, 0.85f, 0.5f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
            ImGui::TextColored(col, "%s", text);
        }
    }

    void CodeMode::DrawPopups()
    {
        // A tab with unsaved changes being closed.
        if (m_ConfirmClose >= 0)
        {
            ImGui::OpenPopup("Unsaved Changes##code");
            if (ImGui::BeginPopupModal("Unsaved Changes##code", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                Document* doc = m_ConfirmClose < (int)m_Docs.size() ? m_Docs[(size_t)m_ConfirmClose].get() : nullptr;
                ImGui::Text("Save the changes to %s?", doc ? doc->Path.filename().string().c_str() : "?");
                if (ImGui::Button("Save") && doc)
                {
                    if (Save(*doc)) CloseTab(m_ConfirmClose);
                    m_ConfirmClose = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Don't Save"))
                {
                    CloseTab(m_ConfirmClose);
                    m_ConfirmClose = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    m_ConfirmClose = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // New script / folder, rename, delete.
        if (m_OpenActionPopup)
        {
            ImGui::OpenPopup("##fileaction");
            m_OpenActionPopup = false;
        }
        if (ImGui::BeginPopupModal("##fileaction", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            bool done = false;
            std::error_code ec;
            if (m_FileAction == FileAction::Delete)
            {
                ImGui::Text("Delete %s?", m_ActionPath.filename().string().c_str());
                ImGui::TextDisabled(fs::is_directory(m_ActionPath, ec) ? "The folder and everything in it are deleted." : "This can't be undone.");
                if (ImGui::Button("Delete"))
                {
                    for (size_t i = m_Docs.size(); i-- > 0;)
                    {
                        fs::path rel = fs::relative(m_Docs[i]->Path, m_ActionPath, ec);
                        if (fs::equivalent(m_Docs[i]->Path, m_ActionPath, ec) || (!rel.empty() && *rel.begin() != ".."))
                            CloseTab((int)i);
                    }
                    fs::remove_all(m_ActionPath, ec);
                    if (ec) Log::Error("Could not delete '{}': {}", m_ActionPath.string(), ec.message());
                    done = true;
                }
            }
            else
            {
                const char* title = m_FileAction == FileAction::NewScript ? "New C# script" : m_FileAction == FileAction::NewFolder ? "New folder" : "Rename";
                ImGui::TextUnformatted(title);
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(280);
                bool enter = ImGui::InputText("##name", &m_ActionName, ImGuiInputTextFlags_EnterReturnsTrue);
                if (ImGui::Button("OK") || enter)
                {
                    std::string name = m_ActionName;
                    if (m_FileAction == FileAction::NewScript)
                    {
                        // A Behaviour named after the file, in the project's namespace.
                        std::string cls = Identifier(fs::path(name).stem().string());
                        fs::path file = m_ActionPath / (cls + ".cs");
                        if (fs::exists(file, ec)) Log::Error("'{}' already exists", file.filename().string());
                        else
                        {
                            fs::create_directories(m_ActionPath, ec);
                            std::ofstream out(file, std::ios::binary);
                            out << "using System.Numerics;\nusing Dimension;\n\nnamespace " << Identifier(Project::GetConfig().Name) << "\n{\n"
                                << "    public class " << cls << " : Behaviour\n    {\n"
                                << "        public override void OnCreate()\n        {\n        }\n\n"
                                << "        public override void OnUpdate(float deltaTime)\n        {\n        }\n    }\n}\n";
                            out.close();
                            OpenFile(file);
                            if (Document* doc = Current()) doc->Buffer.SetCursor({ 9, 8 }); // (in OnCreate)
                        }
                    }
                    else if (m_FileAction == FileAction::NewFolder)
                        fs::create_directories(m_ActionPath / name, ec);
                    else
                    {
                        bool folder = fs::is_directory(m_ActionPath, ec);
                        fs::path to = m_ActionPath.parent_path() / (folder ? name : name + ".cs");
                        fs::rename(m_ActionPath, to, ec);
                        if (ec) Log::Error("Could not rename '{}': {}", m_ActionPath.filename().string(), ec.message());
                        else
                            for (auto& d : m_Docs)
                            {
                                // Open files follow (the file itself, or ones in the folder).
                                fs::path rel = fs::relative(d->Path, m_ActionPath, ec);
                                if (fs::equivalent(d->Path, to, ec)) continue;
                                if (!folder && d->Path == m_ActionPath) d->Path = to;
                                else if (folder && !rel.empty() && *rel.begin() != "..") d->Path = to / rel;
                                d->DiskTime = fs::last_write_time(d->Path, ec);
                            }
                    }
                    done = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
            if (done)
            {
                m_FileAction = FileAction::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ---- The code view

    void CodeMode::UpdateStates(Document& doc)
    {
        if (doc.StatesVersion == doc.Buffer.Version()) return;
        const auto& lines = doc.Buffer.Lines();
        doc.States.resize(lines.size() + 1);
        doc.States[0] = {};
        std::vector<Token> scratch;
        int longest = 0;
        for (size_t l = 0; l < lines.size(); ++l)
        {
            scratch.clear();
            doc.States[l + 1] = Code::LexLine(lines[l], doc.States[l], scratch);
            longest = std::max(longest, doc.Buffer.VisualColumn((int)l, (int)lines[l].size()));
        }
        doc.LongestLine = longest;
        doc.StatesVersion = doc.Buffer.Version();
    }

    void CodeMode::TypeText(Document& doc, const std::string& text)
    {
        TextBuffer& b = doc.Buffer;
        const std::string& line = b.Line(b.Cursor().Line);
        char next = b.Cursor().Col < (int)line.size() ? line[(size_t)b.Cursor().Col] : '\0';
        if (text.size() == 1 && !b.HasSelection())
        {
            char c = text[0];
            // Typing a closer that is already there steps over it.
            if ((c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') && next == c)
            {
                b.MoveRight(false, false);
                return;
            }
            // A '}' on a line of blanks goes one level out.
            if (c == '}' && line.find_first_not_of(" \t") == std::string::npos && !line.empty())
            {
                b.OutdentLines();
                b.Type("}");
                return;
            }
            // Openers close themselves before blanks, line ends and closers (quotes not after a word character).
            char closer = Closer(c);
            bool roomy = next == '\0' || next == ' ' || next == '\t' || next == ')' || next == ']' || next == '}' || next == ';' || next == ',';
            char before = b.Cursor().Col > 0 ? line[(size_t)b.Cursor().Col - 1] : ' ';
            bool quote = c == '"' || c == '\'';
            if (closer && roomy && !(quote && (std::isalnum((unsigned char)before) || before == '_' || before == '"' || before == '\'')))
            {
                b.Type(std::string(1, c) + closer);
                b.MoveLeft(false, false);
                return;
            }
        }
        // A selection with an opener typed over it: wrapped instead.
        if (text.size() == 1 && b.HasSelection() && Closer(text[0]) && b.SelStart().Line == b.SelEnd().Line)
        {
            std::string inner = b.SelectedText();
            TextPos start = b.SelStart();
            b.Insert(text + inner + Closer(text[0]));
            b.SetSelection({ start.Line, start.Col + 1 }, { start.Line, start.Col + 1 + (int)inner.size() });
            return;
        }
        b.Type(text);
    }

    void CodeMode::NewLine(Document& doc)
    {
        // The line's indentation again, a level more after an opening brace, and a brace pair split around a new line.
        TextBuffer& b = doc.Buffer;
        TextPos c = b.SelStart();
        std::string indent = b.IndentOf(c.Line);
        const std::string& line = b.Line(c.Line);
        size_t last = line.find_last_not_of(" \t", c.Col > 0 ? (size_t)c.Col - 1 : std::string::npos);
        bool opened = c.Col > 0 && last != std::string::npos && last < (size_t)c.Col && (line[last] == '{' || line[last] == '(' || line[last] == '[');
        size_t after = line.find_first_not_of(" \t", (size_t)c.Col);
        bool closes = after != std::string::npos && (line[after] == '}' || line[after] == ')' || line[after] == ']');
        if (opened && closes && !b.HasSelection())
        {
            // { | }  ->  {\n    |\n}
            b.Replace(c, { c.Line, (int)after }, "\n" + indent + b.IndentUnit + "\n" + indent);
            b.SetCursor({ c.Line + 1, (int)(indent.size() + b.IndentUnit.size()) });
            return;
        }
        b.Type("\n" + indent + (opened ? b.IndentUnit : ""));
        // (blanks the cursor had after it would now start the line: dropped)
        TextPos now = b.Cursor();
        const std::string& rest = b.Line(now.Line);
        size_t blanks = rest.find_first_not_of(" \t", (size_t)now.Col);
        if (blanks == std::string::npos) blanks = rest.size();
        if (blanks > (size_t)now.Col)
        {
            b.SetSelection(now, { now.Line, (int)blanks });
            b.DeleteSelection();
        }
    }

    void CodeMode::HandleKeys(Document& doc, int pageLines)
    {
        ImGuiIO& io = ImGui::GetIO();
        TextBuffer& b = doc.Buffer;
        bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
        auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
        bool moved = false, edited = false;
        uint64_t version = b.Version();
        TextPos before = b.Cursor(), anchorBefore = b.Anchor();
        if (AssistKeys(doc))
        {
            io.InputQueueCharacters.resize(0);
            return;
        }

        if (ctrl && !alt)
        {
            if (pressed(ImGuiKey_S))
            {
                if (shift) SaveAll();
                else Save(doc);
            }
            else if (pressed(ImGuiKey_Z)) shift ? b.Redo() : b.Undo();
            else if (pressed(ImGuiKey_Y)) b.Redo();
            else if (pressed(ImGuiKey_A)) b.SelectAll();
            else if (pressed(ImGuiKey_C) || pressed(ImGuiKey_X) || pressed(ImGuiKey_Insert))
            {
                // Without a selection: the whole line (as Visual Studio does).
                bool cut = ImGui::IsKeyPressed(ImGuiKey_X);
                if (b.HasSelection()) ImGui::SetClipboardText(b.SelectedText().c_str());
                else ImGui::SetClipboardText((b.Line(b.Cursor().Line) + "\n").c_str());
                if (cut)
                {
                    if (b.HasSelection()) b.DeleteSelection();
                    else b.DeleteLines();
                }
            }
            else if (pressed(ImGuiKey_V))
            {
                if (const char* clip = ImGui::GetClipboardText()) b.Insert(clip);
            }
            else if (pressed(ImGuiKey_Slash)) b.ToggleLineComment();
            else if (pressed(ImGuiKey_D)) b.DuplicateLines();
            else if (shift && pressed(ImGuiKey_K)) b.DeleteLines();
            else if (pressed(ImGuiKey_F) || pressed(ImGuiKey_H))
            {
                m_FindOpen = m_FocusFind = true;
                m_ReplaceOpen = ImGui::IsKeyPressed(ImGuiKey_H);
                if (b.HasSelection() && b.SelStart().Line == b.SelEnd().Line) m_Find = b.SelectedText();
            }
            else if (pressed(ImGuiKey_B))
            {
                SaveAll();
                if (BuildScripts) BuildScripts();
            }
            else if (pressed(ImGuiKey_W))
            {
                if (b.IsDirty()) m_ConfirmClose = m_Current;
                else CloseTab(m_Current);
                return;
            }
            else if (pressed(ImGuiKey_Space))
            {
                if (shift) RequestSignature(doc);
                else RequestCompletion(doc, true);
            }
            else if (pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd)) m_Zoom = std::min(m_Zoom + 0.1f, 3.0f);
            else if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) m_Zoom = std::max(m_Zoom - 0.1f, 0.5f);
            else if (pressed(ImGuiKey_0)) m_Zoom = 1.0f;
        }
        if (alt && !ctrl && (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow)))
            b.MoveLines(ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 1);
        else if (!alt)
        {
            if (pressed(ImGuiKey_LeftArrow)) b.MoveLeft(shift, ctrl), moved = true;
            else if (pressed(ImGuiKey_RightArrow)) b.MoveRight(shift, ctrl), moved = true;
            else if (pressed(ImGuiKey_UpArrow) && !ctrl) b.MoveVertical(-1, shift), moved = true;
            else if (pressed(ImGuiKey_DownArrow) && !ctrl) b.MoveVertical(1, shift), moved = true;
            else if (pressed(ImGuiKey_PageUp)) b.MoveVertical(-pageLines, shift), moved = true;
            else if (pressed(ImGuiKey_PageDown)) b.MoveVertical(pageLines, shift), moved = true;
            else if (pressed(ImGuiKey_Home)) ctrl ? b.MoveToStart(shift) : b.MoveHome(shift), moved = true;
            else if (pressed(ImGuiKey_End)) ctrl ? b.MoveToEnd(shift) : b.MoveEnd(shift), moved = true;
            else if (pressed(ImGuiKey_Backspace))
            {
                // Between a pair just opened: both go.
                TextPos c = b.Cursor();
                const std::string& line = b.Line(c.Line);
                if (!b.HasSelection() && !ctrl && c.Col > 0 && c.Col < (int)line.size() && Closer(line[(size_t)c.Col - 1]) == line[(size_t)c.Col])
                    b.Replace({ c.Line, c.Col - 1 }, { c.Line, c.Col + 1 }, "");
                else b.Backspace(ctrl);
            }
            else if (pressed(ImGuiKey_Delete)) b.Delete(ctrl);
            else if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) NewLine(doc);
            else if (pressed(ImGuiKey_Tab))
            {
                if (shift) b.OutdentLines();
                else if (b.HasSelection() && b.SelStart().Line != b.SelEnd().Line) b.IndentLines();
                else
                {
                    // To the next indent stop.
                    if (b.IndentUnit == "\t") b.Type("\t");
                    else
                    {
                        int visual = b.VisualColumn(b.SelStart().Line, b.SelStart().Col);
                        b.Type(std::string((size_t)(b.TabSize - visual % b.TabSize), ' '));
                    }
                }
            }
            else if (pressed(ImGuiKey_F3))
            {
                if (m_Find.empty() && b.HasSelection()) m_Find = b.SelectedText();
                FindNext(doc, shift);
            }
            else if (pressed(ImGuiKey_F8)) NextProblem(shift);
            else if (pressed(ImGuiKey_Escape) && b.HasSelection()) b.SetCursor(b.Cursor()), moved = true;
        }

        // Typed characters (not with Ctrl, which makes shortcuts; AltGr arrives as Ctrl+Alt and types).
        if (!(ctrl && !alt))
        {
            std::string typed;
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
            {
                unsigned int c = io.InputQueueCharacters[i];
                if (c >= 32 && c != 127) typed += ToUtf8(c);
            }
            if (!typed.empty())
            {
                // A character that ends a name ("(", ".", ";") takes the list's choice first, when the word starts it.
                if (typed.size() == 1 && (typed[0] == '(' || typed[0] == '.' || typed[0] == ';') && m_Completion.Open && !m_Completion.Shown.empty() &&
                    m_Completion.Scores[(size_t)m_Completion.Selected] <= 1 && m_Completion.Items[(size_t)m_Completion.Shown[(size_t)m_Completion.Selected]].Insert.empty() &&
                    b.Cursor().Col > m_Completion.StartCol)
                    AcceptCompletion(doc);
                if (typed.size() == 1) TypeText(doc, typed);
                else b.Type(typed);
                TypedAssist(doc, typed);
            }
        }
        io.InputQueueCharacters.resize(0);

        edited = b.Version() != version;
        if (moved || edited || b.Cursor() != before || b.Anchor() != anchorBefore)
        {
            doc.ScrollToCursor = true;
            m_BlinkStart = ImGui::GetTime();
        }
    }

    void CodeMode::DrawCode(Document& doc, const ImVec2& size)
    {
        TextBuffer& b = doc.Buffer;
        UpdateStates(doc);
        ImFont* font = ImGuiLayer::GetMonoFont();
        float fontSize = std::round(15.0f * m_Zoom);
        if (font) ImGui::PushFont(font, fontSize);
        else ImGui::PushFont(nullptr, fontSize);
        ImFont* drawFont = ImGui::GetFont();
        float charW = drawFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "M").x;
        float lineH = std::round(fontSize * 1.4f);
        int lines = b.LineCount();
        int digits = std::max(3, (int)std::to_string(lines).size());
        float gutter = (float)(digits + 3) * charW;
        float pad = 6.0f;

        if (m_FocusCode)
        {
            ImGui::SetNextWindowFocus();
            m_FocusCode = false;
        }
        bool light = ImGuiLayer::IsLightTheme();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, light ? IM_COL32(255, 255, 255, 255) : IM_COL32(30, 30, 30, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##code", size, ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleVar();
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImRect inner = window->InnerRect;
        float viewH = inner.GetHeight(), viewW = inner.GetWidth();
        int pageLines = std::max(1, (int)(viewH / lineH) - 1);
        bool focused = ImGui::IsWindowFocused();

        // Room for every line (and to scroll the last one up to the top).
        ImVec2 origin = ImGui::GetCursorScreenPos(); // the content's top-left (scrolled)
        ImGui::Dummy(ImVec2(gutter + pad + (float)(doc.LongestLine + 8) * charW, (float)lines * lineH + std::max(0.0f, viewH - lineH * 2.0f)));
        ImVec2 text = ImVec2(origin.x + gutter + pad, origin.y);

        // Keyboard.
        if (focused)
        {
            ImGui::SetNextFrameWantCaptureKeyboard(true);
            ImGui::GetIO().WantTextInput = true;
            HandleKeys(doc, pageLines);
        }
        if (focused && ImGui::GetIO().KeyCtrl && ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
        {
            m_Zoom = std::clamp(m_Zoom + ImGui::GetIO().MouseWheel * 0.1f, 0.5f, 3.0f);
            ImGui::SetScrollY(window->Scroll.y); // (the wheel zooms instead of scrolling)
        }

        // Mouse: click to place, drag to select, double-click a word, triple-click a line, Shift+click to extend;
        // in the line numbers, whole lines.
        ImVec2 mouse = ImGui::GetMousePos();
        auto posAt = [&](ImVec2 m) {
            int line = std::clamp((int)std::floor((m.y - text.y) / lineH), 0, lines - 1);
            int visual = std::max(0, (int)std::lround((m.x - text.x) / charW));
            return TextPos{ line, b.ColumnAtVisual(line, visual) };
        };
        bool overText = ImGui::IsWindowHovered() && inner.Contains(mouse);
        float gutterRight = inner.Min.x + gutter;
        if (overText && mouse.x > gutterRight) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (overText && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_Completion.Open = m_Completion.Waiting = false;
            TextPos at = posAt(mouse);
            int clicks = ImGui::GetIO().MouseClickedCount[0];
            if (mouse.x <= gutterRight)
            {
                m_DragLines = true;
                m_DragFirstLine = at.Line;
                b.SelectLines(at.Line, at.Line);
            }
            else if (clicks == 2) b.SelectWordAt(at);
            else if (clicks >= 3) b.SelectLines(at.Line, at.Line);
            else
            {
                b.SetCursor(at, ImGui::GetIO().KeyShift);
                m_DragLines = false;
            }
            m_Dragging = clicks == 1 || mouse.x <= gutterRight;
            m_BlinkStart = ImGui::GetTime();
        }
        if (m_Dragging)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_Dragging = false;
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
            {
                TextPos at = posAt(mouse);
                if (m_DragLines) b.SelectLines(m_DragFirstLine, at.Line);
                else b.SetCursor(at, true);
                // Past the edges: scrolling along.
                if (mouse.y < inner.Min.y) ImGui::SetScrollY(window->Scroll.y - lineH);
                else if (mouse.y > inner.Max.y) ImGui::SetScrollY(window->Scroll.y + lineH);
                if (!m_DragLines && mouse.x < gutterRight) ImGui::SetScrollX(window->Scroll.x - charW * 2.0f);
                else if (mouse.x > inner.Max.x) ImGui::SetScrollX(window->Scroll.x + charW * 2.0f);
            }
        }

        // Keeping the cursor in view.
        TextPos cursor = b.Cursor();
        if (doc.ScrollToCursor)
        {
            doc.ScrollToCursor = false;
            float y = (float)cursor.Line * lineH;
            if (y < window->Scroll.y) ImGui::SetScrollY(y);
            else if (y + lineH > window->Scroll.y + viewH) ImGui::SetScrollY(y + lineH - viewH + lineH * 0.5f);
            float x = (float)b.VisualColumn(cursor.Line, cursor.Col) * charW;
            float textW = viewW - gutter - pad;
            if (x < window->Scroll.x) ImGui::SetScrollX(std::max(0.0f, x - charW * 4.0f));
            else if (x + charW > window->Scroll.x + textW) ImGui::SetScrollX(x - textW + charW * 4.0f);
        }

        // The visible lines.
        int first = std::max(0, (int)std::floor(window->Scroll.y / lineH));
        int last = std::min(lines - 1, first + (int)std::ceil(viewH / lineH) + 1);
        TextPos selA = b.SelStart(), selB = b.SelEnd();
        ImU32 selColor = light ? IM_COL32(173, 214, 255, 255) : IM_COL32(38, 79, 120, 255);
        ImU32 lineColor = light ? IM_COL32(0, 0, 0, 14) : IM_COL32(255, 255, 255, 12);
        ImU32 guideColor = light ? IM_COL32(0, 0, 0, 26) : IM_COL32(255, 255, 255, 22);
        std::vector<Token> tokens;
        for (int l = first; l <= last; ++l)
        {
            float y = text.y + (float)l * lineH;
            const std::string& s = b.Line(l);
            // Current line, selection.
            if (l == cursor.Line && !b.HasSelection())
                draw->AddRectFilled(ImVec2(inner.Min.x, y), ImVec2(inner.Max.x, y + lineH), lineColor);
            if (b.HasSelection() && l >= selA.Line && l <= selB.Line)
            {
                int a = l == selA.Line ? b.VisualColumn(l, selA.Col) : 0;
                int e = l == selB.Line ? b.VisualColumn(l, selB.Col) : b.VisualColumn(l, (int)s.size()) + 1; // (+ the line break)
                draw->AddRectFilled(ImVec2(text.x + (float)a * charW, y), ImVec2(text.x + (float)e * charW, y + lineH), selColor);
            }
            // Indent guides through the leading blanks (blank lines take the next line's).
            int indent = b.VisualColumn(l, (int)b.IndentOf(l).size());
            if (s.find_first_not_of(" \t") == std::string::npos)
                for (int k = l + 1; k < std::min(lines, l + 50); ++k)
                    if (b.Line(k).find_first_not_of(" \t") != std::string::npos)
                    {
                        indent = b.VisualColumn(k, (int)b.IndentOf(k).size());
                        break;
                    }
            for (int g = b.TabSize; g < indent; g += b.TabSize)
                draw->AddLine(ImVec2(text.x + (float)g * charW + 0.5f, y), ImVec2(text.x + (float)g * charW + 0.5f, y + lineH), guideColor);
            // The text, token by token (tabs drawn as their width of nothing).
            tokens.clear();
            Code::LexLine(s, doc.States[(size_t)l], tokens);
            float ty = y + (lineH - fontSize) * 0.5f;
            int visual = 0;
            for (const Token& t : tokens)
            {
                ImU32 color = TokenColor(t.Kind);
                int i = t.Start, end = t.Start + t.Length;
                while (i < end)
                {
                    if (s[(size_t)i] == '\t')
                    {
                        visual = (visual / b.TabSize + 1) * b.TabSize;
                        ++i;
                        continue;
                    }
                    int run = i, count = 0;
                    while (run < end && s[(size_t)run] != '\t')
                    {
                        run = Code::NextChar(s, run);
                        ++count;
                    }
                    draw->AddText(drawFont, fontSize, ImVec2(text.x + (float)visual * charW, ty), color, s.data() + i, s.data() + run);
                    visual += count;
                    i = run;
                }
            }
        }
        // The build's problems: a wavy line under each (a short one where it has no width), the message on hover.
        const Problem* hovered = nullptr;
        int hoverLine = (int)std::floor((mouse.y - text.y) / lineH);
        int hoverVisual = (int)std::floor((mouse.x - text.x) / charW);
        bool canHover = overText && mouse.x > gutterRight && !m_Dragging;
        std::vector<int> lineLevel((size_t)(last - first + 1), -1); // per visible line: the worst problem on it
        for (const TextBuffer::Mark& m : b.Marks)
        {
            if (m.Id < 0 || m.Id >= (int)m_Problems.size()) continue;
            const Problem& p = m_Problems[(size_t)m.Id];
            if ((p.D.Level == Severity::Error && !m_ShowErrors) || (p.D.Level == Severity::Warning && !m_ShowWarnings)) continue;
            if (m.End.Line < first || m.Start.Line > last) continue;
            ImU32 color = SeverityColor(p.D.Level);
            for (int l = std::max(first, m.Start.Line); l <= std::min(last, m.End.Line); ++l)
            {
                int& level = lineLevel[(size_t)(l - first)];
                level = level < 0 ? (int)p.D.Level : std::min(level, (int)p.D.Level);
                int va = l == m.Start.Line ? b.VisualColumn(l, m.Start.Col) : b.VisualColumn(l, (int)b.IndentOf(l).size());
                int vb = l == m.End.Line ? b.VisualColumn(l, m.End.Col) : b.VisualColumn(l, (int)b.Line(l).size());
                if (vb <= va) vb = va + 1;
                float x0 = text.x + (float)va * charW, x1 = text.x + (float)vb * charW;
                float y = text.y + (float)l * lineH + lineH - 3.0f;
                float wave = std::max(2.0f, std::round(charW * 0.35f));
                ImVec2 pts[256];
                int n = 0;
                for (float x = x0; n < 255; x += wave)
                {
                    pts[n++] = ImVec2(std::min(x, x1), y + ((n & 1) ? -1.5f : 1.0f));
                    if (x >= x1) break;
                }
                draw->AddPolyline(pts, n, color, ImDrawFlags_None, 1.2f);
                if (canHover && l == hoverLine && hoverVisual >= va && hoverVisual < vb && (!hovered || p.D.Level < hovered->D.Level)) hovered = &p;
            }
        }
        {
            int wordLine = -1, wordCol = -1;
            if (canHover && m_Language && hoverLine >= 0 && hoverLine < lines && hoverVisual >= 0)
            {
                const std::string& s = b.Line(hoverLine);
                int col = b.ColumnAtVisual(hoverLine, hoverVisual);
                if (col < (int)s.size() && b.VisualColumn(hoverLine, col) <= hoverVisual && IsWordByte(s[(size_t)col]))
                {
                    while (col > 0 && IsWordByte(s[(size_t)col - 1])) --col;
                    wordLine = hoverLine;
                    wordCol = col;
                }
            }
            double now = ImGui::GetTime();
            if (m_Hover.Doc != &doc || m_Hover.Line != wordLine || m_Hover.Col != wordCol)
            {
                m_Hover = HoverState{};
                m_Hover.Doc = &doc;
                m_Hover.Line = wordLine;
                m_Hover.Col = wordCol;
                m_Hover.Since = now;
            }
            if (wordLine >= 0 && !m_Hover.Asked && now - m_Hover.Since > 0.4 && !m_Completion.Open)
            {
                m_Hover.Asked = true;
                m_Hover.Tag = m_NextTag++;
                LanguageService::Question q{ LanguageService::Query::Hover, doc.Path, OverlaysFor(doc), wordLine,
                                             Code::ByteToUtf16(b.Line(wordLine), wordCol), m_Hover.Tag };
                m_Language->Send(std::move(q));
            }
        }
        bool hoverInfo = m_Hover.Have && !m_Hover.Text.empty() && m_Hover.Line >= 0 && !m_Completion.Open;
        if (hovered || hoverInfo)
        {
            ImGui::PopFont(); // (the tooltip in the interface's font)
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
            if (hovered)
            {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(SeverityColor(hovered->D.Level)), "%s %s", hovered->D.Level == Severity::Error ? "error" : "warning",
                                   hovered->D.Code.c_str());
                ImGui::TextUnformatted(hovered->D.Message.c_str());
            }
            if (hovered && hoverInfo) ImGui::Separator();
            if (hoverInfo)
            {
                ImGui::PushFont(font, std::round(fontSize * 0.93f));
                ImGui::TextUnformatted(m_Hover.Text.c_str());
                ImGui::PopFont();
                if (!m_Hover.Info.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextUnformatted(m_Hover.Info.c_str());
                }
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
            ImGui::PushFont(font, fontSize);
        }

        // The cursor (blinking; steady for a moment after it moves).
        if (focused)
        {
            double t = ImGui::GetTime() - m_BlinkStart;
            if (t < 0.6 || std::fmod(t, 1.0) < 0.6)
            {
                float x = text.x + (float)b.VisualColumn(cursor.Line, cursor.Col) * charW;
                float y = text.y + (float)cursor.Line * lineH;
                draw->AddRectFilled(ImVec2(x, y + 1.0f), ImVec2(x + 2.0f, y + lineH - 1.0f), light ? IM_COL32(0, 0, 0, 255) : IM_COL32(235, 235, 235, 255));
            }
        }
        // The line numbers, over everything at the left edge (they don't scroll sideways).
        ImU32 gutterBg = light ? IM_COL32(245, 245, 245, 255) : IM_COL32(30, 30, 30, 255);
        draw->AddRectFilled(ImVec2(inner.Min.x, inner.Min.y), ImVec2(gutterRight, inner.Max.y), gutterBg);
        for (int l = first; l <= last; ++l)
        {
            char number[16];
            std::snprintf(number, sizeof(number), "%*d", digits, l + 1);
            float y = text.y + (float)l * lineH + (lineH - fontSize) * 0.5f;
            ImU32 color = l == cursor.Line ? (light ? IM_COL32(30, 30, 30, 255) : IM_COL32(200, 200, 200, 255))
                                           : (light ? IM_COL32(140, 140, 140, 255) : IM_COL32(110, 110, 110, 255));
            int level = lineLevel[(size_t)(l - first)];
            if (level >= 0) color = SeverityColor((Severity)level); // (a line with a problem: its number in the problem's colour)
            draw->AddText(drawFont, fontSize, ImVec2(inner.Min.x + charW, y), color, number);
        }
        // Where the problems are in the whole file: ticks at the right edge.
        if (!b.Marks.empty())
        {
            float trackTop = inner.Min.y, trackH = inner.GetHeight();
            for (const TextBuffer::Mark& m : b.Marks)
            {
                if (m.Id < 0 || m.Id >= (int)m_Problems.size()) continue;
                Severity level = m_Problems[(size_t)m.Id].D.Level;
                if ((level == Severity::Error && !m_ShowErrors) || (level == Severity::Warning && !m_ShowWarnings)) continue;
                float y = trackTop + trackH * ((float)m.Start.Line + 0.5f) / (float)std::max(1, lines);
                draw->AddRectFilled(ImVec2(inner.Max.x - 6.0f, y - 1.5f), ImVec2(inner.Max.x - 1.0f, y + 1.5f), SeverityColor(level));
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        DrawAssist(doc, text, charW, lineH, font, fontSize);
    }

    // ---- Problems (the last build's errors and warnings)

    void CodeMode::SetBuildResult(std::vector<Diagnostic> diagnostics, bool succeeded)
    {
        for (auto& doc : m_Docs)
        {
            doc->Buffer.Marks.clear();
            doc->CheckedVersion = 0; // (checked live again: that takes over from the build for its assembly)
        }
        m_Problems.clear();
        for (Diagnostic& d : diagnostics)
        {
            Problem p;
            p.D = std::move(d);
            fs::path file = p.D.File;
            std::error_code ec;
            if (p.D.Line >= 0 && !p.D.File.empty() && file.is_absolute() && fs::exists(file, ec))
            {
                p.Path = file;
                p.Key = PathKey(file);
            }
            m_Problems.push_back(std::move(p));
        }
        RebuildProblems();
        int errors = 0;
        for (const Problem& p : m_Problems) errors += p.D.Level == Severity::Error;
        m_HaveBuildResult = true;
        m_LastBuildOk = succeeded;
        if (!succeeded && errors > 0) m_ShowProblems = true;
    }

    void CodeMode::RebuildProblems()
    {
        // Errors first, then by file and place.
        std::stable_sort(m_Problems.begin(), m_Problems.end(), [](const Problem& a, const Problem& b) {
            if (a.D.Level != b.D.Level) return a.D.Level < b.D.Level;
            if (a.Key != b.Key) return a.Key < b.Key;
            if (a.D.Line != b.D.Line) return a.D.Line < b.D.Line;
            return a.D.Col < b.D.Col;
        });
        m_ErrorFiles.clear();
        for (const Problem& p : m_Problems)
            if (p.D.Level == Severity::Error && !p.Key.empty() && std::find(m_ErrorFiles.begin(), m_ErrorFiles.end(), p.Key) == m_ErrorFiles.end())
                m_ErrorFiles.push_back(p.Key);
        m_SelectedProblem = -1;
        for (auto& doc : m_Docs) AttachMarks(*doc);
    }

    // ---- Live checking

    void CodeMode::UpdateLive()
    {
        if (!m_Language && !m_LanguageTried && CSharpRuntime::IsAvailable())
        {
            m_LanguageTried = true;
            auto service = std::make_unique<LanguageService>();
            if (service->Start(Platform::ExecutableDirectory() / "managed", Project::GetDirectory())) m_Language = std::move(service);
        }
        if (!m_Language) return;
        if (auto result = m_Language->Poll()) ApplyLive(*result);
        PollAssist();

        // The file in front, once typing in it has paused (the other open files' unsaved texts go along).
        Document* doc = Current();
        if (!doc) return;
        double now = ImGui::GetTime();
        uint64_t version = doc->Buffer.Version();
        if (version != doc->SeenVersion)
        {
            doc->SeenVersion = version;
            doc->EditTime = now;
        }
        if (doc->CheckedVersion == version || now - doc->EditTime < 0.35 || m_Language->Busy()) return;
        std::vector<LanguageService::Overlay> overlays;
        for (auto& d : m_Docs)
            if (d.get() == doc || d->Buffer.IsDirty()) overlays.push_back({ d->Path, d->Buffer.GetText() });
        doc->CheckedVersion = version;
        m_LivePath = doc->Path;
        m_Language->Request(doc->Path, std::move(overlays), version);
    }

    void CodeMode::ApplyLive(LanguageService::Result& result)
    {
        if (!result.Error.empty())
        {
            if (m_LiveError.empty()) Log::Error("Code Mode: live checking failed: {}", result.Error);
            m_LiveError = result.Error;
            return;
        }
        m_LiveError.clear();
        if (m_LiveMs < 0.0) Log::Info("Code Mode: live checking ready ({} files of {} checked in {:.0f} ms)", result.Files.size(), result.Assembly, result.Milliseconds);
        m_LiveMs = result.Milliseconds;
        // Made for an older text of the file (typed in since): the next check replaces it.
        std::string asked = PathKey(m_LivePath);
        for (auto& doc : m_Docs)
            if (PathKey(doc->Path) == asked && doc->Buffer.Version() != result.Tag) return;

        // Everything known about the assembly's files is replaced by what the check found.
        std::vector<std::string> keys;
        for (const fs::path& f : result.Files) keys.push_back(PathKey(f));
        std::sort(keys.begin(), keys.end());
        for (auto& doc : m_Docs) DetachMarks(*doc);
        std::erase_if(m_Problems, [&](const Problem& p) { return !p.Key.empty() && std::binary_search(keys.begin(), keys.end(), p.Key); });
        for (Diagnostic& d : result.Diagnostics)
        {
            Problem p;
            p.Path = fs::path(std::u8string(d.File.begin(), d.File.end())); // (UTF-8 from the service)
            p.Key = PathKey(p.Path);
            p.Live = true;
            p.D = std::move(d);
            m_Problems.push_back(std::move(p));
        }
        RebuildProblems();
    }

    void CodeMode::AttachMarks(Document& doc)
    {
        TextBuffer& b = doc.Buffer;
        b.Marks.clear();
        std::string key = PathKey(doc.Path);
        for (size_t i = 0; i < m_Problems.size(); ++i)
        {
            const Problem& p = m_Problems[i];
            if (p.Key != key || p.D.Line < 0) continue;
            int line = std::min(p.D.Line, b.LineCount() - 1);
            const std::string& s = b.Line(line);
            TextPos start{ line, Code::Utf16ToByte(s, p.D.Col) };
            TextPos end = start;
            if (p.D.EndLine >= 0)
            {
                int endLine = std::clamp(p.D.EndLine, line, b.LineCount() - 1);
                end = { endLine, Code::Utf16ToByte(b.Line(endLine), p.D.EndCol) };
            }
            if (end <= start)
            {
                // Only a place: under the word there (or the one character).
                end = start;
                if (end.Col < (int)s.size() && IsWordByte(s[(size_t)end.Col]))
                    while (end.Col < (int)s.size() && IsWordByte(s[(size_t)end.Col])) end.Col = Code::NextChar(s, end.Col);
                else if (end.Col < (int)s.size()) end.Col = Code::NextChar(s, end.Col);
            }
            b.Marks.push_back({ start, end, (int)i });
        }
    }

    void CodeMode::DetachMarks(Document& doc)
    {
        // The places the edits moved them to (what the list shows, and where they go when the file opens again).
        for (const TextBuffer::Mark& m : doc.Buffer.Marks)
        {
            if (m.Id < 0 || m.Id >= (int)m_Problems.size()) continue;
            Diagnostic& d = m_Problems[(size_t)m.Id].D;
            d.Line = m.Start.Line;
            d.Col = Code::ByteToUtf16(doc.Buffer.Line(m.Start.Line), m.Start.Col);
            d.EndLine = m.End.Line;
            d.EndCol = Code::ByteToUtf16(doc.Buffer.Line(m.End.Line), m.End.Col);
        }
        doc.Buffer.Marks.clear();
    }

    void CodeMode::GoToProblem(int index)
    {
        if (index < 0 || index >= (int)m_Problems.size()) return;
        m_SelectedProblem = index;
        const Problem& p = m_Problems[(size_t)index];
        if (p.Path.empty() || !OpenFile(p.Path)) return;
        Document* doc = Current();
        if (!doc) return;
        for (const TextBuffer::Mark& m : doc->Buffer.Marks)
            if (m.Id == index)
            {
                doc->Buffer.SetCursor(m.Start);
                doc->Buffer.BreakMerge();
                doc->ScrollToCursor = true;
                break;
            }
        m_FocusCode = true;
        m_BlinkStart = ImGui::GetTime();
    }

    void CodeMode::NextProblem(bool backwards)
    {
        // The next one shown in the list that is in a file (wrapping around).
        int n = (int)m_Problems.size();
        for (int step = 1; step <= n; ++step)
        {
            int i = m_SelectedProblem < 0 ? (backwards ? n - step : step - 1) : ((m_SelectedProblem + (backwards ? -step : step)) % n + n) % n;
            const Problem& p = m_Problems[(size_t)i];
            bool shown = p.D.Level == Severity::Error ? m_ShowErrors : m_ShowWarnings;
            if (shown && !p.Path.empty())
            {
                m_ShowProblems = true;
                GoToProblem(i);
                return;
            }
        }
    }

    void CodeMode::DrawProblems(const ImVec2& size)
    {
        ImGui::BeginChild("##problems", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        int errors = 0, warnings = 0;
        for (const Problem& p : m_Problems) (p.D.Level == Severity::Error ? errors : warnings)++;
        ImGui::TextDisabled("PROBLEMS");
        ImGui::SameLine(0, 16);
        char label[48];
        std::snprintf(label, sizeof(label), "Errors (%d)", errors);
        ImGui::Checkbox(label, &m_ShowErrors);
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), "Warnings (%d)", warnings);
        ImGui::Checkbox(label, &m_ShowWarnings);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize("x").x - ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::SmallButton("x")) m_ShowProblems = false;
        ImGui::SetItemTooltip("Hide (the counts in the status bar bring it back)");

        // The places in open files are their marks' (they follow edits made since the build).
        auto placeOf = [&](int index, int& line, int& col) {
            const Problem& p = m_Problems[(size_t)index];
            line = p.D.Line;
            col = p.D.Col;
            for (auto& doc : m_Docs)
                for (const TextBuffer::Mark& m : doc->Buffer.Marks)
                    if (m.Id == index)
                    {
                        line = m.Start.Line;
                        col = Code::ByteToUtf16(doc->Buffer.Line(m.Start.Line), m.Start.Col);
                    }
        };
        fs::path project = Project::GetDirectory();
        bool any = false;
        if (ImGui::BeginTable("##problemlist", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                              ImGui::GetContentRegionAvail()))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("MSB3021 ").x + 20.0f);
            ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 240.0f);
            ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000:000").x + 8.0f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)m_Problems.size(); ++i)
            {
                const Problem& p = m_Problems[(size_t)i];
                if ((p.D.Level == Severity::Error && !m_ShowErrors) || (p.D.Level == Severity::Warning && !m_ShowWarnings)) continue;
                any = true;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImVec4 color = ImGui::ColorConvertU32ToFloat4(SeverityColor(p.D.Level));
                // The whole row clicks (to the place in the file).
                if (ImGui::Selectable("##row", m_SelectedProblem == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) GoToProblem(i);
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText((p.D.Code + ": " + p.D.Message).c_str());
                    ImGui::EndPopup();
                }
                ImGui::SameLine(0, 0);
                ImGui::TextColored(color, "%s %s", p.D.Level == Severity::Error ? "x" : "!", p.D.Code.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.D.Message.c_str());
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(p.D.Message.c_str()).x > ImGui::GetColumnWidth()) ImGui::SetTooltip("%s", p.D.Message.c_str());
                ImGui::TableNextColumn();
                if (!p.Path.empty())
                {
                    std::error_code ec;
                    fs::path rel = fs::relative(p.Path, project, ec);
                    std::string shown = (ec || rel.empty() || rel.native()[0] == '.' ? p.Path : rel).generic_string();
                    ImGui::TextUnformatted(shown.c_str());
                    ImGui::TableNextColumn();
                    int line = 0, col = 0;
                    placeOf(i, line, col);
                    ImGui::Text("%d:%d", line + 1, col + 1);
                }
                else
                {
                    ImGui::TextDisabled("%s", p.D.File.empty() ? "-" : p.D.File.c_str());
                    ImGui::TableNextColumn();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!any)
        {
            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x + 8.0f, ImGui::GetFrameHeightWithSpacing() * 2.0f));
            ImGui::TextDisabled(!m_HaveBuildResult ? "Build the scripts (Ctrl+B) to see their problems here." : m_Problems.empty() ? "No problems: the last build was clean." : "Nothing shown (see the filters above).");
        }
        ImGui::EndChild();
    }

    // ---- Completion, parameter hints, hover

    std::vector<LanguageService::Overlay> CodeMode::OverlaysFor(Document& doc)
    {
        std::vector<LanguageService::Overlay> overlays;
        for (auto& d : m_Docs)
            if (d.get() == &doc || d->Buffer.IsDirty()) overlays.push_back({ d->Path, d->Buffer.GetText() });
        return overlays;
    }

    void CodeMode::RequestCompletion(Document& doc, bool explicitly)
    {
        if (!m_Language) return;
        TextBuffer& b = doc.Buffer;
        TextPos c = b.Cursor();
        const std::string& line = b.Line(c.Line);
        int start = c.Col;
        while (start > 0 && IsWordByte(line[(size_t)start - 1])) --start;
        m_Completion = CompletionState{};
        m_Completion.Doc = &doc;
        m_Completion.Line = c.Line;
        m_Completion.StartCol = start;
        m_Completion.Waiting = true;
        m_Completion.Explicit = explicitly;
        m_Completion.Tag = m_NextTag++;
        m_Language->Send({ LanguageService::Query::Completion, doc.Path, OverlaysFor(doc), c.Line, Code::ByteToUtf16(line, c.Col), m_Completion.Tag });
    }

    void CodeMode::RequestSignature(Document& doc)
    {
        if (!m_Language) return;
        TextBuffer& b = doc.Buffer;
        TextPos c = b.Cursor();
        if (m_Signature.Doc != &doc) m_Signature = SignatureState{};
        m_Signature.Doc = &doc;
        m_Signature.Tag = m_NextTag++;
        m_Signature.AskedVersion = b.Version();
        m_Signature.AskedCursor = c;
        m_Language->Send({ LanguageService::Query::Signature, doc.Path, OverlaysFor(doc), c.Line, Code::ByteToUtf16(b.Line(c.Line), c.Col), m_Signature.Tag });
    }

    void CodeMode::TypedAssist(Document& doc, const std::string& typed)
    {
        if (!m_Language || typed.empty()) return;
        TextBuffer& b = doc.Buffer;
        TextPos c = b.Cursor();
        const std::string& line = b.Line(c.Line);
        char ch = typed.back(); // (several at once when typing fast or pasting a word: the last one counts)
        if (c.Col == 0 || line[(size_t)c.Col - 1] != ch) return; // (typed over or wrapped a selection)
        auto wordBefore = [&](int end) {
            int start = end;
            while (start > 0 && IsWordByte(line[(size_t)start - 1])) --start;
            return line.substr((size_t)start, (size_t)(end - start));
        };
        if (ch == '.')
        {
            // (not in a number: "1.5")
            std::string left = wordBefore(c.Col - 1);
            if (left.empty() || !std::isdigit((unsigned char)left[0])) RequestCompletion(doc, true);
            else m_Completion.Open = m_Completion.Waiting = false;
        }
        else if (IsWordByte(ch))
        {
            // The first letter of a word opens the list (it then narrows as the word goes on).
            std::string word = wordBefore(c.Col);
            if (!m_Completion.Open && !m_Completion.Waiting && !word.empty() && word.size() <= typed.size() && !std::isdigit((unsigned char)word[0]))
                RequestCompletion(doc, false);
        }
        else if (ch == ' ')
        {
            // After "override " (what to override) and "new " (types).
            std::string word = wordBefore(c.Col - 1);
            if (word == "override" || word == "new") RequestCompletion(doc, true);
            else m_Completion.Open = m_Completion.Waiting = false;
        }
        else m_Completion.Open = m_Completion.Waiting = false;

        if (ch == '(' || ch == ',') RequestSignature(doc);
        else if (ch == ')' && m_Signature.Open) RequestSignature(doc);
    }

    void CodeMode::PollAssist()
    {
        using Q = LanguageService::Query;
        Document* doc = Current();
        if (auto r = m_Language->Poll(Q::Completion))
            if (r->Tag == m_Completion.Tag && m_Completion.Doc == doc && doc && r->Error.empty())
            {
                m_Completion.Items = std::move(r->Items);
                m_Completion.Waiting = false;
                m_Completion.Open = true;
                m_Completion.FilteredVersion = 0;
                FilterCompletion(*doc);
            }
        if (auto r = m_Language->Poll(Q::Signature))
            if (r->Tag == m_Signature.Tag && m_Signature.Doc == doc && doc)
            {
                m_Signature.Signatures = std::move(r->Signatures);
                m_Signature.Open = !m_Signature.Signatures.empty();
                m_Signature.Active = std::clamp(r->ActiveSignature, 0, std::max(0, (int)m_Signature.Signatures.size() - 1));
                m_Signature.Param = r->ActiveParam;
            }
        if (auto r = m_Language->Poll(Q::Hover))
            if (r->Tag == m_Hover.Tag)
            {
                m_Hover.Have = true;
                m_Hover.Text = std::move(r->HoverText);
                m_Hover.Info = std::move(r->HoverDoc);
            }
        // Another file in front: the list and hints were for the one before.
        if (m_Completion.Doc && m_Completion.Doc != doc) m_Completion = CompletionState{};
        if (m_Signature.Doc && m_Signature.Doc != doc) m_Signature = SignatureState{};
        if (!doc) return;
        if (m_Completion.Open) FilterCompletion(*doc);
        // The hints follow the cursor (asked again when it moves or the text changes; they close outside a call).
        if (m_Signature.Open && (doc->Buffer.Version() != m_Signature.AskedVersion || doc->Buffer.Cursor() != m_Signature.AskedCursor) &&
            !m_Language->Busy(Q::Signature))
            RequestSignature(*doc);
    }

    void CodeMode::FilterCompletion(Document& doc)
    {
        CompletionState& cs = m_Completion;
        TextBuffer& b = doc.Buffer;
        TextPos c = b.Cursor();
        if (cs.FilteredVersion == b.Version() && cs.FilteredCursor == c) return;
        cs.FilteredVersion = b.Version();
        cs.FilteredCursor = c;
        // Still in the word it began with (else closed).
        const std::string& line = b.Line(std::min(cs.Line, b.LineCount() - 1));
        bool inWord = c.Line == cs.Line && c.Col >= cs.StartCol && c.Col <= (int)line.size() && !b.HasSelection();
        for (int i = cs.StartCol; inWord && i < c.Col; ++i) inWord = IsWordByte(line[(size_t)i]);
        std::string word = inWord ? line.substr((size_t)cs.StartCol, (size_t)(c.Col - cs.StartCol)) : std::string();
        if (!inWord || (word.empty() && !cs.Explicit))
        {
            cs.Open = cs.Waiting = false;
            return;
        }
        auto lower = [](std::string s) {
            for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
            return s;
        };
        std::string lw = lower(word);
        std::vector<std::pair<int, int>> matches; // (score, item)
        for (int i = 0; i < (int)cs.Items.size(); ++i)
        {
            const std::string& label = cs.Items[(size_t)i].Label;
            int score = -1;
            if (word.empty() || label.compare(0, word.size(), word) == 0) score = 0;
            else
            {
                std::string ll = lower(label);
                if (ll.compare(0, lw.size(), lw) == 0) score = 1;
                else
                {
                    // The capitals ("gc" -> GetComponent), then anywhere inside.
                    size_t j = 0;
                    bool humps = true;
                    for (char w : lw)
                    {
                        while (j < label.size() && !(std::tolower((unsigned char)label[j]) == w &&
                                                     (j == 0 || std::isupper((unsigned char)label[j]) || label[j - 1] == '_')))
                            ++j;
                        if (j == label.size())
                        {
                            humps = false;
                            break;
                        }
                        ++j;
                    }
                    if (humps) score = 2;
                    else if (lw.size() >= 2 && ll.find(lw) != std::string::npos) score = 3;
                }
            }
            if (score >= 0) matches.push_back({ score, i });
        }
        std::stable_sort(matches.begin(), matches.end(), [](auto& a, auto& b) { return a.first < b.first; });
        cs.Shown.clear();
        cs.Scores.clear();
        for (auto& [score, i] : matches)
        {
            cs.Shown.push_back(i);
            cs.Scores.push_back(score);
        }
        // (a word that is already the only match, whole: nothing to offer)
        if (cs.Shown.empty() || (cs.Shown.size() == 1 && cs.Items[(size_t)cs.Shown[0]].Label == word && !cs.Explicit))
        {
            cs.Open = false;
            return;
        }
        cs.Selected = 0;
        cs.ScrollToSelected = true;
    }

    void CodeMode::AcceptCompletion(Document& doc)
    {
        CompletionState& cs = m_Completion;
        if (!cs.Open || cs.Shown.empty()) return;
        const auto& item = cs.Items[(size_t)cs.Shown[(size_t)std::clamp(cs.Selected, 0, (int)cs.Shown.size() - 1)]];
        TextBuffer& b = doc.Buffer;
        TextPos start{ cs.Line, cs.StartCol }, end = b.Cursor();
        if (item.Insert.empty())
        {
            b.Replace(start, end, item.Label);
        }
        else
        {
            // A whole declaration (an override): its lines at this line's indentation, the cursor where "$0" is.
            std::string indent = b.IndentOf(cs.Line), text, unit = "    ";
            size_t from = 0;
            bool first = true;
            while (from <= item.Insert.size())
            {
                size_t nl = item.Insert.find('\n', from);
                std::string part = item.Insert.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
                if (!first)
                {
                    std::string lead;
                    while (part.compare(0, unit.size(), unit) == 0)
                    {
                        lead += b.IndentUnit;
                        part.erase(0, unit.size());
                    }
                    text += "\n" + indent + lead;
                }
                text += part;
                first = false;
                if (nl == std::string::npos) break;
                from = nl + 1;
            }
            size_t mark = text.find("$0");
            std::string before = mark == std::string::npos ? text : text.substr(0, mark);
            if (mark != std::string::npos) text.erase(mark, 2);
            b.Replace(start, end, text);
            TextPos caret = start;
            for (char ch : before)
            {
                if (ch == '\n') ++caret.Line, caret.Col = 0;
                else ++caret.Col;
            }
            b.SetCursor(caret);
        }
        b.BreakMerge();
        cs = CompletionState{};
        doc.ScrollToCursor = true;
    }

    bool CodeMode::AssistKeys(Document& doc)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl || io.KeyAlt) return false;
        auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
        CompletionState& cs = m_Completion;
        if (cs.Open && !cs.Shown.empty())
        {
            int n = (int)cs.Shown.size();
            int step = pressed(ImGuiKey_DownArrow) ? 1 : pressed(ImGuiKey_UpArrow) ? -1 : pressed(ImGuiKey_PageDown) ? 9 : pressed(ImGuiKey_PageUp) ? -9 : 0;
            if (step)
            {
                cs.Selected = std::abs(step) == 1 ? (cs.Selected + step + n) % n : std::clamp(cs.Selected + step, 0, n - 1);
                cs.ScrollToSelected = true;
                return true;
            }
            if (!io.KeyShift && (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Tab)))
            {
                AcceptCompletion(doc);
                return true;
            }
            if (pressed(ImGuiKey_Escape))
            {
                cs.Open = cs.Waiting = false;
                return true;
            }
        }
        if (m_Signature.Open)
        {
            if (pressed(ImGuiKey_Escape))
            {
                m_Signature = SignatureState{};
                return true;
            }
            int n = (int)m_Signature.Signatures.size();
            if (n > 1 && (pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_UpArrow)))
            {
                m_Signature.Active = (m_Signature.Active + (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : -1) + n) % n;
                return true;
            }
        }
        return false;
    }

    namespace
    {
        // A completion item's kind: a letter in a coloured square.
        void KindIcon(ImDrawList* draw, ImVec2 at, float size, const std::string& kind)
        {
            struct Look
            {
                const char* Kind;
                const char* Letter;
                ImU32 Color;
            };
            static const Look looks[] = {
                { "method", "M", IM_COL32(180, 120, 220, 255) },   { "property", "P", IM_COL32(90, 160, 230, 255) },
                { "field", "F", IM_COL32(70, 140, 200, 255) },     { "constant", "C", IM_COL32(70, 140, 200, 255) },
                { "event", "E", IM_COL32(230, 170, 60, 255) },     { "class", "C", IM_COL32(230, 150, 60, 255) },
                { "struct", "S", IM_COL32(100, 180, 160, 255) },   { "interface", "I", IM_COL32(110, 190, 230, 255) },
                { "enum", "E", IM_COL32(220, 190, 80, 255) },      { "enumMember", "e", IM_COL32(220, 190, 80, 255) },
                { "delegate", "D", IM_COL32(180, 120, 220, 255) }, { "namespace", "N", IM_COL32(150, 150, 150, 255) },
                { "local", "L", IM_COL32(120, 190, 120, 255) },    { "parameter", "p", IM_COL32(120, 190, 120, 255) },
                { "keyword", "K", IM_COL32(130, 130, 140, 255) },  { "typeParameter", "T", IM_COL32(100, 180, 160, 255) },
            };
            const Look* look = nullptr;
            for (const Look& l : looks)
                if (kind == l.Kind) look = &l;
            ImU32 color = look ? look->Color : IM_COL32(130, 130, 130, 255);
            draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), (color & 0x00FFFFFF) | 0x50000000, 3.0f);
            const char* letter = look ? look->Letter : "?";
            ImVec2 ts = ImGui::CalcTextSize(letter);
            draw->AddText(ImVec2(at.x + (size - ts.x) * 0.5f, at.y + (size - ts.y) * 0.5f), color, letter);
        }
    }

    void CodeMode::DrawAssist(Document& doc, ImVec2 text, float charW, float lineH, ImFont* font, float fontSize)
    {
        TextBuffer& b = doc.Buffer;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
        TextPos c = b.Cursor();
        float lineTop = text.y + (float)c.Line * lineH;

        // Parameter hints: above the line (below when there is no room).
        if (m_Signature.Open && m_Signature.Doc == &doc && !m_Signature.Signatures.empty())
        {
            const auto& sig = m_Signature.Signatures[(size_t)std::clamp(m_Signature.Active, 0, (int)m_Signature.Signatures.size() - 1)];
            float x = text.x + (float)b.VisualColumn(c.Line, c.Col) * charW - charW * 4.0f;
            bool above = lineTop - vp->WorkPos.y > lineH * 4.0f;
            ImGui::SetNextWindowPos(ImVec2(std::max(vp->WorkPos.x, x), above ? lineTop - 2.0f : lineTop + lineH + 2.0f), ImGuiCond_Always, ImVec2(0.0f, above ? 1.0f : 0.0f));
            ImGui::SetNextWindowViewport(vp->ID);
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::min(vp->WorkSize.x * 0.8f, charW * 110.0f), FLT_MAX));
            ImGui::Begin("##signature", nullptr, flags | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            if (m_Signature.Signatures.size() > 1)
            {
                ImGui::TextDisabled("%d of %d", m_Signature.Active + 1, (int)m_Signature.Signatures.size());
                ImGui::SetItemTooltip("Up/Down: the other overloads");
                ImGui::SameLine();
            }
            // The label with the parameter being typed in bold colour.
            ImGui::PushFont(font, std::round(fontSize * 0.93f));
            const LanguageService::SignatureParam* param =
                m_Signature.Param < (int)sig.Params.size() ? &sig.Params[(size_t)m_Signature.Param] : (!sig.Params.empty() && sig.Label.find("params ") != std::string::npos ? &sig.Params.back() : nullptr);
            ImVec4 normal = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImVec4 strong = ImGuiLayer::IsLightTheme() ? ImVec4(0.0f, 0.35f, 0.8f, 1.0f) : ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
            if (param)
            {
                std::string a = sig.Label.substr(0, (size_t)param->Start), mid = sig.Label.substr((size_t)param->Start, (size_t)param->Length),
                            z = sig.Label.substr((size_t)(param->Start + param->Length));
                ImGui::TextUnformatted(a.c_str());
                ImGui::SameLine(0, 0);
                ImGui::TextColored(strong, "%s", mid.c_str());
                ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x, r1.y), r1, ImGui::GetColorU32(strong));
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(z.c_str());
            }
            else ImGui::TextColored(normal, "%s", sig.Label.c_str());
            ImGui::PopFont();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 45.0f);
            if (!sig.Doc.empty()) ImGui::TextDisabled("%s", sig.Doc.c_str());
            if (param && !param->Doc.empty())
            {
                ImGui::TextColored(strong, "%s:", param->Name.c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(param->Doc.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::End();
        }

        // The completion list: under the word, with the chosen item's details beside it.
        CompletionState& cs = m_Completion;
        if (!cs.Open || cs.Doc != &doc || cs.Shown.empty()) return;
        ImGui::PushFont(font, std::round(fontSize * 0.93f));
        float rowH = std::round(ImGui::GetFontSize() * 1.45f);
        int visible = std::min(10, (int)cs.Shown.size());
        float longest = 12.0f;
        for (size_t i = 0; i < std::min<size_t>(cs.Shown.size(), 300); ++i) longest = std::max(longest, (float)cs.Items[(size_t)cs.Shown[i]].Label.size());
        float width = std::clamp(longest + 6.0f, 24.0f, 56.0f) * ImGui::CalcTextSize("M").x;
        float height = rowH * (float)visible + ImGui::GetStyle().WindowPadding.y * 2.0f;
        float x = text.x + (float)b.VisualColumn(cs.Line, cs.StartCol) * charW - rowH - 6.0f;
        float below = lineTop + lineH + 2.0f;
        bool up = below + height > vp->WorkPos.y + vp->WorkSize.y;
        ImVec2 pos(std::clamp(x, vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - width), up ? lineTop - height - 2.0f : below);
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::Begin("##completion", nullptr, (flags & ~ImGuiWindowFlags_NoDecoration) | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoCollapse);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin((int)cs.Shown.size(), rowH);
        if (cs.ScrollToSelected) clipper.IncludeItemByIndex(cs.Selected);
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& item = cs.Items[(size_t)cs.Shown[(size_t)i]];
                ImGui::PushID(i);
                ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##item", i == cs.Selected, 0, ImVec2(0, rowH)))
                {
                    cs.Selected = i;
                    AcceptCompletion(doc);
                    m_FocusCode = true;
                    ImGui::PopID();
                    clipper.End();
                    goto done;
                }
                if (i == cs.Selected && cs.ScrollToSelected)
                {
                    ImGui::SetScrollHereY(0.5f);
                    cs.ScrollToSelected = false;
                }
                float icon = rowH - 6.0f;
                KindIcon(draw, ImVec2(at.x + 2.0f, at.y + 3.0f), icon, item.Kind);
                draw->AddText(ImVec2(at.x + icon + 8.0f, at.y + (rowH - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), item.Label.c_str());
                ImGui::PopID();
            }
    done:
        ImVec2 listMin = ImGui::GetWindowPos(), listSize = ImGui::GetWindowSize();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopFont();

        // The chosen item: what it is, and its documentation.
        if (cs.Open && !cs.Shown.empty())
        {
            const auto& item = cs.Items[(size_t)cs.Shown[(size_t)std::clamp(cs.Selected, 0, (int)cs.Shown.size() - 1)]];
            if (!item.Detail.empty())
            {
                float wrap = std::min(ImGui::GetFontSize() * 32.0f, vp->WorkSize.x * 0.4f);
                bool right = listMin.x + listSize.x + wrap + 20.0f < vp->WorkPos.x + vp->WorkSize.x;
                ImGui::SetNextWindowPos(ImVec2(right ? listMin.x + listSize.x + 2.0f : listMin.x - 2.0f, listMin.y), ImGuiCond_Always, ImVec2(right ? 0.0f : 1.0f, 0.0f));
                ImGui::SetNextWindowViewport(vp->ID);
                ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(wrap + 20.0f, FLT_MAX));
                ImGui::Begin("##completiondetail", nullptr, flags | ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
                ImGui::PushFont(font, std::round(fontSize * 0.87f));
                ImGui::TextUnformatted(item.Detail.c_str());
                ImGui::PopFont();
                if (!item.Doc.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", item.Doc.c_str());
                }
                ImGui::PopTextWrapPos();
                ImGui::End();
            }
        }
    }
}
