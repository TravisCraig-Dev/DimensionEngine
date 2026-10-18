// Dimension Editor entry point.
// Usage: DimensionEditor [projectDir] [--play] [--frames N] [--screenshot file.png]
#include "EditorLayer.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scripting/GameModule.h>
#include <cstring>

using namespace Dimension;

int main(int argc, char** argv)
{
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
        else if (!std::strcmp(argv[i], "--focus") && i + 1 < argc) options.FocusWindow = argv[++i];
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
    return 0;
}
