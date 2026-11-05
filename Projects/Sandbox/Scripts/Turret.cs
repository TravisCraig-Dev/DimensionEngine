using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>
    /// Keeps Head turned toward Target at most TurnSpeed degrees a second (Quaternion.LookRotation and RotateTowards on
    /// Transform.Rotation), its aim kept level within MaxPitch; Barrel's end lights up while it is on target (within
    /// OnTargetAngle, Quaternion.Angle).
    /// </summary>
    public class Turret : Behaviour
    {
        public Entity Target;
        public Entity Head;
        [Tooltip("Optional: lit while on target")] public Entity Barrel;
        [Tooltip("Degrees per second")] public float TurnSpeed = 90.0f;
        [Tooltip("Steepest it aims up or down, degrees")] public float MaxPitch = 30.0f;
        public float OnTargetAngle = 3.0f;

        private bool m_OnTarget;

        public override void OnUpdate(float dt)
        {
            if (!Target.IsValid || !Head.IsValid) return;
            Transform head = Head.Transform;
            Vector3 to = Target.Transform.WorldPosition + Vector3.Up - head.WorldPosition;
            if (to.LengthSquared() < 1e-4f) return;
            // Its aim, no steeper than MaxPitch.
            Vector3 flat = Vector3.ProjectOnPlane(to, Vector3.Up);
            float pitch = Vector3.SignedAngle(flat, to, Vector3.Cross(Vector3.Up, flat));
            if (System.MathF.Abs(pitch) > MaxPitch && flat.LengthSquared() > 1e-6f)
                to = Vector3.Transform(Vector3.Normalize(flat), Quaternion.AngleAxis(Mathf.Clamp(pitch, -MaxPitch, MaxPitch), Vector3.Cross(Vector3.Up, flat))) * to.Length();
            Quaternion want = Quaternion.LookRotation(to, Vector3.Up);
            head.Rotation = Quaternion.RotateTowards(head.Rotation, want, TurnSpeed * dt);
            bool onTarget = Quaternion.Angle(head.Rotation, want) < OnTargetAngle;
            if (onTarget != m_OnTarget && Barrel.IsValid && Barrel.GetComponent<MeshRenderer>() is { } barrel)
                barrel.Emission = onTarget ? 2.0f : 0.0f;
            m_OnTarget = onTarget;
        }
    }
}
