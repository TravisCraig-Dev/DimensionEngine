#pragma once
#include "Dimension/Reflection/TypeInfo.h"
#include "Dimension/Scene/Scene.h"
#include <deque>

namespace Dimension
{
    // Runtime description of a component type: drives "Add Component", the
    // inspector and the scene serializer without per-type code.
    struct ComponentInfo
    {
        std::string Name;     // serialized key and display name
        std::string Category; // "Rendering", "Physics 2D", ...
        ComponentTypeId TypeId = 0;
        std::vector<FieldInfo> Fields;
        bool Hidden = false;    // not shown in the inspector's component list
        bool Removable = true;

        std::function<bool(Scene&, Entity)> Has;
        std::function<void(Scene&, Entity)> Add;
        std::function<void(Scene&, Entity)> Remove;
        std::function<void*(Scene&, Entity)> Get;
    };

    class ComponentRegistry
    {
    public:
        template <class T>
        static FieldListBuilder<T> Register(const std::string& name, const std::string& category, bool hidden = false, bool removable = true)
        {
            ComponentInfo& info = Infos().emplace_back();
            info.Name = name;
            info.Category = category;
            info.TypeId = GetComponentTypeId<T>();
            info.Hidden = hidden;
            info.Removable = removable;
            info.Has = [](Scene& s, Entity e) { return s.GetRegistry().Has<T>(e); };
            info.Add = [](Scene& s, Entity e) { if (!s.GetRegistry().Has<T>(e)) s.GetRegistry().Emplace<T>(e); };
            info.Remove = [](Scene& s, Entity e) { s.GetRegistry().Remove<T>(e); };
            info.Get = [](Scene& s, Entity e) -> void* { return s.GetRegistry().TryGet<T>(e); };
            return FieldListBuilder<T>(info.Fields);
        }

        static const std::deque<ComponentInfo>& All() { return Infos(); }
        static const ComponentInfo* Find(const std::string& name);
        template <class T> static const ComponentInfo* Find()
        {
            for (auto& i : Infos())
                if (i.TypeId == GetComponentTypeId<T>()) return &i;
            return nullptr;
        }

        // Registers all engine components. Called once by the Application.
        static void RegisterBuiltins();
        // Removes components registered after the built-ins (by a game module that is being unloaded).
        static void RemoveGameComponents();

    private:
        static std::deque<ComponentInfo>& Infos(); // storage lives in the engine DLL
        static size_t& BuiltinCount();
    };
}
