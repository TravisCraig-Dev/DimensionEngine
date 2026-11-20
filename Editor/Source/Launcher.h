#pragma once
#include <Dimension/Core/Base.h>
#include <optional>
#include <string>

// The launch screen: what the editor shows when it starts without a project. Recent projects, the Sandbox sample, a
// new project, or any project folder. It runs as its own small application before the editor's (Launcher::Run), and
// the editor then opens what was picked.
namespace Editor::Launcher
{
    struct Choice
    {
        Dimension::fs::path Project;
        bool Create = false;      // make a new project there first (ProjectHub::Create)
        std::string Name;         // (new) its name
        std::string StarterScene; // (new) the scene template its first scene starts from
    };
    // Shows the launch screen until a project is picked (nothing: the window was closed). Automation: after
    // `maxFrames` frames (0: no limit), saving a screenshot when `screenshot` names a file.
    std::optional<Choice> Run(uint64_t maxFrames = 0, const std::string& screenshot = {});

    // Where new projects go by default: Documents/Dimension Projects.
    Dimension::fs::path DefaultProjectsFolder();
    // The Sandbox sample as shipped: the source tree's in a development build, else the install's Samples/Sandbox.
    // Empty when neither is there.
    Dimension::fs::path SandboxSample();
    // The Sandbox project to open: the sample itself in a development build; from an install (read-only, shared) a
    // copy made in DefaultProjectsFolder()/Sandbox the first time. Empty with `error` set when that fails.
    Dimension::fs::path SandboxProject(std::string* error = nullptr);
}
