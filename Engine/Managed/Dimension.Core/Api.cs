using System;
using System.Numerics;
using System.Runtime.InteropServices;
using Dimension.Interop;

namespace Dimension
{
    /// <summary>Base class for C# gameplay scripts (the engine's MonoBehaviour).</summary>
    /// <remarks>Public fields of type float, int, bool, string, Vector2/3/4, Color and Entity appear in the Inspector.</remarks>
    public abstract class Behaviour
    {
        public Entity Entity { get; internal set; }
        public Transform Transform => Entity.Transform;
        public string Name { get => Entity.Name; set { var e = Entity; e.Name = value; } }

        public virtual void OnCreate() { }
        public virtual void OnUpdate(float deltaTime) { }
        public virtual void OnFixedUpdate(float fixedDeltaTime) { }
        public virtual void OnDestroy() { }
        public virtual void OnCollisionEnter(Entity other) { }
        public virtual void OnCollisionExit(Entity other) { }
        public virtual void OnTriggerEnter(Entity other) { }
        public virtual void OnTriggerExit(Entity other) { }
        /// <summary>Called when a UI Button on this entity is clicked.</summary>
        public virtual void OnClick() { }
        /// <summary>Called when a UI Slider (its value) or UI Toggle (1 = on, 0 = off) on this entity changes.</summary>
        public virtual void OnValueChanged(float value) { }
        /// <summary>Called when the text of a UI Input Field on this entity is edited.</summary>
        public virtual void OnTextChanged(string text) { }
        /// <summary>Called when Enter is pressed in a UI Input Field on this entity.</summary>
        public virtual void OnSubmit(string text) { }
        /// <summary>Called when an Animator state event on this entity is reached (the event's function name).</summary>
        public virtual void OnAnimationEvent(string name) { }

        // ---- Coroutines and delayed calls (like MonoBehaviour) ---------------------------------
        internal CoroutineRunner Coroutines;

        /// <summary>Runs `routine` until its first yield now, then resumes it every frame after OnUpdate.
        /// Yield null (next frame), WaitForSeconds, WaitUntil, WaitWhile, another IEnumerator (run nested) or a Coroutine (wait for it).</summary>
        public Coroutine StartCoroutine(System.Collections.IEnumerator routine) => (Coroutines ??= new CoroutineRunner()).Start(routine, null);
        public void StopCoroutine(Coroutine coroutine) => Coroutines?.Stop(coroutine);
        public void StopAllCoroutines() => Coroutines?.StopAll();

        /// <summary>Calls the parameterless method `methodName` on this script after `delay` seconds.</summary>
        public void Invoke(string methodName, float delay) => StartInvoke(methodName, delay, -1.0f);
        /// <summary>Calls `methodName` after `delay` seconds, then every `rate` seconds.</summary>
        public void InvokeRepeating(string methodName, float delay, float rate) => StartInvoke(methodName, delay, MathF.Max(rate, 1e-4f));
        public void CancelInvoke() => Coroutines?.StopTagged(null);
        public void CancelInvoke(string methodName) => Coroutines?.StopTagged(methodName);
        public bool IsInvoking(string methodName) => Coroutines != null && Coroutines.HasTagged(methodName);

        /// <summary>Destroys `entity` after `delay` seconds.</summary>
        public void Destroy(Entity entity, float delay) => StartCoroutine(DestroyLater(entity, delay));

        private void StartInvoke(string methodName, float delay, float rate)
        {
            var method = GetType().GetMethod(methodName, System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public |
                                                         System.Reflection.BindingFlags.NonPublic, null, Type.EmptyTypes, null);
            if (method == null)
            {
                Debug.LogError($"{GetType().Name}.Invoke: no parameterless method '{methodName}'");
                return;
            }
            (Coroutines ??= new CoroutineRunner()).Start(InvokeRoutine(method, delay, rate), methodName);
        }

        private System.Collections.IEnumerator InvokeRoutine(System.Reflection.MethodInfo method, float delay, float rate)
        {
            if (delay > 0.0f) yield return new WaitForSeconds(delay);
            do
            {
                method.Invoke(this, null);
                if (rate > 0.0f) yield return new WaitForSeconds(rate);
            } while (rate > 0.0f);
        }

        private static System.Collections.IEnumerator DestroyLater(Entity entity, float delay)
        {
            yield return new WaitForSeconds(delay);
            if (entity.IsValid) entity.Destroy();
        }

        public T GetComponent<T>() where T : Component, new() => Entity.GetComponent<T>();
        public bool HasComponent<T>() where T : Component, new() => Entity.HasComponent<T>();
        public Entity Find(string name) => Entity.Find(name);
        public Entity Instantiate(ulong prefabGuid, Vector3 position) => Entity.Instantiate(prefabGuid, position);
        public void Destroy(Entity entity) => entity.Destroy();
    }

    /// <summary>RGBA color in sRGB space, components 0..1.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Color
    {
        public float R, G, B, A;
        public Color(float r, float g, float b, float a = 1.0f) { R = r; G = g; B = b; A = a; }
        public static Color White => new(1, 1, 1);
        public static Color Black => new(0, 0, 0);
        public static Color Red => new(1, 0, 0);
        public static Color Green => new(0, 1, 0);
        public static Color Blue => new(0, 0, 1);
        public static Color Lerp(Color a, Color b, float t) =>
            new(a.R + (b.R - a.R) * t, a.G + (b.G - a.G) * t, a.B + (b.B - a.B) * t, a.A + (b.A - a.A) * t);
        /// <summary>Hue/saturation/value to RGB (all 0..1).</summary>
        public static Color FromHSV(float h, float s, float v)
        {
            h = (h % 1.0f + 1.0f) % 1.0f * 6.0f;
            int i = (int)MathF.Floor(h);
            float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
            return i switch { 0 => new(v, t, p), 1 => new(q, v, p), 2 => new(p, v, t), 3 => new(p, q, v), 4 => new(t, p, v), _ => new(v, p, q) };
        }
        public override string ToString() => $"({R:0.###}, {G:0.###}, {B:0.###}, {A:0.###})";
    }

    /// <summary>Handle to a scene entity (GameObject).</summary>
    public readonly unsafe struct Entity : IEquatable<Entity>
    {
        internal readonly IntPtr Scene;
        internal readonly ulong Id;
        internal Entity(IntPtr scene, ulong id) { Scene = scene; Id = id; }

        public static Entity Null => default;
        public bool IsValid => Scene != IntPtr.Zero && Native.Api.IsValid(Scene, Id) != 0;
        public Transform Transform => new Transform { Entity = this };

        public string Name
        {
            get => Native.FromUtf8(Native.Api.GetName(Scene, Id));
            set { var self = this; Native.WithUtf8(value, p => Native.Api.SetName(self.Scene, self.Id, (byte*)p)); }
        }

        public bool HasComponent(string componentName)
        {
            var self = this;
            return Native.WithUtf8(componentName, p => Native.Api.HasComponent(self.Scene, self.Id, (byte*)p)) != 0;
        }
        public bool HasComponent<T>() where T : Component, new() => HasComponent(new T().NativeName);

        public T GetComponent<T>() where T : Component, new()
        {
            var c = new T { Entity = this };
            return HasComponent(c.NativeName) ? c : null;
        }

        public T AddComponent<T>() where T : Component, new()
        {
            var c = new T { Entity = this };
            var self = this;
            if (!HasComponent(c.NativeName)) Native.WithUtf8(c.NativeName, p => Native.Api.AddComponent(self.Scene, self.Id, (byte*)p));
            return c;
        }

        public void Destroy() { if (Scene != IntPtr.Zero) Native.Api.Destroy(Scene, Id); }

        /// <summary>Finds an entity by name in the scene of the currently running script.</summary>
        public static Entity Find(string name)
        {
            IntPtr scene = Native.CurrentScene;
            ulong id = Native.WithUtf8<ulong>(name, p => Native.Api.FindEntity(scene, (byte*)p));
            return id == ulong.MaxValue ? Null : new Entity(scene, id);
        }

        /// <summary>Instantiates a prefab asset (by GUID, as shown in the Inspector) at a world position.</summary>
        public static Entity Instantiate(ulong prefabGuid, Vector3 position)
        {
            IntPtr scene = Native.CurrentScene;
            ulong id = Native.Api.Instantiate(scene, prefabGuid, (float*)&position);
            return id == ulong.MaxValue ? Null : new Entity(scene, id);
        }

        public bool Equals(Entity other) => Scene == other.Scene && Id == other.Id;
        public override bool Equals(object obj) => obj is Entity e && Equals(e);
        public override int GetHashCode() => HashCode.Combine(Scene, Id);
        public static bool operator ==(Entity a, Entity b) => a.Equals(b);
        public static bool operator !=(Entity a, Entity b) => !a.Equals(b);
        public override string ToString() => IsValid ? $"Entity({Name})" : "Entity(null)";
    }

    public static unsafe class Debug
    {
        private static void Write(int level, string message) =>
            Native.WithUtf8(message ?? "null", p => Native.Api.Log(level, (byte*)p));
        public static void Log(object message) => Write(1, message?.ToString());
        public static void LogWarning(object message) => Write(2, message?.ToString());
        public static void LogError(object message) => Write(3, message?.ToString());
    }

    public static unsafe class Time
    {
        /// <summary>Seconds since the previous frame.</summary>
        public static float DeltaTime => Native.Api.GetDeltaTime();
        /// <summary>Seconds since play mode started (stops while paused).</summary>
        public static float time => Native.Api.GetTime(Native.CurrentScene);
        /// <summary>Frames simulated since play mode started.</summary>
        public static ulong FrameCount => Native.Api.GetFrameCount(Native.CurrentScene);
    }

    /// <summary>Key codes (same values as GLFW / the engine's Key enum).</summary>
    public enum KeyCode
    {
        Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
        Alpha0 = 48, Alpha1, Alpha2, Alpha3, Alpha4, Alpha5, Alpha6, Alpha7, Alpha8, Alpha9,
        A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        Escape = 256, Enter, Tab, Backspace, Insert, Delete, RightArrow, LeftArrow, DownArrow, UpArrow,
        F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        LeftShift = 340, LeftControl, LeftAlt, LeftSuper, RightShift, RightControl, RightAlt, RightSuper,
    }

    public static unsafe class Input
    {
        public static bool GetKey(KeyCode key) => Native.Api.IsKeyDown((int)key) != 0;
        public static bool GetKeyDown(KeyCode key) => Native.Api.IsKeyPressed((int)key) != 0;
        /// <summary>0 = left, 1 = right, 2 = middle.</summary>
        public static bool GetMouseButton(int button) => Native.Api.IsMouseDown(button) != 0;
        public static Vector2 MousePosition
        {
            get { Vector2 v; Native.Api.GetMousePosition((float*)&v); return v; }
        }
    }

    public struct RaycastHit
    {
        public Entity Entity;
        public Vector3 Point;
        public Vector3 Normal;
        public float Distance;
    }

    public static unsafe class Physics
    {
        /// <summary>Layer mask with every layer except "Ignore Raycast" (the default for queries).</summary>
        public const uint DefaultLayers = ~(1u << 1);

        public static bool Raycast(Vector3 origin, Vector3 direction, out RaycastHit hit, float maxDistance = 1000.0f, uint layerMask = DefaultLayers)
        {
            ulong id;
            Vector3 point, normal;
            float distance;
            int ok = Native.Api.Raycast3DMask(Native.CurrentScene, (float*)&origin, (float*)&direction, maxDistance, layerMask, &id,
                                              (float*)&point, (float*)&normal, &distance);
            hit = new RaycastHit { Entity = ok != 0 ? new Entity(Native.CurrentScene, id) : Entity.Null, Point = point, Normal = normal, Distance = distance };
            return ok != 0;
        }

        /// <summary>Entities whose 3D colliders overlap the sphere.</summary>
        public static Entity[] OverlapSphere(Vector3 center, float radius, uint layerMask = DefaultLayers)
        {
            const int max = 256;
            ulong* ids = stackalloc ulong[max];
            int count = Math.Min(Native.Api.OverlapSphere(Native.CurrentScene, (float*)&center, radius, layerMask, ids, max), max);
            var result = new Entity[count];
            for (int i = 0; i < count; ++i) result[i] = new Entity(Native.CurrentScene, ids[i]);
            return result;
        }
    }

    /// <summary>Runtime scene switching (scenes come from File > Build Settings). Loads happen at the end of the frame.</summary>
    public static unsafe class SceneManager
    {
        public static void LoadScene(string name) => Native.WithUtf8(name, p => Native.Api.LoadSceneByName((byte*)p));
        public static void LoadScene(int buildIndex) => Native.Api.LoadSceneByIndex(buildIndex);
        /// <summary>Name of the scene the calling script runs in.</summary>
        public static string ActiveSceneName => Native.FromUtf8(Native.Api.GetSceneName(Native.CurrentScene));
        public static int SceneCountInBuild => Native.Api.GetSceneCount();
    }
}
