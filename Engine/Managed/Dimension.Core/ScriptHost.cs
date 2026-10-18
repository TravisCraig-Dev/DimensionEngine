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
                        w.WriteStartArray("fields");
                        foreach (FieldInfo f in type.GetFields(BindingFlags.Public | BindingFlags.Instance))
                        {
                            if (!s_Supported.Contains(f.FieldType) || f.IsInitOnly) continue;
                            cls.Fields.Add(f);
                            w.WriteStartObject();
                            w.WriteString("name", f.Name);
                            w.WriteString("type", TypeName(f.FieldType));
                            w.WritePropertyName("default");
                            WriteDefault(w, defaults != null ? f.GetValue(defaults) : null);
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
                    case 1: b.OnUpdate(dt); break;
                    case 2: b.OnFixedUpdate(dt); break;
                    case 3: b.OnDestroy(); break;
                    case 4: b.OnClick(); break;
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

        [UnmanagedCallersOnly]
        internal static void DestroyInstance(IntPtr handle)
        {
            try { GCHandle.FromIntPtr(handle).Free(); }
            catch (Exception e) { Debug.LogError($"Freeing C# script failed: {e.Message}"); }
        }
    }
}
