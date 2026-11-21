// Dimension Editor entry point.
// Usage: DimensionEditor [projectDir] [--play] [--frames N] [--screenshot file.png] [--build outputFolder] [--launcher]
// Without a project folder the launch screen (Launcher.h) picks one.
#include <Dimension/Core/AppRuntime.h>
#include "EditorLayer.h"
#include "Launcher.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Core/Profiler.h>
#include <Dimension/Platform/Platform.h>
#include <Dimension/Platform/SplashScreen.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Project/ProjectHub.h>
#include <Dimension/Scripting/GameModule.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace Dimension;

namespace
{
    // The editor is a Windows GUI program (no console window opens with it). Started from a terminal, it writes its
    // log there anyway; output redirected to a file or pipe already has somewhere to go.
    void UseParentConsole()
    {
#ifdef _WIN32
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
#endif
    }

    // The log also goes to a file, since there's no console to read it in (like Unity's Editor.log):
    // %LOCALAPPDATA%/Dimension/Editor.log, ~/.cache/Dimension/Editor.log.
    void LogToFile()
    {
        std::error_code ec;
        fs::path dir = Platform::UserCacheDirectory() / "Dimension";
        fs::create_directories(dir, ec);
        auto file = std::make_shared<std::ofstream>(dir / "Editor.log", std::ios::trunc);
        if (!*file) return;
        Log::AddSink([file](const LogEntry& e) {
            static const char* levels[] = { "trace", "info", "warn", "error" };
            char time[32];
            std::snprintf(time, sizeof(time), "%10.3f", e.Time);
            *file << "[" << time << "] [" << levels[(int)e.Level] << "] " << e.Message << '\n';
            if (e.Level >= LogLevel::Warn) file->flush();
        });
    }
}

int main(int argc, char** argv)
{
    Dimension::AppRuntime::SetHost(Dimension::AppRuntime::Host::Editor); // Application.Quit is ignored, as in Unity's editor
    UseParentConsole();
    LogToFile();
    fs::path projectDir;
    bool showLauncher = false;
    Editor::EditorOptions options;
    ApplicationSpec spec;
    spec.Name = "Dimension Editor";
    spec.Window.Title = "Dimension Editor";
    spec.Window.AppId = "dimension-editor"; // (the installs' dimension-editor.desktop)
    spec.Window.Width = 1600;
    spec.Window.Height = 900;
    spec.Window.Maximized = true;

    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--play")) options.StartInPlayMode = true;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) spec.MaxFrames = std::atoi(argv[++i]), options.Automated = true;
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) spec.ScreenshotPath = argv[++i], options.Automated = true;
        else if (!std::strcmp(argv[i], "--windowed")) spec.Window.Maximized = false;
        else if (!std::strcmp(argv[i], "--select") && i + 1 < argc) options.SelectEntity = argv[++i];
        else if (!std::strcmp(argv[i], "--select-asset") && i + 1 < argc) options.SelectAsset = argv[++i];
        else if (!std::strcmp(argv[i], "--focus") && i + 1 < argc) options.FocusWindow = argv[++i];
        else if (!std::strcmp(argv[i], "--build") && i + 1 < argc) options.BuildFolder = argv[++i];
        else if (!std::strcmp(argv[i], "--open") && i + 1 < argc) options.OpenWindow = argv[++i];
        else if (!std::strcmp(argv[i], "--line-point") && i + 1 < argc) options.LinePoint = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--anim-key") && i + 1 < argc) options.AnimKey = argv[++i];
        else if (!std::strcmp(argv[i], "--anim-record")) options.AnimRecord = true;
        else if (!std::strcmp(argv[i], "--package-search") && i + 1 < argc) options.PackageSearch = argv[++i];
        else if (!std::strcmp(argv[i], "--run-tests")) options.RunTests = true;
        else if (!std::strcmp(argv[i], "--no-splash")) options.NoSplash = true;
        else if (!std::strcmp(argv[i], "--quality") && i + 1 < argc) options.Quality = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frame-caps")) options.KeepFrameCaps = true;
        else if (!std::strcmp(argv[i], "--deep-profile")) Dimension::Profiler::SetDeepProfiling(true);
        else if (!std::strcmp(argv[i], "--anim-onion")) options.AnimOnion = true;
        else if (!std::strcmp(argv[i], "--frame-event") && i + 1 < argc) options.FrameEvent = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--menu") && i + 1 < argc) options.MenuCommand = argv[++i];
        else if (!std::strcmp(argv[i], "--search") && i + 1 < argc) options.Search = argv[++i];
        else if (!std::strcmp(argv[i], "--project-view") && i + 1 < argc) options.ProjectView = argv[++i];
        else if (!std::strcmp(argv[i], "--project-search") && i + 1 < argc) options.ProjectSearch = argv[++i];
        else if (!std::strcmp(argv[i], "--vfx-node") && i + 1 < argc) options.VfxNode = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--asset-preview") && i + 1 < argc) options.AssetPreview = argv[++i];
        else if (!std::strcmp(argv[i], "--measure") && i + 1 < argc) options.Measure = argv[++i];
        else if (!std::strcmp(argv[i], "--selection-set") && i + 1 < argc) options.SelectionSet = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--press") && i + 1 < argc) options.Press = argv[++i];
        else if (!std::strcmp(argv[i], "--input") && i + 1 < argc) options.Input = argv[++i];
        else if (!std::strcmp(argv[i], "--grid") && i + 1 < argc) options.Grid = argv[++i];
        else if (!std::strcmp(argv[i], "--new-inspector")) options.NewInspector = true;
        else if (!std::strcmp(argv[i], "--lock-inspector")) options.LockInspector = true;
        else if (!std::strcmp(argv[i], "--fold") && i + 1 < argc) options.Fold = argv[++i];
        else if (!std::strcmp(argv[i], "--inspector-search") && i + 1 < argc) options.InspectorSearch = argv[++i];
        else if (!std::strcmp(argv[i], "--profiler-view") && i + 1 < argc) options.ProfilerView = argv[++i];
        else if (!std::strcmp(argv[i], "--profiler-open") && i + 1 < argc) options.ProfilerOpen = argv[++i];
        else if (!std::strcmp(argv[i], "--profiler-pin") && i + 1 < argc) options.ProfilerPin = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--profiler-pick") && i + 1 < argc) options.ProfilerPick = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--view") && i + 1 < argc)
        {
            std::string v = argv[++i];
            options.View = v == "all" ? 0 : std::atoi(v.c_str());
            options.ViewGiven = true;
        }
        else if (!std::strcmp(argv[i], "--find") && i + 1 < argc) options.SearchQuery = argv[++i];
        else if (!std::strcmp(argv[i], "--package") && i + 1 < argc) options.PackagePath = argv[++i];
        else if (!std::strcmp(argv[i], "--open-additive") && i + 1 < argc) options.AdditiveScenes = argv[++i];
        else if (!std::strcmp(argv[i], "--prefab-instance") && i + 1 < argc) options.PrefabInstance = argv[++i];
        else if (!std::strcmp(argv[i], "--new-scene") && i + 1 < argc) options.NewSceneTemplate = argv[++i];
        else if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) options.Layout = argv[++i];
        else if (!std::strcmp(argv[i], "--no-recent")) options.RememberProject = false;
        else if (!std::strcmp(argv[i], "--visualize-nav")) options.VisualizeNavigation = true;
        else if (!std::strcmp(argv[i], "--inspector-debug")) options.InspectorDebug = true;
        else if (!std::strcmp(argv[i], "--draw-mode") && i + 1 < argc) options.DrawMode = argv[++i];
        else if (!std::strcmp(argv[i], "--hide") && i + 1 < argc) options.HideEntities = argv[++i];
        else if (!std::strcmp(argv[i], "--isolate") && i + 1 < argc) options.IsolateEntities = argv[++i];
        else if (!std::strcmp(argv[i], "--wait-for-debugger")) options.WaitForDebugger = true;
        else if (!std::strcmp(argv[i], "--theme") && i + 1 < argc)
        {
            std::string t = argv[++i];
            options.Theme = t == "light" ? 1 : t == "midnight" ? 2 : 0;
        }
        else if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) options.ScenePath = argv[++i];
        else if (!std::strcmp(argv[i], "--prefab") && i + 1 < argc) options.PrefabPath = argv[++i];
        else if (!std::strcmp(argv[i], "--model") && i + 1 < argc) options.ModelPath = argv[++i];
        else if (!std::strcmp(argv[i], "--song") && i + 1 < argc) options.SongPath = argv[++i];
        else if (!std::strcmp(argv[i], "--open-scene") && i + 1 < argc) options.OpenSceneLater = argv[++i];
        else if (!std::strcmp(argv[i], "--menu-open-scene")) options.MenuOpenScene = true;
        else if (!std::strcmp(argv[i], "--launcher")) showLauncher = true;
        else projectDir = argv[i];
    }

    // No project given: the launch screen picks one (or makes one). Automated runs (screenshots, batch builds) take
    // the Sandbox instead in a development build (the test bed, as they always have), the sample from an install.
    std::optional<Editor::Launcher::Choice> choice;
    bool automated = spec.MaxFrames > 0 || !spec.ScreenshotPath.empty() || !options.BuildFolder.empty();
    if (showLauncher || (projectDir.empty() && !automated))
    {
        choice = Editor::Launcher::Run(showLauncher ? spec.MaxFrames : 0, showLauncher ? spec.ScreenshotPath : std::string());
        if (!choice) return 0; // (closed)
        projectDir = choice->Project;
    }
#ifdef DIM_DEFAULT_PROJECT
    if (projectDir.empty() && fs::exists(fs::path(DIM_DEFAULT_PROJECT) / "Project.dproj")) projectDir = DIM_DEFAULT_PROJECT;
#endif
    if (projectDir.empty()) projectDir = Editor::Launcher::SampleProject();
    if (projectDir.empty())
    {
        Log::Error("No project to open: pass a project folder (DimensionEditor <folder>)");
        return 1;
    }

    // The splash screen until the editor is ready to show the scene (EditorLayer closes it); the main window waits
    // hidden behind it. Not for batch builds (no window is ever shown).
    if (!options.NoSplash && options.BuildFolder.empty())
    {
        std::string project = fs::absolute(projectDir).lexically_normal().filename().string();
        if (project.empty()) project = fs::absolute(projectDir).lexically_normal().parent_path().filename().string();
        SplashScreen::Show("Dimension Editor", project);
        SplashScreen::SetStatus("Starting the renderer...");
        // Only where there is a splash screen (Windows). Elsewhere the window shows at once: Wayland never lets a
        // hidden window's frames be presented (vkQueuePresentKHR waits for a frame callback that does not come).
        spec.Window.Hidden = Platform::IsWindows();
    }
    {
        Application app(spec);
        if (choice && choice->Create)
        {
            SplashScreen::SetStatus("Creating the project...");
            if (!ProjectHub::Create(projectDir, choice->Name, choice->StarterScene))
            {
                SplashScreen::Close();
                Log::Error("Could not create a project in '{}'", projectDir.string());
                return 1;
            }
            Log::Info("Created project '{}' in '{}'", choice->Name, projectDir.string());
        }
        SplashScreen::SetStatus("Opening the project (importing changed assets)...");
        if (!Project::Open(projectDir))
        {
            SplashScreen::Close();
            Log::Error("Could not open project '{}'", projectDir.string());
            return 1;
        }
        app.PushLayer(MakeScope<Editor::EditorLayer>(options));
        app.Run();
        SplashScreen::Close(); // (quit before it was ready)
        app.PopAllLayers(); // scenes may hold objects created by the game module
        GameModule::Unload();
    }
    if (!options.BuildFolder.empty()) return options.BuildSucceeded ? 0 : 2;
    return 0;
}
