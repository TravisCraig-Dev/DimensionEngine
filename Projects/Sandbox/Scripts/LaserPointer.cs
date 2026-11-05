using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>
    /// A laser from its object (the Player) that points at World Space canvases instead of the mouse, the way a VR
    /// controller or a crosshair would (EventSystem.SetPointerRay): L turns it on and off, F is its button. Walk up to
    /// the kiosk and press its button with it (a dot shows where it points); with the laser off, the mouse clicks the
    /// kiosk through the camera. Lasers with different PointerIds point, press and drag at the same time.
    /// </summary>
    public class LaserPointer : Behaviour
    {
        [Tooltip("Where the laser starts above the object")] public float Height = 0.4f;
        public float Length = 12.0f;
        [Tooltip("Its own pointer: give each laser (each hand) a different id")] public int PointerId = 0;

        private bool m_On;

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.L))
            {
                m_On = !m_On;
                if (!m_On) EventSystem.ClearPointerRay(PointerId);
            }
            if (!m_On) return;
            Vector3 origin = Transform.WorldPosition + new Vector3(0, Height, 0);
            Vector3 direction = Transform.Forward;
            EventSystem.SetPointerRay(origin, direction, Input.GetKey(KeyCode.F), PointerId);
            Debug.DrawRay(origin, direction * Length, EventSystem.IsPointerOverUI ? Color.Green : Color.Red);
        }

        public override void OnDestroy() => EventSystem.ClearPointerRay(PointerId);
    }
}
