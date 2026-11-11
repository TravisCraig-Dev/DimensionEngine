#include "EditorActions.h"
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scene/SceneGrouping.h>
#include <Dimension/Core/Log.h>
#include <imgui.h>

namespace Editor
{
    Entity CreateObject(EditorContext& ctx, ObjectKind kind, Entity parent, const glm::vec3* position)
    {
        ctx.RecordUndo();
        Scene& scene = ctx.ActiveScene();
        auto& reg = scene.GetRegistry();
        static const char* names[] = { "GameObject", "Sprite", "Square", "Cube", "Sphere", "Plane", "Quad", "Cylinder",
                                       "Camera", "Directional Light", "Point Light", "Audio Source", "Physics Cube", "Physics Sphere",
                                       "Text", "Canvas", "Image", "Text", "Button", "Particle System",
                                       "Panel", "Slider", "Toggle", "Input Field", "Scroll View", "Spot Light", "Tilemap", "Line", "Trail", "Dropdown", "Reflection Probe",
                                       "Terrain" };

        // UI elements must live under a Canvas: use the selection if it is UI, else an existing canvas, else make one.
        bool isUI = kind == ObjectKind::UIImage || kind == ObjectKind::UIText || kind == ObjectKind::UIButton ||
                    kind == ObjectKind::UIPanel || kind == ObjectKind::UISlider || kind == ObjectKind::UIToggle ||
                    kind == ObjectKind::UIInputField || kind == ObjectKind::UIScrollView || kind == ObjectKind::UIDropdown;
        if (isUI)
        {
            auto& r = scene.GetRegistry();
            auto underCanvas = [&](Entity e) {
                for (Entity p = e; !p.IsNull(); p = scene.GetParent(p))
                    if (r.Has<CanvasComponent>(p)) return true;
                return false;
            };
            if (parent.IsNull() || !underCanvas(parent))
            {
                parent = Entity::Null();
                for (auto [ce, c] : r.ViewOf<CanvasComponent>())
                    if (parent.IsNull()) parent = ce; // the first canvas
                if (parent.IsNull())
                {
                    parent = scene.CreateEntity("Canvas");
                    r.Emplace<CanvasComponent>(parent);
                }
            }
        }
        Entity e = scene.CreateEntity(names[(int)kind], parent);
        auto uiRect = [&](glm::vec2 size) { reg.Emplace<UIRectComponent>(e).Size = size; };
        auto& t = reg.Get<TransformComponent>(e);
        if (position) t.Position = *position;

        auto mesh = [&](AssetHandle h) { reg.Emplace<MeshRendererComponent>(e).Mesh = h; };
        switch (kind)
        {
        case ObjectKind::Empty: break;
        case ObjectKind::Sprite: reg.Emplace<SpriteRendererComponent>(e); break;
        case ObjectKind::SpriteSquareBody:
            reg.Emplace<SpriteRendererComponent>(e);
            reg.Emplace<Rigidbody2DComponent>(e);
            reg.Emplace<BoxCollider2DComponent>(e);
            break;
        case ObjectKind::Cube: mesh(AssetManager::kCube); break;
        case ObjectKind::Sphere: mesh(AssetManager::kSphere); break;
        case ObjectKind::Plane: mesh(AssetManager::kPlane); break;
        case ObjectKind::Quad: mesh(AssetManager::kQuad); break;
        case ObjectKind::Cylinder: mesh(AssetManager::kCylinder); break;
        case ObjectKind::Camera:
            reg.Emplace<CameraComponent>(e).Clear = ClearMode::Skybox;
            if (!position) t.Position = { 0.0f, 1.0f, 10.0f };
            break;
        case ObjectKind::DirectionalLight:
            reg.Emplace<DirectionalLightComponent>(e);
            t.Rotation = glm::radians(glm::vec3(-50.0f, -30.0f, 0.0f));
            if (!position) t.Position = { 0.0f, 3.0f, 0.0f };
            break;
        case ObjectKind::PointLight:
            reg.Emplace<PointLightComponent>(e);
            if (!position) t.Position = { 0.0f, 2.0f, 0.0f };
            break;
        case ObjectKind::Tilemap: reg.Emplace<TilemapComponent>(e); break;
        case ObjectKind::LineRenderer: reg.Emplace<LineRendererComponent>(e); break;
        case ObjectKind::Terrain:
            mesh(AssetManager::kPlane);
            reg.Get<MeshRendererComponent>(e).Smoothness = 0.05f;
            reg.Emplace<TerrainComponent>(e);
            if (!position) t.Position = { 0.0f, 0.0f, 0.0f };
            break;
        case ObjectKind::ReflectionProbe:
            reg.Emplace<ReflectionProbeComponent>(e);
            if (!position) t.Position = { 0.0f, 1.5f, 0.0f };
            break;
        case ObjectKind::TrailRenderer: reg.Emplace<TrailRendererComponent>(e); break;
        case ObjectKind::UIDropdown:
            uiRect({ 320.0f, 50.0f });
            reg.Emplace<UIDropdownComponent>(e);
            break;
        case ObjectKind::SpotLight:
            reg.Emplace<SpotLightComponent>(e);
            t.Rotation = glm::radians(glm::vec3(-90.0f, 0.0f, 0.0f)); // pointing down
            if (!position) t.Position = { 0.0f, 4.0f, 0.0f };
            break;
        case ObjectKind::AudioSource: reg.Emplace<AudioSourceComponent>(e); break;
        case ObjectKind::PhysicsCube:
            mesh(AssetManager::kCube);
            reg.Emplace<RigidbodyComponent>(e);
            reg.Emplace<BoxColliderComponent>(e);
            break;
        case ObjectKind::Text3D: reg.Emplace<TextRendererComponent>(e); break;
        case ObjectKind::ParticleSystem: reg.Emplace<ParticleSystemComponent>(e); break;
        case ObjectKind::Canvas: reg.Emplace<CanvasComponent>(e); break;
        case ObjectKind::UIImage:
            uiRect({ 200.0f, 200.0f });
            reg.Emplace<UIImageComponent>(e);
            break;
        case ObjectKind::UIText:
            uiRect({ 320.0f, 60.0f });
            reg.Emplace<UITextComponent>(e).Color = { 1, 1, 1, 1 };
            break;
        case ObjectKind::UIButton:
        {
            uiRect({ 240.0f, 60.0f });
            reg.Emplace<UIImageComponent>(e);
            reg.Emplace<UIButtonComponent>(e);
            Entity label = scene.CreateEntity("Text", e);
            auto& lr = reg.Emplace<UIRectComponent>(label);
            lr.AnchorMin = { 0.0f, 0.0f };
            lr.AnchorMax = { 1.0f, 1.0f };
            lr.Size = { 0.0f, 0.0f };
            reg.Emplace<UITextComponent>(label).Text = "Button";
            break;
        }
        case ObjectKind::UIPanel:
        {
            uiRect({ 400.0f, 300.0f });
            auto& img = reg.Emplace<UIImageComponent>(e);
            img.Color = { 0.1f, 0.1f, 0.12f, 0.85f };
            reg.Emplace<UILayoutGroupComponent>(e);
            break;
        }
        case ObjectKind::UISlider: uiRect({ 320.0f, 32.0f }); reg.Emplace<UISliderComponent>(e); break;
        case ObjectKind::UIToggle: uiRect({ 240.0f, 36.0f }); reg.Emplace<UIToggleComponent>(e); break;
        case ObjectKind::UIInputField: uiRect({ 320.0f, 50.0f }); reg.Emplace<UIInputFieldComponent>(e); break;
        case ObjectKind::UIScrollView:
        {
            uiRect({ 400.0f, 300.0f });
            reg.Emplace<UIImageComponent>(e).Color = { 0.12f, 0.12f, 0.14f, 0.9f };
            reg.Emplace<UIScrollViewComponent>(e);
            Entity content = scene.CreateEntity("Content", e);
            auto& cr = reg.Emplace<UIRectComponent>(content);
            cr.AnchorMin = { 0.0f, 0.0f };
            cr.AnchorMax = { 1.0f, 1.0f };
            cr.Size = { 0.0f, 0.0f };
            reg.Emplace<UILayoutGroupComponent>(content);
            for (int i = 0; i < 8; ++i)
            {
                Entity item = scene.CreateEntity("Item " + std::to_string(i + 1), content);
                reg.Emplace<UIRectComponent>(item).Size = { 0.0f, 50.0f };
                auto& txt = reg.Emplace<UITextComponent>(item);
                txt.Text = "Item " + std::to_string(i + 1);
                txt.Color = { 1, 1, 1, 1 };
                txt.FontSize = 28.0f;
            }
            break;
        }
        case ObjectKind::PhysicsSphere:
            mesh(AssetManager::kSphere);
            reg.Emplace<RigidbodyComponent>(e);
            reg.Emplace<SphereColliderComponent>(e);
            break;
        }
        scene.UpdateTransforms();
        ctx.Select(e);
        return e;
    }

    void DrawCreateMenuItems(EditorContext& ctx, Entity parent)
    {
        if (ImGui::MenuItem("Create Empty")) CreateObject(ctx, ObjectKind::Empty, parent);
        if (ImGui::BeginMenu("2D Object"))
        {
            if (ImGui::MenuItem("Sprite")) CreateObject(ctx, ObjectKind::Sprite, parent);
            if (ImGui::MenuItem("Sprite (Physics Square)")) CreateObject(ctx, ObjectKind::SpriteSquareBody, parent);
            if (ImGui::MenuItem("Tilemap")) CreateObject(ctx, ObjectKind::Tilemap, parent);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("3D Object"))
        {
            if (ImGui::MenuItem("Cube")) CreateObject(ctx, ObjectKind::Cube, parent);
            if (ImGui::MenuItem("Sphere")) CreateObject(ctx, ObjectKind::Sphere, parent);
            if (ImGui::MenuItem("Cylinder")) CreateObject(ctx, ObjectKind::Cylinder, parent);
            if (ImGui::MenuItem("Plane")) CreateObject(ctx, ObjectKind::Plane, parent);
            if (ImGui::MenuItem("Quad")) CreateObject(ctx, ObjectKind::Quad, parent);
            if (ImGui::MenuItem("Terrain")) CreateObject(ctx, ObjectKind::Terrain, parent);
            ImGui::Separator();
            if (ImGui::MenuItem("Physics Cube")) CreateObject(ctx, ObjectKind::PhysicsCube, parent);
            if (ImGui::MenuItem("Physics Sphere")) CreateObject(ctx, ObjectKind::PhysicsSphere, parent);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Light"))
        {
            if (ImGui::MenuItem("Directional Light")) CreateObject(ctx, ObjectKind::DirectionalLight, parent);
            if (ImGui::MenuItem("Point Light")) CreateObject(ctx, ObjectKind::PointLight, parent);
            if (ImGui::MenuItem("Spot Light")) CreateObject(ctx, ObjectKind::SpotLight, parent);
            ImGui::Separator();
            if (ImGui::MenuItem("Reflection Probe")) CreateObject(ctx, ObjectKind::ReflectionProbe, parent);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Text (3D)")) CreateObject(ctx, ObjectKind::Text3D, parent);
        if (ImGui::BeginMenu("Effects"))
        {
            if (ImGui::MenuItem("Particle System")) CreateObject(ctx, ObjectKind::ParticleSystem, parent);
            if (ImGui::MenuItem("Line")) CreateObject(ctx, ObjectKind::LineRenderer, parent);
            if (ImGui::MenuItem("Trail")) CreateObject(ctx, ObjectKind::TrailRenderer, parent);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("UI"))
        {
            if (ImGui::MenuItem("Canvas")) CreateObject(ctx, ObjectKind::Canvas, Entity::Null());
            if (ImGui::MenuItem("Image")) CreateObject(ctx, ObjectKind::UIImage, parent);
            if (ImGui::MenuItem("Text")) CreateObject(ctx, ObjectKind::UIText, parent);
            if (ImGui::MenuItem("Button")) CreateObject(ctx, ObjectKind::UIButton, parent);
            if (ImGui::MenuItem("Slider")) CreateObject(ctx, ObjectKind::UISlider, parent);
            if (ImGui::MenuItem("Toggle")) CreateObject(ctx, ObjectKind::UIToggle, parent);
            if (ImGui::MenuItem("Input Field")) CreateObject(ctx, ObjectKind::UIInputField, parent);
            if (ImGui::MenuItem("Dropdown")) CreateObject(ctx, ObjectKind::UIDropdown, parent);
            ImGui::Separator();
            if (ImGui::MenuItem("Panel")) CreateObject(ctx, ObjectKind::UIPanel, parent);
            if (ImGui::MenuItem("Scroll View")) CreateObject(ctx, ObjectKind::UIScrollView, parent);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Audio Source")) CreateObject(ctx, ObjectKind::AudioSource, parent);
        if (ImGui::MenuItem("Camera")) CreateObject(ctx, ObjectKind::Camera, parent);
    }

    void DeleteSelected(EditorContext& ctx)
    {
        auto roots = ctx.SelectedRoots();
        if (roots.empty()) return;
        ctx.RecordUndo();
        for (Entity e : roots) ctx.ActiveScene().DestroyEntity(e);
        ctx.Select(Entity::Null());
    }

    void DuplicateSelected(EditorContext& ctx)
    {
        auto roots = ctx.SelectedRoots();
        if (roots.empty()) return;
        ctx.RecordUndo();
        Entity primary = ctx.Selected;
        std::vector<Entity> copies;
        Entity primaryCopy = Entity::Null();
        for (Entity e : roots)
        {
            Entity copy = ctx.ActiveScene().DuplicateEntity(e);
            copies.push_back(copy);
            if (e == primary) primaryCopy = copy;
        }
        ctx.Select(Entity::Null());
        for (Entity c : copies) ctx.AddToSelection(c);
        if (!primaryCopy.IsNull()) ctx.AddToSelection(primaryCopy); // keep it primary
    }

    namespace
    {
        // Inside a prefab instance (not its root): its structure belongs to the prefab.
        bool InsidePrefabInstance(Scene& scene, Entity e)
        {
            Entity root = Prefabs::FindInstanceRoot(scene, e);
            return !root.IsNull() && root != e;
        }
    }

    void GroupSelected(EditorContext& ctx)
    {
        Scene& scene = ctx.ActiveScene();
        auto roots = ctx.SelectedRoots();
        if (roots.empty()) return;
        for (Entity e : roots)
            if (InsidePrefabInstance(scene, e))
            {
                Log::Warn("Objects inside a prefab instance cannot be grouped (open the prefab to change it)");
                return;
            }
        ctx.RecordUndo("Group");
        Entity group = SceneGrouping::Group(scene, roots);
        if (!group.IsNull()) ctx.Select(group);
    }

    void UngroupSelected(EditorContext& ctx)
    {
        Scene& scene = ctx.ActiveScene();
        std::vector<Entity> groups;
        for (Entity e : ctx.SelectedRoots())
            if (!scene.GetChildren(e).empty() && Prefabs::FindInstanceRoot(scene, e).IsNull()) groups.push_back(e);
        if (groups.empty())
        {
            Log::Info("Select an object with children (not part of a prefab instance) to ungroup it");
            return;
        }
        ctx.RecordUndo("Ungroup");
        std::vector<Entity> freed;
        for (Entity g : groups)
            for (Entity c : SceneGrouping::Ungroup(scene, g)) freed.push_back(c);
        ctx.Select(Entity::Null());
        for (Entity c : freed) ctx.AddToSelection(c);
    }

    void SortChildren(EditorContext& ctx, Entity parent, bool recursive)
    {
        Scene& scene = ctx.ActiveScene();
        if (!parent.IsNull() && !Prefabs::FindInstanceRoot(scene, parent).IsNull())
        {
            Log::Warn("A prefab instance's children keep the prefab's order (open the prefab to sort them)");
            return;
        }
        ctx.RecordUndo(recursive ? "Sort Children (All Levels)" : "Sort Children");
        SceneGrouping::SortChildren(scene, parent, recursive);
    }

    void CopySelected(EditorContext& ctx)
    {
        auto roots = ctx.SelectedRoots();
        if (roots.empty()) return;
        nlohmann::json clip;
        clip["dimensionClipboard"] = 2;
        clip["trees"] = nlohmann::json::array();
        for (Entity e : roots) clip["trees"].push_back(SceneSerializer::SerializeEntityTree(ctx.ActiveScene(), e));
        ImGui::SetClipboardText(clip.dump().c_str());
    }

    void PasteClipboard(EditorContext& ctx)
    {
        if (ctx.IsPlaying()) return;
        const char* text = ImGui::GetClipboardText();
        if (!text) return;
        nlohmann::json tree = nlohmann::json::parse(text, nullptr, false);
        if (tree.is_discarded() || !tree.is_object() || !tree.contains("dimensionClipboard")) return;
        Scene& scene = ctx.ActiveScene();
        Entity parent = ctx.HasValidSelection() ? scene.GetParent(ctx.Selected) : Entity::Null();
        ctx.RecordUndo();
        std::vector<nlohmann::json> trees;
        if (tree.contains("trees")) for (auto& t : tree["trees"]) trees.push_back(t);
        else trees.push_back(tree); // single-tree format from older clipboards
        ctx.Select(Entity::Null());
        for (auto& t : trees)
        {
            Entity pasted = SceneSerializer::DeserializeEntityTree(scene, t, parent, true);
            if (!pasted.IsNull()) ctx.AddToSelection(pasted);
        }
    }

    void CreatePrefab(EditorContext& ctx, Entity e, const fs::path& folder)
    {
        Scene& scene = ctx.ActiveScene();
        if (!scene.IsValid(e)) return;
        std::string name = scene.GetRegistry().Get<TagComponent>(e).Name;
        fs::path path = folder / (name + ".prefab");
        for (int i = 1; fs::exists(path); ++i) path = folder / (name + " " + std::to_string(i) + ".prefab");
        ctx.RecordUndo();
        Prefabs::CreateFromEntity(scene, e, path);
    }
}
