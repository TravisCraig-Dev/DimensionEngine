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
            m_Scene->SetUIInput(Input::GetMousePosition(), Input::RawMouseDown(MouseButton::Left), { (float)size.x, (float)size.y });
            m_Scene->OnUpdateRuntime(dt);
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

    // Window size comes from the project file; read it before creating the app.
    ApplicationSpec spec;
    spec.Name = "Dimension Player";
    spec.EnableImGui = false;
    spec.MaxFrames = maxFrames;
    spec.ScreenshotPath = screenshot;
    spec.Window.Width = 1280;
    spec.Window.Height = 720;
    {
        Application app(spec);
        if (!Project::Open(projectDir)) return 1;
        auto& config = Project::GetConfig();
        app.GetWindow().SetTitle(config.Name);
        GameModule::Load(ExecutableDirectory() / (config.GameModule + ".dll"));
        if (CSharpRuntime::Init(ExecutableDirectory() / "managed"))
            CSharpRuntime::LoadScripts(CSharpProject::AssemblyPath(Project::GetDirectory()));

        fs::path scene = sceneOverride.empty() ? AssetManager::GetAbsolutePath(config.StartScene) : sceneOverride;
        if (scene.empty()) Log::Error("Project has no start scene. Set one in the editor (File > Project Settings).");
        app.PushLayer(MakeScope<PlayerLayer>(scene));
        app.Run();
        app.PopAllLayers();
        GameModule::Unload();
    }
    return 0;
}
