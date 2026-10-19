using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Rebuilds a Line Renderer every frame as an arch whose height breathes and a ripple travels along it.</summary>
    public class LineWave : Behaviour
    {
        public int Points = 32;
        public float Length = 4.0f;
        public float Height = 2.4f;
        public float Ripple = 0.15f;
        public float Speed = 2.0f;

        private Vector3[] m_Positions = Array.Empty<Vector3>();

        public override void OnUpdate(float dt)
        {
            var line = GetComponent<LineRenderer>();
            if (line == null) return;
            int count = Math.Max(Points, 2);
            if (m_Positions.Length != count) m_Positions = new Vector3[count];
            float time = Time.time * Speed;
            for (int i = 0; i < count; i++)
            {
                float t = i / (float)(count - 1);
                float arch = Height * MathF.Sin(MathF.PI * t) * (1.0f + 0.1f * MathF.Sin(time));
                float ripple = Ripple * MathF.Sin(t * 12.0f - time * 3.0f) * MathF.Sin(MathF.PI * t);
                m_Positions[i] = new Vector3(Length * (t - 0.5f), arch + ripple, 0.0f);
            }
            line.SetPositions(m_Positions);
        }
    }
}
