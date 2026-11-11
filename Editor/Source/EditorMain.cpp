// Dimension Editor entry point.
// Usage: DimensionEditor [projectDir] [--play] [--frames N] [--screenshot file.png] [--build outputFolder]
#include <Dimension/Core/AppRuntime.h>
#include "EditorLayer.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scripting/GameModule.h>
#include <cstring>

using namespace Dimension;

int main(int argc, char** argv)
{
    Dimension::AppRuntime::SetHost(Dimension::AppRuntime::Host::Editor); // Application.Quit is ignored, as in Unity's editor
    fs::path projectDir = DIM_DEFAULT_PROJECT;
    Editor::EditorOptions options;
    ApplicationSpec spec;
    spec.Name = "Dimension Editor";
    spec.Window.Title = "Dimension Editor";
    spec.Window.Width = 1600;
    spec.Window.Height = 900;
    spec.Window.Maximized = true;

    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--play")) options.StartInPlayMode = true;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) spec.MaxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) spec.ScreenshotPath = argv[++i];
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
        else if (!std::strcmp(argv[i], "--anim-onion")) options.AnimOnion = true;
        else if (!std::strcmp(argv[i], "--frame-event") && i + 1 < argc) options.FrameEvent = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--menu") && i + 1 < argc) options.MenuCommand = argv[++i];
        else if (!std::strcmp(argv[i], "--search") && i + 1 < argc) options.Search = argv[++i];
        else if (!std::strcmp(argv[i], "--project-view") && i + 1 < argc) options.ProjectView = argv[++i];
        else if (!std::strcmp(argv[i], "--project-search") && i + 1 < argc) options.ProjectSearch = argv[++i];
        else if (!std::strcmp(argv[i], "--asset-preview") && i + 1 < argc) options.AssetPreview = argv[++i];
        else if (!std::strcmp(argv[i], "--measure") && i + 1 < argc) options.Measure = argv[++i];
        else if (!std::strcmp(argv[i], "--selection-set") && i + 1 < argc) options.SelectionSet = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--press") && i + 1 < argc) options.Press = argv[++i];
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
        else if (!std::strcmp(argv[i], "--open-scene") && i + 1 < argc) options.OpenSceneLater = argv[++i];
        else if (!std::strcmp(argv[i], "--menu-open-scene")) options.MenuOpenScene = true;
        else projectDir = argv[i];
    }

    {
        Application app(spec);
        if (!Project::Open(projectDir))
        {
            Log::Error("Could not open project '{}'", projectDir.string());
            return 1;
        }
        app.PushLayer(MakeScope<Editor::EditorLayer>(options));
        app.Run();
        app.PopAllLayers(); // scenes may hold objects created by the game module
        GameModule::Unload();
    }
    if (!options.BuildFolder.empty()) return options.BuildSucceeded ? 0 : 2;
    return 0;
}
