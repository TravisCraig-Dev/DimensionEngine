# Dimension Engine

A 2D/3D game engine written in C++23 on **Vulkan 1.3**. Its core is a custom **Entity Component System**, and it ships with a Unity-style **editor** and a standalone **player** runtime.

```
Engine/    DimensionEngine.dll (ECS, renderer, scene, physics, audio, assets, UI, ImGui)
Editor/    DimensionEditor: docking editor (Hierarchy, Inspector, Scene/Game views, Project, Console)
Player/    DimensionPlayer: runs a project's start scene without the editor (used for builds)
Game/      SandboxGame.dll: gameplay scripts and systems, hot-reloaded by the Editor, loaded by the Player
Tests/     DimensionTests: headless tests for the ECS, scenes, serialization, scripts, physics, audio and more (pass a name filter as the first argument)
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
- HDR linear pipeline with an sRGB/ACES composite and physically based shading (Cook-Torrance GGX) for 1 directional light and up to 32 point and spot lights, with sky reflections.
- Post processing in Scene Settings: physically based bloom (thresholded 13-tap downsample and tent upsample over a mip chain), exposure, ACES tonemapping, vignette and FXAA. Bloom and FXAA also show in the Scene view.
- **Reflection probes** (GameObject > Light > Reflection Probe): capture the surroundings at a point (six HDR renders from its position, converted to an equirectangular image with a blurred mip chain for rough surfaces). Meshes whose center lies inside a probe's box reflect it instead of the sky; with Box Projection, reflections line up with the box's walls. Probes re-capture when first shown, when moved, resized or given another resolution, when play starts, on **Bake Reflection Probes** (Inspector), or every frame with Realtime. Up to 8 per view; the selected probe draws its box. Demo3D has one covering the arena, so the chrome sphere, gold torus and metal tentacles reflect the scene.
- **Depth of field** (Scene Settings > Post Processing): objects nearer or farther than Focus Distance blur, reaching the full Bokeh Size (a fraction of the image height) Focus Range away. A half-resolution prefilter computes each pixel's circle of confusion from the depth buffer. A 32-sample disc gather then blurs it, limiting background samples so sharp foreground edges do not bleed, while blurred foreground still spreads over what is behind it. Composite blends the result in before tonemapping. It applies in the Game view and Player (the Scene view stays sharp); `Scenes/Instancing.scene` uses it for a tilt-shift look.
- **GPU instancing**: opaque, non-skinned meshes that share a mesh, sub-mesh, material and pipeline are drawn with one instanced call per pass (main, depth prepass and every shadow map), with per-instance transforms and entity ids (for picking) in a GPU buffer. Materials are uploaded once per distinct content, so objects using the same simple Mesh Renderer settings batch too. In `Scenes/Instancing.scene` (1,600 cubes) this takes the frame from 4,903 draw calls to 37.
- **One render list per frame**: Mesh Renderers are resolved once per frame (mesh, world-space bounds, uploaded materials and skinning matrices, and a batch id for each part). Each pass (shadow cascades, point and spot shadow faces, depth prepass, main) then only culls against precomputed world bounds and buckets visible parts by batch with a counting sort, writing all instance data into one buffer. On a 10,000-cube scene, CPU recording fell from about 9 ms to 2.6 ms per frame (1.7 ms of it building the list), and the frame from 10.3 ms to the display's pacing limit (3.5 ms). Window > Stats shows instanced meshes and has a toggle to compare; the player logs CPU update, recording and list times on exit.
- **Screen-space ambient occlusion** (Scene Settings > Post Processing > Ambient Occlusion): a depth prepass of opaque geometry, 16-sample hemisphere SSAO at half resolution with normals reconstructed from depth, and a depth-aware blur. The main pass reuses the prepass depth, and the occlusion darkens only ambient light and sky reflections, not direct light. Radius, Intensity and Bias are adjustable, and Show Occlusion displays the AO buffer. Perspective cameras only; Demo3D has it on.
- **Material assets** (`.mat`): base color, normal, metallic-roughness and emission maps, tiling/offset, Opaque/Cutout/Transparent blend modes, double-sided rendering. Normal mapping needs no mesh tangents. Per-draw material data reaches shaders through buffer device addresses.
- Cascaded shadow maps for the directional light: up to 4 cascades in a 2x2 atlas, split over the shadow distance with practical (log/uniform) splits, each fitted and texel-snapped to its slice of the camera frustum. Cascades blend at their borders, shadows fade out at the shadow distance, and Debug Cascades tints each cascade. 3x3 PCF and normal-offset bias.
- Spot lights (GameObject > Light > Spot Light): range, outer and inner cone angles with a smooth falloff, a cone gizmo, and optional shadows from a single perspective shadow map.
- Point- and spot-light shadows: up to 4 point or spot lights with Cast Shadows share a shadow atlas (512 px per tile): point lights render 6 cube faces, spot lights one tile. The shader picks the face from the direction to the light and compares linear distances (bias in world units) with 3x3 PCF.
- Sky: a procedural gradient with a sun disk, or an equirectangular **HDR sky texture** (`.hdr`, loaded as RGBA16F with mips) with intensity and rotation (Scene Settings > Sky).
- **Image-based lighting** from the HDR sky: reflections blur with roughness (mip chain), diffuse ambient comes from 9-coefficient spherical harmonics computed when the image loads, scaled by Ambient Intensity and Reflection Intensity. Demo3D uses a generated clear-sky HDR whose sun matches the directional light. Per-camera clear flags.
- Frustum culling for meshes and their shadow casters.
- Batched sprites with sorting order, sprite sheets and flipping, plus world-space **Text (3D)**.
- **Tilemaps** (GameObject > 2D Object > Tilemap): a sprite-sheet tileset painted into a sparse grid, drawn through the sprite batcher, with an optional static Box2D collider built from merged row boxes. **Window > Tile Palette** picks a tile; with a tilemap selected, left-drag in the Scene view paints and Shift-drag (or the Erase tool) erases, one undo step per stroke. Demo2D's level is one tilemap (generated `Sprites/Tiles.png`).
- **Skeletal animation**: glTF skins (joints, weights, inverse bind matrices) and animation clips. Linear-blend skinning runs on the GPU in the mesh shader, with shadows. The **Animator** component handles clip playback, speed, looping and `CrossFade(clip, seconds)`.
- **Animator Controllers** (`.controller`) are state machines like Unity's. They have float, int, bool and trigger parameters, and states with a clip, speed and loop flag. Transitions run from a state or from Any State, with conditions, an optional exit time and a blend duration. Scripts drive them with `SetFloat`, `SetBool`, `SetInteger` and `SetTrigger` in C++ or C#. Edit them in **Window > Animator**, a graph with draggable states, zoom and pan, right-click creation, and live state and parameter display during play.
- **Blend trees**: a state's Motion can be a 1D blend tree, which blends child clips by a float parameter and their thresholds. The two nearest children are mixed, children stay in sync by normalized time, and the loop length follows the weights. During play the Animator panel shows each child's weight. Crossfades work into and out of blend trees.
- **Animator layers and masks**: a controller can have extra layers on top of the base layer. Each has its own states and transitions (sharing the controller's parameters and triggers), a weight, Override or Additive blending, and a mask of joints that covers each listed joint and its children (empty = whole body). Additive layers add their motion's change from its first frame. Edit them in the Animator window's Layers section (pick mask joints from the selected model's skeleton; while playing, a Live Weight slider). Scripts call `SetLayerWeight(layer, weight)`. Demo3D's 'Tentacle (Layers)' sways as a whole while its tip waves (`Animation/TentacleLayered.controller`).
- **Animation events**: states list events at normalized times. When playback passes one, `OnAnimationEvent(name)` is called on the entity's C++ and C# scripts (looping states fire every loop). Demo3D's "Tentacle (Blend Tree)" sweeps Sway, Wave and Curl with a script-driven parameter and squashes on a "Pulse" event.
- Fonts: TTF/OTF assets baked to atlases with stb_truetype (Latin-1), and an embedded default font, so text works with no assets. Meshes include built-in primitives and glTF/GLB/OBJ import.
- **glTF materials** are imported with the model: base color, metallic/roughness, normal and emissive maps and factors (including `KHR_materials_emissive_strength` and the base-color `KHR_texture_transform`), alpha mode (opaque, mask, blend) and double-sided. Each primitive becomes a sub-mesh drawn with its own material. Until extracted, the materials and their embedded or external textures are read-only sub-assets of the model; **Extract Materials** in the model's Inspector writes them out as `.mat` assets plus textures (normal and metallic-roughness maps imported as linear). A Mesh Renderer's Material overrides every part. Alpha-cutout materials also cut their shadows. The sample `Models/Showcase.glb` shows each material type.
- GPU entity-ID buffer for click selection, an editor grid, gizmo lines and billboard icons for cameras, lights and audio sources.

**Editor**
- Docking layout that is saved per project, a play-mode tint, a scene-view orientation cube, plus a toolbar with Move, Rotate and Scale tools (ImGuizmo), Local/Global mode, 2D mode and a grid toggle.
- **Multi-select**: Ctrl/Shift-click in the Hierarchy, Ctrl-click in the Scene view. Delete, duplicate, copy/paste and the transform gizmo act on the whole selection, and the Inspector shows only the components all selected objects share; edits, Reset and Remove apply to all of them. Values that differ across the selection show as `--`: per axis for vectors, a dash in checkboxes, and `--` for names, enums, assets, entity references and colors (next to the active object's swatch). Editing one axis of a vector changes only that axis on every object, so dragging Position X keeps each object's own Y and Z. The header (active, name, tag, layer) multi-edits the same way.
- **Component clipboard**: right-click a component header (or its "..." button) for Copy Component, Paste Component Values and Paste Component As New. Values go through the OS clipboard as JSON, so they paste across scenes and editor instances.
- **Edit > Preferences** (per user, `%APPDATA%/Dimension/EditorPreferences.json`): UI scale, scene camera speed and field of view, grid, clear console on play, save scene before play, and an autosave interval that backs the open scene up to `.dimension/Autosave` without touching the scene file.
- **Prefabs**: instances stay linked to their `.prefab` asset. Per-field overrides show in blue in the Inspector, and the prefab header offers **Apply**, **Revert**, **Unpack** and **Select**. Changes to the asset (Apply, or edits on disk) reach every instance without touching their overrides. Added and removed components and children propagate too. The root's position, rotation and name belong to the instance, as in Unity.
- **Prefab Mode**: double-click a prefab in the Project panel (or click **Open** in the prefab header or the asset's Inspector) to edit it on its own, in a stage lit by a neutral studio light. A breadcrumb bar over the Scene view leads back to the scene. Auto Save (on by default) writes the asset when each edit finishes; with it off, Save or Ctrl+S writes it, and leaving asks first. The scene keeps its unsaved changes, undo history, selection and camera, and its instances update on return without losing their overrides. Objects outside the prefab root are not saved, a prefab cannot contain itself, and Play leaves Prefab Mode first. `--prefab <path>` opens one on startup.
- **Hierarchy**: drag-and-drop parenting and reordering, rename, duplicate, delete, create menus, and "Create Prefab".
- **Inspector**: reflected component fields, Add Component search, script class picker with public fields, texture import settings, and a material editor.
- **Scene view**: Unity-style camera, GPU picking, transform gizmos, and dropping assets in to create objects or instantiate prefabs.
- **Game view**: renders the primary camera. **Play/Pause/Step** run on a cloned scene, and edits made while playing are discarded on Stop.
- **Project** browser: folder tree, thumbnails and material swatches, New Material/Scene/Folder, search, rename, move by drag, delete, and import by dropping files from Windows Explorer.
- **Console** with level filters, collapsing and clear-on-play. Also **Scene Settings**, **Stats**, and **Project Settings**.
- Copy/paste of GameObjects through the OS clipboard, snapshot-based **Undo/Redo**, scene save/load in JSON, unsaved-change prompts, and **File > Build Settings** (Ctrl+Shift+B): an ordered list of scenes in the build (drag scenes in; scene 0 starts the game), product name, window size, fullscreen and VSync, an output folder, and **Build** / **Build And Run** (Ctrl+B).
- **SceneManager**: scripts switch scenes with `SceneManager::LoadScene("Name")` or a build index (C#: `SceneManager.LoadScene`). Loads apply at the end of the frame, in the Player and in editor play mode (Stop returns to the edited scene). Sample scripts `LoadSceneButton` and `LoadSceneAfterDelay`; Demo3D and Demo2D link to each other.
- **Several scenes at once**: loads go into the running scene. `LoadSceneMode::Single` (the default) destroys every object not marked persistent and takes over the new scene's name and settings. `LoadSceneMode::Additive` adds a scene's objects; loading the same scene twice gives the second copy new ids. `SceneManager::UnloadScene("Name")` removes the root objects a scene contributed. `GetScene().DontDestroyOnLoad(entity)` (C#: `SceneManager.DontDestroyOnLoad(entity)`) keeps an object's whole root hierarchy, and its script instances and state, across single loads. `Scene::GetLoadedScenes()` / `SceneManager.LoadedSceneNames` list what is loaded, with persistent objects under "DontDestroyOnLoad". Demo3D's music uses the sample `KeepAcrossScenes` script, so it keeps playing through 3D -> 2D -> 3D, and the copy that comes back with Demo3D removes itself. Physics gravity stays as it was when play started.
- Shipped games use `DimensionGame.exe` (renamed to the product name): no console window, `Player.log` next to the executable, borderless fullscreen with Alt+Enter, and the project's window size and VSync. `DimensionEditor --build <folder>` builds headlessly (exit code 0 on success) for CI.

**Particles**
- The Particle System component covers duration and looping, randomized start lifetime, speed, size, rotation, spin and color, gravity and drag, rate-over-time and burst emission, and cone, sphere, box or point shapes in local or world space.
- **Color over Lifetime** (a Gradient), **Size over Lifetime** and **Speed over Lifetime** (Curves). Curve and Gradient are reflection field types with inline editors: drag keys, double-click to add (or to pick a gradient key's color), right-click to remove.
- **Collision** against a ground plane or, in play mode, the 3D physics colliders, with bounce, dampen and lifetime loss.
- **Texture sheet animation**: a particle texture can be a grid of frames (columns x rows, optionally only the first N) played over each particle's life, any number of cycles, optionally from a random start frame. **Noise** adds swirling curl-noise drift (strength, frequency, scroll speed). Curl noise is divergence-free, so particles swirl without bunching up. Demo3D's campfire smoke uses a generated 4x4 smoke flipbook (`Sprites/SmokeSheet.png`) with noise, and the flames flicker with noise.
- **Sub-emitters**: another Particle System bursts at each particle's birth, collision or death (fireworks).
- **Trails**: tapering, fading ribbons behind each particle.
- Particles render with alpha or additive blending, using a texture or the built-in soft particle; HDR start colors glow with bloom. Demo3D has fireworks (death sub-emitter + trails) and a spark fountain bouncing on the physics ground.
- Particles are camera-facing quads sorted with sprites. They preview live in the editor, the emitter shape is drawn when selected, and scripts can call `Play`, `Stop`, `Emit` and `Clear`.

**Lines and trails**
- **Line Renderer** (GameObject > Effects > Line): a polyline drawn as a joined, mitered ribbon. Positions can be local or world space, optionally looped, with a width curve and color gradient along the line. The ribbon faces the camera or lies flat facing the object's Z axis, and takes a texture that stretches or tiles every world unit, with alpha or additive blending. The Inspector edits the point list (count, per-point XYZ, remove), and the selected line shows a handle at each point.
- **Trail Renderer** (GameObject > Effects > Trail): records the object's world position while playing (every Min Vertex Distance) and fades points out after Time seconds, with width and color over the trail's age. Scripts can toggle `Emitting` and call `Clear()`.
- Both batch with sprites and particles. Demo3D has a trail on the orbiting moon, a gradient arch whose shape is rebuilt every frame by the C# `LineWave` script, and a ring lying flat around the campfire.

**UI (screen space)**
- Canvas with constant-pixel or scale-with-screen-size modes, and a UI Rect with Unity-style anchors, pivot, position and size.
- UI Image (simple or 9-sliced), UI Text (word wrap, horizontal and vertical alignment) and UI Button (hover and pressed tints). A click calls `OnClick()` on the button's scripts.
- Widgets: UI Slider (drag, whole numbers), UI Toggle (checkbox with label) and UI Input Field (click to focus, typing, Backspace, Enter to submit, caret, placeholder, character limit) and **UI Dropdown** (GameObject > UI > Dropdown). Changes call `OnUIValueChanged`, `OnUITextChanged` and `OnUISubmit` on the entity's scripts.
- **Input Field editing**: click to place the caret, drag to select, Left/Right (Ctrl: by word), Home/End, Shift to extend the selection, Backspace/Delete (Ctrl: by word), Ctrl+A/C/X/V with the system clipboard, and Tab/Shift+Tab to the next or previous field (selecting its text). Keys repeat while held, modifiers register even on quick taps, and with Num Lock off the keypad navigates. Long text scrolls to keep the caret visible. While a field has focus, gameplay scripts see no keyboard, so typing a name does not move the player.
- The dropdown's options are a string list edited in the Inspector. Clicking it opens a list drawn over all other UI, below the box or above it when there is no room. The list highlights the hovered item, marks the selection, scrolls with the wheel past Max Visible Items, and blocks clicks to widgets underneath. Picking an item reports its index through `OnUIValueChanged`; clicking elsewhere closes it.
- UI Layout Group stacks child rects vertically or horizontally with padding and spacing. UI Scroll View clips its children and scrolls its content with the mouse wheel; a vertical layout group used as content grows to fit its items. While the content overflows, a scrollbar on the right edge shows the visible fraction; drag its handle or click the track to jump. Demo3D's settings panel is a scroll view with a Cube Color dropdown driven by the C# `ColorDropdown` script.
- The Demo3D scene has a settings panel: a slider sets the sun intensity, a toggle enables the torus's Rotator script, and a C# script greets the name typed into the input field.
- UI renders in the Game view and the Player after tonemapping, so its colors are exact. GameObject > UI creates elements and adds a Canvas automatically.
- Coordinates use a top-left origin with y pointing down, unlike Unity's bottom-left.

**Gameplay systems**
- 2D physics with **Box2D v3**: rigidbodies, box and circle colliders, triggers, collision and trigger callbacks, raycasts, `OverlapCircle`/`OverlapBox`, and **Joint 2D** (fixed, hinge with angle limits, distance).
- 3D physics with **Jolt**: rigidbodies, box, sphere and capsule colliders (any can be **Is Trigger**), collision and trigger callbacks, raycasts, `OverlapSphere`/`OverlapBox`, and **Joint** (fixed, hinge with limits, point, distance) to another body or the world; connected bodies do not collide with each other unless Enable Collision is set.
- **Mesh Collider**: the exact triangles of a mesh (static and kinematic bodies) or its convex hull (dynamic bodies), from the Mesh Renderer's mesh or any mesh asset, including the built-in primitives.
- **Character Controller** (Unity-style, on Jolt's CharacterVirtual): a capsule moved by scripts with `Move(motion)` or `SimpleMove(speed)` (gravity applied; set `VerticalSpeed` to jump), that slides along walls, walks up steps (Step Offset) and slopes (Slope Limit), pushes rigidbodies and reports `IsGrounded`/`Velocity`. Raycasts and triggers see it. C#: `CharacterController.Move/SimpleMove/Jump`. Demo3D's green cube is now a character driven by the `CharacterMover` script (camera-relative WASD + Space); the torus and the glTF showcase have mesh colliders.
- **Joint motors, breakable joints and moving platforms**: hinge joints (3D and 2D) can drive toward a Motor Speed (degrees per second; positive turns the object counter-clockwise around its axis) with a maximum torque, and scripts can change it while playing. Every joint reports its reaction force and torque each step and breaks when it exceeds Break Force or Break Torque (0 = unbreakable; a hinge's own motor torque does not count). Breaking calls `OnJointBreak(force)` on the entity's C++ and C# scripts. C#: `Joint`/`Joint2D` (`UseMotor`, `MotorSpeed`, `IsBroken`, `Force`, `Torque`). Character Controllers standing on a moving body (e.g. a kinematic platform) ride along with it (Ride Moving Platforms, on by default). The sample `PlatformMover` script moves a kinematic platform back and forth; Demo3D has a moving platform and a motorized turnstile.
- **Physics layers**: every entity has a Layer (Inspector header). File > Project Settings names up to 32 layers and edits the layer collision matrix (2D and 3D). Queries take a layer mask; the default skips "Ignore Raycast". Joint gizmos show anchors, links and hinge axes. Demo3D has a swinging chain of point joints; Demo2D a pendulum and a hinged seesaw.
- Audio with **miniaudio**: Audio Source with 2D or 3D spatial sound, the listener follows the primary camera. Sources can **stream** long clips (music) instead of decoding them up front, be muted, and fire overlapping `PlayOneShot` sounds.
- **Audio Mixer** (Window > Audio Mixer): the project's groups (Master, Music, SFX by default, plus your own) with faders in dB, mute, solo and a low-pass filter, nested under any parent group. Audio Sources pick a group in **Output**. Volumes, mute, solo and filters change live; scripts call `AudioMixer::SetVolumeDb("Music", -12)` (C#: `AudioMixer.SetVolume`). Demo3D streams a generated music loop through Music, and its settings panel has a Music Volume slider.
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
- **API.** The `Behaviour` callbacks mirror the C++ ones (UI events are `OnValueChanged`, `OnTextChanged` and `OnSubmit`). Components available from C# are `Transform`, `Rigidbody` and `Rigidbody2D`, `MeshRenderer` and `SpriteRenderer` color, `UIText`, `UISlider`, `UIToggle`, `UIInputField`, `UIDropdown` (`Value`, `SelectedText`, `Options`), `Animator`, `ParticleSystem`, `LineRenderer` (`SetPositions`/`GetPositions`/`SetPosition`/`PositionCount`), `TrailRenderer` (`Emitting`, `Clear`) and `AudioSource`. Static helpers are `Input`, `Debug`, `Time` (`DeltaTime`, `time`, `FrameCount`), `Mathf` (Lerp, SmoothDamp, MoveTowards, PingPong, PerlinNoise, ...), `Random` (Range, Value, InsideUnitSphere, ColorHSV, ...), `Physics.Raycast` (with layer mask), `Physics.OverlapSphere`, `SceneManager`, `Entity.Find` and `Entity.Instantiate`. `Transform` adds `Forward`/`Right`/`Up`, `LocalToWorld` and `LookAt`.
- **Coroutines.** `StartCoroutine(IEnumerator)` runs until the first `yield`, then resumes after each `OnUpdate`: yield `null` (next frame), `new WaitForSeconds(s)`, `new WaitUntil(...)`, `new WaitWhile(...)`, another `IEnumerator` (runs nested) or a `Coroutine` (waits for it). Also `StopCoroutine`, `StopAllCoroutines`, `Invoke(name, delay)`, `InvokeRepeating(name, delay, rate)`, `CancelInvoke` and `Destroy(entity, delay)`. The sample `CoroutineLamp` cycles a traffic light and flashes on T.
- **Inspector fields.** Public fields of type `float`, `int`, `bool`, `string`, `Vector2/3/4`, `Color` and `Entity` appear in the Inspector, and the values save with the scene and prefabs.
- **Requirements.** C# scripting needs the .NET 10 SDK at build time and the .NET 10 runtime at run time. Without them the engine builds and runs with C++ scripting only. Build copies `nethost.dll`, the `managed/` folder and the compiled scripts.

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
With no arguments, both open `Projects/Sandbox`. A built game is `<Name>.exe` next to `DimensionEngine.dll`, the game module DLL, and a `Project/` folder. You can also pass `--frames N --screenshot out.png` to either program for automated checks (the player also takes `--move x,y,frame` and `--click x,y,frame` to drive the UI with a simulated pointer, and `--no-instancing` and `--no-vsync` for benchmarks; it logs draw-call and frame-time stats on exit), and `--build <folder>`, `--scene <path>` or `--open "Build Settings"|Preferences|"Project Settings"|"Tile Palette"|"Audio Mixer"` to the editor.

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

The available callbacks are `OnCreate`, `OnUpdate`, `OnFixedUpdate` (60 Hz), `OnDestroy`, `OnCollisionEnter/Exit`, `OnTriggerEnter/Exit`, `OnClick` (UI Button), `OnUIValueChanged` (UI Slider, UI Toggle and UI Dropdown), `OnUITextChanged`/`OnUISubmit` (UI Input Field), and `OnAnimationEvent` (Animator state events). `GetScene().IsPointerOverUI()` tells gameplay code when the mouse is over UI. Helpers include timers (`Invoke(fn, delay)`, `InvokeRepeating(fn, delay, interval)`, `CancelInvoke`), `Time()`, `Get<T>()`, `Find(name)`, `Instantiate(prefab, pos)`, `Destroy(e)`, `Raycast(origin, dir)`, `Raycast2D(origin, dir)`, `Input::IsKeyDown`, `Physics2DWorld::*`, `Physics3DWorld::*` and `AudioWorld::Play`. Entity and asset fields such as `Entity Target;` or `.As(FieldType::Prefab)` get drag-and-drop slots.

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
- **Scene targets** hold HDR color, an entity-ID buffer (R32_UINT), depth (sampled by SSAO), two half-resolution AO images, a bloom mip chain, an LDR image for FXAA, and the tonemapped RGBA8 output. The editor shows the output as an ImGui image; the player blits it to the swapchain.
- **Play mode** runs `Scene::Clone()` and then `OnRuntimeStart()`, which creates Box2D/Jolt worlds, audio sources and script instances. Stop throws the clone away.
- **Assets** are referenced by GUID. `.meta` files hold the GUID and import settings, so moving or renaming in the Project panel keeps references intact.

## Roadmap toward Unity parity

Done in this milestone: everything listed under Features. These are the largest remaining areas, in a suggested order:

1. **Rendering:** soft (PCSS) shadows, light cookies, clustered lighting for many lights, GGX-prefiltered (instead of box-filtered) probe mips, blending between overlapping probes, per-slot material overrides on Mesh Renderers, vertex tangents for imported normal maps, MSAA, screen-space reflections, and instancing for skinned and transparent meshes.
2. **Editor:** component reordering, mixed-value display for curves, gradients and lists, nested prefabs and prefab variants, an asset import pipeline with a cache, and themes.
3. **Animation:** 2D blend trees, layer sync and per-layer IK, root motion, IK, sub-state machines and timeline curves.
4. **UI system:** horizontal scrolling, grid layouts, gamepad/arrow-key navigation between all widgets, multi-line input fields, MSDF text and rich text, and editing UI rects directly in the Scene view.
5. **Scripting:** custom C# components shown in Add Component, C# events/delegates on engine objects, C++20 coroutines for native scripts, keeping script state across a reload while playing, and debugger attach.
6. **More physics:** motors for slider/distance joints, convex decomposition for dynamic concave meshes, cloth and soft bodies, and vehicles.
7. **More audio:** mixer snapshots and send/return effects (reverb, echo), level meters, audio occlusion, and compressed formats (Ogg Vorbis).
8. **Effects:** GPU particles, velocity/force over lifetime, smooth (tangent) curves, and line joins/caps and Scene-view point dragging for Line Renderers.
9. **Platform:** Linux support (file dialogs and paths), an asset bundle format with build-time stripping of unused assets, asynchronous (background) scene loading, and an application icon.
