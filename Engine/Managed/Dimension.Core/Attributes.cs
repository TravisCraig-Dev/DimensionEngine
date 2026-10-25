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
}
