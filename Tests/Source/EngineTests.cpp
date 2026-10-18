// Headless engine tests (no GPU/window needed). Run: ctest --preset debug, or DimensionTests.exe
#include <Dimension/Dimension.h>
#include <Dimension/Scene/ComponentRegistry.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/UI/UI.h>
#include <Dimension/Animation/Animation.h>
#include <Dimension/Animation/AnimatorController.h>
#include <Dimension/Renderer/Mesh.h>
#include <Dimension/Renderer/Environment.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scripting/CSharpRuntime.h>
#include <windows.h>

namespace
{
    std::filesystem::path ExecutableDir()
    {
        wchar_t buffer[MAX_PATH];
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::filesystem::path(buffer).parent_path();
    }
}
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace Dimension;

namespace
{
    int g_Failures = 0, g_Checks = 0;
    const char* g_Current = "";

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++g_Checks;                                                                                  \
        if (!(cond)) { ++g_Failures; std::printf("  FAIL [%s] %s:%d: %s\n", g_Current, __FILE__, __LINE__, #cond); } \
    } while (0)

    bool Near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
    bool Near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) { return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(eps))); }

    struct Position { float X = 0, Y = 0; };
    struct Velocity { float X = 0, Y = 0; };

    void TestRegistryBasics()
    {
        Registry r;
        Entity a = r.Create(), b = r.Create(), c = r.Create();
        CHECK(r.Valid(a) && r.Valid(b) && r.Valid(c));
        CHECK(r.AliveCount() == 3);
        r.Emplace<Position>(a, 1.0f, 2.0f);
        r.Emplace<Position>(b);
        r.Emplace<Velocity>(b, 3.0f, 4.0f);
        CHECK(r.Has<Position>(a) && !r.Has<Velocity>(a));
        CHECK(r.Get<Position>(a).Y == 2.0f);
        CHECK(r.TryGet<Velocity>(c) == nullptr);

        r.Destroy(a);
        CHECK(!r.Valid(a));
        CHECK(!r.Has<Position>(a));
        Entity d = r.Create(); // reuses a's slot with a new generation
        CHECK(d.Index == a.Index && d.Generation != a.Generation);
        CHECK(!r.Valid(a) && r.Valid(d));
        CHECK(!r.Has<Position>(d));

        r.Remove<Velocity>(b);
        CHECK(!r.Has<Velocity>(b) && r.Has<Position>(b));
    }

    void TestViews()
    {
        Registry r;
        std::vector<Entity> es;
        for (int i = 0; i < 100; ++i)
        {
            Entity e = r.Create();
            es.push_back(e);
            r.Emplace<Position>(e, (float)i, 0.0f);
            if (i % 3 == 0) r.Emplace<Velocity>(e, 1.0f, 0.0f);
        }
        int count = 0;
        float sum = 0;
        for (auto [e, p, v] : r.ViewOf<Position, Velocity>())
        {
            ++count;
            sum += p.X;
            p.X += v.X;
        }
        CHECK(count == 34);
        CHECK(sum == 1683.0f); // 0+3+...+99
        CHECK(r.Get<Position>(es[3]).X == 4.0f);

        // Each() tolerates removing the current entity's components.
        int visited = 0;
        r.ViewOf<Velocity>().Each([&](Entity e, Velocity&) {
            ++visited;
            r.Remove<Velocity>(e);
        });
        CHECK(visited == 34);
        CHECK(r.Pool<Velocity>().Size() == 0);
    }

    void TestClonePreservesHandles()
    {
        Registry r;
        Entity a = r.Create(), b = r.Create();
        r.Destroy(a);
        Entity c = r.Create();
        r.Emplace<Position>(b, 5.0f, 6.0f);
        r.Emplace<Position>(c, 7.0f, 8.0f);
        Registry copy = r.Clone();
        CHECK(copy.Valid(b) && copy.Valid(c) && !copy.Valid(a));
        CHECK(copy.Get<Position>(c).X == 7.0f);
        copy.Get<Position>(b).X = 100.0f;
        CHECK(r.Get<Position>(b).X == 5.0f); // deep copy
    }

    void TestHierarchy()
    {
        Scene scene("Test");
        auto& reg = scene.GetRegistry();
        Entity parent = scene.CreateEntity("Parent");
        Entity child = scene.CreateEntity("Child");
        reg.Get<TransformComponent>(parent).Position = { 10, 0, 0 };
        reg.Get<TransformComponent>(parent).Scale = { 2, 2, 2 };
        reg.Get<TransformComponent>(child).Position = { 12, 4, 0 };
        scene.UpdateTransforms();

        scene.SetParent(child, parent, true);
        CHECK(scene.GetParent(child) == parent);
        CHECK(scene.GetRootEntities().size() == 1);
        CHECK(Near(reg.Get<TransformComponent>(child).GetWorldPosition(), { 12, 4, 0 }));
        CHECK(Near(reg.Get<TransformComponent>(child).Position, { 1, 2, 0 }));

        CHECK(!scene.IsDescendantOf(parent, child));
        scene.SetParent(parent, child); // cycle: rejected
        CHECK(scene.GetParent(parent).IsNull());

        reg.Get<TagComponent>(parent).Active = false;
        scene.UpdateTransforms();
        CHECK(!reg.Get<TransformComponent>(child).ActiveInHierarchy);

        Entity other = scene.CreateEntity("Other");
        scene.SetSiblingIndex(other, 0);
        CHECK(scene.GetRootEntities().front() == other);

        Guid childGuid = scene.GetGuid(child);
        CHECK(scene.FindEntityByGuid(childGuid) == child);
        scene.DestroyEntity(parent);
        CHECK(!scene.IsValid(child) && !scene.IsValid(parent));
        CHECK(scene.FindEntityByGuid(childGuid).IsNull());
        CHECK(scene.GetEntityCount() == 1);
    }

    class TestScript : public ScriptableEntity
    {
    public:
        float Speed = 1.0f;
        Entity Target;
        int Updates = 0;
        static inline int Created = 0, Destroyed = 0;
        void OnCreate() override { ++Created; }
        void OnUpdate(float) override { ++Updates; Transform().Position.x += Speed; }
        void OnDestroy() override { ++Destroyed; }
    };

    void TestSerializationRoundTrip()
    {
        Scene scene("RoundTrip");
        auto& reg = scene.GetRegistry();
        Entity a = scene.CreateEntity("A");
        Entity b = scene.CreateEntity("B", a);
        Entity c = scene.CreateEntity("C", a);
        reg.Get<TransformComponent>(b).Position = { 1, 2, 3 };
        reg.Get<TransformComponent>(b).Rotation = { 0.1f, 0.2f, 0.3f };
        auto& cam = reg.Emplace<CameraComponent>(a);
        cam.Projection = ProjectionType::Orthographic;
        cam.OrthographicSize = 7.5f;
        reg.Emplace<MeshRendererComponent>(b).Mesh = AssetManager::kSphere;
        auto& sc = reg.Emplace<ScriptComponent>(c).Add("TestScript");
        sc.Fields["Speed"] = 2.5f;
        sc.Fields["Target"] = (uint64_t)scene.GetGuid(b);
        scene.GetSettings().AmbientIntensity = 0.75f;
        scene.GetSettings().Bloom = true;
        scene.GetSettings().BloomThreshold = 2.5f;
        scene.GetSettings().FXAA = true;
        scene.GetSettings().Vignette = 0.3f;

        auto json = SceneSerializer::Serialize(scene);
        auto loaded = SceneSerializer::Deserialize(json);
        CHECK(loaded->GetEntityCount() == 3);
        Entity la = loaded->FindEntityByGuid(scene.GetGuid(a));
        Entity lb = loaded->FindEntityByGuid(scene.GetGuid(b));
        Entity lc = loaded->FindEntityByGuid(scene.GetGuid(c));
        CHECK(!la.IsNull() && !lb.IsNull() && !lc.IsNull());
        auto& lreg = loaded->GetRegistry();
        CHECK(loaded->GetParent(lb) == la);
        CHECK(loaded->GetChildren(la).size() == 2 && loaded->GetChildren(la)[0] == lb); // order preserved
        CHECK(Near(lreg.Get<TransformComponent>(lb).Position, { 1, 2, 3 }));
        CHECK(Near(lreg.Get<TransformComponent>(lb).Rotation, { 0.1f, 0.2f, 0.3f }));
        CHECK(lreg.Get<CameraComponent>(la).Projection == ProjectionType::Orthographic);
        CHECK(Near(lreg.Get<CameraComponent>(la).OrthographicSize, 7.5f));
        CHECK(lreg.Get<MeshRendererComponent>(lb).Mesh == AssetManager::kSphere);
        CHECK(lreg.Get<ScriptComponent>(lc).Scripts.size() == 1 && lreg.Get<ScriptComponent>(lc).Scripts[0].ClassName == "TestScript");
        CHECK(Near(loaded->GetSettings().AmbientIntensity, 0.75f));
        CHECK(loaded->GetSettings().Bloom && loaded->GetSettings().FXAA);
        CHECK(Near(loaded->GetSettings().BloomThreshold, 2.5f) && Near(loaded->GetSettings().Vignette, 0.3f));
        CHECK(SceneSerializer::Serialize(*loaded) == json); // stable
    }

    void TestDuplicateRemapsReferences()
    {
        Scene scene("Dup");
        auto& reg = scene.GetRegistry();
        Entity root = scene.CreateEntity("Root");
        Entity kid = scene.CreateEntity("Kid", root);
        auto& sc = reg.Emplace<ScriptComponent>(root).Add("TestScript");
        sc.Fields["Target"] = (uint64_t)scene.GetGuid(kid);

        Entity copy = scene.DuplicateEntity(root);
        CHECK(scene.IsValid(copy) && copy != root);
        CHECK(scene.GetGuid(copy) != scene.GetGuid(root));
        CHECK(scene.GetChildren(copy).size() == 1);
        Entity copyKid = scene.GetChildren(copy)[0];
        CHECK(copyKid != kid);
        // Script field stays as JSON; check the generic EntityRef remap through a reflected component instead.
        CHECK(scene.GetRootEntities().size() == 2 && scene.GetRootEntities()[1] == copy);
    }

    void TestScriptsRuntime()
    {
        Scene scene("Scripts");
        auto& reg = scene.GetRegistry();
        Entity target = scene.CreateEntity("Target");
        Entity e = scene.CreateEntity("Scripted");
        auto& sc = reg.Emplace<ScriptComponent>(e).Add("TestScript");
        sc.Fields["Speed"] = 3.0f;
        sc.Fields["Target"] = (uint64_t)scene.GetGuid(target);

        TestScript::Created = TestScript::Destroyed = 0;
        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        CHECK(TestScript::Created == 1);
        auto* inst = static_cast<TestScript*>(runtime->GetRegistry().Get<ScriptComponent>(e).Scripts[0].Instance.get());
        CHECK(inst && Near(inst->Speed, 3.0f));
        CHECK(inst && inst->Target == target);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(inst->Updates == 2);
        CHECK(Near(runtime->GetRegistry().Get<TransformComponent>(e).Position.x, 6.0f));
        CHECK(Near(reg.Get<TransformComponent>(e).Position.x, 0.0f)); // edit scene untouched

        runtime->SetPaused(true);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(inst->Updates == 2);
        runtime->Step(1);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(inst->Updates == 3);

        runtime->DestroyEntityDeferred(e);
        runtime->SetPaused(false);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(!runtime->IsValid(e));
        CHECK(TestScript::Destroyed == 1);
        runtime->OnRuntimeStop();
    }

    void TestPhysics2D()
    {
        Scene scene("Physics2D");
        auto& reg = scene.GetRegistry();
        Entity ground = scene.CreateEntity("Ground");
        reg.Get<TransformComponent>(ground).Scale = { 20, 1, 1 };
        reg.Emplace<BoxCollider2DComponent>(ground);
        Entity box = scene.CreateEntity("Box");
        reg.Get<TransformComponent>(box).Position = { 0, 5, 0 };
        reg.Emplace<Rigidbody2DComponent>(box);
        reg.Emplace<BoxCollider2DComponent>(box);

        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        for (int i = 0; i < 240; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
        float y = runtime->GetRegistry().Get<TransformComponent>(box).Position.y;
        CHECK(y < 1.1f && y > 0.9f); // resting on top of the ground (0.5 + 0.5)
        auto hit = Physics2DWorld::Raycast(*runtime, { 0.0f, 10.0f }, { 0.0f, -1.0f }, 100.0f);
        CHECK(hit && hit.Hit == box);
        CHECK(Near(hit.Point.y, 1.5f, 0.05f) && Near(hit.Normal.y, 1.0f));
        auto miss = Physics2DWorld::Raycast(*runtime, { 50.0f, 10.0f }, { 0.0f, -1.0f }, 5.0f);
        CHECK(!miss);
        runtime->OnRuntimeStop();
    }

    void TestPhysics3D()
    {
        Scene scene("Physics3D");
        auto& reg = scene.GetRegistry();
        Entity ground = scene.CreateEntity("Ground");
        reg.Get<TransformComponent>(ground).Scale = { 20, 1, 20 };
        reg.Emplace<BoxColliderComponent>(ground);
        Entity ball = scene.CreateEntity("Ball");
        reg.Get<TransformComponent>(ball).Position = { 0, 6, 0 };
        reg.Emplace<RigidbodyComponent>(ball);
        reg.Emplace<SphereColliderComponent>(ball);
        Entity capsule = scene.CreateEntity("Capsule");
        reg.Get<TransformComponent>(capsule).Position = { 5, 4, 0 };
        reg.Emplace<RigidbodyComponent>(capsule).AngularDamping = 50.0f;
        reg.Emplace<CapsuleColliderComponent>(capsule);

        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        for (int i = 0; i < 300; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
        float y = runtime->GetRegistry().Get<TransformComponent>(ball).Position.y;
        CHECK(y < 1.1f && y > 0.9f); // sphere radius 0.5 on a box whose top is at 0.5
        float cy = runtime->GetRegistry().Get<TransformComponent>(capsule).Position.y;
        CHECK(cy > 0.4f && cy < 1.6f); // capsule (height 2) came to rest on the ground
        auto hit = Physics3DWorld::Raycast(*runtime, { 0, 10, 0 }, { 0, -1, 0 }, 100.0f);
        CHECK(hit && hit.Hit == ball);
        CHECK(Near(hit.Point.y, 1.5f, 0.05f) && Near(hit.Normal.y, 1.0f, 0.01f));
        CHECK(!Physics3DWorld::Raycast(*runtime, { 0, 10, 0 }, { 0, 1, 0 }, 100.0f));
        runtime->OnRuntimeStop();
    }

    void TestReflectionJson()
    {
        const ComponentInfo* info = ComponentRegistry::Find<PointLightComponent>();
        CHECK(info != nullptr);
        PointLightComponent light;
        light.Range = 42.0f;
        light.Color = { 0.1f, 0.2f, 0.3f };
        nlohmann::json j;
        for (auto& f : info->Fields) j[f.Name] = FieldToJson(f, &light, nullptr);
        PointLightComponent other;
        for (auto& f : info->Fields) FieldFromJson(f, &other, j[f.Name], nullptr);
        CHECK(Near(other.Range, 42.0f));
        CHECK(Near(other.Color, { 0.1f, 0.2f, 0.3f }));
    }

    void TestMaterialRoundTrip()
    {
        Material m;
        m.BaseColor = { 0.1f, 0.2f, 0.3f, 0.4f };
        m.Metallic = 0.75f;
        m.Roughness = 0.2f;
        m.NormalMap = Guid(uint64_t(123456));
        m.BlendMode = MaterialBlendMode::Transparent;
        m.DoubleSided = true;
        fs::path path = fs::temp_directory_path() / "dimension_test.mat";
        CHECK(m.Save(path));
        auto loaded = Material::Load(path);
        CHECK(loaded != nullptr);
        if (loaded)
        {
            CHECK(Near(loaded->BaseColor.a, 0.4f) && Near(loaded->Metallic, 0.75f) && Near(loaded->Roughness, 0.2f));
            CHECK((uint64_t)loaded->NormalMap == 123456);
            CHECK(loaded->BlendMode == MaterialBlendMode::Transparent && loaded->DoubleSided);
        }
        fs::remove(path);
    }

    class ClickCounter : public ScriptableEntity
    {
    public:
        static inline int Clicks = 0;
        void OnClick() override { ++Clicks; }
    };

    void TestUI()
    {
        Scene scene("UI");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        auto& c = reg.Emplace<CanvasComponent>(canvas);
        c.ReferenceResolution = { 1000, 500 };
        Entity button = scene.CreateEntity("Button", canvas);
        auto& r = reg.Emplace<UIRectComponent>(button);
        r.AnchorMin = r.AnchorMax = { 1.0f, 1.0f }; // bottom-right corner
        r.Pivot = { 1.0f, 1.0f };
        r.Position = { -10.0f, -10.0f };
        r.Size = { 100.0f, 50.0f };
        reg.Emplace<UIImageComponent>(button);
        reg.Emplace<UIButtonComponent>(button);
        reg.Emplace<ScriptComponent>(button).Add("ClickCounter");
        Entity fill = scene.CreateEntity("Fill", button);
        auto& fr = reg.Emplace<UIRectComponent>(fill);
        fr.AnchorMin = { 0, 0 };
        fr.AnchorMax = { 1, 1 };
        fr.Size = { -20.0f, -20.0f }; // 10 units inset on each side
        scene.UpdateTransforms();

        // Screen is twice the reference resolution -> scale 2.
        auto order = UI::ComputeLayout(scene, { 2000, 1000 });
        CHECK(order.size() == 2 && order[0] == button);
        auto& rb = reg.Get<UIRectComponent>(button); // re-fetch: adding Fill's rect reallocated the pool
        auto& rf = reg.Get<UIRectComponent>(fill);
        CHECK(Near(rb.ComputedMax.x, 1980.0f) && Near(rb.ComputedMax.y, 980.0f));
        CHECK(Near(rb.ComputedMin.x, 1780.0f) && Near(rb.ComputedMin.y, 880.0f));
        CHECK(Near(rf.ComputedMin.x, 1800.0f) && Near(rf.ComputedMax.y, 960.0f));

        ClickCounter::Clicks = 0;
        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        glm::vec2 inside(1900, 930), outside(10, 10);
        runtime->SetUIInput(inside, false, { 2000, 1000 });
        runtime->OnUpdateRuntime(0.016f);
        CHECK(runtime->IsPointerOverUI());
        runtime->SetUIInput(inside, true, { 2000, 1000 });
        runtime->OnUpdateRuntime(0.016f);
        CHECK(runtime->GetRegistry().Get<UIButtonComponent>(button).Pressed);
        runtime->SetUIInput(inside, false, { 2000, 1000 });
        runtime->OnUpdateRuntime(0.016f);
        CHECK(ClickCounter::Clicks == 1);
        // Press inside, release outside: no click (like Unity).
        runtime->SetUIInput(inside, true, { 2000, 1000 });
        runtime->OnUpdateRuntime(0.016f);
        runtime->SetUIInput(outside, false, { 2000, 1000 });
        runtime->OnUpdateRuntime(0.016f);
        CHECK(ClickCounter::Clicks == 1);
        CHECK(!runtime->IsPointerOverUI());
        runtime->OnRuntimeStop();
    }

    class WidgetListener : public ScriptableEntity
    {
    public:
        static inline float LastValue = -1.0f;
        static inline int ValueEvents = 0;
        static inline std::string LastText, Submitted;
        void OnUIValueChanged(float v) override { LastValue = v; ++ValueEvents; }
        void OnUITextChanged(const std::string& t) override { LastText = t; }
        void OnUISubmit(const std::string& t) override { Submitted = t; }
    };

    void TestUIWidgets()
    {
        Scene scene("Widgets");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        reg.Emplace<CanvasComponent>(canvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;

        // Vertical layout panel at the top-left: 300 x 400, padding 10, spacing 5.
        Entity panel = scene.CreateEntity("Panel", canvas);
        {
            auto& r = reg.Emplace<UIRectComponent>(panel);
            r.AnchorMin = r.AnchorMax = { 0, 0 };
            r.Pivot = { 0, 0 };
            r.Size = { 300, 400 };
            auto& g = reg.Emplace<UILayoutGroupComponent>(panel);
            g.Padding = glm::vec4(10.0f);
            g.Spacing = 5.0f;
        }
        auto child = [&](const char* name, glm::vec2 size) {
            Entity e = scene.CreateEntity(name, panel);
            reg.Emplace<UIRectComponent>(e).Size = size;
            reg.Emplace<ScriptComponent>(e).Add("WidgetListener");
            return e;
        };
        Entity slider = child("Slider", { 0, 20 });
        Entity toggle = child("Toggle", { 0, 30 });
        Entity field = child("Field", { 0, 40 });
        {
            auto& s = reg.Emplace<UISliderComponent>(slider);
            s.MinValue = 0; s.MaxValue = 10; s.Value = 0; s.WholeNumbers = true;
            reg.Emplace<UIToggleComponent>(toggle);
            reg.Emplace<UIInputFieldComponent>(field).CharacterLimit = 5;
        }

        // Scroll view to the right: 200 x 100 view with 6 items of 50 -> content 300 high, max scroll 200.
        Entity view = scene.CreateEntity("View", canvas);
        {
            auto& r = reg.Emplace<UIRectComponent>(view);
            r.AnchorMin = r.AnchorMax = { 0, 0 };
            r.Pivot = { 0, 0 };
            r.Position = { 400, 0 };
            r.Size = { 200, 100 };
            reg.Emplace<UIScrollViewComponent>(view).ScrollSpeed = 30.0f;
        }
        Entity content = scene.CreateEntity("Content", view);
        {
            auto& r = reg.Emplace<UIRectComponent>(content);
            r.AnchorMin = { 0, 0 }; r.AnchorMax = { 1, 1 }; r.Size = { 0, 0 };
            auto& g = reg.Emplace<UILayoutGroupComponent>(content);
            g.Padding = glm::vec4(0.0f);
            g.Spacing = 0.0f;
        }
        Entity items[6];
        for (int i = 0; i < 6; ++i)
        {
            items[i] = scene.CreateEntity("Item", content);
            reg.Emplace<UIRectComponent>(items[i]).Size = { 0, 50 };
            reg.Emplace<UIImageComponent>(items[i]);
        }
        scene.UpdateTransforms();

        glm::vec2 screen(1000, 1000);
        UI::ComputeLayout(scene, screen);
        auto R = [&](Entity e) -> UIRectComponent& { return reg.Get<UIRectComponent>(e); };
        CHECK(Near(R(slider).ComputedMin.x, 10) && Near(R(slider).ComputedMin.y, 10) && Near(R(slider).ComputedMax.x, 290));
        CHECK(Near(R(toggle).ComputedMin.y, 35) && Near(R(toggle).ComputedMax.y, 65));
        CHECK(Near(R(field).ComputedMin.y, 70) && Near(R(field).ComputedMax.y, 110));
        CHECK(Near(R(content).ComputedMax.y - R(content).ComputedMin.y, 300)); // grew to fit its children
        CHECK(Near(R(items[5]).ClipMax.y, 100)); // clipped to the view
        CHECK(Near(R(items[2]).ComputedMin.y, 100));

        auto runtime = scene.Clone();
        auto& rr = runtime->GetRegistry();
        runtime->OnRuntimeStart();
        auto step = [&](glm::vec2 p, bool down, float scroll = 0.0f) {
            runtime->SetUIInput(p, down, screen, true, scroll);
            runtime->OnUpdateRuntime(0.016f);
        };

        // Items below the view are not hit: clipped.
        step({ 500, 150 }, false);
        CHECK(!runtime->IsPointerOverUI());
        // Scroll down 2 notches (60) then far past the end: clamps at 200.
        step({ 500, 50 }, false, -2.0f);
        UI::ComputeLayout(*runtime, screen);
        CHECK(Near(rr.Get<UIScrollViewComponent>(view).ScrollPosition, 60));
        CHECK(Near(rr.Get<UIRectComponent>(items[2]).ComputedMin.y, 40));
        step({ 500, 50 }, false, -100.0f);
        UI::ComputeLayout(*runtime, screen);
        CHECK(Near(rr.Get<UIScrollViewComponent>(view).ScrollPosition, 200));
        CHECK(Near(rr.Get<UIRectComponent>(items[5]).ComputedMax.y, 100));

        // Slider: track spans x 20..280 (handle radius 10). Press at the middle, drag to the end.
        WidgetListener::ValueEvents = 0;
        step({ 150, 20 }, false);
        step({ 150, 20 }, true);
        CHECK(Near(rr.Get<UISliderComponent>(slider).Value, 5) && Near(WidgetListener::LastValue, 5));
        step({ 900, 300 }, true); // dragging continues outside the rect
        CHECK(Near(rr.Get<UISliderComponent>(slider).Value, 10));
        step({ 900, 300 }, false);
        step({ 20, 20 }, false); // released: no longer dragging
        CHECK(Near(rr.Get<UISliderComponent>(slider).Value, 10) && WidgetListener::ValueEvents == 2);

        // Toggle: click flips IsOn and reports 1.
        step({ 100, 50 }, true);
        step({ 100, 50 }, false);
        CHECK(rr.Get<UIToggleComponent>(toggle).IsOn && Near(WidgetListener::LastValue, 1));

        // Input field: focus, type past the limit, backspace, submit.
        step({ 100, 90 }, true);
        step({ 100, 90 }, false);
        CHECK(rr.Get<UIInputFieldComponent>(field).Focused);
        runtime->InjectUIKeyboard("Hello world");
        step({ 100, 90 }, false);
        CHECK(rr.Get<UIInputFieldComponent>(field).Text == "Hello" && WidgetListener::LastText == "Hello");
        runtime->InjectUIKeyboard("", true);
        step({ 100, 90 }, false);
        CHECK(rr.Get<UIInputFieldComponent>(field).Text == "Hell");
        runtime->InjectUIKeyboard("", false, true);
        step({ 100, 90 }, false);
        CHECK(WidgetListener::Submitted == "Hell" && !rr.Get<UIInputFieldComponent>(field).Focused);
        // Unfocused: typing is ignored.
        runtime->InjectUIKeyboard("x");
        step({ 100, 90 }, false);
        CHECK(rr.Get<UIInputFieldComponent>(field).Text == "Hell");
        runtime->OnRuntimeStop();

        // Widgets serialize through the reflection registry.
        CHECK(ComponentRegistry::Find("UI Slider") && ComponentRegistry::Find("UI Scroll View") && ComponentRegistry::Find("UI Layout Group"));
        CHECK(UI::CodepointCount("h\xC3\xA9llo") == 5);
    }

    void TestCascadeSplits()
    {
        auto u = ComputeCascadeSplits(1.0f, 101.0f, 4, 0.0f); // uniform
        CHECK(Near(u[0], 26.0f) && Near(u[1], 51.0f) && Near(u[2], 76.0f) && Near(u[3], 101.0f));
        auto l = ComputeCascadeSplits(1.0f, 100.0f, 2, 1.0f); // logarithmic: geometric mean
        CHECK(Near(l[0], 10.0f, 0.01f) && Near(l[1], 100.0f) && Near(l[3], 100.0f));
        auto m = ComputeCascadeSplits(0.1f, 40.0f, 4, 0.75f);
        CHECK(m[0] > 0.1f && m[0] < m[1] && m[1] < m[2] && m[2] < m[3] && Near(m[3], 40.0f));
        CHECK(m[0] < 10.0f); // practical splits keep the first cascade small for sharp nearby shadows
        auto one = ComputeCascadeSplits(0.1f, 40.0f, 1, 0.75f);
        CHECK(Near(one[0], 40.0f));
    }

    void TestPointShadowFaces()
    {
        glm::vec3 light(1, 2, 3);
        const float n = 0.05f, f = 20.0f;
        const glm::vec3 dirs[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (int face = 0; face < 6; ++face)
        {
            glm::mat4 vp = PointShadowFaceViewProj(light, face, n, f);
            glm::vec4 c = vp * glm::vec4(light + dirs[face] * 4.0f, 1.0f);
            glm::vec3 ndc = glm::vec3(c) / c.w;
            CHECK(Near(ndc.x, 0.0f) && Near(ndc.y, 0.0f) && ndc.z > 0.0f && ndc.z < 1.0f);
            CHECK(Near(n * f / (f - ndc.z * (f - n)), 4.0f, 1e-3f)); // the shader's depth linearization
            // A point 45 degrees off-axis lands on the face edge.
            glm::vec3 side = dirs[(face + 2) % 6];
            glm::vec4 e = vp * glm::vec4(light + dirs[face] * 2.0f + side * 1.99f, 1.0f);
            float edge = std::max(std::abs(e.x / e.w), std::abs(e.y / e.w));
            CHECK(edge > 0.98f && edge <= 1.0f);
        }

        // Spot lights: the cone axis maps to the tile center and the outer cone edge stays inside the tile.
        glm::vec3 dir = glm::normalize(glm::vec3(0.3f, -1.0f, -0.2f));
        glm::mat4 svp = SpotShadowViewProj(light, dir, 60.0f, n, f);
        glm::vec4 c = svp * glm::vec4(light + dir * 5.0f, 1.0f);
        glm::vec3 ndc = glm::vec3(c) / c.w;
        CHECK(Near(ndc.x, 0.0f) && Near(ndc.y, 0.0f) && Near(n * f / (f - ndc.z * (f - n)), 5.0f, 1e-3f));
        glm::vec3 perp = glm::normalize(glm::cross(dir, glm::vec3(0, 0, 1)));
        glm::vec3 rim = glm::normalize(dir * std::cos(glm::radians(30.0f)) + perp * std::sin(glm::radians(30.0f)));
        glm::vec4 rc = svp * glm::vec4(light + rim * 5.0f, 1.0f);
        CHECK(std::abs(rc.x / rc.w) < 1.0f && std::abs(rc.y / rc.w) < 1.0f);
    }

    class TriggerCounter : public ScriptableEntity
    {
    public:
        static inline int Enters = 0, Exits = 0, Collisions = 0;
        void OnTriggerEnter(Entity) override { ++Enters; }
        void OnTriggerExit(Entity) override { ++Exits; }
        void OnCollisionEnter(Entity) override { ++Collisions; }
    };

    void TestPhysicsLayersTriggersJoints()
    {
        auto& config = Project::GetConfig();
        auto savedMatrix = config.LayerCollisionMatrix;
        config.SetLayersCollide(0, 3, false); // layer 3 ("ghosts") passes through the default layer

        // ---- 3D
        {
            Scene scene("Physics3D+");
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Scale = { 40, 1, 40 };
            reg.Emplace<BoxColliderComponent>(ground);
            auto ball = [&](const char* name, glm::vec3 pos, int layer) {
                Entity e = scene.CreateEntity(name);
                reg.Get<TransformComponent>(e).Position = pos;
                reg.Get<TagComponent>(e).Layer = layer;
                reg.Emplace<RigidbodyComponent>(e);
                reg.Emplace<SphereColliderComponent>(e);
                return e;
            };
            Entity solid = ball("Solid", { 0, 3, 0 }, 0);
            Entity ghost = ball("Ghost", { 3, 3, 0 }, 3);
            // Trigger volume the falling probe passes through.
            Entity zone = scene.CreateEntity("Zone");
            reg.Get<TransformComponent>(zone).Position = { -6, 4, 0 };
            reg.Emplace<BoxColliderComponent>(zone).IsTrigger = true;
            reg.Emplace<ScriptComponent>(zone).Add("TriggerCounter");
            Entity probe = ball("Probe", { -6, 7, 0 }, 0);
            // Pendulum: a ball hinged to the world 2 units from its center, and a ball hanging from it by a point joint.
            Entity bob = ball("Bob", { 10, 6, 0 }, 0);
            reg.Get<RigidbodyComponent>(bob).UseGravity = true;
            auto& hinge = reg.Emplace<JointComponent>(bob);
            hinge.Type = JointType::Hinge;
            hinge.Anchor = { -2, 0, 0 }; // pivot at (8, 6, 0)
            hinge.Axis = { 0, 0, 1 };
            Entity tail = ball("Tail", { 13, 6, 0 }, 0);
            auto& link = reg.Emplace<JointComponent>(tail);
            link.Type = JointType::Distance;
            link.ConnectedBody = bob;
            scene.UpdateTransforms();

            TriggerCounter::Enters = TriggerCounter::Exits = TriggerCounter::Collisions = 0;
            auto runtime = scene.Clone();
            runtime->OnRuntimeStart();
            CHECK(runtime->GetPhysics3D() && runtime->GetPhysics3D()->GetJointCount() == 2);
            for (int i = 0; i < 120; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
            auto& rr = runtime->GetRegistry();
            CHECK(Near(rr.Get<TransformComponent>(solid).Position.y, 1.0f, 0.1f)); // rests on the ground
            CHECK(rr.Get<TransformComponent>(ghost).Position.y < -2.0f);           // fell through (layers do not collide)
            CHECK(TriggerCounter::Enters == 1 && TriggerCounter::Exits == 1 && TriggerCounter::Collisions == 0);
            glm::vec3 bobPos = rr.Get<TransformComponent>(bob).Position;
            CHECK(Near(glm::distance(bobPos, glm::vec3(8, 6, 0)), 2.0f, 0.05f)); // hinge keeps the radius
            CHECK(bobPos.y < 5.0f);                                                // and it swung down
            CHECK(std::abs(bobPos.z) < 0.05f);                                     // in the hinge plane
            CHECK(Near(glm::distance(rr.Get<TransformComponent>(tail).Position, bobPos), 3.0f, 0.1f));

            // Overlap queries and layer masks.
            auto nearHits = Physics3DWorld::OverlapSphere(*runtime, rr.Get<TransformComponent>(solid).Position, 0.2f);
            CHECK(nearHits.size() == 1 && nearHits[0] == solid);
            auto all = Physics3DWorld::OverlapBox(*runtime, { 0, 0, 0 }, { 20, 0.6f, 20 });
            CHECK(std::find(all.begin(), all.end(), ground) != all.end() && std::find(all.begin(), all.end(), solid) != all.end());
            CHECK(Physics3DWorld::OverlapSphere(*runtime, rr.Get<TransformComponent>(solid).Position, 0.2f, ~1u).empty()); // mask excludes layer 0
            CHECK(!Physics3DWorld::Raycast(*runtime, { 0, 10, 0 }, { 0, -1, 0 }, 100.0f, 1u << 5)); // nothing on layer 5
            runtime->OnRuntimeStop();
        }

        // ---- 2D
        {
            Scene scene("Physics2D+");
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Scale = { 40, 1, 1 };
            reg.Emplace<BoxCollider2DComponent>(ground);
            auto body = [&](const char* name, glm::vec2 pos, int layer) {
                Entity e = scene.CreateEntity(name);
                reg.Get<TransformComponent>(e).Position = { pos, 0 };
                reg.Get<TagComponent>(e).Layer = layer;
                reg.Emplace<Rigidbody2DComponent>(e);
                reg.Emplace<CircleCollider2DComponent>(e);
                return e;
            };
            Entity solid = body("Solid", { 0, 3 }, 0);
            Entity ghost = body("Ghost", { 3, 3 }, 3);
            Entity bob = body("Bob", { 10, 6 }, 0);
            auto& hinge = reg.Emplace<Joint2DComponent>(bob);
            hinge.Type = Joint2DType::Hinge;
            hinge.Anchor = { -2, 0 };
            Entity tail = body("Tail", { 13, 6 }, 0);
            auto& link = reg.Emplace<Joint2DComponent>(tail);
            link.Type = Joint2DType::Distance;
            link.ConnectedBody = bob;
            scene.UpdateTransforms();

            auto runtime = scene.Clone();
            runtime->OnRuntimeStart();
            CHECK(runtime->GetPhysics2D() && runtime->GetPhysics2D()->GetJointCount() == 2);
            auto& rr = runtime->GetRegistry();
            float lowest = 1e9f;
            for (int i = 0; i < 120; ++i)
            {
                runtime->OnUpdateRuntime(1.0f / 60.0f);
                lowest = std::min(lowest, rr.Get<TransformComponent>(bob).Position.y);
            }
            CHECK(Near(rr.Get<TransformComponent>(solid).Position.y, 1.0f, 0.1f));
            CHECK(rr.Get<TransformComponent>(ghost).Position.y < -2.0f);
            glm::vec2 bobPos = glm::vec2(rr.Get<TransformComponent>(bob).Position);
            CHECK(Near(glm::distance(bobPos, glm::vec2(8, 6)), 2.0f, 0.05f) && lowest < 4.5f); // swung through the bottom
            CHECK(Near(glm::distance(glm::vec2(rr.Get<TransformComponent>(tail).Position), bobPos), 3.0f, 0.1f));
            auto hits = Physics2DWorld::OverlapCircle(*runtime, glm::vec2(rr.Get<TransformComponent>(solid).Position), 0.2f);
            CHECK(hits.size() == 1 && hits[0] == solid);
            auto box = Physics2DWorld::OverlapBox(*runtime, { 0, 0 }, { 20, 0.6f });
            CHECK(std::find(box.begin(), box.end(), ground) != box.end());
            CHECK(Physics2DWorld::OverlapCircle(*runtime, glm::vec2(rr.Get<TransformComponent>(solid).Position), 0.2f, ~1u).empty());
            CHECK(!Physics2DWorld::Raycast(*runtime, { 0, 10 }, { 0, -1 }, 100.0f, 1u << 5));
            runtime->OnRuntimeStop();
        }

        // Joint references survive serialization.
        {
            Scene scene("JointSave");
            auto& reg = scene.GetRegistry();
            Entity a = scene.CreateEntity("A");
            Entity b = scene.CreateEntity("B");
            reg.Emplace<JointComponent>(a).ConnectedBody = b;
            reg.Get<TagComponent>(a).Layer = 7;
            auto loaded = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
            Entity la = loaded->FindEntityByGuid(scene.GetGuid(a));
            Entity lb = loaded->FindEntityByGuid(scene.GetGuid(b));
            CHECK(!la.IsNull() && loaded->GetRegistry().Get<JointComponent>(la).ConnectedBody == lb);
            CHECK(loaded->GetRegistry().Get<TagComponent>(la).Layer == 7);
        }
        config.LayerCollisionMatrix = savedMatrix;
    }

    void TestBlendTreesAndEvents()
    {
        AnimatorController c;
        c.Parameters = { { "Energy", AnimParameterType::Float, 0.25f } };
        AnimState tree;
        tree.Name = "Locomotion";
        tree.Motion = AnimMotionType::BlendTree1D;
        tree.BlendParameter = "Energy";
        tree.Children = { { "Curl", 1.0f }, { "Sway", 0.0f }, { "Wave", 0.5f } }; // unsorted on purpose
        tree.Events = { { 0.5f, "Half" } };
        AnimState once;
        once.Name = "Once";
        once.Clip = "Sway";
        once.Loop = false;
        once.Events = { { 0.25f, "A" }, { 1.0f, "End" } };
        AnimState loop = once;
        loop.Name = "Loop";
        loop.Loop = true;
        loop.Events = { { 0.25f, "A" }, { 0.0f, "Start" } };
        c.States = { tree, once, loop };

        std::vector<float> w;
        AnimatorStateMachine::BlendWeights(tree, 0.25f, w);
        CHECK(Near(w[1], 0.5f) && Near(w[2], 0.5f) && Near(w[0], 0.0f));
        AnimatorStateMachine::BlendWeights(tree, -1.0f, w);
        CHECK(Near(w[1], 1.0f) && Near(w[0] + w[2], 0.0f)); // clamped below
        AnimatorStateMachine::BlendWeights(tree, 3.0f, w);
        CHECK(Near(w[0], 1.0f));                            // clamped above
        AnimatorStateMachine::BlendWeights(tree, 0.5f, w);
        CHECK(Near(w[2], 1.0f));                            // exactly on a child

        auto duration = [](const std::string& clip) { return clip == "Sway" ? 2.0f : clip == "Wave" ? 1.0f : 4.0f; };
        auto enter = [](const AnimState&, float) {};
        AnimatorRuntime rt; // Energy defaults to 0.25: halfway between Sway (2 s) and Wave (1 s)
        CHECK(Near(AnimatorStateMachine::StateDuration(c, rt, tree, duration), 1.5f));
        AnimatorStateMachine::Update(c, rt, 0.0f, duration, enter);
        AnimatorStateMachine::Update(c, rt, 0.75f, duration, enter); // 0.75 s of a 1.5 s loop
        CHECK(Near(AnimatorStateMachine::NormalizedTime(c, rt, duration), 0.5f));
        CHECK(rt.FiredEvents.size() == 1 && rt.FiredEvents[0] == "Half");
        rt.Parameters["Energy"] = 1.0f; // Curl only: 4 s loop
        AnimatorStateMachine::Update(c, rt, 1.0f, duration, enter);
        CHECK(Near(AnimatorStateMachine::NormalizedTime(c, rt, duration), 0.75f) && rt.FiredEvents.empty());

        // Event collection: one-shot vs looping, including the wrap at the loop boundary.
        std::vector<std::string> fired;
        AnimatorStateMachine::CollectEvents(once, 0.0f, 0.5f, fired);
        AnimatorStateMachine::CollectEvents(once, 0.5f, 3.0f, fired); // past the end: clamped, End fires once
        AnimatorStateMachine::CollectEvents(once, 3.0f, 4.0f, fired);
        CHECK(fired.size() == 2 && fired[0] == "A" && fired[1] == "End");
        fired.clear();
        AnimatorStateMachine::CollectEvents(loop, 0.0f, 2.3f, fired); // A at 0.25, 1.25, 2.25; Start at 1.0, 2.0
        CHECK(fired.size() == 5);
        CHECK(std::count(fired.begin(), fired.end(), std::string("A")) == 3 && std::count(fired.begin(), fired.end(), std::string("Start")) == 2);

        fs::path path = fs::temp_directory_path() / "dimension_blend.controller";
        CHECK(c.Save(path));
        auto loaded = AnimatorController::Load(path);
        CHECK(loaded->States[0].Motion == AnimMotionType::BlendTree1D && loaded->States[0].BlendParameter == "Energy");
        CHECK(loaded->States[0].Children.size() == 3 && loaded->States[0].Children[2].Clip == "Wave" && Near(loaded->States[0].Children[2].Threshold, 0.5f));
        CHECK(loaded->States[1].Events.size() == 2 && loaded->States[1].Events[1].Function == "End" && loaded->States[1].Motion == AnimMotionType::Clip);
        fs::remove(path);
    }

    void TestParticleModules()
    {
        // Curves and gradients.
        Curve curve{ { { 0.0f, 0.0f }, { 0.5f, 2.0f }, { 1.0f, 1.0f } } };
        CHECK(Near(curve.Evaluate(0.25f), 1.0f) && Near(curve.Evaluate(0.75f), 1.5f) && Near(curve.Evaluate(-1.0f), 0.0f) && Near(curve.Evaluate(2.0f), 1.0f));
        Gradient gradient = Gradient::Linear({ 1, 0, 0, 1 }, { 0, 0, 1, 0 });
        glm::vec4 mid = gradient.Evaluate(0.5f);
        CHECK(Near(mid.r, 0.5f) && Near(mid.b, 0.5f) && Near(mid.a, 0.5f));
        const ComponentInfo* info = ComponentRegistry::Find<ParticleSystemComponent>();
        ParticleSystemComponent a, b;
        a.SizeOverLifetime = curve;
        a.ColorOverLifetime = gradient;
        for (auto& f : info->Fields) FieldFromJson(f, &b, FieldToJson(f, &a, nullptr), nullptr);
        CHECK(b.SizeOverLifetime == a.SizeOverLifetime && b.ColorOverLifetime == a.ColorOverLifetime);

        auto makeSystem = [](Scene& scene, const char* name, glm::vec3 pos) -> Entity {
            Entity e = scene.CreateEntity(name);
            scene.GetRegistry().Get<TransformComponent>(e).Position = pos;
            auto& ps = scene.GetRegistry().Emplace<ParticleSystemComponent>(e);
            ps.RateOverTime = 0.0f;
            ps.Shape = ParticleShape::Point;
            ps.StartSpeed = { 0.0f, 0.0f };
            ps.StartLifetime = { 10.0f, 10.0f };
            return e;
        };

        // Plane collision with bounce, and trails.
        {
            Scene scene("Particles+");
            auto& reg = scene.GetRegistry();
            Entity e = makeSystem(scene, "Bouncer", { 0, 3, 0 });
            auto& ps = reg.Get<ParticleSystemComponent>(e);
            ps.BurstCount = 5;
            ps.GravityModifier = 1.0f;
            ps.Collision = ParticleCollision::Plane;
            ps.CollisionPlaneHeight = 0.0f;
            ps.CollisionRadius = 0.05f;
            ps.Bounce = 0.6f;
            ps.Trails = true;
            scene.UpdateTransforms();
            float lowest = 1e9f, maxUp = 0.0f;
            for (int i = 0; i < 180; ++i)
            {
                ParticleSystems::Update(scene, 1.0f / 60.0f);
                for (auto& p : reg.Get<ParticleSystemComponent>(e).Particles)
                {
                    lowest = std::min(lowest, p.Position.y);
                    maxUp = std::max(maxUp, p.Velocity.y);
                }
            }
            auto& live = reg.Get<ParticleSystemComponent>(e);
            CHECK(live.Particles.size() == 5);
            CHECK(lowest >= 0.05f - 1e-3f); // never below the plane (+ radius)
            CHECK(maxUp > 2.0f);            // bounced back up (impact ~7.7 m/s, 60% kept)
            CHECK(live.Particles[0].TrailCount == Particle::kTrailPoints);
        }

        // Sub-emitters: each dying particle bursts in another system; a system cannot feed itself.
        {
            Scene scene("SubEmit");
            auto& reg = scene.GetRegistry();
            Entity parent = makeSystem(scene, "Rocket", { 0, 0, 0 });
            Entity child = makeSystem(scene, "Burst", { 0, 0, 0 });
            auto& pp = reg.Get<ParticleSystemComponent>(parent);
            pp.BurstCount = 3;
            pp.Looping = false;
            pp.Duration = 100.0f;
            pp.StartLifetime = { 0.1f, 0.1f };
            pp.SubEmitter = child;
            pp.SubEmitterTrigger = ParticleSubEmitTrigger::Death;
            pp.SubEmitCount = 4;
            Entity selfish = makeSystem(scene, "Selfish", { 5, 0, 0 });
            auto& sp = reg.Get<ParticleSystemComponent>(selfish);
            sp.BurstCount = 2;
            sp.StartLifetime = { 0.05f, 0.05f };
            sp.SubEmitter = selfish;
            scene.UpdateTransforms();
            for (int i = 0; i < 30; ++i) ParticleSystems::Update(scene, 1.0f / 60.0f);
            CHECK(reg.Get<ParticleSystemComponent>(parent).Particles.empty());
            CHECK(reg.Get<ParticleSystemComponent>(child).Particles.size() == 12);
            CHECK(reg.Get<ParticleSystemComponent>(selfish).Particles.empty()); // no runaway self-emission
        }

        // World collision against 3D physics colliders (play mode).
        {
            Scene scene("ParticlePhysics");
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Scale = { 20, 1, 20 }; // top at y = 0.5
            reg.Emplace<BoxColliderComponent>(ground);
            Entity e = makeSystem(scene, "Rain", { 0, 4, 0 });
            auto& ps = reg.Get<ParticleSystemComponent>(e);
            ps.BurstCount = 10;
            ps.GravityModifier = 1.0f;
            ps.Collision = ParticleCollision::World;
            ps.CollisionRadius = 0.05f;
            ps.Bounce = 0.0f;
            scene.UpdateTransforms();
            auto runtime = scene.Clone();
            runtime->OnRuntimeStart();
            float lowest = 1e9f;
            for (int i = 0; i < 120; ++i)
            {
                runtime->OnUpdateRuntime(1.0f / 60.0f);
                for (auto& p : runtime->GetRegistry().Get<ParticleSystemComponent>(e).Particles) lowest = std::min(lowest, p.Position.y);
            }
            CHECK(runtime->GetRegistry().Get<ParticleSystemComponent>(e).Particles.size() == 10);
            CHECK(lowest > 0.5f && lowest < 0.6f); // resting on the collider's top face
            runtime->OnRuntimeStop();
        }
    }

    void TestSceneManager()
    {
        fs::path assets = fs::temp_directory_path() / "dimension_scenes_test" / "Assets";
        fs::remove_all(assets.parent_path());
        fs::create_directories(assets / "Levels");
        for (const char* name : { "Menu", "Level1", "Secret" })
        {
            Scene scene(name);
            scene.CreateEntity(std::string(name) + " Root");
            CHECK(SceneSerializer::SaveToFile(scene, assets / "Levels" / (std::string(name) + ".scene")));
        }
        AssetManager::Init(assets);
        auto& config = Project::GetConfig();
        auto saved = config.BuildScenes;
        config.BuildScenes = { AssetManager::GetHandle(assets / "Levels/Menu.scene"), AssetManager::GetHandle(assets / "Levels/Level1.scene") };

        CHECK(SceneManager::GetBuildScenes().size() == 2 && SceneManager::GetBuildIndex("Level1") == 1 && SceneManager::GetBuildIndex("Secret") == -1);
        CHECK(!SceneManager::HasPendingLoad() && SceneManager::LoadPending() == nullptr);

        SceneManager::LoadScene("Level1");
        CHECK(SceneManager::HasPendingLoad());
        auto level = SceneManager::LoadPending();
        CHECK(level && level->GetName() == "Level1" && !level->FindEntityByName("Level1 Root").IsNull());
        CHECK(!SceneManager::HasPendingLoad());

        SceneManager::LoadScene(0);
        auto menu = SceneManager::LoadPending();
        CHECK(menu && menu->GetName() == "Menu");

        SceneManager::LoadScene("Levels/Secret"); // not in the build list: Assets-relative path
        auto secret = SceneManager::LoadPending();
        CHECK(secret && secret->GetName() == "Secret");

        SceneManager::LoadScene("Nope");
        CHECK(SceneManager::LoadPending() == nullptr && !SceneManager::HasPendingLoad());
        SceneManager::LoadScene(7);
        CHECK(SceneManager::LoadPending() == nullptr);

        // The last request wins; Cancel clears it.
        SceneManager::LoadScene("Menu");
        SceneManager::LoadScene("Level1");
        CHECK(SceneManager::LoadPending()->GetName() == "Level1");
        SceneManager::LoadScene("Menu");
        SceneManager::CancelPendingLoad();
        CHECK(!SceneManager::HasPendingLoad());

        config.BuildScenes = saved;
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path());
    }

    class TimerProbe : public ScriptableEntity
    {
    public:
        static inline int Once = 0, Ticks = 0;
        int RepeatId = 0;
        void OnCreate() override
        {
            Invoke([] { ++Once; }, 0.5f);
            RepeatId = InvokeRepeating([this] {
                if (++Ticks == 5) CancelInvoke(RepeatId); // cancelling from inside the callback
            }, 0.2f, 0.2f);
        }
    };

    void TestScriptTimers()
    {
        Scene scene("Timers");
        Entity e = scene.CreateEntity("Probe");
        scene.GetRegistry().Emplace<ScriptComponent>(e).Add("TimerProbe");
        TimerProbe::Once = TimerProbe::Ticks = 0;
        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        for (int i = 0; i < 90; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f); // 1.5 s
        CHECK(TimerProbe::Once == 1);
        CHECK(TimerProbe::Ticks == 5); // 0.2 .. 1.0, then cancelled
        CHECK(Near(runtime->GetTime(), 1.5f, 1e-3f) && runtime->GetFrameCount() == 90);
        runtime->OnRuntimeStop();
    }

    void TestAudioMixer()
    {
        auto& groups = Project::GetConfig().MixerGroups;
        auto saved = groups;
        // Master <- Music <- Ambience, Master <- SFX
        groups = AudioMixer::DefaultGroups();
        AudioMixerGroup ambience;
        ambience.Name = "Ambience";
        ambience.Parent = 1;
        groups.push_back(ambience);

        CHECK(AudioMixer::FindGroup("SFX") == 2 && AudioMixer::FindGroup("Nope") == -1);
        CHECK(Near(AudioMixer::DbToLinear(0.0f), 1.0f) && Near(AudioMixer::DbToLinear(-6.0206f), 0.5f, 1e-3f) && AudioMixer::DbToLinear(-80.0f) == 0.0f);
        CHECK(Near(AudioMixer::LinearToDb(0.5f), -6.0206f, 1e-3f) && AudioMixer::LinearToDb(0.0f) == AudioMixer::kSilentDb);

        AudioMixer::SetVolumeDb("Music", -12.0f);
        CHECK(Near(AudioMixer::GetVolumeDb("Music"), -12.0f) && Near(AudioMixer::EffectiveGain(1), AudioMixer::DbToLinear(-12.0f)));
        AudioMixer::SetVolumeDb("Music", 99.0f);
        CHECK(Near(AudioMixer::GetVolumeDb("Music"), 20.0f)); // clamped
        groups[1].VolumeDb = 0.0f;

        groups[2].Mute = true;
        CHECK(AudioMixer::EffectiveGain(2) == 0.0f && Near(AudioMixer::EffectiveGain(1), 1.0f));
        groups[2].Mute = false;
        // Solo Music: Music, its child Ambience and the path to Master stay audible; SFX goes quiet.
        groups[1].Solo = true;
        CHECK(Near(AudioMixer::EffectiveGain(1), 1.0f) && Near(AudioMixer::EffectiveGain(3), 1.0f) && Near(AudioMixer::EffectiveGain(0), 1.0f));
        CHECK(AudioMixer::EffectiveGain(2) == 0.0f);
        groups[1].Solo = false;
        // A cycle (Music <-> Ambience) falls back to Master instead of hanging.
        groups[1].Parent = 3;
        CHECK(Near(AudioMixer::EffectiveGain(3), 1.0f));
        groups[1].Parent = 0;

        // With an audio device: build the graph and play a streamed source and a one-shot through it.
        AudioEngine::Init();
        if (AudioEngine::IsAvailable())
        {
            AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
            Scene scene("Audio");
            Entity e = scene.CreateEntity("Speaker");
            auto& src = scene.GetRegistry().Emplace<AudioSourceComponent>(e);
            src.Clip = AssetManager::GetHandle("Audio/Music.wav");
            src.Output = "Ambience";
            src.Stream = true;
            src.Loop = true;
            CHECK(src.Clip.IsValid());
            auto runtime = scene.Clone();
            runtime->OnRuntimeStart();
            for (int g = 0; g < 4; ++g) CHECK(AudioMixer::GetGroupNode(g) != nullptr);
            CHECK(AudioWorld::IsPlaying(*runtime, e));
            AudioWorld::PlayOneShot(*runtime, e, AssetManager::GetHandle("Audio/Chime.wav"), 0.5f);
            runtime->OnUpdateRuntime(1.0f / 60.0f);
            AudioWorld::Stop(*runtime, e);
            CHECK(!AudioWorld::IsPlaying(*runtime, e));
            runtime->OnRuntimeStop();
            AssetManager::Shutdown();
        }
        else
            std::printf("  (no audio device: graph checks skipped)\n");
        AudioEngine::Shutdown();
        groups = saved;
    }

    class CharacterProbe : public ScriptableEntity
    {
    public:
        glm::vec3 Speed{ 0.0f };
        bool Jump = false;
        void OnUpdate(float) override
        {
            auto& cc = Get<CharacterControllerComponent>();
            if (Jump && cc.IsGrounded) { cc.VerticalSpeed = 5.0f; Jump = false; }
            Physics3DWorld::SimpleMoveCharacter(GetScene(), Self(), Speed);
        }
    };

    void TestMeshCollidersAndCharacters()
    {
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets"); // CPU mesh data only
        Scene scene("Physics3D++");
        auto& reg = scene.GetRegistry();
        // Static concave mesh collider from the built-in Plane (10 x 10), scaled 2x.
        Entity ground = scene.CreateEntity("Ground");
        reg.Get<TransformComponent>(ground).Scale = { 2, 1, 2 };
        reg.Emplace<MeshRendererComponent>(ground).Mesh = AssetManager::kPlane;
        reg.Emplace<MeshColliderComponent>(ground);
        auto dynamicBody = [&](const char* name, glm::vec3 pos, AssetHandle mesh) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = pos;
            reg.Emplace<MeshRendererComponent>(e).Mesh = mesh;
            reg.Emplace<RigidbodyComponent>(e);
            reg.Emplace<MeshColliderComponent>(e).Convex = true;
            return e;
        };
        Entity cube = dynamicBody("Hull Cube", { 4, 3, 0 }, AssetManager::kCube);     // convex hull of the unit cube
        Entity torusBall = dynamicBody("Hull Sphere", { -4, 3, 0 }, AssetManager::kSphere);

        // A wall and a low step for the character.
        auto box = [&](const char* name, glm::vec3 pos, glm::vec3 size) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = pos;
            reg.Emplace<BoxColliderComponent>(e).Size = size;
            return e;
        };
        box("Step", { 0, 0.1f, -3.0f }, { 4, 0.2f, 1 });   // 0.2 high: climbable with Step Offset 0.3
        box("Wall", { 0, 1, -8.0f }, { 4, 2, 0.5f });      // stops the character
        Entity character = scene.CreateEntity("Character");
        reg.Get<TransformComponent>(character).Position = { 0, 2, 0 };
        auto& cc = reg.Emplace<CharacterControllerComponent>(character);
        cc.Height = 2.0f;
        cc.Radius = 0.4f;
        reg.Emplace<ScriptComponent>(character).Add("CharacterProbe");
        // Two overlapping jointed bodies: with collision ignored they stay put instead of pushing apart.
        Entity a = dynamicBody("Linked A", { 8, 1, 0 }, AssetManager::kCube);
        Entity b = dynamicBody("Linked B", { 8.5f, 1, 0 }, AssetManager::kCube);
        reg.Get<RigidbodyComponent>(a).UseGravity = reg.Get<RigidbodyComponent>(b).UseGravity = false;
        auto& joint = reg.Emplace<JointComponent>(b);
        joint.Type = JointType::Point;
        joint.ConnectedBody = a;
        scene.UpdateTransforms();

        auto runtime = scene.Clone();
        auto& rr = runtime->GetRegistry();
        runtime->OnRuntimeStart();
        auto* probe = [&]() -> CharacterProbe* {
            for (auto& inst : rr.Get<ScriptComponent>(character).LiveInstances(false))
                if (auto* p = dynamic_cast<CharacterProbe*>(inst.get())) return p;
            return nullptr;
        }();
        CHECK(probe != nullptr);
        if (!probe) { runtime->OnRuntimeStop(); AssetManager::Shutdown(); return; }
        auto run = [&](int frames) { for (int i = 0; i < frames; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f); };
        run(120);
        auto pos = [&](Entity e) { return rr.Get<TransformComponent>(e).Position; };
        CHECK(Near(pos(cube).y, 0.5f, 0.05f));      // hull cube rests on the mesh plane
        CHECK(Near(pos(torusBall).y, 0.5f, 0.05f)); // hull sphere too
        CHECK(Near(pos(character).y, 1.0f, 0.06f) && rr.Get<CharacterControllerComponent>(character).IsGrounded);
        CHECK(glm::distance(pos(a), glm::vec3(8, 1, 0)) < 0.05f && glm::distance(pos(b), glm::vec3(8.5f, 1, 0)) < 0.05f);

        // Walk towards -Z: climb the step, stop at the wall.
        probe->Speed = { 0, 0, -3.0f };
        run(60);
        CHECK(pos(character).z < -1.5f);
        run(120);
        float wallFace = -8.0f + 0.25f;
        CHECK(pos(character).z > wallFace && pos(character).z < wallFace + 0.6f); // stopped by the wall (radius 0.4 + skin)
        // Walk back down onto the floor and jump.
        probe->Speed = { 0, 0, 3.0f };
        run(150); // past the step (z -3.5 .. -2.5) back onto the floor
        probe->Speed = { 0, 0, 0 };
        run(30);
        CHECK(Near(pos(character).y, 1.0f, 0.06f));
        probe->Jump = true;
        float peak = 0.0f;
        for (int i = 0; i < 60; ++i)
        {
            run(1);
            peak = std::max(peak, pos(character).y);
        }
        CHECK(peak > 2.0f); // 5 m/s jump rises ~1.27 m
        run(90);
        CHECK(Near(pos(character).y, 1.0f, 0.06f) && rr.Get<CharacterControllerComponent>(character).IsGrounded);
        // Raycasts see the character's inner body.
        auto hit = Physics3DWorld::Raycast(*runtime, pos(character) + glm::vec3(0, 5, 0), { 0, -1, 0 }, 10.0f);
        CHECK(hit && hit.Hit == character);
        runtime->OnRuntimeStop();
        AssetManager::Shutdown();
    }

    void TestEnvironmentLighting()
    {
        using namespace Environment;
        // Mapping conventions shared with Common.glsl.
        glm::vec2 forward = DirectionToEquirect({ 0, 0, -1 });
        CHECK(Near(forward.x, 0.5f) && Near(forward.y, 0.5f));
        CHECK(Near(DirectionToEquirect({ 0, 1, 0 }).y, 0.0f) && Near(DirectionToEquirect({ 0, -1, 0 }).y, 1.0f));
        CHECK(Near(DirectionToEquirect({ 1, 0, 0 }).x, 0.75f));
        for (glm::vec3 d : { glm::vec3(0.3f, 0.5f, -0.8f), glm::vec3(-0.9f, -0.2f, 0.1f), glm::vec3(0.1f, 0.95f, 0.3f) })
        {
            glm::vec3 back = EquirectToDirection(DirectionToEquirect(d));
            CHECK(glm::distance(back, glm::normalize(d)) < 1e-4f);
        }

        // A constant environment of radiance L gives irradiance (divided by pi) L in every direction.
        const uint32_t w = 64, h = 32;
        std::vector<float> img(w * h * 4);
        for (size_t i = 0; i < img.size(); i += 4) { img[i] = 0.5f; img[i + 1] = 1.0f; img[i + 2] = 2.0f; img[i + 3] = 1.0f; }
        SH9 sh = ComputeIrradianceSH(img.data(), w, h);
        for (glm::vec3 n : { glm::vec3(0, 1, 0), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0), glm::vec3(0.5f, 0.5f, -0.7f) })
        {
            glm::vec3 e = EvaluateSH(sh, n);
            CHECK(Near(e.r, 0.5f, 0.02f) && Near(e.g, 1.0f, 0.03f) && Near(e.b, 2.0f, 0.05f));
        }
        // White sky above, black ground below: an upward normal sees ~1, a downward one ~0, sideways ~0.5.
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                float v = y < h / 2 ? 1.0f : 0.0f;
                for (int c = 0; c < 3; ++c) img[(y * w + x) * 4 + c] = v;
            }
        sh = ComputeIrradianceSH(img.data(), w, h);
        CHECK(Near(EvaluateSH(sh, { 0, 1, 0 }).g, 1.0f, 0.12f));
        CHECK(EvaluateSH(sh, { 0, -1, 0 }).g < 0.12f);
        CHECK(Near(EvaluateSH(sh, { 1, 0, 0 }).g, 0.5f, 0.05f));
    }

    void TestTilemaps()
    {
        TilemapComponent tm;
        tm.SetTile(0, 0, 1);
        tm.SetTile(1, 0, 2);
        tm.SetTile(2, 0, 2);
        tm.SetTile(5, 0, 0);
        tm.SetTile(-3, 4, 7);
        tm.SetTile(1, 0, -1); // erase
        tm.SetTile(1, 0, 3);  // repaint
        CHECK(tm.Count() == 5 && tm.GetTile(1, 0) == 3 && tm.GetTile(-3, 4) == 7 && tm.GetTile(9, 9) == -1);
        CHECK(tm.Data == "0,0,1;1,0,3;2,0,2;5,0,0;-3,4,7;"); // sorted by row then column

        // Data written by the serializer (or by hand) is decoded lazily; garbage entries are skipped.
        TilemapComponent loaded;
        loaded.Data = tm.Data + "oops;4,4;";
        CHECK(loaded.Count() == 5 && loaded.GetTile(5, 0) == 0);
        auto boxes = loaded.ColliderBoxes(); // row 0: [0..2] and [5]; row 4: [-3]
        CHECK(boxes.size() == 3);
        bool longRun = false;
        for (auto& b : boxes) longRun |= Near(b.x, 0.0f) && Near(b.z, 3.0f) && Near(b.w, 1.0f);
        CHECK(longRun);
        loaded.CellSize = 0.5f;
        CHECK(loaded.LocalToCell({ 1.2f, -0.1f }) == glm::ivec2(2, -1));

        // Round trip through the scene serializer, then a body lands on the tilemap collider.
        Scene scene("Tiles");
        auto& reg = scene.GetRegistry();
        Entity map = scene.CreateEntity("Map");
        reg.Get<TransformComponent>(map).Position = { -0.5f, -0.5f, 0 };
        auto& t = reg.Emplace<TilemapComponent>(map);
        for (int x = -5; x <= 5; ++x) t.SetTile(x, -2, 0);
        Entity box = scene.CreateEntity("Box");
        reg.Get<TransformComponent>(box).Position = { 0, 3, 0 };
        reg.Emplace<Rigidbody2DComponent>(box);
        reg.Emplace<BoxCollider2DComponent>(box);
        auto reloaded = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        Entity rmap = reloaded->FindEntityByName("Map");
        CHECK(!rmap.IsNull() && reloaded->GetRegistry().Get<TilemapComponent>(rmap).Count() == 11);
        reloaded->UpdateTransforms();
        auto runtime = reloaded->Clone();
        runtime->OnRuntimeStart();
        for (int i = 0; i < 120; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
        Entity rbox = runtime->FindEntityByName("Box");
        // Row -2 spans world y [-2.5, -1.5]; the unit box rests with its center at -1.0.
        CHECK(Near(runtime->GetRegistry().Get<TransformComponent>(rbox).Position.y, -1.0f, 0.05f));
        runtime->OnRuntimeStop();
    }

    void TestAnimationSampling()
    {
        // Two-joint chain: root at origin, child 1 unit up. Clip rotates the root 90 degrees about Z over 1s.
        Skeleton sk;
        Joint root;
        root.Name = "Root";
        Joint child;
        child.Name = "Child";
        child.Parent = 0;
        child.Translation = { 0, 1, 0 };
        child.InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0, -1, 0));
        sk.Joints = { root, child };
        sk.ComputeOrder();
        CHECK(sk.Order.size() == 2 && sk.Order[0] == 0);

        AnimationClip clip;
        clip.Name = "Turn";
        clip.Duration = 1.0f;
        AnimationChannel ch;
        ch.Joint = 0;
        ch.Path = AnimationPath::Rotation;
        glm::quat q90 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
        ch.Times = { 0.0f, 1.0f };
        ch.Values = { { 0, 0, 0, 1 }, { q90.x, q90.y, q90.z, q90.w } };
        clip.Channels.push_back(ch);

        Pose pose;
        std::vector<glm::mat4> skin;
        pose.SetBindPose(sk);
        Animation::ComputeSkinMatrices(sk, pose, skin);
        CHECK(Near(glm::vec3(skin[1] * glm::vec4(0, 2, 0, 1)), { 0, 2, 0 })); // bind pose = identity skinning

        Animation::Sample(sk, clip, 0.5f, pose);
        CHECK(Near(glm::degrees(glm::angle(pose.Rotations[0])), 45.0f, 0.01f));
        Animation::Sample(sk, clip, 1.0f, pose);
        Animation::ComputeSkinMatrices(sk, pose, skin);
        // A vertex at y=2 bound to the child ends up at x=-2 after a 90 degree turn of the root.
        CHECK(Near(glm::vec3(skin[1] * glm::vec4(0, 2, 0, 1)), { -2, 0, 0 }));
        CHECK(Near(glm::vec3(skin[0] * glm::vec4(0, 1, 0, 1)), { -1, 0, 0 }));

        Pose a, b, blended;
        a.SetBindPose(sk);
        Animation::Sample(sk, clip, 1.0f, b);
        Animation::Blend(a, b, 0.5f, blended);
        CHECK(Near(glm::degrees(glm::angle(blended.Rotations[0])), 45.0f, 0.01f));
    }

    void CheckSkinnedFile(const fs::path& path, size_t minJoints, size_t minClips)
    {
        MeshData data;
        CHECK(MeshData::LoadFromFile(path, data));
        CHECK(data.SkeletonData != nullptr);
        if (!data.SkeletonData) return;
        CHECK(data.SkeletonData->Joints.size() >= minJoints);
        CHECK(data.Clips.size() >= minClips);
        CHECK(data.Skin.size() == data.Vertices.size());
        size_t badWeights = 0;
        for (auto& sv : data.Skin)
        {
            float sum = sv.Weights.x + sv.Weights.y + sv.Weights.z + sv.Weights.w;
            if (std::abs(sum - 1.0f) > 1e-3f || glm::any(glm::greaterThanEqual(sv.Joints, glm::uvec4((uint32_t)data.SkeletonData->Joints.size()))))
                ++badWeights;
        }
        CHECK(badWeights == 0);
        // Bind pose must reproduce the authored mesh: skin matrices are identity.
        Pose pose;
        pose.SetBindPose(*data.SkeletonData);
        std::vector<glm::mat4> skin;
        Animation::ComputeSkinMatrices(*data.SkeletonData, pose, skin);
        float maxError = 0.0f;
        for (size_t i = 0; i < data.Vertices.size(); i += 7)
        {
            const auto& sv = data.Skin[i];
            glm::mat4 m = skin[sv.Joints.x] * sv.Weights.x + skin[sv.Joints.y] * sv.Weights.y + skin[sv.Joints.z] * sv.Weights.z +
                          skin[sv.Joints.w] * sv.Weights.w;
            glm::vec3 p = data.Vertices[i].Position;
            maxError = std::max(maxError, glm::length(glm::vec3(m * glm::vec4(p, 1.0f)) - p) / std::max(1.0f, glm::length(p)));
        }
        CHECK(maxError < 1e-3f);
        std::printf("  %s: %zu joints, %zu clips, %zu vertices, bind error %.2e\n", path.filename().string().c_str(),
                    data.SkeletonData->Joints.size(), data.Clips.size(), data.Vertices.size(), maxError);
    }

    void TestGltfMaterials()
    {
        fs::path model = fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Showcase.glb";
        MeshData data;
        CHECK(MeshData::LoadFromFile(model, data));
        CHECK(data.Materials.size() == 5 && data.Images.size() == 3 && data.SubMeshes.size() == 5);
        if (data.Materials.size() != 5 || data.SubMeshes.size() != 5) return;
        uint32_t covered = 0;
        for (size_t i = 0; i < data.SubMeshes.size(); ++i)
        {
            CHECK(data.SubMeshes[i].MaterialSlot == (int32_t)i);
            covered += data.SubMeshes[i].IndexCount;
        }
        CHECK(covered == data.Indices.size());
        auto& slate = data.Materials[0];
        CHECK(slate.Name == "Slate" && Near(slate.Values.Metallic, 0.0f) && Near(slate.Values.Roughness, 0.6f));
        CHECK(Near(slate.Values.BaseColor.g, 0.3320f, 0.003f)); // linear 0.09 stored as sRGB
        CHECK(Near(data.Materials[4].Values.Metallic, 0.8f));
        auto& brick = data.Materials[1];
        CHECK(brick.BaseColorImage == 0 && brick.NormalImage == 1 && !data.Images[0].Bytes.empty());
        auto& neon = data.Materials[2];
        CHECK(Near(neon.Values.EmissionStrength, 6.0f) && neon.Values.EmissionColor.b > 0.99f);
        CHECK(data.Materials[3].Values.BlendMode == MaterialBlendMode::Transparent && Near(data.Materials[3].Values.BaseColor.a, 0.3f));
        auto& grate = data.Materials[4];
        CHECK(grate.Values.BlendMode == MaterialBlendMode::Cutout && grate.Values.DoubleSided && grate.BaseColorImage == 2);

        // OBJ files get a single sub-mesh using the renderer's own material.
        MeshData obj;
        CHECK(MeshData::LoadFromFile(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Torus.obj", obj));
        CHECK(obj.SubMeshes.size() == 1 && obj.SubMeshes[0].MaterialSlot == -1 && obj.Materials.empty());

        // Extraction writes .mat files and embedded textures and records them in the model's meta.
        fs::path assets = fs::temp_directory_path() / "dimension_gltf_test" / "Assets";
        fs::remove_all(assets.parent_path());
        fs::create_directories(assets);
        fs::copy_file(model, assets / "Showcase.glb");
        AssetManager::Init(assets);
        AssetHandle h = AssetManager::GetHandle(assets / "Showcase.glb");
        CHECK(h.IsValid() && !AssetManager::AreMeshMaterialsExtracted(h));
        CHECK(AssetManager::ExtractMeshMaterials(h));
        CHECK(AssetManager::AreMeshMaterialsExtracted(h));
        fs::path dir = assets / "Showcase Materials";
        CHECK(fs::exists(dir / "Slate.mat") && fs::exists(dir / "Brick.png") && fs::exists(dir / "Grate.png"));
        auto brickMat = Material::Load(dir / "Brick.mat");
        CHECK(brickMat && brickMat->BaseColorMap == AssetManager::GetHandle(dir / "Brick.png"));
        CHECK(brickMat && brickMat->NormalMap.IsValid());
        if (brickMat && brickMat->NormalMap.IsValid())
            CHECK(AssetManager::GetMetadata(brickMat->NormalMap)->ImportSettings.value("srgb", true) == false);
        auto& guids = AssetManager::GetMetadata(h)->ImportSettings["materials"];
        CHECK(guids.size() == 5 && AssetHandle(guids[0].get<uint64_t>()) == AssetManager::GetHandle(dir / "Slate.mat"));
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path());
    }

    void TestSkinnedImport()
    {
        CheckSkinnedFile(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Tentacle.gltf", 6, 3);
        if (const char* extra = std::getenv("DIM_TEST_EXTRA_GLTF")) CheckSkinnedFile(extra, 1, 1);
    }

    void TestPrefabs()
    {
        fs::path assets = fs::temp_directory_path() / "dimension_prefab_test" / "Assets";
        fs::remove_all(assets.parent_path());
        fs::create_directories(assets);
        AssetManager::Init(assets);

        Scene scene("Prefabs");
        auto& reg = scene.GetRegistry();
        Entity source = scene.CreateEntity("Enemy");
        reg.Emplace<MeshRendererComponent>(source).Color = { 1, 0, 0, 1 };
        Entity eye = scene.CreateEntity("Eye", source);
        reg.Get<TransformComponent>(eye).Position = { 0, 1, 0 };
        auto& sc = reg.Emplace<ScriptComponent>(source).Add("TestScript");
        sc.Fields["Target"] = (uint64_t)scene.GetGuid(eye); // reference inside the prefab

        AssetHandle prefab = Prefabs::CreateFromEntity(scene, source, assets / "Enemy.prefab");
        CHECK(prefab.IsValid());
        CHECK(reg.Has<PrefabLinkComponent>(source) && reg.Get<PrefabLinkComponent>(source).IsRoot);
        CHECK(reg.Has<PrefabLinkComponent>(eye) && !reg.Get<PrefabLinkComponent>(eye).IsRoot);

        Entity a = scene.InstantiatePrefab(prefab, { 5, 0, 0 });
        Entity b = scene.InstantiatePrefab(prefab, { -5, 0, 0 });
        CHECK(scene.GetChildren(a).size() == 1 && scene.GetChildren(b).size() == 1);
        // The script reference points at the instance's own child, not the original.
        uint64_t targetA = reg.Get<ScriptComponent>(a).Scripts[0].Fields["Target"].get<uint64_t>();
        CHECK(targetA == (uint64_t)scene.GetGuid(scene.GetChildren(a)[0]));

        // Override in A, structural change applied from B.
        reg.Get<MeshRendererComponent>(a).Color = { 0, 1, 0, 1 };
        Prefabs::RecordFieldOverride(scene, a, "Mesh Renderer", "Color");
        reg.Get<MeshRendererComponent>(b).Smoothness = 0.9f;
        reg.Emplace<PointLightComponent>(b).Range = 3.0f;
        scene.CreateEntity("Antenna", b);
        CHECK(Prefabs::Apply(scene, b));

        auto& mrA = reg.Get<MeshRendererComponent>(a);
        CHECK(Near(mrA.Color.g, 1.0f));                       // override kept
        CHECK(Near(mrA.Smoothness, 0.9f));                     // non-overridden field propagated
        CHECK(reg.Has<PointLightComponent>(a) && Near(reg.Get<PointLightComponent>(a).Range, 3.0f)); // component propagated
        CHECK(scene.GetChildren(a).size() == 2);               // child propagated
        CHECK(Near(reg.Get<TransformComponent>(a).Position.x, 5.0f)); // root position is instance-owned
        CHECK(scene.GetChildren(source).size() == 2);          // the original is an instance too

        // Removing a component on an instance is an override that survives syncs.
        reg.Remove<PointLightComponent>(a);
        Prefabs::RecordComponentRemoved(scene, a, "Point Light");
        Prefabs::SyncAll(scene);
        CHECK(!reg.Has<PointLightComponent>(a));

        // Revert restores everything except instance-owned values.
        scene.CreateEntity("Extra", a);
        Prefabs::Revert(scene, a);
        CHECK(Near(reg.Get<MeshRendererComponent>(a).Color.r, 1.0f));
        CHECK(reg.Has<PointLightComponent>(a));
        CHECK(scene.GetChildren(a).size() == 2);
        CHECK(Near(reg.Get<TransformComponent>(a).Position.x, 5.0f));

        // Duplicates stay linked; unpacking removes the link.
        Entity c = scene.DuplicateEntity(a);
        CHECK(reg.Has<PrefabLinkComponent>(c) && reg.Get<PrefabLinkComponent>(c).Prefab == prefab);
        Prefabs::Unpack(scene, c);
        CHECK(!reg.Has<PrefabLinkComponent>(c));

        // Links survive save/load.
        auto loaded = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        Entity la = loaded->FindEntityByGuid(scene.GetGuid(a));
        CHECK(!la.IsNull() && loaded->GetRegistry().Has<PrefabLinkComponent>(la));
        CHECK(loaded->GetRegistry().Get<PrefabLinkComponent>(la).SourceId == reg.Get<PrefabLinkComponent>(a).SourceId);

        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path());
    }

    void TestParticles()
    {
        Scene scene("Particles");
        auto& reg = scene.GetRegistry();
        Entity e = scene.CreateEntity("Emitter");
        reg.Get<TransformComponent>(e).Position = { 10, 0, 0 };
        auto& ps = reg.Emplace<ParticleSystemComponent>(e);
        ps.RateOverTime = 100.0f;
        ps.StartLifetime = { 1.0f, 1.0f };
        ps.StartSpeed = { 2.0f, 2.0f };
        ps.ShapeAngle = 0.0f;
        ps.ShapeRadius = 0.0f;
        ps.BurstCount = 10;
        scene.UpdateTransforms();

        // Edit-mode preview simulates too (PlayOnAwake).
        for (int i = 0; i < 30; ++i) scene.OnUpdateEditor(1.0f / 60.0f); // 0.5 s
        auto& p = reg.Get<ParticleSystemComponent>(e);
        CHECK(p.Particles.size() >= 55 && p.Particles.size() <= 62); // 10 burst + ~50 emitted
        float maxY = 0.0f;
        for (auto& q : p.Particles) maxY = std::max(maxY, q.Position.y);
        CHECK(Near(maxY, 1.0f, 0.05f));                 // oldest particles moved 2 units/s for 0.5 s straight up
        CHECK(Near(p.Particles.front().Position.x, 10.0f)); // world space: emitted at the entity
        std::printf("  %zu particles, highest at y=%.3f\n", p.Particles.size(), maxY);

        for (int i = 0; i < 120; ++i) scene.OnUpdateEditor(1.0f / 60.0f); // steady state: rate * lifetime
        CHECK(p.Particles.size() >= 95 && p.Particles.size() <= 105);

        p.MaxParticles = 20;
        p.Clear();
        for (int i = 0; i < 60; ++i) scene.OnUpdateEditor(1.0f / 60.0f);
        CHECK(p.Particles.size() <= 20);

        p.Stop();
        for (int i = 0; i < 90; ++i) scene.OnUpdateEditor(1.0f / 60.0f);
        CHECK(p.Particles.empty() && !p.IsAlive()); // existing particles finish, no new ones

        p.Emit(5);
        scene.OnUpdateEditor(0.0f);
        CHECK(p.Particles.size() == 5);
    }

    void TestCSharpScripts()
    {
        fs::path managed = ExecutableDir() / "managed";
        fs::path scripts = fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/.dimension/ScriptAssemblies/GameScripts.dll";
        if (!fs::exists(scripts) || !CSharpRuntime::Init(managed))
        {
            std::printf("  skipped: C# runtime or compiled sample scripts not available\n");
            return;
        }
        CHECK(CSharpRuntime::LoadScripts(scripts));
        const ScriptInfo* spinner = ScriptRegistry::Find("Game.Spinner");
        CHECK(spinner != nullptr && spinner->Module == "csharp");
        if (!spinner) return;
        bool hasField = false;
        for (auto& f : spinner->Fields) hasField |= f.Name == "DegreesPerSecond" && f.Type == FieldType::Vec3;
        CHECK(hasField);

        Scene scene("CSharp");
        auto& reg = scene.GetRegistry();
        Entity e = scene.CreateEntity("Spinny");
        auto& sc = reg.Emplace<ScriptComponent>(e).Add("Game.Spinner");
        sc.Fields["DegreesPerSecond"] = { 0.0f, 90.0f, 0.0f };
        sc.Fields["BobHeight"] = 0.0f;
        Entity r = scene.CreateEntity("Rainbowy");
        reg.Emplace<MeshRendererComponent>(r).Color = { 1, 1, 1, 1 };
        reg.Emplace<ScriptComponent>(r).Add("Game.Rainbow");
        // UI events reach C#: the Greeter writes into the "Greeting" text when its input field is submitted.
        Entity canvas = scene.CreateEntity("Canvas");
        reg.Emplace<CanvasComponent>(canvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
        Entity greeting = scene.CreateEntity("Greeting", canvas);
        reg.Emplace<UIRectComponent>(greeting);
        reg.Emplace<UITextComponent>(greeting).Text = "";
        Entity field = scene.CreateEntity("Field", canvas);
        {
            auto& fr = reg.Emplace<UIRectComponent>(field);
            fr.AnchorMin = fr.AnchorMax = fr.Pivot = { 0, 0 };
            fr.Size = { 200, 50 };
        }
        reg.Emplace<UIInputFieldComponent>(field);
        reg.Emplace<ScriptComponent>(field).Add("Game.Greeter");
        scene.UpdateTransforms();

        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        runtime->SetUIInput({ 50, 25 }, true, { 800, 600 });
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        runtime->SetUIInput({ 50, 25 }, false, { 800, 600 });
        runtime->InjectUIKeyboard("Ada", false, true);
        for (int i = 0; i < 59; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(runtime->GetRegistry().Get<UITextComponent>(greeting).Text == "Hello, Ada!");
        float yaw = glm::degrees(runtime->GetRegistry().Get<TransformComponent>(e).Rotation.y);
        CHECK(Near(yaw, 90.0f, 1.0f)); // 90 deg/s for one second, applied by C# through the native API
        glm::vec4 c = runtime->GetRegistry().Get<MeshRendererComponent>(r).Color;
        CHECK(!(Near(c.r, 1.0f) && Near(c.g, 1.0f) && Near(c.b, 1.0f))); // recolored from C#
        runtime->OnRuntimeStop();
        runtime.reset();

        // Coroutines (WaitForSeconds, nested routines) and InvokeRepeating from C#.
        {
            Scene co("Coroutines");
            Entity lamp = co.CreateEntity("Lamp");
            co.GetRegistry().Emplace<MeshRendererComponent>(lamp).Color = { 1, 1, 1, 1 };
            co.GetRegistry().Emplace<ScriptComponent>(lamp).Add("Game.CoroutineLamp").Fields["TickStep"] = 1.0f;
            auto rt = co.Clone();
            rt->OnRuntimeStart();
            auto color = [&] { return rt->GetRegistry().Get<MeshRendererComponent>(lamp).Color; };
            auto run = [&](int frames) { for (int i = 0; i < frames; ++i) rt->OnUpdateRuntime(1.0f / 60.0f); };
            run(30); // t = 0.5: red (1 s)
            CHECK(Near(color().r, 1.0f) && Near(color().g, 0.0f));
            run(45); // t = 1.25: yellow (0.5 s)
            CHECK(Near(color().r, 1.0f) && Near(color().g, 0.8f));
            run(45); // t = 2.0: green (1 s)
            CHECK(Near(color().r, 0.0f) && Near(color().g, 1.0f));
            run(45); // t = 2.75: red again
            CHECK(Near(color().r, 1.0f) && Near(color().g, 0.0f));
            float x = rt->GetRegistry().Get<TransformComponent>(lamp).Position.x; // one step per 0.25 s tick
            CHECK(x >= 9.5f && x <= 11.5f);
            rt->OnRuntimeStop();
        }
        CSharpRuntime::UnloadScripts();
        CHECK(ScriptRegistry::Find("Game.Spinner") == nullptr);
    }

    void TestAnimatorStateMachine()
    {
        AnimatorController c;
        c.Parameters = { { "Energy", AnimParameterType::Float, 0.0f }, { "Poke", AnimParameterType::Trigger, 0.0f } };
        auto state = [](const char* name, const char* clip, bool loop) { AnimState st; st.Name = name; st.Clip = clip; st.Loop = loop; return st; };
        c.States = { state("Sway", "SwayClip", true), state("Wave", "WaveClip", true), state("Curl", "CurlClip", false) };
        AnimTransition toWave{ 0, 1, false, 1.0f, 0.4f, { { "Energy", AnimConditionOp::Greater, 0.5f } } };
        AnimTransition toSway{ 1, 0, false, 1.0f, 0.4f, { { "Energy", AnimConditionOp::Less, 0.5f } } };
        AnimTransition anyCurl{ -1, 2, false, 1.0f, 0.2f, { { "Poke", AnimConditionOp::IsTrue, 0.0f } } };
        AnimTransition curlBack{ 2, 0, true, 1.0f, 0.4f, {} };
        c.Transitions = { toWave, toSway, anyCurl, curlBack };

        AnimatorRuntime rt;
        std::vector<std::string> entered;
        auto duration = [](const std::string&) { return 2.0f; };
        auto enter = [&](const AnimState& s, float) { entered.push_back(s.Name); };
        auto step = [&](float dt) { return AnimatorStateMachine::Update(c, rt, dt, duration, enter); };

        step(0.1f);
        CHECK(rt.State == 0 && entered.back() == "Sway");      // default state
        rt.Parameters["Energy"] = 1.0f;
        CHECK(step(0.1f) == 0 && entered.back() == "Wave");    // condition transition
        rt.Triggers.insert("Poke");
        CHECK(step(0.1f) == 2 && entered.back() == "Curl");    // any-state trigger wins
        CHECK(rt.Triggers.empty());                             // trigger consumed
        CHECK(step(1.0f) == -1 && rt.State == 2);               // exit time (1.0 = 2 s) not reached yet
        CHECK(step(1.0f) == 3 && entered.back() == "Sway");    // exit time reached
        CHECK(step(0.1f) == 0 && entered.back() == "Wave");    // Energy still high
        rt.Parameters["Energy"] = 0.0f;
        CHECK(step(0.1f) == 1 && entered.back() == "Sway");

        fs::path path = fs::temp_directory_path() / "dimension_test.controller";
        CHECK(c.Save(path));
        auto loaded = AnimatorController::Load(path);
        CHECK(loaded->States.size() == 3 && loaded->Transitions.size() == 4 && loaded->Parameters.size() == 2);
        CHECK(loaded->Transitions[3].HasExitTime && loaded->Transitions[2].From == -1 && !loaded->States[2].Loop);
        fs::remove(path);

        c.RemoveState(1); // removing Wave drops its transitions and reindexes the rest
        CHECK(c.Transitions.size() == 2 && c.Transitions[0].To == 1 && c.Transitions[1].From == 1);
    }

    class CounterA : public ScriptableEntity
    {
    public:
        int Updates = 0;
        int Step = 1;
        void OnUpdate(float) override { Updates += Step; }
    };
    class CounterB : public ScriptableEntity
    {
    public:
        CounterA* Sibling = nullptr;
        void OnCreate() override { Sibling = GetScript<CounterA>(); }
        void OnUpdate(float) override { if (Sibling) Sibling->Step = 10; }
    };

    void TestMultipleScripts()
    {
        Scene scene("Multi");
        auto& reg = scene.GetRegistry();
        Entity e = scene.CreateEntity("Many");
        auto& sc = reg.Emplace<ScriptComponent>(e);
        sc.Add("CounterA");
        sc.Add("CounterB");
        sc.Add("TestScript").Enabled = false;

        // Round trip keeps order and flags.
        auto json = SceneSerializer::Serialize(scene);
        auto loaded = SceneSerializer::Deserialize(json);
        auto& lsc = loaded->GetRegistry().Get<ScriptComponent>(loaded->FindEntityByGuid(scene.GetGuid(e)));
        CHECK(lsc.Scripts.size() == 3 && lsc.Scripts[1].ClassName == "CounterB" && !lsc.Scripts[2].Enabled);

        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        auto& rsc = runtime->GetRegistry().Get<ScriptComponent>(e);
        auto* a = dynamic_cast<CounterA*>(rsc.Scripts[0].Instance.get());
        auto* b = dynamic_cast<CounterB*>(rsc.Scripts[1].Instance.get());
        auto* t = dynamic_cast<TestScript*>(rsc.Scripts[2].Instance.get());
        CHECK(a && b && t && b->Sibling == a);
        runtime->OnUpdateRuntime(0.016f); // A += 1 (step 1), then B sets step 10
        runtime->OnUpdateRuntime(0.016f); // A += 10
        CHECK(a && a->Updates == 11);
        CHECK(t && t->Updates == 0); // disabled script receives no updates
        runtime->OnRuntimeStop();

        // Legacy single-script scene format still loads.
        nlohmann::json legacy = { { "entities", nlohmann::json::array({ { { "guid", 77 }, { "parent", 0 },
            { "components", { { "Tag", { { "Name", "Old" } } }, { "Script", { { "Class", "TestScript" }, { "Fields", { { "Speed", 4.0 } } } } } } } } }) } };
        auto old = SceneSerializer::Deserialize(legacy);
        auto& osc = old->GetRegistry().Get<ScriptComponent>(old->FindEntityByGuid(Guid(uint64_t(77))));
        CHECK(osc.Scripts.size() == 1 && osc.Scripts[0].ClassName == "TestScript" && osc.Scripts[0].Fields["Speed"] == 4.0);
    }

    const char* g_Filter = nullptr; // run only tests whose name contains this (first command-line argument)

    void Run(const char* name, void (*fn)())
    {
        if (g_Filter && !std::strstr(name, g_Filter)) return;
        g_Current = name;
        int before = g_Failures;
        fn();
        std::printf("%s %s\n", g_Failures == before ? "[ OK ]" : "[FAIL]", name);
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep output when a test crashes
    if (argc > 1) g_Filter = argv[1];
    ComponentRegistry::RegisterBuiltins();
    ScriptRegistry::Register<ClickCounter>("ClickCounter");
    ScriptRegistry::Register<CharacterProbe>("CharacterProbe");
    ScriptRegistry::Register<TimerProbe>("TimerProbe");
    ScriptRegistry::Register<TriggerCounter>("TriggerCounter");
    ScriptRegistry::Register<WidgetListener>("WidgetListener");
    ScriptRegistry::Register<CounterA>("CounterA");
    ScriptRegistry::Register<CounterB>("CounterB");
    ScriptRegistry::Register<TestScript>("TestScript").Field("Speed", &TestScript::Speed).Field("Target", &TestScript::Target);

    Run("Registry basics", TestRegistryBasics);
    Run("Views", TestViews);
    Run("Registry clone", TestClonePreservesHandles);
    Run("Scene hierarchy", TestHierarchy);
    Run("Serialization round trip", TestSerializationRoundTrip);
    Run("Duplicate entity", TestDuplicateRemapsReferences);
    Run("Script runtime", TestScriptsRuntime);
    Run("Physics 2D", TestPhysics2D);
    Run("Physics 3D", TestPhysics3D);
    Run("Physics layers, triggers, joints", TestPhysicsLayersTriggersJoints);
    Run("Mesh colliders and characters", TestMeshCollidersAndCharacters);
    Run("Reflection JSON", TestReflectionJson);
    Run("Material round trip", TestMaterialRoundTrip);
    Run("UI layout and clicks", TestUI);
    Run("UI widgets", TestUIWidgets);
    Run("Shadow cascade splits", TestCascadeSplits);
    Run("Point shadow faces", TestPointShadowFaces);
    Run("Animation sampling", TestAnimationSampling);
    Run("Skinned glTF import", TestSkinnedImport);
    Run("glTF materials", TestGltfMaterials);
    Run("Prefab instances", TestPrefabs);
    Run("Scene manager", TestSceneManager);
    Run("Script timers", TestScriptTimers);
    Run("Audio mixer", TestAudioMixer);
    Run("Environment lighting", TestEnvironmentLighting);
    Run("Tilemaps", TestTilemaps);
    Run("Particle simulation", TestParticles);
    Run("Particle modules", TestParticleModules);
    Run("C# scripting", TestCSharpScripts);
    Run("Multiple scripts per entity", TestMultipleScripts);
    Run("Animator state machine", TestAnimatorStateMachine);
    Run("Blend trees and animation events", TestBlendTreesAndEvents);

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
