using Dimension;

namespace Game
{
    /// <summary>
    /// A UI slot a UIDragItem can be dropped on (OnDrop): it centers the item on itself and names itself in Label. It
    /// shows HoverColor while something is dragged over it (OnPointerEnter / Exit with e.Dragging: while dragging, the
    /// pointer is over what lies beneath the dragged item).
    /// </summary>
    public class UIDropSlot : Behaviour
    {
        [Tooltip("Optional: says where the item is")] public Entity Label;
        public Color HoverColor = new(0.35f, 0.55f, 0.9f, 1.0f);

        private UIImage m_Image;
        private Color m_Color;

        public override void OnCreate()
        {
            m_Image = GetComponent<UIImage>();
            if (m_Image != null) m_Color = m_Image.Color;
        }

        public override void OnDrop(PointerEventData e)
        {
            if (m_Image != null) m_Image.Color = m_Color;
            if (e.PointerDrag.GetComponent<UIDragItem>() is not { } item) return;
            UIRect rect = e.PointerDrag.GetComponent<UIRect>(), mine = GetComponent<UIRect>();
            if (rect == null || mine == null) return;
            // Center it on the slot: the gap between the centers, in canvas units.
            Rect a = rect.ScreenRect, b = mine.ScreenRect;
            var gap = new System.Numerics.Vector2(b.X + b.Width / 2 - (a.X + a.Width / 2), b.Y + b.Height / 2 - (a.Y + a.Height / 2));
            rect.AnchoredPosition += gap / rect.Scale;
            item.Dropped = true;
            item.SlotName = Name;
            if (Label.IsValid && Label.GetComponent<UIText>() is { } text) text.Text = $"{e.PointerDrag.Name} is in {Name}";
        }

        public override void OnPointerEnter(PointerEventData e) { if (m_Image != null && e.Dragging) m_Image.Color = HoverColor; }
        public override void OnPointerExit(PointerEventData e) { if (m_Image != null) m_Image.Color = m_Color; }
    }
}
