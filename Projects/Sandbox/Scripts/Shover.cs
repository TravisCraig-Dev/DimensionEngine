using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Put on a camera: clicking (right button by default) shoves the physics object under the mouse away from
    /// the camera, pushing where the click lands so it also spins. A sphere cast along the mouse ray, so small objects
    /// are easy to hit.</summary>
    public class Shover : Behaviour
    {
        [Tooltip("0 left, 1 right, 2 middle")] public int Button = 1;
        [Tooltip("Impulse, N*s (a 30 kg crate moves at about 5 m/s)")] public float Strength = 150.0f;
        [Tooltip("Upward part of the push, relative to the sideways part")] public float Lift = 0.4f;
        [Tooltip("Thickness of the cast")] public float Radius = 0.3f;

        public override void OnUpdate(float dt)
        {
            if (!Input.GetMouseButtonDown(Button) || Camera.Main == null) return;
            Ray ray = Camera.Main.ScreenPointToRay(Input.MousePosition);
            if (!Physics.SphereCast(ray, Radius, out RaycastHit hit, 500.0f)) return;
            if (hit.Entity.GetComponent<Rigidbody>() is not { } body) return;
            // Along the ground (a camera looking down would otherwise push into it), with some lift.
            Vector3 push = new(ray.Direction.X, 0.0f, ray.Direction.Z);
            push = Vector3.Normalize(push.LengthSquared() > 1e-6f ? push : Vector3.UnitZ) + Vector3.UnitY * Lift;
            body.AddImpulseAtPosition(Vector3.Normalize(push) * Strength, hit.Point);
            Debug.DrawRay(hit.Point, hit.Normal, new Color(1.0f, 0.3f, 0.2f), 1.0f);
            Debug.Log($"Shoved {hit.Entity.Name}");
        }
    }
}
