// Dimension Player: runs a project's start scene without the editor.
// Usage: DimensionPlayer [projectDir] [--scene path] [--frames N] [--screenshot file]
//        [--move x,y,frame] [--click x,y,frame]   simulated pointer for automated checks (framebuffer pixels)
//        [--no-instancing] [--no-vsync]           draw every mesh separately / uncapped frame rate (for comparisons)
//        [--no-clusters]                          shade every light at every pixel (for comparisons)
// A built game ships as DimensionPlayer.exe next to a "Project" folder.
#include <Dimension/Dimension.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Scripting/GameModule.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scripting/CSharpRuntime.h>
#include <Dimension/Scripting/CSharpProject.h>
#include <Dimension/Platform/AppIcon.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace Dimension;

namespace
{
    // Scripted pointer: from Frame on the pointer sits at Position; a click is down for one frame, then up.
    struct PointerEvent
    {
        int Frame;
        glm::vec2 Position;
        bool Click;
    };

    class PlayerLayer : public Layer
    {
    public:
        PlayerLayer(fs::path scenePath, std::vector<PointerEvent> pointerEvents)
            : m_ScenePath(std::move(scenePath)), m_PointerEvents(std::move(pointerEvents)) {}

        void OnAttach() override
        {
            m_Scene = SceneSerializer::LoadFromFile(m_ScenePath);
            if (!m_Scene)
            {
                Log::Error("Could not load start scene; showing an empty scene");
                m_Scene = MakeRef<Scene>("Empty");
            }
            Prefabs::SyncAll(*m_Scene);
            auto size = Application::Get().GetWindow().GetFramebufferSize();
            m_Target = MakeScope<SceneRenderTarget>(size.x, size.y);
            m_Scene->OnRuntimeStart();
        }

        void OnDetach() override
        {
            if (m_Frame > 0)
            {
                // Summary for automated runs (frame times skip the first frames, which include loading).
                const auto& st = m_Renderer.GetStats();
                float avg = m_TimedFrames ? m_TimedSeconds / m_TimedFrames * 1000.0f : 0.0f;
                float n = (float)std::max(1, m_TimedFrames);
                Log::Info("Stats: {} draw calls, {} meshes ({} instanced), {} triangles, {} shadow casters; {:.2f} ms/frame over {} frames "
                          "(CPU: update {:.2f} ms, render recording {:.2f} ms, of which render list {:.2f} ms)",
                          st.DrawCalls, st.Meshes, st.InstancedMeshes, st.Triangles, st.ShadowCasters, avg, m_TimedFrames,
                          m_UpdateMs / n, m_RenderMs / n, m_ListMs / n);
            }
            m_Scene->OnRuntimeStop();
            m_Scene.reset();
            m_Target.reset();
        }

        void OnUpdate(float dt) override
        {
            auto size = Application::Get().GetWindow().GetFramebufferSize();
            m_Target->Resize(size.x, size.y);
            if (Input::IsKeyPressed(Key::Escape)) Application::Get().Close();
            if (Input::IsKeyPressed(Key::Enter) && (Input::RawKeyDown(Key::LeftAlt) || Input::RawKeyDown(Key::RightAlt)))
            {
                auto& window = Application::Get().GetWindow();
                window.SetFullscreen(!window.IsFullscreen());
            }
            glm::vec2 pointer = Input::GetMousePosition();
            bool down = Input::RawMouseDown(MouseButton::Left);
            if (!m_PointerEvents.empty())
            {
                // Simulated input replaces the mouse entirely.
                down = false;
                pointer = m_SimulatedPointer;
                for (auto& ev : m_PointerEvents)
                {
                    if (ev.Frame <= m_Frame) pointer = m_SimulatedPointer = ev.Position;
                    if (ev.Click && ev.Frame == m_Frame) down = true;
                }
            }
            ++m_Frame;
            if (m_Frame > 30)
            {
                m_TimedSeconds += dt;
                ++m_TimedFrames;
            }
            m_Scene->SetUIInput(pointer, down, { (float)size.x, (float)size.y }, true, Input::GetScrollDelta());
            auto updateStart = std::chrono::steady_clock::now();
            m_Scene->OnUpdateRuntime(dt);
            if (m_Frame > 30) m_UpdateMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - updateStart).count();
            // Scene loads requested by scripts take effect between frames, inside the running scene
            // (objects marked DontDestroyOnLoad survive).
            if (SceneManager::HasPendingLoad()) SceneManager::ApplyPending(*m_Scene);
        }

        void OnRender(VkCommandBuffer cmd) override
        {
            Entity cam = m_Scene->GetPrimaryCamera();
            CameraData data;
            if (!cam.IsNull()) data = CameraData::FromEntity(*m_Scene, cam, m_Target->GetAspect());
            else Log::Warn("Scene has no camera");
            SceneRenderOptions options;
            options.RenderUI = true;
            m_Renderer.Render(cmd, *m_Scene, data, *m_Target, options);
            if (m_Frame > 30) { m_RenderMs += m_Renderer.GetStats().CpuMs; m_ListMs += m_Renderer.GetStats().ListMs; }
        }

        void OnCompose(VkCommandBuffer cmd, VkImage swapchain, VkExtent2D extent) override
        {
            m_Target->BlitOutputTo(cmd, swapchain, extent);
        }

    private:
        fs::path m_ScenePath;
        std::vector<PointerEvent> m_PointerEvents;
        glm::vec2 m_SimulatedPointer{ -1.0f };
        int m_Frame = 0;
        float m_TimedSeconds = 0.0f;
        float m_UpdateMs = 0.0f, m_RenderMs = 0.0f, m_ListMs = 0.0f;
        int m_TimedFrames = 0;
        Ref<Scene> m_Scene;
        Scope<SceneRenderTarget> m_Target;
        SceneRenderer m_Renderer;
    };

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
}

int main(int argc, char** argv)
{
    fs::path projectDir;
    fs::path sceneOverride;
    int maxFrames = 0;
    std::string screenshot;
    std::vector<PointerEvent> pointerEvents;
    bool noVSync = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) sceneOverride = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--no-instancing")) SceneRenderer::SetInstancingEnabled(false);
        else if (!std::strcmp(argv[i], "--no-clusters")) SceneRenderer::SetLightClusteringEnabled(false);
        else if (!std::strcmp(argv[i], "--no-vsync")) noVSync = true;
        else if ((!std::strcmp(argv[i], "--click") || !std::strcmp(argv[i], "--move")) && i + 1 < argc)
        {
            bool click = !std::strcmp(argv[i], "--click");
            float x = 0, y = 0;
            int frame = 0;
            if (std::sscanf(argv[++i], "%f,%f,%d", &x, &y, &frame) == 3) pointerEvents.push_back({ frame, { x, y }, click });
        }
        else projectDir = argv[i];
    }
    if (projectDir.empty())
    {
        fs::path bundled = ExecutableDirectory() / "Project";
        projectDir = fs::exists(bundled / Project::kFileName) ? bundled : fs::path(DIM_DEFAULT_PROJECT);
    }

#ifdef DIM_GAME_BUILD
    // No console in shipped builds: keep a log file next to the executable (like Unity's Player.log).
    auto logFile = std::make_shared<std::ofstream>(ExecutableDirectory() / "Player.log", std::ios::trunc);
    Log::AddSink([logFile](const LogEntry& e) {
        static const char* levels[] = { "trace", "info", "warn", "error" };
        *logFile << "[" << e.Time << "] [" << levels[(int)e.Level] << "] " << e.Message << std::endl;
    });
#endif
    // The project is opened first so its window settings apply.
    if (!Project::Open(projectDir)) return 1;
    auto& config = Project::GetConfig();
    ApplicationSpec spec;
    spec.Name = config.Name;
    spec.EnableImGui = false;
    spec.MaxFrames = maxFrames;
    spec.ScreenshotPath = screenshot;
    spec.VSync = config.VSync && !noVSync;
    spec.Window.Title = config.Name;
    spec.Window.Width = std::max(320u, config.WindowWidth);
    spec.Window.Height = std::max(200u, config.WindowHeight);
    spec.Window.Fullscreen = config.Fullscreen && maxFrames == 0; // automated runs stay windowed
    {
        Application app(spec);
        // The project's icon on the window (a built game also has it in its executable).
        if (config.Icon)
            if (auto image = AppIcon::Load(AssetManager::GetAbsolutePath(config.Icon)))
            {
                std::vector<AppIcon::Image> sizes{ AppIcon::Resize(*image, 16), AppIcon::Resize(*image, 32), AppIcon::Resize(*image, 48) };
                std::vector<Window::IconImage> icons;
                for (auto& s : sizes) icons.push_back({ s.Width, s.Height, s.Rgba.data() });
                app.GetWindow().SetIcon(icons);
            }
        GameModule::Load(ExecutableDirectory() / (config.GameModule + ".dll"));
        if (CSharpRuntime::Init(ExecutableDirectory() / "managed"))
            CSharpRuntime::LoadScripts(CSharpProject::AssemblyPath(Project::GetDirectory()));

        auto buildScenes = SceneManager::GetBuildScenes();
        fs::path scene = !sceneOverride.empty() ? sceneOverride : buildScenes.empty() ? fs::path() : AssetManager::GetAbsolutePath(buildScenes[0]);
        if (scene.empty()) Log::Error("Project has no scenes to run. Add some in the editor (File > Build Settings).");
        app.PushLayer(MakeScope<PlayerLayer>(scene, std::move(pointerEvents)));
        app.Run();
        app.PopAllLayers();
        GameModule::Unload();
    }
    return 0;
}
