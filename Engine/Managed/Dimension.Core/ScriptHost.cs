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
        // Editor extensions by id: [MenuItem] methods, EditorWindow classes and Inspector classes (with their instance
        // once drawn).
        private sealed class EditorItem
        {
            public MethodInfo Method;
            public Type WindowType;
            public EditorWindow Window;
            public Type InspectorType;
            public Inspector Inspector;
        }
        private static readonly List<EditorItem> s_EditorItems = new();
        // Property drawers by the attribute or field type they draw.
        private static readonly Dictionary<Type, PropertyDrawer> s_Drawers = new();

        private static void RegisterDrawers(Assembly assembly)
        {
            foreach (Type type in assembly.GetTypes().Where(t => !t.IsAbstract && t.IsSubclassOf(typeof(PropertyDrawer))).OrderBy(t => t.FullName))
                if (type.GetCustomAttribute<CustomPropertyDrawerAttribute>() is { Target: not null } target)
                    s_Drawers[target.Target] = (PropertyDrawer)Activator.CreateInstance(type);
        }

        // The drawer for a field: its first attribute with one, else its type's (null: none).
        private static PropertyDrawer DrawerFor(FieldInfo f)
        {
            foreach (Attribute a in f.GetCustomAttributes())
                if (s_Drawers.TryGetValue(a.GetType(), out var byAttribute))
                {
                    byAttribute.Field = f;
                    byAttribute.Attribute = a;
                    return byAttribute;
                }
            if (!s_Drawers.TryGetValue(f.FieldType, out var byType)) return null;
            byType.Field = f;
            byType.Attribute = null;
            return byType;
        }

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
                if (!type.IsAbstract && type.IsSubclassOf(typeof(Inspector)) && type.GetCustomAttribute<CustomInspectorAttribute>() is { } inspector)
                {
                    s_EditorItems.Add(new EditorItem { InspectorType = type });
                    RegisterEditorItem("inspector", "", null, inspector.Target);
                }
            }
        }

        private static void RegisterEditorItem(string kind, string path, string title, string inspects = null)
        {
            using var stream = new MemoryStream();
            using (var w = new Utf8JsonWriter(stream))
            {
                w.WriteStartObject();
                w.WriteString("kind", kind);
                w.WriteNumber("id", s_EditorItems.Count - 1);
                w.WriteString("path", path ?? "");
                if (title != null) w.WriteString("title", title);
                if (inspects != null) w.WriteString("inspects", inspects);
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

        // Draws a custom Inspector: for a script, its live instance (`handle`) or one made from its saved values
        // (`fieldsUtf8`, as GetFields writes them); returns the values after drawing (null for a built-in component).
        [UnmanagedCallersOnly]
        internal static byte* Inspect(int id, IntPtr handle, byte* classUtf8, byte* fieldsUtf8, IntPtr scene, ulong entity)
        {
            try
            {
                if (id < 0 || id >= s_EditorItems.Count || s_EditorItems[id].InspectorType == null) return null;
                Native.CurrentScene = scene;
                EditorItem item = s_EditorItems[id];
                item.Inspector ??= (Inspector)Activator.CreateInstance(item.InspectorType);
                Behaviour target = TargetFor(handle, classUtf8, fieldsUtf8, scene, entity);
                item.Inspector.Target = target;
                item.Inspector.Entity = new Entity(scene, entity);
                item.Inspector.OnInspectorGUI();
                return target != null ? FieldsJson(target) : null;
            }
            catch (Exception e)
            {
                Debug.LogError($"Custom Inspector failed: {(e as TargetInvocationException)?.InnerException ?? e}");
                return null;
            }
        }

        // Draws field `index` of a script with its property drawer (the script as Inspect finds it); returns the values
        // after drawing.
        [UnmanagedCallersOnly]
        internal static byte* DrawField(int index, IntPtr handle, byte* classUtf8, byte* fieldsUtf8, IntPtr scene, ulong entity)
        {
            try
            {
                Native.CurrentScene = scene;
                Behaviour target = TargetFor(handle, classUtf8, fieldsUtf8, scene, entity);
                if (target == null || !s_Classes.TryGetValue(target.GetType().FullName, out var cls) || index < 0 || index >= cls.Fields.Count) return null;
                FieldInfo f = cls.Fields[index];
                if (DrawerFor(f) is not { } drawer) return null;
                object value = drawer.OnGUI(f.Name, f.GetValue(target));
                if (value != null && value.GetType() == f.FieldType) f.SetValue(target, value);
                return FieldsJson(target);
            }
            catch (Exception e)
            {
                Debug.LogError($"Property drawer failed: {(e as TargetInvocationException)?.InnerException ?? e}");
                return null;
            }
        }

        // The live script (`handle`), or one made from its class and saved values.
        private static Behaviour TargetFor(IntPtr handle, byte* classUtf8, byte* fieldsUtf8, IntPtr scene, ulong entity)
        {
            if (handle != IntPtr.Zero) return (Behaviour)GCHandle.FromIntPtr(handle).Target;
            if (classUtf8 == null || !s_Classes.TryGetValue(Native.FromUtf8(classUtf8), out var cls)) return null;
            var target = (Behaviour)Activator.CreateInstance(cls.Type);
            target.Entity = new Entity(scene, entity);
            if (fieldsUtf8 != null) ApplyFields(target, cls, Native.FromUtf8(fieldsUtf8));
            return target;
        }

        // Field values (a JSON array in registration order, entities as packed ids) into a script.
        private static void ApplyFields(Behaviour b, ScriptClass cls, string json)
        {
            using var doc = JsonDocument.Parse(json);
            var values = doc.RootElement;
            for (int i = 0; i < cls.Fields.Count && i < values.GetArrayLength(); ++i)
            {
                FieldInfo f = cls.Fields[i];
                if (values[i].ValueKind == JsonValueKind.Null && f.FieldType != typeof(Entity)) continue;
                f.SetValue(b, ReadValue(f.FieldType, values[i], b.Entity.Scene));
            }
        }

        private static object ReadValue(Type t, string json, IntPtr scene)
        {
            using var doc = JsonDocument.Parse(json);
            return ReadValue(t, doc.RootElement, scene);
        }

        // A field's value from what the engine writes (see WriteValue).
        private static object ReadValue(Type t, JsonElement v, IntPtr scene)
        {
            static float F(JsonElement e, int i) => e.ValueKind == JsonValueKind.Array && e.GetArrayLength() > i ? e[i].GetSingle() : 0.0f;
            if (ElementType(t) is { } element)
            {
                var items = new List<object>();
                if (v.ValueKind == JsonValueKind.Array)
                    foreach (JsonElement item in v.EnumerateArray()) items.Add(ReadValue(element, item, scene));
                if (t.IsArray)
                {
                    Array array = Array.CreateInstance(element, items.Count);
                    for (int i = 0; i < items.Count; ++i) array.SetValue(items[i], i);
                    return array;
                }
                var list = (System.Collections.IList)Activator.CreateInstance(t);
                foreach (object item in items) list.Add(item);
                return list;
            }
            if (t == typeof(Entity)) return v.ValueKind == JsonValueKind.Number ? new Entity(scene, v.GetUInt64()) : Entity.Null;
            if (v.ValueKind == JsonValueKind.Null) return t == typeof(string) ? "" : Activator.CreateInstance(t);
            return
                t.IsEnum ? Enum.ToObject(t, v.GetInt32()) :
                t == typeof(float) ? v.GetSingle() :
                t == typeof(int) ? v.GetInt32() :
                t == typeof(bool) ? v.GetBoolean() :
                t == typeof(string) ? v.GetString() :
                t == typeof(Vector2) ? new Vector2(F(v, 0), F(v, 1)) :
                t == typeof(Vector3) ? new Vector3(F(v, 0), F(v, 1), F(v, 2)) :
                t == typeof(Vector4) ? new Vector4(F(v, 0), F(v, 1), F(v, 2), F(v, 3)) :
                t == typeof(Color) ? new Color(F(v, 0), F(v, 1), F(v, 2), F(v, 3)) :
                Activator.CreateInstance(t, v.GetUInt64()); // an asset reference
        }

        // A script's fields with the controls the Inspector uses for them (and their property drawers).
        internal static void DrawDefaultFields(Behaviour b)
        {
            if (b == null || !s_Classes.TryGetValue(b.GetType().FullName, out var cls)) return;
            foreach (FieldInfo f in cls.Fields)
            {
                if (f.GetCustomAttribute<HideInInspectorAttribute>() != null) continue;
                if (f.GetCustomAttribute<HeaderAttribute>() is { } header) EditorGUI.Separator(header.Text);
                object v = f.GetValue(b);
                var range = f.GetCustomAttribute<RangeAttribute>();
                object edited = DrawerFor(f) is { } drawer ? drawer.OnGUI(f.Name, v) : DrawValue(f.Name, v, f.FieldType, range);
                if (edited != null && edited.GetType() == f.FieldType && !Equals(edited, v)) f.SetValue(b, edited);
            }
        }
        // A value with the control for its type: a list as its size and then each element (a changed list comes back
        // as a new one).
        private static object DrawValue(string label, object v, Type t, RangeAttribute range)
        {
            if (ElementType(t) is { } element)
            {
                var items = new List<object>();
                if (v is System.Collections.IEnumerable current)
                    foreach (object item in current) items.Add(item);
                bool changed = false;
                int size = Math.Max(0, EditorGUI.IntField(label + " Size", items.Count));
                while (items.Count > size) { items.RemoveAt(items.Count - 1); changed = true; }
                while (items.Count < size) { items.Add(element == typeof(string) ? "" : Activator.CreateInstance(element)); changed = true; }
                for (int i = 0; i < items.Count; ++i)
                {
                    object item = DrawValue($"{label} {i}", items[i], element, range);
                    if (!Equals(item, items[i])) { items[i] = item; changed = true; }
                }
                if (!changed) return v;
                if (t.IsArray)
                {
                    Array array = Array.CreateInstance(element, items.Count);
                    for (int i = 0; i < items.Count; ++i) array.SetValue(items[i], i);
                    return array;
                }
                var list = (System.Collections.IList)Activator.CreateInstance(t);
                foreach (object item in items) list.Add(item);
                return list;
            }
            return v switch
            {
                float x when range != null => EditorGUI.Slider(label, x, range.Min, range.Max),
                float x => EditorGUI.FloatField(label, x),
                int x => EditorGUI.IntField(label, x),
                bool x => EditorGUI.Toggle(label, x),
                string x => EditorGUI.TextField(label, x),
                Vector2 x => ToVector2(EditorGUI.Vector3Field(label, new Vector3(x, 0.0f))),
                Vector3 x => EditorGUI.Vector3Field(label, x),
                Vector4 x => ToVector4(EditorGUI.ColorField(label, new Color(x.X, x.Y, x.Z, x.W))),
                Color x => EditorGUI.ColorField(label, x),
                Entity x => EditorGUI.EntityField(label, x),
                IAssetReference x => EditorGUI.AssetField(label, x),
                Enum x => EditorGUI.EnumField(label, x),
                _ => v,
            };
        }

        private static Vector2 ToVector2(Vector3 v) => new(v.X, v.Y);
        private static Vector4 ToVector4(Color c) => new(c.R, c.G, c.B, c.A);

        private static readonly Type[] s_Supported =
        {
            typeof(float), typeof(int), typeof(bool), typeof(string), typeof(Vector2), typeof(Vector3), typeof(Vector4), typeof(Color), typeof(Entity),
            typeof(Prefab), typeof(Texture), typeof(Material), typeof(Mesh), typeof(AudioClip),
        };

        // Serialized field types: the ones above, enums (stored as their int value), and lists and arrays of either.
        private static bool IsScalar(Type t) =>
            s_Supported.Contains(t) ||
            (t.IsEnum && Type.GetTypeCode(Enum.GetUnderlyingType(t)) is TypeCode.Int32 or TypeCode.Int16 or TypeCode.UInt16 or TypeCode.Byte or TypeCode.SByte);
        private static Type ElementType(Type t) =>
            t.IsArray && t.GetArrayRank() == 1 ? t.GetElementType() :
            t.IsGenericType && t.GetGenericTypeDefinition() == typeof(List<>) ? t.GetGenericArguments()[0] : null;
        private static bool IsSupported(Type t) => IsScalar(t) || (ElementType(t) is { } e && IsScalar(e));

        // A field's type for the engine: "type", with an enum's names and values, or a list's "element" type.
        private static void WriteType(Utf8JsonWriter w, Type t)
        {
            if (ElementType(t) is { } element)
            {
                w.WriteString("type", "List");
                w.WriteStartObject("element");
                WriteType(w, element);
                w.WriteEndObject();
                return;
            }
            if (t.IsEnum)
            {
                w.WriteString("type", "Enum");
                w.WriteStartArray("enumNames");
                foreach (string name in Enum.GetNames(t)) w.WriteStringValue(name);
                w.WriteEndArray();
                w.WriteStartArray("enumValues");
                foreach (object v in Enum.GetValues(t)) w.WriteNumberValue(Convert.ToInt32(v));
                w.WriteEndArray();
                return;
            }
            w.WriteString("type", TypeName(t));
        }

        private static string TypeName(Type t) =>
            t == typeof(float) ? "Float" : t == typeof(int) ? "Int" : t == typeof(bool) ? "Bool" : t == typeof(string) ? "String" :
            t == typeof(Vector2) ? "Vec2" : t == typeof(Vector3) ? "Vec3" : t == typeof(Vector4) ? "Vec4" : t == typeof(Color) ? "Color4" :
            t == typeof(Prefab) ? "Prefab" : t == typeof(Texture) ? "Texture" : t == typeof(Material) ? "Material" : t == typeof(Mesh) ? "Mesh" :
            t == typeof(AudioClip) ? "Audio" : "Entity";

        // A field's value as the engine reads it (entities as packed ids, null for none; lists as arrays).
        private static void WriteValue(Utf8JsonWriter w, object v)
        {
            switch (v)
            {
                case Entity e:
                    if (e.Scene == IntPtr.Zero) w.WriteNullValue();
                    else w.WriteNumberValue(e.Id);
                    break;
                case Enum e: w.WriteNumberValue(Convert.ToInt32(e)); break;
                case System.Collections.IList list:
                    w.WriteStartArray();
                    foreach (object item in list) WriteValue(w, item);
                    w.WriteEndArray();
                    break;
                case float f: w.WriteNumberValue(f); break;
                case int i: w.WriteNumberValue(i); break;
                case bool b: w.WriteBooleanValue(b); break;
                case string s: w.WriteStringValue(s); break;
                case Vector2 a: w.WriteStartArray(); w.WriteNumberValue(a.X); w.WriteNumberValue(a.Y); w.WriteEndArray(); break;
                case Vector3 a: w.WriteStartArray(); w.WriteNumberValue(a.X); w.WriteNumberValue(a.Y); w.WriteNumberValue(a.Z); w.WriteEndArray(); break;
                case Vector4 a: w.WriteStartArray(); w.WriteNumberValue(a.X); w.WriteNumberValue(a.Y); w.WriteNumberValue(a.Z); w.WriteNumberValue(a.W); w.WriteEndArray(); break;
                case Color c: w.WriteStartArray(); w.WriteNumberValue(c.R); w.WriteNumberValue(c.G); w.WriteNumberValue(c.B); w.WriteNumberValue(c.A); w.WriteEndArray(); break;
                case IAssetReference a: w.WriteNumberValue(a.Guid); break;
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
                    if (!IsSupported(f.FieldType) || f.IsInitOnly || f.IsNotSerialized) continue;
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
                RegisterDrawers(assembly); // before the classes: their fields say which have one

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
                            WriteType(w, f.FieldType);
                            w.WritePropertyName("default");
                            WriteValue(w, defaults != null ? f.GetValue(defaults) : null);
                            if (f.GetCustomAttribute<RangeAttribute>() is { } range) { w.WriteNumber("min", range.Min); w.WriteNumber("max", range.Max); }
                            if (f.GetCustomAttribute<TooltipAttribute>() is { } tip) w.WriteString("tooltip", tip.Text);
                            if (f.GetCustomAttribute<HeaderAttribute>() is { } header) w.WriteString("header", header.Text);
                            if (f.GetCustomAttribute<HideInInspectorAttribute>() != null) w.WriteBoolean("hidden", true);
                            if (DrawerFor(f) != null) w.WriteBoolean("drawer", true);
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
            s_Drawers.Clear();
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
                    ElementType(t) != null ? ReadValue(t, Native.FromUtf8((byte*)value), b.Entity.Scene) : // lists come as JSON
                    t.IsEnum ? Enum.ToObject(t, *(int*)value) :
                    t == typeof(float) ? *(float*)value :
                    t == typeof(int) ? *(int*)value :
                    t == typeof(bool) ? *(byte*)value != 0 :
                    t == typeof(string) ? Native.FromUtf8((byte*)value) :
                    t == typeof(Vector2) ? *(Vector2*)value :
                    t == typeof(Vector3) ? *(Vector3*)value :
                    t == typeof(Vector4) ? *(Vector4*)value :
                    t == typeof(Color) ? *(Color*)value :
                    typeof(IAssetReference).IsAssignableFrom(t) ? Activator.CreateInstance(t, *(ulong*)value) :
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
                return FieldsJson((Behaviour)GCHandle.FromIntPtr(handle).Target);
            }
            catch (Exception e)
            {
                Debug.LogError($"Reading C# fields failed: {e.Message}");
                return null;
            }
        }

        private static byte* FieldsJson(Behaviour b)
        {
            var cls = s_Classes[b.GetType().FullName];
            using var stream = new MemoryStream();
            using (var w = new Utf8JsonWriter(stream))
            {
                w.WriteStartArray();
                foreach (FieldInfo f in cls.Fields)
                {
                    WriteValue(w, f.GetValue(b));
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
