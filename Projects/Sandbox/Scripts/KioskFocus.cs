using Dimension;

namespace Game
{
    /// <summary>
    /// Keyboard / gamepad use of World Space canvases: K (or the gamepad's Y) selects FirstButton on a panel in the
    /// world (EventSystem.SetSelected); the D-pad (and the arrows in scenes with UI Keyboard Navigation on; Demo3D
    /// keeps them for gameplay) then moves between its widgets and on to the nearest panel beside it, and A / Enter
    /// presses the selected one. K again clears the selection.
    /// </summary>
    public class KioskFocus : Behaviour
    {
        public Entity FirstButton;
        [Tooltip("Select FirstButton when the scene starts")] public bool SelectOnStart;

        public override void OnCreate()
        {
            if (SelectOnStart) EventSystem.SetSelected(FirstButton);
        }

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.K) || Input.GetGamepadButtonDown(GamepadButton.Y))
                EventSystem.SetSelected(EventSystem.CurrentSelected.IsValid ? Entity.Null : FirstButton);
        }
    }
}
