using System.Numerics;
using Dimension;

namespace Game
{
    // Editor extensions (they run in the editor only): two menu commands and a window.
    public static class SceneTools
    {
        public static float Spacing = 2.0f;

        // Puts the selected objects in a row along X, Spacing apart, starting where the first one stands.
        [MenuItem("Tools/Line Up Selected")]
        public static void LineUpSelected()
        {
            Entity[] selected = Selection.Entities;
            if (selected.Length < 2) return;
            Undo.Record("Line Up Selected");
            Vector3 start = selected[0].Transform.Position;
            for (int i = 1; i < selected.Length; ++i)
                selected[i].Transform.Position = start + new Vector3(Spacing * i, 0.0f, 0.0f);
        }

        // A new empty object named Marker, selected.
        [MenuItem("Tools/Add Marker")]
        public static void AddMarker()
        {
            Undo.Record("Add Marker");
            Selection.Active = EditorScene.CreateEntity("Marker");
        }
    }

    // Window > Scene Tools: the selected object's name and position, and the line-up spacing.
    [EditorWindow("Scene Tools", "Window/Scene Tools")]
    public class SceneToolsWindow : EditorWindow
    {
        public override void OnGUI()
        {
            Entity active = Selection.Active;
            EditorGUI.Separator("Selected");
            if (!active.IsValid)
                EditorGUI.Label("Nothing selected.");
            else
            {
                string name = EditorGUI.TextField("Name", active.Name);
                if (name != active.Name)
                {
                    Undo.Record("Rename");
                    var e = active;
                    e.Name = name;
                }
                Vector3 position = active.Transform.Position;
                Vector3 moved = EditorGUI.Vector3Field("Position", position);
                if (moved != position)
                {
                    Undo.Record("Move");
                    active.Transform.Position = moved;
                }
                EditorGUI.Label($"{Selection.Entities.Length} selected");
            }
            EditorGUI.Separator("Line Up");
            SceneTools.Spacing = EditorGUI.Slider("Spacing", SceneTools.Spacing, 0.5f, 10.0f);
            if (EditorGUI.Button("Line Up Selected")) SceneTools.LineUpSelected();
            EditorGUI.SameLine();
            if (EditorGUI.Button("Add Marker")) SceneTools.AddMarker();
        }
    }
}
