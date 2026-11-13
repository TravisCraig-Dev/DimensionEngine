#include "Dimension/Scene/Prefab.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Scene/SceneSerializer.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Asset/AssetManager.h"
#include "Dimension/Core/Log.h"
#include <algorithm>
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
                json components = o.value("components", json::object()); // (items() of a temporary would dangle)
                for (auto& [name, fields] : components.items())
                {
                    if (!comps.contains(name) || !comps[name].is_object()) comps[name] = json::object();
                    for (auto& [field, value] : fields.items()) comps[name][field] = value;
                }
            }
            for (auto& rec : variant.value("added", json::array())) entities.push_back(rec);
            return base;
        }

        // Entity references inside a record's components (fields and script fields), rewritten by `remap`.
        void RemapRecordRefs(json& comps, const std::function<uint64_t(uint64_t)>& remap)
        {
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

        // The id a nested prefab's entity gets inside the prefab that contains it: stable for a given nested instance
        // (its root keeps the reference's id), so instances and their overrides line up across loads.
        uint64_t DeriveNestedId(uint64_t outerRoot, uint64_t inner)
        {
            uint64_t x = outerRoot ^ (inner * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull);
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; // splitmix64 finalizer
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            x ^= x >> 31;
            return x ? x : 1;
        }

        std::optional<json> LoadPrefabJson(AssetHandle prefab, int depth = 0);
        // Prefabs being loaded (outermost first), to catch a prefab nested inside itself.
        std::vector<AssetHandle> s_Loading;

        // Replaces nested prefab references ({ "guid", "parent", "nested": { "prefab", "variant" } }) with the inner
        // prefab's entities (its current content, with the variant's changes applied), under derived ids. Each such
        // record gets "nestedLink": { "prefab", "source", "root" } so instances know what they came from.
        json ExpandNested(json tree, int depth)
        {
            if (!tree.contains("entities")) return tree;
            bool any = false;
            for (auto& rec : tree["entities"]) any |= rec.contains("nested");
            if (!any) return tree;
            json out = json::array();
            for (auto& rec : tree["entities"])
            {
                if (!rec.contains("nested"))
                {
                    out.push_back(rec);
                    continue;
                }
                AssetHandle inner(rec["nested"].value("prefab", uint64_t(0)));
                uint64_t outerRoot = rec.value("guid", uint64_t(0)), parent = rec.value("parent", uint64_t(0));
                bool cycle = std::find(s_Loading.begin(), s_Loading.end(), inner) != s_Loading.end();
                std::optional<json> content = !cycle && depth < 32 ? LoadPrefabJson(inner, depth + 1) : std::nullopt;
                if (!content)
                {
                    // Missing inner prefab (or a nesting cycle): keep an empty object in its place.
                    if (cycle)
                    {
                        static std::unordered_set<uint64_t> reported;
                        if (reported.insert((uint64_t)inner).second)
                            Log::Error("Prefab '{}' contains itself through nested prefabs; that instance is left empty",
                                       AssetManager::GetAbsolutePath(inner).filename().string());
                    }
                    json placeholder = rec;
                    placeholder.erase("nested");
                    if (!placeholder.contains("components") || placeholder["components"].empty())
                        placeholder["components"] = { { "Tag", { { "Name", "Missing Prefab" } } } };
                    out.push_back(placeholder);
                    continue;
                }
                json resolved = ExpandNested(ApplyVariant(std::move(*content), rec["nested"].value("variant", json::object())), depth + 1);
                json& entities = resolved["entities"];
                if (entities.empty()) continue;
                uint64_t innerRoot = entities[0].value("guid", uint64_t(0));
                std::unordered_map<uint64_t, uint64_t> ids;
                for (auto& r : entities)
                {
                    uint64_t id = r.value("guid", uint64_t(0));
                    ids[id] = id == innerRoot ? outerRoot : DeriveNestedId(outerRoot, id);
                }
                auto remap = [&](uint64_t g) {
                    auto it = ids.find(g);
                    return it != ids.end() ? it->second : g;
                };
                for (auto& r : entities)
                {
                    uint64_t source = r.value("guid", uint64_t(0));
                    r["guid"] = remap(source);
                    if (source == innerRoot)
                    {
                        if (parent) r["parent"] = parent;
                        else r.erase("parent");
                    }
                    else if (uint64_t p = r.value("parent", uint64_t(0))) r["parent"] = remap(p);
                    if (r.contains("components")) RemapRecordRefs(r["components"], remap);
                    r["nestedLink"] = { { "prefab", (uint64_t)inner }, { "source", source }, { "root", source == innerRoot } };
                    out.push_back(std::move(r));
                }
            }
            tree["entities"] = std::move(out);
            return tree;
        }

        // The prefab's full content: a plain prefab as stored, a variant resolved through its chain of bases, with
        // nested prefabs expanded.
        std::optional<json> LoadPrefabJson(AssetHandle prefab, int depth)
        {
            auto j = ReadRaw(prefab);
            if (!j) return std::nullopt;
            s_Loading.push_back(prefab);
            struct Pop { ~Pop() { s_Loading.pop_back(); } } pop;
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
                return ExpandNested(ApplyVariant(std::move(*resolved), *j), depth);
            }
            if (j->contains("entities") && (*j)["entities"].is_array() && !(*j)["entities"].empty()) return ExpandNested(std::move(*j), depth);
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

        // How saving a prefab links each scene entity to it (by scene guid): its id in the file, and for parts of a
        // nested prefab, which one and the entity's id there.
        struct LinkAssignment
        {
            uint64_t Id = 0;
            AssetHandle Nested = AssetHandle::None();
            uint64_t NestedSource = 0;
            bool NestedRoot = false;
        };
        using LinkAssignments = std::unordered_map<uint64_t, LinkAssignment>;

        // Serializes an instance in prefab-id space: entity ids become the stable SourceIds, references inside the
        // subtree are remapped, and link components are stripped. Instances of other prefabs inside it (and nested
        // parts it already had) become nested references; `assigned` receives each entity's resulting link.
        json BuildPrefabJson(Scene& scene, Entity root, AssetHandle prefab, LinkAssignments* assigned = nullptr)
        {
            json tree = SceneSerializer::SerializeEntityTree(scene, root);
            std::vector<Entity> subtree;
            CollectSubtree(scene, root, subtree);
            auto& reg = scene.GetRegistry();
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
                RemapRecordRefs(comps, remap);
            }
            tree["prefab"] = true;

            // Nested prefab instances, deepest first: parts this prefab already nests, and instances of other prefabs
            // placed inside it (not the root itself: that is what the prefab is made from).
            struct Nested { Entity Root; AssetHandle Prefab; bool Existing; int Depth; };
            std::vector<Nested> nested;
            for (Entity e : subtree)
            {
                if (e == root) continue;
                auto* link = reg.TryGet<PrefabLinkComponent>(e);
                if (!link) continue;
                bool existing = link->Prefab == prefab && link->NestedRoot && link->NestedPrefab.IsValid();
                bool placed = link->Prefab != prefab && link->IsRoot && link->Prefab.IsValid();
                if (!existing && !placed) continue;
                int depth = 0;
                for (Entity p = scene.GetParent(e); !p.IsNull() && p != root; p = scene.GetParent(p)) ++depth;
                nested.push_back({ e, existing ? link->NestedPrefab : link->Prefab, existing, depth });
            }
            std::stable_sort(nested.begin(), nested.end(), [](const Nested& a, const Nested& b) { return a.Depth > b.Depth; });

            std::unordered_map<uint64_t, LinkAssignment> links; // scene guid -> link (ids change as instances collapse)
            for (Entity e : subtree) links[scene.GetGuid(e)] = { ids[scene.GetGuid(e)] };
            for (const Nested& n : nested)
            {
                if (n.Prefab == prefab) { Log::Error("A prefab cannot contain an instance of itself"); continue; }
                auto innerJson = LoadPrefabJson(n.Prefab);
                if (!innerJson) continue; // inner prefab missing: its objects stay part of this prefab
                std::vector<Entity> sub;
                CollectSubtree(scene, n.Root, sub);
                // Each entity's id in the inner prefab: members map to theirs; added objects (and references to deeper
                // nested prefabs) keep their current id.
                std::unordered_map<uint64_t, uint64_t> toInner; // id in this prefab -> id in the inner prefab's space
                for (Entity e : sub)
                {
                    uint64_t current = links[scene.GetGuid(e)].Id;
                    uint64_t inner = current;
                    auto* link = reg.TryGet<PrefabLinkComponent>(e);
                    if (n.Existing && link && link->Prefab == prefab && link->NestedPrefab == n.Prefab && link->NestedSource) inner = link->NestedSource;
                    if (!n.Existing && link && link->Prefab == n.Prefab && link->SourceId) inner = link->SourceId;
                    toInner[current] = inner;
                }
                uint64_t outerRoot = links[scene.GetGuid(n.Root)].Id;
                json content{ { "prefab", true }, { "entities", json::array() } };
                json kept = json::array();
                size_t rootAt = SIZE_MAX;
                uint64_t rootParent = 0;
                for (auto& rec : tree["entities"])
                {
                    uint64_t id = rec.value("guid", uint64_t(0));
                    auto it = toInner.find(id);
                    if (it == toInner.end())
                    {
                        kept.push_back(rec);
                        continue;
                    }
                    json r = rec;
                    r["guid"] = it->second;
                    if (id == outerRoot)
                    {
                        rootAt = kept.size();
                        rootParent = rec.value("parent", uint64_t(0));
                        r.erase("parent");
                    }
                    else if (uint64_t p = r.value("parent", uint64_t(0)))
                        if (auto pit = toInner.find(p); pit != toInner.end()) r["parent"] = pit->second;
                    RemapRecordRefs(r["components"], [&](uint64_t g) {
                        auto x = toInner.find(g);
                        return x != toInner.end() ? x->second : g;
                    });
                    if (id == outerRoot) content["entities"].insert(content["entities"].begin(), std::move(r));
                    else content["entities"].push_back(std::move(r));
                }
                if (rootAt == SIZE_MAX || content["entities"].empty()) continue;
                // The instance's changes to the inner prefab, as a variant; its root's place and name always belong to
                // this prefab.
                json variant = DiffVariant(n.Prefab, *innerJson, content);
                variant.erase("prefab");
                variant.erase("base");
                const json& rootRec = content["entities"][0];
                uint64_t innerRoot = rootRec.value("guid", uint64_t(0));
                json* rootOverride = nullptr;
                for (auto& o : variant["overrides"])
                    if (o.value("guid", uint64_t(0)) == innerRoot) rootOverride = &o;
                if (!rootOverride)
                {
                    variant["overrides"].push_back({ { "guid", innerRoot } });
                    rootOverride = &variant["overrides"].back();
                }
                const json& rc = rootRec["components"];
                for (const char* field : { "Position", "Rotation" })
                    if (rc.contains("Transform") && rc["Transform"].contains(field)) (*rootOverride)["components"]["Transform"][field] = rc["Transform"][field];
                if (rc.contains("Tag") && rc["Tag"].contains("Name")) (*rootOverride)["components"]["Tag"]["Name"] = rc["Tag"]["Name"];
                json reference{ { "guid", outerRoot }, { "nested", { { "prefab", (uint64_t)n.Prefab }, { "variant", variant } } }, { "components", json::object() } };
                if (rootParent) reference["parent"] = rootParent;
                kept.insert(kept.begin() + (std::ptrdiff_t)rootAt, std::move(reference));
                tree["entities"] = std::move(kept);
                // Where loading puts each of these objects (see ExpandNested).
                for (Entity e : sub)
                {
                    auto& a = links[scene.GetGuid(e)];
                    uint64_t inner = toInner[a.Id];
                    a = { inner == innerRoot ? outerRoot : DeriveNestedId(outerRoot, inner), n.Prefab, inner, e == n.Root };
                }
            }
            if (assigned) *assigned = std::move(links);
            return tree;
        }

        // Links the saved subtree to `prefab` as `assigned` says (adopting new objects), clearing overrides.
        void LinkSaved(Scene& scene, Entity root, AssetHandle prefab, const LinkAssignments& assigned)
        {
            std::vector<Entity> subtree;
            CollectSubtree(scene, root, subtree);
            auto& reg = scene.GetRegistry();
            for (Entity e : subtree)
            {
                auto it = assigned.find(scene.GetGuid(e));
                if (it == assigned.end()) continue;
                auto& link = reg.EmplaceOrReplace<PrefabLinkComponent>(e);
                link.Prefab = prefab;
                link.SourceId = it->second.Id;
                link.IsRoot = e == root;
                link.Overrides.clear();
                link.NestedPrefab = it->second.Nested;
                link.NestedSource = it->second.NestedSource;
                link.NestedRoot = it->second.NestedRoot;
            }
        }

        // A record's nested-prefab origin onto its entity's link.
        void ReadNestedLink(PrefabLinkComponent& link, const json& rec)
        {
            if (const auto it = rec.find("nestedLink"); it != rec.end() && it->is_object())
            {
                link.NestedPrefab = AssetHandle(it->value("prefab", uint64_t(0)));
                link.NestedSource = it->value("source", uint64_t(0));
                link.NestedRoot = it->value("root", false);
            }
            else
            {
                link.NestedPrefab = AssetHandle::None();
                link.NestedSource = 0;
                link.NestedRoot = false;
            }
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
                // Compared with the base as loaded (nested prefabs expanded on both sides).
                AssetHandle base((*raw)["base"].get<uint64_t>());
                if (auto baseJson = LoadPrefabJson(base)) return WriteJson(path, DiffVariant(base, *baseJson, ExpandNested(full, 0)));
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
        std::unordered_map<uint64_t, const json*> records;
        for (auto& rec : (*tree)["entities"]) records[rec.value("guid", uint64_t(0))] = &rec;
        for (auto& [source, e] : mapping)
        {
            auto& link = reg.EmplaceOrReplace<PrefabLinkComponent>(e);
            link.Prefab = prefab;
            link.SourceId = source;
            link.IsRoot = e == root;
            link.Overrides.clear();
            if (auto it = records.find(source); it != records.end()) ReadNestedLink(link, *it->second);
        }
        return root;
    }

    bool IsNestedPart(Scene& scene, Entity e)
    {
        auto* link = scene.GetRegistry().TryGet<PrefabLinkComponent>(e);
        return link && link->NestedPrefab.IsValid();
    }

    AssetHandle GetNestedPrefab(Scene& scene, Entity e)
    {
        auto* link = scene.GetRegistry().TryGet<PrefabLinkComponent>(e);
        return link ? link->NestedPrefab : AssetHandle::None();
    }

    bool ApplyToNested(Scene& scene, Entity part)
    {
        auto& reg = scene.GetRegistry();
        auto* partLink = reg.TryGet<PrefabLinkComponent>(part);
        if (!partLink || !partLink->NestedPrefab.IsValid()) return false;
        AssetHandle inner = partLink->NestedPrefab, outer = partLink->Prefab;
        // The nested instance's root: the nearest ancestor (or the part) that roots this nested prefab.
        Entity root = Entity::Null();
        for (Entity cur = part; !cur.IsNull(); cur = scene.GetParent(cur))
            if (auto* l = reg.TryGet<PrefabLinkComponent>(cur); l && l->Prefab == outer && l->NestedPrefab == inner && l->NestedRoot)
            {
                root = cur;
                break;
            }
        if (root.IsNull()) return false;

        // Edit the inner prefab as Prefab Mode would: its own scene, where its nested prefabs stay references.
        auto stage = OpenForEditing(inner);
        if (!stage) return false;
        auto& sreg = stage->GetRegistry();
        std::unordered_map<uint64_t, Entity> stageBySource;
        for (auto [e, link] : sreg.ViewOf<PrefabLinkComponent>())
            if (link.Prefab == inner) stageBySource[link.SourceId] = e;

        std::vector<Entity> subtree, added;
        CollectSubtree(scene, root, subtree);
        auto memberSource = [&](Entity e) -> uint64_t {
            auto* l = reg.TryGet<PrefabLinkComponent>(e);
            return l && l->Prefab == outer && l->NestedPrefab == inner ? l->NestedSource : 0;
        };
        // Entity references inside the instance point at the matching stage objects.
        EntityRefResolver toStage;
        toStage.ToGuid = [&](Entity x) -> uint64_t {
            if (!scene.IsValid(x)) return 0;
            if (uint64_t s = memberSource(x)) return s;
            return (uint64_t)scene.GetGuid(x);
        };
        toStage.FromGuid = [&](uint64_t g) -> Entity {
            auto it = stageBySource.find(g);
            return it != stageBySource.end() ? it->second : Entity::Null();
        };
        for (Entity e : subtree)
        {
            uint64_t source = memberSource(e);
            auto it = source ? stageBySource.find(source) : stageBySource.end();
            if (it == stageBySource.end())
            {
                // An object added to the instance: copy it (with its children) under its parent's counterpart.
                Entity parent = scene.GetParent(e);
                if (!memberSource(parent)) continue; // inside another added object: copied with it
                auto pit = stageBySource.find(memberSource(parent));
                if (pit == stageBySource.end()) continue;
                SceneSerializer::DeserializeEntityTree(*stage, SceneSerializer::SerializeEntityTree(scene, e), pit->second, true);
                added.push_back(e);
                continue;
            }
            Entity target = it->second;
            bool isRoot = e == root;
            for (const ComponentInfo& info : ComponentRegistry::All())
            {
                if (info.TypeId == GetComponentTypeId<PrefabLinkComponent>()) continue;
                bool has = info.Has(scene, e), stageHas = info.Has(*stage, target);
                if (!has)
                {
                    if (stageHas && info.Removable) info.Remove(*stage, target);
                    continue;
                }
                if (!stageHas) info.Add(*stage, target);
                const void* from = info.Get(scene, e);
                void* to = info.Get(*stage, target);
                for (const FieldInfo& f : info.Fields)
                {
                    if (IsInstanceOwned(isRoot, info.Name, f.Name)) continue; // where the instance sits is its own
                    FieldFromJson(f, to, FieldToJson(f, from, &toStage), &toStage);
                }
                if (info.TypeId == GetComponentTypeId<ScriptComponent>())
                {
                    auto& dst = *static_cast<ScriptComponent*>(to);
                    dst.Scripts.clear();
                    for (const auto& s : static_cast<const ScriptComponent*>(from)->Scripts)
                    {
                        ScriptInstance& copy = dst.Add(s.ClassName);
                        copy.Enabled = s.Enabled;
                        copy.Fields = s.Fields;
                    }
                }
            }
        }
        if (!SaveEdited(*stage, inner)) return false;
        // The instance now matches the inner prefab: drop its overrides there, and the added objects (they come back from
        // the prefab, linked), then refresh everything.
        for (Entity e : subtree)
            if (auto* l = reg.TryGet<PrefabLinkComponent>(e); l && memberSource(e)) l->Overrides.clear();
        for (Entity e : added)
            if (scene.IsValid(e)) scene.DestroyEntity(e);
        SyncAll(scene);
        Log::Info("Applied the nested instance to prefab '{}'", AssetManager::GetAbsolutePath(inner).filename().string());
        return true;
    }

    AssetHandle CreateFromEntity(Scene& scene, Entity root, const fs::path& path)
    {
        if (!scene.IsValid(root)) return AssetHandle::None();
        // Instances of other prefabs inside it are nested (the root's own prefab, if it is an instance, is not: the
        // new prefab starts as a copy of it).
        LinkAssignments assigned;
        json tree = BuildPrefabJson(scene, root, AssetHandle::None(), &assigned);
        if (!WriteJson(path, tree)) return AssetHandle::None();
        AssetHandle handle = AssetManager::Import(path);
        if (!handle.IsValid()) return handle;
        LinkSaved(scene, root, handle, assigned);
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
        LinkAssignments assigned;
        if (!WritePrefab(prefab, path, BuildPrefabJson(scene, root, prefab, &assigned))) return false;
        // New objects become linked so later saves keep their ids; prefab instances placed inside are now nested.
        LinkSaved(scene, root, prefab, assigned);
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
        for (const auto& rec : records)
            if (auto it = bySource.find(rec.value("guid", uint64_t(0))); it != bySource.end())
                if (auto* link = reg.TryGet<PrefabLinkComponent>(it->second)) ReadNestedLink(*link, rec);

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
                if (info->Upgrade) info->Upgrade(comp, data);
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
        LinkAssignments assigned;
        if (!WritePrefab(prefab, path, BuildPrefabJson(scene, root, prefab, &assigned))) return false;

        // Everything in the instance now matches the asset: adopt new objects (prefab instances added to it become
        // nested) and clear overrides.
        LinkSaved(scene, root, prefab, assigned);
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
