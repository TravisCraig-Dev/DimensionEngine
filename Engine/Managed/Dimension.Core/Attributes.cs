using System;

namespace Dimension
{
    // Inspector and Add Component attributes for Behaviour scripts (like Unity's).

    /// <summary>Limits a float or int field to [min, max] (the Inspector clamps while dragging).</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class RangeAttribute : Attribute
    {
        public readonly float Min, Max;
        public RangeAttribute(float min, float max) { Min = min; Max = max; }
    }

    /// <summary>Text shown when hovering the field's name in the Inspector.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class TooltipAttribute : Attribute
    {
        public readonly string Text;
        public TooltipAttribute(string text) { Text = text; }
    }

    /// <summary>A heading drawn above the field in the Inspector.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class HeaderAttribute : Attribute
    {
        public readonly string Text;
        public HeaderAttribute(string text) { Text = text; }
    }

    /// <summary>Lists a parameterless method in the script's Inspector menu ("..." on its header): choosing it runs the
    /// method in the editor (on the script holding its saved values, or the live one while playing), and changes it
    /// makes to the fields are saved as one undo step.</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class ContextMenuAttribute : Attribute
    {
        public readonly string Name;
        public ContextMenuAttribute(string name) { Name = name; }
    }

    /// <summary>Runs the script in the editor outside play mode too: it is created (OnCreate) when it appears in the
    /// edited scene, updated (OnUpdate) every editor frame, given the saved values again when they are edited, and
    /// ended (OnDestroy) when it goes or play mode starts. Application.IsPlaying tells the two apart.</summary>
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class ExecuteInEditModeAttribute : Attribute { }

    /// <summary>Keeps a public field saved with the scene but out of the Inspector.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class HideInInspectorAttribute : Attribute { }

    /// <summary>Saves and shows a private field like a public one. ([System.NonSerialized] keeps a public field out.)</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class SerializeFieldAttribute : Attribute { }

    /// <summary>Where the script appears in the Inspector's Add Component menu: "Category/Name" (or just "Name").</summary>
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class AddComponentMenuAttribute : Attribute
    {
        public readonly string Path;
        public AddComponentMenuAttribute(string path) { Path = path; }
    }

    /// <summary>Components the script needs: adding the script in the editor adds them too, if missing.</summary>
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = true)]
    public sealed class RequireComponentAttribute : Attribute
    {
        public readonly Type[] Types;
        public RequireComponentAttribute(params Type[] types) { Types = types; }
    }

    /// <summary>When a [RuntimeInitializeOnLoadMethod] runs as play (or the game) starts, in this order.</summary>
    public enum RuntimeInitializeLoadType
    {
        /// <summary>After the first scene's objects have started (OnCreate).</summary>
        AfterSceneLoad = 0,
        /// <summary>Before the first scene's objects start (its objects exist; new ones made here start with them).</summary>
        BeforeSceneLoad = 1,
        /// <summary>As the scripts are ready, before anything of the scene.</summary>
        AfterAssembliesLoaded = 2,
        /// <summary>Before AfterAssembliesLoaded (no splash screen here: kept for Unity's code).</summary>
        BeforeSplashScreen = 3,
        /// <summary>First of all: reset static state of your systems here (as Unity's, when domain reload is off).</summary>
        SubsystemRegistration = 4,
    }

    /// <summary>A static method without parameters run once as play starts in the editor and as a built game starts
    /// (like Unity's), at its load type's point: set up or reset static state, make objects every scene needs.</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class RuntimeInitializeOnLoadMethodAttribute : Attribute
    {
        public RuntimeInitializeLoadType LoadType { get; }
        public RuntimeInitializeOnLoadMethodAttribute() : this(RuntimeInitializeLoadType.AfterSceneLoad) { }
        public RuntimeInitializeOnLoadMethodAttribute(RuntimeInitializeLoadType loadType) { LoadType = loadType; }
    }

    /// <summary>A class whose static constructor runs whenever the editor loads the scripts (at start, after each
    /// recompile), like Unity's (editor only; not in built games).</summary>
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class InitializeOnLoadAttribute : Attribute { }

    /// <summary>A static method without parameters run whenever the editor loads the scripts (editor only).</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class InitializeOnLoadMethodAttribute : Attribute { }
}
