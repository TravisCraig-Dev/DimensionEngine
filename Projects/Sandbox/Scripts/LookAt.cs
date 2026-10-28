using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Turns the object to face its Target ([ExecuteInEditMode]: in the editor too, so it follows the target
    /// as you move either). Upright keeps it turning about the vertical only.</summary>
    [ExecuteInEditMode]
    [AddComponentMenu("Gameplay/Look At")]
    public class LookAt : Behaviour
    {
        public Entity Target;
        public bool Upright = true;

        private int m_Updates; // shown in the Inspector's Debug mode

        public override void OnUpdate(float dt)
        {
            ++m_Updates;
            if (!Target.IsValid) return;
            Vector3 to = Target.Transform.WorldPosition - Transform.WorldPosition;
            if (Upright) to.Y = 0.0f;
            if (to.LengthSquared() < 1e-6f) return;
            to = Vector3.Normalize(to);
            // Forward is -Z: turned by yaw about Y it points along (-sin yaw, 0, -cos yaw).
            float yaw = MathF.Atan2(-to.X, -to.Z);
            float pitch = MathF.Asin(Math.Clamp(to.Y, -1.0f, 1.0f));
            Transform.EulerAngles = new Vector3(pitch, yaw, 0.0f) * (180.0f / MathF.PI);
        }
    }
}
