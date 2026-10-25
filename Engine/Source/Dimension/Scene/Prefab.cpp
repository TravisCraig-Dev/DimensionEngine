#include "Dimension/Scene/Prefab.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include <fstream>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Dimension::Prefabs
{
    using json = nlohmann::json;

    namespace
    {
        std::optional<json> ReadRaw(AssetHandle prefab)
        {
            fs::path path = AssetManager::GetAbsolutePath(prefab);
            std::ifstream in(path);
            if (!in) return std::nullopt;
            try
            {
                return json::parse(in);
            }
            catch (const std::exception& e)
            {
                Log::Error("Invalid prefab '{}': {}", path.string(), e.what());
            }
            return std::nullopt;
        }

        // Variant file: { "prefab": true, "base": guid, "overrides": [ { "guid", "parent"?, "components": { Component:
        // { field: value } }, "removed": [Component] } ], "added": [ entity records ], "removedEntities": [guid] }.
        // Entities keep the base's ids, so instances and their overrides line up across the chain.
        json ApplyVariant(json base, const json& variant)
        {
            json& entities = base["entities"];
            std::unordered_set<uint64_t> removed;
            for (auto& id : variant.value("removedEntities", json::array()))
                if (id.is_number_unsigned()) removed.insert(id.get<uint64_t>());
            if (!removed.empty())
            {
                json kept = json::array();
                for (auto& rec : entities)
                    if (!removed.contains(rec.value("guid", uint64_t(0)))) kept.push_back(rec);
                entities = std::move(kept);
            }
            std::unordered_map<uint64_t, size_t> index;
            for (size_t i = 0; i < entities.size(); ++i) index[entities[i].value("guid", uint64_t(0))] = i;
            for (auto& o : variant.value("overrides", json::array()))
            {
                auto it = index.find(o.value("guid", uint64_t(0)));
                if (it == index.end()) continue; // the base no longer has it
                json& rec = entities[it->second];
                if (o.contains("parent")) rec["parent"] = o["parent"];
                json& comps = rec["components"];
                for (auto& name : o.value("removed", json::array()))
                    if (name.is_string()) comps.erase(name.get<std::string>());
                for (auto& [name, fields] : o.value("components", json::object()).items())
                {
                    if (!comps.contains(name) || !comps[name].is_object()) comps[name] = json::object();
                    for (auto& [field, value] : fields.items()) comps[name][field] = value;
                }
            }
            for (auto& rec : variant.value("added", json::array())) entities.push_back(rec);
            return base;
        }

        // The prefab's full content: a plain prefab as stored, a variant resolved through its chain of bases.
        std::optional<json> LoadPrefabJson(AssetHandle prefab, int depth = 0)
        {
            auto j = ReadRaw(prefab);
            if (!j) return std::nullopt;
            if (j->contains("base"))
            {
                AssetHandle base((*j)["base"].get<uint64_t>());
                if (depth > 16 || base == prefab)
                {
                    Log::Error("Prefab variant '{}' has a cycle in its bases", AssetManager::GetAbsolutePath(prefab).filename().string());
                    return std::nullopt;
                }
                auto resolved = LoadPrefabJson(base, depth + 1);
                if (!resolved)
                {
                    Log::Error("Base prefab of variant '{}' is missing", AssetManager::GetAbsolutePath(prefab).filename().string());
                    return std::nullopt;
                }
                return ApplyVariant(std::move(*resolved), *j);
            }
            if (j->contains("entities") && (*j)["entities"].is_array() && !(*j)["entities"].empty()) return j;
            return std::nullopt;
        }

        // What makes `full` (resolved content) differ from `base`, in variant form.
        json DiffVariant(AssetHandle baseHandle, const json& base, const json& full)
        {
            json v{ { "prefab", true }, { "base", (uint64_t)baseHandle }, { "overrides", json::array() }, { "added", json::array() },
                    { "removedEntities", json::array() } };
            std::unordered_map<uint64_t, const json*> baseById;
            for (auto& rec : base["entities"]) baseById[rec.value("guid", uint64_t(0))] = &rec;
            std::unordered_set<uint64_t> present;
            for (auto& rec : full["entities"])
            {
                uint64_t id = rec.value("guid", uint64_t(0));
                present.insert(id);
                auto it = baseById.find(id);
                if (it == baseById.end())
                {
                    v["added"].push_back(rec);
                    continue;
                }
                const json& b = *it->second;
                json o{ { "guid", id } };
                if (rec.value("parent", uint64_t(0)) != b.value("parent", uint64_t(0))) o["parent"] = rec.value("parent", uint64_t(0));
                json changed = json::object(), removedComps = json::array();
                const json& bc = b["components"];
                for (auto& [name, data] : rec["components"].items())
                {
                    if (!bc.contains(name)) { changed[name] = data; continue; }
                    if (!data.is_object() || !bc[name].is_object())
                    {
                        if (data != bc[name]) changed[name] = data;
                        continue;
                    }
                    json fields = json::object();
                    for (auto& [field, value] : data.items())
                        if (!bc[name].contains(field) || bc[name][field] != value) fields[field] = value;
                    if (!fields.empty()) changed[name] = fields;
                }
                for (auto& [name, data] : bc.items())
                    if (!rec["components"].contains(name)) removedComps.push_back(name);
                if (!changed.empty()) o["components"] = changed;
                if (!removedComps.empty()) o["removed"] = removedComps;
                if (o.size() > 1) v["overrides"].push_back(o);
            }
            for (auto& [id, rec] : baseById)
                if (!present.contains(id)) v["removedEntities"].push_back(id);
            return v;
        }

        void CollectSubtree(Scene& scene, Entity e, std::vector<Entity>& out)
        {
            out.push_back(e);
            for (Entity c : scene.GetChildren(e)) CollectSubtree(scene, c, out);
        }

        // Entities of the instance linked to `prefab` (an inner instance of another prefab is not "ours").
        PrefabLinkComponent* LinkOf(Scene& scene, Entity e, AssetHandle prefab)
        {
            auto* link = scene.GetRegistry().TryGet<PrefabLinkComponent>(e);
            return link && link->Prefab == prefab ? link : nullptr;
        }

        // Serializes an instance in prefab-id space: entity ids become the stable SourceIds,
        // references inside the subtree are remapped, and link components are stripped.
        json BuildPrefabJson(Scene& scene, Entity root, AssetHandle prefab)
        {
            json tree = SceneSerializer::SerializeEntityTree(scene, root);
            std::vector<Entity> subtree;
            CollectSubtree(scene, root, subtree);
            std::unordered_map<uint64_t, uint64_t> ids; // scene guid -> prefab id
            for (Entity e : subtree)
            {
                uint64_t guid = scene.GetGuid(e);
                auto* link = LinkOf(scene, e, prefab);
                ids[guid] = link && link->SourceId ? link->SourceId : guid;
            }
            auto remap = [&](uint64_t g) {
                auto it = ids.find(g);
                return it != ids.end() ? it->second : g;
            };

            for (auto& rec : tree["entities"])
            {
                rec["guid"] = remap(rec["guid"].get<uint64_t>());
                if (uint64_t p = rec.value("parent", uint64_t(0))) rec["parent"] = remap(p);
                json& comps = rec["components"];
                comps.erase("Prefab Link");
                for (auto& [name, data] : comps.items())
                {
                    const ComponentInfo* info = ComponentRegistry::Find(name);
                    if (!info) continue;
                    for (auto& f : info->Fields)
                        if (f.Type == FieldType::EntityRef && data.contains(f.Name) && data[f.Name].is_number_unsigned())
                            data[f.Name] = remap(data[f.Name].get<uint64_t>());
                    if (name == "Script" && data.contains("Scripts"))
                        for (auto& s : data["Scripts"])
                            if (s.contains("Fields")) RemapScriptEntityRefs(s["Fields"], s.value("Class", std::string()), remap);
                }
            }
            tree["prefab"] = true;
            return tree;
        }

        bool WriteJson(const fs::path& path, const json& j)
        {
            std::ofstream out(path);
            if (!out) return false;
            out << j.dump(2);
            return (bool)out;
        }

        // Writes a prefab's new full content: a variant keeps only its differences from its base.
        bool WritePrefab(AssetHandle prefab, const fs::path& path, const json& full)
        {
            auto raw = ReadRaw(prefab);
            if (raw && raw->contains("base"))
            {
                AssetHandle base((*raw)["base"].get<uint64_t>());
                if (auto baseJson = LoadPrefabJson(base)) return WriteJson(path, DiffVariant(base, *baseJson, full));
                Log::Error("Cannot save variant '{}': its base prefab is missing", path.filename().string());
                return false;
            }
            return WriteJson(path, full);
        }
    }

    AssetHandle CreateVariant(AssetHandle base, const fs::path& path)
    {
        if (!LoadPrefabJson(base)) return AssetHandle::None();
        json v{ { "prefab", true }, { "base", (uint64_t)base }, { "overrides", json::array() }, { "added", json::array() }, { "removedEntities", json::array() } };
        if (!WriteJson(path, v)) return AssetHandle::None();
        AssetHandle handle = AssetManager::Import(path);
        if (handle.IsValid()) Log::Info("Created prefab variant '{}'", path.filename().string());
        return handle;
    }

    AssetHandle GetVariantBase(AssetHandle prefab)
    {
        auto raw = ReadRaw(prefab);
        return raw && raw->contains("base") && (*raw)["base"].is_number_unsigned() ? AssetHandle((*raw)["base"].get<uint64_t>()) : AssetHandle::None();
    }

    bool IsVariantOf(AssetHandle prefab, AssetHandle base)
    {
        for (int depth = 0; depth < 16 && prefab.IsValid(); ++depth)
        {
            prefab = GetVariantBase(prefab);
            if (prefab == base) return true;
        }
        return false;
    }

    void RemapScriptEntityRefs(json& fields, const std::string& scriptClass, const std::function<uint64_t(uint64_t)>& remap)
    {
        const ScriptInfo* info = ScriptRegistry::Find(scriptClass);
        if (!info || !fields.is_object()) return;
        for (auto& f : info->Fields)
            if (f.Type == FieldType::EntityRef && fields.contains(f.Name) && fields[f.Name].is_number_unsigned())
            {
                uint64_t g = fields[f.Name].get<uint64_t>();
                if (g) fields[f.Name] = remap(g);
            }
    }

    bool IsInstanceOwned(bool isRoot, const std::string& component, const std::string& field)
    {
        if (!isRoot) return false;
        if (component == "Transform") return field == "Position" || field == "Rotation";
        if (component == "Tag") return field == "Name";
        return false;
    }

    Entity Instantiate(Scene& scene, AssetHandle prefab, Entity parent)
    {
        auto tree = LoadPrefabJson(prefab);
        if (!tree)
        {
            Log::Error("Prefab not found or empty ({})", (uint64_t)prefab);
            return Entity::Null();
        }
        std::vector<std::pair<uint64_t, Entity>> mapping;
        Entity root = SceneSerializer::DeserializeEntityTree(scene, *tree, parent, true, &mapping);
        auto& reg = scene.GetRegistry();
        for (auto& [source, e] : mapping)
        {
            auto& link = reg.EmplaceOrReplace<PrefabLinkComponent>(e);
            link.Prefab = prefab;
            link.SourceId = source;
            link.IsRoot = e == root;
            link.Overrides.clear();
        }
        return root;
    }

    AssetHandle CreateFromEntity(Scene& scene, Entity root, const fs::path& path)
    {
        if (!scene.IsValid(root)) return AssetHandle::None();
        // Entities that were instances of another prefab become part of the new one.
        json tree = BuildPrefabJson(scene, root, AssetHandle::None());
        if (!WriteJson(path, tree)) return AssetHandle::None();
        AssetHandle handle = AssetManager::Import(path);
        if (!handle.IsValid()) return handle;

        std::vector<Entity> subtree;
        CollectSubtree(scene, root, subtree);
        auto& reg = scene.GetRegistry();
        for (Entity e : subtree)
        {
            uint64_t guid = scene.GetGuid(e);
            auto& link = reg.EmplaceOrReplace<PrefabLinkComponent>(e);
            link.Prefab = handle;
            link.SourceId = guid;
            link.IsRoot = e == root;
            link.Overrides.clear();
        }
        Log::Info("Created prefab '{}'", path.filename().string());
        return handle;
    }

    Entity FindInstanceRoot(Scene& scene, Entity e)
    {
        auto& reg = scene.GetRegistry();
        for (Entity cur = e; !cur.IsNull(); cur = scene.GetParent(cur))
            if (auto* link = reg.TryGet<PrefabLinkComponent>(cur); link && link->IsRoot) return cur;
        return Entity::Null();
    }

    Ref<Scene> OpenForEditing(AssetHandle prefab)
    {
        if (!LoadPrefabJson(prefab)) return nullptr;
        auto scene = MakeRef<Scene>(AssetManager::GetAbsolutePath(prefab).stem().string());
        Entity root = Instantiate(*scene, prefab);
        if (root.IsNull()) return nullptr;
        scene->UpdateTransforms();
        return scene;
    }

    Entity FindEditedRoot(Scene& scene, AssetHandle prefab)
    {
        for (auto [e, link] : scene.GetRegistry().ViewOf<PrefabLinkComponent>())
            if (link.IsRoot && link.Prefab == prefab && scene.GetParent(e).IsNull()) return e;
        return Entity::Null();
    }

    bool SaveEdited(Scene& scene, AssetHandle prefab)
    {
        Entity root = FindEditedRoot(scene, prefab);
        fs::path path = AssetManager::GetAbsolutePath(prefab);
        if (root.IsNull() || path.empty())
        {
            Log::Error("Cannot save prefab: its root object or asset is missing");
            return false;
        }
        size_t outside = 0;
        for (Entity e : scene.GetRootEntities())
            if (e != root) ++outside;
        if (outside) Log::Warn("{} object(s) outside the prefab root are not saved; parent them under '{}'", outside,
                               scene.GetRegistry().Get<TagComponent>(root).Name);
        if (!WritePrefab(prefab, path, BuildPrefabJson(scene, root, prefab))) return false;
        // New objects become linked so later saves keep their ids.
        std::vector<Entity> subtree;
        CollectSubtree(scene, root, subtree);
        auto& reg = scene.GetRegistry();
        for (Entity e : subtree)
        {
            if (auto* link = LinkOf(scene, e, prefab))
            {
                link->Overrides.clear();
                continue;
            }
            uint64_t guid = scene.GetGuid(e);
            auto& added = reg.EmplaceOrReplace<PrefabLinkComponent>(e);
            added.Prefab = prefab;
            added.SourceId = guid;
            added.IsRoot = false;
            added.Overrides.clear();
        }
        return true;
    }

    void Sync(Scene& scene, Entity root)
    {
        auto& reg = scene.GetRegistry();
        auto* rootLink = reg.TryGet<PrefabLinkComponent>(root);
        if (!rootLink || !rootLink->IsRoot) return;
        AssetHandle prefab = rootLink->Prefab;
        auto tree = LoadPrefabJson(prefab);
        if (!tree) return; // missing prefab: leave the instance as it is

        std::vector<Entity> subtree;
        CollectSubtree(scene, root, subtree);
        std::unordered_map<uint64_t, Entity> bySource;
        for (Entity e : subtree)
            if (auto* link = LinkOf(scene, e, prefab)) bySource[link->SourceId] = e;

        const json& records = (*tree)["entities"];
        // Create entities that exist in the prefab but not in the instance (parents come first in the file).
        std::unordered_set<uint64_t> sources;
        for (size_t i = 0; i < records.size(); ++i)
        {
            uint64_t source = records[i].value("guid", uint64_t(0));
            sources.insert(source);
            if (i == 0)
            {
                bySource[source] = root;
                reg.Get<PrefabLinkComponent>(root).SourceId = source;
                continue;
            }
            if (bySource.contains(source)) continue;
            uint64_t parentSource = records[i].value("parent", uint64_t(0));
            auto parentIt = bySource.find(parentSource);
            Entity parent = parentIt != bySource.end() ? parentIt->second : root;
            Entity e = scene.CreateEntity("GameObject", parent);
            auto& link = reg.Emplace<PrefabLinkComponent>(e);
            link.Prefab = prefab;
            link.SourceId = source;
            bySource[source] = e;
        }

        EntityRefResolver resolver;
        resolver.ToGuid = [&](Entity x) { return scene.IsValid(x) ? (uint64_t)scene.GetGuid(x) : uint64_t(0); };
        resolver.FromGuid = [&](uint64_t g) -> Entity {
            if (auto it = bySource.find(g); it != bySource.end()) return it->second;
            return scene.FindEntityByGuid(Guid(g));
        };
        auto toSceneGuid = [&](uint64_t g) -> uint64_t {
            auto it = bySource.find(g);
            return it != bySource.end() ? (uint64_t)scene.GetGuid(it->second) : g;
        };

        for (size_t i = 0; i < records.size(); ++i)
        {
            Entity e = bySource[records[i].value("guid", uint64_t(0))];
            bool isRoot = i == 0;
            const json& comps = records[i]["components"];
            PrefabLinkComponent link = reg.Get<PrefabLinkComponent>(e); // copy: component storage may move below

            for (auto& [name, data] : comps.items())
            {
                const ComponentInfo* info = ComponentRegistry::Find(name);
                if (!info || info->TypeId == GetComponentTypeId<PrefabLinkComponent>()) continue;
                if (!info->Has(scene, e))
                {
                    if (link.HasOverride("-" + name)) continue;
                    info->Add(scene, e);
                }
                void* comp = info->Get(scene, e);
                for (auto& f : info->Fields)
                {
                    if (!data.contains(f.Name) || IsInstanceOwned(isRoot, name, f.Name) || link.HasOverride(name + "." + f.Name)) continue;
                    FieldFromJson(f, comp, data[f.Name], &resolver);
                }
                if (info->TypeId == GetComponentTypeId<ScriptComponent>() && data.contains("Scripts") && !link.HasOverride("Script.Scripts"))
                {
                    auto* sc = static_cast<ScriptComponent*>(comp);
                    sc->Scripts.clear();
                    for (auto& s : data["Scripts"])
                    {
                        ScriptInstance& inst = sc->Add(s.value("Class", std::string()));
                        inst.Enabled = s.value("Enabled", true);
                        if (s.contains("Fields") && s["Fields"].is_object())
                        {
                            inst.Fields = s["Fields"];
                            RemapScriptEntityRefs(inst.Fields, inst.ClassName, toSceneGuid);
                        }
                    }
                }
            }
            // Components the prefab no longer has (unless the instance added them itself).
            for (auto& info : ComponentRegistry::All())
            {
                if (!info.Removable || !info.Has(scene, e) || comps.contains(info.Name)) continue;
                if (link.HasOverride("+" + info.Name)) continue;
                info.Remove(scene, e);
            }
        }

        // Linked entities whose source object was deleted from the prefab.
        for (Entity e : subtree)
        {
            if (!scene.IsValid(e)) continue;
            if (auto* link = LinkOf(scene, e, prefab); link && !link->IsRoot && !sources.contains(link->SourceId)) scene.DestroyEntity(e);
        }
        scene.UpdateTransforms();
    }

    void SyncAll(Scene& scene)
    {
        std::vector<Entity> roots;
        for (auto [e, link] : scene.GetRegistry().ViewOf<PrefabLinkComponent>())
            if (link.IsRoot) roots.push_back(e);
        for (Entity r : roots)
            if (scene.IsValid(r)) Sync(scene, r);
    }

    bool Apply(Scene& scene, Entity root)
    {
        auto& reg = scene.GetRegistry();
        auto* rootLink = reg.TryGet<PrefabLinkComponent>(root);
        if (!rootLink || !rootLink->IsRoot) return false;
        AssetHandle prefab = rootLink->Prefab;
        fs::path path = AssetManager::GetAbsolutePath(prefab);
        if (path.empty())
        {
            Log::Error("Cannot apply: the prefab asset is missing");
            return false;
        }
        if (!WritePrefab(prefab, path, BuildPrefabJson(scene, root, prefab))) return false;

        // Everything in the instance now matches the asset: adopt new objects and clear overrides.
        std::vector<Entity> subtree;
        CollectSubtree(scene, root, subtree);
        for (Entity e : subtree)
        {
            auto* link = LinkOf(scene, e, prefab);
            if (!link)
            {
                uint64_t guid = scene.GetGuid(e);
                auto& added = reg.EmplaceOrReplace<PrefabLinkComponent>(e);
                added.Prefab = prefab;
                added.SourceId = guid;
                added.IsRoot = false;
                added.Overrides.clear();
            }
            else
                link->Overrides.clear();
        }
        Log::Info("Applied changes to prefab '{}'", path.filename().string());
        SyncAll(scene); // other instances in this scene pick up the change immediately
        return true;
    }

    void Revert(Scene& scene, Entity root)
    {
        auto& reg = scene.GetRegistry();
        auto* rootLink = reg.TryGet<PrefabLinkComponent>(root);
        if (!rootLink || !rootLink->IsRoot) return;
        AssetHandle prefab = rootLink->Prefab;
        std::vector<Entity> subtree;
        CollectSubtree(scene, root, subtree);
        for (Entity e : subtree)
        {
            if (!scene.IsValid(e)) continue;
            if (auto* link = LinkOf(scene, e, prefab)) link->Overrides.clear();
            else scene.DestroyEntity(e); // objects added to the instance
        }
        Sync(scene, root);
    }

    void Unpack(Scene& scene, Entity root)
    {
        auto* rootLink = scene.GetRegistry().TryGet<PrefabLinkComponent>(root);
        if (!rootLink) return;
        AssetHandle prefab = rootLink->Prefab;
        std::vector<Entity> subtree;
        CollectSubtree(scene, root, subtree);
        for (Entity e : subtree)
            if (LinkOf(scene, e, prefab)) scene.GetRegistry().Remove<PrefabLinkComponent>(e);
    }

    void RecordFieldOverride(Scene& scene, Entity e, const std::string& component, const std::string& field)
    {
        auto* link = scene.GetRegistry().TryGet<PrefabLinkComponent>(e);
        if (!link || IsInstanceOwned(link->IsRoot, component, field) || link->HasOverride("+" + component)) return;
        link->AddOverride(component + "." + field);
    }

    void RecordComponentAdded(Scene& scene, Entity e, const std::string& component)
    {
        auto* link = scene.GetRegistry().TryGet<PrefabLinkComponent>(e);
        if (!link) return;
        if (link->HasOverride("-" + component)) std::erase(link->Overrides, "-" + component);
        else link->AddOverride("+" + component);
    }

    void RecordComponentRemoved(Scene& scene, Entity e, const std::string& component)
    {
        auto* link = scene.GetRegistry().TryGet<PrefabLinkComponent>(e);
        if (!link) return;
        std::string prefix = component + ".";
        std::erase_if(link->Overrides, [&](const std::string& o) { return o.rfind(prefix, 0) == 0; });
        if (link->HasOverride("+" + component)) std::erase(link->Overrides, "+" + component);
        else link->AddOverride("-" + component);
    }

    bool IsOverridden(Scene& scene, Entity e, const std::string& component, const std::string& field)
    {
        auto* link = scene.GetRegistry().TryGet<PrefabLinkComponent>(e);
        return link && (link->HasOverride(component + "." + field) || link->HasOverride("+" + component));
    }
}
