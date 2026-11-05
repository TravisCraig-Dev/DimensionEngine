using Dimension;

namespace Game
{
    /// <summary>
    /// Scrolls its UI Rect sideways forever, like a news ticker: put it on the text inside a UI Mask with soft
    /// horizontal edges, so the words fade in on one side and out on the other.
    /// </summary>
    public class TickerScroll : Behaviour
    {
        [Tooltip("Pixels per second (reference resolution)")] public float Speed = 80.0f;
        [Tooltip("Where it starts again after leaving on the left")] public float Restart = 420.0f;
        public float Width = 900.0f;

        private UIRect m_Rect;

        public override void OnCreate() => m_Rect = GetComponent<UIRect>();

        public override void OnUpdate(float dt)
        {
            if (m_Rect == null) return;
            var p = m_Rect.AnchoredPosition;
            p.X -= Speed * dt;
            if (p.X < -Width) p.X = Restart;
            m_Rect.AnchoredPosition = p;
        }
    }
}
