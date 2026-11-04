using System;
using Dimension;

namespace Game
{
    /// <summary>On an orthographic camera: the mouse wheel zooms in and out (Input.MouseScrollDelta) between Closest and
    /// Farthest, easing there; letting go of the middle button (GetMouseButtonUp) puts the zoom back as it started.</summary>
    public class ScrollZoom : Behaviour
    {
        [Tooltip("Orthographic Size per notch of the wheel, as a fraction of the current one")] public float Step = 0.12f;
        public float Closest = 3.0f;
        public float Farthest = 12.0f;
        [Tooltip("How quickly it eases to the new zoom (per second)")] public float Ease = 10.0f;

        private Camera m_Camera;
        private float m_Start, m_Target;

        public override void OnCreate()
        {
            m_Camera = GetComponent<Camera>();
            if (m_Camera == null) return;
            m_Start = m_Target = m_Camera.OrthographicSize;
        }

        public override void OnUpdate(float dt)
        {
            if (m_Camera == null) return;
            float notches = Input.MouseScrollDelta.Y;
            if (notches != 0.0f) m_Target = Math.Clamp(m_Target * MathF.Pow(1.0f - Step, notches), Closest, Farthest);
            if (Input.GetMouseButtonUp(2)) m_Target = m_Start;
            float size = m_Camera.OrthographicSize;
            if (MathF.Abs(size - m_Target) > 1e-3f) m_Camera.OrthographicSize = size + (m_Target - size) * (1.0f - MathF.Exp(-Ease * dt));
        }
    }
}
