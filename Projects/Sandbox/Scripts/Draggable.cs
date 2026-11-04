using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Press on the object and drag to carry it over the ground (a physics object follows through its velocity,
    /// so it still bumps into things); it lifts a little while held.</summary>
    public class Draggable : Behaviour
    {
        [Tooltip("Meters lifted while held")] public float Lift = 0.5f;
        [Tooltip("How quickly it follows the pointer (1/s)")] public float Stiffness = 10.0f;

        private float m_Height;
        private bool m_Held;

        public override void OnMouseDown()
        {
            m_Held = true;
            m_Height = Transform.WorldPosition.Y + Lift;
        }

        public override void OnMouseDrag()
        {
            if (!m_Held || Camera.Main == null) return;
            // Where the mouse ray crosses the carrying height.
            Ray ray = Camera.Main.ScreenPointToRay(Input.MousePosition);
            if (System.MathF.Abs(ray.Direction.Y) < 1e-4f) return;
            float t = (m_Height - ray.Origin.Y) / ray.Direction.Y;
            if (t <= 0.0f) return;
            Vector3 target = ray.GetPoint(t), at = Transform.WorldPosition;
            if (GetComponent<Rigidbody>() is { } body) body.Velocity = (target - at) * Stiffness;
            else Transform.Position = target;
            Debug.DrawLine(at, target, new Color(0.4f, 0.9f, 1.0f));
        }

        public override void OnMouseUp()
        {
            m_Held = false;
            GetComponent<Rigidbody>()?.WakeUp(); // held still it may have fallen asleep in the air
        }
    }
}
