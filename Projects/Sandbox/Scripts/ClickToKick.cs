using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Put on a 2D camera: clicking a physics object kicks it up and sets it spinning (the collider under the
    /// mouse, found with Physics2D.OverlapPoint at the world point the camera shows there).</summary>
    public class ClickToKick : Behaviour
    {
        [Tooltip("Upward impulse")] public float Kick = 6.0f;
        [Tooltip("Spin given, degrees per second")] public float Spin = 360.0f;

        public override void OnUpdate(float dt)
        {
            if (!Input.GetMouseButtonDown(0) || EventSystem.IsPointerOverUI || Camera.Main == null) return; // not a click on a button
            Vector3 world = Camera.Main.ScreenToWorldPoint(new Vector3(Input.MousePosition, 1.0f));
            Entity under = Physics2D.OverlapPoint(new Vector2(world.X, world.Y));
            if (!under.IsValid || under.GetComponent<Rigidbody2D>() is not { } body) return;
            body.AddImpulse(new Vector2(0.0f, Kick));
            body.AngularVelocity = world.X < under.Transform.WorldPosition.X ? -Spin : Spin; // spin away from the click
            Debug.Log($"Kicked {under.Name}");
        }
    }
}
