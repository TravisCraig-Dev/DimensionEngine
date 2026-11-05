using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>
    /// On a camera: F fires a copy of Ball (an inactive template with a Rigidbody) from the camera toward the mouse at
    /// Speed. The template is set up for fast shots: Continuous collision detection (it stops at the thin walls instead
    /// of passing through them between physics steps), Interpolate (it flies smoothly at any frame rate), and its center
    /// of mass low so it rolls steadily. Balls go after Lifetime seconds.
    /// </summary>
    public class Cannon : Behaviour
    {
        public Entity Ball;
        [Tooltip("m/s")] public float Speed = 150.0f;
        [Tooltip("Seconds before a ball goes")] public float Lifetime = 8.0f;

        public override void OnCreate()
        {
            // The template itself never plays: it waits inactive to be copied.
            if (Ball.IsValid && Ball.GetComponent<Rigidbody>() is { } body)
            {
                body.CollisionDetectionMode = CollisionDetectionMode.Continuous;
                body.Interpolation = RigidbodyInterpolation.Interpolate;
                body.MaxAngularVelocity = 100.0f;
                Ball.SetActive(false);
            }
        }

        public override void OnUpdate(float dt)
        {
            if (!Input.GetKeyDown(KeyCode.F) || !Ball.IsValid || Camera.Main is not { } cam) return;
            Ray ray = cam.ScreenPointToRay(Input.MousePosition);
            Fire(ray.Origin + ray.Direction * 1.5f, ray.Direction);
        }

        /// <summary>A ball from `from` flying along `direction`.</summary>
        public Entity Fire(Vector3 from, Vector3 direction)
        {
            Entity ball = Entity.Instantiate(Ball);
            ball.Transform.Position = from;
            ball.SetActive(true);
            var body = ball.GetComponent<Rigidbody>();
            body.Velocity = Vector3.Normalize(direction) * Speed;
            body.CenterOfMass = new Vector3(0, -0.05f, 0);
            Destroy(ball, Lifetime);
            return ball;
        }
    }
}
