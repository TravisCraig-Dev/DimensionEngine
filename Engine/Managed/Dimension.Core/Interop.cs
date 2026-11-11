using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Dimension.Interop
{
    // Engine functions callable from C#. Layout must match NativeApi in CSharpRuntime.cpp.
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct NativeApi
    {
        public delegate* unmanaged<int, byte*, void> Log;
        public delegate* unmanaged<IntPtr, ulong, float*, float*, float*, void> GetTransform;
        public delegate* unmanaged<IntPtr, ulong, float*, float*, float*, void> SetTransform;
        public delegate* unmanaged<IntPtr, ulong, float*, void> GetWorldPosition;
        public delegate* unmanaged<IntPtr, ulong, byte*, int> HasComponent;
        public delegate* unmanaged<IntPtr, ulong, byte*, void> AddComponent;
        public delegate* unmanaged<IntPtr, ulong, int> IsValid;
        public delegate* unmanaged<IntPtr, byte*, ulong> FindEntity;
        public delegate* unmanaged<IntPtr, ulong, void> Destroy;
        public delegate* unmanaged<IntPtr, ulong, float*, ulong> Instantiate;
        public delegate* unmanaged<IntPtr, ulong, byte*> GetName;
        public delegate* unmanaged<IntPtr, ulong, byte*, void> SetName;
        public delegate* unmanaged<int, int> IsKeyDown;
        public delegate* unmanaged<int, int> IsKeyPressed;
        public delegate* unmanaged<int, int> IsMouseDown;
        public delegate* unmanaged<float*, void> GetMousePosition;
        public delegate* unmanaged<IntPtr, ulong, float*, void> GetVelocity2D;
        public delegate* unmanaged<IntPtr, ulong, float*, void> SetVelocity2D;
        public delegate* unmanaged<IntPtr, ulong, float*, void> ApplyImpulse2D;
        public delegate* unmanaged<IntPtr, ulong, float*, void> ApplyForce2D;
        public delegate* unmanaged<IntPtr, ulong, float*, void> GetVelocity3D;
        public delegate* unmanaged<IntPtr, ulong, float*, void> SetVelocity3D;
        public delegate* unmanaged<IntPtr, ulong, float*, void> AddImpulse3D;
        public delegate* unmanaged<IntPtr, ulong, float*, void> AddForce3D;
        public delegate* unmanaged<IntPtr, float*, float*, float, ulong*, float*, float*, float*, int> Raycast3D;
        public delegate* unmanaged<IntPtr, ulong, byte*, void> SetUIText;
        public delegate* unmanaged<IntPtr, ulong, byte*> GetUIText;
        public delegate* unmanaged<IntPtr, ulong, byte*, float, void> AnimatorCrossFade;
        public delegate* unmanaged<IntPtr, ulong, int, void> ParticlesEmit;
        public delegate* unmanaged<IntPtr, ulong, int, void> ParticlesPlay;
        public delegate* unmanaged<IntPtr, ulong, int, void> AudioPlay;
        public delegate* unmanaged<IntPtr, ulong, float*, int> GetColor;
        public delegate* unmanaged<IntPtr, ulong, float*, void> SetColor;
        public delegate* unmanaged<byte*, void> RegisterScriptClass;
        public delegate* unmanaged<float> GetDeltaTime;
        public delegate* unmanaged<IntPtr, ulong, byte*, float, int, void> AnimatorSetParameter;
        public delegate* unmanaged<IntPtr, ulong, byte*> AnimatorGetState;
        public delegate* unmanaged<IntPtr, ulong, float> GetUIValue;
        public delegate* unmanaged<IntPtr, ulong, float, void> SetUIValue;
        public delegate* unmanaged<IntPtr, float*, float, uint, ulong*, int, int> OverlapSphere;
        public delegate* unmanaged<IntPtr, float*, float*, float, uint, ulong*, float*, float*, float*, int> Raycast3DMask;
        public delegate* unmanaged<byte*, void> LoadSceneByName;
        public delegate* unmanaged<int, void> LoadSceneByIndex;
        public delegate* unmanaged<IntPtr, byte*> GetSceneName;
        public delegate* unmanaged<int> GetSceneCount;
        public delegate* unmanaged<IntPtr, float> GetTime;
        public delegate* unmanaged<IntPtr, ulong> GetFrameCount;
        public delegate* unmanaged<IntPtr, ulong, float*, void> GetWorldMatrix;
        public delegate* unmanaged<IntPtr, ulong, int> AudioIsPlaying;
        public delegate* unmanaged<IntPtr, ulong, ulong, float, void> AudioPlayOneShot;
        public delegate* unmanaged<byte*, float, void> MixerSetVolume;
        public delegate* unmanaged<byte*, float> MixerGetVolume;
        public delegate* unmanaged<IntPtr, ulong, float*, int, int> CharacterMove;
        public delegate* unmanaged<IntPtr, ulong, float*, float*, int> CharacterState;
        public delegate* unmanaged<IntPtr, ulong, float, void> CharacterSetVerticalSpeed;
        public delegate* unmanaged<IntPtr, ulong, float*, int, void> LineSetPositions;
        public delegate* unmanaged<IntPtr, ulong, float*, int, int> LineGetPositions;
        public delegate* unmanaged<IntPtr, ulong, void> TrailClear;
        public delegate* unmanaged<IntPtr, ulong, int, void> TrailSetEmitting;
        public delegate* unmanaged<IntPtr, ulong, int> TrailIsEmitting;
        public delegate* unmanaged<IntPtr, ulong, byte*> DropdownGetOptions;
        public delegate* unmanaged<IntPtr, ulong, byte*, void> DropdownSetOptions;
        public delegate* unmanaged<byte*, int, int, void> LoadSceneWithMode;
        public delegate* unmanaged<byte*, void> UnloadScene;
        public delegate* unmanaged<IntPtr, ulong, void> DontDestroyOnLoad;
        public delegate* unmanaged<IntPtr, int> GetLoadedSceneCount;
        public delegate* unmanaged<IntPtr, int, byte*> GetLoadedSceneName;
        public delegate* unmanaged<IntPtr, ulong, int, float, void> SetJointMotor;
        public delegate* unmanaged<IntPtr, ulong, float*, int> GetJointState;
        public delegate* unmanaged<byte*, float, int> MixerTransition;
        public delegate* unmanaged<int, int, int, int> GamepadButton;
        public delegate* unmanaged<int, int, float> GamepadAxis;
        public delegate* unmanaged<byte*, int, int, int> LoadSceneAsync;
        public delegate* unmanaged<int, float*, int> AsyncState;
        public delegate* unmanaged<int, int, void> AsyncAllowActivation;
        public delegate* unmanaged<float, float, float, int, void> GamepadVibrate;
        public delegate* unmanaged<IntPtr, ulong, byte*, byte*, float*, int> GetComponentField;
        public delegate* unmanaged<IntPtr, ulong, byte*, byte*, float*, int, int> SetComponentField;
        public delegate* unmanaged<IntPtr, ulong, int, float> VehicleState;
        public delegate* unmanaged<IntPtr, ulong, int, float, float> TimelineControl;
        public delegate* unmanaged<int, float, int, int> MicrophoneStart;
        public delegate* unmanaged<int, float> MicrophoneState; // 0 stop (returns 0), 1 recording, 2 level, 3 peak, 4 position, 5 sample rate
        public delegate* unmanaged<byte*, int> MicrophoneSave;
        public delegate* unmanaged<byte*> MicrophoneDevices;
        public delegate* unmanaged<IntPtr, ulong, float*, float*, void> RagdollImpulse;
        public delegate* unmanaged<IntPtr, ulong, int, float> RagdollQuery; // 0 face up (1/0), 1 get-up progress
        public delegate* unmanaged<IntPtr, ulong, float*, int> BreakableBreak; // world point; returns the piece count
        public delegate* unmanaged<IntPtr, ulong, byte*, void> RagdollSetPartial; // bone name ("" = whole body)
        public delegate* unmanaged<IntPtr, float*, float, float, int, int> TerrainCrater; // world point, radius, depth, paint layer
        public delegate* unmanaged<IntPtr, ulong, int, float*, float> NavAgent; // see NavMeshAgent
        public delegate* unmanaged<IntPtr, int, float*, float*, float, float*, int, int> NavQuery; // see NavMesh
        // Editor extensions (EditorApi.cs): a control (see EditorGUI), a text field, selection (0 get, 1 set, 2 create
        // an entity named, 3 count, 4 the n-th), an undo step, and registering a menu item or window (JSON).
        public delegate* unmanaged<int, byte*, float*, int, int> EditorGui;
        public delegate* unmanaged<byte*, byte*, byte*> EditorGuiText;
        public delegate* unmanaged<int, ulong, byte*, ulong> EditorSelection;
        public delegate* unmanaged<byte*, void> EditorUndo;
        public delegate* unmanaged<byte*, void> RegisterEditorItem;
        // A picker: kind 0 an entity (packed id), else an asset GUID of that kind (see EditorGUI.AssetKind).
        public delegate* unmanaged<int, byte*, ulong, ulong> EditorGuiRef;
        // A gizmo while OnDrawGizmos runs: 0 a line (from, to, color), 1 a wire sphere (matrix, center, radius,
        // color), 2 a wire cube (matrix, center, size, color).
        public delegate* unmanaged<int, float*, void> Gizmo;
        public delegate* unmanaged<IntPtr, int> SceneRunning; // is the scene playing?
        // A Scene view handle from Inspector.OnSceneGUI (see Handles): kind, values in and out, their count, text.
        public delegate* unmanaged<int, float*, int, byte*, int> EditorHandle;
        // The Profiler: 0 begin a sample (name), 1 end it, 2 set a counter (name, value).
        public delegate* unmanaged<int, byte*, float, void> ProfilerSample;
        // Debug.DrawLine: from, to, color (rgba), duration (11 floats).
        public delegate* unmanaged<float*, void> DebugLine;
        // PlayerPrefs: op (see PlayerPrefs in Api.cs), key, value as text -> a value as text, or null.
        public delegate* unmanaged<int, byte*, byte*, byte*> PlayerPrefs;
        // Input actions by name: 0 button held, 1 pressed this frame, 2 released this frame, 3 axis value.
        public delegate* unmanaged<int, byte*, float> InputAction;
        // Localization: op (see Localization in Api.cs), text, scene, entity -> text, or null.
        public delegate* unmanaged<int, byte*, IntPtr, ulong, byte*> Localization;
        // A data asset's file as JSON ({"class", "fields"}), or null.
        public delegate* unmanaged<ulong, byte*> DataAssetFile;
        // Hierarchy, active state, layer and tag: scene, entity, op (see Entity in Api.cs), argument, text.
        public delegate* unmanaged<IntPtr, ulong, int, ulong, byte*, ulong> Hierarchy;
        public delegate* unmanaged<IntPtr, ulong, byte*> GetTag;
        public delegate* unmanaged<IntPtr, byte*, ulong*, int, int> FindAllWithTag;
        // The view and cameras: scene, camera, op (see Camera in Components.cs), values in/out -> 0 when no camera.
        public delegate* unmanaged<IntPtr, ulong, int, float*, int> ViewOp;
        public delegate* unmanaged<IntPtr, ulong> MainCamera;
        public delegate* unmanaged<IntPtr, int, float, float> TimeControl; // 0 scale, 1 set scale, 2 unscaled dt, 3 unscaled time
        public delegate* unmanaged<int, int, int> CursorControl;           // 0 get mode, 1 set mode (0 normal, 1 hidden, 2 locked)
        public delegate* unmanaged<int, int> IsMousePressed;
        // 2D physics (see Physics2D in Api.cs): raycast op, origin+direction, max distance, mask, ids out, hit data out, max.
        public delegate* unmanaged<IntPtr, int, float*, float, uint, ulong*, float*, int, int> Raycast2D;
        public delegate* unmanaged<IntPtr, int, float*, uint, ulong*, int, int> Overlap2D; // 0 circle, 1 box, 2 point
        public delegate* unmanaged<IntPtr, ulong, int, float, float> Body2D;               // 0 angular velocity, 1 set, 2 torque
        // 3D (see Physics in Api.cs): op, values in, max distance, mask, ids out, hit data out (7 floats each), max.
        public delegate* unmanaged<IntPtr, int, float*, float, uint, ulong*, float*, int, int> Query3D;
        public delegate* unmanaged<IntPtr, ulong, int, float*, void> Body3D; // see Rigidbody in Components.cs
        // Scripts (see ScriptHost.ScriptsOf): an object's managed scripts, every one in a scene, objects with a component,
        // adding a script.
        public delegate* unmanaged<IntPtr, ulong, IntPtr*, int, int> EntityScripts;
        public delegate* unmanaged<IntPtr, IntPtr*, int, int, int> SceneScripts;
        public delegate* unmanaged<IntPtr, byte*, ulong*, int, int, int> EntitiesWithComponent;
        public delegate* unmanaged<IntPtr, ulong, byte*, IntPtr> AddScript;
        // Copies (kind 0 an object, 1 a prefab GUID) with a pose (position, rotation xyzw; flags 1 / 2) under a parent.
        public delegate* unmanaged<IntPtr, int, ulong, float*, int, ulong, ulong> InstantiateObject;
        public delegate* unmanaged<IntPtr, ulong, byte*, IntPtr, int> RemoveComponent; // a script (handle) or a component (name)
        public delegate* unmanaged<IntPtr, ulong, int, int, byte*, float, float> AnimatorQuery; // see Animator in Components.cs
        public delegate* unmanaged<IntPtr, ulong, int, int, byte*> AnimatorStateName;           // layer; 0 current, 1 fading out, 2 layer name
        public delegate* unmanaged<IntPtr, ulong, int, float, float> AudioControl;               // see AudioSource in Components.cs
        public delegate* unmanaged<IntPtr, ulong, int, ulong, ulong> AudioClip;                  // 0 get, 1 set the clip
        public delegate* unmanaged<IntPtr, ulong, float*, float, void> PlayClipAtPoint;
        public delegate* unmanaged<IntPtr, ulong, byte*, byte*, int, ulong, ulong> ComponentAsset; // 0 get, 1 set; ~0: no such asset field
        public delegate* unmanaged<IntPtr, ulong, float*, int> UIRectScreen;                       // min x, y, max x, y, scale
        public delegate* unmanaged<IntPtr, ulong, int, float, float> SpriteAnimatorTime;           // 0 get, 1 seek
        public delegate* unmanaged<IntPtr, ulong, int, float> ParticlesControl;                    // see ParticleSystem
        public delegate* unmanaged<IntPtr, ulong, float*, int, int, void> ParticlesEmitWith;
        public delegate* unmanaged<IntPtr, ulong, int, int, int, int, float*, int> TilemapOp;      // see Tilemap
        public delegate* unmanaged<IntPtr, ulong, float*, float*, int> TerrainSample;              // ground xyz, normal xyz
        public delegate* unmanaged<IntPtr, ulong, int, int, ulong, ulong> RendererMaterials;       // see MeshRenderer
        public delegate* unmanaged<IntPtr, int, int, int, float*, int> PhysicsSettings;            // see Physics.Gravity
        public delegate* unmanaged<IntPtr, ulong, ulong, int, int, int> PhysicsIgnore;
        public delegate* unmanaged<IntPtr, ulong, void> RefreshColliders;
        public delegate* unmanaged<int, byte*> LayerName;
        public delegate* unmanaged<IntPtr, ulong, void> SetMainCamera;
        public delegate* unmanaged<IntPtr, ulong, void> RebuildJoint;
        public delegate* unmanaged<IntPtr, ulong, byte*, byte*, int, ulong, ulong> ComponentEntity;  // 0 get, 1 set
        public delegate* unmanaged<IntPtr, ulong, int, float*, float> RigidbodyOp;  // see Rigidbody in Components.cs
        public delegate* unmanaged<IntPtr, int, float*, float, uint, int, ulong*, float*, int, int> PhysicsQuery;  // see Physics in Api.cs
        public delegate* unmanaged<IntPtr, ulong, int, float*, int> ColliderQuery;
        public delegate* unmanaged<int, int, int, float> InputQuery;  // see Input in Api.cs
        public delegate* unmanaged<int, int, byte*> InputText;
        public delegate* unmanaged<int, double, double, double, double> AppControl;  // see Application in Api.cs
        public delegate* unmanaged<int, byte*> AppText;
        public delegate* unmanaged<IntPtr, ulong, int> CharacterFlags;
        public delegate* unmanaged<IntPtr, ulong, int, float*, int, int, int> NavAgentPath;
        public delegate* unmanaged<IntPtr, ulong, int, byte*, float*, ulong*, float> MaterialOp;  // see Material in Assets.cs
        public delegate* unmanaged<IntPtr, ulong, ulong> RendererMaterialInstance;
        public delegate* unmanaged<IntPtr, ulong, int, double, double> AudioTiming;  // see AudioSettings in Components.cs
        public delegate* unmanaged<IntPtr, ulong, int, float*, void> Body2DOp;  // see Rigidbody2D in Components.cs
        public delegate* unmanaged<IntPtr, ulong, byte*, byte*, int, byte*, byte*> ComponentString;
        public delegate* unmanaged<IntPtr, int, float*, ulong*, int, int> UIEvents;  // see EventSystem in Api.cs
        public delegate* unmanaged<IntPtr, ulong, int, int, byte*, float*, int> AnimatorOp;  // see Animator in Components.cs
        public delegate* unmanaged<IntPtr, ulong, int, int, ulong, byte**, ulong> TimelineTrack;  // see Timeline in Components.cs
        public delegate* unmanaged<IntPtr, ulong, ulong, void> TimelineAssetChanged;
        public delegate* unmanaged<IntPtr, ulong, int, float*, int> EnvironmentOp;  // see LightProbes / WindZone in Components.cs
        public delegate* unmanaged<IntPtr, int, ulong*, int*, uint*, int, int, int> TextureOp;  // see Texture2D in Assets.cs
        public delegate* unmanaged<int, byte*, ulong*, byte*> BundleOp;  // see AssetBundle in Assets.cs
        public delegate* unmanaged<IntPtr, ulong, int, float*, ulong*, int> VideoOp;  // see VideoPlayer in Components.cs
        public delegate* unmanaged<int, float*, byte*, int> VoiceOp;  // see VoiceChat in Components.cs
        public delegate* unmanaged<IntPtr, ulong, int, int, float, byte*, float> BlendShapeOp;  // see MeshRenderer in Components.cs
        public delegate* unmanaged<IntPtr, ulong, int, byte*> BlendShapeName;
        public delegate* unmanaged<IntPtr, ulong, int, int, int, ulong, float*, ulong> ConstraintOp;  // see ConstraintBase in Components.cs
    }

    // C# functions callable from the engine. Layout must match ManagedApi in CSharpRuntime.cpp.
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct ManagedApi
    {
        public delegate* unmanaged<byte*, int, int> LoadScripts; // path, 1 = fixed (a built game: no reloads)
        public delegate* unmanaged<void> UnloadScripts;
        public delegate* unmanaged<byte*, IntPtr, ulong, IntPtr> CreateInstance;
        public delegate* unmanaged<IntPtr, int, void*, void> SetField;
        public delegate* unmanaged<IntPtr, int, float, void> Invoke;
        public delegate* unmanaged<IntPtr, int, ulong, void> InvokeEntity;
        public delegate* unmanaged<IntPtr, void> DestroyInstance;
        public delegate* unmanaged<IntPtr, int, byte*, void> InvokeString;
        public delegate* unmanaged<IntPtr, ulong, int, float, ulong, byte*, void> RaiseEvent;
        public delegate* unmanaged<IntPtr, byte*> GetFields;
        public delegate* unmanaged<int, int> Debugger; // 0 = is attached?, 1 = launch (JIT debugger prompt), 2 = break
        public delegate* unmanaged<int, int, IntPtr, void> EditorCall; // 0 run menu item, 1 window OnGUI; its id; the scene
        // A custom Inspector: id, live script (or 0), class, saved values (JSON), scene, entity -> values after (JSON)
        public delegate* unmanaged<int, IntPtr, byte*, byte*, IntPtr, ulong, byte*> Inspect;
        // A field's property drawer: field index, then as Inspect
        public delegate* unmanaged<int, IntPtr, byte*, byte*, IntPtr, ulong, byte*> DrawField;
        // The Inspector's Debug mode: a script's unsaved fields as text (arguments as DrawField's)
        public delegate* unmanaged<int, IntPtr, byte*, byte*, IntPtr, ulong, byte*> DebugFields;
        // OnDrawGizmos (and OnDrawGizmosSelected when the first argument is 1), and [ContextMenu] item n (returns the
        // values after); the rest as DrawField's
        public delegate* unmanaged<int, IntPtr, byte*, byte*, IntPtr, ulong, byte*> DrawGizmos;
        public delegate* unmanaged<int, IntPtr, byte*, byte*, IntPtr, ulong, byte*> RunContextMenu;
        // A custom Inspector's OnSceneGUI (arguments and result as Inspect's)
        public delegate* unmanaged<int, IntPtr, byte*, byte*, IntPtr, ulong, byte*> SceneGUI;
        // A data asset's file was written (0: play starts, every data asset object is made again).
        public delegate* unmanaged<ulong, void> DataAssetChanged;
        // A collision: phase (0 enter, 1 stay, 2 exit), other, data (see ScriptHost.InvokeCollision), contact count.
        public delegate* unmanaged<IntPtr, int, ulong, float*, int, void> InvokeCollision;
        // Coroutines waiting for a point in the frame (0 after a physics step, 1 its end), for a scene.
        public delegate* unmanaged<IntPtr, int, void> CoroutinePhase;
        // A UI pointer event: kind, data (see ScriptHost.InvokePointer), the over / press / drag elements.
        public delegate* unmanaged<IntPtr, int, float*, ulong*, void> InvokePointer;
        // An Animator state's behaviours (see ScriptHost.InvokeStateBehaviour).
        public delegate* unmanaged<IntPtr, ulong, int, int, int, byte*, byte*, byte*, float*, void> InvokeStateBehaviour;
        // A script's property or method by name for UI Bindings (see ScriptHost.MemberOp).
        public delegate* unmanaged<IntPtr, int, byte*, byte*, byte*> MemberOp;
        // The Test Runner (see ScriptHost.RunTests).
        public delegate* unmanaged<int, byte*, byte*> RunTests;
    }

    internal static unsafe class Native
    {
        internal static NativeApi Api;

        // Current scene of the script being called (used by static helpers such as Entity.Find).
        [ThreadStatic] internal static IntPtr CurrentScene;

        internal static string FromUtf8(byte* s) => s == null ? null : Marshal.PtrToStringUTF8((IntPtr)s);

        // Calls `fn` with a NUL-terminated UTF-8 copy of `s` on the stack (or heap for long strings).
        internal static T WithUtf8<T>(string s, Func<IntPtr, T> fn)
        {
            s ??= string.Empty;
            int max = Encoding.UTF8.GetMaxByteCount(s.Length) + 1;
            if (max <= 1024)
            {
                byte* buffer = stackalloc byte[max];
                int n = Encoding.UTF8.GetBytes(s, new Span<byte>(buffer, max));
                buffer[n] = 0;
                return fn((IntPtr)buffer);
            }
            IntPtr heap = Marshal.StringToCoTaskMemUTF8(s);
            try { return fn(heap); }
            finally { Marshal.FreeCoTaskMem(heap); }
        }

        internal static void WithUtf8(string s, Action<IntPtr> fn) => WithUtf8<int>(s, p => { fn(p); return 0; });
    }

    // Entry point called once by the engine after the runtime starts.
    public static unsafe class Host
    {
        [UnmanagedCallersOnly]
        public static int Initialize(IntPtr nativeApi, IntPtr managedApi)
        {
            try
            {
                Native.Api = Unsafe.Read<NativeApi>((void*)nativeApi);
                var api = (ManagedApi*)managedApi;
                api->LoadScripts = &ScriptHost.LoadScripts;
                api->UnloadScripts = &ScriptHost.UnloadScripts;
                api->CreateInstance = &ScriptHost.CreateInstance;
                api->SetField = &ScriptHost.SetField;
                api->Invoke = &ScriptHost.Invoke;
                api->InvokeEntity = &ScriptHost.InvokeEntity;
                api->DestroyInstance = &ScriptHost.DestroyInstance;
                api->InvokeString = &ScriptHost.InvokeString;
                api->RaiseEvent = &ScriptHost.RaiseEvent;
                api->GetFields = &ScriptHost.GetFields;
                api->Debugger = &ScriptHost.DebuggerControl;
                api->EditorCall = &ScriptHost.EditorCall;
                api->Inspect = &ScriptHost.Inspect;
                api->DrawField = &ScriptHost.DrawField;
                api->DebugFields = &ScriptHost.DebugFields;
                api->DrawGizmos = &ScriptHost.DrawGizmos;
                api->RunContextMenu = &ScriptHost.RunContextMenu;
                api->SceneGUI = &ScriptHost.SceneGUI;
                api->DataAssetChanged = &ScriptHost.DataAssetChanged;
                api->InvokeCollision = &ScriptHost.InvokeCollision;
                api->CoroutinePhase = &ScriptHost.CoroutinePhase;
                api->InvokePointer = &ScriptHost.InvokePointer;
                api->InvokeStateBehaviour = &ScriptHost.InvokeStateBehaviour;
                api->MemberOp = &ScriptHost.MemberOp;
                api->RunTests = &ScriptHost.RunTests;
                Debug.Log($"C# scripting ready (.NET {Environment.Version}, {System.Runtime.InteropServices.RuntimeEnvironment.GetRuntimeDirectory()})");
                return 1;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e);
                return 0;
            }
        }
    }
}
