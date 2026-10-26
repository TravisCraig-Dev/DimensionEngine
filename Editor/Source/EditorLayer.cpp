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
#include <Dimension/Scene/SceneManager.h>
#include <Dimension/Scripting/CSharpRuntime.h>
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
#ifdef _WIN32
#include <windows.h>
#endif

namespace Editor
{
    namespace
    {
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
        m_Content = MakeScope<ContentBrowserPanel>(m_Ctx);
        m_Console = MakeScope<ConsolePanel>();
        m_SceneSettings = MakeScope<SceneSettingsPanel>(m_Ctx);
        m_Animator = MakeScope<AnimatorPanel>(m_Ctx);
        m_Timeline = MakeScope<TimelinePanel>(m_Ctx);

        // Per-project editor layout (like Unity's Library/ folder, not meant for version control).
        fs::path settingsDir = Project::GetDirectory() / ".dimension";
        fs::create_directories(settingsDir);
        m_IniPath = (settingsDir / "EditorLayout.ini").string();
        ImGui::GetIO().IniFilename = m_IniPath.c_str();
        m_ResetLayout = !fs::exists(m_IniPath);

        EditorPreferences::Get().Load();
        if (m_Options.Theme >= 0) EditorPreferences::Get().Theme = m_Options.Theme; // not saved unless Preferences change
        ApplyPreferences();
        m_Camera.SetFlySpeed(EditorPreferences::Get().CameraFlySpeed);
        m_ShowGrid = EditorPreferences::Get().ShowGrid;
        if (m_Options.OpenWindow == "Build Settings") m_ShowBuildSettings = true;
        else if (m_Options.OpenWindow == "Preferences") m_ShowPreferences = true;
        else if (m_Options.OpenWindow == "Project Settings") m_ShowProjectSettings = true;
        else if (m_Options.OpenWindow == "Audio Mixer") m_ShowAudioMixer = true;
        else if (m_Options.OpenWindow == "Timeline") m_ShowTimeline = true;
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

    void EditorLayer::NewScene()
    {
        if (m_Ctx.IsPlaying()) Stop();
        auto scene = MakeRef<Scene>("Untitled");
        auto& reg = scene->GetRegistry();
        Entity cam = scene->CreateEntity("Main Camera");
        reg.Emplace<CameraComponent>(cam).Clear = ClearMode::Skybox;
        reg.Get<TransformComponent>(cam).Position = { 0.0f, 2.0f, 8.0f };
        reg.Get<TransformComponent>(cam).Rotation = glm::radians(glm::vec3(-12.0f, 0.0f, 0.0f));
        reg.Get<TagComponent>(cam).Tag = "MainCamera";
        Entity light = scene->CreateEntity("Directional Light");
        reg.Emplace<DirectionalLightComponent>(light);
        reg.Get<TransformComponent>(light).Position = { 0.0f, 3.0f, 0.0f };
        reg.Get<TransformComponent>(light).Rotation = glm::radians(glm::vec3(-50.0f, -30.0f, 0.0f));
        scene->UpdateTransforms();

        m_Ctx.EditorScene = scene;
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
        m_Ctx.ScenePath = path;
        m_Ctx.Dirty = false;
        m_Ctx.Select(Entity::Null());
        m_Ctx.ClearHistory();
        UpdateWindowTitle();
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
        if (!SceneSerializer::SaveToFile(*m_Ctx.EditorScene, m_Ctx.ScenePath)) return false;
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
        return SaveScene();
    }

    bool EditorLayer::ConfirmDiscardChanges()
    {
        if (m_Ctx.InPrefabMode() && !LeavePrefabMode()) return false;
        if (!m_Ctx.Dirty) return true;
        int choice = FileDialogs::YesNoCancel("Unsaved Changes", "The scene '" + m_Ctx.EditorScene->GetName() + "' has unsaved changes. Save them?");
        if (choice == 0) return SaveScene();
        return choice == 1;
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
        m_Ctx.RuntimeScene = m_Ctx.EditorScene->Clone();
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
            m_Ctx.RequestOpenPrefab.reset();
            OpenPrefabMode(h);
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

        PollGameModule(dt);
        PollCSharp(dt);
        Autosave(dt);
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
            m_Ctx.EditorScene->OnUpdateEditor(dt);

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

    void EditorLayer::OpenPrefabMode(AssetHandle prefab)
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
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
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
        m_Camera.Focus(center, radius);
    }

    void EditorLayer::OnRender(VkCommandBuffer cmd)
    {
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

            SceneRenderOptions options;
            options.EditorOverlays = true;
            options.Grid = m_ShowGrid;
            options.Grid2D = m_Camera.Is2D();
            options.Selected = m_Ctx.Selected;
            options.Selection = m_Ctx.Selection;
            options.DefaultLight = m_Ctx.InPrefabMode();
            m_SceneRenderer.Render(cmd, scene, cam, *m_SceneTarget, options);
        }
        if (m_GameViewVisible)
        {
            Entity camera = scene.GetPrimaryCamera();
            CameraData cam;
            cam.ClearColor = { 0, 0, 0, 1 };
            if (!camera.IsNull()) cam = CameraData::FromEntity(scene, camera, m_GameTarget->GetAspect());
            SceneRenderOptions gameOptions;
            gameOptions.RenderUI = true;
            m_GameRenderer.Render(cmd, scene, cam, *m_GameTarget, gameOptions);
        }
    }

    // Line Renderer points: with a line selected, its points show as handles in the Scene view. Clicking one picks
    // it and a move gizmo edits that point (undoable); clicking elsewhere returns to the object's own gizmo.
    // Shift-click on a segment inserts a point there; Delete removes the picked point (a line keeps at least two).
    // Returns true while the mouse is over a handle, a segment being inserted on, or the point gizmo (so the click
    // does not re-select).
    bool EditorLayer::UpdateLinePointEditing(ImVec2 origin, ImVec2 size)
    {
        Scene& scene = m_Ctx.ActiveScene();
        auto* line = m_Ctx.HasValidSelection() ? scene.GetRegistry().TryGet<LineRendererComponent>(m_Ctx.Selected) : nullptr;
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
        if (hovered >= 0) ImGui::SetTooltip("Point %d (click to move it%s)", hovered, line->Positions.size() > 2 ? ", Delete removes it" : "");

        // Shift: a "+" marker follows the mouse along the nearest segment; clicking inserts a point there.
        int insertAfter = -1;
        glm::vec3 insertWorld(0.0f);
        if (hovered < 0 && m_SceneViewHovered && ImGui::GetIO().KeyShift && !ImGuizmo::IsUsing() && line->Positions.size() >= 2)
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
                line->Positions[(size_t)m_LinePoint] = line->UseWorldSpace ? world : glm::vec3(glm::inverse(t.World) * glm::vec4(world, 1.0f));
                Prefabs::RecordFieldOverride(scene, m_Ctx.Selected, "Line Renderer", "Positions");
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
                line->Positions.insert(line->Positions.begin() + insertAfter + 1, p);
                Prefabs::RecordFieldOverride(scene, m_Ctx.Selected, "Line Renderer", "Positions");
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
        auto* line = m_LinePoint >= 0 && m_Ctx.HasValidSelection() ? scene.GetRegistry().TryGet<LineRendererComponent>(m_Ctx.Selected) : nullptr;
        if (!line || m_LinePoint >= (int)line->Positions.size()) return false;
        if (line->Positions.size() > 2)
        {
            m_Ctx.RecordUndo();
            line->Positions.erase(line->Positions.begin() + m_LinePoint);
            Prefabs::RecordFieldOverride(scene, m_Ctx.Selected, "Line Renderer", "Positions");
            m_Ctx.MarkDirty();
        }
        m_LinePoint = std::min(m_LinePoint, (int)line->Positions.size() - 1);
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
        if (m_ShowContent) m_Content->OnImGui(&m_ShowContent);
        if (m_ShowConsole) m_Console->OnImGui(&m_ShowConsole);
        if (m_ShowSceneSettings) m_SceneSettings->OnImGui(&m_ShowSceneSettings);
        if (m_ShowAnimator) m_Animator->OnImGui(&m_ShowAnimator);
        if (m_ShowTimeline) m_Timeline->OnImGui(&m_ShowTimeline);
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
        ImGui::DockBuilderDockWindow("Project", bottom);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Animator", bottom);
        ImGui::DockBuilderDockWindow("Audio Mixer", bottom);
        ImGui::DockBuilderDockWindow("Timeline", bottom);
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
            if (ImGui::MenuItem("New Scene", "Ctrl+N") && ConfirmDiscardChanges()) NewScene();
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) OpenSceneWithDialog();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, !m_Ctx.IsPlaying())) SaveScene();
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, !m_Ctx.IsPlaying())) SaveSceneAs();
            ImGui::Separator();
            if (ImGui::MenuItem("Open Project..."))
            {
                if (auto dir = FileDialogs::PickFolder(Project::GetDirectory()); dir && ConfirmDiscardChanges())
                {
                    if (m_Ctx.IsPlaying()) Stop();
                    ImGui::SaveIniSettingsToDisk(m_IniPath.c_str());
                    m_Ctx.EditorScene.reset();
                    if (Project::Open(*dir))
                    {
                        fs::path settingsDir = Project::GetDirectory() / ".dimension";
                        fs::create_directories(settingsDir);
                        m_IniPath = (settingsDir / "EditorLayout.ini").string();
                        ImGui::GetIO().IniFilename = m_IniPath.c_str();
                        if (fs::exists(m_IniPath)) ImGui::LoadIniSettingsFromDisk(m_IniPath.c_str());
                        m_Content = MakeScope<ContentBrowserPanel>(m_Ctx);
                        fs::path start = AssetManager::GetAbsolutePath(Project::GetConfig().StartScene);
                        if (!start.empty() && fs::exists(start)) OpenScene(start);
                        else NewScene();
                    }
                }
            }
            if (ImGui::MenuItem("Project Settings...")) m_ShowProjectSettings = true;
            if (ImGui::MenuItem("Build Settings...", "Ctrl+Shift+B")) m_ShowBuildSettings = true;
            if (ImGui::MenuItem("Build And Run", "Ctrl+B", false, !m_Ctx.IsPlaying())) BuildGame(true, Project::GetConfig().BuildFolder);
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) Application::Get().GetWindow().SetShouldClose(OnCloseRequested());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_Ctx.CanUndo() && !m_Ctx.IsPlaying())) m_Ctx.Undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_Ctx.CanRedo() && !m_Ctx.IsPlaying())) m_Ctx.Redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, m_Ctx.HasValidSelection())) CopySelected(m_Ctx);
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_Ctx.IsPlaying())) PasteClipboard(m_Ctx);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_Ctx.HasValidSelection())) DuplicateSelected(m_Ctx);
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences...")) m_ShowPreferences = true;
            if (ImGui::MenuItem("Delete", "Del", false, m_Ctx.HasValidSelection())) DeleteSelected(m_Ctx);
            if (ImGui::MenuItem("Frame Selected", "F", false, m_Ctx.HasValidSelection())) FocusSelected();
            ImGui::Separator();
            if (ImGui::MenuItem(m_Ctx.IsPlaying() ? "Stop" : "Play", "Ctrl+P")) m_Ctx.IsPlaying() ? Stop() : Play();
            if (ImGui::MenuItem("Pause", "Ctrl+Shift+P", m_Ctx.State == PlayState::Paused, m_Ctx.IsPlaying())) TogglePause();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scripts"))
        {
            bool building = m_BuildState && m_BuildState->load() == 1;
            if (ImGui::MenuItem("Build && Reload Game Module", "Ctrl+R", false, !building)) BuildGameModule();
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
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window"))
        {
            ImGui::MenuItem("Hierarchy", nullptr, &m_ShowHierarchy);
            ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
            ImGui::MenuItem("Project", nullptr, &m_ShowContent);
            ImGui::MenuItem("Console", nullptr, &m_ShowConsole);
            ImGui::MenuItem("Scene Settings", nullptr, &m_ShowSceneSettings);
            ImGui::MenuItem("Animator", nullptr, &m_ShowAnimator);
            ImGui::MenuItem("Timeline", nullptr, &m_ShowTimeline);
            ImGui::MenuItem("Audio Mixer", nullptr, &m_ShowAudioMixer);
            ImGui::MenuItem("Tile Palette", nullptr, &m_ShowTilePalette);
            ImGui::MenuItem("Stats", nullptr, &m_ShowStats);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) m_ResetLayout = true;
            ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
            ImGui::EndMenu();
        }
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

            // Play controls (center)
            float w = ImGui::GetFrameHeight() * 1.25f * 3 + ImGui::GetStyle().ItemSpacing.x * 2;
            ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f - w * 0.5f);
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

        bool tilePainting = UpdateTilePainting(origin, avail) || UpdateTerrainBrush(origin, avail);
        bool lineEditing = UpdateLinePointEditing(origin, avail);

        // Transform gizmo
        bool gizmoHovered = lineEditing;
        if (m_Ctx.HasValidSelection() && m_GizmoOperation != 0 && !m_Camera.IsControlling() && !tilePainting && m_LinePoint < 0)
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

            bool snap = ImGui::GetIO().KeyCtrl;
            float snapValue = m_GizmoOperation == ImGuizmo::ROTATE ? 15.0f : 0.5f;
            float snaps[3] = { snapValue, snapValue, snapValue };

            gizmoHovered = ImGuizmo::IsOver();
            if (gizmoHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_Ctx.RecordUndo();
            if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), (ImGuizmo::OPERATION)m_GizmoOperation,
                                     m_GizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(world), nullptr, snap ? snaps : nullptr))
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
        ImGui::Text("Meshes: %u   Sprites: %u   Culled: %u", st.Meshes, st.Sprites, st.Culled);
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

        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) shift ? TogglePause() : (m_Ctx.IsPlaying() ? Stop() : Play());
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) shift ? SaveSceneAs() : SaveScene();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false) && ConfirmDiscardChanges()) NewScene();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) BuildGameModule();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_B, false))
        {
            if (shift) m_ShowBuildSettings = true;
            else if (!m_Ctx.IsPlaying()) BuildGame(true, Project::GetConfig().BuildFolder);
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) OpenSceneWithDialog();
        if (m_GameViewFocused && m_Ctx.IsPlaying()) return; // the game owns the keyboard

        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) m_Ctx.Undo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) m_Ctx.Redo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateSelected(m_Ctx);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySelected(m_Ctx);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) PasteClipboard(m_Ctx);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !((m_SceneViewHovered || m_SceneViewFocused) && DeleteLinePoint())) DeleteSelected(m_Ctx);

        bool viewActive = m_SceneViewHovered || m_SceneViewFocused;
        if (viewActive && !ctrl && !m_Camera.IsControlling())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) m_GizmoOperation = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_GizmoOperation = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_GizmoOperation = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_GizmoOperation = ImGuizmo::SCALE;
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FocusSelected();
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
            ImVec2 points[49];
            for (int i = 0; i <= 48; ++i)
            {
                float angle = i / 48.0f * 6.2831853f;
                glm::vec2 p(local.x + std::cos(angle) * radius, local.z + std::sin(angle) * radius);
                glm::vec4 c = viewProj * (t.World * glm::vec4(p.x, Terrain::HeightAt(*terrain, heights, p.x, p.y) + 0.05f, p.y, 1.0f));
                glm::vec2 n = glm::vec2(c) / std::max(c.w, 1e-4f);
                points[i] = ImVec2(origin.x + (n.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - n.y * 0.5f) * size.y);
            }
            bool paint = m_Ctx.TerrainTool - 1 >= (int)Terrain::BrushMode::PaintGrass;
            bool detail = Terrain::IsDetailMode((Terrain::BrushMode)(m_Ctx.TerrainTool - 1));
            ImGui::GetWindowDrawList()->AddPolyline(points, 49, detail ? IM_COL32(240, 200, 90, 255) : paint ? IM_COL32(120, 230, 120, 255) : IM_COL32(90, 200, 255, 255), 0, 2.0f);
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
                Terrain::ApplyBrush(*terrain, mode, glm::vec2(local.x, local.z), radius, m_Ctx.TerrainBrushStrength,
                                    std::min(ImGui::GetIO().DeltaTime, 0.05f), m_TerrainFlatten);
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
