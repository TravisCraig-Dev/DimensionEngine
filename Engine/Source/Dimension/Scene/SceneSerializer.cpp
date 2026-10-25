#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Scene/Prefab.h"
#include <fstream>

namespace Dimension
{
    using json = nlohmann::json;

    namespace
    {
        void CollectDepthFirst(Scene& scene, Entity e, std::vector<Entity>& out)
        {
            out.push_back(e);
            for (Entity c : scene.GetChildren(e)) CollectDepthFirst(scene, c, out);
        }

        json SerializeEntity(Scene& scene, Entity e, const EntityRefResolver& resolver, Entity treeRoot)
        {
            json j;
            j["guid"] = (uint64_t)scene.GetGuid(e);
            Entity parent = scene.GetParent(e);
            j["parent"] = (parent.IsNull() || e == treeRoot) ? uint64_t(0) : (uint64_t)scene.GetGuid(parent);

            json& comps = j["components"];
            for (auto& info : ComponentRegistry::All())
            {
                void* data = info.Get(scene, e);
                if (!data) continue;
                json c = json::object();
                for (auto& f : info.Fields) c[f.Name] = FieldToJson(f, data, &resolver);
                if (info.TypeId == GetComponentTypeId<ScriptComponent>())
                {
                    json list = json::array();
                    for (auto& s : static_cast<ScriptComponent*>(data)->Scripts)
                        list.push_back({ { "Class", s.ClassName }, { "Fields", s.Fields }, { "Enabled", s.Enabled } });
                    c["Scripts"] = std::move(list);
                }
                if (info.TypeId == GetComponentTypeId<PrefabLinkComponent>())
                {
                    auto* link = static_cast<PrefabLinkComponent*>(data);
                    c["SourceId"] = link->SourceId;
                    c["Root"] = link->IsRoot;
                    c["Overrides"] = link->Overrides;
                }
                comps[info.Name] = std::move(c);
            }
            return j;
        }

        // Reads the script list; also accepts the single-script format of older scene files.
        void ReadScripts(ScriptComponent& sc, const json& data)
        {
            sc.Scripts.clear();
            if (data.contains("Scripts") && data["Scripts"].is_array())
            {
                for (auto& s : data["Scripts"])
                {
                    ScriptInstance& inst = sc.Add(s.value("Class", std::string()));
                    if (s.contains("Fields") && s["Fields"].is_object()) inst.Fields = s["Fields"];
                    inst.Enabled = s.value("Enabled", true);
                }
            }
            else if (data.contains("Class") && !data["Class"].get<std::string>().empty())
            {
                ScriptInstance& inst = sc.Add(data["Class"].get<std::string>());
                if (data.contains("Fields") && data["Fields"].is_object()) inst.Fields = data["Fields"];
            }
        }

        // Creates entities for all records, then fills components (so EntityRef
        // fields can point anywhere in the set).
        std::vector<Entity> DeserializeEntities(Scene& scene, const json& records, Entity attachParent, bool newIdentity)
        {
            std::unordered_map<uint64_t, Entity> local; // serialized guid -> created entity
            std::vector<Entity> created;
            created.reserve(records.size());

            for (auto& rec : records)
            {
                uint64_t guid = rec.value("guid", uint64_t(0));
                uint64_t parentGuid = rec.value("parent", uint64_t(0));
                Entity parent = attachParent;
                if (parentGuid)
                {
                    if (auto it = local.find(parentGuid); it != local.end()) parent = it->second;
                }
                Guid id = (newIdentity || !guid) ? Guid() : Guid(guid);
                Entity e = scene.CreateEntity("GameObject", parent, id);
                local[guid] = e;
                created.push_back(e);
            }

            EntityRefResolver resolver;
            resolver.ToGuid = [&](Entity e) { return (uint64_t)scene.GetGuid(e); };
            resolver.FromGuid = [&](uint64_t g) -> Entity {
                if (!g) return Entity::Null();
                if (auto it = local.find(g); it != local.end()) return it->second;
                return scene.FindEntityByGuid(Guid(g));
            };

            for (size_t i = 0; i < records.size(); ++i)
            {
                Entity e = created[i];
                const json& comps = records[i]["components"];
                if (!comps.is_object()) continue;
                for (auto& [name, data] : comps.items())
                {
                    const ComponentInfo* info = ComponentRegistry::Find(name);
                    if (!info)
                    {
                        Log::Warn("Unknown component '{}' skipped", name);
                        continue;
                    }
                    info->Add(scene, e);
                    void* comp = info->Get(scene, e);
                    for (auto& f : info->Fields)
                        if (data.contains(f.Name)) FieldFromJson(f, comp, data[f.Name], &resolver);
                    if (info->TypeId == GetComponentTypeId<ScriptComponent>())
                    {
                        auto* sc = static_cast<ScriptComponent*>(comp);
                        ReadScripts(*sc, data);
                        // References to entities inside the copied set must point at the new copies.
                        if (newIdentity)
                            for (auto& s : sc->Scripts)
                                Prefabs::RemapScriptEntityRefs(s.Fields, s.ClassName, [&](uint64_t g) {
                                    auto it = local.find(g);
                                    return it != local.end() ? (uint64_t)scene.GetGuid(it->second) : g;
                                });
                    }
                    if (info->TypeId == GetComponentTypeId<PrefabLinkComponent>())
                    {
                        auto* link = static_cast<PrefabLinkComponent*>(comp);
                        link->SourceId = data.value("SourceId", uint64_t(0));
                        link->IsRoot = data.value("Root", false);
                        if (data.contains("Overrides") && data["Overrides"].is_array())
                            link->Overrides = data["Overrides"].get<std::vector<std::string>>();
                    }
                }
            }
            return created;
        }
    }

    json SceneSerializer::Serialize(Scene& scene)
    {
        json j;
        j["version"] = 1;
        j["name"] = scene.GetName();
        auto& s = scene.GetSettings();
        j["settings"] = { { "ambientColor", { s.AmbientColor.r, s.AmbientColor.g, s.AmbientColor.b } },
                          { "ambientIntensity", s.AmbientIntensity },
                          { "gravity2D", { s.Gravity2D.x, s.Gravity2D.y } },
                          { "gravity", { s.Gravity.x, s.Gravity.y, s.Gravity.z } },
                          { "exposure", s.Exposure },
                          { "skyTop", { s.SkyTop.r, s.SkyTop.g, s.SkyTop.b } },
                          { "skyHorizon", { s.SkyHorizon.r, s.SkyHorizon.g, s.SkyHorizon.b } },
                          { "skyGround", { s.SkyGround.r, s.SkyGround.g, s.SkyGround.b } },
                          { "tonemapping", s.Tonemapping },
                          { "bloom", s.Bloom },
                          { "bloomIntensity", s.BloomIntensity },
                          { "bloomThreshold", s.BloomThreshold },
                          { "bloomKnee", s.BloomKnee },
                          { "bloomRadius", s.BloomRadius },
                          { "fxaa", s.FXAA },
                          { "vignette", s.Vignette },
                          { "ssao", s.SSAO },
                          { "ssaoRadius", s.SSAORadius },
                          { "ssaoIntensity", s.SSAOIntensity },
                          { "ssaoBias", s.SSAOBias },
                          { "ssaoDebug", s.SSAODebug },
                          { "uiKeyboardNavigation", s.UIKeyboardNavigation },
                          { "dof", s.DepthOfField },
                          { "focusDistance", s.FocusDistance },
                          { "focusRange", s.FocusRange },
                          { "bokehSize", s.BokehSize },
                          { "skyMode", s.SkyMode },
                          { "skyTexture", (uint64_t)s.SkyTexture },
                          { "skyIntensity", s.SkyIntensity },
                          { "skyRotation", s.SkyRotation },
                          { "reflectionIntensity", s.ReflectionIntensity } };

        EntityRefResolver resolver;
        resolver.ToGuid = [&](Entity e) { return scene.IsValid(e) ? (uint64_t)scene.GetGuid(e) : uint64_t(0); };

        std::vector<Entity> ordered;
        for (Entity root : scene.GetRootEntities()) CollectDepthFirst(scene, root, ordered);
        json& entities = j["entities"] = json::array();
        for (Entity e : ordered) entities.push_back(SerializeEntity(scene, e, resolver, Entity::Null()));
        return j;
    }

    void SceneSerializer::ReadSettings(const json& js, SceneSettings& s)
    {
        auto vec = [](const json& a, float* out, int n) {
            if (a.is_array()) for (int i = 0; i < n && i < (int)a.size(); ++i) out[i] = a[i].get<float>();
        };
        if (js.contains("ambientColor")) vec(js["ambientColor"], &s.AmbientColor.x, 3);
        s.AmbientIntensity = js.value("ambientIntensity", s.AmbientIntensity);
        if (js.contains("gravity2D")) vec(js["gravity2D"], &s.Gravity2D.x, 2);
        if (js.contains("gravity")) vec(js["gravity"], &s.Gravity.x, 3);
        s.Exposure = js.value("exposure", s.Exposure);
        if (js.contains("skyTop")) vec(js["skyTop"], &s.SkyTop.x, 3);
        if (js.contains("skyHorizon")) vec(js["skyHorizon"], &s.SkyHorizon.x, 3);
        if (js.contains("skyGround")) vec(js["skyGround"], &s.SkyGround.x, 3);
        s.Tonemapping = js.value("tonemapping", s.Tonemapping);
        s.Bloom = js.value("bloom", s.Bloom);
        s.BloomIntensity = js.value("bloomIntensity", s.BloomIntensity);
        s.BloomThreshold = js.value("bloomThreshold", s.BloomThreshold);
        s.BloomKnee = js.value("bloomKnee", s.BloomKnee);
        s.BloomRadius = js.value("bloomRadius", s.BloomRadius);
        s.FXAA = js.value("fxaa", s.FXAA);
        s.Vignette = js.value("vignette", s.Vignette);
        s.SSAO = js.value("ssao", s.SSAO);
        s.SSAORadius = js.value("ssaoRadius", s.SSAORadius);
        s.SSAOIntensity = js.value("ssaoIntensity", s.SSAOIntensity);
        s.SSAOBias = js.value("ssaoBias", s.SSAOBias);
        s.SSAODebug = js.value("ssaoDebug", s.SSAODebug);
        s.UIKeyboardNavigation = js.value("uiKeyboardNavigation", s.UIKeyboardNavigation);
        s.DepthOfField = js.value("dof", s.DepthOfField);
        s.FocusDistance = js.value("focusDistance", s.FocusDistance);
        s.FocusRange = js.value("focusRange", s.FocusRange);
        s.BokehSize = js.value("bokehSize", s.BokehSize);
        s.SkyMode = js.value("skyMode", s.SkyMode);
        s.SkyTexture = Guid(js.value("skyTexture", uint64_t(0)));
        s.SkyIntensity = js.value("skyIntensity", s.SkyIntensity);
        s.SkyRotation = js.value("skyRotation", s.SkyRotation);
        s.ReflectionIntensity = js.value("reflectionIntensity", s.ReflectionIntensity);
    }

    std::vector<Entity> SceneSerializer::DeserializeInto(Scene& scene, const json& data)
    {
        if (!data.contains("entities") || !data["entities"].is_array()) return {};
        const json& records = data["entities"];
        bool clash = false;
        for (auto& rec : records)
            if (uint64_t g = rec.value("guid", uint64_t(0)); g && !scene.FindEntityByGuid(Guid(g)).IsNull()) clash = true;
        auto created = DeserializeEntities(scene, records, Entity::Null(), clash);
        std::vector<Entity> roots;
        for (Entity e : created)
            if (scene.GetParent(e).IsNull()) roots.push_back(e);
        return roots;
    }

    Ref<Scene> SceneSerializer::Deserialize(const json& data)
    {
        auto scene = MakeRef<Scene>(data.value("name", std::string("Untitled")));
        if (data.contains("settings")) ReadSettings(data["settings"], scene->GetSettings());
        if (data.contains("entities")) DeserializeEntities(*scene, data["entities"], Entity::Null(), false);
        scene->UpdateTransforms();
        return scene;
    }

    bool SceneSerializer::SaveToFile(Scene& scene, const fs::path& path)
    {
        std::ofstream out(path);
        if (!out)
        {
            Log::Error("Cannot write scene '{}'", path.string());
            return false;
        }
        out << Serialize(scene).dump(2);
        return true;
    }

    Ref<Scene> SceneSerializer::LoadFromFile(const fs::path& path)
    {
        std::ifstream in(path);
        if (!in)
        {
            Log::Error("Cannot open scene '{}'", path.string());
            return nullptr;
        }
        try
        {
            return Deserialize(json::parse(in));
        }
        catch (const std::exception& e)
        {
            Log::Error("Failed to parse scene '{}': {}", path.string(), e.what());
            return nullptr;
        }
    }

    json SceneSerializer::SerializeEntityTree(Scene& scene, Entity root)
    {
        EntityRefResolver resolver;
        resolver.ToGuid = [&](Entity e) { return scene.IsValid(e) ? (uint64_t)scene.GetGuid(e) : uint64_t(0); };
        std::vector<Entity> ordered;
        CollectDepthFirst(scene, root, ordered);
        json j;
        j["entities"] = json::array();
        for (Entity e : ordered) j["entities"].push_back(SerializeEntity(scene, e, resolver, root));
        return j;
    }

    Entity SceneSerializer::DeserializeEntityTree(Scene& scene, const json& tree, Entity parent, bool newIdentity,
                                                  std::vector<std::pair<uint64_t, Entity>>* mapping)
    {
        if (!tree.contains("entities") || tree["entities"].empty()) return Entity::Null();
        auto created = DeserializeEntities(scene, tree["entities"], parent, newIdentity);
        if (mapping)
            for (size_t i = 0; i < created.size(); ++i)
                mapping->emplace_back(tree["entities"][i].value("guid", uint64_t(0)), created[i]);
        scene.UpdateTransforms();
        return created.empty() ? Entity::Null() : created.front();
    }

    bool SceneSerializer::SavePrefab(Scene& scene, Entity root, const fs::path& path)
    {
        std::ofstream out(path);
        if (!out) return false;
        json j = SerializeEntityTree(scene, root);
        j["prefab"] = true;
        out << j.dump(2);
        return true;
    }
}
