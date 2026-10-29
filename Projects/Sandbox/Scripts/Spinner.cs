using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Rotates the entity and bobs it up and down. Public fields show in the Inspector; attributes shape them.</summary>
    [AddComponentMenu("Effects/Spinner")]
    public class Spinner : Behaviour
    {
        [Header("Rotation")]
        [Tooltip("Turn rate around each axis")]
        public Vector3 DegreesPerSecond = new(0, 90, 0);
        [Header("Bobbing")]
        [Range(0.0f, 2.0f)] public float BobHeight = 0.25f;
        [Range(0.0f, 10.0f), Tooltip("Bobs per second (radians)")] public float BobSpeed = 2.0f;

        // Private state that survives a script reload while playing.
        [SerializeField, HideInInspector] private float m_Time;
        [SerializeField, HideInInspector] private float m_BaseY;

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
            // While playing, in the editor: where it faces now, and a fading fan of where it faced (a ray kept a second,
            // four times a second).
            Debug.DrawRay(p, Transform.Forward * 1.5f, new Color(0.2f, 0.9f, 1.0f));
            if ((int)(m_Time * 4.0f) != (int)((m_Time - dt) * 4.0f)) Debug.DrawRay(p, Transform.Forward * 1.2f, new Color(1.0f, 0.5f, 0.2f), 1.0f);
        }
    }
}
