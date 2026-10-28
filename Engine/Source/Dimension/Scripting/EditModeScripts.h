#pragma once
#include <cstddef>

namespace Dimension
{
    class Scene;

    // [ExecuteInEditMode] scripts (C#, or C++ registered with ScriptOptions::ExecuteInEditMode) running in the editor
    // outside play mode: a live instance per such script, kept apart from the scene's own (play mode never sees them).
    // An instance is created (OnCreate) when its script appears, given the saved values again when they change
    // (OnFieldsChanged), updated (OnUpdate) on every Update, and ended (OnDestroy) when its script, object or class goes.
    namespace EditModeScripts
    {
        void Update(Scene& scene, float dt);
        // Ends them (OnDestroy unless `quietly`): every scene's, or only `scene`'s. Before entering play mode and before
        // script classes are unloaded.
        void Clear(Scene* scene = nullptr, bool quietly = false);
        size_t Count(const Scene* scene = nullptr);
    }
}
