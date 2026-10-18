#include "EditorLayer.h"
#include "EditorActions.h"
#include "EditorUI.h"
#include <Dimension/Core/Input.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Platform/FileDialogs.h>
#include <Dimension/Renderer/Mesh.h>
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Scripting/GameModule.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scripting/CSharpRuntime.h>
#include <Dimension/Scripting/CSharpProject.h>
#include <Dimension/Scripting/ScriptRegistry.h>
#include <thread>
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

        // Per-project editor layout (like Unity's Library/ folder, not meant for version control).
        fs::path settingsDir = Project::GetDirectory() / ".dimension";
        fs::create_directories(settingsDir);
        m_IniPath = (settingsDir / "EditorLayout.ini").string();
        ImGui::GetIO().IniFilename = m_IniPath.c_str();
        m_ResetLayout = !fs::exists(m_IniPath);

        LoadGameModule();
        InitCSharp();
        m_SceneTarget = MakeScope<SceneRenderTarget>(m_SceneViewSize.x, m_SceneViewSize.y);
        m_GameTarget = MakeScope<SceneRenderTarget>(m_GameViewSize.x, m_GameViewSize.y);

        fs::path start = AssetManager::GetAbsolutePath(Project::GetConfig().StartScene);
        if (!start.empty() && fs::exists(start)) OpenScene(start);
        else NewScene();

        Log::Info("Editor ready. Scene view: RMB+WASD fly, Alt+LMB orbit, MMB pan, wheel zoom, F frame, W/E/R gizmos.");
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

    bool EditorLayer::SaveSceneAs()
    {
        auto path = FileDialogs::SaveFile({ { L"Scene", L"*.scene" } }, L"scene", AssetManager::GetAssetsDirectory());
        if (!path) return false;
        m_Ctx.ScenePath = *path;
        m_Ctx.EditorScene->SetName(path->stem().string());
        return SaveScene();
    }

    bool EditorLayer::ConfirmDiscardChanges()
    {
        if (!m_Ctx.Dirty) return true;
        int choice = FileDialogs::YesNoCancel("Unsaved Changes", "The scene '" + m_Ctx.EditorScene->GetName() + "' has unsaved changes. Save them?");
        if (choice == 0) return SaveScene();
        return choice == 1;
    }

    void EditorLayer::UpdateWindowTitle()
    {
        std::string title = "Dimension Editor - " + Project::GetConfig().Name + " - " + m_Ctx.EditorScene->GetName() + (m_Ctx.Dirty ? "*" : "");
        if (m_Ctx.IsPlaying()) title += "  [PLAYING]";
        Application::Get().GetWindow().SetTitle(title);
    }

    // ---- Play mode ------------------------------------------------------------------------

    void EditorLayer::Play()
    {
        if (m_Ctx.IsPlaying()) return;
        if (m_Console->ClearOnPlay) m_Console->Clear();
        m_Ctx.RuntimeScene = m_Ctx.EditorScene->Clone();
        m_Ctx.RuntimeScene->OnRuntimeStart();
        m_Ctx.State = PlayState::Play;
        m_FocusGameView = 3;
        UpdateWindowTitle();
    }

    void EditorLayer::Stop()
    {
        if (!m_Ctx.IsPlaying()) return;
        m_Ctx.RuntimeScene->OnRuntimeStop();
        m_Ctx.RuntimeScene.reset();
        m_Ctx.State = PlayState::Edit;
        if (!m_Ctx.EditorScene->IsValid(m_Ctx.Selected)) m_Ctx.Select(Entity::Null());
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
        if (m_Ctx.RequestOpenScene)
        {
            fs::path p = *m_Ctx.RequestOpenScene;
            m_Ctx.RequestOpenScene.reset();
            if (ConfirmDiscardChanges()) OpenScene(p);
        }

        PollGameModule(dt);
        PollCSharp(dt);
        m_SceneTarget->Resize(m_SceneViewSize.x, m_SceneViewSize.y);
        m_GameTarget->Resize(m_GameViewSize.x, m_GameViewSize.y);

        // Pick up files changed outside the editor when the window regains focus.
        bool focused = Application::Get().GetWindow().IsFocused();
        if (focused && !m_WasWindowFocused)
        {
            AssetManager::Refresh();
            if (!m_Ctx.IsPlaying())
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
        if (m_Ctx.IsPlaying()) m_Ctx.RuntimeScene->OnUpdateRuntime(dt);
        else m_Ctx.EditorScene->OnUpdateEditor(dt);

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
        if (m_ShowStats) DrawStats();
        DrawGameView();
        DrawSceneView();
        if (m_ShowProjectSettings) DrawProjectSettings();
        if (m_ShowAbout) DrawAbout();
        if (m_ShowImGuiDemo) ImGui::ShowDemoWindow(&m_ShowImGuiDemo);
        HandleShortcuts();
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
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Scene", "Ctrl+N") && ConfirmDiscardChanges()) NewScene();
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O"))
            {
                auto p = FileDialogs::OpenFile({ { L"Scene", L"*.scene" } }, AssetManager::GetAssetsDirectory());
                if (p && ConfirmDiscardChanges()) OpenScene(*p);
            }
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
            if (ImGui::MenuItem("Build Game...", nullptr, false, !m_Ctx.IsPlaying())) BuildGame();
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
            if (m_BuildState && m_BuildState->load() == 1) label = "Compiling scripts...   " + label;
            else if (m_CSharpBuild && m_CSharpBuild->load() == 1) label = "Compiling C#...   " + label;
            else if (m_CSharpReloadPending) label = "C# changed: reload after Stop   " + label;
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

        // Transform gizmo
        bool gizmoHovered = false;
        if (m_Ctx.HasValidSelection() && m_GizmoOperation != 0 && !m_Camera.IsControlling())
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
        if (m_SceneViewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoHovered && !alt)
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
                                           { avail.x, avail.y }, imageHovered || ImGui::IsMouseDown(ImGuiMouseButton_Left));
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
        ImGui::End();
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
        ImGui::Text("Shadow casters: %u", st.ShadowCasters);
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
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
            auto p = FileDialogs::OpenFile({ { L"Scene", L"*.scene" } }, AssetManager::GetAssetsDirectory());
            if (p && ConfirmDiscardChanges()) OpenScene(*p);
        }
        if (m_GameViewFocused && m_Ctx.IsPlaying()) return; // the game owns the keyboard

        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) m_Ctx.Undo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) m_Ctx.Redo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateSelected(m_Ctx);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySelected(m_Ctx);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) PasteClipboard(m_Ctx);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelected(m_Ctx);

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

    void EditorLayer::BuildGame()
    {
        if (!SaveScene()) return;
        auto& config = Project::GetConfig();
        if (!config.StartScene.IsValid())
        {
            Log::Error("Set a start scene in Project Settings before building");
            return;
        }
        auto dest = FileDialogs::PickFolder(Project::GetDirectory());
        if (!dest) return;

        std::error_code ec;
        fs::path player = ExecutableDirectory() / "DimensionPlayer.exe";
        if (!fs::exists(player))
        {
            Log::Error("Player runtime not found at '{}'", player.string());
            return;
        }
        std::string exeName = config.Name.empty() ? "Game" : config.Name;
        fs::create_directories(*dest, ec);
        fs::copy_file(player, *dest / (exeName + ".exe"), fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            Log::Error("Copying player failed: {}", ec.message());
            return;
        }
        for (const char* dll : { "DimensionEngine.dll" })
            fs::copy_file(ExecutableDirectory() / dll, *dest / dll, fs::copy_options::overwrite_existing, ec);
        fs::path module = ExecutableDirectory() / (config.GameModule + ".dll");
        if (fs::exists(module)) fs::copy_file(module, *dest / module.filename(), fs::copy_options::overwrite_existing, ec);
        if (fs::exists(ExecutableDirectory() / "nethost.dll"))
        {
            fs::copy_file(ExecutableDirectory() / "nethost.dll", *dest / "nethost.dll", fs::copy_options::overwrite_existing, ec);
            fs::copy(ExecutableDirectory() / "managed", *dest / "managed", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
        fs::path projectOut = *dest / "Project";
        fs::remove_all(projectOut, ec);
        fs::create_directories(projectOut, ec);
        fs::copy_file(Project::GetDirectory() / Project::kFileName, projectOut / Project::kFileName, ec);
        fs::copy(Project::GetAssetsDirectory(), projectOut / "Assets", fs::copy_options::recursive, ec);
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
            return;
        }
        Log::Info("Built '{}' to {}", exeName, dest->string());
        FileDialogs::OpenWithDefaultApp(*dest);
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
        if (m_CSharpReloadPending && !m_Ctx.IsPlaying())
        {
            m_CSharpReloadPending = false;
            // Edit-time scenes never hold C# instances, so the assembly can be swapped directly.
            CSharpRuntime::LoadScripts(CSharpProject::AssemblyPath(project));
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
