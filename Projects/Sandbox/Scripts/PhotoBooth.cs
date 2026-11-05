using System;
using Dimension;

namespace Game
{
    /// <summary>
    /// Snapshots with a camera that draws only on demand: its object's Camera (Manual Render) renders into a
    /// RenderTexture made here when N is pressed or every Interval seconds (Camera.Render); the frame after, the
    /// picture is read back (Texture2D.ReadPixels) into a texture shown on Frame's Mesh Renderer, and its PNG size is
    /// logged (EncodeToPNG).
    /// </summary>
    public class PhotoBooth : Behaviour
    {
        [Tooltip("Shows the last photo (a Mesh Renderer)")] public Entity Frame;
        [Tooltip("Seconds between automatic photos (0: only N)")] public float Interval = 3.0f;
        public int Width = 256, Height = 144;

        private RenderTexture m_Target;
        private Texture2D m_Photo;
        private float m_Wait;
        private int m_ReadIn = -1; // frames until the photo can be read back
        public int Photos;

        public override void OnCreate()
        {
            if (GetComponent<Camera>() is not { } camera) return;
            m_Target = new RenderTexture(Width, Height);
            m_Photo = new Texture2D(Width, Height);
            camera.TargetTexture = m_Target;
            camera.ManualRender = true;
            if (Frame.GetComponent<MeshRenderer>() is { } frame) frame.Texture = m_Photo;
            m_Wait = 0.5f;
        }

        public override void OnUpdate(float dt)
        {
            if (m_Target == null) return;
            if (m_ReadIn > 0 && --m_ReadIn == 0)
            {
                // Drawn last frame: read it back and show it.
                if (m_Photo.ReadPixels(m_Target))
                {
                    m_Photo.Apply();
                    ++Photos;
                    if (Photos == 1) Debug.Log($"{Name}: first photo, {m_Photo.EncodeToPNG().Length} bytes as PNG");
                }
            }
            m_Wait -= dt;
            if (Input.GetKeyDown(KeyCode.N) || (Interval > 0.0f && m_Wait <= 0.0f))
            {
                m_Wait = Interval;
                GetComponent<Camera>()?.Render();
                m_ReadIn = 2;
            }
        }
    }
}
