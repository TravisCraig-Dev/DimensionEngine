#include "Dimension/Scripting/EditModeScripts.h"
#include "Dimension/Scene/Components.h"
#include "Dimension/Scripting/ScriptRegistry.h"
#include "Dimension/Scripting/ScriptableEntity.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <memory>
#include <vector>

namespace Dimension::EditModeScripts
{
    namespace
    {
        struct Running
        {
            Scene* Owner = nullptr;
            uint64_t Object = 0;   // the object's GUID
            size_t Index = 0;      // the script's place on it
            std::string Class;
            nlohmann::json Fields; // the saved values it was last given
            std::shared_ptr<ScriptableEntity> Instance;
            bool Seen = false;
        };
        std::vector<Running> s_Running;
    }

    void Update(Scene& scene, float dt)
    {
        if (scene.IsRunning()) return;
        for (Running& r : s_Running)
            if (r.Owner == &scene) r.Seen = false;
        EntityRefResolver resolver;
        resolver.FromGuid = [&scene](uint64_t g) { return scene.FindEntityByGuid(Guid(g)); };

        // Find the scripts first: what they do in OnCreate and OnUpdate may change the scene.
        struct Pending
        {
            Entity E;
            size_t Index;
        };
        std::vector<Pending> found;
        auto& reg = scene.GetRegistry();
        for (auto [e, sc] : reg.ViewOf<ScriptComponent>())
        {
            if (auto* t = reg.TryGet<TransformComponent>(e); t && !t->ActiveInHierarchy) continue;
            for (size_t i = 0; i < sc.Scripts.size(); ++i)
            {
                const ScriptInstance& script = sc.Scripts[i];
                const ScriptInfo* info = ScriptRegistry::Find(script.ClassName);
                if (script.Enabled && info && info->ExecuteInEditMode) found.push_back({ e, i });
            }
        }
        std::vector<std::shared_ptr<ScriptableEntity>> created, updated;
        for (const Pending& p : found)
        {
            if (!scene.IsValid(p.E) || !reg.Has<ScriptComponent>(p.E)) continue;
            auto& sc = reg.Get<ScriptComponent>(p.E);
            if (p.Index >= sc.Scripts.size()) continue;
            const ScriptInstance& script = sc.Scripts[p.Index];
            const ScriptInfo* info = ScriptRegistry::Find(script.ClassName);
            if (!info) continue;
            uint64_t guid = (uint64_t)scene.GetGuid(p.E);
            auto it = std::find_if(s_Running.begin(), s_Running.end(), [&](const Running& r) {
                return r.Owner == &scene && r.Object == guid && r.Index == p.Index && r.Class == script.ClassName;
            });
            // The same object may come back as another entity (undo restores the scene): start over then.
            if (it != s_Running.end() && it->Instance->Self() != p.E)
            {
                it->Instance->OnDestroy();
                s_Running.erase(it);
                it = s_Running.end();
            }
            if (it == s_Running.end())
            {
                Running r;
                r.Owner = &scene;
                r.Object = guid;
                r.Index = p.Index;
                r.Class = script.ClassName;
                r.Fields = script.Fields;
                r.Instance = scene.CreateDetachedScriptInstance(p.E, script, *info, resolver);
                r.Seen = true;
                created.push_back(r.Instance);
                updated.push_back(r.Instance);
                s_Running.push_back(std::move(r));
                continue;
            }
            it->Seen = true;
            if (it->Fields != script.Fields)
            {
                // Edited in the Inspector: the new values, keeping the rest of the script's state.
                it->Fields = script.Fields;
                void* obj = info->ToObject(it->Instance.get());
                for (auto& f : info->Fields)
                    if (script.Fields.is_object() && script.Fields.contains(f.Name)) FieldFromJson(f, obj, script.Fields[f.Name], &resolver);
                it->Instance->OnFieldsChanged();
            }
            updated.push_back(it->Instance);
        }
        // Scripts that went (removed, disabled, their object deleted or switched off).
        std::vector<std::shared_ptr<ScriptableEntity>> ended;
        std::erase_if(s_Running, [&](Running& r) {
            if (r.Owner != &scene || r.Seen) return false;
            ended.push_back(r.Instance);
            return true;
        });
        for (auto& s : ended)
            if (scene.IsValid(s->Self())) s->OnDestroy();
        for (auto& s : created) s->OnCreate();
        for (auto& s : updated) s->OnUpdate(dt);
    }

    void Clear(Scene* scene, bool quietly)
    {
        std::vector<Running> ending;
        std::erase_if(s_Running, [&](Running& r) {
            if (scene && r.Owner != scene) return false;
            ending.push_back(std::move(r));
            return true;
        });
        if (!quietly)
            for (Running& r : ending)
                if (r.Owner->IsValid(r.Instance->Self())) r.Instance->OnDestroy();
    }

    size_t Count(const Scene* scene)
    {
        return (size_t)std::count_if(s_Running.begin(), s_Running.end(), [&](const Running& r) { return !scene || r.Owner == scene; });
    }
}
