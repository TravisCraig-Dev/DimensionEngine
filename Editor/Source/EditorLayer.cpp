#include <Dimension/Terrain/Terrain.h>
#include "EditorLayer.h"
#include "Dimension/Platform/AppIcon.h"
#include "Dimension/ImGui/ImGuiLayer.h"
#include "Dimension/Asset/AssetDependencies.h"
#include "EditorActions.h"
#include "EditorUI.h"
#include "EditorPreferences.h"
#include <Dimension/Core/Input.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Platform/FileDialogs.h>
#include <Dimension/Renderer/Mesh.h>
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Scripting/GameModule.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scene/SceneSearch.h>
#include <Dimension/Scene/SceneSnapping.h>
#include <Dimension/Scene/ProjectSearch.h>
#include <Dimension/Scene/SceneTemplates.h>
#include <Dimension/Project/RecentScenes.h>
#include <Dimension/Project/SceneBookmarks.h>
#include <Dimension/Scene/SceneBounds.h>
#include <Dimension/Scene/SceneMeasure.h>
#include <Dimension/Scene/SelectionHistory.h>
#include <Dimension/Scene/ViewAlign.h>
#include <Dimension/Project/ProjectHub.h>
#include <Dimension/Asset/AssetPackage.h>
#include "Shortcuts.h"
#include <Dimension/Navigation/NavMesh.h>
#include <Dimension/Core/Profiler.h>
#include <Dimension/Scene/SceneManager.h>
#include <Dimension/Scripting/CSharpRuntime.h>
#include <Dimension/Scripting/EditModeScripts.h>
#include <Dimension/Scripting/CSharpProject.h>
#include <Dimension/Scripting/ScriptRegistry.h>
#include <Dimension/UI/UI.h>
#include <Dimension/Asset/ImportCache.h>
#include <thread>
#include <process.h>
#include <cstdio>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <functional>
#include <fstream>
#include <optional>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Editor
{
    namespace
    {
        // The world-space ray under the mouse in a view `size` pixels big at `origin` (from the near plane outward).
        void MouseRay(const glm::mat4& viewProj, const ImVec2& origin, const ImVec2& size, glm::vec3& from, glm::vec3& direction)
        {
            glm::mat4 inv = glm::inverse(viewProj);
            ImVec2 mouse = ImGui::GetMousePos();
            glm::vec2 ndc((mouse.x - origin.x) / size.x * 2.0f - 1.0f, 1.0f - (mouse.y - origin.y) / size.y * 2.0f);
            glm::vec4 a = inv * glm::vec4(ndc, 0.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
            from = glm::vec3(a) / a.w;
            direction = glm::normalize(glm::vec3(b) / b.w - from);
        }

        fs::path ExecutableDirectory()
        {
#ifdef _WIN32
            wchar_t buffer[MAX_PATH];
            GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            return fs::path(buffer).parent_path();
#else
            return fs::current_path();
#endif
        }

        enum class Glyph { Play, Pause, Step, Stop, Hand, Move, Rotate, Scale };

        // Button with a vector glyph (no icon font needed).
        bool GlyphButton(const char* id, Glyph glyph, bool active, const char* tooltip)
        {
            float s = ImGui::GetFrameHeight();
            ImVec2 size(s * 1.25f, s);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool pressed = ImGui::Button(id, size);
            if (active) ImGui::PopStyleColor();
            if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);

            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            float r = s * 0.28f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            switch (glyph)
            {
            case Glyph::Play: dl->AddTriangleFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.8f, c.y + r), ImVec2(c.x + r, c.y), col); break;
            case Glyph::Pause:
                dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.25f, c.y + r), col);
                dl->AddRectFilled(ImVec2(c.x + r * 0.25f, c.y - r), ImVec2(c.x + r * 0.8f, c.y + r), col);
                break;
            case Glyph::Step:
                dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r, c.y + r), ImVec2(c.x + r * 0.4f, c.y), col);
                dl->AddRectFilled(ImVec2(c.x + r * 0.5f, c.y - r), ImVec2(c.x + r, c.y + r), col);
                break;
            case Glyph::Stop: dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), col); break;
            case Glyph::Hand: dl->AddCircle(c, r, col, 16, 2.0f); dl->AddCircleFilled(c, r * 0.35f, col); break;
            case Glyph::Move:
                dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, 2.0f);
                dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, 2.0f);
                dl->AddTriangleFilled(ImVec2(c.x + r * 1.2f, c.y), ImVec2(c.x + r * 0.6f, c.y - r * 0.4f), ImVec2(c.x + r * 0.6f, c.y + r * 0.4f), col);
                dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 1.2f), ImVec2(c.x - r * 0.4f, c.y - r * 0.6f), ImVec2(c.x + r * 0.4f, c.y - r * 0.6f), col);
                break;
            case Glyph::Rotate:
                dl->PathArcTo(c, r, 0.6f, 5.6f, 20);
                dl->PathStroke(col, 0, 2.0f);
                dl->AddTriangleFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.9f), ImVec2(c.x + r * 1.25f, c.y - r * 0.05f), ImVec2(c.x + r * 0.45f, c.y - r * 0.25f), col);
                break;
            case Glyph::Scale:
                dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r * 0.2f, c.y + r * 0.2f), col, 0, 0, 1.5f);
                dl->AddRectFilled(ImVec2(c.x + r * 0.35f, c.y + r * 0.35f), ImVec2(c.x + r, c.y + r), col);
                break;
            }
            return pressed;
        }
    }

    // ---- Lifecycle -------------------------------------------------------------------

    void EditorLayer::OnAttach()
    {
        m_Hierarchy = MakeScope<HierarchyPanel>(m_Ctx);
        m_Inspector = MakeScope<InspectorPanel>(m_Ctx);
        m_Inspector->DebugMode = m_Options.InspectorDebug;
        if (!m_Options.Layout.empty()) m_PendingLayout = m_Options.Layout;
        if (m_Options.RememberProject) ProjectHub::Remember(Project::GetDirectory());
        if (m_Options.DrawMode == "wireframe") m_DrawMode = SceneDrawMode::Wireframe;
        else if (m_Options.DrawMode == "shaded-wireframe") m_DrawMode = SceneDrawMode::ShadedWireframe;
        else if (m_Options.DrawMode == "unlit") m_DrawMode = SceneDrawMode::Unlit;
        else if (m_Options.DrawMode == "overdraw") m_DrawMode = SceneDrawMode::Overdraw;
        m_Content = MakeScope<ContentBrowserPanel>(m_Ctx);
        m_Console = MakeScope<ConsolePanel>();
        m_SceneSettings = MakeScope<SceneSettingsPanel>(m_Ctx);
        m_Animator = MakeScope<AnimatorPanel>(m_Ctx);
        m_Timeline = MakeScope<TimelinePanel>(m_Ctx);
        m_Navigation = MakeScope<NavigationPanel>(m_Ctx);
        m_Profiler = MakeScope<ProfilerPanel>();
        m_FrameDebugger = MakeScope<FrameDebuggerPanel>(m_Ctx);
        m_SpriteEditor = MakeScope<SpriteEditorPanel>(m_Ctx);
        m_AssetPreviews = MakeScope<AssetPreviews>();
        m_Ctx.Previews = m_AssetPreviews.get();
        Navigation::ApplyAreaNames(); // the project's area names in the Area dropdowns

        // Per-project editor layout (like Unity's Library/ folder, not meant for version control).
        fs::path settingsDir = Project::GetDirectory() / ".dimension";
        fs::create_directories(settingsDir);
        m_IniPath = (settingsDir / "EditorLayout.ini").string();
        ImGui::GetIO().IniFilename = m_IniPath.c_str();
        m_ResetLayout = !fs::exists(m_IniPath);

        EditorPreferences::Get().Load();
        Shortcuts::Load(EditorPreferences::Get().Shortcuts);
        if (m_Options.Theme >= 0) EditorPreferences::Get().Theme = m_Options.Theme; // not saved unless Preferences change
        ApplyPreferences();
        m_Camera.SetFlySpeed(EditorPreferences::Get().CameraFlySpeed);
        m_ShowGrid = EditorPreferences::Get().ShowGrid;
        if (m_Options.OpenWindow == "Build Settings") m_ShowBuildSettings = true;
        else if (m_Options.OpenWindow == "Preferences") m_ShowPreferences = true;
        else if (m_Options.OpenWindow == "Project Settings") m_ShowProjectSettings = true;
        else if (m_Options.OpenWindow == "Audio Mixer") m_ShowAudioMixer = true;
        else if (m_Options.OpenWindow == "Timeline") m_ShowTimeline = true;
        else if (m_Options.OpenWindow == "Navigation") m_ShowNavigation = true;
        else if (m_Options.OpenWindow == "Profiler") m_ShowProfiler = true;
        else if (m_Options.OpenWindow == "Sprite Editor") m_ShowSpriteEditor = true;
        else if (m_Options.OpenWindow == "Snap Settings") m_ShowSnapSettings = true;
        else if (m_Options.OpenWindow == "Lighting") m_ShowLighting = true;
        else if (m_Options.OpenWindow == "Shortcuts") m_ShowShortcuts = true;
        else if (m_Options.OpenWindow == "New Scene") m_ShowNewScene = true;
        else if (m_Options.OpenWindow == "Project Hub") m_ShowHub = true;
        else if (m_Options.OpenWindow == "Undo History") m_ShowUndoHistory = true;
        else if (m_Options.OpenWindow == "Views") m_OpenViewsMenu = true;
        else if (m_Options.OpenWindow == "Grid") m_OpenGridMenu = true;
        else if (m_Options.OpenWindow == "Export Package" && !m_Options.SelectAsset.empty())
            OpenExportPackage({ AssetManager::GetHandle(m_Options.SelectAsset) });
        else if (m_Options.OpenWindow == "Import Package" && !m_Options.PackagePath.empty()) OpenImportPackage(m_Options.PackagePath);
        else if (m_Options.OpenWindow == "Search")
        {
            OpenSearch(true);
            m_SearchQuery = m_Options.SearchQuery;
        }
        else if (!m_Options.OpenWindow.empty()) m_ScriptWindows[m_Options.OpenWindow] = true; // a C# extension's window, by title
        else if (m_Options.OpenWindow == "Frame Debugger")
        {
            m_ShowFrameDebugger = true;
            m_FrameDebugger->Start(m_Options.FrameEvent);
        }
        else if (m_Options.OpenWindow == "Tile Palette") m_ShowTilePalette = true;

        LoadGameModule();
        // Imports of files that changed or are gone (their new import gets a new entry).
        if (size_t stale = ImportCache::ClearUnused(Project::GetAssetsDirectory())) Log::Info("Removed {} stale import cache entries", stale);
        InitCSharp();
        m_SceneTarget = MakeScope<SceneRenderTarget>(m_SceneViewSize.x, m_SceneViewSize.y);
        m_GameTarget = MakeScope<SceneRenderTarget>(m_GameViewSize.x, m_GameViewSize.y);

        fs::path start = !m_Options.ScenePath.empty() ? fs::path(m_Options.ScenePath) : AssetManager::GetAbsolutePath(Project::GetConfig().StartScene);
        if (!start.empty() && fs::exists(start)) OpenScene(start);
        else NewScene();

        Log::Info("Editor ready. Scene view: RMB+WASD fly, Alt+LMB orbit, MMB pan, wheel zoom, F frame, W/E/R gizmos.");
        if (!m_Options.PrefabPath.empty())
        {
            fs::path p = m_Options.PrefabPath;
            if (p.is_relative()) p = AssetManager::GetAssetsDirectory() / p;
            if (AssetHandle h = AssetManager::Import(p); h.IsValid()) OpenPrefabMode(h);
        }
        m_Ctx.HierarchySearch = m_Options.Search;
        if (!m_Options.SelectEntity.empty())
        {
            // Comma-separated names select several entities; the last one is primary.
            std::string names = m_Options.SelectEntity;
            size_t start = 0;
            while (start <= names.size())
            {
                size_t comma = names.find(',', start);
                std::string name = names.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                m_Ctx.AddToSelection(m_Ctx.EditorScene->FindEntityByName(name));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            FocusSelected();
        }
        // --select-asset "A,B": several select them all (the last is the primary).
        for (size_t start = 0; start < m_Options.SelectAsset.size();)
        {
            size_t comma = m_Options.SelectAsset.find(',', start);
            std::string path = m_Options.SelectAsset.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (AssetHandle h = AssetManager::GetHandle(path); h.IsValid())
            {
                auto list = m_Ctx.SelectedAssetList();
                list.push_back(h);
                m_Ctx.SelectAssets(list, h);
                m_Content->ShowDirectory((AssetManager::GetAssetsDirectory() / path).parent_path());
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!m_Options.ProjectView.empty()) m_Content->SetListView(m_Options.ProjectView == "list", false);
        if (!m_Options.ProjectSearch.empty()) m_Content->SetSearch(m_Options.ProjectSearch);
        if (!m_Options.AssetPreview.empty()) m_Inspector->SetPreview(m_Options.AssetPreview);
        if (m_Options.ViewGiven) m_Options.View == 0 ? FrameAll() : GoToView(m_Options.View);
        // --fold "Transform,Script:Spinner" (automation): those headers folded, for the session.
        for (size_t start = 0; start < m_Options.Fold.size();)
        {
            size_t comma = m_Options.Fold.find(',', start);
            std::string key = m_Options.Fold.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            auto& folded = EditorPreferences::Get().FoldedComponents;
            if (!key.empty() && std::find(folded.begin(), folded.end(), key) == folded.end()) folded.push_back(key);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (m_Options.NewInspector) NewInspector(true);      // --new-inspector: one locked to the --select selection
        if (m_Options.LockInspector) m_Inspector->LockToSelection();
        if (!m_Options.InspectorSearch.empty()) m_Inspector->SetSearch(m_Options.InspectorSearch);
        if (m_Options.SelectionSet > 0) RestoreSelection(m_Options.SelectionSet);
        // --grid "plane=1,cell=0.5,opacity=0.6,follow" (automation; for the session, the Preferences file unchanged).
        for (size_t start = 0; start < m_Options.Grid.size();)
        {
            size_t comma = m_Options.Grid.find(',', start);
            std::string part = m_Options.Grid.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            size_t eq = part.find('=');
            std::string key = part.substr(0, eq);
            float value = eq == std::string::npos ? 1.0f : (float)std::atof(part.c_str() + eq + 1);
            auto& prefs = EditorPreferences::Get();
            if (key == "plane") prefs.GridPlane = std::clamp((int)value, 0, 2);
            else if (key == "cell") prefs.GridCellSize = std::max(0.0f, value);
            else if (key == "opacity") prefs.GridOpacity = std::clamp(value, 0.0f, 1.0f);
            else if (key == "follow") prefs.GridFollowSelection = value != 0.0f;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        // --measure "x,y,z;x,y,z" (automation): the tool on, with that measurement.
        if (!m_Options.Measure.empty())
        {
            SetMeasuring(true);
            float v[6] = {};
            if (std::sscanf(m_Options.Measure.c_str(), "%f,%f,%f;%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
            {
                m_MeasureA = glm::vec3(v[0], v[1], v[2]);
                m_MeasureB = glm::vec3(v[3], v[4], v[5]);
            }
        }
        // --new-scene "Template", --open-additive and --prefab-instance (automation).
        for (const auto& t : SceneTemplates::All())
            if (!m_Options.NewSceneTemplate.empty() && t.Name == m_Options.NewSceneTemplate) NewSceneFrom(t);
        for (size_t start = 0; start < m_Options.AdditiveScenes.size();)
        {
            size_t comma = m_Options.AdditiveScenes.find(',', start);
            if (comma == std::string::npos) comma = m_Options.AdditiveScenes.size();
            OpenSceneAdditive(fs::path(m_Options.AdditiveScenes.substr(start, comma - start)));
            start = comma + 1;
        }
        if (!m_Options.PrefabInstance.empty())
            if (Entity e = m_Ctx.EditorScene->FindEntityByName(m_Options.PrefabInstance); !e.IsNull())
                if (Entity root = Prefabs::FindInstanceRoot(*m_Ctx.EditorScene, e); !root.IsNull())
                    OpenPrefabMode(m_Ctx.EditorScene->GetRegistry().Get<PrefabLinkComponent>(root).Prefab, (uint64_t)m_Ctx.EditorScene->GetGuid(root));
        if (m_Options.StartInPlayMode) Play();
    }

    void EditorLayer::OnDetach()
    {
        if (m_Ctx.IsPlaying()) Stop();
        ImGui::SaveIniSettingsToDisk(m_IniPath.c_str());
        ImGui::GetIO().IniFilename = nullptr;
        m_Ctx.EditorScene.reset();
        m_SceneTarget.reset();
        m_GameTarget.reset();
    }

    bool EditorLayer::OnCloseRequested()
    {
        if (m_Ctx.IsPlaying()) Stop();
        return ConfirmDiscardChanges();
    }

    void EditorLayer::OnFilesDropped(const std::vector<fs::path>& files) { m_Content->ImportFiles(files); }

    // ---- Scenes -------------------------------------------------------------------------

    void EditorLayer::NewScene() { NewSceneFrom(SceneTemplates::All().front()); } // Basic 3D

    void EditorLayer::NewSceneFrom(const SceneTemplates::Template& t)
    {
        if (m_Ctx.IsPlaying()) Stop();
        auto scene = SceneTemplates::Create(t);
        if (!scene)
        {
            Log::Error("Could not read the template '{}'", t.Name);
            return;
        }

        m_Ctx.EditorScene = scene;
        m_Ctx.Sections.Clear();
        m_Ctx.ScenePath.clear();
        m_Ctx.Dirty = false;
        m_Ctx.Select(Entity::Null());
        m_Ctx.ClearHistory();
        UpdateWindowTitle();
    }

    void EditorLayer::OpenScene(const fs::path& path)
    {
        if (m_Ctx.IsPlaying()) Stop();
        auto scene = SceneSerializer::LoadFromFile(path);
        if (!scene) return;
        Prefabs::SyncAll(*scene); // pick up prefab edits made since the scene was saved
        m_Ctx.EditorScene = scene;
        m_Ctx.Sections.Clear();
        m_Ctx.ScenePath = path;
        m_Ctx.Dirty = false;
        m_Ctx.Select(Entity::Null());
        m_Ctx.ClearHistory();
        UpdateWindowTitle();
        RecentScenes::Add(path);
        Log::Info("Opened scene '{}'", path.filename().string());
    }

    bool EditorLayer::SaveScene()
    {
        if (m_Ctx.IsPlaying())
        {
            Log::Warn("Exit play mode before saving the scene");
            return false;
        }
        if (m_Ctx.InPrefabMode()) return SavePrefabStage();
        if (m_Ctx.ScenePath.empty()) return SaveSceneAs();
        // With scenes open alongside it, only the main scene's objects go into its file; the others save to theirs.
        if (m_Ctx.Sections.Count() > 1)
        {
            std::ofstream out(m_Ctx.ScenePath);
            out << m_Ctx.Sections.Serialize(*m_Ctx.EditorScene, 0).dump(2);
            if (!out) return false;
            for (size_t i = 1; i < m_Ctx.Sections.Count(); ++i)
                if (m_Ctx.Sections.IsDirty(*m_Ctx.EditorScene, i) && m_Ctx.Sections.Save(*m_Ctx.EditorScene, i))
                    Log::Info("Saved scene '{}'", m_Ctx.Sections.Added()[i - 1].Path.filename().string());
        }
        else if (!SceneSerializer::SaveToFile(*m_Ctx.EditorScene, m_Ctx.ScenePath)) return false;
        AssetHandle h = AssetManager::Import(m_Ctx.ScenePath);
        if (!Project::GetConfig().StartScene.IsValid() && h.IsValid())
        {
            Project::GetConfig().StartScene = h;
            Project::Save();
        }
        m_Ctx.Dirty = false;
        UpdateWindowTitle();
        Log::Info("Saved scene '{}'", m_Ctx.ScenePath.filename().string());
        return true;
    }

    void EditorLayer::OpenSceneWithDialog()
    {
        auto p = FileDialogs::OpenFile({ { L"Scene", L"*.scene" } }, AssetManager::GetAssetsDirectory());
        if (p && ConfirmDiscardChanges()) OpenScene(*p);
    }

    bool EditorLayer::SaveSceneAs()
    {
        if (m_Ctx.InPrefabMode()) return SavePrefabStage();
        auto path = FileDialogs::SaveFile({ { L"Scene", L"*.scene" } }, L"scene", AssetManager::GetAssetsDirectory());
        if (!path) return false;
        m_Ctx.ScenePath = *path;
        m_Ctx.EditorScene->SetName(path->stem().string());
        RecentScenes::Add(*path);
        return SaveScene();
    }

    bool EditorLayer::ConfirmDiscardChanges()
    {
        if (m_Ctx.InPrefabMode() && !LeavePrefabMode()) return false;
        // The scenes open alongside the main one, each asked about on its own.
        for (size_t i = 1; i < m_Ctx.Sections.Count(); ++i)
            if (m_Ctx.Sections.IsDirty(*m_Ctx.EditorScene, i))
            {
                int choice = FileDialogs::YesNoCancel("Unsaved Changes", "The scene '" + m_Ctx.Sections.Added()[i - 1].Name + "' has unsaved changes. Save them?");
                if (choice == 2) return false;
                if (choice == 0 && !m_Ctx.Sections.Save(*m_Ctx.EditorScene, i)) return false;
            }
        if (!m_Ctx.Dirty) return true;
        int choice = FileDialogs::YesNoCancel("Unsaved Changes", "The scene '" + m_Ctx.EditorScene->GetName() + "' has unsaved changes. Save them?");
        if (choice == 0) return SaveScene();
        return choice == 1;
    }

    void EditorLayer::OpenSceneAdditive(const fs::path& path)
    {
        if (m_Ctx.IsPlaying() || m_Ctx.InPrefabMode())
        {
            Log::Warn("Open scenes additively outside play mode and Prefab Mode");
            return;
        }
        if (!m_Ctx.ScenePath.empty() && fs::equivalent(path, m_Ctx.ScenePath))
        {
            Log::Warn("'{}' is the main scene", path.filename().string());
            return;
        }
        if (!m_Ctx.Sections.Open(*m_Ctx.EditorScene, path))
        {
            Log::Warn("'{}' is already open or cannot be read", path.filename().string());
            return;
        }
        m_Ctx.ClearHistory(); // earlier undo steps do not know its objects
        Log::Info("Opened scene '{}' additively", path.filename().string());
    }

    void EditorLayer::CloseAddedScene(size_t section)
    {
        if (section == 0 || section >= m_Ctx.Sections.Count() || m_Ctx.IsPlaying()) return;
        if (m_Ctx.Sections.IsDirty(*m_Ctx.EditorScene, section))
        {
            int choice = FileDialogs::YesNoCancel("Unsaved Changes",
                                                  "The scene '" + m_Ctx.Sections.Added()[section - 1].Name + "' has unsaved changes. Save them?");
            if (choice == 2) return;
            if (choice == 0 && !m_Ctx.Sections.Save(*m_Ctx.EditorScene, section)) return;
        }
        m_Ctx.Select(Entity::Null());
        m_Ctx.Sections.Close(*m_Ctx.EditorScene, section);
        m_Ctx.ClearHistory();
    }

    void EditorLayer::UpdateWindowTitle()
    {
        std::string title = "Dimension Editor - " + Project::GetConfig().Name + " - ";
        if (m_Ctx.InPrefabMode()) title += m_Ctx.PrefabModeSceneName() + " > Prefab: ";
        title += m_Ctx.EditorScene->GetName() + (m_Ctx.Dirty ? "*" : "");
        if (m_Ctx.IsPlaying()) title += "  [PLAYING]";
        Application::Get().GetWindow().SetTitle(title);
    }

    // ---- Play mode ------------------------------------------------------------------------

    void EditorLayer::Play()
    {
        if (m_Ctx.IsPlaying()) return;
        if (m_Ctx.InPrefabMode() && !LeavePrefabMode()) return; // play always runs the scene
        if (EditorPreferences::Get().SaveSceneBeforePlay && m_Ctx.Dirty && !m_Ctx.ScenePath.empty()) SaveScene();
        if (m_Console->ClearOnPlay) m_Console->Clear();
        SceneManager::CancelPendingLoad();
        EditModeScripts::Clear(); // edit-mode scripts end as play starts (and start again after Stop)
        m_Ctx.RuntimeScene = m_Ctx.EditorScene->Clone();
        m_Ctx.Sections.RegisterLoaded(*m_Ctx.RuntimeScene); // scenes open alongside play as loaded additively
        m_MixerBeforePlay = AudioMixer::Groups();
        m_Ctx.RuntimeScene->OnRuntimeStart();
        m_Ctx.State = PlayState::Play;
        m_FocusGameView = 3;
        UpdateWindowTitle();
    }

    void EditorLayer::Stop()
    {
        if (!m_Ctx.IsPlaying()) return;
        SceneManager::CancelPendingLoad();
        m_Ctx.RuntimeScene->OnRuntimeStop();
        m_Ctx.RuntimeScene.reset();
        m_Ctx.State = PlayState::Edit;
        // Volumes and filters changed while playing (scripts, snapshot transitions) go back to their edit-time values.
        AudioMixer::CancelTransition();
        for (auto& g : AudioMixer::Groups())
            for (auto& before : m_MixerBeforePlay)
                if (before.Name == g.Name)
                {
                    g.VolumeDb = before.VolumeDb;
                    g.LowPassHz = before.LowPassHz;
                }
        AudioMixer::Apply();
        if (!m_Ctx.EditorScene->IsValid(m_Ctx.Selected)) m_Ctx.Select(Entity::Null());
        Input::StopAllVibration();
        Input::SetGameInputEnabled(true);
        ImGui::SetWindowFocus("Scene");
        UpdateWindowTitle();
    }

    void EditorLayer::TogglePause()
    {
        if (!m_Ctx.IsPlaying()) return;
        bool paused = m_Ctx.State == PlayState::Paused;
        m_Ctx.State = paused ? PlayState::Play : PlayState::Paused;
        m_Ctx.RuntimeScene->SetPaused(!paused);
    }

    // ---- Frame ------------------------------------------------------------------------------

    void EditorLayer::OnUpdate(float dt)
    {
        if (!m_Options.OpenSceneLater.empty() && ++m_AutomationFrames == 30)
        {
            fs::path p = m_Options.OpenSceneLater;
            m_Ctx.RequestOpenScene = p.is_relative() ? AssetManager::GetAssetsDirectory() / p : p;
        }
        if (m_Ctx.RequestOpenScene)
        {
            fs::path p = *m_Ctx.RequestOpenScene;
            m_Ctx.RequestOpenScene.reset();
            if (ConfirmDiscardChanges()) OpenScene(p);
        }
        if (m_Ctx.RequestOpenPrefab)
        {
            AssetHandle h = *m_Ctx.RequestOpenPrefab;
            std::optional<uint64_t> from = m_Ctx.RequestOpenPrefabFrom;
            m_Ctx.RequestOpenPrefab.reset();
            m_Ctx.RequestOpenPrefabFrom.reset();
            OpenPrefabMode(h, from);
        }
        if (m_LeavePrefabRequested)
        {
            m_LeavePrefabRequested = false;
            LeavePrefabMode();
        }
        // Prefab Mode auto save: write the asset once an edit is finished (no widget or gizmo held).
        if (m_Ctx.InPrefabMode() && m_Ctx.PrefabAutoSave && m_Ctx.Dirty && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing() &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            SavePrefabStage();

        ApplyPendingLayout();
        PollGameModule(dt);
        PollCSharp(dt);
        Autosave(dt);
        // Scene visibility follows the scene file: loaded when another opens, saved when it changes.
        {
            fs::path file = Project::GetDirectory() / ".dimension" / "SceneVisibility.json";
            std::string key = m_Ctx.ScenePath.generic_string();
            if (m_Ctx.InPrefabMode()) key = m_VisibilityScene; // the prefab is edited inside the scene's settings
            if (key != m_VisibilityScene)
            {
                m_VisibilityScene = key;
                m_Ctx.Visibility.Load(file, key);
            }
            if (!m_Options.IsolateEntities.empty())
            {
                // --isolate "A,B" (automation).
                std::vector<Entity> isolated;
                std::string names = m_Options.IsolateEntities;
                for (size_t start = 0; start <= names.size();)
                {
                    size_t comma = names.find(',', start);
                    if (comma == std::string::npos) comma = names.size();
                    isolated.push_back(m_Ctx.ActiveScene().FindEntityByName(names.substr(start, comma - start)));
                    start = comma + 1;
                }
                m_Ctx.Visibility.Isolate(m_Ctx.ActiveScene(), isolated);
                m_Options.IsolateEntities.clear();
            }
            if (!m_Options.HideEntities.empty())
            {
                // --hide "A,B" (automation): for this session only.
                std::string names = m_Options.HideEntities;
                for (size_t start = 0; start <= names.size();)
                {
                    size_t comma = names.find(',', start);
                    if (comma == std::string::npos) comma = names.size();
                    m_Ctx.Visibility.SetHidden(m_Ctx.ActiveScene(), m_Ctx.ActiveScene().FindEntityByName(names.substr(start, comma - start)), true);
                    start = comma + 1;
                }
                m_Options.HideEntities.clear();
            }
            if (m_Ctx.VisibilityChanged)
            {
                m_Ctx.VisibilityChanged = false;
                if (!key.empty()) m_Ctx.Visibility.Save(file, key);
            }
        }
        m_SceneTarget->Resize(m_SceneViewSize.x, m_SceneViewSize.y);
        m_GameTarget->Resize(m_GameViewSize.x, m_GameViewSize.y);

        // Pick up files changed outside the editor when the window regains focus.
        bool focused = Application::Get().GetWindow().IsFocused();
        if (focused && !m_WasWindowFocused)
        {
            AssetManager::Refresh();
            if (!m_Ctx.IsPlaying() && !m_Ctx.InPrefabMode())
            {
                // Prefab files may have changed outside the editor.
                auto before = SceneSerializer::Serialize(*m_Ctx.EditorScene);
                Prefabs::SyncAll(*m_Ctx.EditorScene);
                if (SceneSerializer::Serialize(*m_Ctx.EditorScene) != before) m_Ctx.Dirty = true;
            }
        }
        m_WasWindowFocused = focused;

        if (m_SceneViewHovered || m_Camera.IsControlling()) m_Camera.Update(dt, m_SceneViewHovered);

        Input::SetGameInputEnabled(m_Ctx.IsPlaying() && m_GameViewFocused);
        if (m_Ctx.IsPlaying())
        {
            m_Ctx.RuntimeScene->OnUpdateRuntime(dt);
            // SceneManager loads from scripts go into the play-mode scene; Stop returns to the edited scene.
            if (SceneManager::HasPendingLoad() && SceneManager::ApplyPending(*m_Ctx.RuntimeScene))
            {
                if (!m_Ctx.RuntimeScene->IsValid(m_Ctx.Selected)) m_Ctx.Select(Entity::Null());
                std::erase_if(m_Ctx.Selection, [&](Entity e) { return !m_Ctx.RuntimeScene->IsValid(e); });
                UpdateWindowTitle();
            }
        }
        else
        {
            m_Ctx.EditorScene->OnUpdateEditor(dt);
            // [ExecuteInEditMode] scripts of the scene being edited.
            EditModeScripts::Update(m_Ctx.ActiveScene(), dt);
            if (EditModeScripts::Count(&m_Ctx.ActiveScene()) > 0) m_Ctx.ActiveScene().UpdateTransforms();
        }

        if (auto pick = m_SceneTarget->PollPick())
        {
            Entity e = *pick ? m_Ctx.ActiveScene().GetRegistry().EntityFromIndex(*pick - 1) : Entity::Null();
            if (m_PickAdditive) m_Ctx.ToggleSelect(e);
            else m_Ctx.Select(e);
        }
        if (m_Ctx.RequestFocusSelected)
        {
            m_Ctx.RequestFocusSelected = false;
            FocusSelected();
        }

        m_StatsTimer += dt;
        if (m_StatsTimer > 0.25f)
        {
            m_DisplayFps = 1.0f / std::max(dt, 1e-5f);
            m_DisplayMs = dt * 1000.0f;
            m_StatsTimer = 0.0f;
        }
        static bool wasDirty = false;
        if (wasDirty != m_Ctx.Dirty)
        {
            wasDirty = m_Ctx.Dirty;
            UpdateWindowTitle();
        }
    }

    // ---- Prefab Mode ----------------------------------------------------------------------------

    void EditorLayer::OpenPrefabMode(AssetHandle prefab, std::optional<uint64_t> from)
    {
        if (m_Ctx.IsPlaying()) Stop();
        if (m_Ctx.InPrefabMode())
        {
            if (prefab == m_Ctx.PrefabModeAsset()) return;
            if (!LeavePrefabMode()) return;
        }
        auto stage = Prefabs::OpenForEditing(prefab);
        if (!stage)
        {
            Log::Error("Could not open the prefab for editing");
            return;
        }
        // In context: opened from an instance, the scene around it comes along (copied into a container placed so the
        // prefab stands where the instance does), shown greyed behind the prefab.
        m_Ctx.PrefabContext = Entity::Null();
        Entity instance = from ? m_Ctx.EditorScene->FindEntityByGuid(Guid(*from)) : Entity::Null();
        if (!instance.IsNull())
        {
            Scene& outer = *m_Ctx.EditorScene;
            glm::mat4 placed = outer.GetRegistry().Get<TransformComponent>(instance).World;
            Entity container = stage->CreateEntity("Scene Context");
            std::vector<std::pair<uint64_t, Entity>> mapping;
            for (Entity root : outer.GetRootEntities())
                SceneSerializer::DeserializeEntityTree(*stage, SceneSerializer::SerializeEntityTree(outer, root), container, true, &mapping);
            for (auto& [guid, copy] : mapping)
                if (guid == *from && stage->IsValid(copy)) stage->DestroyEntity(copy); // the prefab itself takes its place
            glm::vec3 scale, translation, skew;
            glm::vec4 perspective;
            glm::quat rotation;
            if (glm::decompose(glm::inverse(placed), scale, rotation, translation, skew, perspective))
            {
                auto& t = stage->GetRegistry().Get<TransformComponent>(container);
                t.Position = translation;
                t.Rotation = glm::eulerAngles(rotation);
                t.Scale = scale;
            }
            stage->UpdateTransforms();
            m_Ctx.PrefabContext = container;
        }
        m_SceneCamera = m_Camera;
        m_Ctx.EnterPrefabMode(prefab, stage);
        Entity root = Prefabs::FindEditedRoot(*stage, prefab);
        m_Ctx.Select(root);
        FocusSelected();
        m_FocusSceneView = 3; // prefabs are edited in the Scene view
        UpdateWindowTitle();
        Log::Info("Editing prefab '{}' (Prefab Mode)", AssetManager::GetDisplayName(prefab));
    }

    bool EditorLayer::SavePrefabStage()
    {
        if (!m_Ctx.InPrefabMode()) return false;
        if (!Prefabs::SaveEdited(*m_Ctx.EditorScene, m_Ctx.PrefabModeAsset()))
        {
            m_Ctx.PrefabAutoSave = false; // do not retry every frame
            return false;
        }
        m_Ctx.Dirty = false;
        UpdateWindowTitle();
        return true;
    }

    bool EditorLayer::LeavePrefabMode()
    {
        if (!m_Ctx.InPrefabMode()) return true;
        if (m_Ctx.Dirty)
        {
            int choice = m_Ctx.PrefabAutoSave ? 0
                       : FileDialogs::YesNoCancel("Unsaved Prefab", "The prefab '" + m_Ctx.EditorScene->GetName() + "' has unsaved changes. Save them?");
            if (choice == 2) return false;
            if (choice == 0 && !SavePrefabStage()) return false;
        }
        m_Ctx.ExitPrefabMode();
        // Instances in the scene pick up the edited prefab (their overrides are kept).
        auto before = SceneSerializer::Serialize(*m_Ctx.EditorScene);
        Prefabs::SyncAll(*m_Ctx.EditorScene);
        if (SceneSerializer::Serialize(*m_Ctx.EditorScene) != before) m_Ctx.Dirty = true;
        m_Camera = m_SceneCamera;
        UpdateWindowTitle();
        return true;
    }

    void EditorLayer::DrawPrefabModeBar()
    {
        // Breadcrumb like Unity's: back to the scene, the prefab's name, Save and Auto Save.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, UI::AccentBackground(0.35f));
        ImGui::BeginChild("##PrefabModeBar", ImVec2(0, ImGui::GetFrameHeight() + 8.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        if (ImGui::Button("< Scene")) m_LeavePrefabRequested = true; // handled next update, outside the panels
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s  >", m_Ctx.PrefabModeSceneName().c_str());
            ImGui::SameLine();
            ImGui::TextColored(UI::AccentText(), "%s%s", m_Ctx.EditorScene->GetName().c_str(), m_Ctx.Dirty ? "*" : "");
            float right = ImGui::CalcTextSize("Auto Save").x + ImGui::GetFrameHeight() + ImGui::CalcTextSize("Save").x + 40.0f;
            ImGui::SameLine(ImGui::GetWindowWidth() - right);
            ImGui::BeginDisabled(!m_Ctx.Dirty);
            if (ImGui::Button("Save")) SavePrefabStage();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Checkbox("Auto Save", &m_Ctx.PrefabAutoSave);
            if (m_Ctx.EditorScene->IsValid(m_Ctx.PrefabContext))
            {
                ImGui::SameLine(ImGui::GetWindowWidth() - right - ImGui::CalcTextSize("Context").x - ImGui::GetFrameHeight() - 24.0f);
                ImGui::Checkbox("Context", &m_Ctx.PrefabContextShown);
                ImGui::SetItemTooltip("The scene around the instance the prefab was opened from, greyed");
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    namespace
    {
        // The sphere around an object's point handles (false: it has none).
        bool HandleBounds(Dimension::Scene& scene, Dimension::Entity e, glm::vec3& center, float& radius);
    }

    void EditorLayer::FocusSelected()
    {
        if (!m_Ctx.HasValidSelection()) return;
        Scene& scene = m_Ctx.ActiveScene();
        auto& t = scene.GetRegistry().Get<TransformComponent>(m_Ctx.Selected);
        float radius = 1.0f;
        glm::vec3 center = t.GetWorldPosition();
        if (auto* mr = scene.GetRegistry().TryGet<MeshRendererComponent>(m_Ctx.Selected))
            if (auto mesh = AssetManager::GetMesh(mr->Mesh))
            {
                center = glm::vec3(t.World * glm::vec4(mesh->GetBounds().Center(), 1.0f));
                radius = glm::length(mesh->GetBounds().Extents() * glm::abs(t.Scale));
            }
        // Objects drawn only by handles (links, paths, obstacles): frame the handles.
        if (!scene.GetRegistry().Has<MeshRendererComponent>(m_Ctx.Selected)) HandleBounds(scene, m_Ctx.Selected, center, radius);
        m_Camera.Focus(center, radius);
    }

    SelectionSnapshot EditorLayer::CaptureSelection() { return m_Ctx.CaptureSelection(); }

    bool EditorLayer::CanApplySelection(const SelectionSnapshot& s) { return m_Ctx.CanApplySelection(s); }

    bool EditorLayer::ApplySelection(const SelectionSnapshot& s)
    {
        if (!m_Ctx.ApplySelection(s)) return false;
        // Selected assets: their folder in the Project window.
        if (s.Objects.empty())
            if (const AssetMetadata* m = AssetManager::GetMetadata(m_Ctx.SelectedAsset)) m_Content->ShowDirectory((AssetManager::GetAssetsDirectory() / m->Path).parent_path());
        return true;
    }

    void EditorLayer::SelectionBack()
    {
        if (auto s = m_SelectionHistory.Back([this](const SelectionSnapshot& o) { return CanApplySelection(o); }); s && ApplySelection(*s))
            m_SelectionHistory.ReplaceCurrent(CaptureSelection()); // as restored (not a new step)
    }

    void EditorLayer::SelectionForward()
    {
        if (auto s = m_SelectionHistory.Forward([this](const SelectionSnapshot& o) { return CanApplySelection(o); }); s && ApplySelection(*s))
            m_SelectionHistory.ReplaceCurrent(CaptureSelection());
    }

    void EditorLayer::SaveSelection(int slot)
    {
        SelectionSnapshot s = CaptureSelection();
        if (s.Empty())
        {
            Log::Info("Nothing is selected to save as selection {}", slot);
            return;
        }
        SelectionSets::Set(m_Ctx.ScenePath, slot, s);
        Log::Info("Saved the selection as selection {} (Alt+{} selects it again)", slot, slot);
    }

    void EditorLayer::RestoreSelection(int slot)
    {
        auto s = SelectionSets::Get(m_Ctx.ScenePath, slot);
        if (!s) Log::Info("No selection saved as selection {} (Ctrl+Alt+{} saves one)", slot, slot);
        else if (!ApplySelection(*s)) Log::Info("Nothing of selection {} is left", slot);
    }

    void EditorLayer::DrawSelectionMenu()
    {
        if (ImGui::MenuItem("Back", Shortcuts::Label("edit.selectBack").c_str(), false, m_SelectionHistory.CanGoBack())) SelectionBack();
        if (ImGui::MenuItem("Forward", Shortcuts::Label("edit.selectForward").c_str(), false, m_SelectionHistory.CanGoForward())) SelectionForward();
        ImGui::Separator();
        auto saved = SelectionSets::Slots(m_Ctx.ScenePath);
        bool any = !CaptureSelection().Empty();
        for (int slot = 1; slot <= SelectionSets::kSlots; ++slot)
        {
            std::string n = std::to_string(slot);
            bool has = std::find(saved.begin(), saved.end(), slot) != saved.end();
            if (ImGui::BeginMenu(("Selection " + n + (has ? "" : " (empty)")).c_str()))
            {
                if (ImGui::MenuItem("Select", Shortcuts::Label(("edit.selectionSet" + n).c_str()).c_str(), false, has)) RestoreSelection(slot);
                if (ImGui::MenuItem(has ? "Replace with Selection" : "Save Selection", Shortcuts::Label(("edit.saveSelectionSet" + n).c_str()).c_str(), false, any))
                    SaveSelection(slot);
                if (ImGui::MenuItem("Clear", nullptr, false, has)) SelectionSets::Clear(m_Ctx.ScenePath, slot);
                ImGui::EndMenu();
            }
        }
    }

    GridSettings EditorLayer::SceneGridSettings()
    {
        const auto& prefs = EditorPreferences::Get();
        GridSettings grid;
        grid.Plane = (GridPlane)std::clamp(prefs.GridPlane, 0, 2);
        grid.CellSize = prefs.GridCellSize;
        grid.Opacity = prefs.GridOpacity;
        // At the selected object's height (its position along the plane's normal).
        Scene& scene = m_Ctx.ActiveScene();
        if (prefs.GridFollowSelection && m_Ctx.HasValidSelection())
            grid.Offset = scene.GetRegistry().Get<TransformComponent>(m_Ctx.Selected).GetWorldPosition()[SceneGrid::NormalAxis(grid.Plane)];
        return grid;
    }

    void EditorLayer::DrawGridMenu()
    {
        auto& prefs = EditorPreferences::Get();
        bool changed = false;
        ImGui::TextDisabled("Plane");
        const char* planes[] = { "XZ (ground)", "XY", "YZ" };
        for (int i = 0; i < 3; ++i) changed |= ImGui::RadioButton(planes[i], &prefs.GridPlane, i);
        if (m_Camera.Is2D()) ImGui::TextDisabled("The 2D view always shows the XY grid");
        ImGui::Separator();
        bool automatic = prefs.GridCellSize <= 0.0f;
        if (ImGui::Checkbox("Cell size adapts to the view", &automatic))
        {
            prefs.GridCellSize = automatic ? 0.0f : 1.0f;
            changed = true;
        }
        if (!automatic)
        {
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::DragFloat("Cell Size", &prefs.GridCellSize, 0.05f, 0.01f, 1000.0f, "%.2f m")) prefs.GridCellSize = std::max(prefs.GridCellSize, 0.01f), changed = true;
        }
        ImGui::SetNextItemWidth(140.0f);
        changed |= ImGui::SliderFloat("Opacity", &prefs.GridOpacity, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::Checkbox("Follow selection's height", &prefs.GridFollowSelection);
        ImGui::SetItemTooltip("The grid moves along its normal to the selected object (the ground grid to its Y)");
        if (changed) prefs.Save();
    }

    void EditorLayer::NewInspector(bool lock)
    {
        int n = m_NextInspector++;
        std::string title = "Inspector " + std::to_string(n) + "###Inspector" + std::to_string(n);
        ExtraInspector extra{ MakeScope<InspectorPanel>(m_Ctx, title, n), true };
        extra.Panel->DebugMode = m_Inspector->DebugMode;
        if (lock) extra.Panel->LockToSelection();
        m_ExtraInspectors.push_back(std::move(extra));
    }

    void EditorLayer::MoveSelectionToView()
    {
        // To the point the Scene view orbits (the middle of the view).
        auto roots = m_Ctx.SelectedRoots();
        if (roots.empty()) return;
        m_Ctx.RecordUndo("Move To View");
        ViewAlign::MoveTo(m_Ctx.ActiveScene(), roots, m_Camera.GetPosition() + m_Camera.GetForward() * m_Camera.GetDistance());
    }

    void EditorLayer::AlignSelectionWithView()
    {
        auto roots = m_Ctx.SelectedRoots();
        if (roots.empty()) return;
        m_Ctx.RecordUndo("Align With View");
        ViewAlign::AlignWithView(m_Ctx.ActiveScene(), roots, glm::inverse(m_Camera.GetView()));
    }

    void EditorLayer::AlignViewToSelected()
    {
        if (!m_Ctx.HasValidSelection()) return;
        if (m_Camera.Is2D()) m_Camera.Set2D(false); // a 3D view from it
        m_Camera.SetFromView(ViewAlign::ViewFrom(m_Ctx.ActiveScene(), m_Ctx.Selected));
    }

    void EditorLayer::SetMeasuring(bool on)
    {
        m_Measuring = on;
        m_MeasureA.reset();
        m_MeasureB.reset();
        if (on) m_FocusSceneView = 2; // its clicks are in the Scene view
    }

    void EditorLayer::FrameAll()
    {
        // What the Scene view shows (hidden and isolated-away objects left out).
        Scene& scene = m_Ctx.ActiveScene();
        auto hidden = m_Ctx.Visibility.Hidden(scene); // isolation included; empty while the toggle is off
        auto skip = [&](Entity e) { return std::find(hidden.begin(), hidden.end(), e) != hidden.end(); };
        if (auto box = SceneBounds::World(scene, skip)) m_Camera.Focus(box->Center(), std::max(glm::length(box->Extents()), 0.5f));
    }

    void EditorLayer::GoToView(int slot)
    {
        if (auto b = SceneBookmarks::Get(m_Ctx.ScenePath, slot)) m_Camera.SetBookmark(*b);
        else Log::Info("No Scene view saved as view {} (Ctrl+Shift+{} saves one)", slot, slot);
    }

    void EditorLayer::SaveView(int slot)
    {
        SceneBookmarks::Set(m_Ctx.ScenePath, slot, m_Camera.GetBookmark());
        Log::Info("Saved the Scene view as view {} (Ctrl+{} goes back to it)", slot, slot);
    }

    void EditorLayer::DrawViewsMenu()
    {
        // Frame All, and the nine view slots: Go (when saved), Save the current view there, Clear.
        if (ImGui::MenuItem("Frame All", Shortcuts::Label("scene.frameAll").c_str())) FrameAll();
        ImGui::Separator();
        auto saved = SceneBookmarks::Slots(m_Ctx.ScenePath);
        if (ImGui::BeginTable("##views", 4, ImGuiTableFlags_SizingFixedFit))
        {
            for (int slot = 1; slot <= SceneBookmarks::kSlots; ++slot)
            {
                bool has = std::find(saved.begin(), saved.end(), slot) != saved.end();
                std::string n = std::to_string(slot);
                ImGui::PushID(slot);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!has);
                if (ImGui::Selectable(("View " + n).c_str(), false, ImGuiSelectableFlags_DontClosePopups)) GoToView(slot);
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", Shortcuts::Label(("scene.view" + n).c_str()).c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(has ? "Replace" : "Save")) SaveView(slot);
                ImGui::SetItemTooltip("Save the current view here (%s)", Shortcuts::Label(("scene.saveView" + n).c_str()).c_str());
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!has);
                if (ImGui::SmallButton("Clear")) SceneBookmarks::Clear(m_Ctx.ScenePath, slot);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled(m_Ctx.ScenePath.empty() ? "Unsaved scene: its views are shared by all unsaved scenes" : "Kept per user for this scene");
    }

    void EditorLayer::OnRender(VkCommandBuffer cmd)
    {
        m_AssetPreviews->Render(cmd);
        Scene& scene = m_Ctx.ActiveScene();
        if (m_SceneViewVisible)
        {
            CameraData cam;
            cam.View = m_Camera.GetView();
            cam.Projection = m_Camera.GetProjection(m_SceneTarget->GetAspect());
            cam.Position = m_Camera.GetPosition();
            cam.ClearColor = { 0.19f, 0.19f, 0.2f, 1.0f };
            cam.Orthographic = m_Camera.Is2D();
            // Scene view uses the skybox-ish color of the primary camera if there is one.
            Entity primary = scene.GetPrimaryCamera();
            if (!primary.IsNull())
            {
                auto& pc = scene.GetRegistry().Get<CameraComponent>(primary);
                cam.ClearColor = pc.ClearColor;
                cam.Skybox = pc.Clear == ClearMode::Skybox && !m_Camera.Is2D();
            }
            else
                cam.Skybox = !m_Camera.Is2D();
            // Wireframe and Overdraw show on a plain dark background.
            if (m_DrawMode == SceneDrawMode::Wireframe || m_DrawMode == SceneDrawMode::Overdraw)
            {
                cam.ClearColor = m_DrawMode == SceneDrawMode::Overdraw ? glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) : glm::vec4(0.11f, 0.11f, 0.12f, 1.0f);
                cam.Skybox = false;
            }

            SceneRenderOptions options;
            options.EditorOverlays = true;
            options.Grid = m_ShowGrid;
            options.Grid2D = m_Camera.Is2D();
            options.GridConfig = SceneGridSettings();
            options.Selected = m_Ctx.Selected;
            options.Selection = m_Ctx.Selection;
            options.DefaultLight = m_Ctx.InPrefabMode();
            DIM_PROFILE_SCOPE("Scene View");
            options.ShowNavMeshes = m_ShowNavigation;
            options.DrawMode = m_DrawMode;
            options.Hidden = m_Ctx.Visibility.Hidden(scene);
            options.Unpickable = m_Ctx.Visibility.Unpickable(scene);
            // Prefab Mode in context: the scene around the prefab, greyed and not picked (or hidden).
            if (m_Ctx.InPrefabMode() && scene.IsValid(m_Ctx.PrefabContext))
            {
                std::vector<Entity> context{ m_Ctx.PrefabContext };
                for (size_t i = 0; i < context.size(); ++i)
                    for (Entity c : scene.GetChildren(context[i])) context.push_back(c);
                auto& into = m_Ctx.PrefabContextShown ? options.Greyed : options.Hidden;
                into.insert(into.end(), context.begin(), context.end());
                if (m_Ctx.PrefabContextShown) options.Unpickable.insert(options.Unpickable.end(), context.begin(), context.end());
            }
            options.FrameDebugStop = m_ShowFrameDebugger ? m_FrameDebugger->StopFor(0) : -2;
            options.Highlighting = m_Ctx.HierarchySearch.find_first_not_of(' ') != std::string::npos;
            if (options.Highlighting) options.Highlight = m_Ctx.SearchMatches();
            // C# scripts' gizmos (OnDrawGizmos, and OnDrawGizmosSelected for the selection).
            if (m_ShowGizmos)
                for (const auto& g : CSharpRuntime::CollectGizmos(scene, m_Ctx.Selection)) m_SceneRenderer.DrawLine(g.A, g.B, g.Color);
            int timer = RenderDevice::Get().BeginGpuTimer(cmd, "Scene View");
            m_SceneRenderer.Render(cmd, scene, cam, *m_SceneTarget, options);
            RenderDevice::Get().EndGpuTimer(cmd, timer);
        }
        if (m_GameViewVisible)
        {
            Entity camera = scene.GetPrimaryCamera();
            CameraData cam;
            cam.ClearColor = { 0, 0, 0, 1 };
            if (!camera.IsNull()) cam = CameraData::FromEntity(scene, camera, m_GameTarget->GetAspect());
            SceneRenderOptions gameOptions;
            gameOptions.RenderUI = true;
            gameOptions.FrameDebugStop = m_ShowFrameDebugger ? m_FrameDebugger->StopFor(1) : -2;
            {
                DIM_PROFILE_SCOPE("Game View");
                int timer = RenderDevice::Get().BeginGpuTimer(cmd, "Game View");
                m_GameRenderer.Render(cmd, scene, cam, *m_GameTarget, gameOptions);
                RenderDevice::Get().EndGpuTimer(cmd, timer);
            }
            const RenderStats& st = m_GameRenderer.GetStats();
            Profiler::SetCounter("Draw Calls", (float)st.DrawCalls);
            Profiler::SetCounter("Triangles", (float)st.Triangles);
            Profiler::SetCounter("Meshes", (float)st.Meshes);
            Profiler::SetCounter("Occluded", (float)st.Occluded);
        }
    }

    namespace
    {
        // The points edited with handles in the Scene view: a Line Renderer's, or the curve of a Terrain Path or a
        // Path Placer (a list: points can be inserted and removed), or fixed handles: an Off Mesh Link's ends, a NavMesh
        // Obstacle's center and size (box) or height and radius (capsule). In the object's space unless UseWorldSpace.
        struct EditablePoints
        {
            std::vector<glm::vec3> Positions;
            bool UseWorldSpace = false, Loop = false;
            const char* Component = "";
            std::vector<const char*> Fields; // per handle (one entry: the list's field)
            std::vector<const char*> Names;  // per handle, for the tooltip (empty: numbered points)
            std::vector<glm::vec3>* List = nullptr;
            std::function<void(size_t, const glm::vec3&)> Set;
            const char* FieldOf(size_t i) const { return Fields[std::min(i, Fields.size() - 1)]; }
        };
        EditablePoints ListPoints(std::vector<glm::vec3>& list, bool world, bool loop, const char* component, const char* field)
        {
            EditablePoints p;
            p.Positions = list;
            p.UseWorldSpace = world;
            p.Loop = loop;
            p.Component = component;
            p.Fields = { field };
            p.List = &list;
            p.Set = [&list](size_t i, const glm::vec3& v) { list[i] = v; };
            return p;
        }
        std::optional<EditablePoints> EditablePointsOf(Dimension::Scene& scene, Dimension::Entity e)
        {
            auto& reg = scene.GetRegistry();
            if (auto* line = reg.TryGet<Dimension::LineRendererComponent>(e)) return ListPoints(line->Positions, line->UseWorldSpace, line->Loop, "Line Renderer", "Positions");
            if (auto* path = reg.TryGet<Dimension::TerrainPathComponent>(e)) return ListPoints(path->Points, false, false, "Terrain Path", "Points");
            if (auto* placer = reg.TryGet<Dimension::PathPlacerComponent>(e)) return ListPoints(placer->Points, false, placer->Closed, "Path Placer", "Points");
            if (auto* link = reg.TryGet<Dimension::OffMeshLinkComponent>(e))
            {
                EditablePoints p;
                p.Positions = { link->Start, link->End };
                p.Component = "Off Mesh Link";
                p.Fields = p.Names = { "Start", "End" };
                p.Set = [link](size_t i, const glm::vec3& v) { (i == 0 ? link->Start : link->End) = v; };
                return p;
            }
            if (auto* ob = reg.TryGet<Dimension::NavMeshObstacleComponent>(e))
            {
                EditablePoints p;
                p.Component = "NavMesh Obstacle";
                if (ob->Shape == Dimension::NavObstacleShape::Box)
                {
                    p.Positions = { ob->Center, ob->Center + ob->Size * 0.5f };
                    p.Fields = { "Center", "Size" };
                    p.Names = { "Center", "Corner (Size)" };
                    p.Set = [ob](size_t i, const glm::vec3& v) {
                        if (i == 0) ob->Center = v;
                        else ob->Size = glm::max(glm::abs(v - ob->Center) * 2.0f, glm::vec3(0.01f));
                    };
                }
                else
                {
                    p.Positions = { ob->Center, ob->Center + glm::vec3(0.0f, ob->Height * 0.5f, 0.0f), ob->Center + glm::vec3(ob->Radius, 0.0f, 0.0f) };
                    p.Fields = { "Center", "Height", "Radius" };
                    p.Names = { "Center", "Top (Height)", "Side (Radius)" };
                    p.Set = [ob](size_t i, const glm::vec3& v) {
                        if (i == 0) ob->Center = v;
                        else if (i == 1) ob->Height = std::max(2.0f * (v.y - ob->Center.y), 0.01f);
                        else ob->Radius = std::max(glm::length(glm::vec2(v.x - ob->Center.x, v.z - ob->Center.z)), 0.01f);
                    };
                }
                return p;
            }
            return std::nullopt;
        }

        bool HandleBounds(Dimension::Scene& scene, Dimension::Entity e, glm::vec3& center, float& radius)
        {
            auto points = EditablePointsOf(scene, e);
            if (!points || points->Positions.empty()) return false;
            const glm::mat4& world = scene.GetRegistry().Get<Dimension::TransformComponent>(e).World;
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (const glm::vec3& p : points->Positions)
            {
                glm::vec3 w = points->UseWorldSpace ? p : glm::vec3(world * glm::vec4(p, 1.0f));
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
            center = (lo + hi) * 0.5f;
            radius = std::max(glm::length(hi - lo) * 0.5f, 1.0f);
            return true;
        }
    }

    bool EditorLayer::SwitchProject(const fs::path& dir)
    {
        if (!ConfirmDiscardChanges()) return false;
        if (m_Ctx.IsPlaying()) Stop();
        ImGui::SaveIniSettingsToDisk(m_IniPath.c_str());
        m_Ctx.EditorScene.reset();
        m_Ctx.Sections.Clear();
        if (!Project::Open(dir))
        {
            Log::Error("Could not open the project '{}'", dir.string());
            return false;
        }
        fs::path settingsDir = Project::GetDirectory() / ".dimension";
        fs::create_directories(settingsDir);
        m_IniPath = (settingsDir / "EditorLayout.ini").string();
        ImGui::GetIO().IniFilename = m_IniPath.c_str();
        if (fs::exists(m_IniPath)) ImGui::LoadIniSettingsFromDisk(m_IniPath.c_str());
        m_Content = MakeScope<ContentBrowserPanel>(m_Ctx);
        fs::path start = AssetManager::GetAbsolutePath(Project::GetConfig().StartScene);
        if (!start.empty() && fs::exists(start)) OpenScene(start);
        else NewScene();
        ProjectHub::Remember(Project::GetDirectory());
        UpdateWindowTitle();
        return true;
    }

    void EditorLayer::DrawProjectHub()
    {
        // The projects opened before (newest first; open one, or take it off the list), a new project from a starter
        // scene, and any other project folder.
        ImGui::SetNextWindowSize(ImVec2(820.0f, 480.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Project Hub", &m_ShowHub))
        {
            ImGui::End();
            return;
        }
        std::optional<fs::path> open;
        ImGui::BeginChild("##recent", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0.0f), ImGuiChildFlags_Borders);
        ImGui::SeparatorText("Recent Projects");
        auto recent = ProjectHub::Recent();
        if (ImGui::BeginTable("##projects", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Opened", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, 24.0f);
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            for (size_t i = 0; i < recent.size(); ++i)
            {
                const auto& p = recent[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool current = p.Exists && fs::equivalent(p.Path, Project::GetDirectory());
                ImGui::BeginDisabled(!p.Exists);
                std::string label = p.Name + (current ? "  (open)" : "") + (p.Exists ? "" : "  (missing)");
                if (ImGui::Selectable(label.c_str(), current, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap) && !current &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    open = p.Path;
                ImGui::EndDisabled();
                ImGui::TextDisabled("%s", p.Path.string().c_str());
                ImGui::TableSetColumnIndex(1);
                int64_t ago = std::max<int64_t>(now - p.LastOpened, 0);
                std::string when = ago < 3600 ? "just now" : ago < 86400 ? std::to_string(ago / 3600) + " h ago" : std::to_string(ago / 86400) + " days ago";
                ImGui::TextDisabled("%s", when.c_str());
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("x")) ProjectHub::Forget(p.Path);
                ImGui::SetItemTooltip("Take it off the list (the project stays on disk)");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (recent.empty()) ImGui::TextDisabled("Projects you open show here.");
        ImGui::TextDisabled("Double-click a project to open it.");
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##new", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        ImGui::SeparatorText("New Project");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##name", "Name", &m_HubName);
        if (m_HubLocation.empty()) m_HubLocation = Project::GetDirectory().parent_path().string();
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 1.6f);
        ImGui::InputTextWithHint("##location", "Location", &m_HubLocation);
        ImGui::SameLine();
        if (ImGui::Button("..."))
            if (auto dir = FileDialogs::PickFolder(m_HubLocation)) m_HubLocation = dir->string();
        auto templates = SceneTemplates::All();
        std::vector<std::string> builtins;
        for (const auto& t : templates)
            if (!t.Scene.IsValid()) builtins.push_back(t.Name);
        m_HubStarter = std::clamp(m_HubStarter, 0, (int)builtins.size() - 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##starter", ("Start with: " + builtins[m_HubStarter]).c_str()))
        {
            for (int i = 0; i < (int)builtins.size(); ++i)
                if (ImGui::Selectable(builtins[i].c_str(), i == m_HubStarter)) m_HubStarter = i;
            ImGui::EndCombo();
        }
        fs::path target = fs::path(m_HubLocation) / m_HubName;
        bool named = m_HubName.find_first_not_of(' ') != std::string::npos;
        bool taken = named && ProjectHub::IsProject(target);
        if (named) ImGui::TextDisabled("%s", target.string().c_str());
        if (taken) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "A project is already there.");
        ImGui::BeginDisabled(!named || taken || m_Ctx.IsPlaying());
        if (ImGui::Button("Create and Open", ImVec2(-FLT_MIN, 0.0f)) && ConfirmDiscardChanges())
        {
            if (ProjectHub::Create(target, m_HubName, builtins[m_HubStarter]))
            {
                Log::Info("Created project '{}'", m_HubName);
                open = target;
                m_HubName.clear();
            }
            else Log::Error("Could not create a project in '{}'", target.string());
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::SeparatorText("Other");
        if (ImGui::Button("Open Project Folder...", ImVec2(-FLT_MIN, 0.0f)))
            if (auto dir = FileDialogs::PickFolder(Project::GetDirectory()))
            {
                if (ProjectHub::IsProject(*dir) || FileDialogs::Confirm("Open Project", "'" + dir->string() + "' has no project yet. Make it one?"))
                    open = *dir;
            }
        ImGui::EndChild();
        ImGui::End();
        if (open && SwitchProject(*open)) m_ShowHub = false;
    }

    std::vector<std::pair<const char*, bool*>> EditorLayer::WindowToggles()
    {
        return { { "Hierarchy", &m_ShowHierarchy }, { "Inspector", &m_ShowInspector }, { "Project", &m_ShowContent }, { "Console", &m_ShowConsole },
                 { "Scene Settings", &m_ShowSceneSettings }, { "Lighting", &m_ShowLighting }, { "Animator", &m_ShowAnimator },
                 { "Timeline", &m_ShowTimeline }, { "Navigation", &m_ShowNavigation }, { "Profiler", &m_ShowProfiler },
                 { "Frame Debugger", &m_ShowFrameDebugger }, { "Sprite Editor", &m_ShowSpriteEditor }, { "Snap Settings", &m_ShowSnapSettings },
                 { "Audio Mixer", &m_ShowAudioMixer }, { "Tile Palette", &m_ShowTilePalette }, { "Stats", &m_ShowStats },
                 { "Shortcuts", &m_ShowShortcuts }, { "Undo History", &m_ShowUndoHistory } };
    }

    // Named layouts: the window arrangement (ImGui's settings) and which windows are open, in .dimension/Layouts.
    fs::path EditorLayer::LayoutsFolder() const { return Project::GetDirectory() / ".dimension" / "Layouts"; }

    std::vector<std::string> EditorLayer::LayoutNames() const
    {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(LayoutsFolder(), ec))
            if (e.path().extension() == ".ini") names.push_back(e.path().stem().string());
        std::sort(names.begin(), names.end());
        return names;
    }

    void EditorLayer::SaveLayout(const std::string& name)
    {
        std::error_code ec;
        fs::create_directories(LayoutsFolder(), ec);
        std::ofstream(LayoutsFolder() / (name + ".ini")) << ImGui::SaveIniSettingsToMemory();
        nlohmann::json open = nlohmann::json::object();
        for (auto& [window, flag] : WindowToggles()) open[window] = *flag;
        std::ofstream(LayoutsFolder() / (name + ".json")) << open.dump(2);
        Log::Info("Saved layout '{}'", name);
    }

    void EditorLayer::ApplyPendingLayout()
    {
        // Between frames (ImGui takes new settings only then): "Default" is the built-in arrangement.
        if (m_PendingLayout.empty()) return;
        std::string name = std::move(m_PendingLayout);
        m_PendingLayout.clear();
        if (name == "Default")
        {
            m_ResetLayout = true;
            return;
        }
        std::ifstream ini(LayoutsFolder() / (name + ".ini"));
        if (!ini) return;
        std::stringstream text;
        text << ini.rdbuf();
        std::string settings = text.str();
        ImGui::LoadIniSettingsFromMemory(settings.c_str(), settings.size());
        std::ifstream flags(LayoutsFolder() / (name + ".json"));
        if (flags)
            try
            {
                auto open = nlohmann::json::parse(flags);
                for (auto& [window, flag] : WindowToggles())
                    if (open.contains(window)) *flag = open[window].get<bool>();
            }
            catch (const std::exception&)
            {
            }
        Log::Info("Layout '{}'", name);
    }

    void EditorLayer::DrawLayoutsMenu()
    {
        if (!ImGui::BeginMenu("Layouts")) return;
        if (ImGui::MenuItem("Default")) m_PendingLayout = "Default";
        auto names = LayoutNames();
        for (const auto& n : names)
            if (ImGui::MenuItem(n.c_str())) m_PendingLayout = n;
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputTextWithHint("##layoutName", "Name", &m_LayoutName);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_LayoutName.find_first_not_of(' ') == std::string::npos || m_LayoutName == "Default");
        if (ImGui::Button("Save Layout"))
        {
            SaveLayout(m_LayoutName);
            m_LayoutName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (!names.empty() && ImGui::BeginMenu("Delete Layout"))
        {
            for (const auto& n : names)
                if (ImGui::MenuItem(n.c_str()))
                {
                    std::error_code ec;
                    fs::remove(LayoutsFolder() / (n + ".ini"), ec);
                    fs::remove(LayoutsFolder() / (n + ".json"), ec);
                }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    void EditorLayer::DrawUndoHistory()
    {
        // Every step, oldest first, named after what it did; the steps undone below the current one (greyed). Clicking
        // a step goes back or forward to just after it ("Start": before them all).
        ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Undo History", &m_ShowUndoHistory))
        {
            ImGui::End();
            return;
        }
        // The current step's name follows the scene as it changes: refreshed now and then.
        if (m_HistoryCache.empty() || ImGui::GetFrameCount() - m_HistoryFrame > 20 || m_HistoryDone != m_Ctx.HistoryDone() ||
            m_HistoryCount != m_HistoryCache.size())
        {
            m_HistoryCache = m_Ctx.History();
            m_HistoryFrame = ImGui::GetFrameCount();
            m_HistoryDone = m_Ctx.HistoryDone();
            m_HistoryCount = m_HistoryCache.size();
        }
        std::optional<size_t> jump;
        ImGui::BeginDisabled(m_Ctx.IsPlaying());
        if (ImGui::Selectable("Start", m_HistoryDone == 0)) jump = 0;
        for (size_t i = 0; i < m_HistoryCache.size(); ++i)
        {
            const auto& step = m_HistoryCache[i];
            ImGui::PushID((int)i);
            if (step.Undone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(step.Name.c_str(), i + 1 == m_HistoryDone)) jump = i + 1;
            if (step.Undone) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (m_HistoryCache.empty()) ImGui::TextDisabled("Changes to the scene show here.");
        if (m_Ctx.IsPlaying()) ImGui::TextDisabled("Play mode changes are not recorded.");
        ImGui::End();
        if (jump && *jump != m_Ctx.HistoryDone())
        {
            m_Ctx.JumpTo(*jump);
            m_HistoryCache.clear();
        }
    }

    void EditorLayer::DrawNoteLabels(const ImVec2& origin, const ImVec2& size)
    {
        // Notes (Note component, Show In Scene) as labels at their objects: the first line, all of it when hovered.
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        glm::mat4 viewProj = m_Camera.GetProjection(size.x / std::max(1.0f, size.y)) * m_Camera.GetView();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 mouse = ImGui::GetMousePos();
        for (auto [e, note, t] : reg.ViewOf<NoteComponent, TransformComponent>())
        {
            if (!note.ShowInScene || note.Text.empty() || !t.ActiveInHierarchy) continue;
            glm::vec4 clip = viewProj * glm::vec4(t.GetWorldPosition(), 1.0f);
            if (clip.w <= 1e-4f) continue;
            ImVec2 at(origin.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
            if (at.x < origin.x || at.y < origin.y || at.x > origin.x + size.x || at.y > origin.y + size.y) continue;
            std::string first = note.Text.substr(0, note.Text.find('\n'));
            ImVec2 textSize = ImGui::CalcTextSize(first.c_str());
            ImVec2 box(at.x + 10.0f, at.y - textSize.y - 14.0f);
            bool hovered = mouse.x >= box.x && mouse.x <= box.x + textSize.x + 12.0f && mouse.y >= box.y && mouse.y <= box.y + textSize.y + 8.0f;
            const std::string& shown = hovered ? note.Text : first;
            ImVec2 full = ImGui::CalcTextSize(shown.c_str(), nullptr, false, 320.0f);
            ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(note.Color.r, note.Color.g, note.Color.b, 0.92f));
            dl->AddLine(at, ImVec2(box.x, box.y + textSize.y + 8.0f), color, 1.5f);
            dl->AddRectFilled(box, ImVec2(box.x + full.x + 12.0f, box.y + full.y + 8.0f), color, 4.0f);
            dl->AddText(nullptr, 0.0f, ImVec2(box.x + 6.0f, box.y + 4.0f), IM_COL32(25, 25, 25, 255), shown.c_str(), nullptr, 320.0f);
        }
    }

    void EditorLayer::DrawNewSceneWindow()
    {
        // The templates a new scene starts from (the built-in ones, then the project's: scenes labelled Template), and
        // saving the open scene as one.
        ImGui::SetNextWindowSize(ImVec2(620.0f, 440.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("New Scene", &m_ShowNewScene))
        {
            ImGui::End();
            return;
        }
        auto templates = SceneTemplates::All();
        m_NewSceneTemplate = std::clamp(m_NewSceneTemplate, 0, (int)templates.size() - 1);
        bool create = false;
        float footer = ImGui::GetFrameHeightWithSpacing() * 3.0f + ImGui::GetStyle().ItemSpacing.y * 4.0f;
        ImGui::BeginChild("##templates", ImVec2(220.0f, -footer), ImGuiChildFlags_Borders);
        bool projectHeader = false;
        for (int i = 0; i < (int)templates.size(); ++i)
        {
            if (templates[i].Scene.IsValid() && !projectHeader)
            {
                ImGui::SeparatorText("Project");
                projectHeader = true;
            }
            if (ImGui::Selectable(templates[i].Name.c_str(), i == m_NewSceneTemplate, ImGuiSelectableFlags_AllowDoubleClick))
            {
                m_NewSceneTemplate = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) create = true;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##about", ImVec2(0.0f, -footer));
        const auto& chosen = templates[m_NewSceneTemplate];
        ImGui::TextUnformatted(chosen.Name.c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", chosen.Description.c_str());
        ImGui::PopStyleColor();
        if (chosen.Scene.IsValid())
        {
            ImGui::Spacing();
            ImGui::TextDisabled("A copy of %s (its objects get new identities).", AssetManager::GetDisplayName(chosen.Scene).c_str());
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(m_Ctx.IsPlaying());
        if (ImGui::Button("Create", ImVec2(120.0f, 0.0f))) create = true;
        ImGui::EndDisabled();
        ImGui::Separator();
        // The open scene as a project template.
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputTextWithHint("##templateName", "Template name", &m_TemplateName);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##templateAbout", "Description", &m_TemplateDescription);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_TemplateName.find_first_not_of(' ') == std::string::npos || m_Ctx.IsPlaying() || m_Ctx.InPrefabMode());
        if (ImGui::Button("Save Open Scene as Template"))
        {
            if (SceneTemplates::SaveAsTemplate(*m_Ctx.EditorScene, m_TemplateName, m_TemplateDescription).IsValid())
                Log::Info("Saved scene template '{}'", m_TemplateName);
            m_TemplateName.clear();
            m_TemplateDescription.clear();
        }
        ImGui::EndDisabled();
        ImGui::End();
        if (create && !m_Ctx.IsPlaying() && ConfirmDiscardChanges())
        {
            NewSceneFrom(chosen);
            m_ShowNewScene = false;
        }
    }

    void EditorLayer::DrawShortcutsWindow()
    {
        // Every shortcut by category: its key, a button to bind a new one (the next key pressed, with its modifiers;
        // Escape cancels) and one to restore the default. Keys used twice show in orange.
        ImGui::SetNextWindowSize(ImVec2(560.0f, 560.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Shortcuts", &m_ShowShortcuts))
        {
            ImGui::End();
            return;
        }
        auto& all = Shortcuts::All();
        bool changed = false;
        if (m_RebindingShortcut >= 0 && m_RebindingShortcut < (int)all.size())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_RebindingShortcut = -1;
            else
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
                {
                    ImGuiKey key = (ImGuiKey)k;
                    bool modifier = key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
                                    key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper ||
                                    (key >= ImGuiKey_ReservedForModCtrl && key <= ImGuiKey_ReservedForModSuper);
                    bool mouse = key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY;
                    if (modifier || mouse || !ImGui::IsKeyPressed(key, false)) continue;
                    all[m_RebindingShortcut].Key = key | ImGui::GetIO().KeyMods;
                    m_RebindingShortcut = -1;
                    changed = true;
                    break;
                }
        }
        if (ImGui::BeginTable("##shortcuts", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##bind", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, 54.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            const char* category = nullptr;
            for (int i = 0; i < (int)all.size(); ++i)
            {
                Shortcut& s = all[i];
                ImGui::PushID(i);
                if (!category || std::strcmp(category, s.Category) != 0)
                {
                    category = s.Category;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, UI::AccentText());
                    ImGui::TextUnformatted(category);
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(s.Name);
                ImGui::TableSetColumnIndex(1);
                auto conflicts = Shortcuts::Conflicts(s);
                std::string key = m_RebindingShortcut == i ? "Press a key...  (Esc cancels)" : s.Key ? Shortcuts::ChordName(s.Key) : "None";
                if (!conflicts.empty()) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", key.c_str());
                else ImGui::TextUnformatted(key.c_str());
                if (!conflicts.empty() && ImGui::IsItemHovered())
                {
                    std::string also;
                    for (const Shortcut* c : conflicts) also += std::string(also.empty() ? "" : ", ") + c->Name;
                    ImGui::SetTooltip("Also bound to: %s", also.c_str());
                }
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton(m_RebindingShortcut == i ? "Cancel" : "Bind")) m_RebindingShortcut = m_RebindingShortcut == i ? -1 : i;
                ImGui::TableSetColumnIndex(3);
                ImGui::BeginDisabled(s.Key == s.Default);
                if (ImGui::SmallButton("Reset"))
                {
                    s.Key = s.Default;
                    changed = true;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (changed)
        {
            EditorPreferences::Get().Shortcuts = Shortcuts::Save();
            EditorPreferences::Get().Save();
        }
        ImGui::End();
    }

    void EditorLayer::OpenExportPackage(const std::vector<AssetHandle>& assets)
    {
        m_ExportRoots = assets;
        std::erase_if(m_ExportRoots, [](AssetHandle h) { return !h.IsValid(); });
        m_ExportExcluded.clear();
        m_ShowExportPackage = !m_ExportRoots.empty();
    }

    void EditorLayer::DrawExportPackage()
    {
        // The chosen assets (with what they reference) as a .dimensionpackage: every file listed, each can be left out.
        ImGui::SetNextWindowSize(ImVec2(520.0f, 460.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Export Package", &m_ShowExportPackage))
        {
            ImGui::End();
            return;
        }
        ImGui::Checkbox("Include dependencies", &m_ExportDependencies);
        ImGui::SetItemTooltip("Also the assets these reference (a material's textures, a prefab's meshes and materials, ...)");
        auto files = AssetPackage::FilesFor(m_ExportRoots, m_ExportDependencies);
        int included = 0;
        ImGui::BeginChild("##files", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
        for (const fs::path& f : files)
        {
            std::string name = f.generic_string();
            bool on = !m_ExportExcluded.contains(name);
            if (ImGui::Checkbox(name.c_str(), &on))
            {
                if (on) m_ExportExcluded.erase(name);
                else m_ExportExcluded.insert(name);
            }
            included += on;
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(included == 0);
        if (ImGui::Button(("Export " + std::to_string(included) + " File" + (included == 1 ? "" : "s") + "...").c_str()))
        {
            std::vector<fs::path> chosen;
            for (const fs::path& f : files)
                if (!m_ExportExcluded.contains(f.generic_string())) chosen.push_back(f);
            if (auto out = FileDialogs::SaveFile({ { L"Dimension Package", L"*.dimensionpackage" } }, L"dimensionpackage"))
            {
                if (AssetPackage::Export(chosen, *out)) Log::Info("Exported {} files to '{}'", chosen.size(), out->filename().string());
                else Log::Error("Could not write '{}'", out->string());
                m_ShowExportPackage = false;
            }
        }
        ImGui::EndDisabled();
        ImGui::End();
    }

    void EditorLayer::ChooseImportPackage()
    {
        if (auto path = FileDialogs::OpenFile({ { L"Dimension Package", L"*.dimensionpackage" } })) OpenImportPackage(*path);
    }

    void EditorLayer::OpenImportPackage(const fs::path& package)
    {
        auto contents = AssetPackage::Read(package);
        if (!contents)
        {
            Log::Error("'{}' is not a Dimension package", package.filename().string());
            return;
        }
        m_ImportContents = std::move(*contents);
        m_ImportName = package.filename().string();
        // Everything that would change the project starts chosen.
        m_ImportChosen.clear();
        for (const auto& f : m_ImportContents.Files) m_ImportChosen.push_back(AssetPackage::Check(f) != AssetPackage::Status::Same);
        m_ShowImportPackage = true;
    }

    void EditorLayer::DrawImportPackage()
    {
        // A package's files with what importing each does (New, Same, Changed: replaces the project's, New GUID: its GUID
        // is taken here), each can be left out.
        ImGui::SetNextWindowSize(ImVec2(560.0f, 460.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Import Package", &m_ShowImportPackage))
        {
            ImGui::End();
            return;
        }
        ImGui::Text("%s", m_ImportName.c_str());
        int chosen = 0;
        ImGui::BeginChild("##files", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("##import", 2, ImGuiTableFlags_SizingStretchProp))
        {
            for (size_t i = 0; i < m_ImportContents.Files.size(); ++i)
            {
                const auto& f = m_ImportContents.Files[i];
                auto status = AssetPackage::Check(f);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool on = m_ImportChosen[i];
                ImGui::PushID((int)i);
                if (ImGui::Checkbox(f.Path.generic_string().c_str(), &on)) m_ImportChosen[i] = on;
                ImGui::PopID();
                chosen += on;
                ImGui::TableSetColumnIndex(1);
                switch (status)
                {
                case AssetPackage::Status::New: ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "New"); break;
                case AssetPackage::Status::Same: ImGui::TextDisabled("Same"); break;
                case AssetPackage::Status::Changed: ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Changed"); break;
                case AssetPackage::Status::GuidTaken: ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "New GUID"); break;
                }
                if (status == AssetPackage::Status::Changed && ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces the project's file (its GUID kept)");
                if (status == AssetPackage::Status::GuidTaken && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Another asset of this project has its GUID: it gets a new one, and the package's references to it\n"
                                      "reach the project's asset instead");
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(chosen == 0 || m_Ctx.IsPlaying());
        if (ImGui::Button(("Import " + std::to_string(chosen) + " File" + (chosen == 1 ? "" : "s")).c_str()))
        {
            int written = AssetPackage::Import(m_ImportContents, m_ImportChosen);
            Log::Info("Imported {} files from '{}'", written, m_ImportName);
            m_ShowImportPackage = false;
        }
        ImGui::EndDisabled();
        ImGui::End();
    }

    void EditorLayer::OpenSearch(bool open)
    {
        m_ShowSearch = open;
        if (open)
        {
            m_SearchFocus = true;
            m_SearchIndex = 0;
        }
    }

    std::vector<EditorLayer::EditorCommand> EditorLayer::EditorCommands()
    {
        // What the Search window offers to run: the menus' commands, the windows (shown or hidden), the Scene view's
        // modes, and the scripts' [MenuItem]s and windows.
        std::vector<EditorCommand> out;
        auto add = [&](std::string name, std::function<void()> run) { out.push_back({ std::move(name), std::move(run) }); };
        add("File/New Scene...", [this] { m_ShowNewScene = true; });
        for (const fs::path& p : RecentScenes::List())
            add("File/Open Recent/" + p.filename().string(), [this, p] { if (ConfirmDiscardChanges()) OpenScene(p); });
        add("File/Open Scene...", [this] { OpenSceneWithDialog(); });
        add("File/Save Scene", [this] { SaveScene(); });
        add("File/Save Scene As...", [this] { SaveSceneAs(); });
        add("File/Build Settings...", [this] { m_ShowBuildSettings = true; });
        add("File/Project Settings...", [this] { m_ShowProjectSettings = true; });
        add("Edit/Undo", [this] { m_Ctx.Undo(); });
        add("Edit/Redo", [this] { m_Ctx.Redo(); });
        add("Edit/Preferences...", [this] { m_ShowPreferences = true; });
        add(m_Ctx.IsPlaying() ? "Play/Stop" : "Play/Play", [this] { m_Ctx.IsPlaying() ? Stop() : Play(); });
        add("Play/Pause", [this] { TogglePause(); });
        for (auto& [name, flag] : WindowToggles()) add(std::string("Window/") + name, [flag] { *flag = true; });
        add("Window/Layouts/Default", [this] { m_PendingLayout = "Default"; });
        for (const auto& n : LayoutNames()) add("Window/Layouts/" + n, [this, n] { m_PendingLayout = n; });
        add("File/Project Hub...", [this] { m_ShowHub = true; });
        const char* modes[] = { "Shaded", "Wireframe", "Shaded Wireframe", "Unlit", "Overdraw" };
        for (int m = 0; m < 5; ++m) add(std::string("Scene View/Draw Mode: ") + modes[m], [this, m] { m_DrawMode = (SceneDrawMode)m; });
        add("Scene View/Frame Selected", [this] { FocusSelected(); });
        add("Scene View/Toggle Grid", [this] { m_ShowGrid = !m_ShowGrid; });
        add("Scene View/Toggle Gizmos", [this] { m_ShowGizmos = !m_ShowGizmos; });
        add("Scene View/Isolate Selection", [this] { if (m_Ctx.HasValidSelection()) m_Ctx.Visibility.Isolate(m_Ctx.ActiveScene(), m_Ctx.Selection); });
        add("Scene View/Exit Isolation", [this] { m_Ctx.Visibility.EndIsolation(); });
        add("Lighting/Bake All Probes", [this] { BakeAllProbes(); });
        for (const auto& item : CSharpRuntime::GetEditorItems())
        {
            if (!item.Inspects.empty()) continue;
            if (item.Window) add(item.MenuPath, [this, title = item.Title] { m_ScriptWindows[title] = true; });
            else add(item.MenuPath, [this, id = item.Id] { CSharpRuntime::CallEditorItem(id, false, m_Ctx.ActiveScene()); });
        }
        return out;
    }

    void EditorLayer::DrawSearchWindow()
    {
        // One list for the open scene's objects, the editor's commands, the project's assets and the objects of the
        // other scenes (Ctrl+K). Up/Down and Enter pick, as does a click; Escape closes.
        ImGui::SetNextWindowSize(ImVec2(620.0f, 480.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.3f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.3f));
        if (m_SearchFocus) ImGui::SetNextWindowFocus();
        if (!ImGui::Begin("Search", &m_ShowSearch))
        {
            ImGui::End();
            return;
        }
        if (m_SearchFocus)
        {
            ImGui::SetKeyboardFocusHere();
            m_SearchFocus = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##query", "Objects, scenes, assets and commands  (t:Type  l:Label  tag:Tag)", &m_SearchQuery)) m_SearchIndex = 0;

        struct Result
        {
            const char* Group;
            std::string Label, Detail;
            std::function<void()> Pick;
        };
        std::vector<Result> results;
        std::string query = m_SearchQuery;
        bool anyWord = query.find_first_not_of(' ') != std::string::npos;
        if (anyWord)
        {
            Scene& scene = m_Ctx.ActiveScene();
            int shown = 0;
            for (Entity e : SceneSearch::Find(scene, query))
            {
                if (++shown > 30) break;
                uint64_t guid = (uint64_t)scene.GetGuid(e);
                results.push_back({ "In This Scene", scene.GetRegistry().Get<TagComponent>(e).Name, scene.GetName(), [this, guid] {
                                        Entity found = m_Ctx.ActiveScene().FindEntityByGuid(Guid(guid));
                                        if (found.IsNull()) return;
                                        m_Ctx.Select(found);
                                        FocusSelected();
                                    } });
            }
            // Commands: every word in the name (type and label terms are for objects and assets).
            bool plain = query.find(':') == std::string::npos;
            if (plain)
            {
                std::string lowered;
                for (char c : query) lowered += (char)std::tolower((unsigned char)c);
                for (auto& command : EditorCommands())
                {
                    std::string name;
                    for (char c : command.Name) name += (char)std::tolower((unsigned char)c);
                    bool all = true;
                    for (size_t start = 0; start < lowered.size() && all;)
                    {
                        size_t end = lowered.find(' ', start);
                        if (end == std::string::npos) end = lowered.size();
                        if (end > start && name.find(lowered.substr(start, end - start)) == std::string::npos) all = false;
                        start = end + 1;
                    }
                    if (all) results.push_back({ "Commands", command.Name, "", command.Run });
                }
            }
            shown = 0;
            for (AssetHandle h : AssetManager::Search(query))
            {
                if (++shown > 30) break;
                const AssetMetadata* meta = AssetManager::GetMetadata(h);
                if (!meta) continue;
                results.push_back({ "Assets", meta->Path.filename().string(), meta->Path.parent_path().generic_string(), [this, h] {
                                        m_Ctx.SelectAsset(h);
                                        if (const AssetMetadata* m = AssetManager::GetMetadata(h))
                                            m_Content->ShowDirectory((AssetManager::GetAssetsDirectory() / m->Path).parent_path());
                                        m_ShowContent = m_ShowInspector = true;
                                    } });
            }
            AssetHandle open = m_Ctx.ScenePath.empty() ? AssetHandle::None() : AssetManager::GetHandle(m_Ctx.ScenePath);
            for (const auto& hit : ProjectSearch::FindInScenes(query, open, 30))
            {
                const AssetMetadata* meta = AssetManager::GetMetadata(hit.Scene);
                if (!meta) continue;
                results.push_back({ "In Other Scenes", hit.Name, meta->Path.generic_string(), [this, hit] {
                                        if (m_Ctx.IsPlaying() || !ConfirmDiscardChanges()) return;
                                        OpenScene(AssetManager::GetAbsolutePath(hit.Scene));
                                        Entity found = m_Ctx.EditorScene->FindEntityByGuid(Guid(hit.Object));
                                        if (found.IsNull()) return;
                                        m_Ctx.Select(found);
                                        FocusSelected();
                                    } });
            }
        }

        if (!results.empty())
        {
            m_SearchIndex = std::clamp(m_SearchIndex, 0, (int)results.size() - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_SearchIndex = std::min(m_SearchIndex + 1, (int)results.size() - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_SearchIndex = std::max(m_SearchIndex - 1, 0);
        }
        int picked = ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !results.empty() ? m_SearchIndex : -1;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_ShowSearch = false;

        ImGui::BeginChild("##results");
        const char* group = nullptr;
        for (int i = 0; i < (int)results.size(); ++i)
        {
            const Result& r = results[i];
            if (!group || std::strcmp(group, r.Group) != 0)
            {
                group = r.Group;
                ImGui::SeparatorText(group);
            }
            ImGui::PushID(i);
            bool selected = i == m_SearchIndex;
            if (ImGui::Selectable(r.Label.c_str(), selected, ImGuiSelectableFlags_AllowOverlap)) picked = i;
            if (selected && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) ImGui::SetScrollHereY();
            if (!r.Detail.empty())
            {
                ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::CalcTextSize(r.Label.c_str()).x + 24.0f));
                ImGui::TextDisabled("%s", r.Detail.c_str());
            }
            ImGui::PopID();
        }
        if (anyWord && results.empty()) ImGui::TextDisabled("Nothing found.");
        if (!anyWord) ImGui::TextDisabled("Type to search this scene's objects, the editor's commands, assets and the other scenes' objects.");
        ImGui::EndChild();
        ImGui::End();
        if (picked >= 0 && picked < (int)results.size())
        {
            m_ShowSearch = false;
            results[picked].Pick();
        }
    }

    void EditorLayer::BakeAllProbes()
    {
        SceneRenderer::RequestProbeBake(); // every reflection probe
        Scene& scene = m_Ctx.ActiveScene();
        for (auto [e, lpv] : scene.GetRegistry().ViewOf<LightProbeVolumeComponent>()) lpv.RequestBake();
    }

    void EditorLayer::DrawLightingWindow()
    {
        // The scene's lighting in one place: its environment, every light (edited in the list, like Unity's Light
        // Explorer) and every probe, with baking for all of them.
        ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.45f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Lighting", &m_ShowLighting))
        {
            ImGui::End();
            return;
        }
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        // One undo step per edit: the value before it goes into the step (drags start one when they begin).
        auto commit = [&](auto& value, const auto& before, bool changed) {
            if (ImGui::IsItemActivated() || (changed && !ImGui::IsItemActive()))
            {
                auto after = value;
                value = before;
                m_Ctx.RecordUndo();
                value = after;
            }
            if (changed) m_Ctx.MarkDirty();
        };

        ImGui::SeparatorText("Environment");
        {
            auto& s = scene.GetSettings();
            SceneSettings before = s;
            ImGui::SetNextItemWidth(220.0f);
            bool changed = ImGui::ColorEdit3("Ambient Color", &s.AmbientColor.x, ImGuiColorEditFlags_Float);
            commit(s, before, changed);
            ImGui::SetNextItemWidth(220.0f);
            changed = ImGui::DragFloat("Ambient Intensity", &s.AmbientIntensity, 0.01f, 0.0f, 10.0f);
            commit(s, before, changed);
            ImGui::TextDisabled("Sky: %s   Fog: %s   Clouds: %s", s.SkyMode == 1 ? "texture" : "procedural", s.Fog ? "on" : "off", s.Clouds ? "on" : "off");
            ImGui::SameLine();
            if (ImGui::SmallButton("Scene Settings...")) m_ShowSceneSettings = true;
        }

        ImGui::SeparatorText("Lights");
        int shadowCasters = 0;
        if (ImGui::BeginTable("##lights", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 74.0f);
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("Intensity", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Shadows", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableHeadersRow();
            auto row = [&](Entity e, const char* type, glm::vec3& color, float& intensity, float* range, bool& shadows) {
                ImGui::PushID((int)e.Index);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                auto& tag = reg.Get<TagComponent>(e);
                TagComponent tagBefore = tag;
                bool changed = ImGui::Checkbox("##active", &tag.Active);
                commit(tag, tagBefore, changed);
                if (changed) scene.UpdateTransforms();
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Selectable(tag.Name.c_str(), m_Ctx.IsSelected(e), ImGuiSelectableFlags_AllowOverlap)) m_Ctx.Select(e);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(type);
                ImGui::TableSetColumnIndex(3);
                glm::vec3 colorBefore = color;
                changed = ImGui::ColorEdit3("##color", &color.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
                commit(color, colorBefore, changed);
                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(-FLT_MIN);
                float intensityBefore = intensity;
                changed = ImGui::DragFloat("##intensity", &intensity, 0.02f, 0.0f, 1000.0f, "%.2f");
                commit(intensity, intensityBefore, changed);
                ImGui::TableSetColumnIndex(5);
                if (range)
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    float rangeBefore = *range;
                    changed = ImGui::DragFloat("##range", range, 0.1f, 0.01f, 1000.0f, "%.1f m");
                    commit(*range, rangeBefore, changed);
                }
                else
                    ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(6);
                bool shadowsBefore = shadows;
                changed = ImGui::Checkbox("##shadows", &shadows);
                commit(shadows, shadowsBefore, changed);
                ImGui::PopID();
            };
            for (auto [e, l] : reg.ViewOf<DirectionalLightComponent>()) row(e, "Directional", l.Color, l.Intensity, nullptr, l.CastShadows);
            for (auto [e, l] : reg.ViewOf<PointLightComponent>())
            {
                row(e, "Point", l.Color, l.Intensity, &l.Range, l.CastShadows);
                shadowCasters += l.CastShadows;
            }
            for (auto [e, l] : reg.ViewOf<SpotLightComponent>())
            {
                row(e, "Spot", l.Color, l.Intensity, &l.Range, l.CastShadows);
                shadowCasters += l.CastShadows;
            }
            ImGui::EndTable();
        }
        if (shadowCasters > 4)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%d point and spot lights cast shadows: only 4 at a time do (the nearest).", shadowCasters);

        ImGui::SeparatorText("Probes");
        int reflection = 0;
        for (auto [e, p] : reg.ViewOf<ReflectionProbeComponent>())
        {
            ++reflection;
            ImGui::PushID((int)e.Index);
            if (ImGui::Selectable(reg.Get<TagComponent>(e).Name.c_str(), m_Ctx.IsSelected(e))) m_Ctx.Select(e);
            ImGui::SameLine(260.0f);
            ImGui::TextDisabled("Reflection probe, %d px%s", 64 << std::clamp(p.Resolution, 0, 3), p.Realtime ? ", realtime" : "");
            ImGui::PopID();
        }
        int volumes = 0;
        for (auto [e, v] : reg.ViewOf<LightProbeVolumeComponent>())
        {
            ++volumes;
            ImGui::PushID((int)e.Index);
            if (ImGui::Selectable(reg.Get<TagComponent>(e).Name.c_str(), m_Ctx.IsSelected(e))) m_Ctx.Select(e);
            ImGui::SameLine(260.0f);
            bool baking = !v.Realtime && (v.BakeNext >= 0 || v.BakePending > 0);
            std::string state = v.Realtime ? "realtime" : baking ? "baking " + std::to_string(std::max(v.BakeNext, 0) - v.BakePending) + " / " + std::to_string(v.ProbeCount())
                              : (int)v.Baked.size() >= v.ProbeCount() * 9 ? "baked" : "not baked";
            ImGui::TextDisabled("Light probe volume, %d probes, %s", v.ProbeCount(), state.c_str());
            ImGui::PopID();
        }
        if (reflection + volumes == 0) ImGui::TextDisabled("No reflection probes or light probe volumes in this scene.");
        ImGui::BeginDisabled(reflection + volumes == 0);
        if (ImGui::Button("Bake All Probes")) BakeAllProbes();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Reflection probes capture the scene again; light probe volumes bake in the background.");
        ImGui::End();
    }

    void EditorLayer::DrawSnapSettings()
    {
        // The steps snapping uses (the toolbar's Snap), and how to snap to vertices and surfaces.
        ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 340.0f, viewport->WorkPos.y + 60.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Snap Settings", &m_ShowSnapSettings, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            auto& prefs = EditorPreferences::Get();
            bool edited = false;
            if (ImGui::Checkbox("Snap", &prefs.Snap)) edited = true;
            ImGui::SameLine();
            ImGui::TextDisabled("(Ctrl inverts it while held)");
            auto step = [&](const char* label, float* v, float speed, float lo, float hi, const char* format) {
                ImGui::SetNextItemWidth(160.0f);
                ImGui::DragFloat(label, v, speed, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
                edited |= ImGui::IsItemDeactivatedAfterEdit();
            };
            step("Move", &prefs.SnapMove, 0.01f, 0.01f, 100.0f, "%.2f m");
            step("Rotate", &prefs.SnapRotate, 0.5f, 0.5f, 180.0f, "%.1f deg");
            step("Scale", &prefs.SnapScale, 0.01f, 0.01f, 10.0f, "%.2f");
            if (edited) prefs.Save();
            ImGui::SeparatorText("While moving");
            ImGui::TextDisabled("Hold V: drag a vertex onto another object's vertex");
            ImGui::TextDisabled("Ctrl+Shift: rest the object on the surface under the mouse");
        }
        ImGui::End();
    }

    void EditorLayer::MoveSelection(const glm::vec3& delta)
    {
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        for (Entity e : m_Ctx.SelectedRoots())
        {
            auto& t = reg.Get<TransformComponent>(e);
            Entity parent = scene.GetParent(e);
            glm::mat4 parentWorld = parent.IsNull() ? glm::mat4(1.0f) : reg.Get<TransformComponent>(parent).World;
            t.Position = glm::vec3(glm::inverse(parentWorld) * glm::vec4(t.GetWorldPosition() + delta, 1.0f));
        }
        scene.UpdateTransforms();
        m_Ctx.MarkDirty();
    }

    // Vertex snapping (like Unity's V): while V is held with the Move tool, the selection's vertex nearest the mouse is
    // marked; dragging it puts it on the nearest vertex of another object under the mouse (or, with none near, moves it
    // across the view at its depth). One undo step per drag.
    bool EditorLayer::UpdateMeasure(const ImVec2& origin, const ImVec2& size)
    {
        // Click two points (vertices within a few pixels snap; Shift: surfaces only); a third click starts again. Esc
        // clears the measurement, then leaves the tool.
        if (!m_Measuring) return false;
        Scene& scene = m_Ctx.ActiveScene();
        glm::mat4 viewProj = m_Camera.GetProjection(size.x / std::max(1.0f, size.y)) * m_Camera.GetView();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        auto toScreen = [&](const glm::vec3& p, ImVec2& out) {
            glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
            if (clip.w <= 1e-4f) return false;
            out = ImVec2(origin.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
            return true;
        };
        std::optional<SceneMeasure::Pick> hover;
        if (m_SceneViewHovered && !m_Camera.IsControlling())
        {
            glm::vec3 from, dir;
            MouseRay(viewProj, origin, size, from, dir);
            ImVec2 mouse = ImGui::GetMousePos();
            auto pick = SceneMeasure::PickPoint(scene, viewProj, glm::vec2(size.x, size.y), glm::vec2(mouse.x - origin.x, mouse.y - origin.y), from, dir,
                                                ImGui::GetIO().KeyShift ? 0.0f : 12.0f);
            if (pick.Hit()) hover = pick;
            if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
            {
                if (!m_MeasureA || m_MeasureB) m_MeasureA = hover->Point, m_MeasureB.reset();
                else m_MeasureB = hover->Point;
            }
        }
        if (m_SceneViewFocused && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            if (m_MeasureA) m_MeasureA.reset(), m_MeasureB.reset();
            else m_Measuring = false;
        }

        // The ruler: the straight line with its length, and its X, Y and Z legs (red, green, blue) with theirs.
        std::optional<glm::vec3> end = m_MeasureB ? m_MeasureB : (m_MeasureA && hover ? std::optional<glm::vec3>(hover->Point) : std::nullopt);
        auto label = [&](const ImVec2& at, const std::string& text, ImU32 bg, ImU32 fg) {
            ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            ImVec2 a(at.x - ts.x * 0.5f - 4.0f, at.y - ts.y * 0.5f - 2.0f), b(at.x + ts.x * 0.5f + 4.0f, at.y + ts.y * 0.5f + 2.0f);
            draw->AddRectFilled(a, b, bg, 3.0f);
            draw->AddText(ImVec2(a.x + 4.0f, a.y + 2.0f), fg, text.c_str());
        };
        auto segment = [&](const glm::vec3& p, const glm::vec3& q, ImU32 color, float thickness, ImVec2* mid) {
            ImVec2 a, b;
            if (!toScreen(p, a) || !toScreen(q, b)) return false;
            draw->AddLine(a, b, color, thickness);
            if (mid) *mid = ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            return true;
        };
        std::string summary;
        if (m_MeasureA && end)
        {
            glm::vec3 a = *m_MeasureA, b = *end, d = b - a;
            glm::vec3 corners[4] = { a, { b.x, a.y, a.z }, { b.x, b.y, a.z }, b };
            const ImU32 axisColors[3] = { IM_COL32(230, 80, 70, 230), IM_COL32(110, 210, 90, 230), IM_COL32(80, 140, 240, 230) };
            const char* axisNames[3] = { "X", "Y", "Z" };
            for (int i = 0; i < 3; ++i)
            {
                ImVec2 mid;
                if (std::abs(d[i]) > 1e-4f && segment(corners[i], corners[i + 1], axisColors[i], 1.5f, &mid))
                    label(mid, std::string(axisNames[i]) + " " + SceneMeasure::FormatLength(d[i]), IM_COL32(20, 20, 24, 190), axisColors[i]);
            }
            ImVec2 mid;
            if (segment(a, b, IM_COL32(255, 215, 70, 255), 2.5f, &mid))
                label(mid, SceneMeasure::FormatLength(glm::length(d)), IM_COL32(255, 215, 70, 235), IM_COL32(25, 25, 25, 255));
            for (const glm::vec3& p : { a, b })
                if (ImVec2 s; toScreen(p, s)) draw->AddCircleFilled(s, 4.0f, IM_COL32(255, 215, 70, 255));
            summary = "Distance " + SceneMeasure::FormatLength(glm::length(d)) + "   X " + SceneMeasure::FormatLength(d.x) + "   Y " +
                      SceneMeasure::FormatLength(d.y) + "   Z " + SceneMeasure::FormatLength(d.z);
        }
        else if (m_MeasureA)
            if (ImVec2 s; toScreen(*m_MeasureA, s)) draw->AddCircleFilled(s, 4.0f, IM_COL32(255, 215, 70, 255));
        // What a click would pick: a vertex (square), a surface (ring) or the ground (grey ring).
        if (hover)
            if (ImVec2 s; toScreen(hover->Point, s))
            {
                if (hover->Kind == SceneMeasure::PickKind::Vertex) draw->AddRect(ImVec2(s.x - 5, s.y - 5), ImVec2(s.x + 5, s.y + 5), IM_COL32(90, 220, 120, 255), 0.0f, 0, 2.0f);
                else draw->AddCircle(s, 6.0f, hover->Kind == SceneMeasure::PickKind::Ground ? IM_COL32(170, 170, 170, 220) : IM_COL32(255, 215, 70, 255), 16, 2.0f);
            }
        // The tool's line at the bottom of the view.
        std::string help = summary.empty() ? (m_MeasureA ? "Measure: click the second point" : "Measure: click the first point")
                                           : summary;
        help += "      (vertices snap, Shift: surfaces only, Esc: clear)";
        ImVec2 ts = ImGui::CalcTextSize(help.c_str());
        ImVec2 at(origin.x + 8.0f, origin.y + size.y - ts.y - 12.0f);
        draw->AddRectFilled(ImVec2(at.x - 6, at.y - 4), ImVec2(at.x + ts.x + 6, at.y + ts.y + 4), IM_COL32(20, 20, 24, 200), 4.0f);
        draw->AddText(at, IM_COL32(235, 235, 235, 255), help.c_str());
        return m_SceneViewHovered;
    }

    bool EditorLayer::UpdateVertexSnap(const ImVec2& origin, const ImVec2& size)
    {
        bool held = ImGui::IsKeyDown(ImGuiKey_V) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput;
        if (m_VertexDrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_VertexDrag = false;
        if (!m_VertexDrag && (!held || !m_SceneViewHovered || !m_Ctx.HasValidSelection() || m_GizmoOperation != ImGuizmo::TRANSLATE ||
                              m_Camera.IsControlling()))
            return false;
        Scene& scene = m_Ctx.ActiveScene();
        glm::mat4 viewProj = m_Camera.GetProjection(size.x / std::max(1.0f, size.y)) * m_Camera.GetView();
        ImVec2 mouse = ImGui::GetMousePos();
        glm::vec2 screen(mouse.x - origin.x, mouse.y - origin.y), viewport(size.x, size.y);
        std::vector<Entity> selected = m_Ctx.SelectedRoots();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        auto toScreen = [&](const glm::vec3& p, ImVec2& out) {
            glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
            if (clip.w <= 1e-5f) return false;
            out = ImVec2(origin.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
            return true;
        };
        auto marker = [&](const glm::vec3& p, ImU32 color) {
            ImVec2 s;
            if (!toScreen(p, s)) return;
            draw->AddRect(ImVec2(s.x - 5, s.y - 5), ImVec2(s.x + 5, s.y + 5), color, 0.0f, 0, 2.0f);
        };
        glm::vec3 pivot = scene.GetRegistry().Get<TransformComponent>(m_Ctx.Selected).GetWorldPosition();
        if (!m_VertexDrag)
        {
            auto own = SceneSnapping::NearestVertex(scene, viewProj, viewport, screen, 1e6f, { m_Ctx.Selected });
            glm::vec3 grab = own.Hit ? own.Position : pivot; // no mesh: the pivot itself
            marker(grab, IM_COL32(255, 210, 60, 255));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                m_Ctx.RecordUndo();
                m_VertexDrag = true;
                m_VertexOffset = grab - pivot;
            }
            return true;
        }
        glm::vec3 grab = pivot + m_VertexOffset, target;
        auto onto = SceneSnapping::NearestVertex(scene, viewProj, viewport, screen, 40.0f, {}, selected);
        if (onto.Hit)
        {
            target = onto.Position;
            marker(target, IM_COL32(90, 220, 120, 255));
        }
        else
        {
            // Across the view: the plane through the grabbed vertex facing the camera.
            glm::vec3 from, dir;
            MouseRay(viewProj, origin, size, from, dir);
            glm::vec3 normal = glm::normalize(glm::vec3(glm::inverse(m_Camera.GetView())[2]));
            float denom = glm::dot(dir, normal);
            if (std::abs(denom) < 1e-5f) return true;
            target = from + dir * (glm::dot(grab - from, normal) / denom);
        }
        if (glm::length(target - grab) > 1e-6f) MoveSelection(target - grab);
        marker(target, IM_COL32(255, 210, 60, 255));
        return true;
    }

    // Line Renderer, Terrain Path and Path Placer points (and Off Mesh Link ends, NavMesh Obstacle shapes): with one
    // selected, its points show as handles in the Scene
    // view. Clicking one picks it and a move gizmo edits that point (undoable); clicking elsewhere returns to the
    // object's own gizmo. Shift-click on a segment inserts a point there; Delete removes the picked point (a line
    // keeps at least two).
    // Returns true while the mouse is over a handle, a segment being inserted on, or the point gizmo (so the click
    // does not re-select).
    bool EditorLayer::UpdateLinePointEditing(ImVec2 origin, ImVec2 size)
    {
        Scene& scene = m_Ctx.ActiveScene();
        auto line = m_Ctx.HasValidSelection() ? EditablePointsOf(scene, m_Ctx.Selected) : std::nullopt;
        if (!line || m_Ctx.Selection.size() > 1 || m_Camera.IsControlling())
        {
            if (!line) m_LinePoint = -1;
            return false;
        }
        if (m_LinePointOwner != m_Ctx.Selected) m_LinePoint = -1;
        if (m_Options.LinePoint >= 0)
        {
            m_LinePoint = m_Options.LinePoint;
            m_Options.LinePoint = -1;
        }
        m_LinePointOwner = m_Ctx.Selected;
        if (m_LinePoint >= (int)line->Positions.size()) m_LinePoint = -1;

        const auto& t = scene.GetRegistry().Get<TransformComponent>(m_Ctx.Selected);
        glm::mat4 view = m_Camera.GetView(), proj = m_Camera.GetProjection(size.x / std::max(1.0f, size.y));
        auto worldOf = [&](size_t i) { return line->UseWorldSpace ? line->Positions[i] : glm::vec3(t.World * glm::vec4(line->Positions[i], 1.0f)); };
        auto screenOf = [&](const glm::vec3& p, bool& visible) {
            glm::vec4 clip = proj * view * glm::vec4(p, 1.0f);
            visible = clip.w > 1e-4f;
            glm::vec2 ndc = glm::vec2(clip) / std::max(clip.w, 1e-4f);
            return ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - ndc.y * 0.5f) * size.y);
        };

        // Handles (drawn over the scene) and click-to-pick.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 mouse = ImGui::GetMousePos();
        int hovered = -1;
        float best = 9.0f;
        ImU32 accent = ImGui::GetColorU32(ImGuiCol_HeaderActive);
        for (size_t i = 0; i < line->Positions.size(); ++i)
        {
            bool visible;
            ImVec2 s = screenOf(worldOf(i), visible);
            if (!visible) continue;
            float d = std::hypot(mouse.x - s.x, mouse.y - s.y);
            if (d < best && m_SceneViewHovered) { best = d; hovered = (int)i; }
            bool picked = (int)i == m_LinePoint;
            dl->AddCircleFilled(s, picked ? 6.0f : 4.5f, picked ? accent : IM_COL32(255, 170, 60, 255));
            dl->AddCircle(s, picked ? 6.0f : 4.5f, IM_COL32(20, 20, 20, 255), 0, 1.5f);
        }
        if (hovered >= 0)
        {
            if (!line->Names.empty())
                ImGui::SetTooltip("%s (click to move it)", line->Names[std::min((size_t)hovered, line->Names.size() - 1)]);
            else
                ImGui::SetTooltip("Point %d (click to move it%s)", hovered, line->Positions.size() > 2 ? ", Delete removes it" : "");
        }

        // Shift: a "+" marker follows the mouse along the nearest segment; clicking inserts a point there.
        int insertAfter = -1;
        glm::vec3 insertWorld(0.0f);
        if (line->List && hovered < 0 && m_SceneViewHovered && ImGui::GetIO().KeyShift && !ImGuizmo::IsUsing() && line->Positions.size() >= 2)
        {
            size_t segments = line->Positions.size() - (line->Loop ? 0 : 1);
            float nearest = 10.0f;
            for (size_t i = 0; i < segments; ++i)
            {
                size_t next = (i + 1) % line->Positions.size();
                bool va, vb;
                glm::vec3 wa = worldOf(i), wb = worldOf(next);
                ImVec2 a = screenOf(wa, va), b = screenOf(wb, vb);
                if (!va || !vb) continue;
                glm::vec2 ab(b.x - a.x, b.y - a.y), am(mouse.x - a.x, mouse.y - a.y);
                float s = std::clamp(glm::dot(am, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
                float d = glm::length(am - ab * s);
                if (d < nearest)
                {
                    nearest = d;
                    insertAfter = (int)i;
                    insertWorld = glm::mix(wa, wb, s);
                }
            }
            if (insertAfter >= 0)
            {
                bool visible;
                ImVec2 s = screenOf(insertWorld, visible);
                dl->AddCircleFilled(s, 5.5f, IM_COL32(90, 200, 120, 255));
                dl->AddLine(ImVec2(s.x - 3.0f, s.y), ImVec2(s.x + 3.0f, s.y), IM_COL32(255, 255, 255, 255), 1.5f);
                dl->AddLine(ImVec2(s.x, s.y - 3.0f), ImVec2(s.x, s.y + 3.0f), IM_COL32(255, 255, 255, 255), 1.5f);
                ImGui::SetTooltip("Click to insert a point");
            }
        }

        bool overGizmo = false;
        if (m_LinePoint >= 0)
        {
            ImGuizmo::SetOrthographic(m_Camera.Is2D());
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
            ImGuizmo::PushID(0x51E);
            glm::mat4 m = glm::translate(glm::mat4(1.0f), worldOf((size_t)m_LinePoint));
            overGizmo = ImGuizmo::IsOver();
            if (overGizmo && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_Ctx.RecordUndo();
            bool snap = ImGui::GetIO().KeyCtrl;
            float snaps[3] = { 0.25f, 0.25f, 0.25f };
            if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(m), nullptr,
                                     snap ? snaps : nullptr))
            {
                glm::vec3 world(m[3]);
                line->Set((size_t)m_LinePoint, line->UseWorldSpace ? world : glm::vec3(glm::inverse(t.World) * glm::vec4(world, 1.0f)));
                Prefabs::RecordFieldOverride(scene, m_Ctx.Selected, line->Component, line->FieldOf((size_t)m_LinePoint));
                m_Ctx.MarkDirty();
            }
            overGizmo |= ImGuizmo::IsUsing();
            ImGuizmo::PopID();
        }

        if (m_SceneViewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overGizmo && !ImGui::GetIO().KeyAlt)
        {
            if (insertAfter >= 0)
            {
                m_Ctx.RecordUndo();
                glm::vec3 p = line->UseWorldSpace ? insertWorld : glm::vec3(glm::inverse(t.World) * glm::vec4(insertWorld, 1.0f));
                line->List->insert(line->List->begin() + insertAfter + 1, p);
                Prefabs::RecordFieldOverride(scene, m_Ctx.Selected, line->Component, line->FieldOf(0));
                m_Ctx.MarkDirty();
                m_LinePoint = insertAfter + 1;
                return true;
            }
            if (hovered >= 0)
            {
                m_LinePoint = hovered;
                return true; // consumed: keep the selection
            }
            m_LinePoint = -1; // back to the object's gizmo (the click selects as usual)
        }
        if (m_LinePoint >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape)) m_LinePoint = -1;
        return overGizmo || hovered >= 0 || insertAfter >= 0;
    }

    bool EditorLayer::DeleteLinePoint()
    {
        Scene& scene = m_Ctx.ActiveScene();
        auto line = m_LinePoint >= 0 && m_Ctx.HasValidSelection() ? EditablePointsOf(scene, m_Ctx.Selected) : std::nullopt;
        if (!line || m_LinePoint >= (int)line->Positions.size()) return false;
        if (line->List && line->List->size() > 2)
        {
            m_Ctx.RecordUndo();
            line->List->erase(line->List->begin() + m_LinePoint);
            Prefabs::RecordFieldOverride(scene, m_Ctx.Selected, line->Component, line->FieldOf(0));
            m_Ctx.MarkDirty();
        }
        m_LinePoint = std::min(m_LinePoint, (int)(line->List ? line->List->size() : line->Positions.size()) - 1);
        return true; // handled even when kept at two points: Delete never removes the object while a point is picked
    }

    // ---- UI ----------------------------------------------------------------------------------------

    void EditorLayer::OnImGui()
    {
        // Tint the editor chrome while playing (like Unity) so edits are not made by accident.
        ImGuiStyle& style = ImGui::GetStyle();
        ImGuiStyle saved = style;
        if (m_Ctx.IsPlaying())
        {
            const ImVec4 tint(0.80f, 0.88f, 1.15f, 1.0f);
            for (ImGuiCol c : { ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_MenuBarBg, ImGuiCol_TitleBg, ImGuiCol_TitleBgActive,
                                ImGuiCol_Tab, ImGuiCol_TabSelected, ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected, ImGuiCol_FrameBg })
            {
                ImVec4& col = style.Colors[c];
                col = ImVec4(std::min(1.0f, col.x * tint.x), std::min(1.0f, col.y * tint.y), std::min(1.0f, col.z * tint.z), col.w);
            }
        }

        DrawMenuBar();
        DrawToolbar();
        DrawDockspace();

        if (m_ShowHierarchy) m_Hierarchy->OnImGui(&m_ShowHierarchy);
        if (m_ShowInspector) m_Inspector->OnImGui(&m_ShowInspector);
        for (auto& extra : m_ExtraInspectors) extra.Panel->OnImGui(&extra.Open);
        std::erase_if(m_ExtraInspectors, [](const ExtraInspector& x) { return !x.Open; });
        if (m_ShowContent) m_Content->OnImGui(&m_ShowContent);
        if (m_ShowConsole) m_Console->OnImGui(&m_ShowConsole);
        if (m_ShowSceneSettings) m_SceneSettings->OnImGui(&m_ShowSceneSettings);
        if (m_ShowAnimator) m_Animator->OnImGui(&m_ShowAnimator);
        if (m_ShowTimeline) m_Timeline->OnImGui(&m_ShowTimeline);
        if (m_ShowNavigation) m_Navigation->OnImGui(&m_ShowNavigation);
        if (m_ShowProfiler) m_Profiler->OnImGui(&m_ShowProfiler);
        if (m_Ctx.OpenSpriteEditor)
        {
            m_ShowSpriteEditor = true;
            m_SpriteEditor->Open(m_Ctx.SelectedAsset);
            m_Ctx.OpenSpriteEditor = false;
        }
        if (m_ShowSpriteEditor) m_SpriteEditor->OnImGui(&m_ShowSpriteEditor);
        if (m_ShowSnapSettings) DrawSnapSettings();
        if (m_ShowSearch) DrawSearchWindow();
        if (m_ShowLighting) DrawLightingWindow();
        if (m_ShowShortcuts) DrawShortcutsWindow();
        if (m_ShowNewScene) DrawNewSceneWindow();
        if (m_ShowHub) DrawProjectHub();
        if (m_ShowUndoHistory) DrawUndoHistory();
        else m_RebindingShortcut = -1;
        if (m_ShowExportPackage) DrawExportPackage();
        if (m_ShowImportPackage) DrawImportPackage();
        // The Project window's Export Package... and Import Package...
        if (m_Ctx.RequestExportPackage)
        {
            fs::path path = *m_Ctx.RequestExportPackage;
            m_Ctx.RequestExportPackage.reset();
            fs::path relative = path.lexically_relative(AssetManager::GetAssetsDirectory());
            if (fs::is_directory(path)) OpenExportPackage(AssetPackage::AssetsIn(relative == "." ? fs::path() : relative));
            else if (AssetHandle h = AssetManager::GetHandle(relative); h.IsValid()) OpenExportPackage({ h });
        }
        if (m_Ctx.RequestOpenAdditive)
        {
            fs::path path = *m_Ctx.RequestOpenAdditive;
            m_Ctx.RequestOpenAdditive.reset();
            OpenSceneAdditive(path);
        }
        if (m_Ctx.RequestCloseSection > 0)
        {
            CloseAddedScene((size_t)m_Ctx.RequestCloseSection);
            m_Ctx.RequestCloseSection = 0;
        }
        if (m_Ctx.RequestSaveSection > 0)
        {
            if (!m_Ctx.IsPlaying() && m_Ctx.Sections.Save(*m_Ctx.EditorScene, (size_t)m_Ctx.RequestSaveSection))
                Log::Info("Saved scene '{}'", m_Ctx.Sections.Added()[m_Ctx.RequestSaveSection - 1].Path.filename().string());
            m_Ctx.RequestSaveSection = 0;
        }
        if (m_Ctx.RequestImportPackage)
        {
            m_Ctx.RequestImportPackage = false;
            ChooseImportPackage();
        }
        DrawScriptWindows();
        if (m_ShowFrameDebugger)
        {
            SceneRenderer& debugged = m_FrameDebugger->View() == 0 ? m_SceneRenderer : m_GameRenderer;
            m_FrameDebugger->OnImGui(&m_ShowFrameDebugger, debugged.GetFrameEvents(), debugged.GetFramePreview(), debugged.GetFramePreviewSourceSize());
        }
        if (m_ShowStats) DrawStats();
        DrawGameView();
        DrawSceneView();
        if (m_ShowProjectSettings) DrawProjectSettings();
        if (m_ShowBuildSettings) DrawBuildSettings();
        if (m_ShowPreferences) DrawPreferences();
        if (m_ShowAudioMixer) DrawAudioMixer();
        if (m_ShowTilePalette) DrawTilePalette();
        if (m_ShowAbout) DrawAbout();
        if (m_ShowImGuiDemo) ImGui::ShowDemoWindow(&m_ShowImGuiDemo);
        m_SelectionHistory.Record(CaptureSelection()); // each new selection (Back and Forward step through them)
        if (!m_Options.Press.empty() && ImGui::GetFrameCount() == 40) Shortcuts::Press(m_Options.Press); // --press (automation)
        HandleShortcuts();
        if (!m_Options.BuildFolder.empty() && !m_BatchBuildDone)
        {
            m_BatchBuildDone = true;
            bool ok = BuildGame(false, m_Options.BuildFolder);
            if (m_OptionsOut) m_OptionsOut->BuildSucceeded = ok;
            Log::Info("Batch build {}", ok ? "succeeded" : "FAILED");
            Application::Get().Close();
        }
        if (!m_Options.FocusWindow.empty() && m_FocusRequestFrames > 0 && --m_FocusRequestFrames == 0)
            ImGui::SetWindowFocus(m_Options.FocusWindow.c_str());
        style = saved;
    }

    void EditorLayer::BuildDefaultLayout(unsigned int id)
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, viewport->WorkSize);
        ImGuiID center = id;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
        ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.32f, nullptr, &right);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Game", center);
        ImGui::DockBuilderDockWindow("Scene", center);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Scene Settings", rightBottom);
        ImGui::DockBuilderDockWindow("Stats", rightBottom);
        ImGui::DockBuilderDockWindow("Navigation", rightBottom);
        ImGui::DockBuilderDockWindow("Project", bottom);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Animator", bottom);
        ImGui::DockBuilderDockWindow("Audio Mixer", bottom);
        ImGui::DockBuilderDockWindow("Timeline", bottom);
        ImGui::DockBuilderDockWindow("Profiler", bottom);
        ImGui::DockBuilderFinish(id);
    }

    void EditorLayer::DrawDockspace()
    {
        ImGuiID id = ImGui::GetID("EditorDockspace");
        if (m_ResetLayout)
        {
            m_ShowHierarchy = m_ShowInspector = m_ShowContent = m_ShowConsole = m_ShowStats = m_ShowSceneSettings = true;
            BuildDefaultLayout(id);
            m_ResetLayout = false;
        }
        ImGui::DockSpaceOverViewport(id, ImGui::GetMainViewport());
    }

    void EditorLayer::DrawMenuBar()
    {
        if (m_Options.MenuOpenScene && ++m_MenuAutomationFrames == 30) OpenSceneWithDialog(); // automation: same path as the menu
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Scene...", Shortcuts::Label("file.new").c_str())) m_ShowNewScene = true;
            if (ImGui::MenuItem("Open Scene...", Shortcuts::Label("file.open").c_str())) OpenSceneWithDialog();
            // The project's last opened scenes (newest first).
            if (ImGui::BeginMenu("Open Recent"))
            {
                auto recent = RecentScenes::List();
                for (const fs::path& p : recent)
                {
                    std::string label = p.lexically_relative(AssetManager::GetAssetsDirectory()).generic_string();
                    if (ImGui::MenuItem(label.c_str(), nullptr, !m_Ctx.ScenePath.empty() && p == m_Ctx.ScenePath.lexically_normal()) &&
                        ConfirmDiscardChanges())
                        OpenScene(p);
                }
                if (recent.empty()) ImGui::TextDisabled("No scenes yet");
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Recent", nullptr, false, !recent.empty())) RecentScenes::Clear();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Open Scene Additive...", nullptr, false, !m_Ctx.IsPlaying() && !m_Ctx.InPrefabMode()))
                if (auto p = FileDialogs::OpenFile({ { L"Scene", L"*.scene" } }, AssetManager::GetAssetsDirectory())) OpenSceneAdditive(*p);
            if (ImGui::MenuItem("Save", Shortcuts::Label("file.save").c_str(), false, !m_Ctx.IsPlaying())) SaveScene();
            if (ImGui::MenuItem("Save As...", Shortcuts::Label("file.saveAs").c_str(), false, !m_Ctx.IsPlaying())) SaveSceneAs();
            ImGui::Separator();
            if (ImGui::MenuItem("Project Hub...")) m_ShowHub = true;
            if (ImGui::MenuItem("Open Project..."))
                if (auto dir = FileDialogs::PickFolder(Project::GetDirectory())) SwitchProject(*dir);
            if (ImGui::MenuItem("Project Settings...")) m_ShowProjectSettings = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Import Package...")) ChooseImportPackage();
            if (ImGui::MenuItem("Export Package...", nullptr, false, m_Ctx.SelectedAsset.IsValid()))
                OpenExportPackage(m_Ctx.SelectedAssetList());
            ImGui::Separator();
            if (ImGui::MenuItem("Build Settings...", Shortcuts::Label("file.buildSettings").c_str())) m_ShowBuildSettings = true;
            if (ImGui::MenuItem("Build And Run", Shortcuts::Label("file.buildAndRun").c_str(), false, !m_Ctx.IsPlaying())) BuildGame(true, Project::GetConfig().BuildFolder);
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) Application::Get().GetWindow().SetShouldClose(OnCloseRequested());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            // "Undo Delete Wall A": the steps' names.
            auto history = m_Ctx.History();
            size_t done = m_Ctx.HistoryDone();
            std::string undo = done > 0 ? "Undo " + history[done - 1].Name : "Undo";
            std::string redo = done < history.size() ? "Redo " + history[done].Name : "Redo";
            if (undo.size() > 48) undo = undo.substr(0, 45) + "...";
            if (redo.size() > 48) redo = redo.substr(0, 45) + "...";
            if (ImGui::MenuItem(undo.c_str(), Shortcuts::Label("edit.undo").c_str(), false, m_Ctx.CanUndo() && !m_Ctx.IsPlaying())) m_Ctx.Undo();
            if (ImGui::MenuItem(redo.c_str(), Shortcuts::Label("edit.redo").c_str(), false, m_Ctx.CanRedo() && !m_Ctx.IsPlaying())) m_Ctx.Redo();
            if (ImGui::MenuItem("Undo History", nullptr, m_ShowUndoHistory)) m_ShowUndoHistory = !m_ShowUndoHistory;
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", Shortcuts::Label("edit.copy").c_str(), false, m_Ctx.HasValidSelection())) CopySelected(m_Ctx);
            if (ImGui::MenuItem("Paste", Shortcuts::Label("edit.paste").c_str(), false, !m_Ctx.IsPlaying())) PasteClipboard(m_Ctx);
            if (ImGui::MenuItem("Duplicate", Shortcuts::Label("edit.duplicate").c_str(), false, m_Ctx.HasValidSelection())) DuplicateSelected(m_Ctx);
            if (ImGui::MenuItem("Group", Shortcuts::Label("edit.group").c_str(), false, m_Ctx.HasValidSelection() && !m_Ctx.IsPlaying())) GroupSelected(m_Ctx);
            if (ImGui::MenuItem("Ungroup", Shortcuts::Label("edit.ungroup").c_str(), false, m_Ctx.HasValidSelection() && !m_Ctx.IsPlaying()))
                UngroupSelected(m_Ctx);
            if (ImGui::BeginMenu("Selection"))
            {
                DrawSelectionMenu();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences...")) m_ShowPreferences = true;
            if (ImGui::MenuItem("Delete", Shortcuts::Label("edit.delete").c_str(), false, m_Ctx.HasValidSelection())) DeleteSelected(m_Ctx);
            if (ImGui::MenuItem("Frame Selected", Shortcuts::Label("scene.frame").c_str(), false, m_Ctx.HasValidSelection())) FocusSelected();
            if (ImGui::MenuItem("Frame All", Shortcuts::Label("scene.frameAll").c_str())) FrameAll();
            ImGui::Separator();
            if (ImGui::MenuItem(m_Ctx.IsPlaying() ? "Stop" : "Play", Shortcuts::Label("play.play").c_str())) m_Ctx.IsPlaying() ? Stop() : Play();
            if (ImGui::MenuItem("Pause", Shortcuts::Label("play.pause").c_str(), m_Ctx.State == PlayState::Paused, m_Ctx.IsPlaying())) TogglePause();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scripts"))
        {
            bool building = m_BuildState && m_BuildState->load() == 1;
            if (ImGui::MenuItem("Build && Reload Game Module", Shortcuts::Label("file.buildModule").c_str(), false, !building)) BuildGameModule();
            if (ImGui::MenuItem("Reload Game Module", nullptr, false, !building)) ReloadGameModule();
            ImGui::Separator();
            bool csBuilding = m_CSharpBuild && m_CSharpBuild->load() == 1;
            if (ImGui::MenuItem("Build C# Scripts", nullptr, false, CSharpRuntime::IsAvailable() && !csBuilding)) BuildCSharp();
            if (ImGui::MenuItem("Open C# Project Folder", nullptr, false, CSharpRuntime::IsAvailable()))
                FileDialogs::OpenWithDefaultApp(CSharpProject::ScriptsDirectory(Project::GetDirectory()));
            if (ImGui::BeginMenu("C# Debugger", CSharpRuntime::IsAvailable()))
            {
                bool attached = CSharpRuntime::IsDebuggerAttached();
                ImGui::TextDisabled(attached ? "A debugger is attached" : "No debugger attached");
                ImGui::TextDisabled("Attach from Rider (Run > Attach to Process) or Visual Studio to process %d", _getpid());
                if (ImGui::MenuItem("Launch Debugger Prompt...", nullptr, false, !attached)) CSharpRuntime::LaunchDebugger();
                ImGui::EndMenu();
            }
            ImGui::TextDisabled(CSharpRuntime::IsAvailable() ? "C#: %zu classes" : "C#: unavailable", CSharpRuntime::GetScriptCount());
            ImGui::Separator();
            ImGui::TextDisabled("Module: %s", GameModule::GetPath().filename().string().c_str());
            ImGui::TextDisabled("%zu scripts registered", ScriptRegistry::All().size());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("GameObject"))
        {
            DrawCreateMenuItems(m_Ctx, Entity::Null());
            ImGui::Separator();
            bool selected = m_Ctx.HasValidSelection();
            if (ImGui::MenuItem("Move To View", Shortcuts::Label("scene.moveToView").c_str(), false, selected && !m_Ctx.IsPlaying())) MoveSelectionToView();
            if (ImGui::MenuItem("Align With View", Shortcuts::Label("scene.alignWithView").c_str(), false, selected && !m_Ctx.IsPlaying()))
                AlignSelectionWithView();
            if (ImGui::MenuItem("Align View to Selected", Shortcuts::Label("scene.alignViewToSelected").c_str(), false, selected)) AlignViewToSelected();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window"))
        {
            ImGui::MenuItem("Hierarchy", nullptr, &m_ShowHierarchy);
            ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
            if (ImGui::MenuItem("New Inspector")) NewInspector(true);
            ImGui::SetItemTooltip("Another Inspector window, locked to the selection (unlock it to follow the selection)");
            ImGui::MenuItem("Project", nullptr, &m_ShowContent);
            ImGui::MenuItem("Console", nullptr, &m_ShowConsole);
            ImGui::MenuItem("Scene Settings", nullptr, &m_ShowSceneSettings);
            ImGui::MenuItem("Animator", nullptr, &m_ShowAnimator);
            ImGui::MenuItem("Timeline", nullptr, &m_ShowTimeline);
            ImGui::MenuItem("Navigation", nullptr, &m_ShowNavigation);
            ImGui::MenuItem("Profiler", nullptr, &m_ShowProfiler);
            ImGui::MenuItem("Frame Debugger", nullptr, &m_ShowFrameDebugger);
            ImGui::MenuItem("Sprite Editor", nullptr, &m_ShowSpriteEditor);
            ImGui::MenuItem("Snap Settings", nullptr, &m_ShowSnapSettings);
            ImGui::MenuItem("Lighting", nullptr, &m_ShowLighting);
            ImGui::MenuItem("Shortcuts", nullptr, &m_ShowShortcuts);
            if (ImGui::MenuItem("Search", Shortcuts::Label("edit.search").c_str(), m_ShowSearch)) OpenSearch(!m_ShowSearch);
            ImGui::MenuItem("Audio Mixer", nullptr, &m_ShowAudioMixer);
            ImGui::MenuItem("Tile Palette", nullptr, &m_ShowTilePalette);
            ImGui::MenuItem("Stats", nullptr, &m_ShowStats);
            ImGui::Separator();
            DrawLayoutsMenu();
            if (ImGui::MenuItem("Reset Layout")) m_ResetLayout = true;
            ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
            ImGui::EndMenu();
        }
        DrawScriptMenus();
        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About Dimension")) m_ShowAbout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void EditorLayer::DrawToolbar()
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        float height = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
        if (ImGui::BeginViewportSideBar("##Toolbar", viewport, ImGuiDir_Up, height, flags & ~ImGuiWindowFlags_MenuBar))
        {
            // Transform tools (left)
            auto tool = [&](const char* id, Glyph g, int op, const char* tip) {
                if (GlyphButton(id, g, m_GizmoOperation == op, tip)) m_GizmoOperation = op;
                ImGui::SameLine();
            };
            tool("##hand", Glyph::Hand, 0, "View tool (Q)");
            tool("##move", Glyph::Move, ImGuizmo::TRANSLATE, "Move tool (W)");
            tool("##rotate", Glyph::Rotate, ImGuizmo::ROTATE, "Rotate tool (E)");
            tool("##scale", Glyph::Scale, ImGuizmo::SCALE, "Scale tool (R)");
            if (ImGui::Button(m_GizmoLocal ? "Local" : "Global", ImVec2(60, 0))) m_GizmoLocal = !m_GizmoLocal;
            ImGui::SameLine();
            bool is2D = m_Camera.Is2D();
            if (is2D) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button("2D", ImVec2(36, 0))) m_Camera.Set2D(!is2D);
            if (is2D) ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Checkbox("Grid", &m_ShowGrid);
            ImGui::SameLine(0.0f, 2.0f);
            bool openGrid = m_OpenGridMenu && ImGui::GetFrameCount() >= 60; // (--open Grid: after the startup focus changes)
            if (ImGui::ArrowButton("##gridSettings", ImGuiDir_Down) || openGrid) ImGui::OpenPopup("##gridMenu");
            if (openGrid) m_OpenGridMenu = false;
            ImVec2 gridBelow(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grid settings");
            ImGui::SetNextWindowPos(gridBelow, ImGuiCond_Appearing);
            if (ImGui::BeginPopup("##gridMenu"))
            {
                DrawGridMenu();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Gizmos", &m_ShowGizmos);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scripts' gizmos (C# OnDrawGizmos) in the Scene view");
            // The Scene view's draw mode.
            ImGui::SameLine();
            const char* modes[] = { "Shaded", "Wireframe", "Shaded Wireframe", "Unlit", "Overdraw" };
            ImGui::SetNextItemWidth(130.0f);
            int mode = (int)m_DrawMode;
            if (ImGui::Combo("##drawmode", &mode, modes, IM_ARRAYSIZE(modes))) m_DrawMode = (SceneDrawMode)mode;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scene view draw mode");
            // Isolation (Shift+H): a button to leave it.
            if (m_Ctx.Visibility.Isolating())
            {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button("Exit Isolation")) m_Ctx.Visibility.EndIsolation();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only the isolated objects show in the Scene view (Shift+H)");
            }
            // Scene visibility: with objects hidden or unpickable, a toggle to show and pick everything.
            if (!m_Ctx.Visibility.Empty())
            {
                ImGui::SameLine();
                bool on = m_Ctx.Visibility.Enabled;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button("Visibility")) m_Ctx.Visibility.Enabled = !on;
                if (on) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(on ? "Objects hidden or made unpickable in the Hierarchy apply (click to show and pick everything)"
                                         : "Showing and picking everything (click to apply the Hierarchy's visibility again)");
            }
            ImGui::SameLine();
            // Snapping: the toggle, and its steps in the menu beside it.
            auto& prefs = EditorPreferences::Get();
            bool snapOn = prefs.Snap;
            if (snapOn) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button("Snap", ImVec2(44, 0)))
            {
                prefs.Snap = !prefs.Snap;
                prefs.Save();
            }
            if (snapOn) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Moves land on the grid, rotation and scale go in steps (Ctrl inverts this while held).\n"
                                  "Hold V to drag a vertex onto another object's vertex; Ctrl+Shift while moving puts the "
                                  "object on the surface under the mouse.");
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::ArrowButton("##snapsettings", ImGuiDir_Down)) m_ShowSnapSettings = !m_ShowSnapSettings;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap Settings");
            // Camera bookmarks and Frame All.
            ImGui::SameLine();
            // (--open Views: once the startup focus changes are over, which would close it.)
            bool openViews = m_OpenViewsMenu && ImGui::GetFrameCount() >= 60;
            if (ImGui::Button("Views") || openViews) ImGui::OpenPopup("##viewsMenu");
            if (openViews) m_OpenViewsMenu = false;
            ImVec2 below(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scene view bookmarks (Ctrl+1-9, Ctrl+Shift+1-9 saves) and Frame All");
            ImGui::SetNextWindowPos(below, ImGuiCond_Appearing); // a dropdown under the button
            if (ImGui::BeginPopup("##viewsMenu"))
            {
                DrawViewsMenu();
                ImGui::EndPopup();
            }
            // The measuring tool.
            ImGui::SameLine();
            bool measuring = m_Measuring;
            if (measuring) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button("Measure")) SetMeasuring(!m_Measuring);
            if (measuring) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Measure distances in the Scene view (%s): click two points", Shortcuts::Label("tool.measure").c_str());

            // Play controls (center)
            float w = ImGui::GetFrameHeight() * 1.25f * 3 + ImGui::GetStyle().ItemSpacing.x * 2;
            // In the middle, or after the tools when they reach past it (narrow windows).
            ImGui::SameLine();
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() * 0.5f - w * 0.5f));
            if (GlyphButton("##play", m_Ctx.IsPlaying() ? Glyph::Stop : Glyph::Play, m_Ctx.IsPlaying(), m_Ctx.IsPlaying() ? "Stop (Ctrl+P)" : "Play (Ctrl+P)"))
                m_Ctx.IsPlaying() ? Stop() : Play();
            ImGui::SameLine();
            ImGui::BeginDisabled(!m_Ctx.IsPlaying());
            if (GlyphButton("##pause", Glyph::Pause, m_Ctx.State == PlayState::Paused, "Pause (Ctrl+Shift+P)")) TogglePause();
            ImGui::SameLine();
            if (GlyphButton("##step", Glyph::Step, false, "Step one frame") && m_Ctx.RuntimeScene)
            {
                if (m_Ctx.State != PlayState::Paused) TogglePause();
                m_Ctx.RuntimeScene->Step(1);
            }
            ImGui::EndDisabled();

            // Right side: compile status + scene name
            std::string label = m_Ctx.EditorScene->GetName() + (m_Ctx.Dirty ? "*" : "");
            if (m_Ctx.InPrefabMode()) label = "Prefab Mode: " + label;
            if (m_BuildState && m_BuildState->load() == 1) label = "Compiling scripts...   " + label;
            else if (m_CSharpBuild && m_CSharpBuild->load() == 1) label = "Compiling C#...   " + label;
            else if (m_ReloadPending) label = "Scripts changed: reload after Stop   " + label;
            float tw = ImGui::CalcTextSize(label.c_str()).x;
            ImGui::SameLine(ImGui::GetWindowWidth() - tw - 16);
            ImGui::TextDisabled("%s", label.c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void EditorLayer::DrawSceneView()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (m_FocusSceneView > 0)
        {
            ImGui::SetNextWindowFocus();
            --m_FocusSceneView;
        }
        bool visible = ImGui::Begin("Scene");
        ImGui::PopStyleVar();
        m_SceneViewVisible = visible;
        m_SceneViewHovered = false;
        if (!visible)
        {
            ImGui::End();
            return;
        }
        m_SceneViewFocused = ImGui::IsWindowFocused();
        if (m_Ctx.InPrefabMode()) DrawPrefabModeBar();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        m_SceneViewSize = { (uint32_t)std::max(1.0f, avail.x), (uint32_t)std::max(1.0f, avail.y) };
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef((ImTextureID)m_SceneTarget->GetImGuiTexture()), avail);
        m_SceneViewHovered = ImGui::IsItemHovered();
        // Right/middle clicks focus the view too, so keyboard shortcuts work after camera moves.
        if (m_SceneViewHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
            ImGui::SetWindowFocus();

        Scene& scene = m_Ctx.ActiveScene();

        // Asset drops: meshes, textures, prefabs, scenes.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(UI::kAssetPayload))
            {
                AssetHandle h(*(const uint64_t*)p->Data);
                if (auto* meta = AssetManager::GetMetadata(h))
                {
                    glm::vec3 pos = m_Camera.Is2D() ? glm::vec3(glm::vec2(m_Camera.GetPosition()), 0.0f) : m_Camera.GetPosition() + m_Camera.GetForward() * 6.0f;
                    switch (meta->Type)
                    {
                    case AssetType::Mesh:
                    {
                        Entity e = CreateObject(m_Ctx, ObjectKind::Empty, Entity::Null(), &pos);
                        scene.GetRegistry().Get<TagComponent>(e).Name = meta->Path.stem().string();
                        scene.GetRegistry().Emplace<MeshRendererComponent>(e).Mesh = h;
                        if (auto mesh = AssetManager::GetMesh(h); mesh && mesh->IsSkinned())
                        {
                            auto& anim = scene.GetRegistry().Emplace<AnimatorComponent>(e);
                            if (!mesh->GetClips().empty()) anim.Clip = mesh->GetClips().front().Name;
                        }
                        break;
                    }
                    case AssetType::Texture:
                    {
                        Entity e = CreateObject(m_Ctx, ObjectKind::Sprite, Entity::Null(), &pos);
                        scene.GetRegistry().Get<TagComponent>(e).Name = meta->Path.stem().string();
                        scene.GetRegistry().Get<SpriteRendererComponent>(e).Texture = h;
                        break;
                    }
                    case AssetType::Prefab:
                        if (h == m_Ctx.PrefabModeAsset())
                        {
                            Log::Warn("A prefab cannot contain an instance of itself");
                            break;
                        }
                        m_Ctx.RecordUndo();
                        m_Ctx.Select(scene.InstantiatePrefab(h, pos));
                        break;
                    case AssetType::Scene: m_Ctx.RequestOpenScene = AssetManager::GetAbsolutePath(h); break;
                    default: break;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        bool measuring = UpdateMeasure(origin, avail); // the measuring tool owns the view's clicks
        bool tilePainting = !measuring && (UpdateTilePainting(origin, avail) || UpdateTerrainBrush(origin, avail));
        bool lineEditing = !measuring && UpdateLinePointEditing(origin, avail);
        bool vertexSnapping = !measuring && !tilePainting && !lineEditing && m_LinePoint < 0 && UpdateVertexSnap(origin, avail);
        bool handles = !measuring && !tilePainting && !lineEditing && !vertexSnapping && UpdateScriptHandles(origin, avail);

        // Transform gizmo
        bool gizmoHovered = measuring || lineEditing || vertexSnapping || handles;
        if (m_Ctx.HasValidSelection() && m_GizmoOperation != 0 && !m_Camera.IsControlling() && !tilePainting && m_LinePoint < 0 && !vertexSnapping &&
            !measuring && m_HandleDrag == 0)
        {
            auto& reg = scene.GetRegistry();
            auto& t = reg.Get<TransformComponent>(m_Ctx.Selected);
            ImGuizmo::SetOrthographic(m_Camera.Is2D());
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(origin.x, origin.y, avail.x, avail.y);
            glm::mat4 view = m_Camera.GetView();
            glm::mat4 proj = m_Camera.GetProjection(avail.x / std::max(1.0f, avail.y));
            glm::mat4 world = t.World;
            glm::mat4 worldBefore = world;

            // Snapping (the toolbar's Snap, inverted while Ctrl is held): moves land on the world grid (the axes being
            // moved), rotation and scale go in steps. Ctrl+Shift while moving puts the selection on the surface under
            // the mouse, resting on it.
            const auto& prefs = EditorPreferences::Get();
            const ImGuiIO& io = ImGui::GetIO();
            bool translate = m_GizmoOperation == ImGuizmo::TRANSLATE;
            bool onSurface = translate && io.KeyCtrl && io.KeyShift;
            bool snap = !onSurface && prefs.Snap != io.KeyCtrl;
            float snapValue = m_GizmoOperation == ImGuizmo::ROTATE ? prefs.SnapRotate : prefs.SnapScale;
            float snaps[3] = { snapValue, snapValue, snapValue };

            gizmoHovered = ImGuizmo::IsOver();
            if (gizmoHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_Ctx.RecordUndo();
            bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), (ImGuizmo::OPERATION)m_GizmoOperation,
                                                m_GizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(world), nullptr,
                                                snap && !translate ? snaps : nullptr);
            if (onSurface && ImGuizmo::IsUsing())
            {
                glm::vec3 from, dir;
                MouseRay(proj * view, origin, avail, from, dir);
                auto hit = SceneSnapping::Raycast(scene, from, dir, m_Ctx.SelectedRoots());
                if (hit.Hit)
                {
                    glm::vec3 p = hit.Point + hit.Normal * SceneSnapping::RestOffset(scene, m_Ctx.Selected, hit.Normal);
                    world[3] = glm::vec4(p, 1.0f);
                    changed = glm::vec3(world[3]) != glm::vec3(worldBefore[3]);
                }
            }
            else if (changed && snap && translate)
            {
                glm::vec3 before = glm::vec3(worldBefore[3]), now = glm::vec3(world[3]);
                glm::bvec3 moved = glm::greaterThan(glm::abs(now - before), glm::vec3(1e-6f));
                world[3] = glm::vec4(SceneSnapping::ToGrid(now, prefs.SnapMove, moved), 1.0f);
            }
            if (changed)
            {
                Entity parent = scene.GetParent(m_Ctx.Selected);
                glm::mat4 parentWorld = parent.IsNull() ? glm::mat4(1.0f) : reg.Get<TransformComponent>(parent).World;
                glm::mat4 local = glm::inverse(parentWorld) * world;
                glm::vec3 scale, translation, skew;
                glm::vec4 perspective;
                glm::quat rotation;
                if (glm::decompose(local, scale, rotation, translation, skew, perspective))
                {
                    t.Position = translation;
                    if (m_GizmoOperation == ImGuizmo::ROTATE) t.Rotation = glm::eulerAngles(rotation);
                    if (m_GizmoOperation == ImGuizmo::SCALE) t.Scale = scale;
                    m_Ctx.MarkDirty();
                }
                // Apply the same world-space change to the rest of the selection (top-level ones only).
                glm::mat4 delta = world * glm::inverse(worldBefore);
                for (Entity other : m_Ctx.SelectedRoots())
                {
                    if (other == m_Ctx.Selected || scene.IsDescendantOf(m_Ctx.Selected, other) || scene.IsDescendantOf(other, m_Ctx.Selected)) continue;
                    auto& ot = reg.Get<TransformComponent>(other);
                    Entity op = scene.GetParent(other);
                    glm::mat4 opWorld = op.IsNull() ? glm::mat4(1.0f) : reg.Get<TransformComponent>(op).World;
                    glm::mat4 newLocal = glm::inverse(opWorld) * (delta * ot.World);
                    if (glm::decompose(newLocal, scale, rotation, translation, skew, perspective))
                    {
                        ot.Position = translation;
                        if (m_GizmoOperation == ImGuizmo::ROTATE) ot.Rotation = glm::eulerAngles(rotation);
                        if (m_GizmoOperation == ImGuizmo::SCALE) ot.Scale = scale;
                    }
                }
                scene.UpdateTransforms();
            }
            gizmoHovered |= ImGuizmo::IsUsing();
        }

        // Orientation cube (top-right): click a face to snap the view, drag to orbit.
        if (!m_Camera.Is2D())
        {
            const float size = 96.0f;
            ImVec2 cubePos(origin.x + avail.x - size - 8.0f, origin.y + 8.0f);
            glm::mat4 view = m_Camera.GetView();
            glm::mat4 before = view;
            ImGuizmo::ViewManipulate(glm::value_ptr(view), m_Camera.GetDistance(), cubePos, ImVec2(size, size), 0x00000000);
            if (view != before) m_Camera.SetFromView(view);
            ImVec2 mouse = ImGui::GetMousePos();
            if (mouse.x >= cubePos.x && mouse.x <= cubePos.x + size && mouse.y >= cubePos.y && mouse.y <= cubePos.y + size)
                gizmoHovered = true;
        }

        // Click to select (GPU picking)
        bool alt = ImGui::GetIO().KeyAlt;
        if (m_SceneViewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoHovered && !alt && !tilePainting)
        {
            ImVec2 mouse = ImGui::GetMousePos();
            m_SceneTarget->RequestPick((uint32_t)(mouse.x - origin.x), (uint32_t)(mouse.y - origin.y));
            m_PickAdditive = ImGui::GetIO().KeyCtrl;
        }

        DrawNoteLabels(origin, avail);

        // Overlay info
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + 6));
        ImGui::TextDisabled("%s  |  speed %.1f%s", m_Camera.Is2D() ? "2D Ortho" : "Perspective", m_Camera.GetFlySpeed(),
                            m_Ctx.IsPlaying() ? "  |  PLAYING (changes are temporary)" : "");
        ImGui::End();
    }

    void EditorLayer::DrawGameView()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (m_FocusGameView > 0)
        {
            ImGui::SetNextWindowFocus();
            --m_FocusGameView;
        }
        bool visible = ImGui::Begin("Game");
        ImGui::PopStyleVar();
        m_GameViewVisible = visible;
        if (!visible)
        {
            m_GameViewFocused = false;
            ImGui::End();
            return;
        }
        m_GameViewFocused = ImGui::IsWindowFocused();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        m_GameViewSize = { (uint32_t)std::max(1.0f, avail.x), (uint32_t)std::max(1.0f, avail.y) };
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef((ImTextureID)m_GameTarget->GetImGuiTexture()), avail);
        bool imageHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) ImGui::SetWindowFocus();
        if (m_Ctx.IsPlaying())
        {
            ImVec2 mouse = ImGui::GetMousePos();
            m_Ctx.RuntimeScene->SetUIInput({ mouse.x - origin.x, mouse.y - origin.y },
                                           imageHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left),
                                           { avail.x, avail.y }, imageHovered || ImGui::IsMouseDown(ImGuiMouseButton_Left),
                                           imageHovered ? ImGui::GetIO().MouseWheel : 0.0f);
        }
        if (m_Ctx.ActiveScene().GetPrimaryCamera().IsNull())
        {
            const char* msg = "No camera rendering";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, origin.y + (avail.y - ts.y) * 0.5f));
            ImGui::TextUnformatted(msg);
        }
        if (m_Ctx.IsPlaying() && !m_GameViewFocused)
        {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + 6));
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Click to give the game input focus");
        }
        if (!m_Ctx.IsPlaying()) UpdateUIRectEditing(origin, avail, imageHovered);
        DrawNavigationOverlay(origin, avail);
        ImGui::End();
    }

    void EditorLayer::UpdateUIRectEditing(ImVec2 origin, ImVec2 size, bool hovered)
    {
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        auto order = Dimension::UI::ComputeLayout(scene, { size.x, size.y });
        ImVec2 mouse = ImGui::GetMousePos();
        glm::vec2 m(mouse.x - origin.x, mouse.y - origin.y);
        Entity sel = m_Ctx.HasValidSelection() ? m_Ctx.Selected : Entity::Null();
        auto* rect = sel.IsNull() ? nullptr : reg.TryGet<UIRectComponent>(sel);
        bool visible = rect && reg.Get<TransformComponent>(sel).ActiveInHierarchy && std::find(order.begin(), order.end(), sel) != order.end();
        // Children of a layout group are placed by it: outline only.
        Entity parent = sel.IsNull() ? Entity::Null() : scene.GetParent(sel);
        bool editable = visible && (parent.IsNull() || !reg.Has<UILayoutGroupComponent>(parent));

        // Handles: 1-4 corners (top-left, top-right, bottom-right, bottom-left), 5-8 edges (top, right, bottom, left).
        // Each moves the rect's min/max x/y by the mouse: -1 min, +1 max, 0 neither.
        static const glm::ivec2 kSides[9] = { { 0, 0 }, { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 }, { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
        auto handlePos = [&](int h) {
            glm::vec2 mn = rect->ComputedMin, mx = rect->ComputedMax, c = (mn + mx) * 0.5f;
            glm::ivec2 sd = kSides[h];
            return glm::vec2(sd.x < 0 ? mn.x : sd.x > 0 ? mx.x : c.x, sd.y < 0 ? mn.y : sd.y > 0 ? mx.y : c.y);
        };
        // The topmost UI element under the mouse (draw order: last on top).
        Entity topmost = Entity::Null();
        for (auto it = order.rbegin(); it != order.rend() && topmost.IsNull(); ++it)
            if (auto* r = reg.TryGet<UIRectComponent>(*it); r && glm::all(glm::greaterThanEqual(m, r->ComputedMin)) && glm::all(glm::lessThanEqual(m, r->ComputedMax)))
                topmost = *it;
        int hot = -1;
        if (editable && m_UIRectDrag < 0 && hovered)
        {
            for (int h = 1; h <= 8 && hot < 0; ++h)
                if (glm::all(glm::lessThanEqual(glm::abs(m - handlePos(h)), glm::vec2(6.0f)))) hot = h;
            if (hot < 0 && topmost == sel) hot = 0; // dragging inside moves it (unless a child is on top: that selects it)
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (hot >= 0)
            {
                m_Ctx.RecordUndo();
                m_UIRectDrag = hot;
                m_UIRectDragStart = m;
                m_UIRectStartPosition = rect->Position;
                m_UIRectStartSize = rect->Size;
            }
            else if (!topmost.IsNull())
                m_Ctx.Select(topmost);
        }
        if (m_UIRectDrag >= 0)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !editable)
                m_UIRectDrag = -1;
            else
            {
                float s = std::max(rect->ComputedScale, 1e-4f);
                glm::vec2 d = (m - m_UIRectDragStart) / s; // canvas units
                if (ImGui::GetIO().KeyCtrl) d = glm::round(d / 10.0f) * 10.0f; // Ctrl: steps of 10 units
                glm::vec2 position = m_UIRectStartPosition, sizeUnits = m_UIRectStartSize;
                Dimension::UI::DragRect(*rect, kSides[m_UIRectDrag], d * s, position, sizeUnits);
                position = glm::round(position);
                sizeUnits = glm::round(sizeUnits);
                if (position != rect->Position || sizeUnits != rect->Size)
                {
                    rect->Position = position;
                    rect->Size = sizeUnits;
                    Prefabs::RecordFieldOverride(scene, sel, "UI Rect", "Position");
                    Prefabs::RecordFieldOverride(scene, sel, "UI Rect", "Size");
                    m_Ctx.MarkDirty();
                }
            }
        }

        if (!visible) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
        auto screen = [&](glm::vec2 p) { return ImVec2(origin.x + p.x, origin.y + p.y); };
        const ImU32 blue = IM_COL32(80, 170, 255, 255), faint = IM_COL32(80, 170, 255, 120);
        dl->AddRect(screen(rect->ComputedMin), screen(rect->ComputedMax), editable ? blue : faint, 0.0f, 0, 1.5f);
        if (editable)
        {
            for (int h = 1; h <= 8; ++h)
            {
                glm::vec2 p = handlePos(h);
                bool active = hot == h || m_UIRectDrag == h;
                dl->AddRectFilled(screen(p - glm::vec2(4.0f)), screen(p + glm::vec2(4.0f)), active ? IM_COL32(255, 255, 255, 255) : blue);
                dl->AddRect(screen(p - glm::vec2(4.0f)), screen(p + glm::vec2(4.0f)), IM_COL32(20, 40, 70, 255));
            }
            // Pivot.
            glm::vec2 pivot = glm::mix(rect->ComputedMin, rect->ComputedMax, rect->Pivot);
            dl->AddCircle(screen(pivot), 5.0f, blue, 16, 1.5f);
            if (hot >= 0 || m_UIRectDrag >= 0)
                ImGui::SetMouseCursor(m_UIRectDrag == 0 || hot == 0 ? ImGuiMouseCursor_ResizeAll
                                      : (kSides[std::max(hot, m_UIRectDrag)].x == 0 ? ImGuiMouseCursor_ResizeNS
                                         : kSides[std::max(hot, m_UIRectDrag)].y == 0 ? ImGuiMouseCursor_ResizeEW
                                         : kSides[std::max(hot, m_UIRectDrag)].x == kSides[std::max(hot, m_UIRectDrag)].y ? ImGuiMouseCursor_ResizeNWSE
                                                                                                                         : ImGuiMouseCursor_ResizeNESW));
            if (m_UIRectDrag >= 0)
            {
                char info[64];
                std::snprintf(info, sizeof(info), "pos %.0f, %.0f   size %.0f x %.0f", rect->Position.x, rect->Position.y, rect->Size.x, rect->Size.y);
                glm::vec2 at = rect->ComputedMax + glm::vec2(6.0f, 4.0f);
                dl->AddText(screen(at), IM_COL32(255, 255, 255, 230), info);
            }
        }
        dl->PopClipRect();
    }

    void EditorLayer::DrawNavigationOverlay(ImVec2 origin, ImVec2 size)
    {
        // Toggle in the Game view's bottom-right corner, clear of most game HUDs (like the Visualize button on Unity's
        // Selectables).
        const char* label = m_Options.VisualizeNavigation ? "Navigation: On" : "Navigation";
        ImVec2 ts = ImGui::CalcTextSize(label);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - ts.x - ImGui::GetStyle().FramePadding.x * 2.0f - 8.0f,
                                         origin.y + size.y - ImGui::GetFrameHeight() - 6.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, m_Options.VisualizeNavigation ? ImVec4(0.75f, 0.6f, 0.1f, 0.85f) : ImVec4(0.1f, 0.1f, 0.12f, 0.6f));
        if (ImGui::SmallButton(label)) m_Options.VisualizeNavigation = !m_Options.VisualizeNavigation;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shows where the arrow keys and gamepad move between UI widgets.\nYellow: nearest widget (automatic). Cyan: an explicit UI Navigation link.");
        if (!m_Options.VisualizeNavigation) return;

        Scene& scene = m_Ctx.ActiveScene();
        auto order = Dimension::UI::ComputeLayout(scene, { size.x, size.y });
        auto links = Dimension::UI::NavigationLinks(scene, order);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
        for (const auto& link : links)
        {
            static const glm::vec2 dirs[4] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
            glm::vec2 dir = dirs[(int)link.Direction];
            glm::vec2 perp(-dir.y, dir.x);
            // Leave from the side facing the move and arrive at the target's side facing back; opposite moves between
            // the same pair sit side by side.
            auto sidePoint = [&](glm::vec2 mn, glm::vec2 mx, glm::vec2 side) {
                glm::vec2 c = (mn + mx) * 0.5f, half = (mx - mn) * 0.5f;
                glm::vec2 p = c + side * half + perp * std::min(6.0f, std::abs(glm::dot(perp, half)) * 0.8f);
                return ImVec2(origin.x + p.x, origin.y + p.y);
            };
            ImVec2 a = sidePoint(link.FromMin, link.FromMax, dir), b = sidePoint(link.ToMin, link.ToMax, -dir);
            float reach = std::max(20.0f, std::hypot(b.x - a.x, b.y - a.y) * 0.35f);
            ImVec2 c1(a.x + dir.x * reach, a.y + dir.y * reach), c2(b.x - dir.x * reach, b.y - dir.y * reach);
            ImU32 col = link.Explicit ? IM_COL32(80, 220, 255, 230) : IM_COL32(255, 210, 60, 220);
            dl->AddBezierCubic(a, c1, c2, b, col, 2.0f);
            // Arrowhead along the final tangent.
            glm::vec2 t = glm::normalize(glm::vec2(b.x - c2.x, b.y - c2.y) + dir * 1e-3f);
            glm::vec2 n(-t.y, t.x);
            dl->AddTriangleFilled(b, ImVec2(b.x - t.x * 9 + n.x * 4.5f, b.y - t.y * 9 + n.y * 4.5f), ImVec2(b.x - t.x * 9 - n.x * 4.5f, b.y - t.y * 9 - n.y * 4.5f), col);
            dl->AddCircleFilled(a, 2.5f, col);
        }
        dl->PopClipRect();
    }

    void EditorLayer::DrawStats()
    {
        if (!ImGui::Begin("Stats", &m_ShowStats))
        {
            ImGui::End();
            return;
        }
        auto& device = Application::Get().GetDevice();
        const auto& st = m_SceneRenderer.GetStats();
        ImGui::Text("%.0f FPS  (%.2f ms)", m_DisplayFps, m_DisplayMs);
        ImGui::Text("GPU: %s", device.GetProperties().deviceName);
        ImGui::Separator();
        ImGui::Text("Scene view: %u draw calls, %u triangles", st.DrawCalls, st.Triangles);
        ImGui::Text("Meshes: %u   Sprites: %u   Culled: %u   Occluded: %u", st.Meshes, st.Sprites, st.Culled, st.Occluded);
        ImGui::Text("Shadow casters: %u   Instanced meshes: %u", st.ShadowCasters, st.InstancedMeshes);
        bool gpuCulling = SceneRenderer::IsGpuLightCullingEnabled();
        if (gpuCulling) ImGui::Text("Point/spot lights: %u   (culled on the GPU)", st.Lights);
        else ImGui::Text("Point/spot lights: %u   in clusters: %u", st.Lights, st.ClusterLightRefs);
        ImGui::SameLine();
        UI::HelpMarker("Lights are sorted into 16 x 9 x 24 view clusters; each pixel shades only its cluster's lights (up to 1024 lights).");
        if (ImGui::Checkbox("GPU Light Culling", &gpuCulling)) SceneRenderer::SetGpuLightCullingEnabled(gpuCulling);
        ImGui::SameLine();
        UI::HelpMarker("A compute shader fills the clusters, testing each light against each cluster's bounds. Off: the CPU builds them.");
        if (st.GpuParticleCapacity > 0) ImGui::Text("GPU particle slots: %u", st.GpuParticleCapacity);
        bool instancing = SceneRenderer::IsInstancingEnabled();
        if (ImGui::Checkbox("GPU Instancing", &instancing)) SceneRenderer::SetInstancingEnabled(instancing);
        ImGui::SameLine();
        UI::HelpMarker("Identical opaque meshes (same mesh and material) are drawn with one instanced call per pass.");
        ImGui::Text("Entities: %zu", m_Ctx.ActiveScene().GetEntityCount());
        bool vsync = device.GetVSync();
        if (ImGui::Checkbox("VSync", &vsync)) device.SetVSync(vsync);
        ImGui::End();
    }

    void EditorLayer::DrawProjectSettings()
    {
        ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
        if (!ImGui::Begin("Project Settings", &m_ShowProjectSettings, ImGuiWindowFlags_NoDocking))
        {
            ImGui::End();
            return;
        }
        auto& config = Project::GetConfig();
        bool changed = false;
        if (UI::BeginProperties("ProjectProps"))
        {
            UI::PropertyLabel("Product Name");
            changed |= ImGui::InputText("##name", &config.Name);
            changed |= UI::AssetField("Start Scene", config.StartScene, AssetType::Scene).Changed;
            UI::PropertyLabel("Window Width");
            changed |= ImGui::InputScalar("##w", ImGuiDataType_U32, &config.WindowWidth);
            UI::PropertyLabel("Window Height");
            changed |= ImGui::InputScalar("##h", ImGuiDataType_U32, &config.WindowHeight);
            UI::EndProperties();
        }
        if (ImGui::CollapsingHeader("Physics Layers"))
        {
            ImGui::TextDisabled("Layers 0-1 are built in. Name a layer to use it.");
            for (int i = 0; i < ProjectConfig::kMaxLayers; ++i)
            {
                ImGui::PushID(i);
                ImGui::Text("%2d", i);
                ImGui::SameLine(40);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (i < 2)
                    ImGui::TextDisabled("%s", config.PhysicsLayers[i].c_str());
                else
                    changed |= ImGui::InputText("##layer", &config.PhysicsLayers[i]);
                ImGui::PopID();
            }
        }
        if (ImGui::CollapsingHeader("Layer Collision Matrix"))
        {
            // Unity-style triangle: row i, column j (j <= i) for named layers.
            std::vector<int> named;
            for (int i = 0; i < ProjectConfig::kMaxLayers; ++i)
                if (!config.PhysicsLayers[i].empty()) named.push_back(i);
            if (ImGui::BeginTable("##matrix", (int)named.size() + 1, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders))
            {
                ImGui::TableSetupColumn("");
                for (int j : named) ImGui::TableSetupColumn(std::to_string(j).c_str());
                ImGui::TableHeadersRow();
                for (int i : named)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d: %s", i, config.PhysicsLayers[i].c_str());
                    for (int j : named)
                    {
                        ImGui::TableNextColumn();
                        if (j > i) continue;
                        bool on = config.LayersCollide(i, j);
                        ImGui::PushID(i * 64 + j);
                        if (ImGui::Checkbox("##c", &on))
                        {
                            config.SetLayersCollide(i, j, on);
                            changed = true;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s <-> %s", config.PhysicsLayers[i].c_str(), config.PhysicsLayers[j].c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Changes apply the next time play mode starts.");
        }
        if (ImGui::CollapsingHeader("Import Defaults", config.ImportRules.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Folder rules: the import settings new textures and models get there (the deepest folder's rule wins).
            ImGui::TextDisabled("Set from a texture or model's Inspector: Make Default for Folder.");
            std::optional<size_t> remove;
            if (config.ImportRules.empty()) ImGui::TextDisabled("No import defaults yet.");
            else if (ImGui::BeginTable("##importRules", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders))
            {
                ImGui::TableSetupColumn("Folder");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Settings");
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < config.ImportRules.size(); ++i)
                {
                    const ImportRule& r = config.ImportRules[i];
                    ImGui::PushID((int)i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.Folder.empty() ? "Assets" : ("Assets/" + r.Folder).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.Type == AssetType::Texture ? "Texture" : r.Type == AssetType::Mesh ? "Model" : AssetTypeToString(r.Type));
                    ImGui::TableNextColumn();
                    std::string summary;
                    for (auto& [k, v] : r.Settings.items()) summary += (summary.empty() ? "" : ", ") + k + " " + v.dump();
                    ImGui::TextWrapped("%s", summary.c_str());
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Apply to Existing"))
                    {
                        int n = ImportDefaults::ApplyToExisting(r.Folder, r.Type);
                        Log::Info("Applied the import defaults of {} to {} asset(s)", r.Folder.empty() ? "Assets" : r.Folder, n);
                    }
                    ImGui::SetItemTooltip("Give the assets already in this folder these settings (not those under a deeper folder's rule)");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) remove = i;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (remove)
            {
                config.ImportRules.erase(config.ImportRules.begin() + *remove);
                changed = true;
            }
        }
        ImGui::TextDisabled("Project folder: %s", Project::GetDirectory().string().c_str());
        if (changed) Project::Save();
        ImGui::End();
    }

    void EditorLayer::DrawAbout()
    {
        ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
        if (ImGui::Begin("About Dimension", &m_ShowAbout, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize))
        {
            ImGui::Text("Dimension Engine 0.1.0");
            ImGui::TextWrapped("2D/3D Vulkan game engine with an ECS core. Built with GLFW, Dear ImGui, ImGuizmo, "
                               "VMA, volk, glm, Box2D, Jolt Physics, miniaudio, cgltf, stb and nlohmann/json.");
        }
        ImGui::End();
    }

    void EditorLayer::HandleShortcuts()
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) return;
        bool ctrl = io.KeyCtrl, shift = io.KeyShift;

        // The keys are the Shortcuts window's (Shortcuts::Pressed).
        if (m_RebindingShortcut >= 0) return; // the Shortcuts window is listening for a new key
        using Shortcuts::Pressed;
        if (Pressed("play.play")) m_Ctx.IsPlaying() ? Stop() : Play();
        if (Pressed("play.pause")) TogglePause();
        if (Pressed("file.save")) SaveScene();
        if (Pressed("file.saveAs")) SaveSceneAs();
        if (Pressed("file.new")) m_ShowNewScene = true;
        if (Pressed("file.buildModule")) BuildGameModule();
        if (Pressed("file.buildSettings")) m_ShowBuildSettings = true;
        if (Pressed("file.buildAndRun") && !m_Ctx.IsPlaying()) BuildGame(true, Project::GetConfig().BuildFolder);
        if (Pressed("file.open")) OpenSceneWithDialog();
        if (Pressed("edit.search")) OpenSearch(true);
        if (m_GameViewFocused && m_Ctx.IsPlaying()) return; // the game owns the keyboard

        if (Pressed("edit.undo")) m_Ctx.Undo();
        if (Pressed("edit.redo")) m_Ctx.Redo();
        if (Pressed("edit.duplicate")) DuplicateSelected(m_Ctx);
        if (Pressed("edit.copy")) CopySelected(m_Ctx);
        if (Pressed("edit.paste")) PasteClipboard(m_Ctx);
        if (Pressed("edit.delete") && !((m_SceneViewHovered || m_SceneViewFocused) && DeleteLinePoint())) DeleteSelected(m_Ctx);
        if (Pressed("edit.group") && !m_Ctx.IsPlaying()) GroupSelected(m_Ctx);
        if (Pressed("scene.moveToView") && !m_Ctx.IsPlaying()) MoveSelectionToView();
        if (Pressed("scene.alignWithView") && !m_Ctx.IsPlaying()) AlignSelectionWithView();
        if (Pressed("scene.alignViewToSelected")) AlignViewToSelected();
        if (Pressed("edit.ungroup") && !m_Ctx.IsPlaying()) UngroupSelected(m_Ctx);
        // Selection history and saved selections.
        if (Pressed("edit.selectBack")) SelectionBack();
        if (Pressed("edit.selectForward")) SelectionForward();
        for (int slot = 1; slot <= SelectionSets::kSlots; ++slot)
        {
            std::string n = std::to_string(slot);
            if (Pressed(("edit.selectionSet" + n).c_str())) RestoreSelection(slot);
            if (Pressed(("edit.saveSelectionSet" + n).c_str())) SaveSelection(slot);
        }
        // Isolate the selection (only it shows), or leave isolation.
        if (Pressed("scene.isolate"))
        {
            if (m_Ctx.Visibility.Isolating()) m_Ctx.Visibility.EndIsolation();
            else if (m_Ctx.HasValidSelection()) m_Ctx.Visibility.Isolate(m_Ctx.ActiveScene(), m_Ctx.Selection);
        }
        // Hide or show the selection in the Scene view (all hidden: show them, else hide them).
        else if (Pressed("scene.hide") && m_Ctx.HasValidSelection())
        {
            Scene& scene = m_Ctx.ActiveScene();
            bool allHidden = std::all_of(m_Ctx.Selection.begin(), m_Ctx.Selection.end(), [&](Entity s) { return m_Ctx.Visibility.IsHiddenSelf(scene, s); });
            for (Entity s : m_Ctx.Selection) m_Ctx.Visibility.SetHidden(scene, s, !allHidden);
            m_Ctx.VisibilityChanged = true;
        }

        bool viewActive = m_SceneViewHovered || m_SceneViewFocused;
        if (viewActive && !m_Camera.IsControlling())
        {
            if (Pressed("tool.view")) m_GizmoOperation = 0;
            if (Pressed("tool.move")) m_GizmoOperation = ImGuizmo::TRANSLATE;
            if (Pressed("tool.rotate")) m_GizmoOperation = ImGuizmo::ROTATE;
            if (Pressed("tool.scale")) m_GizmoOperation = ImGuizmo::SCALE;
            if (Pressed("tool.measure")) SetMeasuring(!m_Measuring);
            if (Pressed("scene.frame")) FocusSelected();
            if (Pressed("scene.frameAll")) FrameAll();
            for (int slot = 1; slot <= SceneBookmarks::kSlots; ++slot)
            {
                std::string n = std::to_string(slot);
                if (Pressed(("scene.view" + n).c_str())) GoToView(slot);
                if (Pressed(("scene.saveView" + n).c_str())) SaveView(slot);
            }
        }
    }

    void EditorLayer::ApplyPreferences()
    {
        auto& prefs = EditorPreferences::Get();
        ImGuiLayer::ApplyEditorStyle(std::clamp(prefs.Theme, 0, 2), prefs.Accent);
        ImGui::GetStyle().FontScaleMain = std::clamp(prefs.UIScale, 0.5f, 3.0f);
        m_Camera.FieldOfView = std::clamp(prefs.CameraFieldOfView, 10.0f, 150.0f);
        if (m_Console) m_Console->ClearOnPlay = prefs.ClearConsoleOnPlay;
    }

    void EditorLayer::DrawPreferences()
    {
        ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
        if (!ImGui::Begin("Preferences", &m_ShowPreferences, ImGuiWindowFlags_NoDocking))
        {
            ImGui::End();
            return;
        }
        auto& prefs = EditorPreferences::Get();
        bool changed = false;
        ImGui::SeparatorText("Interface");
        if (UI::BeginProperties("PrefsUI"))
        {
            UI::PropertyLabel("Theme");
            const char* themes[] = { "Dark", "Light", "Midnight" };
            changed |= ImGui::Combo("##theme", &prefs.Theme, themes, 3);
            UI::PropertyLabel("Accent Color", "Selection, active items and focus highlights");
            float accent[3] = { ((prefs.Accent >> 16) & 0xFF) / 255.0f, ((prefs.Accent >> 8) & 0xFF) / 255.0f, (prefs.Accent & 0xFF) / 255.0f };
            if (ImGui::ColorEdit3("##accent", accent, ImGuiColorEditFlags_NoInputs))
            {
                auto byte = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
                prefs.Accent = (byte(accent[0]) << 16) | (byte(accent[1]) << 8) | byte(accent[2]);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) { prefs.Accent = ImGuiLayer::kDefaultAccent; changed = true; }
            UI::PropertyLabel("UI Scale");
            changed |= ImGui::SliderFloat("##scale", &prefs.UIScale, 0.75f, 2.0f, "%.2f");
            UI::PropertyLabel("Clear Console On Play");
            changed |= ImGui::Checkbox("##clear", &prefs.ClearConsoleOnPlay);
            UI::EndProperties();
        }
        ImGui::SeparatorText("Scene View");
        if (UI::BeginProperties("PrefsScene"))
        {
            UI::PropertyLabel("Camera Fly Speed", "Starting speed; the mouse wheel changes it while flying");
            if (ImGui::DragFloat("##fly", &prefs.CameraFlySpeed, 0.1f, 0.1f, 500.0f))
            {
                m_Camera.SetFlySpeed(prefs.CameraFlySpeed);
                changed = true;
            }
            UI::PropertyLabel("Field Of View");
            changed |= ImGui::SliderFloat("##fov", &prefs.CameraFieldOfView, 20.0f, 120.0f, "%.0f deg");
            UI::PropertyLabel("Show Grid");
            if (ImGui::Checkbox("##grid", &prefs.ShowGrid))
            {
                m_ShowGrid = prefs.ShowGrid;
                changed = true;
            }
            UI::EndProperties();
        }
        ImGui::SeparatorText("Saving");
        if (UI::BeginProperties("PrefsSave"))
        {
            UI::PropertyLabel("Autosave (minutes)", "Backs up the open scene to .dimension/Autosave; 0 turns it off");
            changed |= ImGui::DragFloat("##autosave", &prefs.AutosaveMinutes, 0.1f, 0.0f, 120.0f, "%.1f");
            UI::PropertyLabel("Save Scene Before Play");
            changed |= ImGui::Checkbox("##saveplay", &prefs.SaveSceneBeforePlay);
            UI::EndProperties();
        }
        ImGui::TextDisabled("Stored in %s", EditorPreferences::FilePath().string().c_str());
        if (changed)
        {
            ApplyPreferences();
            prefs.Save();
        }
        ImGui::End();
    }

    // Post-fader level meter beside a mixer fader, on the fader's dB scale: the RMS bar (green, yellow above -12 dB,
    // red above -3 dB), the decaying peak as a thin line, and a peak hold that drops after a second.
    void EditorLayer::DrawLevelMeter(int group, float height)
    {
        if ((int)m_MixerMeters.size() <= group) m_MixerMeters.resize(group + 1);
        MeterDisplay& m = m_MixerMeters[group];
        AudioLevel level = AudioMixer::GetLevel(group);
        float dt = ImGui::GetIO().DeltaTime;
        float rmsDb = AudioMixer::LinearToDb(level.Rms), peakDb = AudioMixer::LinearToDb(level.Peak);
        m.Rms = rmsDb;
        m.Peak = std::max(peakDb, m.Peak - 30.0f * dt); // falls 30 dB per second
        if (peakDb >= m.Hold) { m.Hold = peakDb; m.HoldTime = 1.0f; }
        else if ((m.HoldTime -= dt) <= 0.0f) m.Hold = std::max(m.Hold - 40.0f * dt, AudioMixer::kSilentDb);

        const float lo = AudioMixer::kSilentDb, hi = 10.0f, w = 8.0f;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, height));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Level: RMS %.1f dB, peak %.1f dB", std::max(m.Rms, lo), std::max(m.Hold, lo));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto y = [&](float db) { return p0.y + height * (1.0f - (std::clamp(db, lo, hi) - lo) / (hi - lo)); };
        ImVec2 p1(p0.x + w, p0.y + height);
        dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 2.0f);
        float top = y(m.Rms);
        // Colored zones of the bar, bottom to top.
        struct Zone { float From, To; ImU32 Color; } zones[] = { { lo, -12.0f, IM_COL32(70, 200, 90, 255) },
                                                              { -12.0f, -3.0f, IM_COL32(225, 200, 60, 255) },
                                                              { -3.0f, hi, IM_COL32(230, 70, 60, 255) } };
        for (auto& z : zones)
        {
            float zTop = std::max(y(z.To), top), zBottom = y(z.From);
            if (zTop < zBottom) dl->AddRectFilled(ImVec2(p0.x + 1, zTop), ImVec2(p1.x - 1, zBottom), z.Color);
        }
        if (m.Peak > lo) dl->AddLine(ImVec2(p0.x + 1, y(m.Peak)), ImVec2(p1.x - 1, y(m.Peak)), IM_COL32(255, 255, 255, 170));
        if (m.Hold > lo) dl->AddLine(ImVec2(p0.x, y(m.Hold)), ImVec2(p1.x, y(m.Hold)), m.Hold > 0.0f ? IM_COL32(255, 80, 70, 255) : IM_COL32(240, 240, 240, 255), 2.0f);
        dl->AddLine(ImVec2(p0.x, y(0.0f)), ImVec2(p1.x, y(0.0f)), IM_COL32(255, 255, 255, 60)); // 0 dB mark
    }

    void EditorLayer::DrawAudioMixer()
    {
        // First appearance in an existing layout: dock next to the Console instead of floating.
        if (ImGuiWindow* console = ImGui::FindWindowByName("Console"); console && console->DockId)
            ImGui::SetNextWindowDockID(console->DockId, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(720, 300), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Audio Mixer", &m_ShowAudioMixer))
        {
            ImGui::End();
            return;
        }
        auto& groups = AudioMixer::Groups();
        bool live = false, structure = false, save = false;
        int remove = -1;
        const float stripW = 150.0f;
        ImGui::TextDisabled("Audio Sources pick a group in their Output field. Volume, mute, solo and low-pass apply live;"
                            " new groups and parents apply when play starts.");
        // Snapshots: click to recall, right-click to overwrite, rename or delete. Scripts blend to them over time.
        {
            auto& snaps = AudioMixer::Snapshots();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Snapshots");
            ImGui::SameLine();
            UI::HelpMarker("A snapshot stores every group's volume and low-pass cutoff. Scripts blend to one with "
                           "AudioMixer::TransitionToSnapshot(\"Paused\", 0.5f) (C#: AudioMixer.TransitionToSnapshot).");
            int removeSnap = -1;
            for (int s = 0; s < (int)snaps.size(); ++s)
            {
                ImGui::SameLine();
                ImGui::PushID(1000 + s);
                if (ImGui::Button(snaps[s].Name.c_str()))
                {
                    AudioMixer::TransitionToSnapshot(snaps[s].Name, 0.0f);
                    save = !m_Ctx.IsPlaying();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recall (right-click for more)");
                if (ImGui::BeginPopupContextItem("##snap"))
                {
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::InputText("##rename", &snaps[s].Name)) save = true;
                    if (ImGui::MenuItem("Overwrite With Current Mix"))
                    {
                        AudioMixer::CaptureSnapshot(snaps[s].Name);
                        save = true;
                    }
                    if (ImGui::MenuItem("Delete")) removeSnap = s;
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Snapshot"))
            {
                std::string name = "Snapshot";
                for (int n = 2; AudioMixer::FindSnapshot(name) >= 0; ++n) name = "Snapshot " + std::to_string(n);
                AudioMixer::CaptureSnapshot(name);
                save = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Capture the current mix as a new snapshot");
            if (removeSnap >= 0)
            {
                snaps.erase(snaps.begin() + removeSnap);
                save = true;
            }
        }
        if (ImGui::BeginChild("##strips", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
        {
            for (int i = 0; i < (int)groups.size(); ++i)
            {
                auto& g = groups[i];
                if (i > 0) ImGui::SameLine();
                ImGui::PushID(i);
                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, i == 0 ? UI::AccentBackground(0.18f) : ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
                ImGui::BeginChild("##strip", ImVec2(stripW, 0), ImGuiChildFlags_Borders);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (i == 0) ImGui::TextUnformatted(g.Name.c_str());
                else
                {
                    ImGui::InputText("##name", &g.Name);
                    save |= ImGui::IsItemDeactivatedAfterEdit();
                }
                // Left: full-height fader. Right: readout and controls (fits short docks).
                float faderH = std::max(40.0f, ImGui::GetContentRegionAvail().y);
                if (ImGui::VSliderFloat("##db", ImVec2(28, faderH), &g.VolumeDb, AudioMixer::kSilentDb, 10.0f, "", ImGuiSliderFlags_None)) live = true;
                save |= ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { g.VolumeDb = 0.0f; live = save = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Volume (double-click: 0 dB)");
                ImGui::SameLine(0.0f, 3.0f);
                DrawLevelMeter(i, faderH);
                ImGui::SameLine();
                ImGui::BeginGroup();
                float controlsW = ImGui::GetContentRegionAvail().x;
                ImGui::Text(g.VolumeDb <= AudioMixer::kSilentDb ? "-inf dB" : "%.1f dB", g.VolumeDb);
                auto toggle = [&](const char* label, bool& value, ImVec4 on, const char* tip) {
                    if (value) ImGui::PushStyleColor(ImGuiCol_Button, on);
                    if (ImGui::Button(label, ImVec2((controlsW - ImGui::GetStyle().ItemSpacing.x) * 0.5f, 0))) { value = !value; live = save = true; }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                    if (value) ImGui::PopStyleColor();
                };
                toggle("M", g.Mute, ImVec4(0.75f, 0.3f, 0.25f, 1), "Mute");
                ImGui::SameLine();
                toggle("S", g.Solo, ImVec4(0.75f, 0.65f, 0.2f, 1), "Solo");
                ImGui::SetNextItemWidth(controlsW);
                if (ImGui::DragFloat("##lpf", &g.LowPassHz, 25.0f, 0.0f, 20000.0f, g.LowPassHz <= 0.0f ? "LPF off" : "LPF %.0f")) live = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Low-pass cutoff in Hz (0 = off)");
                save |= ImGui::IsItemDeactivatedAfterEdit();
                // Insert effects in a popup (strips are narrow).
                {
                    bool sending = g.SendTarget >= 0 && g.SendTarget < (int)groups.size() && g.SendTarget != i;
                    std::string fx = std::string("FX") + (g.Echo ? " E" : "") + (g.Reverb ? " R" : "") + (sending ? " >" : "") + "##fx";
                    bool any = g.Echo || g.Reverb || g.Convolution || sending;
                    if (any) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.7f, 1));
                    if (ImGui::Button(fx.c_str(), ImVec2(controlsW, 0))) ImGui::OpenPopup("##effects");
                    if (any) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(sending ? "Echo, reverb and send (to %s)" : "Echo, reverb and send on this group",
                                          sending ? groups[g.SendTarget].Name.c_str() : "");
                    if (ImGui::BeginPopup("##effects"))
                    {
                        auto edited = [&](bool changed) { live |= changed; save |= ImGui::IsItemDeactivatedAfterEdit(); };
                        ImGui::SeparatorText("Echo");
                        if (ImGui::Checkbox("Enabled##echo", &g.Echo)) live = save = true;
                        ImGui::SetNextItemWidth(180);
                        edited(ImGui::DragFloat("Delay (ms)", &g.EchoDelayMs, 2.0f, 10.0f, 5000.0f, "%.0f"));
                        ImGui::SetItemTooltip("A new delay time applies when play starts");
                        ImGui::SetNextItemWidth(180);
                        edited(ImGui::SliderFloat("Decay##echo", &g.EchoDecay, 0.0f, 0.95f));
                        ImGui::SetNextItemWidth(180);
                        edited(ImGui::SliderFloat("Wet##echo", &g.EchoWet, 0.0f, 1.0f));
                        ImGui::SeparatorText("Reverb");
                        if (ImGui::Checkbox("Enabled##reverb", &g.Reverb)) live = save = true;
                        ImGui::SetNextItemWidth(180);
                        edited(ImGui::SliderFloat("Room Size", &g.ReverbRoomSize, 0.0f, 1.0f));
                        ImGui::SetNextItemWidth(180);
                        edited(ImGui::SliderFloat("Damping", &g.ReverbDamping, 0.0f, 1.0f));
                        ImGui::SetNextItemWidth(180);
                        edited(ImGui::SliderFloat("Wet##reverb", &g.ReverbWet, 0.0f, 1.0f));
                        ImGui::SeparatorText("Convolution Reverb");
                        if (ImGui::Checkbox("Enabled##conv", &g.Convolution)) live = save = true;
                        {
                            AssetHandle impulse(g.ConvolutionImpulse);
                            ImGui::SetNextItemWidth(180);
                            if (UI::AssetField("Impulse Response", impulse, AssetType::Audio).Changed)
                            {
                                g.ConvolutionImpulse = (uint64_t)impulse;
                                save = true;
                            }
                            ImGui::SetItemTooltip("An audio clip of a room's response (a recorded clap or a generated tail);\n"
                                                  "a new one applies when play starts");
                        }
                        ImGui::SetNextItemWidth(180);
                        edited(ImGui::SliderFloat("Wet##conv", &g.ConvolutionWet, 0.0f, 2.0f));
                        ImGui::SeparatorText("Send");
                        ImGui::SetNextItemWidth(180);
                        const char* sendName = sending ? groups[g.SendTarget].Name.c_str() : "None";
                        if (ImGui::BeginCombo("To##send", sendName))
                        {
                            if (ImGui::Selectable("None", !sending)) { g.SendTarget = -1; live = save = true; }
                            for (int t = 0; t < (int)groups.size(); ++t)
                            {
                                if (t == i) continue;
                                // Targets that would feed back into this group are disabled.
                                auto trial = groups;
                                trial[i].SendTarget = t;
                                bool loops = AudioMixer::ValidSends(trial)[i] != t;
                                if (ImGui::Selectable(groups[t].Name.c_str(), g.SendTarget == t, loops ? ImGuiSelectableFlags_Disabled : 0))
                                {
                                    g.SendTarget = t;
                                    live = save = true;
                                }
                                if (loops) ImGui::SetItemTooltip("Would feed back into this group");
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SetItemTooltip("A copy of this group's output (after its volume and effects) also goes to that group, e.g. a "
                                              "return group with a reverb shared by several groups");
                        ImGui::SetNextItemWidth(180);
                        ImGui::BeginDisabled(!sending);
                        edited(ImGui::SliderFloat("Level (dB)##send", &g.SendLevelDb, AudioMixer::kSilentDb, 10.0f, "%.1f"));
                        ImGui::EndDisabled();
                        ImGui::EndPopup();
                    }
                }
                if (i > 0)
                {
                    ImGui::SetNextItemWidth(controlsW);
                    const char* parentName = g.Parent >= 0 && g.Parent < (int)groups.size() ? groups[g.Parent].Name.c_str() : "Master";
                    if (ImGui::BeginCombo("##parent", parentName))
                    {
                        for (int p = 0; p < (int)groups.size(); ++p)
                            if (p != i && ImGui::Selectable(groups[p].Name.c_str(), p == g.Parent))
                            {
                                g.Parent = p;
                                structure = save = true;
                            }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output (parent group)");
                    if (ImGui::Button("Remove", ImVec2(controlsW, 0))) remove = i;
                }
                ImGui::EndGroup();
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                ImGui::PopID();
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Group", ImVec2(80, 0)))
            {
                AudioMixerGroup g;
                g.Name = "Group " + std::to_string(groups.size());
                groups.push_back(g);
                structure = save = true;
            }
        }
        ImGui::EndChild();
        if (remove > 0)
        {
            int parent = groups[remove].Parent;
            groups.erase(groups.begin() + remove);
            for (auto& g : groups)
            {
                if (g.Parent == remove) g.Parent = parent > remove ? parent - 1 : parent;
                else if (g.Parent > remove) --g.Parent;
                if (g.SendTarget == remove) g.SendTarget = -1;
                else if (g.SendTarget > remove) --g.SendTarget;
            }
            structure = save = true;
        }
        if (live) AudioMixer::Apply();
        if (structure) AudioMixer::MarkStructureDirty();
        if (save) Project::Save();
        ImGui::End();
    }

    void EditorLayer::DrawTilePalette()
    {
        ImGui::SetNextWindowSize(ImVec2(300, 360), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Tile Palette", &m_ShowTilePalette))
        {
            ImGui::End();
            return;
        }
        Scene& scene = m_Ctx.ActiveScene();
        auto* tm = m_Ctx.HasValidSelection() ? scene.GetRegistry().TryGet<TilemapComponent>(m_Ctx.Selected) : nullptr;
        if (!tm)
        {
            ImGui::TextWrapped("Select an entity with a Tilemap (GameObject > 2D Object > Tilemap) to paint it.");
            ImGui::End();
            return;
        }
        ImGui::RadioButton("Paint", &m_TileTool, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Erase", &m_TileTool, 1);
        ImGui::SameLine();
        ImGui::TextDisabled("(Shift: erase)");
        ImGui::TextDisabled("%zu tiles. Left-drag in the Scene view.", tm->Count());

        auto tex = AssetManager::GetTexture(tm->Tileset);
        int cols = std::max(1, tm->Columns), rows = std::max(1, tm->Rows), count = cols * rows;
        m_TileBrush = std::clamp(m_TileBrush, 0, count - 1);
        if (!tex) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Assign a Tileset texture to the Tilemap.");
        else
        {
            float cell = 48.0f, spacing = ImGui::GetStyle().ItemSpacing.x;
            int perRow = std::max(1, (int)((ImGui::GetContentRegionAvail().x + spacing) / (cell + spacing + 8)));
            for (int i = 0; i < count; ++i)
            {
                ImGui::PushID(i);
                ImVec2 uv0((float)(i % cols) / cols, (float)(i / cols) / rows), uv1(uv0.x + 1.0f / cols, uv0.y + 1.0f / rows);
                bool selected = i == m_TileBrush;
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.95f, 1));
                if (ImGui::ImageButton("##tile", ImTextureRef((ImTextureID)tex->GetImGuiId()), ImVec2(cell, cell), uv0, uv1)) { m_TileBrush = i; m_TileTool = 0; }
                if (selected) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tile %d", i);
                if ((i + 1) % perRow != 0 && i + 1 < count) ImGui::SameLine();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Clear All Tiles") && FileDialogs::Confirm("Clear Tilemap", "Remove every tile from this tilemap?"))
        {
            m_Ctx.RecordUndo();
            tm->Clear();
            m_Ctx.MarkDirty();
        }
        ImGui::End();
    }

    bool EditorLayer::UpdateTerrainBrush(const ImVec2& origin, const ImVec2& size)
    {
        // The Inspector's terrain tool sculpts or paints the selected terrain where the mouse points.
        Scene& scene = m_Ctx.ActiveScene();
        auto* terrain = m_Ctx.HasValidSelection() && m_Ctx.TerrainTool > 0 && !m_Ctx.IsPlaying()
                            ? scene.GetRegistry().TryGet<TerrainComponent>(m_Ctx.Selected) : nullptr;
        if (!terrain || m_Camera.IsControlling())
        {
            if (m_TerrainStroke && terrain) Terrain::CommitEdit(*terrain);
            m_TerrainStroke = false;
            return false;
        }
        auto& t = scene.GetRegistry().Get<TransformComponent>(m_Ctx.Selected);
        glm::mat4 viewProj = m_Camera.GetProjection(size.x / std::max(1.0f, size.y)) * m_Camera.GetView();
        glm::mat4 inv = glm::inverse(viewProj);
        ImVec2 mouse = ImGui::GetMousePos();
        glm::vec2 ndc((mouse.x - origin.x) / size.x * 2.0f - 1.0f, 1.0f - (mouse.y - origin.y) / size.y * 2.0f);
        glm::vec4 a = inv * glm::vec4(ndc, 0.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
        glm::vec3 from = glm::vec3(a) / a.w, to = glm::vec3(b) / b.w;
        glm::vec3 hit;
        bool onTerrain = m_SceneViewHovered && Terrain::Raycast(*terrain, t.World, from, to - from, hit);
        if (onTerrain)
        {
            // The brush outline, following the ground.
            std::vector<float> heights = Terrain::BuildHeights(*terrain);
            glm::mat4 toLocal = glm::inverse(t.World);
            glm::vec3 local = glm::vec3(toLocal * glm::vec4(hit, 1.0f));
            float radius = m_Ctx.TerrainBrushSize;
            bool stamping = m_Ctx.TerrainTool - 1 == (int)Terrain::BrushMode::Stamp;
            ImVec2 points[49];
            for (int i = 0; i <= 48; ++i)
            {
                float angle = i / 48.0f * 6.2831853f;
                glm::vec2 p(local.x + std::cos(angle) * radius, local.z + std::sin(angle) * radius);
                if (stamping)
                {
                    // The stamp's turned square, 12 points per side.
                    int side = (i / 12) % 4;
                    float f = (i % 12) / 12.0f * 2.0f - 1.0f;
                    glm::vec2 q = side == 0 ? glm::vec2(f, -1) : side == 1 ? glm::vec2(1, f) : side == 2 ? glm::vec2(-f, 1) : glm::vec2(-1, -f);
                    float a = glm::radians(m_Ctx.TerrainStamp.Rotation), c = std::cos(a), s = std::sin(a);
                    q *= radius;
                    p = glm::vec2(local.x + c * q.x - s * q.y, local.z + s * q.x + c * q.y);
                }
                glm::vec4 c = viewProj * (t.World * glm::vec4(p.x, Terrain::HeightAt(*terrain, heights, p.x, p.y) + 0.05f, p.y, 1.0f));
                glm::vec2 n = glm::vec2(c) / std::max(c.w, 1e-4f);
                points[i] = ImVec2(origin.x + (n.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - n.y * 0.5f) * size.y);
            }
            auto tool = (Terrain::BrushMode)(m_Ctx.TerrainTool - 1);
            ImU32 color = Terrain::IsDetailMode(tool) ? IM_COL32(240, 200, 90, 255) : Terrain::IsPaintMode(tool) ? IM_COL32(120, 230, 120, 255)
                        : Terrain::IsHoleMode(tool)   ? IM_COL32(240, 90, 90, 255) : IM_COL32(90, 200, 255, 255);
            ImGui::GetWindowDrawList()->AddPolyline(points, 49, color, 0, 2.0f);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
            {
                m_Ctx.RecordUndo();
                Terrain::BeginEdit(*terrain);
                m_TerrainFlatten = local.y;
                m_TerrainStroke = true;
            }
            if (m_TerrainStroke && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                auto mode = (Terrain::BrushMode)(m_Ctx.TerrainTool - 1);
                // Shift reverses raise and lower.
                if (ImGui::GetIO().KeyShift && mode == Terrain::BrushMode::Raise) mode = Terrain::BrushMode::Lower;
                else if (ImGui::GetIO().KeyShift && mode == Terrain::BrushMode::Lower) mode = Terrain::BrushMode::Raise;
                else if (ImGui::GetIO().KeyShift && mode == Terrain::BrushMode::DigHole) mode = Terrain::BrushMode::FillHole;
                else if (ImGui::GetIO().KeyShift && mode == Terrain::BrushMode::FillHole) mode = Terrain::BrushMode::DigHole;
                Terrain::StampSettings stamp = m_Ctx.TerrainStamp;
                stamp.Carve = ImGui::GetIO().KeyShift;
                Terrain::ApplyBrush(*terrain, mode, glm::vec2(local.x, local.z), radius, m_Ctx.TerrainBrushStrength,
                                    std::min(ImGui::GetIO().DeltaTime, 0.05f), m_TerrainFlatten, &stamp, m_Ctx.TerrainPaintLayer);
            }
        }
        if (m_TerrainStroke && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            Terrain::CommitEdit(*terrain);
            m_Ctx.MarkDirty();
            m_TerrainStroke = false;
        }
        return m_SceneViewHovered || m_TerrainStroke;
    }

    bool EditorLayer::UpdateTilePainting(const ImVec2& origin, const ImVec2& size)
    {
        if (!m_ShowTilePalette || !m_Ctx.HasValidSelection() || m_Camera.IsControlling())
        {
            m_TileStroke = false;
            return false;
        }
        Scene& scene = m_Ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        auto* tm = reg.TryGet<TilemapComponent>(m_Ctx.Selected);
        if (!tm || !m_SceneViewHovered)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_TileStroke = false;
            return m_TileStroke;
        }
        auto& t = reg.Get<TransformComponent>(m_Ctx.Selected);

        // Mouse ray -> the tilemap's local XY plane.
        glm::mat4 viewProj = m_Camera.GetProjection(size.x / std::max(1.0f, size.y)) * m_Camera.GetView();
        glm::mat4 inv = glm::inverse(viewProj);
        ImVec2 mouse = ImGui::GetMousePos();
        glm::vec2 ndc((mouse.x - origin.x) / size.x * 2.0f - 1.0f, 1.0f - (mouse.y - origin.y) / size.y * 2.0f);
        glm::vec4 a = inv * glm::vec4(ndc, 0.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
        glm::mat4 invWorld = glm::inverse(t.World);
        glm::vec3 la = glm::vec3(invWorld * (a / a.w)), lb = glm::vec3(invWorld * (b / b.w));
        if (std::abs(la.z - lb.z) < 1e-6f) return false;
        float s = la.z / (la.z - lb.z);
        glm::vec3 hit = la + (lb - la) * s;
        glm::ivec2 cell = tm->LocalToCell(glm::vec2(hit));

        // Hover outline.
        float cs = tm->CellSize;
        auto toScreen = [&](float x, float y) {
            glm::vec4 c = viewProj * (t.World * glm::vec4(x, y, 0.0f, 1.0f));
            glm::vec2 n = glm::vec2(c) / c.w;
            return ImVec2(origin.x + (n.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - n.y * 0.5f) * size.y);
        };
        bool erase = m_TileTool == 1 || ImGui::GetIO().KeyShift;
        ImU32 col = erase ? IM_COL32(255, 90, 80, 255) : IM_COL32(90, 200, 255, 255);
        ImGui::GetWindowDrawList()->AddQuad(toScreen(cell.x * cs, cell.y * cs), toScreen((cell.x + 1) * cs, cell.y * cs),
                                            toScreen((cell.x + 1) * cs, (cell.y + 1) * cs), toScreen(cell.x * cs, (cell.y + 1) * cs), col, 2.0f);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
        {
            m_Ctx.RecordUndo(); // one undo step per stroke
            m_TileStroke = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_TileStroke = false;
        if (m_TileStroke)
        {
            int tile = erase ? -1 : m_TileBrush;
            if (tm->GetTile(cell.x, cell.y) != tile)
            {
                tm->SetTile(cell.x, cell.y, tile);
                m_Ctx.MarkDirty();
            }
        }
        return true; // the palette owns left clicks over a selected tilemap
    }

    void EditorLayer::Autosave(float dt)
    {
        float minutes = EditorPreferences::Get().AutosaveMinutes;
        if (minutes <= 0.0f || m_Ctx.IsPlaying() || !m_Ctx.Dirty || m_Ctx.InPrefabMode()) return;
        m_AutosaveTimer += dt;
        if (m_AutosaveTimer < minutes * 60.0f) return;
        m_AutosaveTimer = 0.0f;
        // A backup copy: the real scene file is only written by Save.
        fs::path dir = Project::GetDirectory() / ".dimension" / "Autosave";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string name = m_Ctx.ScenePath.empty() ? m_Ctx.EditorScene->GetName() : m_Ctx.ScenePath.stem().string();
        fs::path file = dir / (name + ".scene");
        if (SceneSerializer::SaveToFile(*m_Ctx.EditorScene, file)) Log::Trace("Autosaved a backup of '{}' to {}", name, file.string());
    }

    void EditorLayer::DrawBuildSettings()
    {
        ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Build Settings", &m_ShowBuildSettings, ImGuiWindowFlags_NoDocking))
        {
            ImGui::End();
            return;
        }
        auto& config = Project::GetConfig();
        bool changed = false;

        ImGui::SeparatorText("Scenes In Build");
        auto& scenes = config.BuildScenes;
        int moveUp = -1, remove = -1;
        if (ImGui::BeginChild("##scenes", ImVec2(0, 170), ImGuiChildFlags_Borders))
        {
            if (scenes.empty())
                ImGui::TextDisabled(config.StartScene.IsValid() ? "Empty: the Start Scene is built alone." : "Drag scenes here from the Project panel.");
            for (int i = 0; i < (int)scenes.size(); ++i)
            {
                ImGui::PushID(i);
                bool missing = !AssetManager::GetMetadata(scenes[i]);
                ImGui::Text("%d", i);
                ImGui::SameLine(32);
                if (missing) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "(missing scene)");
                else ImGui::TextUnformatted(AssetManager::GetDisplayName(scenes[i]).c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
                ImGui::BeginDisabled(i == 0);
                if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveUp = i;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) remove = i;
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(UI::kAssetPayload))
            {
                AssetHandle h(*(const uint64_t*)payload->Data);
                auto* meta = AssetManager::GetMetadata(h);
                if (meta && meta->Type == AssetType::Scene && std::find(scenes.begin(), scenes.end(), h) == scenes.end())
                {
                    scenes.push_back(h);
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (moveUp > 0) { std::swap(scenes[moveUp], scenes[moveUp - 1]); changed = true; }
        if (remove >= 0) { scenes.erase(scenes.begin() + remove); changed = true; }
        if (ImGui::Button("Add Open Scene"))
        {
            AssetHandle current = m_Ctx.ScenePath.empty() ? AssetHandle::None() : AssetManager::GetHandle(m_Ctx.ScenePath);
            if (!current.IsValid()) Log::Warn("Save the scene inside the Assets folder before adding it to the build");
            else if (std::find(scenes.begin(), scenes.end(), current) == scenes.end())
            {
                scenes.push_back(current);
                changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Scene 0 starts the game. Scripts load others with SceneManager::LoadScene.");

        ImGui::SeparatorText("Player");
        if (UI::BeginProperties("BuildPlayer"))
        {
            UI::PropertyLabel("Product Name");
            changed |= ImGui::InputText("##name", &config.Name);
            UI::PropertyLabel("Window Size");
            int size[2] = { (int)config.WindowWidth, (int)config.WindowHeight };
            if (ImGui::InputInt2("##size", size))
            {
                config.WindowWidth = (uint32_t)std::clamp(size[0], 320, 7680);
                config.WindowHeight = (uint32_t)std::clamp(size[1], 200, 4320);
                changed = true;
            }
            UI::PropertyLabel("Fullscreen", "Borderless fullscreen; Alt+Enter toggles in the game");
            changed |= ImGui::Checkbox("##fs", &config.Fullscreen);
            UI::PropertyLabel("VSync");
            changed |= ImGui::Checkbox("##vsync", &config.VSync);
            changed |= UI::AssetField("Icon", config.Icon, AssetType::Texture).Changed;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Image for the game's executable, window and taskbar (square PNG, 256 px or more). Empty: the engine icon.");
            UI::EndProperties();
        }

        ImGui::SeparatorText("Output");
        changed |= ImGui::Checkbox("Strip Unused Assets", &config.StripUnusedAssets);
        ImGui::SameLine();
        UI::HelpMarker("Copies only the assets the build's scenes use (following prefabs, materials, controllers and models), "
                       "plus everything in Assets/Resources for scripts that load files by path.");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
        changed |= ImGui::InputTextWithHint("##folder", "Output folder", &config.BuildFolder);
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
            if (auto folder = FileDialogs::PickFolder(config.BuildFolder.empty() ? Project::GetDirectory() : fs::path(config.BuildFolder)))
            {
                config.BuildFolder = folder->string();
                changed = true;
            }
        if (changed) Project::Save();

        ImGui::Spacing();
        ImGui::BeginDisabled(m_Ctx.IsPlaying());
        if (ImGui::Button("Build", ImVec2(120, 0))) BuildGame(false, config.BuildFolder);
        ImGui::SameLine();
        if (ImGui::Button("Build And Run", ImVec2(120, 0))) BuildGame(true, config.BuildFolder);
        ImGui::EndDisabled();
        ImGui::End();
    }

    bool EditorLayer::BuildGame(bool run, fs::path folder)
    {
        bool batch = !m_Options.BuildFolder.empty();
        if (!batch && !SaveScene()) return false;
        auto& config = Project::GetConfig();
        auto scenes = SceneManager::GetBuildScenes();
        if (scenes.empty())
        {
            Log::Error("Add scenes in File > Build Settings (or set a start scene) before building");
            m_ShowBuildSettings = true;
            return false;
        }
        for (AssetHandle h : scenes)
            if (!AssetManager::GetMetadata(h))
            {
                Log::Error("Build Settings lists a missing scene; remove it first");
                m_ShowBuildSettings = true;
                return false;
            }
        if (folder.empty())
        {
            auto picked = FileDialogs::PickFolder(Project::GetDirectory());
            if (!picked) return false;
            folder = *picked;
        }
        if (!batch && config.BuildFolder != folder.string())
        {
            config.BuildFolder = folder.string();
            Project::Save();
        }

        std::error_code ec;
        // The shipping runtime has no console window; fall back to the dev player if it is missing.
        fs::path player = ExecutableDirectory() / "DimensionGame.exe";
        if (!fs::exists(player)) player = ExecutableDirectory() / "DimensionPlayer.exe";
        if (!fs::exists(player))
        {
            Log::Error("Player runtime not found next to the editor");
            return false;
        }
        std::string exeName = config.Name.empty() ? "Game" : config.Name;
        fs::create_directories(folder, ec);
        fs::path exe = folder / (exeName + ".exe");
        fs::copy_file(player, exe, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            Log::Error("Copying the player failed: {} (is the game still running?)", ec.message());
            return false;
        }
        if (config.Icon)
        {
            auto image = AppIcon::Load(AssetManager::GetAbsolutePath(config.Icon));
            if (!image || !AppIcon::WriteToExecutable(exe, *image)) Log::Warn("The game keeps the engine icon");
        }
        for (const char* dll : { "DimensionEngine.dll" })
            fs::copy_file(ExecutableDirectory() / dll, folder / dll, fs::copy_options::overwrite_existing, ec);
        fs::path module = ExecutableDirectory() / (config.GameModule + ".dll");
        if (fs::exists(module)) fs::copy_file(module, folder / module.filename(), fs::copy_options::overwrite_existing, ec);
        if (fs::exists(ExecutableDirectory() / "nethost.dll"))
        {
            fs::copy_file(ExecutableDirectory() / "nethost.dll", folder / "nethost.dll", fs::copy_options::overwrite_existing, ec);
            fs::copy(ExecutableDirectory() / "managed", folder / "managed", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
        fs::path projectOut = folder / "Project";
        fs::remove_all(projectOut, ec);
        fs::create_directories(projectOut, ec);
        fs::copy_file(Project::GetDirectory() / Project::kFileName, projectOut / Project::kFileName, ec);
        if (config.StripUnusedAssets)
        {
            std::vector<AssetHandle> roots = scenes;
            if (config.Icon) roots.push_back(config.Icon);
            AssetDependencySet deps = AssetDependencies::Collect(roots);
            fs::path src = Project::GetAssetsDirectory(), dst = projectOut / "Assets";
            uintmax_t shipped = 0, total = 0;
            auto copyOne = [&](const fs::path& rel) {
                std::error_code fe;
                fs::create_directories((dst / rel).parent_path(), fe);
                fs::copy_file(src / rel, dst / rel, fs::copy_options::overwrite_existing, fe);
                if (fe) { ec = fe; return; }
                shipped += fs::file_size(src / rel, fe);
            };
            for (AssetHandle h : deps.Assets)
                if (const AssetMetadata* m = AssetManager::GetMetadata(h))
                {
                    copyOne(m->Path);
                    fs::path meta = m->Path;
                    meta += ".meta";
                    if (fs::exists(src / meta)) copyOne(meta);
                }
            for (const fs::path& f : deps.ExtraFiles)
                if (fs::exists(src / f)) copyOne(f);
            for (auto& entry : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec))
                if (entry.is_regular_file()) total += entry.file_size();
            Log::Info("Stripped unused assets: shipping {} asset(s), {:.1f} of {:.1f} MB", deps.Assets.size(), shipped / 1048576.0, total / 1048576.0);
        }
        else
            fs::copy(Project::GetAssetsDirectory(), projectOut / "Assets", fs::copy_options::recursive, ec);
        // Work already done in the editor: convex decompositions (Mesh Colliders) and imported textures and models
        // (keyed by their path in Assets, so the game reads them instead of decoding the source files again).
        for (const char* cache : { "Convex", "Import" })
            if (fs::path from = Project::GetDirectory() / ".dimension" / "Cache" / cache; fs::exists(from))
            {
                std::error_code ce;
                fs::path to = projectOut / ".dimension" / "Cache" / cache;
                fs::create_directories(to, ce);
                fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ce);
                if (ce) Log::Warn("Could not copy the {} cache: {} (the game rebuilds it)", cache, ce.message());
            }
        fs::path scripts = CSharpProject::AssemblyPath(Project::GetDirectory());
        if (fs::exists(scripts))
        {
            fs::path rel = scripts.lexically_relative(Project::GetDirectory());
            fs::create_directories((projectOut / rel).parent_path(), ec);
            fs::copy_file(scripts, projectOut / rel, fs::copy_options::overwrite_existing, ec);
        }
        if (ec)
        {
            Log::Error("Copying assets failed: {}", ec.message());
            return false;
        }
        Log::Info("Built '{}' ({} scene(s)) to {}", exeName, scenes.size(), folder.string());
        if (run) FileDialogs::OpenWithDefaultApp(exe);
        else if (!batch) FileDialogs::OpenWithDefaultApp(folder);
        return true;
    }

    // ---- Game module (hot reload) ------------------------------------------------------------

    void EditorLayer::LoadGameModule()
    {
        fs::path dll = ExecutableDirectory() / (Project::GetConfig().GameModule + ".dll");
        if (!GameModule::Load(dll)) Log::Warn("Running without a game module: scripts will be unavailable");
    }

    void EditorLayer::ReloadGameModule()
    {
        if (m_Ctx.IsPlaying())
        {
            m_ReloadPending = true;
            return;
        }
        m_ReloadPending = false;
        // Objects created by module code (component pools, script instances) must not outlive it,
        // so the scene round-trips through JSON around the reload (like Unity's domain reload).
        auto snapshot = SceneSerializer::Serialize(*m_Ctx.EditorScene);
        std::vector<uint64_t> selection;
        for (Entity e : m_Ctx.Selection)
            if (m_Ctx.EditorScene->IsValid(e)) selection.push_back(m_Ctx.EditorScene->GetGuid(e));
        uint64_t primary = m_Ctx.HasValidSelection() ? (uint64_t)m_Ctx.EditorScene->GetGuid(m_Ctx.Selected) : 0;
        bool dirty = m_Ctx.Dirty;

        Application::Get().GetDevice().WaitIdle();
        m_Ctx.Select(Entity::Null());
        m_Ctx.EditorScene.reset();
        bool ok = GameModule::Reload();
        m_Ctx.EditorScene = SceneSerializer::Deserialize(snapshot);
        for (uint64_t g : selection) m_Ctx.AddToSelection(m_Ctx.EditorScene->FindEntityByGuid(Guid(g)));
        if (primary) m_Ctx.AddToSelection(m_Ctx.EditorScene->FindEntityByGuid(Guid(primary)));
        m_Ctx.Dirty = dirty;
        if (ok) Log::Info("Game module reloaded");
    }

    void EditorLayer::BuildGameModule()
    {
        if (m_BuildState && m_BuildState->load() == 1) return;
#if defined(DIM_SOURCE_DIR) && defined(DIM_BUILD_DIR) && defined(DIM_CMAKE_COMMAND)
        m_BuildState = std::make_shared<std::atomic<int>>(1);
        std::string target = Project::GetConfig().GameModule;
        // Outer quotes: cmd /c strips them, leaving the inner quoted paths intact.
        std::string command = std::string("\"\"") + DIM_SOURCE_DIR + "/scripts/dev.bat\" \"" + DIM_CMAKE_COMMAND + "\" --build \"" +
                              DIM_BUILD_DIR + "\" --target " + target + " 2>&1\"";
        Log::Info("Compiling game module '{}'...", target);
        std::thread([state = m_BuildState, command] {
            FILE* pipe = _popen(command.c_str(), "r");
            if (!pipe)
            {
                Log::Error("Could not start the build");
                state->store(3);
                return;
            }
            char line[1024];
            while (std::fgets(line, sizeof(line), pipe))
            {
                std::string text(line);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                if (text.empty()) continue;
                bool isError = text.find(": error") != std::string::npos || text.find("FAILED") != std::string::npos;
                if (isError) Log::Error("{}", text);
                else if (text.find(": warning") != std::string::npos) Log::Warn("{}", text);
                else Log::Trace("{}", text);
            }
            int code = _pclose(pipe);
            state->store(code == 0 ? 2 : 3);
        }).detach();
#else
        Log::Warn("This editor build does not know its CMake build folder; build the game module from your IDE.");
#endif
    }

    void EditorLayer::PollGameModule(float dt)
    {
        if (m_BuildState)
        {
            int state = m_BuildState->load();
            if (state == 2)
            {
                Log::Info("Game module compiled");
                m_BuildState.reset();
            }
            else if (state == 3)
            {
                Log::Error("Game module build failed (see errors above); keeping the previous version");
                m_BuildState.reset();
            }
        }
        if (m_ReloadPending && !m_Ctx.IsPlaying()) ReloadGameModule();

        // Builds started from the IDE are picked up too: watch the DLL on disk.
        m_ModuleCheckTimer += dt;
        if (m_ModuleCheckTimer < 0.5f) return;
        m_ModuleCheckTimer = 0.0f;
        if ((!m_BuildState || m_BuildState->load() != 1) && GameModule::HasChangedOnDisk())
        {
            if (m_Ctx.IsPlaying())
            {
                if (!m_ReloadPending) Log::Info("Game module changed; it will reload when you stop playing");
                m_ReloadPending = true;
            }
            else
                ReloadGameModule();
        }
    }

    // ---- C# scripts ------------------------------------------------------------------------------

    void EditorLayer::InitCSharp()
    {
        if (!CSharpRuntime::Init(ExecutableDirectory() / "managed")) return;
        InstallScriptHooks();
        if (m_Options.WaitForDebugger) CSharpRuntime::WaitForDebugger(60.0f);
        fs::path project = Project::GetDirectory();
        CSharpProject::Scaffold(project, ExecutableDirectory() / "managed");
        fs::path dll = CSharpProject::AssemblyPath(project);
        m_CSharpBuiltSources = CSharpProject::LatestSourceTime(project);
        std::error_code ec;
        bool upToDate = fs::exists(dll) && fs::last_write_time(dll, ec) >= m_CSharpBuiltSources;
        if (upToDate) CSharpRuntime::LoadScripts(dll);
        else if (CSharpProject::HasSources(project)) BuildCSharp();
    }

    void EditorLayer::InstallScriptHooks()
    {
        // What C# editor extensions reach (EditorApi.cs): the editor's controls, its selection and undo.
        // In the Inspector's property grid (custom Inspectors, property drawers) each control takes a row like the
        // built-in fields: its label on the left, the control filling the value column (SameLine keeps the next
        // control in the row). In windows they are plain controls with their label beside them.
        CSharpRuntime::EditorHooks hooks;
        static bool s_SameRow = false;
        auto row = [](const char* label, bool labelled = true) -> std::string {
            if (!ImGui::GetCurrentTable()) return label;
            if (s_SameRow) ImGui::SameLine();
            else UI::PropertyLabel(labelled ? label : "");
            s_SameRow = false;
            return labelled ? std::string("##") + label : std::string(label);
        };
        hooks.Gui = [row](int op, const char* label, float* v, int n) -> int {
            bool grid = ImGui::GetCurrentTable() != nullptr;
            switch (op)
            {
            case 0: row("", false); ImGui::TextUnformatted(label); return 0;
            case 1: return ImGui::Button(row(label, false).c_str()) ? 1 : 0;
            case 2:
            {
                bool b = n > 0 && v[0] != 0.0f;
                bool changed = ImGui::Checkbox(row(label).c_str(), &b);
                if (n > 0) v[0] = b ? 1.0f : 0.0f;
                return changed ? 1 : 0;
            }
            case 3: return n > 0 && ImGui::DragFloat(row(label).c_str(), v, 0.05f) ? 1 : 0;
            case 4:
            {
                int i = n > 0 ? (int)v[0] : 0;
                bool changed = ImGui::DragInt(row(label).c_str(), &i);
                if (n > 0) v[0] = (float)i;
                return changed ? 1 : 0;
            }
            case 5: return n >= 3 && ImGui::SliderFloat(row(label).c_str(), v, v[1], v[2]) ? 1 : 0;
            case 6: return n >= 3 && ImGui::DragFloat3(row(label).c_str(), v, 0.05f) ? 1 : 0;
            case 7: return n >= 4 && ImGui::ColorEdit4(row(label).c_str(), v) ? 1 : 0;
            case 8:
                if (grid)
                {
                    // A heading row, as a field's [Header].
                    s_SameRow = false;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Dummy(ImVec2(0.0f, 2.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, UI::AccentText());
                    ImGui::TextUnformatted(label);
                    ImGui::PopStyleColor();
                }
                else if (*label) ImGui::SeparatorText(label);
                else ImGui::Separator();
                return 0;
            case 9:
                if (grid) s_SameRow = true;
                else ImGui::SameLine();
                return 0;
            case 11:
            {
                // A dropdown: the label then its options, separated by \x1f; the chosen index in and out.
                std::vector<std::string> parts;
                for (const char* s = label;;)
                {
                    const char* end = std::strchr(s, '\x1f');
                    parts.emplace_back(s, end ? end : s + std::strlen(s));
                    if (!end) break;
                    s = end + 1;
                }
                int current = n > 0 ? (int)v[0] : -1;
                bool changed = false;
                const char* preview = current >= 0 && current + 1 < (int)parts.size() ? parts[current + 1].c_str() : "";
                if (ImGui::BeginCombo(row(parts[0].c_str()).c_str(), preview))
                {
                    for (int i = 0; i + 1 < (int)parts.size(); ++i)
                        if (ImGui::Selectable(parts[i + 1].c_str(), i == current) && i != current)
                        {
                            current = i;
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
                if (n > 0) v[0] = (float)current;
                return changed ? 1 : 0;
            }
            case 12:
            {
                // Flags: the label then the names (separated by \x1f); the mask in v[0], each name's value after it.
                std::vector<std::string> parts;
                for (const char* s = label;;)
                {
                    const char* end = std::strchr(s, '\x1f');
                    parts.emplace_back(s, end ? end : s + std::strlen(s));
                    if (!end) break;
                    s = end + 1;
                }
                int mask = n > 0 ? (int)v[0] : 0;
                int count = std::min((int)parts.size() - 1, n - 1);
                std::string shown;
                for (int i = 0; i < count; ++i)
                {
                    int bits = (int)v[i + 1];
                    if (bits != 0 && (mask & bits) == bits) shown += (shown.empty() ? "" : ", ") + parts[i + 1];
                }
                if (shown.empty()) shown = mask == 0 ? "Nothing" : std::to_string(mask);
                bool changed = false;
                if (ImGui::BeginCombo(row(parts[0].c_str()).c_str(), shown.c_str()))
                {
                    for (int i = 0; i < count; ++i)
                    {
                        int bits = (int)v[i + 1];
                        bool on = bits == 0 ? mask == 0 : (mask & bits) == bits;
                        if (ImGui::Selectable(parts[i + 1].c_str(), on, ImGuiSelectableFlags_DontClosePopups))
                        {
                            mask = bits == 0 ? 0 : on ? (mask & ~bits) : (mask | bits);
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (n > 0) v[0] = (float)mask;
                return changed ? 1 : 0;
            }
            case 10:
                if (grid)
                {
                    s_SameRow = false;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                }
                ImGui::Spacing();
                return 0;
            }
            return 0;
        };
        hooks.Text = [row](const char* label, const std::string& text) {
            std::string edited = text;
            ImGui::InputText(row(label).c_str(), &edited);
            return edited;
        };
        // Pickers: the Inspector's own fields (a row of the grid, or a one-row grid of their own in a window).
        auto picker = [](const char* label, const std::function<void()>& draw) {
            s_SameRow = false;
            if (ImGui::GetCurrentTable()) draw();
            else if (UI::BeginProperties(label))
            {
                draw();
                UI::EndProperties();
            }
        };
        hooks.EntityField = [this, picker](const char* label, Entity e) {
            picker(label, [&] { UI::EntityField(label, e, m_Ctx.ActiveScene()); });
            return e;
        };
        hooks.AssetField = [picker](int kind, const char* label, uint64_t guid) {
            const AssetType types[] = { AssetType::None, AssetType::Prefab, AssetType::Texture, AssetType::Material, AssetType::Mesh, AssetType::Audio };
            AssetHandle handle(guid);
            if (kind > 0 && kind < 6) picker(label, [&] { UI::AssetField(label, handle, types[kind]); });
            return (uint64_t)handle;
        };
        hooks.Selection = [this](int op, Entity e, const char* text) -> Entity {
            Scene& scene = m_Ctx.ActiveScene();
            switch (op)
            {
            case 0: return m_Ctx.HasValidSelection() ? m_Ctx.Selected : Entity::Null();
            case 1: m_Ctx.Select(scene.IsValid(e) ? e : Entity::Null()); return e;
            case 2:
            {
                Entity created = scene.CreateEntity(*text ? text : "GameObject");
                m_Ctx.MarkDirty();
                return created;
            }
            case 3: return Entity{ (uint32_t)m_Ctx.Selection.size(), 0 };
            case 4: return e.Index < m_Ctx.Selection.size() ? m_Ctx.Selection[e.Index] : Entity::Null();
            }
            return Entity::Null();
        };
        hooks.Undo = [this](const char* name) {
            m_Ctx.RecordUndo(name); // C#'s Undo.Record("Name") names the step
            m_Ctx.MarkDirty();
        };
        hooks.Handle = [this](int kind, float* v, int n, const char* text) { return DoHandle(kind, v, n, text); };
        CSharpRuntime::SetEditorHooks(hooks);
    }

    bool EditorLayer::UpdateScriptHandles(const ImVec2& origin, const ImVec2& size)
    {
        // The selected object's C# scripts whose custom Inspectors have OnSceneGUI: their handles, edits saved like the
        // Inspector's (one undo step per drag).
        if (!m_Ctx.HasValidSelection() || m_Ctx.Selection.size() > 1 || m_Camera.IsControlling()) return false;
        Scene& scene = m_Ctx.ActiveScene();
        Entity e = m_Ctx.Selected;
        auto& reg = scene.GetRegistry();
        if (!reg.Has<ScriptComponent>(e)) return false;
        EntityRefResolver resolver;
        resolver.ToGuid = [&scene](Entity x) { return scene.IsValid(x) ? (uint64_t)scene.GetGuid(x) : uint64_t(0); };
        resolver.FromGuid = [&scene](uint64_t g) { return scene.FindEntityByGuid(Guid(g)); };
        glm::mat4 view = m_Camera.GetView(), proj = m_Camera.GetProjection(size.x / std::max(1.0f, size.y));
        m_HandleView = { true, origin, size, view, proj, 0, false };
        ImGuizmo::SetOrthographic(m_Camera.Is2D());
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
        bool changedAny = false;
        for (size_t i = 0; i < reg.Get<ScriptComponent>(e).Scripts.size(); ++i)
        {
            ScriptInstance& script = reg.Get<ScriptComponent>(e).Scripts[i];
            const ScriptInfo* info = ScriptRegistry::Find(script.ClassName);
            const CSharpRuntime::EditorItem* custom = info ? CSharpRuntime::FindInspector(script.ClassName) : nullptr;
            if (!custom || !custom->SceneGUI || !script.Enabled) continue;
            std::shared_ptr<ScriptableEntity> temp;
            ScriptableEntity* target = script.Instance.get();
            if (!target)
            {
                temp = info->Create();
                target = temp.get();
                for (auto& f : info->Fields)
                    if (script.Fields.is_object() && script.Fields.contains(f.Name)) FieldFromJson(f, info->ToObject(target), script.Fields[f.Name], &resolver);
            }
            m_HandleView.Counter = (int)i << 16;
            if (!CSharpRuntime::SceneGUIScript(custom->Id, *target, scene, e)) continue;
            auto& fields = reg.Get<ScriptComponent>(e).Scripts[i].Fields;
            nlohmann::json before = fields.is_object() ? fields : nlohmann::json::object(), after = before;
            for (auto& f : info->Fields) after[f.Name] = FieldToJson(f, info->ToObject(target), &resolver);
            if (!m_HandleEditActive)
            {
                fields = before;
                m_Ctx.RecordUndo();
            }
            fields = after;
            Prefabs::RecordFieldOverride(scene, e, "Script", "Scripts");
            m_Ctx.MarkDirty();
            changedAny = true;
        }
        m_HandleEditActive = changedAny ? (m_HandleDrag != 0 || ImGuizmo::IsUsing() || ImGui::IsMouseDown(ImGuiMouseButton_Left))
                                        : m_HandleEditActive && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool hot = m_HandleView.Hot || m_HandleDrag != 0;
        m_HandleView.Active = false;
        return hot;
    }

    int EditorLayer::DoHandle(int kind, float* v, int n, const char* text)
    {
        // Handles of Inspector.OnSceneGUI (see UpdateScriptHandles); outside it (tests, other windows) nothing is drawn.
        HandleView& hv = m_HandleView;
        if (!hv.Active) return 0;
        int id = ++hv.Counter;
        glm::mat4 viewProj = hv.Proj * hv.View;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 mouse = ImGui::GetMousePos();
        auto project = [&](const glm::vec3& p, ImVec2& out) {
            glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
            if (clip.w <= 1e-5f) return false;
            out = ImVec2(hv.Origin.x + (clip.x / clip.w * 0.5f + 0.5f) * hv.Size.x, hv.Origin.y + (0.5f - clip.y / clip.w * 0.5f) * hv.Size.y);
            return true;
        };
        auto colorOf = [](const float* c, float boost) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(c[0] * boost, 1.0f), std::min(c[1] * boost, 1.0f), std::min(c[2] * boost, 1.0f), c[3]));
        };
        // A dot dragged on a plane (through `point`, facing `normal`): true while hovered or dragged; `point` follows.
        auto dragDot = [&](int dotId, glm::vec3& point, const glm::vec3& normal, float radius, const float* color) {
            ImVec2 s;
            if (!project(point, s)) return false;
            float dx = mouse.x - s.x, dy = mouse.y - s.y;
            bool hovered = m_SceneViewHovered && dx * dx + dy * dy <= (radius + 3.0f) * (radius + 3.0f);
            if (hovered && m_HandleDrag == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_HandleDrag = dotId;
            bool dragging = m_HandleDrag == dotId;
            if (dragging)
            {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_HandleDrag = 0;
                else
                {
                    glm::vec3 from, dir;
                    MouseRay(viewProj, hv.Origin, hv.Size, from, dir);
                    float denom = glm::dot(dir, normal);
                    if (std::abs(denom) > 1e-5f) point = from + dir * (glm::dot(point - from, normal) / denom);
                }
            }
            dl->AddCircleFilled(s, radius, colorOf(color, hovered || dragging ? 1.35f : 1.0f));
            dl->AddCircle(s, radius, IM_COL32(0, 0, 0, 160), 0, 1.5f);
            return hovered || dragging;
        };
        bool hot = false;
        switch (kind)
        {
        case 0: // the Move tool's arrows
        {
            if (n < 3) return 0;
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(v[0], v[1], v[2]));
            ImGuizmo::PushID(id);
            if (ImGuizmo::Manipulate(glm::value_ptr(hv.View), glm::value_ptr(hv.Proj), ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(m)))
                v[0] = m[3].x, v[1] = m[3].y, v[2] = m[3].z;
            hot = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
            ImGuizmo::PopID();
            break;
        }
        case 1: // a dot across the view
        {
            if (n < 8) return 0;
            glm::vec3 p(v[0], v[1], v[2]);
            glm::vec3 forward = glm::normalize(glm::vec3(glm::inverse(hv.View)[2]));
            hot = dragDot(id, p, forward, std::max(v[3] * 0.5f, 3.0f), v + 4);
            v[0] = p.x, v[1] = p.y, v[2] = p.z;
            break;
        }
        case 2: // a horizontal ring with four dots for its radius
        {
            if (n < 8) return 0;
            glm::vec3 center(v[0], v[1], v[2]);
            float radius = v[3];
            ImVec2 a, b;
            constexpr int kSegments = 64;
            for (int s = 0; s < kSegments; ++s)
            {
                float t0 = glm::two_pi<float>() * s / kSegments, t1 = glm::two_pi<float>() * (s + 1) / kSegments;
                if (project(center + glm::vec3(std::cos(t0), 0.0f, std::sin(t0)) * radius, a) &&
                    project(center + glm::vec3(std::cos(t1), 0.0f, std::sin(t1)) * radius, b))
                    dl->AddLine(a, b, colorOf(v + 4, 1.0f), 2.0f);
            }
            const glm::vec3 directions[] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
            for (int k = 0; k < 4; ++k)
            {
                glm::vec3 dot = center + directions[k] * radius;
                if (dragDot(id * 4 + k + (1 << 30), dot, glm::vec3(0, 1, 0), 5.0f, v + 4))
                {
                    hot = true;
                    if (m_HandleDrag == id * 4 + k + (1 << 30)) radius = glm::length(glm::vec2(dot.x - center.x, dot.z - center.z));
                }
            }
            v[3] = radius;
            break;
        }
        case 3: // a label
        {
            ImVec2 s;
            if (n >= 3 && project({ v[0], v[1], v[2] }, s))
            {
                dl->AddText(ImVec2(s.x + 7.0f, s.y - 8.0f), IM_COL32(0, 0, 0, 200), text);
                dl->AddText(ImVec2(s.x + 6.0f, s.y - 9.0f), IM_COL32(255, 255, 255, 255), text);
            }
            break;
        }
        case 4: // a line
        {
            ImVec2 a, b;
            if (n >= 10 && project({ v[0], v[1], v[2] }, a) && project({ v[3], v[4], v[5] }, b)) dl->AddLine(a, b, colorOf(v + 6, 1.0f), 2.0f);
            break;
        }
        }
        hv.Hot |= hot;
        return hot ? 1 : 0;
    }

    void EditorLayer::DrawScriptMenus()
    {
        // Each extension under its menu path: the first part is the menu (an existing one gains the item), the parts
        // between are submenus. Windows show a check while open.
        for (const auto& item : CSharpRuntime::GetEditorItems())
        {
            if (!item.Inspects.empty()) continue;
            std::vector<std::string> parts;
            for (size_t start = 0; start <= item.MenuPath.size();)
            {
                size_t slash = item.MenuPath.find('/', start);
                if (slash == std::string::npos) slash = item.MenuPath.size();
                if (slash > start) parts.push_back(item.MenuPath.substr(start, slash - start));
                start = slash + 1;
            }
            if (parts.size() < 2) parts.insert(parts.begin(), "Tools");
            size_t opened = 0;
            while (opened + 1 < parts.size() && ImGui::BeginMenu(parts[opened].c_str())) ++opened;
            if (opened + 1 == parts.size())
            {
                bool* open = item.Window ? &m_ScriptWindows[item.Title] : nullptr;
                if (ImGui::MenuItem(parts.back().c_str(), nullptr, open && *open))
                {
                    if (open) *open = !*open;
                    else CSharpRuntime::CallEditorItem(item.Id, false, m_Ctx.ActiveScene());
                }
            }
            for (size_t i = 0; i < opened; ++i) ImGui::EndMenu();
        }
    }

    void EditorLayer::DrawScriptWindows()
    {
        // A command from the command line runs once the scripts are in.
        if (!m_Options.MenuCommand.empty())
            for (const auto& item : CSharpRuntime::GetEditorItems())
                if (!item.Window && item.MenuPath == m_Options.MenuCommand)
                {
                    CSharpRuntime::CallEditorItem(item.Id, false, m_Ctx.ActiveScene());
                    m_Options.MenuCommand.clear();
                    break;
                }
        for (const auto& item : CSharpRuntime::GetEditorItems())
        {
            if (!item.Window) continue;
            auto it = m_ScriptWindows.find(item.Title);
            if (it == m_ScriptWindows.end() || !it->second) continue;
            ImGui::SetNextWindowSize(ImVec2(360.0f, 300.0f), ImGuiCond_FirstUseEver);
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                    ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
            if (ImGui::Begin(item.Title.c_str(), &it->second)) CSharpRuntime::CallEditorItem(item.Id, true, m_Ctx.ActiveScene());
            ImGui::End();
        }
    }

    void EditorLayer::BuildCSharp()
    {
        if (!CSharpRuntime::IsAvailable() || (m_CSharpBuild && m_CSharpBuild->load() == 1)) return;
        fs::path project = Project::GetDirectory();
        m_CSharpBuiltSources = CSharpProject::LatestSourceTime(project);
        m_CSharpBuild = std::make_shared<std::atomic<int>>(1);
        std::string command = CSharpProject::BuildCommand(project);
        Log::Info("Compiling C# scripts...");
        std::thread([state = m_CSharpBuild, command] {
            FILE* pipe = _popen(command.c_str(), "r");
            if (!pipe)
            {
                state->store(3);
                return;
            }
            char line[2048];
            std::vector<std::string> seen; // MSBuild repeats diagnostics at the end of the build
            while (std::fgets(line, sizeof(line), pipe))
            {
                std::string text(line);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                if (text.empty() || std::find(seen.begin(), seen.end(), text) != seen.end()) continue;
                seen.push_back(text);
                if (text.find(": error") != std::string::npos) Log::Error("{}", text);
                else if (text.find(": warning") != std::string::npos) Log::Warn("{}", text);
                else Log::Trace("{}", text);
            }
            state->store(_pclose(pipe) == 0 ? 2 : 3);
        }).detach();
    }

    void EditorLayer::PollCSharp(float dt)
    {
        if (!CSharpRuntime::IsAvailable()) return;
        fs::path project = Project::GetDirectory();
        if (m_CSharpBuild)
        {
            int state = m_CSharpBuild->load();
            if (state == 3)
            {
                Log::Error("C# build failed (see errors above); keeping the previous scripts");
                m_CSharpBuild.reset();
            }
            else if (state == 2)
            {
                m_CSharpBuild.reset();
                m_CSharpReloadPending = true; // new assembly is on disk
            }
        }
        if (m_CSharpReloadPending)
        {
            m_CSharpReloadPending = false;
            fs::path dll = CSharpProject::AssemblyPath(project);
            // Edit-time scenes never hold C# instances, so the assembly can be swapped directly. While playing, the
            // live scripts keep their serialized fields across the swap (and get OnReloaded instead of OnCreate).
            if (m_Ctx.IsPlaying() && m_Ctx.RuntimeScene)
                m_Ctx.RuntimeScene->ReloadScriptModule("csharp", [&] { return CSharpRuntime::LoadScripts(dll); });
            else
                CSharpRuntime::LoadScripts(dll);
        }

        // Rebuild automatically when sources change (debounced).
        m_CSharpCheckTimer += dt;
        if (m_CSharpCheckTimer < 1.0f) return;
        m_CSharpCheckTimer = 0.0f;
        if (m_CSharpBuild) return;
        auto latest = CSharpProject::LatestSourceTime(project);
        if (latest > m_CSharpBuiltSources && fs::file_time_type::clock::now() - latest > std::chrono::milliseconds(500)) BuildCSharp();
        else if (CSharpRuntime::ScriptsChangedOnDisk()) m_CSharpReloadPending = true; // built from the IDE
    }
}
