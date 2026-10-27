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
#include <Dimension/Scene/PathPlacer.h>
#include <Dimension/Scene/SceneSearch.h>
#include <Dimension/Physics/Wind.h>
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
        uint32_t s1 = rtc.EditLayers[(size_t)n * n + (size_t)(n / 2) * n + n / 2];
        CHECK((s1 & 255) > 200); // dirt (the first paint-only layer) at the center
        rt->OnRuntimeStop();
        CHECK(Near(heightAt(scene, 0.0f, 0.0f), before, 1e-4f)); // the edited scene is untouched
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
        CHECK((t.EditLayers[(size_t)n * n + sampleAt(0, -10)] & 255) > 240);       // dirt on the road
        CHECK((t.EditLayers[(size_t)n * n + sampleAt(0, 15)] >> 8 & 255) > 240);   // sand in the river
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
            paintOnlyEmpty &= (splat[mesh.Vertices.size() + i] & 0xFFFFFFu) == 0; // dirt, sand and moss are painted only
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
            Terrain::ApplyBrush(s, Terrain::BrushMode::PaintRock, { 20.0f, 0.0f }, 4.0f, 1.0f, 1.0f, 0.0f);
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

        // Paint-only layers: painting sand fills the second splat map there; an edit without them still saves the
        // older 3 bytes per sample, and with them 6.
        {
            TerrainComponent s = t;
            Terrain::ApplyBrush(s, Terrain::BrushMode::PaintRock, { 0.0f, 0.0f }, 4.0f, 1.0f, 1.0f, 0.0f);
            Terrain::CommitEdit(s);
            int n = Terrain::SampleCount(s);
            size_t count = (size_t)n * n;
            std::string threeBytes = s.PaintedLayers;
            for (int i = 0; i < 10; ++i) Terrain::ApplyBrush(s, Terrain::BrushMode::PaintSand, { -20.0f, 10.0f }, 5.0f, 1.0f, 0.1f, 0.0f);
            Terrain::CommitEdit(s);
            CHECK(s.PaintedLayers.size() > threeBytes.size() * 3 / 2); // 6 bytes per sample now
            std::vector<uint32_t> layers = Terrain::BuildSplat(s, Terrain::BuildMesh(s, Terrain::BuildHeights(s)));
            size_t at = (size_t)((10.0f / 60.0f + 0.5f) * (n - 1) + 0.5f) * n + (size_t)((-20.0f / 80.0f + 0.5f) * (n - 1) + 0.5f);
            uint32_t first = layers[at], second = layers[count + at];
            CHECK((second >> 8 & 255) > 200 && (first & 0xFFFFFFu) != 0xFFFFFFu && (first & 255) + (first >> 8 & 255) + (first >> 16 & 255) < 40);
            CHECK((layers[count + (size_t)(n / 2) * n + n / 2] & 0xFFFFFFu) == 0); // the rock patch has no sand
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
