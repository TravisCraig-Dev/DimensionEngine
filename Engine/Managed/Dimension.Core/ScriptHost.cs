using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using Dimension.Interop;

namespace Dimension
{
    // Loads user script assemblies into a collectible context (so they can be hot-reloaded) and
    // dispatches engine callbacks to Behaviour instances. All entry points catch exceptions:
    // an exception must never cross back into native code.
    internal static unsafe class ScriptHost
    {
        private sealed class ScriptLoadContext : AssemblyLoadContext
        {
            public ScriptLoadContext() : base("DimensionScripts", isCollectible: true) { }

            protected override Assembly Load(AssemblyName name)
            {
                // Share the engine API assembly so Behaviour etc. have one type identity.
                if (name.Name == typeof(Behaviour).Assembly.GetName().Name) return typeof(Behaviour).Assembly;
                return null; // framework assemblies resolve from the default context
            }
        }

        private sealed class ScriptClass
        {
            public Type Type;
            public List<FieldInfo> Fields = new();
        }

        private static ScriptLoadContext s_Context;
        private static readonly Dictionary<string, ScriptClass> s_Classes = new();
        // Editor extensions by id: [MenuItem] methods and EditorWindow classes (with their instance once drawn).
        private sealed class EditorItem
        {
            public MethodInfo Method;
            public Type WindowType;
            public EditorWindow Window;
        }
        private static readonly List<EditorItem> s_EditorItems = new();

        private static void RegisterEditorItems(Assembly assembly)
        {
            foreach (Type type in assembly.GetTypes().OrderBy(t => t.FullName))
            {
                foreach (MethodInfo m in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.DeclaredOnly))
                    if (m.GetCustomAttribute<MenuItemAttribute>() is { } item && m.GetParameters().Length == 0)
                    {
                        s_EditorItems.Add(new EditorItem { Method = m });
                        RegisterEditorItem("menu", item.Path, null);
                    }
                if (!type.IsAbstract && type.IsSubclassOf(typeof(EditorWindow)))
                {
                    var window = type.GetCustomAttribute<EditorWindowAttribute>() ?? new EditorWindowAttribute(type.Name);
                    s_EditorItems.Add(new EditorItem { WindowType = type });
                    RegisterEditorItem("window", window.MenuPath, window.Title);
                }
            }
        }

        private static void RegisterEditorItem(string kind, string path, string title)
        {
            using var stream = new MemoryStream();
            using (var w = new Utf8JsonWriter(stream))
            {
                w.WriteStartObject();
                w.WriteString("kind", kind);
                w.WriteNumber("id", s_EditorItems.Count - 1);
                w.WriteString("path", path ?? "");
                if (title != null) w.WriteString("title", title);
                w.WriteEndObject();
            }
            byte[] json = stream.ToArray();
            var withNul = new byte[json.Length + 1];
            Array.Copy(json, withNul, json.Length);
            fixed (byte* q = withNul) Native.Api.RegisterEditorItem(q);
        }

        [UnmanagedCallersOnly]
        internal static void EditorCall(int kind, int id, IntPtr scene)
        {
            try
            {
                if (id < 0 || id >= s_EditorItems.Count) return;
                Native.CurrentScene = scene;
                EditorItem item = s_EditorItems[id];
                if (kind == 0 && item.Method != null) item.Method.Invoke(null, null);
                else if (kind == 1 && item.WindowType != null)
                {
                    item.Window ??= (EditorWindow)Activator.CreateInstance(item.WindowType);
                    item.Window.OnGUI();
                }
            }
            catch (Exception e) { Debug.LogError($"Editor extension failed: {(e as TargetInvocationException)?.InnerException ?? e}"); }
        }

        private static readonly Type[] s_Supported =
            { typeof(float), typeof(int), typeof(bool), typeof(string), typeof(Vector2), typeof(Vector3), typeof(Vector4), typeof(Color), typeof(Entity) };

        private static string TypeName(Type t) =>
            t == typeof(float) ? "Float" : t == typeof(int) ? "Int" : t == typeof(bool) ? "Bool" : t == typeof(string) ? "String" :
            t == typeof(Vector2) ? "Vec2" : t == typeof(Vector3) ? "Vec3" : t == typeof(Vector4) ? "Vec4" : t == typeof(Color) ? "Color4" : "Entity";

        private static void WriteDefault(Utf8JsonWriter w, object v)
        {
            switch (v)
            {
                case float f: w.WriteNumberValue(f); break;
                case int i: w.WriteNumberValue(i); break;
                case bool b: w.WriteBooleanValue(b); break;
                case string s: w.WriteStringValue(s); break;
                case Vector2 a: w.WriteStartArray(); w.WriteNumberValue(a.X); w.WriteNumberValue(a.Y); w.WriteEndArray(); break;
                case Vector3 a: w.WriteStartArray(); w.WriteNumberValue(a.X); w.WriteNumberValue(a.Y); w.WriteNumberValue(a.Z); w.WriteEndArray(); break;
                case Vector4 a: w.WriteStartArray(); w.WriteNumberValue(a.X); w.WriteNumberValue(a.Y); w.WriteNumberValue(a.Z); w.WriteNumberValue(a.W); w.WriteEndArray(); break;
                case Color c: w.WriteStartArray(); w.WriteNumberValue(c.R); w.WriteNumberValue(c.G); w.WriteNumberValue(c.B); w.WriteNumberValue(c.A); w.WriteEndArray(); break;
                default: w.WriteNullValue(); break;
            }
        }

        private static IEnumerable<FieldInfo> SerializedFields(Type type)
        {
            var chain = new List<Type>();
            for (Type t = type; t != null && t != typeof(Behaviour); t = t.BaseType) chain.Insert(0, t);
            foreach (Type t in chain)
                foreach (FieldInfo f in t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly))
                {
                    if (!s_Supported.Contains(f.FieldType) || f.IsInitOnly || f.IsNotSerialized) continue;
                    if (!f.IsPublic && f.GetCustomAttribute<SerializeFieldAttribute>() == null) continue;
                    yield return f;
                }
        }

        [UnmanagedCallersOnly]
        internal static int LoadScripts(byte* pathUtf8)
        {
            try
            {
                UnloadInternal();
                string path = Native.FromUtf8(pathUtf8);
                if (!File.Exists(path)) return 0;
                s_Context = new ScriptLoadContext();
                // Load from memory so the compiler can overwrite the file while we run.
                string pdb = Path.ChangeExtension(path, ".pdb");
                using var dll = new MemoryStream(File.ReadAllBytes(path));
                using var symbols = File.Exists(pdb) ? new MemoryStream(File.ReadAllBytes(pdb)) : null;
                Assembly assembly = s_Context.LoadFromStream(dll, symbols);

                foreach (Type type in assembly.GetTypes().Where(t => !t.IsAbstract && t.IsSubclassOf(typeof(Behaviour))).OrderBy(t => t.FullName))
                {
                    var cls = new ScriptClass { Type = type };
                    object defaults = null;
                    try { defaults = Activator.CreateInstance(type); } catch (Exception e) { Debug.LogError($"{type.FullName}: constructor failed: {e.Message}"); }

                    using var stream = new MemoryStream();
                    using (var w = new Utf8JsonWriter(stream))
                    {
                        w.WriteStartObject();
                        w.WriteString("name", type.FullName);
                        if (type.GetCustomAttribute<AddComponentMenuAttribute>() is { } menu) w.WriteString("menu", menu.Path);
                        w.WriteStartArray("requires");
                        foreach (var require in type.GetCustomAttributes<RequireComponentAttribute>(true))
                            foreach (Type t in require.Types)
                                if (t != null && t.IsSubclassOf(typeof(Component)) && !t.IsAbstract && Activator.CreateInstance(t) is Component c)
                                    w.WriteStringValue(c.NativeName);
                        w.WriteEndArray();
                        w.WriteStartArray("fields");
                        // Public fields (unless [NonSerialized]) and private ones marked [SerializeField], base class first.
                        foreach (FieldInfo f in SerializedFields(type))
                        {
                            cls.Fields.Add(f);
                            w.WriteStartObject();
                            w.WriteString("name", f.Name);
                            w.WriteString("type", TypeName(f.FieldType));
                            w.WritePropertyName("default");
                            WriteDefault(w, defaults != null ? f.GetValue(defaults) : null);
                            if (f.GetCustomAttribute<RangeAttribute>() is { } range) { w.WriteNumber("min", range.Min); w.WriteNumber("max", range.Max); }
                            if (f.GetCustomAttribute<TooltipAttribute>() is { } tip) w.WriteString("tooltip", tip.Text);
                            if (f.GetCustomAttribute<HeaderAttribute>() is { } header) w.WriteString("header", header.Text);
                            if (f.GetCustomAttribute<HideInInspectorAttribute>() != null) w.WriteBoolean("hidden", true);
                            w.WriteEndObject();
                        }
                        w.WriteEndArray();
                        w.WriteEndObject();
                    }
                    s_Classes[type.FullName] = cls;
                    byte[] json = stream.ToArray();
                    var withNul = new byte[json.Length + 1]; // native side expects a C string
                    Array.Copy(json, withNul, json.Length);
                    fixed (byte* q = withNul) Native.Api.RegisterScriptClass(q);
                }
                RegisterEditorItems(assembly);
                return s_Classes.Count;
            }
            catch (Exception e)
            {
                Debug.LogError($"Failed to load C# scripts: {e}");
                return 0;
            }
        }

        // Separate, non-inlined frame: otherwise (especially in Debug builds) the JIT can keep the
        // context alive in a stack slot of the caller and the collectible assembly never unloads.
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        private static WeakReference BeginUnload()
        {
            s_Classes.Clear();
            s_EditorItems.Clear();
            EngineEvents.Clear(); // listeners may be methods of the scripts being unloaded
            if (s_Context == null) return null;
            var weak = new WeakReference(s_Context);
            s_Context.Unload();
            s_Context = null;
            return weak;
        }

        private static void UnloadInternal()
        {
            WeakReference weak = BeginUnload();
            if (weak == null) return;
            for (int i = 0; i < 10 && weak.IsAlive; ++i)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
            if (weak.IsAlive) Debug.LogWarning("Old C# script assembly is still referenced and could not be fully unloaded");
        }

        [UnmanagedCallersOnly]
        internal static void UnloadScripts()
        {
            try { UnloadInternal(); }
            catch (Exception e) { Debug.LogError($"Unloading C# scripts failed: {e}"); }
        }

        [UnmanagedCallersOnly]
        internal static IntPtr CreateInstance(byte* classNameUtf8, IntPtr scene, ulong entity)
        {
            try
            {
                string name = Native.FromUtf8(classNameUtf8);
                if (!s_Classes.TryGetValue(name, out var cls)) return IntPtr.Zero;
                var behaviour = (Behaviour)Activator.CreateInstance(cls.Type);
                behaviour.Entity = new Entity(scene, entity);
                return GCHandle.ToIntPtr(GCHandle.Alloc(behaviour));
            }
            catch (Exception e)
            {
                Debug.LogError($"Creating C# script failed: {e}");
                return IntPtr.Zero;
            }
        }

        [UnmanagedCallersOnly]
        internal static void SetField(IntPtr handle, int index, void* value)
        {
            try
            {
                var b = (Behaviour)GCHandle.FromIntPtr(handle).Target;
                var cls = s_Classes[b.GetType().FullName];
                FieldInfo f = cls.Fields[index];
                Type t = f.FieldType;
                object v =
                    t == typeof(float) ? *(float*)value :
                    t == typeof(int) ? *(int*)value :
                    t == typeof(bool) ? *(byte*)value != 0 :
                    t == typeof(string) ? Native.FromUtf8((byte*)value) :
                    t == typeof(Vector2) ? *(Vector2*)value :
                    t == typeof(Vector3) ? *(Vector3*)value :
                    t == typeof(Vector4) ? *(Vector4*)value :
                    t == typeof(Color) ? *(Color*)value :
                    (object)new Entity(b.Entity.Scene, *(ulong*)value);
                f.SetValue(b, v);
            }
            catch (Exception e) { Debug.LogError($"Setting C# field failed: {e.Message}"); }
        }

        // 0 Create, 1 Update, 2 FixedUpdate, 3 Destroy, 4 Click
        [UnmanagedCallersOnly]
        internal static void Invoke(IntPtr handle, int method, float dt)
        {
            Behaviour b = null;
            try
            {
                b = (Behaviour)GCHandle.FromIntPtr(handle).Target;
                Native.CurrentScene = b.Entity.Scene;
                switch (method)
                {
                    case 0: b.OnCreate(); break;
                    case 1: b.OnUpdate(dt); b.Coroutines?.Tick(dt); break;
                    case 2: b.OnFixedUpdate(dt); break;
                    case 3: b.OnDestroy(); b.StopAllCoroutines(); break;
                    case 4: b.OnClick(); break;
                    case 5: b.OnValueChanged(dt); break;
                    case 6: b.OnJointBreak(dt); break;
                    case 7: b.OnReloaded(); break;
                }
            }
            catch (Exception e) { Debug.LogError($"{b?.GetType().Name}: {e}"); }
        }

        // 0 TextChanged, 1 Submit
        [UnmanagedCallersOnly]
        internal static unsafe void InvokeString(IntPtr handle, int method, byte* text)
        {
            Behaviour b = null;
            try
            {
                b = (Behaviour)GCHandle.FromIntPtr(handle).Target;
                Native.CurrentScene = b.Entity.Scene;
                string s = Native.FromUtf8(text) ?? string.Empty;
                switch (method)
                {
                    case 0: b.OnTextChanged(s); break;
                    case 1: b.OnSubmit(s); break;
                    case 2: b.OnAnimationEvent(s); break;
                }
            }
            catch (Exception e) { Debug.LogError($"{b?.GetType().Name}: {e}"); }
        }

        // 0 CollisionEnter, 1 CollisionExit, 2 TriggerEnter, 3 TriggerExit
        [UnmanagedCallersOnly]
        internal static void InvokeEntity(IntPtr handle, int method, ulong other)
        {
            Behaviour b = null;
            try
            {
                b = (Behaviour)GCHandle.FromIntPtr(handle).Target;
                Native.CurrentScene = b.Entity.Scene;
                var o = new Entity(b.Entity.Scene, other);
                switch (method)
                {
                    case 0: b.OnCollisionEnter(o); break;
                    case 1: b.OnCollisionExit(o); break;
                    case 2: b.OnTriggerEnter(o); break;
                    case 3: b.OnTriggerExit(o); break;
                }
            }
            catch (Exception e) { Debug.LogError($"{b?.GetType().Name}: {e}"); }
        }

        private static IntPtr s_FieldsBuffer; // the last GetFields result (native reads it before the next call)

        // Current values of a script's serialized fields, in registration order (entities as packed ids).
        [UnmanagedCallersOnly]
        internal static byte* GetFields(IntPtr handle)
        {
            try
            {
                var b = (Behaviour)GCHandle.FromIntPtr(handle).Target;
                var cls = s_Classes[b.GetType().FullName];
                using var stream = new MemoryStream();
                using (var w = new Utf8JsonWriter(stream))
                {
                    w.WriteStartArray();
                    foreach (FieldInfo f in cls.Fields)
                    {
                        object v = f.GetValue(b);
                        if (v is Entity e) w.WriteNumberValue(e.Id);
                        else WriteDefault(w, v);
                    }
                    w.WriteEndArray();
                }
                byte[] json = stream.ToArray();
                if (s_FieldsBuffer != IntPtr.Zero) Marshal.FreeHGlobal(s_FieldsBuffer);
                s_FieldsBuffer = Marshal.AllocHGlobal(json.Length + 1);
                Marshal.Copy(json, 0, s_FieldsBuffer, json.Length);
                ((byte*)s_FieldsBuffer)[json.Length] = 0;
                return (byte*)s_FieldsBuffer;
            }
            catch (Exception e)
            {
                Debug.LogError($"Reading C# fields failed: {e.Message}");
                return null;
            }
        }

        // Debugger support for the engine: 0 asks whether a managed debugger is attached, 1 offers to attach one (the
        // system's just-in-time debugger prompt), 2 breaks into an attached debugger.
        [UnmanagedCallersOnly]
        internal static int DebuggerControl(int action)
        {
            try
            {
                switch (action)
                {
                    case 0: return System.Diagnostics.Debugger.IsAttached ? 1 : 0;
                    case 1: return System.Diagnostics.Debugger.Launch() ? 1 : 0;
                    case 2: if (System.Diagnostics.Debugger.IsAttached) System.Diagnostics.Debugger.Break(); return 1;
                }
            }
            catch (Exception e) { Debug.LogError($"Debugger request failed: {e.Message}"); }
            return 0;
        }

        [UnmanagedCallersOnly]
        internal static void RaiseEvent(IntPtr scene, ulong entity, int kind, float value, ulong other, byte* text)
        {
            try
            {
                if (kind < 0) EngineEvents.ClearScene(scene);
                else EngineEvents.Raise(scene, entity, (EngineEvents.Kind)kind, value, other, text != null ? Native.FromUtf8(text) : null);
            }
            catch (Exception e) { Debug.LogError($"Engine event failed: {e}"); }
        }

        [UnmanagedCallersOnly]
        internal static void DestroyInstance(IntPtr handle)
        {
            try { GCHandle.FromIntPtr(handle).Free(); }
            catch (Exception e) { Debug.LogError($"Freeing C# script failed: {e.Message}"); }
        }
    }
}
