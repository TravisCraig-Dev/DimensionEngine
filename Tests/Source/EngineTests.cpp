// Headless engine tests (no GPU/window needed). Run: ctest --preset debug, or DimensionTests.exe
#include <Dimension/Core/AppRuntime.h>
#include <Dimension/Dimension.h>
#include <Dimension/Scene/ComponentRegistry.h>
#include <Dimension/Scene/SceneSerializer.h>
#include <Dimension/Renderer/SpriteAtlas.h>
#include <Dimension/Renderer/SpriteSlicing.h>
#include <Dimension/Renderer/Material.h>
#include <Dimension/UI/UI.h>
#include <Dimension/Animation/Animation.h>
#include <Dimension/Animation/AnimatorController.h>
#include <Dimension/Renderer/Mesh.h>
#include <Dimension/Renderer/Environment.h>
#include <Dimension/Renderer/SceneRenderer.h>
#include <Dimension/Scene/Prefab.h>
#include <Dimension/Scene/PathPlacer.h>
#include <Dimension/Scene/SceneSearch.h>
#include <Dimension/Scene/SceneSnapping.h>
#include <Dimension/Scene/SceneVisibility.h>
#include <Dimension/Asset/Preset.h>
#include <Dimension/Asset/ImportDefaults.h>
#include <Dimension/Asset/AssetListing.h>
#include <Dimension/Asset/ProjectFavorites.h>
#include <Dimension/Scene/SceneBounds.h>
#include <Dimension/Scene/SceneMeasure.h>
#include <Dimension/Scene/SelectionHistory.h>
#include <Dimension/Scene/SceneGrouping.h>
#include <Dimension/Scene/ViewAlign.h>
#include <Dimension/Renderer/SceneGrid.h>
#include <Dimension/Reflection/FieldSearch.h>
#include <Dimension/Scene/DebugDraw.h>
#include <Dimension/Core/PlayerPrefs.h>
#include <Dimension/Project/Localization.h>
#include <Dimension/Asset/DataAsset.h>
#include <Dimension/Scene/CameraProjection.h>
#include <Dimension/Project/InputActions.h>
#include <Dimension/Core/Profiler.h>
#include <Dimension/Project/SceneBookmarks.h>
#include <Dimension/Scene/ProjectSearch.h>
#include <Dimension/Asset/AssetPackage.h>
#include <Dimension/Scene/SceneSections.h>
#include <Dimension/Scene/SceneTemplates.h>
#include <Dimension/Project/RecentScenes.h>
#include <Dimension/Project/ProjectHub.h>
#include <Dimension/Scene/SceneDiff.h>
#include <Dimension/Scripting/EditModeScripts.h>
#include <Dimension/Physics/Wind.h>
#include <Dimension/Navigation/NavMesh.h>
#include <Dimension/Scene/LODGroup.h>
#include <Dimension/Renderer/MeshSimplify.h>
#include <Dimension/Renderer/Occlusion.h>
#include <Dimension/Core/Profiler.h>
#include <Dimension/Audio/Convolver.h>
#include <Dimension/Terrain/Terrain.h>
#include <Dimension/Animation/Timeline.h>
#include <Dimension/Audio/Microphone.h>
#include <Dimension/Physics/Water.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Scripting/CSharpRuntime.h>
#include <Dimension/Platform/AppIcon.h>
#include <Dimension/Asset/AssetDependencies.h>
#include <Dimension/Asset/AssetPrefetch.h>
#include <Dimension/Scene/SceneManager.h>
#include <Dimension/Physics/ConvexDecomposition.h>
#include <Dimension/Asset/ImportCache.h>
#include <Dimension/Physics/Cloth.h>
#include <Dimension/Physics/Fracture.h>
#include <Dimension/Physics/Rope.h>
#include <set>
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
#include <chrono>
#include <fstream>
#include <thread>

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
    bool Near(const glm::vec2& a, const glm::vec2& b, float eps = 1e-3f) { return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec2(eps))); }
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
        scene.GetSettings().SSAO = true;
        scene.GetSettings().SSAORadius = 0.8f;
        scene.GetSettings().DepthOfField = true;
        scene.GetSettings().FocusDistance = 12.5f;

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
        CHECK(loaded->GetSettings().SSAO && Near(loaded->GetSettings().SSAORadius, 0.8f) && !loaded->GetSettings().SSAODebug);
        CHECK(loaded->GetSettings().DepthOfField && Near(loaded->GetSettings().FocusDistance, 12.5f) && Near(loaded->GetSettings().FocusRange, 6.0f));
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

    // Records its callbacks as "<object> <callback>" (OnEnable / OnDisable order with SetActive).
    class EnableProbe : public ScriptableEntity
    {
    public:
        static inline std::vector<std::string> Log;
        int Updates = 0;
        void Note(const char* what) { Log.push_back(Name() + " " + what); }
        void OnCreate() override { Note("create"); }
        void OnEnable() override { Note("enable"); }
        void OnDisable() override { Note("disable"); }
        void OnDestroy() override { Note("destroy"); }
        void OnUpdate(float) override { ++Updates; }
    };

    void TestActiveState()
    {
        Scene scene("Active");
        auto& reg = scene.GetRegistry();
        Entity group = scene.CreateEntity("Group");
        Entity child = scene.CreateEntity("Child", group);
        Entity off = scene.CreateEntity("Off");
        reg.Get<TagComponent>(off).Active = false;
        reg.Emplace<ScriptComponent>(child).Add("EnableProbe");
        reg.Emplace<ScriptComponent>(off).Add("EnableProbe");
        reg.Get<TagComponent>(child).Tag = "Enemy";
        reg.Get<TagComponent>(off).Tag = "Enemy";

        // Tags, on any scene.
        CHECK(scene.FindEntityByTag("Enemy") == child && scene.FindEntitiesByTag("Enemy").size() == 2 && scene.FindEntityByTag("None").IsNull());

        EnableProbe::Log.clear();
        auto rt = scene.Clone();
        auto& rreg = rt->GetRegistry();
        auto log = [] { std::string s; for (auto& l : EnableProbe::Log) s += (s.empty() ? "" : ", ") + l; EnableProbe::Log.clear(); return s; };
        rt->OnRuntimeStart();
        // OnEnable after every OnCreate, only for active objects.
        std::string start = log();
        CHECK(start == "Child create, Off create, Child enable" || start == "Off create, Child create, Child enable");
        auto* probe = static_cast<EnableProbe*>(rreg.Get<ScriptComponent>(child).Scripts[0].Instance.get());

        // Turning the parent off disables the child's scripts at once; its updates stop.
        rt->SetActive(group, false);
        CHECK(log() == "Child disable" && !rt->IsActiveInHierarchy(child) && !rreg.Get<TransformComponent>(child).ActiveInHierarchy);
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(probe->Updates == 0 && log().empty());
        rt->SetActive(group, true);
        CHECK(log() == "Child enable");
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(probe->Updates == 1);

        // A script's own checkbox.
        probe->SetEnabled(false);
        CHECK(log() == "Child disable" && !probe->IsEnabled());
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(probe->Updates == 1);
        probe->SetEnabled(true);
        CHECK(log() == "Child enable" && probe->IsEnabled());

        // Moved under an inactive object, then that object turned on: parents first.
        rt->SetParent(child, off);
        CHECK(log() == "Child disable");
        rt->SetActive(off, true);
        CHECK(log() == "Off enable, Child enable");

        // Changed without SetActive (the Inspector's box): told on the next frame.
        rreg.Get<TagComponent>(off).Active = false;
        rt->OnUpdateRuntime(1.0f / 60.0f);
        std::string direct = log();
        CHECK(direct == "Off disable, Child disable" || direct == "Child disable, Off disable");
        rreg.Get<TagComponent>(off).Active = true;
        rt->OnUpdateRuntime(1.0f / 60.0f);
        log();

        // OnDisable before OnDestroy, also when play stops; an object already off only gets OnDestroy.
        rt->DestroyEntity(child);
        CHECK(log() == "Child disable, Child destroy");
        rt->SetActive(off, false);
        log();
        rt->OnRuntimeStop();
        CHECK(log() == "Off destroy");

        // A layer changed while playing reaches the colliders at the next step (raycasts by layer mask see it).
        Scene phys("Layers");
        Entity box3 = phys.CreateEntity("Box3D");
        phys.GetRegistry().Emplace<RigidbodyComponent>(box3).Type = BodyType::Static;
        phys.GetRegistry().Emplace<BoxColliderComponent>(box3);
        Entity box2 = phys.CreateEntity("Box2D");
        phys.GetRegistry().Get<TransformComponent>(box2).Position = { 0, 10, 0 };
        phys.GetRegistry().Emplace<Rigidbody2DComponent>(box2).Type = BodyType::Static;
        phys.GetRegistry().Emplace<BoxCollider2DComponent>(box2);
        auto prt = phys.Clone();
        prt->OnRuntimeStart();
        uint32_t notFive = ~(1u << 5);
        CHECK(Physics3DWorld::Raycast(*prt, { -5, 0, 0 }, { 1, 0, 0 }, 20.0f, notFive).Hit == box3);
        CHECK(Physics2DWorld::Raycast(*prt, { -5, 10 }, { 1, 0 }, 20.0f, notFive).Hit == box2);
        prt->GetRegistry().Get<TagComponent>(box3).Layer = 5;
        prt->GetRegistry().Get<TagComponent>(box2).Layer = 5;
        prt->OnUpdateRuntime(1.0f / 30.0f); // at least one fixed step
        CHECK(Physics3DWorld::Raycast(*prt, { -5, 0, 0 }, { 1, 0, 0 }, 20.0f, notFive).Hit.IsNull());
        CHECK(Physics2DWorld::Raycast(*prt, { -5, 10 }, { 1, 0 }, 20.0f, notFive).Hit.IsNull());
        CHECK(Physics3DWorld::Raycast(*prt, { -5, 0, 0 }, { 1, 0, 0 }, 20.0f, 1u << 5).Hit == box3);
        CHECK(Physics2DWorld::Raycast(*prt, { -5, 10 }, { 1, 0 }, 20.0f, 1u << 5).Hit == box2);
        prt->OnRuntimeStop();
    }

    void TestCameraProjection()
    {
        Scene scene("Cameras");
        auto& reg = scene.GetRegistry();
        Entity cam = scene.CreateEntity("Camera");
        reg.Emplace<CameraComponent>(cam); // 60 degrees, looking down -Z
        reg.Get<TransformComponent>(cam).Position = { 0, 0, 10 };
        reg.Get<TransformComponent>(cam).Scale = { 3, 3, 3 }; // ignored, as by the renderer
        scene.UpdateTransforms();
        glm::vec2 view(800, 600);
        using namespace CameraProjection;

        // The point looked at is the view's center, its distance the depth; up is toward the top row.
        glm::vec3 center = WorldToScreen(scene, cam, { 0, 0, 0 }, view);
        CHECK(Near(center.x, 400.0f) && Near(center.y, 300.0f) && Near(center.z, 10.0f));
        glm::vec3 upRight = WorldToScreen(scene, cam, { 1, 1, 0 }, view);
        CHECK(upRight.x > 400.0f && upRight.y < 300.0f);
        // tan(30 deg) * 10 above the center is the top edge.
        CHECK(Near(WorldToScreen(scene, cam, { 0, std::tan(glm::radians(30.0f)) * 10.0f, 0 }, view).y, 0.0f, 1e-3f));
        CHECK(WorldToScreen(scene, cam, { 0, 0, 20 }, view).z < 0.0f); // behind

        // Round trips through a turned camera, and the ray through a pixel.
        reg.Get<TransformComponent>(cam).Rotation = { -0.6f, 0.9f, 0.1f };
        scene.UpdateTransforms();
        bool same = true;
        for (glm::vec3 p : { glm::vec3(1, 2, 3), glm::vec3(-4, 0.5f, -2), glm::vec3(7, -3, 1) })
        {
            glm::vec3 s = WorldToScreen(scene, cam, p, view);
            if (s.z > 0.0f) same &= glm::length(ScreenToWorld(scene, cam, s, view) - p) < 1e-3f;
        }
        CHECK(same);
        glm::vec3 origin, dir, target(2, -1, 0.5f);
        ScreenPointToRay(scene, cam, glm::vec2(WorldToScreen(scene, cam, target, view)), view, origin, dir);
        glm::vec3 toTarget = glm::normalize(target - origin);
        CHECK(glm::dot(dir, toTarget) > 0.99999f && Near(glm::length(dir), 1.0f));

        // Orthographic: the view is OrthographicSize high (half), its width by the aspect.
        auto& c = reg.Get<CameraComponent>(cam);
        c.Projection = ProjectionType::Orthographic;
        c.OrthographicSize = 5.0f;
        reg.Get<TransformComponent>(cam).Rotation = { 0, 0, 0 };
        scene.UpdateTransforms();
        glm::vec3 corner = ScreenToWorld(scene, cam, { 0, 0, 4 }, view);
        CHECK(Near(corner.x, -5.0f * 800.0f / 600.0f, 1e-3f) && Near(corner.y, 5.0f, 1e-3f) && Near(corner.z, 6.0f, 1e-3f));
        ScreenPointToRay(scene, cam, { 800, 600 }, view, origin, dir);
        CHECK(Near(dir.z, -1.0f) && Near(origin.x, 5.0f * 800.0f / 600.0f, 1e-3f) && Near(origin.y, -5.0f, 1e-3f));
    }

    void TestTimeScaleAndCursor()
    {
        // Time scale: script steps, play time and physics slow down; the unscaled clock does not.
        Scene scene("Time");
        auto& reg = scene.GetRegistry();
        Entity e = scene.CreateEntity("Scripted");
        reg.Emplace<ScriptComponent>(e).Add("TestScript").Fields["Speed"] = 0.0f;
        Entity ball = scene.CreateEntity("Ball");
        reg.Get<TransformComponent>(ball).Position = { 0, 10, 0 };
        reg.Emplace<RigidbodyComponent>(ball);
        reg.Emplace<SphereColliderComponent>(ball);
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        CHECK(Near(rt->GetTimeScale(), 1.0f));
        rt->SetTimeScale(0.5f);
        rt->OnUpdateRuntime(0.0625f);
        CHECK(Near(rt->GetDeltaTime(), 0.03125f) && Near(rt->GetUnscaledDeltaTime(), 0.0625f) && Near(rt->GetTime(), 0.03125f) &&
              Near(rt->GetUnscaledTime(), 0.0625f));
        rt->SetTimeScale(0.0f);
        float y = rt->GetRegistry().Get<TransformComponent>(ball).Position.y;
        for (int i = 0; i < 30; ++i) rt->OnUpdateRuntime(0.0625f);
        CHECK(Near(rt->GetRegistry().Get<TransformComponent>(ball).Position.y, y) && Near(rt->GetTime(), 0.03125f));
        CHECK(Near(rt->GetUnscaledTime(), 0.0625f * 31.0f, 1e-3f));
        rt->SetTimeScale(1.0f);
        rt->OnUpdateRuntime(0.0625f);
        CHECK(rt->GetRegistry().Get<TransformComponent>(ball).Position.y < y); // falling again
        rt->SetTimeScale(-3.0f);
        CHECK(Near(rt->GetTimeScale(), 0.0f));
        rt->SetTimeScale(250.0f);
        CHECK(Near(rt->GetTimeScale(), 100.0f));

        // Cursor: the game's mode applies only with game input and while not released; play stop resets it.
        bool gameInput = Input::IsGameInputEnabled();
        Input::SetGameInputEnabled(true);
        Input::SetCursorMode(CursorMode::Locked);
        CHECK(Input::GetEffectiveCursorMode() == CursorMode::Locked);
        Input::SetCursorReleased(true);
        CHECK(Input::GetEffectiveCursorMode() == CursorMode::Normal && Input::GetCursorMode() == CursorMode::Locked);
        Input::SetCursorReleased(false);
        Input::SetGameInputEnabled(false);
        CHECK(Input::GetEffectiveCursorMode() == CursorMode::Normal);
        Input::SetGameInputEnabled(true);
        rt->OnRuntimeStop();
        CHECK(Input::GetCursorMode() == CursorMode::Normal);
        auto again = scene.Clone();
        again->OnRuntimeStart();
        CHECK(Near(again->GetTimeScale(), 1.0f));
        again->OnRuntimeStop();
        Input::SetGameInputEnabled(gameInput);
    }

    void TestPhysics2DQueries()
    {
        Scene scene("Queries2D");
        auto& reg = scene.GetRegistry();
        Entity boxes[3];
        for (int i = 0; i < 3; ++i)
        {
            boxes[i] = scene.CreateEntity("Box " + std::to_string(i));
            reg.Get<TransformComponent>(boxes[i]).Position = { 2.0f + 3.0f * i, 0, 0 };
            reg.Emplace<BoxCollider2DComponent>(boxes[i]);
        }
        reg.Get<TagComponent>(boxes[1]).Layer = 5;
        Entity disc = scene.CreateEntity("Disc");
        reg.Get<TransformComponent>(disc).Position = { 0, 5, 0 };
        reg.Emplace<CircleCollider2DComponent>(disc);
        Entity spinner = scene.CreateEntity("Spinner"), pushed = scene.CreateEntity("Pushed");
        for (Entity e : { spinner, pushed })
        {
            reg.Get<TransformComponent>(e).Position = { e == spinner ? -10.0f : -20.0f, 20, 0 };
            reg.Emplace<Rigidbody2DComponent>(e).GravityScale = 0.0f;
            reg.Emplace<BoxCollider2DComponent>(e);
        }
        auto rt = scene.Clone();
        rt->OnRuntimeStart();

        // Every hit along a ray, nearest first, with its point, normal and distance; masks leave layers out.
        auto hits = Physics2DWorld::RaycastAll(*rt, { 0, 0 }, { 1, 0 }, 20.0f);
        CHECK(hits.size() == 3 && hits[0].Hit == boxes[0] && hits[1].Hit == boxes[1] && hits[2].Hit == boxes[2]);
        CHECK(hits.size() == 3 && Near(hits[0].Distance, 1.5f) && Near(hits[2].Distance, 7.5f) && Near(hits[1].Point, glm::vec2(4.5f, 0.0f)) &&
              Near(hits[0].Normal, glm::vec2(-1.0f, 0.0f)));
        auto masked = Physics2DWorld::RaycastAll(*rt, { 0, 0 }, { 1, 0 }, 20.0f, ~(1u << 5));
        CHECK(masked.size() == 2 && masked[1].Hit == boxes[2]);
        CHECK(Physics2DWorld::RaycastAll(*rt, { 0, 0 }, { 1, 0 }, 4.0f).size() == 1); // max distance

        // Points inside boxes and circles (not their bounding boxes).
        auto at = [&](glm::vec2 p) { auto v = Physics2DWorld::OverlapPoint(*rt, p); return v.empty() ? Entity::Null() : v[0]; };
        CHECK(at({ 5.0f, 0.2f }) == boxes[1] && at({ 3.5f, 0.0f }).IsNull());
        CHECK(at({ 0.0f, 5.3f }) == disc && at({ 0.4f, 5.4f }).IsNull());
        CHECK(Physics2DWorld::OverlapPoint(*rt, { 5.0f, 0.0f }, ~(1u << 5)).empty());

        // Spin: set directly, or from a torque over a step.
        Physics2DWorld::SetAngularVelocity(*rt, spinner, 2.0f);
        CHECK(Near(Physics2DWorld::GetAngularVelocity(*rt, spinner), 2.0f));
        Physics2DWorld::ApplyTorque(*rt, pushed, 50.0f);
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(Physics2DWorld::GetAngularVelocity(*rt, pushed) > 0.1f);
        rt->OnRuntimeStop();
    }

    void TestPhysics3DCasts()
    {
        Scene scene("Casts");
        auto& reg = scene.GetRegistry();
        Entity boxes[3];
        for (int i = 0; i < 3; ++i)
        {
            boxes[i] = scene.CreateEntity("Box " + std::to_string(i));
            reg.Get<TransformComponent>(boxes[i]).Position = { 3.0f + 3.0f * i, 0, 0 };
            reg.Emplace<BoxColliderComponent>(boxes[i]);
        }
        Entity spinner = scene.CreateEntity("Spinner"), twisted = scene.CreateEntity("Twisted"), struck = scene.CreateEntity("Struck");
        float x = -10.0f;
        for (Entity e : { spinner, twisted, struck })
        {
            reg.Get<TransformComponent>(e).Position = { x, 10, 0 };
            x -= 5.0f;
            reg.Emplace<RigidbodyComponent>(e).UseGravity = false;
            reg.Emplace<BoxColliderComponent>(e);
        }
        auto rt = scene.Clone();
        rt->OnRuntimeStart();

        // A sphere moved along +X touches the first box's face after 2 m (its center 0.5 short of the face).
        RaycastHit s = Physics3DWorld::SphereCast(*rt, { 0, 0, 0 }, 0.5f, { 1, 0, 0 }, 20.0f);
        CHECK(s.Hit == boxes[0] && Near(s.Distance, 2.0f, 0.02f) && Near(s.Point.x, 2.5f, 0.02f) && Near(s.Normal, glm::vec3(-1, 0, 0), 0.02f));
        // Passing 0.2 m over the boxes: a thin sphere misses, a thicker one grazes the first.
        CHECK(Physics3DWorld::SphereCast(*rt, { 0, 1.2f, 0 }, 0.5f, { 1, 0, 0 }, 20.0f).Hit.IsNull());
        CHECK(Physics3DWorld::SphereCast(*rt, { 0, 1.2f, 0 }, 0.8f, { 1, 0, 0 }, 20.0f).Hit == boxes[0]);
        CHECK(Physics3DWorld::SphereCast(*rt, { 0, 0, 0 }, 0.5f, { 1, 0, 0 }, 1.5f).Hit.IsNull()); // max distance
        // Boxes: face on, then turned 45 degrees (its corner leads, half a diagonal ahead).
        RaycastHit b = Physics3DWorld::BoxCast(*rt, { 0, 0, 0 }, glm::vec3(0.5f), glm::quat(1, 0, 0, 0), { 1, 0, 0 }, 20.0f);
        CHECK(b.Hit == boxes[0] && Near(b.Distance, 2.0f, 0.02f));
        RaycastHit turned = Physics3DWorld::BoxCast(*rt, { 0, 0, 0 }, glm::vec3(0.5f), glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0)),
                                                    { 1, 0, 0 }, 20.0f);
        CHECK(turned.Hit == boxes[0] && Near(turned.Distance, 2.5f - std::sqrt(0.5f), 0.02f));
        // Layer masks leave the first box out.
        rt->GetRegistry().Get<TagComponent>(boxes[0]).Layer = 5;
        rt->OnUpdateRuntime(1.0f / 30.0f);
        RaycastHit masked = Physics3DWorld::SphereCast(*rt, { 0, 0, 0 }, 0.5f, { 1, 0, 0 }, 20.0f, ~(1u << 5));
        CHECK(masked.Hit == boxes[1] && Near(masked.Distance, 5.0f, 0.02f));

        // Spin set directly, from a torque over a step, and from an impulse off the center (which also pushes).
        Physics3DWorld::SetAngularVelocity(*rt, spinner, { 0, 2, 0 });
        CHECK(Near(Physics3DWorld::GetAngularVelocity(*rt, spinner), glm::vec3(0, 2, 0), 0.01f));
        Physics3DWorld::AddTorque(*rt, twisted, { 0, 0, 20 });
        glm::vec3 at = rt->GetRegistry().Get<TransformComponent>(struck).Position;
        Physics3DWorld::AddImpulseAtPosition(*rt, struck, { 0, 0, 1 }, at + glm::vec3(0.5f, 0, 0));
        CHECK(Physics3DWorld::GetLinearVelocity(*rt, struck).z > 0.5f && Physics3DWorld::GetAngularVelocity(*rt, struck).y < -0.5f);
        rt->OnUpdateRuntime(1.0f / 30.0f);
        CHECK(Physics3DWorld::GetAngularVelocity(*rt, twisted).z > 0.1f);
        rt->OnRuntimeStop();
    }

    // The collision details a script is given.
    class CollisionProbe : public ScriptableEntity
    {
    public:
        Collision Entered;
        int Enters = 0, Stays = 0, Exits = 0;
        float LastStayImpulse = -1.0f;
        void OnCollisionEnter(const Collision& c) override { Entered = c; ++Enters; }
        void OnCollisionStay(const Collision& c) override { ++Stays; LastStayImpulse = c.Impulse; }
        void OnCollisionExit(const Collision& c) override { ++Exits; }
    };
    // A script written before collision details (only the Entity overload): still called.
    class LegacyCollisionProbe : public ScriptableEntity
    {
    public:
        int Enters = 0, Exits = 0;
        void OnCollisionEnter(Entity) override { ++Enters; }
        void OnCollisionExit(Entity) override { ++Exits; }
    };

    void TestCollisionDetails()
    {
        for (bool twoD : { false, true })
        {
            Scene scene(twoD ? "Collisions2D" : "Collisions3D");
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            reg.Get<TransformComponent>(ground).Scale = { 20, 1, 20 };
            Entity box = scene.CreateEntity("Box");
            reg.Get<TransformComponent>(box).Position = { 0, 3, 0 };
            if (twoD)
            {
                reg.Emplace<BoxCollider2DComponent>(ground);
                reg.Emplace<Rigidbody2DComponent>(box);
                reg.Emplace<BoxCollider2DComponent>(box);
            }
            else
            {
                reg.Emplace<BoxColliderComponent>(ground);
                reg.Emplace<RigidbodyComponent>(box);
                reg.Emplace<BoxColliderComponent>(box);
            }
            auto& sc = reg.Emplace<ScriptComponent>(box);
            sc.Add("CollisionProbe");
            sc.Add("LegacyCollisionProbe");
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& rsc = rt->GetRegistry().Get<ScriptComponent>(box);
            auto* probe = static_cast<CollisionProbe*>(rsc.Scripts[0].Instance.get());
            auto* legacy = static_cast<LegacyCollisionProbe*>(rsc.Scripts[1].Instance.get());
            for (int i = 0; i < 180 && probe->Enters == 0; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);

            // Landing: on the ground's top, its normal up toward the box, falling at about sqrt(2 g h) = 6.6 m/s.
            const Collision& c = probe->Entered;
            CHECK(probe->Enters == 1 && legacy->Enters == 1 && c.Other == ground);
            CHECK(Near(c.Normal, glm::vec3(0, 1, 0), 0.01f) && !c.Contacts.empty());
            bool onTop = !c.Contacts.empty();
            for (auto& p : c.Contacts) onTop &= std::abs(p.Point.y) < 0.1f && Near(p.Normal, glm::vec3(0, 1, 0), 0.01f) && std::abs(p.Point.x) < 0.6f;
            CHECK(onTop);
            CHECK(c.RelativeVelocity.y < -5.0f && c.RelativeVelocity.y > -8.0f);
            CHECK(c.Impulse > 3.0f && c.Impulse < 15.0f); // a 1 kg box stopping from about 6.6 m/s

            // Resting: Stay every step with little impulse, until the box falls asleep (sleeping bodies get none, as in Unity).
            for (int i = 0; i < 90; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(probe->Stays > 10 && probe->LastStayImpulse >= 0.0f && probe->LastStayImpulse < 1.0f);
            CHECK(probe->Exits == 0 && legacy->Exits == 0 && probe->Enters == 1); // falling asleep is not leaving

            // Thrown up: Exit, for both scripts.
            if (twoD) Physics2DWorld::SetLinearVelocity(*rt, box, { 0, 12 });
            else Physics3DWorld::SetLinearVelocity(*rt, box, { 0, 12, 0 });
            for (int i = 0; i < 30 && probe->Exits == 0; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(probe->Exits == 1 && legacy->Exits == 1);
            int stays = probe->Stays;
            rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(probe->Stays == stays); // no Stay while apart
            rt->OnRuntimeStop();
        }
    }

    // Records its OnMouse* callbacks (Over and Drag once per frame are counted, not listed).
    class MouseProbe : public ScriptableEntity
    {
    public:
        std::vector<std::string> Log;
        int Overs = 0, Drags = 0;
        void OnMouseEnter() override { Log.push_back("enter"); }
        void OnMouseOver() override { ++Overs; }
        void OnMouseExit() override { Log.push_back("exit"); }
        void OnMouseDown() override { Log.push_back("down"); }
        void OnMouseDrag() override { ++Drags; }
        void OnMouseUp() override { Log.push_back("up"); }
        void OnMouseUpAsButton() override { Log.push_back("click"); }
        std::string Take() { std::string s; for (auto& l : Log) s += (s.empty() ? "" : " ") + l; Log.clear(); return s; }
    };
    class AppProbe : public ScriptableEntity
    {
    public:
        std::vector<std::string> Log;
        int FixedSteps = 0;
        void OnFixedUpdate(float) override { ++FixedSteps; }
        void OnApplicationFocus(bool f) override { Log.push_back(f ? "focus" : "blur"); }
        void OnApplicationPause(bool p) override { Log.push_back(p ? "pause" : "resume"); }
        void OnApplicationQuit() override { Log.push_back("quit"); }
        void OnDestroy() override { Log.push_back("destroy"); }
        std::string Take() { std::string s; for (auto& l : Log) s += (s.empty() ? "" : " ") + l; Log.clear(); return s; }
    };
    class TriggerStayProbe : public ScriptableEntity
    {
    public:
        int Enters = 0, Stays = 0, Exits = 0;
        void OnTriggerEnter(Entity) override { ++Enters; }
        void OnTriggerStay(Entity) override { ++Stays; }
        void OnTriggerExit(Entity) override { ++Exits; }
    };

    void TestAppAndTiming()
    {
        Scene scene("App");
        Entity e = scene.CreateEntity("Probe");
        scene.GetRegistry().Emplace<ScriptComponent>(e).Add("AppProbe");
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto* probe = static_cast<AppProbe*>(rt->GetRegistry().Get<ScriptComponent>(e).Scripts[0].Instance.get());
        // The fixed step: 1/60 by default, then 1/100 (ten steps in 0.1 s); FixedTime counts the steps.
        CHECK(Near(rt->GetFixedTimestep(), 1.0f / 60.0f, 1e-6f));
        rt->OnUpdateRuntime(0.1f);
        CHECK(probe->FixedSteps == 6 && Near(rt->GetFixedTime(), 0.1f, 1e-4f));
        rt->SetFixedTimestep(0.01f);
        probe->FixedSteps = 0;
        rt->OnUpdateRuntime(0.1f);
        CHECK((probe->FixedSteps == 10 || probe->FixedSteps == 9 || probe->FixedSteps == 11) && rt->GetFixedTime() <= rt->GetTime() + 1e-4f);
        // The longest frame step.
        float t = rt->GetTime();
        rt->SetMaximumDeltaTime(0.05f);
        rt->OnUpdateRuntime(0.5f);
        CHECK(Near(rt->GetTime() - t, 0.05f, 1e-5f));
        rt->SetMaximumDeltaTime(0.1f);
        // Time since the last (single) scene load.
        CHECK(Near(rt->GetTimeSinceLevelLoad(), rt->GetTime(), 1e-6f));
        Scene next("Next");
        rt->LoadSceneData(SceneSerializer::Serialize(next), false);
        rt->OnUpdateRuntime(0.05f);
        CHECK(Near(rt->GetTimeSinceLevelLoad(), 0.05f, 1e-5f) && rt->GetTime() > 0.25f);
        rt->OnRuntimeStop();

        // The window's focus and minimizing reach running scripts; closing calls OnApplicationQuit.
        rt = scene.Clone();
        rt->OnRuntimeStart();
        probe = static_cast<AppProbe*>(rt->GetRegistry().Get<ScriptComponent>(e).Scripts[0].Instance.get());
        AppRuntime::ReportWindowState(true, false);
        CHECK(probe->Take().empty());
        AppRuntime::ReportWindowState(false, false);
        AppRuntime::ReportWindowState(false, true);
        AppRuntime::ReportWindowState(true, false);
        CHECK(probe->Take() == "blur pause resume focus" && AppRuntime::IsFocused() && !AppRuntime::IsMinimized());
        rt->NotifyApplicationQuit();
        CHECK(probe->Take() == "quit"); // (hosts stop the scene after it: OnDisable and OnDestroy follow)
        rt->OnRuntimeStop();
        AppRuntime::ReportWindowState(false, false); // a stopped scene hears nothing (and is not called)
        AppRuntime::ReportWindowState(true, false);

        // Quit requests are taken once; the frame rate setting; the real clock.
        AppRuntime::RequestQuit();
        CHECK(AppRuntime::TakeQuitRequest() && !AppRuntime::TakeQuitRequest());
        AppRuntime::SetTargetFrameRate(30);
        CHECK(AppRuntime::GetTargetFrameRate() == 30);
        AppRuntime::SetTargetFrameRate(0);
        CHECK(AppRuntime::GetTargetFrameRate() == -1);
        double before = AppRuntime::GetRealtimeSinceStartup();
        CHECK(before > 0.0 && AppRuntime::GetRealtimeSinceStartup() >= before);
    }

    void TestMouseEvents()
    {
        Scene scene("Mouse");
        auto& reg = scene.GetRegistry();
        Entity cam = scene.CreateEntity("Camera");
        reg.Emplace<CameraComponent>(cam);
        reg.Get<TransformComponent>(cam).Position = { 0, 0, 10 };
        Entity cube = scene.CreateEntity("Cube"), tile = scene.CreateEntity("Tile"), hidden = scene.CreateEntity("Hidden");
        reg.Emplace<BoxColliderComponent>(cube);
        reg.Get<TransformComponent>(tile).Position = { 3, 0, 0 };
        reg.Emplace<BoxCollider2DComponent>(tile);
        reg.Get<TransformComponent>(hidden).Position = { -3, 0, 0 };
        reg.Emplace<BoxColliderComponent>(hidden);
        for (Entity e : { cube, tile, hidden }) reg.Emplace<ScriptComponent>(e).Add("MouseProbe");
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        rt->SetActive(hidden, false);
        auto probe = [&](Entity e) { return static_cast<MouseProbe*>(rt->GetRegistry().Get<ScriptComponent>(e).Scripts[0].Instance.get()); };
        glm::vec2 view(800, 600);
        auto screenOf = [&](glm::vec3 p) { return glm::vec2(CameraProjection::WorldToScreen(*rt, cam, p, view)); };
        auto frame = [&](glm::vec2 at, bool down, bool valid = true) {
            rt->SetUIInput(at, down, view, valid);
            rt->OnUpdateRuntime(1.0f / 60.0f);
        };
        MouseProbe* c = probe(cube);
        glm::vec2 onCube = screenOf({ 0, 0, 0 }), empty = screenOf({ 0, 3, 0 });

        frame(onCube, false);
        frame(onCube, false);
        CHECK(c->Take() == "enter" && c->Overs == 2 && rt->GetObjectUnderPointer() == cube);
        // Pressed on it, dragged off and released elsewhere: Drag continues off it, Up but no click.
        frame(onCube, true);
        frame(empty, true);
        frame(empty, true);
        frame(empty, false);
        CHECK(c->Take() == "down exit up" && c->Drags == 2 && rt->GetObjectUnderPointer().IsNull());
        // A click on it.
        frame(onCube, true);
        frame(onCube, false);
        CHECK(c->Take() == "enter down up click");
        // Pressed elsewhere and released over it: nothing for it but hovering.
        frame(empty, true);
        frame(onCube, true);
        frame(onCube, false);
        CHECK(c->Take() == "exit enter" && c->Drags == 2);
        // The pointer leaving the view.
        frame(onCube, false, false);
        CHECK(c->Take() == "exit");

        // 2D colliders (in the z = 0 plane), and an inactive object gets nothing.
        frame(screenOf({ 3, 0, 0 }), true);
        frame(screenOf({ 3, 0, 0 }), false);
        CHECK(probe(tile)->Take() == "enter down up click" && rt->GetObjectUnderPointer().IsNull() == false);
        frame(screenOf({ -3, 0, 0 }), true);
        frame(screenOf({ -3, 0, 0 }), false);
        CHECK(probe(hidden)->Take().empty() && probe(hidden)->Overs == 0 && rt->GetObjectUnderPointer().IsNull());
        rt->OnRuntimeStop();
    }

    void TestTriggerStay()
    {
        for (bool twoD : { false, true })
        {
            Scene scene("TriggerStay");
            auto& reg = scene.GetRegistry();
            Entity zone = scene.CreateEntity("Zone");
            reg.Get<TransformComponent>(zone).Scale = { 6, 6, 6 };
            Entity ball = scene.CreateEntity("Ball");
            // (Off the step grid: Box2D v3.1.1 reports a sensor exit for a step where a circle's center lies exactly on the
            // sensor polygon's edge.)
            reg.Get<TransformComponent>(ball).Position = { -5.03f, 0, 0 };
            if (twoD)
            {
                reg.Emplace<BoxCollider2DComponent>(zone).IsTrigger = true;
                reg.Emplace<Rigidbody2DComponent>(ball).GravityScale = 0.0f;
                reg.Emplace<CircleCollider2DComponent>(ball).Radius = 0.25f;
            }
            else
            {
                reg.Emplace<BoxColliderComponent>(zone).IsTrigger = true;
                reg.Emplace<RigidbodyComponent>(ball).UseGravity = false;
                reg.Emplace<SphereColliderComponent>(ball).Radius = 0.25f;
            }
            reg.Emplace<ScriptComponent>(ball).Add("TriggerStayProbe");
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto* probe = static_cast<TriggerStayProbe*>(rt->GetRegistry().Get<ScriptComponent>(ball).Scripts[0].Instance.get());
            // Through the 6 m zone at 4 m/s: about 1.6 s inside.
            if (twoD) Physics2DWorld::SetLinearVelocity(*rt, ball, { 4, 0 });
            else Physics3DWorld::SetLinearVelocity(*rt, ball, { 4, 0, 0 });
            for (int i = 0; i < 180; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(probe->Enters == 1 && probe->Exits == 1);
            CHECK(probe->Stays > 70 && probe->Stays < 110);
            rt->OnRuntimeStop();
        }
    }

    class CloneProbe : public ScriptableEntity
    {
    public:
        float Speed = 1.0f;
        Entity Target;
        static inline int Created = 0;
        void OnCreate() override { ++Created; }
    };

    void TestInstantiateAndRemove()
    {
        Scene scene("Clones");
        auto& reg = scene.GetRegistry();
        Entity crate = scene.CreateEntity("Crate");
        reg.Get<TransformComponent>(crate).Position = { 0, 5, 0 };
        reg.Emplace<RigidbodyComponent>(crate);
        reg.Emplace<BoxColliderComponent>(crate);
        Entity lid = scene.CreateEntity("Lid", crate), outside = scene.CreateEntity("Outside");
        reg.Get<TransformComponent>(lid).Position = { 0, 0.6f, 0 };
        auto& lidScripts = reg.Emplace<ScriptComponent>(lid);
        lidScripts.Add("CloneProbe").Fields["Target"] = (uint64_t)scene.GetGuid(crate);
        lidScripts.Add("CloneProbe").Fields["Target"] = (uint64_t)scene.GetGuid(outside);
        lidScripts.Add("EnableProbe");
        Entity shelf = scene.CreateEntity("Shelf");
        reg.Get<TransformComponent>(shelf).Position = { 10, 0, 0 };
        reg.Get<TransformComponent>(shelf).Rotation = { 0, glm::radians(90.0f), 0 };
        Entity wall = scene.CreateEntity("Wall");
        reg.Get<TransformComponent>(wall).Position = { -10, 0, -20 };
        reg.Emplace<BoxColliderComponent>(wall);

        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        CloneProbe::Created = 0;
        // A live value the scene does not have saved.
        static_cast<CloneProbe*>(rr.Get<ScriptComponent>(lid).Scripts[0].Instance.get())->Speed = 9.0f;

        glm::vec3 at(5, 3, 0);
        glm::quat turned = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
        Entity copy = rt->Instantiate(crate, Entity::Null(), &at, &turned);
        CHECK(!copy.IsNull() && copy != crate && rt->GetChildren(copy).size() == 1 && rt->GetParent(copy).IsNull());
        CHECK(Near(rr.Get<TransformComponent>(copy).GetWorldPosition(), at) && Near(rr.Get<TransformComponent>(copy).Rotation.y, glm::radians(90.0f)));
        Entity copyLid = rt->GetChildren(copy)[0];
        auto& copied = rr.Get<ScriptComponent>(copyLid).Scripts;
        auto* first = static_cast<CloneProbe*>(copied[0].Instance.get());
        auto* second = static_cast<CloneProbe*>(copied[1].Instance.get());
        // Started (OnCreate), with the live value, a reference inside the copy pointing to the copy, one outside kept.
        CHECK(CloneProbe::Created == 2 && first && Near(first->Speed, 9.0f) && first->Target == copy && second->Target == outside);
        // Its body works: it falls from where it was put.
        for (int i = 0; i < 30; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        glm::vec3 fallen = rr.Get<TransformComponent>(copy).Position;
        CHECK(fallen.y < 2.8f && Near(fallen.x, 5.0f, 0.05f));
        // Under a parent, at the original's world pose by default (the parent turns it, the local values make up for it).
        Entity onShelf = rt->Instantiate(outside, shelf);
        CHECK(rt->GetParent(onShelf) == shelf && Near(rr.Get<TransformComponent>(onShelf).GetWorldPosition(), rr.Get<TransformComponent>(outside).GetWorldPosition()));

        // Removing a script: at the end of the frame, after OnDisable and before nothing else of it runs.
        EnableProbe::Log.clear();
        auto* probe = copied[2].Instance.get();
        CHECK(rt->RemoveScriptDeferred(copyLid, probe) && rr.Get<ScriptComponent>(copyLid).Scripts.size() == 3);
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(rr.Get<ScriptComponent>(copyLid).Scripts.size() == 2);
        CHECK(EnableProbe::Log.size() == 2 && EnableProbe::Log[0] == "Lid disable" && EnableProbe::Log[1] == "Lid destroy");
        // Removing the copy's Rigidbody: its body is made again, static (it stops falling); a collider: no more hits.
        CHECK(!rt->RemoveComponentDeferred(copy, "Transform") && !rt->RemoveComponentDeferred(copy, "Camera"));
        CHECK(rt->RemoveComponentDeferred(copy, "Rigidbody"));
        rt->OnUpdateRuntime(1.0f / 60.0f);
        float y = rr.Get<TransformComponent>(copy).Position.y;
        for (int i = 0; i < 20; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(rr.Get<RigidbodyComponent>(copy).Type == BodyType::Static && Near(rr.Get<TransformComponent>(copy).Position.y, y));
        CHECK(Physics3DWorld::Raycast(*rt, { -10, 0, 0 }, { 0, 0, -1 }, 50.0f).Hit == wall);
        CHECK(rt->RemoveComponentDeferred(wall, "Box Collider"));
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(Physics3DWorld::Raycast(*rt, { -10, 0, 0 }, { 0, 0, -1 }, 50.0f).Hit.IsNull() && !rr.Has<BoxColliderComponent>(wall));
        rt->OnRuntimeStop();
    }

    void TestPhysicsQueriesAndTriggers()
    {
        auto near3 = [](glm::vec3 a, glm::vec3 b, float eps) { return glm::distance(a, b) < eps; };
        // 3D: a floor, a trigger box over its middle, and a crate further along +Z.
        {
            Scene scene("Queries3D");
            auto& reg = scene.GetRegistry();
            auto box = [&](const char* name, glm::vec3 at, glm::vec3 size, bool trigger) {
                Entity e = scene.CreateEntity(name);
                reg.Get<TransformComponent>(e).Position = at;
                auto& c = reg.Emplace<BoxColliderComponent>(e);
                c.Size = size;
                c.IsTrigger = trigger;
                return e;
            };
            Entity floor = box("Floor", { 0, -0.5f, 0 }, { 20, 1, 20 }, false);
            Entity zone = box("Zone", { 0, 1, 0 }, { 1, 1, 1 }, true);
            Entity crate = box("Crate", { 0, 1, 5 }, { 1, 1, 1 }, false);
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            rt->OnUpdateRuntime(1.0f / 60.0f);
            Scene& s = *rt;
            // A ray down through the trigger: it counts, or the floor is hit.
            RaycastHit hit = Physics3DWorld::Raycast(s, { 0, 5, 0 }, { 0, -1, 0 }, 10.0f);
            CHECK(hit.Hit == zone && Near(hit.Distance, 3.5f, 0.02f));
            hit = Physics3DWorld::Raycast(s, { 0, 5, 0 }, { 0, -1, 0 }, 10.0f, Physics::kDefaultQueryMask, false);
            CHECK(hit.Hit == floor && Near(hit.Distance, 5.0f, 0.02f));
            CHECK(Physics3DWorld::RaycastAll(s, { 0, 5, 0 }, { 0, -1, 0 }, 10.0f, Physics::kDefaultQueryMask, false, false).size() == 1);
            // A standing capsule moved along +Z meets the trigger first, else the crate.
            hit = Physics3DWorld::CapsuleCast(s, { 0, 0.8f, -5 }, { 0, 1.6f, -5 }, 0.3f, { 0, 0, 1 }, 20.0f);
            CHECK(hit.Hit == zone && Near(hit.Distance, 4.2f, 0.05f));
            hit = Physics3DWorld::CapsuleCast(s, { 0, 0.8f, -5 }, { 0, 1.6f, -5 }, 0.3f, { 0, 0, 1 }, 20.0f, Physics::kDefaultQueryMask, false);
            CHECK(hit.Hit == crate && Near(hit.Distance, 9.2f, 0.05f));
            // A lying capsule (ends along X) fits between the trigger and crate; one reaching both overlaps both.
            auto found = Physics3DWorld::OverlapCapsule(s, { -1, 1, 2.5f }, { 1, 1, 2.5f }, 0.4f);
            CHECK(found.empty());
            found = Physics3DWorld::OverlapCapsule(s, { 0, 1, 0 }, { 0, 1, 5 }, 0.2f);
            CHECK(found.size() == 2);
            found = Physics3DWorld::OverlapCapsule(s, { 0, 1, 0 }, { 0, 1, 5 }, 0.2f, Physics::kDefaultQueryMask, false);
            CHECK(found.size() == 1 && found[0] == crate);
            CHECK(Physics3DWorld::OverlapSphere(s, { 0, 1, 0 }, 0.2f, Physics::kDefaultQueryMask, false).empty());
            // Closest point and bounds.
            glm::vec3 c;
            CHECK(Physics3DWorld::ClosestPoint(s, crate, { 0, 1, 10 }, c) && near3(c, { 0, 1, 5.5f }, 0.02f));
            // (an edge: Jolt's boxes are rounded there by their convex radius, 5 cm)
            CHECK(Physics3DWorld::ClosestPoint(s, crate, { 3, 3, 5 }, c) && near3(c, { 0.5f, 1.5f, 5 }, 0.04f));
            CHECK(Physics3DWorld::ClosestPoint(s, crate, { 0.1f, 1.2f, 5 }, c) && near3(c, { 0.1f, 1.2f, 5 }, 1e-4f));
            glm::vec3 lo, hi;
            CHECK(Physics3DWorld::GetBounds(s, crate, lo, hi) && near3(lo, { -0.5f, 0.5f, 4.5f }, 0.05f) && near3(hi, { 0.5f, 1.5f, 5.5f }, 0.05f));
            rt->OnRuntimeStop();
        }
        // 2D: the same with a trigger circle.
        {
            Scene scene("Queries2D");
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            reg.Emplace<BoxCollider2DComponent>(ground).Size = { 20, 1 };
            Entity zone = scene.CreateEntity("Zone");
            reg.Get<TransformComponent>(zone).Position = { 0, 1, 0 };
            auto& cc = reg.Emplace<CircleCollider2DComponent>(zone);
            cc.Radius = 0.5f;
            cc.IsTrigger = true;
            Entity crate = scene.CreateEntity("Crate");
            reg.Get<TransformComponent>(crate).Position = { 5, 1, 0 };
            reg.Emplace<BoxCollider2DComponent>(crate).Size = { 1, 1 };
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            rt->OnUpdateRuntime(1.0f / 60.0f);
            Scene& s = *rt;
            RaycastHit2D hit = Physics2DWorld::Raycast(s, { 0, 5 }, { 0, -1 }, 10.0f);
            CHECK(hit.Hit == zone && Near(hit.Distance, 3.5f, 0.02f));
            hit = Physics2DWorld::Raycast(s, { 0, 5 }, { 0, -1 }, 10.0f, ~(1u << 1), false);
            CHECK(hit.Hit == ground && Near(hit.Distance, 5.0f, 0.02f));
            hit = Physics2DWorld::CircleCast(s, { -5, 1 }, 0.25f, { 1, 0 }, 20.0f);
            CHECK(hit.Hit == zone && Near(hit.Distance, 4.25f, 0.05f));
            hit = Physics2DWorld::CircleCast(s, { -5, 1 }, 0.25f, { 1, 0 }, 20.0f, ~(1u << 1), false);
            CHECK(hit.Hit == crate && Near(hit.Distance, 9.25f, 0.05f) && Near(hit.Point.x, 4.5f, 0.05f));
            hit = Physics2DWorld::BoxCast(s, { 5, 5 }, { 0.25f, 0.25f }, 0.0f, { 0, -1 }, 10.0f);
            CHECK(hit.Hit == crate && Near(hit.Distance, 3.25f, 0.05f));
            CHECK(Physics2DWorld::OverlapCircle(s, { 0, 1 }, 0.2f).size() == 1 &&
                  Physics2DWorld::OverlapCircle(s, { 0, 1 }, 0.2f, ~(1u << 1), false).empty());
            CHECK(Physics2DWorld::OverlapPoint(s, { 0, 1 }, ~(1u << 1), false).empty());
            glm::vec2 c;
            CHECK(Physics2DWorld::ClosestPoint(s, crate, { 10, 1 }, c) && Near(c.x, 5.5f, 0.02f) && Near(c.y, 1.0f, 0.02f));
            CHECK(Physics2DWorld::ClosestPoint(s, crate, { 5.2f, 1.1f }, c) && Near(c.x, 5.2f, 1e-4f) && Near(c.y, 1.1f, 1e-4f));
            glm::vec2 lo, hi;
            CHECK(Physics2DWorld::GetBounds(s, crate, lo, hi) && Near(lo.x, 4.5f, 0.05f) && Near(hi.y, 1.5f, 0.05f));
            rt->OnRuntimeStop();
        }
    }

    void TestRigidbodySettingsWhilePlaying()
    {
        const float dt = 1.0f / 60.0f;
        // 3D
        {
            Scene scene("Bodies3D");
            auto& reg = scene.GetRegistry();
            auto crate = [&](const char* name, glm::vec3 at, BodyType type) {
                Entity e = scene.CreateEntity(name);
                reg.Get<TransformComponent>(e).Position = at;
                reg.Emplace<RigidbodyComponent>(e).Type = type;
                reg.Emplace<BoxColliderComponent>(e);
                return e;
            };
            Entity a = crate("A", { 0, 10, 0 }, BodyType::Dynamic);
            Entity paddle = crate("Paddle", { -3, 30, 0 }, BodyType::Kinematic);
            Entity ball = crate("Ball", { 0, 30, 0 }, BodyType::Dynamic);
            Entity lone = scene.CreateEntity("Lone"); // a collider alone: given a static body
            reg.Get<TransformComponent>(lone).Position = { 10, 10, 0 };
            reg.Emplace<BoxColliderComponent>(lone);
            reg.Get<RigidbodyComponent>(ball).UseGravity = false;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& rr = rt->GetRegistry();
            auto pos = [&](Entity e) { return rr.Get<TransformComponent>(e).Position; };
            auto run = [&](int n) { for (int i = 0; i < n; ++i) rt->OnUpdateRuntime(dt); };
            auto set = [&](Entity e, auto&& change) { change(rr.Get<RigidbodyComponent>(e)); rt->RefreshBody(e); };
            run(20);
            CHECK(pos(a).y < 9.8f);
            // Kinematic: it stops where it is; Static too; Dynamic again: it falls.
            set(a, [](auto& rb) { rb.Type = BodyType::Kinematic; });
            float held = pos(a).y;
            run(20);
            CHECK(Near(pos(a).y, held, 1e-3f));
            set(a, [](auto& rb) { rb.Type = BodyType::Static; });
            run(20);
            CHECK(Near(pos(a).y, held, 1e-3f));
            set(a, [](auto& rb) { rb.Type = BodyType::Dynamic; });
            run(20);
            CHECK(pos(a).y < held - 0.3f);
            // No gravity: it keeps its speed; frozen Y position: it slides along X only; frozen rotation: no spin.
            set(a, [](auto& rb) { rb.UseGravity = false; });
            Physics3DWorld::SetLinearVelocity(*rt, a, { 0, 0, 0 });
            held = pos(a).y;
            run(20);
            CHECK(Near(pos(a).y, held, 1e-3f));
            set(a, [](auto& rb) { rb.UseGravity = true; rb.Constraints = 4 | 112; });
            Physics3DWorld::SetLinearVelocity(*rt, a, { 2, 0, 0 });
            Physics3DWorld::SetAngularVelocity(*rt, a, { 0, 3, 0 });
            float x0 = pos(a).x;
            run(30);
            CHECK(Near(pos(a).y, held, 1e-3f) && pos(a).x > x0 + 0.5f && glm::length(rr.Get<TransformComponent>(a).Rotation) < 1e-3f);
            // Mass: the same push moves a heavier body slower.
            set(a, [](auto& rb) { rb.Constraints = 0; rb.UseGravity = false; rb.Mass = 10.0f; });
            Physics3DWorld::SetLinearVelocity(*rt, a, { 0, 0, 0 });
            Physics3DWorld::AddImpulse(*rt, a, { 0, 0, 10 });
            CHECK(Near(Physics3DWorld::GetLinearVelocity(*rt, a).z, 1.0f, 0.05f));
            // Teleport.
            rt->MoveBody(a, { 5, 50, 5 }, glm::vec3(0.0f), true);
            Physics3DWorld::SetLinearVelocity(*rt, a, { 0, 0, 0 });
            run(1);
            CHECK(glm::distance(pos(a), glm::vec3(5, 50, 5)) < 0.05f);
            // A kinematic paddle moved toward a floating ball pushes it on.
            for (int i = 0; i < 60; ++i)
            {
                glm::vec3 p = pos(paddle);
                rt->MoveBody(paddle, p + glm::vec3(4.0f * dt, 0, 0), glm::vec3(0.0f), false);
                rt->OnUpdateRuntime(dt);
            }
            CHECK(pos(paddle).x > 0.9f && pos(ball).x > 1.5f && Physics3DWorld::GetLinearVelocity(*rt, ball).x > 0.5f);
            // A lone collider's static body made dynamic falls.
            set(lone, [](auto& rb) { rb.Type = BodyType::Dynamic; });
            run(20);
            CHECK(pos(lone).y < 9.8f);
            rt->OnRuntimeStop();
        }
        // 2D
        {
            Scene scene("Bodies2D");
            auto& reg = scene.GetRegistry();
            auto box = [&](const char* name, glm::vec3 at, glm::vec2 size, BodyType type) {
                Entity e = scene.CreateEntity(name);
                reg.Get<TransformComponent>(e).Position = at;
                reg.Emplace<Rigidbody2DComponent>(e).Type = type;
                reg.Emplace<BoxCollider2DComponent>(e).Size = size;
                return e;
            };
            Entity a = box("A", { 20, 10, 0 }, { 1, 1 }, BodyType::Dynamic);
            Entity platform = box("Platform", { 0, 0, 0 }, { 6, 0.5f }, BodyType::Kinematic);
            Entity rider = box("Rider", { 0, 0.76f, 0 }, { 1, 1 }, BodyType::Dynamic);
            reg.Get<BoxCollider2DComponent>(rider).Friction = 1.0f;
            reg.Get<BoxCollider2DComponent>(platform).Friction = 1.0f;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& rr = rt->GetRegistry();
            auto* world = rt->GetPhysics2D();
            auto pos = [&](Entity e) { return rr.Get<TransformComponent>(e).Position; };
            auto run = [&](int n) { for (int i = 0; i < n; ++i) rt->OnUpdateRuntime(dt); };
            auto set = [&](Entity e, auto&& change) { change(rr.Get<Rigidbody2DComponent>(e)); rt->RefreshBody(e); };
            run(30);
            CHECK(pos(a).y < 9.8f && Near(pos(rider).y, 0.75f, 0.05f));
            set(a, [](auto& rb) { rb.Type = BodyType::Kinematic; });
            float held = pos(a).y;
            run(20);
            CHECK(Near(pos(a).y, held, 1e-3f));
            set(a, [](auto& rb) { rb.Type = BodyType::Dynamic; rb.GravityScale = -1.0f; });
            run(20);
            CHECK(pos(a).y > held + 0.3f);
            // Mass from density, and set by scaling it.
            CHECK(Near(world->GetMass(a), 1.0f, 1e-3f));
            world->SetMass(a, 4.0f);
            CHECK(Near(world->GetMass(a), 4.0f, 1e-3f) && Near(rr.Get<BoxCollider2DComponent>(a).Density, 4.0f, 1e-3f));
            // A kinematic body given a velocity moves (and its Transform with it).
            set(a, [](auto& rb) { rb.Type = BodyType::Kinematic; });
            Physics2DWorld::SetLinearVelocity(*rt, a, { 1, 0 });
            float x0 = pos(a).x;
            run(30);
            CHECK(Near(pos(a).x, x0 + 0.5f, 0.02f));
            // Teleport.
            rt->MoveBody(a, { 30, 3, 0 }, { 0, 0, 1.0f }, true);
            Physics2DWorld::SetLinearVelocity(*rt, a, { 0, 0 });
            run(1);
            CHECK(Near(pos(a).x, 30.0f, 1e-3f) && Near(rr.Get<TransformComponent>(a).Rotation.z, 1.0f, 1e-3f));
            // A kinematic platform moved along carries the box riding on it, and stops when it is left alone.
            for (int i = 0; i < 60; ++i)
            {
                glm::vec3 p = pos(platform);
                rt->MoveBody(platform, p + glm::vec3(2.0f * dt, 0, 0), glm::vec3(0.0f), false);
                rt->OnUpdateRuntime(dt);
            }
            CHECK(Near(pos(platform).x, 2.0f, 0.02f) && pos(rider).x > 1.5f && Near(pos(rider).y, 0.75f, 0.05f));
            run(10);
            CHECK(Near(pos(platform).x, 2.0f, 0.02f) && glm::length(Physics2DWorld::GetLinearVelocity(*rt, platform)) < 1e-4f);
            // Fixed rotation.
            set(rider, [](auto& rb) { rb.FixedRotation = true; });
            float angle = rr.Get<TransformComponent>(rider).Rotation.z;
            Physics2DWorld::SetAngularVelocity(*rt, rider, 5.0f);
            Physics2DWorld::ApplyTorque(*rt, rider, 50.0f);
            run(10);
            CHECK(Near(rr.Get<TransformComponent>(rider).Rotation.z, angle, 1e-3f));
            rt->OnRuntimeStop();
        }
    }

    void TestJointRebuildWhilePlaying()
    {
        const float dt = 1.0f / 60.0f;
        // 3D: a crate pinned to the world by a point joint; broken, mended, then joined to a second crate.
        {
            Scene scene("Joints3D");
            auto& reg = scene.GetRegistry();
            auto crate = [&](const char* name, glm::vec3 at) {
                Entity e = scene.CreateEntity(name);
                reg.Get<TransformComponent>(e).Position = at;
                reg.Emplace<RigidbodyComponent>(e).Type = BodyType::Dynamic;
                reg.Emplace<BoxColliderComponent>(e).Size = glm::vec3(0.5f);
                return e;
            };
            Entity a = crate("A", { 0, 5, 0 }), b = crate("B", { 2, 5, 0 });
            reg.Emplace<JointComponent>(a).Type = JointType::Point;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& rr = rt->GetRegistry();
            auto pos = [&](Entity e) { return rr.Get<TransformComponent>(e).Position; };
            auto run = [&](int n) { for (int i = 0; i < n; ++i) rt->OnUpdateRuntime(dt); };
            run(30);
            CHECK(Near(pos(a).y, 5.0f, 0.05f) && pos(b).y < 4.5f); // A held, B falls
            // Breaks under its own weight with a tiny break force; Reconnect (RebuildJoint) mends it where it is now.
            rr.Get<JointComponent>(a).BreakForce = 1.0f;
            Physics3DWorld::WakeUp(*rt, a); // hanging still it sleeps, and a sleeping joint feels no force
            run(10);
            CHECK(rr.Get<JointComponent>(a).Broken && pos(a).y < 5.0f);
            rr.Get<JointComponent>(a).BreakForce = 0.0f;
            rt->RebuildJoint(a);
            float heldAt = pos(a).y;
            run(30);
            CHECK(!rr.Get<JointComponent>(a).Broken && Near(pos(a).y, heldAt, 0.1f) && rt->GetPhysics3D()->GetJointCount() == 1);
            // Joined to B instead of the world: both fall, kept their distance apart.
            rr.Get<JointComponent>(a).ConnectedBody = b;
            rt->RebuildJoint(a);
            float apart = glm::distance(pos(a), pos(b));
            run(30);
            CHECK(pos(a).y < heldAt - 1.0f && Near(glm::distance(pos(a), pos(b)), apart, 0.1f) && rt->GetPhysics3D()->GetJointCount() == 1);
            // The component gone: rebuilding removes the joint.
            rr.Remove<JointComponent>(a);
            rt->RebuildJoint(a);
            CHECK(rt->GetPhysics3D()->GetJointCount() == 0);
            rt->OnRuntimeStop();
        }
        // 2D: the same with a hinge.
        {
            Scene scene("Joints2D");
            auto& reg = scene.GetRegistry();
            auto box = [&](const char* name, glm::vec3 at) {
                Entity e = scene.CreateEntity(name);
                reg.Get<TransformComponent>(e).Position = at;
                reg.Emplace<Rigidbody2DComponent>(e).Type = BodyType::Dynamic;
                reg.Emplace<BoxCollider2DComponent>(e).Size = glm::vec2(0.5f);
                return e;
            };
            Entity a = box("A", { 0, 5, 0 }), b = box("B", { 2, 5, 0 });
            reg.Emplace<Joint2DComponent>(a).Type = Joint2DType::Hinge;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& rr = rt->GetRegistry();
            auto pos = [&](Entity e) { return rr.Get<TransformComponent>(e).Position; };
            auto run = [&](int n) { for (int i = 0; i < n; ++i) rt->OnUpdateRuntime(dt); };
            run(30);
            CHECK(Near(pos(a).y, 5.0f, 0.05f) && pos(b).y < 4.5f && rt->GetPhysics2D()->GetJointCount() == 1);
            rr.Get<Joint2DComponent>(a).BreakForce = 0.1f;
            run(10);
            CHECK(rr.Get<Joint2DComponent>(a).Broken && rt->GetPhysics2D()->GetJointCount() == 0);
            rr.Get<Joint2DComponent>(a).BreakForce = 0.0f;
            rt->RebuildJoint(a);
            float heldAt = pos(a).y;
            run(30);
            CHECK(!rr.Get<Joint2DComponent>(a).Broken && Near(pos(a).y, heldAt, 0.1f) && rt->GetPhysics2D()->GetJointCount() == 1);
            rr.Get<Joint2DComponent>(a).ConnectedBody = b;
            rt->RebuildJoint(a);
            run(30);
            CHECK(pos(a).y < heldAt - 1.0f && rt->GetPhysics2D()->GetJointCount() == 1);
            rr.Remove<Joint2DComponent>(a);
            rt->RebuildJoint(a);
            CHECK(rt->GetPhysics2D()->GetJointCount() == 0);
            rt->OnRuntimeStop();
        }
    }

    void TestCameraSwitching()
    {
        Scene scene("Cameras");
        auto& reg = scene.GetRegistry();
        Entity a = scene.CreateEntity("A"), b = scene.CreateEntity("B");
        reg.Emplace<CameraComponent>(a).Primary = true;
        reg.Emplace<CameraComponent>(b).Primary = false;
        scene.UpdateTransforms();
        CHECK(scene.GetPrimaryCamera() == a && scene.GetCameraCuts() == 0); // the first camera shown is no cut
        scene.SetPrimaryCamera(b);
        CHECK(scene.GetPrimaryCamera() == b && scene.GetCameraCuts() == 1 && !reg.Get<CameraComponent>(a).Primary);
        CHECK(scene.GetPrimaryCamera() == b && scene.GetCameraCuts() == 1); // asking again is no cut
        // The primary one switched off: the game shows the first active camera, another cut.
        reg.Get<TagComponent>(b).Active = false;
        scene.UpdateTransforms();
        CHECK(scene.GetPrimaryCamera() == a && scene.GetCameraCuts() == 2);
        scene.SetPrimaryCamera(Entity::Null()); // not a camera: nothing changes
        CHECK(!reg.Get<CameraComponent>(a).Primary && reg.Get<CameraComponent>(b).Primary);
    }

    void TestPhysicsSettingsWhilePlaying()
    {
        const float dt = 1.0f / 60.0f;
        // 3D: a crate resting on a floor.
        {
            Scene scene("Physics3D");
            auto& reg = scene.GetRegistry();
            Entity floor = scene.CreateEntity("Floor");
            reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
            reg.Emplace<BoxColliderComponent>(floor).Size = { 20, 1, 20 };
            Entity crate = scene.CreateEntity("Crate");
            reg.Get<TransformComponent>(crate).Position = { 0, 0.5f, 0 };
            reg.Emplace<RigidbodyComponent>(crate).Type = BodyType::Dynamic;
            reg.Emplace<BoxColliderComponent>(crate);
            reg.Get<TagComponent>(crate).Layer = 3;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto* world = rt->GetPhysics3D();
            auto y = [&] { return rt->GetRegistry().Get<TransformComponent>(crate).Position.y; };
            auto run = [&](int steps) { for (int i = 0; i < steps; ++i) rt->OnUpdateRuntime(dt); };
            run(120); // settles and sleeps
            CHECK(Near(y(), 0.5f, 0.05f) && Physics3DWorld::IsSleeping(*rt, crate));

            // Gravity turned up: the sleeping crate wakes and rises.
            world->SetGravity({ 0, 9.81f, 0 });
            run(20);
            CHECK(y() > 0.6f && Near(world->GetGravity().y, 9.81f, 1e-4f) && rt->GetSettings().Gravity.y > 0.0f);
            world->SetGravity({ 0, -9.81f, 0 });
            run(120);
            CHECK(Near(y(), 0.5f, 0.05f));

            // Layers 3 and 0 no longer collide: it falls through (the project's matrix is left alone).
            world->SetLayersCollide(3, 0, false);
            run(30);
            CHECK(y() < 0.0f && !world->GetLayersCollide(0, 3) && Project::GetConfig().LayersCollide(3, 0));
            world->SetLayersCollide(3, 0, true);
            Physics3DWorld::SetLinearVelocity(*rt, crate, { 0, 0, 0 });
            rt->GetRegistry().Get<TransformComponent>(crate).Position.y = 2.0f;
            rt->UpdateTransforms();
            world->RebuildBody(crate);
            run(120);
            CHECK(Near(y(), 0.5f, 0.05f));

            // An ignored pair: the resting crate drops through that floor at once.
            world->SetIgnoreCollision(crate, floor, true);
            CHECK(world->IsCollisionIgnored(floor, crate));
            run(30);
            CHECK(y() < 0.0f);
            world->SetIgnoreCollision(crate, floor, false);
            CHECK(!world->IsCollisionIgnored(crate, floor));

            // A collider changed while playing: a bigger box rests higher, keeping the Rigidbody's mass; a trigger
            // lets things through.
            Physics3DWorld::SetLinearVelocity(*rt, crate, { 0, 0, 0 });
            rt->GetRegistry().Get<TransformComponent>(crate).Position.y = 2.0f;
            rt->UpdateTransforms();
            world->RebuildBody(crate);
            run(60);
            rt->GetRegistry().Get<BoxColliderComponent>(crate).Size = { 2, 2, 2 };
            world->RefreshColliders(crate);
            run(90);
            CHECK(Near(y(), 1.0f, 0.06f));
            rt->GetRegistry().Get<BoxColliderComponent>(floor).IsTrigger = true;
            world->RefreshColliders(floor);
            run(30);
            CHECK(y() < 0.5f);
            rt->OnRuntimeStop();
        }

        // 2D: the same on a Box2D floor.
        {
            Scene scene("Physics2D");
            auto& reg = scene.GetRegistry();
            Entity floor = scene.CreateEntity("Floor");
            reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
            reg.Emplace<BoxCollider2DComponent>(floor).Size = { 20, 1 };
            Entity box = scene.CreateEntity("Box");
            reg.Get<TransformComponent>(box).Position = { 0, 0.5f, 0 };
            reg.Emplace<Rigidbody2DComponent>(box).Type = BodyType::Dynamic;
            reg.Emplace<BoxCollider2DComponent>(box);
            reg.Get<TagComponent>(box).Layer = 3;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto* world = rt->GetPhysics2D();
            auto y = [&] { return rt->GetRegistry().Get<TransformComponent>(box).Position.y; };
            auto run = [&](int steps) { for (int i = 0; i < steps; ++i) rt->OnUpdateRuntime(dt); };
            auto reset = [&] {
                rt->GetRegistry().Get<TransformComponent>(box).Position.y = 2.0f;
                rt->UpdateTransforms();
                world->RebuildBody(box);
                run(120);
            };
            run(150);
            CHECK(Near(y(), 0.5f, 0.05f));
            world->SetGravity({ 0, 9.81f });
            run(20);
            CHECK(y() > 0.6f && Near(world->GetGravity().y, 9.81f, 1e-4f));
            world->SetGravity({ 0, -9.81f });
            reset();
            CHECK(Near(y(), 0.5f, 0.05f));
            world->SetLayersCollide(0, 3, false);
            run(30);
            CHECK(y() < 0.0f && !world->GetLayersCollide(3, 0));
            world->SetLayersCollide(0, 3, true);
            reset();
            world->SetIgnoreCollision(floor, box, true);
            run(30);
            CHECK(y() < 0.0f && world->IsCollisionIgnored(box, floor));
            world->SetIgnoreCollision(floor, box, false);
            reset();
            rt->GetRegistry().Get<BoxCollider2DComponent>(box).Size = { 2, 2 };
            world->RefreshColliders(box);
            run(90);
            CHECK(Near(y(), 1.0f, 0.06f));
            rt->OnRuntimeStop();
        }
    }

    void TestTilemapEditsAndTerrainSample()
    {
        // Cells, bounds, revisions and cell <-> world.
        {
            TilemapComponent tm;
            tm.CellSize = 0.5f;
            glm::ivec2 lo, hi;
            CHECK(!tm.CellBounds(lo, hi));
            uint32_t r0 = tm.Revision();
            tm.SetTile(-2, 3, 1);
            tm.SetTile(4, -1, 2);
            uint32_t r1 = tm.Revision();
            tm.SetTile(4, -1, 2); // the same tile: nothing changes
            tm.SetTile(7, 7, -1); // clearing an empty cell: nothing either
            CHECK(r1 != r0 && tm.Revision() == r1);
            CHECK(tm.CellBounds(lo, hi) && lo == glm::ivec2(-2, -1) && hi == glm::ivec2(4, 3));
            glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(2, 1, 0));
            glm::vec3 c = tm.CellToWorld(world, { 4, -1 });
            CHECK(Near(c.x, 2.0f + 2.25f, 1e-5f) && Near(c.y, 1.0f - 0.25f, 1e-5f));
            CHECK(tm.WorldToCell(world, c) == glm::ivec2(4, -1) && tm.WorldToCell(world, { 1.99f, 0.99f, 3.0f }) == glm::ivec2(-1, -1));
            tm.Data = "0,0,5;"; // replaced (as by loading or undo)
            CHECK(tm.GetTile(0, 0) == 5 && tm.Revision() != r1);
        }

        // Tiles painted or cleared while playing: the collider follows from the next step.
        {
            Scene scene("Tiles");
            Entity map = scene.CreateEntity("Map");
            auto& tm = scene.GetRegistry().Emplace<TilemapComponent>(map);
            tm.SetTile(0, 0, 0);
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& live = rt->GetRegistry().Get<TilemapComponent>(map);
            auto down = [&](float x) { return Physics2DWorld::Raycast(*rt, { x, 5.0f }, { 0.0f, -1.0f }, 10.0f); };
            const float dt = 1.0f / 60.0f;
            rt->OnUpdateRuntime(dt);
            CHECK(down(0.5f).Hit == map && !down(4.5f));
            live.SetTile(4, 0, 1);
            live.SetTile(0, 0, -1);
            rt->OnUpdateRuntime(dt);
            CHECK(!down(0.5f) && down(4.5f).Hit == map && Near(down(4.5f).Point.y, 1.0f, 1e-3f));
            live.Clear();
            rt->OnUpdateRuntime(dt);
            CHECK(!down(4.5f));
            live.SetTile(2, 1, 0); // painted again after it had no tiles at all
            rt->OnUpdateRuntime(dt);
            CHECK(down(2.5f).Hit == map && Near(down(2.5f).Point.y, 2.0f, 1e-3f));
            rt->OnRuntimeStop();
        }

        // The ground under a point, its normal, and outside the terrain.
        {
            TerrainComponent terrain;
            terrain.Size = { 40.0f, 40.0f };
            terrain.Resolution = 64;
            terrain.Height = 10.0f;
            glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(10, 5, 0));
            glm::vec3 ground, normal;
            CHECK(Terrain::Sample(terrain, world, { 13.0f, 100.0f, 2.0f }, ground, normal));
            auto heights = Terrain::GetHeights(terrain);
            CHECK(Near(ground.x, 13.0f, 1e-4f) && Near(ground.z, 2.0f, 1e-4f) && Near(ground.y, 5.0f + Terrain::HeightAt(terrain, heights, 3.0f, 2.0f), 1e-4f));
            glm::vec3 hit;
            CHECK(Terrain::Raycast(terrain, world, { 13.0f, 100.0f, 2.0f }, { 0, -1, 0 }, hit) && Near(hit.y, ground.y, 0.02f));
            CHECK(Near(glm::length(normal), 1.0f, 1e-4f) && normal.y > 0.0f);
            // A tilted terrain's normal turns with it.
            glm::vec3 flatN;
            terrain.Height = 0.0f;
            glm::mat4 tilted = glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0, 0, 1));
            CHECK(Terrain::Sample(terrain, tilted, { 0.0f, 3.0f, 0.0f }, ground, flatN) && Near(flatN.x, -0.5f, 1e-3f) && Near(flatN.y, 0.866f, 1e-3f));
            CHECK(!Terrain::Sample(terrain, world, { 31.0f, 0.0f, 0.0f }, ground, normal));
        }
    }

    void TestParticleEmitAndPause()
    {
        Scene scene("Particles");
        auto& reg = scene.GetRegistry();
        auto make = [&](const char* name, glm::vec3 pos, float yawDegrees) {
            Entity e = scene.CreateEntity(name);
            auto& t = reg.Get<TransformComponent>(e);
            t.Position = pos;
            t.Rotation = { 0.0f, glm::radians(yawDegrees), 0.0f };
            auto& ps = reg.Emplace<ParticleSystemComponent>(e);
            ps.PlayOnAwake = false;
            ps.RateOverTime = 0.0f;
            ps.ColorOverLifetime = Gradient::Linear(glm::vec4(1.0f), glm::vec4(1.0f));
            return e;
        };
        Entity world = make("World", { 0, 0, 0 }, 0.0f);
        Entity local = make("Local", { 10, 0, 0 }, 90.0f);
        reg.Get<ParticleSystemComponent>(local).SimulationSpace = ParticleSpace::Local;
        scene.UpdateTransforms();
        const float dt = 0.125f;

        // Emit with start values of its own: given ones replace the system's, the rest are its own.
        ParticleEmitParams p;
        p.HasPosition = p.HasVelocity = p.HasColor = p.HasLifetime = true;
        p.Position = { 5, 0, 0 };
        p.Velocity = { 0, 2, 0 };
        p.Color = { 1, 0, 0, 1 };
        p.Lifetime = 3.0f;
        auto& wp = reg.Get<ParticleSystemComponent>(world);
        wp.Emit(p, 3);
        ParticleSystems::Update(scene, dt);
        CHECK(wp.Particles.size() == 3);
        bool ok = true;
        for (const Particle& q : wp.Particles)
            ok &= glm::length(glm::vec2(q.Position.x - 5.0f, q.Position.z)) < wp.ShapeRadius + 1e-3f && Near(q.Position.y, 0.25f, 0.01f) &&
                  Near(q.Velocity.y, 2.0f, 1e-4f) && q.Color == glm::vec4(1, 0, 0, 1) && q.Lifetime == 3.0f &&
                  q.Size >= wp.StartSize.x && q.Size <= wp.StartSize.y;
        CHECK(ok);

        // A local-space system keeps a world velocity in its own frame (turned 90 degrees: world +X is its +Z).
        auto& lp = reg.Get<ParticleSystemComponent>(local);
        ParticleEmitParams v;
        v.HasVelocity = true;
        v.Velocity = { 1, 0, 0 };
        lp.Emit(v, 1);
        ParticleSystems::Update(scene, dt);
        glm::vec3 worldVel = lp.Particles.empty() ? glm::vec3(0.0f) : glm::vec3(reg.Get<TransformComponent>(local).World * glm::vec4(lp.Particles[0].Velocity, 0.0f));
        CHECK(lp.Particles.size() == 1 && Near(worldVel.x, 1.0f, 1e-4f) && Near(worldVel.z, 0.0f, 1e-4f));

        // Pause freezes it (no emitting, no moving, no aging); Play goes on from there.
        wp.Clear();
        wp.RateOverTime = 40.0f;
        wp.Play();
        ParticleSystems::Update(scene, dt);
        size_t count = wp.Particles.size();
        CHECK(count == 5);
        wp.Pause();
        glm::vec3 held = wp.Particles[0].Position;
        float heldAge = wp.Particles[0].Age, heldTime = wp.Time;
        for (int i = 0; i < 5; ++i) ParticleSystems::Update(scene, dt);
        CHECK(wp.Particles.size() == count && wp.Particles[0].Position == held && wp.Particles[0].Age == heldAge && wp.Time == heldTime);
        wp.Play();
        ParticleSystems::Update(scene, dt);
        CHECK(!wp.Paused && wp.Playing && Near(wp.Time, heldTime + dt, 1e-4f) && wp.Particles.size() == count + 5);
        // Stop forgets a pause; Restart (play mode starting) too.
        wp.Pause();
        wp.Stop();
        CHECK(!wp.Paused && !wp.Playing);
        wp.Pause();
        ParticleSystems::Restart(scene);
        CHECK(!wp.Paused && wp.PendingCustom.empty());

        // Max Particles caps emitting from code too.
        wp.Clear();
        wp.Stop();
        wp.MaxParticles = 4;
        wp.Emit(p, 10);
        ParticleSystems::Update(scene, dt);
        CHECK(wp.Particles.size() == 4 && wp.PendingCustom.empty());
    }

    void TestSpriteAnimatorSeek()
    {
        Scene scene("Sprites");
        Entity coin = scene.CreateEntity("Coin");
        auto& reg = scene.GetRegistry();
        reg.Emplace<SpriteRendererComponent>(coin);
        auto& anim = reg.Emplace<SpriteAnimatorComponent>(coin);
        anim.FramesPerSecond = 10.0f;
        anim.FirstFrame = 2;
        anim.FrameCount = 4;
        anim.Loop = true;
        anim.Playing = false;
        auto frame = [&](Scene& s) { return s.GetRegistry().Get<SpriteRendererComponent>(coin).Frame; };

        // Seeking shows the frame at once, also while stopped; looping wraps.
        scene.SetSpriteAnimatorTime(coin, 0.25f);
        CHECK(frame(scene) == 4 && Near(anim.Time, 0.25f, 1e-5f));
        scene.SetSpriteAnimatorTime(coin, 0.65f);
        CHECK(frame(scene) == 2 + 2);
        scene.SetSpriteAnimatorTime(coin, -3.0f);
        CHECK(frame(scene) == 2 && anim.Time == 0.0f);

        // Without looping, a seek past the end shows the last frame and stops it.
        anim.Loop = false;
        anim.Playing = true;
        scene.SetSpriteAnimatorTime(coin, 5.0f);
        CHECK(frame(scene) == 5 && !anim.Playing);

        // Playing goes on from a seek.
        anim.Loop = true;
        anim.Playing = true;
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        rt->SetSpriteAnimatorTime(coin, 0.15f);
        rt->OnUpdateRuntime(0.1f);
        auto& ra = rt->GetRegistry().Get<SpriteAnimatorComponent>(coin);
        CHECK(Near(ra.Time, 0.25f, 1e-4f) && frame(*rt) == 4);
        rt->OnRuntimeStop();
    }

    void TestAudioSourceControl()
    {
        AudioEngine::Init();
        if (!AudioEngine::IsAvailable())
        {
            std::printf("  skipped: no audio device\n");
            return;
        }
        std::error_code ec;
        fs::path assets = fs::temp_directory_path() / "dimension_audio_control" / "Assets";
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        for (const char* name : { "Tone.opus", "Tone2.opus" }) fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Tests" / "Data" / "Tone.opus", assets / name);
        AssetManager::Init(assets);
        AssetHandle tone = AssetManager::GetHandle(assets / "Tone.opus"), other = AssetManager::GetHandle(assets / "Tone2.opus");
        Scene scene("Audio");
        Entity speaker = scene.CreateEntity("Speaker");
        auto& src = scene.GetRegistry().Emplace<AudioSourceComponent>(speaker);
        src.Clip = tone;
        src.PlayOnAwake = false;
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto wait = [](float seconds) { std::this_thread::sleep_for(std::chrono::milliseconds((int)(seconds * 1000.0f))); };
        float length = AudioWorld::GetLength(*rt, speaker);
        CHECK(length > 0.3f);

        // Pause holds the position; UnPause goes on from it.
        AudioWorld::Play(*rt, speaker);
        wait(0.1f);
        AudioWorld::Pause(*rt, speaker);
        float held = AudioWorld::GetTime(*rt, speaker);
        CHECK(held > 0.03f && AudioWorld::IsPaused(*rt, speaker) && !AudioWorld::IsPlaying(*rt, speaker));
        wait(0.1f);
        CHECK(Near(AudioWorld::GetTime(*rt, speaker), held, 0.005f));
        AudioWorld::UnPause(*rt, speaker);
        CHECK(AudioWorld::IsPlaying(*rt, speaker) && !AudioWorld::IsPaused(*rt, speaker));
        // Seeking, and Stop forgets a pause.
        AudioWorld::SetTime(*rt, speaker, 0.0f);
        CHECK(AudioWorld::GetTime(*rt, speaker) < 0.03f);
        AudioWorld::SetTime(*rt, speaker, 1000.0f); // clamped into the clip
        CHECK(AudioWorld::GetTime(*rt, speaker) <= length);
        AudioWorld::Pause(*rt, speaker);
        AudioWorld::Stop(*rt, speaker);
        AudioWorld::UnPause(*rt, speaker);
        CHECK(!AudioWorld::IsPaused(*rt, speaker) && !AudioWorld::IsPlaying(*rt, speaker));

        // Looping keeps it playing past its end; off, it ends.
        AudioWorld::SetLoop(*rt, speaker, true);
        AudioWorld::Play(*rt, speaker);
        wait(length + 0.15f);
        CHECK(AudioWorld::IsPlaying(*rt, speaker) && rt->GetRegistry().Get<AudioSourceComponent>(speaker).Loop);
        AudioWorld::SetLoop(*rt, speaker, false);
        wait(length + 0.15f);
        CHECK(!AudioWorld::IsPlaying(*rt, speaker));

        // Another clip: it stops, Play plays the new one.
        AudioWorld::Play(*rt, speaker);
        AudioWorld::SetClip(*rt, speaker, other);
        CHECK(rt->GetRegistry().Get<AudioSourceComponent>(speaker).Clip == other && !AudioWorld::IsPlaying(*rt, speaker));
        AudioWorld::Play(*rt, speaker);
        CHECK(AudioWorld::IsPlaying(*rt, speaker));
        AudioWorld::Stop(*rt, speaker);

        // A one-shot at a point, without a source: released once it ends.
        AudioWorld::PlayClipAtPoint(*rt, tone, { 3, 0, 0 });
        CHECK(rt->GetAudio()->GetOneShotCount() == 1);
        wait(length + 0.15f);
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(rt->GetAudio()->GetOneShotCount() == 0);
        rt->OnRuntimeStop();
        rt.reset();
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestMeshImportSettings()
    {
        fs::path models = fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models";
        MeshData plain, big;
        MeshImportSettings twice;
        twice.Scale = 2.0f;
        CHECK(twice.Key() != MeshImportSettings{}.Key() && MeshImportSettings{}.Key().empty());
        CHECK(MeshData::LoadFromFile(models / "Tentacle.gltf", plain) && MeshData::LoadFromFile(models / "Tentacle.gltf", big, twice));
        CHECK(plain.Vertices.size() == big.Vertices.size() && Near(big.Vertices[7].Position, plain.Vertices[7].Position * 2.0f, 1e-4f));
        // The bind pose still skins every vertex onto itself, and animated translations scale too.
        Pose bind;
        bind.SetBindPose(*big.SkeletonData);
        std::vector<glm::mat4> skin;
        Animation::ComputeSkinMatrices(*big.SkeletonData, bind, skin);
        bool identity = true;
        for (auto& m : skin)
            for (int c = 0; c < 4; ++c) identity &= Near(glm::vec3(m[c]), glm::vec3(glm::mat4(1.0f)[c]), 1e-3f);
        CHECK(identity);
        int b1 = plain.SkeletonData->FindJoint("Bone1");
        CHECK(Near(big.SkeletonData->Joints[b1].Translation, plain.SkeletonData->Joints[b1].Translation * 2.0f, 1e-5f));
        // Without animations or materials.
        MeshImportSettings bare;
        bare.ImportAnimations = false;
        bare.ImportMaterials = false;
        MeshData showcase, stripped;
        CHECK(MeshData::LoadFromFile(models / "Showcase.glb", showcase) && MeshData::LoadFromFile(models / "Showcase.glb", stripped, bare));
        CHECK(!showcase.Materials.empty() && stripped.Materials.empty() && stripped.Images.empty());
        CHECK(std::all_of(stripped.SubMeshes.begin(), stripped.SubMeshes.end(), [](const SubMesh& m) { return m.MaterialSlot == -1; }));
        MeshData still;
        CHECK(MeshData::LoadFromFile(models / "Tentacle.gltf", still, bare) && still.Clips.empty() && !plain.Clips.empty());
        // Leaving out single clips by name.
        if (plain.Clips.size() >= 2)
        {
            MeshImportSettings some;
            some.ExcludedClips = { plain.Clips[0].Name };
            MeshData fewer;
            CHECK(!some.IsDefault() && some.Key() != MeshImportSettings{}.Key());
            CHECK(MeshData::LoadFromFile(models / "Tentacle.gltf", fewer, some) && fewer.Clips.size() == plain.Clips.size() - 1);
            CHECK(std::none_of(fewer.Clips.begin(), fewer.Clips.end(), [&](const AnimationClip& c) { return c.Name == plain.Clips[0].Name; }));
        }
        else
            CHECK(false); // the Tentacle model has several clips
    }

    void TestImportCache()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_import_cache";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "Assets", ec);
        fs::path src = fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets";
        for (const char* f : { "Models/Tentacle.gltf", "Models/Torus.obj", "Textures/ClearSky.hdr", "Textures/Brick.png" })
        {
            fs::create_directories((dir / "Assets" / f).parent_path(), ec);
            fs::copy_file(src / f, dir / "Assets" / f, ec);
        }
        CHECK(Project::Open(dir));
        CHECK(ImportCache::Directory() == dir / ".dimension" / "Cache" / "Import");
        auto time = [](auto f) { auto t0 = std::chrono::steady_clock::now(); f(); return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count(); };

        // Meshes: the second load reads the cached import (same data, tangents and all) and is faster.
        for (const char* f : { "Models/Tentacle.gltf", "Models/Torus.obj" })
        {
            MeshData first, second;
            float cold = time([&] { CHECK(MeshData::LoadFromFile(dir / "Assets" / f, first)); });
            // The first read of a new cache file can wait on a virus scanner; time the next one.
            CHECK(MeshData::LoadFromFile(dir / "Assets" / f, second));
            float warm = time([&] { CHECK(MeshData::LoadFromFile(dir / "Assets" / f, second)); });
            bool same = first.Vertices.size() == second.Vertices.size() && first.Indices == second.Indices && first.Name == second.Name &&
                        std::memcmp(first.Vertices.data(), second.Vertices.data(), first.Vertices.size() * sizeof(Vertex)) == 0 &&
                        first.Clips.size() == second.Clips.size() && first.Materials.size() == second.Materials.size() &&
                        (first.SkeletonData != nullptr) == (second.SkeletonData != nullptr);
            if (same && first.SkeletonData)
                same = first.SkeletonData->Joints.size() == second.SkeletonData->Joints.size() && second.SkeletonData->Order == first.SkeletonData->Order &&
                       first.Clips[0].Channels.size() == second.Clips[0].Channels.size() && first.Clips[0].Channels[0].Values == second.Clips[0].Channels[0].Values;
            CHECK(same);
            std::printf("  %s: import %.1f ms, cached %.1f ms%c", f, cold, warm, 10);
            CHECK(warm < cold);
        }
        // Images, HDR included.
        auto hdr = DecodedImage::Decode(dir / "Assets/Textures/ClearSky.hdr");
        auto hdr2 = DecodedImage::Decode(dir / "Assets/Textures/ClearSky.hdr");
        CHECK(hdr && hdr2 && hdr->IsHdr() && hdr2->Width == hdr->Width && hdr2->Rgba32f == hdr->Rgba32f);
        auto png = DecodedImage::Decode(dir / "Assets/Textures/Brick.png");
        CHECK(png && !png->IsHdr() && DecodedImage::Decode(dir / "Assets/Textures/Brick.png")->Rgba8 == png->Rgba8);
        size_t entries = 0;
        for (auto& e : fs::directory_iterator(ImportCache::Directory(), ec)) entries += e.is_regular_file();
        CHECK(entries == 4);

        // An edited file gets a fresh import; ClearUnused drops the stale entry (and nothing live).
        fs::last_write_time(dir / "Assets/Models/Torus.obj", fs::last_write_time(dir / "Assets/Models/Torus.obj") + std::chrono::seconds(5));
        MeshData again;
        CHECK(MeshData::LoadFromFile(dir / "Assets/Models/Torus.obj", again) && !again.Vertices.empty());
        CHECK(ImportCache::ClearUnused(dir / "Assets") == 1);
        entries = 0;
        for (auto& e : fs::directory_iterator(ImportCache::Directory(), ec)) entries += e.is_regular_file();
        CHECK(entries == 4);
        // A damaged entry is ignored (and re-imported over).
        for (auto& e : fs::directory_iterator(ImportCache::Directory(), ec))
            if (e.path().extension() == ".mesh") fs::resize_file(e.path(), 10, ec);
        MeshData repaired;
        CHECK(MeshData::LoadFromFile(dir / "Assets/Models/Tentacle.gltf", repaired) && repaired.SkeletonData != nullptr);
        AssetManager::Shutdown();
        fs::remove_all(dir, ec);
    }

    void TestLightClusters()
    {
        // Camera at the origin looking down -Z, 60 degree vertical field of view, 16:9, near 0.1, far 100.
        glm::mat4 view(1.0f), proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        LightClusters c;
        std::vector<glm::vec4> lights = {
            { 0, 0, -10, 1 },     // ahead, small: only central tiles around depth 10
            { 0, 0, 10, 2 },      // behind the camera: nowhere
            { 0, 0, -500, 5 },    // past the far plane: nowhere
            { 0, 0, 0, 3 },       // around the camera: every tile of the near slices
            { 30, 0, -10, 1 },    // off to the right, outside the view: nowhere (or edge tiles only)
        };
        c.Build(lights, view, proj);
        CHECK(Near(c.Near, 0.1f, 1e-3f) && Near(c.Far, 100.0f, 0.1f) && !c.Orthographic);
        auto cellsWith = [&](uint32_t light) {
            std::vector<glm::ivec3> out;
            for (int s = 0; s < LightClusters::kSlices; ++s)
                for (int y = 0; y < LightClusters::kTilesY; ++y)
                    for (int x = 0; x < LightClusters::kTilesX; ++x)
                    {
                        glm::uvec2 cell = c.Cells[LightClusters::CellIndex(x, y, s)];
                        for (uint32_t k = 0; k < cell.y; ++k)
                            if (c.Indices[cell.x + k] == light) out.push_back({ x, y, s });
                    }
            return out;
        };
        auto ahead = cellsWith(0);
        bool central = !ahead.empty();
        for (auto& cell : ahead)
            central &= cell.x >= 6 && cell.x <= 9 && cell.y >= 3 && cell.y <= 5 && cell.z >= c.SliceOf(8.9f) && cell.z <= c.SliceOf(11.1f);
        CHECK(central && ahead.size() < 60);
        CHECK(cellsWith(1).empty() && cellsWith(2).empty() && cellsWith(4).empty());
        auto around = cellsWith(3);
        CHECK(around.size() == (size_t)LightClusters::kTilesX * LightClusters::kTilesY * (c.SliceOf(3.0f) + 1));
        // Without culling every cluster lists every light (the comparison mode).
        c.Build(lights, view, proj, false);
        CHECK(c.Indices.size() == lights.size() * LightClusters::kCount);
        // Orthographic views slice depth linearly.
        c.Build(lights, view, glm::ortho(-10.0f, 10.0f, -5.0f, 5.0f, 0.5f, 50.0f));
        CHECK(c.Orthographic && Near(c.Near, 0.5f, 1e-3f) && Near(c.Far, 50.0f, 1e-2f) && c.SliceOf(27.25f) == 12 && !cellsWith(0).empty());
    }

    void TestInactiveColliders()
    {
        // An inactive object's collider is out of the world (rays pass, bodies fall through) until it is activated.
        Scene scene("Inactive");
        auto& reg = scene.GetRegistry();
        Entity box3 = scene.CreateEntity("Box3D");
        reg.Emplace<BoxColliderComponent>(box3);
        Entity box2 = scene.CreateEntity("Box2D");
        reg.Get<TransformComponent>(box2).Position = { 10, 0, 0 };
        reg.Emplace<BoxCollider2DComponent>(box2);
        for (Entity e : { box3, box2 }) reg.Get<TagComponent>(e).Active = false;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto hit3 = [&] { return (bool)Physics3DWorld::Raycast(*rt, { 0, 5, 0 }, { 0, -1, 0 }, 10.0f); };
        auto hit2 = [&] { return !Physics2DWorld::Raycast(*rt, { 10, 5 }, { 0, -1 }, 10.0f).Hit.IsNull(); };
        CHECK(!hit3() && !hit2());
        for (Entity e : { box3, box2 }) rt->GetRegistry().Get<TagComponent>(e).Active = true;
        rt->OnUpdateRuntime(1.0f / 30.0f);
        CHECK(hit3() && hit2());
        for (Entity e : { box3, box2 }) rt->GetRegistry().Get<TagComponent>(e).Active = false;
        rt->OnUpdateRuntime(1.0f / 30.0f);
        CHECK(!hit3() && !hit2());
        rt->OnRuntimeStop();
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
        auto all = Physics3DWorld::RaycastAll(*runtime, { 0, 10, 0 }, { 0, -1, 0 }, 100.0f); // through the ball into the ground
        CHECK(all.size() == 2 && all[0].Hit == ball && all[1].Hit == ground && all[0].Distance < all[1].Distance && Near(all[1].Point.y, 0.5f, 0.01f));
        runtime->OnRuntimeStop();
    }

    void TestConvexDecomposition()
    {
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets"); // CPU mesh data only
        AssetHandle torusHandle = AssetManager::GetHandle(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Torus.obj");
        auto torus = AssetManager::GetMeshData(torusHandle);
        CHECK(torus != nullptr);
        if (!torus) return;
        auto parts = ConvexDecomposition::Decompose(torus, 16);
        CHECK(parts && parts->size() >= 4 && parts->size() <= 16);
        // Every part is convex (all its points behind each face) and they cover the ring: the hole stays empty.
        bool convex = true, holeClear = true;
        glm::vec3 lo(1e9f), hi(-1e9f);
        for (const ConvexPart& part : *parts)
        {
            for (size_t k = 0; k + 2 < part.Triangles.size(); k += 3)
            {
                glm::vec3 a = part.Points[part.Triangles[k]], b = part.Points[part.Triangles[k + 1]], c = part.Points[part.Triangles[k + 2]];
                glm::vec3 n = glm::cross(b - a, c - a);
                if (glm::length(n) < 1e-9f) continue;
                n = glm::normalize(n);
                for (const glm::vec3& p : part.Points) convex &= glm::dot(p - a, n) < 1e-3f;
            }
            for (const glm::vec3& p : part.Points)
            {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            // No part reaches the ring's middle (hole radius 0.42).
            glm::vec3 c(0.0f);
            for (const glm::vec3& p : part.Points) c += p / (float)part.Points.size();
            holeClear &= glm::length(glm::vec2(c.x, c.z)) > 0.4f;
        }
        CHECK(convex && holeClear);
        CHECK(Near(lo, { -0.98f, -0.28f, -0.98f }, 0.05f) && Near(hi, { 0.98f, 0.28f, 0.98f }, 0.05f));
        CHECK(ConvexDecomposition::Decompose(torus, 16) == parts); // cached

        // A ring dropped over a thin pole: decomposed, it slides down around the pole to the ground; as one convex
        // hull it would sit on top of the pole.
        auto drop = [&](bool decompose) {
            Scene scene("Ring Toss");
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            reg.Get<TransformComponent>(ground).Scale = { 20, 1, 20 };
            reg.Emplace<BoxColliderComponent>(ground);
            Entity pole = scene.CreateEntity("Pole");
            reg.Get<TransformComponent>(pole).Position = { 0, 1.5f, 0 };
            reg.Get<TransformComponent>(pole).Scale = { 0.2f, 3.0f, 0.2f };
            reg.Emplace<BoxColliderComponent>(pole);
            Entity ring = scene.CreateEntity("Ring");
            reg.Get<TransformComponent>(ring).Position = { 0, 4.0f, 0 };
            reg.Emplace<RigidbodyComponent>(ring);
            auto& mc = reg.Emplace<MeshColliderComponent>(ring);
            mc.Mesh = torusHandle;
            mc.ConvexDecomposition = decompose;
            auto runtime = scene.Clone();
            runtime->OnRuntimeStart();
            for (int i = 0; i < 180; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
            float y = runtime->GetRegistry().Get<TransformComponent>(ring).Position.y;
            runtime->OnRuntimeStop();
            return y;
        };
        float hullY = drop(false), partsY = drop(true);
        CHECK(hullY > 2.8f);  // resting on the pole's top (3 units high)
        CHECK(partsY < 0.6f); // around the pole on the ground
        AssetManager::Shutdown();
    }

    void TestCloth()
    {
        auto run = [](Ref<Scene>& rt, float seconds) { for (int i = 0; i < (int)(seconds * 60.0f); ++i) rt->OnUpdateRuntime(1.0f / 60.0f); };
        // A curtain pinned along its top edge, rotated to hang in the XZ plane at first (lying flat): it swings down.
        {
            Scene scene("Curtain");
            auto& reg = scene.GetRegistry();
            Entity e = scene.CreateEntity("Curtain");
            reg.Get<TransformComponent>(e).Position = { 0, 3, 0 };
            reg.Get<TransformComponent>(e).Rotation = { -glm::half_pi<float>(), 0, 0 }; // top edge (+Y) points to -Z: flat
            auto& c = reg.Emplace<ClothComponent>(e);
            c.Resolution = 12;
            reg.Emplace<MeshRendererComponent>(e);
            ClothComponent probe = c;
            CHECK(Cloth::PinnedVertices(probe).size() == 13 && Cloth::BuildGrid(probe).Vertices.size() == 169);
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            run(rt, 3.0f);
            auto& sim = rt->GetRegistry().Get<ClothComponent>(e);
            CHECK(sim.SimulatedPositions.size() == 169 && sim.SimulatedNormals.size() == 169);
            if (sim.SimulatedPositions.size() == 169)
            {
                auto world = [&](size_t i) { return glm::vec3(rt->GetRegistry().Get<TransformComponent>(e).World * glm::vec4(sim.SimulatedPositions[i], 1.0f)); };
                // Pinned top edge (row 12) stays at its place, about 1 m in front of the origin at y = 3.
                glm::vec3 pinned = world(12 * 13 + 6);
                CHECK(Near(pinned, { 0.0f, 3.0f, -1.0f }, 0.02f));
                // The free bottom edge (row 0) has swung down from level, still about the sheet's length (2 m) away.
                glm::vec3 bottom = world(6);
                CHECK(bottom.y < 2.3f && Near(glm::distance(bottom, pinned), 2.0f, 0.15f));
                // Moving the object drags the pinned edge with it.
                rt->GetRegistry().Get<TransformComponent>(e).Position.x = 1.0f;
                run(rt, 0.5f);
                CHECK(Near(world(12 * 13 + 6).x, 1.0f, 0.02f));
            }
            rt->OnRuntimeStop();
        }
        // Tearing: a heavy, stretchy curtain hung by its two top corners rips near them; without Tear Stretch the
        // same cloth only stretches.
        for (float tear : { 0.0f, 0.3f })
        {
            Scene scene("Rip");
            auto& reg = scene.GetRegistry();
            Entity e = scene.CreateEntity("Rag");
            reg.Get<TransformComponent>(e).Position = { 0, 3, 0 };
            auto& c = reg.Emplace<ClothComponent>(e);
            c.Resolution = 10;
            c.Pin = ClothPin::TopCorners;
            c.Mass = 40.0f;
            c.StretchCompliance = 0.01f;
            c.TearStretch = tear;
            reg.Emplace<MeshRendererComponent>(e);
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            run(rt, 2.0f);
            auto& sim = rt->GetRegistry().Get<ClothComponent>(e);
            size_t full = Cloth::BuildGrid(c).Indices.size();
            if (tear == 0.0f)
                CHECK(sim.TornEdges == 0 && sim.TornIndices.empty());
            else
            {
                CHECK(sim.TornEdges > 0 && !sim.TornIndices.empty() && sim.TornIndices.size() < full && sim.TornIndices.size() % 3 == 0);
                // Still simulated (every vertex, finite) after the rebuilds.
                bool finite = sim.SimulatedPositions.size() == 121;
                for (auto& p : sim.SimulatedPositions) finite &= std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
                CHECK(finite);
            }
            rt->OnRuntimeStop();
        }
        // Attach To: a 10 kg box without gravity holding a 2 kg banner by its top edge is pulled down by it (together
        // they fall at about 2/12 g), and the banner's top edge travels with the box. Unattached, the box stays put.
        for (bool attached : { false, true })
        {
            Scene scene("Banner");
            auto& reg = scene.GetRegistry();
            Entity holder = scene.CreateEntity("Holder");
            reg.Get<TransformComponent>(holder).Position = { 0, 5, 0 };
            auto& rb = reg.Emplace<RigidbodyComponent>(holder);
            rb.Mass = 10.0f;
            rb.UseGravity = false;
            rb.LinearDamping = 0.0f;
            reg.Emplace<BoxColliderComponent>(holder).Size = { 0.2f, 0.2f, 0.2f };
            Entity banner = scene.CreateEntity("Banner");
            reg.Get<TransformComponent>(banner).Position = { 0, 3.8f, 0 }; // top edge at y = 4.8, just under the holder
            auto& c = reg.Emplace<ClothComponent>(banner);
            c.Resolution = 8;
            c.Mass = 2.0f;
            if (attached) c.AttachTo = holder;
            reg.Emplace<MeshRendererComponent>(banner);
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            run(rt, 1.0f);
            float y = rt->GetRegistry().Get<TransformComponent>(holder).GetWorldPosition().y;
            if (!attached)
                CHECK(Near(y, 5.0f, 0.01f));
            else
            {
                CHECK(y < 4.7f && y > 3.6f); // dragged down by the banner's weight (about 0.8 m in a second)
                auto& sim = rt->GetRegistry().Get<ClothComponent>(banner);
                glm::vec3 top = glm::vec3(rt->GetRegistry().Get<TransformComponent>(banner).World * glm::vec4(sim.SimulatedPositions[8 * 9 + 4], 1.0f));
                CHECK(sim.SimulatedPositions.size() == 81 && Near(top.y, y - 0.2f, 0.1f)); // the pinned edge stayed with it
            }
            rt->OnRuntimeStop();
        }
        // An unpinned sheet dropped on a sphere drapes over it: its middle rests on top, its edges hang lower.
        {
            Scene scene("Drape");
            auto& reg = scene.GetRegistry();
            Entity ball = scene.CreateEntity("Ball");
            reg.Get<TransformComponent>(ball).Position = { 0, 1, 0 };
            reg.Emplace<SphereColliderComponent>(ball).Radius = 0.5f;
            Entity e = scene.CreateEntity("Sheet");
            reg.Get<TransformComponent>(e).Position = { 0, 2, 0 };
            reg.Get<TransformComponent>(e).Rotation = { -glm::half_pi<float>(), 0, 0 }; // horizontal
            auto& c = reg.Emplace<ClothComponent>(e);
            c.Resolution = 16;
            c.Pin = ClothPin::None;
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            run(rt, 2.5f);
            auto& sim = rt->GetRegistry().Get<ClothComponent>(e);
            auto& t = rt->GetRegistry().Get<TransformComponent>(e);
            CHECK(sim.SimulatedPositions.size() == 17 * 17);
            if (sim.SimulatedPositions.size() == 17 * 17)
            {
                glm::vec3 middle = glm::vec3(t.World * glm::vec4(sim.SimulatedPositions[8 * 17 + 8], 1.0f));
                glm::vec3 corner = glm::vec3(t.World * glm::vec4(sim.SimulatedPositions[0], 1.0f));
                CHECK(middle.y > 1.45f && middle.y < 1.65f); // on the ball's top (1.5) plus the cloth's thickness
                CHECK(corner.y < middle.y - 0.4f);           // the corners hang down around it
            }
            rt->OnRuntimeStop();
        }
    }

    void TestSoftBodies()
    {
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets"); // CPU mesh data only
        // A sphere dropped on the ground: inflated, it keeps its shape (slightly squashed); limp, it flattens.
        auto drop = [](float pressure, float stretch) {
            Scene scene("Soft");
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            reg.Get<TransformComponent>(ground).Scale = { 20, 1, 20 };
            reg.Emplace<BoxColliderComponent>(ground);
            Entity ball = scene.CreateEntity("Ball");
            reg.Get<TransformComponent>(ball).Position = { 0, 1.5f, 0 };
            reg.Emplace<MeshRendererComponent>(ball).Mesh = AssetManager::kSphere;
            auto& sb = reg.Emplace<SoftBodyComponent>(ball);
            sb.Pressure = pressure;
            sb.StretchCompliance = stretch;
            reg.Emplace<SphereColliderComponent>(ball); // ignored: the soft body is the collider
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < 180; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            auto& rr = rt->GetRegistry();
            auto& sim = rr.Get<SoftBodyComponent>(ball);
            auto& t = rr.Get<TransformComponent>(ball);
            float top = -1e9f, bottom = 1e9f;
            for (auto& p : sim.SimulatedPositions)
            {
                float y = glm::vec3(t.World * glm::vec4(p, 1.0f)).y;
                top = std::max(top, y);
                bottom = std::min(bottom, y);
            }
            struct Result { float Top, Bottom, Y; size_t Vertices; bool RigidBody; };
            Result r{ top, bottom, t.Position.y, sim.SimulatedPositions.size(), rr.Has<RigidbodyComponent>(ball) && rr.Get<RigidbodyComponent>(ball).RuntimeBody != 0xFFFFFFFF };
            rt->OnRuntimeStop();
            return r;
        };
        // (A triangulated closed surface is rigid by its edges alone, so "limp" also needs stretchy edges.)
        auto firm = drop(40.0f, 0.0001f), limp = drop(0.0f, 0.05f);
        std::printf("  firm top %.2f bottom %.2f, limp top %.2f%c", firm.Top, firm.Bottom, limp.Top, 10);
        CHECK(firm.Vertices == AssetManager::GetMeshData(AssetManager::kSphere)->Vertices.size()); // every mesh vertex follows
        CHECK(!firm.RigidBody);
        CHECK(firm.Bottom > -0.1f && firm.Bottom < 0.15f);   // resting on the ground
        CHECK(firm.Top > 0.75f && firm.Top < 1.1f);          // about its 1 m height
        CHECK(firm.Y < 1.0f && firm.Y > 0.2f);               // the object came down with it
        CHECK(limp.Top < firm.Top - 0.2f);                   // without pressure it slumps
        AssetManager::Shutdown();
    }

    void TestTowing()
    {
        // A car pulling a free-rolling trailer by a tow hitch: the trailer follows with the hitch points together, it
        // stays behind once unhitched, and a low Break Force tears it loose on its own.
        auto build = [&](Scene& scene, float breakForce) {
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            reg.Get<TransformComponent>(ground).Scale = { 400, 1, 400 };
            reg.Emplace<BoxColliderComponent>(ground).Friction = 1.0f;
            Entity car = scene.CreateEntity("Car");
            reg.Get<TransformComponent>(car).Position = { 0, 1.0f, 0 };
            reg.Emplace<RigidbodyComponent>(car).Mass = 1200.0f;
            reg.Emplace<BoxColliderComponent>(car).Size = { 1.8f, 0.5f, 4.2f };
            reg.Emplace<VehicleComponent>(car);
            Entity trailer = scene.CreateEntity("Trailer");
            reg.Get<TransformComponent>(trailer).Position = { 0, 1.0f, 4.6f }; // its coupling (-2 z) meets the hitch (+2.6 z)
            reg.Emplace<RigidbodyComponent>(trailer).Mass = 400.0f;
            reg.Emplace<BoxColliderComponent>(trailer).Size = { 1.6f, 0.5f, 3.0f };
            auto& tv = reg.Emplace<VehicleComponent>(trailer);
            tv.Drive = VehicleDrive::Unpowered;
            tv.WheelBase = 1.6f;
            tv.MaxSteerAngle = 0.0f;
            auto& hitch = reg.Emplace<TowHitchComponent>(trailer);
            hitch.TowedBy = car;
            hitch.Hitch = { 0.0f, 0.0f, 2.6f };
            hitch.Coupling = { 0.0f, 0.0f, -2.0f };
            hitch.BreakForce = breakForce;
            scene.UpdateTransforms();
            return std::make_pair(car, trailer);
        };
        Scene scene("Towing");
        auto [car, trailer] = build(scene, 0.0f);
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        auto run = [&](Scene& s, float seconds) { for (int i = 0; i < (int)(seconds * 60.0f); ++i) s.OnUpdateRuntime(1.0f / 60.0f); };
        auto gap = [&] {
            glm::vec3 a = glm::vec3(rr.Get<TransformComponent>(car).World * glm::vec4(0.0f, 0.0f, 2.6f, 1.0f));
            glm::vec3 b = glm::vec3(rr.Get<TransformComponent>(trailer).World * glm::vec4(0.0f, 0.0f, -2.0f, 1.0f));
            return glm::distance(a, b);
        };
        run(*rt, 1.0f);
        rr.Get<VehicleComponent>(car).Throttle = 1.0f;
        run(*rt, 4.0f);
        float carZ = rr.Get<TransformComponent>(car).Position.z, trailerZ = rr.Get<TransformComponent>(trailer).Position.z;
        CHECK(carZ < -10.0f && trailerZ < carZ + 6.0f && trailerZ > carZ + 3.0f && gap() < 0.1f);
        CHECK(rr.Get<TowHitchComponent>(trailer).CurrentForce > 0.0f);
        // Steering: the trailer swings to follow, still hitched.
        rr.Get<VehicleComponent>(car).Steering = 1.0f;
        run(*rt, 1.5f);
        CHECK(gap() < 0.1f && rr.Get<TransformComponent>(trailer).Position.x > 0.5f);
        // Unhitched: the car pulls away and the trailer rolls to a stop behind.
        rr.Get<VehicleComponent>(car).Steering = 0.0f;
        rr.Get<TowHitchComponent>(trailer).Attached = false;
        run(*rt, 3.0f);
        CHECK(gap() > 3.0f);
        rt->OnRuntimeStop();

        // Torn loose by a pull over its Break Force.
        Scene weak("WeakHitch");
        auto [car2, trailer2] = build(weak, 500.0f);
        auto rt2 = weak.Clone();
        rt2->OnRuntimeStart();
        run(*rt2, 1.0f);
        rt2->GetRegistry().Get<VehicleComponent>(car2).Throttle = 1.0f;
        run(*rt2, 2.0f);
        CHECK(!rt2->GetRegistry().Get<TowHitchComponent>(trailer2).Attached);
        rt2->OnRuntimeStop();
    }

    void TestVehicles()
    {
        Scene scene("Vehicle");
        auto& reg = scene.GetRegistry();
        Entity ground = scene.CreateEntity("Ground");
        reg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
        reg.Get<TransformComponent>(ground).Scale = { 400, 1, 400 };
        reg.Emplace<BoxColliderComponent>(ground).Friction = 1.0f;
        Entity car = scene.CreateEntity("Car");
        reg.Get<TransformComponent>(car).Position = { 0, 1.0f, 0 };
        reg.Emplace<RigidbodyComponent>(car).Mass = 1200.0f;
        reg.Emplace<BoxColliderComponent>(car).Size = { 1.8f, 0.5f, 4.2f };
        reg.Emplace<VehicleComponent>(car);
        Entity wheel = scene.CreateEntity("Wheel FL");
        scene.SetParent(wheel, car);
        reg.Get<VehicleComponent>(car).WheelFrontLeft = wheel;

        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        auto& rr = runtime->GetRegistry();
        auto run = [&](float seconds) { for (int i = 0; i < (int)(seconds * 60.0f); ++i) runtime->OnUpdateRuntime(1.0f / 60.0f); };
        auto& v = rr.Get<VehicleComponent>(car);
        auto pos = [&] { return rr.Get<TransformComponent>(car).Position; };
        // Settles on its suspension, all wheels down.
        run(2.0f);
        CHECK(v.WheelGrounded[0] && v.WheelGrounded[1] && v.WheelGrounded[2] && v.WheelGrounded[3]);
        CHECK(pos().y > 0.3f && pos().y < 1.0f && std::abs(v.SpeedKmh) < 1.0f);
        // The wheel visual sits at the front-left wheel (front is -Z, left is -X), below the body.
        glm::vec3 w = rr.Get<TransformComponent>(wheel).Position;
        CHECK(Near(w.x, -0.8f, 0.05f) && Near(w.z, -1.3f, 0.05f) && w.y < -0.3f);
        // Full throttle: drives forward through the gears.
        v.Throttle = 1.0f;
        run(4.0f);
        CHECK(v.SpeedKmh > 25.0f && pos().z < -10.0f && std::abs(pos().x) < 1.0f);
        CHECK(v.Gear >= 1 && v.EngineRPM > 1000.0f);
        // Steering right (+X while facing -Z).
        float x0 = pos().x;
        v.Steering = 1.0f;
        run(1.5f);
        CHECK(pos().x > x0 + 2.0f);
        // Brakes stop it.
        v.Steering = 0.0f;
        v.Throttle = 0.0f;
        v.Brake = 1.0f;
        run(5.0f);
        CHECK(std::abs(v.SpeedKmh) < 1.0f);
        // Reverse from rest.
        v.Brake = 0.0f;
        v.Throttle = -1.0f;
        run(2.0f);
        CHECK(v.SpeedKmh < -5.0f && v.Gear < 0);
        // Pressing forward while rolling backward brakes before driving off.
        v.Throttle = 1.0f;
        run(0.25f);
        CHECK(v.SpeedKmh < 0.0f && v.Gear <= 0);
        runtime->OnRuntimeStop();
    }

    void TestMotorcycleAndTank()
    {
        auto world = [](Scene& scene) {
            auto& reg = scene.GetRegistry();
            Entity ground = scene.CreateEntity("Ground");
            reg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            reg.Get<TransformComponent>(ground).Scale = { 400, 1, 400 };
            reg.Emplace<BoxColliderComponent>(ground).Friction = 1.0f;
        };
        auto run = [](Ref<Scene>& rt, float seconds) { for (int i = 0; i < (int)(seconds * 60.0f); ++i) rt->OnUpdateRuntime(1.0f / 60.0f); };

        // Motorcycle: balances itself on two wheels, rides off, and leans into a turn.
        {
            Scene scene("Bike");
            world(scene);
            auto& reg = scene.GetRegistry();
            Entity bike = scene.CreateEntity("Bike");
            reg.Get<TransformComponent>(bike).Position = { 0, 0.9f, 0 };
            reg.Emplace<RigidbodyComponent>(bike).Mass = 240.0f;
            reg.Emplace<BoxColliderComponent>(bike).Size = { 0.3f, 0.5f, 1.6f };
            auto& v = reg.Emplace<VehicleComponent>(bike);
            v.Type = VehicleType::Motorcycle;
            v.WheelBase = 1.5f;
            v.WheelRadius = 0.31f;
            v.WheelWidth = 0.15f;
            v.EngineTorque = 150.0f;
            v.MaxRPM = 10000.0f;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& rv = rt->GetRegistry().Get<VehicleComponent>(bike);
            auto& t = rt->GetRegistry().Get<TransformComponent>(bike);
            auto up = [&] { return t.GetRotationQuat() * glm::vec3(0, 1, 0); };
            run(rt, 1.0f);
            CHECK(rv.WheelGrounded[0] && rv.WheelGrounded[2] && !rv.WheelGrounded[1]); // front and rear (left slots)
            rv.Throttle = 1.0f;
            run(rt, 4.0f);
            CHECK(rv.SpeedKmh > 20.0f && t.Position.z < -10.0f && up().y > 0.9f); // upright at speed
            // Slowed down, full right steering turns it (a high speed only allows the lean Jolt permits), upright.
            rv.Throttle = 0.0f;
            rv.Brake = 1.0f;
            run(rt, 3.0f);
            rv.Brake = 0.0f;
            rv.Throttle = 0.3f;
            run(rt, 1.0f);
            auto heading = [&] { glm::vec3 f = t.GetRotationQuat() * glm::vec3(0, 0, -1); return glm::normalize(glm::vec2(f.x, f.z)); };
            glm::vec2 h0 = heading();
            rv.Steering = 1.0f;
            run(rt, 1.0f);
            glm::vec2 h1 = heading();
            CHECK(glm::dot(h0, h1) < 0.85f && h0.x * h1.y - h0.y * h1.x > 0.0f && up().y > 0.7f); // turned right (toward +X), still up
            rt->OnRuntimeStop();
        }

        // Tank: drives forward on its tracks, and turns on the spot when only steering.
        {
            Scene scene("Tank");
            world(scene);
            auto& reg = scene.GetRegistry();
            Entity tank = scene.CreateEntity("Tank");
            reg.Get<TransformComponent>(tank).Position = { 0, 1.0f, 0 };
            reg.Emplace<RigidbodyComponent>(tank).Mass = 4000.0f;
            reg.Emplace<BoxColliderComponent>(tank).Size = { 2.4f, 0.8f, 4.0f };
            auto& v = reg.Emplace<VehicleComponent>(tank);
            v.Type = VehicleType::Tank;
            v.TrackWidth = 2.6f;
            v.WheelBase = 3.2f;
            v.WheelRadius = 0.3f;
            v.EngineTorque = 2000.0f;
            v.BrakeTorque = 15000.0f;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& rv = rt->GetRegistry().Get<VehicleComponent>(tank);
            auto& t = rt->GetRegistry().Get<TransformComponent>(tank);
            run(rt, 1.5f);
            CHECK(rv.WheelGrounded[0] && rv.WheelGrounded[1] && rv.WheelGrounded[2] && rv.WheelGrounded[3]);
            rv.Throttle = 1.0f;
            run(rt, 3.0f);
            CHECK(rv.SpeedKmh > 5.0f && t.Position.z < -2.0f);
            rv.Throttle = 0.0f;
            rv.Brake = 1.0f;
            run(rt, 3.0f);
            rv.Brake = 0.0f;
            glm::vec3 stopped = t.Position;
            float yaw0 = glm::eulerAngles(t.GetRotationQuat()).y;
            glm::vec3 f0 = t.GetRotationQuat() * glm::vec3(0, 0, -1);
            rv.Steering = 1.0f;
            run(rt, 0.75f);
            glm::vec3 f1 = t.GetRotationQuat() * glm::vec3(0, 0, -1);
            (void)yaw0;
            CHECK(glm::dot(glm::vec2(f0.x, f0.z), glm::vec2(f1.x, f1.z)) < 0.9f);        // turned more than 25 degrees in 0.75 s
            CHECK(glm::cross(f0, f1).y < 0.0f);                                          // to the right (clockwise from above)
            CHECK(glm::length(glm::vec2(t.Position.x - stopped.x, t.Position.z - stopped.z)) < 1.5f); // on the spot
            rt->OnRuntimeStop();
        }
    }

    void TestTimelines()
    {
        // Sampling: linear halfway, step holds, smooth eases (flat at the ends, matches the keys).
        TimelineTrack track;
        float zero[4] = { 0, 0, 0, 0 }, ten[4] = { 10, 20, 0, 0 }, four[4] = { 4, 0, 0, 0 };
        Timeline::SetKey(track, 2.0f, ten, 2);
        Timeline::SetKey(track, 0.0f, zero, 2);
        CHECK(track.Keys.size() == 2 && track.Keys[0].Time == 0.0f); // sorted
        Timeline::SetKey(track, 2.0f, four, 1); // same time: replaced
        CHECK(track.Keys.size() == 2 && track.Keys[1].Value[0] == 4.0f);
        Timeline::SetKey(track, 2.0f, ten, 2);
        float v[4];
        track.Interpolation = TimelineInterpolation::Linear;
        CHECK(Timeline::Sample(track, 1.0f, v) && Near(v[0], 5.0f) && Near(v[1], 10.0f));
        CHECK(Timeline::Sample(track, -1.0f, v) && v[0] == 0.0f && Timeline::Sample(track, 9.0f, v) && v[0] == 10.0f); // clamped
        track.Interpolation = TimelineInterpolation::Step;
        CHECK(Timeline::Sample(track, 1.99f, v) && v[0] == 0.0f);
        track.Interpolation = TimelineInterpolation::Smooth;
        float early[4], mid[4];
        Timeline::Sample(track, 0.2f, early);
        Timeline::Sample(track, 1.0f, mid);
        CHECK(early[0] < 1.0f && Near(mid[0], 5.0f)); // eases in, symmetric in the middle

        // An asset animating the object's position and a child light's intensity (by path), with an event.
        fs::path assets = fs::temp_directory_path() / "dimension_timeline_test" / "Assets";
        fs::remove_all(assets.parent_path());
        fs::create_directories(assets);
        AssetManager::Init(assets);
        AssetHandle handle = AssetManager::CreateTimeline(assets / "Door.timeline");
        CHECK(handle.IsValid() && AssetManager::GetTimeline(handle) != nullptr);
        {
            auto tl = AssetManager::GetTimeline(handle);
            tl->Duration = 2.0f;
            TimelineTrack pos;
            pos.Component = "Transform";
            pos.Field = "Position";
            pos.Interpolation = TimelineInterpolation::Linear;
            float a[4] = { 0, 0, 0, 0 }, b[4] = { 0, 4, 0, 0 };
            Timeline::SetKey(pos, 0.0f, a, 3);
            Timeline::SetKey(pos, 2.0f, b, 3);
            TimelineTrack light;
            light.Path = "Lamp";
            light.Component = "Point Light";
            light.Field = "Intensity";
            light.Interpolation = TimelineInterpolation::Linear;
            float off[4] = { 0 }, on[4] = { 8 };
            Timeline::SetKey(light, 0.0f, off, 1);
            Timeline::SetKey(light, 1.0f, on, 1);
            tl->Tracks = { pos, light };
            tl->Events = { { 1.5f, "OnDoorOpen" } };
            AssetManager::SaveTimeline(handle);
        }
        auto back = TimelineAsset::Load(assets / "Door.timeline");
        CHECK(back->Tracks.size() == 2 && back->Tracks[1].Path == "Lamp" && back->Tracks[0].Keys.size() == 2 && Near(back->Tracks[0].Keys[1].Value[1], 4.0f) &&
              back->Events.size() == 1 && Near(back->Duration, 2.0f));

        Scene scene("Timeline");
        auto& reg = scene.GetRegistry();
        Entity door = scene.CreateEntity("Door");
        Entity lamp = scene.CreateEntity("Lamp", door);
        reg.Emplace<PointLightComponent>(lamp).Intensity = 1.0f;
        CHECK(Timeline::ResolvePath(scene, door, "Lamp") == lamp && Timeline::ResolvePath(scene, door, "") == door &&
              Timeline::ResolvePath(scene, door, "Nope").IsNull() && Timeline::PathOf(scene, door, lamp) == "Lamp");
        CHECK(Timeline::Apply(scene, door, *back, 0.5f) == 2);
        CHECK(Near(reg.Get<TransformComponent>(door).Position.y, 1.0f) && Near(reg.Get<PointLightComponent>(lamp).Intensity, 4.0f));
        // Played by a Timeline component while playing: it plays once and stops at the end.
        auto& comp = reg.Emplace<TimelineComponent>(door);
        comp.Timeline = handle;
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        for (int i = 0; i < 60; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        auto& rr = rt->GetRegistry();
        CHECK(Near(rr.Get<TransformComponent>(door).Position.y, 2.0f, 0.1f) && Near(rr.Get<PointLightComponent>(lamp).Intensity, 8.0f, 0.01f));
        for (int i = 0; i < 120; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(!rr.Get<TimelineComponent>(door).Playing && Near(rr.Get<TransformComponent>(door).Position.y, 4.0f));
        rt->OnRuntimeStop();
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path());
    }

    void TestMicrophone()
    {
        // Records ~0.3 s from the default capture device (skipped on machines without one), then saves it as a WAV clip.
        if (!Microphone::Start(-1, 1.0f, false))
        {
            std::printf("    (no capture device; skipped)\n");
            CHECK(!Microphone::IsRecording());
            return;
        }
        CHECK(Microphone::IsRecording());
        uint32_t rate = Microphone::GetSampleRate();
        CHECK(rate >= 8000);
        CHECK(Microphone::GetBufferFrames() == (size_t)rate);
        auto start = std::chrono::steady_clock::now();
        while (Microphone::GetPosition() < rate / 4 && std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(Microphone::GetPosition() >= rate / 4);
        CHECK(Microphone::GetLevel() >= 0.0f && Microphone::GetLevel() <= 1.5f);
        Microphone::Stop();
        CHECK(!Microphone::IsRecording());
        uint64_t frames = Microphone::GetPosition();
        std::vector<float> samples;
        CHECK(Microphone::GetSamples(samples) == (size_t)std::min<uint64_t>(frames, rate));
        fs::path wav = fs::temp_directory_path() / "dimension_microphone_test.wav";
        CHECK(Microphone::SaveWav(wav));
        auto info = AudioEngine::GetClipInfo(wav);
        CHECK(info && info->Format == "WAV" && info->Channels == 1 && info->SampleRate == rate && info->Frames == samples.size());
        fs::remove(wav);
    }

    void TestNestedTimelines()
    {
        // Control tracks: a master timeline's clips play another timeline on an object, with Clip In, Speed and loop
        // or hold; the nested timeline's tracks resolve from that object and its events fire on it.
        fs::path assets = fs::temp_directory_path() / "dimension_nested_timelines" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        AssetHandle doorHandle = AssetManager::CreateTimeline(assets / "Door.timeline");
        AssetHandle masterHandle = AssetManager::CreateTimeline(assets / "Master.timeline");
        AssetHandle selfHandle = AssetManager::CreateTimeline(assets / "Self.timeline");
        {
            auto door = AssetManager::GetTimeline(doorHandle);
            door->Duration = 2.0f;
            TimelineTrack rise;
            rise.Component = "Transform";
            rise.Field = "Position";
            rise.Interpolation = TimelineInterpolation::Linear;
            float a[3] = { 0, 0, 0 }, b[3] = { 0, 4, 0 };
            Timeline::SetKey(rise, 0.0f, a, 3);
            Timeline::SetKey(rise, 2.0f, b, 3);
            door->Tracks = { rise };
            door->Events = { { 1.0f, "OnHalfOpen" } };
            AssetManager::SaveTimeline(doorHandle);

            auto master = AssetManager::GetTimeline(masterHandle);
            master->Duration = 6.0f;
            TimelineTrack control;
            control.Kind = TimelineTrackKind::Control;
            control.Path = "Door";
            TimelineClip once;
            once.Start = 1.0f;
            once.Duration = 3.0f;
            once.Timeline = doorHandle;
            once.Loop = false;
            TimelineClip looped;
            looped.Start = 4.5f;
            looped.Duration = 1.5f;
            looped.Timeline = doorHandle;
            looped.ClipIn = 0.5f;
            looped.Speed = 2.0f;
            control.Clips = { once, looped };
            master->Tracks = { control };
            AssetManager::SaveTimeline(masterHandle);

            auto self = AssetManager::GetTimeline(selfHandle);
            TimelineTrack loop;
            loop.Kind = TimelineTrackKind::Control;
            TimelineClip again;
            again.Duration = 5.0f;
            again.Timeline = selfHandle;
            loop.Clips = { again };
            self->Tracks = { loop };
        }
        auto master = TimelineAsset::Load(assets / "Master.timeline");
        CHECK(master->Tracks.size() == 1 && master->Tracks[0].Kind == TimelineTrackKind::Control && master->Tracks[0].Clips.size() == 2);
        const TimelineClip& loopedBack = master->Tracks[0].Clips[1];
        CHECK(loopedBack.Timeline == doorHandle && Near(loopedBack.ClipIn, 0.5f) && Near(loopedBack.Speed, 2.0f) && loopedBack.Loop &&
              !master->Tracks[0].Clips[0].Loop);
        CHECK(Near(Timeline::ControlTime(loopedBack, 2.0f, 5.9f), std::fmod(0.5f + 1.4f * 2.0f, 2.0f)) &&
              Near(Timeline::ControlTime(master->Tracks[0].Clips[0], 2.0f, 3.9f), 2.0f));

        Scene scene("Nested");
        auto& reg = scene.GetRegistry();
        Entity root = scene.CreateEntity("Cutscene");
        Entity door = scene.CreateEntity("Door", root);
        auto& pos = reg.Get<TransformComponent>(door).Position;
        pos = { 9, 9, 9 };
        Timeline::Apply(scene, root, *master, 0.5f);
        CHECK(Near(pos.y, 9.0f)); // before the first clip: untouched
        Timeline::Apply(scene, root, *master, 2.0f);
        CHECK(Near(pos.x, 0.0f) && Near(pos.y, 2.0f));
        Timeline::Apply(scene, root, *master, 3.9f);
        CHECK(Near(pos.y, 4.0f)); // held at the nested timeline's end
        pos.y = 7.0f;
        Timeline::Apply(scene, root, *master, 4.25f);
        CHECK(Near(pos.y, 7.0f)); // a gap: nothing applied
        Timeline::Apply(scene, root, *master, 5.0f);
        CHECK(Near(pos.y, 3.0f)); // 0.5 + 0.5 * 2 = 1.5 s in
        Timeline::Apply(scene, root, *master, 5.9f);
        CHECK(Near(pos.y, 2.6f, 1e-3f)); // 3.3 s wraps to 1.3 s

        // Events: once in the first clip, and in each pass of the looped clip (0.5 -> 2 and 0 -> 1.5).
        std::vector<std::pair<Entity, std::string>> events;
        Timeline::Advance(scene, root, *master, -1.0f, 6.0f, events);
        CHECK(events.size() == 3);
        bool onDoor = true;
        for (auto& [target, fn] : events) onDoor &= target == door && fn == "OnHalfOpen";
        CHECK(onDoor);
        events.clear();
        Timeline::Advance(scene, root, *master, 1.5f, 1.9f, events);
        CHECK(events.empty());
        Timeline::Advance(scene, root, *master, 1.9f, 2.1f, events);
        CHECK(events.size() == 1);

        // A timeline that plays itself stops nesting instead of recursing forever.
        auto self = AssetManager::GetTimeline(selfHandle);
        CHECK(Timeline::Apply(scene, root, *self, 1.0f) == 1);
        events.clear();
        Timeline::Advance(scene, root, *self, -1.0f, 5.0f, events);

        // Played: the master's Timeline component drives the door through the nested timeline.
        reg.Emplace<TimelineComponent>(root).Timeline = masterHandle;
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        for (int i = 0; i < 32; ++i) rt->OnUpdateRuntime(0.0625f); // 2 s
        CHECK(Near(rt->GetRegistry().Get<TransformComponent>(door).Position.y, 2.0f, 1e-3f));
        rt->OnRuntimeStop();
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestTimelineClips()
    {
        // Clip tracks: activation, animation (with crossfades) and audio, saved and loaded.
        fs::path assets = fs::temp_directory_path() / "dimension_timeline_clips" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Tests" / "Data" / "Tone.opus", assets / "Tone.opus");
        AssetManager::Init(assets);
        AssetHandle tone = AssetManager::GetHandle(assets / "Tone.opus");
        AssetHandle handle = AssetManager::CreateTimeline(assets / "Cutscene.timeline");
        {
            auto tl = AssetManager::GetTimeline(handle);
            tl->Duration = 3.0f;
            TimelineTrack act;
            act.Kind = TimelineTrackKind::Activation;
            act.Path = "Lamp";
            act.Clips = { { 0.5f, 0.5f } };
            TimelineTrack anim;
            anim.Kind = TimelineTrackKind::Animation;
            anim.Path = "Hero";
            TimelineClip walk;
            walk.Start = 0.2f;
            walk.Duration = 0.8f;
            walk.Animation = "Walk";
            TimelineClip run;
            run.Start = 0.8f;
            run.Duration = 1.2f;
            run.Animation = "Run";
            run.Speed = 2.0f;
            run.ClipIn = 0.1f;
            run.Loop = false;
            anim.Clips = { run, walk };
            Timeline::SortClips(anim);
            TimelineTrack audio;
            audio.Kind = TimelineTrackKind::Audio;
            audio.Path = "Speaker";
            TimelineClip beep;
            beep.Start = 0.0f;
            beep.Duration = 0.9f;
            beep.Audio = tone;
            audio.Clips = { beep };
            tl->Tracks = { act, anim, audio };
            AssetManager::SaveTimeline(handle);
        }
        auto back = TimelineAsset::Load(assets / "Cutscene.timeline");
        CHECK(back->Tracks.size() == 3 && back->Tracks[0].Kind == TimelineTrackKind::Activation && back->Tracks[0].Clips.size() == 1);
        const TimelineTrack& animBack = back->Tracks[1];
        CHECK(animBack.Kind == TimelineTrackKind::Animation && animBack.Clips.size() == 2 && animBack.Clips[0].Animation == "Walk" &&
              Near(animBack.Clips[1].Speed, 2.0f) && Near(animBack.Clips[1].ClipIn, 0.1f) && !animBack.Clips[1].Loop);
        CHECK(back->Tracks[2].Kind == TimelineTrackKind::Audio && back->Tracks[2].Clips[0].Audio == tone);

        Scene scene("Clips");
        auto& reg = scene.GetRegistry();
        Entity root = scene.CreateEntity("Cutscene");
        Entity lamp = scene.CreateEntity("Lamp", root);
        Entity hero = scene.CreateEntity("Hero", root);
        Entity speaker = scene.CreateEntity("Speaker", root);
        reg.Emplace<AnimatorComponent>(hero);
        auto& src = reg.Emplace<AudioSourceComponent>(speaker);
        src.PlayOnAwake = false;
        auto& tag = reg.Get<TagComponent>(lamp);
        auto& animator = reg.Get<AnimatorComponent>(hero);
        // Activation: only inside the clip.
        Timeline::Apply(scene, root, *back, 0.2f);
        CHECK(!tag.Active);
        Timeline::Apply(scene, root, *back, 0.7f);
        CHECK(tag.Active);
        Timeline::Apply(scene, root, *back, 1.2f);
        CHECK(!tag.Active);
        // Animation: before the first clip the Animator is its own; then Walk; then a crossfade into Run; then Run holds.
        Timeline::Apply(scene, root, *back, 0.1f);
        CHECK(!animator.TimelineDriven);
        Timeline::Apply(scene, root, *back, 0.5f);
        CHECK(animator.TimelineDriven && animator.TimelineClip == "Walk" && Near(animator.TimelineTime, 0.3f) && animator.TimelinePrevClip.empty());
        Timeline::Apply(scene, root, *back, 0.9f);
        CHECK(animator.TimelineClip == "Run" && animator.TimelinePrevClip == "Walk" && Near(animator.TimelineWeight, 0.5f) &&
              Near(animator.TimelineTime, 0.1f + 0.1f * 2.0f) && Near(animator.TimelinePrevTime, 0.7f) && !animator.TimelineLoop);
        Timeline::Apply(scene, root, *back, 2.5f);
        CHECK(animator.TimelineDriven && animator.TimelineClip == "Run" && Near(animator.TimelineTime, 0.1f + 1.2f * 2.0f));
        Timeline::Release(scene, root, *back);
        CHECK(!animator.TimelineDriven);

        // Played: the audio clip starts on the first frame through the Audio Source (when there is a device) and stops at its end.
        reg.Emplace<TimelineComponent>(root).Timeline = handle;
        AudioEngine::Init();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(rr.Get<AudioSourceComponent>(speaker).Clip == tone);
        if (AudioEngine::IsAvailable()) CHECK(AudioWorld::IsPlaying(*rt, speaker));
        for (int i = 0; i < 60; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        if (AudioEngine::IsAvailable()) CHECK(!AudioWorld::IsPlaying(*rt, speaker));
        CHECK(rr.Get<AnimatorComponent>(hero).TimelineDriven && rr.Get<AnimatorComponent>(hero).TimelineClip == "Run");
        rt->OnRuntimeStop();
        rt.reset();
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestWaves()
    {
        // Gerstner waves: the surface sampled over a displaced point is that point's height; heights stay within the
        // waves' amplitude; the surface moves; the grid follows.
        WaterVolumeComponent w;
        w.Size = { 40, 4, 40 };
        w.WaveHeight = 1.0f;
        w.WaveLength = 10.0f;
        w.WaveDirection = 30.0f;
        w.Steepness = 0.6f;
        CHECK(Water::HasWaves(w));
        float maxDiff = 0.0f, lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 50; ++i)
        {
            glm::vec2 rest(std::sin(i * 1.7f) * 15.0f, std::cos(i * 2.3f) * 15.0f);
            float time = i * 0.37f;
            glm::vec3 p = Water::Displace(w, rest, time);
            Water::SurfacePoint s = Water::Sample(w, glm::vec2(p.x, p.z), time);
            maxDiff = std::max(maxDiff, std::abs(s.Height - p.y));
            lo = std::min(lo, p.y);
            hi = std::max(hi, p.y);
            CHECK(s.Normal.y > 0.3f && Near(glm::length(s.Normal), 1.0f, 1e-3f));
        }
        CHECK(maxDiff < 0.02f);
        float amplitude = 0.5f * (1.0f + 0.45f + 0.25f);
        CHECK(lo >= 2.0f - amplitude - 1e-3f && hi <= 2.0f + amplitude + 1e-3f && hi - lo > 0.8f);
        CHECK(Water::Sample(w, glm::vec2(0.0f), 0.0f).Height != Water::Sample(w, glm::vec2(0.0f), 0.7f).Height);
        // Flat water: the top face.
        WaterVolumeComponent flat;
        CHECK(!Water::HasWaves(flat) && Water::Sample(flat, glm::vec2(3.0f), 1.0f).Height == 2.0f);

        Scene scene("Sea");
        auto& reg = scene.GetRegistry();
        Entity sea = scene.CreateEntity("Sea");
        reg.Get<TransformComponent>(sea).Position = { 0, 2, 0 };
        reg.Emplace<WaterVolumeComponent>(sea) = w;
        Water::Update(scene, 1.0f);
        auto& ws = reg.Get<WaterVolumeComponent>(sea);
        CHECK(ws.SimulatedPositions.size() == Water::SurfaceVertices(ws).size() && ws.SimulatedNormals.size() == ws.SimulatedPositions.size());

        // A floating ball rides the waves: it bobs up and down by about their height.
        Entity floor = scene.CreateEntity("Floor");
        reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
        reg.Emplace<BoxColliderComponent>(floor).Size = { 100, 1, 100 };
        Entity ball = scene.CreateEntity("Ball");
        reg.Get<TransformComponent>(ball).Position = { 0, 4.2f, 0 };
        reg.Emplace<RigidbodyComponent>(ball).Mass = 2.0f;
        reg.Emplace<SphereColliderComponent>(ball).Radius = 0.3f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        float bobLo = 1e9f, bobHi = -1e9f;
        for (int i = 0; i < 12 * 60; ++i)
        {
            rt->OnUpdateRuntime(1.0f / 60.0f);
            if (i > 4 * 60)
            {
                float y = rt->GetRegistry().Get<TransformComponent>(ball).GetWorldPosition().y;
                bobLo = std::min(bobLo, y);
                bobHi = std::max(bobHi, y);
            }
        }
        rt->OnRuntimeStop();
        CHECK(bobHi - bobLo > 0.4f && bobLo > 2.0f && bobHi < 5.5f);
    }

    void TestDents()
    {
        // A ball dropped on a deformable block: a fast hit pushes the block's top in around the contact (and no deeper
        // than Max Dent), a gentle one leaves no mark; Dent() dents by hand and Repair() undoes it.
        fs::path assets = fs::temp_directory_path() / "dimension_dent_test" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        auto drop = [&](float height, DeformableComponent* out) {
            Scene scene("Dent");
            auto& reg = scene.GetRegistry();
            Entity block = scene.CreateEntity("Block");
            reg.Get<TransformComponent>(block).Scale = { 4.0f, 4.0f, 4.0f }; // a big ball (the collider matches the mesh)
            reg.Emplace<MeshRendererComponent>(block).Mesh = AssetManager::kSphere; // a smooth, finely divided surface
            reg.Emplace<SphereColliderComponent>(block).Radius = 0.5f;
            auto& d = reg.Emplace<DeformableComponent>(block);
            d.MinSpeed = 3.0f;
            d.Strength = 2.0f;
            d.Radius = 0.8f;
            d.MaxDent = 0.15f;
            Entity ball = scene.CreateEntity("Ball");
            reg.Get<TransformComponent>(ball).Position = { 0.0f, 2.0f + height, 0.0f };
            reg.Emplace<SphereColliderComponent>(ball).Radius = 0.3f;
            reg.Emplace<RigidbodyComponent>(ball).Mass = 50.0f;
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < 90; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            *out = rt->GetRegistry().Get<DeformableComponent>(block);
            rt->OnRuntimeStop();
        };
        DeformableComponent hard, soft;
        drop(6.0f, &hard);  // about 11 m/s
        drop(0.05f, &soft); // about 2 m/s
        auto mesh = AssetManager::GetMeshData(AssetManager::kSphere);
        CHECK(soft.DentedPositions.empty() && hard.DentedPositions.size() == mesh->Vertices.size());
        float deepest = 0.0f;
        bool farUntouched = true, inward = true;
        for (size_t i = 0; i < hard.DentedPositions.size(); ++i)
        {
            glm::vec3 a = mesh->Vertices[i].Position, b = hard.DentedPositions[i];
            float moved = glm::distance(a, b) * 4.0f; // in meters (the block is scaled 4x)
            deepest = std::max(deepest, moved);
            if (a.y < 0.0f) farUntouched &= moved < 1e-6f;
            if (moved > 1e-4f) inward &= b.y < a.y;
        }
        CHECK(deepest > 0.05f && deepest <= 0.15f + 1e-4f && farUntouched && inward);
        // A Mesh Collider follows the dents: a ray down onto a dented dome lands in the dent.
        {
            Scene scene("Collider");
            Entity dome = scene.CreateEntity("Dome");
            scene.GetRegistry().Get<TransformComponent>(dome).Scale = { 4.0f, 4.0f, 4.0f };
            scene.GetRegistry().Emplace<MeshRendererComponent>(dome).Mesh = AssetManager::kSphere;
            scene.GetRegistry().Emplace<MeshColliderComponent>(dome);
            auto& dd = scene.GetRegistry().Emplace<DeformableComponent>(dome);
            dd.Radius = 1.0f;
            dd.MaxDent = 0.5f;
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            RaycastHit before = Physics3DWorld::Raycast(*rt, { 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f);
            CHECK(Physics3DWorld::Dent(*rt, dome, { 0.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 0.3f) > 0);
            RaycastHit after = Physics3DWorld::Raycast(*rt, { 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f);
            CHECK(before && after && Near(before.Point.y, 2.0f, 0.02f) && Near(after.Point.y, 1.7f, 0.03f));
            rt->OnRuntimeStop();
        }
        // By hand, then repaired.
        Scene scene("Manual");
        Entity can = scene.CreateEntity("Can");
        scene.GetRegistry().Emplace<MeshRendererComponent>(can).Mesh = AssetManager::kCube;
        scene.GetRegistry().Emplace<DeformableComponent>(can).MaxDent = 0.2f;
        scene.GetRegistry().Get<DeformableComponent>(can).Radius = 1.0f; // reaches the cube's top corners
        scene.UpdateTransforms();
        CHECK(Physics3DWorld::Dent(scene, can, { 0.0f, 0.5f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 0.1f) > 0);
        auto& dc = scene.GetRegistry().Get<DeformableComponent>(can);
        CHECK(!dc.DentedPositions.empty());
        dc.Repair();
        CHECK(dc.DentedPositions.empty());
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestBoat()
    {
        // A boat floating in a Water Volume: full throttle drives it forward (-Z), right steering turns it right
        // (clockwise seen from above), the keel keeps it from sliding sideways, and out of the water it has no thrust.
        auto run = [&](float throttle, float steering, float startY, float seconds) {
            Scene scene("Boat");
            auto& reg = scene.GetRegistry();
            Entity sea = scene.CreateEntity("Sea");
            reg.Get<TransformComponent>(sea).Position = { 0, -2, 0 };
            auto& water = reg.Emplace<WaterVolumeComponent>(sea);
            water.Size = { 200, 4, 200 };
            water.Buoyancy = 2.0f;
            water.LinearDrag = 0.5f;
            water.AngularDrag = 0.5f;
            Entity boat = scene.CreateEntity("Boat");
            reg.Get<TransformComponent>(boat).Position = { 0, startY, 0 };
            reg.Emplace<BoxColliderComponent>(boat).Size = { 1.6f, 0.8f, 4.0f };
            reg.Emplace<RigidbodyComponent>(boat).Mass = 400.0f;
            auto& b = reg.Emplace<BoatComponent>(boat);
            b.Throttle = throttle;
            b.Steering = steering;
            b.Propeller = { 0.0f, -0.4f, 2.0f };
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            bool everInWater = false;
            for (int i = 0; i < (int)(seconds * 16.0f); ++i)
            {
                rt->OnUpdateRuntime(0.0625f);
                everInWater |= rt->GetRegistry().Get<BoatComponent>(boat).PropellerInWater;
            }
            auto& t = rt->GetRegistry().Get<TransformComponent>(boat);
            glm::vec3 pos = t.Position, forward = t.Forward(), right = t.Right();
            glm::vec3 vel = Physics3DWorld::GetLinearVelocity(*rt, boat);
            rt->OnRuntimeStop();
            float upright = t.Up().y;
            struct Result { glm::vec3 Pos, Forward, Velocity, Right; bool InWater; float Upright; };
            return Result{ pos, forward, vel, right, everInWater, upright };
        };
        auto idle = run(0.0f, 0.0f, 0.2f, 4.0f);
        auto ahead = run(1.0f, 0.0f, 0.2f, 4.0f);
        CHECK(idle.InWater && glm::length(glm::vec2(idle.Pos.x, idle.Pos.z)) < 0.5f);
        CHECK(ahead.Pos.z < -5.0f && std::abs(ahead.Pos.x) < 1.0f && ahead.Upright > 0.9f); // and level, not standing on its stern
        auto turning = run(1.0f, 1.0f, 0.2f, 2.0f); // up to 45 degrees per second
        // Heading turned clockwise seen from above, from -Z toward +X (the boat's right), and it moves mostly along it.
        float turned = std::atan2(turning.Forward.x, -turning.Forward.z);
        CHECK(turned > 0.4f && turned < 1.6f);
        CHECK(std::abs(glm::dot(turning.Velocity, turning.Right)) < 0.5f * std::abs(glm::dot(turning.Velocity, turning.Forward)));
        auto dry = run(1.0f, 0.0f, 50.0f, 1.0f); // falling from high up: the propeller never touches water
        CHECK(!dry.InWater && std::abs(dry.Pos.z) < 0.01f);
    }

    void TestDestructible()
    {
        // A static wall broken by a script: with strong glue its pieces stay standing (stuck to each other and the
        // ground); without glue, or with glue too weak for their weight, they collapse.
        fs::path assets = fs::temp_directory_path() / "dimension_destructible_test" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        auto collapse = [&](float glue) {
            Scene scene("Wall");
            auto& reg = scene.GetRegistry();
            Entity floor = scene.CreateEntity("Floor");
            reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
            reg.Emplace<BoxColliderComponent>(floor).Size = { 40, 1, 40 };
            Entity wall = scene.CreateEntity("Wall");
            reg.Get<TransformComponent>(wall).Position = { 0, 1.5f, 0 };
            reg.Get<TransformComponent>(wall).Scale = { 4.0f, 3.0f, 0.3f };
            reg.Emplace<MeshRendererComponent>(wall).Mesh = AssetManager::kCube;
            reg.Emplace<BoxColliderComponent>(wall);
            reg.Emplace<RigidbodyComponent>(wall).Mass = 200.0f;
            reg.Get<RigidbodyComponent>(wall).Type = BodyType::Static;
            auto& br = reg.Emplace<BreakableComponent>(wall);
            br.Pieces = 16;
            br.BreakSpeed = 0.0f;
            br.ExplosionSpeed = 0.0f;
            br.DebrisLifetime = 0.0f;
            br.GlueStrength = glue;
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            std::vector<Entity> pieces = Physics3DWorld::Break(*rt, wall, { 0.0f, 1.5f, 0.15f });
            std::vector<glm::vec3> start;
            for (Entity p : pieces) start.push_back(rt->GetRegistry().Get<TransformComponent>(p).Position);
            for (int i = 0; i < 120; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            float drop = 0.0f;
            for (size_t i = 0; i < pieces.size(); ++i) drop += (start[i].y - rt->GetRegistry().Get<TransformComponent>(pieces[i]).Position.y) / pieces.size();
            rt->OnRuntimeStop();
            return std::make_pair(pieces.size(), drop);
        };
        // A heavy ball swinging on a rope into a glued wall: the hit breaks it, the ball keeps its speed into the
        // pieces (rather than stopping against the whole wall), and part of the wall comes loose while the rest stands.
        {
            Scene scene("Wrecking");
            auto& reg = scene.GetRegistry();
            Entity floor = scene.CreateEntity("Floor");
            reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
            reg.Emplace<BoxColliderComponent>(floor).Size = { 40, 1, 40 };
            Entity wall = scene.CreateEntity("Wall");
            reg.Get<TransformComponent>(wall).Position = { 1.0f, 1.5f, 0 };
            reg.Get<TransformComponent>(wall).Scale = { 0.3f, 3.0f, 4.0f };
            reg.Emplace<MeshRendererComponent>(wall).Mesh = AssetManager::kCube;
            reg.Emplace<BoxColliderComponent>(wall);
            auto& wb = reg.Emplace<BreakableComponent>(wall);
            wb.Pieces = 40;
            wb.Seed = 2;
            wb.BreakSpeed = 3.0f;
            wb.ImpactFocus = 0.5f;
            wb.FocusRadius = 0.3f;
            wb.GlueStrength = 4000.0f; // holds the wall up, not the blow
            wb.DebrisLifetime = 0.0f;
            Entity ball = scene.CreateEntity("Ball");
            reg.Get<TransformComponent>(ball).Position = { -3.9f, 7.0f, 0 };
            reg.Get<TransformComponent>(ball).Scale = { 1.2f, 1.2f, 1.2f };
            reg.Emplace<SphereColliderComponent>(ball).Radius = 0.5f;
            reg.Emplace<RigidbodyComponent>(ball).Mass = 600.0f;
            Entity chain = scene.CreateEntity("Chain");
            reg.Get<TransformComponent>(chain).Position = { 0, 6.9f, 0 };
            auto& rope = reg.Emplace<RopeComponent>(chain);
            rope.Length = 4.0f; // strikes 2.3 m up, above the pieces anchored to the ground
            rope.AttachTo = ball;
            rope.AttachOffset = { 0, 0.5f, 0 };
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            auto& rr = rt->GetRegistry();
            float hitSpeed = 0.0f;
            for (int i = 0; i < 200 && rt->IsValid(wall); ++i)
            {
                hitSpeed = glm::length(Physics3DWorld::GetLinearVelocity(*rt, ball));
                rt->OnUpdateRuntime(1.0f / 60.0f);
            }
            rt->OnUpdateRuntime(1.0f / 60.0f);
            float afterSpeed = glm::length(Physics3DWorld::GetLinearVelocity(*rt, ball));
            std::vector<std::pair<Entity, glm::vec3>> pieces;
            for (auto [e, tag, t] : rr.ViewOf<TagComponent, TransformComponent>())
                if (tag.Name == "Wall Piece") pieces.push_back({ e, t.Position });
            for (int i = 0; i < 120; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            int loose = 0, standing = 0;
            for (auto& [e, start] : pieces)
                (glm::distance(rr.Get<TransformComponent>(e).Position, start) > 0.15f ? loose : standing)++;
            printf("  wrecking: hit %.2f m/s, %.2f m/s after the break, %d pieces loose, %d standing\n", hitSpeed, afterSpeed, loose, standing);
            CHECK(!rt->IsValid(wall) && hitSpeed > 7.0f && afterSpeed > 0.3f * hitSpeed && loose >= 3 && standing >= 10);
            rt->OnRuntimeStop();
        }
        auto [glued, gluedDrop] = collapse(1e6f);
        auto [loose, looseDrop] = collapse(0.0f);
        auto [weak, weakDrop] = collapse(1.0f);
        CHECK(glued >= 8 && loose == glued && weak == glued);
        CHECK(gluedDrop < 0.02f && looseDrop > 0.3f && weakDrop > 0.3f);
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestParticleLights()
    {
        // About Light Ratio of the particles carry a light, the same ones as they move and age; none at 0, all at 1.
        ParticleSystemComponent ps;
        ps.LightRatio = 0.3f;
        std::vector<Particle> particles(4000);
        uint32_t state = 12345u;
        auto rnd = [&] { state = state * 1664525u + 1013904223u; return (state >> 8) / 16777216.0f; };
        int carrying = 0;
        for (auto& q : particles)
        {
            q.Lifetime = 1.5f + rnd();
            q.AngularVelocity = glm::radians(-45.0f + 90.0f * rnd());
            carrying += ps.CarriesLight(q);
        }
        CHECK(carrying > 1000 && carrying < 1400);
        bool stable = true;
        for (auto& q : particles)
        {
            bool before = ps.CarriesLight(q);
            q.Age = 0.7f;
            q.Position = glm::vec3(rnd(), rnd(), rnd());
            q.Color.a = 0.5f;
            stable &= ps.CarriesLight(q) == before;
        }
        CHECK(stable);
        ps.LightRatio = 0.0f;
        CHECK(!ps.CarriesLight(particles[0]) && !ps.CarriesLight(particles[1]));
        ps.LightRatio = 1.0f;
        CHECK(ps.CarriesLight(particles[0]) && ps.CarriesLight(particles[1]));
    }

    void TestSceneSearch()
    {
        // Words must all match: names (any case), t:Type (components without spaces, script classes), tag:.
        Scene scene("Search");
        auto& reg = scene.GetRegistry();
        Entity player = scene.CreateEntity("Player");
        reg.Get<TagComponent>(player).Tag = "Player";
        reg.Emplace<RigidbodyComponent>(player);
        reg.Emplace<ScriptComponent>(player).Add("CarDriver");
        Entity lamp = scene.CreateEntity("Street Lamp");
        Entity bulb = scene.CreateEntity("Bulb", lamp);
        reg.Emplace<PointLightComponent>(bulb);
        reg.Emplace<MeshRendererComponent>(lamp);
        Entity crate = scene.CreateEntity("Crate");
        reg.Emplace<MeshRendererComponent>(crate);
        reg.Emplace<RigidbodyComponent>(crate);
        auto names = [&](const std::string& q) {
            std::string out;
            for (Entity e : SceneSearch::Find(scene, q)) out += reg.Get<TagComponent>(e).Name + ";";
            return out;
        };
        CHECK(names("lamp") == "Street Lamp;");
        CHECK(names("t:Rigidbody") == "Player;Crate;");
        CHECK(names("t:meshrenderer") == "Street Lamp;Crate;");
        CHECK(names("t:PointLight") == "Bulb;");
        CHECK(names("t:CarDriver") == "Player;");
        CHECK(names("tag:player") == "Player;");
        CHECK(names("t:Rigidbody cr") == "Crate;");
        CHECK(names("t:Nothing").empty() && names("   ").empty());
    }

    void TestWindZones()
    {
        // The field: Directional blows along the zone's forward (-Z) with its gusts; Spherical outward, fading to its
        // edge; zones add up.
        Scene scene("Windy");
        auto& reg = scene.GetRegistry();
        Entity gale = scene.CreateEntity("Gale");
        reg.Get<TransformComponent>(gale).Rotation = { 0.0f, glm::half_pi<float>(), 0.0f }; // forward -Z turned to -X
        auto& z = reg.Emplace<WindZoneComponent>(gale);
        z.Main = 10.0f;
        z.Turbulence = 0.0f;
        z.PulseMagnitude = 0.0f;
        scene.UpdateTransforms();
        Wind::Field field = Wind::Gather(scene, 0.0f);
        glm::vec3 w = field.At({ 3.0f, 1.0f, -7.0f });
        CHECK(Near(w, { -10.0f, 0.0f, 0.0f }, 1e-3f));
        z.PulseMagnitude = 0.5f;
        z.PulseFrequency = 0.25f; // a gust peaks at 1 s
        CHECK(Near(Wind::Gather(scene, 1.0f).At({ 0, 0, 0 }).x, -15.0f, 1e-3f));
        z.PulseMagnitude = 0.0f;
        Entity blast = scene.CreateEntity("Blast");
        reg.Get<TransformComponent>(blast).Position = { 20.0f, 0.0f, 0.0f };
        auto& s = reg.Emplace<WindZoneComponent>(blast);
        s.Mode = WindZoneMode::Spherical;
        s.Radius = 10.0f;
        s.Main = 8.0f;
        s.Turbulence = 0.0f;
        s.PulseMagnitude = 0.0f;
        scene.UpdateTransforms();
        field = Wind::Gather(scene, 0.0f);
        CHECK(Near(field.At({ 25.0f, 0.0f, 0.0f }), { -10.0f + 4.0f, 0.0f, 0.0f }, 1e-3f)); // half way out: half of Main, plus the gale
        CHECK(Near(field.At({ 20.0f, 0.0f, 12.0f }), { -10.0f, 0.0f, 0.0f }, 1e-3f));         // outside its radius

        // Playing: a light, large ball is blown along; a heavy crate stays put; particles with External Forces drift
        // with the wind (and without, they do not).
        reg.Get<WindZoneComponent>(blast).Main = 0.0f;
        Entity ground = scene.CreateEntity("Ground");
        reg.Get<TransformComponent>(ground).Position = { 0.0f, -0.5f, 0.0f };
        reg.Emplace<BoxColliderComponent>(ground).Size = { 200.0f, 1.0f, 200.0f };
        Entity ball = scene.CreateEntity("Beach Ball");
        reg.Get<TransformComponent>(ball).Position = { 0.0f, 0.5f, 0.0f };
        reg.Emplace<SphereColliderComponent>(ball).Radius = 0.5f;
        reg.Emplace<RigidbodyComponent>(ball).Mass = 0.5f;
        Entity crate = scene.CreateEntity("Crate");
        reg.Get<TransformComponent>(crate).Position = { 0.0f, 0.5f, 6.0f };
        reg.Emplace<BoxColliderComponent>(crate).Friction = 0.8f;
        reg.Emplace<RigidbodyComponent>(crate).Mass = 300.0f;
        Entity smoke = scene.CreateEntity("Smoke");
        reg.Get<TransformComponent>(smoke).Position = { 0.0f, 3.0f, -6.0f };
        auto& ps = reg.Emplace<ParticleSystemComponent>(smoke);
        ps.StartSpeed = { 0.0f, 0.0f };
        ps.StartLifetime = { 5.0f, 5.0f };
        ps.RateOverTime = 20.0f;
        ps.ExternalForces = 2.0f;
        Entity still = scene.CreateEntity("Still Smoke");
        reg.Get<TransformComponent>(still).Position = { 0.0f, 3.0f, -10.0f };
        auto& ps2 = reg.Emplace<ParticleSystemComponent>(still);
        ps2 = ps;
        ps2.ExternalForces = 0.0f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        for (int i = 0; i < 120; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        auto& rr = rt->GetRegistry();
        CHECK(rr.Get<TransformComponent>(ball).Position.x < -3.0f);
        CHECK(std::abs(rr.Get<TransformComponent>(crate).Position.x) < 0.05f);
        auto meanX = [&](Entity e) {
            const auto& parts = rr.Get<ParticleSystemComponent>(e).Particles;
            float sum = 0.0f;
            for (const auto& q : parts) sum += q.Position.x;
            return parts.empty() ? 0.0f : sum / parts.size();
        };
        CHECK(!rr.Get<ParticleSystemComponent>(smoke).Particles.empty() && meanX(smoke) < -2.0f && std::abs(meanX(still)) < 0.05f);
        rt->OnRuntimeStop();
    }

    void TestTerrainCrater()
    {
        // A crater sinks the ground in the middle by about Depth, throws up a rim around it, leaves the ground far off
        // alone, paints it dirt, and (while playing) the collider follows.
        Scene scene("Battlefield");
        auto& reg = scene.GetRegistry();
        Entity land = scene.CreateEntity("Land");
        auto& tc = reg.Emplace<TerrainComponent>(land);
        tc.Size = { 64.0f, 64.0f };
        tc.Resolution = 64;
        tc.FlatCenter = 1.0f; // flat everywhere, at Flat Height
        reg.Emplace<MeshRendererComponent>(land);
        scene.UpdateTransforms();
        auto heightAt = [&](Scene& s, float x, float z) {
            auto& t = s.GetRegistry().Get<TerrainComponent>(land);
            return Terrain::HeightAt(t, Terrain::BuildHeights(t), x, z);
        };
        float before = heightAt(scene, 0.0f, 0.0f), far0 = heightAt(scene, 25.0f, 25.0f);
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        rt->OnUpdateRuntime(1.0f / 60.0f);
        auto ray = [&] { return Physics3DWorld::Raycast(*rt, { 0.0f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f); };
        RaycastHit hit0 = ray();
        CHECK(hit0 && Near(hit0.Point.y, before, 0.05f));
        CHECK(Terrain::DigCrater(*rt, { 0.0f, before, 0.0f }, 6.0f, 2.0f) > 0);
        rt->OnUpdateRuntime(1.0f / 60.0f);
        float center = heightAt(*rt, 0.0f, 0.0f), rim = heightAt(*rt, 6.0f, 0.0f), far1 = heightAt(*rt, 25.0f, 25.0f);
        CHECK(Near(center, before - 2.0f, 0.1f) && rim > before + 0.2f && Near(far1, far0, 1e-3f));
        RaycastHit hit1 = ray();
        CHECK(hit1 && Near(hit1.Point.y, center, 0.1f));
        auto& rtc = rt->GetRegistry().Get<TerrainComponent>(land);
        Terrain::BeginEdit(rtc); // the committed edits, decoded again
        int n = Terrain::SampleCount(rtc);
        CHECK(Terrain::LayerWeight(rtc, (size_t)(n / 2) * n + n / 2, 3) > 0.8f); // dirt (the fourth layer) at the center
        rt->OnRuntimeStop();
        CHECK(Near(heightAt(scene, 0.0f, 0.0f), before, 1e-4f)); // the edited scene is untouched
    }

    // A yard for navigation tests: 30 x 30 m of ground, a wall across the middle (z = 0) from x = -15 to 6 with a gap
    // beyond it, a raised platform at (10, 10) with no way up, and a dynamic crate (not baked in) at (-5, 0, -10).
    Entity BuildNavYard(Scene& scene)
    {
        auto& reg = scene.GetRegistry();
        auto box = [&](const char* name, glm::vec3 at, glm::vec3 size) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = at;
            reg.Emplace<BoxColliderComponent>(e).Size = size;
            return e;
        };
        box("Ground", { 0.0f, -0.5f, 0.0f }, { 30.0f, 1.0f, 30.0f });
        box("Wall", { -4.5f, 1.5f, 0.0f }, { 21.0f, 3.0f, 0.6f });
        box("Platform", { 10.0f, 0.5f, 10.0f }, { 4.0f, 1.0f, 4.0f });
        Entity crate = box("Crate", { -5.0f, 0.5f, -10.0f }, { 1.0f, 1.0f, 1.0f });
        reg.Emplace<RigidbodyComponent>(crate).Mass = 20.0f;
        Entity surface = scene.CreateEntity("Navigation");
        auto& s = reg.Emplace<NavMeshSurfaceComponent>(surface);
        s.Size = { 30.0f, 10.0f, 30.0f };
        scene.UpdateTransforms();
        return surface;
    }

    void TestNavMeshBake()
    {
        // Baked while editing (a temporary physics world): open ground walkable, the wall and Agent Radius around it
        // not, the platform a separate area, the dynamic crate ignored.
        Scene scene("Yard");
        Entity surface = BuildNavYard(scene);
        auto mesh = Navigation::Bake(scene, surface);
        CHECK(mesh && mesh->NodeCount() > 10000);
        CHECK(scene.GetPhysics3D() == nullptr); // the edited scene keeps no physics
        if (!mesh) return;
        const NavMeshData& m = *mesh;
        int open = Navigation::Nearest(m, { -5.0f, 0.0f, -5.0f }, 0.2f);
        CHECK(open >= 0 && Near(m.Heights[open], 0.0f, 0.01f));
        CHECK(Navigation::Nearest(m, { 0.0f, 0.0f, 0.0f }, 0.5f) < 0);   // in the wall
        CHECK(Navigation::Nearest(m, { 0.0f, 0.0f, 0.55f }, 0.05f) < 0);  // closer than Agent Radius to it
        CHECK(Navigation::Nearest(m, { 0.0f, 0.0f, 1.0f }, 0.2f) >= 0);
        CHECK(Navigation::Nearest(m, { 0.0f, 3.0f, 0.0f }, 0.2f) < 0);   // its top: too narrow
        int crate = Navigation::Nearest(m, { -5.0f, 0.0f, -10.0f }, 0.2f);
        CHECK(crate >= 0 && m.Islands[crate] == m.Islands[open]);
        int platform = Navigation::Nearest(m, { 10.0f, 1.0f, 10.0f }, 0.2f);
        CHECK(platform >= 0 && Near(m.Heights[platform], 1.0f, 0.01f) && m.Islands[platform] != m.Islands[open]);
        CHECK(Navigation::Walk(m, open, { -5.0f, -5.0f }, { -5.0f, -3.0f }) && !Navigation::Walk(m, open, { -5.0f, -5.0f }, { -5.0f, 5.0f }));

        // Paths: around the wall's end (Complete), and toward the platform as near as it gets (Partial).
        std::vector<glm::vec3> corners;
        CHECK(Navigation::FindPath(m, { -5.0f, 0.0f, -5.0f }, { -5.0f, 0.0f, 5.0f }, corners) == NavPathStatus::Complete);
        float length = 0.0f, farthestX = -100.0f;
        for (size_t i = 0; i + 1 < corners.size(); ++i) length += glm::distance(corners[i], corners[i + 1]);
        for (const glm::vec3& c : corners) farthestX = std::max(farthestX, c.x);
        CHECK(corners.size() >= 3 && corners.size() < 12 && Near(corners.back(), { -5.0f, 0.0f, 5.0f }, 0.01f));
        CHECK(farthestX > 6.0f && length > 2.0f * glm::length(glm::vec2(11.5f, 5.0f)) - 0.5f && length < 30.0f);
        CHECK(Navigation::FindPath(m, { -5.0f, 0.0f, -5.0f }, { 10.0f, 1.0f, 10.0f }, corners) == NavPathStatus::Partial);
        CHECK(glm::distance(glm::vec2(corners.back().x, corners.back().z), glm::vec2(10.0f, 10.0f)) < 3.0f && Near(corners.back().y, 0.0f, 0.01f));
        CHECK(Navigation::FindPath(m, { -5.0f, 0.0f, -5.0f }, { -8.0f, 0.0f, -8.0f }, corners) == NavPathStatus::Complete && corners.size() == 2);
    }

    // A wall across the yard (z = 0) with a 1.4 m door at x = 0 and a 5 m gap past x = 10; baked for agents from 0.3 m.
    Entity BuildDoorYard(Scene& scene)
    {
        auto& reg = scene.GetRegistry();
        auto box = [&](const char* name, glm::vec3 at, glm::vec3 size) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = at;
            reg.Emplace<BoxColliderComponent>(e).Size = size;
            return e;
        };
        box("Ground", { 0.0f, -0.5f, 0.0f }, { 30.0f, 1.0f, 30.0f });
        box("Wall West", { -7.85f, 1.5f, 0.0f }, { 14.3f, 3.0f, 0.6f });
        box("Wall East", { 5.35f, 1.5f, 0.0f }, { 9.3f, 3.0f, 0.6f });
        Entity surface = scene.CreateEntity("Navigation");
        auto& s = reg.Emplace<NavMeshSurfaceComponent>(surface);
        s.Size = { 30.0f, 10.0f, 30.0f };
        s.AgentRadius = 0.3f;
        scene.UpdateTransforms();
        return surface;
    }

    void TestNavAgentSizesAndAreas()
    {
        // One surface for agents of any size: a 0.3 m agent goes through the door, a 0.9 m one around by the gap.
        Scene scene("Doors");
        Entity surface = BuildDoorYard(scene);
        auto& reg = scene.GetRegistry();
        auto mesh = Navigation::Bake(scene, surface);
        CHECK(mesh && mesh->Clearance.size() == (size_t)mesh->NodeCount());
        if (!mesh) return;
        auto maxX = [](const std::vector<glm::vec3>& corners) {
            float x = -100.0f;
            for (const glm::vec3& c : corners) x = std::max(x, c.x);
            return x;
        };
        std::vector<glm::vec3> corners;
        glm::vec3 from(-3.0f, 0.0f, -5.0f), to(-3.0f, 0.0f, 5.0f);
        Navigation::Filter small{ 0.3f }, big{ 0.9f };
        CHECK(Navigation::FindPath(*mesh, from, to, corners, {}, nullptr, small) == NavPathStatus::Complete && maxX(corners) < 2.0f);
        CHECK(Navigation::FindPath(*mesh, from, to, corners, {}, nullptr, big) == NavPathStatus::Complete && maxX(corners) > 10.0f);
        int door = Navigation::Nearest(*mesh, { 0.0f, 0.0f, 0.0f }, 0.2f);
        CHECK(door >= 0 && mesh->Clearance[door] > 0.3f && mesh->Clearance[door] < 0.9f);

        // Mud (cost 10) over the door: the small agent goes round too; with mud left out of its Area Mask likewise;
        // with cost 1 and mud allowed it goes through again.
        Entity mud = scene.CreateEntity("Mud");
        auto& volume = reg.Emplace<NavMeshModifierVolumeComponent>(mud);
        volume.Size = { 4.0f, 2.0f, 4.0f };
        volume.Area = NavArea::Mud;
        volume.Cost = 10.0f;
        scene.UpdateTransforms();
        mesh = Navigation::Bake(scene, surface);
        door = Navigation::Nearest(*mesh, { 0.0f, 0.0f, 0.0f }, 0.2f);
        CHECK(door >= 0 && mesh->Areas[door] == (uint8_t)NavArea::Mud && mesh->Costs[door] == 10.0f);
        CHECK(Navigation::FindPath(*mesh, from, to, corners, {}, nullptr, small) == NavPathStatus::Complete && maxX(corners) > 10.0f);
        // Cost 0: the area's cost from the project (Window > Navigation).
        volume.Cost = 0.0f;
        float projectMud = Project::GetConfig().NavAreaCosts[(int)NavArea::Mud];
        Project::GetConfig().NavAreaCosts[(int)NavArea::Mud] = 7.0f;
        mesh = Navigation::Bake(scene, surface);
        CHECK(mesh->Costs[Navigation::Nearest(*mesh, { 0.0f, 0.0f, 0.0f }, 0.2f)] == 7.0f);
        Project::GetConfig().NavAreaCosts[(int)NavArea::Mud] = projectMud;
        volume.Cost = 1.0f;
        mesh = Navigation::Bake(scene, surface);
        CHECK(Navigation::FindPath(*mesh, from, to, corners, {}, nullptr, small) == NavPathStatus::Complete && maxX(corners) < 2.0f);
        Navigation::Filter noMud{ 0.3f, ~(1u << (int)NavArea::Mud) };
        CHECK(Navigation::FindPath(*mesh, from, to, corners, {}, nullptr, noMud) == NavPathStatus::Complete && maxX(corners) > 10.0f);
        // Not Walkable: the door is gone from the mesh.
        volume.Area = NavArea::NotWalkable;
        mesh = Navigation::Bake(scene, surface);
        CHECK(Navigation::Nearest(*mesh, { 0.0f, 0.0f, 0.0f }, 0.5f) < 0 && Navigation::Nearest(*mesh, { 0.0f, 0.0f, -2.5f }, 0.2f) >= 0);
        reg.Remove<NavMeshModifierVolumeComponent>(mud);

        // Playing: both agents arrive, the big one never in the doorway.
        auto agent = [&](const char* name, glm::vec3 at, float radius) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = at;
            auto& a = reg.Emplace<NavMeshAgentComponent>(e);
            a.Radius = radius;
            a.Speed = 6.0f;
            a.Acceleration = 20.0f;
            return e;
        };
        Entity mouse = agent("Mouse", { -3.0f, 0.0f, -5.0f }, 0.3f), bear = agent("Bear", { -6.0f, 0.0f, -5.0f }, 0.9f);
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        CHECK(Navigation::SetDestination(*rt, mouse, { -3.0f, 0.0f, 5.0f }) && Navigation::SetDestination(*rt, bear, { -6.0f, 0.0f, 5.0f }));
        bool bearInDoor = false;
        for (int i = 0; i < 900; ++i)
        {
            rt->OnUpdateRuntime(1.0f / 60.0f);
            glm::vec3 b = rr.Get<TransformComponent>(bear).Position;
            bearInDoor |= std::abs(b.x) < 1.5f && std::abs(b.z) < 1.0f;
        }
        CHECK(Near(rr.Get<TransformComponent>(mouse).Position, { -3.0f, 0.0f, 5.0f }, 0.1f));
        CHECK(!bearInDoor && Near(rr.Get<TransformComponent>(bear).Position, { -6.0f, 0.0f, 5.0f }, 0.1f));
        rt->OnRuntimeStop();
    }

    void TestOcean()
    {
        // The ring surface: centered where asked, out to the radius, waves near and calm far away.
        WaterVolumeComponent sea;
        sea.Size = { 20.0f, 4.0f, 20.0f };
        sea.WaveHeight = 1.0f;
        sea.WaveLength = 10.0f;
        sea.Ocean = true;
        std::vector<Vertex> rings;
        Water::OceanVertices(sea, { 50.0f, -30.0f }, 2000.0f, 1.3f, rings);
        float farthest = 0.0f, nearMove = 0.0f, farMove = 0.0f;
        for (const Vertex& v : rings)
        {
            float r = glm::distance(glm::vec2(v.Position.x, v.Position.z), glm::vec2(50.0f, -30.0f));
            farthest = std::max(farthest, r);
            float lift = std::abs(v.Position.y - 2.0f);
            if (r < 30.0f) nearMove = std::max(nearMove, lift);
            if (r > 200.0f) farMove = std::max(farMove, lift);
        }
        CHECK(rings.size() > 10000 && Near(glm::vec2(rings[0].Position.x, rings[0].Position.z), { 50.0f, -30.0f }, 1.0f) && farthest > 1990.0f && farthest < 2050.0f);
        CHECK(nearMove > 0.2f && farMove == 0.0f);

        // Buoyancy reaches anywhere across: a crate 200 m past the box's side floats (and sinks without Ocean).
        auto drop = [&](bool ocean) {
            Scene scene("Sea");
            auto& reg = scene.GetRegistry();
            Entity water = scene.CreateEntity("Sea");
            reg.Get<TransformComponent>(water).Position = { 0.0f, -2.0f, 0.0f };
            auto& w = reg.Emplace<WaterVolumeComponent>(water);
            w.Size = { 20.0f, 4.0f, 20.0f };
            w.Buoyancy = 2.0f;
            w.LinearDrag = 1.0f;
            w.Ocean = ocean;
            Entity crate = scene.CreateEntity("Crate");
            reg.Get<TransformComponent>(crate).Position = { 200.0f, 1.0f, 0.0f };
            reg.Emplace<BoxColliderComponent>(crate);
            reg.Emplace<RigidbodyComponent>(crate).Mass = 50.0f;
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < 240; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            float y = rt->GetRegistry().Get<TransformComponent>(crate).Position.y;
            rt->OnRuntimeStop();
            return y;
        };
        float floating = drop(true), falling = drop(false);
        CHECK(floating > -0.8f && floating < 0.6f && falling < -10.0f);
    }

    void TestNavUpdateArea()
    {
        // The wall knocked down while playing: updating its area alone opens the way straight through (and the rest of
        // the mesh stays as it was).
        Scene scene("Yard");
        BuildNavYard(scene);
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        Entity surface = rt->FindEntityByName("Navigation");
        auto before = rr.Get<NavMeshSurfaceComponent>(surface).Baked;
        std::vector<glm::vec3> corners;
        CHECK(Navigation::FindPath(*before, { -5.0f, 0.0f, -5.0f }, { -5.0f, 0.0f, 5.0f }, corners) == NavPathStatus::Complete && corners.size() > 2);
        rt->DestroyEntity(rt->FindEntityByName("Wall"));
        auto t0 = std::chrono::steady_clock::now();
        CHECK(Navigation::UpdateArea(*rt, { -15.0f, 0.0f, -0.3f }, { 6.0f, 3.0f, 0.3f }) == 1);
        double updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        Navigation::Bake(*rt, surface);
        double bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Navigation::UpdateArea(*rt, { -15.0f, 0.0f, -0.3f }, { 6.0f, 3.0f, 0.3f }); // (the bake above was a full one, for timing)
        auto after = rr.Get<NavMeshSurfaceComponent>(surface).Baked;
        printf("  navigation: area update %.1f ms, full bake %.1f ms\n", updateMs, bakeMs);
        CHECK(after && after != before && Navigation::FindPath(*after, { -5.0f, 0.0f, -5.0f }, { -5.0f, 0.0f, 5.0f }, corners) == NavPathStatus::Complete &&
              corners.size() == 2);
        CHECK(after->RawStart == before->RawStart || after->RawHeights.size() > before->RawHeights.size()); // spots added where the wall stood
        int platform = Navigation::Nearest(*after, { 10.0f, 1.0f, 10.0f }, 0.2f);
        CHECK(platform >= 0 && Near(after->Heights[platform], 1.0f, 0.01f)); // untouched parts stay
        CHECK(updateMs < bakeMs);
        rt->OnRuntimeStop();

        // A crater dug into a terrain updates the mesh over it by itself: its bottom is walkable, 2 m down.
        Scene field("Field");
        auto& reg = field.GetRegistry();
        Entity land = field.CreateEntity("Land");
        auto& tc = reg.Emplace<TerrainComponent>(land);
        tc.Size = { 40.0f, 40.0f };
        tc.Resolution = 64;
        tc.FlatCenter = 1.0f;
        reg.Emplace<MeshRendererComponent>(land);
        Entity nav = field.CreateEntity("Navigation");
        auto& ns = reg.Emplace<NavMeshSurfaceComponent>(nav);
        ns.Size = { 36.0f, 30.0f, 36.0f };
        ns.CellSize = 0.5f;
        field.UpdateTransforms();
        auto ft = field.Clone();
        ft->OnRuntimeStart();
        auto& fr = ft->GetRegistry();
        auto flat = fr.Get<NavMeshSurfaceComponent>(nav).Baked;
        int middle = Navigation::Nearest(*flat, { 0.0f, 0.0f, 0.0f }, 20.0f);
        CHECK(middle >= 0);
        if (middle < 0) return;
        float ground = flat->Heights[middle];
        CHECK(Terrain::DigCrater(*ft, { 0.0f, ground, 0.0f }, 8.0f, 2.0f) > 0);
        auto dug = fr.Get<NavMeshSurfaceComponent>(nav).Baked;
        int bottom = Navigation::Nearest(*dug, { 0.0f, ground - 2.0f, 0.0f }, 0.5f);
        CHECK(dug != flat && bottom >= 0 && Near(dug->Heights[bottom], ground - 2.0f, 0.25f));
        ft->OnRuntimeStop();
    }

    void TestWaterFoam()
    {
        // Crest foam: none on flat water, some on steep waves' crests, more the steeper they are.
        WaterVolumeComponent sea;
        sea.WaveHeight = 1.2f;
        sea.WaveLength = 10.0f;
        auto most = [&](float steepness) {
            sea.Steepness = steepness;
            float m = 0.0f;
            for (float x = 0.0f; x < 20.0f; x += 0.25f) m = std::max(m, Water::CrestFoam(sea, { x, 0.0f }, 0.7f));
            return m;
        };
        float steep = most(0.9f), gentle = most(0.1f);
        sea.WaveHeight = 0.0f;
        CHECK(steep > 0.5f && gentle < steep && most(0.9f) == 0.0f);

        // Shore foam while playing: around an islet standing in a calm pond, not out in the open (the normals' length
        // carries it to the shader).
        Scene scene("Pond");
        auto& reg = scene.GetRegistry();
        Entity pond = scene.CreateEntity("Pond");
        reg.Get<TransformComponent>(pond).Position = { 0.0f, -2.0f, 0.0f };
        auto& w = reg.Emplace<WaterVolumeComponent>(pond);
        w.Size = { 20.0f, 4.0f, 20.0f };
        w.SurfaceResolution = 40;
        w.ShoreFoam = 1.0f;
        Entity bed = scene.CreateEntity("Bed");
        reg.Get<TransformComponent>(bed).Position = { 0.0f, -4.5f, 0.0f };
        reg.Emplace<BoxColliderComponent>(bed).Size = { 30.0f, 1.0f, 30.0f };
        Entity islet = scene.CreateEntity("Islet");
        reg.Get<TransformComponent>(islet).Position = { 4.0f, -1.0f, 0.0f };
        reg.Emplace<BoxColliderComponent>(islet).Size = { 3.0f, 3.0f, 3.0f }; // its top 0.5 above the surface
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        rt->OnUpdateRuntime(1.0f / 60.0f);
        auto& rw = rt->GetRegistry().Get<WaterVolumeComponent>(pond);
        const auto& rest = Water::SurfaceVertices(rw);
        CHECK(rw.SimulatedNormals.size() == rest.size() && !rest.empty());
        float nearIslet = 0.0f, open = 0.0f;
        for (size_t i = 0; i < rw.SimulatedNormals.size() && i < rest.size(); ++i)
        {
            float foam = glm::length(rw.SimulatedNormals[i]) - 1.0f;
            glm::vec2 at(rest[i].Position.x, rest[i].Position.z);
            if (glm::distance(at, glm::vec2(4.0f, 0.0f)) < 2.0f) nearIslet = std::max(nearIslet, foam);
            if (glm::distance(at, glm::vec2(-5.0f, 0.0f)) < 2.0f) open = std::max(open, foam);
        }
        CHECK(nearIslet > 0.9f && open < 0.01f);
        rt->OnRuntimeStop();
    }

    void TestNavCrowd()
    {
        // Two groups of 100 agents swap sides of an open field, walking through each other: all arrive, they barely
        // touch, and a frame's update stays quick (neighbours from a grid, the nearest few each).
        Scene scene("Field");
        auto& reg = scene.GetRegistry();
        Entity ground = scene.CreateEntity("Ground");
        reg.Get<TransformComponent>(ground).Position = { 0.0f, -0.5f, 0.0f };
        reg.Emplace<BoxColliderComponent>(ground).Size = { 60.0f, 1.0f, 60.0f };
        Entity surface = scene.CreateEntity("Navigation");
        auto& s = reg.Emplace<NavMeshSurfaceComponent>(surface);
        s.Size = { 60.0f, 10.0f, 60.0f };
        s.CellSize = 0.5f;
        s.AgentRadius = 0.4f;
        std::vector<std::pair<Entity, glm::vec3>> crowd;
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < 100; ++i)
            {
                float x = (i % 10) * 1.6f - 7.2f + side * 0.8f, z = (i / 10) * 1.6f + 6.0f;
                glm::vec3 start(x, 0.0f, side ? z : -z), goal(x, 0.0f, side ? -z : z);
                Entity e = scene.CreateEntity("Walker");
                reg.Get<TransformComponent>(e).Position = start;
                auto& a = reg.Emplace<NavMeshAgentComponent>(e);
                a.Radius = 0.4f;
                a.Speed = 3.0f;
                a.Acceleration = 10.0f;
                a.StoppingDistance = 0.3f;
                crowd.push_back({ e, goal });
            }
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        for (auto& [e, goal] : crowd) Navigation::SetDestination(*rt, e, goal);
        double navMs = 0.0;
        int frames = 0;
        float closest = 100.0f;
        for (int i = 0; i < 1500; ++i)
        {
            auto t0 = std::chrono::steady_clock::now();
            Navigation::Update(*rt, 1.0f / 60.0f);
            navMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++frames;
            if (i % 10 == 0)
                for (size_t a = 0; a < crowd.size(); ++a)
                    for (size_t b = a + 1; b < crowd.size(); ++b)
                    {
                        glm::vec3 pa = rr.Get<TransformComponent>(crowd[a].first).Position, pb = rr.Get<TransformComponent>(crowd[b].first).Position;
                        closest = std::min(closest, glm::distance(glm::vec2(pa.x, pa.z), glm::vec2(pb.x, pb.z)));
                    }
        }
        int arrived = 0;
        for (auto& [e, goal] : crowd) arrived += glm::distance(rr.Get<TransformComponent>(e).Position, goal) < 1.0f;
        printf("  crowd: %d of %zu arrived, closest %.2f m, %.2f ms per update\n", arrived, crowd.size(), closest, navMs / frames);
        CHECK(arrived >= 196 && closest > 0.45f);
        CHECK(navMs / frames < 25.0); // a Debug build
        rt->OnRuntimeStop();
    }

    void TestNavGeneratedLinks()
    {
        // Generate Links: the yard's platform (1 m up, no way up) gets drops down its edges, one way.
        Scene scene("Yard");
        Entity surface = BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        auto& s = reg.Get<NavMeshSurfaceComponent>(surface);
        s.GenerateLinks = true;
        s.DropHeight = 2.0f;
        auto mesh = Navigation::Bake(scene, surface);
        CHECK(mesh && !mesh->AutoLinks.empty());
        if (!mesh) return;
        auto rt = scene.Clone();
        rt->StartQueryPhysics();
        auto links = Navigation::LinksOn(*rt, *mesh);
        std::vector<glm::vec3> corners;
        std::vector<float> arcs;
        CHECK(Navigation::FindPath(*mesh, { 10.0f, 1.0f, 10.0f }, { 0.0f, 0.0f, 10.0f }, corners, links, &arcs) == NavPathStatus::Complete);
        CHECK(std::count_if(arcs.begin(), arcs.end(), [](float a) { return a >= 0.0f; }) == 1);
        CHECK(Navigation::FindPath(*mesh, { 0.0f, 0.0f, 10.0f }, { 10.0f, 1.0f, 10.0f }, corners, links) == NavPathStatus::Partial);
        // No links over the 3 m wall (higher than Drop Height is not the point: its top is too narrow to stand on).
        for (const NavMeshData::AutoLink& l : mesh->AutoLinks) CHECK(mesh->Heights[l.From] < 1.5f);

        // Two slabs a meter apart over a deep pit: jumps across, both ways, when Jump Distance allows.
        Scene pit("Pit");
        auto& pr = pit.GetRegistry();
        auto slab = [&](const char* name, float x) {
            Entity e = pit.CreateEntity(name);
            pr.Get<TransformComponent>(e).Position = { x, -0.5f, 0.0f };
            pr.Emplace<BoxColliderComponent>(e).Size = { 5.0f, 1.0f, 5.0f };
        };
        slab("West", -3.0f);
        slab("East", 3.0f);
        Entity bottom = pit.CreateEntity("Bottom");
        pr.Get<TransformComponent>(bottom).Position = { 0.0f, -20.0f, 0.0f };
        pr.Emplace<BoxColliderComponent>(bottom).Size = { 20.0f, 1.0f, 20.0f };
        Entity pitSurface = pit.CreateEntity("Navigation");
        auto& ps = pr.Emplace<NavMeshSurfaceComponent>(pitSurface);
        ps.Size = { 16.0f, 30.0f, 10.0f };
        ps.GenerateLinks = true;
        ps.JumpDistance = 1.5f;
        pit.UpdateTransforms();
        auto pitMesh = Navigation::Bake(pit, pitSurface);
        auto pitRt = pit.Clone();
        pitRt->StartQueryPhysics();
        CHECK(Navigation::FindPath(*pitMesh, { -3.0f, 0.0f, 0.0f }, { 3.0f, 0.0f, 0.0f }, corners, Navigation::LinksOn(*pitRt, *pitMesh), &arcs) == NavPathStatus::Complete);
        CHECK(Navigation::FindPath(*pitMesh, { 3.0f, 0.0f, 0.0f }, { -3.0f, 0.0f, 0.0f }, corners, Navigation::LinksOn(*pitRt, *pitMesh)) == NavPathStatus::Complete);
        pr.Get<NavMeshSurfaceComponent>(pitSurface).JumpDistance = 0.5f;
        pitMesh = Navigation::Bake(pit, pitSurface);
        CHECK(Navigation::FindPath(*pitMesh, { -3.0f, 0.0f, 0.0f }, { 3.0f, 0.0f, 0.0f }, corners, Navigation::LinksOn(*pitRt, *pitMesh)) == NavPathStatus::Partial);
    }

    void TestNavMeshObstacles()
    {
        // A carving obstacle filling the gap past the wall's end cuts the mesh there: the far side is out of reach
        // (Partial) and the agent stops short of the wall; moved away, the way is open again and the agent arrives.
        // A plain obstacle in another agent's way is walked around.
        Scene scene("Yard");
        BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        Entity gate = scene.CreateEntity("Gate");
        reg.Get<TransformComponent>(gate).Position = { 10.5f, 1.0f, 0.0f };
        auto& ob = reg.Emplace<NavMeshObstacleComponent>(gate);
        ob.Size = { 9.0f, 2.0f, 1.0f };
        ob.Carve = true;
        ob.CarveOnlyStationary = false;
        Entity post = scene.CreateEntity("Post");
        reg.Get<TransformComponent>(post).Position = { 0.0f, 1.0f, -8.0f };
        reg.Emplace<NavMeshObstacleComponent>(post).Size = { 1.0f, 2.0f, 1.0f };
        auto agent = [&](const char* name, glm::vec3 at) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = at;
            reg.Emplace<NavMeshAgentComponent>(e).Speed = 5.0f;
            return e;
        };
        Entity walker = agent("Walker", { -5.0f, 0.0f, -4.0f });
        Entity runner = agent("Runner", { -9.0f, 0.0f, -8.0f });
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        Entity surface = rt->FindEntityByName("Navigation");
        rt->OnUpdateRuntime(1.0f / 60.0f);
        auto& s = rr.Get<NavMeshSurfaceComponent>(surface);
        CHECK(s.Data && s.Baked && s.Data != s.Baked);
        int inGap = Navigation::Nearest(*s.Baked, { 10.0f, 0.0f, 0.0f }, 0.2f);
        CHECK(inGap >= 0 && s.Data->IsBlocked(inGap) && Navigation::Nearest(*s.Data, { 10.0f, 0.0f, 0.0f }, 0.2f) < 0);
        CHECK(!Navigation::SetDestination(*rt, walker, { -5.0f, 0.0f, 5.0f }) || rr.Get<NavMeshAgentComponent>(walker).PathStatus == NavPathStatus::Partial);
        CHECK(Navigation::SetDestination(*rt, runner, { 9.0f, 0.0f, -8.0f }));
        float nearPost = 100.0f, maxZ = -100.0f;
        for (int i = 0; i < 300; ++i)
        {
            rt->OnUpdateRuntime(1.0f / 60.0f);
            glm::vec3 r = rr.Get<TransformComponent>(runner).Position;
            nearPost = std::min(nearPost, glm::length(glm::vec2(r.x, r.z + 8.0f)));
            maxZ = std::max(maxZ, rr.Get<TransformComponent>(walker).Position.z);
        }
        CHECK(rr.Get<NavMeshAgentComponent>(walker).PathStatus == NavPathStatus::Partial && maxZ < -0.4f);
        CHECK(nearPost > 0.95f && Near(rr.Get<TransformComponent>(runner).Position, { 9.0f, 0.0f, -8.0f }, 0.1f));
        // The gate opens (moves away): the mesh is whole again and the walker finds its way.
        rr.Get<TransformComponent>(gate).Position = { 10.5f, 1.0f, 40.0f };
        for (int i = 0; i < 600; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(!rr.Get<NavMeshSurfaceComponent>(surface).Data->IsBlocked(inGap));
        CHECK(Near(rr.Get<TransformComponent>(walker).Position, { -5.0f, 0.0f, 5.0f }, 0.1f));
        rt->OnRuntimeStop();
    }

    void TestOffMeshLinks()
    {
        // The platform (top at 1 m, no way up) joins the ground by a link: paths up use it, the agent jumps across
        // in an arc and ends on the platform. One way only, the way down is gone.
        Scene scene("Yard");
        BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        Entity jump = scene.CreateEntity("Jump");
        auto& link = reg.Emplace<OffMeshLinkComponent>(jump);
        link.Start = { 6.5f, 0.0f, 10.0f };
        link.End = { 9.0f, 1.0f, 10.0f };
        link.ArcHeight = 1.0f;
        Entity hopper = scene.CreateEntity("Hopper");
        reg.Get<TransformComponent>(hopper).Position = { 0.0f, 0.0f, 10.0f };
        reg.Emplace<NavMeshAgentComponent>(hopper).Speed = 4.0f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        auto& mesh = *rr.Get<NavMeshSurfaceComponent>(rt->FindEntityByName("Navigation")).Data;
        auto links = Navigation::LinksOn(*rt, mesh);
        CHECK(links.size() == 2);
        std::vector<glm::vec3> corners;
        std::vector<float> arcs;
        CHECK(Navigation::FindPath(mesh, { 0.0f, 0.0f, 10.0f }, { 10.0f, 1.0f, 10.5f }, corners, links, &arcs) == NavPathStatus::Complete);
        int crossings = 0;
        for (float a : arcs) crossings += a >= 0.0f;
        CHECK(crossings == 1 && arcs.size() == corners.size() && Near(corners.back(), { 10.0f, 1.0f, 10.5f }, 0.01f));
        CHECK(Navigation::FindPath(mesh, { 0.0f, 0.0f, 10.0f }, { 10.0f, 1.0f, 10.5f }, corners) == NavPathStatus::Partial); // without links
        CHECK(Navigation::SetDestination(*rt, hopper, { 10.0f, 1.0f, 10.5f }));
        float peak = 0.0f;
        bool crossed = false;
        for (int i = 0; i < 360; ++i)
        {
            rt->OnUpdateRuntime(1.0f / 60.0f);
            peak = std::max(peak, rr.Get<TransformComponent>(hopper).Position.y);
            crossed |= rr.Get<NavMeshAgentComponent>(hopper).Traversing;
        }
        CHECK(crossed && peak > 1.3f && Near(rr.Get<TransformComponent>(hopper).Position, { 10.0f, 1.0f, 10.5f }, 0.1f));
        // One way: from the platform there is no way back down.
        rr.Get<OffMeshLinkComponent>(jump).Bidirectional = false;
        CHECK(Navigation::FindPath(mesh, { 10.0f, 1.0f, 10.0f }, { 0.0f, 0.0f, 10.0f }, corners, Navigation::LinksOn(*rt, mesh)) == NavPathStatus::Partial);
        rt->OnRuntimeStop();
    }

    void TestOffMeshDoors()
    {
        // A door onto the platform (a link with Agents Wait), closed: paths still go through it; the agent walks up to
        // it and waits; opened, it goes on. Closed without Agents Wait it is no way at all (Partial).
        Scene scene("Yard");
        BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        Entity door = scene.CreateEntity("Door");
        auto& link = reg.Emplace<OffMeshLinkComponent>(door);
        link.Start = { 6.5f, 0.0f, 10.0f };
        link.End = { 9.0f, 1.0f, 10.0f };
        link.Activated = false;
        link.AgentsWait = true;
        Entity guest = scene.CreateEntity("Guest");
        reg.Get<TransformComponent>(guest).Position = { 0.0f, 0.0f, 10.0f };
        reg.Emplace<NavMeshAgentComponent>(guest).Speed = 4.0f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        CHECK(Navigation::SetDestination(*rt, guest, { 10.0f, 1.0f, 10.5f }) && rr.Get<NavMeshAgentComponent>(guest).PathStatus == NavPathStatus::Complete);
        for (int i = 0; i < 240; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        glm::vec3 waitingAt = rr.Get<TransformComponent>(guest).Position;
        CHECK(rr.Get<NavMeshAgentComponent>(guest).Waiting && glm::distance(glm::vec2(waitingAt.x, waitingAt.z), glm::vec2(6.5f, 10.0f)) < 0.6f && waitingAt.y < 0.1f);
        rr.Get<OffMeshLinkComponent>(door).Activated = true; // open
        for (int i = 0; i < 180; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(!rr.Get<NavMeshAgentComponent>(guest).Waiting && Near(rr.Get<TransformComponent>(guest).Position, { 10.0f, 1.0f, 10.5f }, 0.1f));

        // Four more come while it is closed: they line up before it (one at it, the others an agent's width apart, back
        // the way they came), and when it opens they all go up.
        rr.Get<OffMeshLinkComponent>(door).Activated = false;
        std::vector<Entity> crowd;
        std::vector<glm::vec3> spots = { { 9.5f, 1.0f, 9.0f }, { 10.5f, 1.0f, 9.0f }, { 9.5f, 1.0f, 11.0f }, { 10.5f, 1.0f, 11.0f } };
        for (int i = 0; i < 4; ++i)
        {
            Entity e = rt->CreateEntity("Visitor");
            rr.Get<TransformComponent>(e).Position = { -4.0f - i * 1.5f, 0.0f, 6.0f + i * 1.5f };
            auto& a = rr.Emplace<NavMeshAgentComponent>(e);
            a.Speed = 4.0f;
            a.Radius = 0.4f;
            crowd.push_back(e);
        }
        rt->UpdateTransforms();
        for (int i = 0; i < 4; ++i) Navigation::SetDestination(*rt, crowd[i], spots[i]);
        for (int i = 0; i < 360; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        std::vector<float> fromDoor;
        bool allWaiting = true;
        for (Entity e : crowd)
        {
            glm::vec3 q = rr.Get<TransformComponent>(e).Position;
            fromDoor.push_back(glm::distance(glm::vec2(q.x, q.z), glm::vec2(6.5f, 10.0f)));
            allWaiting &= rr.Get<NavMeshAgentComponent>(e).Waiting;
        }
        std::sort(fromDoor.begin(), fromDoor.end());
        CHECK(allWaiting && fromDoor[0] < 0.4f);
        for (size_t i = 1; i < fromDoor.size(); ++i) CHECK(fromDoor[i] - fromDoor[i - 1] > 0.8f && fromDoor[i] - fromDoor[i - 1] < 1.6f);
        rr.Get<OffMeshLinkComponent>(door).Activated = true;
        for (int i = 0; i < 600; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        int arrived = 0;
        for (int i = 0; i < 4; ++i) arrived += glm::distance(rr.Get<TransformComponent>(crowd[i]).Position, spots[i]) < 0.6f;
        CHECK(arrived == 4);

        // A plain link, closed: no way up.
        rr.Get<OffMeshLinkComponent>(door).Activated = false;
        rr.Get<OffMeshLinkComponent>(door).AgentsWait = false;
        auto& mesh = *rr.Get<NavMeshSurfaceComponent>(rt->FindEntityByName("Navigation")).Data;
        std::vector<glm::vec3> corners;
        CHECK(Navigation::FindPath(mesh, { 0.0f, 0.0f, 10.0f }, { 10.0f, 1.0f, 10.5f }, corners, Navigation::LinksOn(*rt, mesh)) == NavPathStatus::Partial);
        rt->OnRuntimeStop();
    }

    void TestNavRootMotion()
    {
        // An agent moved by its animation: here the "animation" steps it forward (the way it faces) at the Speed
        // parameter the agent sets, up to 2.5 m/s like a walk clip. It turns toward its path, goes around the wall
        // and arrives, the parameter falling to 0; steps that would go through the wall are kept on the mesh. An agent
        // moving itself sets the parameter to its speed.
        Scene scene("Yard");
        BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        Entity walker = scene.CreateEntity("Walker");
        reg.Get<TransformComponent>(walker).Position = { -5.0f, 0.0f, -5.0f };
        auto& a = reg.Emplace<NavMeshAgentComponent>(walker);
        a.Speed = 4.0f;
        a.Acceleration = 8.0f;
        a.StoppingDistance = 0.3f;
        a.SpeedParameter = "Speed";
        a.RootMotion = true;
        reg.Emplace<AnimatorComponent>(walker).ApplyRootMotion = true;
        Entity runner = scene.CreateEntity("Runner");
        reg.Get<TransformComponent>(runner).Position = { 10.0f, 0.0f, -10.0f };
        reg.Emplace<NavMeshAgentComponent>(runner).SpeedParameter = "Speed";
        reg.Emplace<AnimatorComponent>(runner);
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        auto param = [&](Entity e) {
            auto& p = rr.Get<AnimatorComponent>(e).Runtime.Parameters;
            auto it = p.find("Speed");
            return it == p.end() ? -1.0f : it->second;
        };
        const float dt = 1.0f / 60.0f;
        auto animate = [&](float speed) {
            auto& t = rr.Get<TransformComponent>(walker);
            float yaw = t.Rotation.y;
            glm::vec3 step = glm::vec3(-std::sin(yaw), 0.0f, -std::cos(yaw)) * std::min(speed, 2.5f) * dt;
            t.Position += step; // as root motion does
            t.World[3] += glm::vec4(step, 0.0f);
        };
        CHECK(Navigation::SetDestination(*rt, walker, { -5.0f, 0.0f, 5.0f }) && Navigation::SetDestination(*rt, runner, { 10.0f, 0.0f, 0.0f }));
        bool throughWall = false;
        float most = 0.0f, runnerMost = 0.0f;
        for (int i = 0; i < 900; ++i)
        {
            rt->OnUpdateRuntime(dt);
            most = std::max(most, param(walker));
            runnerMost = std::max(runnerMost, param(runner));
            animate(param(walker));
            glm::vec3 p = rr.Get<TransformComponent>(walker).Position;
            throughWall |= std::abs(p.z) < 0.5f && p.x < 6.0f;
        }
        rt->OnUpdateRuntime(dt);
        glm::vec3 at = rr.Get<TransformComponent>(walker).Position;
        CHECK(!throughWall && glm::distance(glm::vec2(at.x, at.z), glm::vec2(-5.0f, 5.0f)) < 0.6f);
        CHECK(most > 2.5f && param(walker) < 0.3f);
        CHECK(runnerMost > 3.0f && param(runner) < 0.1f);
        // Animation stepping straight at the wall: it stops at the wall's edge on the mesh.
        Navigation::ResetPath(*rt, walker);
        rr.Get<TransformComponent>(walker).Rotation.y = 0.0f; // facing -Z, toward the wall
        for (int i = 0; i < 120; ++i)
        {
            animate(2.5f);
            rt->OnUpdateRuntime(dt);
        }
        CHECK(rr.Get<TransformComponent>(walker).Position.z > 0.5f);
        rt->OnRuntimeStop();
    }

    void TestNavMovingPlatforms()
    {
        // A barge (a deck 6 x 10 m, 3 m up) with a Moving surface sails off and turns while an agent walks from its
        // stern to its bow: the yard's own surface leaves the barge out, the agent is carried along, keeps to the deck
        // and arrives at the bow wherever the barge has got to, and turns with it afterwards.
        Scene scene("Yard");
        Entity yard = BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        Entity barge = scene.CreateEntity("Barge");
        reg.Get<TransformComponent>(barge).Position = { 0.0f, 3.0f, 6.0f };
        reg.Emplace<BoxColliderComponent>(barge).Size = { 6.0f, 0.5f, 10.0f };
        auto& deck = reg.Emplace<NavMeshSurfaceComponent>(barge);
        deck.Size = { 8.0f, 3.0f, 12.0f };
        deck.Moving = true;
        Entity sailor = scene.CreateEntity("Sailor");
        reg.Get<TransformComponent>(sailor).Position = { 0.0f, 3.25f, 9.5f };
        reg.Emplace<NavMeshAgentComponent>(sailor).Speed = 1.5f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        glm::vec3 onDeck;
        CHECK(Navigation::SamplePosition(*rt, { 0.0f, 3.5f, 6.0f }, 1.0f, onDeck) && Near(onDeck.y, 3.25f, 0.05f));
        CHECK(Navigation::Nearest(*rr.Get<NavMeshSurfaceComponent>(yard).Data, { 0.0f, 3.25f, 6.0f }, 0.5f) < 0); // not in the yard's mesh
        CHECK(Navigation::SetDestination(*rt, sailor, { 0.0f, 3.25f, 2.5f }));
        const float dt = 1.0f / 60.0f;
        auto local = [&] {
            return glm::vec3(glm::inverse(rr.Get<TransformComponent>(barge).World) * glm::vec4(rr.Get<TransformComponent>(sailor).GetWorldPosition(), 1.0f));
        };
        bool kept = true;
        for (int i = 0; i < 360; ++i)
        {
            auto& bt = rr.Get<TransformComponent>(barge);
            bt.Position.x += 1.5f * dt; // sailing along +X, turning
            bt.Rotation.y += 0.25f * dt;
            rt->OnUpdateRuntime(dt);
            glm::vec3 l = local();
            kept &= std::abs(l.x) < 3.0f && std::abs(l.z) < 5.0f && Near(l.y, 0.25f, 0.1f);
        }
        glm::vec3 l = local();
        CHECK(kept && Near(l, { 0.0f, 0.25f, -3.5f }, 0.3f)); // at the bow, as set when the barge stood at the start
        CHECK(rr.Get<TransformComponent>(sailor).GetWorldPosition().x > 4.0f); // carried along
        float yaw0 = rr.Get<TransformComponent>(sailor).Rotation.y;
        for (int i = 0; i < 60; ++i)
        {
            rr.Get<TransformComponent>(barge).Rotation.y += 0.5f * dt;
            rt->OnUpdateRuntime(dt);
        }
        CHECK(Near(rr.Get<TransformComponent>(sailor).Rotation.y - yaw0, 0.5f, 0.05f) && Near(local(), { 0.0f, 0.25f, -3.5f }, 0.3f));
        rt->OnRuntimeStop();
    }

    void TestNavCrossSurfaces()
    {
        // A barge (Moving surface) drifting by the yard with a gangway (an off-mesh link on it) down to the ground: an
        // agent in the yard sent onto the deck walks to the gangway, crosses over and walks the deck to the spot; sent
        // back into the yard it crosses back. Without the gangway there is no way over (a partial path).
        Scene scene("Yard");
        Entity yard = BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        Entity barge = scene.CreateEntity("Barge");
        reg.Get<TransformComponent>(barge).Position = { 0.0f, 3.0f, 6.0f };
        reg.Emplace<BoxColliderComponent>(barge).Size = { 6.0f, 0.5f, 10.0f };
        auto& deck = reg.Emplace<NavMeshSurfaceComponent>(barge);
        deck.Size = { 8.0f, 3.0f, 12.0f };
        deck.Moving = true;
        Entity gangway = scene.CreateEntity("Gangway", barge);
        auto& link = reg.Emplace<OffMeshLinkComponent>(gangway);
        link.Start = { 0.0f, -3.0f, 6.5f }; // the ground beside the barge
        link.End = { 0.0f, 0.25f, 4.0f };   // its deck
        link.ArcHeight = 0.0f;
        Entity walker = scene.CreateEntity("Walker");
        reg.Get<TransformComponent>(walker).Position = { -8.0f, 0.0f, 14.0f };
        reg.Emplace<NavMeshAgentComponent>(walker).Speed = 3.5f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        const float dt = 1.0f / 60.0f;
        auto run = [&](int frames) {
            for (int i = 0; i < frames; ++i)
            {
                rr.Get<TransformComponent>(barge).Position.x += 0.15f * dt; // drifting
                rt->OnUpdateRuntime(dt);
            }
        };
        auto onDeck = [&] {
            return glm::vec3(glm::inverse(rr.Get<TransformComponent>(barge).World) * glm::vec4(rr.Get<TransformComponent>(walker).GetWorldPosition(), 1.0f));
        };
        auto& a = rr.Get<NavMeshAgentComponent>(walker);
        CHECK(Navigation::SetDestination(*rt, walker, { 1.0f, 3.25f, 4.0f }) && a.PathStatus == NavPathStatus::Complete && a.CrossMesh);
        run(600);
        CHECK(a.Mesh == rr.Get<NavMeshSurfaceComponent>(barge).Data && Near(onDeck(), { 1.0f, 0.25f, -2.0f }, 0.3f));
        CHECK(Navigation::SetDestination(*rt, walker, { 8.0f, 0.0f, 14.0f }));
        run(600);
        CHECK(a.Mesh == rr.Get<NavMeshSurfaceComponent>(yard).Data && Near(rr.Get<TransformComponent>(walker).Position, { 8.0f, 0.0f, 14.0f }, 0.3f));
        rr.Get<OffMeshLinkComponent>(gangway).Activated = false;
        Navigation::SetDestination(*rt, walker, { 1.0f, 3.25f, 4.0f });
        CHECK(a.PathStatus == NavPathStatus::Partial && !a.CrossMesh);
        rt->OnRuntimeStop();
    }

    void TestNavQueriesAndPaths()
    {
        // The yard (BuildNavYard): a wall along z = 0 from x = -15 to 6, 0.6 thick; the mesh stops Agent Radius from it.
        Scene scene("Yard");
        BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        Entity walker = scene.CreateEntity("Walker");
        reg.Get<TransformComponent>(walker).Position = { -5.0f, 0.0f, -5.0f };
        auto& a = reg.Emplace<NavMeshAgentComponent>(walker);
        a.Speed = 6.0f;
        a.Acceleration = 20.0f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        auto pos = [&] { return rr.Get<TransformComponent>(walker).Position; };
        auto run = [&](int n) { for (int i = 0; i < n; ++i) rt->OnUpdateRuntime(1.0f / 60.0f); };
        run(1);
        float edgeZ = -0.3f - 0.5f; // the wall's face, less the bake's Agent Radius
        // Straight at the wall: stopped at the mesh's edge before it, facing back; a short way is clear.
        Navigation::NavHit hit;
        CHECK(Navigation::Raycast(*rt, { -5, 0, -5 }, { -5, 0, 5 }, hit) && hit.Hit);
        CHECK(Near(hit.Position.z, edgeZ, 0.3f) && Near(hit.Position.x, -5.0f, 0.05f) && hit.Normal.z < -0.9f && Near(hit.Distance, 5.0f + edgeZ, 0.3f));
        CHECK(!Navigation::Raycast(*rt, { -5, 0, -5 }, { -5, 0, -3 }, hit) && !hit.Hit && Near(hit.Position.z, -3.0f, 0.05f) && Near(hit.Distance, 2.0f, 0.05f));
        // The nearest edge from 1.2 m short of it.
        CHECK(Navigation::FindClosestEdge(*rt, { -5, 0, edgeZ - 1.2f }, hit) && Near(hit.Distance, 1.2f, 0.3f) && Near(hit.Position.z, edgeZ, 0.3f) && hit.Normal.z < -0.9f);
        CHECK(!Navigation::FindClosestEdge(*rt, { 50, 0, 50 }, hit)); // off the mesh
        // A path found first, then given to the agent: it walks it (around the wall's end) and arrives.
        std::vector<glm::vec3> corners;
        CHECK(Navigation::CalculatePath(*rt, { -5, 0, -5 }, { -5, 0, 5 }, corners) == NavPathStatus::Complete && corners.size() >= 3);
        CHECK(Navigation::CalculatePath(*rt, { -5, 0, -5 }, { -5, 0, 5 }, corners, 0u) == NavPathStatus::Invalid); // no areas allowed
        CHECK(Navigation::CalculatePath(*rt, { -5, 0, -5 }, { -5, 0, 5 }, corners) == NavPathStatus::Complete);
        CHECK(Navigation::SetPath(*rt, walker, corners, NavPathStatus::Complete));
        auto given = Navigation::GetPath(*rt, walker);
        CHECK(given.size() == corners.size() && Near(given.back(), corners.back(), 1e-3f));
        run(600);
        CHECK(glm::distance(pos(), glm::vec3(-5, 0, 5)) < 0.5f);
        // A path of its own: straight to a point (corners from where it stands).
        CHECK(Navigation::SetPath(*rt, walker, { pos(), { -8, 0, 8 } }, NavPathStatus::Complete));
        run(120);
        CHECK(glm::distance(pos(), glm::vec3(-8, 0, 8)) < 0.5f);
        // Scripts moving it: with Update Position off the object stays while its NextPosition walks; on again, it jumps there.
        Navigation::SetUpdatePosition(*rt, walker, false);
        rr.Get<NavMeshAgentComponent>(walker).UpdateRotation = false;
        float yaw = rr.Get<TransformComponent>(walker).Rotation.y;
        glm::vec3 stay = pos();
        CHECK(Navigation::SetDestination(*rt, walker, { -12, 0, 8 }));
        run(90);
        glm::vec3 next = Navigation::GetNextPosition(*rt, walker);
        CHECK(Near(pos(), stay, 1e-4f) && next.x < stay.x - 2.0f && Near(rr.Get<TransformComponent>(walker).Rotation.y, yaw, 1e-5f));
        Navigation::SetUpdatePosition(*rt, walker, true);
        CHECK(Near(pos(), next, 1e-3f));
        run(120);
        CHECK(glm::distance(pos(), glm::vec3(-12, 0, 8)) < 0.5f);
        rt->OnRuntimeStop();
    }

    void TestNavMeshAgents()
    {
        // Playing (baked as play starts): an agent sent to the far side of the wall walks around its end and arrives;
        // two agents walking at each other pass without bumping; one with a Target follows it.
        Scene scene("Yard");
        BuildNavYard(scene);
        auto& reg = scene.GetRegistry();
        auto agent = [&](const char* name, glm::vec3 at) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = at;
            auto& a = reg.Emplace<NavMeshAgentComponent>(e);
            a.Speed = 6.0f;
            a.Acceleration = 20.0f;
            return e;
        };
        Entity walker = agent("Walker", { -5.0f, 0.0f, -5.0f });
        Entity left = agent("Left", { -9.0f, 0.0f, -8.0f });
        Entity right = agent("Right", { 9.0f, 0.0f, -8.0f });
        reg.Get<NavMeshAgentComponent>(left).Speed = reg.Get<NavMeshAgentComponent>(right).Speed = 3.0f;
        Entity flag = scene.CreateEntity("Flag");
        reg.Get<TransformComponent>(flag).Position = { 8.0f, 0.0f, -13.0f };
        Entity follower = agent("Follower", { 0.0f, 0.0f, -12.0f });
        reg.Get<NavMeshAgentComponent>(follower).Target = flag;
        reg.Get<NavMeshAgentComponent>(follower).StoppingDistance = 1.0f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        CHECK(Navigation::SetDestination(*rt, walker, { -5.0f, 0.0f, 5.0f }));
        CHECK(Navigation::SetDestination(*rt, left, { 9.0f, 0.0f, -8.0f }) && Navigation::SetDestination(*rt, right, { -9.0f, 0.0f, -8.0f }));
        bool throughWall = false;
        float closest = 100.0f;
        for (int i = 0; i < 600; ++i)
        {
            rt->OnUpdateRuntime(1.0f / 60.0f);
            glm::vec3 p = rr.Get<TransformComponent>(walker).Position;
            throughWall |= std::abs(p.z) < 0.5f && p.x < 6.0f;
            closest = std::min(closest, glm::distance(rr.Get<TransformComponent>(left).Position, rr.Get<TransformComponent>(right).Position));
        }
        auto& w = rr.Get<NavMeshAgentComponent>(walker);
        CHECK(!throughWall && Near(rr.Get<TransformComponent>(walker).Position, { -5.0f, 0.0f, 5.0f }, 0.1f) && w.RemainingDistance < 0.1f);
        CHECK(glm::length(w.Velocity) < 0.2f);
        CHECK(closest > 0.85f);
        CHECK(Near(rr.Get<TransformComponent>(left).Position, { 9.0f, 0.0f, -8.0f }, 0.2f) && Near(rr.Get<TransformComponent>(right).Position, { -9.0f, 0.0f, -8.0f }, 0.2f));
        float toFlag = glm::distance(rr.Get<TransformComponent>(follower).Position, { 8.0f, 0.0f, -13.0f });
        CHECK(toFlag > 0.9f && toFlag < 1.2f);
        // The flag moves: the follower goes after it.
        rr.Get<TransformComponent>(flag).Position = { -10.0f, 0.0f, -13.0f };
        for (int i = 0; i < 360; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(glm::distance(rr.Get<TransformComponent>(follower).Position, { -10.0f, 0.0f, -13.0f }) < 1.2f);
        // Facing the way it walked (forward -Z turned toward -X: yaw +90 degrees).
        CHECK(Near(rr.Get<TransformComponent>(follower).Rotation.y, glm::half_pi<float>(), 0.2f));
        glm::vec3 sample;
        CHECK(Navigation::SamplePosition(*rt, { 3.0f, 2.0f, 3.0f }, 3.0f, sample) && Near(sample, { 3.0f, 0.0f, 3.0f }, 0.01f));
        CHECK(!Navigation::SamplePosition(*rt, { 3.0f, 8.0f, 3.0f }, 3.0f, sample));
        rt->OnRuntimeStop();
    }

    void TestProfiler()
    {
        // Sections nest, repeated ones add up (calls counted), counters keep their last value, and a frame is recorded
        // when the next one begins.
        Profiler::Clear();
        Profiler::SetPaused(false);
        Profiler::BeginFrame();
        {
            DIM_PROFILE_SCOPE("Update");
            for (int i = 0; i < 3; ++i)
            {
                DIM_PROFILE_SCOPE("Physics");
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            DIM_PROFILE_SCOPE("Scripts");
        }
        {
            DIM_PROFILE_SCOPE("Render");
        }
        Profiler::SetCounter("Draw Calls", 10.0f);
        Profiler::SetCounter("Draw Calls", 12.0f);
        Profiler::BeginFrame();
        const auto& history = Profiler::History();
        CHECK(history.size() == 1);
        if (history.size() != 1) return;
        const auto& f = history.back();
        CHECK(f.Sections.size() == 4);
        if (f.Sections.size() != 4) return;
        CHECK(std::string(f.Sections[0].Name) == "Update" && f.Sections[0].Depth == 0 && f.Sections[0].Calls == 1);
        CHECK(std::string(f.Sections[1].Name) == "Physics" && f.Sections[1].Depth == 1 && f.Sections[1].Calls == 3 && f.Sections[1].Ms >= 5.0f);
        CHECK(std::string(f.Sections[2].Name) == "Scripts" && f.Sections[2].Depth == 1);
        CHECK(std::string(f.Sections[3].Name) == "Render" && f.Sections[3].Depth == 0);
        CHECK(f.Sections[0].Ms >= f.Sections[1].Ms && f.FrameMs >= f.Sections[0].Ms);
        CHECK(f.Counters.size() == 1 && f.Counters[0].second == 12.0f);
        // Paused: nothing more is recorded.
        Profiler::SetPaused(true);
        Profiler::BeginFrame();
        Profiler::BeginFrame();
        CHECK(Profiler::History().size() == 1);
        Profiler::SetPaused(false);
        Profiler::Clear();
    }

    void TestOcclusionTest()
    {
        // A camera at the origin looking down -Z; the depth grid as if a wall 5 m away filled the left half of the
        // view (sky, depth 1, elsewhere).
        glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
        OccluderDepth occ;
        occ.Width = 64;
        occ.Height = 36;
        occ.ViewProj = proj * view;
        glm::vec4 wall = proj * glm::vec4(0, 0, -5, 1);
        float wallDepth = wall.z / wall.w;
        occ.Farthest.assign((size_t)occ.Width * occ.Height, 1.0f);
        for (uint32_t y = 0; y < occ.Height; ++y)
            for (uint32_t x = 0; x < occ.Width / 2; ++x) occ.Farthest[y * occ.Width + x] = wallDepth;
        CHECK(occ.Hides({ -6.0f, 0.0f, -20.0f }, glm::vec3(0.5f)));  // behind the wall
        CHECK(!occ.Hides({ -1.0f, 0.0f, -3.0f }, glm::vec3(0.5f))); // in front of it
        CHECK(!occ.Hides({ 6.0f, 0.0f, -20.0f }, glm::vec3(0.5f)));  // beside it: open sky
        CHECK(!occ.Hides({ -0.6f, 0.0f, -20.0f }, glm::vec3(0.5f))); // reaching past its edge (with the ring around)
        CHECK(!occ.Hides({ 0.0f, 0.0f, 1.0f }, glm::vec3(0.5f)));    // behind the camera
        CHECK(!occ.Hides({ -6.0f, 0.0f, -20.0f }, glm::vec3(30.0f))); // too big to judge
        // A low wall: the bottom half of the view (the top rows of the grid are the top of the view).
        occ.Farthest.assign((size_t)occ.Width * occ.Height, 1.0f);
        for (uint32_t y = occ.Height / 2; y < occ.Height; ++y)
            for (uint32_t x = 0; x < occ.Width; ++x) occ.Farthest[y * occ.Width + x] = wallDepth;
        CHECK(occ.Hides({ 0.0f, -6.0f, -20.0f }, glm::vec3(0.5f)) && !occ.Hides({ 0.0f, 6.0f, -20.0f }, glm::vec3(0.5f)));
        OccluderDepth none;
        CHECK(!none.Hides({ -6.0f, 0.0f, -20.0f }, glm::vec3(0.5f)));
    }

    void TestMeshSimplify()
    {
        // A flat 32 x 32 grid (two materials, left and right half): flat ground collapses to few triangles, its
        // outline and the line between the materials stay, and the area is kept.
        std::vector<Vertex> verts;
        std::vector<uint32_t> left, right;
        const int n = 32;
        for (int z = 0; z <= n; ++z)
            for (int x = 0; x <= n; ++x)
                verts.push_back({ { x / (float)n, 0.0f, z / (float)n }, { 0, 1, 0 }, { x / (float)n, z / (float)n } });
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x)
            {
                uint32_t a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
                auto& out = x < n / 2 ? left : right;
                out.insert(out.end(), { a, c, b, b, c, d });
            }
        MeshData grid;
        grid.Vertices = verts;
        grid.Indices = left;
        grid.Indices.insert(grid.Indices.end(), right.begin(), right.end());
        grid.SubMeshes = { { 0, (uint32_t)left.size(), 0 }, { (uint32_t)left.size(), (uint32_t)right.size(), 1 } };
        auto area = [](const MeshData& m, int sub) {
            float sum = 0.0f;
            const SubMesh& s = m.SubMeshes[sub];
            for (uint32_t i = s.IndexOffset; i < s.IndexOffset + s.IndexCount; i += 3)
            {
                glm::vec3 a = m.Vertices[m.Indices[i]].Position, b = m.Vertices[m.Indices[i + 1]].Position, c = m.Vertices[m.Indices[i + 2]].Position;
                sum += 0.5f * glm::length(glm::cross(b - a, c - a));
            }
            return sum;
        };
        MeshData simple = MeshSimplify::Simplify(grid, 0.1f);
        CHECK(MeshSimplify::TriangleCount(grid) == 2048 && MeshSimplify::TriangleCount(simple) <= 205);
        CHECK(simple.SubMeshes.size() == 2 && simple.SubMeshes[0].IndexCount > 0 && simple.SubMeshes[1].IndexCount > 0);
        CHECK(Near(area(simple, 0), 0.5f, 1e-3f) && Near(area(simple, 1), 0.5f, 1e-3f)); // nothing lost, halves still meet at x = 0.5
        CHECK(simple.Vertices.size() == grid.Vertices.size()); // the vertices stay; only triangles change
        // Every triangle still faces up.
        bool up = true;
        for (size_t i = 0; i < simple.Indices.size(); i += 3)
        {
            glm::vec3 a = simple.Vertices[simple.Indices[i]].Position, b = simple.Vertices[simple.Indices[i + 1]].Position, c = simple.Vertices[simple.Indices[i + 2]].Position;
            up &= glm::cross(b - a, c - a).y > 0.0f;
        }
        CHECK(up);

        // A sphere: a quarter of the triangles, still closed (every edge between two triangles) and round; with a
        // small error limit it stops early.
        fs::path assets = fs::temp_directory_path() / "dimension_simplify" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        auto sphere = AssetManager::GetMeshData(AssetManager::kSphere);
        CHECK(sphere != nullptr);
        if (sphere)
        {
            size_t full = MeshSimplify::TriangleCount(*sphere);
            MeshData quarter = MeshSimplify::Simplify(*sphere, 0.25f, 0.2f);
            size_t count = MeshSimplify::TriangleCount(quarter);
            CHECK(count <= full / 4 + 2 && count > full / 8);
            std::map<std::pair<glm::vec3, glm::vec3>, int, bool (*)(const std::pair<glm::vec3, glm::vec3>&, const std::pair<glm::vec3, glm::vec3>&)> edges(
                [](const std::pair<glm::vec3, glm::vec3>& a, const std::pair<glm::vec3, glm::vec3>& b) {
                    auto key = [](const std::pair<glm::vec3, glm::vec3>& e) { return std::make_tuple(e.first.x, e.first.y, e.first.z, e.second.x, e.second.y, e.second.z); };
                    return key(a) < key(b);
                });
            float minRadius = 1.0f;
            for (size_t i = 0; i < quarter.Indices.size(); i += 3)
                for (int k = 0; k < 3; ++k)
                {
                    // (to a ten-thousandth: the generated sphere's seam and poles are a rounding error apart)
                    glm::vec3 a = glm::round(quarter.Vertices[quarter.Indices[i + k]].Position * 1e4f), b = glm::round(quarter.Vertices[quarter.Indices[i + (k + 1) % 3]].Position * 1e4f);
                    auto less = [](glm::vec3 u, glm::vec3 v) { return std::tie(u.x, u.y, u.z) < std::tie(v.x, v.y, v.z); };
                    edges[less(a, b) ? std::make_pair(a, b) : std::make_pair(b, a)]++;
                    // The middle of each triangle stays near the surface (a quarter of the triangles is still round).
                    glm::vec3 c = (quarter.Vertices[quarter.Indices[i]].Position + quarter.Vertices[quarter.Indices[i + 1]].Position +
                                   quarter.Vertices[quarter.Indices[i + 2]].Position) / 3.0f;
                    minRadius = std::min(minRadius, glm::length(c));
                }
            bool closed = true;
            for (auto& [e, uses] : edges) closed &= uses == 2;
            CHECK(closed && minRadius > 0.4f);
            CHECK(MeshSimplify::TriangleCount(MeshSimplify::Simplify(*sphere, 0.01f, 0.002f)) > full / 2);
        }
        AssetManager::Shutdown();
    }

    void TestLODFadeAndGenerated()
    {
        // Within Fade Width above a threshold the level shares the pixels with the next: at 0.52 of the screen (LOD 1
        // Below 0.5, Fade Width 0.1) LOD 0 keeps 40% of them and LOD 1 the rest; LOD 2 stays hidden.
        Scene scene("Rocks");
        auto& reg = scene.GetRegistry();
        Entity rock = scene.CreateEntity("Rock");
        auto& lod = reg.Emplace<LODGroupComponent>(rock);
        lod.Size = 2.0f;
        Entity l0 = scene.CreateEntity("LOD0", rock), l1 = scene.CreateEntity("LOD1", rock), l2 = scene.CreateEntity("LOD2", rock);
        scene.UpdateTransforms();
        glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
        float d = 2.0f * (1.0f / std::tan(glm::radians(30.0f))) * 0.5f / 0.52f;
        LODGroups::Pick pick = LODGroups::Evaluate(scene, rock, { 0.0f, 0.0f, d }, proj, false);
        CHECK(pick.Level == 0 && pick.Next == 1 && Near(pick.Coverage, 0.4f, 0.01f));
        LODGroups::Frame frame;
        LODGroups::Evaluate(scene, { 0.0f, 0.0f, d }, proj, false, frame);
        CHECK(frame.Fade.count(l0.Index) && frame.Fade[l0.Index] == 102u && frame.Fade.count(l1.Index) && frame.Fade[l1.Index] == (102u | 256u));
        CHECK(l2.Index < frame.Hidden.size() && frame.Hidden[l2.Index]);
        // Well inside a level: no fade. Fade Width 0: switching at once.
        CHECK(LODGroups::Evaluate(scene, rock, { 0.0f, 0.0f, 2.0f }, proj, false).Next == -1);
        lod.FadeWidth = 0.0f;
        CHECK(LODGroups::Evaluate(scene, rock, { 0.0f, 0.0f, d }, proj, false).Coverage == 1.0f);
        lod.FadeWidth = 0.1f;
        // The last level fades out to nothing before culling (Cull Below 0.01).
        float dc = 2.0f * (1.0f / std::tan(glm::radians(30.0f))) * 0.5f / 0.0105f;
        pick = LODGroups::Evaluate(scene, rock, { 0.0f, 0.0f, dc }, proj, false);
        CHECK(pick.Level == 2 && pick.Next == -1 && Near(pick.Coverage, 0.5f, 0.02f));

        // Generated levels: the group's own mesh is LOD 0 and LODs 1-2 are made from it, half the triangles each.
        fs::path assets = fs::temp_directory_path() / "dimension_lodgen" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        Entity statue = scene.CreateEntity("Statue");
        reg.Emplace<MeshRendererComponent>(statue).Mesh = AssetManager::kSphere;
        auto& gen = reg.Emplace<LODGroupComponent>(statue);
        gen.GeneratedLevels = 2;
        gen.Size = 2.0f;
        scene.UpdateTransforms();
        CHECK(LODGroups::LevelCount(scene, statue) == 3);
        LODGroups::Frame distant;
        LODGroups::Evaluate(scene, { 0.0f, 0.0f, 5.0f }, proj, false, distant); // 0.35 of the screen: LOD 1
        CHECK(distant.Generated.count(statue.Index) && distant.Generated[statue.Index].Level == 1);
        AssetHandle level1 = LODGroups::GeneratedMesh(AssetManager::kSphere, 1, 0.5f), level2 = LODGroups::GeneratedMesh(AssetManager::kSphere, 2, 0.5f);
        auto full = AssetManager::GetMeshData(AssetManager::kSphere);
        auto half = AssetManager::GetMeshData(level1), quarter = AssetManager::GetMeshData(level2);
        CHECK(full && half && quarter && level1 != AssetManager::kSphere && LODGroups::GeneratedMesh(AssetManager::kSphere, 1, 0.5f) == level1);
        if (full && half && quarter)
        {
            size_t f = MeshSimplify::TriangleCount(*full), h = MeshSimplify::TriangleCount(*half), q = MeshSimplify::TriangleCount(*quarter);
            CHECK(h <= f / 2 + 2 && h > f / 3 && q <= f / 4 + 2 && q > f / 8);
        }
        AssetManager::Shutdown();
    }

    void TestLODImpostor()
    {
        // With Impostor, a group of 3 children has a 4th level: past LOD 3 Below its children all hide and the group
        // draws its impostor (fading in over the last child within Fade Width), down to Cull Below.
        Scene scene("Rocks");
        auto& reg = scene.GetRegistry();
        Entity rock = scene.CreateEntity("Rock");
        auto& lod = reg.Emplace<LODGroupComponent>(rock);
        lod.Size = 2.0f;
        lod.Impostor = true;
        Entity l0 = scene.CreateEntity("LOD0", rock), l1 = scene.CreateEntity("LOD1", rock), l2 = scene.CreateEntity("LOD2", rock);
        scene.UpdateTransforms();
        CHECK(LODGroups::LevelCount(scene, rock) == 4 && LODGroups::ImpostorLevel(scene, rock) == 3);
        glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
        auto atHeight = [&](float h) { return glm::vec3(0.0f, 0.0f, 2.0f * (1.0f / std::tan(glm::radians(30.0f))) * 0.5f / h); };
        LODGroups::Frame distant;
        LODGroups::Evaluate(scene, atHeight(0.05f), proj, false, distant); // below LOD 3 Below (0.12): the impostor
        CHECK(distant.Impostors.size() == 1 && distant.Impostors[0].first == rock && distant.Impostors[0].second == 0u);
        for (Entity c : { l0, l1, l2 }) CHECK(c.Index < distant.Hidden.size() && distant.Hidden[c.Index]);
        LODGroups::Frame between;
        LODGroups::Evaluate(scene, atHeight(0.125f), proj, false, between); // just above it: LOD 2 fading into the impostor
        CHECK(between.Impostors.size() == 1 && (between.Impostors[0].second & 256u) && between.Fade.count(l2.Index));
        LODGroups::Frame close;
        LODGroups::Evaluate(scene, atHeight(0.8f), proj, false, close);
        CHECK(close.Impostors.empty());
        LODGroups::Frame gone;
        LODGroups::Evaluate(scene, atHeight(0.005f), proj, false, gone); // below Cull Below: nothing
        CHECK(gone.Impostors.empty());
    }

    void TestLODGroups()
    {
        // A 2 m object seen by a 60 degree camera covers 1.73 / distance of the view's height: LOD 0 near, LOD 1 from
        // 3.5 m (below 0.5), LOD 2 from 6.9 m (below 0.25), culled from 173 m (below 0.01; its last child).
        Scene scene("Rocks");
        auto& reg = scene.GetRegistry();
        Entity rock = scene.CreateEntity("Rock");
        auto& lod = reg.Emplace<LODGroupComponent>(rock);
        lod.Size = 2.0f;
        Entity l0 = scene.CreateEntity("Rock_LOD0", rock), l1 = scene.CreateEntity("Rock_LOD1", rock), l2 = scene.CreateEntity("Rock_LOD2", rock);
        Entity moss = scene.CreateEntity("Moss", l0);
        scene.UpdateTransforms();
        glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
        auto at = [&](float d) { return LODGroups::Choose(scene, rock, { 0.0f, 0.0f, d }, proj, false); };
        float h = 0.0f;
        CHECK(LODGroups::Choose(scene, rock, { 0.0f, 0.0f, 2.0f }, proj, false, &h) == 0 && Near(h, 0.866f, 0.01f));
        CHECK(at(3.0f) == 0 && at(4.0f) == 1 && at(6.5f) == 1 && at(7.5f) == 2 && at(150.0f) == 2 && at(200.0f) == -1);
        lod.Bias = 2.0f; // keeps detail twice as far
        CHECK(at(6.5f) == 0 && at(7.5f) == 1);
        lod.Bias = 1.0f;
        std::vector<uint8_t> hidden;
        LODGroups::Hidden(scene, { 0.0f, 0.0f, 5.0f }, proj, false, hidden);
        auto isHidden = [&](Entity e) { return e.Index < hidden.size() && hidden[e.Index]; };
        CHECK(isHidden(l0) && isHidden(moss) && !isHidden(l1) && isHidden(l2) && !isHidden(rock));
        // Orthographic: by the view's height alone (a 10 m tall view: 2 m is 0.2 of it).
        glm::mat4 ortho = glm::ortho(-8.9f, 8.9f, -5.0f, 5.0f, 0.1f, 100.0f);
        CHECK(LODGroups::Choose(scene, rock, { 0.0f, 0.0f, 50.0f }, ortho, true, &h) == 2 && Near(h, 0.2f, 1e-3f));
        // Size 0: the children's meshes (unit cubes, one of them 3 m tall).
        fs::path assets = fs::temp_directory_path() / "dimension_lod" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        lod.Size = 0.0f;
        reg.Emplace<MeshRendererComponent>(l0).Mesh = AssetManager::kCube;
        reg.Emplace<MeshRendererComponent>(l1).Mesh = AssetManager::kCube;
        reg.Get<TransformComponent>(l1).Scale = { 1.0f, 3.0f, 1.0f };
        scene.UpdateTransforms();
        CHECK(Near(LODGroups::WorldSize(scene, rock), 3.0f, 1e-3f));
        AssetManager::Shutdown();
    }

    void TestSoftBuoyancy()
    {
        // A loose sheet of cloth and a soft ball dropped into water float at the surface; in water too thin to hold
        // them (Buoyancy below 1) they sink to the bottom.
        fs::path assets = fs::temp_directory_path() / "dimension_soft_buoyancy" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        auto run = [&](float buoyancy) {
            Scene scene("Pond");
            auto& reg = scene.GetRegistry();
            Entity bed = scene.CreateEntity("Bed");
            reg.Get<TransformComponent>(bed).Position = { 0, -4.5f, 0 };
            reg.Emplace<BoxColliderComponent>(bed).Size = { 30, 1, 30 };
            Entity pond = scene.CreateEntity("Pond");
            reg.Get<TransformComponent>(pond).Position = { 0, -2, 0 };
            auto& water = reg.Emplace<WaterVolumeComponent>(pond);
            water.Size = { 20, 4, 20 }; // the surface at y = 0
            water.Buoyancy = buoyancy;
            water.LinearDrag = 1.0f;
            Entity sheet = scene.CreateEntity("Sheet");
            reg.Get<TransformComponent>(sheet).Position = { -3, 1, 0 };
            reg.Get<TransformComponent>(sheet).Rotation = { -glm::half_pi<float>(), 0, 0 }; // lying flat
            auto& c = reg.Emplace<ClothComponent>(sheet);
            c.Pin = ClothPin::None;
            c.Resolution = 10;
            reg.Emplace<MeshRendererComponent>(sheet);
            Entity ball = scene.CreateEntity("Ball");
            reg.Get<TransformComponent>(ball).Position = { 3, 1.5f, 0 };
            reg.Emplace<MeshRendererComponent>(ball).Mesh = AssetManager::kSphere;
            reg.Emplace<SoftBodyComponent>(ball);
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < 300; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            auto& rr = rt->GetRegistry();
            auto meanY = [&](Entity e, const std::vector<glm::vec3>& points) {
                float sum = 0.0f;
                for (const glm::vec3& q : points) sum += glm::vec3(rr.Get<TransformComponent>(e).World * glm::vec4(q, 1.0f)).y;
                return points.empty() ? 1e9f : sum / points.size();
            };
            glm::vec2 out(meanY(sheet, rr.Get<ClothComponent>(sheet).SimulatedPositions), meanY(ball, rr.Get<SoftBodyComponent>(ball).SimulatedPositions));
            rt->OnRuntimeStop();
            return out;
        };
        glm::vec2 floating = run(1.8f);
        CHECK(floating.x > -0.3f && floating.x < 0.3f);  // the sheet lies on the surface
        CHECK(floating.y > -0.6f && floating.y < 0.6f);  // the ball bobs in it
        glm::vec2 sunk = run(0.3f);
        CHECK(sunk.x < -3.0f && sunk.y < -3.0f);
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestRopeSnap()
    {
        // A lamp hanging on a rope loads it with its weight; over Break Force the rope snaps and the lamp falls.
        auto lampScene = [&](float breakForce) {
            auto scene = std::make_unique<Scene>("Lamp");
            auto& reg = scene->GetRegistry();
            Entity floor = scene->CreateEntity("Floor");
            reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
            reg.Emplace<BoxColliderComponent>(floor).Size = { 40, 1, 40 };
            Entity post = scene->CreateEntity("Post");
            reg.Get<TransformComponent>(post).Position = { 0, 5, 0 };
            Entity lamp = scene->CreateEntity("Lamp");
            reg.Get<TransformComponent>(lamp).Position = { 0, 2.05f, 0 };
            reg.Emplace<SphereColliderComponent>(lamp).Radius = 0.2f;
            reg.Emplace<RigidbodyComponent>(lamp).Mass = 5.0f;
            auto& cord = reg.Emplace<RopeComponent>(post);
            cord.Length = 3.0f;
            cord.AttachTo = lamp;
            cord.BreakForce = breakForce;
            scene->UpdateTransforms();
            return std::make_tuple(std::move(scene), post, lamp);
        };
        {
            auto [scene, post, lamp] = lampScene(0.0f);
            auto rt = scene->Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < 180; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            auto& rr = rt->GetRegistry();
            float tension = rr.Get<RopeComponent>(post).Tension;
            CHECK(tension > 30.0f && tension < 90.0f); // about its weight, 49 N
            CHECK(rr.Get<RopeComponent>(post).BrokenAt < 0 && rr.Get<TransformComponent>(lamp).Position.y > 1.5f);
            rt->OnRuntimeStop();
        }
        {
            auto [scene, post, lamp] = lampScene(25.0f);
            auto rt = scene->Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < 180; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            auto& rr = rt->GetRegistry();
            CHECK(rr.Get<RopeComponent>(post).BrokenAt >= 0 && rr.Get<TransformComponent>(lamp).Position.y < 0.5f);
            rt->OnRuntimeStop();
        }

        // A rope tied between two objects snaps when they pull it more than Break Stretch longer than it is; the pieces
        // hang from either end, and the tube closes up at the break.
        Scene scene("Line");
        auto& reg = scene.GetRegistry();
        Entity a = scene.CreateEntity("A"), b = scene.CreateEntity("B");
        reg.Get<TransformComponent>(a).Position = { 0, 5, 0 };
        reg.Get<TransformComponent>(b).Position = { 2.9f, 5, 0 };
        auto& line = reg.Emplace<RopeComponent>(a);
        line.Length = 3.0f;
        line.Segments = 12;
        line.AttachTo = b;
        line.BreakStretch = 0.2f;
        line.Damping = 3.0f; // the pieces settle quickly
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        for (int i = 0; i < 30; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(rr.Get<RopeComponent>(a).BrokenAt < 0);
        for (int i = 0; i < 60 && rr.Get<RopeComponent>(a).BrokenAt < 0; ++i)
        {
            rr.Get<TransformComponent>(b).Position.x += 0.05f; // 3 m/s apart
            rt->OnUpdateRuntime(1.0f / 60.0f);
        }
        const auto& snapped = rr.Get<RopeComponent>(a);
        CHECK(snapped.BrokenAt >= 0 && rr.Get<TransformComponent>(b).Position.x < 3.9f);
        for (int i = 0; i < 120; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        const auto& pts = snapped.SimulatedPoints;
        int k = snapped.BrokenAt;
        CHECK(pts.size() == 13 && k >= 3 && k <= 8 && glm::length(pts[0]) < 0.05f && glm::distance(pts[k], pts[k + 1]) > 1.0f &&
              pts[k].y < -0.5f && pts[k + 1].y < -0.5f); // both pieces hang down, broken near the middle
        std::vector<Vertex> tube;
        Rope::BuildTube(snapped, pts, tube);
        CHECK(tube.size() == 13u * (Rope::kSides + 1) && glm::distance(tube[k * (Rope::kSides + 1) + 2].Position, pts[k]) < 1e-5f &&
              glm::distance(tube[0 * (Rope::kSides + 1) + 2].Position, pts[0]) > 0.01f);
        rt->OnRuntimeStop();
    }

    void TestRope()
    {
        // The starting shapes: straight down, or sagging between two ends to exactly Length.
        RopeComponent shape;
        shape.Length = 5.0f;
        shape.Segments = 20;
        auto down = Rope::RestPoints(shape, { 0, 3, 0 }, { 0, -1, 0 }, nullptr);
        CHECK(down.size() == 21 && Near(down.back().y, -2.0f, 1e-4f));
        glm::vec3 end(4, 3, 0);
        auto sag = Rope::RestPoints(shape, { 0, 3, 0 }, { 0, -1, 0 }, &end);
        float length = 0.0f, lowest = 3.0f;
        for (size_t i = 1; i < sag.size(); ++i) { length += glm::distance(sag[i], sag[i - 1]); lowest = std::min(lowest, sag[i].y); }
        CHECK(Near(length, 5.0f, 0.01f) && lowest < 2.0f && Near(sag.back().x, 4.0f, 1e-4f));
        std::vector<Vertex> tube;
        Rope::BuildTube(shape, down, tube);
        CHECK(tube.size() == 21u * (Rope::kSides + 1) && Near(glm::distance(tube[0].Position, down[0]), shape.Radius, 1e-4f));

        // Simulated: a rope hanging from an object settles at its length below it (without stretching) and follows the
        // object; a lamp on a rope between two posts hangs on it instead of falling; a rope lies on the floor.
        Scene scene("Ropes");
        auto& reg = scene.GetRegistry();
        Entity floor = scene.CreateEntity("Floor");
        reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
        reg.Emplace<BoxColliderComponent>(floor).Size = { 40, 1, 40 };
        Entity hook = scene.CreateEntity("Hook");
        reg.Get<TransformComponent>(hook).Position = { 0, 4, 0 };
        auto& hanging = reg.Emplace<RopeComponent>(hook);
        hanging.Length = 2.0f;
        hanging.Segments = 16;
        Entity post = scene.CreateEntity("Post");
        reg.Get<TransformComponent>(post).Position = { 5, 5, 0 };
        Entity lamp = scene.CreateEntity("Lamp");
        reg.Get<TransformComponent>(lamp).Position = { 6, 5, 0 }; // 1 m out: it swings down and hangs 3 m below
        reg.Emplace<SphereColliderComponent>(lamp).Radius = 0.2f;
        reg.Emplace<RigidbodyComponent>(lamp).Mass = 5.0f;
        auto& cord = reg.Emplace<RopeComponent>(post);
        cord.Length = 3.0f;
        cord.AttachTo = lamp;
        Entity coil = scene.CreateEntity("Coil");
        reg.Get<TransformComponent>(coil).Position = { -5, 3.0f, 0 };
        reg.Get<TransformComponent>(coil).Rotation = { 0, 0, 0.6f }; // falls tilted (straight down it would balance on its end)
        auto& loose = reg.Emplace<RopeComponent>(coil);
        loose.PinStart = false;
        loose.Length = 1.5f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        for (int i = 0; i < 240; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        const auto& pts = rr.Get<RopeComponent>(hook).SimulatedPoints;
        float simLength = 0.0f;
        for (size_t i = 1; i < pts.size(); ++i) simLength += glm::distance(pts[i], pts[i - 1]);
        CHECK(pts.size() == 17 && glm::length(pts[0]) < 0.01f && Near(pts.back().y, -2.0f, 0.1f) && Near(simLength, 2.0f, 0.05f));
        // Moving the hook (at 3 m/s) drags the rope along: its points are in the hook's space, so they trail behind.
        for (int i = 0; i < 20; ++i)
        {
            rr.Get<TransformComponent>(hook).Position.x += 0.05f;
            rt->OnUpdateRuntime(1.0f / 60.0f);
        }
        CHECK(glm::length(pts[0]) < 0.06f && pts.back().x < -0.05f);
        glm::vec3 lampPos = rr.Get<TransformComponent>(lamp).Position;
        CHECK(glm::distance(lampPos, glm::vec3(5, 5, 0)) < 3.1f && lampPos.y > 1.5f); // hangs, held by the rope
        const auto& coilPts = rr.Get<RopeComponent>(coil).SimulatedPoints;
        bool onFloor = !coilPts.empty();
        const glm::mat4& coilWorld = rr.Get<TransformComponent>(coil).World;
        for (const glm::vec3& p : coilPts)
        {
            float y = (coilWorld * glm::vec4(p, 1.0f)).y; // the points are in the coil's space
            onFloor &= y > -0.01f && y < 0.2f;
        }
        CHECK(onFloor);
        rt->OnRuntimeStop();
    }

    void TestFracture()
    {
        // Shattering: the pieces fill the mesh exactly (their volumes add up), each is closed by its caps (its own
        // mesh's volume matches), stays inside the source, and the same seed gives the same pieces.
        MeshData cube = MeshData::Cube();
        float cubeVolume = Fracture::Volume(cube);
        CHECK(Near(cubeVolume, 1.0f, 1e-4f));
        auto pieces = Fracture::Shatter(cube, 8, 3, 1);
        CHECK(pieces.size() >= 4 && pieces.size() <= 8);
        float sum = 0.0f;
        bool closed = true, inside = true, capped = true;
        for (auto& p : pieces)
        {
            sum += p.Volume;
            closed &= Near(Fracture::Volume(p.Mesh), p.Volume, 1e-4f) && p.Volume > 0.0f;
            for (auto& v : p.Mesh.Vertices)
            {
                glm::vec3 q = v.Position + p.Center;
                inside &= std::abs(q.x) <= 0.5001f && std::abs(q.y) <= 0.5001f && std::abs(q.z) <= 0.5001f;
            }
            capped &= !p.Mesh.SubMeshes.empty() && p.Mesh.SubMeshes.back().MaterialSlot == 1;
        }
        CHECK(Near(sum, cubeVolume, 1e-3f) && closed && inside && capped);
        auto again = Fracture::Shatter(cube, 8, 3, 1);
        bool same = again.size() == pieces.size();
        for (size_t i = 0; same && i < pieces.size(); ++i) same = Near(again[i].Volume, pieces[i].Volume, 1e-6f) && again[i].Mesh.Vertices.size() == pieces[i].Mesh.Vertices.size();
        CHECK(same && Fracture::Shatter(cube, 8, 4, 1).size() > 0);
        // Focused on a corner: the pieces there are smaller than the rest, and they still fill the cube.
        {
            Fracture::Focus focus{ glm::vec3(0.5f), 0.6f, 0.3f };
            auto shards = Fracture::Shatter(cube, 20, 7, 1, &focus);
            float total = 0.0f, nearSum = 0.0f, farSum = 0.0f;
            int nearCount = 0, farCount = 0;
            for (auto& p : shards)
            {
                total += p.Volume;
                if (glm::distance(p.Center, glm::vec3(0.5f)) < 0.45f) { nearSum += p.Volume; ++nearCount; }
                else { farSum += p.Volume; ++farCount; }
            }
            CHECK(Near(total, 1.0f, 1e-3f) && nearCount >= 4 && farCount >= 2 && nearSum / nearCount < 0.5f * farSum / farCount);
        }
        // A rounder mesh too.
        MeshData sphere = MeshData::Sphere(24, 12);
        float sphereSum = 0.0f;
        for (auto& p : Fracture::Shatter(sphere, 6, 1, 0)) sphereSum += p.Volume;
        CHECK(Near(sphereSum, Fracture::Volume(sphere), Fracture::Volume(sphere) * 0.002f));

        // In a scene: a falling crate breaks on the floor into pieces that share its mass and go after their lifetime;
        // a script breaks a static wall; the pieces' meshes are released when play stops.
        fs::path assets = fs::temp_directory_path() / "dimension_fracture_test" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        Scene scene("Fracture");
        auto& reg = scene.GetRegistry();
        Entity floor = scene.CreateEntity("Floor");
        reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
        reg.Emplace<BoxColliderComponent>(floor).Size = { 40, 1, 40 };
        Entity crate = scene.CreateEntity("Crate");
        reg.Get<TransformComponent>(crate).Position = { 0, 5, 0 };
        reg.Emplace<MeshRendererComponent>(crate).Mesh = AssetManager::kCube;
        reg.Emplace<BoxColliderComponent>(crate);
        reg.Emplace<RigidbodyComponent>(crate).Mass = 12.0f;
        auto& breakable = reg.Emplace<BreakableComponent>(crate);
        breakable.Pieces = 6;
        breakable.BreakSpeed = 4.0f;
        breakable.DebrisLifetime = 1.5f;
        Entity wall = scene.CreateEntity("Wall");
        reg.Get<TransformComponent>(wall).Position = { 6, 1, 0 };
        reg.Get<TransformComponent>(wall).Scale = { 0.3f, 2, 3 };
        reg.Emplace<MeshRendererComponent>(wall).Mesh = AssetManager::kCube;
        reg.Emplace<BoxColliderComponent>(wall);
        reg.Emplace<BreakableComponent>(wall).BreakSpeed = 0.0f; // scripts only
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        auto countPieces = [&] {
            int n = 0;
            float mass = 0.0f;
            for (auto [e, tag] : rr.ViewOf<TagComponent>())
                if (tag.Name == "Crate Piece") { ++n; mass += rr.Get<RigidbodyComponent>(e).Mass; }
            return std::make_pair(n, mass);
        };
        for (int i = 0; i < 90 && rt->IsValid(crate); ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(!rt->IsValid(crate)); // fell about 4.5 m (9 m/s at the floor) and broke
        auto [count, mass] = countPieces();
        CHECK(count >= 3 && count <= 6 && Near(mass, 12.0f, 0.05f));
        AssetHandle pieceMesh = AssetHandle::None();
        for (auto [e, tag] : rr.ViewOf<TagComponent>())
            if (tag.Name == "Crate Piece") pieceMesh = rr.Get<MeshRendererComponent>(e).Mesh;
        CHECK(AssetManager::GetMeshData(pieceMesh) != nullptr);
        // The wall stands until broken; then its pieces fly away from the point, toward -X here.
        CHECK(Physics3DWorld::Break(*rt, floor, { 0, 0, 0 }).empty()); // not Breakable
        std::vector<Entity> wallPieces = Physics3DWorld::Break(*rt, wall, { 6.5f, 1.0f, 0.0f });
        CHECK(wallPieces.size() >= 3);
        for (int i = 0; i < 10; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        float meanX = 0.0f;
        for (Entity p : wallPieces) meanX += rr.Get<TransformComponent>(p).Position.x / wallPieces.size();
        CHECK(!rt->IsValid(wall) && meanX < 6.0f);
        // A focused break cuts its own pieces, whose meshes go when the pieces do.
        Entity vase = rt->CreateEntity("Vase");
        rr.Get<TransformComponent>(vase).Position = { -6, 1, 0 };
        rr.Emplace<MeshRendererComponent>(vase).Mesh = AssetManager::kCube;
        auto& vb = rr.Emplace<BreakableComponent>(vase);
        vb.ImpactFocus = 0.5f;
        vb.DebrisLifetime = 1.0f;
        rt->UpdateTransforms();
        std::vector<Entity> shards = Physics3DWorld::Break(*rt, vase, { -6.5f, 1.5f, 0.5f });
        CHECK(shards.size() >= 3);
        AssetHandle shardMesh = shards.empty() ? AssetHandle::None() : rr.Get<MeshRendererComponent>(shards[0]).Mesh;
        CHECK(shardMesh != pieceMesh && AssetManager::GetMeshData(shardMesh) != nullptr);
        // Debris goes after its lifetime (the crate's 1.5 s, the vase's 1 s; the wall's stay 8 s).
        for (int i = 0; i < 100; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(countPieces().first == 0 && AssetManager::GetMeshData(shardMesh) == nullptr);
        bool wallStays = true;
        for (Entity p : wallPieces) wallStays &= rt->IsValid(p);
        CHECK(wallStays);
        rt->OnRuntimeStop();
        rt.reset();
        CHECK(AssetManager::GetMeshData(pieceMesh) == nullptr);
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestRagdoll()
    {
        // The Tentacle (a chain of bones 0.5 apart) standing on a floor: capsules follow it while animated, and
        // with Simulate on it collapses onto the floor without falling through.
        fs::path assets = fs::temp_directory_path() / "dimension_ragdoll_test" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Tentacle.gltf", assets / "Tentacle.gltf");
        AssetManager::Init(assets);
        AssetHandle model = AssetManager::GetHandle(assets / "Tentacle.gltf");
        auto data = AssetManager::GetMeshData(model);
        CHECK(data && data->SkeletonData && data->SkeletonData->Joints.size() >= 4);
        if (!data || !data->SkeletonData)
        {
            AssetManager::Shutdown();
            return;
        }
        size_t joints = data->SkeletonData->Joints.size();
        Scene scene("Ragdoll");
        auto& reg = scene.GetRegistry();
        Entity floor = scene.CreateEntity("Floor");
        reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
        reg.Emplace<BoxColliderComponent>(floor).Size = { 40, 1, 40 };
        Entity tentacle = scene.CreateEntity("Tentacle");
        reg.Get<TransformComponent>(tentacle).Position = { 0, 0.3f, 0 };
        reg.Emplace<MeshRendererComponent>(tentacle).Mesh = model;
        // (no Animator: skinning would need the GPU mesh; the capsules follow the bind pose)
        auto& rd = reg.Emplace<RagdollComponent>(tentacle);
        rd.Mass = 20.0f;
        rd.MinBoneLength = 0.05f;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        auto& ragdoll = rr.Get<RagdollComponent>(tentacle);
        size_t capsules = ragdoll.Capsules.size();
        CHECK(capsules >= 3 && capsules < joints + 1); // the leaf rides along with its parent
        auto topY = [&] {
            float y = -1e9f;
            for (auto& c : ragdoll.Capsules) y = std::max(y, c.World[3].y);
            return y;
        };
        auto lowY = [&] {
            float y = 1e9f;
            for (auto& c : ragdoll.Capsules) y = std::min(y, c.World[3].y - c.Radius);
            return y;
        };
        float standing = topY();
        for (int i = 0; i < 60; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(Near(topY(), standing, 0.01f) && !ragdoll.Driving); // kinematic: holds the pose
        // Moving the object carries the capsules.
        rr.Get<TransformComponent>(tentacle).Position.x = 2.0f;
        for (int i = 0; i < 10; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(Near(ragdoll.Capsules[0].World[3].x, 2.0f, 0.05f));
        // Limp: it falls over, rests on the floor, and drives the pose.
        ragdoll.Simulate = true;
        Physics3DWorld::AddRagdollImpulse(*rt, tentacle, glm::vec3(30.0f, 0.0f, 0.0f), glm::vec3(2.0f, standing, 0.0f));
        for (int i = 0; i < 4 * 60; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(ragdoll.Driving && ragdoll.Pose.size() == joints);
        CHECK(topY() < standing - 1.0f && lowY() > -0.05f);
        // The object followed its root capsule, so the pose stays near it.
        glm::vec3 objectPos = rr.Get<TransformComponent>(tentacle).GetWorldPosition();
        CHECK(glm::distance(objectPos, glm::vec3(ragdoll.Capsules[0].World[3])) < 1.0f);
        // Raycasts hit the capsules (as the ragdoll's object).
        RaycastHit hit = Physics3DWorld::Raycast(*rt, glm::vec3(ragdoll.Capsules[1].World[3]) + glm::vec3(0, 3, 0), glm::vec3(0, -1, 0), 10.0f);
        CHECK(hit && hit.Hit == tentacle);
        // Back to animated: it no longer drives the pose, and gets up. The object moves onto the floor under the limp
        // root joint, turning only about Y, and the blend starts from exactly the limp pose.
        auto& tt = rr.Get<TransformComponent>(tentacle);
        glm::mat4 limpWorld = tt.World;
        std::vector<glm::mat4> limp(joints);
        for (size_t j = 0; j < joints; ++j) limp[j] = limpWorld * ragdoll.Pose[j];
        int rootJoint = data->SkeletonData->Order[0];
        ragdoll.Simulate = false;
        rt->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(!ragdoll.Driving);
        CHECK(ragdoll.BlendFrom.size() == joints && Near(ragdoll.BlendDuration, 0.6f) && ragdoll.GetUpProgress() < 0.01f);
        CHECK(Near(tt.World[3].y, 0.0f, 0.05f) && Near(tt.Rotation.x, 0.0f, 1e-3f) && Near(tt.Rotation.z, 0.0f, 1e-3f) && Near(tt.World[1].y, 1.0f, 1e-4f));
        bool samePose = true;
        for (size_t j = 0; j < joints; ++j) samePose &= glm::distance(glm::vec3((tt.World * ragdoll.BlendFrom[j])[3]), glm::vec3(limp[j][3])) < 1e-3f;
        CHECK(samePose);
        glm::vec3 bindRoot = glm::vec3(tt.World * glm::inverse(data->SkeletonData->Joints[rootJoint].InverseBind)[3]);
        CHECK(Near(bindRoot.x, limp[rootJoint][3].x, 0.01f) && Near(bindRoot.z, limp[rootJoint][3].z, 0.01f));
        rt->OnRuntimeStop();

        // Partial: only the bones from the middle of the chain up go limp; the base keeps its (bind) pose and the
        // object stays put, while the knocked top falls over. Turning it off neither moves nor turns the object.
        {
            const Skeleton& skeleton = *data->SkeletonData;
            int mid = skeleton.Order[skeleton.Order.size() / 2];
            Scene ps("Partial");
            auto& preg = ps.GetRegistry();
            Entity pfloor = ps.CreateEntity("Floor");
            preg.Get<TransformComponent>(pfloor).Position = { 0, -0.5f, 0 };
            preg.Emplace<BoxColliderComponent>(pfloor).Size = { 40, 1, 40 };
            Entity arm = ps.CreateEntity("Arm");
            preg.Get<TransformComponent>(arm).Position = { 0, 0.3f, 0 };
            preg.Emplace<MeshRendererComponent>(arm).Mesh = model;
            auto& prd = preg.Emplace<RagdollComponent>(arm);
            prd.Mass = 20.0f;
            prd.MinBoneLength = 0.05f;
            prd.PartialRoot = "SomePrefix:" + skeleton.Joints[mid].Name; // prefixes are ignored, as in retargeting
            ps.UpdateTransforms();
            auto prt = ps.Clone();
            prt->OnRuntimeStart();
            auto& pr = prt->GetRegistry().Get<RagdollComponent>(arm);
            std::vector<glm::mat4> standingCaps;
            for (auto& c : pr.Capsules) standingCaps.push_back(c.World);
            pr.Simulate = true;
            float top = 0.0f;
            for (auto& c : pr.Capsules) top = std::max(top, c.World[3].y);
            Physics3DWorld::AddRagdollImpulse(*prt, arm, glm::vec3(20.0f, 0.0f, 0.0f), glm::vec3(0.0f, top, 0.0f));
            for (int i = 0; i < 120; ++i) prt->OnUpdateRuntime(1.0f / 60.0f);
            size_t limpCount = std::count(pr.Limp.begin(), pr.Limp.end(), (uint8_t)1);
            CHECK(pr.Driving && pr.Limp.size() == joints && limpCount > 0 && limpCount < joints && pr.Limp[mid] == 1);
            bool baseStill = true, topMoved = false;
            for (size_t b = 0; b < pr.Capsules.size(); ++b)
            {
                float moved = glm::distance(glm::vec3(pr.Capsules[b].World[3]), glm::vec3(standingCaps[b][3]));
                if (standingCaps[b][3].y < standingCaps[0][3].y + 0.01f) baseStill &= moved < 0.01f; // the root's capsule
                topMoved |= moved > 0.3f;
            }
            float newTop = 0.0f;
            for (auto& c : pr.Capsules) newTop = std::max(newTop, c.World[3].y);
            auto& at = prt->GetRegistry().Get<TransformComponent>(arm);
            CHECK(baseStill && topMoved && newTop < top - 0.2f && Near(at.Position.y, 0.3f, 1e-4f) && Near(at.Position.x, 0.0f, 1e-4f));
            // Unlimped joints keep the (bind) pose in the skin.
            bool baseBind = true;
            for (size_t j = 0; j < joints; ++j)
                if (!pr.Limp[j]) baseBind &= glm::distance(glm::vec3(pr.Pose[j][3]), glm::vec3(glm::inverse(skeleton.Joints[j].InverseBind)[3])) < 1e-3f;
            CHECK(baseBind);
            pr.Simulate = false;
            prt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(!pr.Driving && pr.Limp.empty() && pr.BlendFrom.size() == joints && Near(at.Position.y, 0.3f, 1e-4f) && Near(at.Rotation.y, 0.0f, 1e-5f));
            prt->OnRuntimeStop();
        }
        rt.reset();
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    // An uncompressed 8-bit grayscale TGA (type 3), rows bottom to top.
    void WriteGrayTga(const fs::path& path, int w, int h, const std::function<uint8_t(int x, int y)>& pixel)
    {
        std::ofstream out(path, std::ios::binary);
        uint8_t header[18] = {};
        header[2] = 3;
        header[12] = (uint8_t)(w & 255); header[13] = (uint8_t)(w >> 8);
        header[14] = (uint8_t)(h & 255); header[15] = (uint8_t)(h >> 8);
        header[16] = 8;
        out.write((const char*)header, 18);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                uint8_t v = pixel(x, y);
                out.write((const char*)&v, 1);
            }
    }

    void TestTerrainStamp()
    {
        // A stamp lays its image over the brush's square: a cone rises at the center to base + Height, nothing changes
        // outside the square, stamping again does not raise it further, carving digs the same shape down, and a turned
        // stamp of a one-sided ramp lifts the other side.
        fs::path assets = fs::temp_directory_path() / "dimension_terrain_stamp" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        WriteGrayTga(assets / "Cone.tga", 64, 64, [](int x, int y) {
            float d = glm::length(glm::vec2(x - 31.5f, y - 31.5f)) / 32.0f;
            return (uint8_t)std::clamp((int)std::lround((1.0f - d) * 255.0f), 0, 255);
        });
        WriteGrayTga(assets / "Ramp.tga", 64, 64, [](int x, int) { return (uint8_t)(x * 255 / 63); });
        AssetManager::Init(assets);
        TerrainComponent t;
        t.Size = { 64.0f, 64.0f };
        t.Height = 20.0f;
        t.Resolution = 64;
        t.FlatCenter = 1.0f; // all flat at Flat Height
        t.FlatHeight = 0.25f;
        float base = 5.0f;
        Terrain::StampSettings stamp;
        stamp.Image = AssetManager::GetHandle(assets / "Cone.tga");
        stamp.Height = 8.0f;
        CHECK(stamp.Image.IsValid() && Near(Terrain::StampValue(stamp, { 0.0f, 0.0f }, 10.0f), 1.0f, 0.03f) &&
              Terrain::StampValue(stamp, { 11.0f, 0.0f }, 10.0f) < 0.0f);
        Terrain::BeginEdit(t);
        CHECK(!Terrain::ApplyBrush(t, Terrain::BrushMode::Stamp, { 0, 0 }, 10.0f, 1.0f, 0.1f, base)); // no stamp settings
        for (int i = 0; i < 30; ++i) CHECK(Terrain::ApplyBrush(t, Terrain::BrushMode::Stamp, { 0.0f, 0.0f }, 10.0f, 1.0f, 0.1f, base, &stamp));
        auto at = [&](float x, float z) { return Terrain::HeightAt(t, t.EditHeights, x, z); };
        CHECK(Near(at(0, 0), base + 8.0f, 0.8f) && Near(at(5, 0), base + 4.0f, 0.5f) && Near(at(20, 0), base, 1e-3f));
        float peak = at(0, 0);
        for (int i = 0; i < 10; ++i) Terrain::ApplyBrush(t, Terrain::BrushMode::Stamp, { 0.0f, 0.0f }, 10.0f, 1.0f, 0.1f, base, &stamp);
        CHECK(Near(at(0, 0), peak, 1e-3f)); // one way: already there
        stamp.Carve = true;
        stamp.Height = 4.0f; // (the ground stays within 0 and Height)
        for (int i = 0; i < 30; ++i) Terrain::ApplyBrush(t, Terrain::BrushMode::Stamp, { -20.0f, -20.0f }, 8.0f, 1.0f, 0.1f, base, &stamp);
        CHECK(Near(at(-20, -20), base - 4.0f, 0.6f) && Near(at(0, 0), peak, 1e-3f));
        // The ramp turned 180 degrees: high on -X instead of +X.
        Terrain::StampSettings ramp;
        ramp.Image = AssetManager::GetHandle(assets / "Ramp.tga");
        ramp.Height = 6.0f;
        ramp.Rotation = 180.0f;
        for (int i = 0; i < 30; ++i) Terrain::ApplyBrush(t, Terrain::BrushMode::Stamp, { 20.0f, 20.0f }, 8.0f, 1.0f, 0.1f, base, &ramp);
        CHECK(at(14.0f, 20.0f) > base + 4.5f && at(26.0f, 20.0f) < base + 1.5f);
        Terrain::CommitEdit(t);
        CHECK(Near(Terrain::HeightAt(t, Terrain::BuildHeights(t), 0, 0), peak, 0.01f));
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestPathPlacer()
    {
        // Copies of a source object every Spacing meters along a straight path, facing along it, offset to its right;
        // placing again replaces them; snapped onto a terrain; and from a prefab.
        Scene scene("Fence");
        auto& reg = scene.GetRegistry();
        Entity post = scene.CreateEntity("Post");
        reg.Emplace<MeshRendererComponent>(post);
        reg.Get<TransformComponent>(post).Scale = { 0.2f, 1.0f, 0.2f };
        Entity fence = scene.CreateEntity("Fence");
        reg.Get<TransformComponent>(fence).Position = { 0.0f, 0.0f, 5.0f };
        auto& pp = reg.Emplace<PathPlacerComponent>(fence);
        pp.Points = { { 0, 0, 0 }, { 10, 0, 0 } };
        pp.Source = post;
        pp.Spacing = 2.0f;
        pp.SideOffset = 1.0f;
        pp.SnapToGround = false;
        scene.UpdateTransforms();
        CHECK(PathPlacer::Place(scene, fence) == 6);
        const auto& kids = scene.GetChildren(fence);
        CHECK(kids.size() == 6);
        bool ok = kids.size() == 6;
        for (size_t i = 0; i < kids.size() && ok; ++i)
        {
            const auto& t = reg.Get<TransformComponent>(kids[i]);
            glm::vec3 w = t.GetWorldPosition();
            glm::vec3 forward = -glm::vec3(t.World[2]) / glm::length(glm::vec3(t.World[2]));
            // Heading +X, its right is +Z.
            ok = std::abs(w.x - 2.0f * i) < 0.05f && std::abs(w.z - 6.0f) < 0.05f && forward.x > 0.99f &&
                 std::abs(glm::length(glm::vec3(t.World[0])) - 0.2f) < 1e-3f && reg.Has<MeshRendererComponent>(kids[i]);
        }
        CHECK(ok);
        CHECK(scene.IsValid(post) && scene.GetParent(post).IsNull()); // the source stays where it was
        // Again, closer together: replaced, not added.
        reg.Get<PathPlacerComponent>(fence).Spacing = 1.0f;
        CHECK(PathPlacer::Place(scene, fence) == 11 && scene.GetChildren(fence).size() == 11);
        PathPlacer::Clear(scene, fence);
        CHECK(scene.GetChildren(fence).empty());

        // Onto a terrain: each copy stands on the ground under it.
        Entity land = scene.CreateEntity("Land");
        auto& tc = reg.Emplace<TerrainComponent>(land);
        tc.Size = { 40.0f, 40.0f };
        tc.Resolution = 64;
        reg.Emplace<MeshRendererComponent>(land);
        reg.Get<TransformComponent>(land).Position = { 0.0f, -3.0f, 0.0f };
        reg.Get<PathPlacerComponent>(fence).SnapToGround = true;
        reg.Get<PathPlacerComponent>(fence).Spacing = 2.5f;
        scene.UpdateTransforms();
        CHECK(PathPlacer::Place(scene, fence) == 5);
        std::vector<float> heights = Terrain::BuildHeights(reg.Get<TerrainComponent>(land));
        ok = true;
        for (Entity k : scene.GetChildren(fence))
        {
            glm::vec3 w = reg.Get<TransformComponent>(k).GetWorldPosition();
            ok &= std::abs(w.y - (Terrain::HeightAt(reg.Get<TerrainComponent>(land), heights, w.x, w.z) - 3.0f)) < 0.01f;
        }
        CHECK(ok);

        // A prefab, scattered: random turns and sizes within the limits, the same for the same seed.
        fs::path assets = fs::temp_directory_path() / "dimension_path_placer" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        AssetHandle prefab = Prefabs::CreateFromEntity(scene, post, assets / "Post.prefab");
        CHECK(prefab.IsValid());
        auto& p2 = reg.Get<PathPlacerComponent>(fence);
        p2.Source = Entity::Null();
        p2.Prefab = prefab;
        p2.SnapToGround = false;
        p2.AlignToPath = false;
        p2.RandomRotation = 30.0f;
        p2.RandomScale = 0.5f;
        auto snapshot = [&] {
            std::vector<glm::vec3> out;
            for (Entity k : scene.GetChildren(fence))
            {
                const auto& t = reg.Get<TransformComponent>(k);
                out.push_back({ t.Rotation.y, t.Scale.x, t.Scale.y });
            }
            return out;
        };
        CHECK(PathPlacer::Place(scene, fence) == 5);
        auto first = snapshot();
        ok = first.size() == 5;
        bool varied = false;
        for (const glm::vec3& s : first)
        {
            ok &= std::abs(s.x) <= glm::radians(30.0f) + 1e-4f && s.y >= 0.1f - 1e-4f && s.y <= 0.3f + 1e-4f && s.z >= 0.5f - 1e-4f;
            varied |= std::abs(s.x - first[0].x) > 1e-3f;
        }
        CHECK(ok && varied && reg.Has<PrefabLinkComponent>(scene.GetChildren(fence)[0]));
        PathPlacer::Place(scene, fence);
        auto second = snapshot();
        ok = second.size() == first.size();
        for (size_t i = 0; ok && i < first.size(); ++i) ok = glm::distance(first[i], second[i]) < 1e-5f;
        CHECK(ok);
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestTerrainPath()
    {
        // The curve passes through its points; a road levels the ground to its height across Width and blends out
        // over Falloff; a river carves a channel Depth deep in the middle; both paint their layer; the far ground stays.
        std::vector<glm::vec3> pts = { { -20, 0, 0 }, { 0, 0, 5 }, { 20, 0, 0 } };
        auto curve = Terrain::PathCurve(pts, 8);
        CHECK(curve.size() == 17 && glm::distance(curve.front(), pts[0]) < 1e-5f && glm::distance(curve[8], pts[1]) < 1e-5f && glm::distance(curve.back(), pts[2]) < 1e-5f);

        TerrainComponent t;
        t.Size = { 80.0f, 80.0f };
        t.Height = 20.0f;
        t.Resolution = 128;
        t.FlatCenter = 2.0f; // flat everywhere at 5 m
        t.FlatHeight = 0.25f;
        Terrain::BeginEdit(t);
        CHECK(Near(Terrain::HeightAt(t, t.EditHeights, 0, 0), 5.0f, 1e-3f));
        TerrainPathComponent road;
        road.Points = { { -30, 7, -10 }, { 0, 7, -10 }, { 30, 7, -10 } };
        road.Width = 4.0f;
        road.Falloff = 4.0f;
        road.PaintLayer = 3;
        glm::mat4 id(1.0f);
        CHECK(Terrain::ApplyPath(t, id, road, id) > 100);
        TerrainPathComponent river = road;
        river.Mode = TerrainPathMode::River;
        river.Points = { { -30, 5, 15 }, { 0, 5, 15 }, { 30, 5, 15 } };
        river.Depth = 2.0f;
        river.PaintLayer = 4;
        // The path object moved: its points are in its space.
        glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
        CHECK(Terrain::ApplyPath(t, id, river, moved) > 100);
        auto at = [&](float x, float z) { return Terrain::HeightAt(t, t.EditHeights, x, z); };
        CHECK(Near(at(0, -10), 7.0f, 0.05f) && Near(at(5, -11), 7.0f, 0.05f));     // the road
        float shoulder = at(0, -6.0f);
        CHECK(shoulder > 5.05f && shoulder < 6.95f);                                // blending out
        CHECK(Near(at(0, -2.0f), 5.0f, 1e-3f) && Near(at(0, 5.0f), 5.0f, 1e-3f));   // beyond reach
        CHECK(Near(at(0, 15), 3.0f, 0.1f) && at(0, 16.5f) > 3.0f && at(0, 16.5f) < 5.0f); // the river bed and its slope
        int n = Terrain::SampleCount(t);
        auto sampleAt = [&](float x, float z) { return (size_t)std::lround((z / 80.0f + 0.5f) * (n - 1)) * n + (size_t)std::lround((x / 80.0f + 0.5f) * (n - 1)); };
        CHECK(Terrain::LayerWeight(t, sampleAt(0, -10), 3) > 0.94f); // dirt on the road
        CHECK(Terrain::LayerWeight(t, sampleAt(0, 15), 4) > 0.94f);  // sand in the river
        Terrain::CommitEdit(t);
        CHECK(Near(Terrain::HeightAt(t, Terrain::BuildHeights(t), 0, -10), 7.0f, 0.05f));
    }

    void TestTerrainErosion()
    {
        // Rain on hills: material only moves (the volume stays), only near the brush, downhill overall (the heights'
        // potential energy, the sum of squares, drops), and the same seed erodes the same way.
        TerrainComponent t;
        t.Size = { 100.0f, 100.0f };
        t.Height = 30.0f;
        t.Resolution = 128;
        t.NoiseScale = 0.03f;
        Terrain::BeginEdit(t);
        std::vector<float> before = t.EditHeights;
        TerrainComponent copy = t;
        Terrain::Erode(t, { 0.0f, 0.0f }, 20.0f, 3000, 1);
        Terrain::Erode(copy, { 0.0f, 0.0f }, 20.0f, 3000, 1);
        CHECK(t.EditHeights == copy.EditHeights);
        double sumBefore = 0, sumAfter = 0, energyBefore = 0, energyAfter = 0;
        float maxChange = 0.0f;
        bool local = true;
        int n = Terrain::SampleCount(t);
        float cell = 100.0f / (n - 1);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c)
            {
                size_t i = (size_t)r * n + c;
                float a = before[i], b = t.EditHeights[i];
                sumBefore += a;
                sumAfter += b;
                energyBefore += (double)a * a;
                energyAfter += (double)b * b;
                float change = std::abs(b - a);
                maxChange = std::max(maxChange, change);
                glm::vec2 p(-50.0f + c * cell, -50.0f + r * cell);
                if (change > 1e-5f) local &= glm::length(p) < 20.0f * 1.5f + 4.0f * cell;
            }
        CHECK(std::abs(sumAfter - sumBefore) < sumBefore * 1e-4 && maxChange > 0.05f && local && energyAfter < energyBefore);
        // The brush rains in proportion to strength and time (and does nothing off the terrain).
        TerrainComponent brushed = copy;
        CHECK(Terrain::ApplyBrush(brushed, Terrain::BrushMode::Erode, { 10.0f, 10.0f }, 8.0f, 1.0f, 0.1f, 0.0f) && brushed.EditHeights != copy.EditHeights);
        CHECK(!Terrain::ApplyBrush(brushed, Terrain::BrushMode::Erode, { 500.0f, 0.0f }, 8.0f, 1.0f, 0.1f, 0.0f));
    }

    void TestTerrainDetails()
    {
        // Painting trees and grass tufts: they grow only where painted, about Density per 100 m2, the same every
        // build; erasing removes them; trees collide as capsules.
        TerrainComponent t;
        t.Size = { 60.0f, 60.0f };
        t.Height = 0.0f; // flat: no slope limits
        t.Resolution = 64;
        t.TreeDensity = 4.0f;
        t.GrassDensity = 100.0f;
        CHECK(Terrain::DetailMap(t).empty() && Terrain::BuildDetails(t, Terrain::BuildHeights(t)).Trees.empty());
        Terrain::BeginEdit(t);
        // Full-strength strokes: trees in a 10 m circle at (-15, 0), grass in one at (15, 0).
        for (int i = 0; i < 40; ++i)
        {
            Terrain::ApplyBrush(t, Terrain::BrushMode::PlaceTrees, { -15.0f, 0.0f }, 10.0f, 1.0f, 0.1f, 0.0f);
            Terrain::ApplyBrush(t, Terrain::BrushMode::PlaceGrass, { 15.0f, 0.0f }, 10.0f, 1.0f, 0.1f, 0.0f);
        }
        Terrain::CommitEdit(t);
        CHECK(!t.PaintedDetails.empty() && t.EditDetails.empty() && Terrain::DetailMap(t).size() == (size_t)64 * 64 * 2);
        std::vector<float> heights = Terrain::BuildHeights(t);
        auto details = Terrain::BuildDetails(t, heights);
        bool treesInside = !details.Trees.empty(), grassInside = !details.Grass.empty();
        for (auto& m : details.Trees) treesInside &= glm::distance(glm::vec2(m[3].x, m[3].z), glm::vec2(-15, 0)) < 10.5f;
        for (auto& m : details.Grass) grassInside &= glm::distance(glm::vec2(m[3].x, m[3].z), glm::vec2(15, 0)) < 10.5f;
        CHECK(treesInside && grassInside);
        // About density x area: the soft brush edge thins the outer ring, so expect 40-110% of the full-circle count.
        float area = 3.14159f * 100.0f;
        CHECK(details.Trees.size() > area / 100.0f * 4.0f * 0.4f && details.Trees.size() < area / 100.0f * 4.0f * 1.1f);
        CHECK(details.Grass.size() > area / 100.0f * 100.0f * 0.4f && details.Grass.size() < area / 100.0f * 100.0f * 1.1f);
        float lo = 1e9f, hi = 0.0f;
        for (auto& m : details.Grass) { float sc = glm::length(glm::vec3(m[0])); lo = std::min(lo, sc); hi = std::max(hi, sc); }
        CHECK(lo >= t.GrassScale.x - 1e-3f && hi <= t.GrassScale.y + 1e-3f && hi - lo > 0.2f);
        auto again = Terrain::BuildDetails(t, heights);
        CHECK(again.Grass.size() == details.Grass.size() && again.Grass[0] == details.Grass[0]); // deterministic
        CHECK(Terrain::GetDetails(t).Trees.size() == details.Trees.size());
        // Too steep for grass: none on a 45 degree ramp with Max Slope 30.
        {
            TerrainComponent ramp = t;
            ramp.SculptedHeights.clear();
            ramp.Height = 60.0f;
            Terrain::BeginEdit(ramp);
            int n = Terrain::SampleCount(ramp);
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c) ramp.EditHeights[(size_t)r * n + c] = c * (60.0f / (n - 1));
            ramp.GrassMaxSlope = 30.0f;
            CHECK(Terrain::BuildDetails(ramp, ramp.EditHeights).Grass.empty());
            ramp.GrassMaxSlope = 60.0f;
            CHECK(!Terrain::BuildDetails(ramp, ramp.EditHeights).Grass.empty());
        }
        // Trees collide as capsules (with a tree mesh and a collider radius).
        fs::path assets = fs::temp_directory_path() / "dimension_terrain_details" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        {
            Scene scene("Forest");
            Entity e = scene.CreateEntity("Terrain");
            auto& tc = scene.GetRegistry().Emplace<TerrainComponent>(e) = t;
            tc.TreeMesh = AssetHandle(5); // the built-in cylinder, 1 m tall
            tc.TreeColliderRadius = 0.5f;
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            glm::vec3 tree = glm::vec3(details.Trees[0][3]);
            RaycastHit hit = Physics3DWorld::Raycast(*rt, tree + glm::vec3(-5.0f, 0.3f, 0.0f), glm::vec3(1, 0, 0), 10.0f);
            CHECK(hit && hit.Hit == e && hit.Distance < 5.0f);
            rt->OnRuntimeStop();
        }
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
        // Erasing removes both kinds.
        Terrain::BeginEdit(t);
        for (int i = 0; i < 40; ++i) Terrain::ApplyBrush(t, Terrain::BrushMode::EraseDetails, { 0.0f, 0.0f }, 40.0f, 1.0f, 0.1f, 0.0f);
        Terrain::CommitEdit(t);
        auto erased = Terrain::BuildDetails(t, heights);
        CHECK(erased.Trees.empty() && erased.Grass.empty() && t.PaintedDetails.empty());
    }

    void TestObjMaterials()
    {
        // usemtl groups become sub-meshes with the .mtl's materials (color, opacity, shininess, texture).
        fs::path dir = fs::temp_directory_path() / "dimension_obj_materials";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        {
            std::ofstream mtl(dir / "Two.mtl");
            mtl << "newmtl Bark\nKd 0.4 0.2 0.1\nNs 10\nnewmtl Glass\nKd 0.2 0.5 1.0\nd 0.25\nmap_Kd -bm 1 tex/Glass.png\n";
            std::ofstream obj(dir / "Two.obj");
            obj << "mtllib Two.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                   "usemtl Bark\nf 1 2 3\nusemtl Glass\nf 1 3 4\nusemtl Bark\nf 3 2 1\nusemtl Missing\nf 4 3 1\n";
        }
        MeshData data;
        CHECK(MeshData::LoadFromFile(dir / "Two.obj", data));
        CHECK(data.Materials.size() == 2 && data.Materials[0].Name == "Bark" && data.Materials[1].Name == "Glass");
        CHECK(data.SubMeshes.size() == 3); // Bark (two triangles, grouped), Glass, and the unknown material
        int bark = -1, glass = -1, none = -1;
        for (int i = 0; i < (int)data.SubMeshes.size(); ++i)
        {
            if (data.SubMeshes[i].MaterialSlot == 0) bark = i;
            if (data.SubMeshes[i].MaterialSlot == 1) glass = i;
            if (data.SubMeshes[i].MaterialSlot == -1) none = i;
        }
        CHECK(bark >= 0 && glass >= 0 && none >= 0 && data.SubMeshes[bark].IndexCount == 6 && data.SubMeshes[glass].IndexCount == 3);
        const Material& b = data.Materials[0].Values;
        CHECK(Near(b.BaseColor.r, 0.4f) && Near(b.BaseColor.a, 1.0f) && b.BlendMode == MaterialBlendMode::Opaque && b.Roughness > 0.35f && b.Roughness < 0.45f);
        const Material& g = data.Materials[1].Values;
        CHECK(Near(g.BaseColor.a, 0.25f) && g.BlendMode == MaterialBlendMode::Transparent && data.Materials[1].BaseColorImage == 0);
        CHECK(data.Images.size() == 1 && data.Images[0].File == (dir / "tex" / "Glass.png").lexically_normal());
        // Without a library the model is one plain part.
        MeshData torus;
        CHECK(MeshData::LoadFromFile(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Torus.obj", torus) && torus.Materials.empty() &&
              torus.SubMeshes.size() == 1 && torus.SubMeshes[0].MaterialSlot == -1);
        fs::remove_all(dir, ec);
    }

    void TestBuoyancy()
    {
        // A pool (surface at y = 4) over a floor; balls dropped in float, sink or drift with the current.
        auto pool = [](float buoyancy, glm::vec3 flow, float seconds) {
            Scene scene("Pool");
            auto& reg = scene.GetRegistry();
            Entity floor = scene.CreateEntity("Floor");
            reg.Get<TransformComponent>(floor).Position = { 0, -0.5f, 0 };
            reg.Emplace<BoxColliderComponent>(floor).Size = { 100, 1, 100 };
            Entity water = scene.CreateEntity("Water");
            reg.Get<TransformComponent>(water).Position = { 0, 2, 0 };
            auto& w = reg.Emplace<WaterVolumeComponent>(water);
            w.Size = { 60, 4, 60 };
            w.Buoyancy = buoyancy;
            w.Flow = flow;
            Entity ball = scene.CreateEntity("Ball");
            reg.Get<TransformComponent>(ball).Position = { 0, 7, 0 };
            reg.Emplace<RigidbodyComponent>(ball).Mass = 5.0f;
            reg.Emplace<SphereColliderComponent>(ball).Radius = 0.5f;
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < (int)(seconds * 60); ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            glm::vec3 p = rt->GetRegistry().Get<TransformComponent>(ball).GetWorldPosition();
            glm::vec3 v = Physics3DWorld::GetLinearVelocity(*rt, ball);
            rt->OnRuntimeStop();
            return std::pair{ p, v };
        };
        auto [floatPos, floatVel] = pool(1.6f, glm::vec3(0.0f), 8.0f);
        CHECK(floatPos.y > 3.6f && floatPos.y < 4.6f && glm::length(floatVel) < 1.0f); // bobbing gently at the surface
        auto [sinkPos, sinkVel] = pool(0.4f, glm::vec3(0.0f), 8.0f);
        CHECK(sinkPos.y < 0.7f); // on the floor
        auto [driftPos, driftVel] = pool(1.6f, glm::vec3(1.5f, 0.0f, 0.0f), 8.0f);
        CHECK(driftPos.x > 3.0f && driftVel.x > 0.5f); // carried along by the current
    }

    void TestTerrain()
    {
        TerrainComponent t;
        t.Size = { 80.0f, 60.0f };
        t.Height = 20.0f;
        t.Resolution = 65; // rounds up to an even count for Jolt
        CHECK(Terrain::SampleCount(t) == 66);
        std::vector<float> heights = Terrain::BuildHeights(t);
        CHECK(heights.size() == 66u * 66u && heights == Terrain::BuildHeights(t)); // deterministic
        float lo = *std::min_element(heights.begin(), heights.end()), hi = *std::max_element(heights.begin(), heights.end());
        CHECK(lo >= 0.0f && hi <= 20.0f && hi - lo > 2.0f); // hilly, within Height
        // Exact at the samples; corners of the size map to the corner samples.
        CHECK(Near(Terrain::HeightAt(t, heights, -40.0f, -30.0f), heights[0]) && Near(Terrain::HeightAt(t, heights, 40.0f, 30.0f), heights.back()));
        MeshData mesh = Terrain::BuildMesh(t, heights);
        CHECK(mesh.Vertices.size() == heights.size() && mesh.Indices.size() == 65u * 65u * 6u);
        bool upward = true;
        for (auto& v : mesh.Vertices) upward &= v.Normal.y > 0.0f;
        CHECK(upward);
        std::vector<uint32_t> splat = Terrain::BuildSplat(t, mesh);
        CHECK(splat.size() == mesh.Vertices.size() * 2);
        bool weighted = true, paintOnlyEmpty = true;
        for (size_t i = 0; i < mesh.Vertices.size(); ++i)
        {
            uint32_t s = splat[i];
            weighted &= std::abs((int)((s & 255) + (s >> 8 & 255) + (s >> 16 & 255)) - 255) <= 3;
            paintOnlyEmpty &= (s >> 24) == 0 && splat[mesh.Vertices.size() + i] == 0; // dirt, sand and moss are painted only
        }
        CHECK(weighted && paintOnlyEmpty);
        // A flat center stays at Flat Height; a different seed gives different hills.
        TerrainComponent flat = t;
        flat.FlatCenter = 0.4f;
        flat.FlatHeight = 0.25f;
        std::vector<float> fh = Terrain::BuildHeights(flat);
        CHECK(Near(Terrain::HeightAt(flat, fh, 0.0f, 0.0f), 5.0f, 1e-3f));
        TerrainComponent other = t;
        other.Seed = 2;
        CHECK(Terrain::BuildHeights(other) != heights && Terrain::SettingsKey(other) != Terrain::SettingsKey(t));

        // Sculpting: raising at the center lifts it (and leaves far away alone); the edit survives as serialized data
        // (16-bit), a copy built from that data alone matches, and Clear goes back to the noise.
        {
            TerrainComponent s = t;
            float center0 = Terrain::HeightAt(s, heights, 0.0f, 0.0f), far0 = Terrain::HeightAt(s, heights, 35.0f, 25.0f);
            for (int i = 0; i < 30; ++i) CHECK(Terrain::ApplyBrush(s, Terrain::BrushMode::Raise, { 0.0f, 0.0f }, 6.0f, 1.0f, 1.0f / 30.0f, 0.0f));
            std::vector<float> raised = Terrain::BuildHeights(s);
            CHECK(Terrain::HeightAt(s, raised, 0.0f, 0.0f) > center0 + 5.0f && Near(Terrain::HeightAt(s, raised, 35.0f, 25.0f), far0));
            CHECK(Terrain::SettingsKey(s) != Terrain::SettingsKey(t));
            Terrain::ApplyBrush(s, Terrain::BrushMode::Paint, { 20.0f, 0.0f }, 4.0f, 1.0f, 1.0f, 0.0f, nullptr, 1);
            Terrain::CommitEdit(s);
            CHECK(!s.SculptedHeights.empty() && !s.PaintedLayers.empty() && s.EditHeights.empty());
            TerrainComponent copy = t;
            copy.SculptedHeights = s.SculptedHeights;
            copy.PaintedLayers = s.PaintedLayers;
            std::vector<float> reloaded = Terrain::BuildHeights(copy);
            bool same = reloaded.size() == raised.size();
            for (size_t i = 0; same && i < raised.size(); ++i) same = std::abs(reloaded[i] - raised[i]) < 20.0f / 65535.0f * 2.0f;
            CHECK(same);
            MeshData m = Terrain::BuildMesh(copy, reloaded);
            std::vector<uint32_t> layers = Terrain::BuildSplat(copy, m);
            int n = Terrain::SampleCount(copy);
            uint32_t painted = layers[(size_t)(n / 2) * n + (int)((20.0f / 80.0f + 0.5f) * (n - 1))];
            CHECK((painted >> 8 & 255) > 200); // rock there
            // Rays land on the sculpted surface.
            glm::vec3 hit;
            CHECK(Terrain::Raycast(copy, glm::mat4(1.0f), { 0.0f, 100.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, hit) &&
                  Near(hit.y, Terrain::HeightAt(copy, reloaded, 0.0f, 0.0f), 0.02f));
            Terrain::ClearEdits(copy);
            CHECK(Terrain::BuildHeights(copy) == heights);
        }

        // Painting a later layer: sand (the fifth) fills the second splat map there; an edit of the first three alone
        // still saves the older 3 bytes per sample, and with sand 5.
        {
            TerrainComponent s = t;
            Terrain::ApplyBrush(s, Terrain::BrushMode::Paint, { 0.0f, 0.0f }, 4.0f, 1.0f, 1.0f, 0.0f, nullptr, 1);
            Terrain::CommitEdit(s);
            int n = Terrain::SampleCount(s);
            size_t count = (size_t)n * n;
            std::string threeBytes = s.PaintedLayers;
            for (int i = 0; i < 10; ++i) Terrain::ApplyBrush(s, Terrain::BrushMode::Paint, { -20.0f, 10.0f }, 5.0f, 1.0f, 0.1f, 0.0f, nullptr, 4);
            Terrain::CommitEdit(s);
            CHECK(s.PaintedLayers.size() > threeBytes.size() * 3 / 2); // 5 bytes per sample now
            std::vector<uint32_t> layers = Terrain::BuildSplat(s, Terrain::BuildMesh(s, Terrain::BuildHeights(s)));
            size_t at = (size_t)((10.0f / 60.0f + 0.5f) * (n - 1) + 0.5f) * n + (size_t)((-20.0f / 80.0f + 0.5f) * (n - 1) + 0.5f);
            uint32_t first = layers[at], second = layers[count + at];
            CHECK((second & 255) > 200 && (first & 255) + (first >> 8 & 255) + (first >> 16 & 255) + (first >> 24) < 40);
            CHECK((layers[count + (size_t)(n / 2) * n + n / 2] & 255) == 0); // the rock patch has no sand
        }

        // Any number of layers: 8 make two full splat maps, and painting the last one survives saving and loading;
        // a single layer covers everything.
        {
            TerrainComponent s = t;
            s.Layers.resize(8);
            CHECK(Terrain::LayerCount(s) == 8 && Terrain::SplatMapCount(s) == 2);
            for (int i = 0; i < 10; ++i) Terrain::ApplyBrush(s, Terrain::BrushMode::Paint, { 0.0f, 0.0f }, 5.0f, 1.0f, 0.1f, 0.0f, nullptr, 7);
            int n = Terrain::SampleCount(s);
            size_t center = (size_t)(n / 2) * n + n / 2;
            CHECK(s.EditLayers.size() == (size_t)n * n * 2 && Terrain::LayerWeight(s, center, 7) > 0.8f);
            Terrain::CommitEdit(s);
            TerrainComponent copy = t;
            copy.Layers.resize(8);
            copy.PaintedLayers = s.PaintedLayers;
            Terrain::BeginEdit(copy);
            CHECK(Terrain::LayerWeight(copy, center, 7) > 0.8f && Terrain::LayerWeight(copy, center, 0) < 0.2f);
            TerrainComponent one = t;
            one.Layers.resize(1);
            std::vector<uint32_t> single = Terrain::BuildSplat(one, Terrain::BuildMesh(one, Terrain::BuildHeights(one)));
            bool all = single.size() == (size_t)n * n;
            for (uint32_t w : single) all &= w == 255u;
            CHECK(all);
            one.Layers.clear(); // still one layer
            CHECK(Terrain::LayerCount(one) == 1);
        }

        // Layer rules: rock on slopes over 30 degrees, snow above 70%, sand low and flat (over the others, being later),
        // grass (the first) everywhere else; noise breaks a layer into patches.
        {
            TerrainComponent s = t;
            s.Layers.resize(4);
            s.Layers[1].Rule = true;
            s.Layers[1].SlopeRange = { 30.0f, 90.0f };
            s.Layers[2].Rule = true;
            s.Layers[2].HeightRange = { 0.7f, 1.0f };
            s.Layers[3].Rule = true;
            s.Layers[3].HeightRange = { 0.0f, 0.15f };
            s.Layers[3].SlopeRange = { 0.0f, 20.0f };
            MeshData m;
            auto vertex = [&](float y, float tilt) {
                Vertex v{};
                v.Position = { (float)m.Vertices.size() * 3.0f, y, 0.0f };
                v.Normal = { std::sin(glm::radians(tilt)), std::cos(glm::radians(tilt)), 0.0f };
                m.Vertices.push_back(v);
            };
            vertex(1.0f, 0.0f);   // low, flat: sand
            vertex(10.0f, 50.0f); // steep: rock
            vertex(18.0f, 0.0f);  // high: snow
            vertex(8.0f, 5.0f);   // in between: grass
            vertex(1.0f, 50.0f);  // low but steep: rock
            std::vector<uint32_t> splat = Terrain::BuildSplat(s, m);
            auto weight = [&](size_t i, int layer) { return (splat[i] >> (layer * 8) & 255) / 255.0f; };
            CHECK(splat.size() == 5 && weight(0, 3) > 0.95f && weight(1, 1) > 0.95f && weight(2, 2) > 0.95f && weight(3, 0) > 0.95f && weight(4, 1) > 0.95f);
            bool sums = true;
            for (size_t i = 0; i < 5; ++i) sums &= std::abs(weight(i, 0) + weight(i, 1) + weight(i, 2) + weight(i, 3) - 1.0f) < 0.02f;
            CHECK(sums);
            CHECK(Terrain::SettingsKey(s) != Terrain::SettingsKey(t));
            // Sand with noise over a field of low flat ground: patches, some bare, some full.
            s.Layers[3].Noise = 1.0f;
            s.Layers[3].NoiseScale = 4.0f;
            MeshData field;
            for (int z = 0; z < 20; ++z)
                for (int x = 0; x < 20; ++x)
                {
                    Vertex v{};
                    v.Position = { x * 1.5f, 1.0f, z * 1.5f };
                    v.Normal = { 0.0f, 1.0f, 0.0f };
                    field.Vertices.push_back(v);
                }
            std::vector<uint32_t> patchy = Terrain::BuildSplat(s, field);
            int bare = 0, full = 0;
            for (uint32_t w : patchy)
            {
                bare += (w >> 24) < 40;
                full += (w >> 24) > 215;
            }
            CHECK(bare > 20 && full > 20);
        }

        // Details growing by layer: grass on the first layer (nothing painted), none where a rock rule (slopes over 12
        // degrees) covers it (well past that); with Grass 0 again, none at all.
        {
            TerrainComponent s = t;
            s.Layers.resize(2);
            s.Layers[0].Grass = 1.0f;
            s.Layers[1].Rule = true;
            s.Layers[1].SlopeRange = { 12.0f, 90.0f };
            s.Layers[1].SlopeFade = 1.0f;
            s.GrassDensity = 20.0f;
            s.GrassMaxSlope = 90.0f;
            std::vector<float> h = Terrain::BuildHeights(s);
            Terrain::DetailInstances grown = Terrain::BuildDetails(s, h);
            CHECK(grown.Grass.size() > 200 && grown.Trees.empty());
            int onSteep = 0;
            for (const glm::mat4& g : grown.Grass)
            {
                float x = g[3].x, z = g[3].z, d = 0.5f;
                float dx = (Terrain::HeightAt(s, h, x + d, z) - Terrain::HeightAt(s, h, x - d, z)) / (2.0f * d);
                float dz = (Terrain::HeightAt(s, h, x, z + d) - Terrain::HeightAt(s, h, x, z - d)) / (2.0f * d);
                onSteep += glm::degrees(std::atan(std::sqrt(dx * dx + dz * dz))) > 20.0f; // well past the rule (samples blend near it)
            }
            CHECK(onSteep <= (int)grown.Grass.size() / 50);
            s.Layers[0].Grass = 0.0f;
            CHECK(Terrain::BuildDetails(s, h).Grass.empty());
        }

        // The layer list saves as an array of objects, and a terrain saved with the older six fixed layers loads them
        // into the list.
        {
            const ComponentInfo* info = ComponentRegistry::Find("Terrain");
            CHECK(info && info->Upgrade);
            TerrainComponent s = t;
            s.Layers = { { "Mud", AssetHandle(42), { 0.5f, 0.4f, 0.3f, 1.0f }, 0.5f, AssetHandle(43), 1.5f }, { "Ice", AssetHandle::None(), { 0.8f, 0.9f, 1.0f, 1.0f }, 2.0f } };
            s.HeightBlend = 0.4f;
            nlohmann::json data = nlohmann::json::object();
            for (const FieldInfo& f : info->Fields) data[f.Name] = FieldToJson(f, &s, nullptr);
            CHECK(data["Layers"].is_array() && data["Layers"].size() == 2 && data["Layers"][1]["Name"] == "Ice");
            TerrainComponent back;
            for (const FieldInfo& f : info->Fields)
                if (data.contains(f.Name)) FieldFromJson(f, &back, data[f.Name], nullptr);
            info->Upgrade(&back, data);
            CHECK(back.Layers.size() == 2 && back.Layers[0].Name == "Mud" && back.Layers[0].Texture == AssetHandle(42) && Near(back.Layers[1].Tiling, 2.0f));
            CHECK(back.Layers[0].NormalMap == AssetHandle(43) && Near(back.Layers[0].NormalStrength, 1.5f) && !back.Layers[1].NormalMap.IsValid() && Near(back.HeightBlend, 0.4f));
            nlohmann::json old = { { "Grass Color", { 0.1, 0.2, 0.3, 1.0 } }, { "Sand Texture", 77 }, { "Texture Tiling", 0.5 } };
            TerrainComponent upgraded;
            upgraded.Layers.clear();
            info->Upgrade(&upgraded, old);
            CHECK(upgraded.Layers.size() == 6 && upgraded.Layers[4].Name == "Sand" && upgraded.Layers[4].Texture == AssetHandle(77));
            CHECK(Near(upgraded.Layers[0].Color.g, 0.2f) && Near(upgraded.Layers[5].Tiling, 0.5f));
            // A list longer than its most (16) is cut there.
            nlohmann::json many = nlohmann::json::array();
            for (int i = 0; i < 20; ++i) many.push_back({ { "Name", "L" + std::to_string(i) } });
            for (const FieldInfo& f : info->Fields)
                if (f.Name == "Layers") FieldFromJson(f, &back, many, nullptr);
            CHECK(back.Layers.size() == 16 && back.Layers[15].Name == "L15");
        }

        // Holes: digging removes the triangles around the samples from the mesh and the collider (rays and bodies
        // fall through), keeps the rest, saves as bits and closes again with Fill.
        {
            TerrainComponent s = t;
            for (int i = 0; i < 3; ++i) CHECK(Terrain::ApplyBrush(s, Terrain::BrushMode::DigHole, { 10.0f, -5.0f }, 3.0f, 1.0f, 0.1f, 0.0f));
            Terrain::CommitEdit(s);
            CHECK(!s.PaintedHoles.empty() && s.EditHoles.empty());
            std::vector<uint8_t> holes = Terrain::HoleMap(s);
            int n = Terrain::SampleCount(s);
            CHECK(holes.size() == (size_t)n * n);
            CHECK(Terrain::IsHoleAt(s, holes, 10.0f, -5.0f) && !Terrain::IsHoleAt(s, holes, -30.0f, 20.0f));
            MeshData m = Terrain::BuildMesh(s, Terrain::BuildHeights(s));
            CHECK(m.Indices.size() < 65u * 65u * 6u && m.Indices.size() > 65u * 65u * 5u && m.Indices.size() % 3 == 0);
            bool skipsHoles = true;
            for (uint32_t i : m.Indices) skipsHoles &= holes[i] == 0;
            CHECK(skipsHoles);
            TerrainComponent copy = t;
            copy.PaintedHoles = s.PaintedHoles;
            CHECK(Terrain::HoleMap(copy) == holes && Terrain::SettingsKey(copy) != Terrain::SettingsKey(t));

            Scene scene("TerrainHoles");
            Entity e = scene.CreateEntity("Terrain");
            scene.GetRegistry().Emplace<TerrainComponent>(e) = s;
            Entity ball = scene.CreateEntity("Ball");
            scene.GetRegistry().Get<TransformComponent>(ball).Position = { 10.0f, Terrain::HeightAt(s, Terrain::BuildHeights(s), 10.0f, -5.0f) + 2.0f, -5.0f };
            scene.GetRegistry().Emplace<RigidbodyComponent>(ball);
            scene.GetRegistry().Emplace<SphereColliderComponent>(ball).Radius = 0.3f;
            scene.UpdateTransforms();
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            RaycastHit throughHole = Physics3DWorld::Raycast(*rt, { 11.5f, 100.0f, -5.0f }, { 0, -1, 0 }, 500.0f, Physics::kAllLayers);
            RaycastHit solid = Physics3DWorld::Raycast(*rt, { -30.0f, 100.0f, 20.0f }, { 0, -1, 0 }, 500.0f, Physics::kAllLayers);
            CHECK(!throughHole && solid);
            float startY = rt->GetRegistry().Get<TransformComponent>(ball).Position.y;
            for (int i = 0; i < 120; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(rt->GetRegistry().Get<TransformComponent>(ball).Position.y < startY - 6.0f); // fell into the hole
            rt->OnRuntimeStop();

            Terrain::BeginEdit(s);
            for (int i = 0; i < 3; ++i) Terrain::ApplyBrush(s, Terrain::BrushMode::FillHole, { 10.0f, -5.0f }, 4.0f, 1.0f, 0.1f, 0.0f);
            Terrain::CommitEdit(s);
            CHECK(s.PaintedHoles.empty() && Terrain::HoleMap(s).empty());
        }

        // Physics: the terrain is a static height field matching HeightAt (rays from above land on the surface).
        Scene scene("Terrain");
        Entity e = scene.CreateEntity("Terrain");
        scene.GetRegistry().Get<TransformComponent>(e).Position = { 5, -2, 3 };
        scene.GetRegistry().Emplace<TerrainComponent>(e) = t;
        scene.UpdateTransforms();
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        bool matches = true;
        for (glm::vec2 p : { glm::vec2(0, 0), glm::vec2(12.3f, -7.1f), glm::vec2(-31.0f, 22.5f), glm::vec2(38.0f, 28.0f) })
        {
            RaycastHit hit = Physics3DWorld::Raycast(*rt, { p.x + 5, 100, p.y + 3 }, { 0, -1, 0 }, 500.0f, Physics::kAllLayers);
            matches &= hit && Near(hit.Point.y, Terrain::HeightAt(t, heights, p.x, p.y) - 2.0f, 0.02f);
        }
        CHECK(matches);
        rt->OnRuntimeStop();
    }

    void TestRetargeting()
    {
        MeshData tentacle;
        CHECK(MeshData::LoadFromFile(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Tentacle.gltf", tentacle));
        if (!tentacle.SkeletonData || tentacle.Clips.empty()) return;
        const Skeleton& source = *tentacle.SkeletonData;
        // The target: the same joints under rig-style names, twice as long, with a different bind rotation on one.
        Skeleton target = source;
        for (auto& j : target.Joints)
        {
            j.Name = "mixamorig:" + j.Name;
            j.Translation *= 2.0f;
        }
        int twisted = (int)target.Joints.size() / 2;
        target.Joints[twisted].Rotation = glm::angleAxis(0.7f, glm::vec3(0, 0, 1)) * target.Joints[twisted].Rotation;
        CHECK(Animation::MatchJoint(target, source.Joints[0].Name) == 0 && Animation::MatchJoint(target, "Armature|" + source.Joints[1].Name) == 1);
        CHECK(Animation::MatchJoint(target, "NoSuchJoint") == -1);

        const AnimationClip& clip = tentacle.Clips[0];
        AnimationClip mapped = Animation::RetargetClip(source, target, clip);
        CHECK(mapped.Name == clip.Name && Near(mapped.Duration, clip.Duration) && !mapped.Channels.empty());
        // At any time, each joint turns away from its own bind pose exactly as the source joint does from its own.
        Pose ps, pt;
        bool same = true;
        for (float t : { 0.0f, clip.Duration * 0.37f, clip.Duration * 0.81f })
        {
            Animation::Sample(source, clip, t, ps);
            Animation::Sample(target, mapped, t, pt);
            for (size_t j = 0; j < source.Joints.size(); ++j)
            {
                glm::quat ds = glm::inverse(source.Joints[j].Rotation) * ps.Rotations[j];
                glm::quat dt = glm::inverse(target.Joints[j].Rotation) * pt.Rotations[j];
                same &= std::abs(std::abs(glm::dot(ds, dt)) - 1.0f) < 1e-4f;
                if (source.Joints[j].Parent >= 0) same &= Near(pt.Translations[j], target.Joints[j].Translation, 1e-4f); // its own proportions
            }
        }
        CHECK(same);
    }

    void TestConvolutionReverb()
    {
        // FFT round trip.
        std::vector<std::complex<float>> data(64);
        for (size_t i = 0; i < data.size(); ++i) data[i] = { std::sin(i * 0.37f), std::cos(i * 0.11f) };
        auto original = data;
        Convolver::FFT(data, false);
        Convolver::FFT(data, true);
        float fftError = 0.0f;
        for (size_t i = 0; i < data.size(); ++i) fftError = std::max(fftError, std::abs(data[i] - original[i]));
        CHECK(fftError < 1e-4f);

        // A unit impulse passes the signal through, one block late.
        Convolver delta;
        float one = 1.0f;
        delta.Init(&one, 1, 1, 1, 256);
        CHECK(delta.IsReady() && delta.GetLatency() == 256);
        std::vector<float> in(2000), out(2000);
        for (size_t i = 0; i < in.size(); ++i) in[i] = std::sin(i * 0.05f) + ((i * 7919) % 13) * 0.01f;
        delta.Process(in.data(), out.data(), in.size(), 1.0f, 0.0f);
        float delayError = 0.0f;
        for (size_t i = 256; i < in.size(); ++i) delayError = std::max(delayError, std::abs(out[i] - in[i - 256]));
        CHECK(delayError < 1e-4f);

        // A long random response (several partitions), stereo from a mono response, in odd-sized chunks: matches
        // direct convolution with the normalized response.
        std::vector<float> ir(1500);
        uint32_t seed = 12345;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f - 0.5f; };
        for (auto& v : ir) v = rnd();
        double energy = 0.0;
        for (float v : ir) energy += (double)v * v;
        Convolver conv;
        conv.Init(ir.data(), ir.size(), 1, 2, 256);
        size_t frames = 4000;
        std::vector<float> stereo(frames * 2), result(frames * 2);
        for (auto& v : stereo) v = rnd();
        for (size_t at = 0; at < frames;)
        {
            size_t n = std::min<size_t>(frames - at, 37 + at % 101);
            conv.Process(stereo.data() + at * 2, result.data() + at * 2, n, 1.0f, 0.0f);
            at += n;
        }
        float maxError = 0.0f;
        for (size_t f = 256; f < frames; f += 7)
            for (int c = 0; c < 2; ++c)
            {
                double expected = 0.0;
                size_t t = f - 256;
                for (size_t k = 0; k < ir.size() && k <= t; ++k) expected += (double)ir[k] * stereo[(t - k) * 2 + c];
                expected /= std::sqrt(energy);
                maxError = std::max(maxError, (float)std::abs(expected - result[f * 2 + c]));
            }
        CHECK(maxError < 2e-3f);
        // The sample impulse response is a readable audio asset.
        fs::path hall = fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Audio/Hall IR.wav";
        CHECK(fs::exists(hall));

        // With an audio device: Master runs through the hall's response and a tone still reaches the output.
        AudioEngine::Init();
        if (!AudioEngine::IsAvailable()) return;
        fs::path assets = fs::temp_directory_path() / "dimension_convolution_test" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        fs::copy_file(hall, assets / "Hall IR.wav");
        fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Tests" / "Data" / "Tone.opus", assets / "Tone.opus");
        AssetManager::Init(assets);
        AssetHandle impulse = AssetManager::GetHandle(assets / "Hall IR.wav");
        AssetHandle tone = AssetManager::GetHandle(assets / "Tone.opus");
        CHECK(impulse.IsValid() && tone.IsValid());
        AudioMixer::Groups()[0].Convolution = true;
        AudioMixer::Groups()[0].ConvolutionImpulse = (uint64_t)impulse;
        AudioMixer::Rebuild();
        {
            Scene scene("Hall");
            Entity speaker = scene.CreateEntity("Speaker");
            auto& src = scene.GetRegistry().Emplace<AudioSourceComponent>(speaker);
            src.Clip = tone;
            src.Loop = true;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            AudioMixer::GetLevel(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(AudioMixer::GetLevel(0).Peak > 0.01f);
            rt->OnRuntimeStop();
        }
        AudioMixer::Groups()[0].Convolution = false;
        AudioMixer::Groups()[0].ConvolutionImpulse = 0;
        AudioMixer::Rebuild();
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestReverbZones()
    {
        // A hall (full reverb within 2 m, gone by 6 m) and a smaller cave overlapping its edge, both on SFX.
        Scene scene("Zones");
        auto& reg = scene.GetRegistry();
        Entity listener = scene.CreateEntity("Listener");
        reg.Emplace<CameraComponent>(listener).Primary = true;
        auto zone = [&](const char* name, glm::vec3 pos, float lo, float hi, float wet, float room) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = pos;
            auto& z = reg.Emplace<AudioReverbZoneComponent>(e);
            z.MinDistance = lo;
            z.MaxDistance = hi;
            z.Wet = wet;
            z.RoomSize = room;
            z.Group = "SFX";
            return e;
        };
        zone("Hall", { 10, 0, 0 }, 2.0f, 6.0f, 0.8f, 0.9f);
        Entity cave = zone("Cave", { 16, 0, 0 }, 1.0f, 3.0f, 0.4f, 0.3f);
        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        auto at = [&](float x, const char* group = "SFX") { return AudioWorld::ReverbZoneAt(*runtime, { x, 0, 0 }, group); };
        CHECK(at(0.0f).Weight == 0.0f);                                   // far away
        CHECK(Near(at(9.0f).Weight, 1.0f) && Near(at(9.0f).Wet, 0.8f));     // inside the hall's Min Distance
        CHECK(Near(at(13.0f).Weight, 0.75f) && Near(at(13.0f).Wet, 0.8f));  // fading, with the hall's settings
        CHECK(at(9.0f, "Music").Weight == 0.0f);                          // other groups are unaffected
        AudioMixer::ZoneReverb mixed = at(15.5f); // hall at 0.125, cave at 1
        CHECK(Near(mixed.Weight, 1.0f) && mixed.Wet > 0.4f && mixed.Wet < 0.5f && mixed.RoomSize > 0.3f && mixed.RoomSize < 0.4f);
        runtime->GetRegistry().Get<TagComponent>(cave).Active = false; // inactive zones do nothing
        runtime->OnUpdateRuntime(0.0f);
        CHECK(Near(at(15.5f).Weight, 0.125f));

        // The group's reverb: its own settings faded toward the zone's.
        AudioMixerGroup g;
        g.Reverb = false;
        CHECK(!AudioMixer::EffectiveReverb(g, {}).On);
        auto half = AudioMixer::EffectiveReverb(g, { 0.5f, 0.8f, 0.4f, 0.6f });
        CHECK(half.On && Near(half.Wet, 0.3f) && Near(half.RoomSize, 0.8f)); // from dry, in the zone's room
        g.Reverb = true;
        g.ReverbWet = 0.2f;
        g.ReverbRoomSize = 0.2f;
        auto full = AudioMixer::EffectiveReverb(g, { 1.0f, 0.8f, 0.4f, 0.6f });
        CHECK(full.On && Near(full.Wet, 0.6f) && Near(full.RoomSize, 0.8f));
        CHECK(Near(AudioMixer::EffectiveReverb(g, {}).Wet, 0.2f));
        // Play mode drives the mixer from the listener's position, and stopping clears it.
        int sfx = AudioMixer::FindGroup("SFX");
        runtime->GetRegistry().Get<TransformComponent>(listener).Position = { 9, 0, 0 };
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(sfx < 0 || Near(AudioMixer::GetZoneReverb(sfx).Weight, 1.0f));
        runtime->OnRuntimeStop();
        runtime.reset();
        CHECK(sfx < 0 || AudioMixer::GetZoneReverb(sfx).Weight == 0.0f);
    }

    void TestAudioOcclusion()
    {
        Scene scene("Occlusion");
        auto& reg = scene.GetRegistry();
        Entity listener = scene.CreateEntity("Listener");
        reg.Get<TransformComponent>(listener).Position = { 0, 1, 0 };
        reg.Emplace<CameraComponent>(listener).Primary = true;
        reg.Emplace<SphereColliderComponent>(listener); // the listener's own body never occludes
        Entity source = scene.CreateEntity("Source");
        reg.Get<TransformComponent>(source).Position = { 0, 1, -10 };
        reg.Emplace<SphereColliderComponent>(source); // nor does the source's
        Entity open = scene.CreateEntity("Open Source");
        reg.Get<TransformComponent>(open).Position = { 10, 1, 0 };
        Entity wall = scene.CreateEntity("Wall");
        reg.Get<TransformComponent>(wall).Position = { 0, 1, -5 };
        reg.Get<TransformComponent>(wall).Scale = { 4, 4, 0.5f };
        reg.Emplace<BoxColliderComponent>(wall);
        Entity zone = scene.CreateEntity("Trigger Zone");
        reg.Get<TransformComponent>(zone).Position = { 5, 1, 0 };
        reg.Get<TransformComponent>(zone).Scale = { 1, 4, 4 };
        reg.Emplace<BoxColliderComponent>(zone).IsTrigger = true; // triggers don't block sound
        for (Entity e : { source, open })
        {
            auto& src = reg.Emplace<AudioSourceComponent>(e);
            src.Spatial = true;
            src.Occlusion = true;
        }
        auto& plain = reg.Emplace<AudioSourceComponent>(scene.CreateEntity("No Occlusion"));
        plain.Spatial = true;

        auto runtime = scene.Clone();
        runtime->OnRuntimeStart(); // the first audio update snaps to the current state
        auto& rr = runtime->GetRegistry();
        auto amount = [&](Entity e) { return rr.Get<AudioSourceComponent>(e).OcclusionAmount; };
        CHECK(Near(amount(source), 1.0f) && Near(amount(open), 0.0f));
        CHECK(AudioWorld::IsOccluded(*runtime, { 0, 1, 0 }, listener, source) && !AudioWorld::IsOccluded(*runtime, { 0, 1, 0 }, listener, open));
        CHECK(!AudioWorld::IsOccluded(*runtime, { 3, 1, -10 }, Entity::Null(), source)); // around the wall's end
        // Remove the wall: the sound clears over 0.15 s instead of jumping.
        runtime->DestroyEntity(wall);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        float easing = amount(source);
        CHECK(easing > 0.6f && easing < 0.9f);
        for (int i = 0; i < 12; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(Near(amount(source), 0.0f));
        for (auto [e, src] : rr.ViewOf<AudioSourceComponent>())
            if (!src.Occlusion) CHECK(src.OcclusionAmount == 0.0f);
        runtime->OnRuntimeStop();

        // Audio Occluders: two thin panes of 0.3 add up to 0.6; enough of them saturate at 1.
        {
            Scene panes("Panes");
            auto& pr = panes.GetRegistry();
            Entity ear = panes.CreateEntity("Ear");
            Entity sound = panes.CreateEntity("Sound");
            pr.Get<TransformComponent>(sound).Position = { 0, 0, -10 };
            for (int i = 0; i < 5; ++i)
            {
                Entity pane = panes.CreateEntity("Pane");
                pr.Get<TransformComponent>(pane).Position = { 0, 0, -2.0f - (float)i * 1.5f };
                pr.Get<TransformComponent>(pane).Scale = { 3, 3, 0.05f };
                pr.Emplace<BoxColliderComponent>(pane);
                pr.Emplace<AudioOccluderComponent>(pane).Occlusion = 0.3f;
                if (i >= 2) pr.Get<TagComponent>(pane).Active = false; // first only two (inactive objects have no collider)
            }
            panes.UpdateTransforms();
            auto rt = panes.Clone();
            rt->OnRuntimeStart();
            CHECK(Near(AudioWorld::Occlusion(*rt, { 0, 0, 0 }, ear, sound), 0.6f));
            // Activated while playing: the panes' colliders join on the next physics step.
            for (auto [e, tag] : rt->GetRegistry().ViewOf<TagComponent>()) tag.Active = true;
            rt->OnUpdateRuntime(1.0f / 30.0f);
            CHECK(Near(AudioWorld::Occlusion(*rt, { 0, 0, 0 }, ear, sound), 1.0f));
            rt->OnRuntimeStop();
        }
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

    void TestRichText()
    {
        auto chars = Font::ParseRichText("a<b>b<i>c</i></b><color=#ff8000>d<color=blue>e</color>f</color><size=40>g<size=50%>h</size></size><u>i</u><s>j</s>");
        std::string plain;
        for (auto& c : chars) plain += (char)c.Codepoint;
        CHECK(plain == "abcdefghij");
        if (plain != "abcdefghij") return;
        CHECK(!chars[0].Bold && chars[1].Bold && !chars[1].Italic && chars[2].Bold && chars[2].Italic && !chars[3].Bold);
        CHECK(chars[3].HasColor && Near(chars[3].Color.r, 1.0f) && Near(chars[3].Color.g, 128.0f / 255.0f) && Near(chars[3].Color.b, 0.0f));
        CHECK(chars[4].HasColor && chars[4].Color == glm::vec4(0, 0, 1, 1)); // nested color, then back to the outer one
        CHECK(chars[5].Color == chars[3].Color && !chars[6].HasColor);
        CHECK(Near(chars[6].Size, 40.0f) && Near(chars[6].SizeScale, 1.0f) && Near(chars[7].Size, 40.0f) && Near(chars[7].SizeScale, 0.5f));
        CHECK(chars[8].Underline && !chars[8].Strikethrough && chars[9].Strikethrough && chars[9].Size < 0.0f);
        // Short and alpha hex colors, case-insensitive tags, quotes, and <br>.
        chars = Font::ParseRichText("<COLOR=\"#0f08\">x</Color><br>y");
        CHECK(chars.size() == 3 && Near(chars[0].Color.g, 1.0f) && Near(chars[0].Color.a, 136.0f / 255.0f) && chars[1].Codepoint == '\n' && !chars[2].HasColor);
        // Unknown or malformed tags stay as text; a stray closing tag is harmless.
        CHECK(Font::StripRichText("a < b > c") == "a < b > c");
        CHECK(Font::StripRichText("<wave>hi</wave>") == "<wave>hi</wave>");
        CHECK(Font::StripRichText("<color=nope>x</color>") == "<color=nope>x");
        CHECK(Font::StripRichText("<size=0>x</b>y") == "<size=0>xy");
        CHECK(Font::StripRichText("2D Demo >") == "2D Demo >" && Font::StripRichText("<<b>bold</b>") == "<bold");
        CHECK(Font::StripRichText("caf\xC3\xA9 <i>\xC3\xBC</i>") == "caf\xC3\xA9 \xC3\xBC"); // UTF-8 survives
    }

    void TestBlendTreesDirectional()
    {
        AnimState tree;
        tree.Motion = AnimMotionType::BlendTree2DDirectional;
        tree.Children = { { "Idle", 0, 0 }, { "Forward", 0, 1 }, { "Right", 1, 0 }, { "Back", 0, -1 }, { "Left", -1, 0 } };
        std::vector<float> w;
        auto weights = [&](glm::vec2 p) { AnimatorStateMachine::BlendWeightsAt(tree, p, w); return w; };
        auto is = [](const std::vector<float>& got, std::vector<float> want) {
            if (got.size() != want.size()) return false;
            for (size_t i = 0; i < got.size(); ++i)
                if (!Near(got[i], want[i])) return false;
            return true;
        };
        CHECK(is(weights({ 0, 0 }), { 1, 0, 0, 0, 0 }));
        CHECK(is(weights({ 0, 1 }), { 0, 1, 0, 0, 0 }));
        CHECK(is(weights({ 0, 0.5f }), { 0.5f, 0.5f, 0, 0, 0 }));      // halfway to Forward: half idle
        CHECK(is(weights({ 0.5f, 0.5f }), { 0, 0.5f, 0.5f, 0, 0 }));    // on the Forward-Right edge
        CHECK(is(weights({ 0.25f, 0.25f }), { 0.5f, 0.25f, 0.25f, 0, 0 }));
        CHECK(is(weights({ 2, 2 }), { 0, 0.5f, 0.5f, 0, 0 }));          // past the edge: direction only
        CHECK(is(weights({ -0.3f, 0 }), { 0.7f, 0, 0, 0, 0.3f }));
        CHECK(is(weights({ 0.2f, -0.6f }), { 0.2f, 0, 0.2f, 0.6f, 0 })); // Back-Right quadrant (angle wraps past -90)
        // Weights always sum to 1, wherever the point is.
        bool sums = true;
        for (float a = 0.0f; a < 6.283f; a += 0.37f)
            for (float r : { 0.1f, 0.6f, 1.0f, 3.0f })
            {
                weights({ r * std::cos(a), r * std::sin(a) });
                float s = 0.0f;
                for (float x : w) s += x;
                sums &= Near(s, 1.0f) && std::all_of(w.begin(), w.end(), [](float x) { return x >= 0.0f; });
            }
        CHECK(sums);
        // No center motion: only the direction counts.
        tree.Children.erase(tree.Children.begin());
        CHECK(is(weights({ 0, 0.2f }), { 1, 0, 0, 0 }));
        CHECK(is(weights({ 0.1f, 0.1f }), { 0.5f, 0.5f, 0, 0 }));
        // Two opposite directions: the nearer one plays.
        tree.Children = { { "Forward", 0, 1 }, { "Back", 0, -1 } };
        CHECK(is(weights({ 0.5f, 0.5f }), { 1, 0 }) && is(weights({ 0.4f, -0.1f }), { 0, 1 }));

        // Saved with its Y positions.
        AnimatorController c;
        c.States = { tree };
        c.States[0].BlendParameterY = "Y";
        fs::path path = fs::temp_directory_path() / "dimension_directional.controller";
        CHECK(c.Save(path));
        auto back = AnimatorController::Load(path);
        CHECK(back->States[0].Motion == AnimMotionType::BlendTree2DDirectional && back->States[0].BlendParameterY == "Y" &&
              Near(back->States[0].Children[1].ThresholdY, -1.0f));
        fs::remove(path);
    }

    void TestInverseKinematics()
    {
        // A straight chain up Y: Root -> Mid (1 up) -> Tip (1 up).
        Skeleton sk;
        auto joint = [](const char* name, int parent, glm::vec3 t) {
            Joint j;
            j.Name = name;
            j.Parent = parent;
            j.Translation = t;
            return j;
        };
        sk.Joints = { joint("Root", -1, { 0, 0, 0 }), joint("Mid", 0, { 0, 1, 0 }), joint("Tip", 1, { 0, 1, 0 }) };
        sk.ComputeOrder();
        auto globalPos = [&](const Pose& p, int j) {
            std::vector<glm::mat4> g;
            Animation::JointGlobals(sk, p, g);
            return glm::vec3(g[j][3]);
        };
        Pose pose;
        pose.SetBindPose(sk);
        // Two-bone: reaches a point in range, keeping the bone lengths.
        glm::vec3 target(1.0f, 1.0f, 0.0f);
        CHECK(Animation::SolveIK(sk, pose, 2, 2, target, nullptr, 1.0f));
        CHECK(Near(globalPos(pose, 2), target, 1e-3f));
        CHECK(Near(glm::distance(globalPos(pose, 0), globalPos(pose, 1)), 1.0f) && Near(glm::distance(globalPos(pose, 1), globalPos(pose, 2)), 1.0f));
        // The hint picks the side the middle joint bends toward.
        for (float side : { 1.0f, -1.0f })
        {
            pose.SetBindPose(sk);
            glm::vec3 hint(0.0f, 1.0f, 2.0f * side);
            CHECK(Animation::SolveIK(sk, pose, 2, 2, { 0.0f, 1.2f, 0.0f }, &hint, 1.0f));
            CHECK(Near(globalPos(pose, 2), { 0.0f, 1.2f, 0.0f }, 1e-3f) && globalPos(pose, 1).z * side > 0.5f);
            // Also from a chain already bent the other way (toward +X).
            pose.SetBindPose(sk);
            pose.Rotations[0] = glm::angleAxis(glm::radians(-30.0f), glm::vec3(0, 0, 1));
            pose.Rotations[1] = glm::angleAxis(glm::radians(60.0f), glm::vec3(0, 0, 1));
            CHECK(globalPos(pose, 1).x > 0.4f);
            CHECK(Animation::SolveIK(sk, pose, 2, 2, { 0.3f, 1.2f, 0.0f }, &hint, 1.0f));
            CHECK(Near(globalPos(pose, 2), { 0.3f, 1.2f, 0.0f }, 1e-3f) && globalPos(pose, 1).z * side > 0.5f);
        }
        // Out of reach: the chain stretches straight toward the target.
        pose.SetBindPose(sk);
        Animation::SolveIK(sk, pose, 2, 2, { 3.0f, 0.0f, 0.0f }, nullptr, 1.0f);
        CHECK(Near(globalPos(pose, 2), { 2.0f, 0.0f, 0.0f }, 1e-2f));
        // Half weight lands between the animated pose and the solve.
        pose.SetBindPose(sk);
        Animation::SolveIK(sk, pose, 2, 2, target, nullptr, 0.5f);
        glm::vec3 half = globalPos(pose, 2);
        CHECK(glm::distance(half, target) > 0.1f && glm::distance(half, { 0, 2, 0 }) > 0.1f);
        // Nothing to solve: a root joint as the tip, or no weight.
        CHECK(!Animation::SolveIK(sk, pose, 0, 2, target, nullptr, 1.0f) && !Animation::SolveIK(sk, pose, 2, 2, target, nullptr, 0.0f));

        // Longer chains iterate: the Tentacle's six bones reach a point off to the side.
        MeshData data;
        CHECK(MeshData::LoadFromFile(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Tentacle.gltf", data));
        const Skeleton& tentacle = *data.SkeletonData;
        Pose tp;
        tp.SetBindPose(tentacle);
        int tip = tentacle.FindJoint("Bone5");
        std::vector<glm::mat4> g;
        Animation::JointGlobals(tentacle, tp, g);
        glm::vec3 base(g[tentacle.FindJoint("Bone0")][3]), end(g[tip][3]);
        float reach = glm::distance(base, end);
        glm::vec3 aim = base + glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f)) * reach * 0.7f;
        CHECK(Animation::SolveIK(tentacle, tp, tip, 5, aim, nullptr, 1.0f));
        Animation::JointGlobals(tentacle, tp, g);
        CHECK(glm::distance(glm::vec3(g[tip][3]), aim) < 0.01f * reach);

        // Look-at: the tip's +Y axis aims at a point off to the side, shared over the chain, within the angle limit.
        auto aimDir = [&](const Pose& p, int j, glm::vec3 axis) {
            std::vector<glm::mat4> gl;
            Animation::JointGlobals(sk, p, gl);
            return glm::normalize(glm::mat3(gl[j]) * axis);
        };
        pose.SetBindPose(sk);
        glm::vec3 lookTarget(5.0f, 2.0f, 0.0f); // level with the tip, straight out along +X
        CHECK(Animation::SolveLookAt(sk, pose, 2, { 0, 1, 0 }, 3, lookTarget, 180.0f, 1.0f));
        glm::vec3 tipPos = globalPos(pose, 2);
        CHECK(glm::dot(aimDir(pose, 2, { 0, 1, 0 }), glm::normalize(lookTarget - tipPos)) > 0.999f);
        CHECK(std::abs(pose.Rotations[0].w) < std::cos(glm::radians(5.0f))); // the root took part of the turn (over 10 degrees)
        pose.SetBindPose(sk);
        Animation::SolveLookAt(sk, pose, 2, { 0, 1, 0 }, 1, { 0.0f, 2.0f, -5.0f }, 30.0f, 1.0f); // 90 degrees away, limited to 30
        CHECK(Near(glm::degrees(std::acos(std::clamp(aimDir(pose, 2, { 0, 1, 0 }).y, -1.0f, 1.0f))), 30.0f, 0.1f) && pose.Rotations[1] == glm::quat(1, 0, 0, 0));
    }

    void TestLayerSync()
    {
        AnimatorController c;
        AnimState idle, walk;
        idle.Name = "Idle";
        idle.Clip = "Sway";
        walk.Name = "Walk";
        walk.Clip = "Wave";
        walk.EditorPosition = { 200, 0 };
        walk.Events = { { 0.5f, "OnStep" } };
        c.States = { idle, walk };
        c.Transitions = { { 0, 1, true, 1.0f, 0.25f, {} } };
        AnimLayer hurt;
        hurt.Name = "Hurt";
        hurt.SyncedLayer = 0;
        c.Layers = { hurt };
        c.SyncLayerStates();
        const auto& m = c.Layers[0].Machine;
        // The synced layer takes the source's states and transitions, without motions (it plays the source's) or events.
        CHECK(m.States.size() == 2 && m.States[1].Name == "Walk" && m.States[1].EditorPosition == glm::vec2(200, 0) && m.Transitions.size() == 1);
        CHECK(!HasOwnMotion(m.States[0]) && !HasOwnMotion(m.States[1]) && m.States[1].Events.empty());
        // Its own motion for a state survives syncing again (after the source changed) and save/load.
        c.Layers[0].Machine.States[1].Clip = "Curl";
        c.States[1].Name = "Limp";
        AnimState run;
        run.Name = "Run";
        c.States.push_back(run);
        c.SyncLayerStates();
        CHECK(m.States.size() == 3 && m.States[1].Name == "Limp" && m.States[1].Clip == "Curl" && m.States[2].Name == "Run");
        fs::path path = fs::temp_directory_path() / "dimension_sync.controller";
        CHECK(c.Save(path));
        auto back = AnimatorController::Load(path);
        CHECK(back->Layers.size() == 1 && back->Layers[0].SyncedLayer == 0 && back->Layers[0].Machine.States.size() == 3);
        CHECK(back->Layers[0].Machine.States[1].Clip == "Curl" && !HasOwnMotion(back->Layers[0].Machine.States[0]));
        fs::remove(path);
        // Only earlier, unsynced layers can be followed.
        AnimLayer second;
        second.SyncedLayer = 1; // the synced "Hurt" layer
        AnimLayer self;
        self.SyncedLayer = 3; // itself
        c.Layers.push_back(second);
        c.Layers.push_back(self);
        c.SyncLayerStates();
        CHECK(c.Layers[1].SyncedLayer == -1 && c.Layers[2].SyncedLayer == -1 && c.Layers[0].SyncedLayer == 0);
        CHECK(c.LayerMachine(0) == &c && c.LayerMachine(2) == &c.Layers[1].Machine && c.LayerMachine(4) == nullptr);
    }

    void TestSubStateMachines()
    {
        // Idle at the top level; a "Combat" sub-state machine holding Slash -> Recover, entered at Slash.
        AnimatorController c;
        c.Parameters = { { "Fight", AnimParameterType::Trigger, 0.0f }, { "Calm", AnimParameterType::Trigger, 0.0f } };
        auto state = [](const char* name, int group) { AnimState st; st.Name = name; st.Clip = name; st.Group = group; return st; };
        c.Groups = { { "Combat", -1, 2, { 300, 0 } }, { "Inner", 0, -1, { 0, 0 } } };
        c.States = { state("Idle", -1), state("Recover", 0), state("Slash", 0), state("Feint", 1) };
        AnimTransition fight{ 0, 0, false, 1.0f, 0.1f, { { "Fight", AnimConditionOp::IsTrue, 0.0f } } };
        fight.ToGroup = 0;
        AnimTransition slashDone{ 2, 1, true, 1.0f, 0.1f, {} };
        AnimTransition calm{ AnimTransition::FromGroup(0), 0, false, 1.0f, 0.1f, { { "Calm", AnimConditionOp::IsTrue, 0.0f } } };
        c.Transitions = { fight, slashDone, calm };
        CHECK(c.EntryState(0) == 2 && c.EntryState(1) == 3); // Inner has no default: its first state
        CHECK(c.IsInGroup(3, 0) && c.IsInGroup(3, 1) && !c.IsInGroup(0, 0) && c.IsInGroup(1, 0));

        AnimatorRuntime rt;
        auto duration = [](const std::string&) { return 1.0f; };
        auto step = [&](float dt) { return AnimatorStateMachine::Update(c, rt, dt, duration, [](const AnimState&, float) {}); };
        step(0.1f);
        CHECK(rt.State == 0);
        rt.Triggers.insert("Fight");
        CHECK(step(0.1f) == 0 && rt.State == 2); // into the group: its entry state
        CHECK(step(1.0f) == 1 && rt.State == 1);  // inside the group, normal transitions
        rt.Triggers.insert("Calm");
        CHECK(step(0.1f) == 2 && rt.State == 0);  // the group's transition applies from Recover
        rt.Triggers.insert("Calm");
        CHECK(step(0.1f) == -1 && rt.State == 0); // but not from outside the group
        // A nested state counts as inside the outer group too.
        rt.State = 3;
        rt.Triggers.insert("Calm");
        CHECK(step(0.1f) == 2 && rt.State == 0);

        // Groups, membership and group transitions survive save/load.
        fs::path path = fs::temp_directory_path() / "dimension_substates.controller";
        CHECK(c.Save(path));
        auto back = AnimatorController::Load(path);
        CHECK(back->Groups.size() == 2 && back->Groups[1].Parent == 0 && back->Groups[0].DefaultState == 2 && back->States[3].Group == 1);
        CHECK(back->Transitions[0].ToGroup == 0 && back->Transitions[2].From == AnimTransition::FromGroup(0));
        fs::remove(path);
        // Removing a group moves its contents up and drops its transitions.
        c.RemoveGroup(0);
        CHECK(c.Groups.size() == 1 && c.Groups[0].Parent == -1 && c.States[1].Group == -1 && c.States[3].Group == 0);
        CHECK(c.Transitions.size() == 1 && c.Transitions[0].From == 2);
        // Removing a state fixes group entries.
        c.Groups[0].DefaultState = 3;
        c.RemoveState(0);
        CHECK(c.Groups[0].DefaultState == 2);

        // Entry and Exit nodes. Combat's Entry picks Heavy (in the nested Inner group) when "Heavy" is set, else its
        // default; Slash -> Exit leaves Combat for the top level's Entry ("Armed" -> straight back into Combat, else the
        // default state Idle); Heavy -> Exit leaves Inner: Inner's own transitions decide ("Rest" -> Rest), else
        // Combat's Entry.
        AnimatorController d;
        d.Parameters = { { "Fight", AnimParameterType::Trigger, 0.0f }, { "Heavy", AnimParameterType::Bool, 0.0f },
                         { "Armed", AnimParameterType::Bool, 0.0f }, { "Rest", AnimParameterType::Bool, 0.0f } };
        d.Groups = { { "Combat", -1, 1, { 300, 0 } }, { "Inner", 0, 2, { 0, 0 } } };
        d.States = { state("Idle", -1), state("Slash", 0), state("Heavy", 1), state("Rest", -1) };
        AnimTransition enter{ 0, 0, false, 1.0f, 0.1f, { { "Fight", AnimConditionOp::IsTrue, 0.0f } } };
        enter.ToGroup = 0;
        AnimTransition heavyEntry{ AnimTransition::FromEntry(0), 0, false, 1.0f, 0.0f, { { "Heavy", AnimConditionOp::IsTrue, 0.0f } } };
        heavyEntry.ToGroup = 1;
        AnimTransition slashExit{ 1, 0, true, 1.0f, 0.1f, {} };
        slashExit.ToExit = true;
        AnimTransition heavyExit{ 2, 0, true, 1.0f, 0.1f, {} };
        heavyExit.ToExit = true;
        AnimTransition restFromInner{ AnimTransition::FromGroup(1), 3, true, 50.0f, 0.1f, { { "Rest", AnimConditionOp::IsTrue, 0.0f } } };
        AnimTransition armedEntry{ AnimTransition::FromEntry(-1), 0, false, 1.0f, 0.0f, { { "Armed", AnimConditionOp::IsTrue, 0.0f } } };
        armedEntry.ToGroup = 0;
        d.Transitions = { enter, heavyEntry, slashExit, heavyExit, restFromInner, armedEntry };
        CHECK(AnimTransition::GroupOf(heavyEntry.From) == -1 && AnimTransition::IsEntry(heavyEntry.From) &&
              AnimTransition::EntryGroupOf(heavyEntry.From) == 0 && AnimTransition::EntryGroupOf(armedEntry.From) == -1);
        AnimatorRuntime r;
        auto stepD = [&](float dt) { return AnimatorStateMachine::Update(d, r, dt, duration, [](const AnimState&, float) {}); };
        stepD(0.1f);
        CHECK(r.State == 0);
        r.Triggers.insert("Fight");
        CHECK(stepD(0.1f) == 0 && r.State == 1); // no entry condition holds: Combat's default
        CHECK(stepD(1.0f) == 2 && r.State == 0); // Exit, nothing in Combat holds, top-level entry: Idle
        r.Parameters["Heavy"] = 1.0f;
        r.Triggers.insert("Fight");
        CHECK(stepD(0.1f) == 0 && r.State == 2); // Entry picks Inner, whose default is Heavy
        r.Parameters["Rest"] = 1.0f;
        CHECK(stepD(1.0f) == 3 && r.State == 3); // Heavy -> Exit (leaves Inner): Inner's transition to Rest holds
        r.Parameters["Rest"] = 0.0f;
        r.Parameters["Heavy"] = 0.0f;
        r.State = 2;
        r.StateTime = 0.0f;
        CHECK(stepD(1.0f) == 3 && r.State == 1); // without it: Combat's Entry (Heavy unset: its default, Slash)
        // Exiting to the top level goes through its Entry: Armed sends it straight back into Combat.
        r.Parameters["Rest"] = 0.0f;
        r.Parameters["Heavy"] = 0.0f;
        r.Parameters["Armed"] = 1.0f;
        r.State = 1;
        r.StateTime = 0.0f;
        CHECK(stepD(1.0f) == 2 && r.State == 1);
        // A fresh runtime starts through the top-level Entry too.
        AnimatorRuntime fresh;
        fresh.Parameters["Armed"] = 1.0f;
        AnimatorStateMachine::Update(d, fresh, 0.0f, duration, [](const AnimState&, float) {});
        CHECK(fresh.State == 1);
        fs::path entryPath = fs::temp_directory_path() / "dimension_entry_exit.controller";
        CHECK(d.Save(entryPath));
        auto loaded = AnimatorController::Load(entryPath);
        CHECK(loaded->Transitions[1].From == AnimTransition::FromEntry(0) && loaded->Transitions[2].ToExit && !loaded->Transitions[0].ToExit &&
              loaded->Transitions[5].From == AnimTransition::FromEntry(-1));
        fs::remove(entryPath);
        d.RemoveGroup(1); // its transitions go with it; Combat's stay
        CHECK(std::none_of(d.Transitions.begin(), d.Transitions.end(), [](const AnimTransition& t) { return t.ToGroup == 1 || AnimTransition::GroupOf(t.From) == 1; }));
        CHECK(std::count_if(d.Transitions.begin(), d.Transitions.end(), [](const AnimTransition& t) { return AnimTransition::IsEntry(t.From); }) == 1);
    }

    void TestBlendTrees2D()
    {
        // Idle in the middle, Forward/Right/Back/Left around it (a typical locomotion space).
        AnimatorController c;
        c.Parameters = { { "X", AnimParameterType::Float, 0.5f }, { "Y", AnimParameterType::Float, 0.0f } };
        AnimState tree;
        tree.Name = "Move";
        tree.Motion = AnimMotionType::BlendTree2D;
        tree.BlendParameter = "X";
        tree.BlendParameterY = "Y";
        tree.Children = { { "Idle", 0, 0 }, { "Forward", 0, 1 }, { "Right", 1, 0 }, { "Back", 0, -1 }, { "Left", -1, 0 } };
        c.States = { tree };
        std::vector<float> w;
        auto sum = [&] { float s = 0; for (float v : w) s += v; return s; };
        AnimatorStateMachine::BlendWeights2D(tree, { 0, 0 }, w);
        CHECK(Near(w[0], 1.0f) && Near(sum(), 1.0f)); // exactly on a child
        AnimatorStateMachine::BlendWeights2D(tree, { 1, 0 }, w);
        CHECK(Near(w[2], 1.0f) && Near(sum(), 1.0f));
        AnimatorStateMachine::BlendWeights2D(tree, { 0.5f, 0 }, w);
        CHECK(Near(w[0], 0.5f) && Near(w[2], 0.5f) && Near(w[1] + w[3] + w[4], 0.0f)); // halfway to Right
        AnimatorStateMachine::BlendWeights2D(tree, { 0.5f, 0.5f }, w);
        CHECK(Near(w[0], 1.0f / 3.0f) && Near(w[1], 1.0f / 3.0f) && Near(w[2], 1.0f / 3.0f) && Near(w[3] + w[4], 0.0f)); // diagonal
        AnimatorStateMachine::BlendWeights2D(tree, { 5, 0 }, w);
        CHECK(Near(w[2], 1.0f)); // past the edge: the outermost child
        bool valid = true;
        for (float x = -1.5f; x <= 1.5f; x += 0.25f)
            for (float y = -1.5f; y <= 1.5f; y += 0.25f)
            {
                AnimatorStateMachine::BlendWeights2D(tree, { x, y }, w);
                valid &= Near(sum(), 1.0f) && *std::min_element(w.begin(), w.end()) >= 0.0f;
            }
        CHECK(valid);

        // The runtime reads both parameters; durations mix by weight (Idle 2 s, Right 1 s at X = 0.5).
        auto duration = [](const std::string& clip) { return clip == "Idle" ? 2.0f : 1.0f; };
        AnimatorRuntime rt;
        AnimatorStateMachine::TreeWeights(c, rt, tree, w);
        CHECK(Near(w[0], 0.5f) && Near(w[2], 0.5f));
        CHECK(Near(AnimatorStateMachine::StateDuration(c, rt, tree, duration), 1.5f));
        AnimatorStateMachine::Update(c, rt, 0.0f, duration, [](const AnimState&, float) {});
        AnimatorStateMachine::Update(c, rt, 0.75f, duration, [](const AnimState&, float) {});
        CHECK(Near(AnimatorStateMachine::NormalizedTime(c, rt, duration), 0.5f)); // normalized time, like 1D trees

        // Save/load keeps the Y parameter and positions; 1D trees keep their old format.
        fs::path file = fs::temp_directory_path() / "dimension_blend2d.controller";
        CHECK(c.Save(file));
        auto back = AnimatorController::Load(file);
        CHECK(back && back->States.size() == 1 && back->States[0].Motion == AnimMotionType::BlendTree2D && back->States[0].BlendParameterY == "Y");
        CHECK(back && back->States[0].Children.size() == 5 && Near(back->States[0].Children[3].ThresholdY, -1.0f) &&
              back->States[0].Children[3].Clip == "Back");
        std::error_code ec;
        fs::remove(file, ec);
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

    // Counts its own updates: proves a persistent object keeps its script instance across scene loads.
    class PersistProbe : public ScriptableEntity
    {
    public:
        static inline int Created = 0, Destroyed = 0;
        int Updates = 0;
        void OnCreate() override { ++Created; }
        void OnDestroy() override { ++Destroyed; }
        void OnUpdate(float) override { ++Updates; }
    };

    void TestMultiScene()
    {
        fs::path assets = fs::temp_directory_path() / "dimension_multiscene_test" / "Assets";
        fs::remove_all(assets.parent_path());
        fs::create_directories(assets);
        {
            Scene level("Level");
            level.GetSettings().AmbientIntensity = 0.9f;
            Entity root = level.CreateEntity("Level Root");
            Entity crate = level.CreateEntity("Crate", root);
            auto& reg = level.GetRegistry();
            reg.Get<TransformComponent>(crate).Position = { 0, 5, 0 };
            reg.Emplace<RigidbodyComponent>(crate);
            reg.Emplace<BoxColliderComponent>(crate);
            reg.Emplace<ScriptComponent>(crate).Add("PersistProbe");
            level.CreateEntity("Level Lamp");
            CHECK(SceneSerializer::SaveToFile(level, assets / "Level.scene"));
            Scene overlay("Overlay");
            overlay.CreateEntity("Overlay Root");
            CHECK(SceneSerializer::SaveToFile(overlay, assets / "Overlay.scene"));
        }
        AssetManager::Init(assets);

        Scene hub("Hub");
        auto& reg = hub.GetRegistry();
        Entity manager = hub.CreateEntity("Manager");
        Entity managerChild = hub.CreateEntity("Manager Child", manager);
        reg.Emplace<ScriptComponent>(manager).Add("PersistProbe");
        hub.CreateEntity("Hub Prop");
        hub.UpdateTransforms();
        PersistProbe::Created = PersistProbe::Destroyed = 0;
        hub.OnRuntimeStart();
        auto probe = [&](Entity e) { return std::static_pointer_cast<PersistProbe>(reg.Get<ScriptComponent>(e).Scripts[0].Instance); };
        for (int i = 0; i < 10; ++i) hub.OnUpdateRuntime(1.0f / 60.0f);
        auto managerScript = probe(manager);
        CHECK(managerScript && managerScript->Updates == 10);
        CHECK(hub.GetLoadedScenes().size() == 1 && hub.GetLoadedScenes()[0].Name == "Hub");

        // A child marks its root; the root moves to the DontDestroyOnLoad entry.
        hub.DontDestroyOnLoad(managerChild);
        CHECK(hub.IsPersistent(manager) && hub.IsPersistent(managerChild) && !hub.IsPersistent(hub.FindEntityByName("Hub Prop")));
        CHECK(hub.GetLoadedScenes().size() == 2 && hub.GetLoadedScenes()[1].Name == Scene::kPersistentSceneName);

        // Single load: the prop goes, the manager (same script instance, same state) stays; name and settings change.
        SceneManager::LoadScene("Level");
        CHECK(SceneManager::ApplyPending(hub) && !SceneManager::HasPendingLoad());
        CHECK(hub.GetName() == "Level" && Near(hub.GetSettings().AmbientIntensity, 0.9f));
        CHECK(hub.FindEntityByName("Hub Prop").IsNull() && hub.IsValid(manager) && hub.IsValid(managerChild));
        CHECK(probe(manager) == managerScript && PersistProbe::Destroyed == 0 && PersistProbe::Created == 2);
        Entity crate = hub.FindEntityByName("Crate");
        CHECK(!crate.IsNull() && hub.GetLoadedScenes()[0].Name == "Level" && hub.GetLoadedScenes()[0].Roots.size() == 2);
        // Loaded objects run: the crate has a physics body and falls; the manager keeps counting.
        for (int i = 0; i < 30; ++i) hub.OnUpdateRuntime(1.0f / 60.0f);
        CHECK(reg.Get<TransformComponent>(crate).Position.y < 4.9f);
        CHECK(managerScript->Updates == 40 && probe(crate)->Updates == 30);

        // Additive load twice: the second copy gets fresh ids (no Guid clash), both are tracked.
        SceneManager::LoadScene("Overlay", LoadSceneMode::Additive);
        SceneManager::LoadScene("Overlay", LoadSceneMode::Additive);
        CHECK(SceneManager::ApplyPending(hub));
        size_t overlays = 0;
        for (Entity r : hub.GetRootEntities())
            if (reg.Get<TagComponent>(r).Name == "Overlay Root") ++overlays;
        CHECK(overlays == 2 && hub.GetName() == "Level" && hub.GetLoadedScenes().size() == 4);

        // Unload removes one scene's objects only.
        SceneManager::UnloadScene("Level");
        CHECK(SceneManager::ApplyPending(hub));
        CHECK(hub.FindEntityByName("Crate").IsNull() && hub.FindEntityByName("Level Lamp").IsNull() && PersistProbe::Destroyed == 1);
        CHECK(!hub.FindEntityByName("Overlay Root").IsNull() && hub.IsValid(manager));
        SceneManager::UnloadScene("Nope");
        CHECK(!SceneManager::ApplyPending(hub));

        // A queued single load supersedes earlier requests.
        SceneManager::LoadScene("Overlay", LoadSceneMode::Additive);
        SceneManager::LoadScene("Level");
        CHECK(SceneManager::ApplyPending(hub));
        CHECK(hub.FindEntityByName("Overlay Root").IsNull() && !hub.FindEntityByName("Crate").IsNull() && hub.IsValid(manager));
        hub.OnRuntimeStop();
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path());
    }

    class BreakCounter : public ScriptableEntity
    {
    public:
        static inline int Breaks = 0;
        static inline float LastForce = 0.0f;
        void OnJointBreak(float force) override
        {
            ++Breaks;
            LastForce = force;
        }
    };

    void TestJointMotorsAndPlatforms()
    {
        auto angleBetween = [](const glm::quat& a, const glm::quat& b) { return glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(a, b))))); };

        // ---- 3D: motorized hinge, breakable joints
        {
            Scene scene("Joints3D");
            auto& reg = scene.GetRegistry();
            Entity wheel = scene.CreateEntity("Wheel");
            reg.Emplace<RigidbodyComponent>(wheel);
            reg.Emplace<BoxColliderComponent>(wheel);
            auto& hinge = reg.Emplace<JointComponent>(wheel);
            hinge.Type = JointType::Hinge;
            hinge.Axis = { 0, 1, 0 };
            hinge.UseMotor = true;
            hinge.MotorSpeed = 90.0f;

            auto hanging = [&](const char* name, float x, float breakForce) {
                Entity e = scene.CreateEntity(name);
                reg.Get<TransformComponent>(e).Position = { x, 5, 0 };
                reg.Emplace<RigidbodyComponent>(e).Mass = 1.0f;
                reg.Emplace<BoxColliderComponent>(e);
                auto& j = reg.Emplace<JointComponent>(e);
                j.Type = JointType::Point;
                j.Anchor = { 0, 0.5f, 0 };
                j.BreakForce = breakForce;
                reg.Emplace<ScriptComponent>(e).Add("BreakCounter");
                return e;
            };
            Entity weak = hanging("Weak", 5, 5.0f);   // weight 9.81 N > 5 N
            Entity strong = hanging("Strong", -5, 50.0f);
            scene.UpdateTransforms();
            BreakCounter::Breaks = 0;
            scene.OnRuntimeStart();
            for (int i = 0; i < 30; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            glm::quat q0 = reg.Get<TransformComponent>(wheel).GetRotationQuat();
            for (int i = 0; i < 30; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            glm::quat q1 = reg.Get<TransformComponent>(wheel).GetRotationQuat();
            // Positive speed turns the entity counter-clockwise around its axis (right-hand rule), seen from +Y.
            glm::vec3 x0 = q0 * glm::vec3(1, 0, 0), x1 = q1 * glm::vec3(1, 0, 0);
            float signedYaw = glm::degrees(std::atan2(glm::cross(x0, x1).y, glm::dot(x0, x1)));
            CHECK(Near(signedYaw, 45.0f, 4.0f)); // 90 deg/s for 0.5 s
            CHECK(Near(reg.Get<TransformComponent>(wheel).Position.y, 0.0f, 0.05f)); // held by the hinge

            auto& wj = reg.Get<JointComponent>(weak);
            auto& sj = reg.Get<JointComponent>(strong);
            CHECK(wj.Broken && !sj.Broken && BreakCounter::Breaks == 1 && BreakCounter::LastForce > 5.0f);
            CHECK(Near(sj.CurrentForce, 9.81f, 1.5f));
            CHECK(reg.Get<TransformComponent>(weak).Position.y < 3.0f && Near(reg.Get<TransformComponent>(strong).Position.y, 5.0f, 0.1f));

            // Motors can be switched while playing.
            reg.Get<JointComponent>(wheel).MotorSpeed = 0.0f;
            for (int i = 0; i < 30; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            glm::quat q2 = reg.Get<TransformComponent>(wheel).GetRotationQuat();
            for (int i = 0; i < 30; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            CHECK(angleBetween(q2, reg.Get<TransformComponent>(wheel).GetRotationQuat()) < 2.0f);
            scene.OnRuntimeStop();
        }

        // ---- 2D: motorized hinge, breakable joint
        {
            Scene scene("Joints2D");
            auto& reg = scene.GetRegistry();
            Entity wheel = scene.CreateEntity("Wheel");
            reg.Emplace<Rigidbody2DComponent>(wheel).GravityScale = 0.0f;
            reg.Emplace<BoxCollider2DComponent>(wheel);
            auto& hinge = reg.Emplace<Joint2DComponent>(wheel);
            hinge.UseMotor = true;
            hinge.MotorSpeed = 180.0f;
            Entity weak = scene.CreateEntity("Weak");
            reg.Get<TransformComponent>(weak).Position = { 5, 5, 0 };
            reg.Emplace<Rigidbody2DComponent>(weak);
            reg.Emplace<BoxCollider2DComponent>(weak); // 1 x 1, density 1: about 1 kg
            auto& wj = reg.Emplace<Joint2DComponent>(weak);
            wj.Anchor = { 0, 0.5f };
            wj.BreakForce = 5.0f;
            reg.Emplace<ScriptComponent>(weak).Add("BreakCounter");
            scene.UpdateTransforms();
            BreakCounter::Breaks = 0;
            scene.OnRuntimeStart();
            for (int i = 0; i < 30; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            float a0 = reg.Get<TransformComponent>(wheel).Rotation.z;
            for (int i = 0; i < 15; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            float delta = glm::degrees(reg.Get<TransformComponent>(wheel).Rotation.z - a0);
            delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f; // wrap to [-180, 180)
            CHECK(Near(delta, 45.0f, 4.0f)); // 180 deg/s for 0.25 s
            CHECK(reg.Get<Joint2DComponent>(weak).Broken && BreakCounter::Breaks == 1);
            CHECK(reg.Get<TransformComponent>(weak).Position.y < 3.5f);
            scene.OnRuntimeStop();
        }

        // ---- Characters ride moving (kinematic) platforms
        for (bool ride : { true, false })
        {
            Scene scene("Platform");
            auto& reg = scene.GetRegistry();
            Entity platform = scene.CreateEntity("Platform");
            reg.Emplace<RigidbodyComponent>(platform).Type = BodyType::Kinematic;
            reg.Emplace<BoxColliderComponent>(platform).Size = { 6, 0.5f, 6 };
            Entity hero = scene.CreateEntity("Hero");
            reg.Get<TransformComponent>(hero).Position = { 0, 1.3f, 0 };
            reg.Emplace<CharacterControllerComponent>(hero).RideMovingPlatforms = ride;
            reg.Emplace<ScriptComponent>(hero).Add("CharacterProbe");
            scene.UpdateTransforms();
            scene.OnRuntimeStart();
            for (int i = 0; i < 30; ++i) scene.OnUpdateRuntime(1.0f / 60.0f); // settle on the platform
            float x0 = reg.Get<TransformComponent>(hero).Position.x;
            for (int i = 0; i < 60; ++i)
            {
                reg.Get<TransformComponent>(platform).Position.x += 2.0f / 60.0f; // 2 m/s
                scene.OnUpdateRuntime(1.0f / 60.0f);
            }
            float moved = reg.Get<TransformComponent>(hero).Position.x - x0;
            if (ride) CHECK(Near(moved, 2.0f, 0.3f) && reg.Get<CharacterControllerComponent>(hero).IsGrounded);
            else CHECK(std::abs(moved) < 0.3f);
            scene.OnRuntimeStop();
        }
    }

    void TestParticleDecals()
    {
        auto run = [&](int maxDecals, float decalLifetime, float seconds) {
            Scene scene("Decals");
            Entity e = scene.CreateEntity("Paint");
            scene.GetRegistry().Get<TransformComponent>(e).Position = { 0, 3, 0 };
            auto& ps = scene.GetRegistry().Emplace<ParticleSystemComponent>(e);
            ps.Shape = ParticleShape::Sphere;
            ps.ShapeRadius = 0.5f;
            ps.StartSpeed = { 0.0f, 0.0f };
            ps.StartLifetime = { 10.0f, 10.0f };
            ps.GravityModifier = 1.0f;
            ps.RateOverTime = 0.0f;
            ps.BurstCount = 50;
            ps.Looping = false;
            ps.Duration = 100.0f;
            ps.Collision = ParticleCollision::Plane;
            ps.CollisionPlaneHeight = 0.0f;
            ps.CollisionRadius = 0.05f;
            ps.Bounce = 0.5f; // they bounce, but mark only once
            ps.CollisionDecals = true;
            ps.MaxDecals = maxDecals;
            ps.DecalLifetime = decalLifetime;
            scene.UpdateTransforms();
            ParticleSystems::Restart(scene);
            for (int i = 0; i < (int)(seconds * 60.0f); ++i) ParticleSystems::Update(scene, 1.0f / 60.0f);
            return scene.GetRegistry().Get<ParticleSystemComponent>(e).Decals;
        };
        auto marks = run(128, 30.0f, 2.0f);
        bool onGround = true;
        for (auto& m : marks) onGround &= Near(m.Position.y, 0.0f, 1e-3f) && Near(m.Normal.y, 1.0f, 1e-4f) && m.Size > 0.2f;
        CHECK(marks.size() == 50 && onGround);
        CHECK(run(20, 30.0f, 2.0f).size() == 20);   // at most Max Decals
        CHECK(run(128, 1.0f, 4.0f).empty());         // gone after their lifetime
    }

    void TestParticleMeshShape()
    {
        // The Mesh shape: spawn points on the surface by area (each cube face gets a sixth), particles starting on a
        // sphere's surface and leaving along its normals; without a mesh nothing is sampled.
        fs::path assets = fs::temp_directory_path() / "dimension_particle_mesh" / "Assets";
        std::error_code ec;
        fs::remove_all(assets.parent_path(), ec);
        fs::create_directories(assets);
        AssetManager::Init(assets);
        const auto* cube = ParticleSystems::MeshSurfaceSamples(AssetManager::kCube);
        CHECK(cube && cube->size() == (size_t)ParticleSystems::kMeshSamples * 2);
        int faces[6] = {};
        bool onSurface = true;
        for (size_t i = 0; cube && i < cube->size(); i += 2)
        {
            glm::vec3 p((*cube)[i]), n((*cube)[i + 1]);
            int axis = std::abs(n.x) > 0.9f ? 0 : std::abs(n.y) > 0.9f ? 1 : 2;
            onSurface &= Near(std::abs(p[axis]), 0.5f, 1e-4f) && Near(p[axis] * n[axis], 0.5f, 1e-3f);
            faces[axis * 2 + (n[axis] > 0.0f)]++;
        }
        bool even = true;
        for (int f : faces) even &= std::abs(f - ParticleSystems::kMeshSamples / 6) < ParticleSystems::kMeshSamples / 24;
        CHECK(onSurface && even && ParticleSystems::MeshSurfaceSamples(AssetHandle::None()) == nullptr);

        Scene scene("MeshShape");
        Entity e = scene.CreateEntity("Emitter");
        scene.GetRegistry().Get<TransformComponent>(e).Position = { 0, 2, 0 };
        scene.GetRegistry().Emplace<MeshRendererComponent>(e).Mesh = AssetManager::kSphere; // the shape's mesh (None in Shape Mesh)
        auto& ps = scene.GetRegistry().Emplace<ParticleSystemComponent>(e);
        ps.Shape = ParticleShape::Mesh;
        ps.RateOverTime = 0.0f;
        ps.BurstCount = 300;
        ps.StartSpeed = { 1.0f, 1.0f };
        ps.MaxParticles = 1000;
        scene.UpdateTransforms();
        ParticleSystems::Restart(scene);
        ParticleSystems::Update(scene, 1e-4f);
        bool fromSurface = !ps.Particles.empty(), outward = true;
        for (const Particle& p : ps.Particles)
        {
            glm::vec3 rel = p.Position - glm::vec3(0, 2, 0);
            fromSurface &= Near(glm::length(rel), 0.5f, 0.02f);
            outward &= glm::dot(glm::normalize(rel), glm::normalize(p.Velocity)) > 0.95f;
        }
        CHECK(ps.Particles.size() >= 250 && fromSurface && outward);
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path(), ec);
    }

    void TestParticleSheetsAndNoise()
    {
        // Flipbook: 4 x 2 sheet, 6 frames used, one cycle over the lifetime.
        ParticleSystemComponent ps;
        ps.TextureSheet = true;
        ps.SheetColumns = 4;
        ps.SheetRows = 2;
        ps.SheetFrames = 6;
        Particle p{};
        p.Lifetime = 2.0f;
        p.Age = 0.0f;
        CHECK(ps.SheetFrame(p) == 0);
        p.Age = 1.0f;
        CHECK(ps.SheetFrame(p) == 3);
        p.Age = 1.99f;
        CHECK(ps.SheetFrame(p) == 5);
        ps.SheetCycles = 2.0f; // twice per life
        p.Age = 1.0f;
        CHECK(ps.SheetFrame(p) == 0);
        p.SheetOffset = 0.5f;  // random start shifts the phase
        CHECK(ps.SheetFrame(p) == 3);
        glm::vec2 mn, mx;
        ps.SheetUV(5, mn, mx); // column 1, row 1 (bottom row)
        CHECK(Near(mn.x, 0.25f) && Near(mx.x, 0.5f) && Near(mn.y, 1.0f) && Near(mx.y, 0.5f));
        ps.TextureSheet = false;
        ps.SheetUV(ps.SheetFrame(p), mn, mx);
        CHECK(Near(mn.x, 0.0f) && Near(mx.x, 1.0f) && Near(mn.y, 1.0f) && Near(mx.y, 0.0f));

        // Noise: bounded, smooth, and its curl is (numerically) divergence-free.
        float lo = 1e9f, hi = -1e9f, maxStep = 0.0f;
        for (int i = 0; i < 2000; ++i)
        {
            glm::vec3 q(i * 0.173f, std::sin(i * 0.37f) * 9.0f, i * -0.061f);
            float v = ParticleSystems::GradientNoise(q);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            maxStep = std::max(maxStep, std::abs(ParticleSystems::GradientNoise(q + glm::vec3(0.01f, 0, 0)) - v));
        }
        CHECK(lo > -1.5f && hi < 1.5f && hi - lo > 0.8f && maxStep < 0.05f);
        float divergence = 0.0f, magnitude = 0.0f;
        for (int i = 0; i < 50; ++i)
        {
            glm::vec3 q(i * 0.71f, i * 0.33f, -i * 0.57f);
            const float e = 0.01f;
            float div = (ParticleSystems::CurlNoise(q + glm::vec3(e, 0, 0)).x - ParticleSystems::CurlNoise(q - glm::vec3(e, 0, 0)).x +
                         ParticleSystems::CurlNoise(q + glm::vec3(0, e, 0)).y - ParticleSystems::CurlNoise(q - glm::vec3(0, e, 0)).y +
                         ParticleSystems::CurlNoise(q + glm::vec3(0, 0, e)).z - ParticleSystems::CurlNoise(q - glm::vec3(0, 0, e)).z) / (2 * e);
            divergence += std::abs(div);
            magnitude += glm::length(ParticleSystems::CurlNoise(q));
        }
        CHECK(magnitude / 50 > 0.3f && divergence / 50 < 0.05f); // measured: about 0.84 and 0.013

        // Simulation: motionless particles only move when Noise is on.
        for (bool noise : { false, true })
        {
            Scene scene("Noise");
            auto& reg = scene.GetRegistry();
            Entity e = scene.CreateEntity("Emitter");
            auto& sys = reg.Emplace<ParticleSystemComponent>(e);
            sys.RateOverTime = 0.0f;
            sys.BurstCount = 50;
            sys.StartSpeed = { 0.0f, 0.0f };
            sys.StartLifetime = { 10.0f, 10.0f };
            sys.Shape = ParticleShape::Box;
            sys.BoxSize = glm::vec3(4.0f);
            sys.Noise = noise;
            sys.NoiseStrength = 2.0f;
            scene.UpdateTransforms();
            ParticleSystems::Update(scene, 1.0f / 60.0f);
            std::vector<glm::vec3> start;
            for (auto& q : sys.Particles) start.push_back(q.Position);
            for (int i = 0; i < 60; ++i) ParticleSystems::Update(scene, 1.0f / 60.0f);
            float moved = 0.0f;
            for (size_t i = 0; i < start.size(); ++i) moved += glm::distance(start[i], sys.Particles[i].Position);
            moved /= std::max<size_t>(1, start.size());
            if (noise) CHECK(moved > 0.2f && moved < 2.5f); // about strength * |curl| * 1 s
            else CHECK(moved < 1e-5f);
        }
    }

    void TestUITextEditing()
    {
        UI::SetTextMeasure([](const std::string& t, float) { return 10.0f * (float)UI::CodepointCount(t); }); // 10 px per character
        Scene scene("TextEditing");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        reg.Emplace<CanvasComponent>(canvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
        auto field = [&](const char* name, float y) {
            Entity e = scene.CreateEntity(name, canvas);
            auto& r = reg.Emplace<UIRectComponent>(e);
            r.AnchorMin = r.AnchorMax = { 0, 0 };
            r.Pivot = { 0, 0 };
            r.Position = { 0, y };
            r.Size = { 300, 40 }; // text starts at x = 10 (padding); 28 characters fit
            reg.Emplace<UIInputFieldComponent>(e).FontSize = 20.0f;
            return e;
        };
        Entity a = field("A", 0), b = field("B", 50);
        scene.UpdateTransforms();
        glm::vec2 screen(1000, 1000);
        std::string clipboard;
        glm::vec2 mouse(0.0f);
        bool down = false, wasDown = false;
        auto frame = [&](auto&& setup) {
            UI::PointerState p;
            p.Position = mouse;
            p.Down = down;
            p.Pressed = down && !wasDown;
            p.Released = !down && wasDown;
            p.SetClipboard = [&](const std::string& t) { clipboard = t; };
            setup(p);
            UI::Update(scene, p, screen, 0.016f);
            wasDown = down;
        };
        auto none = [](UI::PointerState&) {};
        auto& fa = reg.Get<UIInputFieldComponent>(a);
        auto& fb = reg.Get<UIInputFieldComponent>(b);

        // Click to focus, type, move the caret and insert in the middle.
        mouse = { 45, 20 };
        down = true;
        frame(none);
        down = false;
        frame(none);
        CHECK(fa.Focused && !fb.Focused && fa.Caret == 0);
        frame([](auto& p) { p.Text = "hello world"; });
        frame([](auto& p) { p.Left = true; });
        frame([](auto& p) { p.Left = true; });
        frame([](auto& p) { p.Text = "X"; });
        CHECK(fa.Text == "hello worXld" && fa.Caret == 10);

        // Home, Shift+End selects everything; typing replaces the selection.
        frame([](auto& p) { p.Home = true; });
        frame([](auto& p) { p.End = true; p.Shift = true; });
        CHECK(fa.SelectionAnchor == 0 && fa.Caret == 12);
        frame([](auto& p) { p.Text = "abc def"; });
        CHECK(fa.Text == "abc def" && fa.SelectionAnchor == -1);
        frame([](auto& p) { p.Backspace = true; p.Ctrl = true; }); // word-wise
        CHECK(fa.Text == "abc " && fa.Caret == 4);

        // Clipboard: copy all, paste (newlines dropped) over the selection, cut part of it.
        frame([](auto& p) { p.SelectAll = true; p.Copy = true; });
        CHECK(clipboard == "abc ");
        frame([](auto& p) { p.Paste = true; p.PasteText = "x\ny"; });
        CHECK(fa.Text == "xy" && fa.Caret == 2);
        // Mouse: click after 'x' (x = 10 + 10), drag to after 'y', cut.
        mouse = { 21, 20 };
        down = true;
        frame(none);
        CHECK(fa.Caret == 1 && fa.SelectionAnchor == 1);
        mouse = { 31, 20 };
        frame(none);
        down = false;
        frame([](auto& p) { p.Cut = true; });
        CHECK(clipboard == "y" && fa.Text == "x" && fa.Caret == 1);
        frame([](auto& p) { p.Home = true; });
        frame([](auto& p) { p.Delete = true; });
        CHECK(fa.Text.empty());

        // Long text scrolls to keep the caret visible; Home scrolls back.
        frame([](auto& p) { p.Text = std::string(40, 'm'); });
        CHECK(Near(fa.ScrollX, 120.0f)); // 400 px of text, 280 px visible
        frame([](auto& p) { p.Home = true; });
        CHECK(Near(fa.ScrollX, 0.0f));

        // Tab moves to the next field and selects its text; the character limit applies to pastes.
        fb.Text = "keep";
        fb.CharacterLimit = 6;
        frame([](auto& p) { p.Tab = true; });
        CHECK(!fa.Focused && fb.Focused && fb.SelectionAnchor == 0 && fb.Caret == 4);
        frame([](auto& p) { p.End = true; });
        frame([](auto& p) { p.Paste = true; p.PasteText = "abcdef"; });
        CHECK(fb.Text == "keepab");
        frame([](auto& p) { p.Tab = true; p.Shift = true; }); // back to A (wraps)
        CHECK(fa.Focused && !fb.Focused);
        UI::SetTextMeasure(nullptr);
    }

    void TestAnimatorLayers()
    {
        // Real skeleton and clips (CPU only): Tentacle = Bone0 -> Bone1 -> ... -> Bone5, clips Wave/Sway/Curl.
        MeshData data;
        CHECK(MeshData::LoadFromFile(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models/Tentacle.gltf", data));
        const Skeleton& sk = *data.SkeletonData;
        int b2 = sk.FindJoint("Bone2"), b3 = sk.FindJoint("Bone3"), b5 = sk.FindJoint("Bone5");
        CHECK(b2 >= 0 && b3 >= 0 && b5 >= 0);

        // Mask: Bone3 and everything below it.
        std::vector<float> mask;
        Animation::BuildMask(sk, { "Bone3" }, mask);
        CHECK(mask[b3] == 1.0f && mask[b5] == 1.0f && mask[b2] == 0.0f && mask[sk.FindJoint("Bone0")] == 0.0f);
        Animation::BuildMask(sk, {}, mask);
        CHECK(std::all_of(mask.begin(), mask.end(), [](float w) { return w == 1.0f; }));

        // Override: masked joints follow the layer (fully at weight 1, halfway at 0.5); the rest keep the base.
        const AnimationClip* sway = Animation::FindClip(data.Clips, "Sway");
        const AnimationClip* wave = Animation::FindClip(data.Clips, "Wave");
        CHECK(sway && wave);
        Pose base, layer, result;
        Animation::Sample(sk, *sway, 0.3f * sway->Duration, base);
        Animation::Sample(sk, *wave, 0.3f * wave->Duration, layer);
        Animation::BuildMask(sk, { "Bone3" }, mask);
        result = base;
        Animation::LayerOverride(result, layer, mask, 1.0f);
        auto angle = [](const glm::quat& a, const glm::quat& b) { return glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(a, b))))); };
        CHECK(angle(result.Rotations[b5], layer.Rotations[b5]) < 0.01f && angle(result.Rotations[b2], base.Rotations[b2]) < 0.01f);
        CHECK(angle(base.Rotations[b5], layer.Rotations[b5]) > 1.0f); // the clips really differ there
        result = base;
        Animation::LayerOverride(result, layer, mask, 0.5f);
        CHECK(Near(angle(result.Rotations[b5], base.Rotations[b5]), angle(base.Rotations[b5], layer.Rotations[b5]) * 0.5f, 0.1f));

        // Additive: the layer's change from its reference frame is added on top of the base.
        Pose bindPose, turned, reference;
        bindPose.SetBindPose(sk);
        reference = bindPose;
        turned = bindPose;
        glm::quat bend = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 0, 1));
        turned.Rotations[b3] = turned.Rotations[b3] * bend;
        result = base;
        Animation::LayerAdditive(result, turned, reference, mask, 0.5f);
        CHECK(Near(angle(result.Rotations[b3], base.Rotations[b3]), 20.0f, 0.1f)); // half of the 40 degree bend
        CHECK(angle(result.Rotations[b2], base.Rotations[b2]) < 0.01f);          // outside the mask

        // Layer machines read the controller's parameters; layers survive save/load.
        AnimatorController c;
        c.Parameters = { { "Waving", AnimParameterType::Bool, 0.0f } };
        AnimState idle;
        idle.Name = "Idle";
        idle.Clip = "Sway";
        c.States = { idle };
        AnimLayer arm;
        arm.Name = "Arm";
        arm.Weight = 0.75f;
        arm.Blending = AnimLayerBlending::Additive;
        arm.Mask = { "Bone3" };
        AnimState rest, waving;
        rest.Name = "Rest";
        rest.Clip = "Sway";
        waving.Name = "Waving";
        waving.Clip = "Wave";
        arm.Machine.States = { rest, waving };
        arm.Machine.Transitions = { AnimTransition{ 0, 1, false, 1.0f, 0.2f, { { "Waving", AnimConditionOp::IsTrue, 0.0f } } } };
        c.Layers.push_back(arm);
        c.LinkLayers();
        CHECK(c.Layers[0].Machine.FindParameter("Waving") != nullptr);
        AnimatorRuntime rt;
        auto duration = [](const std::string&) { return 1.0f; };
        std::string entered;
        auto enter = [&](const AnimState& st, float) { entered = st.Name; };
        AnimatorStateMachine::Update(c.Layers[0].Machine, rt, 0.1f, duration, enter);
        CHECK(entered == "Rest");
        rt.Parameters["Waving"] = 1.0f;
        AnimatorStateMachine::Update(c.Layers[0].Machine, rt, 0.1f, duration, enter);
        CHECK(entered == "Waving");

        fs::path path = fs::temp_directory_path() / "dimension_layers.controller";
        CHECK(c.Save(path));
        auto loaded = AnimatorController::Load(path);
        CHECK(loaded->Layers.size() == 1 && loaded->Layers[0].Name == "Arm" && Near(loaded->Layers[0].Weight, 0.75f));
        CHECK(loaded->Layers[0].Blending == AnimLayerBlending::Additive && loaded->Layers[0].Mask == std::vector<std::string>({ "Bone3" }));
        CHECK(loaded->Layers[0].Machine.States.size() == 2 && loaded->Layers[0].Machine.Transitions.size() == 1);
        CHECK(loaded->Layers[0].Machine.FindParameter("Waving") != nullptr); // linked on load
        fs::remove(path);

        // Script-side weight override.
        AnimatorComponent anim;
        anim.SetLayerWeight(1, 0.25f);
        CHECK(anim.Layers.size() == 1 && Near(anim.Layers[0].Weight, 0.25f));
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

    class CoroutineProbe : public ScriptableEntity
    {
    public:
        static inline CoroutineProbe* Instance = nullptr;
        std::vector<std::string> Steps;
        bool Flag = false;
        int Ticks = 0, MainId = 0, ForeverId = 0;

        Coroutine Inner()
        {
            Steps.push_back("inner start");
            co_await WaitForSeconds(0.25f);
            Steps.push_back("inner end");
        }
        Coroutine Main()
        {
            Steps.push_back("start");
            co_await NextFrame();
            Steps.push_back("next frame");
            co_await WaitForSeconds(0.5f);
            Steps.push_back("after 0.5");
            co_await Inner();
            Steps.push_back("after inner");
            co_await WaitUntil([this] { return Flag; });
            Steps.push_back("flag");
        }
        Coroutine Forever()
        {
            for (;;)
            {
                ++Ticks;
                co_await NextFrame();
            }
        }
        Coroutine Immediate() { Steps.push_back("immediate"); co_return; }
        void OnCreate() override
        {
            Instance = this;
            MainId = StartCoroutine(Main());
            ForeverId = StartCoroutine(Forever());
            int done = StartCoroutine(Immediate());
            if (IsCoroutineRunning(done)) Steps.push_back("immediate kept");
        }
    };

    void TestScriptOptions()
    {
        // C++ scripts get the Add Component menu and required components through ScriptOptions.
        ScriptRegistry::Register<ClickCounter>("Menu Probe", { .Menu = "Gameplay/Probe", .Requires = { "Rigidbody", "Sphere Collider" } });
        const ScriptInfo* info = ScriptRegistry::Find("Menu Probe");
        CHECK(info && info->Menu == "Gameplay/Probe" && info->Requires == std::vector<std::string>({ "Rigidbody", "Sphere Collider" }) && info->Module == "native");
        const ScriptInfo* plain = ScriptRegistry::Find("ClickCounter");
        CHECK(plain && plain->Menu.empty() && plain->Requires.empty());
        for (auto& name : info->Requires) CHECK(ComponentRegistry::Find(name) != nullptr); // real component names
    }

    void TestCoroutines()
    {
        Scene scene("Coroutines");
        Entity e = scene.CreateEntity("Probe");
        scene.GetRegistry().Emplace<ScriptComponent>(e).Add("CoroutineProbe");
        CoroutineProbe::Instance = nullptr;
        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        const float dt = 0.0625f; // exact in binary (and under the 0.1 s step cap): waits end on predictable frames
        auto frames = [&](int n) { for (int i = 0; i < n; ++i) runtime->OnUpdateRuntime(dt); };
        frames(1);
        auto* p = CoroutineProbe::Instance;
        CHECK(p != nullptr);
        if (!p) return;
        using V = std::vector<std::string>;
        CHECK((p->Steps == V{ "start", "immediate" })); // runs at once until the first wait; a finished one is not kept
        frames(1);
        CHECK(p->Steps.size() == 3 && p->Steps[2] == "next frame");
        frames(7);
        CHECK(p->Steps.size() == 3); // 7/8 of the 0.5 s wait
        frames(1);
        CHECK(p->Steps.size() == 5 && p->Steps[3] == "after 0.5" && p->Steps[4] == "inner start"); // nested starts at once
        frames(4);
        CHECK(p->Steps.size() == 7 && p->Steps[5] == "inner end" && p->Steps[6] == "after inner"); // caller resumes with it
        frames(2);
        CHECK(p->Steps.size() == 7 && p->IsCoroutineRunning(p->MainId));
        p->Flag = true;
        frames(1);
        CHECK(p->Steps.size() == 8 && p->Steps[7] == "flag" && !p->IsCoroutineRunning(p->MainId));
        // A looping coroutine runs once per frame (from the frame after it started) until stopped.
        int ticks = p->Ticks;
        CHECK(ticks == 17); // the synchronous start plus frames 2..17
        p->StopCoroutine(p->ForeverId);
        frames(3);
        CHECK(p->Ticks == ticks && !p->IsCoroutineRunning(p->ForeverId));
        runtime->OnRuntimeStop(); // destroys the script, and with it any suspended coroutine frames
    }

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
            src.Clip = AssetManager::GetHandle("Audio/Music.ogg"); // streamed Ogg Vorbis
            src.Output = "Ambience";
            src.Stream = true;
            src.Loop = true;
            CHECK(src.Clip.IsValid());
            groups[3].Echo = groups[3].Reverb = true; // Ambience: both effects in its chain
            auto runtime = scene.Clone();
            runtime->OnRuntimeStart();
            for (int g = 0; g < 4; ++g) CHECK(AudioMixer::GetGroupNode(g) != nullptr);
            CHECK(AudioWorld::IsPlaying(*runtime, e));
            // Effects relink live; the music keeps reaching Master through each chain.
            for (int pass = 0; pass < 3; ++pass)
            {
                groups[3].Reverb = pass != 1;
                groups[3].Echo = pass != 2;
                AudioMixer::Apply();
                AudioMixer::GetLevel(0);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                CHECK(AudioMixer::GetLevel(0).Peak > 0.01f);
            }
            groups[3].Echo = groups[3].Reverb = false;
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

    void TestAudioSends()
    {
        auto& groups = Project::GetConfig().MixerGroups;
        auto saved = groups;
        // Master, Music, SFX, Ambience (child of Music), Return (shared reverb).
        groups = AudioMixer::DefaultGroups();
        AudioMixerGroup ambience, ret;
        ambience.Name = "Ambience";
        ambience.Parent = 1;
        ret.Name = "Return"; // no reverb here: its tail would blur the level checks
        groups.push_back(ambience);
        groups.push_back(ret);

        // Sends to the return group are kept; sends that would loop back (to itself, to a descendant, or through
        // another group's send) are dropped.
        groups[1].SendTarget = 4; // Music -> Return
        groups[2].SendTarget = 4; // SFX -> Return
        groups[3].SendTarget = 3; // itself
        auto sends = AudioMixer::ValidSends(groups);
        CHECK(sends[1] == 4 && sends[2] == 4 && sends[3] == -1 && sends[0] == -1 && sends[4] == -1);
        groups[1].SendTarget = 3; // Music -> its own child Ambience (Ambience feeds Music): a loop
        CHECK(AudioMixer::ValidSends(groups)[1] == -1);
        groups[1].SendTarget = 2; // Music -> SFX, then SFX -> Music: the second one closes a loop
        groups[2].SendTarget = 1;
        sends = AudioMixer::ValidSends(groups);
        CHECK(sends[1] == 2 && sends[2] == -1);
        groups[1].SendTarget = groups[2].SendTarget = groups[3].SendTarget = -1;

        // Project files keep sends.
        fs::path dir = fs::temp_directory_path() / "dimension_sends";
        std::error_code ec;
        fs::remove_all(dir, ec);
        auto mix = groups;
        CHECK(Project::Open(dir));
        AudioMixer::Groups() = mix;
        AudioMixer::Groups()[3].SendTarget = 4;
        AudioMixer::Groups()[3].SendLevelDb = -6.0f;
        CHECK(Project::Save() && Project::Open(dir));
        const auto& back = AudioMixer::Groups();
        CHECK(back.size() == 5 && back[3].SendTarget == 4 && Near(back[3].SendLevelDb, -6.0f) && back[1].SendTarget == -1);
        fs::remove_all(dir, ec);

        // With an audio device: music in Ambience reaches the Return group only through the send.
        auto& live = Project::GetConfig().MixerGroups;
        AudioEngine::Init();
        if (AudioEngine::IsAvailable())
        {
            AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
            Scene scene("Sends");
            Entity e = scene.CreateEntity("Speaker");
            auto& src = scene.GetRegistry().Emplace<AudioSourceComponent>(e);
            src.Clip = AssetManager::GetHandle("Audio/Music.ogg");
            src.Output = "Ambience";
            src.Stream = true;
            src.Loop = true;
            auto runtime = scene.Clone();
            runtime->OnRuntimeStart();
            auto level = [](int group) {
                AudioMixer::GetLevel(group);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return AudioMixer::GetLevel(group).Peak;
            };
            live[3].SendLevelDb = 0.0f;
            AudioMixer::Apply();
            float sent = level(4);
            live[3].SendLevelDb = AudioMixer::kSilentDb;
            AudioMixer::Apply();
            level(4); // audio already in flight
            float silent = level(4);
            live[3].SendTarget = -1;
            live[3].SendLevelDb = 0.0f;
            AudioMixer::Apply();
            float unlinked = level(4), ambienceLevel = level(3);
            CHECK(sent > 0.01f && silent < sent * 0.25f && unlinked < sent * 0.25f && ambienceLevel > 0.01f);
            std::printf("  return peak: sent %.3f, send at -80 dB %.4f, unlinked %.4f\n", sent, silent, unlinked);
            runtime->OnRuntimeStop();
            AssetManager::Shutdown();
        }
        else
            std::printf("  (no audio device: graph checks skipped)\n");
        AudioEngine::Shutdown();
        live = saved;
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

    // Drives its Character Controller each update (Move, or SimpleMove with gravity) and records what it hit.
    class CharacterHitProbe : public ScriptableEntity
    {
    public:
        glm::vec3 Motion{ 0.0f }; // per second
        bool Simple = true;
        int Flags = 0;
        std::vector<ControllerColliderHit> Hits;
        void OnUpdate(float dt) override
        {
            Hits.clear();
            if (Simple) Physics3DWorld::SimpleMoveCharacter(GetScene(), Self(), Motion);
            else Flags = Physics3DWorld::MoveCharacter(GetScene(), Self(), Motion * dt);
            if (Simple) Flags = Get<CharacterControllerComponent>().CollisionFlags;
        }
        void OnControllerColliderHit(const ControllerColliderHit& hit) override { Hits.push_back(hit); }
        bool Hit(Scene& scene, const char* name) const
        {
            return std::any_of(Hits.begin(), Hits.end(), [&](const ControllerColliderHit& h) {
                return scene.GetRegistry().Get<TagComponent>(h.Other).Name == name;
            });
        }
    };

    void TestCharacterSettingsAndHits()
    {
        Scene scene("Characters");
        auto& reg = scene.GetRegistry();
        auto box = [&](const char* name, glm::vec3 at, glm::vec3 size) {
            Entity e = scene.CreateEntity(name);
            reg.Get<TransformComponent>(e).Position = at;
            reg.Emplace<BoxColliderComponent>(e).Size = size;
            return e;
        };
        box("Floor", { 0, -0.5f, 0 }, { 40, 1, 40 });
        box("Wall", { 4, 2, 0 }, { 1, 4, 40 });              // its face at x = 3.5
        box("Ceiling", { -10, 3.0f, 0 }, { 6, 1, 40 });      // over x -13..-7, its underside at 2.5
        box("Step", { -4, 0.125f, 0 }, { 2, 0.25f, 40 });    // x -5..-3, 0.25 high
        Entity hero = scene.CreateEntity("Hero");
        reg.Get<TransformComponent>(hero).Position = { 0, 1.0f, 0 }; // capsule 2 high about its position: feet at 0
        auto& cc = reg.Emplace<CharacterControllerComponent>(hero);
        cc.StepOffset = 0.0f;
        reg.Emplace<ScriptComponent>(hero).Add("CharacterHitProbe");
        auto rt = scene.Clone();
        rt->OnRuntimeStart();
        auto& rr = rt->GetRegistry();
        auto* probe = static_cast<CharacterHitProbe*>(rr.Get<ScriptComponent>(hero).Scripts[0].Instance.get());
        auto pos = [&] { return rr.Get<TransformComponent>(hero).Position; };
        auto run = [&](int n) { for (int i = 0; i < n; ++i) rt->OnUpdateRuntime(1.0f / 60.0f); };
        auto setting = [&](auto&& change) { change(rr.Get<CharacterControllerComponent>(hero)); rt->GetPhysics3D()->RefreshCharacter(hero); };

        // Standing: the floor is below it, and a hit.
        run(30);
        CHECK(rr.Get<CharacterControllerComponent>(hero).IsGrounded && (probe->Flags & CharacterCollidedBelow) && probe->Hit(*rt, "Floor"));
        CHECK(Near(pos().y, 1.0f, 0.05f));
        // Into the wall: stopped at it, touching with its side; the hit says how it was moving.
        probe->Motion = { 4, 0, 0 };
        run(90);
        CHECK(Near(pos().x, 3.0f, 0.05f) && (probe->Flags & CharacterCollidedSides) && probe->Hit(*rt, "Wall"));
        auto wall = std::find_if(probe->Hits.begin(), probe->Hits.end(), [&](const ControllerColliderHit& h) { return rr.Get<TagComponent>(h.Other).Name == "Wall"; });
        CHECK(wall != probe->Hits.end() && wall->Normal.x < -0.9f && wall->MoveDirection.x > 0.9f && Near(wall->Point.x, 3.5f, 0.05f)); // (SimpleMove presses down a little too)
        // Wider (Radius 1, live): it stands further from the wall.
        setting([](auto& c) { c.Radius = 1.0f; });
        run(30);
        CHECK(Near(pos().x, 2.5f, 0.06f));
        setting([](auto& c) { c.Radius = 0.5f; });
        // Shorter (Height 1): it settles lower, feet on the floor.
        probe->Motion = { 0, 0, 0 };
        setting([](auto& c) { c.Height = 1.0f; });
        run(40);
        CHECK(Near(pos().y, 0.5f, 0.05f) && rr.Get<CharacterControllerComponent>(hero).IsGrounded);
        setting([](auto& c) { c.Height = 2.0f; });
        run(40);
        CHECK(Near(pos().y, 1.0f, 0.06f));
        // Under the ceiling, moving up (Move, no gravity): its top touches.
        rr.Get<TransformComponent>(hero).Position = { -10, 1.0f, 0 };
        probe->Simple = false;
        probe->Motion = { 0, 3, 0 };
        run(30);
        CHECK(Near(pos().y, 1.5f, 0.05f) && (probe->Flags & CharacterCollidedAbove) && probe->Hit(*rt, "Ceiling"));
        // The step: Step Offset 0 stops it at the step; 0.3 (live) walks up it.
        rr.Get<TransformComponent>(hero).Position = { -1, 1.0f, 0 };
        probe->Simple = true;
        probe->Motion = { -3, 0, 0 };
        run(60);
        CHECK(pos().x > -2.6f && pos().y < 1.1f && probe->Hit(*rt, "Step"));
        setting([](auto& c) { c.StepOffset = 0.3f; });
        run(25); // (on top of it; it walks down its far side later)
        CHECK(pos().x < -3.2f && pos().x > -5.0f && Near(pos().y, 1.25f, 0.06f));
        rt->OnRuntimeStop();
    }

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

    void TestLineRenderers()
    {
        // Straight line along +X seen from +Z: a ribbon in the XY plane, two quads sharing their middle edge.
        RibbonParams params;
        params.CameraPosition = { 0, 0, 10 };
        std::vector<RibbonPoint> pts{ { { 0, 0, 0 }, 1.0f, glm::vec4(1) }, { { 1, 0, 0 }, 1.0f, glm::vec4(1) },
                                      { { 1, 0, 0 }, 1.0f, glm::vec4(1) }, // duplicate: skipped
                                      { { 3, 0, 0 }, 0.5f, glm::vec4(0, 0, 0, 1) } };
        auto quads = BuildRibbon(pts, params);
        CHECK(quads.size() == 2);
        CHECK(Near(std::abs(quads[0].P[0].y), 0.5f) && Near(quads[0].P[0].z, 0.0f) && Near(quads[0].P[0].y, -quads[0].P[1].y));
        CHECK(quads[0].P[2] == quads[1].P[1] && quads[0].P[3] == quads[1].P[0]); // joined, no gaps
        CHECK(Near(std::abs(quads[1].P[2].y), 0.25f));
        CHECK(Near(quads[0].UV[0].x, 0.0f) && Near(quads[0].UV[2].x, 1.0f / 3.0f) && Near(quads[1].UV[2].x, 1.0f));
        CHECK(quads[1].C[2] == glm::vec4(0, 0, 0, 1));
        params.TextureMode = LineTextureMode::Tile;
        CHECK(Near(BuildRibbon(pts, params)[1].UV[2].x, 3.0f)); // one repeat per world unit

        // Right-angle corner: the miter keeps the full width on both segments.
        std::vector<RibbonPoint> corner{ { { 0, 0, 0 }, 1.0f, glm::vec4(1) }, { { 1, 0, 0 }, 1.0f, glm::vec4(1) }, { { 1, 1, 0 }, 1.0f, glm::vec4(1) } };
        params.TextureMode = LineTextureMode::Stretch;
        auto c = BuildRibbon(corner, params);
        CHECK(c.size() == 2 && Near(glm::distance(c[0].P[2], c[0].P[3]), std::sqrt(2.0f), 1e-3f));

        // Loop: a square closes with a fourth segment; Transform Z alignment ignores the camera.
        std::vector<RibbonPoint> square{ { { 0, 0, 0 }, 0.2f, glm::vec4(1) }, { { 1, 0, 0 }, 0.2f, glm::vec4(1) },
                                         { { 1, 1, 0 }, 0.2f, glm::vec4(1) }, { { 0, 1, 0 }, 0.2f, glm::vec4(1) } };
        params.Loop = true;
        params.FaceCamera = false;
        params.Normal = { 0, 0, 1 };
        params.CameraPosition = { 50, 0, 0 };
        auto loop = BuildRibbon(square, params);
        CHECK(loop.size() == 4 && loop[3].P[2] == loop[0].P[1]);
        bool flat = true;
        for (auto& q : loop) for (auto& v : q.P) flat &= Near(v.z, 0.0f);
        CHECK(flat);

        // Line positions round trip through the serializer; trails record points while playing and age them out.
        Scene scene("Lines");
        auto& reg = scene.GetRegistry();
        Entity lineEntity = scene.CreateEntity("Line");
        reg.Emplace<LineRendererComponent>(lineEntity).Positions = { { 0, 0, 0 }, { 1, 2, 3 }, { -4, 5, 6 } };
        Entity mover = scene.CreateEntity("Mover");
        auto& trail = reg.Emplace<TrailRendererComponent>(mover);
        trail.Time = 0.5f;
        trail.MinVertexDistance = 0.05f;
        auto loaded = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        auto& lr = loaded->GetRegistry().Get<LineRendererComponent>(loaded->FindEntityByName("Line"));
        CHECK(lr.Positions.size() == 3 && lr.Positions[2] == glm::vec3(-4, 5, 6));

        auto runtime = loaded->Clone();
        runtime->OnRuntimeStart();
        Entity rm = runtime->FindEntityByName("Mover");
        auto& rt = runtime->GetRegistry().Get<TrailRendererComponent>(rm);
        CHECK(Near(rt.Time, 0.5f) && rt.Points.empty());
        for (int i = 0; i < 60; ++i)
        {
            runtime->GetRegistry().Get<TransformComponent>(rm).Position.x = i * 0.1f;
            runtime->OnUpdateRuntime(1.0f / 60.0f);
        }
        // 0.5 s at 60 Hz keeps ~30 points, all within the last 0.5 s and ordered oldest first.
        CHECK(rt.Points.size() >= 28 && rt.Points.size() <= 32);
        CHECK(runtime->GetTime() - rt.Points.front().Time <= 0.5f + 1e-4f && rt.Points.front().Position.x < rt.Points.back().Position.x);
        rt.Emitting = false;
        for (int i = 0; i < 40; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(rt.Points.empty());
        runtime->OnRuntimeStop();
    }

    void TestUIDropdownAndScrollbar()
    {
        Scene scene("Dropdown");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        reg.Emplace<CanvasComponent>(canvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
        auto rect = [&](Entity e, glm::vec2 pos, glm::vec2 size) {
            auto& r = reg.Emplace<UIRectComponent>(e);
            r.AnchorMin = r.AnchorMax = { 0, 0 };
            r.Pivot = { 0, 0 };
            r.Position = pos;
            r.Size = size;
        };

        // Dropdown 200 x 40 at the top-left: 8 options, 4 visible -> the list spans y 40..200.
        Entity dropdown = scene.CreateEntity("Dropdown", canvas);
        rect(dropdown, { 0, 0 }, { 200, 40 });
        {
            auto& dd = reg.Emplace<UIDropdownComponent>(dropdown);
            dd.Options.clear();
            for (int i = 0; i < 8; ++i) dd.Options.push_back("Item " + std::to_string(i));
            dd.MaxVisibleItems = 4;
            reg.Emplace<ScriptComponent>(dropdown).Add("WidgetListener");
        }
        // A button underneath the list must not react while the list is open.
        Entity button = scene.CreateEntity("Button", canvas);
        rect(button, { 0, 120 }, { 200, 40 });
        reg.Emplace<UIImageComponent>(button);
        reg.Emplace<UIButtonComponent>(button);

        // Scroll view 200 x 100 at x 400 with 300 of content: max scroll 200, scrollbar track x 586..596, y 4..96.
        Entity view = scene.CreateEntity("View", canvas);
        rect(view, { 400, 0 }, { 200, 100 });
        reg.Emplace<UIScrollViewComponent>(view);
        Entity content = scene.CreateEntity("Content", view);
        {
            auto& r = reg.Emplace<UIRectComponent>(content);
            r.AnchorMin = { 0, 0 }; r.AnchorMax = { 1, 0 }; r.Size = { 0, 0 };
            auto& g = reg.Emplace<UILayoutGroupComponent>(content);
            g.Padding = glm::vec4(0.0f);
            g.Spacing = 0.0f;
        }
        for (int i = 0; i < 6; ++i)
        {
            Entity item = scene.CreateEntity("Item", content);
            reg.Emplace<UIRectComponent>(item).Size = { 0, 50 };
            reg.Emplace<UIImageComponent>(item);
        }
        scene.UpdateTransforms();

        // Options survive serialization.
        auto loaded = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        auto& ldd = loaded->GetRegistry().Get<UIDropdownComponent>(loaded->FindEntityByName("Dropdown"));
        CHECK(ldd.Options.size() == 8 && ldd.Options[7] == "Item 7" && ldd.MaxVisibleItems == 4);

        glm::vec2 screen(1000, 1000);
        auto runtime = scene.Clone();
        auto& rr = runtime->GetRegistry();
        runtime->OnRuntimeStart();
        auto step = [&](glm::vec2 p, bool down, float scroll = 0.0f) {
            runtime->SetUIInput(p, down, screen, true, scroll);
            runtime->OnUpdateRuntime(0.016f);
        };
        auto& dd = rr.Get<UIDropdownComponent>(dropdown);

        // Click the box: opens on release.
        WidgetListener::ValueEvents = 0;
        step({ 100, 20 }, false);
        step({ 100, 20 }, true);
        CHECK(!dd.Open);
        step({ 100, 20 }, false);
        CHECK(dd.Open && Near(dd.ListScroll, 0.0f));

        // Hover the third row, scroll the list two items, then pick the fifth option over the button.
        step({ 100, 130 }, false);
        CHECK(dd.HoveredItem == 2);
        step({ 100, 130 }, false, -2.0f);
        CHECK(Near(dd.ListScroll, 2.0f) && dd.HoveredItem == 4);
        step({ 100, 130 }, true);
        CHECK(!dd.Open && dd.Value == 4 && Near(WidgetListener::LastValue, 4.0f) && WidgetListener::ValueEvents == 1);
        CHECK(!rr.Get<UIButtonComponent>(button).Pressed);
        step({ 100, 130 }, false);

        // Reopen, then click outside: closes without changing the value; the wheel does not reach the scroll view.
        step({ 100, 20 }, true);
        step({ 100, 20 }, false);
        CHECK(dd.Open && Near(dd.ListScroll, 1.0f)); // the selection (4) is the last visible row
        step({ 500, 50 }, false, -3.0f);
        UI::ComputeLayout(*runtime, screen);
        CHECK(Near(rr.Get<UIScrollViewComponent>(view).ScrollPosition, 0.0f));
        step({ 900, 900 }, true);
        step({ 900, 900 }, false);
        CHECK(!dd.Open && dd.Value == 4 && WidgetListener::ValueEvents == 1);

        // Scrollbar: the handle covers 92 * 100/300 px at the top. Drag it half way down the free track.
        auto& sv = rr.Get<UIScrollViewComponent>(view);
        UI::ComputeLayout(*runtime, screen);
        CHECK(Near(sv.MaxScroll, 200.0f));
        float handle = 92.0f * 100.0f / 300.0f, freeTrack = 92.0f - handle;
        step({ 591, 10 }, true);
        CHECK(sv.DraggingScrollbar && Near(sv.DragOffset, 6.0f, 0.01f));
        step({ 591, 10 + freeTrack * 0.5f }, true);
        CHECK(Near(sv.ScrollPosition, 100.0f, 0.1f));
        step({ 591, 10 + freeTrack * 0.5f }, false);
        CHECK(!sv.DraggingScrollbar);
        // Clicking the track below the handle centres the handle on the pointer (clamped at the end).
        step({ 591, 95 }, true);
        CHECK(Near(sv.ScrollPosition, 200.0f, 0.1f));
        step({ 591, 95 }, false);
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

    void TestNestedPrefabs()
    {
        fs::path assets = fs::temp_directory_path() / "dimension_nested_test" / "Assets";
        fs::remove_all(assets.parent_path());
        fs::create_directories(assets);
        AssetManager::Init(assets);
        Scene scene("Nested");
        auto& reg = scene.GetRegistry();
        auto color = [&](Scene& s, Entity e) { return s.GetRegistry().Get<MeshRendererComponent>(e).Color; };
        auto findChild = [](Scene& s, Entity parent, const char* name) {
            for (Entity c : s.GetChildren(parent))
                if (s.GetRegistry().Get<TagComponent>(c).Name == name) return c;
            return Entity::Null();
        };

        // Inner prefab: a Wheel with a red Hub.
        Entity wheel = scene.CreateEntity("Wheel");
        Entity hub = scene.CreateEntity("Hub", wheel);
        reg.Emplace<MeshRendererComponent>(hub).Color = { 1, 0, 0, 1 };
        AssetHandle wheelPrefab = Prefabs::CreateFromEntity(scene, wheel, assets / "Wheel.prefab");
        CHECK(wheelPrefab.IsValid());

        // Outer prefab: a Cart with two wheel instances, the right one's hub recolored blue (an override on the nested
        // instance).
        Entity cart = scene.CreateEntity("Cart");
        Entity left = Prefabs::Instantiate(scene, wheelPrefab, cart);
        Entity right = Prefabs::Instantiate(scene, wheelPrefab, cart);
        reg.Get<TransformComponent>(left).Position = { -1, 0, 0 };
        reg.Get<TransformComponent>(right).Position = { 1, 0, 0 };
        reg.Get<TagComponent>(left).Name = "Left Wheel";
        reg.Get<TagComponent>(right).Name = "Right Wheel";
        reg.Get<MeshRendererComponent>(scene.GetChildren(right)[0]).Color = { 0, 0, 1, 1 };
        Prefabs::RecordFieldOverride(scene, scene.GetChildren(right)[0], "Mesh Renderer", "Color");
        AssetHandle cartPrefab = Prefabs::CreateFromEntity(scene, cart, assets / "Cart.prefab");
        CHECK(cartPrefab.IsValid());
        // The file holds the cart and two references to the wheel (not copies of its objects).
        nlohmann::json file = nlohmann::json::parse(std::ifstream(assets / "Cart.prefab"));
        int references = 0;
        for (auto& rec : file["entities"]) references += rec.contains("nested") && rec["nested"]["prefab"].get<uint64_t>() == (uint64_t)wheelPrefab;
        CHECK(file["entities"].size() == 3 && references == 2);
        CHECK(Prefabs::IsNestedPart(scene, scene.GetChildren(left)[0]) && Prefabs::GetNestedPrefab(scene, left) == wheelPrefab);

        // An instance of the cart has both wheels with their hubs and names; the right hub keeps its override.
        Scene world("World");
        Entity c1 = Prefabs::Instantiate(world, cartPrefab);
        CHECK(!c1.IsNull() && world.GetChildren(c1).size() == 2);
        Entity lw = findChild(world, c1, "Left Wheel"), rw = findChild(world, c1, "Right Wheel");
        CHECK(!lw.IsNull() && !rw.IsNull());
        if (lw.IsNull() || rw.IsNull()) return;
        CHECK(world.GetChildren(lw).size() == 1 && world.GetChildren(rw).size() == 1);
        CHECK(color(world, findChild(world, lw, "Hub")) == glm::vec4(1, 0, 0, 1) && color(world, findChild(world, rw, "Hub")) == glm::vec4(0, 0, 1, 1));
        CHECK(Near(world.GetRegistry().Get<TransformComponent>(rw).Position, { 1, 0, 0 }));
        size_t before = world.GetEntityCount();

        // Edit the wheel: a green hub and a new Tire. Both nested wheels follow (the override stays blue), in new
        // instances and, after a sync, in the existing one.
        {
            auto stage = Prefabs::OpenForEditing(wheelPrefab);
            Entity root = Prefabs::FindEditedRoot(*stage, wheelPrefab);
            stage->GetRegistry().Get<MeshRendererComponent>(stage->GetChildren(root)[0]).Color = { 0, 1, 0, 1 };
            stage->GetRegistry().Emplace<MeshRendererComponent>(stage->CreateEntity("Tire", root)).Color = { 0.1f, 0.1f, 0.1f, 1 };
            CHECK(Prefabs::SaveEdited(*stage, wheelPrefab));
        }
        Prefabs::SyncAll(world);
        CHECK(world.GetEntityCount() == before + 2); // one tire per wheel, nothing duplicated
        CHECK(!findChild(world, lw, "Tire").IsNull() && !findChild(world, rw, "Tire").IsNull());
        CHECK(color(world, findChild(world, lw, "Hub")) == glm::vec4(0, 1, 0, 1) && color(world, findChild(world, rw, "Hub")) == glm::vec4(0, 0, 1, 1));
        Entity c2 = Prefabs::Instantiate(world, cartPrefab);
        CHECK(color(world, findChild(world, findChild(world, c2, "Left Wheel"), "Hub")) == glm::vec4(0, 1, 0, 1));

        // Saving the cart again (Prefab Mode) keeps the wheels as references, and its instances stay matched.
        {
            auto stage = Prefabs::OpenForEditing(cartPrefab);
            Entity root = Prefabs::FindEditedRoot(*stage, cartPrefab);
            CHECK(Prefabs::IsNestedPart(*stage, stage->GetChildren(root)[0]));
            stage->GetRegistry().Get<TransformComponent>(root).Scale = { 2, 2, 2 };
            CHECK(Prefabs::SaveEdited(*stage, cartPrefab));
        }
        file = nlohmann::json::parse(std::ifstream(assets / "Cart.prefab"));
        references = 0;
        for (auto& rec : file["entities"]) references += rec.contains("nested");
        CHECK(file["entities"].size() == 3 && references == 2);
        size_t count = world.GetEntityCount();
        Prefabs::SyncAll(world);
        CHECK(world.GetEntityCount() == count && Near(world.GetRegistry().Get<TransformComponent>(c1).Scale, { 2, 2, 2 }));
        CHECK(color(world, findChild(world, rw, "Hub")) == glm::vec4(0, 0, 1, 1));

        // Apply to Nested: the right wheel's blue hub (and a Cap added to it) go into the wheel prefab itself, so every
        // wheel turns blue; the override is gone.
        {
            Entity rightHub = findChild(world, rw, "Hub");
            Entity cap = world.CreateEntity("Cap", rw);
            world.GetRegistry().Emplace<MeshRendererComponent>(cap).Color = { 1, 1, 1, 1 };
            CHECK(Prefabs::ApplyToNested(world, rightHub));
            CHECK(world.GetRegistry().Get<PrefabLinkComponent>(rightHub).Overrides.empty());
            CHECK(color(world, findChild(world, lw, "Hub")) == glm::vec4(0, 0, 1, 1));
            CHECK(!findChild(world, lw, "Cap").IsNull() && !findChild(world, rw, "Cap").IsNull()); // the added object too
            CHECK(world.GetChildren(rw).size() == 3 && world.GetChildren(lw).size() == 3); // hub, tire, cap: not doubled
            nlohmann::json wheelFile = nlohmann::json::parse(std::ifstream(assets / "Wheel.prefab"));
            CHECK(wheelFile["entities"].size() == 4); // wheel, hub, tire, cap
        }

        // Nesting the cart in a Train nests two levels deep, and a wheel edit still reaches the train.
        Entity train = world.CreateEntity("Train");
        world.SetParent(c2, train);
        AssetHandle trainPrefab = Prefabs::CreateFromEntity(world, train, assets / "Train.prefab");
        Scene depot("Depot");
        Entity t1 = Prefabs::Instantiate(depot, trainPrefab);
        CHECK(!t1.IsNull() && depot.GetChildren(t1).size() == 1 && depot.GetEntityCount() == 1 + 1 + 2 * 4);
        if (t1.IsNull() || depot.GetChildren(t1).empty()) return;
        {
            auto stage = Prefabs::OpenForEditing(wheelPrefab);
            Entity root = Prefabs::FindEditedRoot(*stage, wheelPrefab);
            stage->GetRegistry().Get<MeshRendererComponent>(findChild(*stage, root, "Tire")).Color = { 1, 1, 0, 1 };
            CHECK(Prefabs::SaveEdited(*stage, wheelPrefab));
        }
        Prefabs::SyncAll(depot);
        Entity depotCart = depot.GetChildren(t1)[0];
        Entity depotWheel = findChild(depot, depotCart, "Right Wheel");
        CHECK(!depotWheel.IsNull() && color(depot, findChild(depot, depotWheel, "Tire")) == glm::vec4(1, 1, 0, 1));
        // A prefab cannot end up inside itself: a wheel holding a train is refused at load, without hanging.
        {
            auto stage = Prefabs::OpenForEditing(wheelPrefab);
            Entity root = Prefabs::FindEditedRoot(*stage, wheelPrefab);
            Prefabs::Instantiate(*stage, trainPrefab, root);
            Prefabs::SaveEdited(*stage, wheelPrefab);
        }
        Scene broken("Broken");
        Prefabs::Instantiate(broken, trainPrefab); // logs the cycle
        CHECK(broken.GetEntityCount() < 2000);
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path());
    }

    void TestPrefabVariants()
    {
        fs::path assets = fs::temp_directory_path() / "dimension_variant_test" / "Assets";
        fs::remove_all(assets.parent_path());
        fs::create_directories(assets);
        AssetManager::Init(assets);

        // Base: a Turret with a Barrel child.
        Scene scene("Variants");
        auto& reg = scene.GetRegistry();
        Entity turret = scene.CreateEntity("Turret");
        reg.Emplace<MeshRendererComponent>(turret).Color = { 1, 0, 0, 1 };
        reg.Emplace<PointLightComponent>(turret).Range = 2.0f;
        Entity barrel = scene.CreateEntity("Barrel", turret);
        reg.Get<TransformComponent>(barrel).Scale = { 0.2f, 0.2f, 2.0f };
        reg.Emplace<MeshRendererComponent>(barrel).Color = { 0.5f, 0.5f, 0.5f, 1 };
        AssetHandle base = Prefabs::CreateFromEntity(scene, turret, assets / "Turret.prefab");
        CHECK(base.IsValid());

        // A new variant reads exactly like its base.
        AssetHandle variant = Prefabs::CreateVariant(base, assets / "Turret Variant.prefab");
        CHECK(variant.IsValid() && Prefabs::GetVariantBase(variant) == base && Prefabs::GetVariantBase(base) == AssetHandle::None());
        Entity v0 = scene.InstantiatePrefab(variant, { 3, 0, 0 });
        CHECK(!v0.IsNull() && scene.GetChildren(v0).size() == 1 && reg.Get<MeshRendererComponent>(v0).Color == glm::vec4(1, 0, 0, 1));

        // Edit the variant in Prefab Mode: recolor the barrel, drop the light, add a scope. Only that is saved.
        {
            auto stage = Prefabs::OpenForEditing(variant);
            CHECK(stage != nullptr);
            Entity root = Prefabs::FindEditedRoot(*stage, variant);
            auto& sr = stage->GetRegistry();
            Entity b = stage->GetChildren(root)[0];
            sr.Get<MeshRendererComponent>(b).Color = { 0, 0, 1, 1 };
            sr.Remove<PointLightComponent>(root);
            Entity scope = stage->CreateEntity("Scope", root);
            sr.Emplace<MeshRendererComponent>(scope);
            CHECK(Prefabs::SaveEdited(*stage, variant));
        }
        nlohmann::json file = nlohmann::json::parse(std::ifstream(assets / "Turret Variant.prefab"));
        CHECK(!file.contains("entities") && file["base"].get<uint64_t>() == (uint64_t)base);
        CHECK(file["added"].size() == 1 && file["removedEntities"].empty() && file["overrides"].size() == 2);
        bool onlyColor = false;
        for (auto& o : file["overrides"])
            if (o.contains("components") && o["components"].contains("Mesh Renderer"))
                onlyColor = o["components"].size() == 1 && o["components"]["Mesh Renderer"].size() == 1 && o["components"]["Mesh Renderer"].contains("Color");
        CHECK(onlyColor);

        // A change to the base reaches the variant where it does not override (barrel length), not where it does.
        {
            auto stage = Prefabs::OpenForEditing(base);
            Entity root = Prefabs::FindEditedRoot(*stage, base);
            Entity b = stage->GetChildren(root)[0];
            stage->GetRegistry().Get<TransformComponent>(b).Scale = { 0.2f, 0.2f, 3.0f };
            stage->GetRegistry().Get<MeshRendererComponent>(b).Color = { 1, 1, 0, 1 };
            CHECK(Prefabs::SaveEdited(*stage, base));
        }
        Prefabs::SyncAll(scene);
        auto childNamed = [&](Entity root, const char* name) {
            for (Entity c : scene.GetChildren(root))
                if (reg.Get<TagComponent>(c).Name == name) return c;
            return Entity::Null();
        };
        Entity vb = childNamed(v0, "Barrel");
        CHECK(!vb.IsNull() && Near(reg.Get<TransformComponent>(vb).Scale.z, 3.0f));            // from the base
        CHECK(reg.Get<MeshRendererComponent>(vb).Color == glm::vec4(0, 0, 1, 1));             // the variant's own
        CHECK(!childNamed(v0, "Scope").IsNull() && !reg.Has<PointLightComponent>(v0));        // added / removed in the variant
        CHECK(reg.Get<MeshRendererComponent>(childNamed(turret, "Barrel")).Color == glm::vec4(1, 1, 0, 1)); // base instance

        // A variant of the variant inherits both, and Apply from an instance writes into the variant it came from.
        AssetHandle second = Prefabs::CreateVariant(variant, assets / "Turret Variant 2.prefab");
        CHECK(Prefabs::IsVariantOf(second, base) && Prefabs::IsVariantOf(second, variant) && !Prefabs::IsVariantOf(base, second));
        Entity v2 = scene.InstantiatePrefab(second, { -3, 0, 0 });
        CHECK(reg.Get<MeshRendererComponent>(childNamed(v2, "Barrel")).Color == glm::vec4(0, 0, 1, 1) && !childNamed(v2, "Scope").IsNull());
        reg.Get<MeshRendererComponent>(v2).Smoothness = 0.95f;
        Prefabs::RecordFieldOverride(scene, v2, "Mesh Renderer", "Smoothness");
        CHECK(Prefabs::Apply(scene, v2));
        nlohmann::json second2 = nlohmann::json::parse(std::ifstream(assets / "Turret Variant 2.prefab"));
        CHECK(second2["base"].get<uint64_t>() == (uint64_t)variant && second2["overrides"].size() >= 1);
        Prefabs::SyncAll(scene);
        CHECK(reg.Get<MeshRendererComponent>(v0).Smoothness != 0.95f && Near(reg.Get<MeshRendererComponent>(v2).Smoothness, 0.95f)); // the first variant is untouched
        // Saving and loading the scene keeps the variant instances.
        auto back = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        CHECK(back != nullptr);
        if (back)
        {
            Prefabs::SyncAll(*back);
            Entity bv = back->FindEntityByName("Turret");
            CHECK(!bv.IsNull());
        }
        AssetManager::Shutdown();
        fs::remove_all(assets.parent_path());
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

        // Prefab Mode: edit the asset on its own, then instances pick the changes up and keep their overrides.
        reg.Get<MeshRendererComponent>(a).Color = { 0, 1, 0, 1 };
        Prefabs::RecordFieldOverride(scene, a, "Mesh Renderer", "Color");
        auto stage = Prefabs::OpenForEditing(prefab);
        CHECK(stage != nullptr);
        auto& sreg = stage->GetRegistry();
        Entity sroot = Prefabs::FindEditedRoot(*stage, prefab);
        CHECK(!sroot.IsNull() && stage->GetChildren(sroot).size() == 2 && stage->GetRootEntities().size() == 1);
        sreg.Get<MeshRendererComponent>(sroot).Color = { 0, 0, 1, 1 };
        sreg.Get<MeshRendererComponent>(sroot).Smoothness = 0.2f;
        Entity antenna = Entity::Null(), sEye = Entity::Null();
        for (Entity ch : stage->GetChildren(sroot))
            (sreg.Get<TagComponent>(ch).Name == "Antenna" ? antenna : sEye) = ch;
        sreg.Get<TagComponent>(sEye).Name = "Lens";
        stage->DestroyEntity(antenna);
        Entity fin = stage->CreateEntity("Fin", sroot);
        sreg.Emplace<SpotLightComponent>(fin);
        stage->CreateEntity("Stray"); // outside the root: not part of the prefab
        CHECK(Prefabs::SaveEdited(*stage, prefab));
        uint64_t finId = (uint64_t)stage->GetGuid(fin);
        CHECK(sreg.Has<PrefabLinkComponent>(fin) && sreg.Get<PrefabLinkComponent>(fin).SourceId == finId);

        // Reopening shows exactly the saved prefab, and the new child kept its id.
        auto stage2 = Prefabs::OpenForEditing(prefab);
        Entity root2 = Prefabs::FindEditedRoot(*stage2, prefab);
        std::vector<std::string> names;
        uint64_t finSource = 0;
        for (Entity ch : stage2->GetChildren(root2))
        {
            names.push_back(stage2->GetRegistry().Get<TagComponent>(ch).Name);
            if (names.back() == "Fin") finSource = stage2->GetRegistry().Get<PrefabLinkComponent>(ch).SourceId;
        }
        std::sort(names.begin(), names.end());
        CHECK(names == std::vector<std::string>({ "Fin", "Lens" }) && stage2->GetRootEntities().size() == 1 && finSource == finId);

        Prefabs::SyncAll(scene);
        CHECK(Near(reg.Get<MeshRendererComponent>(a).Color.g, 1.0f));    // instance override kept
        CHECK(Near(reg.Get<MeshRendererComponent>(a).Smoothness, 0.2f)); // edit propagated
        CHECK(Near(reg.Get<MeshRendererComponent>(b).Color.b, 1.0f));    // no override: takes the new color
        std::vector<std::string> childrenA;
        for (Entity ch : scene.GetChildren(a)) childrenA.push_back(reg.Get<TagComponent>(ch).Name);
        std::sort(childrenA.begin(), childrenA.end());
        CHECK(childrenA == std::vector<std::string>({ "Fin", "Lens" }));
        Entity finA = Entity::Null();
        for (Entity ch : scene.GetChildren(a)) if (reg.Get<TagComponent>(ch).Name == "Fin") finA = ch;
        CHECK(!finA.IsNull() && reg.Has<SpotLightComponent>(finA));

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
        // Debugger queries reach .NET: none is attached in a test run, and waiting gives up after its time.
        CHECK(!CSharpRuntime::IsDebuggerAttached());
        auto waitStart = std::chrono::steady_clock::now();
        CHECK(!CSharpRuntime::WaitForDebugger(0.2f));
        float waited = std::chrono::duration<float>(std::chrono::steady_clock::now() - waitStart).count();
        CHECK(waited >= 0.19f && waited < 1.0f);
        const ScriptInfo* spinner = ScriptRegistry::Find("Game.Spinner");
        CHECK(spinner != nullptr && spinner->Module == "csharp");
        if (!spinner) return;
        bool hasField = false;
        for (auto& f : spinner->Fields) hasField |= f.Name == "DegreesPerSecond" && f.Type == FieldType::Vec3;
        CHECK(hasField);
        // Inspector and menu attributes: [Range], [Tooltip], [Header], [AddComponentMenu], [RequireComponent], [SerializeField].
        auto fieldOf = [](const ScriptInfo* s, const char* name) -> const FieldInfo* {
            for (auto& f : s->Fields)
                if (f.Name == name) return &f;
            return nullptr;
        };
        const FieldInfo* bob = fieldOf(spinner, "BobHeight");
        CHECK(bob && bob->Min == 0.0f && bob->Max == 2.0f && bob->Header == "Bobbing");
        const FieldInfo* turn = fieldOf(spinner, "DegreesPerSecond");
        CHECK(turn && turn->Tooltip == "Turn rate around each axis" && turn->Header == "Rotation");
        CHECK(spinner->Menu == "Effects/Spinner" && spinner->Requires.empty());
        const ScriptInfo* launcher = ScriptRegistry::Find("Game.Launcher");
        CHECK(launcher && launcher->Requires == std::vector<std::string>({ "Rigidbody", "Sphere Collider" }));
        CHECK(launcher && fieldOf(launcher, "m_Launches") && fieldOf(launcher, "Impulse") && !fieldOf(launcher, "Key")); // private [SerializeField] in, unsupported type out

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

        // The Greeter keeps the name in PlayerPrefs and a visit count in a save file: in a folder of the test's own.
        fs::path playerData = fs::temp_directory_path() / "dimension_csharp_player_data";
        std::error_code pdError;
        fs::remove_all(playerData, pdError);
        PlayerData::SetFolder(playerData);
        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        runtime->SetUIInput({ 50, 25 }, true, { 800, 600 });
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        runtime->SetUIInput({ 50, 25 }, false, { 800, 600 });
        runtime->InjectUIKeyboard("Ada", false, true);
        for (int i = 0; i < 59; ++i) runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(runtime->GetRegistry().Get<UITextComponent>(greeting).Text == "Hello, Ada!");
        CHECK(PlayerPrefs::GetString("Greeter Name") == "Ada");
        {
            std::ifstream save(playerData / "greeter.json");
            nlohmann::json visits = nlohmann::json::parse(save, nullptr, false);
            CHECK(!visits.is_discarded() && visits.value("LastName", std::string()) == "Ada" && visits.value("Count", 0) == 1);
        }
        float yaw = glm::degrees(runtime->GetRegistry().Get<TransformComponent>(e).Rotation.y);
        CHECK(Near(yaw, 90.0f, 1.0f)); // 90 deg/s for one second, applied by C# through the native API
        glm::vec4 c = runtime->GetRegistry().Get<MeshRendererComponent>(r).Color;
        CHECK(!(Near(c.r, 1.0f) && Near(c.g, 1.0f) && Near(c.b, 1.0f))); // recolored from C#
        // Debug.DrawRay from C#: the Spinner's facing ray (cyan, 1.5 long, this update) among the lines.
        bool spinnerRay = false;
        for (const auto& l : DebugDraw::Lines())
            spinnerRay = spinnerRay || (l.Remaining == 0.0f && Near(glm::length(l.B - l.A), 1.5f, 1e-3f) && Near(l.Color.b, 1.0f));
        CHECK(spinnerRay);
        // Profiling from C#: Rainbow's samples (one nested in the other) and counter in the frame's record.
        {
            Profiler::SetPaused(false);
            Profiler::Clear();
            Profiler::BeginFrame();
            runtime->OnUpdateRuntime(1.0f / 60.0f);
            Profiler::BeginSample("Left Open"); // never ended: closed with the frame
            Profiler::BeginFrame();
            CHECK(!Profiler::History().empty());
            if (!Profiler::History().empty())
            {
                const auto& f = Profiler::History().back();
                int rainbow = -1, color = -1, open = -1;
                for (int i = 0; i < (int)f.Sections.size(); ++i)
                {
                    std::string n = f.Sections[i].Name;
                    if (n == "Rainbow") rainbow = i;
                    if (n == "Rainbow Color") color = i;
                    if (n == "Left Open") open = i;
                }
                CHECK(rainbow >= 0 && color > rainbow && f.Sections[color].Depth == f.Sections[rainbow].Depth + 1 && f.Sections[rainbow].Calls == 1);
                CHECK(rainbow >= 0 && f.Sections[rainbow].Ms >= f.Sections[color].Ms && open >= 0 && f.Sections[open].Depth == 0);
                bool counter = false;
                for (const auto& [n, v] : f.Counters) counter = counter || (n == "Rainbow Hue" && v >= 0.0f && v < 1.0f);
                CHECK(counter);
                // A frame later: nothing of it left open.
                Profiler::BeginFrame();
                bool leaked = false;
                for (const auto& sec : Profiler::History().back().Sections) leaked = leaked || std::string(sec.Name) == "Left Open";
                CHECK(!leaked);
            }
            Profiler::EndSample(); // with none open: nothing
            Profiler::SetPaused(true);
            Profiler::BeginFrame();
            Profiler::Clear();
            Profiler::SetPaused(false);
        }
        runtime->OnRuntimeStop();
        runtime.reset();
        {
            // The next run: welcomed back, from the prefs and the save.
            auto again = scene.Clone();
            again->OnRuntimeStart();
            again->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(again->GetRegistry().Get<UITextComponent>(greeting).Text == "Welcome back, Ada! (1 visits)");
            again->OnRuntimeStop();
        }
        PlayerData::SetFolder({});
        fs::remove_all(playerData, pdError);

        // Localization from C#: the Sandbox's LanguageButton switches to the next language on a click; its label
        // (a Localized Key) follows, and the choice is kept for the next run.
        {
            fs::path locDir = fs::temp_directory_path() / "dimension_csharp_localization";
            fs::remove_all(locDir, pdError);
            fs::create_directories(locDir / "Assets");
            fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Localization/Menu.csv", locDir / "Assets/Menu.csv");
            AssetManager::Init(locDir / "Assets");
            PlayerData::SetFolder(locDir / "PlayerData");
            Localization::SetSystemLanguage("en-GB");
            Scene loc("Localization");
            auto& lreg = loc.GetRegistry();
            Entity lcanvas = loc.CreateEntity("Canvas");
            lreg.Emplace<CanvasComponent>(lcanvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
            Entity button = loc.CreateEntity("Language Button", lcanvas);
            {
                auto& br = lreg.Emplace<UIRectComponent>(button);
                br.AnchorMin = br.AnchorMax = br.Pivot = { 0, 0 };
                br.Size = { 200, 50 };
            }
            lreg.Emplace<UIButtonComponent>(button);
            lreg.Emplace<UITextComponent>(button).LocalizedKey = "menu.language";
            lreg.Emplace<ScriptComponent>(button).Add("Game.LanguageButton");
            loc.UpdateTransforms();
            CHECK(Localization::Language() == "en" && Localization::Get("menu.language") == "Language: English");
            auto rt = loc.Clone();
            rt->OnRuntimeStart();
            rt->OnUpdateRuntime(1.0f / 60.0f);
            rt->SetUIInput({ 50, 25 }, true, { 800, 600 });
            rt->OnUpdateRuntime(1.0f / 60.0f);
            rt->SetUIInput({ 50, 25 }, false, { 800, 600 });
            rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(Localization::Language() == "fr" && PlayerPrefs::GetString("Language") == "fr");
            CHECK(Localization::Get(rt->GetRegistry().Get<UITextComponent>(button).LocalizedKey) == "Langue : Fran\xC3\xA7" "ais");
            rt->OnRuntimeStop();
            rt.reset();
            Localization::Reset(); // the next run starts in the language chosen
            CHECK(Localization::Language() == "fr");
            AssetManager::Shutdown();
            Localization::SetSystemLanguage({});
            PlayerData::SetFolder({});
            fs::remove_all(locDir, pdError);
        }

        // Data assets from C#: the Sandbox's Palette (a DataAsset) shared by two PaletteTint scripts, one object for both,
        // taking the asset's new values when it is saved while playing.
        {
            fs::path daDir = fs::temp_directory_path() / "dimension_csharp_data_assets";
            fs::remove_all(daDir, pdError);
            fs::create_directories(daDir / "Assets");
            AssetManager::Init(daDir / "Assets");
            const DataAssets::ClassInfo* palette = DataAssets::FindClass("Game.Palette");
            CHECK(palette && palette->Fields.size() == 2 && palette->Fields[0].Name == "Colors" && !ScriptRegistry::Find("Game.Palette"));
            const ScriptInfo* tint = ScriptRegistry::Find("Game.PaletteTint");
            CHECK(tint && tint->Fields.size() == 2 && tint->Fields[0].Type == FieldType::DataAsset && tint->Fields[0].AssetClass == "Game.Palette");
            // A new asset holds the class's defaults (two colors, 1.5 s each).
            AssetHandle asset = DataAssets::Create(daDir / "Assets/Tints.asset", "Game.Palette");
            nlohmann::json fields = DataAssets::Read(asset)["fields"];
            CHECK(asset.IsValid() && fields["Colors"].size() == 2 && Near(fields["SecondsPerColor"].get<float>(), 1.5f));
            fields["Colors"] = nlohmann::json::array({ nlohmann::json::array({ 0.2f, 0.4f, 0.6f, 1.0f }) });
            DataAssets::Write(daDir / "Assets/Tints.asset", "Game.Palette", fields);

            Scene da("DataAssets");
            Entity a = da.CreateEntity("A"), b = da.CreateEntity("B");
            for (Entity x : { a, b })
            {
                da.GetRegistry().Emplace<MeshRendererComponent>(x).Color = { 1, 1, 1, 1 };
                da.GetRegistry().Emplace<ScriptComponent>(x).Add("Game.PaletteTint").Fields["Palette"] = (uint64_t)asset;
            }
            auto rt = da.Clone();
            rt->OnRuntimeStart();
            rt->OnUpdateRuntime(1.0f / 60.0f);
            auto colorOf = [&](Entity x) { return rt->GetRegistry().Get<MeshRendererComponent>(x).Color; };
            CHECK(Near(colorOf(a).r, 0.2f) && Near(colorOf(a).b, 0.6f) && Near(colorOf(b).g, 0.4f));
            // Saved while playing: both scripts' palette (the same object) has the new color at once.
            fields["Colors"] = nlohmann::json::array({ nlohmann::json::array({ 0.9f, 0.1f, 0.1f, 1.0f }) });
            DataAssets::Write(daDir / "Assets/Tints.asset", "Game.Palette", fields);
            rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(Near(colorOf(a).r, 0.9f) && Near(colorOf(b).r, 0.9f) && Near(colorOf(b).g, 0.1f));
            rt->OnRuntimeStop();
            rt.reset();
            AssetManager::Shutdown();
            fs::remove_all(daDir, pdError);
        }

        // Hierarchy, tags and SetActive from C#: the Sandbox's CoinGroup listens to its children (OnEnable), and the
        // Player falling onto a coin turns it off; it comes back after RespawnSeconds.
        {
            Scene cg("Coins");
            auto& creg = cg.GetRegistry();
            Entity group = cg.CreateEntity("Coins");
            creg.Emplace<ScriptComponent>(group).Add("Game.CoinGroup").Fields["RespawnSeconds"] = 0.5f;
            Entity coin = cg.CreateEntity("Coin", group), other = cg.CreateEntity("Far Coin", group);
            creg.Get<TransformComponent>(other).Position = { 20, 0, 0 };
            for (Entity c : { coin, other }) creg.Emplace<CircleCollider2DComponent>(c).IsTrigger = true;
            Entity player = cg.CreateEntity("Hero");
            creg.Get<TagComponent>(player).Tag = "Player";
            creg.Get<TransformComponent>(player).Position = { 0, 1.5f, 0 };
            creg.Emplace<Rigidbody2DComponent>(player);
            creg.Emplace<BoxCollider2DComponent>(player).Size = { 0.5f, 0.5f };
            auto rt = cg.Clone();
            rt->OnRuntimeStart();
            auto& rreg = rt->GetRegistry();
            int frames = 0;
            while (rreg.Get<TagComponent>(coin).Active && frames < 120) { rt->OnUpdateRuntime(1.0f / 60.0f); ++frames; }
            CHECK(!rreg.Get<TagComponent>(coin).Active && rreg.Get<TagComponent>(other).Active && frames < 120);
            for (int i = 0; i < 40; ++i) rt->OnUpdateRuntime(1.0f / 60.0f); // 0.67 s
            CHECK(rreg.Get<TagComponent>(coin).Active);
            rt->OnRuntimeStop();
        }

        // Camera.Main.ScreenPointToRay + Physics.Raycast from C#: the Sandbox's ClickToMove moves its marker to the
        // ground point under a click (the pointer in view pixels, as the Game view or the Player passes it).
        {
            Scene cm("Click");
            auto& creg = cm.GetRegistry();
            Entity cam = cm.CreateEntity("Main Camera");
            creg.Emplace<CameraComponent>(cam);
            creg.Get<TransformComponent>(cam).Position = { 0, 12, 12 };
            creg.Get<TransformComponent>(cam).Rotation = { glm::radians(-45.0f), 0, 0 };
            Entity ground = cm.CreateEntity("Ground");
            creg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            creg.Get<TransformComponent>(ground).Scale = { 40, 1, 40 };
            creg.Emplace<BoxColliderComponent>(ground);
            Entity marker = cm.CreateEntity("Marker");
            creg.Emplace<ScriptComponent>(cam).Add("Game.ClickToMove").Fields["Marker"] = (uint64_t)cm.GetGuid(marker);
            auto rt = cm.Clone();
            rt->OnRuntimeStart();
            glm::vec2 view(1280, 720);
            glm::vec3 target(3.0f, 0.0f, -2.0f);
            glm::vec3 screen = CameraProjection::WorldToScreen(*rt, cam, target, view);
            rt->SetUIInput(glm::vec2(screen), true, view);
            bool gameInput = Input::IsGameInputEnabled();
            Input::SetGameInputEnabled(true);
            Input::SimulateMouseButton(MouseButton::Left, true);
            rt->OnUpdateRuntime(1.0f / 60.0f);
            Input::SimulateMouseButton(MouseButton::Left, false);
            Input::SetGameInputEnabled(gameInput);
            glm::vec3 moved = rt->GetRegistry().Get<TransformComponent>(marker).Position;
            CHECK(glm::length(moved - target) < 0.02f);
            rt->OnRuntimeStop();
        }

        // Physics2D.OverlapPoint and Rigidbody2D.AngularVelocity from C#: the Sandbox's ClickToKick kicks the body under a
        // click up, spinning away from the side clicked.
        {
            Scene ck("Kick");
            auto& creg = ck.GetRegistry();
            Entity cam = ck.CreateEntity("Main Camera");
            creg.Emplace<CameraComponent>(cam).Projection = ProjectionType::Orthographic;
            creg.Get<TransformComponent>(cam).Position = { 0, 0, 10 };
            creg.Emplace<ScriptComponent>(cam).Add("Game.ClickToKick");
            Entity box = ck.CreateEntity("Box");
            creg.Get<TransformComponent>(box).Position = { 2, 1, 0 };
            creg.Emplace<Rigidbody2DComponent>(box).GravityScale = 0.0f;
            creg.Emplace<BoxCollider2DComponent>(box);
            auto rt = ck.Clone();
            rt->OnRuntimeStart();
            glm::vec2 view(1280, 720);
            glm::vec3 screen = CameraProjection::WorldToScreen(*rt, cam, { 1.7f, 1.0f, 0.0f }, view); // left of center
            rt->SetUIInput(glm::vec2(screen), true, view);
            bool gameInput = Input::IsGameInputEnabled();
            Input::SetGameInputEnabled(true);
            Input::SimulateMouseButton(MouseButton::Left, true);
            rt->OnUpdateRuntime(1.0f / 60.0f);
            Input::SimulateMouseButton(MouseButton::Left, false);
            Input::SetGameInputEnabled(gameInput);
            CHECK(Physics2DWorld::GetLinearVelocity(*rt, box).y > 1.0f && Physics2DWorld::GetAngularVelocity(*rt, box) < -1.0f);
            rt->OnRuntimeStop();
        }

        // Physics.SphereCast and Rigidbody.AddImpulseAtPosition from C#: the Sandbox's Shover pushes the crate under a
        // right-click along the mouse ray.
        {
            Scene sh("Shove");
            auto& creg = sh.GetRegistry();
            Entity cam = sh.CreateEntity("Main Camera");
            creg.Emplace<CameraComponent>(cam);
            creg.Get<TransformComponent>(cam).Position = { 0, 2, 8 };
            creg.Emplace<ScriptComponent>(cam).Add("Game.Shover");
            Entity crate = sh.CreateEntity("Crate");
            creg.Get<TransformComponent>(crate).Position = { 0.3f, 2, 0 };
            creg.Emplace<RigidbodyComponent>(crate).UseGravity = false;
            creg.Emplace<BoxColliderComponent>(crate);
            auto rt = sh.Clone();
            rt->OnRuntimeStart();
            glm::vec2 view(1280, 720);
            rt->SetUIInput(glm::vec2(CameraProjection::WorldToScreen(*rt, cam, { 0.3f, 2, 0.5f }, view)), false, view);
            bool gameInput = Input::IsGameInputEnabled();
            Input::SetGameInputEnabled(true);
            Input::SimulateMouseButton(MouseButton::Right, true);
            rt->OnUpdateRuntime(1.0f / 60.0f);
            Input::SimulateMouseButton(MouseButton::Right, false);
            Input::SetGameInputEnabled(gameInput);
            glm::vec3 v = Physics3DWorld::GetLinearVelocity(*rt, crate);
            CHECK(v.z < -1.0f && glm::length(Physics3DWorld::GetAngularVelocity(*rt, crate)) > 0.01f);
            rt->OnRuntimeStop();
        }

        // OnCollisionEnter(Collision) from C#: the Sandbox's HitFlash flashes a box landing hard (about 6 m/s) but not one
        // set down gently, and tells a Scoreboard: one box through its Board field (a script on another object, picked
        // as that object), another with Board empty through FindObjectOfType<Scoreboard>().
        {
            Scene hf("HitFlash");
            auto& creg = hf.GetRegistry();
            Entity ground = hf.CreateEntity("Ground");
            creg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            creg.Get<TransformComponent>(ground).Scale = { 20, 1, 20 };
            creg.Emplace<BoxColliderComponent>(ground);
            Entity dropped = hf.CreateEntity("Dropped"), placed = hf.CreateEntity("Placed"), unlinked = hf.CreateEntity("Unlinked");
            creg.Get<TransformComponent>(dropped).Position = { -3, 2.5f, 0 };
            creg.Get<TransformComponent>(placed).Position = { 3, 0.52f, 0 };
            creg.Get<TransformComponent>(unlinked).Position = { 6, 4.0f, 0 };
            Entity board = hf.CreateEntity("Board");
            creg.Emplace<UITextComponent>(board);
            creg.Emplace<ScriptComponent>(board).Add("Game.Scoreboard");
            const ScriptInfo* flashInfo = ScriptRegistry::Find("Game.HitFlash");
            bool boardField = false;
            for (auto& f : flashInfo ? flashInfo->Fields : std::vector<FieldInfo>{})
                boardField |= f.Name == "Board" && f.Type == FieldType::EntityRef && f.ScriptClass == "Game.Scoreboard";
            CHECK(boardField);
            for (Entity e : { dropped, placed, unlinked })
            {
                creg.Emplace<MeshRendererComponent>(e).Color = { 1, 1, 1, 1 };
                creg.Emplace<RigidbodyComponent>(e);
                creg.Emplace<BoxColliderComponent>(e);
                auto& s = creg.Emplace<ScriptComponent>(e).Add("Game.HitFlash");
                if (e == dropped) s.Fields["Board"] = (uint64_t)hf.GetGuid(board);
            }
            auto rt = hf.Clone();
            rt->OnRuntimeStart();
            bool flashed = false, placedFlashed = false;
            for (int i = 0; i < 90; ++i)
            {
                rt->OnUpdateRuntime(1.0f / 60.0f);
                flashed |= rt->GetRegistry().Get<MeshRendererComponent>(dropped).Tint.g < 0.9f; // the Tint, so over materials too
                placedFlashed |= rt->GetRegistry().Get<MeshRendererComponent>(placed).Tint.g < 0.99f;
            }
            CHECK(flashed && !placedFlashed);
            CHECK(Near(rt->GetRegistry().Get<MeshRendererComponent>(dropped).Tint.g, 1.0f, 0.01f) &&
                  rt->GetRegistry().Get<MeshRendererComponent>(dropped).Color == glm::vec4(1.0f)); // faded back; its Color untouched
            std::string score = rt->GetRegistry().Get<UITextComponent>(board).Text;
            CHECK(score == "Hard hits: 2 (last: Unlinked into Ground)");
            rt->OnRuntimeStop();
        }

        // OnMouseDown / OnMouseDrag from C#: the Sandbox's Draggable carries a crate after the pointer (pressed on it, then
        // moved 4 m to the side while held), and leaves it when released.
        {
            Scene dg("Drag");
            auto& creg = dg.GetRegistry();
            Entity cam = dg.CreateEntity("Main Camera");
            creg.Emplace<CameraComponent>(cam);
            creg.Get<TransformComponent>(cam).Position = { 0, 12, 12 };
            creg.Get<TransformComponent>(cam).Rotation = { glm::radians(-45.0f), 0, 0 };
            Entity ground = dg.CreateEntity("Ground");
            creg.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            creg.Get<TransformComponent>(ground).Scale = { 40, 1, 40 };
            creg.Emplace<BoxColliderComponent>(ground);
            Entity crate = dg.CreateEntity("Crate");
            creg.Get<TransformComponent>(crate).Position = { 0, 0.5f, 0 };
            creg.Emplace<RigidbodyComponent>(crate);
            creg.Emplace<BoxColliderComponent>(crate);
            creg.Emplace<ScriptComponent>(crate).Add("Game.Draggable");
            auto rt = dg.Clone();
            rt->OnRuntimeStart();
            glm::vec2 view(1280, 720);
            glm::vec2 onCrate(CameraProjection::WorldToScreen(*rt, cam, { 0, 0.5f, 0 }, view));
            glm::vec2 aside(CameraProjection::WorldToScreen(*rt, cam, { 4, 1.0f, 0 }, view)); // at the carrying height
            rt->SetUIInput(onCrate, false, view);
            rt->OnUpdateRuntime(1.0f / 60.0f);
            rt->SetUIInput(onCrate, true, view);
            rt->OnUpdateRuntime(1.0f / 60.0f);
            for (int i = 0; i < 60; ++i)
            {
                rt->SetUIInput(aside, true, view);
                rt->OnUpdateRuntime(1.0f / 60.0f);
            }
            glm::vec3 held = rt->GetRegistry().Get<TransformComponent>(crate).Position;
            CHECK(Near(held.x, 4.0f, 0.3f) && held.y > 0.7f);
            rt->SetUIInput(aside, false, view);
            for (int i = 0; i < 90; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(rt->GetRegistry().Get<TransformComponent>(crate).Position.y < 0.6f); // let go: it drops back
            rt->OnRuntimeStop();
        }

        // Instantiate / Destroy from C#: the Sandbox's Cloner drops copies of a crate (with its children and scripts) on C,
        // keeping at most MaxCopies.
        {
            Scene cl("Cloner");
            auto& creg = cl.GetRegistry();
            Entity crate = cl.CreateEntity("Crate");
            creg.Get<TransformComponent>(crate).Position = { 0, 0.5f, 30 };
            creg.Emplace<RigidbodyComponent>(crate).UseGravity = false;
            creg.Emplace<BoxColliderComponent>(crate);
            creg.Emplace<ScriptComponent>(crate).Add("Game.HitFlash");
            cl.CreateEntity("Slat", crate);
            Entity spawner = cl.CreateEntity("Spawner");
            auto& cloner = creg.Emplace<ScriptComponent>(spawner).Add("Game.Cloner");
            cloner.Fields["Template"] = (uint64_t)cl.GetGuid(crate);
            cloner.Fields["MaxCopies"] = 2;
            auto rt = cl.Clone();
            rt->OnRuntimeStart();
            bool gameInput = Input::IsGameInputEnabled();
            Input::SetGameInputEnabled(true);
            for (int i = 0; i < 3; ++i)
            {
                Input::SimulateKey(Key::C, true);
                rt->OnUpdateRuntime(1.0f / 60.0f);
                Input::SimulateKey(Key::C, false);
                rt->OnUpdateRuntime(1.0f / 60.0f);
            }
            Input::SetGameInputEnabled(gameInput);
            int copies = 0;
            bool whole = true;
            rt->GetRegistry().EachEntity([&](Entity e) {
                if (rt->GetRegistry().Get<TagComponent>(e).Name != "Crate copy") return;
                ++copies;
                auto* sc = rt->GetRegistry().TryGet<ScriptComponent>(e);
                whole &= rt->GetChildren(e).size() == 1 && sc && sc->Scripts.size() == 1 && sc->Scripts[0].Instance != nullptr &&
                         std::abs(rt->GetRegistry().Get<TransformComponent>(e).Position.y - 4.0f) < 0.2f;
            });
            CHECK(copies == 2 && whole);
            rt->OnRuntimeStop();
        }

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
        // C# events: a label's script listens to another entity's UI Button (which has no script) and unsubscribes
        // after its Limit; the listeners end with play mode.
        {
            Scene ev("Events");
            auto& er = ev.GetRegistry();
            Entity evCanvas = ev.CreateEntity("Canvas");
            er.Emplace<CanvasComponent>(evCanvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
            Entity button = ev.CreateEntity("Press Me", evCanvas);
            {
                auto& br = er.Emplace<UIRectComponent>(button);
                br.AnchorMin = br.AnchorMax = br.Pivot = { 0, 0 };
                br.Size = { 200, 50 };
            }
            er.Emplace<UIButtonComponent>(button);
            Entity label = ev.CreateEntity("Counter", evCanvas);
            er.Emplace<UIRectComponent>(label).Position = { 400, 400 };
            er.Emplace<UITextComponent>(label).Text = "";
            auto& counter = er.Emplace<ScriptComponent>(label).Add("Game.ButtonCounter");
            counter.Fields["Button"] = "Press Me";
            counter.Fields["Limit"] = 3;
            ev.UpdateTransforms();
            for (int round = 0; round < 2; ++round) // the second run checks that the first run's listener is gone
            {
                auto rt = ev.Clone();
                rt->OnRuntimeStart();
                auto click = [&] {
                    rt->SetUIInput({ 50, 25 }, true, { 800, 600 });
                    rt->OnUpdateRuntime(1.0f / 60.0f);
                    rt->SetUIInput({ 50, 25 }, false, { 800, 600 });
                    rt->OnUpdateRuntime(1.0f / 60.0f);
                };
                auto text = [&] { return rt->GetRegistry().Get<UITextComponent>(label).Text; };
                rt->OnUpdateRuntime(1.0f / 60.0f);
                CHECK(text() == "Clicks: 0");
                click();
                CHECK(text() == "Clicks: 1");
                click();
                if (round == 1)
                {
                    // Recompiled while playing: the count survives the reload and the script listens again.
                    CHECK(rt->ReloadScriptModule("csharp", [&] { return CSharpRuntime::LoadScripts(scripts); }));
                    rt->OnUpdateRuntime(1.0f / 60.0f);
                    CHECK(text() == "Clicks: 2");
                }
                click();
                click(); // past the limit: unsubscribed
                CHECK(text() == "Clicks: 3");
                rt->OnRuntimeStop();
            }
        }
        {
            // Engine components from C#: a vehicle driven through the Vehicle class (and a field set by name), and a
            // cloth whose wind a script changes.
            Scene phys("CSharp Physics");
            auto& pr = phys.GetRegistry();
            Entity car = phys.CreateEntity("Car");
            pr.Get<TransformComponent>(car).Position = { 0, 1, 0 };
            pr.Emplace<RigidbodyComponent>(car).Mass = 1200.0f;
            pr.Emplace<BoxColliderComponent>(car).Size = { 1.8f, 0.6f, 4.0f };
            pr.Emplace<VehicleComponent>(car);
            auto& drive = pr.Emplace<ScriptComponent>(car).Add("Game.VehicleInput");
            drive.Fields["CruiseThrottle"] = 0.7f;
            drive.Fields["SteerAngle"] = 22.0f;
            Entity ground = phys.CreateEntity("Ground");
            pr.Get<TransformComponent>(ground).Position = { 0, -0.5f, 0 };
            pr.Emplace<BoxColliderComponent>(ground).Size = { 200, 1, 200 };
            Entity flag = phys.CreateEntity("Flag");
            pr.Get<TransformComponent>(flag).Position = { 10, 3, 0 };
            pr.Emplace<ClothComponent>(flag).Resolution = 6;
            pr.Emplace<MeshRendererComponent>(flag);
            pr.Emplace<ScriptComponent>(flag).Add("Game.WindGust");
            phys.UpdateTransforms();
            auto rt = phys.Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < 60; ++i) rt->OnUpdateRuntime(1.0f / 60.0f);
            auto& vc = rt->GetRegistry().Get<VehicleComponent>(car);
            CHECK(Near(vc.Throttle, 0.7f) && Near(vc.MaxSteerAngle, 22.0f));
            CHECK(vc.SpeedKmh > 1.0f); // and it drives
            glm::vec3 wind = rt->GetRegistry().Get<ClothComponent>(flag).Wind;
            CHECK(glm::length(wind) > 1.0f);
            rt->OnRuntimeStop();
        }
        // Editor extensions (the Sandbox's SceneTools.cs): its menu items and window are registered; through stand-in
        // editor hooks, Line Up Selected puts the selection in a row (one undo step), Add Marker creates and selects an
        // entity, and the window draws its controls (a value it returns is kept).
        {
            const auto& items = CSharpRuntime::GetEditorItems();
            auto find = [&](const std::string& path) -> const CSharpRuntime::EditorItem* {
                for (const auto& i : items)
                    if (i.MenuPath == path) return &i;
                return nullptr;
            };
            const auto* lineUp = find("Tools/Line Up Selected");
            const auto* marker = find("Tools/Add Marker");
            const auto* window = find("Window/Scene Tools");
            CHECK(lineUp && !lineUp->Window && marker && window && window->Window && window->Title == "Scene Tools");
            Scene edited("Edited");
            std::vector<Entity> selection;
            for (int i = 0; i < 3; ++i)
            {
                Entity e = edited.CreateEntity("Box " + std::to_string(i));
                edited.GetRegistry().Get<TransformComponent>(e).Position = { (float)i * 7.0f, (float)i, 1.0f };
                selection.push_back(e);
            }
            int undos = 0;
            std::vector<std::string> controls;
            CSharpRuntime::EditorHooks hooks;
            hooks.Selection = [&](int op, Entity e, const char* text) -> Entity {
                switch (op)
                {
                case 0: return selection.empty() ? Entity::Null() : selection.back();
                case 1: selection = { e }; return e;
                case 2: return edited.CreateEntity(text);
                case 3: return Entity{ (uint32_t)selection.size(), 0 };
                case 4: return selection[e.Index];
                }
                return Entity::Null();
            };
            hooks.Undo = [&](const char*) { ++undos; };
            std::string click; // the button "clicked"
            hooks.Gui = [&](int op, const char* label, float* v, int n) -> int {
                controls.push_back(std::to_string(op) + ":" + label);
                if (op == 5 && n >= 3 && std::string(label) == "Spacing") v[0] = 3.0f; // the Spacing slider moved to 3
                return op == 1 && click == label ? 1 : 0;
            };
            hooks.Text = [&](const char* label, const std::string& text) {
                controls.push_back(std::string("text:") + label);
                return text;
            };
            Entity pickEntity = Entity::Null(); // what the entity picker "picks" (null: leaves the value)
            hooks.EntityField = [&](const char* label, Entity value) {
                controls.push_back(std::string("entity:") + label);
                return pickEntity.IsNull() ? value : pickEntity;
            };
            hooks.AssetField = [&](int kind, const char* label, uint64_t value) {
                controls.push_back("asset" + std::to_string(kind) + ":" + label);
                return value;
            };
            CSharpRuntime::SetEditorHooks(hooks);
            if (lineUp && marker && window)
            {
                CSharpRuntime::CallEditorItem(window->Id, true, edited); // Spacing becomes 3
                CSharpRuntime::CallEditorItem(lineUp->Id, false, edited);
                auto& reg = edited.GetRegistry();
                CHECK(undos == 1 && Near(reg.Get<TransformComponent>(selection[1]).Position, { 3.0f, 0.0f, 1.0f }) &&
                      Near(reg.Get<TransformComponent>(selection[2]).Position, { 6.0f, 0.0f, 1.0f }));
                CHECK(std::find(controls.begin(), controls.end(), "5:Spacing") != controls.end() &&
                      std::find(controls.begin(), controls.end(), "text:Name") != controls.end() &&
                      std::find(controls.begin(), controls.end(), "1:Line Up Selected") != controls.end());
                CHECK(std::find(controls.begin(), controls.end(), "entity:Object") != controls.end() &&
                      std::find(controls.begin(), controls.end(), "asset1:Marker Prefab") != controls.end() && // the pickers
                      std::find(controls.begin(), controls.end(), "11:AxisXYZ") != controls.end());   // an enum's dropdown
                CSharpRuntime::CallEditorItem(marker->Id, false, edited);
                CHECK(selection.size() == 1 && edited.IsValid(selection[0]) && edited.GetRegistry().Get<TagComponent>(selection[0]).Name == "Marker");
                // Picking an object in the window's entity field selects it.
                pickEntity = edited.CreateEntity("Picked");
                CSharpRuntime::CallEditorItem(window->Id, true, edited);
                CHECK(selection.size() == 1 && selection[0] == pickEntity);
                pickEntity = Entity::Null();
            }
            // Custom Inspectors (SpinnerInspector.cs): Spinner's draws its presets and then the usual fields (not the
            // hidden ones) from the saved values; a clicked preset comes back into them. The Point Light's draws below
            // the component and moves the object through the C# API.
            const auto* spinnerInspector = CSharpRuntime::FindInspector("Game.Spinner");
            const auto* lightInspector = CSharpRuntime::FindInspector("Point Light");
            const ScriptInfo* spinner = ScriptRegistry::Find("Game.Spinner");
            CHECK(spinnerInspector && lightInspector && spinner && CSharpRuntime::FindInspector("Game.Rainbow") == nullptr);
            if (spinnerInspector && lightInspector && spinner)
            {
                Entity host = edited.CreateEntity("Spinning");
                auto instance = spinner->Create();
                auto field = [&](const char* name) -> void* {
                    for (auto& f : spinner->Fields)
                        if (f.Name == name) return f.Access(spinner->ToObject(instance.get()));
                    return nullptr;
                };
                *static_cast<float*>(field("BobHeight")) = 1.5f;
                controls.clear();
                click.clear();
                CHECK(!CSharpRuntime::InspectScript(spinnerInspector->Id, *instance, edited, host)); // nothing changed
                CHECK(std::find(controls.begin(), controls.end(), "0:Turns 90 deg/s, bobs 1.5 m") != controls.end() &&
                      std::find(controls.begin(), controls.end(), "5:BobHeight") != controls.end() &&
                      std::find(controls.begin(), controls.end(), "6:DegreesPerSecond") != controls.end() &&
                      std::none_of(controls.begin(), controls.end(), [](const std::string& c) { return c.find("m_Time") != std::string::npos; }));
                click = "Fast";
                CHECK(CSharpRuntime::InspectScript(spinnerInspector->Id, *instance, edited, host));
                CHECK(Near(*static_cast<glm::vec3*>(field("DegreesPerSecond")), { 0.0f, 360.0f, 0.0f }) &&
                      Near(*static_cast<float*>(field("BobHeight")), 1.5f));

                edited.GetRegistry().Get<TransformComponent>(host).Position = { 0.0f, 2.0f, 0.0f };
                int undosBefore = undos;
                click = "Raise 1 m";
                CSharpRuntime::InspectComponent(lightInspector->Id, edited, host);
                CHECK(undos == undosBefore + 1 && Near(edited.GetRegistry().Get<TransformComponent>(host).Position.y, 3.0f));
            }
            // Asset fields and property drawers (PrefabDropper.cs): a Prefab field registers as a prefab picker; the
            // [Percent] field, and only it, has a drawer, which draws it as a 0-100 slider and stores 0-1.
            const ScriptInfo* dropper = ScriptRegistry::Find("Game.PrefabDropper");
            CHECK(dropper != nullptr);
            if (dropper)
            {
                const FieldInfo* prefabField = fieldOf(dropper, "Prefab");
                CHECK(prefabField && prefabField->Type == FieldType::Prefab && prefabField->Tooltip == "What to drop");
                auto instance = dropper->Create();
                size_t chance = 0;
                for (size_t k = 0; k < dropper->Fields.size(); ++k)
                {
                    if (dropper->Fields[k].Name == "Chance") chance = k;
                    CHECK(CSharpRuntime::HasFieldDrawer(*instance, k) == (dropper->Fields[k].Name == "Chance"));
                }
                Entity host = edited.CreateEntity("Dropper");
                hooks.Gui = [&](int op, const char* label, float* v, int n) -> int {
                    controls.push_back(std::to_string(op) + ":" + label);
                    if (op == 5 && n >= 3 && std::string(label) == "Chance %")
                    {
                        CHECK(Near(v[0], 75.0f) && Near(v[2], 100.0f)); // shown as a percentage
                        v[0] = 40.0f;
                    }
                    return 0;
                };
                CSharpRuntime::SetEditorHooks(hooks);
                controls.clear();
                CHECK(CSharpRuntime::DrawScriptField(*instance, chance, edited, host));
                CHECK(controls.size() == 1 && controls[0] == "5:Chance %");
                CHECK(Near(*static_cast<float*>(dropper->Fields[chance].Access(dropper->ToObject(instance.get()))), 0.4f));
                CHECK(!CSharpRuntime::DrawScriptField(*instance, 0, edited, host)); // no drawer: nothing drawn or changed
            }
            CSharpRuntime::SetEditorHooks({});
        }
        {
            // Lists, arrays and enums (Patrol.cs): List<Vector3> and string[] register as lists of plain values (saved as
            // arrays, reorderable), the enum as a dropdown storing its values (Once = 5). Defaults come from the class;
            // values saved with the scene reach the running script, which walks the points and stops (Once).
            const ScriptInfo* patrol = ScriptRegistry::Find("Game.Patrol");
            CHECK(patrol != nullptr);
            if (patrol)
            {
                const FieldInfo* points = fieldOf(patrol, "Points");
                const FieldInfo* mode = fieldOf(patrol, "Mode");
                const FieldInfo* calls = fieldOf(patrol, "Calls");
                const FieldInfo* speed = fieldOf(patrol, "Speed");
                CHECK(points && IsValueList(*points) && points->Elements[0].Type == FieldType::Vec3 && points->Tooltip == "Stops, relative to where the object starts");
                CHECK(calls && IsValueList(*calls) && calls->Elements[0].Type == FieldType::String);
                CHECK(mode && mode->Type == FieldType::Enum && (mode->EnumNames == std::vector<std::string>{ "Loop", "PingPong", "Once" }) &&
                      (mode->EnumValues == std::vector<int32_t>{ 0, 1, 5 }));
                CHECK(speed && Near(speed->Min, 0.1f) && Near(speed->Max, 20.0f));
                if (points && mode && calls)
                {
                    auto instance = patrol->Create();
                    void* obj = patrol->ToObject(instance.get());
                    CHECK(FieldToJson(*points, obj, nullptr) == nlohmann::json::parse("[[0,0,0],[4,0,0],[4,0,4]]"));
                    CHECK(FieldToJson(*mode, obj, nullptr) == 1 && (FieldToJson(*calls, obj, nullptr) == nlohmann::json({ "Clear", "All quiet" })));
                    points->ListMove(points->Access(obj), 2, 0); // reordered in the Inspector
                    CHECK(FieldToJson(*points, obj, nullptr) == nlohmann::json::parse("[[4,0,4],[0,0,0],[4,0,0]]"));
                    FieldFromJson(*calls, obj, nlohmann::json({ "A", "B", "C" }), nullptr);
                    CHECK(calls->ListSize(calls->Access(obj)) == 3);
                    // The Inspector's Debug mode: the private fields, from a new script (no OnCreate yet).
                    Scene empty("Debug");
                    auto debug = CSharpRuntime::GetDebugFields(*instance, empty, empty.CreateEntity("Guard"));
                    auto debugValue = [](const std::vector<CSharpRuntime::DebugField>& fields, const char* name) -> std::string {
                        for (const auto& d : fields)
                            if (d.Name == name) return d.Type + " " + d.Value;
                        return "(missing)";
                    };
                    CHECK(debug.size() == 6 && debugValue(debug, "m_Origin") == "Vector3 (0, 0, 0)" && debugValue(debug, "m_Step") == "Int32 1" &&
                          debugValue(debug, "m_Finished") == "Boolean False" && debugValue(debug, "Points") == "(missing)");
                    // [ContextMenu] commands, run on the script: their changes land in its fields.
                    CHECK((CSharpRuntime::GetContextMenu("Game.Patrol") == std::vector<std::string>{ "Reverse Points", "Add Stop Ahead" }));
                    CHECK(CSharpRuntime::RunContextMenu(*instance, 0, empty, empty.CreateEntity("Guard 2")));
                    CHECK(FieldToJson(*points, obj, nullptr) == nlohmann::json::parse("[[4,0,0],[0,0,0],[4,0,4]]"));
                    CHECK(CSharpRuntime::RunContextMenu(*instance, 1, empty, empty.CreateEntity("Guard 3")));
                    CHECK(FieldToJson(*points, obj, nullptr) == nlohmann::json::parse("[[4,0,0],[0,0,0],[4,0,4],[4,0,2]]"));
                    CHECK(!CSharpRuntime::RunContextMenu(*instance, 7, empty, empty.CreateEntity("Guard 4"))); // no such item
                }
                Scene walk("Walk");
                Entity guard = walk.CreateEntity("Guard");
                walk.GetRegistry().Get<TransformComponent>(guard).Position = { 1.0f, 0.0f, 1.0f };
                auto& script = walk.GetRegistry().Emplace<ScriptComponent>(guard).Add("Game.Patrol");
                script.Fields["Points"] = nlohmann::json::parse("[[0,0,0],[2,0,0],[2,0,2]]");
                script.Fields["Mode"] = 5; // Once
                script.Fields["Speed"] = 4.0f;
                script.Fields["Calls"] = nlohmann::json::array();
                walk.UpdateTransforms();
                // Gizmos in the Scene view: the route from the saved values (two lines from where the guard stands),
                // and with it selected, a sphere (three 32-segment circles) at each stop.
                auto gizmos = CSharpRuntime::CollectGizmos(walk, {});
                CHECK(gizmos.size() == 2);
                if (gizmos.size() == 2)
                    CHECK(Near(gizmos[0].A, { 1, 0, 1 }) && Near(gizmos[0].B, { 3, 0, 1 }) && Near(gizmos[1].B, { 3, 0, 3 }) &&
                          Near(glm::vec3(gizmos[0].Color), { 1.0f, 0.8f, 0.2f }) && Near(gizmos[0].Color.a, 1.0f));
                gizmos = CSharpRuntime::CollectGizmos(walk, { guard });
                CHECK(gizmos.size() == 2 + 3 * 96);
                bool onSphere = gizmos.size() > 2;
                for (size_t g = 2; g < gizmos.size() && g < 2 + 96; ++g) onSphere &= Near(glm::length(gizmos[g].A - glm::vec3(1, 0, 1)), 0.3f);
                CHECK(onSphere);
                auto rt = walk.Clone();
                rt->OnRuntimeStart();
                Entity runner = rt->FindEntityByName("Guard");
                for (int i = 0; i < 15; ++i) rt->OnUpdateRuntime(1.0f / 60.0f); // 0.25 s: 1 m toward the second stop
                CHECK(Near(rt->GetRegistry().Get<TransformComponent>(runner).Position, { 2.0f, 0.0f, 1.0f }));
                for (int i = 0; i < 105; ++i) rt->OnUpdateRuntime(1.0f / 60.0f); // at the last stop, and staying (Once)
                CHECK(Near(rt->GetRegistry().Get<TransformComponent>(runner).Position, { 3.0f, 0.0f, 3.0f }));
                // While playing, the live script draws (the route stays where it started, not where the guard is).
                auto liveGizmos = CSharpRuntime::CollectGizmos(*rt, {});
                CHECK(liveGizmos.size() == 2 && Near(liveGizmos[0].A, { 1, 0, 1 }));
                // Debug mode while playing: the live script's state.
                if (auto& live = rt->GetRegistry().Get<ScriptComponent>(runner).Scripts[0].Instance)
                {
                    auto debug = CSharpRuntime::GetDebugFields(*live, *rt, runner);
                    bool finished = false, origin = false;
                    for (const auto& d : debug)
                    {
                        finished |= d.Name == "m_Finished" && d.Value == "True";
                        origin |= d.Name == "m_Origin" && d.Value == "(1, 0, 1)";
                    }
                    CHECK(finished && origin);
                }
                rt->OnRuntimeStop();
            }
        }
        {
            // [Serializable] types and [Flags] enums (WaveSpawner.cs): a class field registers as a struct of its
            // fields, a List of structs as a list of them (new elements start from the struct's defaults), the flags
            // enum as a multi-select. Defaults come from the class; values saved with the scene reach the running
            // script (each wave spawning at its sides), and its values come back unchanged.
            const ScriptInfo* spawner = ScriptRegistry::Find("Game.WaveSpawner");
            CHECK(spawner != nullptr);
            const FieldInfo* area = spawner ? fieldOf(spawner, "Area") : nullptr;
            const FieldInfo* waves = spawner ? fieldOf(spawner, "Waves") : nullptr;
            CHECK(area && area->Type == FieldType::Struct && area->Elements.size() == 2 && area->Elements[0].Type == FieldType::Vec3 &&
                  area->Elements[1].Tooltip == "How far out the sides are");
            CHECK(waves && waves->Type == FieldType::StructList && !IsValueList(*waves) && waves->Elements.size() == 4);
            if (spawner && area && waves && waves->Elements.size() == 4)
            {
                const FieldInfo& count = waves->Elements[1];
                const FieldInfo& sides = waves->Elements[3];
                CHECK(waves->Elements[0].Type == FieldType::Prefab && count.Type == FieldType::Int && Near(count.Min, 1.0f) && Near(count.Max, 20.0f));
                CHECK(sides.Type == FieldType::Enum && sides.EnumFlags && (sides.EnumValues == std::vector<int32_t>{ 0, 1, 2, 4, 8 }));
                auto instance = spawner->Create();
                void* obj = spawner->ToObject(instance.get());
                CHECK(FieldToJson(*area, obj, nullptr) == nlohmann::json::parse(R"({"Center":[0,0,0],"Radius":5})"));
                CHECK(FieldToJson(*waves, obj, nullptr) == nlohmann::json::parse(R"([{"Prefab":0,"Count":3,"Delay":1,"Sides":5}])"));
                void* list = waves->Access(obj);
                waves->ListResize(list, 2); // + Add
                waves->ListMove(list, 1, 0);
                CHECK(FieldToJson(*waves, obj, nullptr) ==
                      nlohmann::json::parse(R"([{"Prefab":0,"Count":0,"Delay":0,"Sides":0},{"Prefab":0,"Count":3,"Delay":1,"Sides":5}])"));
                FieldFromJson(*area, obj, nlohmann::json::parse(R"({"Radius":2})"), nullptr); // the rest kept
                CHECK(FieldToJson(*area, obj, nullptr) == nlohmann::json::parse(R"({"Center":[0,0,0],"Radius":2})"));
                // Its gizmos (only when selected), drawn through Gizmos.Matrix: a ring of the area's radius around it,
                // and a box at each side its waves use (north and south by default).
                Scene arena("Arena");
                Entity spot = arena.CreateEntity("Spawner");
                arena.GetRegistry().Get<TransformComponent>(spot).Position = { 10.0f, 0.0f, 0.0f };
                arena.GetRegistry().Emplace<ScriptComponent>(spot).Add("Game.WaveSpawner");
                arena.UpdateTransforms();
                CHECK(CSharpRuntime::CollectGizmos(arena, {}).empty());
                auto lines = CSharpRuntime::CollectGizmos(arena, { spot });
                CHECK(lines.size() == 48 + 2 * 12);
                bool flat = true, north = false, south = false;
                for (size_t g = 0; g < lines.size(); ++g)
                {
                    const auto& line = lines[g];
                    if (g < 48) flat &= Near(line.A.y, 0.0f) && Near(glm::length(glm::vec2(line.A.x - 10.0f, line.A.z)), 5.0f);
                    else
                    {
                        north |= Near(line.A.z, -5.5f) && line.A.x >= 9.4f && line.A.x <= 10.6f;
                        south |= Near(line.A.z, 4.5f) && line.A.x >= 9.4f && line.A.x <= 10.6f;
                    }
                }
                CHECK(flat && north && south);
            }
            AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
            AssetHandle ball = AssetManager::GetHandle(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Prefabs/Ball.prefab");
            if (spawner && waves)
            {
                Scene field("Field");
                Entity host = field.CreateEntity("Spawner");
                auto& script = field.GetRegistry().Emplace<ScriptComponent>(host).Add("Game.WaveSpawner");
                script.Fields["Area"] = nlohmann::json::parse(R"({"Center":[0,0,0],"Radius":4})");
                nlohmann::json saved = nlohmann::json::parse(R"([{"Count":2,"Delay":0.25,"Sides":3},{"Count":1,"Delay":0.25,"Sides":8}])");
                for (auto& w : saved) w["Prefab"] = (uint64_t)ball;
                script.Fields["Waves"] = saved;
                field.UpdateTransforms();
                auto rt = field.Clone();
                rt->OnRuntimeStart();
                for (int i = 0; i < 36; ++i) rt->OnUpdateRuntime(1.0f / 60.0f); // 0.6 s: both waves
                int north = 0, east = 0, west = 0, total = 0;
                rt->GetRegistry().EachEntity([&](Entity e) {
                    auto* link = rt->GetRegistry().TryGet<PrefabLinkComponent>(e);
                    if (!link || link->Prefab != ball || !link->IsRoot) return;
                    glm::vec3 p = rt->GetRegistry().Get<TransformComponent>(e).Position;
                    ++total;
                    north += p.z < -3.0f;
                    east += p.x > 3.0f;
                    west += p.x < -3.0f;
                });
                CHECK(total == 5 && north == 2 && east == 2 && west == 1);
                // The live script's values back into the native fields (as before a save while playing).
                auto& live = rt->GetRegistry().Get<ScriptComponent>(rt->FindEntityByName("Spawner")).Scripts[0];
                if (live.Instance)
                {
                    live.Instance->OnBeforeSerialize();
                    void* obj = spawner->ToObject(live.Instance.get());
                    CHECK(FieldToJson(*waves, obj, nullptr) == saved);
                    CHECK(FieldToJson(*area, obj, nullptr) == script.Fields["Area"]);
                }
                rt->OnRuntimeStop();
            }
            AssetManager::Shutdown();
        }
        {
            // A Prefab field at play time: the dropper instantiates the Ball prefab picked for it, every Interval.
            AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
            AssetHandle ball = AssetManager::GetHandle(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Prefabs/Ball.prefab");
            CHECK(ball.IsValid());
            Scene drops("Drops");
            Entity spot = drops.CreateEntity("Spot");
            drops.GetRegistry().Get<TransformComponent>(spot).Position = { 5.0f, 0.0f, 0.0f };
            auto& script = drops.GetRegistry().Emplace<ScriptComponent>(spot).Add("Game.PrefabDropper");
            script.Fields["Prefab"] = (uint64_t)ball;
            script.Fields["Interval"] = 0.5f;
            script.Fields["Chance"] = 1.0f;
            drops.UpdateTransforms();
            auto rt = drops.Clone();
            rt->OnRuntimeStart();
            for (int i = 0; i < 66; ++i) rt->OnUpdateRuntime(1.0f / 60.0f); // 1.1 s: two drops
            int balls = 0;
            glm::vec3 at{ 0.0f };
            rt->GetRegistry().EachEntity([&](Entity e) {
                auto* link = rt->GetRegistry().TryGet<PrefabLinkComponent>(e);
                if (link && link->Prefab == ball && link->IsRoot)
                {
                    ++balls;
                    at = rt->GetRegistry().Get<TransformComponent>(e).Position;
                }
            });
            CHECK(balls == 2);
            CHECK(Near(at.x, 5.0f)); // above the spot (then falling)
            rt->OnRuntimeStop();
            AssetManager::Shutdown();
        }
        {
            // [ExecuteInEditMode] (LookAt.cs): in the edited scene the script runs on its own instance, created on the
            // first update and turning the object toward its target; edited values reach it; it ends when its script
            // goes; play mode never runs it (nor the copy of the scene).
            Scene edit("Edit Mode");
            auto& reg = edit.GetRegistry();
            Entity viewer = edit.CreateEntity("Viewer");
            Entity east = edit.CreateEntity("East"), west = edit.CreateEntity("West");
            reg.Get<TransformComponent>(east).Position = { 5.0f, 0.0f, 0.0f };
            reg.Get<TransformComponent>(west).Position = { -5.0f, 0.0f, 0.0f };
            reg.Emplace<ScriptComponent>(viewer).Add("Game.LookAt").Fields["Target"] = (uint64_t)edit.GetGuid(east);
            edit.UpdateTransforms();
            CHECK(ScriptRegistry::Find("Game.LookAt") && ScriptRegistry::Find("Game.LookAt")->ExecuteInEditMode &&
                  !ScriptRegistry::Find("Game.Patrol")->ExecuteInEditMode);
            CHECK(EditModeScripts::Count(&edit) == 0);
            EditModeScripts::Update(edit, 1.0f / 60.0f);
            CHECK(EditModeScripts::Count(&edit) == 1 && reg.Get<ScriptComponent>(viewer).Scripts[0].Instance == nullptr);
            CHECK(Near(reg.Get<TransformComponent>(viewer).Rotation.y, -glm::half_pi<float>())); // facing +X
            reg.Get<TransformComponent>(east).Position = { 0.0f, 0.0f, 5.0f };
            edit.UpdateTransforms();
            EditModeScripts::Update(edit, 1.0f / 60.0f);
            CHECK(Near(std::abs(reg.Get<TransformComponent>(viewer).Rotation.y), glm::pi<float>())); // follows it (+Z)
            reg.Get<ScriptComponent>(viewer).Scripts[0].Fields["Target"] = (uint64_t)edit.GetGuid(west); // edited
            EditModeScripts::Update(edit, 1.0f / 60.0f);
            CHECK(EditModeScripts::Count(&edit) == 1 && Near(reg.Get<TransformComponent>(viewer).Rotation.y, glm::half_pi<float>()));
            {
                auto rt = edit.Clone();
                rt->OnRuntimeStart();
                EditModeScripts::Update(*rt, 1.0f / 60.0f);
                CHECK(EditModeScripts::Count(rt.get()) == 0 && rt->GetRegistry().Get<ScriptComponent>(rt->FindEntityByName("Viewer")).Scripts[0].Instance);
                rt->OnRuntimeStop();
            }
            reg.Get<ScriptComponent>(viewer).Scripts[0].Enabled = false;
            EditModeScripts::Update(edit, 1.0f / 60.0f);
            CHECK(EditModeScripts::Count(&edit) == 0);
            reg.Get<ScriptComponent>(viewer).Scripts[0].Enabled = true;
            EditModeScripts::Update(edit, 1.0f / 60.0f);
            CHECK(EditModeScripts::Count(&edit) == 1);
            EditModeScripts::Clear(&edit);
            CHECK(EditModeScripts::Count() == 0);
            EditModeScripts::Update(edit, 1.0f / 60.0f);
            CHECK(EditModeScripts::Count() == 1);
        }
        CHECK(EditModeScripts::Count() == 0); // the scene went, and its edit-mode scripts with it
        {
            // Handles (RouteInspectors.cs): Inspectors with OnSceneGUI only keep the script's fields; through stand-in
            // hooks, dragging a stop's dot moves that stop, and the radius handle sets the area's radius.
            const CSharpRuntime::EditorItem* patrolHandles = CSharpRuntime::FindInspector("Game.Patrol");
            const CSharpRuntime::EditorItem* spawnerHandles = CSharpRuntime::FindInspector("Game.WaveSpawner");
            const CSharpRuntime::EditorItem* spinnerInspector = CSharpRuntime::FindInspector("Game.Spinner");
            CHECK(patrolHandles && patrolHandles->SceneGUI && !patrolHandles->InspectorGUI && spawnerHandles && spawnerHandles->SceneGUI);
            CHECK(spinnerInspector && spinnerInspector->InspectorGUI && !spinnerInspector->SceneGUI);
            const ScriptInfo* patrol = ScriptRegistry::Find("Game.Patrol");
            const ScriptInfo* spawner = ScriptRegistry::Find("Game.WaveSpawner");
            if (patrolHandles && spawnerHandles && patrol && spawner)
            {
                Scene handles("Handles");
                Entity guard = handles.CreateEntity("Guard");
                handles.GetRegistry().Get<TransformComponent>(guard).Position = { 1.0f, 0.0f, 1.0f };
                handles.UpdateTransforms();
                std::vector<std::string> drawn;
                CSharpRuntime::EditorHooks hooks;
                hooks.Handle = [&](int kind, float* v, int n, const char* text) {
                    drawn.push_back(std::to_string(kind) + (*text ? std::string(":") + text : std::string()));
                    if (kind == 1 && n >= 3 && Near(glm::vec3(v[0], v[1], v[2]), { 5.0f, 0.0f, 1.0f })) v[2] = 3.0f; // stop 1 dragged
                    if (kind == 2 && n >= 4) v[3] = 7.0f;
                    return 0;
                };
                CSharpRuntime::SetEditorHooks(hooks);
                auto route = patrol->Create();
                const FieldInfo* points = fieldOf(patrol, "Points");
                CHECK(CSharpRuntime::SceneGUIScript(patrolHandles->Id, *route, handles, guard));
                CHECK(points && FieldToJson(*points, patrol->ToObject(route.get()), nullptr) == nlohmann::json::parse("[[0,0,0],[4,0,2],[4,0,4]]"));
                CHECK((drawn == std::vector<std::string>{ "1", "3:Stop 0", "1", "3:Stop 1", "1", "3:Stop 2" }));
                auto area = spawner->Create();
                const FieldInfo* areaField = fieldOf(spawner, "Area");
                CHECK(CSharpRuntime::SceneGUIScript(spawnerHandles->Id, *area, handles, guard));
                CHECK(areaField && FieldToJson(*areaField, spawner->ToObject(area.get()), nullptr) == nlohmann::json::parse(R"({"Center":[0,0,0],"Radius":7})"));
                CSharpRuntime::SetEditorHooks({});
            }
        }
        Scene kept("Kept"); // an edit-mode script still running when the scripts unload
        kept.GetRegistry().Emplace<ScriptComponent>(kept.CreateEntity("Looker")).Add("Game.LookAt");
        EditModeScripts::Update(kept, 1.0f / 60.0f);
        CHECK(EditModeScripts::Count() == 1);
        CSharpRuntime::UnloadScripts();
        CHECK(EditModeScripts::Count() == 0); // ended before their classes went
        CHECK(ScriptRegistry::Find("Game.Spinner") == nullptr && CSharpRuntime::GetEditorItems().empty());
    }

    // Scene view snapping: grid rounding (only the axes asked), rays onto mesh surfaces (skipping the objects being
    // moved), how far an object reaches below its pivot (to rest it on a surface, children and rotation included),
    // and the vertex nearest a point on screen.
    void TestSceneSnapping()
    {
        CHECK(Near(SceneSnapping::ToGrid({ 0.26f, 1.74f, -0.3f }, 0.5f), { 0.5f, 1.5f, -0.5f }));
        CHECK(Near(SceneSnapping::ToGrid({ 0.26f, 1.74f, -0.3f }, 0.5f, { true, false, true }), { 0.5f, 1.74f, -0.5f }));
        CHECK(Near(SceneSnapping::ToGrid({ 0.26f, 1.74f, -0.3f }, 0.0f), { 0.26f, 1.74f, -0.3f }));
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets"); // CPU mesh data only

        Scene scene("Snapping");
        auto& reg = scene.GetRegistry();
        auto cube = [&](const char* name, glm::vec3 position, glm::vec3 scale) {
            Entity e = scene.CreateEntity(name);
            auto& t = reg.Get<TransformComponent>(e);
            t.Position = position;
            t.Scale = scale;
            reg.Emplace<MeshRendererComponent>(e).Mesh = AssetManager::kCube;
            return e;
        };
        Entity ground = cube("Ground", { 0.0f, -0.5f, 0.0f }, { 10.0f, 1.0f, 10.0f }); // its top at y = 0
        Entity box = cube("Box", { 0.0f, 5.0f, 0.0f }, glm::vec3(1.0f));
        scene.UpdateTransforms();

        auto hit = SceneSnapping::Raycast(scene, { 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f });
        CHECK(hit.Hit && hit.Object == box && Near(hit.Point, { 0.0f, 5.5f, 0.0f }) && Near(hit.Distance, 4.5f));
        hit = SceneSnapping::Raycast(scene, { 2.0f, 10.0f, 1.0f }, { 0.0f, -2.0f, 0.0f }, { box });
        CHECK(hit.Hit && hit.Object == ground && Near(hit.Point, { 2.0f, 0.0f, 1.0f }) && Near(hit.Normal, { 0.0f, 1.0f, 0.0f }));
        hit = SceneSnapping::Raycast(scene, { 20.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f });
        CHECK(!hit.Hit);
        // From the side: the ground's face toward the ray.
        hit = SceneSnapping::Raycast(scene, { 20.0f, -0.5f, 0.0f }, { -1.0f, 0.0f, 0.0f });
        CHECK(hit.Hit && hit.Object == ground && Near(hit.Point, { 5.0f, -0.5f, 0.0f }) && Near(hit.Normal, { 1.0f, 0.0f, 0.0f }));

        CHECK(Near(SceneSnapping::RestOffset(scene, box, { 0.0f, 1.0f, 0.0f }), 0.5f));
        reg.Get<TransformComponent>(box).Rotation = { 0.0f, 0.0f, glm::radians(45.0f) };
        scene.UpdateTransforms();
        CHECK(Near(SceneSnapping::RestOffset(scene, box, { 0.0f, 1.0f, 0.0f }), std::sqrt(0.5f)));
        reg.Get<TransformComponent>(box).Rotation = glm::vec3(0.0f);
        Entity parent = scene.CreateEntity("Parent");
        Entity child = cube("Child", { 0.0f, -1.0f, 0.0f }, glm::vec3(1.0f));
        scene.SetParent(child, parent);
        reg.Get<TransformComponent>(parent).Position = { 3.0f, 4.0f, 0.0f };
        scene.UpdateTransforms();
        CHECK(Near(SceneSnapping::RestOffset(scene, parent, { 0.0f, 1.0f, 0.0f }), 1.5f));
        CHECK(Near(SceneSnapping::RestOffset(scene, scene.CreateEntity("Empty"), { 0.0f, 1.0f, 0.0f }), 0.0f));

        // Looking down -Z at the box (moved to the origin): its corner (0.5, 0.5, 0.5) on screen.
        reg.Get<TransformComponent>(box).Position = glm::vec3(0.0f);
        reg.Get<TransformComponent>(ground).Position = { 0.0f, -50.0f, 0.0f };
        reg.Get<TransformComponent>(parent).Position = { 0.0f, 50.0f, 0.0f };
        scene.UpdateTransforms();
        glm::mat4 viewProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) *
                             glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec2 viewport(800.0f, 800.0f);
        glm::vec4 clip = viewProj * glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
        glm::vec2 corner((clip.x / clip.w * 0.5f + 0.5f) * viewport.x, (0.5f - clip.y / clip.w * 0.5f) * viewport.y);
        CHECK(corner.x > 400.0f && corner.y < 400.0f); // right of and above the middle
        auto vertex = SceneSnapping::NearestVertex(scene, viewProj, viewport, corner + glm::vec2(3.0f, -2.0f), 20.0f);
        CHECK(vertex.Hit && vertex.Object == box && Near(vertex.Position, { 0.5f, 0.5f, 0.5f }) && Near(vertex.Pixels, std::sqrt(13.0f)));
        CHECK(!SceneSnapping::NearestVertex(scene, viewProj, viewport, corner + glm::vec2(3.0f, -2.0f), 2.0f).Hit);
        CHECK(!SceneSnapping::NearestVertex(scene, viewProj, viewport, corner, 20.0f, {}, { box }).Hit);
        CHECK(!SceneSnapping::NearestVertex(scene, viewProj, viewport, corner, 20.0f, { parent }).Hit);
        vertex = SceneSnapping::NearestVertex(scene, viewProj, viewport, corner, 20.0f, { box });
        CHECK(vertex.Hit && Near(vertex.Position, { 0.5f, 0.5f, 0.5f }));
        AssetManager::Shutdown();
    }

    // Scene visibility: hiding an object hides its children (its own setting stays its own), the toggle shows
    // everything without losing the settings, settings are saved per scene, and they follow the objects into a copy of
    // the scene (play mode) by GUID.
    void TestSceneVisibility()
    {
        Scene scene("Visibility");
        Entity parent = scene.CreateEntity("Parent"), child = scene.CreateEntity("Child"), other = scene.CreateEntity("Other");
        scene.SetParent(child, parent);
        SceneVisibility vis;
        CHECK(vis.Empty() && vis.Hidden(scene).empty());
        vis.SetHidden(scene, parent, true);
        CHECK(vis.IsHiddenSelf(scene, parent) && vis.IsHidden(scene, child) && !vis.IsHiddenSelf(scene, child) && !vis.IsHidden(scene, other));
        auto hidden = vis.Hidden(scene);
        CHECK(hidden.size() == 2 && std::find(hidden.begin(), hidden.end(), child) != hidden.end());
        vis.SetPickable(scene, other, false);
        CHECK(vis.IsUnpickable(scene, other) && !vis.IsUnpickable(scene, child) && vis.Unpickable(scene) == std::vector<Entity>{ other });
        vis.Enabled = false;
        CHECK(vis.Hidden(scene).empty() && vis.Unpickable(scene).empty() && vis.IsHiddenSelf(scene, parent));
        vis.Enabled = true;

        auto copy = scene.Clone();
        auto copied = vis.Hidden(*copy);
        CHECK(copied.size() == 2 && std::find(copied.begin(), copied.end(), copy->FindEntityByName("Child")) != copied.end());

        fs::path file = fs::temp_directory_path() / "dimension_visibility" / "SceneVisibility.json";
        std::error_code ec;
        fs::remove_all(file.parent_path(), ec);
        vis.Save(file, "Scenes/A.scene");
        SceneVisibility other2;
        other2.Save(file, "Scenes/B.scene"); // nothing set: no entry, A's kept
        SceneVisibility loaded;
        loaded.Load(file, "Scenes/A.scene");
        CHECK(loaded.IsHidden(scene, child) && loaded.IsUnpickable(scene, other) && loaded.ToJson() == vis.ToJson());
        loaded.Load(file, "Scenes/B.scene");
        CHECK(loaded.Empty());
        vis.SetHidden(scene, parent, false);
        vis.SetPickable(scene, other, true);
        CHECK(vis.Empty());
        // Isolation: only the isolated object and its children show; ending it brings the rest back.
        vis.Isolate(scene, { parent });
        CHECK(vis.Isolating() && vis.IsHidden(scene, other) && !vis.IsHidden(scene, child));
        hidden = vis.Hidden(scene);
        CHECK(hidden.size() == 1 && hidden[0] == other);
        vis.Enabled = false;
        CHECK(vis.Hidden(scene).empty());
        vis.Enabled = true;
        vis.EndIsolation();
        CHECK(!vis.Isolating() && vis.Hidden(scene).empty());
        vis.Save(file, "Scenes/A.scene"); // cleared: its entry goes
        loaded.Load(file, "Scenes/A.scene");
        CHECK(loaded.Empty());
        fs::remove_all(file.parent_path(), ec);
    }

    // Presets: a component's settings captured into a .preset asset, listed per component type, applied to another
    // object (only the fields the preset has), re-read when the file changes, and a project default per component type
    // that is kept in the project file.
    void TestPresets()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_presets";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        AssetManager::Init(dir / "Assets");
        Scene scene("Presets");
        auto& reg = scene.GetRegistry();
        Entity warm = scene.CreateEntity("Warm"), plain = scene.CreateEntity("Plain"), empty = scene.CreateEntity("Empty");
        auto& light = reg.Emplace<PointLightComponent>(warm);
        light.Intensity = 3.0f;
        light.Range = 20.0f;
        light.Color = { 1.0f, 0.5f, 0.2f };
        reg.Emplace<PointLightComponent>(plain);
        CHECK(!Preset::Capture(scene, empty, "Point Light"));
        auto captured = Preset::Capture(scene, warm, "Point Light");
        CHECK(captured && captured->Component == "Point Light" && captured->Fields["Intensity"] == 3.0f && !captured->Fields.contains("Nope"));
        fs::create_directories(dir / "Assets/Presets", ec);
        fs::path file = dir / "Assets/Presets/Warm Light.preset";
        CHECK(captured && captured->Save(file));
        AssetHandle h = AssetManager::Import(file);
        CHECK(h.IsValid() && AssetManager::GetMetadata(h) && AssetManager::GetMetadata(h)->Type == AssetType::Preset);
        CHECK(Presets::For("Point Light") == std::vector<AssetHandle>{ h } && Presets::For("Rigidbody").empty());

        auto preset = Presets::Get(h);
        CHECK(preset && preset->ApplyTo(scene, plain) && !preset->ApplyTo(scene, empty));
        CHECK(Near(reg.Get<PointLightComponent>(plain).Intensity, 3.0f) && Near(reg.Get<PointLightComponent>(plain).Range, 20.0f) &&
              Near(reg.Get<PointLightComponent>(plain).Color, { 1.0f, 0.5f, 0.2f }));
        Preset rangeOnly{ "Point Light", { { "Range", 7.0f } } };
        CHECK(rangeOnly.ApplyTo(scene, plain) && Near(reg.Get<PointLightComponent>(plain).Range, 7.0f) &&
              Near(reg.Get<PointLightComponent>(plain).Intensity, 3.0f)); // the rest kept

        // Edited on disk: read again.
        Preset brighter = *preset;
        brighter.Fields["Intensity"] = 5.0f;
        brighter.Save(file);
        fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
        CHECK(Presets::Get(h) && Presets::Get(h)->Fields["Intensity"] == 5.0f);

        // The project default for new Point Lights, kept in the project file.
        CHECK(!Presets::DefaultFor("Point Light").IsValid());
        Presets::SetDefault("Point Light", h);
        CHECK(Project::Save() && Project::Open(dir) && Presets::DefaultFor("Point Light") == h);
        Entity added = scene.CreateEntity("Added");
        reg.Emplace<PointLightComponent>(added);
        CHECK(Presets::ApplyDefault(scene, added, "Point Light") && Near(reg.Get<PointLightComponent>(added).Intensity, 5.0f));
        CHECK(!Presets::ApplyDefault(scene, added, "Spot Light"));
        Presets::SetDefault("Point Light", AssetHandle::None());
        CHECK(!Presets::ApplyDefault(scene, added, "Point Light"));
        AssetManager::Shutdown();
        fs::remove_all(dir, ec);
    }

    // Import defaults per folder (new textures and models there get a rule's import settings; the deepest folder's rule
    // wins; a .meta's settings are kept), applying a rule to the assets already there, and batch import settings.
    void TestImportDefaults()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_import_defaults";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        fs::create_directories(dir / "Assets/Textures/Pixel/UI", ec);
        fs::create_directories(dir / "Assets/Textures/PixelArt", ec);
        fs::create_directories(dir / "Assets/Models", ec);
        AssetManager::Init(dir / "Assets");
        fs::path src = fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets";
        auto add = [&](const char* from, const char* to) {
            fs::copy_file(src / from, dir / "Assets" / to, fs::copy_options::overwrite_existing, ec);
            return AssetManager::Import(dir / "Assets" / to);
        };
        auto setting = [](AssetHandle h, const char* key) {
            const AssetMetadata* m = AssetManager::GetMetadata(h);
            return m && m->ImportSettings.contains(key) ? m->ImportSettings[key] : nlohmann::json();
        };
        CHECK(ImportDefaults::Keys(AssetType::Texture).size() == 3 && ImportDefaults::Keys(AssetType::Mesh).size() == 3 &&
              ImportDefaults::Keys(AssetType::Audio).empty());

        AssetHandle brick = add("Textures/Brick.png", "Textures/Brick.png");
        CHECK(setting(brick, "filter") == 0 && setting(brick, "mips") == true);

        // Only import keys are kept ("sprites" and "labels" are per asset); the folder is normalized; set again: replaced.
        ImportDefaults::Set("Textures/Pixel/", AssetType::Texture,
                            { { "filter", 0 }, { "labels", nlohmann::json::array({ "x" }) }, { "sprites", nlohmann::json::array() } });
        ImportDefaults::Set("Textures/Pixel", AssetType::Texture, { { "filter", 1 }, { "mips", false } });
        ImportDefaults::Set("./Textures/Pixel/UI", AssetType::Texture, { { "srgb", false } });
        auto& rules = Project::GetConfig().ImportRules;
        CHECK(rules.size() == 2 && rules[0].Folder == "Textures/Pixel" && rules[1].Folder == "Textures/Pixel/UI");
        CHECK(rules[0].Settings == nlohmann::json({ { "filter", 1 }, { "mips", false } }));

        CHECK(!ImportDefaults::For("Textures/Brick.png", AssetType::Texture));
        CHECK(!ImportDefaults::For("Textures/PixelArt/A.png", AssetType::Texture)); // a folder name, not a prefix
        CHECK(!ImportDefaults::For("Textures/Pixel/A.png", AssetType::Mesh));
        CHECK(ImportDefaults::For("Textures/Pixel/A.png", AssetType::Texture) == &rules[0]);
        CHECK(ImportDefaults::For("Textures/Pixel/UI/Deep/A.png", AssetType::Texture) == &rules[1]);

        AssetHandle hero = add("Textures/Brick.png", "Textures/Pixel/Hero.png");
        AssetHandle button = add("Textures/Brick.png", "Textures/Pixel/UI/Button.png");
        AssetHandle art = add("Textures/Brick.png", "Textures/PixelArt/Art.png");
        CHECK(setting(hero, "filter") == 1 && setting(hero, "mips") == false && setting(hero, "srgb") == true);
        CHECK(setting(button, "srgb") == false && setting(button, "filter") == 0); // the deepest rule alone
        CHECK(setting(art, "filter") == 0);

        // A .meta's settings are the asset's own: a changed rule does not change them on the next import.
        ImportDefaults::Set("Textures/Pixel", AssetType::Texture, { { "filter", 0 }, { "mips", true } });
        AssetManager::Shutdown();
        AssetManager::Init(dir / "Assets");
        hero = AssetManager::GetHandle("Textures/Pixel/Hero.png");
        CHECK(setting(hero, "filter") == 1 && setting(hero, "mips") == false);

        // Apply to the assets already in the folder: Hero (Button is under its own rule).
        CHECK(ImportDefaults::ApplyToExisting("Textures/Pixel", AssetType::Texture) == 1);
        CHECK(setting(hero, "filter") == 0 && setting(hero, "mips") == true);
        CHECK(setting(AssetManager::GetHandle("Textures/Pixel/UI/Button.png"), "srgb") == false);
        CHECK(ImportDefaults::ApplyToExisting("Textures/Pixel", AssetType::Texture) == 0); // already so

        // Batch: one setting on several assets, their other settings kept, written to their .meta files.
        brick = AssetManager::GetHandle("Textures/Brick.png");
        button = AssetManager::GetHandle("Textures/Pixel/UI/Button.png");
        CHECK(ImportDefaults::ApplySettings({ brick, hero, button }, { { "filter", 1 } }) == 3);
        CHECK(ImportDefaults::ApplySettings({ brick, hero, button }, { { "filter", 1 } }) == 0);
        CHECK(setting(brick, "filter") == 1 && setting(button, "filter") == 1 && setting(button, "srgb") == false && setting(brick, "srgb") == true);
        {
            std::ifstream meta(dir / "Assets/Textures/Pixel/UI/Button.png.meta");
            nlohmann::json j = nlohmann::json::parse(meta, nullptr, false);
            CHECK(!j.is_discarded() && j["settings"].value("filter", -1) == 1 && j["settings"].value("srgb", true) == false);
        }
        CHECK(ImportDefaults::ApplySettings({ AssetHandle::None() }, { { "filter", 0 } }) == 0);

        // Models: the rule's scale.
        ImportDefaults::Set("Models", AssetType::Mesh, { { "scale", 0.01f }, { "excludedClips", nlohmann::json::array({ "Idle" }) } });
        AssetHandle torus = add("Models/Torus.obj", "Models/Torus.obj");
        CHECK(Near(AssetManager::GetMeshImportSettings(torus).Scale, 0.01f) && !setting(torus, "excludedClips").is_array());

        // Kept in the project file.
        CHECK(Project::Save() && Project::Open(dir));
        CHECK(Project::GetConfig().ImportRules.size() == 3);
        const ImportRule* models = ImportDefaults::For("Models/Sub/A.fbx", AssetType::Mesh);
        CHECK(models && models->Folder == "Models" && Near(models->Settings.value("scale", 1.0f), 0.01f));
        ImportDefaults::Remove("Textures/Pixel/UI/", AssetType::Texture);
        ImportDefaults::Remove("Models", AssetType::Texture); // not a texture rule: kept
        CHECK(Project::GetConfig().ImportRules.size() == 2 && ImportDefaults::For("Textures/Pixel/UI/A.png", AssetType::Texture)->Folder == "Textures/Pixel");
        AssetManager::Shutdown();
        fs::remove_all(dir, ec);
    }

    // Asset references: what an asset uses and what uses it, and Find References In Scene (the Hierarchy's ref: search:
    // objects using an asset directly or through a material; a prefab instance's link alone does not count).
    void TestAssetReferences()
    {
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
        AssetHandle brickTex = AssetManager::GetHandle("Textures/Brick.png");
        AssetHandle brickNormal = AssetManager::GetHandle("Textures/Brick_Normal.png");
        AssetHandle checker = AssetManager::GetHandle("Textures/Checker.png");
        AssetHandle brickMat = AssetManager::GetHandle("Materials/Brick.mat");
        AssetHandle gold = AssetManager::GetHandle("Materials/Gold.mat");
        AssetHandle ball = AssetManager::GetHandle("Prefabs/Ball.prefab");
        CHECK(brickTex.IsValid() && brickNormal.IsValid() && checker.IsValid() && brickMat.IsValid() && gold.IsValid() && ball.IsValid());

        auto has = [](const std::vector<AssetHandle>& list, AssetHandle h) { return std::find(list.begin(), list.end(), h) != list.end(); };
        auto uses = AssetDependencies::Direct(brickMat);
        CHECK(has(uses, brickTex) && has(uses, brickNormal) && !has(uses, brickMat) && !has(uses, checker));
        CHECK(AssetDependencies::Direct(brickTex).empty() && AssetDependencies::Direct(AssetHandle::None()).empty());
        auto users = AssetDependencies::UsedBy(brickTex);
        CHECK(has(users, brickMat) && !has(users, gold) && !has(users, brickTex));
        CHECK(AssetDependencies::UsedBy(AssetHandle::None()).empty());
        CHECK(AssetDependencies::References({ { "a", (uint64_t)brickTex }, { "b", { (uint64_t)brickTex, (uint64_t)gold, 7, "x" } } }) ==
              std::vector<AssetHandle>({ brickTex, gold }));

        Scene scene("References");
        auto& reg = scene.GetRegistry();
        Entity wall = scene.CreateEntity("Wall"), shiny = scene.CreateEntity("Shiny"), floor = scene.CreateEntity("Floor");
        Entity empty = scene.CreateEntity("Empty"), instance = scene.CreateEntity("Ball Instance");
        Entity slot = scene.CreateEntity("Slots", wall);
        reg.Emplace<MeshRendererComponent>(wall).Material = brickMat;
        reg.Emplace<MeshRendererComponent>(shiny).Material = gold;
        reg.Emplace<MeshRendererComponent>(floor).Texture = checker;
        reg.Emplace<MeshRendererComponent>(slot).Materials = { AssetHandle::None(), brickMat };
        auto& link = reg.Emplace<PrefabLinkComponent>(instance);
        link.Prefab = ball;
        link.IsRoot = true;

        auto names = [&](const std::string& query) {
            std::vector<std::string> out;
            for (Entity e : SceneSearch::Find(scene, query)) out.push_back(reg.Get<TagComponent>(e).Name);
            return out;
        };
        using Names = std::vector<std::string>;
        std::string brickQuery = SceneSearch::ReferenceQuery(brickTex);
        CHECK(brickQuery == "ref:" + std::to_string((uint64_t)brickTex) + ":Brick.png");
        CHECK(names(brickQuery) == Names({ "Wall", "Slots" })); // through Brick.mat (a material slot too)
        CHECK(names(SceneSearch::ReferenceQuery(brickNormal)) == Names({ "Wall", "Slots" }));
        CHECK(names(SceneSearch::ReferenceQuery(brickMat)) == Names({ "Wall", "Slots" }));
        CHECK(names(SceneSearch::ReferenceQuery(gold)) == Names({ "Shiny" }));
        CHECK(names(SceneSearch::ReferenceQuery(checker)) == Names({ "Floor" }));
        CHECK(names(SceneSearch::ReferenceQuery(ball)) == Names({ "Ball Instance" }));
        CHECK(names(brickQuery + " slot") == Names({ "Slots" })); // with other words
        CHECK(names("ref:" + std::to_string((uint64_t)brickTex)) == Names({ "Wall", "Slots" })); // the name is optional
        CHECK(names("ref:nope").empty() && names("ref:12345").empty());
        CHECK(SceneSearch::Matches(scene, wall, brickQuery) && !SceneSearch::Matches(scene, empty, brickQuery));
        // A name with spaces stays one search word.
        std::string hull = SceneSearch::ReferenceQuery(AssetManager::GetHandle("Materials/Boat Hull.mat"));
        CHECK(hull.find(' ') == std::string::npos && hull.find("Boat_Hull.mat") != std::string::npos);
        AssetManager::Shutdown();
    }

    // The Project window's list view (rows described and sorted by name, type, size and date, folders first) and its
    // Favorites (folders and saved searches, kept in the project's .dimension folder).
    void TestProjectListAndFavorites()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_project_list";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        fs::path assets = dir / "Assets";
        fs::create_directories(assets / "Zeta", ec);
        fs::create_directories(assets / "alpha/Inner", ec);
        fs::path src = fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets";
        fs::copy_file(src / "Textures/Brick.png", assets / "brick.png", ec);
        fs::copy_file(src / "Textures/Checker.png", assets / "Checker.png", ec);
        fs::copy_file(src / "Materials/Gold.mat", assets / "Gold.mat", ec);
        std::ofstream(assets / "notes.txt") << "hello";
        AssetManager::Init(assets);
        auto now = fs::file_time_type::clock::now();
        fs::last_write_time(assets / "brick.png", now - std::chrono::hours(48));
        fs::last_write_time(assets / "Checker.png", now - std::chrono::hours(1));
        fs::last_write_time(assets / "Gold.mat", now - std::chrono::hours(24));
        fs::last_write_time(assets / "notes.txt", now - std::chrono::hours(72));

        std::vector<AssetListEntry> rows;
        for (const char* name : { "Gold.mat", "Zeta", "notes.txt", "brick.png", "alpha", "Checker.png" })
            rows.push_back(AssetListing::Describe(assets / name));
        const AssetListEntry& brick = rows[3];
        CHECK(brick.Name == "brick" && !brick.Folder && brick.Type == AssetType::Texture && brick.Handle.IsValid() &&
              brick.Size == fs::file_size(src / "Textures/Brick.png"));
        CHECK(rows[1].Folder && rows[1].Size == 0 && AssetListing::TypeName(rows[1]) == "Folder");
        CHECK(AssetListing::TypeName(rows[2]) == "File" && rows[2].Size == 5 && AssetListing::TypeName(rows[0]) == "Material");

        auto order = [&](AssetSort by, bool ascending) {
            AssetListing::Sort(rows, by, ascending);
            std::string out;
            for (const auto& r : rows) out += (out.empty() ? "" : ",") + r.Name;
            return out;
        };
        CHECK(order(AssetSort::Name, true) == "alpha,Zeta,brick,Checker,Gold,notes"); // case ignored, folders first
        CHECK(order(AssetSort::Name, false) == "alpha,Zeta,notes,Gold,Checker,brick"); // folders keep their order
        CHECK(order(AssetSort::Type, true) == "alpha,Zeta,notes,Gold,brick,Checker"); // File, Material, Texture (ties by name)
        CHECK(order(AssetSort::Type, false) == "alpha,Zeta,brick,Checker,Gold,notes");
        CHECK(order(AssetSort::Modified, true) == "alpha,Zeta,notes,brick,Gold,Checker");
        CHECK(order(AssetSort::Modified, false) == "alpha,Zeta,Checker,Gold,brick,notes");
        AssetSort size = AssetSort::Size;
        std::string bySize = order(size, false);
        CHECK(bySize.rfind("alpha,Zeta,", 0) == 0 && bySize.size() > 11 && bySize.substr(bySize.size() - 5) == "notes"); // 5 bytes: smallest

        CHECK(AssetListing::FormatSize(812) == "812 B" && AssetListing::FormatSize(4300) == "4.2 KB" &&
              AssetListing::FormatSize(13 * 1024 * 1024) == "13.0 MB");
        std::string when = AssetListing::FormatTime(now);
        CHECK(when.size() == 16 && when[4] == '-' && when[10] == ' ' && when[13] == ':');

        // Favorites.
        CHECK(ProjectFavorites::List().empty());
        CHECK(ProjectFavorites::AddFolder(assets / "alpha/Inner"));
        CHECK(!ProjectFavorites::AddFolder("alpha/Inner/")); // the same folder
        CHECK(ProjectFavorites::AddFolder("Zeta") && ProjectFavorites::AddFolder(assets));
        CHECK(!ProjectFavorites::AddFolder("Nope") && !ProjectFavorites::AddFolder(dir) && !ProjectFavorites::AddFolder("brick.png"));
        CHECK(ProjectFavorites::AddSearch("  t:Texture  ") && !ProjectFavorites::AddSearch("t:Texture") && !ProjectFavorites::AddSearch("   "));
        CHECK(ProjectFavorites::AddSearch("l:Wall", "Wall Things"));
        auto favorites = ProjectFavorites::List();
        CHECK(favorites.size() == 5);
        CHECK(favorites[0] == Favorite({ "Inner", "alpha/Inner", "" }) && favorites[1].Name == "Zeta" && favorites[2] == Favorite({ "Assets", "", "" }));
        CHECK(favorites[3] == Favorite({ "t:Texture", "", "t:Texture" }) && favorites[3].IsSearch() && !favorites[0].IsSearch());
        CHECK(favorites[4].Name == "Wall Things" && favorites[4].Search == "l:Wall");
        CHECK(fs::exists(dir / ".dimension/Favorites.json"));

        ProjectFavorites::Rename(favorites[3], "Textures");
        ProjectFavorites::Rename(favorites[1], "  "); // a blank name: kept
        ProjectFavorites::Remove(favorites[4]);
        favorites = ProjectFavorites::List();
        CHECK(favorites.size() == 4 && favorites[3].Name == "Textures" && favorites[1].Name == "Zeta");

        // A folder that is gone is left out of the list, and back when it returns.
        fs::rename(assets / "Zeta", assets / "Zeta2", ec);
        CHECK(ProjectFavorites::List().size() == 3);
        fs::rename(assets / "Zeta2", assets / "Zeta", ec);
        CHECK(ProjectFavorites::List().size() == 4);
        AssetManager::Shutdown();
        fs::remove_all(dir, ec);
    }

    // The Inspector's audio preview: a clip's waveform (lowest and highest sample per slice and channel) and its playback
    // (when there is an audio device).
    void TestAudioPreview()
    {
        // A 1 s mono WAV at 8 kHz: silence, then a square wave of amplitude 0.5.
        fs::path dir = fs::temp_directory_path() / "dimension_audio_preview";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        fs::path wav = dir / "Half.wav";
        {
            const uint32_t rate = 8000, frames = 8000;
            std::vector<int16_t> pcm(frames, 0);
            for (uint32_t i = frames / 2; i < frames; ++i) pcm[i] = (int16_t)((i / 8) % 2 ? 16384 : -16384);
            std::ofstream out(wav, std::ios::binary);
            auto u32 = [&](uint32_t v) { out.write((const char*)&v, 4); };
            auto u16 = [&](uint16_t v) { out.write((const char*)&v, 2); };
            out.write("RIFF", 4); u32(36 + frames * 2); out.write("WAVE", 4);
            out.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
            out.write("data", 4); u32(frames * 2);
            out.write((const char*)pcm.data(), frames * 2);
        }
        auto half = AudioEngine::GetWaveform(wav, 100);
        CHECK(half && half->Channels == 1 && half->Buckets == 100 && half->Min.size() == 100 && half->Max.size() == 100 && Near(half->Seconds, 1.0f, 0.001f));
        if (half)
        {
            bool quiet = true, loud = true;
            for (size_t b = 0; b < 50; ++b) quiet = quiet && half->Min[b] == 0.0f && half->Max[b] == 0.0f;
            for (size_t b = 50; b < 100; ++b) loud = loud && Near(half->Max[b], 0.5f, 0.001f) && Near(half->Min[b], -0.5f, 0.001f);
            CHECK(quiet && loud && Near(half->Peak(), 0.5f, 0.001f));
        }
        CHECK(!AudioEngine::GetWaveform(wav, 0) && !AudioEngine::GetWaveform(dir / "Missing.wav", 64));

        // Real clips: a mono WAV and a stereo Opus tone (each slice within -1..1, min <= max).
        auto chime = AudioEngine::GetWaveform(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Audio/Chime.wav", 256);
        CHECK(chime && chime->Channels == 1 && Near(chime->Peak(), 0.3988f, 0.002f));
        auto tone = AudioEngine::GetWaveform(fs::path(DIM_SOURCE_DIR) / "Tests" / "Data" / "Tone.opus", 64);
        CHECK(tone && tone->Channels == 2 && tone->Min.size() == 128 && Near(tone->Seconds, 1.0f, 0.01f) && tone->Peak() > 0.5f);
        if (tone)
        {
            bool ordered = true;
            for (size_t i = 0; i < tone->Min.size(); ++i) ordered = ordered && tone->Min[i] <= tone->Max[i] && tone->Min[i] >= -1.0f && tone->Max[i] <= 1.0f;
            CHECK(ordered && tone->Max[64 + 32] > 0.3f); // the second channel has the tone too
        }

        // Playback: one clip at a time, from an offset; Stop forgets it.
        AudioEngine::Init();
        CHECK(!AudioEngine::PreviewIsPlaying() && AudioEngine::PreviewFile().empty());
        if (AudioEngine::IsAvailable())
        {
            CHECK(AudioEngine::PreviewPlay(wav, 0.25f));
            CHECK(AudioEngine::PreviewIsPlaying() && AudioEngine::PreviewFile() == wav);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            CHECK(AudioEngine::PreviewPosition() > 0.3f && AudioEngine::PreviewPosition() < 1.0f);
            CHECK(!AudioEngine::PreviewPlay(dir / "Missing.wav") && AudioEngine::PreviewFile().empty()); // the last one stopped too
            CHECK(AudioEngine::PreviewPlay(wav, 0.95f) && AudioEngine::PreviewIsPlaying());
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            CHECK(!AudioEngine::PreviewIsPlaying() && AudioEngine::PreviewFile() == wav); // ran to its end
            CHECK(AudioEngine::PreviewPlay(wav, 0.95f, true));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            CHECK(AudioEngine::PreviewIsPlaying()); // looping
            AudioEngine::PreviewStop();
            CHECK(!AudioEngine::PreviewIsPlaying() && AudioEngine::PreviewFile().empty());
        }
        else
            CHECK(!AudioEngine::PreviewPlay(wav));
        AudioEngine::Shutdown();
        fs::remove_all(dir, ec);
    }

    // Frame All (the box around what a scene draws) and Scene view camera bookmarks (per scene, per user).
    void TestFrameAllAndBookmarks()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_bookmarks";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        AssetManager::Init(dir / "Assets");

        Scene scene("Bounds");
        auto& reg = scene.GetRegistry();
        CHECK(!SceneBounds::World(scene));
        Entity marker = scene.CreateEntity("Marker"); // nothing drawn: positions count
        reg.Get<TransformComponent>(marker).Position = { 3.0f, 1.0f, -2.0f };
        auto onlyMarker = SceneBounds::World(scene);
        CHECK(onlyMarker && Near(onlyMarker->Min, { 3.0f, 1.0f, -2.0f }) && Near(onlyMarker->Max, { 3.0f, 1.0f, -2.0f }));

        Entity cube = scene.CreateEntity("Cube"), parent = scene.CreateEntity("Parent");
        Entity sphere = scene.CreateEntity("Sphere", parent), sprite = scene.CreateEntity("Sprite"), off = scene.CreateEntity("Off");
        reg.Emplace<MeshRendererComponent>(cube).Mesh = AssetManager::kCube;
        reg.Emplace<MeshRendererComponent>(sphere).Mesh = AssetManager::kSphere;
        reg.Emplace<SpriteRendererComponent>(sprite);
        reg.Emplace<MeshRendererComponent>(off).Mesh = AssetManager::kCube;
        reg.Get<TransformComponent>(cube).Position = { 0.0f, 0.5f, 0.0f };
        reg.Get<TransformComponent>(parent).Position = { 10.0f, 0.0f, 0.0f };
        reg.Get<TransformComponent>(sphere).Scale = glm::vec3(2.0f);
        reg.Get<TransformComponent>(sprite).Position = { 0.0f, 0.0f, -5.0f };
        reg.Get<TransformComponent>(sprite).Scale = { 4.0f, 2.0f, 1.0f };
        reg.Get<TransformComponent>(off).Position = { 0.0f, 100.0f, 0.0f };
        reg.Get<TagComponent>(off).Active = false; // inactive: left out
        auto box = SceneBounds::World(scene);
        // Cube -0.5..0.5 raised to 0..1; the sphere (radius 0.5, scaled 2) at x 10: 9..11, -1..1 in y and z; the sprite
        // 4 x 2 at z -5.
        CHECK(box && Near(box->Min, { -2.0f, -1.0f, -5.0f }, 1e-3f) && Near(box->Max, { 11.0f, 1.0f, 1.0f }, 1e-3f));
        // Skipped (hidden in the Scene view): the sphere.
        auto noSphere = SceneBounds::World(scene, [&](Entity e) { return e == sphere; });
        CHECK(noSphere && Near(noSphere->Max.x, 2.0f, 1e-3f) && Near(noSphere->Max.y, 1.0f, 1e-3f));
        // A terrain: its size around the object, its height up.
        Entity land = scene.CreateEntity("Land");
        auto& terrain = reg.Emplace<TerrainComponent>(land);
        terrain.Size = { 40.0f, 20.0f };
        terrain.Height = 6.0f;
        auto withLand = SceneBounds::World(scene);
        CHECK(withLand && Near(withLand->Min, { -20.0f, -1.0f, -10.0f }, 1e-3f) && Near(withLand->Max, { 20.0f, 6.0f, 10.0f }, 1e-3f));

        // Bookmarks.
        fs::path demo = dir / "Assets/Scenes/Demo.scene";
        CHECK(!SceneBookmarks::Get(demo, 1) && SceneBookmarks::Slots(demo).empty());
        CameraBookmark a{ { 1.0f, 2.0f, 3.0f }, 12.5f, 0.75f, -0.4f, false, 6.0f }, b{ { -4.0f, 0.0f, 0.0f }, 3.0f, 0.0f, 0.0f, true, 9.5f };
        SceneBookmarks::Set(demo, 1, a);
        SceneBookmarks::Set(demo, 9, b);
        SceneBookmarks::Set(demo, 0, b);  // slots are 1-9
        SceneBookmarks::Set(demo, 10, b);
        CHECK(SceneBookmarks::Get(demo, 1) == a && SceneBookmarks::Get(demo, 9) == b && SceneBookmarks::Slots(demo) == std::vector<int>({ 1, 9 }));
        CHECK(SceneBookmarks::Get("Scenes/Demo.scene", 1) == a); // the same scene by its Assets path
        CHECK(!SceneBookmarks::Get(dir / "Assets/Scenes/Other.scene", 1) && !SceneBookmarks::Get(demo, 0));
        SceneBookmarks::Set({}, 2, b); // an unsaved scene
        CHECK(SceneBookmarks::Get({}, 2) == b && SceneBookmarks::Slots({}) == std::vector<int>({ 2 }));
        SceneBookmarks::Set(demo, 1, b); // replaced
        CHECK(SceneBookmarks::Get(demo, 1) == b);
        SceneBookmarks::Clear(demo, 9);
        CHECK(SceneBookmarks::Slots(demo) == std::vector<int>({ 1 }) && fs::exists(dir / ".dimension/SceneBookmarks.json"));
        SceneBookmarks::Clear(demo, 1);
        CHECK(SceneBookmarks::Slots(demo).empty() && SceneBookmarks::Get({}, 2) == b);
        AssetManager::Shutdown();
        fs::remove_all(dir, ec);
    }

    // The Scene view's measuring tool: what a click picks (a vertex near the mouse, else the surface, else the ground)
    // and how lengths read.
    void TestSceneMeasure()
    {
        CHECK(SceneMeasure::FormatLength(1.25f) == "1.25 m" && SceneMeasure::FormatLength(12.5f) == "12.5 m");
        CHECK(SceneMeasure::FormatLength(0.35f) == "35.0 cm" && SceneMeasure::FormatLength(0.0042f) == "4.2 mm");
        CHECK(SceneMeasure::FormatLength(1234.0f) == "1.23 km" && SceneMeasure::FormatLength(-0.5f) == "-50.0 cm");
        CHECK(SceneMeasure::FormatLength(0.0f) == "0.0 mm");

        fs::path dir = fs::temp_directory_path() / "dimension_measure";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        AssetManager::Init(dir);
        Scene scene("Measure");
        Entity cube = scene.CreateEntity("Cube");
        scene.GetRegistry().Emplace<MeshRendererComponent>(cube).Mesh = AssetManager::kCube; // -0.5..0.5
        scene.UpdateTransforms();

        // A camera at (3, 3, 3) looking at the middle, 800 x 600.
        glm::vec3 eye(3.0f);
        glm::mat4 viewProj = glm::perspective(glm::radians(60.0f), 800.0f / 600.0f, 0.1f, 100.0f) * glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0, 1, 0));
        glm::vec2 viewport(800.0f, 600.0f);
        auto screenOf = [&](const glm::vec3& p) {
            glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
            return glm::vec2((c.x / c.w * 0.5f + 0.5f) * viewport.x, (0.5f - c.y / c.w * 0.5f) * viewport.y);
        };
        auto pickAt = [&](const glm::vec3& aim, glm::vec2 nudge = {}, float pixels = 12.0f) {
            return SceneMeasure::PickPoint(scene, viewProj, viewport, screenOf(aim) + nudge, eye, glm::normalize(aim - eye), pixels);
        };
        // At a corner: the corner (also a few pixels off it).
        auto corner = pickAt({ 0.5f, 0.5f, 0.5f });
        CHECK(corner.Kind == SceneMeasure::PickKind::Vertex && Near(corner.Point, { 0.5f, 0.5f, 0.5f }) && corner.Object == cube);
        auto nearCorner = pickAt({ 0.5f, 0.5f, 0.42f }, { 0.0f, 0.0f });
        CHECK(nearCorner.Kind == SceneMeasure::PickKind::Vertex && Near(nearCorner.Point, { 0.5f, 0.5f, 0.5f }));
        // Without vertex snapping: the surface point there.
        auto plain = pickAt({ 0.5f, 0.5f, 0.42f }, {}, 0.0f);
        CHECK(plain.Kind == SceneMeasure::PickKind::Surface && Near(plain.Point, { 0.5f, 0.5f, 0.42f }, 1e-3f));
        // The middle of the top face: far from the corners, a surface point.
        auto top = pickAt({ 0.0f, 0.5f, 0.0f });
        CHECK(top.Kind == SceneMeasure::PickKind::Surface && Near(top.Point, { 0.0f, 0.5f, 0.0f }, 1e-3f) && top.Object == cube);
        // Off the cube: the ground plane; at the sky: nothing.
        auto ground = pickAt({ 2.0f, 0.0f, -1.5f });
        CHECK(ground.Kind == SceneMeasure::PickKind::Ground && Near(ground.Point, { 2.0f, 0.0f, -1.5f }, 1e-3f) && ground.Object.IsNull());
        auto sky = SceneMeasure::PickPoint(scene, viewProj, viewport, glm::vec2(400.0f, 5.0f), eye, glm::normalize(glm::vec3(-1.0f, 0.5f, -1.0f)));
        CHECK(!sky.Hit());
        AssetManager::Shutdown();
        fs::remove_all(dir, ec);
    }

    // Selection history (Back and Forward through what was selected) and saved selections per scene.
    void TestSelectionHistory()
    {
        auto objects = [](std::vector<uint64_t> ids) {
            SelectionSnapshot s;
            s.Objects = ids;
            s.Primary = ids.empty() ? 0 : ids.back();
            return s;
        };
        SelectionSnapshot a = objects({ 1 }), b = objects({ 2, 3 }), c = objects({ 4 }), d = objects({ 5 });
        SelectionSnapshot asset;
        asset.Assets = { 100020 };
        asset.PrimaryAsset = 100020;

        SelectionHistory h;
        CHECK(!h.CanGoBack() && !h.CanGoForward() && !h.Back() && !h.Forward());
        h.Record(a);
        h.Record(a); // the current one: not again
        h.Record(SelectionSnapshot{}); // nothing selected: not recorded
        h.Record(b);
        h.Record(asset);
        h.Record(c);
        CHECK(h.Size() == 4 && h.CanGoBack() && !h.CanGoForward());
        CHECK(h.Back() == asset && h.Back() == b && h.CanGoForward());
        CHECK(h.Forward() == asset && h.Back() == b && h.Back() == a && !h.Back() && !h.CanGoBack());
        CHECK(h.Forward() == b);
        // A new selection after going back drops what was ahead.
        h.Record(d);
        CHECK(h.Size() == 3 && !h.CanGoForward() && h.Back() == b && h.Forward() == d);
        // Skipping selections that are no longer usable (their objects deleted).
        h.Record(c);
        auto usable = [&](const SelectionSnapshot& s) { return s != d && s != b; };
        CHECK(h.Back(usable) == a && h.Forward(usable) == c);
        // Restored partly: the current entry updated in place, nothing dropped ahead.
        h.Back(usable);
        h.ReplaceCurrent(objects({ 1, 9 }));
        CHECK(h.Forward() == b && h.Back() == objects({ 1, 9 }));
        // At most kMax entries (the oldest go).
        SelectionHistory many;
        for (uint64_t i = 1; i <= SelectionHistory::kMax + 10; ++i) many.Record(objects({ i }));
        size_t steps = 0;
        std::optional<SelectionSnapshot> last;
        while (auto s = many.Back()) last = s, ++steps;
        CHECK(many.Size() == SelectionHistory::kMax && steps == SelectionHistory::kMax - 1 && last == objects({ 11 }));

        // Saved selections, per scene, per user.
        fs::path dir = fs::temp_directory_path() / "dimension_selection_sets";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        fs::path scene = dir / "Assets/Scenes/Level.scene";
        CHECK(!SelectionSets::Get(scene, 1) && SelectionSets::Slots(scene).empty());
        SelectionSets::Set(scene, 1, b);
        SelectionSets::Set(scene, 4, asset);
        SelectionSets::Set(scene, 10, a); // slots 1-9
        SelectionSets::Set(scene, 2, SelectionSnapshot{}); // empty: nothing saved
        CHECK(SelectionSets::Get(scene, 1) == b && SelectionSets::Get(scene, 4) == asset && SelectionSets::Slots(scene) == std::vector<int>({ 1, 4 }));
        CHECK(SelectionSets::Get("Scenes/Level.scene", 1) == b && !SelectionSets::Get(dir / "Assets/Scenes/Other.scene", 1));
        SelectionSets::Set({}, 3, c); // an unsaved scene
        CHECK(SelectionSets::Get({}, 3) == c && SelectionSets::Slots({}) == std::vector<int>({ 3 }));
        SelectionSets::Set(scene, 4, SelectionSnapshot{}); // saving nothing clears it
        CHECK(SelectionSets::Slots(scene) == std::vector<int>({ 1 }));
        SelectionSets::Clear(scene, 1);
        CHECK(SelectionSets::Slots(scene).empty() && fs::exists(dir / ".dimension/SelectionSets.json"));
        fs::remove_all(dir, ec);
    }

    // Hierarchy grouping (Ctrl+G / Ctrl+Shift+G) and Sort Children: objects keep their place in the world.
    void TestSceneGrouping()
    {
        using SceneGrouping::NameBefore;
        CHECK(NameBefore("Crate 2", "Crate 10") && !NameBefore("Crate 10", "Crate 2"));
        CHECK(NameBefore("apple", "Banana") && NameBefore("Apple", "banana") && NameBefore("a", "a1") && !NameBefore("a1", "a"));
        CHECK(NameBefore("Wall 9b", "Wall 10a") && NameBefore("x007", "x8") && !NameBefore("Same", "Same"));

        Scene scene("Grouping");
        auto& reg = scene.GetRegistry();
        auto names = [&](const std::vector<Entity>& list) {
            std::string out;
            for (Entity e : list) out += (out.empty() ? "" : ",") + reg.Get<TagComponent>(e).Name;
            return out;
        };
        auto world = [&](Entity e) {
            scene.UpdateTransforms();
            return reg.Get<TransformComponent>(e).GetWorldPosition();
        };
        Entity x = scene.CreateEntity("X"), a = scene.CreateEntity("A"), y = scene.CreateEntity("Y"), b = scene.CreateEntity("B");
        Entity p = scene.CreateEntity("P"), c = scene.CreateEntity("C", p), d = scene.CreateEntity("D", a);
        reg.Get<TransformComponent>(a).Position = { 0.0f, 0.0f, 0.0f };
        reg.Get<TransformComponent>(b).Position = { 4.0f, 0.0f, 0.0f };
        reg.Get<TransformComponent>(d).Position = { 0.0f, 1.0f, 0.0f };
        auto& pt = reg.Get<TransformComponent>(p);
        pt.Position = { 10.0f, 0.0f, 0.0f };
        pt.Rotation = { 0.0f, glm::radians(90.0f), 0.0f };
        pt.Scale = glm::vec3(2.0f);
        reg.Get<TransformComponent>(c).Position = { 1.0f, 0.0f, 0.0f };
        glm::vec3 cWorld = world(c), dWorld = world(d);

        // Two root objects (and A's child D, given too, counts with A): a group at their center where A was.
        Entity g = SceneGrouping::Group(scene, { b, d, a });
        CHECK(!g.IsNull() && reg.Get<TagComponent>(g).Name == "Group" && Near(world(g), { 2.0f, 0.0f, 0.0f }));
        CHECK(names(scene.GetRootEntities()) == "X,Group,Y,P" && names(scene.GetChildren(g)) == "A,B");
        CHECK(names(scene.GetChildren(a)) == "D" && Near(world(a), { 0.0f, 0.0f, 0.0f }) && Near(world(b), { 4.0f, 0.0f, 0.0f }) && Near(world(d), dWorld));
        CHECK(SceneGrouping::IsEmptyObject(scene, g));

        // Ungrouping: the children back where the group was, in their order, at the same places; the empty group gone.
        auto freed = SceneGrouping::Ungroup(scene, g);
        CHECK(freed == std::vector<Entity>({ a, b }) && !scene.IsValid(g) && names(scene.GetRootEntities()) == "X,A,B,Y,P");
        CHECK(Near(world(a), { 0.0f, 0.0f, 0.0f }) && Near(world(b), { 4.0f, 0.0f, 0.0f }));

        // Objects with different parents: the group at the root, where the first one's top object is; the child of a
        // rotated, scaled parent stays put in the world.
        Entity g2 = SceneGrouping::Group(scene, { c, y });
        CHECK(!g2.IsNull() && scene.GetParent(g2).IsNull() && names(scene.GetRootEntities()) == "X,A,B,Group,P" && names(scene.GetChildren(g2)) == "Y,C");
        CHECK(Near(world(c), cWorld, 1e-4f) && Near(world(g2), (cWorld + world(y)) * 0.5f, 1e-4f));
        // A group with a component of its own is kept when ungrouped.
        reg.Emplace<PointLightComponent>(g2);
        CHECK(!SceneGrouping::IsEmptyObject(scene, g2));
        CHECK(SceneGrouping::Ungroup(scene, g2).size() == 2 && scene.IsValid(g2) && scene.GetChildren(g2).empty());
        CHECK(names(scene.GetRootEntities()) == "X,A,B,Group,Y,C,P" && Near(world(c), cWorld, 1e-4f));
        CHECK(SceneGrouping::Ungroup(scene, g2).empty() && SceneGrouping::Group(scene, {}).IsNull());

        // Sorting: names in number order, ignoring case; the roots, then all levels.
        Entity box = scene.CreateEntity("Box");
        for (const char* n : { "Crate 10", "crate 2", "Apple", "Crate 1" }) scene.CreateEntity(n, box);
        Entity inner = scene.GetChildren(box)[2]; // Apple
        scene.CreateEntity("Zed", inner);
        scene.CreateEntity("Ant", inner);
        SceneGrouping::SortChildren(scene, box);
        CHECK(names(scene.GetChildren(box)) == "Apple,Crate 1,crate 2,Crate 10" && names(scene.GetChildren(inner)) == "Zed,Ant");
        SceneGrouping::SortChildren(scene, box, true);
        CHECK(names(scene.GetChildren(inner)) == "Ant,Zed");
        SceneGrouping::SortChildren(scene, Entity::Null());
        CHECK(names(scene.GetRootEntities()) == "A,B,Box,C,Group,P,X,Y");
    }

    // Align With View, Move To View and Align View to Selected: objects placed in world space whatever their parents.
    void TestViewAlign()
    {
        Scene scene("Align");
        auto& reg = scene.GetRegistry();
        auto worldOf = [&](Entity e) {
            scene.UpdateTransforms();
            return reg.Get<TransformComponent>(e).World;
        };
        Entity parent = scene.CreateEntity("Parent"), child = scene.CreateEntity("Child", parent);
        auto& pt = reg.Get<TransformComponent>(parent);
        pt.Position = { 10.0f, 0.0f, 0.0f };
        pt.Rotation = { 0.0f, glm::radians(90.0f), 0.0f };
        pt.Scale = glm::vec3(2.0f);
        reg.Get<TransformComponent>(child).Position = { 1.0f, 0.0f, 0.0f };

        // A world pose under a rotated, scaled parent: there exactly, its world scale (2) kept.
        glm::quat tilt = glm::angleAxis(glm::radians(30.0f), glm::vec3(1, 0, 0));
        ViewAlign::SetWorldPose(scene, child, { 1.0f, 2.0f, 3.0f }, tilt);
        glm::mat4 w = worldOf(child);
        glm::mat4 expected = glm::mat4_cast(tilt);
        CHECK(Near(glm::vec3(w[3]), { 1.0f, 2.0f, 3.0f }, 1e-4f) && Near(glm::length(glm::vec3(w[0])), 2.0f, 1e-4f));
        CHECK(Near(glm::normalize(glm::vec3(w[1])), glm::vec3(expected[1]), 1e-4f) && Near(glm::normalize(glm::vec3(w[2])), glm::vec3(expected[2]), 1e-4f));

        // Align With View: a camera at (5, 5, 5) looking at the origin; the object stands there facing the same way.
        glm::vec3 eye(5.0f);
        glm::mat4 cameraWorld = glm::inverse(glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0, 1, 0)));
        Entity cam = scene.CreateEntity("Cam");
        ViewAlign::AlignWithView(scene, { cam, child }, cameraWorld);
        for (Entity e : { cam, child })
        {
            glm::mat4 m = worldOf(e);
            CHECK(Near(glm::vec3(m[3]), eye, 1e-4f) && Near(-glm::normalize(glm::vec3(m[2])), glm::normalize(-eye), 1e-4f));
        }
        CHECK(Near(glm::length(glm::vec3(worldOf(child)[0])), 2.0f, 1e-4f));

        // Move To View: together, spacing kept; a given child moves with its parent once.
        Entity a = scene.CreateEntity("A"), b = scene.CreateEntity("B"), bChild = scene.CreateEntity("BChild", b);
        reg.Get<TransformComponent>(b).Position = { 2.0f, 0.0f, 0.0f };
        reg.Get<TransformComponent>(bChild).Position = { 0.0f, 1.0f, 0.0f };
        ViewAlign::MoveTo(scene, { a, b, bChild }, { 10.0f, 1.0f, 0.0f });
        // Center of A (0,0,0), B (2,0,0) and BChild (2,1,0) is (4/3, 1/3, 0): each moves by (8.67, 0.67, 0).
        glm::vec3 offset = glm::vec3(10.0f, 1.0f, 0.0f) - glm::vec3(4.0f / 3.0f, 1.0f / 3.0f, 0.0f);
        CHECK(Near(glm::vec3(worldOf(a)[3]), offset, 1e-4f) && Near(glm::vec3(worldOf(b)[3]), glm::vec3(2, 0, 0) + offset, 1e-4f));
        CHECK(Near(glm::vec3(worldOf(bChild)[3]), glm::vec3(2, 1, 0) + offset, 1e-4f));
        ViewAlign::MoveTo(scene, {}, glm::vec3(0.0f)); // nothing: no effect

        // Align View to Selected: the view from the object, along its forward.
        Entity look = scene.CreateEntity("Look");
        reg.Get<TransformComponent>(look).Position = { 1.0f, 2.0f, 3.0f };
        reg.Get<TransformComponent>(look).Rotation = { 0.0f, glm::radians(90.0f), 0.0f };
        glm::mat4 view = glm::inverse(ViewAlign::ViewFrom(scene, look));
        CHECK(Near(glm::vec3(view[3]), { 1.0f, 2.0f, 3.0f }, 1e-4f) && Near(-glm::vec3(view[2]), { -1.0f, 0.0f, 0.0f }, 1e-4f));
        // Looking straight down: still a valid view.
        reg.Get<TransformComponent>(look).Rotation = { glm::radians(-90.0f), 0.0f, 0.0f };
        glm::mat4 down = glm::inverse(ViewAlign::ViewFrom(scene, look));
        CHECK(Near(-glm::vec3(down[2]), { 0.0f, -1.0f, 0.0f }, 1e-4f) && !std::isnan(down[0][0]));
    }

    // The Scene view grid: its plane, cell size (fixed or adapting), opacity and offset along its normal.
    void TestSceneGrid()
    {
        CHECK(SceneGrid::NormalAxis(GridPlane::XZ) == 1 && SceneGrid::NormalAxis(GridPlane::XY) == 2 && SceneGrid::NormalAxis(GridPlane::YZ) == 0);
        GridSettings ground;
        // Adapting: 1 m up close, 10 m from 20 m away, 100 m from 200 m; measured from the plane (its offset).
        CHECK(SceneGrid::Spacing(ground, { 0.0f, 5.0f, 0.0f }, false, 0.0f) == 1.0f);
        CHECK(SceneGrid::Spacing(ground, { 0.0f, 30.0f, 0.0f }, false, 0.0f) == 10.0f);
        CHECK(SceneGrid::Spacing(ground, { 0.0f, -250.0f, 0.0f }, false, 0.0f) == 100.0f);
        GridSettings raised = ground;
        raised.Offset = 28.0f;
        CHECK(SceneGrid::Spacing(raised, { 0.0f, 30.0f, 0.0f }, false, 0.0f) == 1.0f);
        CHECK(SceneGrid::Spacing(ground, { 0.0f, 0.0f, 0.0f }, true, 40.0f) == 10.0f); // orthographic: by the view's size
        GridSettings fixedCells = ground;
        fixedCells.CellSize = 0.25f;
        CHECK(SceneGrid::Spacing(fixedCells, { 0.0f, 500.0f, 0.0f }, false, 0.0f) == 0.25f);

        // The ground grid around (3.4, 5, -7.6), 1 m cells: lines on whole meters, at y 0, 121 each way.
        auto lines = SceneGrid::Build(ground, { 3.4f, 5.0f, -7.6f }, false, 0.0f);
        CHECK(lines.size() == 242);
        bool flat = true, whole = true;
        for (const auto& l : lines)
        {
            flat = flat && l.A.y == 0.0f && l.B.y == 0.0f;
            whole = whole && std::fmod(std::abs(l.A.x), 1.0f) < 1e-4f && std::fmod(std::abs(l.A.z), 1.0f) < 1e-4f;
        }
        CHECK(flat && whole);
        // The line along Z through x = 0 is blue, along X through z = 0 red; every 10th brighter than the rest.
        auto find = [&](const std::vector<GridLine>& list, auto pred) -> const GridLine* {
            for (const auto& l : list)
                if (pred(l)) return &l;
            return nullptr;
        };
        const GridLine* zAxis = find(lines, [](const GridLine& l) { return l.A.x == 0.0f && l.B.x == 0.0f; });
        const GridLine* xAxis = find(lines, [](const GridLine& l) { return l.A.z == 0.0f && l.B.z == 0.0f; });
        const GridLine* tenth = find(lines, [](const GridLine& l) { return l.A.x == 10.0f && l.B.x == 10.0f; });
        const GridLine* plain = find(lines, [](const GridLine& l) { return l.A.x == 4.0f && l.B.x == 4.0f; });
        CHECK(zAxis && zAxis->Color.b > zAxis->Color.r && xAxis && xAxis->Color.r > xAxis->Color.b);
        CHECK(tenth && plain && tenth->Color.a > plain->Color.a && Near(plain->Color.a, 0.25f));

        // An XY wall raised to z = 2, half opacity, 0.5 m cells: lines at z 2, on half meters, alpha halved; the line
        // through y = 0 runs along X (red), through x = 0 along Y (green).
        GridSettings wall;
        wall.Plane = GridPlane::XY;
        wall.CellSize = 0.5f;
        wall.Opacity = 0.5f;
        wall.Offset = 2.0f;
        auto wallLines = SceneGrid::Build(wall, { 1.2f, 0.7f, 9.0f }, false, 0.0f);
        bool atOffset = true;
        for (const auto& l : wallLines) atOffset = atOffset && l.A.z == 2.0f && l.B.z == 2.0f;
        const GridLine* yAxis = find(wallLines, [](const GridLine& l) { return l.A.x == 0.0f && l.B.x == 0.0f; });
        const GridLine* wallX = find(wallLines, [](const GridLine& l) { return l.A.y == 0.0f && l.B.y == 0.0f; });
        const GridLine* half = find(wallLines, [](const GridLine& l) { return l.A.x == 1.5f && l.B.x == 1.5f; });
        CHECK(atOffset && yAxis && yAxis->Color.g > yAxis->Color.r && wallX && wallX->Color.r > wallX->Color.g);
        CHECK(half && Near(half->Color.a, 0.125f));
        // YZ: lines in x = offset.
        GridSettings side;
        side.Plane = GridPlane::YZ;
        auto sideLines = SceneGrid::Build(side, { 4.0f, 1.0f, 1.0f }, false, 0.0f);
        bool inX = !sideLines.empty();
        for (const auto& l : sideLines) inX = inX && l.A.x == 0.0f && l.B.x == 0.0f;
        CHECK(inX);
        // Fully transparent: no lines.
        GridSettings hidden;
        hidden.Opacity = 0.0f;
        CHECK(SceneGrid::Build(hidden, glm::vec3(0.0f), false, 0.0f).empty());
    }

    // The Inspector's search box: components and fields by name (every word, case and spaces ignored).
    void TestFieldSearch()
    {
        using namespace FieldSearch;
        CHECK(Matches("light", "Point Light") && Matches("LIGHT point", "Point Light") && Matches("pointlight", "Point Light"));
        CHECK(!Matches("spot", "Point Light") && Matches("", "Anything") && Matches("   ", "Anything"));
        CHECK(!Matches("int", "Point Light") && Matches("int", "Intensity") && Matches("speed", "MaxSpeed") && Matches("max sp", "MaxSpeed"));
        CHECK(Matches("ight", "Point Light") == false && Matches("2", "Layer 2") && Matches("cast", "Cast_Shadows"));
        const ComponentInfo* light = ComponentRegistry::Find("Point Light");
        const ComponentInfo* transform = ComponentRegistry::Find("Transform");
        const ComponentInfo* renderer = ComponentRegistry::Find("Mesh Renderer");
        CHECK(light && transform && renderer);
        if (!light || !transform || !renderer) return;
        auto shownFields = [](const std::string& q, const ComponentInfo& c) {
            std::vector<std::string> out;
            for (const auto& f : c.Fields)
                if (FieldShown(q, c.Name, f)) out.push_back(f.Name);
            return out;
        };
        // A component's name: it shows (with all its fields, in the Inspector).
        CHECK(ComponentNameMatches("point", light->Name) && ComponentShown("point", light->Name, light->Fields));
        // A field's name: the component shows with just the fields that match.
        CHECK(!ComponentNameMatches("intensity", light->Name) && ComponentShown("intensity", light->Name, light->Fields));
        CHECK(shownFields("intensity", *light) == std::vector<std::string>({ "Intensity" }));
        // Component and field words together.
        CHECK(shownFields("light int", *light) == std::vector<std::string>({ "Intensity" }));
        CHECK(!ComponentShown("light int", transform->Name, transform->Fields));
        CHECK(shownFields("scale", *transform) == std::vector<std::string>({ "Scale" }) && shownFields("rot", *transform) == std::vector<std::string>({ "Rotation" }));
        // Nothing matching: hidden.
        CHECK(!ComponentShown("zzz", renderer->Name, renderer->Fields) && shownFields("zzz", *renderer).empty());
        // Hidden fields never show.
        bool hiddenShown = false;
        for (const ComponentInfo& c : ComponentRegistry::All())
            for (const auto& f : c.Fields)
                if (f.Hidden && FieldShown(f.Name, c.Name, f)) hiddenShown = true;
        CHECK(!hiddenShown);
    }

    // The Profiler's timeline: each call of a section where it falls in the frame, nested by depth, and the call under a
    // point of the timeline.
    void TestProfilerTimeline()
    {
        auto wait = [](int ms) {
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until) {}
        };
        Profiler::SetPaused(false);
        Profiler::Clear();
        Profiler::BeginFrame();
        wait(1);
        {
            Profiler::Scope update("Update");
            for (int i = 0; i < 2; ++i)
            {
                Profiler::Scope step("Fixed Step");
                wait(2);
            }
            {
                Profiler::Scope scripts("Scripts");
                wait(1);
            }
        }
        {
            Profiler::Scope render("Render");
            wait(1);
        }
        Profiler::BeginFrame(); // ends the frame
        CHECK(!Profiler::History().empty());
        if (Profiler::History().empty()) return;
        const auto& f = Profiler::History().back();
        // Sections add the calls up; spans keep each one, in start order.
        CHECK(f.Sections.size() == 4 && f.Spans.size() == 5);
        if (f.Spans.size() != 5) return;
        const auto& s = f.Spans;
        CHECK(std::string(s[0].Name) == "Update" && std::string(s[1].Name) == "Fixed Step" && std::string(s[2].Name) == "Fixed Step" &&
              std::string(s[3].Name) == "Scripts" && std::string(s[4].Name) == "Render");
        CHECK(s[0].Depth == 0 && s[1].Depth == 1 && s[2].Depth == 1 && s[3].Depth == 1 && s[4].Depth == 0);
        // Times: Update starts after the first millisecond; its calls inside it, one after another; Render after it.
        CHECK(s[0].StartMs >= 0.9f && s[1].StartMs >= s[0].StartMs && s[2].StartMs >= s[1].StartMs + s[1].Ms - 1e-3f);
        CHECK(s[3].StartMs + s[3].Ms <= s[0].StartMs + s[0].Ms + 1e-3f && s[4].StartMs >= s[0].StartMs + s[0].Ms - 1e-3f);
        CHECK(s[1].Ms >= 1.9f && s[0].Ms >= s[1].Ms + s[2].Ms + s[3].Ms - 1e-3f && f.FrameMs >= s[4].StartMs + s[4].Ms - 1e-3f);
        for (const auto& sec : f.Sections)
            if (std::string(sec.Name) == "Fixed Step") CHECK(sec.Calls == 2 && Near(sec.Ms, s[1].Ms + s[2].Ms, 1e-3f));
        // The call under a point: by row and time.
        CHECK(Profiler::SpanAt(f, 0, s[0].StartMs + 0.5f) == 0 && Profiler::SpanAt(f, 1, s[2].StartMs + s[2].Ms * 0.5f) == 2);
        CHECK(Profiler::SpanAt(f, 1, s[4].StartMs + 0.5f) == -1 && Profiler::SpanAt(f, 0, s[4].StartMs + 0.5f) == 4);
        CHECK(Profiler::SpanAt(f, 2, s[0].StartMs + 0.5f) == -1 && Profiler::SpanAt(f, 0, 0.1f) == -1);
        // At most kMaxSpans calls a frame (Sections still count them all).
        Profiler::BeginFrame();
        for (size_t i = 0; i < Profiler::kMaxSpans + 50; ++i) Profiler::Scope many("Many");
        Profiler::BeginFrame();
        const auto& big = Profiler::History().back();
        CHECK(big.Spans.size() == Profiler::kMaxSpans && big.Sections.size() == 1 && big.Sections[0].Calls == (int)(Profiler::kMaxSpans + 50));
        // Stop recording (other tests' scopes stay out of it).
        Profiler::SetPaused(true);
        Profiler::BeginFrame();
        Profiler::Clear();
        Profiler::SetPaused(false);
    }

    // Profiler recordings (saved and opened again) and two frames compared section by section.
    void TestProfilerRecordings()
    {
        using namespace Profiler;
        FrameRecord a;
        a.FrameMs = 16.5f;
        a.Sections = { { "Update", 0, 6.0f, 1 }, { "Physics", 1, 2.5f, 2 }, { "Scripts", 1, 1.0f, 1 }, { "Render", 0, 8.0f, 1 }, { "Draw", 1, 3.0f, 1 } };
        a.Counters = { { "Draw Calls", 340.0f }, { "Triangles", 51234.0f } };
        a.Gpu = { { "Frame", 0, 3.4f, 1 }, { "Shadows", 1, 0.6f, 1 } };
        a.Spans = { { "Update", 0, 0.5f, 6.0f }, { "Physics", 1, 0.7f, 1.2f }, { "Physics", 1, 2.0f, 1.3f } };
        FrameRecord b = a;
        b.FrameMs = 21.0f;
        b.Sections = { { "Update", 0, 9.0f, 1 }, { "Physics", 1, 5.0f, 3 }, { "Navigation", 1, 1.5f, 1 }, { "Render", 0, 8.2f, 1 }, { "Draw", 1, 3.1f, 1 } };

        fs::path file = fs::temp_directory_path() / "dimension_profile_test.dprof";
        CHECK(SaveRecording(file, { a, b }));
        auto loaded = LoadRecording(file);
        CHECK(loaded && loaded->size() == 2);
        if (loaded && loaded->size() == 2)
        {
            const FrameRecord& la = (*loaded)[0];
            CHECK(Near(la.FrameMs, 16.5f) && la.Sections.size() == 5 && std::string(la.Sections[1].Name) == "Physics" && la.Sections[1].Depth == 1 &&
                  Near(la.Sections[1].Ms, 2.5f) && la.Sections[1].Calls == 2);
            CHECK(la.Gpu.size() == 2 && std::string(la.Gpu[1].Name) == "Shadows" && la.Spans.size() == 3 && Near(la.Spans[2].StartMs, 2.0f) &&
                  Near(la.Spans[2].Ms, 1.3f));
            bool counters = false;
            for (const auto& [n, v] : la.Counters)
                if (n == "Triangles" && v == 51234.0f) counters = true;
            CHECK(counters && Near((*loaded)[1].FrameMs, 21.0f));
            // Names read back are kept (interned): the same text, the same pointer.
            CHECK(la.Sections[1].Name == (*loaded)[1].Sections[1].Name && Intern("Physics") == la.Sections[1].Name);
        }
        // Not a recording: nothing.
        std::ofstream(fs::temp_directory_path() / "dimension_not_profile.dprof") << "{ \"frames\": 3 }";
        CHECK(!LoadRecording(fs::temp_directory_path() / "dimension_not_profile.dprof") && !LoadRecording(fs::temp_directory_path() / "missing.dprof"));
        std::error_code ec;
        fs::remove(file, ec);
        fs::remove(fs::temp_directory_path() / "dimension_not_profile.dprof", ec);

        // Comparing: matched by path (Render's Draw is not Update's), in the first frame's order, the second's own after.
        FrameRecord c = b;
        c.Sections = { { "Update", 0, 9.0f, 1 }, { "Draw", 1, 0.4f, 1 }, { "Physics", 1, 5.0f, 3 }, { "Render", 0, 8.2f, 1 }, { "Draw", 1, 3.1f, 1 } };
        auto rows = Compare(a, c);
        std::vector<std::string> order;
        for (const auto& r : rows) order.push_back(std::string(r.Name) + "@" + std::to_string(r.Depth));
        CHECK(order == std::vector<std::string>({ "Update@0", "Physics@1", "Scripts@1", "Render@0", "Draw@1", "Draw@1" }));
        if (rows.size() == 6)
        {
            CHECK(rows[0].InA && rows[0].InB && Near(rows[0].MsA, 6.0f) && Near(rows[0].MsB, 9.0f));
            CHECK(rows[1].CallsA == 2 && rows[1].CallsB == 3 && Near(rows[1].MsB, 5.0f));
            CHECK(rows[2].InA && !rows[2].InB && rows[2].MsB == 0.0f);                                          // Scripts: only in a
            CHECK(rows[4].InA && rows[4].InB && Near(rows[4].MsA, 3.0f) && Near(rows[4].MsB, 3.1f));           // Render/Draw
            CHECK(!rows[5].InA && rows[5].InB && Near(rows[5].MsB, 0.4f) && rows[5].MsA == 0.0f);               // Update/Draw: only in c
        }
        auto same = Compare(a, a);
        bool allSame = same.size() == a.Sections.size();
        for (const auto& r : same) allSame = allSame && r.InA && r.InB && r.MsA == r.MsB;
        CHECK(allSame);
    }

    // Debug lines from game code: until the next update, or for a duration of game time; kept while paused, cleared
    // when play stops.
    void TestDebugDraw()
    {
        DebugDraw::Clear();
        DebugDraw::Line({ 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0, 1 });
        DebugDraw::Ray({ 0, 1, 0 }, { 0, 2, 0 }, { 0, 1, 0, 1 }, 0.5f);
        CHECK(DebugDraw::Lines().size() == 2 && Near(DebugDraw::Lines()[1].B, { 0.0f, 3.0f, 0.0f }));
        DebugDraw::Tick(0.0625f); // the next update: the one-update line goes
        CHECK(DebugDraw::Lines().size() == 1 && Near(DebugDraw::Lines()[0].Remaining, 0.4375f));
        for (int i = 0; i < 6; ++i) DebugDraw::Tick(0.0625f); // 0.375 s more: still there (0.0625 left)
        CHECK(DebugDraw::Lines().size() == 1);
        DebugDraw::Tick(0.0625f);
        CHECK(DebugDraw::Lines().empty());

        // Through a running scene: Scene::OnUpdateRuntime ticks them; paused, they stay; stopping play clears them.
        Scene scene("Debug Lines");
        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        DebugDraw::Line({ 0, 0, 0 }, { 0, 0, 1 });        // drawn during this update
        DebugDraw::Line({ 0, 0, 0 }, { 0, 1, 0 }, glm::vec4(1.0f), 10.0f);
        runtime->SetPaused(true);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(DebugDraw::Lines().size() == 2); // paused: nothing ticks
        runtime->SetPaused(false);
        runtime->OnUpdateRuntime(1.0f / 60.0f);
        CHECK(DebugDraw::Lines().size() == 1 && DebugDraw::Lines()[0].Remaining > 9.9f);
        runtime->OnRuntimeStop();
        CHECK(DebugDraw::Lines().empty());
    }

    // PlayerPrefs: typed values by key kept in the player data folder between runs; a folder per game.
    void TestPlayerPrefs()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_player_prefs";
        std::error_code ec;
        fs::remove_all(dir, ec);
        PlayerData::SetFolder(dir / "Game A");
        CHECK(PlayerData::Folder() == dir / "Game A" && !PlayerPrefs::HasKey("Score"));
        PlayerPrefs::SetInt("Score", 42);
        PlayerPrefs::SetFloat("Volume", 0.75f);
        PlayerPrefs::SetString("Name", "Ada Lovelace");
        CHECK(PlayerPrefs::GetInt("Score") == 42 && Near(PlayerPrefs::GetFloat("Volume"), 0.75f) && PlayerPrefs::GetString("Name") == "Ada Lovelace");
        // Another type, or not set: the default.
        CHECK(PlayerPrefs::GetFloat("Score", -1.0f) == -1.0f && PlayerPrefs::GetInt("Name", 7) == 7 && PlayerPrefs::GetString("Volume", "x") == "x");
        CHECK(PlayerPrefs::GetInt("Missing", 3) == 3 && PlayerPrefs::HasKey("Volume") && !PlayerPrefs::HasKey("Missing"));
        // Written already: read back from the file.
        CHECK(fs::exists(dir / "Game A/PlayerPrefs.json") && !fs::exists(dir / "Game A/PlayerPrefs.json.tmp"));
        PlayerPrefs::Reload();
        CHECK(PlayerPrefs::GetInt("Score") == 42 && PlayerPrefs::GetString("Name") == "Ada Lovelace");
        PlayerPrefs::SetInt("Score", 43); // replaced
        PlayerPrefs::DeleteKey("Volume");
        PlayerPrefs::Reload();
        CHECK(PlayerPrefs::GetInt("Score") == 43 && !PlayerPrefs::HasKey("Volume"));
        // Another game's folder: its own prefs; back again, the first's.
        PlayerData::SetFolder(dir / "Game B");
        CHECK(!PlayerPrefs::HasKey("Score"));
        PlayerPrefs::SetInt("Score", 1);
        PlayerData::SetFolder(dir / "Game A");
        CHECK(PlayerPrefs::GetInt("Score") == 43);
        PlayerPrefs::DeleteAll();
        CHECK(!PlayerPrefs::HasKey("Score") && !PlayerPrefs::HasKey("Name"));
        PlayerData::SetFolder(dir / "Game B");
        CHECK(PlayerPrefs::GetInt("Score") == 1);
        // A damaged file: starts empty (and is written over on the next change).
        fs::create_directories(dir / "Game C", ec);
        std::ofstream(dir / "Game C/PlayerPrefs.json") << "{ broken";
        PlayerData::SetFolder(dir / "Game C");
        CHECK(!PlayerPrefs::HasKey("Score") && PlayerPrefs::GetString("Any", "fine") == "fine");
        PlayerPrefs::SetInt("Recovered", 1);
        PlayerPrefs::Reload();
        CHECK(PlayerPrefs::GetInt("Recovered") == 1);
        // Product names as folder names.
        CHECK(PlayerData::FolderName("My Game: Deluxe") == "My Game Deluxe" && PlayerData::FolderName("A/B\\C?") == "ABC" &&
              PlayerData::FolderName("???") == "Game" && PlayerData::FolderName("Trailing. ") == "Trailing");
        PlayerData::SetFolder({});
        fs::remove_all(dir, ec);
    }

    // Input actions: buttons and axes by name from keys, mouse buttons and gamepad controls (fed here from a fake
    // source), with Down/Up on the frames they change; a project's own actions saved in its file.
    // Localization: CSV string tables merged per language, the language picked (saved choice, system, default) and
    // texts falling back to the default language.
    void TestLocalization()
    {
        // CSV cells: quotes around commas and line breaks, "" for a quote, CRLF rows, a byte order mark, blank lines.
        auto rows = Localization::ParseCsv("\xEF\xBB\xBFKey,en\r\na,\"x, y\"\r\n\r\nb,\"say \"\"hi\"\"\"\nc,\"two\r\nlines\"\nd,");
        CHECK(rows.size() == 5 && rows[0] == std::vector<std::string>({ "Key", "en" }));
        CHECK(rows[1][1] == "x, y" && rows[2][1] == "say \"hi\"" && rows[3][1] == "two\nlines");
        CHECK(rows[4] == std::vector<std::string>({ "d", "" }));
        // Headers: "Name (code)" or the code alone; comment rows and empty keys are skipped; one column is no table.
        std::string error;
        auto table = StringTable::Parse("Key,English (en),fr,\n# menu\nplay,Play,Jouer\n,orphan\nquit,Quit\n", &error);
        CHECK(table && table->Languages.size() == 2 && table->Languages[0].Code == "en" && table->Languages[0].Name == "English");
        CHECK(table && table->Languages[1].Code == "fr" && table->Languages[1].Name == "fr");
        CHECK(table && table->Entries.size() == 2 && table->Entries[1].Key == "quit" && table->Entries[1].Texts == std::vector<std::string>({ "Quit", "" }));
        CHECK(!StringTable::Parse("Key\nplay\n", &error) && !error.empty() && !StringTable::Parse("", &error));

        // A project with two tables: the second adds German and repeats a key (the first table's wins).
        fs::path dir = fs::temp_directory_path() / "dimension_localization";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "Assets/UI");
        std::ofstream(dir / "Assets/UI/A.csv") << "Key,English (en),Fran\xC3\xA7" "ais (fr)\nmenu.play,Play,Jouer\nmenu.quit,Quit,\n";
        std::ofstream(dir / "Assets/UI/B.csv") << "Key,Deutsch (de),English (en)\nmenu.play,Spielen,Start\nhud.score,Punkte: {0},Score: {0}\n";
        auto& config = Project::GetConfig();
        std::string savedDefault = config.DefaultLanguage;
        config.DefaultLanguage.clear();
        PlayerData::SetFolder(dir / "PlayerData");
        Localization::SetSystemLanguage("fr-CA");
        AssetManager::Init(dir / "Assets");
        CHECK(AssetManager::GetAssetsOfType(AssetType::StringTable).size() == 2);
        const auto& langs = Localization::Languages();
        CHECK(langs.size() == 3 && langs[0].Code == "en" && langs[1].Code == "fr" && langs[2].Code == "de" && langs[2].Name == "Deutsch");
        // The system's "fr-CA" picks French (its base language); a text French lacks comes from the default (English).
        CHECK(Localization::Language() == "fr" && Localization::Get("menu.play") == "Jouer" && Localization::Get("menu.quit") == "Quit");
        CHECK(Localization::Get("menu.play", "de") == "Play" && Localization::Get("menu.play", "en") == "Play"); // A.csv's row, no German
        CHECK(Localization::Get("hud.score", "fr") == "Score: {0}" && Localization::Get("missing.key") == "missing.key");
        CHECK(Localization::Has("hud.score") && !Localization::Has("missing.key"));
        CHECK(Localization::Keys() == std::vector<std::string>({ "hud.score", "menu.play", "menu.quit" }));
        CHECK(Localization::Match("DE_at") == "de" && Localization::Match("en") == "en" && Localization::Match("pt-BR").empty());
        // Choosing a language: kept in PlayerPrefs, so the next run (another project load) starts in it.
        CHECK(Localization::SetLanguage("de-AT") && Localization::Language() == "de" && PlayerPrefs::GetString("Language") == "de");
        CHECK(!Localization::SetLanguage("ja") && Localization::Language() == "de");
        AssetManager::Init(dir / "Assets");
        CHECK(Localization::Language() == "de" && Localization::Get("hud.score") == "Punkte: {0}");
        // No choice and a system language no table has: the project's default language.
        PlayerPrefs::DeleteKey("Language");
        Localization::SetSystemLanguage("ja-JP");
        config.DefaultLanguage = "de";
        Localization::Reset();
        CHECK(Localization::Language() == "de" && Localization::DefaultLanguage() == "de");
        CHECK(Localization::Get("menu.quit", "fr") == "Quit"); // German lacks it too: any translation beats none
        config.DefaultLanguage.clear();
        Localization::Reset();
        CHECK(Localization::Language() == "en"); // else the first column

        // Editing a table reloads it; removing one drops its keys and languages.
        std::ofstream(dir / "Assets/UI/A.csv") << "Key,English (en),Fran\xC3\xA7" "ais (fr)\nmenu.play,Play!,Jouer !\n";
        fs::last_write_time(dir / "Assets/UI/A.csv", fs::last_write_time(dir / "Assets/UI/A.csv") + std::chrono::seconds(2));
        AssetManager::Refresh();
        CHECK(Localization::Get("menu.play") == "Play!" && !Localization::Has("menu.quit"));
        // Builds ship every string table (no scene references them); packages and searches only what they are asked for.
        auto shipped = AssetDependencies::Collect({}).Assets;
        CHECK(shipped.size() == 2 && AssetDependencies::Collect({}, false).Assets.empty());
        fs::remove(dir / "Assets/UI/B.csv");
        fs::remove(dir / "Assets/UI/B.csv.meta");
        AssetManager::Refresh();
        CHECK(Localization::Languages().size() == 2 && !Localization::Has("hud.score"));
        // UI Text's key is a saved field.
        const ComponentInfo* text = ComponentRegistry::Find("UI Text");
        CHECK(text && std::any_of(text->Fields.begin(), text->Fields.end(), [](const FieldInfo& f) { return f.Name == "Localized Key"; }));

        AssetManager::Shutdown();
        Localization::SetSystemLanguage({});
        config.DefaultLanguage = savedDefault;
        PlayerData::SetFolder({});
        fs::remove_all(dir, ec);
    }

    // Data assets: .asset files of a data class's fields, created with its defaults, read, filtered by class (bases
    // too), shipped when referenced, and the scripts told when one is saved and when play starts.
    struct TestStats
    {
        float Speed = 3.5f;
        std::string Title = "Grunt";
    };

    void TestDataAssets()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_data_assets";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "Assets");
        AssetManager::Init(dir / "Assets");
        CHECK(AssetManager::TypeFromExtension("Boss.asset") == AssetType::DataAsset);

        // A data class as the C# runtime registers one: its fields over storage holding the defaults.
        auto addClass = [](const std::string& name, std::vector<std::string> bases) {
            DataAssets::ClassInfo& c = DataAssets::AddClass(name, "test");
            c.Bases = std::move(bases);
            FieldInfo speed;
            speed.Name = "Speed";
            speed.Type = FieldType::Float;
            speed.Access = [](void* o) -> void* { return &static_cast<TestStats*>(o)->Speed; };
            FieldInfo title;
            title.Name = "Title";
            title.Type = FieldType::String;
            title.Access = [](void* o) -> void* { return &static_cast<TestStats*>(o)->Title; };
            c.Fields = { speed, title };
            c.Create = [] { return std::shared_ptr<void>(std::make_shared<TestStats>()); };
        };
        addClass("Game.Stats", {});
        addClass("Game.BossStats", { "Game.Stats" });
        std::vector<uint64_t> told;
        DataAssets::SetChangedHook([&](AssetHandle h) { told.push_back((uint64_t)h); });

        AssetHandle grunt = DataAssets::Create(dir / "Assets/Grunt.asset", "Game.Stats");
        AssetHandle boss = DataAssets::Create(dir / "Assets/Boss.asset", "Game.BossStats");
        CHECK(grunt.IsValid() && boss.IsValid() && AssetManager::GetMetadata(boss)->Type == AssetType::DataAsset);
        CHECK(!DataAssets::Create(dir / "Assets/Nope.asset", "Game.Unknown").IsValid() && !fs::exists(dir / "Assets/Nope.asset"));
        nlohmann::json file = DataAssets::Read(grunt);
        CHECK(file["class"] == "Game.Stats" && file["fields"]["Speed"] == 3.5f && file["fields"]["Title"] == "Grunt");
        CHECK(told == std::vector<uint64_t>({ (uint64_t)grunt, (uint64_t)boss }));
        // Saving tells the scripts; so does play starting (every object made again: handle 0).
        nlohmann::json fields = file["fields"];
        fields["Speed"] = 9.0f;
        CHECK(DataAssets::Write(dir / "Assets/Grunt.asset", "Game.Stats", fields) == grunt);
        CHECK(DataAssets::Read(grunt)["fields"]["Speed"] == 9.0f && told.back() == (uint64_t)grunt);
        Scene stage("Stage");
        stage.OnRuntimeStart();
        CHECK(told.back() == 0);
        stage.OnRuntimeStop();

        // Fields of a class take its assets and those of classes deriving from it; DataAsset itself takes any.
        CHECK(DataAssets::ClassOf(boss) == "Game.BossStats" && DataAssets::ClassOf(AssetHandle::None()).empty());
        CHECK(DataAssets::IsA("Game.BossStats", "Game.Stats") && !DataAssets::IsA("Game.Stats", "Game.BossStats"));
        CHECK(DataAssets::IsA("Game.Stats", "Dimension.DataAsset") && !DataAssets::IsA("", "Game.Stats"));
        // A data asset field saves the asset's GUID.
        AssetHandle slot = AssetHandle::None();
        FieldInfo ref;
        ref.Name = "Stats";
        ref.Type = FieldType::DataAsset;
        ref.Access = [](void* o) -> void* { return o; };
        FieldFromJson(ref, &slot, (uint64_t)boss, nullptr);
        CHECK(slot == boss && FieldToJson(ref, &slot, nullptr) == (uint64_t)boss);
        // A scene referencing one ships it (and what the asset references in turn).
        fields["Icon"] = (uint64_t)grunt;
        DataAssets::Write(dir / "Assets/Boss.asset", "Game.BossStats", fields);
        Scene level("Level");
        level.GetRegistry().Emplace<ScriptComponent>(level.CreateEntity("Spawner")).Add("Game.Spawner").Fields["Stats"] = (uint64_t)boss;
        SceneSerializer::SaveToFile(level, dir / "Assets/Level.scene");
        AssetManager::Refresh();
        auto shipped = AssetDependencies::Collect({ AssetManager::GetHandle(dir / "Assets/Level.scene") }).Assets;
        CHECK(shipped.contains(boss) && shipped.contains(grunt));
        // The scripts unloading takes their classes away.
        DataAssets::ClearModule("test");
        CHECK(!DataAssets::FindClass("Game.Stats") && DataAssets::ClassOf(boss) == "Game.BossStats");

        DataAssets::SetChangedHook({});
        AssetManager::Shutdown();
        fs::remove_all(dir, ec);
    }

    void TestInputActions()
    {
        // A fake input: the keys and buttons held, and the gamepad axes.
        std::set<std::pair<int, int>> held;
        float axes[6] = {};
        InputActions::Source fake;
        fake.Down = [&](InputBinding::Kind k, int code) { return held.contains({ (int)k, code }); };
        fake.Axis = [&](int a) { return axes[a]; };
        auto key = [](Key k) { return std::pair<int, int>{ (int)InputBinding::Kind::Key, (int)k }; };

        auto defaults = InputActions::Defaults();
        auto find = [&](const char* name) -> const InputAction& {
            for (const auto& a : defaults)
                if (a.Name == name) return a;
            return defaults.front();
        };
        const InputAction& horizontal = find("Horizontal");
        const InputAction& vertical = find("Vertical");
        const InputAction& fire = find("Fire");
        CHECK(defaults.size() == 6 && horizontal.Axis && !find("Jump").Axis);
        // Axes add up their bindings: D +1, A -1, both 0; the stick with its dead zone (0.15) rescaled.
        held = { key(Key::D) };
        CHECK(InputActions::Evaluate(horizontal, fake) == 1.0f);
        held = { key(Key::D), key(Key::A) };
        CHECK(InputActions::Evaluate(horizontal, fake) == 0.0f);
        held = { key(Key::A), key(Key::Left) };
        CHECK(InputActions::Evaluate(horizontal, fake) == -1.0f); // clamped
        held.clear();
        axes[(int)GamepadAxis::LeftX] = 0.1f;
        CHECK(InputActions::Evaluate(horizontal, fake) == 0.0f);   // inside the dead zone
        axes[(int)GamepadAxis::LeftX] = -0.575f;
        CHECK(Near(InputActions::Evaluate(horizontal, fake), -0.5f, 1e-4f));
        axes[(int)GamepadAxis::LeftX] = 0.0f;
        axes[(int)GamepadAxis::LeftY] = -1.0f; // the stick pushed up (+Y is down): Vertical +1, like W
        CHECK(Near(InputActions::Evaluate(vertical, fake), 1.0f));
        axes[(int)GamepadAxis::LeftY] = 0.0f;
        // A button from a trigger: past half way.
        axes[(int)GamepadAxis::RightTrigger] = 0.4f;
        CHECK(InputActions::Evaluate(fire, fake) == 0.0f);
        axes[(int)GamepadAxis::RightTrigger] = 0.9f;
        CHECK(InputActions::Evaluate(fire, fake) == 1.0f);
        axes[(int)GamepadAxis::RightTrigger] = 0.0f;
        held = { { (int)InputBinding::Kind::MouseButton, 0 } };
        CHECK(InputActions::Evaluate(fire, fake) == 1.0f);

        // Frames: Down on the first, held, Up on the release (names in any case).
        held.clear();
        InputActions::Update(fake);
        held = { { (int)InputBinding::Kind::GamepadButton, (int)GamepadButton::A } };
        InputActions::Update(fake);
        CHECK(InputActions::GetButton("Jump") && InputActions::GetButtonDown("jump") && !InputActions::GetButtonUp("Jump"));
        CHECK(InputActions::GetButton("Submit")); // A as well
        InputActions::Update(fake);
        CHECK(InputActions::GetButton("Jump") && !InputActions::GetButtonDown("Jump"));
        held.clear();
        InputActions::Update(fake);
        CHECK(!InputActions::GetButton("Jump") && InputActions::GetButtonUp("JUMP") && !InputActions::GetButtonDown("Jump"));
        InputActions::Update(fake);
        CHECK(!InputActions::GetButtonUp("Jump"));
        held = { key(Key::W) };
        InputActions::Update(fake);
        CHECK(InputActions::GetAxis("Vertical") == 1.0f && InputActions::GetAxis("Horizontal") == 0.0f);
        CHECK(!InputActions::GetButton("No Such Action") && InputActions::GetAxis("No Such Action") == 0.0f);

        // A project's own actions: saved in its file, used instead of the defaults.
        fs::path dir = fs::temp_directory_path() / "dimension_input_actions";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        CHECK(InputActions::Current().size() == defaults.size()); // none of its own yet: the defaults
        auto& config = Project::GetConfig();
        config.InputActions = { { "Dash", false, 0.15f, { { InputBinding::Kind::Key, (int)Key::LeftShift, 1.0f }, { InputBinding::Kind::GamepadButton, (int)GamepadButton::RightBumper, 1.0f } } },
                                { "Turn", true, 0.25f, { { InputBinding::Kind::Key, (int)Key::E, 1.0f }, { InputBinding::Kind::Key, (int)Key::Q, -1.0f }, { InputBinding::Kind::GamepadAxis, (int)GamepadAxis::RightX, 0.5f } } },
                                { "Steer", true, 0.15f, { { InputBinding::Kind::Key, (int)Key::D, 1.0f }, { InputBinding::Kind::Key, (int)Key::A, -1.0f } }, 3.0f, 3.0f, true } };
        config.InputActionsSet = true;
        auto saved = config.InputActions;
        CHECK(Project::Save() && Project::Open(dir));
        CHECK(Project::GetConfig().InputActionsSet && Project::GetConfig().InputActions == saved && InputActions::Current().size() == 3);
        held = { key(Key::Q) };
        axes[(int)GamepadAxis::RightX] = 1.0f;
        InputActions::Update(fake);
        CHECK(Near(InputActions::GetAxis("Turn"), -0.5f) && !InputActions::GetButton("Jump")); // Jump is not one of its actions now
        // A smoothed axis (Sensitivity 3, Gravity 3, Snap): GetAxis ramps up at 3 per second, GetAxisRaw jumps.
        held = { key(Key::D) };
        InputActions::Update(fake, 0.1f);
        CHECK(Near(InputActions::GetAxis("Steer"), 0.3f, 1e-4f) && InputActions::GetAxisRaw("Steer") == 1.0f);
        for (int i = 0; i < 4; ++i) InputActions::Update(fake, 0.1f);
        CHECK(InputActions::GetAxis("Steer") == 1.0f);
        held = { key(Key::A) }; // the other way: snaps through 0
        InputActions::Update(fake, 0.1f);
        CHECK(Near(InputActions::GetAxis("Steer"), -0.3f, 1e-4f) && InputActions::GetAxisRaw("Steer") == -1.0f);
        held.clear(); // let go: back to 0 at Gravity
        InputActions::Update(fake, 0.05f);
        CHECK(Near(InputActions::GetAxis("Steer"), -0.15f, 1e-4f) && InputActions::GetAxisRaw("Steer") == 0.0f);
        InputActions::Update(fake, 0.1f);
        CHECK(InputActions::GetAxis("Steer") == 0.0f);
        // Not smoothed (no Sensitivity): GetAxis is GetAxisRaw.
        held = { key(Key::E) };
        InputActions::Update(fake, 0.1f);
        CHECK(InputActions::GetAxis("Turn") == 1.0f && InputActions::GetAxisRaw("Turn") == 1.0f);
        held = { key(Key::Q) };
        axes[(int)GamepadAxis::RightX] = 1.0f;
        InputActions::Update(fake);
        {
            std::ifstream in(dir / "Project.dproj");
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            CHECK(text.find("\"control\": \"Left Shift\"") != std::string::npos); // readable in the file
            CHECK(text.find("\"sensitivity\"") != std::string::npos); // only where smoothed: Steer
        }
        // Back to the defaults: none saved.
        Project::GetConfig().InputActions.clear();
        Project::GetConfig().InputActionsSet = false;
        CHECK(Project::Save() && Project::Open(dir) && !Project::GetConfig().InputActionsSet && InputActions::Current().size() == 6);
        held.clear();
        InputActions::Update(fake);
        fs::remove_all(dir, ec);

        // Any key: a key or a mouse button held.
        CHECK(!Input::IsAnyKeyDown());
        Input::SimulateKey(Key::K, true);
        CHECK(Input::IsAnyKeyDown());
        Input::SimulateKey(Key::K, false);
        Input::SimulateMouseButton(MouseButton::Middle, true);
        CHECK(Input::IsAnyKeyDown());
        Input::SimulateMouseButton(MouseButton::Middle, false);
        CHECK(!Input::IsAnyKeyDown());
    }

    // Search across scene files (the Search window): objects of scenes that are not open, by name, component, script
    // class and tag, skipping the open scene.
    void TestProjectSearch()
    {
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
        AssetHandle navigation = AssetManager::GetHandle("Scenes/Navigation.scene");
        AssetHandle demo = AssetManager::GetHandle("Scenes/Demo3D.scene");
        CHECK(navigation.IsValid() && demo.IsValid());
        auto in = [](const std::vector<ProjectSearch::SceneObject>& hits, AssetHandle scene, const std::string& name) {
            return std::any_of(hits.begin(), hits.end(), [&](const auto& h) { return h.Scene == scene && h.Name == name; });
        };
        auto walls = ProjectSearch::FindInScenes("wall c"); // every word in the name
        CHECK(in(walls, navigation, "Wall C") && !in(walls, navigation, "Wall B"));
        CHECK(std::all_of(walls.begin(), walls.end(), [](const auto& h) { return h.Object != 0; }));
        auto agents = ProjectSearch::FindInScenes("t:navmeshagent");
        CHECK(in(agents, navigation, "Agent 1") && !in(agents, navigation, "Ground"));
        CHECK(in(ProjectSearch::FindInScenes("t:spinner"), demo, "C# Spinner")); // a C# script class
        CHECK(in(ProjectSearch::FindInScenes("ground tag:untagged"), navigation, "Ground"));
        CHECK(!in(ProjectSearch::FindInScenes("wall c", navigation), navigation, "Wall C")); // the open scene is skipped
        CHECK(ProjectSearch::FindInScenes("   ").empty() && ProjectSearch::FindInScenes("t:Camera", {}, 2).size() == 2);
        AssetManager::Shutdown();
    }

    // Asset packages: a material exported with what it references, imported into an empty project with the same
    // GUIDs (so the reference holds); importing again finds the same files; a changed file is replaced keeping the
    // project's GUID; a GUID the project already uses elsewhere gives the import a new one; broken packages and paths
    // leaving the Assets folder are refused.
    void TestAssetPackages()
    {
        fs::path temp = fs::temp_directory_path() / "dimension_packages";
        std::error_code ec;
        fs::remove_all(temp, ec);
        fs::create_directories(temp / "Empty", ec);
        fs::path package = temp / "Wood.dimensionpackage";

        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
        AssetHandle wood = AssetManager::GetHandle("Materials/Wood.mat");
        AssetHandle texture(uint64_t(100002)); // its Base Map
        const AssetMetadata* textureMeta = AssetManager::GetMetadata(texture);
        CHECK(wood.IsValid() && textureMeta);
        fs::path texturePath = textureMeta ? textureMeta->Path : fs::path();
        auto alone = AssetPackage::FilesFor({ wood }, false);
        auto withDeps = AssetPackage::FilesFor({ wood }, true);
        CHECK(alone == std::vector<fs::path>{ fs::path("Materials/Wood.mat") });
        CHECK(withDeps.size() == 2 && std::find(withDeps.begin(), withDeps.end(), texturePath) != withDeps.end());
        auto inMaterials = AssetPackage::AssetsIn("Materials");
        CHECK(std::find(inMaterials.begin(), inMaterials.end(), wood) != inMaterials.end() &&
              std::find(inMaterials.begin(), inMaterials.end(), texture) == inMaterials.end());
        CHECK(AssetPackage::Export(withDeps, package));
        AssetManager::Shutdown();

        AssetManager::Init(temp / "Empty");
        auto contents = AssetPackage::Read(package);
        CHECK(contents && contents->Files.size() == 2);
        if (contents && contents->Files.size() == 2)
        {
            CHECK(std::all_of(contents->Files.begin(), contents->Files.end(),
                              [](const auto& f) { return f.Guid != 0 && AssetPackage::Check(f) == AssetPackage::Status::New; }));
            CHECK(AssetPackage::Import(*contents) == 2);
            CHECK(AssetManager::GetHandle("Materials/Wood.mat") == wood && AssetManager::GetMetadata(texture) &&
                  AssetManager::GetMetadata(texture)->Path == texturePath); // the material's Base Map reference holds
            CHECK(std::all_of(contents->Files.begin(), contents->Files.end(),
                              [](const auto& f) { return AssetPackage::Check(f) == AssetPackage::Status::Same; }));

            // Changed in the project: replaced, keeping its GUID.
            std::ofstream(temp / "Empty/Materials/Wood.mat") << "{}";
            size_t woodIndex = contents->Files[0].Path == fs::path("Materials/Wood.mat") ? 0 : 1;
            CHECK(AssetPackage::Check(contents->Files[woodIndex]) == AssetPackage::Status::Changed);
            std::vector<bool> onlyWood(2, false);
            onlyWood[woodIndex] = true;
            CHECK(AssetPackage::Import(*contents, onlyWood) == 1);
            CHECK(AssetPackage::Check(contents->Files[woodIndex]) == AssetPackage::Status::Same && AssetManager::GetHandle("Materials/Wood.mat") == wood);

            // Its GUID now held by another file: imported under a new one.
            fs::rename(temp / "Empty/Materials/Wood.mat", temp / "Empty/Materials/Oak.mat", ec);
            fs::rename(temp / "Empty/Materials/Wood.mat.meta", temp / "Empty/Materials/Oak.mat.meta", ec);
            AssetManager::Shutdown(); // the project opened again with the rename
            AssetManager::Init(temp / "Empty");
            CHECK(AssetManager::GetHandle("Materials/Oak.mat") == wood);
            CHECK(AssetPackage::Check(contents->Files[woodIndex]) == AssetPackage::Status::GuidTaken);
            CHECK(AssetPackage::Import(*contents, onlyWood) == 1);
            AssetHandle copy = AssetManager::GetHandle("Materials/Wood.mat");
            CHECK(copy.IsValid() && copy != wood && AssetManager::GetHandle("Materials/Oak.mat") == wood);
        }
        AssetManager::Shutdown();

        // Not a package, or one reaching outside the Assets folder.
        std::ofstream(temp / "junk.dimensionpackage") << "not a package";
        CHECK(!AssetPackage::Read(temp / "junk.dimensionpackage"));
        {
            std::string head = R"({"version":1,"files":[{"path":"../escape.txt","size":2,"meta":""}]})";
            uint64_t length = head.size();
            std::ofstream out(temp / "escape.dimensionpackage", std::ios::binary);
            out.write("DIMPKG ", 8);
            out.write(reinterpret_cast<const char*>(&length), 8);
            out << head << "hi";
        }
        CHECK(!AssetPackage::Read(temp / "escape.dimensionpackage"));
        fs::remove_all(temp, ec);
    }

    // Several scenes open at once: an added scene's objects join the main scene but stay its own (children follow
    // their root), each section serializes and saves only its objects with its own name and settings, roots move
    // between sections, closing removes them, and in a playing copy an added scene unloads like a loaded one.
    void TestSceneSections()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_sections";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        {
            Scene a("Arena");
            a.CreateEntity("A1");
            a.CreateEntity("A2");
            CHECK(SceneSerializer::SaveToFile(a, dir / "Arena.scene"));
            Scene b("Props");
            b.GetSettings().Exposure = 2.0f;
            Entity b1 = b.CreateEntity("B1");
            b.SetParent(b.CreateEntity("B1 Child"), b1);
            CHECK(SceneSerializer::SaveToFile(b, dir / "Props.scene"));
        }
        auto main = SceneSerializer::LoadFromFile(dir / "Arena.scene");
        CHECK(main != nullptr);
        if (!main) return;
        Scene& scene = *main;
        SceneSections sections;
        CHECK(sections.Open(scene, dir / "Props.scene") && !sections.Open(scene, dir / "Props.scene") && sections.Count() == 2);
        Entity b1 = scene.FindEntityByName("B1"), child = scene.FindEntityByName("B1 Child"), a1 = scene.FindEntityByName("A1"),
               a2 = scene.FindEntityByName("A2");
        CHECK(sections.SectionOf(scene, b1) == 1 && sections.SectionOf(scene, child) == 1 && sections.SectionOf(scene, a1) == 0);
        CHECK(sections.Roots(scene, 1) == std::vector<Entity>{ b1 } && sections.Roots(scene, 0).size() == 2);
        CHECK(!sections.IsDirty(scene, 1));
        auto mainJson = sections.Serialize(scene, 0);
        CHECK(mainJson["entities"].size() == 2 && mainJson["name"] == "Arena");

        scene.GetRegistry().Get<TransformComponent>(b1).Position.x = 4.0f;
        CHECK(sections.IsDirty(scene, 1) && sections.Save(scene, 1) && !sections.IsDirty(scene, 1));
        auto saved = SceneSerializer::LoadFromFile(dir / "Props.scene");
        CHECK(saved && saved->GetName() == "Props" && Near(saved->GetSettings().Exposure, 2.0f) &&
              saved->GetRegistry().AliveCount() == 2 && !saved->FindEntityByName("B1 Child").IsNull() &&
              Near(saved->GetRegistry().Get<TransformComponent>(saved->FindEntityByName("B1")).Position.x, 4.0f));

        sections.MoveRoot(scene, a2, 1);
        Entity added = scene.CreateEntity("New");
        CHECK(sections.SectionOf(scene, a2) == 1 && sections.SectionOf(scene, added) == 0 && sections.IsDirty(scene, 1));
        CHECK(sections.Serialize(scene, 0)["entities"].size() == 2); // A1 and New
        sections.MoveRoot(scene, a2, 0);
        CHECK(sections.SectionOf(scene, a2) == 0 && !sections.IsDirty(scene, 1));

        // In a copy that plays, the added scene unloads like a scene loaded additively.
        auto playing = scene.Clone();
        sections.RegisterLoaded(*playing);
        CHECK(playing->UnloadScene("Props") && playing->FindEntityByName("B1").IsNull() && !playing->FindEntityByName("A1").IsNull());

        sections.Close(scene, 1);
        CHECK(sections.Count() == 1 && !scene.IsValid(b1) && !scene.IsValid(child) && scene.IsValid(a1) && scene.IsValid(a2));
        fs::remove_all(dir, ec);
    }

    // Scene templates: the built-in ones, and a project template (a scene labelled Template) whose copies get new
    // GUIDs with references between their objects following. Recent scenes: newest first, at most ten, moved up when
    // opened again, missing files left out, kept in the project.
    void TestSceneTemplates()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_templates";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        AssetManager::Init(dir / "Assets");
        auto all = SceneTemplates::All();
        CHECK(all.size() == 4 && all[0].Name == "Basic 3D" && all[3].Name == "Empty");
        auto find = [&](const std::string& name) {
            for (auto& t : SceneTemplates::All())
                if (t.Name == name) return t;
            return SceneTemplates::Template{};
        };
        auto basic = SceneTemplates::Create(find("Basic 3D"));
        CHECK(basic && basic->GetRegistry().AliveCount() == 2 && basic->GetPrimaryCamera() != Entity::Null() &&
              !basic->FindEntityByName("Directional Light").IsNull());
        auto flat = SceneTemplates::Create(find("Basic 2D"));
        CHECK(flat && flat->GetRegistry().Get<CameraComponent>(flat->GetPrimaryCamera()).Projection == ProjectionType::Orthographic);
        auto outdoor = SceneTemplates::Create(find("Outdoor"));
        CHECK(outdoor && outdoor->GetSettings().Fog && !outdoor->FindEntityByName("Ground").IsNull());
        CHECK(SceneTemplates::Create(find("Empty"))->GetRegistry().AliveCount() == 0);

        // A project template, with a joint that references another of its objects.
        Scene arena("Arena");
        Entity post = arena.CreateEntity("Post"), door = arena.CreateEntity("Door");
        arena.GetRegistry().Emplace<JointComponent>(door).ConnectedBody = post;
        AssetHandle h = SceneTemplates::SaveAsTemplate(arena, "Arena", "A post and a door on a hinge");
        CHECK(h.IsValid() && AssetManager::GetLabels(h) == std::vector<std::string>{ SceneTemplates::kLabel });
        auto mine = find("Arena");
        CHECK(mine.Scene == h && mine.Description == "A post and a door on a hinge" && SceneTemplates::All().size() == 5);
        auto copy = SceneTemplates::Create(mine), second = SceneTemplates::Create(mine);
        CHECK(copy && second && copy->GetName() == "Untitled" && copy->GetRegistry().AliveCount() == 2);
        if (copy && second)
        {
            Entity copiedPost = copy->FindEntityByName("Post"), copiedDoor = copy->FindEntityByName("Door");
            CHECK(copy->GetGuid(copiedPost) != arena.GetGuid(post) && copy->GetGuid(copiedPost) != second->GetGuid(second->FindEntityByName("Post")));
            CHECK(copy->GetRegistry().Get<JointComponent>(copiedDoor).ConnectedBody == copiedPost);
        }

        // Recent scenes.
        fs::create_directories(dir / "Assets/Scenes", ec);
        for (int i = 0; i < 12; ++i)
        {
            std::ofstream(dir / ("Assets/Scenes/S" + std::to_string(i) + ".scene")) << "{}";
            RecentScenes::Add(dir / ("Assets/Scenes/S" + std::to_string(i) + ".scene"));
        }
        auto recent = RecentScenes::List();
        CHECK(recent.size() == RecentScenes::kMax && recent[0].filename() == "S11.scene" && recent[9].filename() == "S2.scene");
        RecentScenes::Add(dir / "Assets/Scenes/S5.scene");
        CHECK(RecentScenes::List()[0].filename() == "S5.scene" && RecentScenes::List().size() == RecentScenes::kMax);
        fs::remove(dir / "Assets/Scenes/S11.scene", ec);
        RecentScenes::Remove(dir / "Assets/Scenes/S10.scene");
        recent = RecentScenes::List();
        CHECK(recent.size() == 8 && std::none_of(recent.begin(), recent.end(), [](const fs::path& p) { return p.filename() == "S10.scene"; }));
        CHECK(Project::Open(dir) && RecentScenes::List().size() == 8); // kept with the project
        RecentScenes::Clear();
        CHECK(RecentScenes::List().empty());
        AssetManager::Shutdown();
        fs::remove_all(dir, ec);
    }

    // The Project Hub: a new project gets its name and a first scene from the chosen template as its start scene (and
    // cannot be made over an existing one); the recent list keeps the newest first with names from the project files,
    // and marks projects that are gone.
    void TestProjectHub()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_hub";
        std::error_code ec;
        fs::remove_all(dir, ec);
        ProjectHub::SetRecentFile(dir / "RecentProjects.json");
        fs::path game = dir / "My Game";
        CHECK(!ProjectHub::IsProject(game) && ProjectHub::Create(game, "My Game", "Outdoor") && ProjectHub::IsProject(game));
        CHECK(Project::GetConfig().Name == "My Game" && Project::GetConfig().StartScene.IsValid());
        fs::path start = AssetManager::GetAbsolutePath(Project::GetConfig().StartScene);
        CHECK(start.filename() == "Main.scene");
        auto scene = SceneSerializer::LoadFromFile(start);
        CHECK(scene && scene->GetName() == "Main" && !scene->FindEntityByName("Ground").IsNull() && scene->GetSettings().Fog);
        CHECK(!ProjectHub::Create(game, "Again", "Empty"));
        AssetManager::Shutdown();

        fs::path other = dir / "Other";
        CHECK(ProjectHub::Create(other, "Other Game", "Empty"));
        AssetManager::Shutdown();
        ProjectHub::Remember(game);
        ProjectHub::Remember(other);
        auto recent = ProjectHub::Recent();
        CHECK(recent.size() == 2 && recent[0].Name == "Other Game" && recent[1].Name == "My Game" && recent[0].LastOpened > 0);
        ProjectHub::Remember(game);
        recent = ProjectHub::Recent();
        CHECK(recent.size() == 2 && recent[0].Name == "My Game");
        fs::remove_all(other, ec);
        recent = ProjectHub::Recent();
        CHECK(recent.size() == 2 && recent[0].Exists && !recent[1].Exists && recent[1].Name == "Other Game");
        ProjectHub::Forget(other);
        CHECK(ProjectHub::Recent().size() == 1);
        ProjectHub::SetRecentFile({});
        fs::remove_all(dir, ec);
    }

    // Undo steps' names: what changed between two states of a scene, said briefly. Notes: the Note component.
    void TestSceneDiff()
    {
        Scene scene("Diff");
        auto& reg = scene.GetRegistry();
        nlohmann::json state = SceneSerializer::Serialize(scene);
        auto step = [&]() {
            nlohmann::json now = SceneSerializer::Serialize(scene);
            std::string name = SceneDiff::Describe(state, now);
            state = now;
            return name;
        };
        Entity cube = scene.CreateEntity("Cube");
        CHECK(step() == "Create Cube");
        Entity car = scene.CreateEntity("Car");
        Entity wheel = scene.CreateEntity("Wheel");
        scene.SetParent(wheel, car);
        CHECK(step() == "Create Car"); // with its child: once
        scene.DestroyEntity(cube);
        CHECK(step() == "Delete Cube");
        reg.Get<TagComponent>(car).Name = "Truck";
        CHECK(step() == "Rename Car to Truck");
        reg.Get<TransformComponent>(car).Position.x = 3.0f;
        CHECK(step() == "Truck: Transform (Position)");
        reg.Emplace<PointLightComponent>(car);
        CHECK(step() == "Add Point Light to Truck");
        reg.Remove<PointLightComponent>(car);
        CHECK(step() == "Remove Point Light from Truck");
        reg.Get<TransformComponent>(car).Position.y = 1.0f;
        reg.Get<TransformComponent>(wheel).Position.y = 1.0f;
        CHECK(step() == "Transform on 2 objects");
        reg.Get<TransformComponent>(car).Scale = glm::vec3(2.0f);
        reg.Get<TagComponent>(wheel).Name = "Tire";
        CHECK(step() == "Edit 2 objects");
        scene.GetSettings().Fog = true;
        CHECK(step() == "Scene Settings");
        CHECK(step() == "No change");
        Entity lamp = scene.CreateEntity("Lamp");
        step();
        scene.SetSiblingIndex(lamp, 0);
        CHECK(step() == "Reorder objects");
        scene.SetParent(wheel, Entity::Null(), false);
        CHECK(step().rfind("Reparent Tire", 0) == 0);
        Entity a = scene.CreateEntity("A");
        scene.DestroyEntity(lamp);
        CHECK(step() == "Create A; Delete Lamp");

        const ComponentInfo* note = ComponentRegistry::Find("Note");
        CHECK(note && note->Fields.size() == 3 && note->Fields[0].Name == "Text" && note->Fields[0].Multiline);
        reg.Emplace<NoteComponent>(a).Text = "Fix the\nlighting here";
        CHECK(step() == "Add Note to A");
        (void)a;
    }

    void TestAssetLabels()
    {
        // Labels live in the .meta files; the search matches names, t:Type and l:Label terms together.
        fs::path root = fs::temp_directory_path() / "dimension_labels";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Things", ec);
        fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Sprites/Box.png", root / "Things/Crate.png", ec);
        fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Materials/Wood.mat", root / "Things/Crate Wood.mat", ec);
        fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Materials/Gold.mat", root / "Gold.mat", ec);
        AssetManager::Init(root);
        AssetHandle crate = AssetManager::GetHandle("Things/Crate.png"), wood = AssetManager::GetHandle("Things/Crate Wood.mat"),
                    gold = AssetManager::GetHandle("Gold.mat");
        CHECK(crate.IsValid() && wood.IsValid() && gold.IsValid());
        AssetManager::SetLabels(crate, { "Props", "Level1", "Props" });
        AssetManager::SetLabels(wood, { "Props" });
        CHECK(AssetManager::GetLabels(crate) == std::vector<std::string>({ "Props", "Level1" }));
        CHECK(AssetManager::GetAllLabels() == std::vector<std::string>({ "Level1", "Props" }));
        CHECK(AssetManager::Search("crate") == std::vector<AssetHandle>({ wood, crate })); // by name, sorted by path
        CHECK(AssetManager::Search("l:props t:mat") == std::vector<AssetHandle>({ wood }));
        CHECK(AssetManager::Search("t:Texture") == std::vector<AssetHandle>({ crate }));
        CHECK(AssetManager::Search("l:level1 crate") == std::vector<AssetHandle>({ crate }) && AssetManager::Search("l:nothing").empty());
        AssetManager::Shutdown();
        // Kept in the .meta file: there after loading the project again.
        AssetManager::Init(root);
        CHECK(AssetManager::GetLabels(AssetManager::GetHandle("Things/Crate.png")).size() == 2 && AssetManager::GetLabels(gold).empty());
        AssetManager::SetLabels(crate, {});
        CHECK(AssetManager::GetAllLabels() == std::vector<std::string>({ "Props" }));
        AssetManager::Shutdown();
        fs::remove_all(root, ec);
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

    void TestMaterialSlotsAndSoftShadows()
    {
        // Per-slot material overrides round trip through the scene serializer, keeping empty slots in place.
        Scene scene("Slots");
        Entity e = scene.CreateEntity("Model");
        auto& mr = scene.GetRegistry().Emplace<MeshRendererComponent>(e);
        mr.Materials = { Guid(uint64_t(9001)), Guid::None(), Guid(uint64_t(9003)) };
        auto& sun = scene.GetRegistry().Emplace<DirectionalLightComponent>(e);
        sun.SoftShadows = true;
        sun.LightAngle = 2.5f;
        auto loaded = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        Entity le = loaded->FindEntityByName("Model");
        auto& lmr = loaded->GetRegistry().Get<MeshRendererComponent>(le);
        CHECK(lmr.Materials.size() == 3 && (uint64_t)lmr.Materials[0] == 9001 && !lmr.Materials[1] && (uint64_t)lmr.Materials[2] == 9003);
        auto& lsun = loaded->GetRegistry().Get<DirectionalLightComponent>(le);
        CHECK(lsun.SoftShadows && Near(lsun.LightAngle, 2.5f));
        // Older scenes without the field load with no overrides.
        nlohmann::json j = SceneSerializer::Serialize(scene);
        for (auto& ent : j["entities"]) ent["components"]["Mesh Renderer"].erase("Materials");
        auto old = SceneSerializer::Deserialize(j);
        CHECK(old->GetRegistry().Get<MeshRendererComponent>(old->FindEntityByName("Model")).Materials.empty());
    }

    void TestUIGridAndHorizontalScroll()
    {
        Scene scene("Grid");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        reg.Emplace<CanvasComponent>(canvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
        auto rect = [&](Entity e, glm::vec2 pos, glm::vec2 size) -> UIRectComponent& {
            auto& r = reg.Emplace<UIRectComponent>(e);
            r.AnchorMin = r.AnchorMax = { 0, 0 };
            r.Pivot = { 0, 0 };
            r.Position = pos;
            r.Size = size;
            return r;
        };
        auto content = [&](Entity parent, UILayoutDirection dir, glm::vec2 anchorMax) {
            Entity c = scene.CreateEntity("Content", parent);
            auto& r = reg.Emplace<UIRectComponent>(c);
            r.AnchorMin = { 0, 0 }; r.AnchorMax = anchorMax; r.Size = { 0, 0 }; r.Pivot = { 0, 0 };
            auto& g = reg.Emplace<UILayoutGroupComponent>(c);
            g.Direction = dir;
            g.Padding = glm::vec4(0.0f);
            return c;
        };

        // A 200 x 100 vertical scroll view whose content is a flexible grid of 50 px cells, 10 px apart: 3 columns,
        // 7 cells -> 3 rows, 170 px tall, so it scrolls 70.
        Entity gridView = scene.CreateEntity("GridView", canvas);
        rect(gridView, { 0, 0 }, { 200, 100 });
        reg.Emplace<UIScrollViewComponent>(gridView);
        Entity grid = content(gridView, UILayoutDirection::Grid, { 1, 0 });
        {
            auto& g = reg.Get<UILayoutGroupComponent>(grid);
            g.CellSize = { 50, 50 };
            g.Spacing = 10.0f;
        }
        std::vector<Entity> cells;
        for (int i = 0; i < 7; ++i)
        {
            cells.push_back(scene.CreateEntity("Cell", grid));
            rect(cells.back(), { 0, 0 }, { 10, 10 }); // ignored: cells take the grid's cell size
        }

        // A 200 x 100 horizontal-only view with a row of five 80 px items: 400 wide, so it scrolls 200 sideways.
        Entity rowView = scene.CreateEntity("RowView", canvas);
        rect(rowView, { 0, 300 }, { 200, 100 });
        {
            auto& v = reg.Emplace<UIScrollViewComponent>(rowView);
            v.Vertical = false;
            v.Horizontal = true;
        }
        Entity row = content(rowView, UILayoutDirection::Horizontal, { 0, 1 });
        reg.Get<UILayoutGroupComponent>(row).Spacing = 0.0f;
        for (int i = 0; i < 5; ++i)
        {
            Entity item = scene.CreateEntity("Item", row);
            rect(item, { 0, 0 }, { 80, 0 });
            reg.Emplace<UIImageComponent>(item);
        }
        scene.UpdateTransforms();

        glm::vec2 screen(1000, 1000);
        UI::ComputeLayout(scene, screen);
        auto& gv = reg.Get<UIScrollViewComponent>(gridView);
        CHECK(Near(gv.MaxScroll, 70.0f) && Near(gv.MaxScrollX, 0.0f));
        auto& c4 = reg.Get<UIRectComponent>(cells[4]); // second row, second column
        CHECK(Near(c4.ComputedMin, glm::vec2(60, 60)) && Near(c4.ComputedMax, glm::vec2(110, 110)));
        // A fixed column count overrides the fit: two columns -> cell 4 starts the third row; 4 rows = 230 px.
        reg.Get<UILayoutGroupComponent>(grid).Columns = 2;
        UI::ComputeLayout(scene, screen);
        CHECK(Near(c4.ComputedMin, glm::vec2(0, 120)) && Near(gv.MaxScroll, 130.0f));

        auto& rv = reg.Get<UIScrollViewComponent>(rowView);
        CHECK(Near(rv.MaxScrollX, 200.0f) && Near(rv.MaxScroll, 0.0f));
        // The wheel scrolls a horizontal-only view sideways.
        UI::PointerState p;
        p.Position = { 100, 320 };
        p.Scroll = -1.0f;
        UI::Update(scene, p, screen);
        UI::ComputeLayout(scene, screen);
        CHECK(Near(rv.ScrollPositionX, 60.0f) && Near(reg.Get<UIRectComponent>(row).ComputedMin.x, -60.0f));
        // In a view that scrolls both ways, Shift + wheel goes sideways and the plain wheel goes down.
        rv.Vertical = true;
        p.Shift = true;
        UI::Update(scene, p, screen);
        CHECK(Near(rv.ScrollPositionX, 120.0f) && Near(rv.ScrollPosition, 0.0f));
        rv.Vertical = false;
        rv.ScrollPositionX = 0.0f;
        UI::ComputeLayout(scene, screen);

        // Horizontal scrollbar along the bottom: track x 4..196, handle 192 * 200/400 = 96 px. Drag it half way.
        p = UI::PointerState();
        p.Position = { 10, 391 };
        p.Down = p.Pressed = true;
        UI::Update(scene, p, screen);
        CHECK(rv.DraggingScrollbar && rv.DraggingHorizontal && Near(rv.DragOffset, 6.0f, 0.01f));
        p.Pressed = false;
        p.Position.x = 10 + 48.0f;
        UI::Update(scene, p, screen);
        CHECK(Near(rv.ScrollPositionX, 100.0f, 0.1f));
        p.Down = false;
        p.Released = true;
        UI::Update(scene, p, screen);
        CHECK(!rv.DraggingScrollbar);

        // New fields survive serialization.
        auto loaded = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        auto& lg = loaded->GetRegistry().Get<UILayoutGroupComponent>(loaded->GetChildren(loaded->FindEntityByName("GridView"))[0]);
        CHECK(lg.Direction == UILayoutDirection::Grid && lg.Columns == 2 && Near(lg.CellSize, glm::vec2(50, 50)));
        auto& lv = loaded->GetRegistry().Get<UIScrollViewComponent>(loaded->FindEntityByName("RowView"));
        CHECK(lv.Horizontal && !lv.Vertical);
    }

    void TestSmoothCurvesAndParticleForces()
    {
        // Smooth curves pass through every key, stay within neighbouring keys (monotone), and are flat at a peak.
        Curve c;
        c.Keys = { { 0.0f, 0.0f }, { 0.5f, 1.0f }, { 1.0f, 0.0f } };
        Curve linear = c;
        c.Smooth = true;
        CHECK(Near(c.Evaluate(0.0f), 0.0f) && Near(c.Evaluate(0.5f), 1.0f) && Near(c.Evaluate(1.0f), 0.0f));
        CHECK(c.Evaluate(0.25f) > linear.Evaluate(0.25f)); // eases into the peak
        bool bounded = true;
        for (int i = 0; i <= 100; ++i) bounded &= c.Evaluate(i / 100.0f) <= 1.0f + 1e-5f && c.Evaluate(i / 100.0f) >= -1e-5f;
        CHECK(bounded);
        CHECK(Near(c.Evaluate(0.49f), c.Evaluate(0.51f), 1e-3f)); // zero slope at the peak
        Curve ramp;
        ramp.Keys = { { 0.0f, 0.0f }, { 0.2f, 0.9f }, { 1.0f, 1.0f } };
        ramp.Smooth = true;
        bool monotone = true;
        for (int i = 0; i < 100; ++i) monotone &= ramp.Evaluate((i + 1) / 100.0f) >= ramp.Evaluate(i / 100.0f) - 1e-6f;
        CHECK(monotone);

        // The smooth flag round trips; plain curves keep the old array format.
        ParticleSystemComponent ps;
        ps.SizeOverLifetime = c;
        const ComponentInfo* info = ComponentRegistry::Find<ParticleSystemComponent>();
        nlohmann::json j;
        for (auto& f : info->Fields) j[f.Name] = FieldToJson(f, &ps, nullptr);
        CHECK(j["Size Over Lifetime"].is_object() && j["Speed Over Lifetime"].is_array());
        ParticleSystemComponent back;
        for (auto& f : info->Fields) FieldFromJson(f, &back, j[f.Name], nullptr);
        CHECK(back.SizeOverLifetime == c && !back.SpeedOverLifetime.Smooth);

        // Hand-set tangents: a Free key's slope is used on both sides; Broken keys can kink; Auto keys are unchanged.
        Curve tangents = c;
        tangents.Keys[1].Mode = CurveTangentMode::Free;
        tangents.Keys[1].InTangent = tangents.Keys[1].OutTangent = 2.0f; // rising through the middle key
        float h = 1e-3f;
        CHECK(Near((tangents.Evaluate(0.5f + h) - tangents.Evaluate(0.5f - h)) / (2 * h), 2.0f, 0.02f));
        CHECK(Near(tangents.Evaluate(0.5f), 1.0f) && tangents.Evaluate(0.6f) > 1.0f); // overshoots, as asked
        Curve kinked = c;
        kinked.Keys[1].Mode = CurveTangentMode::Broken;
        kinked.Keys[1].InTangent = 4.0f;
        kinked.Keys[1].OutTangent = -4.0f;
        CHECK(Near((kinked.Evaluate(0.5f) - kinked.Evaluate(0.5f - h)) / h, 4.0f, 0.05f));
        CHECK(Near((kinked.Evaluate(0.5f + h) - kinked.Evaluate(0.5f)) / h, -4.0f, 0.05f));
        CHECK(kinked.OutSlope(0) == c.OutSlope(0));
        // They round trip; auto keys keep the two-number format.
        ps.SizeOverLifetime = kinked;
        nlohmann::json kj = FieldToJson(*std::find_if(info->Fields.begin(), info->Fields.end(), [](const FieldInfo& f) { return f.Name == "Size Over Lifetime"; }), &ps, nullptr);
        CHECK(kj["keys"][0].size() == 2 && kj["keys"][1].size() == 5);
        ParticleSystemComponent kb;
        for (auto& f : info->Fields)
            if (f.Name == "Size Over Lifetime") FieldFromJson(f, &kb, kj, nullptr);
        CHECK(kb.SizeOverLifetime == kinked);

        // Force over lifetime accelerates; velocity over lifetime moves without accumulating; orbits keep the radius.
        Scene scene("Forces");
        auto& reg = scene.GetRegistry();
        Entity systems[3];
        for (int i = 0; i < 3; ++i)
        {
            systems[i] = scene.CreateEntity("System");
            auto& p = reg.Emplace<ParticleSystemComponent>(systems[i]);
            p.RateOverTime = 0.0f;
            p.StartSpeed = { 0.0f, 0.0f };
            p.StartLifetime = { 10.0f, 10.0f };
            p.Shape = ParticleShape::Point;
        }
        // References only after every component exists (the pool may move them while growing).
        auto& force = reg.Get<ParticleSystemComponent>(systems[0]);
        auto& vel = reg.Get<ParticleSystemComponent>(systems[1]);
        auto& orbit = reg.Get<ParticleSystemComponent>(systems[2]);
        force.ForceOverLifetime = true;
        force.Force = { 2.0f, 0.0f, 0.0f };
        vel.VelocityOverLifetime = true;
        vel.LifetimeVelocity = { 0.0f, 0.0f, 3.0f };
        orbit.VelocityOverLifetime = true;
        orbit.OrbitalSpeed = 90.0f;
        scene.UpdateTransforms();
        ParticleSystems::Restart(scene);
        for (auto* p : { &force, &vel, &orbit }) p->Emit(1);
        ParticleSystems::Update(scene, 0.0f);
        CHECK(force.Particles.size() == 1 && vel.Particles.size() == 1 && orbit.Particles.size() == 1);
        if (force.Particles.size() != 1 || vel.Particles.size() != 1 || orbit.Particles.size() != 1) return;
        orbit.Particles[0].Position = { 1.0f, 0.0f, 0.0f };
        for (int i = 0; i < 60; ++i) ParticleSystems::Update(scene, 1.0f / 60.0f);
        // x = a t^2 / 2 = 1 after one second (semi-implicit Euler: within a few percent), velocity 2.
        CHECK(Near(force.Particles[0].Velocity.x, 2.0f, 1e-3f) && Near(force.Particles[0].Position.x, 1.0f, 0.05f));
        CHECK(Near(vel.Particles[0].Position.z, 3.0f, 1e-3f) && Near(glm::length(vel.Particles[0].Velocity), 0.0f));
        const Particle& op = orbit.Particles.back();
        // 90 degrees per second for one second: (1, 0, 0) -> (0, 0, -1) (counter-clockwise seen from above).
        CHECK(Near(glm::length(glm::vec2(op.Position.x, op.Position.z)), 1.0f, 1e-3f) && Near(op.Position.z, -1.0f, 1e-3f));
    }

    void TestAudioMeter()
    {
        // A full-scale sine has peak 1 and RMS 1/sqrt(2); RMS settles over ~0.3 s (one-pole), the peak resets on read.
        AudioMeter meter;
        const uint32_t rate = 48000, block = 480;
        std::vector<float> samples(block * 2);
        uint32_t t = 0;
        for (int b = 0; b < 200; ++b) // 2 seconds
        {
            for (uint32_t i = 0; i < block; ++i, ++t)
                samples[i * 2] = samples[i * 2 + 1] = std::sin(2.0f * 3.14159265f * 440.0f * t / rate);
            meter.Process(samples.data(), block, 2, rate);
        }
        AudioLevel level = meter.Read();
        CHECK(Near(level.Peak, 1.0f, 0.01f) && Near(level.Rms, 0.7071f, 0.01f));
        CHECK(meter.Read().Peak == 0.0f); // reset by the previous read
        // Silence decays the RMS: after 0.3 s it falls to about 1/e of the mean square.
        std::fill(samples.begin(), samples.end(), 0.0f);
        for (int b = 0; b < 30; ++b) meter.Process(samples.data(), block, 2, rate);
        CHECK(Near(meter.Read().Rms, 0.7071f * std::sqrt(std::exp(-1.0f)), 0.01f));
        // Without an audio device the mixer reports silence.
        CHECK(AudioMixer::GetLevel(0).Peak >= 0.0f && AudioMixer::GetLevel(99).Rms == 0.0f);
    }

    // Icon images in an executable's GLFW_ICON group: (size, first bytes of each image).
    std::vector<std::pair<int, std::vector<uint8_t>>> ReadExeIcons(const fs::path& exe)
    {
        std::vector<std::pair<int, std::vector<uint8_t>>> icons;
        HMODULE module = LoadLibraryExW(exe.wstring().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!module) return icons;
        if (HRSRC group = FindResourceW(module, L"GLFW_ICON", MAKEINTRESOURCEW(14)))
        {
            auto* g = (const uint8_t*)LockResource(LoadResource(module, group));
            uint16_t count = *(const uint16_t*)(g + 4);
            for (uint16_t i = 0; i < count; ++i)
            {
                const uint8_t* e = g + 6 + 14 * i;
                uint16_t id = *(const uint16_t*)(e + 12);
                if (HRSRC icon = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3)))
                {
                    auto* data = (const uint8_t*)LockResource(LoadResource(module, icon));
                    DWORD size = SizeofResource(module, icon);
                    icons.push_back({ e[0] == 0 ? 256 : e[0], std::vector<uint8_t>(data, data + std::min<DWORD>(size, 64)) });
                }
            }
        }
        FreeLibrary(module);
        return icons;
    }

    void TestAppIcon()
    {
        // Resizing fits non-square images with transparent margins and keeps edge colors (premultiplied filtering).
        AppIcon::Image src;
        src.Width = 64;
        src.Height = 32;
        src.Rgba.resize(64 * 32 * 4);
        for (size_t i = 0; i < src.Rgba.size(); i += 4) { src.Rgba[i] = 255; src.Rgba[i + 1] = 0; src.Rgba[i + 2] = 0; src.Rgba[i + 3] = 255; }
        auto small = AppIcon::Resize(src, 16);
        auto px = [&](int x, int y) { return &small.Rgba[(y * 16 + x) * 4]; };
        CHECK(small.Width == 16 && px(8, 0)[3] == 0 && px(8, 15)[3] == 0);    // letterbox rows are transparent
        CHECK(px(8, 8)[0] == 255 && px(8, 8)[3] == 255 && px(8, 8)[1] == 0); // opaque red inside
        CHECK(px(8, 4)[0] == 255);                                            // partial edge row keeps its red
        auto ico = AppIcon::BuildIco(src);
        CHECK(ico.size() > 6 && ico[2] == 1 && ico[4] == (uint8_t)std::size(AppIcon::kSizes));

        // The engine's executables carry the default icon; Build writes the project icon into the game's copy.
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        fs::path exeDir = fs::path(self).parent_path();
        auto engineIcons = ReadExeIcons(exeDir / "DimensionGame.exe");
        CHECK(engineIcons.size() == std::size(AppIcon::kSizes));
        fs::path copy = fs::temp_directory_path() / "dimension_icon_test.exe";
        std::error_code ec;
        fs::copy_file(exeDir / "DimensionGame.exe", copy, fs::copy_options::overwrite_existing, ec);
        CHECK(!ec && AppIcon::WriteToExecutable(copy, src));
        auto written = ReadExeIcons(copy);
        CHECK(written.size() == std::size(AppIcon::kSizes));
        static const uint8_t png[] = { 0x89, 'P', 'N', 'G' };
        bool allPng = !written.empty();
        for (auto& [size, bytes] : written) allPng &= bytes.size() >= 4 && std::equal(png, png + 4, bytes.begin());
        CHECK(allPng && written.back().first == 256 && written.front().first == 16);
        CHECK(written.size() == engineIcons.size() && written[0].second != engineIcons[0].second);
        fs::remove(copy, ec);
    }

    void TestMixerSnapshots()
    {
        fs::path dir = fs::temp_directory_path() / "dimension_snapshot_project";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        AudioMixer::SetVolumeDb("Music", -6.0f);
        AudioMixer::CaptureSnapshot("Normal");
        AudioMixer::SetVolumeDb("Music", -30.0f);
        AudioMixer::Groups()[0].LowPassHz = 500.0f;
        AudioMixer::CaptureSnapshot("Paused");
        CHECK(AudioMixer::Snapshots().size() == 2 && AudioMixer::FindSnapshot("Paused") == 1);

        // Blend back to Normal over one second: halfway in dB, and the cutoff halfway in octaves (500 Hz -> open).
        CHECK(AudioMixer::TransitionToSnapshot("Normal", 1.0f) && AudioMixer::IsTransitioning());
        AudioMixer::Update(0.5f);
        CHECK(Near(AudioMixer::GetVolumeDb("Music"), -18.0f, 0.01f));
        CHECK(Near(AudioMixer::Groups()[0].LowPassHz, std::sqrt(500.0f * 22000.0f), 1.0f));
        AudioMixer::Update(0.6f);
        CHECK(!AudioMixer::IsTransitioning() && Near(AudioMixer::GetVolumeDb("Music"), -6.0f) && AudioMixer::Groups()[0].LowPassHz == 0.0f);
        CHECK(AudioMixer::TransitionToSnapshot("Paused", 0.0f) && Near(AudioMixer::GetVolumeDb("Music"), -30.0f) && !AudioMixer::IsTransitioning());
        CHECK(!AudioMixer::TransitionToSnapshot("Missing", 1.0f));

        // Snapshots save with the project.
        CHECK(Project::Save());
        CHECK(Project::Open(dir));
        auto& snaps = AudioMixer::Snapshots();
        CHECK(snaps.size() == 2 && snaps[1].Name == "Paused" && snaps[1].Values.size() == 3);
        CHECK(snaps[1].Values[0].Group == "Master" && Near(snaps[1].Values[0].LowPassHz, 500.0f) && Near(snaps[1].Values[1].VolumeDb, -30.0f));
        fs::remove_all(dir, ec);
    }

    void TestSpotCookies()
    {
        // A light pointing down (-Y) with a 90 degree cone: the axis hits the cookie's center, the rim its edge circle.
        glm::vec3 dir(0, -1, 0);
        float cosOuter = std::cos(glm::radians(45.0f));
        CHECK(Near(SpotCookieUV(dir, cosOuter, 0.0f, { 0, -3, 0 }), glm::vec2(0.5f)));
        glm::vec3 right, up;
        SpotCookieBasis(dir, right, up);
        CHECK(Near(glm::dot(right, dir), 0.0f) && Near(glm::dot(up, dir), 0.0f) && Near(glm::dot(right, up), 0.0f));
        // 45 degrees off the axis toward the basis' right and up vectors: the right and top edges of the texture.
        CHECK(Near(SpotCookieUV(dir, cosOuter, 0.0f, dir + right), glm::vec2(1.0f, 0.5f)));
        CHECK(Near(SpotCookieUV(dir, cosOuter, 0.0f, dir + up), glm::vec2(0.5f, 0.0f)));
        // Rolling the light a quarter turn carries the cookie with it: the point at the basis' right now samples the
        // bottom edge.
        CHECK(Near(SpotCookieUV(dir, cosOuter, glm::half_pi<float>(), dir + right), glm::vec2(0.5f, 1.0f)));
        // Halfway out from the axis in angle tangent: a quarter of the way in from the center.
        CHECK(Near(SpotCookieUV(dir, cosOuter, 0.0f, dir + right * 0.5f), glm::vec2(0.75f, 0.5f)));

        // Point light cookies wrap an equirectangular image around the light: its forward (-Z) is the image center,
        // its up the top row, and the image turns with the light.
        glm::mat3 identity(1.0f);
        CHECK(Near(PointCookieUV(identity, { 0, 0, -2 }), glm::vec2(0.5f, 0.5f)));
        CHECK(Near(PointCookieUV(identity, { 0, 5, 0 }).y, 0.0f));
        CHECK(Near(PointCookieUV(identity, { 1, 0, 0 }), glm::vec2(0.75f, 0.5f)));
        glm::mat3 yawed = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0, 1, 0))); // forward -> -X
        CHECK(Near(PointCookieUV(yawed, { -3, 0, 0 }), glm::vec2(0.5f, 0.5f)));
        glm::mat3 pitched = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(1, 0, 0))); // up -> +Z
        CHECK(Near(PointCookieUV(pitched, { 0, 0, 1 }).y, 0.0f));
    }

    void TestSliderJoints()
    {
        // ---- 3D: a body on a vertical slider falls to its lower limit; a motor pushes another along +Axis.
        {
            Scene scene("Sliders3D");
            auto& reg = scene.GetRegistry();
            Entity drop = scene.CreateEntity("Drop");
            reg.Get<TransformComponent>(drop).Position = { 0, 5, 0 };
            reg.Get<TransformComponent>(drop).Rotation = { 0, 0, glm::radians(90.0f) }; // local X axis points up
            reg.Emplace<RigidbodyComponent>(drop).Mass = 1.0f;
            reg.Emplace<BoxColliderComponent>(drop);
            Entity motor = scene.CreateEntity("Motor");
            reg.Get<TransformComponent>(motor).Position = { 10, 5, 0 };
            reg.Emplace<RigidbodyComponent>(motor).Mass = 1.0f;
            reg.Emplace<BoxColliderComponent>(motor);
            {
                auto& j = reg.Emplace<JointComponent>(drop);
                j.Type = JointType::Slider;
                j.Axis = { 1, 0, 0 };
                j.UseLimits = true;
                j.SliderMin = -2.0f;
                j.SliderMax = 1.0f;
            }
            {
                auto& j = reg.Emplace<JointComponent>(motor);
                j.Type = JointType::Slider;
                j.Axis = { 0, 0, 1 }; // horizontal, so gravity does not act along it
                j.UseMotor = true;
                j.MotorSpeed = 2.0f;
                j.MotorMaxTorque = 1000.0f;
                j.BreakForce = 100.0f; // holds the 9.81 N weight; the motor's own force does not count
            }
            scene.UpdateTransforms();
            scene.OnRuntimeStart();
            for (int i = 0; i < 120; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            auto& dp = reg.Get<TransformComponent>(drop).Position;
            CHECK(Near(dp.y, 3.0f, 0.05f) && Near(dp.x, 0.0f, 0.02f) && Near(dp.z, 0.0f, 0.02f)); // stopped at -2
            auto& mp = reg.Get<TransformComponent>(motor).Position;
            CHECK(Near(mp.z, 4.0f, 0.2f) && Near(mp.y, 5.0f, 0.05f)); // 2 units/s for 2 s, held up by the slider
            auto& mj = reg.Get<JointComponent>(motor);
            CHECK(!mj.Broken && Near(mj.CurrentForce, 9.81f, 1.5f));
            // Reversing the motor while playing.
            mj.MotorSpeed = -4.0f;
            for (int i = 0; i < 30; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            CHECK(Near(reg.Get<TransformComponent>(motor).Position.z, 2.0f, 0.25f));
            scene.OnRuntimeStop();
        }

        // ---- 2D: the same with a prismatic joint.
        {
            Scene scene("Sliders2D");
            auto& reg = scene.GetRegistry();
            Entity drop = scene.CreateEntity("Drop");
            reg.Get<TransformComponent>(drop).Position = { 0, 5, 0 };
            reg.Emplace<Rigidbody2DComponent>(drop);
            reg.Emplace<BoxCollider2DComponent>(drop);
            Entity motor = scene.CreateEntity("Motor");
            reg.Get<TransformComponent>(motor).Position = { 10, 5, 0 };
            reg.Emplace<Rigidbody2DComponent>(motor);
            reg.Emplace<BoxCollider2DComponent>(motor);
            {
                auto& j = reg.Emplace<Joint2DComponent>(drop);
                j.Type = Joint2DType::Slider;
                j.Axis = { 0, 1 };
                j.UseLimits = true;
                j.LowerTranslation = -2.0f;
                j.UpperTranslation = 1.0f;
            }
            {
                auto& j = reg.Emplace<Joint2DComponent>(motor);
                j.Type = Joint2DType::Slider;
                j.Axis = { 1, 0 };
                j.UseMotor = true;
                j.MotorSpeed = 2.0f;
                j.MotorMaxTorque = 1000.0f;
                j.BreakForce = 100.0f;
            }
            scene.UpdateTransforms();
            scene.OnRuntimeStart();
            for (int i = 0; i < 120; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            auto& dp = reg.Get<TransformComponent>(drop).Position;
            CHECK(Near(dp.y, 3.0f, 0.05f) && Near(dp.x, 0.0f, 0.02f));
            auto& mp = reg.Get<TransformComponent>(motor).Position;
            CHECK(Near(mp.x, 14.0f, 0.2f) && Near(mp.y, 5.0f, 0.05f));
            auto& mj = reg.Get<Joint2DComponent>(motor);
            CHECK(!mj.Broken && Near(mj.CurrentForce, 9.81f, 1.5f));
            std::printf("  3D/2D slider forces ok (2D holding force %.2f N)\n", mj.CurrentForce);
            scene.OnRuntimeStop();
        }
    }

    void TestLineCapsAndJoins()
    {
        RibbonParams params;
        params.FaceCamera = false;
        params.Normal = { 0, 0, 1 };
        // A straight line with round caps: one segment plus two half discs of 4 triangles each, every fan vertex
        // half a width from its end, and the caps reaching past the ends.
        std::vector<RibbonPoint> straight{ { { 0, 0, 0 }, 0.4f, glm::vec4(1) }, { { 2, 0, 0 }, 0.4f, glm::vec4(1) } };
        params.CapVertices = 3;
        auto capped = BuildRibbon(straight, params);
        CHECK(capped.size() == 1 + 2 * 4);
        bool onCircle = true;
        float minX = 1e9f, maxX = -1e9f;
        for (size_t i = 1; i < capped.size(); ++i)
            for (int c = 1; c < 4; ++c)
            {
                onCircle &= Near(glm::distance(capped[i].P[c], capped[i].P[0]), 0.2f, 1e-4f);
                minX = std::min(minX, capped[i].P[c].x);
                maxX = std::max(maxX, capped[i].P[c].x);
            }
        CHECK(onCircle && Near(minX, -0.2f, 1e-4f) && Near(maxX, 2.2f, 1e-4f));

        // A right-angle corner with rounded joins: two square-ended segments and a 2-triangle fan on each side.
        std::vector<RibbonPoint> corner{ { { 0, 0, 0 }, 0.4f, glm::vec4(1) }, { { 1, 0, 0 }, 0.4f, glm::vec4(1) }, { { 1, 1, 0 }, 0.4f, glm::vec4(1) } };
        params.CapVertices = 0;
        params.CornerVertices = 1;
        auto joined = BuildRibbon(corner, params);
        CHECK(joined.size() == 2 + 2 * 2);
        // Square segment ends (no miter): the first segment ends exactly at x = 1, half a width either side.
        CHECK(Near(joined[0].P[2].x, 1.0f) && Near(joined[0].P[3].x, 1.0f) && Near(std::abs(joined[0].P[2].y), 0.2f));
        // The outer fan (below-right of the left turn) has its middle vertex on the diagonal, 0.2 from the corner.
        bool diagonal = false;
        for (size_t i = 2; i < joined.size(); ++i)
            for (int c = 1; c < 4; ++c)
            {
                glm::vec3 d = joined[i].P[c] - glm::vec3(1, 0, 0);
                diagonal |= Near(glm::length(d), 0.2f, 1e-4f) && Near(d.x, 0.1414f, 1e-3f) && Near(d.y, -0.1414f, 1e-3f);
            }
        CHECK(diagonal);
        // Mitered joins are unchanged when both counts are 0.
        params.CornerVertices = 0;
        CHECK(BuildRibbon(corner, params).size() == 2);
    }

    void TestAudioFormatsAndEffects()
    {
        // The sample music is Ogg Vorbis (stb_vorbis); the chime is WAV.
        fs::path audio = fs::path(DIM_SOURCE_DIR) / "Projects" / "Sandbox" / "Assets" / "Audio";
        auto music = AudioEngine::GetClipInfo(audio / "Music.ogg");
        CHECK(music && music->Format == "Ogg Vorbis" && music->SampleRate == 22050 && music->Channels == 1);
        CHECK(music && Near(music->Seconds, 423360.0f / 22050.0f, 0.05f));
        auto chime = AudioEngine::GetClipInfo(audio / "Chime.wav");
        CHECK(chime && chime->Format == "WAV" && chime->Seconds > 0.1f);
        CHECK(!AudioEngine::GetClipInfo(audio / "Missing.ogg"));
        // Opus (libopus + opusfile): a 1 s stereo 440 Hz tone at 48 kHz, written with libsndfile.
        auto tone = AudioEngine::GetClipInfo(fs::path(DIM_SOURCE_DIR) / "Tests" / "Data" / "Tone.opus");
        CHECK(tone && tone->Format == "Opus" && tone->Channels == 2 && tone->SampleRate == 48000 && Near(tone->Seconds, 1.0f, 0.01f));
        CHECK(AssetManager::TypeFromExtension("Music.opus") == AssetType::Audio);
        // And it plays through an Audio Source (the resource manager decodes it), when there is an audio device.
        AudioEngine::Init();
        if (AudioEngine::IsAvailable())
        {
            fs::path assets = fs::temp_directory_path() / "dimension_opus_test" / "Assets";
            fs::remove_all(assets.parent_path());
            fs::create_directories(assets);
            fs::copy_file(fs::path(DIM_SOURCE_DIR) / "Tests" / "Data" / "Tone.opus", assets / "Tone.opus");
            AssetManager::Init(assets);
            Scene scene("Opus");
            Entity e = scene.CreateEntity("Speaker");
            auto& src = scene.GetRegistry().Emplace<AudioSourceComponent>(e);
            src.Clip = AssetManager::GetHandle(assets / "Tone.opus");
            src.Loop = true;
            auto rt = scene.Clone();
            rt->OnRuntimeStart();
            rt->OnUpdateRuntime(1.0f / 60.0f);
            CHECK(src.Clip.IsValid() && AudioWorld::IsPlaying(*rt, e));
            rt->OnRuntimeStop();
            AssetManager::Shutdown();
            fs::remove_all(assets.parent_path());
        }

        // Group effects round trip through the project file.
        fs::path dir = fs::temp_directory_path() / "dimension_fx_project";
        std::error_code ec;
        fs::remove_all(dir, ec);
        CHECK(Project::Open(dir));
        auto& sfx = AudioMixer::Groups()[2];
        sfx.Echo = true;
        sfx.EchoDelayMs = 250.0f;
        sfx.Reverb = true;
        sfx.ReverbRoomSize = 0.9f;
        AudioMixer::Apply(); // no device: a no-op
        CHECK(Project::Save() && Project::Open(dir));
        auto& back = AudioMixer::Groups()[2];
        CHECK(back.Echo && Near(back.EchoDelayMs, 250.0f) && back.Reverb && Near(back.ReverbRoomSize, 0.9f) && !AudioMixer::Groups()[1].Echo);
        fs::remove_all(dir, ec);
    }

    void TestDistanceSprings()
    {
        // A 1 kg body hanging from a 2 m spring joint settles where k * stretch = m * g, with k = m (2 pi f)^2.
        // f = 1 Hz: k = 39.5 N/m -> about 0.25 m of stretch. A rigid joint does not stretch at all.
        auto settle3D = [](float frequency) {
            Scene scene("Spring3D");
            auto& reg = scene.GetRegistry();
            Entity bob = scene.CreateEntity("Bob");
            reg.Get<TransformComponent>(bob).Position = { 0, 3, 0 };
            reg.Emplace<RigidbodyComponent>(bob).Mass = 1.0f;
            reg.Emplace<SphereColliderComponent>(bob).Radius = 0.2f;
            auto& j = reg.Emplace<JointComponent>(bob);
            j.Type = JointType::Distance;
            j.ConnectedAnchor = { 0, 5, 0 };
            j.SpringFrequency = frequency;
            j.SpringDamping = 1.0f;
            scene.UpdateTransforms();
            scene.OnRuntimeStart();
            for (int i = 0; i < 300; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            float y = reg.Get<TransformComponent>(bob).Position.y;
            scene.OnRuntimeStop();
            return y;
        };
        float rigid = settle3D(0.0f), spring = settle3D(1.0f);
        float stretch = 9.81f / (4.0f * glm::pi<float>() * glm::pi<float>());
        CHECK(Near(rigid, 3.0f, 0.02f) && Near(spring, 3.0f - stretch, 0.03f));

        auto settle2D = [](float frequency, bool motor) {
            Scene scene("Spring2D");
            auto& reg = scene.GetRegistry();
            Entity bob = scene.CreateEntity("Bob");
            reg.Get<TransformComponent>(bob).Position = { 0, 3, 0 };
            reg.Emplace<Rigidbody2DComponent>(bob);
            reg.Emplace<CircleCollider2DComponent>(bob);
            auto& j = reg.Emplace<Joint2DComponent>(bob);
            j.Type = Joint2DType::Distance;
            j.ConnectedAnchor = { 0, 5 };
            j.SpringFrequency = frequency;
            j.SpringDamping = 1.0f;
            j.UseMotor = motor;
            j.MotorSpeed = -0.5f; // winch: shortens the rope at 0.5 units/s
            j.MotorMaxTorque = 1000.0f;
            scene.UpdateTransforms();
            scene.OnRuntimeStart();
            for (int i = 0; i < 120; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            float y = reg.Get<TransformComponent>(bob).Position.y;
            scene.OnRuntimeStop();
            return y;
        };
        float rigid2 = settle2D(0.0f, false), spring2 = settle2D(1.0f, false), winch = settle2D(1.0f, true);
        CHECK(Near(rigid2, 3.0f, 0.02f) && spring2 < 2.9f && winch > spring2 + 0.3f);
        std::printf("  3D spring rest %.3f (expected %.3f); 2D spring %.3f, winched %.3f\n", spring, 3.0f - stretch, spring2, winch);
    }

    void TestDistanceWinch3D()
    {
        // A 1 kg bob on a rigid 2 m distance joint below a world point; the winch reels it in at 0.5 m/s, or out.
        auto run = [](float speed, float maxForce, int steps, float* length) {
            Scene scene("Winch3D");
            auto& reg = scene.GetRegistry();
            Entity bob = scene.CreateEntity("Bob");
            reg.Get<TransformComponent>(bob).Position = { 0, 3, 0 };
            reg.Emplace<RigidbodyComponent>(bob).Mass = 1.0f;
            reg.Emplace<SphereColliderComponent>(bob).Radius = 0.2f;
            auto& j = reg.Emplace<JointComponent>(bob);
            j.Type = JointType::Distance;
            j.ConnectedAnchor = { 0, 5, 0 };
            j.UseMotor = true;
            j.MotorSpeed = speed;
            j.MotorMaxTorque = maxForce;
            scene.UpdateTransforms();
            scene.OnRuntimeStart();
            for (int i = 0; i < steps; ++i) scene.OnUpdateRuntime(1.0f / 60.0f);
            float y = reg.Get<TransformComponent>(bob).Position.y;
            *length = reg.Get<JointComponent>(bob).CurrentLength;
            scene.OnRuntimeStop();
            return y;
        };
        float inLength = 0, outLength = 0, stallLength = 0;
        float reeledIn = run(-0.5f, 1000.0f, 120, &inLength);   // 2 s: 1 m shorter
        float reeledOut = run(0.5f, 1000.0f, 120, &outLength);  // 1 m longer
        float stalled = run(-0.5f, 5.0f, 120, &stallLength);    // 5 N cannot lift 9.81 N
        CHECK(Near(inLength, 1.0f, 0.02f) && Near(reeledIn, 4.0f, 0.05f));
        CHECK(Near(outLength, 3.0f, 0.02f) && Near(reeledOut, 2.0f, 0.05f));
        CHECK(stallLength > 1.9f && Near(stalled, 3.0f, 0.05f));
        std::printf("  reeled in to y %.3f (length %.3f), out to y %.3f (length %.3f); stalled at %.3f\n", reeledIn, inLength, reeledOut, outLength,
                    stalled);
    }

    void TestGltfTangents()
    {
        // A one-triangle glTF with TANGENT data, placed under a node rotated 90 degrees about Z.
        fs::path dir = fs::temp_directory_path() / "dimension_tangents";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const float data[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0,             // positions
                               0, 0, 1, 0, 0, 1, 0, 0, 1,             // normals
                               1, 0, 0, -1, 1, 0, 0, -1, 1, 0, 0, -1 }; // tangents (+X, left-handed)
        std::ofstream(dir / "tri.bin", std::ios::binary).write((const char*)data, sizeof(data));
        std::ofstream(dir / "tri.gltf") << R"({
            "asset": { "version": "2.0" },
            "buffers": [ { "uri": "tri.bin", "byteLength": 120 } ],
            "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 36 }, { "buffer": 0, "byteOffset": 36, "byteLength": 36 },
                             { "buffer": 0, "byteOffset": 72, "byteLength": 48 } ],
            "accessors": [ { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
                           { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
                           { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4" } ],
            "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "NORMAL": 1, "TANGENT": 2 } } ] } ],
            "nodes": [ { "mesh": 0, "rotation": [0, 0, 0.7071068, 0.7071068] } ],
            "scenes": [ { "nodes": [0] } ], "scene": 0 })";
        MeshData mesh;
        CHECK(MeshData::LoadFromFile(dir / "tri.gltf", mesh));
        CHECK(mesh.Vertices.size() == 3);
        if (mesh.Vertices.size() == 3)
        {
            const glm::vec4& t = mesh.Vertices[0].Tangent;
            CHECK(Near(glm::vec3(t), glm::vec3(0, 1, 0)) && t.w == -1.0f); // +X rotated to +Y, handedness kept
        }
        fs::remove_all(dir, ec);
    }

    void TestGeneratedTangents()
    {
        // Built-in primitives get tangents along +U, glTF handedness (cross(N, T) * w points up the image, -V).
        MeshData cube = MeshData::Cube();
        bool allSet = true, orthogonal = true, upImage = true;
        for (size_t f = 0; f + 2 < cube.Indices.size(); f += 3)
        {
            const Vertex &a = cube.Vertices[cube.Indices[f]], &b = cube.Vertices[cube.Indices[f + 1]], &c = cube.Vertices[cube.Indices[f + 2]];
            glm::vec3 e1 = b.Position - a.Position, e2 = c.Position - a.Position;
            glm::vec2 d1 = b.UV - a.UV, d2 = c.UV - a.UV;
            float det = d1.x * d2.y - d2.x * d1.y;
            glm::vec3 dPdu = (e1 * d2.y - e2 * d1.y) / det, dPdv = (e2 * d1.x - e1 * d2.x) / det;
            for (const Vertex* v : { &a, &b, &c })
            {
                allSet &= std::abs(v->Tangent.w) == 1.0f;
                orthogonal &= std::abs(glm::dot(glm::vec3(v->Tangent), v->Normal)) < 1e-4f;
                upImage &= glm::dot(glm::vec3(v->Tangent), glm::normalize(dPdu)) > 0.999f;
                upImage &= glm::dot(glm::cross(v->Normal, glm::vec3(v->Tangent)) * v->Tangent.w, dPdv) < 0.0f;
            }
        }
        CHECK(allSet && orthogonal && upImage);
        CHECK(cube.Vertices.size() == 24); // no mirrored UVs: nothing split

        // Two triangles sharing an edge, the second with mirrored UVs: the shared vertices are split.
        std::vector<Vertex> v = {
            { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1 } }, { { 1, 0, 0 }, { 0, 0, 1 }, { 1, 1 } },
            { { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 1, 1 } },
        };
        MeshData mirrored = MeshData::FromArrays(v, { 0, 1, 2, 0, 2, 3 }, "Mirrored");
        CHECK(mirrored.Vertices.size() == 6);
        bool split = true;
        for (int k = 0; k < 3; ++k)
        {
            const Vertex& right = mirrored.Vertices[mirrored.Indices[k]];
            const Vertex& left = mirrored.Vertices[mirrored.Indices[3 + k]];
            split &= right.Tangent.w == 1.0f && Near(glm::vec3(right.Tangent), glm::vec3(1, 0, 0));
            split &= left.Tangent.w == -1.0f && Near(glm::vec3(left.Tangent), glm::vec3(-1, 0, 0));
        }
        CHECK(split);

        // Authored tangents are kept; a triangle without UV area gets none.
        std::vector<Vertex> flat = { { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 0 } }, { { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0 } }, { { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0 } } };
        flat[0].Tangent = glm::vec4(0, 1, 0, -1);
        MeshData noUV = MeshData::FromArrays(flat, { 0, 1, 2 }, "NoUV");
        CHECK(noUV.Vertices[0].Tangent == glm::vec4(0, 1, 0, -1) && noUV.Vertices[1].Tangent == glm::vec4(0.0f));
    }

    void TestRootMotion()
    {
        // A root that walks +2 along Z per 1 s loop (bob in Y). Root motion keeps the root in place horizontally,
        // keeps the bob, and hands out the travel frame by frame, including across the loop seam.
        Skeleton sk;
        Joint root;
        root.Name = "Hips";
        sk.Joints = { root };
        sk.ComputeOrder();
        AnimationClip walk;
        walk.Name = "Walk";
        walk.Duration = 1.0f;
        AnimationChannel ch;
        ch.Joint = 0;
        ch.Path = AnimationPath::Translation;
        ch.Times = { 0.0f, 0.5f, 1.0f };
        ch.Values = { { 0, 0, 0, 0 }, { 0, 0.1f, 1, 0 }, { 0, 0, 2, 0 } };
        walk.Channels.push_back(ch);

        RootMotionState state;
        glm::vec3 travelled(0.0f);
        float time = 0.0f, dt = 1.0f / 60.0f;
        bool pinned = true, bobbed = false;
        for (int frame = 0; frame <= 150; ++frame) // 2.5 loops
        {
            Pose pose;
            Animation::Sample(sk, walk, time, pose);
            travelled += Animation::ExtractRootMotion(sk, 0, pose, state, { 0, 0, 0 }, { 0, 0, 2 }, time, true, "Walk");
            pinned &= Near(pose.Translations[0].z, 0.0f, 1e-4f) && Near(pose.Translations[0].x, 0.0f, 1e-4f);
            bobbed |= pose.Translations[0].y > 0.05f;
            time = std::fmod(time + dt, walk.Duration);
        }
        CHECK(pinned && bobbed);
        CHECK(Near(travelled.z, 150.0f * dt * 2.0f, 0.02f) && Near(travelled.y, 0.0f) && Near(travelled.x, 0.0f));
        // Switching motion restarts tracking: no jump on the first frame of the new clip.
        Pose pose;
        Animation::Sample(sk, walk, 0.9f, pose);
        CHECK(glm::length(Animation::ExtractRootMotion(sk, 0, pose, state, { 0, 0, 0 }, { 0, 0, 2 }, 0.9f, true, "Run")) == 0.0f);
    }

    void TestUINavigation()
    {
        // Layout (pixels):   [Button 0,0]      [Toggle 300,0]
        //                    [Slider 0,100]
        //                    [Dropdown 0,200]  [Field 300,200]
        Scene scene("Nav");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        reg.Emplace<CanvasComponent>(canvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
        auto widget = [&](const char* name, glm::vec2 pos) {
            Entity e = scene.CreateEntity(name, canvas);
            auto& r = reg.Emplace<UIRectComponent>(e);
            r.AnchorMin = r.AnchorMax = { 0, 0 };
            r.Pivot = { 0, 0 };
            r.Position = pos;
            r.Size = { 200, 40 };
            return e;
        };
        Entity button = widget("Button", { 0, 0 });
        reg.Emplace<UIImageComponent>(button);
        reg.Emplace<UIButtonComponent>(button);
        reg.Emplace<ScriptComponent>(button).Add("ClickCounter");
        Entity toggle = widget("Toggle", { 300, 0 });
        reg.Emplace<UIToggleComponent>(toggle);
        Entity slider = widget("Slider", { 0, 100 });
        {
            auto& s = reg.Emplace<UISliderComponent>(slider);
            s.MinValue = 0;
            s.MaxValue = 10;
            s.Value = 5;
            s.WholeNumbers = true;
        }
        Entity dropdown = widget("Dropdown", { 0, 200 });
        reg.Emplace<UIDropdownComponent>(dropdown); // options A, B, C; value 0
        Entity field = widget("Field", { 300, 200 });
        reg.Emplace<UIInputFieldComponent>(field);
        scene.UpdateTransforms();

        auto runtime = scene.Clone();
        runtime->OnRuntimeStart();
        auto& rr = runtime->GetRegistry();
        glm::vec2 screen(1000, 1000);
        auto press = [&](auto setter) {
            UI::PointerState p;
            p.Position = { 900, 900 }; // away from every widget
            setter(p);
            UI::Update(*runtime, p, screen);
        };
        auto sel = [&] { return runtime->GetUISelected(); };

        press([](UI::PointerState& p) { p.NavDown = true; });
        CHECK(sel() == button); // nothing selected: navigation starts at the top-left widget
        press([](UI::PointerState& p) { p.NavRight = true; });
        CHECK(sel() == toggle);
        press([](UI::PointerState& p) { p.NavDown = true; });
        CHECK(sel() == field); // straight down from the toggle
        press([](UI::PointerState& p) { p.NavLeft = true; });
        CHECK(sel() == dropdown);
        press([](UI::PointerState& p) { p.NavUp = true; });
        CHECK(sel() == slider);
        // Left/Right step a selected slider instead of moving.
        press([](UI::PointerState& p) { p.NavRight = true; });
        press([](UI::PointerState& p) { p.NavRight = true; });
        CHECK(sel() == slider && Near(rr.Get<UISliderComponent>(slider).Value, 7.0f));
        press([](UI::PointerState& p) { p.NavUp = true; });
        int clicksBefore = ClickCounter::Clicks;
        press([](UI::PointerState& p) { p.Submit = true; });
        CHECK(sel() == button && ClickCounter::Clicks == clicksBefore + 1);
        // Toggle flips on Submit.
        press([](UI::PointerState& p) { p.NavRight = true; });
        press([](UI::PointerState& p) { p.Submit = true; });
        CHECK(rr.Get<UIToggleComponent>(toggle).IsOn);
        // Dropdown: Submit opens, Down moves through the list, Submit picks, and the list closes.
        press([](UI::PointerState& p) { p.NavDown = true; });
        press([](UI::PointerState& p) { p.NavLeft = true; });
        CHECK(sel() == dropdown);
        press([](UI::PointerState& p) { p.Submit = true; });
        CHECK(rr.Get<UIDropdownComponent>(dropdown).Open);
        press([](UI::PointerState& p) { p.NavDown = true; });
        press([](UI::PointerState& p) { p.NavDown = true; });
        press([](UI::PointerState& p) { p.Submit = true; });
        CHECK(!rr.Get<UIDropdownComponent>(dropdown).Open && rr.Get<UIDropdownComponent>(dropdown).Value == 2);
        // A mouse click leaves navigation: Submit (Space) no longer presses the clicked button.
        UI::PointerState click;
        click.Position = { 50, 20 };
        click.Down = click.Pressed = true;
        UI::Update(*runtime, click, screen);
        click.Down = click.Pressed = false;
        click.Released = true;
        UI::Update(*runtime, click, screen);
        int afterMouse = ClickCounter::Clicks;
        press([](UI::PointerState& p) { p.Submit = true; });
        CHECK(sel() == button && !runtime->IsUINavigating() && ClickCounter::Clicks == afterMouse);
        // Submit focuses a text field; Cancel leaves it.
        runtime->SetUISelected(field);
        runtime->SetUINavigating(true);
        press([](UI::PointerState& p) { p.Submit = true; });
        CHECK(rr.Get<UIInputFieldComponent>(field).Focused);
        press([](UI::PointerState& p) { p.Cancel = true; });
        CHECK(!rr.Get<UIInputFieldComponent>(field).Focused);
        runtime->OnRuntimeStop();

        // Explicit links (UI Navigation): the button's Down goes to the field, Right nowhere; the slider opts out of
        // arrow navigation, and an Automatic widget's link overrides the nearest-widget rule.
        {
            auto& nav = reg.Emplace<UINavigationComponent>(button);
            nav.Mode = UINavigationMode::Explicit;
            nav.SelectOnDown = field;
            reg.Emplace<UINavigationComponent>(slider).Mode = UINavigationMode::None;
            reg.Emplace<UINavigationComponent>(dropdown).SelectOnUp = toggle;
        }
        auto linked = scene.Clone();
        linked->OnRuntimeStart();
        runtime = std::move(linked);
        runtime->SetUISelected(button);
        runtime->SetUINavigating(true);
        press([](UI::PointerState& p) { p.NavRight = true; });
        CHECK(sel() == button); // Explicit without a Right link: stays
        press([](UI::PointerState& p) { p.NavDown = true; });
        CHECK(sel() == field);
        press([](UI::PointerState& p) { p.NavLeft = true; });
        CHECK(sel() == dropdown);
        press([](UI::PointerState& p) { p.NavUp = true; });
        CHECK(sel() == toggle); // the link, not the slider straight above (which also opted out)
        runtime->SetUISelected(field);
        press([](UI::PointerState& p) { p.NavUp = true; });
        CHECK(sel() == toggle);
        press([](UI::PointerState& p) { p.NavLeft = true; });
        CHECK(sel() == button);
        // Links survive a scene round trip (entity references by GUID).
        auto back = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        CHECK(back != nullptr);
        Entity backButton = back->FindEntityByName("Button"), backField = back->FindEntityByName("Field");
        auto* backNav = back->GetRegistry().TryGet<UINavigationComponent>(backButton);
        CHECK(backNav && backNav->Mode == UINavigationMode::Explicit && backNav->SelectOnDown == backField && backNav->SelectOnUp.IsNull());

        // The editor's Visualize Navigation overlay: every move each reachable widget offers, explicit or automatic.
        auto links = UI::NavigationLinks(*runtime, UI::ComputeLayout(*runtime, screen));
        auto linkTo = [&](Entity from, UI::NavigationDirection d) -> const UI::NavigationLink* {
            for (auto& l : links)
                if (l.From == from && l.Direction == d) return &l;
            return nullptr;
        };
        auto* down = linkTo(button, UI::NavigationDirection::Down);
        CHECK(down && down->To == field && down->Explicit && Near(down->ToMin, { 300, 200 }));
        CHECK(!linkTo(button, UI::NavigationDirection::Right) && !linkTo(button, UI::NavigationDirection::Up)); // Explicit: only its link
        auto* up = linkTo(dropdown, UI::NavigationDirection::Up);
        CHECK(up && up->To == toggle && up->Explicit);
        auto* right = linkTo(dropdown, UI::NavigationDirection::Right);
        CHECK(right && right->To == field && !right->Explicit); // automatic nearest widget
        bool sliderListed = false;
        for (auto& l : links) sliderListed |= l.From == slider || l.To == slider; // opted out of navigation
        CHECK(!sliderListed);
        runtime->OnRuntimeStop();
    }

    void TestUIRectDragging()
    {
        // Stretched anchors, an off-center pivot and a scaled canvas: dragging an edge moves only that edge.
        Scene scene("Drag");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        auto& cv = reg.Emplace<CanvasComponent>(canvas);
        cv.ScaleMode = CanvasScaleMode::ScaleWithScreenSize;
        cv.ReferenceResolution = { 1920, 1080 };
        Entity e = scene.CreateEntity("Panel", canvas);
        auto& r = reg.Emplace<UIRectComponent>(e);
        r.AnchorMin = { 0.2f, 0.3f };
        r.AnchorMax = { 0.6f, 0.7f };
        r.Pivot = { 0.25f, 0.75f };
        r.Position = { 10, -20 };
        r.Size = { 30, 40 };
        scene.UpdateTransforms();
        glm::vec2 screen(960, 540); // canvas scale 0.5
        auto layout = [&] { UI::ComputeLayout(scene, screen); return std::make_pair(r.ComputedMin, r.ComputedMax); };
        auto [min0, max0] = layout();
        CHECK(Near(r.ComputedScale, 0.5f));
        auto drag = [&](glm::ivec2 sides, glm::vec2 delta) {
            glm::vec2 p = r.Position, s = r.Size;
            UI::DragRect(r, sides, delta, p, s);
            r.Position = p;
            r.Size = s;
            return layout();
        };
        auto [min1, max1] = drag({ 1, 0 }, { 24, 0 }); // right edge
        CHECK(Near(max1, max0 + glm::vec2(24, 0)) && Near(min1, min0) && Near(r.Size.x, 30.0f + 48.0f));
        auto [min2, max2] = drag({ -1, -1 }, { -10, 6 }); // top-left corner
        CHECK(Near(min2, min1 + glm::vec2(-10, 6)) && Near(max2, max1));
        auto [min3, max3] = drag({ 0, 1 }, { 0, -8 }); // bottom edge up
        CHECK(Near(max3, max2 + glm::vec2(0, -8)) && Near(min3, min2));
        auto [min4, max4] = drag({ 0, 0 }, { 8, 8 }); // move
        CHECK(Near(min4, min3 + glm::vec2(8)) && Near(max4, max3 + glm::vec2(8)));
    }

    void TestSpriteSlicing()
    {
        // A 20 x 10 image: an opaque 4 x 3 block at (1, 1), an L of touching pixels at (12, 2), a lone pixel at (18, 8)
        // and one at (8, 1) (the top row's second, by its x).
        int w = 20, h = 10;
        std::vector<uint32_t> px((size_t)w * h, 0u);
        auto set = [&](int x, int y) { px[(size_t)y * w + x] = 0xFF00FF00u; };
        for (int y = 1; y < 4; ++y)
            for (int x = 1; x < 5; ++x) set(x, y);
        for (int y = 2; y < 7; ++y) set(12, y);
        for (int x = 12; x < 15; ++x) set(x, 7);
        set(18, 8);
        set(8, 1);
        set(9, 2); // touching (8, 1) diagonally
        auto islands = SpriteSlicing::Islands(px, w, h, 8, 2);
        CHECK(islands.size() == 3);
        if (islands.size() == 3)
            CHECK(islands[0] == glm::ivec4(1, 1, 4, 3) && islands[1] == glm::ivec4(8, 1, 2, 2) && islands[2] == glm::ivec4(12, 2, 3, 6));
        CHECK(SpriteSlicing::Islands(px, w, h, 8, 1).size() == 4); // the lone pixel too
        // Grids: equal cells covering the image; empty ones left out when asked.
        auto grid = SpriteSlicing::Grid(w, h, 4, 2);
        CHECK(grid.size() == 8 && grid[0] == glm::ivec4(0, 0, 5, 5) && grid[7] == glm::ivec4(15, 5, 5, 5));
        auto filled = SpriteSlicing::Grid(w, h, 4, 2, &px);
        CHECK(filled.size() == 5); // the top row's last cell and the bottom row's first two are empty
        auto sized = SpriteSlicing::GridBySize(w, h, { 8, 8 }, { 1, 0 }, { 2, 0 });
        CHECK(sized.size() == 4 && sized[1] == glm::ivec4(11, 0, 8, 8) && sized[3] == glm::ivec4(11, 8, 8, 2));
        // Saved with the texture: the Sandbox's tiles are named.
        std::vector<SpriteRect> named = { { "A", { 0, 0, 4, 4 } }, { "B", { 4, 0, 2, 9 } } };
        auto back = SpriteSlicing::FromJson(SpriteSlicing::ToJson(named));
        CHECK(back.size() == 2 && back[1].Name == "B" && back[1].Rect == glm::ivec4(4, 0, 2, 9));
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
        AssetHandle tiles = AssetManager::GetHandle("Sprites/Tiles.png");
        const SpriteRect* bush = AssetManager::FindSprite(tiles, "Bush");
        CHECK(AssetManager::GetSprites(tiles).size() == 8 && bush && bush->Rect == glm::ivec4(32, 16, 16, 16) && !AssetManager::FindSprite(tiles, "Tree"));
        AssetManager::Shutdown();
    }

    void TestSpriteAtlas()
    {
        // Packing: nothing overlaps or leaves the page, padding kept; the page grows (powers of two) as needed, and
        // what cannot fit Max Size is left out.
        std::vector<glm::ivec2> sizes = { { 64, 64 }, { 32, 16 }, { 100, 20 }, { 16, 64 }, { 8, 8 }, { 50, 50 } };
        RectPacking p = PackRects(sizes, 2, 1024);
        bool inside = true, apart = true;
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            inside &= p.Placed[i] && p.Positions[i].x >= 2 && p.Positions[i].y >= 2 && p.Positions[i].x + sizes[i].x <= (int)p.Width - 2 &&
                      p.Positions[i].y + sizes[i].y <= (int)p.Height - 2;
            for (size_t j = 0; j < i; ++j)
            {
                glm::ivec2 a = p.Positions[i], b = p.Positions[j];
                bool separate = a.x + sizes[i].x + 2 <= b.x || b.x + sizes[j].x + 2 <= a.x || a.y + sizes[i].y + 2 <= b.y || b.y + sizes[j].y + 2 <= a.y;
                apart &= separate;
            }
        }
        CHECK(inside && apart && p.Width <= 256 && p.Height <= 256 && (p.Width & (p.Width - 1)) == 0);
        RectPacking tight = PackRects({ { 300, 300 }, { 10, 10 } }, 2, 256);
        CHECK(!tight.Placed[0] && tight.Placed[1]);

        // The Sandbox's atlas: its sprites packed, each region holding that sprite's pixels (and its edge repeated into
        // the padding); it saves and loads.
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
        AssetHandle handle = AssetManager::GetHandle("Sprites/Sprites.spriteatlas");
        CHECK(handle.IsValid() && AssetManager::GetMetadata(handle)->Type == AssetType::SpriteAtlas);
        auto atlas = SpriteAtlas::Load(AssetManager::GetAbsolutePath(handle));
        atlas->Pack();
        CHECK(atlas->Sprites.size() == 4 && atlas->Regions.size() == 4 && atlas->Left.empty() && atlas->Pixels.size() == (size_t)atlas->Width * atlas->Height);
        // The box alone without padding: a page that is just its image, to compare with.
        AssetHandle box = AssetManager::GetHandle("Sprites/Box.png");
        SpriteAtlas alone;
        alone.Sprites = { box };
        alone.Padding = 0;
        alone.Pack();
        glm::vec4 whole = alone.Regions.contains(box) ? alone.Regions[box] : glm::vec4(0.0f);
        int w = (int)std::lround(whole.z * alone.Width), h = (int)std::lround(whole.w * alone.Height);
        CHECK(w > 0 && h > 0 && atlas->Regions.contains(box));
        if (w > 0 && atlas->Regions.contains(box))
        {
            glm::vec4 r = atlas->Regions[box];
            int x0 = (int)std::lround(r.x * atlas->Width), y0 = (int)std::lround(r.y * atlas->Height);
            CHECK(Near((r.z - r.x) * atlas->Width, (float)w, 0.01f) && Near((r.w - r.y) * atlas->Height, (float)h, 0.01f));
            bool same = true;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) same &= atlas->Pixels[(size_t)(y0 + y) * atlas->Width + x0 + x] == alone.Pixels[(size_t)y * alone.Width + x];
            CHECK(same);
            CHECK(atlas->Pixels[(size_t)y0 * atlas->Width + x0 - 1] == alone.Pixels[0]); // the edge, repeated into the padding
        }
        fs::path copy = fs::temp_directory_path() / "dimension_atlas_test.spriteatlas";
        atlas->Padding = 4;
        CHECK(atlas->Save(copy));
        auto back = SpriteAtlas::Load(copy);
        CHECK(back->Sprites == atlas->Sprites && back->Padding == 4 && back->MaxSize == 2048);
        fs::remove(copy);
        AssetManager::Shutdown();
    }

    void TestBuildDependencies()
    {
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
        auto names = [](const AssetDependencySet& set) {
            std::set<std::string> out;
            for (AssetHandle h : set.Assets)
                if (auto* m = AssetManager::GetMetadata(h)) out.insert(m->Path.generic_string());
            return out;
        };
        auto d2 = names(AssetDependencies::Collect({ AssetManager::GetHandle("Scenes/Demo2D.scene") }));
        CHECK(d2.contains("Scenes/Demo2D.scene") && d2.contains("Sprites/Tiles.png") && d2.contains("Sprites/Player.png"));
        CHECK(!d2.contains("Models/Showcase.glb") && !d2.contains("Audio/Music.ogg") && !d2.contains("Textures/ClearSky.hdr"));
        CHECK(d2.contains("Sprites/Sprites.spriteatlas")); // its sprites ship, so the atlas they draw from does
        auto deps3 = AssetDependencies::Collect({ AssetManager::GetHandle("Scenes/Demo3D.scene") });
        auto d3 = names(deps3);
        // Through components (models, music), scene settings (sky), materials (textures), prefabs and controllers.
        for (const char* p : { "Models/Showcase.glb", "Models/Tentacle.gltf", "Audio/Music.ogg", "Textures/ClearSky.hdr", "Materials/Brick.mat",
                               "Textures/Brick_Normal.png", "Prefabs/Ball.prefab", "Animation/TentacleLayered.controller" })
        {
            if (!d3.contains(p)) std::printf("  missing from Demo3D's dependencies: %s\n", p);
            CHECK(d3.contains(p));
        }
        CHECK(!d3.contains("Scenes/Instancing.scene") && !d3.contains("Sprites/GameIcon.png"));
        // The text glTF's buffer file ships alongside it.
        bool hasBuffer = false;
        for (auto& f : deps3.ExtraFiles) hasBuffer |= f.parent_path() == fs::path("Models") && f.extension() == ".bin";
        std::error_code ec;
        bool needsBuffer = false;
        for (auto& e : fs::directory_iterator(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets/Models", ec)) needsBuffer |= e.path().extension() == ".bin";
        CHECK(hasBuffer == needsBuffer);
        std::printf("  Demo3D ships %zu assets (+%zu files), Demo2D %zu\n", deps3.Assets.size(), deps3.ExtraFiles.size(), d2.size());
        AssetManager::Shutdown();
    }

    void TestAsyncSceneLoading()
    {
        AssetManager::Init(fs::path(DIM_SOURCE_DIR) / "Projects/Sandbox/Assets");
        AssetPrefetch::Clear();
        Scene running("Running");
        Entity keep = running.CreateEntity("Keeper");
        running.DontDestroyOnLoad(keep);
        auto op = SceneManager::LoadSceneAsync("Scenes/Demo2D");
        CHECK(op != nullptr && !SceneManager::LoadSceneAsync("Scenes/Missing"));
        if (!op) return;
        op->AllowSceneActivation = false;
        SceneManager::WaitForAsyncLoads();
        // Ready but held: progress parks at 0.9, nothing to apply, and the sprites are already decoded.
        CHECK(Near(op->Progress.load(), 0.9f) && op->IsReady() && !op->IsDone && !SceneManager::HasPendingLoad());
        CHECK(AssetPrefetch::Size() > 0 && AssetPrefetch::Contains(AssetManager::GetAbsolutePath(AssetManager::GetHandle("Sprites/Tiles.png"))));
        op->AllowSceneActivation = true;
        CHECK(SceneManager::HasPendingLoad());
        CHECK(SceneManager::ApplyPending(running));
        CHECK(op->IsDone && !op->Failed && Near(op->Progress.load(), 1.0f));
        // Single load: the scene's objects arrive, persistent objects stay.
        CHECK(!running.FindEntityByName("Player").IsNull() && running.IsValid(keep));
        AssetPrefetch::Clear();
        AssetManager::Shutdown();
    }

    void TestInputWordWrap()
    {
        UI::SetTextMeasure([](const std::string& t, float) { return 10.0f * (float)UI::CodepointCount(t); }); // 10 px per character
        Scene scene("Wrap");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        reg.Emplace<CanvasComponent>(canvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
        Entity e = scene.CreateEntity("Notes", canvas);
        {
            auto& r = reg.Emplace<UIRectComponent>(e);
            r.AnchorMin = r.AnchorMax = { 0, 0 };
            r.Pivot = { 0, 0 };
            r.Size = { 110, 200 }; // 10 px padding each side: 9 characters per line
            auto& f = reg.Emplace<UIInputFieldComponent>(e);
            f.FontSize = 20.0f;
            f.MultiLine = true;
        }
        scene.UpdateTransforms();
        glm::vec2 screen(1000, 1000);
        auto& field = reg.Get<UIInputFieldComponent>(e);
        auto frame = [&](auto&& setup) {
            UI::PointerState p;
            p.Position = { 900, 900 };
            setup(p);
            UI::Update(scene, p, screen, 0.016f);
        };
        frame([](UI::PointerState& p) { p.Position = { 20, 10 }; p.Down = p.Pressed = true; });
        frame([](UI::PointerState& p) { p.Position = { 20, 10 }; p.Released = true; });
        // "hello world again" wraps into "hello " / "world " / "again" (whole words move down).
        frame([](UI::PointerState& p) { p.Text = "hello world again"; });
        CHECK(field.Caret == 17 && field.ScrollX == 0.0f);
        frame([](UI::PointerState& p) { p.LineUp = true; });
        CHECK(field.Caret == 11); // same x (5 characters in) on the line above
        frame([](UI::PointerState& p) { p.End = true; });
        CHECK(field.Caret == 11); // End stops before the wrapping space
        frame([](UI::PointerState& p) { p.Home = true; });
        CHECK(field.Caret == 6);
        frame([](UI::PointerState& p) { p.LineUp = true; });
        CHECK(field.Caret == 0);
        // Clicking the second visual line (22 px rows from y 5) at its far right lands at that line's end.
        frame([](UI::PointerState& p) { p.Position = { 105, 5 + 22 + 11 }; p.Down = p.Pressed = true; });
        frame([](UI::PointerState& p) { p.Released = true; });
        CHECK(field.Caret == 11);
        // A word longer than the line breaks between characters; with Word Wrap off nothing wraps.
        frame([](UI::PointerState& p) { p.SelectAll = true; });
        frame([](UI::PointerState& p) { p.Text = "abcdefghijklmnop"; });
        frame([](UI::PointerState& p) { p.Home = true; });
        CHECK(field.Caret == 9);
        field.WordWrap = false;
        frame([](UI::PointerState& p) { p.End = true; });
        frame([](UI::PointerState& p) { p.Home = true; });
        CHECK(field.Caret == 0);
        frame([](UI::PointerState& p) { p.End = true; });
        CHECK(field.Caret == 16 && field.ScrollX > 0.0f);
        UI::SetTextMeasure(nullptr);
    }

    void TestMultiLineInput()
    {
        UI::SetTextMeasure([](const std::string& t, float) { return 10.0f * (float)UI::CodepointCount(t); }); // 10 px per character
        Scene scene("MultiLine");
        auto& reg = scene.GetRegistry();
        Entity canvas = scene.CreateEntity("Canvas");
        reg.Emplace<CanvasComponent>(canvas).ScaleMode = CanvasScaleMode::ConstantPixelSize;
        Entity e = scene.CreateEntity("Notes", canvas);
        {
            auto& r = reg.Emplace<UIRectComponent>(e);
            r.AnchorMin = r.AnchorMax = { 0, 0 };
            r.Pivot = { 0, 0 };
            r.Size = { 300, 60 }; // text at x 10, lines 22 px tall from y 5: two lines visible
            auto& f = reg.Emplace<UIInputFieldComponent>(e);
            f.FontSize = 20.0f;
            f.MultiLine = true;
        }
        scene.UpdateTransforms();
        glm::vec2 screen(1000, 1000);
        auto& field = reg.Get<UIInputFieldComponent>(e);
        auto frame = [&](auto&& setup) {
            UI::PointerState p;
            p.Position = { 900, 900 };
            setup(p);
            UI::Update(scene, p, screen, 0.016f);
        };
        // Click to focus, then type two lines: Enter inserts a newline instead of submitting.
        frame([](UI::PointerState& p) { p.Position = { 20, 10 }; p.Down = p.Pressed = true; });
        frame([](UI::PointerState& p) { p.Position = { 20, 10 }; p.Released = true; });
        CHECK(field.Focused);
        frame([](UI::PointerState& p) { p.Text = "ab"; });
        frame([](UI::PointerState& p) { p.Enter = true; });
        frame([](UI::PointerState& p) { p.Text = "cde"; });
        CHECK(field.Focused && field.Text == "ab\ncde" && field.Caret == 6);
        // Up keeps the x position (clamped to the shorter line); Down returns to the same column.
        frame([](UI::PointerState& p) { p.LineUp = true; });
        CHECK(field.Caret == 2);
        frame([](UI::PointerState& p) { p.LineDown = true; });
        CHECK(field.Caret == 5);
        // Home/End stay on the line; Shift+Up selects back to the same column above.
        frame([](UI::PointerState& p) { p.Home = true; });
        CHECK(field.Caret == 3);
        frame([](UI::PointerState& p) { p.End = true; });
        CHECK(field.Caret == 6);
        frame([](UI::PointerState& p) { p.LineUp = true; p.Shift = true; });
        CHECK(field.SelectionAnchor == 6 && field.Caret == 2);
        // Pasting keeps newlines; a third line scrolls the field to keep the caret visible.
        frame([](UI::PointerState& p) { p.End = true; p.Ctrl = true; });
        frame([](UI::PointerState& p) { p.Paste = true; p.PasteText = "\nfg"; });
        CHECK(field.Text == "ab\ncde\nfg" && field.ScrollY > 0.0f);
        // Clicking the first visible line puts the caret there (line 1 after scrolling one line).
        float scrolled = field.ScrollY;
        frame([&](UI::PointerState& p) { p.Position = { 10 + 15, 5 + 11 }; p.Down = p.Pressed = true; });
        frame([](UI::PointerState& p) { p.Released = true; });
        size_t expectedLine = (size_t)std::floor((11 + scrolled) / 22.0f);
        CHECK(expectedLine == 1 && (field.Caret == 4 || field.Caret == 5)); // between 'c' and 'd' (rounding to the nearest edge)
        // Ctrl+Enter submits.
        frame([](UI::PointerState& p) { p.Enter = true; p.Ctrl = true; });
        CHECK(!field.Focused && field.Text == "ab\ncde\nfg");
        UI::SetTextMeasure(nullptr);
    }

    void TestComponentOrder()
    {
        Scene scene("Order");
        Entity e = scene.CreateEntity("Thing");
        auto& reg = scene.GetRegistry();
        reg.Emplace<MeshRendererComponent>(e);
        reg.Emplace<PointLightComponent>(e);
        reg.Emplace<AudioSourceComponent>(e);
        auto names = [&](Scene& s, Entity x) {
            std::vector<std::string> n;
            for (auto* i : ComponentRegistry::GetDisplayOrder(s, x)) n.push_back(i->Name);
            return n;
        };
        auto before = names(scene, e);
        CHECK(before.size() == 4 && before[0] == "Transform");
        // Move the last component to the top: it stops below Transform, which never moves.
        std::string last = before.back();
        CHECK(ComponentRegistry::MoveComponent(scene, e, last, -1));
        CHECK(ComponentRegistry::MoveComponent(scene, e, last, -1));
        CHECK(!ComponentRegistry::MoveComponent(scene, e, last, -1));
        CHECK(!ComponentRegistry::MoveComponent(scene, e, "Transform", 1));
        auto after = names(scene, e);
        CHECK(after[0] == "Transform" && after[1] == last && after[2] == before[1] && after[3] == before[2]);
        // The order saves with the scene; a component added later goes to the end.
        auto loaded = SceneSerializer::Deserialize(SceneSerializer::Serialize(scene));
        Entity le = loaded->FindEntityByName("Thing");
        CHECK(names(*loaded, le) == after);
        loaded->GetRegistry().Emplace<CameraComponent>(le);
        auto withCamera = names(*loaded, le);
        CHECK(withCamera.size() == 5 && withCamera.back() == "Camera");
    }

    const char* g_Filter = nullptr; // run only tests whose name contains this (first command-line argument; '|' separates several)

    void Run(const char* name, void (*fn)())
    {
        if (g_Filter)
        {
            bool match = false;
            for (const char* f = g_Filter; *f && !match;)
            {
                const char* end = std::strchr(f, '|');
                std::string part(f, end ? (size_t)(end - f) : std::strlen(f));
                match = !part.empty() && std::strstr(name, part.c_str()) != nullptr;
                f = end ? end + 1 : f + part.size();
            }
            if (!match) return;
        }
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
    ScriptRegistry::Register<CoroutineProbe>("CoroutineProbe");
    ScriptRegistry::Register<CharacterProbe>("CharacterProbe");
    ScriptRegistry::Register<TimerProbe>("TimerProbe");
    ScriptRegistry::Register<TriggerCounter>("TriggerCounter");
    ScriptRegistry::Register<WidgetListener>("WidgetListener");
    ScriptRegistry::Register<PersistProbe>("PersistProbe");
    ScriptRegistry::Register<BreakCounter>("BreakCounter");
    ScriptRegistry::Register<CounterA>("CounterA");
    ScriptRegistry::Register<CounterB>("CounterB");
    ScriptRegistry::Register<EnableProbe>("EnableProbe");
    ScriptRegistry::Register<CollisionProbe>("CollisionProbe");
    ScriptRegistry::Register<MouseProbe>("MouseProbe");
    ScriptRegistry::Register<AppProbe>("AppProbe");
    ScriptRegistry::Register<CharacterHitProbe>("CharacterHitProbe");
    ScriptRegistry::Register<CloneProbe>("CloneProbe").Field("Speed", &CloneProbe::Speed).Field("Target", &CloneProbe::Target);
    ScriptRegistry::Register<TriggerStayProbe>("TriggerStayProbe");
    ScriptRegistry::Register<LegacyCollisionProbe>("LegacyCollisionProbe");
    ScriptRegistry::Register<TestScript>("TestScript").Field("Speed", &TestScript::Speed).Field("Target", &TestScript::Target);

    Run("Registry basics", TestRegistryBasics);
    Run("Views", TestViews);
    Run("Registry clone", TestClonePreservesHandles);
    Run("Scene hierarchy", TestHierarchy);
    Run("Serialization round trip", TestSerializationRoundTrip);
    Run("Duplicate entity", TestDuplicateRemapsReferences);
    Run("Script runtime", TestScriptsRuntime);
    Run("Active state and tags", TestActiveState);
    Run("Camera projection", TestCameraProjection);
    Run("Time scale and cursor", TestTimeScaleAndCursor);
    Run("2D physics queries", TestPhysics2DQueries);
    Run("3D shape casts and spin", TestPhysics3DCasts);
    Run("Collision details", TestCollisionDetails);
    Run("Mouse events", TestMouseEvents);
    Run("Trigger stay", TestTriggerStay);
    Run("Instantiate and remove", TestInstantiateAndRemove);
    Run("Audio source control", TestAudioSourceControl);
    Run("Sprite animator seek", TestSpriteAnimatorSeek);
    Run("Particle emit and pause", TestParticleEmitAndPause);
    Run("Tilemap edits and terrain sample", TestTilemapEditsAndTerrainSample);
    Run("Physics settings while playing", TestPhysicsSettingsWhilePlaying);
    Run("Camera switching", TestCameraSwitching);
    Run("Joint rebuild while playing", TestJointRebuildWhilePlaying);
    Run("Rigidbody settings while playing", TestRigidbodySettingsWhilePlaying);
    Run("Physics queries and triggers", TestPhysicsQueriesAndTriggers);
    Run("App and timing control", TestAppAndTiming);
    Run("Character settings and hits", TestCharacterSettingsAndHits);
    Run("NavMesh queries and agent paths", TestNavQueriesAndPaths);
    Run("Physics 2D", TestPhysics2D);
    Run("Physics 3D", TestPhysics3D);
    Run("Inactive colliders", TestInactiveColliders);
    Run("Clustered lights", TestLightClusters);
    Run("Import cache", TestImportCache);
    Run("Model import settings", TestMeshImportSettings);
    Run("Physics layers, triggers, joints", TestPhysicsLayersTriggersJoints);
    Run("Mesh colliders and characters", TestMeshCollidersAndCharacters);
    Run("Reflection JSON", TestReflectionJson);
    Run("Material round trip", TestMaterialRoundTrip);
    Run("UI layout and clicks", TestUI);
    Run("UI widgets", TestUIWidgets);
    Run("UI dropdown and scrollbar", TestUIDropdownAndScrollbar);
    Run("UI text editing", TestUITextEditing);
    Run("Animator layers and masks", TestAnimatorLayers);
    Run("Shadow cascade splits", TestCascadeSplits);
    Run("Point shadow faces", TestPointShadowFaces);
    Run("Animation sampling", TestAnimationSampling);
    Run("Skinned glTF import", TestSkinnedImport);
    Run("glTF materials", TestGltfMaterials);
    Run("Prefab instances", TestPrefabs);
    Run("Prefab variants", TestPrefabVariants);
    Run("Scene manager", TestSceneManager);
    Run("Multi-scene runtime", TestMultiScene);
    Run("Joint motors, breaking and moving platforms", TestJointMotorsAndPlatforms);
    Run("Particle texture sheets and noise", TestParticleSheetsAndNoise);
    Run("Particle mesh shape", TestParticleMeshShape);
    Run("Particle decals", TestParticleDecals);
    Run("Script timers", TestScriptTimers);
    Run("Coroutines (C++)", TestCoroutines);
    Run("C++ script options", TestScriptOptions);
    Run("Audio mixer", TestAudioMixer);
    Run("Environment lighting", TestEnvironmentLighting);
    Run("Tilemaps", TestTilemaps);
    Run("Line and trail renderers", TestLineRenderers);
    Run("Particle simulation", TestParticles);
    Run("Particle modules", TestParticleModules);
    Run("C# scripting", TestCSharpScripts);
    Run("Multiple scripts per entity", TestMultipleScripts);
    Run("Animator state machine", TestAnimatorStateMachine);
    Run("Blend trees and animation events", TestBlendTreesAndEvents);
    Run("2D blend trees", TestBlendTrees2D);
    Run("Sub-state machines", TestSubStateMachines);
    Run("Reverb zones", TestReverbZones);
    Run("Convolution reverb", TestConvolutionReverb);
    Run("Animation retargeting", TestRetargeting);
    Run("Terrain", TestTerrain);
    Run("Terrain details", TestTerrainDetails);
    Run("Terrain stamp", TestTerrainStamp);
    Run("Terrain erosion", TestTerrainErosion);
    Run("Terrain path", TestTerrainPath);
    Run("Path placer", TestPathPlacer);
    Run("Rope snap", TestRopeSnap);
    Run("Particle lights", TestParticleLights);
    Run("Soft buoyancy", TestSoftBuoyancy);
    Run("Scene search", TestSceneSearch);
    Run("Wind zones", TestWindZones);
    Run("Terrain crater", TestTerrainCrater);
    Run("NavMesh bake", TestNavMeshBake);
    Run("NavMesh agents", TestNavMeshAgents);
    Run("Sprite atlas", TestSpriteAtlas);
    Run("Sprite slicing", TestSpriteSlicing);
    Run("Asset labels", TestAssetLabels);
    Run("Scene snapping", TestSceneSnapping);
    Run("Scene visibility", TestSceneVisibility);
    Run("Presets", TestPresets);
    Run("Import defaults and batch import settings", TestImportDefaults);
    Run("Asset references and Find References In Scene", TestAssetReferences);
    Run("Project list view and Favorites", TestProjectListAndFavorites);
    Run("Audio preview (waveform and playback)", TestAudioPreview);
    Run("Frame All and camera bookmarks", TestFrameAllAndBookmarks);
    Run("Scene view measuring tool", TestSceneMeasure);
    Run("Selection history and saved selections", TestSelectionHistory);
    Run("Hierarchy grouping and sorting", TestSceneGrouping);
    Run("Align With View and Move To View", TestViewAlign);
    Run("Scene view grid settings", TestSceneGrid);
    Run("Inspector search", TestFieldSearch);
    Run("Profiler timeline", TestProfilerTimeline);
    Run("Profiler recordings and comparison", TestProfilerRecordings);
    Run("Debug lines", TestDebugDraw);
    Run("Player prefs", TestPlayerPrefs);
    Run("Input actions", TestInputActions);
    Run("Localization", TestLocalization);
    Run("Data assets", TestDataAssets);
    Run("Project search", TestProjectSearch);
    Run("Asset packages", TestAssetPackages);
    Run("Scene sections", TestSceneSections);
    Run("Scene templates", TestSceneTemplates);
    Run("Project hub", TestProjectHub);
    Run("Scene diff", TestSceneDiff);
    Run("NavMesh root motion", TestNavRootMotion);
    Run("NavMesh moving platforms", TestNavMovingPlatforms);
    Run("NavMesh across surfaces", TestNavCrossSurfaces);
    Run("LOD groups", TestLODGroups);
    Run("LOD impostor", TestLODImpostor);
    Run("Mesh simplify", TestMeshSimplify);
    Run("Occlusion test", TestOcclusionTest);
    Run("Profiler", TestProfiler);
    Run("LOD fade and generated", TestLODFadeAndGenerated);
    Run("NavMesh obstacles", TestNavMeshObstacles);
    Run("Off-mesh links", TestOffMeshLinks);
    Run("Off-mesh doors", TestOffMeshDoors);
    Run("NavMesh agent sizes and areas", TestNavAgentSizesAndAreas);
    Run("NavMesh generated links", TestNavGeneratedLinks);
    Run("NavMesh crowd", TestNavCrowd);
    Run("NavMesh update area", TestNavUpdateArea);
    Run("Ocean", TestOcean);
    Run("Water foam", TestWaterFoam);
    Run("OBJ materials", TestObjMaterials);
    Run("Buoyancy", TestBuoyancy);
    Run("Water waves", TestWaves);
    Run("Ragdoll", TestRagdoll);
    Run("Timelines", TestTimelines);
    Run("Timeline clip tracks", TestTimelineClips);
    Run("Nested timelines", TestNestedTimelines);
    Run("Fracture", TestFracture);
    Run("Rope", TestRope);
    Run("Destructible", TestDestructible);
    Run("Boat", TestBoat);
    Run("Towing", TestTowing);
    Run("Dents", TestDents);
    Run("Microphone", TestMicrophone);
    Run("Nested prefabs", TestNestedPrefabs);
    Run("2D simple directional blend trees", TestBlendTreesDirectional);
    Run("Rich text tags", TestRichText);
    Run("Audio occlusion", TestAudioOcclusion);
    Run("Vehicles", TestVehicles);
    Run("Cloth", TestCloth);
    Run("Soft bodies", TestSoftBodies);
    Run("Motorcycles and tanks", TestMotorcycleAndTank);
    Run("Convex decomposition", TestConvexDecomposition);
    Run("Inverse kinematics", TestInverseKinematics);
    Run("Synced animator layers", TestLayerSync);
    Run("Material slots and soft shadows", TestMaterialSlotsAndSoftShadows);
    Run("Component order", TestComponentOrder);
    Run("Multi-line input fields", TestMultiLineInput);
    Run("Input field word wrap", TestInputWordWrap);
    Run("Async scene loading", TestAsyncSceneLoading);
    Run("Build dependencies", TestBuildDependencies);
    Run("UI navigation", TestUINavigation);
    Run("UI rect dragging", TestUIRectDragging);
    Run("Root motion", TestRootMotion);
    Run("glTF tangents", TestGltfTangents);
    Run("Generated tangents", TestGeneratedTangents);
    Run("Distance joint springs", TestDistanceSprings);
    Run("3D distance joint winch", TestDistanceWinch3D);
    Run("Audio formats and effects", TestAudioFormatsAndEffects);
    Run("Line caps and joins", TestLineCapsAndJoins);
    Run("Slider joints", TestSliderJoints);
    Run("Spot light cookies", TestSpotCookies);
    Run("Audio mixer snapshots", TestMixerSnapshots);
    Run("Audio sends", TestAudioSends);
    Run("Application icon", TestAppIcon);
    Run("Audio level meter", TestAudioMeter);
    Run("Smooth curves and particle forces", TestSmoothCurvesAndParticleForces);
    Run("UI grid and horizontal scroll", TestUIGridAndHorizontalScroll);

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
