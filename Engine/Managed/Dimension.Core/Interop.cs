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
        public delegate* unmanaged<IntPtr, ulong, float*, int, void> CharacterMove;
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
    }

    // C# functions callable from the engine. Layout must match ManagedApi in CSharpRuntime.cpp.
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct ManagedApi
    {
        public delegate* unmanaged<byte*, int> LoadScripts;
        public delegate* unmanaged<void> UnloadScripts;
        public delegate* unmanaged<byte*, IntPtr, ulong, IntPtr> CreateInstance;
        public delegate* unmanaged<IntPtr, int, void*, void> SetField;
        public delegate* unmanaged<IntPtr, int, float, void> Invoke;
        public delegate* unmanaged<IntPtr, int, ulong, void> InvokeEntity;
        public delegate* unmanaged<IntPtr, void> DestroyInstance;
        public delegate* unmanaged<IntPtr, int, byte*, void> InvokeString;
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
                Debug.Log($"C# scripting ready (.NET {Environment.Version})");
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
