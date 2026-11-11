#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <vector>

namespace Dimension
{
    // Hosts the .NET runtime (hostfxr) and exposes C# Behaviour classes as engine scripts.
    //
    // Managed code lives in Dimension.Core.dll (engine API) and a user scripts assembly that is
    // loaded into a collectible AssemblyLoadContext, so it can be rebuilt and reloaded while the
    // editor runs. C# classes register into ScriptRegistry under their full type name (module
    // "csharp"), so the Inspector, serialization, prefabs and play mode treat them like C++ scripts.
    class Scene;
    class ScriptableEntity;

    class CSharpRuntime
    {
    public:
        // Editor extensions in the scripts (EditorApi.cs): menu items ([MenuItem]) and windows (EditorWindow), each
        // under a menu path, and custom Inspectors ([CustomInspector]: Inspects names the script class or built-in
        // component), registered when the scripts load.
        struct EditorItem
        {
            int Id = -1;
            bool Window = false;
            std::string MenuPath, Title, Inspects;
            bool SceneGUI = false;     // an Inspector overriding OnSceneGUI
            bool InspectorGUI = false; // ... and OnInspectorGUI (else the script's fields show as usual)
        };
        static const std::vector<EditorItem>& GetEditorItems();
        // Runs a menu item, or draws a window's contents (inside the caller's window), with `scene` as the scene.
        static void CallEditorItem(int id, bool window, Scene& scene);
        // The custom Inspector for a script class or component name (null: none).
        static const EditorItem* FindInspector(const std::string& inspected);
        // Draws a custom Inspector for a C# script instance (live, or a stand-in holding the saved values) and takes
        // the values it leaves into the instance's fields; true when they changed.
        static bool InspectScript(int id, ScriptableEntity& script, Scene& scene, Entity e);
        // Draws a custom Inspector for a built-in component of `e` (it changes the scene through the C# API).
        static void InspectComponent(int id, Scene& scene, Entity e);
        // C# property drawers ([CustomPropertyDrawer]): whether one draws a script's field (by index), and drawing it
        // (true when the script's values changed, as InspectScript).
        static bool HasFieldDrawer(ScriptableEntity& script, size_t field);
        static bool DrawScriptField(ScriptableEntity& script, size_t field, Scene& scene, Entity e);
        // The Inspector's Debug mode: a C# script's fields that are not saved (private state), with their values as
        // text: the live script's while playing, else a new one's holding the saved values.
        struct DebugField
        {
            std::string Name, Type, Value;
        };
        static std::vector<DebugField> GetDebugFields(ScriptableEntity& script, Scene& scene, Entity e);
        // Scene view gizmos of the scene's C# scripts (world-space lines): every enabled script overriding
        // OnDrawGizmos draws (the live script while playing, else a stand-in holding its saved values), and those of
        // `selected` objects draw OnDrawGizmosSelected too.
        struct GizmoLine
        {
            glm::vec3 A, B;
            glm::vec4 Color;
        };
        static std::vector<GizmoLine> CollectGizmos(Scene& scene, const std::vector<Entity>& selected);
        // A C# script's [ContextMenu] items, and running one (as InspectScript: true when its values changed).
        static const std::vector<std::string>& GetContextMenu(const std::string& className);
        static bool RunContextMenu(ScriptableEntity& script, size_t item, Scene& scene, Entity e);
        // A custom Inspector's OnSceneGUI for a C# script (its handles go through EditorHooks::Handle); true when the
        // script's values changed, as InspectScript.
        static bool SceneGUIScript(int id, ScriptableEntity& script, Scene& scene, Entity e);
        // What the extensions reach in the editor: controls (op, label, values, count; see EditorGUI in EditorApi.cs:
        // returns 1 when clicked/changed), a text field, the selection (0 get, 1 set, 2 create an entity named `text`,
        // 3 count (in Index), 4 the n-th (n in the entity's Index)), an undo step, and pickers for an entity and for
        // an asset (kind 1 Prefab, 2 Texture, 3 Material, 4 Mesh, 5 Audio; the GUID in and out).
        struct EditorHooks
        {
            std::function<int(int, const char*, float*, int)> Gui;
            std::function<std::string(const char*, const std::string&)> Text;
            std::function<Entity(int, Entity, const char*)> Selection;
            std::function<void(const char*)> Undo;
            std::function<Entity(const char*, Entity)> EntityField;
            std::function<uint64_t(int, const char*, uint64_t)> AssetField;
            // A Scene view handle (kind as Handles in EditorApi.cs: 0 position, 1 free move, 2 radius, 3 label, 4 line;
            // values in and out, their count, text): returns 1 while it is hovered or dragged.
            std::function<int(int, float*, int, const char*)> Handle;
        };
        static void SetEditorHooks(const EditorHooks& hooks);
        // The scripts' unit tests ([Test] methods; Window > Test Runner): listed, or run (those whose full name starts
        // with `filter`). JSON array of { name, passed, ms, message }.
        static std::string RunTests(bool run, const std::string& filter = {});

        // `managedDir` contains Dimension.Core.dll and its runtimeconfig.json.
        // Starts .NET: a runtime shipped beside the game (`<managedDir>/../dotnet`, see BundleRuntime) when there is one,
        // else the one installed on the machine.
        static bool Init(const fs::path& managedDir);
        // Copies the installed .NET runtime this engine runs on (hostfxr and the Microsoft.NETCore.App framework, in the
        // dotnet folder layout) into `dotnetDir`, so a built game runs C# without .NET installed. False when none.
        static bool BundleRuntime(const fs::path& dotnetDir, uintmax_t* bytes = nullptr);
        // The installed Microsoft.NETCore.App framework folder the engine runs on (its newest .NET 10); empty when none.
        static fs::path InstalledFramework();
        // Where the runtime in use came from: "bundled" or "installed" (empty before Init).
        static const char* RuntimeSource();
        static bool IsAvailable();

        // Loads (or reloads) a user scripts assembly. No script instances may be alive. Not reloadable (a built
        // game): loaded once from the file and kept, so its ReadyToRun code is used (hot reload needs a collectible
        // in-memory load, which compiles everything again).
        static bool LoadScripts(const fs::path& assembly, bool reloadable = true);
        static void UnloadScripts();
        static const fs::path& GetScriptsPath();
        // True when the assembly on disk is newer than the loaded one.
        static bool ScriptsChangedOnDisk();
        static size_t GetScriptCount();

        // Debugging C# scripts: any .NET debugger can attach to the running editor or game (Rider: Run > Attach to
        // Process; Visual Studio: Attach to Process, Managed (.NET Core)), and breakpoints in the script sources work.
        static bool IsDebuggerAttached();
        // Offers to attach one through the system's just-in-time debugger prompt (Visual Studio).
        static bool LaunchDebugger();
        // Blocks until a debugger attaches or `seconds` pass (the --wait-for-debugger flag); true when attached.
        static bool WaitForDebugger(float seconds);
    };
}
