// Dimension Player: runs a project's start scene without the editor.
// Usage: DimensionPlayer [projectDir] [--scene path] [--frames N] [--screenshot file]
//        [--move x,y,frame] [--click x,y,frame]   simulated pointer for automated checks (framebuffer pixels)
//        [--key name,frame]   a key pressed for that one frame (Right, Left, Up, Down, Enter, Space, Tab, Escape, A-Z)
//        [--no-instancing] [--no-vsync]           draw every mesh separately / uncapped frame rate (for comparisons)
//        [--no-clusters]                          shade every light at every pixel (for comparisons)
//        [--cpu-light-culling]                    build the light clusters on the CPU instead of in a compute shader
//        [--cpu-blend-shapes]                     apply skinned models' blend shapes on the CPU instead of in a compute shader
//        [--no-occlusion]                         draw meshes hidden behind others too (for comparisons)
//        [--fixed-dt seconds]                     every frame advances the game by this step (repeatable runs)
//        [--profile file.dprof]                   save the profiler's last frames (CPU sections, GPU passes) on exit
//        [--wait-for-debugger]                    wait up to 60 s for a C# debugger to attach
//        [--system-language fr-FR]                start as if the system's language were that (unless one was chosen)
//        [--ime text]                             an input method composing this text from frame 60 (\uXXXX escapes allowed)
// A built game ships as DimensionPlayer.exe next to a "Project" folder.
#include <Dimension/Dimension.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Core/PlayerPrefs.h>
#include <Dimension/Project/Localization.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Renderer/CameraStack.h>
#include <Dimension/Core/Profiler.h>
#include <Dimension/Renderer/GpuMemory.h>
#include <Dimension/Scripting/GameModule.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scripting/CSharpRuntime.h>
#include <Dimension/Renderer/Quality.h>
#include <Dimension/Scripting/CSharpProject.h>
#include <Dimension/Platform/AppIcon.h>
#include <Dimension/Core/AppRuntime.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <Dimension/Platform/Platform.h>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace Dimension;

namespace
{
    // Scripted pointer: from Frame on the pointer sits at Position; a click is down for one frame, then up.
    float s_FixedDt = 0.0f; // --fixed-dt: the step every frame advances the game by (0 = real time)
    std::string s_ImeText;  // --ime: an input method composing this text from frame 60

    struct PointerEvent
    {
        int Frame;
        glm::vec2 Position;
        bool Click;
    };
    // Scripted key: down for one frame.
    struct KeyEvent
    {
        int Frame;
        Key Code;
    };

    bool ParseKey(const std::string& name, Key& key)
    {
        static const std::pair<const char*, Key> named[] = { { "Right", Key::Right }, { "Left", Key::Left }, { "Up", Key::Up },
                                                             { "Down", Key::Down }, { "Enter", Key::Enter }, { "Space", Key::Space },
                                                             { "Tab", Key::Tab }, { "Escape", Key::Escape } };
        for (auto& [n, k] : named)
            if (name == n) { key = k; return true; }
        if (name.size() == 1 && std::isalpha((unsigned char)name[0]))
        {
            key = (Key)std::toupper((unsigned char)name[0]);
            return true;
        }
        return false;
    }

    class PlayerLayer : public Layer
    {
    public:
        PlayerLayer(fs::path scenePath, std::vector<PointerEvent> pointerEvents, std::vector<KeyEvent> keyEvents)
            : m_ScenePath(std::move(scenePath)), m_PointerEvents(std::move(pointerEvents)), m_KeyEvents(std::move(keyEvents)) {}

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
            CSharpRuntime::RunRuntimeInitializers(*m_Scene, false); // [RuntimeInitializeOnLoadMethod] (BeforeSceneLoad...)
            m_Scene->OnRuntimeStart();
            CSharpRuntime::RunRuntimeInitializers(*m_Scene, true); // (AfterSceneLoad)
        }

        void OnDetach() override
        {
            if (m_Frame > 0)
            {
                // Summary for automated runs (frame times skip the first frames, which include loading).
                const auto& st = m_Renderer.GetStats();
                float avg = m_TimedFrames ? m_TimedSeconds / m_TimedFrames * 1000.0f : 0.0f;
                float n = (float)std::max(1, m_TimedFrames);
                Log::Info("Stats: {} draw calls, {} meshes ({} instanced, {} occluded), {} triangles, {} shadow casters; {:.2f} ms/frame over {} frames "
                          "(CPU: update {:.2f} ms, render recording {:.2f} ms, of which render list {:.2f} ms)",
                          st.DrawCalls, st.Meshes, st.InstancedMeshes, st.Occluded, st.Triangles, st.ShadowCasters, avg, m_TimedFrames,
                          m_UpdateMs / n, m_RenderMs / n, m_ListMs / n);
                // GPU memory by kind (and the device-local heaps' use against their budget): for runs that check it fits.
                std::string memory;
                for (int k = 0; k < (int)GpuMemory::Kind::Count; ++k)
                {
                    GpuMemory::Usage u = GpuMemory::Get((GpuMemory::Kind)k);
                    memory += std::format("{}{} {:.0f} MB ({})", k ? ", " : "", GpuMemory::Name((GpuMemory::Kind)k), u.Bytes / 1048576.0, u.Count);
                }
                for (const GpuMemory::Heap& h : GpuMemory::Heaps())
                    if (h.DeviceLocal) memory += std::format("; device heap {:.0f} of {:.0f} MB", h.Used / 1048576.0, h.Budget / 1048576.0);
                Log::Info("GPU memory: {}", memory);
            }
            m_Scene->NotifyApplicationQuit();
            m_Scene->OnRuntimeStop();
            m_Scene.reset();
            m_Target.reset();
        }

        void OnUpdate(float dt) override
        {
            if (s_FixedDt > 0.0f) dt = s_FixedDt;
            auto size = Application::Get().GetWindow().GetFramebufferSize();
            uint32_t tw, th;
            Quality::ScaledSize(size.x, size.y, tw, th); // (the quality level's Render Scale; the UI keeps the window's size)
            m_Target->Resize(tw, th);
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
            if (!m_PointerEvents.empty()) Input::SimulateMouseButton(MouseButton::Left, down); // scripts see the click too
            for (auto& ev : m_KeyEvents) Input::SimulateKey(ev.Code, false);
            for (auto& ev : m_KeyEvents)
                if (ev.Frame == m_Frame - 1) Input::SimulateKey(ev.Code, true); // (m_Frame was advanced above)
            if (!s_ImeText.empty() && m_Frame >= 60) Input::SetComposition(s_ImeText, (int)s_ImeText.size()); // (--ime)
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
            // Other cameras with smaller viewports draw over it (picture in picture, split screen).
            m_Cameras.Render(cmd, *m_Scene, { m_Target->GetWidth(), m_Target->GetHeight() }, options.Underlays);
            m_Renderer.Render(cmd, *m_Scene, data, *m_Target, options);
            const RenderStats& st = m_Renderer.GetStats();
            Profiler::SetCounter("Draw Calls", (float)st.DrawCalls);
            Profiler::SetCounter("Triangles", (float)st.Triangles);
            if (m_Frame > 30) { m_RenderMs += m_Renderer.GetStats().CpuMs; m_ListMs += m_Renderer.GetStats().ListMs; }
        }

        void OnCompose(VkCommandBuffer cmd, VkImage swapchain, VkExtent2D extent) override
        {
            m_Target->BlitOutputTo(cmd, swapchain, extent);
        }

    private:
        fs::path m_ScenePath;
        std::vector<PointerEvent> m_PointerEvents;
        std::vector<KeyEvent> m_KeyEvents;
        glm::vec2 m_SimulatedPointer{ -1.0f };
        int m_Frame = 0;
        float m_TimedSeconds = 0.0f;
        float m_UpdateMs = 0.0f, m_RenderMs = 0.0f, m_ListMs = 0.0f;
        int m_TimedFrames = 0;
        Ref<Scene> m_Scene;
        Scope<SceneRenderTarget> m_Target;
        SceneRenderer m_Renderer;
        CameraStack m_Cameras;
    };

    fs::path ExecutableDirectory() { return Platform::ExecutableDirectory(); }
}

int main(int argc, char** argv)
{
    AppRuntime::SetHost(AppRuntime::Host::Player);
    fs::path projectDir;
    fs::path sceneOverride;
    int qualityOverride = -1;
    int maxFrames = 0;
    std::string screenshot;
    fs::path profileFile;
    std::vector<PointerEvent> pointerEvents;
    std::vector<KeyEvent> keyEvents;
    bool noVSync = false, waitForDebugger = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) sceneOverride = argv[++i];
        else if (!std::strcmp(argv[i], "--quality") && i + 1 < argc) qualityOverride = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) profileFile = argv[++i];
        else if (!std::strcmp(argv[i], "--no-instancing")) SceneRenderer::SetInstancingEnabled(false);
        else if (!std::strcmp(argv[i], "--no-clusters")) SceneRenderer::SetLightClusteringEnabled(false);
        else if (!std::strcmp(argv[i], "--cpu-light-culling")) SceneRenderer::SetGpuLightCullingEnabled(false);
        else if (!std::strcmp(argv[i], "--cpu-blend-shapes")) SceneRenderer::SetGpuBlendShapesEnabled(false);
        else if (!std::strcmp(argv[i], "--no-occlusion")) SceneRenderer::SetOcclusionCullingEnabled(false);
        else if (!std::strcmp(argv[i], "--no-vsync")) noVSync = true;
        else if (!std::strcmp(argv[i], "--fixed-dt") && i + 1 < argc) s_FixedDt = (float)std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--wait-for-debugger")) waitForDebugger = true;
        else if (!std::strcmp(argv[i], "--system-language") && i + 1 < argc) Localization::SetSystemLanguage(argv[++i]);
        else if (!std::strcmp(argv[i], "--ime") && i + 1 < argc)
        {
            // UTF-8, with \uXXXX escapes for what a command line cannot carry.
            std::string in = argv[++i];
            for (size_t k = 0; k < in.size(); ++k)
            {
                if (in[k] == '\\' && k + 5 < in.size() + 1 && in[k + 1] == 'u')
                {
                    uint32_t cp = (uint32_t)std::strtoul(in.substr(k + 2, 4).c_str(), nullptr, 16);
                    if (cp < 0x80) s_ImeText += (char)cp;
                    else if (cp < 0x800) { s_ImeText += (char)(0xC0 | (cp >> 6)); s_ImeText += (char)(0x80 | (cp & 0x3F)); }
                    else { s_ImeText += (char)(0xE0 | (cp >> 12)); s_ImeText += (char)(0x80 | ((cp >> 6) & 0x3F)); s_ImeText += (char)(0x80 | (cp & 0x3F)); }
                    k += 5;
                }
                else
                    s_ImeText += in[k];
            }
        }
        else if ((!std::strcmp(argv[i], "--click") || !std::strcmp(argv[i], "--move")) && i + 1 < argc)
        {
            bool click = !std::strcmp(argv[i], "--click");
            float x = 0, y = 0;
            int frame = 0;
            if (std::sscanf(argv[++i], "%f,%f,%d", &x, &y, &frame) == 3) pointerEvents.push_back({ frame, { x, y }, click });
        }
        else if (!std::strcmp(argv[i], "--key") && i + 1 < argc)
        {
            std::string arg = argv[++i];
            size_t comma = arg.find(',');
            Key key;
            if (comma != std::string::npos && ParseKey(arg.substr(0, comma), key)) keyEvents.push_back({ std::atoi(arg.c_str() + comma + 1), key });
            else Log::Warn("--key expects name,frame (got '{}')", arg);
        }
        else projectDir = argv[i];
    }
    if (projectDir.empty())
    {
        fs::path bundled = ExecutableDirectory() / "Project";
#ifdef DIM_DEFAULT_PROJECT
        projectDir = fs::exists(bundled / Project::kFileName) ? bundled : fs::path(DIM_DEFAULT_PROJECT);
#else
        projectDir = bundled;
#endif
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
    if (qualityOverride >= 0) Quality::SetCurrent(qualityOverride); // (--quality: else the project's default level)
    auto& config = Project::GetConfig();
    // The player's data (PlayerPrefs, save files) beside other programs' in %APPDATA%, under the product's name.
    PlayerData::SetFolder(Platform::UserDataDirectory() / PlayerData::FolderName(config.Name));
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
        if (!config.GameModule.empty()) // (projects with only C# scripts have none)
            GameModule::Load(ExecutableDirectory() / Platform::SharedLibraryName(config.GameModule));
        if (CSharpRuntime::Init(ExecutableDirectory() / "managed"))
        {
            if (waitForDebugger) CSharpRuntime::WaitForDebugger(60.0f);
            CSharpRuntime::LoadScripts(CSharpProject::AssemblyPath(Project::GetDirectory()), false);
        }

        auto buildScenes = SceneManager::GetBuildScenes();
        fs::path scene = !sceneOverride.empty() ? sceneOverride : buildScenes.empty() ? fs::path() : AssetManager::GetAbsolutePath(buildScenes[0]);
        if (scene.empty()) Log::Error("Project has no scenes to run. Add some in the editor (File > Build Settings).");
        app.PushLayer(MakeScope<PlayerLayer>(scene, std::move(pointerEvents), std::move(keyEvents)));
        app.Run();
        if (!profileFile.empty() && !Profiler::SaveRecording(profileFile, Profiler::History()))
            Log::Error("Could not save the profile to {}", profileFile.string());
        app.PopAllLayers();
        GameModule::Unload();
    }
    return 0;
}
