using System.Numerics;
using Dimension;

namespace Game
{
    // Custom Inspectors (they run in the editor only). Spinner's: presets above its usual fields.
    [CustomInspector(typeof(Spinner))]
    public class SpinnerInspector : Inspector
    {
        public override void OnInspectorGUI()
        {
            var spinner = (Spinner)Target;
            EditorGUI.Label($"Turns {spinner.DegreesPerSecond.Length():0} deg/s, bobs {spinner.BobHeight:0.##} m");
            if (EditorGUI.Button("Slow")) spinner.DegreesPerSecond = new Vector3(0, 30, 0);
            EditorGUI.SameLine();
            if (EditorGUI.Button("Fast")) spinner.DegreesPerSecond = new Vector3(0, 360, 0);
            EditorGUI.SameLine();
            if (EditorGUI.Button("No Bob")) spinner.BobHeight = 0.0f;
            EditorGUI.Separator();
            DrawDefaultInspector();
        }
    }

    // Below the Point Light's own fields: its height, and a button to raise it.
    [CustomInspector("Point Light")]
    public class PointLightInspector : Inspector
    {
        public override void OnInspectorGUI()
        {
            EditorGUI.Label($"Height {Entity.Transform.Position.Y:0.0} m");
            if (EditorGUI.Button("Raise 1 m"))
            {
                Undo.Record("Raise Light");
                Entity.Transform.Position += Vector3.UnitY;
            }
        }
    }
}
