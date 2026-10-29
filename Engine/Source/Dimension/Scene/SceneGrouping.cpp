#include "Dimension/Scene/SceneGrouping.h"
#include "Dimension/Scene/ComponentRegistry.h"
#include "Dimension/Scene/Components.h"
#include <algorithm>
#include <cctype>

namespace Dimension::SceneGrouping
{
    namespace
    {
        size_t IndexAmongSiblings(Scene& scene, Entity e)
        {
            Entity parent = scene.GetParent(e);
            const auto& list = parent.IsNull() ? scene.GetRootEntities() : scene.GetChildren(parent);
            return (size_t)(std::find(list.begin(), list.end(), e) - list.begin());
        }

        // Hierarchy order (depth first), to keep the grouped objects in the order they were in.
        void Walk(Scene& scene, Entity e, std::vector<Entity>& out)
        {
            out.push_back(e);
            for (Entity c : scene.GetChildren(e)) Walk(scene, c, out);
        }
    }

    bool NameBefore(const std::string& a, const std::string& b)
    {
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size())
        {
            bool da = std::isdigit((unsigned char)a[i]) != 0, db = std::isdigit((unsigned char)b[j]) != 0;
            if (da && db)
            {
                // Whole numbers compared by value (leading zeros ignored, then fewer digits first).
                size_t ei = i, ej = j;
                while (ei < a.size() && std::isdigit((unsigned char)a[ei])) ++ei;
                while (ej < b.size() && std::isdigit((unsigned char)b[ej])) ++ej;
                std::string na = a.substr(i, ei - i), nb = b.substr(j, ej - j);
                na.erase(0, std::min(na.find_first_not_of('0'), na.size()));
                nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size()));
                if (na.size() != nb.size()) return na.size() < nb.size();
                if (na != nb) return na < nb;
                i = ei;
                j = ej;
                continue;
            }
            char ca = (char)std::tolower((unsigned char)a[i]), cb = (char)std::tolower((unsigned char)b[j]);
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
        if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
        return a < b; // same apart from case: a stable tie-break
    }

    Entity Group(Scene& scene, const std::vector<Entity>& objects, const std::string& name)
    {
        // The given objects without those under another given one, in hierarchy order.
        std::vector<Entity> order;
        for (Entity r : scene.GetRootEntities()) Walk(scene, r, order);
        std::vector<Entity> roots;
        for (Entity e : order)
        {
            if (std::find(objects.begin(), objects.end(), e) == objects.end()) continue;
            bool nested = false;
            for (Entity o : objects)
                if (o != e && scene.IsValid(o) && scene.IsDescendantOf(e, o)) nested = true;
            if (!nested) roots.push_back(e);
        }
        if (roots.empty()) return Entity::Null();

        scene.UpdateTransforms();
        auto& reg = scene.GetRegistry();
        glm::vec3 center(0.0f);
        for (Entity e : roots) center += reg.Get<TransformComponent>(e).GetWorldPosition();
        center /= (float)roots.size();
        Entity parent = scene.GetParent(roots.front());
        for (Entity e : roots)
            if (scene.GetParent(e) != parent) parent = Entity::Null();
        // Where the first one is; with different parents, where its top-level ancestor is among the root objects.
        Entity anchor = roots.front();
        if (scene.GetParent(anchor) != parent)
            while (!scene.GetParent(anchor).IsNull()) anchor = scene.GetParent(anchor);
        size_t index = IndexAmongSiblings(scene, anchor);

        Entity group = scene.CreateEntity(name, Entity::Null());
        reg.Get<TransformComponent>(group).Position = center;
        scene.UpdateTransforms();
        scene.SetParent(group, parent, true);
        scene.SetSiblingIndex(group, index);
        for (Entity e : roots) scene.SetParent(e, group, true);
        scene.UpdateTransforms();
        return group;
    }

    bool IsEmptyObject(Scene& scene, Entity e)
    {
        for (const auto& info : ComponentRegistry::All())
            if (!info.Hidden && info.Name != "Transform" && info.Has(scene, e)) return false;
        return true;
    }

    std::vector<Entity> Ungroup(Scene& scene, Entity group)
    {
        if (!scene.IsValid(group)) return {};
        std::vector<Entity> children = scene.GetChildren(group); // a copy: reparenting changes the list
        if (children.empty()) return {};
        Entity parent = scene.GetParent(group);
        size_t index = IndexAmongSiblings(scene, group);
        scene.UpdateTransforms();
        for (size_t i = 0; i < children.size(); ++i)
        {
            scene.SetParent(children[i], parent, true);
            scene.SetSiblingIndex(children[i], index + 1 + i); // after the group, in their order
        }
        if (IsEmptyObject(scene, group)) scene.DestroyEntity(group);
        scene.UpdateTransforms();
        return children;
    }

    void SortChildren(Scene& scene, Entity parent, bool recursive)
    {
        auto& reg = scene.GetRegistry();
        std::vector<Entity> list = parent.IsNull() ? scene.GetRootEntities() : scene.GetChildren(parent);
        std::stable_sort(list.begin(), list.end(), [&](Entity a, Entity b) {
            return NameBefore(reg.Get<TagComponent>(a).Name, reg.Get<TagComponent>(b).Name);
        });
        for (size_t i = 0; i < list.size(); ++i) scene.SetSiblingIndex(list[i], i);
        if (recursive)
            for (Entity c : list) SortChildren(scene, c, true);
    }
}
