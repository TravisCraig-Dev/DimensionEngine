using System;
using System.Numerics;
using Dimension.Interop;

namespace Dimension
{
    // Editor extensions (like Unity's UnityEditor): code in the scripts assembly that runs in the editor only. A static
    // method with [MenuItem("Tools/Do Something")] becomes a menu command (its first path part is the menu; an
    // existing one such as Window or GameObject gains the item); a class deriving from EditorWindow with
    // [EditorWindow("Title", "Window/Title")] becomes a window opened from that menu path, drawn by its OnGUI with
    // EditorGUI. Selection, Undo and EditorScene reach the scene being edited. In a built game none of this is called.

    [AttributeUsage(AttributeTargets.Method)]
    public sealed class MenuItemAttribute : Attribute
    {
        public string Path { get; }
        public MenuItemAttribute(string path) => Path = path;
    }

    [AttributeUsage(AttributeTargets.Class)]
    public sealed class EditorWindowAttribute : Attribute
    {
        public string Title { get; }
        public string MenuPath { get; }
        public EditorWindowAttribute(string title, string menuPath = null)
        {
            Title = title;
            MenuPath = menuPath ?? "Window/" + title;
        }
    }

    // A custom Inspector (like Unity's CustomEditor): a class deriving from Inspector with
    // [CustomInspector(typeof(MyScript))] draws that script's Inspector in place of its fields (DrawDefaultInspector
    // draws them as usual); with [CustomInspector("Point Light")] it draws below a built-in component's fields.
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class CustomInspectorAttribute : Attribute
    {
        public string Target { get; }
        public CustomInspectorAttribute(Type script) => Target = script?.FullName ?? "";
        public CustomInspectorAttribute(string component) => Target = component ?? "";
    }

    public abstract class Inspector
    {
        // The script being inspected, holding its saved values (changes made to it are saved, as one undo step per
        // edit); while playing, the live script. Null for a built-in component.
        public Behaviour Target { get; internal set; }
        // The object being inspected.
        public Entity Entity { get; internal set; }
        public abstract void OnInspectorGUI();
        // The script's fields as the Inspector draws them without a custom one.
        public void DrawDefaultInspector() => ScriptHost.DrawDefaultFields(Target);
    }

    // A property drawer (like Unity's PropertyDrawer): a class deriving from PropertyDrawer with
    // [CustomPropertyDrawer(typeof(MyAttribute))] draws every script field marked with that attribute; with a field type
    // (typeof(Vector3)) it draws every field of that type that no attribute's drawer claims. OnGUI draws the field with
    // EditorGUI and returns its value; in the Inspector each control takes a row like the built-in fields.
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class CustomPropertyDrawerAttribute : Attribute
    {
        public Type Target { get; }
        public CustomPropertyDrawerAttribute(Type target) => Target = target;
    }

    public abstract class PropertyDrawer
    {
        // The field being drawn, and the attribute that chose this drawer (null when chosen by the field's type).
        public System.Reflection.FieldInfo Field { get; internal set; }
        public Attribute Attribute { get; internal set; }
        public abstract object OnGUI(string label, object value);
    }

    // A dockable editor window; one instance per class, kept while the scripts stay loaded.
    public abstract class EditorWindow
    {
        public abstract void OnGUI();
    }

    // Immediate-mode controls for EditorWindow.OnGUI (the editor's own look). Fields return the edited value; buttons
    // return true when clicked. Labels identify controls, so give each a distinct one within a window.
    public static unsafe class EditorGUI
    {
        private static int Op(int op, string label, float* v = null, int n = 0) =>
            Native.WithUtf8(label, p => Native.Api.EditorGui(op, (byte*)p, v, n));

        public static void Label(string text) => Op(0, text);
        public static bool Button(string label) => Op(1, label) != 0;
        public static bool Toggle(string label, bool value)
        {
            float v = value ? 1 : 0;
            Op(2, label, &v, 1);
            return v != 0;
        }
        public static float FloatField(string label, float value)
        {
            Op(3, label, &value, 1);
            return value;
        }
        public static int IntField(string label, int value)
        {
            float v = value;
            Op(4, label, &v, 1);
            return (int)v;
        }
        public static float Slider(string label, float value, float min, float max)
        {
            float* v = stackalloc float[3] { value, min, max };
            Op(5, label, v, 3);
            return v[0];
        }
        public static Vector3 Vector3Field(string label, Vector3 value)
        {
            Op(6, label, (float*)&value, 3);
            return value;
        }
        public static Color ColorField(string label, Color value)
        {
            Op(7, label, (float*)&value, 4);
            return value;
        }
        // A dropdown of `options`; returns the chosen index.
        public static int Popup(string label, int index, string[] options)
        {
            float v = index;
            Op(11, label + "\x1f" + string.Join("\x1f", options), &v, 1);
            return (int)v;
        }
        // A dropdown of an enum's names.
        public static T EnumField<T>(string label, T value) where T : struct, Enum => (T)EnumField(label, (Enum)value);

        internal static Enum EnumField(string label, Enum value)
        {
            Type t = value.GetType();
            Array values = Enum.GetValues(t);
            int index = Popup(label, Array.IndexOf(values, value), Enum.GetNames(t));
            return index >= 0 && index < values.Length ? (Enum)values.GetValue(index) : value;
        }

        // A heading line (empty: a plain separator).
        public static void Separator(string text = "") => Op(8, text);
        // The next control on the same line.
        public static void SameLine() => Op(9, "");
        public static void Space() => Op(10, "");
        public static string TextField(string label, string value) =>
            Native.WithUtf8(label, l => Native.WithUtf8(value, t => Native.FromUtf8(Native.Api.EditorGuiText((byte*)l, (byte*)t))));
        // An object of the scene being edited (pick from a list, or drop one from the Hierarchy).
        public static Entity EntityField(string label, Entity value) =>
            new Entity(Native.CurrentScene, Native.WithUtf8(label, p => Native.Api.EditorGuiRef(0, (byte*)p, value.Id)));
        // An asset of the reference's type (pick from the project's, or drop one from the Project window).
        public static T AssetField<T>(string label, T value) where T : struct, IAssetReference => (T)AssetField(label, (IAssetReference)value);

        internal static IAssetReference AssetField(string label, IAssetReference value)
        {
            Type t = value.GetType();
            ulong guid = Native.WithUtf8(label, p => Native.Api.EditorGuiRef(AssetKind(t), (byte*)p, value.Guid));
            return (IAssetReference)Activator.CreateInstance(t, guid);
        }

        // Shared with the engine: 1 Prefab, 2 Texture, 3 Material, 4 Mesh, 5 Audio.
        internal static int AssetKind(Type t) =>
            t == typeof(Prefab) ? 1 : t == typeof(Texture) ? 2 : t == typeof(Material) ? 3 : t == typeof(Mesh) ? 4 : t == typeof(AudioClip) ? 5 : 0;
    }

    // The editor's selection in the scene being edited.
    public static unsafe class Selection
    {
        public static Entity Active
        {
            get => new Entity(Native.CurrentScene, Native.Api.EditorSelection(0, 0, null));
            set => Native.Api.EditorSelection(1, value.Id, null);
        }
        public static Entity[] Entities
        {
            get
            {
                int count = (int)Native.Api.EditorSelection(3, 0, null);
                var all = new Entity[count];
                for (int i = 0; i < count; ++i) all[i] = new Entity(Native.CurrentScene, Native.Api.EditorSelection(4, (ulong)i, null));
                return all;
            }
        }
    }

    public static class Undo
    {
        // Call before changing the scene: the change becomes one undo step (and the scene is marked changed).
        public static unsafe void Record(string name) => Native.WithUtf8(name, p => Native.Api.EditorUndo((byte*)p));
    }

    // The scene being edited.
    public static unsafe class EditorScene
    {
        public static Entity CreateEntity(string name) =>
            new Entity(Native.CurrentScene, Native.WithUtf8(name, p => Native.Api.EditorSelection(2, 0, (byte*)p)));
    }
}
