using System.Collections.Generic;
using System.Numerics;
using Dimension;

namespace Game
{
    public enum PatrolMode { Loop, PingPong, Once = 5 }

    /// <summary>Walks the object through its Points (relative to where it starts) at Speed, logging the next of its
    /// Calls at each stop. Lists, arrays and enums show in the Inspector: reorder the points with their arrows.</summary>
    [AddComponentMenu("Gameplay/Patrol")]
    public class Patrol : Behaviour
    {
        [Tooltip("Stops, relative to where the object starts")]
        public List<Vector3> Points = new() { new(0, 0, 0), new(4, 0, 0), new(4, 0, 4) };
        public PatrolMode Mode = PatrolMode.PingPong;
        [Range(0.1f, 20.0f)] public float Speed = 2.0f;
        [Tooltip("Logged at the stops, in turn")] public string[] Calls = { "Clear", "All quiet" };

        private Vector3 m_Origin;
        private int m_Target, m_Step = 1, m_Calls;
        private bool m_Finished;

        public override void OnCreate()
        {
            m_Origin = Transform.Position;
            m_Target = Points.Count > 1 ? 1 : 0;
        }

        public override void OnUpdate(float dt)
        {
            if (m_Finished || Points.Count < 2) return;
            Vector3 goal = m_Origin + Points[m_Target];
            Vector3 position = Transform.Position, toGoal = goal - position;
            float step = Speed * dt;
            if (toGoal.Length() > step)
            {
                Transform.Position = position + Vector3.Normalize(toGoal) * step;
                return;
            }
            Transform.Position = goal;
            if (Calls.Length > 0) Debug.Log($"{Name}: {Calls[m_Calls++ % Calls.Length]}");
            int next = m_Target + m_Step;
            if (next >= 0 && next < Points.Count)
            {
                m_Target = next;
                return;
            }
            switch (Mode)
            {
                case PatrolMode.Loop: m_Target = 0; break;
                case PatrolMode.PingPong: m_Step = -m_Step; m_Target += m_Step; break;
                default: m_Finished = true; break;
            }
        }
    }
}
