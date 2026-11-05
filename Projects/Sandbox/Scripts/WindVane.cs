using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>
    /// A weather vane: turns its object (about the vertical axis, TurnSpeed degrees a second) to point where the wind
    /// at its position blows (WindZone.GetWindAt: every Wind Zone of the scene, gusts and swirls included).
    /// </summary>
    public class WindVane : Behaviour
    {
        [Tooltip("Degrees per second")] public float TurnSpeed = 180.0f;
        [Tooltip("Slower winds leave it where it is")] public float MinWind = 0.2f;

        public override void OnUpdate(float dt)
        {
            Vector3 wind = WindZone.GetWindAt(Transform.WorldPosition);
            Vector3 flat = new(wind.X, 0.0f, wind.Z);
            if (flat.Length() < MinWind) return;
            Quaternion want = Quaternion.LookRotation(flat, Vector3.Up);
            Transform.Rotation = Quaternion.RotateTowards(Transform.Rotation, want, TurnSpeed * dt);
        }
    }
}
