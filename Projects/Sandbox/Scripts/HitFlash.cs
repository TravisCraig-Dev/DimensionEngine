using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Flashes the object's color when it hits something hard (its speed along the contact normal), and draws
    /// the contact normals for a moment.</summary>
    public class HitFlash : Behaviour
    {
        [Tooltip("Impact speed (m/s) that flashes")] public float MinSpeed = 2.0f;
        public Color FlashColor = new(1.0f, 0.35f, 0.2f);
        [Range(0.05f, 3.0f)] public float FlashSeconds = 0.4f;

        private MeshRenderer m_Renderer;
        private Color m_BaseColor;
        private float m_Flash;

        public override void OnCreate()
        {
            m_Renderer = GetComponent<MeshRenderer>();
            if (m_Renderer != null) m_BaseColor = m_Renderer.Color;
        }

        public override void OnCollisionEnter(Collision collision)
        {
            // Closing speed: this object's velocity relative to the other, against the normal (which points at us).
            float speed = -Vector3.Dot(collision.RelativeVelocity, collision.Normal);
            if (speed < MinSpeed) return;
            m_Flash = 1.0f;
            foreach (ContactPoint p in collision.Contacts) Debug.DrawRay(p.Point, p.Normal * 0.75f, FlashColor, 0.5f);
            Debug.Log($"{Name} hit {collision.Other.Name} at {speed:0.0} m/s ({collision.Impulse:0} N*s, {collision.ContactCount} contacts)");
        }

        public override void OnUpdate(float dt)
        {
            if (m_Flash <= 0.0f || m_Renderer == null) return;
            m_Flash = MathF.Max(m_Flash - dt / FlashSeconds, 0.0f);
            m_Renderer.Color = Color.Lerp(m_BaseColor, FlashColor, m_Flash);
        }
    }
}
