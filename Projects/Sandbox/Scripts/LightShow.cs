using System;
using Dimension;

namespace Game
{
    /// <summary>Put on a parent of lights: F makes them flicker like candles (each its own way: brightness, reach and a
    /// warmer tint), F again puts them back as they were.</summary>
    public class LightShow : Behaviour
    {
        public bool Flickering = false;
        [Range(0.0f, 1.0f)] public float Depth = 0.6f;
        [Tooltip("Flickers per second (roughly)")] public float Speed = 9.0f;
        public Color Warm = new(1.0f, 0.6f, 0.25f);

        private Light[] m_Lights = Array.Empty<Light>();
        private float[] m_Intensity, m_Range, m_Phase;
        private Color[] m_Color;
        private float m_Time;

        public override void OnCreate()
        {
            m_Lights = GetComponentsInChildren<Light>();
            int n = m_Lights.Length;
            m_Intensity = new float[n]; m_Range = new float[n]; m_Phase = new float[n]; m_Color = new Color[n];
            for (int i = 0; i < n; ++i)
            {
                m_Intensity[i] = m_Lights[i].Intensity;
                m_Range[i] = m_Lights[i].Range;
                m_Color[i] = m_Lights[i].Color;
                m_Phase[i] = i * 7.31f;
            }
            Debug.Log($"{Name}: {n} lights, F flickers them");
        }

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.F))
            {
                Flickering = !Flickering;
                if (!Flickering) Restore();
            }
            if (!Flickering) return;
            m_Time += dt;
            for (int i = 0; i < m_Lights.Length; ++i)
            {
                // Two out-of-step waves make an uneven flicker; 1 is the light as it was, lower is dimmer.
                float t = m_Time * Speed + m_Phase[i];
                float wave = 0.5f + 0.3f * MathF.Sin(t) + 0.2f * MathF.Sin(t * 2.7f + 1.3f);
                float k = 1.0f - Depth * wave;
                Light light = m_Lights[i];
                light.Intensity = m_Intensity[i] * k;
                if (light.Type != LightType.Directional) light.Range = m_Range[i] * (0.85f + 0.15f * k);
                light.Color = Color.Lerp(m_Color[i], Warm, 0.5f);
            }
        }

        public override void OnDisable() => Restore();

        private void Restore()
        {
            for (int i = 0; i < m_Lights.Length; ++i)
            {
                m_Lights[i].Intensity = m_Intensity[i];
                if (m_Lights[i].Type != LightType.Directional) m_Lights[i].Range = m_Range[i];
                m_Lights[i].Color = m_Color[i];
            }
        }
    }
}
