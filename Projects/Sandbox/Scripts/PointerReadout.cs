using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>
    /// A UI Text telling what the pointer is over (EventSystem.PointerOver: a button, and where on it as a fraction,
    /// UIRect.ScreenPointToNormalized; or the game) and which button is selected (EventSystem.CurrentSelected). Tab
    /// selects FirstButton for keyboard / gamepad use (EventSystem.SetSelected; arrows move on, Enter presses), Tab again
    /// clears the selection. The game's own clicks (ClickToKick, TileBuilder) check EventSystem.IsPointerOverUI.
    /// </summary>
    public class PointerReadout : Behaviour
    {
        [Tooltip("Selected by Tab")] public Entity FirstButton;

        private UIText m_Text;

        public override void OnCreate() => m_Text = GetComponent<UIText>();

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.Tab))
                EventSystem.SetSelected(EventSystem.CurrentSelected.IsValid ? Entity.Null : FirstButton);
            if (m_Text == null) return;

            Vector2 mouse = Input.MousePosition;
            string over = "the game";
            Entity hit = EventSystem.PointerOver;
            if (hit.IsValid)
            {
                // Name the button rather than its background image or label.
                Entity named = hit;
                for (Entity e = hit; e.IsValid; e = e.Parent)
                    if (e.HasComponent<UIButton>()) { named = e; break; }
                Vector2 at = named.GetComponent<UIRect>()?.ScreenPointToNormalized(mouse) ?? default;
                over = $"{named.Name} ({at.X * 100:0}%, {at.Y * 100:0}%)";
            }
            Entity selected = EventSystem.CurrentSelected;
            m_Text.Text = $"Pointer over {over}; selected: {(selected.IsValid ? selected.Name : "none")} (Tab)";
        }
    }
}
