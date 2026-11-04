using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Drives its object round a loop over the terrain, on the ground (Terrain.TrySample: the height there and
    /// the surface normal, to sit Clearance above the slope) and nosing up and down the hills.</summary>
    public class Rover : Behaviour
    {
        public Vector3 Center = Vector3.Zero;
        public float Radius = 28.0f;
        [Tooltip("Meters per second")] public float Speed = 8.0f;
        [Tooltip("Height of its middle above the ground")] public float Clearance = 0.6f;

        private Terrain m_Terrain;
        private float m_Angle;

        public override void OnCreate() => m_Terrain = Terrain.Active;

        public override void OnUpdate(float dt)
        {
            if (m_Terrain == null || Radius <= 0.0f) return;
            m_Angle += Speed / Radius * dt;
            Vector3 here = OnGround(m_Angle, out Vector3 normal);
            Vector3 ahead = OnGround(m_Angle + 1.0f / Radius, out _); // a meter on
            Transform.Position = here + normal * Clearance;
            Transform.LookAt(ahead + normal * Clearance);
        }

        private Vector3 OnGround(float angle, out Vector3 normal)
        {
            Vector3 p = Center + new Vector3(MathF.Cos(angle), 0.0f, MathF.Sin(angle)) * Radius;
            if (!m_Terrain.TrySample(p, out Vector3 ground, out normal)) { normal = Vector3.UnitY; return p; }
            return ground;
        }
    }
}
