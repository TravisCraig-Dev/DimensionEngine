using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>
    /// Draws the path Agent is walking (NavMeshAgent.Path's corners) with this object's Line Renderer, from the agent on;
    /// and with Edge's Line Renderer, a short line from the agent to the nearest edge of the mesh (NavMesh.FindClosestEdge).
    /// When the way to its destination is clear in a straight line (NavMesh.Raycast), the path is just that line.
    /// </summary>
    public class PathPreview : Behaviour
    {
        public Entity Agent;
        [Tooltip("Optional: an object with a Line Renderer for the line to the nearest edge")] public Entity Edge;
        [Tooltip("How far above the ground the lines float")] public float Lift = 0.15f;

        private NavMeshAgent m_Agent;
        private bool m_WasStraight;

        public override void OnCreate() => m_Agent = Agent.IsValid ? Agent.GetComponent<NavMeshAgent>() : null;

        public override void OnUpdate(float dt)
        {
            if (m_Agent == null || GetComponent<LineRenderer>() is not { } line) return;
            Vector3 up = new(0, Lift, 0);
            Vector3 at = m_Agent.Transform.WorldPosition;
            Vector3[] points;
            if (!m_Agent.HasPath) points = Array.Empty<Vector3>();
            else
            {
                bool straight = !NavMesh.Raycast(at, m_Agent.Destination, out _);
                if (straight != m_WasStraight) Debug.Log(straight ? "The ogre's way is clear" : "The ogre has to go around");
                m_WasStraight = straight;
                // From the agent: the corners it has yet to reach (those nearer the end than it is).
                Vector3[] corners = m_Agent.Path.Corners;
                if (straight || corners.Length < 2) corners = new[] { at, m_Agent.Destination };
                int first = 1;
                float left = m_Agent.RemainingDistance, tail = 0.0f;
                for (int i = corners.Length - 1; i > 0; --i)
                {
                    tail += Vector3.Distance(corners[i - 1], corners[i]);
                    if (tail >= left - 0.01f) { first = i; break; }
                }
                points = new Vector3[corners.Length - first + 1];
                points[0] = at + up;
                for (int i = first; i < corners.Length; ++i) points[i - first + 1] = corners[i] + up;
            }
            line.SetPositions(points);

            if (Edge.IsValid && Edge.GetComponent<LineRenderer>() is { } edgeLine)
                edgeLine.SetPositions(NavMesh.FindClosestEdge(at, out NavMeshHit edge) ? new[] { at + up, edge.Position + up } : Array.Empty<Vector3>());
        }
    }
}
