#include "Dimension/Scene/SceneTemplates.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Scene/SceneSerializer.h"
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>

namespace Dimension::SceneTemplates
{
    namespace
    {
        Entity AddCamera(Scene& scene, const glm::vec3& position, const glm::vec3& degrees)
        {
            auto& reg = scene.GetRegistry();
            Entity cam = scene.CreateEntity("Main Camera");
            reg.Emplace<CameraComponent>(cam).Clear = ClearMode::Skybox;
            reg.Get<TransformComponent>(cam).Position = position;
            reg.Get<TransformComponent>(cam).Rotation = glm::radians(degrees);
            reg.Get<TagComponent>(cam).Tag = "MainCamera";
            return cam;
        }

        Entity AddSun(Scene& scene)
        {
            auto& reg = scene.GetRegistry();
            Entity light = scene.CreateEntity("Directional Light");
            reg.Emplace<DirectionalLightComponent>(light);
            reg.Get<TransformComponent>(light).Position = { 0.0f, 3.0f, 0.0f };
            reg.Get<TransformComponent>(light).Rotation = glm::radians(glm::vec3(-50.0f, -30.0f, 0.0f));
            return light;
        }

        Ref<Scene> Builtin(const std::string& name)
        {
            auto scene = MakeRef<Scene>("Untitled");
            auto& reg = scene->GetRegistry();
            if (name == "Basic 3D")
            {
                AddCamera(*scene, { 0.0f, 2.0f, 8.0f }, { -12.0f, 0.0f, 0.0f });
                AddSun(*scene);
            }
            else if (name == "Basic 2D")
            {
                Entity cam = AddCamera(*scene, { 0.0f, 0.0f, 10.0f }, glm::vec3(0.0f));
                auto& camera = reg.Get<CameraComponent>(cam);
                camera.Projection = ProjectionType::Orthographic;
                camera.Clear = ClearMode::SolidColor;
                camera.ClearColor = { 0.45f, 0.65f, 0.85f, 1.0f };
            }
            else if (name == "Outdoor")
            {
                AddCamera(*scene, { 0.0f, 3.0f, 12.0f }, { -10.0f, 0.0f, 0.0f });
                Entity sun = AddSun(*scene);
                auto& light = reg.Get<DirectionalLightComponent>(sun);
                light.Intensity = 2.0f;
                light.SoftShadows = true;
                Entity ground = scene->CreateEntity("Ground");
                auto& mr = reg.Emplace<MeshRendererComponent>(ground);
                mr.Mesh = AssetManager::kPlane;
                mr.Color = { 0.42f, 0.55f, 0.32f, 1.0f };
                mr.Smoothness = 0.2f;
                reg.Get<TransformComponent>(ground).Scale = { 10.0f, 1.0f, 10.0f };
                auto& s = scene->GetSettings();
                s.Fog = true;
                s.FogDensity = 0.003f;
                s.Tonemapping = true;
                s.Bloom = true;
            }
            scene->UpdateTransforms();
            return scene;
        }
    }

    std::vector<Template> All()
    {
        std::vector<Template> out = {
            { "Basic 3D", "A camera and a sun, under the procedural sky." },
            { "Basic 2D", "An orthographic camera looking down -Z at a plain sky-blue background, for sprites and tilemaps." },
            { "Outdoor", "A camera, a sun with soft shadows, a grass ground plane, fog, bloom and tonemapping." },
            { "Empty", "Nothing at all." },
        };
        for (AssetHandle h : AssetManager::Search(std::string("t:Scene l:") + kLabel)) // by path
        {
            const AssetMetadata* meta = AssetManager::GetMetadata(h);
            if (!meta) continue;
            std::string description = meta->ImportSettings.value("templateDescription", std::string());
            out.push_back({ meta->Path.stem().string(), description.empty() ? meta->Path.generic_string() : description, h });
        }
        return out;
    }

    Ref<Scene> Create(const Template& t)
    {
        if (!t.Scene.IsValid()) return Builtin(t.Name);
        std::ifstream in(AssetManager::GetAbsolutePath(t.Scene));
        if (!in) return nullptr;
        try
        {
            return SceneSerializer::DeserializeCopy(nlohmann::json::parse(in), "Untitled");
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }

    AssetHandle SaveAsTemplate(Scene& scene, const std::string& name, const std::string& description)
    {
        fs::path dir = AssetManager::GetAssetsDirectory() / "Scene Templates";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path path = dir / (name + ".scene");
        nlohmann::json data = SceneSerializer::Serialize(scene);
        data["name"] = name;
        std::ofstream(path) << data.dump(2);
        AssetHandle h = AssetManager::Import(path);
        if (!h.IsValid()) return h;
        std::vector<std::string> labels = AssetManager::GetLabels(h);
        if (std::find(labels.begin(), labels.end(), kLabel) == labels.end()) labels.push_back(kLabel);
        AssetManager::SetLabels(h, labels);
        if (!description.empty())
        {
            nlohmann::json settings = AssetManager::GetMetadata(h)->ImportSettings;
            settings["templateDescription"] = description;
            AssetManager::SetImportSettings(h, settings);
        }
        return h;
    }
}
