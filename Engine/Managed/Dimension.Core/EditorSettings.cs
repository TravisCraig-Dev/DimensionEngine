using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text.Json;

namespace Dimension
{
    // Settings for editor tools written in C# (like Unity's ScriptableSingleton and SettingsProvider): a settings class
    // kept in a file of its own, and pages for them in Project Settings and Preferences.

    /// <summary>Where a ScriptableSingleton's file is: under the project (shared with everyone on it, e.g. in version
    /// control) or in your own preferences folder (this machine's user only).</summary>
    public enum FilePathLocation { ProjectFolder, PreferencesFolder }

    /// <summary>The file a ScriptableSingleton loads from and saves to: a path relative to the project folder
    /// ("ProjectSettings/MyTool.json") or to the preferences folder (%APPDATA%\Dimension).</summary>
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class FilePathAttribute : Attribute
    {
        public string Path { get; }
        public FilePathLocation Location { get; }
        public FilePathAttribute(string path, FilePathLocation location)
        {
            Path = path;
            Location = location;
        }
    }

    /// <summary>One instance of a settings class for the whole editor (like Unity's ScriptableSingleton): Instance loads
    /// it from its [FilePath] file the first time (its public and [SerializeField] fields, as JSON; defaults when there is
    /// none) and Save() writes it. Without [FilePath] it lives only in memory until the scripts reload.</summary>
    public abstract class ScriptableSingleton<T> where T : ScriptableSingleton<T>, new()
    {
        private static T s_Instance;

        public static T Instance => s_Instance ??= Load();

        /// <summary>The file it is kept in (null without [FilePath]).</summary>
        public static string GetFilePath() => SettingsFiles.PathFor(typeof(T));

        /// <summary>Writes the fields to its file (folders made as needed).</summary>
        protected void Save()
        {
            string path = GetFilePath();
            if (path == null)
            {
                Debug.LogWarning($"{typeof(T).Name}: no [FilePath], so Save keeps nothing");
                return;
            }
            try
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path)!);
                File.WriteAllText(path, SettingsFiles.Write(this));
            }
            catch (Exception e) { Debug.LogError($"{typeof(T).Name}: saving {path} failed: {e.Message}"); }
        }

        private static T Load()
        {
            var value = new T();
            string path = GetFilePath();
            if (path != null && File.Exists(path))
                try { SettingsFiles.Read(value, File.ReadAllText(path)); }
                catch (Exception e) { Debug.LogError($"{typeof(T).Name}: reading {path} failed: {e.Message}"); }
            return value;
        }
    }

    internal static class SettingsFiles
    {
        public static string PathFor(Type type)
        {
            if (type.GetCustomAttribute<FilePathAttribute>() is not { } a || string.IsNullOrEmpty(a.Path)) return null;
            string root = a.Location == FilePathLocation.ProjectFolder
                ? Application.ProjectPath
                : System.IO.Path.Combine(Environment.GetEnvironmentVariable("APPDATA") ?? Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                                         "Dimension"); // (where the editor's own preferences are)
            return System.IO.Path.GetFullPath(System.IO.Path.Combine(root, a.Path));
        }

        private static IEnumerable<FieldInfo> Fields(Type type)
        {
            for (Type t = type; t != null && t != typeof(object); t = t.BaseType)
                foreach (FieldInfo f in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                    if (!f.IsInitOnly && !f.IsDefined(typeof(NonSerializedAttribute), false) && (f.IsPublic || f.IsDefined(typeof(SerializeFieldAttribute), false)))
                        yield return f;
        }

        private static readonly JsonSerializerOptions s_Options = new() { WriteIndented = true, IncludeFields = true };

        public static string Write(object value)
        {
            var fields = new Dictionary<string, JsonElement>();
            foreach (FieldInfo f in Fields(value.GetType())) fields[f.Name] = JsonSerializer.SerializeToElement(f.GetValue(value), f.FieldType, s_Options);
            return JsonSerializer.Serialize(fields, s_Options);
        }

        // Fields the file has (others keep their defaults; ones it cannot read are left alone).
        public static void Read(object value, string json)
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            foreach (FieldInfo f in Fields(value.GetType()))
                if (doc.RootElement.TryGetProperty(f.Name, out JsonElement e))
                    try { f.SetValue(value, e.Deserialize(f.FieldType, s_Options)); }
                    catch (Exception) { }
        }
    }

    /// <summary>Which settings window a SettingsProvider's page is in: Project Settings or Preferences.</summary>
    public enum SettingsScope { Project, User }

    /// <summary>A page of settings (like Unity's SettingsProvider): returned by a static method marked
    /// [SettingsProvider], shown as a section of Project Settings (Scope Project) or Preferences (Scope User), titled by
    /// the last part of its path ("Project/My Tool"), drawn by OnGUI (override it) or GuiHandler with EditorGUI.</summary>
    public class SettingsProvider
    {
        public string SettingsPath { get; }
        public SettingsScope Scope { get; }
        public string Label { get; set; }
        /// <summary>Words a search for the page could use (kept for Unity's code).</summary>
        public IEnumerable<string> Keywords { get; set; }
        public Action<string> GuiHandler { get; set; }

        public SettingsProvider(string path, SettingsScope scope = SettingsScope.User, IEnumerable<string> keywords = null)
        {
            SettingsPath = path ?? "";
            Scope = scope;
            Keywords = keywords;
            int slash = SettingsPath.LastIndexOf('/');
            Label = slash >= 0 ? SettingsPath[(slash + 1)..] : SettingsPath;
        }

        /// <summary>Draws the page (searchContext: what the window's search holds, "" for now).</summary>
        public virtual void OnGUI(string searchContext) => GuiHandler?.Invoke(searchContext);
    }

    /// <summary>A static method without parameters returning a SettingsProvider (or several, as an array) to add to
    /// Project Settings or Preferences.</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class SettingsProviderAttribute : Attribute { }
}
