# Dimension Engine

A 2D/3D game engine written in C++23 on **Vulkan 1.3**. Its core is a custom **Entity Component System**, and it ships with a Unity-style **editor** and a standalone **player** runtime.

```
Engine/    DimensionEngine.dll (ECS, renderer, scene, physics, audio, assets, UI, ImGui)
Editor/    DimensionEditor: docking editor (Hierarchy, Inspector, Scene/Game views, Project, Console)
Player/    DimensionPlayer: runs a project's start scene without the editor (used for builds)
Game/      SandboxGame.dll: gameplay scripts and systems, hot-reloaded by the Editor, loaded by the Player
Tests/     DimensionTests: headless tests for the ECS, scenes, serialization, scripts and physics
Projects/  Sandbox sample project (Demo3D and Demo2D scenes)
```

## Features

**Core and ECS**
- Sparse-set ECS with generational entity handles and typed views (`for (auto [e, t, s] : reg.ViewOf<Transform, Sprite>())`).
- Deep registry cloning that preserves entity handles, which powers Play mode.
- Reflection for components and script fields. The inspector, serializer and prefabs are all driven by one field declaration.
- An ECS `System` interface with a registry. Gameplay scripts can be written in C++ (`ScriptableEntity`) or **C#** (`Behaviour`), and both hot-reload in the editor.

**Rendering (Vulkan 1.3)**
- Dynamic rendering, synchronization2 and a bindless texture table. There are no render-pass objects and no per-material descriptor sets.
- volk loads Vulkan directly from the GPU driver, so **no Vulkan SDK is required** to build or run.
- VMA for memory, frames in flight, deferred GPU resource destruction, and a per-frame transient ring buffer.
- HDR linear pipeline with an sRGB/ACES composite and physically based shading (Cook-Torrance GGX) for 1 directional and 32 point lights, with sky reflections.
- **Material assets** (`.mat`): base color, normal, metallic-roughness and emission maps, tiling/offset, Opaque/Cutout/Transparent blend modes, double-sided rendering. Normal mapping needs no mesh tangents. Per-draw material data reaches shaders through buffer device addresses.
- Directional-light shadow maps with PCF, fitted and texel-snapped to the camera, plus a procedural sky with a sun disk and per-camera clear flags.
- Frustum culling for meshes and their shadow casters.
- Batched sprites with sorting order, sprite sheets and flipping, plus world-space **Text (3D)**.
- **Skeletal animation**: glTF skins (joints, weights, inverse bind matrices) and animation clips. Linear-blend skinning runs on the GPU in the mesh shader, with shadows. The **Animator** component handles clip playback, speed, looping and `CrossFade(clip, seconds)`.
- **Animator Controllers** (`.controller`) are state machines like Unity's. They have float, int, bool and trigger parameters, and states with a clip, speed and loop flag. Transitions run from a state or from Any State, with conditions, an optional exit time and a blend duration. Scripts drive them with `SetFloat`, `SetBool`, `SetInteger` and `SetTrigger` in C++ or C#. Edit them in **Window > Animator**, a graph with draggable states, zoom and pan, right-click creation, and live state and parameter display during play.
- Fonts: TTF/OTF assets baked to atlases with stb_truetype (Latin-1), and an embedded default font, so text works with no assets. Meshes include built-in primitives and glTF/GLB/OBJ import.
- GPU entity-ID buffer for click selection, an editor grid, gizmo lines and billboard icons for cameras, lights and audio sources.

**Editor**
- Docking layout that is saved per project, a play-mode tint, a scene-view orientation cube, plus a toolbar with Move, Rotate and Scale tools (ImGuizmo), Local/Global mode, 2D mode and a grid toggle.
- **Multi-select**: Ctrl/Shift-click in the Hierarchy, Ctrl-click in the Scene view. Delete, duplicate, copy/paste and the transform gizmo act on the whole selection, and Inspector edits apply to every selected object with that component.
- **Prefabs**: instances stay linked to their `.prefab` asset. Per-field overrides show in blue in the Inspector, and the prefab header offers **Apply**, **Revert**, **Unpack** and **Select**. Changes to the asset (Apply, or edits on disk) reach every instance without touching their overrides. Added and removed components and children propagate too. The root's position, rotation and name belong to the instance, as in Unity.
- **Hierarchy**: drag-and-drop parenting and reordering, rename, duplicate, delete, create menus, and "Create Prefab".
- **Inspector**: reflected component fields, Add Component search, script class picker with public fields, texture import settings, and a material editor.
- **Scene view**: Unity-style camera, GPU picking, transform gizmos, and dropping assets in to create objects or instantiate prefabs.
- **Game view**: renders the primary camera. **Play/Pause/Step** run on a cloned scene, and edits made while playing are discarded on Stop.
- **Project** browser: folder tree, thumbnails and material swatches, New Material/Scene/Folder, search, rename, move by drag, delete, and import by dropping files from Windows Explorer.
- **Console** with level filters, collapsing and clear-on-play. Also **Scene Settings**, **Stats**, and **Project Settings**.
- Copy/paste of GameObjects through the OS clipboard, snapshot-based **Undo/Redo**, scene save/load in JSON, unsaved-change prompts, and **File > Build Game**.

**Particles**
- The Particle System component covers duration and looping, randomized start lifetime, speed, size, rotation, spin and color, gravity and drag, rate-over-time and burst emission, and cone, sphere, box or point shapes in local or world space.
- Color and size change over each particle's lifetime. Particles render with alpha or additive blending, using a texture or the built-in soft particle.
- Particles are camera-facing quads sorted with sprites. They preview live in the editor, the emitter shape is drawn when selected, and scripts can call `Play`, `Stop`, `Emit` and `Clear`.

**UI (screen space)**
- Canvas with constant-pixel or scale-with-screen-size modes, and a UI Rect with Unity-style anchors, pivot, position and size.
- UI Image, UI Text (word wrap, horizontal and vertical alignment) and UI Button (hover and pressed tints). A click calls `OnClick()` on the button's scripts.
- UI renders in the Game view and the Player after tonemapping, so its colors are exact. GameObject > UI creates elements and adds a Canvas automatically.
- Coordinates use a top-left origin with y pointing down, unlike Unity's bottom-left.

**Gameplay systems**
- 2D physics with **Box2D v3**: rigidbodies, box and circle colliders, triggers, collision and trigger callbacks, and raycasts.
- 3D physics with **Jolt**: rigidbodies, box, sphere and capsule colliders, collision callbacks and raycasts.
- Audio with **miniaudio**: Audio Source with 2D or 3D spatial sound, the listener follows the primary camera.
- Assets: a GUID database stored in `.meta` sidecar files, hot reload when the editor regains focus, prefabs, and scenes.

## C# scripting

Gameplay can also be written in C#. The engine hosts the .NET runtime, as Unity hosts Mono, and C# classes appear in the Inspector's Script component next to the C++ ones.

```csharp
using System.Numerics;
using Dimension;

namespace Game
{
    public class Spinner : Behaviour
    {
        public Vector3 DegreesPerSecond = new(0, 90, 0);   // shown in the Inspector

        public override void OnUpdate(float dt) => Transform.Rotate(DegreesPerSecond * dt);
        public override void OnCollisionEnter(Entity other) => Debug.Log($"{Name} hit {other.Name}");
    }
}
```

- **Writing scripts.** Put `.cs` files in `<Project>/Scripts/`. The editor generates `GameScripts.csproj` there, so Rider or Visual Studio can open it with full IntelliSense against `Dimension.Core.dll`.
- **Reloading.** The editor watches the folder. Saving a file recompiles the scripts with `dotnet build` in about a second, shows any errors in the Console, and **hot-reloads** the assembly. The assembly loads into a collectible `AssemblyLoadContext` from memory, so the file is never locked.
- **API.** The `Behaviour` callbacks mirror the C++ ones. Components available from C# are `Transform`, `Rigidbody` and `Rigidbody2D`, `MeshRenderer` and `SpriteRenderer` color, `UIText`, `Animator`, `ParticleSystem` and `AudioSource`. Static helpers are `Input`, `Debug`, `Time`, `Physics.Raycast`, `Entity.Find` and `Entity.Instantiate`.
- **Inspector fields.** Public fields of type `float`, `int`, `bool`, `string`, `Vector2/3/4`, `Color` and `Entity` appear in the Inspector, and the values save with the scene and prefabs.
- **Requirements.** C# scripting needs the .NET 10 SDK at build time and the .NET 10 runtime at run time. Without them the engine builds and runs with C++ scripting only. Build Game copies `nethost.dll`, the `managed/` folder and the compiled scripts.

## Building

You need Windows, Visual Studio 2022 or newer with the C++ workload, and a GPU driver with Vulkan 1.3. The CMake and Ninja that ship with Visual Studio are enough. All dependencies are fetched at configure time.

**JetBrains Rider or Visual Studio:** open the folder. The IDE picks up `CMakePresets.json`; choose the `debug` or `release` preset.

**Command line.** `scripts\dev.bat` loads the MSVC environment and assumes VS 18 Community, so edit the path if yours differs.
```bat
scripts\dev.bat cmake --preset debug
scripts\dev.bat cmake --build --preset debug
scripts\dev.bat ctest --preset debug
```
Binaries land in `out/build/<preset>/bin/`.

**Validation layers (optional).** Debug builds enable `VK_LAYER_KHRONOS_validation` when it is installed, either from the Vulkan SDK or by pointing `VK_ADD_LAYER_PATH` at a self-built layer.

## Running

```bat
DimensionEditor.exe [projectDir] [--play] [--windowed]
DimensionPlayer.exe [projectDir] [--scene file.scene]
```
With no arguments, both open `Projects/Sandbox`. A built game is `<Name>.exe` next to `DimensionEngine.dll`, the game module DLL, and a `Project/` folder. You can also pass `--frames N --screenshot out.png` to either program for automated checks.

### Editor controls

| Action | Input |
|---|---|
| Look / fly | Hold RMB and move the mouse. Add WASD to move, Q/E to go down/up, Shift to go faster, and the wheel to change speed |
| Orbit, pan, zoom | Alt+LMB, MMB, mouse wheel |
| Frame selection | F, or double-click in the Hierarchy |
| Tools | Q view, W move, E rotate, R scale. Hold Ctrl to snap |
| Select | Click in the Scene view; click empty space to deselect. Hold Ctrl to add or remove, Shift for a range in the Hierarchy |
| Play, Pause | Ctrl+P, Ctrl+Shift+P |
| Undo, Redo, Duplicate, Delete | Ctrl+Z, Ctrl+Y, Ctrl+D, Del |
| Copy, Paste | Ctrl+C, Ctrl+V |
| Snap view to an axis | Click a face of the orientation cube |
| Save, Open, New scene | Ctrl+S, Ctrl+O, Ctrl+N |

In Play mode, click the Game view to give the game keyboard input.

## Writing gameplay code

Scripts live in `Game/Source`, which builds `SandboxGame.dll`. The editor loads that DLL and **hot-reloads** it:

1. Edit a script in your IDE.
2. Press **Ctrl+R** in the editor (Scripts > Build & Reload), or build the `SandboxGame` target from your IDE.
3. The editor notices the new DLL within a second and reloads it. It saves the open scene to JSON, unloads the old module, loads the new one and restores the scene and selection, similar to Unity's domain reload.

Compiler errors appear in the Console, and the previous version stays loaded. A rebuild finished during Play mode is applied after you press Stop. Changes to the engine itself still need an editor restart, because the running editor holds `DimensionEngine.dll` open. The project file chooses the module by name (`"gameModule": "SandboxGame"`), and the module exports its registration function with `DIM_GAME_MODULE_ENTRY`.

Scripts are registered once and then appear in the Inspector's Script component. An entity can carry **any number of scripts**, in C++ and C# mixed. They run in the order listed, each can be enabled or disabled, and they find each other with `GetScript<T>()`. Scripts can also be added straight from Add Component.

```cpp
class Rotator : public ScriptableEntity
{
public:
    glm::vec3 DegreesPerSecond{ 0, 45, 0 };   // public field shown in the Inspector
    void OnUpdate(float dt) override { Transform().Rotation += glm::radians(DegreesPerSecond) * dt; }
};

ScriptRegistry::Register<Rotator>("Rotator").Field("Degrees Per Second", &Rotator::DegreesPerSecond);
```

The available callbacks are `OnCreate`, `OnUpdate`, `OnFixedUpdate` (60 Hz), `OnDestroy`, `OnCollisionEnter/Exit`, `OnTriggerEnter/Exit` and `OnClick` (UI Button). `GetScene().IsPointerOverUI()` tells gameplay code when the mouse is over UI. Helpers include `Get<T>()`, `Find(name)`, `Instantiate(prefab, pos)`, `Destroy(e)`, `Raycast(origin, dir)`, `Raycast2D(origin, dir)`, `Input::IsKeyDown`, `Physics2DWorld::*`, `Physics3DWorld::*` and `AudioWorld::Play`. Entity and asset fields such as `Entity Target;` or `.As(FieldType::Prefab)` get drag-and-drop slots.

**ECS systems** run over component sets every frame in Play mode:

```cpp
class PulseSystem : public System {
    void OnUpdate(Scene& scene, float dt) override {
        for (auto [e, tag, mesh] : scene.GetRegistry().ViewOf<TagComponent, MeshRendererComponent>()) { ... }
    }
};
SystemRegistry::Register<PulseSystem>("Pulse");
```

**New components** need a struct and a single registration. The Inspector, save/load, prefabs and Add Component menu then work automatically:

```cpp
ComponentRegistry::Register<HealthComponent>("Health", "Gameplay")
    .Field("Max", &HealthComponent::Max).Range(1, 1000)
    .Field("Tint", &HealthComponent::Tint).As(FieldType::Color4);
```

## Architecture notes

- **Modules.** The engine is a DLL that exports all of its symbols, so the editor, the player and the game module share one copy of engine state. That includes the registries, input, assets and the ImGui context. Component type ids come from a name table inside the engine, so all modules agree on them. Game modules are copied before loading, which keeps the original file free for the linker.
- **Frame flow.** Each Application layer runs `OnUpdate`, then `OnImGui`, then `OnRender` (off-screen scene targets), then `OnCompose` (swapchain blit). ImGui is drawn last, followed by present.
- **One pipeline layout.** Set 0 is the bindless `sampler2D[]`, set 1 is a dynamic UBO in the transient ring, and there are 128 bytes of push constants. Shaders are GLSL compiled to SPIR-V at build time by a glslang that is built from source, then embedded as headers.
- **Scene targets** hold HDR color, an entity-ID buffer (R32_UINT), depth, and the tonemapped RGBA8 output. The editor shows the output as an ImGui image; the player blits it to the swapchain.
- **Play mode** runs `Scene::Clone()` and then `OnRuntimeStart()`, which creates Box2D/Jolt worlds, audio sources and script instances. Stop throws the clone away.
- **Assets** are referenced by GUID. `.meta` files hold the GUID and import settings, so moving or renaming in the Project panel keeps references intact.

## Roadmap toward Unity parity

Done in this milestone: everything listed under Features. These are the largest remaining areas, in a suggested order:

1. **Rendering:** cascaded and point-light shadows, image-based lighting from cubemaps, materials imported from glTF, MSAA/FXAA, bloom, transparent 3D materials, frustum culling and GPU instancing.
2. **Editor:** multi-object component display, copy/paste of components, nested prefabs and prefab variants, a dedicated prefab editing mode, an asset import pipeline with a cache, and editor preferences.
3. **Animation:** blend trees, layers and masks, root motion, IK, animation events and timeline curves.
4. **UI system:** sliders, toggles, input fields, scroll views, layout groups, 9-slice images, MSDF text and rich text, and editing UI rects directly in the Scene view.
5. **Scripting:** more of the engine API in C# (custom components, coroutines, events), keeping script state across a reload while playing, and debugger attach.
6. **More physics:** mesh colliders, joints, overlap queries, and layers/masks.
7. **More audio:** mixer groups and streaming music.
8. **Effects:** GPU particles, particle collision, sub-emitters, trails and curve editors.
9. **Platform:** Linux support (file dialogs and paths), a packaged release build without a console, and an asset bundle format.
