using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Flashes the object's color when it hits something hard (its speed along the contact normal), plays
    /// HitSound there, throws sparks from Sparks (a Particle System) off the contact, and draws the contact normals for
    /// a moment.</summary>
    public class HitFlash : Behaviour
    {
        [Tooltip("Impact speed (m/s) that flashes")] public float MinSpeed = 2.0f;
        public Color FlashColor = new(1.0f, 0.35f, 0.2f);
        [Range(0.05f, 3.0f)] public float FlashSeconds = 0.4f;
        [Tooltip("Told about each hard hit (empty: the scene's first Scoreboard, if any)")] public Scoreboard Board;
        [Tooltip("Played where it hits, louder the harder (none: silent)")] public AudioClip HitSound;
        [Tooltip("A Particle System that throws the sparks (none: no sparks)")] public Entity Sparks;
        [Tooltip("Sparks at 8 m/s (fewer for softer hits)")] public int SparkCount = 24;

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
            (Board ?? FindObjectOfType<Scoreboard>())?.Add($"{Name} into {collision.Other.Name}");
            if (HitSound.IsValid && collision.ContactCount > 0)
                AudioSource.PlayClipAtPoint(HitSound, collision.Contacts[0].Point, MathF.Min(speed / 8.0f, 1.0f));
            ThrowSparks(collision, speed);
        }

        private void ThrowSparks(Collision collision, float speed)
        {
            var sparks = Sparks.IsValid ? Sparks.GetComponent<ParticleSystem>() : null;
            if (sparks == null || collision.ContactCount == 0) return;
            ContactPoint contact = collision.Contacts[0];
            int count = (int)(SparkCount * MathF.Min(speed / 8.0f, 1.5f));
            for (int i = 0; i < count; ++i)
            {
                // Out of the contact, back toward this object (the normal points at it) and up, in a wide spray.
                Vector3 dir = Vector3.Normalize(contact.Normal + Vector3.UnitY * 0.6f + Dimension.Random.InsideUnitSphere * 0.9f);
                sparks.Emit(new EmitParams
                {
                    Position = contact.Point,
                    Velocity = dir * speed * Dimension.Random.Range(0.4f, 0.9f),
                    Color = Color.Lerp(FlashColor, new Color(1.0f, 0.9f, 0.5f), Dimension.Random.Value),
                });
            }
        }

        public override void OnUpdate(float dt)
        {
            if (m_Flash <= 0.0f || m_Renderer == null) return;
            m_Flash = MathF.Max(m_Flash - dt / FlashSeconds, 0.0f);
            m_Renderer.Color = Color.Lerp(m_BaseColor, FlashColor, m_Flash);
        }
    }
}
