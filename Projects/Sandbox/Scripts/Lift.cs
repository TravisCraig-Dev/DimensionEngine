using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>A kinematic platform that rides up and back along Travel with MovePosition, easing at the ends and
    /// waiting there. It moves by velocity during each physics step, so what stands on it rides along and what is in
    /// its way is pushed.</summary>
    public class Lift : Behaviour
    {
        public Vector2 Travel = new(0.0f, 4.5f);
        [Tooltip("Seconds one way")] public float Seconds = 3.0f;
        [Tooltip("Seconds it waits at each end")] public float Wait = 1.0f;

        private Rigidbody2D m_Body;
        private Vector2 m_Start;
        private float m_Time;

        public override void OnCreate()
        {
            m_Body = GetComponent<Rigidbody2D>();
            if (m_Body == null) return;
            m_Body.IsKinematic = true;
            m_Start = m_Body.Position;
        }

        public override void OnFixedUpdate(float dt)
        {
            if (m_Body == null) return;
            m_Time += dt;
            float leg = MathF.Max(Seconds, 0.01f) + Wait, t = m_Time % (2.0f * leg);
            float s = Math.Clamp((t < leg ? t : t - leg) / MathF.Max(Seconds, 0.01f), 0.0f, 1.0f);
            s = s * s * (3.0f - 2.0f * s); // ease in and out
            if (t >= leg) s = 1.0f - s;    // on the way back
            m_Body.MovePosition(m_Start + Travel * s);
        }
    }
}
