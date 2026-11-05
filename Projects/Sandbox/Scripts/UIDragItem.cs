using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>
    /// A UI element dragged with the mouse (UI pointer events): it lights up under the pointer (OnPointerEnter / Exit),
    /// follows the pointer while dragged (OnBeginDrag / OnDrag / OnEndDrag), and goes back where it was unless a
    /// UIDropSlot took it (OnDrop there). A double click (ClickCount) sends it back to its first place.
    /// </summary>
    public class UIDragItem : Behaviour
    {
        [Tooltip("Color while the pointer is over it")] public Color HoverColor = new(1.0f, 0.9f, 0.5f, 1.0f);

        /// <summary>Set by a slot that took it during this drag.</summary>
        public bool Dropped;
        public string SlotName = "";

        private UIRect m_Rect;
        private UIImage m_Image;
        private Color m_Color;
        private Vector2 m_DragStart, m_Home;

        public override void OnCreate()
        {
            m_Rect = GetComponent<UIRect>();
            m_Image = GetComponent<UIImage>();
            if (m_Image != null) m_Color = m_Image.Color;
            if (m_Rect != null) m_Home = m_Rect.AnchoredPosition;
        }

        public override void OnPointerEnter(PointerEventData e) { if (m_Image != null) m_Image.Color = HoverColor; }
        public override void OnPointerExit(PointerEventData e) { if (m_Image != null && !e.Dragging) m_Image.Color = m_Color; }

        public override void OnBeginDrag(PointerEventData e)
        {
            Dropped = false;
            m_DragStart = m_Rect.AnchoredPosition;
        }

        // The pointer's pixels in this canvas's units.
        public override void OnDrag(PointerEventData e) => m_Rect.AnchoredPosition += e.Delta / m_Rect.Scale;

        public override void OnEndDrag(PointerEventData e)
        {
            if (!Dropped) m_Rect.AnchoredPosition = m_DragStart;
            if (m_Image != null && !m_Rect.ContainsScreenPoint(e.Position)) m_Image.Color = m_Color;
        }

        public override void OnPointerClick(PointerEventData e)
        {
            if (e.ClickCount < 2) return;
            m_Rect.AnchoredPosition = m_Home;
            SlotName = "";
        }
    }
}
