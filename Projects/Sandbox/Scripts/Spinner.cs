using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Rotates the entity and bobs it up and down. Public fields show in the Inspector.</summary>
    public class Spinner : Behaviour
    {
        public Vector3 DegreesPerSecond = new(0, 90, 0);
        public float BobHeight = 0.25f;
        public float BobSpeed = 2.0f;

        private float m_Time;
        private float m_BaseY;

        public override void OnCreate()
        {
            m_BaseY = Transform.Position.Y;
            Debug.Log($"C# Spinner started on '{Name}'");
        }

        public override void OnUpdate(float dt)
        {
            m_Time += dt;
            Transform.Rotate(DegreesPerSecond * dt);
            var p = Transform.Position;
            p.Y = m_BaseY + System.MathF.Sin(m_Time * BobSpeed) * BobHeight;
            Transform.Position = p;
        }
    }
}
