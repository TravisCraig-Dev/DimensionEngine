using System.Numerics;
using Dimension;

namespace Game
{
    // Custom Inspectors with Scene view handles (OnSceneGUI, while the object is selected).

    // Patrol: a dot to drag at each stop (labelled). Without OnInspectorGUI its fields show as usual.
    [CustomInspector(typeof(Patrol))]
    public class PatrolInspector : Inspector
    {
        public override void OnSceneGUI()
        {
            var patrol = (Patrol)Target;
            Vector3 origin = Entity.Transform.WorldPosition;
            for (int i = 0; i < patrol.Points.Count; ++i)
            {
                Vector3 at = origin + patrol.Points[i];
                Vector3 moved = Handles.FreeMoveHandle(at, 14.0f);
                if (moved != at) patrol.Points[i] = moved - origin;
                Handles.Label(at, $"Stop {i}");
            }
        }
    }

    // Wave Spawner: a ring to drag the area's radius, and the move arrows for its center.
    [CustomInspector(typeof(WaveSpawner))]
    public class WaveSpawnerInspector : Inspector
    {
        public override void OnSceneGUI()
        {
            var spawner = (WaveSpawner)Target;
            Vector3 origin = Entity.Transform.WorldPosition;
            Handles.Color = new Color(0.3f, 0.8f, 1.0f);
            spawner.Area.Radius = Handles.RadiusHandle(origin + spawner.Area.Center, spawner.Area.Radius);
            spawner.Area.Center = Handles.PositionHandle(origin + spawner.Area.Center) - origin;
        }
    }
}
