#pragma once
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Scene/Scene.h"
#include <string>
#include <vector>

namespace Dimension
{
    // What new scenes start from (File > New Scene, like Unity's scene templates): the built-in templates, and the
    // project's own (scene assets labelled "Template"; File > Save as Scene Template... makes one). A scene made from a
    // template is a copy: its objects get new GUIDs (references between them follow), so it shares nothing with it.
    namespace SceneTemplates
    {
        constexpr const char* kLabel = "Template";

        struct Template
        {
            std::string Name, Description;
            AssetHandle Scene = AssetHandle::None(); // a project template's scene (None: built-in)
        };
        // The built-in templates (Basic 3D first), then the project's by path.
        std::vector<Template> All();
        // A new scene from a template ("Untitled"); null when its file cannot be read.
        Ref<Scene> Create(const Template& t);
        // Saves a copy of `scene` as a project template: Assets/Scene Templates/<name>.scene, labelled Template.
        AssetHandle SaveAsTemplate(Scene& scene, const std::string& name, const std::string& description = "");
    }
}
