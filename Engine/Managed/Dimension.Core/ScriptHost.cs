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
            private readonly AssemblyDependencyResolver m_Resolver;
            private readonly bool m_Collectible;

            public ScriptLoadContext(string scriptsPath, bool collectible) : base("DimensionScripts", isCollectible: collectible)
            {
                m_Collectible = collectible;
                // The scripts' own dependencies (NuGet packages, libraries beside them) as their .deps.json lists them.
                try { m_Resolver = new AssemblyDependencyResolver(scriptsPath); }
                catch (Exception) { m_Resolver = null; } // (no .deps.json: only the framework and the engine)
            }

            protected override Assembly Load(AssemblyName name)
            {
                // Share the engine API assembly so Behaviour etc. have one type identity.
                if (name.Name == typeof(Behaviour).Assembly.GetName().Name) return typeof(Behaviour).Assembly;
                string path = m_Resolver?.ResolveAssemblyToPath(name);
                if (path == null) return null; // framework assemblies resolve from the default context
                if (!m_Collectible) return LoadFromAssemblyPath(path);
                // Hot-reloadable: from memory, so a rebuild can write the file again.
                using var dll = new MemoryStream(File.ReadAllBytes(path));
                return LoadFromStream(dll);
            }

            protected override IntPtr LoadUnmanagedDll(string name)
            {
                // A package's native library (runtimes/<platform>/native).
                string path = m_Resolver?.ResolveUnmanagedDllToPath(name);
                return path != null ? LoadUnmanagedDllFromPath(path) : IntPtr.Zero;
            }
        }

        private sealed class ScriptClass
        {
            public Type Type;
            public List<FieldInfo> Fields = new();
            public List<MethodInfo> ContextMenu = new(); // [ContextMenu] methods, in order
        }

        private static ScriptLoadContext s_Context;
        private static readonly Dictionary<string, ScriptClass> s_Classes = new();
        // DataAsset classes (made into .asset files) and the objects made from assets so far, by GUID: one per asset,
        // shared by every field referring to it, made again when play starts.
        private static readonly Dictionary<string, ScriptClass> s_DataClasses = new();
        private static readonly Dictionary<ulong, DataAsset> s_DataAssets = new();
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
                    bool sceneGUI = type.GetMethod(nameof(Inspector.OnSceneGUI), Type.EmptyTypes)?.DeclaringType != typeof(Inspector);
                    bool inspectorGUI = type.GetMethod(nameof(Inspector.OnInspectorGUI), Type.EmptyTypes)?.DeclaringType != typeof(Inspector);
                    RegisterEditorItem("inspector", "", null, inspector.Target, sceneGUI, inspectorGUI);
                }
            }
        }

        private static void RegisterEditorItem(string kind, string path, string title, string inspects = null, bool sceneGUI = false,
                                               bool inspectorGUI = false)
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
                if (sceneGUI) w.WriteBoolean("sceneGUI", true);
                if (inspectorGUI) w.WriteBoolean("inspectorGUI", true);
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

        // The Inspector's Debug mode: a script's fields that are not saved (private state; auto-properties under their
        // property's name), with their values as text: [{"name", "type", "value"}]. The script as Inspect finds it (while
        // playing, the live one).
        [UnmanagedCallersOnly]
        internal static byte* DebugFields(int unused, IntPtr handle, byte* classUtf8, byte* fieldsUtf8, IntPtr scene, ulong entity)
        {
            try
            {
                Native.CurrentScene = scene;
                Behaviour target = TargetFor(handle, classUtf8, fieldsUtf8, scene, entity);
                if (target == null || !s_Classes.TryGetValue(target.GetType().FullName, out var cls)) return null;
                using var stream = new MemoryStream();
                using (var w = new Utf8JsonWriter(stream))
                {
                    w.WriteStartArray();
                    var chain = new List<Type>();
                    for (Type t = target.GetType(); t != null && t != typeof(Behaviour); t = t.BaseType) chain.Insert(0, t);
                    foreach (Type t in chain)
                        foreach (FieldInfo f in t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly))
                        {
                            if (cls.Fields.Contains(f)) continue;
                            string name = f.Name.StartsWith("<") && f.Name.Contains(">k__BackingField") ? f.Name[1..f.Name.IndexOf('>')] : f.Name;
                            w.WriteStartObject();
                            w.WriteString("name", name);
                            w.WriteString("type", TypeLabel(f.FieldType));
                            w.WriteString("value", DebugText(f.GetValue(target)));
                            w.WriteEndObject();
                        }
                    w.WriteEndArray();
                }
                return Utf8Result(stream.ToArray());
            }
            catch (Exception e)
            {
                Debug.LogError($"Reading debug fields failed: {e.Message}");
                return null;
            }
        }

        private static string TypeLabel(Type t) =>
            t.IsGenericType ? $"{t.Name[..t.Name.IndexOf('`')]}<{string.Join(", ", t.GetGenericArguments().Select(TypeLabel))}>" : t.Name;

        private static string DebugText(object v) => v switch
        {
            null => "null",
            float f => f.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
            double d => d.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
            Vector2 a => $"({DebugText(a.X)}, {DebugText(a.Y)})",
            Vector3 a => $"({DebugText(a.X)}, {DebugText(a.Y)}, {DebugText(a.Z)})",
            Entity e => e.IsValid ? e.Name : "None",
            string s => $"\"{s}\"",
            System.Collections.ICollection c => $"{c.Count} item{(c.Count == 1 ? "" : "s")}",
            _ => v.ToString(),
        };

        // A result native code reads before the next call (the buffer is reused).
        private static byte* Utf8Result(byte[] json)
        {
            if (s_FieldsBuffer != IntPtr.Zero) Marshal.FreeHGlobal(s_FieldsBuffer);
            s_FieldsBuffer = Marshal.AllocHGlobal(json.Length + 1);
            Marshal.Copy(json, 0, s_FieldsBuffer, json.Length);
            ((byte*)s_FieldsBuffer)[json.Length] = 0;
            return (byte*)s_FieldsBuffer;
        }

        private static readonly HashSet<Type> s_GizmoFailures = new(); // logged once each

        // The Scene view's gizmos of a script (the script as Inspect finds it): OnDrawGizmos, and OnDrawGizmosSelected
        // when `selected` is 1. What they draw reaches the engine through Gizmos.
        [UnmanagedCallersOnly]
        internal static byte* DrawGizmos(int selected, IntPtr handle, byte* classUtf8, byte* fieldsUtf8, IntPtr scene, ulong entity)
        {
            Behaviour target = null;
            try
            {
                Native.CurrentScene = scene;
                target = TargetFor(handle, classUtf8, fieldsUtf8, scene, entity);
                if (target == null) return null;
                Gizmos.Reset();
                target.OnDrawGizmos();
                if (selected != 0)
                {
                    Gizmos.Reset();
                    target.OnDrawGizmosSelected();
                }
            }
            catch (Exception e)
            {
                if (target != null && s_GizmoFailures.Add(target.GetType())) Debug.LogError($"{target.GetType().Name}.OnDrawGizmos failed: {e}");
            }
            return null;
        }

        // Runs [ContextMenu] item `index` of a script (as Inspect finds it); returns its values after.
        [UnmanagedCallersOnly]
        internal static byte* RunContextMenu(int index, IntPtr handle, byte* classUtf8, byte* fieldsUtf8, IntPtr scene, ulong entity)
        {
            try
            {
                Native.CurrentScene = scene;
                Behaviour target = TargetFor(handle, classUtf8, fieldsUtf8, scene, entity);
                if (target == null || !s_Classes.TryGetValue(target.GetType().FullName, out var cls) || index < 0 || index >= cls.ContextMenu.Count) return null;
                cls.ContextMenu[index].Invoke(target, null);
                return FieldsJson(target);
            }
            catch (Exception e)
            {
                Debug.LogError($"Context menu item failed: {(e as TargetInvocationException)?.InnerException ?? e}");
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

        // A custom Inspector's OnSceneGUI for a script (as Inspect finds it); returns its values after.
        [UnmanagedCallersOnly]
        internal static byte* SceneGUI(int id, IntPtr handle, byte* classUtf8, byte* fieldsUtf8, IntPtr scene, ulong entity)
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
                Handles.Reset();
                item.Inspector.OnSceneGUI();
                return target != null ? FieldsJson(target) : null;
            }
            catch (Exception e)
            {
                Debug.LogError($"OnSceneGUI failed: {(e as TargetInvocationException)?.InnerException ?? e}");
                return null;
            }
        }

        // The live C# scripts on an object, in order.
        internal static List<Behaviour> ScriptsOf(Entity e)
        {
            var result = new List<Behaviour>();
            if (e.Scene == IntPtr.Zero) return result;
            IntPtr* handles = stackalloc IntPtr[32];
            int count = Native.Api.EntityScripts(e.Scene, e.Id, handles, 32);
            if (count > 32)
            {
                var more = new IntPtr[count];
                fixed (IntPtr* p = more) count = Math.Min(count, Native.Api.EntityScripts(e.Scene, e.Id, p, more.Length));
                foreach (IntPtr h in more.Take(count)) if (GCHandle.FromIntPtr(h).Target is Behaviour b) result.Add(b);
                return result;
            }
            for (int i = 0; i < count; ++i) if (GCHandle.FromIntPtr(handles[i]).Target is Behaviour b) result.Add(b);
            return result;
        }

        // Every live C# script in a scene (of active objects unless `includeInactive`).
        internal static List<Behaviour> ScriptsInScene(IntPtr scene, bool includeInactive)
        {
            var result = new List<Behaviour>();
            int count = Native.Api.SceneScripts(scene, null, 0, includeInactive ? 1 : 0);
            var handles = new IntPtr[count];
            fixed (IntPtr* p = handles) count = Math.Min(count, Native.Api.SceneScripts(scene, p, handles.Length, includeInactive ? 1 : 0));
            for (int i = 0; i < count; ++i) if (GCHandle.FromIntPtr(handles[i]).Target is Behaviour b) result.Add(b);
            return result;
        }

        // A field of a script type: the object's script of that type (stored as the object).
        private static bool IsScript(Type t) => typeof(Behaviour).IsAssignableFrom(t);
        private static Behaviour ScriptOn(IntPtr scene, ulong entity, Type t) =>
            ScriptsOf(new Entity(scene, entity)).FirstOrDefault(t.IsInstanceOfType);

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
            if (IsDataAsset(t)) return v.ValueKind == JsonValueKind.Number ? LoadDataAsset(v.GetUInt64(), t) : null;
            if (t == typeof(Entity)) return v.ValueKind == JsonValueKind.Number ? new Entity(scene, v.GetUInt64()) : Entity.Null;
            if (IsScript(t)) return v.ValueKind == JsonValueKind.Number ? ScriptOn(scene, v.GetUInt64(), t) : null;
            if (v.ValueKind == JsonValueKind.Null) return t == typeof(string) ? "" : Activator.CreateInstance(t);
            if (IsSerializable(t))
            {
                // A new instance (classes too, as Unity makes them), with the fields the object has.
                object o = Activator.CreateInstance(t);
                if (v.ValueKind == JsonValueKind.Object)
                    foreach (FieldInfo f in SerializedFields(t))
                        if (v.TryGetProperty(f.Name, out JsonElement field)) f.SetValue(o, ReadValue(f.FieldType, field, scene));
                return o;
            }
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
            if (IsSerializable(t))
            {
                // Its fields under a heading: a struct comes back as a changed copy, a class is changed in place.
                object target = v ?? Activator.CreateInstance(t);
                if (t.IsValueType)
                {
                    object copy = Activator.CreateInstance(t);
                    foreach (FieldInfo f in SerializedFields(t)) f.SetValue(copy, f.GetValue(target));
                    target = copy;
                }
                EditorGUI.Separator(label);
                bool changed = false;
                foreach (FieldInfo f in SerializedFields(t))
                {
                    if (f.GetCustomAttribute<HideInInspectorAttribute>() != null) continue;
                    object before = f.GetValue(target);
                    object after = DrawValue(f.Name, before, f.FieldType, f.GetCustomAttribute<RangeAttribute>());
                    if (after != null && !Equals(after, before))
                    {
                        f.SetValue(target, after);
                        changed = true;
                    }
                }
                return changed || v == null ? target : v;
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
            typeof(Prefab), typeof(Texture), typeof(Material), typeof(Mesh), typeof(AudioClip), typeof(AnimatorController), typeof(TimelineAsset),
            typeof(VideoClip),
        };

        // Serialized field types: the ones above, data assets (by GUID), enums (stored as their int value), and lists
        // and arrays of either.
        private static bool IsDataAsset(Type t) => typeof(DataAsset).IsAssignableFrom(t);
        private static bool IsScalar(Type t) =>
            s_Supported.Contains(t) || IsDataAsset(t) || IsScript(t) ||
            (t.IsEnum && Type.GetTypeCode(Enum.GetUnderlyingType(t)) is TypeCode.Int32 or TypeCode.Int16 or TypeCode.UInt16 or TypeCode.Byte or TypeCode.SByte);
        private static Type ElementType(Type t) =>
            t.IsArray && t.GetArrayRank() == 1 ? t.GetElementType() :
            t.IsGenericType && t.GetGenericTypeDefinition() == typeof(List<>) ? t.GetGenericArguments()[0] : null;
        // [Serializable] structs and classes of the scripts (not the engine's or .NET's own), with their serialized
        // fields, nested up to kMaxDepth deep.
        private const int kMaxDepth = 4;
        private static bool IsSerializable(Type t) =>
            !t.IsPrimitive && !t.IsEnum && !t.IsGenericType && !t.IsArray && t != typeof(string) && !s_Supported.Contains(t) &&
            t.IsDefined(typeof(SerializableAttribute), false) && !typeof(Behaviour).IsAssignableFrom(t) && !IsDataAsset(t) &&
            t.Assembly != typeof(object).Assembly && t.Assembly != typeof(Behaviour).Assembly &&
            (t.IsValueType || (!t.IsAbstract && t.GetConstructor(Type.EmptyTypes) != null));
        private static bool IsSupported(Type t, int depth = 0)
        {
            bool Single(Type s) => IsScalar(s) || (depth < kMaxDepth && IsSerializable(s));
            return Single(t) || (ElementType(t) is { } e && Single(e));
        }

        // A field's type for the engine: "type", with an enum's names and values, a list's "element" type, or a
        // [Serializable] type's "fields".
        private static void WriteType(Utf8JsonWriter w, Type t, int depth = 0)
        {
            if (ElementType(t) is { } element)
            {
                w.WriteString("type", "List");
                w.WriteStartObject("element");
                WriteType(w, element, depth);
                w.WriteEndObject();
                return;
            }
            if (IsSerializable(t))
            {
                w.WriteString("type", "Struct");
                object defaults = Activator.CreateInstance(t);
                w.WriteStartArray("fields");
                foreach (FieldInfo f in SerializedFields(t, depth + 1)) WriteField(w, f, defaults, depth + 1);
                w.WriteEndArray();
                return;
            }
            if (t.IsEnum)
            {
                if (t.IsDefined(typeof(FlagsAttribute), false)) w.WriteBoolean("flags", true);
                w.WriteString("type", "Enum");
                w.WriteStartArray("enumNames");
                foreach (string name in Enum.GetNames(t)) w.WriteStringValue(name);
                w.WriteEndArray();
                w.WriteStartArray("enumValues");
                foreach (object v in Enum.GetValues(t)) w.WriteNumberValue(Convert.ToInt32(v));
                w.WriteEndArray();
                return;
            }
            if (IsScript(t))
            {
                // Another object's script: picked as that object (among those with a script of the type).
                w.WriteString("type", "Entity");
                w.WriteString("scriptClass", t.FullName);
                return;
            }
            if (IsDataAsset(t))
            {
                w.WriteString("type", "DataAsset");
                w.WriteString("class", t.FullName);
                return;
            }
            w.WriteString("type", TypeName(t));
        }

        private static string TypeName(Type t) =>
            t == typeof(float) ? "Float" : t == typeof(int) ? "Int" : t == typeof(bool) ? "Bool" : t == typeof(string) ? "String" :
            t == typeof(Vector2) ? "Vec2" : t == typeof(Vector3) ? "Vec3" : t == typeof(Vector4) ? "Vec4" : t == typeof(Color) ? "Color4" :
            t == typeof(Prefab) ? "Prefab" : t == typeof(Texture) ? "Texture" : t == typeof(Material) ? "Material" : t == typeof(Mesh) ? "Mesh" :
            t == typeof(AudioClip) ? "Audio" : t == typeof(AnimatorController) ? "AnimatorController" : t == typeof(TimelineAsset) ? "Timeline" :
            t == typeof(VideoClip) ? "VideoClip" : "Entity";

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
                case DataAsset d: w.WriteNumberValue(d.Guid); break;
                case Behaviour b:
                    if (b.Entity.Scene == IntPtr.Zero) w.WriteNullValue();
                    else w.WriteNumberValue(b.Entity.Id);
                    break;
                case System.Collections.IList list:
                    w.WriteStartArray();
                    foreach (object item in list) WriteValue(w, item);
                    w.WriteEndArray();
                    break;
                case object o when IsSerializable(o.GetType()):
                    // A [Serializable] value: its fields by name.
                    w.WriteStartObject();
                    foreach (FieldInfo f in SerializedFields(o.GetType()))
                    {
                        w.WritePropertyName(f.Name);
                        WriteValue(w, f.GetValue(o));
                    }
                    w.WriteEndObject();
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

        // A field's registration: its name, type, default (from `defaults`, an instance of its owner) and attributes.
        private static void WriteField(Utf8JsonWriter w, FieldInfo f, object defaults, int depth)
        {
            w.WriteStartObject();
            w.WriteString("name", f.Name);
            WriteType(w, f.FieldType, depth);
            w.WritePropertyName("default");
            WriteValue(w, defaults != null ? f.GetValue(defaults) : null);
            if (f.GetCustomAttribute<RangeAttribute>() is { } range) { w.WriteNumber("min", range.Min); w.WriteNumber("max", range.Max); }
            if (f.GetCustomAttribute<TooltipAttribute>() is { } tip) w.WriteString("tooltip", tip.Text);
            if (f.GetCustomAttribute<HeaderAttribute>() is { } header) w.WriteString("header", header.Text);
            if (f.GetCustomAttribute<HideInInspectorAttribute>() != null) w.WriteBoolean("hidden", true);
            if (DrawerFor(f) != null) w.WriteBoolean("drawer", true);
            w.WriteEndObject();
        }

        private static IEnumerable<FieldInfo> SerializedFields(Type type, int depth = 0)
        {
            var chain = new List<Type>();
            for (Type t = type; t != null && t != typeof(Behaviour) && t != typeof(DataAsset); t = t.BaseType) chain.Insert(0, t);
            foreach (Type t in chain)
                foreach (FieldInfo f in t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly))
                {
                    if (!IsSupported(f.FieldType, depth) || f.IsInitOnly || f.IsDefined(typeof(NonSerializedAttribute), false)) continue;
                    if (!f.IsPublic && f.GetCustomAttribute<SerializeFieldAttribute>() == null) continue;
                    yield return f;
                }
        }

        [UnmanagedCallersOnly]
        internal static int LoadScripts(byte* pathUtf8, int fixedLoad)
        {
            try
            {
                UnloadInternal();
                string path = Native.FromUtf8(pathUtf8);
                if (!File.Exists(path)) return 0;
                Assembly assembly;
                if (fixedLoad != 0)
                {
                    // A built game: loaded once from the file, in a context that stays, so its ahead-of-time
                    // (ReadyToRun) code runs instead of being compiled again (collectible and in-memory loads skip it).
                    s_Context = new ScriptLoadContext(Path.GetFullPath(path), collectible: false);
                    assembly = s_Context.LoadFromAssemblyPath(Path.GetFullPath(path));
                }
                else
                {
                    s_Context = new ScriptLoadContext(Path.GetFullPath(path), collectible: true);
                    // Load from memory so the compiler can overwrite the file while we run.
                    string pdb = Path.ChangeExtension(path, ".pdb");
                    using var dll = new MemoryStream(File.ReadAllBytes(path));
                    using var symbols = File.Exists(pdb) ? new MemoryStream(File.ReadAllBytes(pdb)) : null;
                    assembly = s_Context.LoadFromStream(dll, symbols);
                }
                RegisterDrawers(assembly); // before the classes: their fields say which have one
                foreach (Type type in assembly.GetTypes().Where(t => !t.IsAbstract && t.IsSubclassOf(typeof(StateMachineBehaviour))))
                {
                    s_StateBehaviourTypes[type.FullName] = type;
                    s_StateBehaviourTypes.TryAdd(type.Name, type); // the short name works too while it is unique
                }

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
                        if (type.IsDefined(typeof(ExecuteInEditModeAttribute), true)) w.WriteBoolean("executeInEditMode", true);
                        // The script classes it derives from (fields of those types take it).
                        w.WriteStartArray("bases");
                        for (Type b = type.BaseType; b != null && b != typeof(Behaviour); b = b.BaseType) w.WriteStringValue(b.FullName);
                        w.WriteEndArray();
                        // Gizmos (an OnDrawGizmos or OnDrawGizmosSelected of its own) and [ContextMenu] methods.
                        bool Overrides(string name) => type.GetMethod(name, BindingFlags.Public | BindingFlags.Instance, Type.EmptyTypes)?.DeclaringType != typeof(Behaviour);
                        if (Overrides(nameof(Behaviour.OnDrawGizmos)) || Overrides(nameof(Behaviour.OnDrawGizmosSelected))) w.WriteBoolean("gizmos", true);
                        // OnCollisionStay is only called (every physics step) for classes that have one.
                        if (type.GetMethod(nameof(Behaviour.OnCollisionStay), BindingFlags.Public | BindingFlags.Instance, new[] { typeof(Collision) })
                                ?.DeclaringType != typeof(Behaviour))
                            w.WriteBoolean("collisionStay", true);
                        if (type.GetMethod(nameof(Behaviour.OnTriggerStay), BindingFlags.Public | BindingFlags.Instance, new[] { typeof(Entity) })
                                ?.DeclaringType != typeof(Behaviour))
                            w.WriteBoolean("triggerStay", true);
                        if (type.GetMethod(nameof(Behaviour.OnControllerColliderHit), BindingFlags.Public | BindingFlags.Instance, new[] { typeof(ControllerColliderHit) })
                                ?.DeclaringType != typeof(Behaviour))
                            w.WriteBoolean("controllerHit", true);
                        // OnMouse* callbacks are only called (every frame for Over / Drag) on classes that have one.
                        string[] mouse = { nameof(Behaviour.OnMouseEnter), nameof(Behaviour.OnMouseOver), nameof(Behaviour.OnMouseExit),
                                           nameof(Behaviour.OnMouseDown), nameof(Behaviour.OnMouseDrag), nameof(Behaviour.OnMouseUp),
                                           nameof(Behaviour.OnMouseUpAsButton) };
                        if (mouse.Any(Overrides)) w.WriteBoolean("mouseEvents", true);
                        if (Overrides(nameof(Behaviour.OnAnimatorMove))) w.WriteBoolean("animatorMove", true);
                        if (type.GetMethod(nameof(Behaviour.OnLateUpdate), BindingFlags.Public | BindingFlags.Instance, new[] { typeof(float) })
                                ?.DeclaringType != typeof(Behaviour))
                            w.WriteBoolean("lateUpdate", true);
                        // UI pointer events, by PointerEventKind bit: only the overridden ones are sent.
                        uint pointer = 0;
                        for (int k = 0; k < PointerEventData.MethodNames.Length; ++k)
                            if (type.GetMethod(PointerEventData.MethodNames[k], BindingFlags.Public | BindingFlags.Instance, new[] { typeof(PointerEventData) })
                                    ?.DeclaringType != typeof(Behaviour))
                                pointer |= 1u << k;
                        if (pointer != 0) w.WriteNumber("pointerEvents", pointer);
                        w.WriteStartArray("contextMenu");
                        for (Type t = type; t != null && t != typeof(Behaviour); t = t.BaseType)
                            foreach (MethodInfo m in t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly))
                                if (m.GetCustomAttribute<ContextMenuAttribute>() is { } item && m.GetParameters().Length == 0)
                                {
                                    cls.ContextMenu.Add(m);
                                    w.WriteStringValue(item.Name);
                                }
                        w.WriteEndArray();
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
                            WriteField(w, f, defaults, 0);
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
                RegisterDataClasses(assembly);
                RegisterEditorItems(assembly);
                return s_Classes.Count;
            }
            catch (Exception e)
            {
                Debug.LogError($"Failed to load C# scripts: {e}");
                return 0;
            }
        }

        // DataAsset classes: their fields (not entities: an asset belongs to no scene) and the data classes they derive
        // from (fields of those take them too).
        private static void RegisterDataClasses(Assembly assembly)
        {
            foreach (Type type in assembly.GetTypes().Where(t => !t.IsAbstract && t.IsSubclassOf(typeof(DataAsset))).OrderBy(t => t.FullName))
            {
                var cls = new ScriptClass { Type = type };
                object defaults = null;
                try { defaults = Activator.CreateInstance(type); } catch (Exception e) { Debug.LogError($"{type.FullName}: constructor failed: {e.Message}"); }
                using var stream = new MemoryStream();
                using (var w = new Utf8JsonWriter(stream))
                {
                    w.WriteStartObject();
                    w.WriteString("name", type.FullName);
                    w.WriteBoolean("dataAsset", true);
                    w.WriteStartArray("bases");
                    for (Type b = type.BaseType; b != null && b != typeof(DataAsset); b = b.BaseType) w.WriteStringValue(b.FullName);
                    w.WriteEndArray();
                    w.WriteStartArray("fields");
                    foreach (FieldInfo f in SerializedFields(type))
                    {
                        // (An asset belongs to no scene: no objects or their scripts.)
                        Type element = ElementType(f.FieldType) ?? f.FieldType;
                        if (element == typeof(Entity) || IsScript(element)) continue;
                        cls.Fields.Add(f);
                        WriteField(w, f, defaults, 0);
                    }
                    w.WriteEndArray();
                    w.WriteEndObject();
                }
                s_DataClasses[type.FullName] = cls;
                byte[] json = stream.ToArray();
                var withNul = new byte[json.Length + 1];
                Array.Copy(json, withNul, json.Length);
                fixed (byte* q = withNul) Native.Api.RegisterScriptClass(q);
            }
        }

        // The object of a data asset (made from its file on first use), or null when there is none of type `expected`.
        internal static DataAsset LoadDataAsset(ulong guid, Type expected)
        {
            if (guid == 0) return null;
            if (!s_DataAssets.TryGetValue(guid, out DataAsset asset))
            {
                string json = Native.FromUtf8(Native.Api.DataAssetFile(guid));
                if (json == null) return null;
                using var doc = JsonDocument.Parse(json);
                string name = doc.RootElement.GetProperty("class").GetString();
                if (!s_DataClasses.TryGetValue(name ?? "", out ScriptClass cls))
                {
                    Debug.LogWarning($"Data asset {guid}: no data class '{name}'");
                    return null;
                }
                asset = (DataAsset)Activator.CreateInstance(cls.Type);
                asset.Guid = guid;
                s_DataAssets[guid] = asset; // before its fields: assets referring to each other get this object
                ReadDataFields(asset, cls, doc.RootElement);
            }
            if (!expected.IsInstanceOfType(asset))
            {
                Debug.LogWarning($"Data asset {guid} is a {asset.GetType().FullName}, not a {expected.FullName}");
                return null;
            }
            return asset;
        }

        private static void ReadDataFields(DataAsset asset, ScriptClass cls, JsonElement file)
        {
            if (file.TryGetProperty("fields", out JsonElement fields) && fields.ValueKind == JsonValueKind.Object)
                foreach (FieldInfo f in cls.Fields)
                    if (fields.TryGetProperty(f.Name, out JsonElement v) && v.ValueKind != JsonValueKind.Null)
                        f.SetValue(asset, ReadValue(f.FieldType, v, IntPtr.Zero));
            try { asset.OnLoaded(); }
            catch (Exception e) { Debug.LogError($"{cls.Type.FullName}.OnLoaded failed: {e}"); }
        }

        [UnmanagedCallersOnly]
        internal static void DataAssetChanged(ulong guid)
        {
            try
            {
                if (guid == 0)
                {
                    s_DataAssets.Clear();
                    return;
                }
                // Saved: the object scripts hold takes the new values (in place, so every reference sees them).
                if (!s_DataAssets.TryGetValue(guid, out DataAsset asset)) return;
                string json = Native.FromUtf8(Native.Api.DataAssetFile(guid));
                using var doc = json != null ? JsonDocument.Parse(json) : null;
                if (doc == null || doc.RootElement.GetProperty("class").GetString() != asset.GetType().FullName ||
                    !s_DataClasses.TryGetValue(asset.GetType().FullName, out ScriptClass cls))
                {
                    s_DataAssets.Remove(guid);
                    return;
                }
                ReadDataFields(asset, cls, doc.RootElement);
            }
            catch (Exception e) { Debug.LogError($"Reloading data asset failed: {e}"); }
        }

        // Separate, non-inlined frame: otherwise (especially in Debug builds) the JIT can keep the
        // context alive in a stack slot of the caller and the collectible assembly never unloads.
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        private static WeakReference BeginUnload()
        {
            s_Classes.Clear();
            s_DataClasses.Clear();
            s_DataAssets.Clear();
            s_EditorItems.Clear();
            s_Drawers.Clear();
            s_GizmoFailures.Clear();
            s_StateBehaviourTypes.Clear();
            s_StateBehaviours.Clear();
            s_MissingStateBehaviours.Clear();
            s_EventMethods.Clear();
            EngineEvents.Clear(); // listeners may be methods of the scripts being unloaded
            if (s_Context == null) return null;
            var weak = new WeakReference(s_Context);
            if (s_Context.IsCollectible) s_Context.Unload(); // (a built game's stays until exit)
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
                behaviour.Handle = GCHandle.ToIntPtr(GCHandle.Alloc(behaviour));
                return behaviour.Handle;
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
                    ElementType(t) != null || IsSerializable(t) ? ReadValue(t, Native.FromUtf8((byte*)value), b.Entity.Scene) : // as JSON
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
                    IsDataAsset(t) ? LoadDataAsset(*(ulong*)value, t) :
                    IsScript(t) ? ScriptOn(b.Entity.Scene, *(ulong*)value, t) :
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
                    case 8: b.OnEnable(); break;
                    case 9: b.OnDisable(); break;
                    case 10: b.OnMouseEnter(); break;
                    case 11: b.OnMouseOver(); break;
                    case 12: b.OnMouseExit(); break;
                    case 13: b.OnMouseDown(); break;
                    case 14: b.OnMouseDrag(); break;
                    case 15: b.OnMouseUp(); break;
                    case 16: b.OnMouseUpAsButton(); break;
                    case 17: b.OnApplicationFocus(dt != 0.0f); break;
                    case 18: b.OnApplicationPause(dt != 0.0f); break;
                    case 19: b.OnApplicationQuit(); break;
                    case 20: b.OnAnimatorMove(); break;
                    case 21: b.OnLateUpdate(dt); break;
                }
            }
            catch (Exception e) { Debug.LogError($"{b?.GetType().Name}: {e}"); }
        }

        // Coroutines waiting on WaitForFixedUpdate (0) or WaitForEndOfFrame (1), in that scene.
        [UnmanagedCallersOnly]
        internal static void CoroutinePhase(IntPtr scene, int phase)
        {
            try { CoroutineRunner.ResumePhase(scene, phase); }
            catch (Exception e) { Debug.LogError($"Coroutines: {e}"); }
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
                    case 2:
                        b.OnAnimationEvent(s);
                        // Like Unity's animation events (and timeline signals): a method of that name, if the class has
                        // one taking nothing, is called too.
                        if (EventMethod(b.GetType(), s) is { } named) named.Invoke(b, null);
                        break;
                }
            }
            catch (Exception e) { Debug.LogError($"{b?.GetType().Name}: {e}"); }
        }

        // 0 CollisionEnter, 1 CollisionExit, 2 TriggerEnter, 3 TriggerExit, 4 TriggerStay
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
                    case 4: b.OnTriggerStay(o); break;
                }
            }
            catch (Exception e) { Debug.LogError($"{b?.GetType().Name}: {e}"); }
        }

        // data: normal, relative velocity, impulse, then per contact its point, normal and separation (7 floats each).
        [UnmanagedCallersOnly]
        internal static void InvokeCollision(IntPtr handle, int phase, ulong other, float* data, int contactCount)
        {
            Behaviour b = null;
            try
            {
                b = (Behaviour)GCHandle.FromIntPtr(handle).Target;
                Native.CurrentScene = b.Entity.Scene;
                var contacts = new ContactPoint[contactCount];
                for (int i = 0; i < contactCount; ++i)
                {
                    float* d = data + 7 + i * 7;
                    contacts[i] = new ContactPoint { Point = new Vector3(d[0], d[1], d[2]), Normal = new Vector3(d[3], d[4], d[5]), Separation = d[6] };
                }
                if (phase == 3)
                {
                    // A Character Controller's hit: normal, move direction, move length, then the contact's point.
                    b.OnControllerColliderHit(new ControllerColliderHit
                    {
                        Entity = new Entity(b.Entity.Scene, other), Controller = b.Entity.GetComponent<CharacterController>(),
                        Normal = new Vector3(data[0], data[1], data[2]), MoveDirection = new Vector3(data[3], data[4], data[5]),
                        MoveLength = data[6], Point = contactCount > 0 ? contacts[0].Point : default,
                    });
                    return;
                }
                var c = new Collision(new Entity(b.Entity.Scene, other), contacts, new Vector3(data[0], data[1], data[2]),
                                      new Vector3(data[3], data[4], data[5]), data[6]);
                switch (phase)
                {
                    case 0: b.OnCollisionEnter(c); break;
                    case 1: b.OnCollisionStay(c); break;
                    case 2: b.OnCollisionExit(c); break;
                }
            }
            catch (Exception e) { Debug.LogError($"{b?.GetType().Name}: {e}"); }
        }

        // data: position xy, delta xy, press position xy, scroll, button, click count, dragging; ids: over, press, drag.
        [UnmanagedCallersOnly]
        internal static void InvokePointer(IntPtr handle, int kind, float* data, ulong* ids)
        {
            Behaviour b = null;
            try
            {
                b = (Behaviour)GCHandle.FromIntPtr(handle).Target;
                IntPtr scene = b.Entity.Scene;
                Native.CurrentScene = scene;
                Entity E(ulong id) => id == ulong.MaxValue ? Entity.Null : new Entity(scene, id);
                var e = new PointerEventData
                {
                    Position = new Vector2(data[0], data[1]), Delta = new Vector2(data[2], data[3]),
                    PressPosition = new Vector2(data[4], data[5]), ScrollDelta = data[6], Button = (int)data[7],
                    ClickCount = (int)data[8], Dragging = data[9] != 0.0f, PointerId = (int)data[10],
                    PointerEnter = E(ids[0]), PointerPress = E(ids[1]), PointerDrag = E(ids[2]),
                };
                switch (kind)
                {
                    case 0: b.OnPointerEnter(e); break;
                    case 1: b.OnPointerExit(e); break;
                    case 2: b.OnPointerDown(e); break;
                    case 3: b.OnPointerUp(e); break;
                    case 4: b.OnPointerClick(e); break;
                    case 5: b.OnBeginDrag(e); break;
                    case 6: b.OnDrag(e); break;
                    case 7: b.OnEndDrag(e); break;
                    case 8: b.OnDrop(e); break;
                    case 9: b.OnSelect(e); break;
                    case 10: b.OnDeselect(e); break;
                    case 11: b.OnScroll(e); break;
                }
            }
            catch (Exception ex) { Debug.LogError($"{b?.GetType().Name}: {ex}"); }
        }

        // StateMachineBehaviour classes by full (and short) name, and their objects: one per entity, layer, state and
        // class, made the first time (dropped when play stops).
        private static readonly Dictionary<string, Type> s_StateBehaviourTypes = new();
        private readonly record struct StateBehaviourKey(IntPtr Scene, ulong Entity, int Layer, int State, string Class);
        private static readonly Dictionary<StateBehaviourKey, StateMachineBehaviour> s_StateBehaviours = new();
        private static readonly HashSet<string> s_MissingStateBehaviours = new();

        [UnmanagedCallersOnly]
        internal static void InvokeStateBehaviour(IntPtr scene, ulong entity, int kind, int layer, int state, byte* classes, byte* name, byte* tag, float* info)
        {
            string current = null;
            try
            {
                Native.CurrentScene = scene;
                var e = new Entity(scene, entity);
                Animator animator = e.GetComponent<Animator>();
                var stateInfo = new AnimatorStateInfo(Native.FromUtf8(name), info[0], info[1], info[2], info[3] != 0.0f, Native.FromUtf8(tag));
                foreach (string cls in Native.FromUtf8(classes).Split('\n', StringSplitOptions.RemoveEmptyEntries))
                {
                    current = cls.Trim();
                    var key = new StateBehaviourKey(scene, entity, layer, state, current);
                    if (!s_StateBehaviours.TryGetValue(key, out var behaviour))
                    {
                        if (!s_StateBehaviourTypes.TryGetValue(current, out Type type))
                        {
                            if (s_MissingStateBehaviours.Add(current)) Debug.LogError($"State behaviour class '{current}' (state {stateInfo.Name}) is not a StateMachineBehaviour in the scripts");
                            continue;
                        }
                        behaviour = (StateMachineBehaviour)Activator.CreateInstance(type);
                        s_StateBehaviours[key] = behaviour;
                    }
                    switch (kind)
                    {
                        case 0: behaviour.OnStateEnter(animator, stateInfo, layer); break;
                        case 1: behaviour.OnStateUpdate(animator, stateInfo, layer); break;
                        case 2: behaviour.OnStateExit(animator, stateInfo, layer); break;
                    }
                }
            }
            catch (Exception ex) { Debug.LogError($"{current}: {ex}"); }
        }

        private static readonly Dictionary<(Type, string), MethodInfo> s_EventMethods = new();
        private static MethodInfo EventMethod(Type type, string name)
        {
            if (string.IsNullOrEmpty(name) || name == nameof(Behaviour.OnAnimationEvent)) return null;
            if (s_EventMethods.TryGetValue((type, name), out var m)) return m;
            m = type.GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, Type.EmptyTypes);
            if (m != null && (m.DeclaringType == typeof(Behaviour) || m.ReturnType != typeof(void) && m.ReturnType != typeof(System.Collections.IEnumerator))) m = null;
            s_EventMethods[(type, name)] = m;
            return m;
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

        // A member of a script that is not a field, for UI Bindings: op 0 a readable property's value or a method with no
        // parameters' result (JSON; null when there is none), 1 a value (JSON) into a writable property or a method with
        // one parameter ("true", or null).
        [UnmanagedCallersOnly]
        internal static byte* MemberOp(IntPtr handle, int op, byte* nameUtf8, byte* jsonUtf8)
        {
            try
            {
                var b = (Behaviour)GCHandle.FromIntPtr(handle).Target;
                string name = Marshal.PtrToStringUTF8((IntPtr)nameUtf8) ?? "";
                const BindingFlags flags = BindingFlags.Public | BindingFlags.Instance;
                Type t = b.GetType();
                PropertyInfo p = t.GetProperty(name, flags);
                if (op == 0)
                {
                    object value;
                    if (p != null && p.CanRead && p.GetIndexParameters().Length == 0) value = p.GetValue(b);
                    else if (t.GetMethod(name, flags, Type.EmptyTypes) is { ReturnType: var rt } m && rt != typeof(void)) value = m.Invoke(b, null);
                    else return null;
                    using var stream = new MemoryStream();
                    using (var w = new Utf8JsonWriter(stream)) WriteValue(w, value);
                    return Utf8Result(stream.ToArray());
                }
                string json = Marshal.PtrToStringUTF8((IntPtr)jsonUtf8) ?? "null";
                if (p != null && p.CanWrite && p.GetIndexParameters().Length == 0)
                {
                    p.SetValue(b, ReadValue(p.PropertyType, json, b.Entity.Scene));
                    return Utf8Result(System.Text.Encoding.UTF8.GetBytes("true"));
                }
                foreach (MethodInfo m in t.GetMethods(flags))
                    if (m.Name == name && m.GetParameters() is { Length: 1 } ps)
                    {
                        m.Invoke(b, new[] { ReadValue(ps[0].ParameterType, json, b.Entity.Scene) });
                        return Utf8Result(System.Text.Encoding.UTF8.GetBytes("true"));
                    }
                return null;
            }
            catch (Exception e)
            {
                Debug.LogError($"UI Binding member access failed: {(e as TargetInvocationException)?.InnerException?.Message ?? e.Message}");
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
            return Utf8Result(stream.ToArray());
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
                if (kind < 0)
                {
                    EngineEvents.ClearScene(scene);
                    foreach (var key in s_StateBehaviours.Keys.Where(k => k.Scene == scene).ToList()) s_StateBehaviours.Remove(key);
                }
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
