using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Put on a UI Image frame: a bar (Fill, a child UI Image) that fills up with the Scoreboard's hard hits and
    /// drains over time, going from Cold to Hot. Nearly full (over 90%) it shakes and its Icon shows FullIcon; the frame lights up
    /// under the mouse.</summary>
    public class HitMeter : Behaviour
    {
        [Tooltip("Counted hits (empty: the scene's first Scoreboard)")] public Scoreboard Board;
        [Tooltip("The bar, a child UI Image anchored to the frame's left")] public Entity Fill;
        [Tooltip("A UI Image whose picture swaps while the meter is nearly full")] public Entity Icon;
        public Texture FullIcon;
        [Range(0.05f, 1.0f)] public float PerHit = 0.25f;
        [Tooltip("Drained per second")] public float Drain = 0.12f;
        public Color Cold = new(0.25f, 0.75f, 0.45f);
        public Color Hot = new(1.0f, 0.3f, 0.2f);

        private UIRect m_Frame, m_FillRect;
        private UIImage m_FrameImage, m_FillImage, m_IconImage;
        private Color m_FrameColor;
        private Vector2 m_Home;
        private Texture m_IdleIcon;
        private float m_FullWidth, m_Level, m_Time;
        private int m_Seen;

        public override void OnCreate()
        {
            m_Frame = GetComponent<UIRect>();
            m_FrameImage = GetComponent<UIImage>();
            m_FrameColor = m_FrameImage.Color;
            m_Home = m_Frame.AnchoredPosition;
            m_FillRect = Fill.GetComponent<UIRect>();
            m_FillImage = Fill.GetComponent<UIImage>();
            m_FullWidth = m_Frame.Size.X - 2.0f * m_FillRect.AnchoredPosition.X; // the same inset on both ends
            m_IconImage = Icon.IsValid ? Icon.GetComponent<UIImage>() : null;
            if (m_IconImage != null) m_IdleIcon = m_IconImage.Sprite;
            Board ??= FindObjectOfType<Scoreboard>();
        }

        public override void OnUpdate(float dt)
        {
            if (m_Frame == null || m_FillRect == null) return;
            int count = Board?.Count ?? 0;
            m_Level = Math.Clamp(m_Level + (count - m_Seen) * PerHit - Drain * dt, 0.0f, 1.0f);
            m_Seen = count;
            m_Time += dt;

            m_FillRect.Size = new Vector2(m_Level * m_FullWidth, m_FillRect.Size.Y);
            m_FillImage.Color = Color.Lerp(Cold, Hot, m_Level);

            bool full = m_Level > 0.9f;
            m_Frame.AnchoredPosition = full ? m_Home + new Vector2(MathF.Sin(m_Time * 60.0f), MathF.Cos(m_Time * 47.0f)) * 3.0f : m_Home;
            if (m_IconImage != null) m_IconImage.Sprite = full && FullIcon.IsValid ? FullIcon : m_IdleIcon;

            // The frame's laid-out screen rect is where it shows, so this works at any window size.
            bool hovered = m_Frame.ContainsScreenPoint(Input.MousePosition);
            m_FrameImage.Color = hovered ? Color.Lerp(m_FrameColor, Color.White, 0.25f) : m_FrameColor;
        }
    }
}
