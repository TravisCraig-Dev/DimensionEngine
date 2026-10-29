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
        /// <summary>Called when a UI Slider (its value), UI Toggle (1 = on, 0 = off) or UI Dropdown (option index) on this entity changes.</summary>
        public virtual void OnValueChanged(float value) { }
        /// <summary>Called when a Joint or Joint 2D on this entity breaks (its Break Force/Torque was exceeded).</summary>
        public virtual void OnJointBreak(float force) { }
        /// <summary>Called when the text of a UI Input Field on this entity is edited.</summary>
        public virtual void OnTextChanged(string text) { }
        /// <summary>Called when Enter is pressed in a UI Input Field on this entity.</summary>
        public virtual void OnSubmit(string text) { }
        /// <summary>Called when an Animator state event on this entity is reached (the event's function name).</summary>
        public virtual void OnAnimationEvent(string name) { }
        /// <summary>Called instead of OnCreate when the scripts were recompiled and reloaded while playing. Serialized
        /// fields (public and [SerializeField]) keep their values; other state, coroutines and event listeners start over.</summary>
        public virtual void OnReloaded() { }
        /// <summary>Called in the editor to draw the script's gizmos (Gizmos.DrawLine, ...) in the Scene view: while
        /// playing on the live script, otherwise on one holding its saved values (OnCreate has not run).</summary>
        public virtual void OnDrawGizmos() { }
        /// <summary>As OnDrawGizmos, for scripts of the selected objects only.</summary>
        public virtual void OnDrawGizmosSelected() { }

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
        public Component GetComponent(string componentName) => Entity.GetComponent(componentName);
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

        /// <summary>Any component by its Inspector name (null if the entity has none), for its numeric fields.</summary>
        public Component GetComponent(string componentName) => HasComponent(componentName) ? new NamedComponent(componentName) { Entity = this } : null;

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

        /// <summary>Raised when this entity's collider starts touching another (any script can listen, not only this entity's).</summary>
        public event Action<Entity> CollisionEnter
        {
            add => EngineEvents.Add(this, EngineEvents.Kind.CollisionEnter, value);
            remove => EngineEvents.Remove(this, EngineEvents.Kind.CollisionEnter, value);
        }
        public event Action<Entity> CollisionExit
        {
            add => EngineEvents.Add(this, EngineEvents.Kind.CollisionExit, value);
            remove => EngineEvents.Remove(this, EngineEvents.Kind.CollisionExit, value);
        }
        /// <summary>Raised when this entity enters or touches a trigger, or something enters this trigger.</summary>
        public event Action<Entity> TriggerEnter
        {
            add => EngineEvents.Add(this, EngineEvents.Kind.TriggerEnter, value);
            remove => EngineEvents.Remove(this, EngineEvents.Kind.TriggerEnter, value);
        }
        public event Action<Entity> TriggerExit
        {
            add => EngineEvents.Add(this, EngineEvents.Kind.TriggerExit, value);
            remove => EngineEvents.Remove(this, EngineEvents.Kind.TriggerExit, value);
        }

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

        /// <summary>
        /// A line from <paramref name="start"/> to <paramref name="end"/> in the editor's Scene and Game views (with the
        /// gizmos; not in builds), until the next update, or for <paramref name="duration"/> seconds of game time.
        /// </summary>
        public static void DrawLine(System.Numerics.Vector3 start, System.Numerics.Vector3 end, Color? color = null, float duration = 0.0f)
        {
            Color c = color ?? Color.White;
            float* data = stackalloc float[11] { start.X, start.Y, start.Z, end.X, end.Y, end.Z, c.R, c.G, c.B, c.A, duration };
            Native.Api.DebugLine(data);
        }

        /// <summary>A line from <paramref name="start"/> along <paramref name="direction"/> (its length the line's); see DrawLine.</summary>
        public static void DrawRay(System.Numerics.Vector3 start, System.Numerics.Vector3 direction, Color? color = null, float duration = 0.0f) =>
            DrawLine(start, start + direction, color, duration);
    }

    /// <summary>
    /// The engine's Profiler (Window > Profiler) from scripts: samples show as sections (nested in the section that
    /// runs the script, such as Scripts), with their time and calls, in its tables and timeline; counters show beside the
    /// frame's draw calls.
    /// </summary>
    /// <example>
    /// <code>
    /// using (Profiler.Sample("Pathfinding")) { FindPath(); }
    /// Profiler.BeginSample("Spawn"); Spawn(); Profiler.EndSample();
    /// Profiler.SetCounter("Enemies", enemies.Count);
    /// </code>
    /// </example>
    public static unsafe class Profiler
    {
        /// <summary>Starts a section named <paramref name="name"/> (until <see cref="EndSample"/>; nest them freely).</summary>
        public static void BeginSample(string name) => Native.WithUtf8(name ?? "Sample", p => Native.Api.ProfilerSample(0, (byte*)p, 0.0f));

        /// <summary>Ends the innermost section begun with <see cref="BeginSample"/>. One left open ends with the frame.</summary>
        public static void EndSample() => Native.Api.ProfilerSample(1, null, 0.0f);

        /// <summary>A section for a <c>using</c> block: begun now, ended when the block is left (even by an exception).</summary>
        public static SampleScope Sample(string name)
        {
            BeginSample(name);
            return new SampleScope();
        }

        /// <summary>Sets a counter shown with this frame in the Profiler (its last value in the frame).</summary>
        public static void SetCounter(string name, float value) => Native.WithUtf8(name ?? "Counter", p => Native.Api.ProfilerSample(2, (byte*)p, value));

        public readonly struct SampleScope : System.IDisposable
        {
            public void Dispose() => EndSample();
        }
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
        private static float Action(int op, string name) => Native.WithUtf8<float>(name ?? "", p => Native.Api.InputAction(op, (byte*)p));

        /// <summary>
        /// Whether an input action (Project Settings > Input, e.g. "Jump", "Fire") is held: any of its keys, mouse
        /// buttons or gamepad controls. Unknown names are not pressed (with a warning in the Console).
        /// </summary>
        public static bool GetButton(string action) => Action(0, action) != 0.0f;
        /// <summary>The action was pressed this frame.</summary>
        public static bool GetButtonDown(string action) => Action(1, action) != 0.0f;
        /// <summary>The action was released this frame.</summary>
        public static bool GetButtonUp(string action) => Action(2, action) != 0.0f;
        /// <summary>An axis action's value, -1..1 (e.g. "Horizontal": A/D, arrows and the left stick added up).</summary>
        public static float GetAxis(string action) => Action(3, action);

        public static bool GetKey(KeyCode key) => Native.Api.IsKeyDown((int)key) != 0;
        public static bool GetKeyDown(KeyCode key) => Native.Api.IsKeyPressed((int)key) != 0;
        /// <summary>0 = left, 1 = right, 2 = middle.</summary>
        public static bool GetMouseButton(int button) => Native.Api.IsMouseDown(button) != 0;
        public static Vector2 MousePosition
        {
            get { Vector2 v; Native.Api.GetMousePosition((float*)&v); return v; }
        }

        /// <summary>Gamepad button held (standard Xbox-style layout; pad 0-3).</summary>
        public static bool GetGamepadButton(GamepadButton button, int pad = 0) => Native.Api.GamepadButton((int)button, pad, 0) != 0;
        /// <summary>Gamepad button went down this frame.</summary>
        public static bool GetGamepadButtonDown(GamepadButton button, int pad = 0) => Native.Api.GamepadButton((int)button, pad, 1) != 0;
        /// <summary>Sticks -1..1 (+Y is down) with a dead zone; triggers 0..1.</summary>
        public static float GetGamepadAxis(GamepadAxis axis, int pad = 0) => Native.Api.GamepadAxis((int)axis, pad);
        /// <summary>Rumble: low-frequency (heavy) and high-frequency (light) motor speeds 0..1. A positive duration
        /// stops it after that many seconds; 0 keeps it until changed. Stops when play stops.</summary>
        public static void SetGamepadVibration(float lowFrequency, float highFrequency, float duration = 0.0f, int pad = 0) =>
            Native.Api.GamepadVibrate(lowFrequency, highFrequency, duration, pad);
        /// <summary>The left stick, or WASD/arrows when no stick is pushed (+Y up).</summary>
        public static Vector2 GetMoveInput(int pad = 0)
        {
            var stick = new Vector2(GetGamepadAxis(GamepadAxis.LeftX, pad), -GetGamepadAxis(GamepadAxis.LeftY, pad));
            if (stick.LengthSquared() > 0.0f) return stick;
            float x = (GetKey(KeyCode.D) || GetKey(KeyCode.RightArrow) ? 1 : 0) - (GetKey(KeyCode.A) || GetKey(KeyCode.LeftArrow) ? 1 : 0);
            float y = (GetKey(KeyCode.W) || GetKey(KeyCode.UpArrow) ? 1 : 0) - (GetKey(KeyCode.S) || GetKey(KeyCode.DownArrow) ? 1 : 0);
            return new Vector2(x, y);
        }
    }

    public enum GamepadButton
    {
        A = 0, B, X, Y, LeftBumper, RightBumper, Back, Start, Guide, LeftThumb, RightThumb, DpadUp, DpadRight, DpadDown, DpadLeft
    }

    public enum GamepadAxis { LeftX = 0, LeftY, RightX, RightY, LeftTrigger, RightTrigger }

    public struct RaycastHit
    {
        public Entity Entity;
        public Vector3 Point;
        public Vector3 Normal;
        public float Distance;
    }

    /// <summary>Changing terrains while playing.</summary>
    public static unsafe class TerrainTools
    {
        /// <summary>Digs a crater (an explosion's) into the terrain under `point`: a bowl `radius` across and `depth`
        /// deep, with a rim of thrown-up earth, painted with `paintLayer` (3 = dirt; -1 = none) and cleared of trees and
        /// grass. The terrain redraws and collides with its new shape. Returns how many height samples changed.</summary>
        public static int DigCrater(Vector3 point, float radius, float depth, int paintLayer = 3) =>
            Native.Api.TerrainCrater(Native.CurrentScene, (float*)&point, radius, depth, paintLayer);
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
    public static unsafe class Application
    {
        /// <summary>Whether the running script's scene is playing (false for [ExecuteInEditMode] scripts in the editor,
        /// and in editor code).</summary>
        public static bool IsPlaying => Native.CurrentScene != IntPtr.Zero && Native.Api.SceneRunning(Native.CurrentScene) != 0;

        /// <summary>
        /// The folder for the player's data (save games, settings): <c>%APPDATA%\&lt;product name&gt;</c> in a build, the
        /// project's <c>.dimension\PlayerData</c> in the editor (so trying the game never touches a build's saves).
        /// </summary>
        public static string PersistentDataPath => Native.FromUtf8(Native.Api.PlayerPrefs(10, null, null)) ?? "";
    }

    /// <summary>
    /// Small settings kept between runs (like Unity's PlayerPrefs): ints, floats and strings by key, saved in
    /// <see cref="Application.PersistentDataPath"/> whenever one changes. A key of another type, or not set, reads as
    /// the default.
    /// </summary>
    public static unsafe class PlayerPrefs
    {
        private static string Call(int op, string key, string value) =>
            Native.WithUtf8<string>(key ?? "", k => Native.WithUtf8<string>(value ?? "", v => Native.FromUtf8(Native.Api.PlayerPrefs(op, (byte*)k, (byte*)v))));

        private static readonly System.Globalization.CultureInfo Invariant = System.Globalization.CultureInfo.InvariantCulture;

        public static int GetInt(string key, int defaultValue = 0) =>
            Call(0, key, null) is { } s && int.TryParse(s, System.Globalization.NumberStyles.Integer, Invariant, out int v) ? v : defaultValue;
        public static float GetFloat(string key, float defaultValue = 0.0f) =>
            Call(1, key, null) is { } s && float.TryParse(s, System.Globalization.NumberStyles.Float, Invariant, out float v) ? v : defaultValue;
        public static string GetString(string key, string defaultValue = "") => Call(2, key, null) ?? defaultValue;
        public static void SetInt(string key, int value) => Call(3, key, value.ToString(Invariant));
        public static void SetFloat(string key, float value) => Call(4, key, value.ToString("R", Invariant));
        public static void SetString(string key, string value) => Call(5, key, value ?? "");
        public static bool HasKey(string key) => Call(6, key, null) != null;
        public static void DeleteKey(string key) => Call(7, key, null);
        public static void DeleteAll() => Call(8, null, null);
        /// <summary>Changes are written as they are made; this writes again.</summary>
        public static void Save() => Call(9, null, null);
    }

    /// <summary>
    /// Game state saved as JSON files in <see cref="Application.PersistentDataPath"/> (public fields and properties,
    /// nested objects, lists, dictionaries and vectors).
    /// </summary>
    /// <example>
    /// <code>
    /// SaveFile.Write("slot1", new SaveGame { Level = 3, Coins = 120 });
    /// SaveGame game = SaveFile.Read("slot1", new SaveGame());
    /// </code>
    /// </example>
    public static class SaveFile
    {
        private static readonly System.Text.Json.JsonSerializerOptions Options = new() { IncludeFields = true, WriteIndented = true };

        /// <summary>Where a save named <paramref name="name"/> is (<c>name.json</c> in the persistent data folder).</summary>
        public static string PathOf(string name) => System.IO.Path.Combine(Application.PersistentDataPath, name + ".json");

        /// <summary>Writes <paramref name="value"/> (replacing that save whole, so a crash keeps the old one).</summary>
        public static void Write<T>(string name, T value)
        {
            string path = PathOf(name), temp = path + ".tmp";
            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path)!);
            System.IO.File.WriteAllText(temp, System.Text.Json.JsonSerializer.Serialize(value, Options));
            System.IO.File.Move(temp, path, true);
        }

        /// <summary>The save, or <paramref name="fallback"/> when there is none or it cannot be read (a warning logged).</summary>
        public static T Read<T>(string name, T fallback = default)
        {
            string path = PathOf(name);
            if (!System.IO.File.Exists(path)) return fallback;
            try { return System.Text.Json.JsonSerializer.Deserialize<T>(System.IO.File.ReadAllText(path), Options) ?? fallback; }
            catch (Exception e)
            {
                Debug.LogWarning($"Save '{name}' could not be read: {e.Message}");
                return fallback;
            }
        }

        public static bool Exists(string name) => System.IO.File.Exists(PathOf(name));
        public static void Delete(string name) { if (Exists(name)) System.IO.File.Delete(PathOf(name)); }
    }

    public enum LoadSceneMode { Single = 0, Additive = 1 }

    /// <summary>A background scene load (SceneManager.LoadSceneAsync).</summary>
    public sealed unsafe class AsyncOperation
    {
        private readonly int m_Id;
        internal AsyncOperation(int id) { m_Id = id; }
        /// <summary>0 to 0.9 while loading in the background; 1 once the scene is active.</summary>
        public float Progress { get { float p; Native.Api.AsyncState(m_Id, &p); return p; } }
        public bool IsDone { get { float p; return Native.Api.AsyncState(m_Id, &p) != 0; } }
        /// <summary>False holds the loaded scene at 0.9 until set back to true.</summary>
        public bool AllowSceneActivation { set => Native.Api.AsyncAllowActivation(m_Id, value ? 1 : 0); }
    }

    public static unsafe class SceneManager
    {
        public static void LoadScene(string name) => Native.WithUtf8(name, p => Native.Api.LoadSceneByName((byte*)p));
        public static void LoadScene(int buildIndex) => Native.Api.LoadSceneByIndex(buildIndex);
        /// <summary>Single destroys everything not marked DontDestroyOnLoad; Additive adds the scene's objects.</summary>
        public static void LoadScene(string name, LoadSceneMode mode) =>
            Native.WithUtf8(name, p => Native.Api.LoadSceneWithMode((byte*)p, -1, (int)mode));
        public static void LoadScene(int buildIndex, LoadSceneMode mode) => Native.Api.LoadSceneWithMode(null, buildIndex, (int)mode);
        /// <summary>Loads in the background (a worker thread reads the scene and decodes its textures and models) and
        /// activates at the end of a frame once ready. Null if there is no such scene. Yield it in a coroutine with
        /// `yield return new WaitUntil(() => op.IsDone)`.</summary>
        public static AsyncOperation LoadSceneAsync(string name, LoadSceneMode mode = LoadSceneMode.Single)
        {
            int id = Native.WithUtf8(name, p => Native.Api.LoadSceneAsync((byte*)p, -1, (int)mode));
            return id != 0 ? new AsyncOperation(id) : null;
        }
        public static AsyncOperation LoadSceneAsync(int buildIndex, LoadSceneMode mode = LoadSceneMode.Single)
        {
            int id = Native.Api.LoadSceneAsync(null, buildIndex, (int)mode);
            return id != 0 ? new AsyncOperation(id) : null;
        }
        /// <summary>Removes the objects of a scene loaded additively (or the first scene).</summary>
        public static void UnloadScene(string name) => Native.WithUtf8(name, p => Native.Api.UnloadScene((byte*)p));
        /// <summary>Keeps the entity's root object (and its children) alive across single-mode loads.</summary>
        public static void DontDestroyOnLoad(Entity entity) => Native.Api.DontDestroyOnLoad(entity.Scene, entity.Id);
        /// <summary>Names of the loaded scenes (the active one first; "DontDestroyOnLoad" holds persistent objects).</summary>
        public static string[] LoadedSceneNames
        {
            get
            {
                IntPtr scene = Native.CurrentScene;
                int n = Native.Api.GetLoadedSceneCount(scene);
                var names = new string[n];
                for (int i = 0; i < n; i++) names[i] = Native.FromUtf8(Native.Api.GetLoadedSceneName(scene, i));
                return names;
            }
        }
        /// <summary>Name of the scene the calling script runs in.</summary>
        public static string ActiveSceneName => Native.FromUtf8(Native.Api.GetSceneName(Native.CurrentScene));
        public static int SceneCountInBuild => Native.Api.GetSceneCount();
    }
}
