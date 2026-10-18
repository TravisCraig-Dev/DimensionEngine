#pragma once
#include "Dimension/Core/Base.h"
#include <deque>

namespace Dimension
{
    class Scene;

    // ECS system: stateless-ish logic that runs over component sets each frame
    // while the scene is playing. Register custom systems from game code:
    //   SystemRegistry::Register<SpinSystem>("Spin");
    class System
    {
    public:
        virtual ~System() = default;
        virtual void OnStart(Scene& scene) {}
        virtual void OnUpdate(Scene& scene, float dt) {}
        virtual void OnFixedUpdate(Scene& scene, float dt) {}
        virtual void OnStop(Scene& scene) {}
    };

    class SystemRegistry
    {
    public:
        struct Entry
        {
            std::string Name;
            std::function<Scope<System>()> Create;
        };

        template <class T> static void Register(const std::string& name)
        {
            Entries().push_back({ name, [] { return Scope<System>(new T()); } });
        }
        static const std::deque<Entry>& All() { return Entries(); }
        static void Clear() { Entries().clear(); }

    private:
        static std::deque<Entry>& Entries(); // storage lives in the engine DLL
    };
}
