// Headless engine tests (no GPU/window needed). Run: ctest --preset debug, or DimensionTests.exe
#include <Dimension/Dimension.h>
#include <Dimension/Scene/ComponentRegistry.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/UI/UI.h>
#include <Dimension/Animation/Animation.h>
#include <Dimension/Animation/AnimatorController.h>
#include <Dimension/Renderer/Mesh.h>
#include <Dimension/Scene/Prefab.h>
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

        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        for (int i = 0; i < 60; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
        float yaw = glm::degrees(runtime->GetRegistry().Get<TransformComponent>(e).Rotation.y);
        CHECK(Near(yaw, 90.0f, 1.0f)); // 90 deg/s for one second, applied by C# through the native API
        glm::vec4 c = runtime->GetRegistry().Get<MeshRendererComponent>(r).Color;
        CHECK(!(Near(c.r, 1.0f) && Near(c.g, 1.0f) && Near(c.b, 1.0f))); // recolored from C#
        runtime->OnRuntimeStop();
        runtime.reset();
        CSharpRuntime::UnloadScripts();
        CHECK(ScriptRegistry::Find("Game.Spinner") == nullptr);
    }

    void TestAnimatorStateMachine()
    {
        AnimatorController c;
        c.Parameters = { { "Energy", AnimParameterType::Float, 0.0f }, { "Poke", AnimParameterType::Trigger, 0.0f } };
        c.States = { { "Sway", "SwayClip", 1.0f, true, {} }, { "Wave", "WaveClip", 1.0f, true, {} }, { "Curl", "CurlClip", 1.0f, false, {} } };
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

    void Run(const char* name, void (*fn)())
    {
        g_Current = name;
        int before = g_Failures;
        fn();
        std::printf("%s %s\n", g_Failures == before ? "[ OK ]" : "[FAIL]", name);
    }
}

int main()
{
    ComponentRegistry::RegisterBuiltins();
    ScriptRegistry::Register<ClickCounter>("ClickCounter");
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
    Run("Reflection JSON", TestReflectionJson);
    Run("Material round trip", TestMaterialRoundTrip);
    Run("UI layout and clicks", TestUI);
    Run("Animation sampling", TestAnimationSampling);
    Run("Skinned glTF import", TestSkinnedImport);
    Run("Prefab instances", TestPrefabs);
    Run("Particle simulation", TestParticles);
    Run("C# scripting", TestCSharpScripts);
    Run("Multiple scripts per entity", TestMultipleScripts);
    Run("Animator state machine", TestAnimatorStateMachine);

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
