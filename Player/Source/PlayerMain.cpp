// Dimension Player: runs a project's start scene without the editor.
// Usage: DimensionPlayer [projectDir] [--scene path] [--frames N]
// A built game ships as DimensionPlayer.exe next to a "Project" folder.
#include <Dimension/Dimension.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Scripting/GameModule.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scripting/CSharpRuntime.h>
#include <Dimension/Scripting/CSharpProject.h>
#include <cstring>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace Dimension;

namespace
{
    class PlayerLayer : public Layer
    {
    public:
        explicit PlayerLayer(fs::path scenePath) : m_ScenePath(std::move(scenePath)) {}

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
            m_Scene->SetUIInput(Input::GetMousePosition(), Input::RawMouseDown(MouseButton::Left), { (float)size.x, (float)size.y }, true,
                                Input::GetScrollDelta());
            m_Scene->OnUpdateRuntime(dt);
            // Scene switches requested by scripts take effect between frames.
            if (SceneManager::HasPendingLoad())
                if (auto next = SceneManager::LoadPending())
                {
                    m_Scene->OnRuntimeStop();
                    m_Scene = next;
                    m_Scene->OnRuntimeStart();
                }
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
        }

        void OnCompose(VkCommandBuffer cmd, VkImage swapchain, VkExtent2D extent) override
        {
            m_Target->BlitOutputTo(cmd, swapchain, extent);
        }

    private:
        fs::path m_ScenePath;
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
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) sceneOverride = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
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
    spec.VSync = config.VSync;
    spec.Window.Title = config.Name;
    spec.Window.Width = std::max(320u, config.WindowWidth);
    spec.Window.Height = std::max(200u, config.WindowHeight);
    spec.Window.Fullscreen = config.Fullscreen && maxFrames == 0; // automated runs stay windowed
    {
        Application app(spec);
        GameModule::Load(ExecutableDirectory() / (config.GameModule + ".dll"));
        if (CSharpRuntime::Init(ExecutableDirectory() / "managed"))
            CSharpRuntime::LoadScripts(CSharpProject::AssemblyPath(Project::GetDirectory()));

        auto buildScenes = SceneManager::GetBuildScenes();
        fs::path scene = !sceneOverride.empty() ? sceneOverride : buildScenes.empty() ? fs::path() : AssetManager::GetAbsolutePath(buildScenes[0]);
        if (scene.empty()) Log::Error("Project has no scenes to run. Add some in the editor (File > Build Settings).");
        app.PushLayer(MakeScope<PlayerLayer>(scene));
        app.Run();
        app.PopAllLayers();
        GameModule::Unload();
    }
    return 0;
}
