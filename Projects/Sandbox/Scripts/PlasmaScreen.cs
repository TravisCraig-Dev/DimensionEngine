using System;
using Dimension;

namespace Game
{
    /// <summary>
    /// A texture drawn by a script (Texture2D): a Size x Size plasma pattern, recomputed and sent to the GPU (Apply)
    /// FramesPerSecond times a second, shown as its object's Mesh Renderer texture with point filtering (crisp
    /// pixels).
    /// </summary>
    public class PlasmaScreen : Behaviour
    {
        [Range(8, 256)] public int Size = 48;
        public float FramesPerSecond = 15.0f;

        private Texture2D m_Texture;
        private float m_Time, m_Wait;

        public override void OnCreate()
        {
            m_Texture = new Texture2D(Size, Size, FilterMode.Point);
            Draw();
            if (GetComponent<MeshRenderer>() is { } renderer) renderer.Texture = m_Texture;
        }

        public override void OnUpdate(float dt)
        {
            m_Time += dt;
            m_Wait -= dt;
            if (m_Wait > 0.0f) return;
            m_Wait = 1.0f / MathF.Max(FramesPerSecond, 1.0f);
            Draw();
        }

        private void Draw()
        {
            for (int y = 0; y < m_Texture.Height; ++y)
                for (int x = 0; x < m_Texture.Width; ++x)
                {
                    float u = x / (float)m_Texture.Width, v = y / (float)m_Texture.Height;
                    float p = MathF.Sin(u * 10.0f + m_Time) + MathF.Sin(v * 8.0f - m_Time * 1.3f) + MathF.Sin((u + v) * 6.0f + m_Time * 0.7f);
                    m_Texture.SetPixel(x, y, Color.FromHSV((p / 6.0f + 0.5f + m_Time * 0.05f) % 1.0f, 0.7f, 1.0f));
                }
            m_Texture.Apply();
        }
    }
}
