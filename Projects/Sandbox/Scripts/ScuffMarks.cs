using System.Collections.Generic;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Leaves a mark where the object lands hard: a copy of Mark (an inactive object with a Decal) projected onto
    /// the surface it hit, fading out over Seconds; past MaxMarks the oldest goes.</summary>
    public class ScuffMarks : Behaviour
    {
        [Tooltip("An inactive object with a Decal, copied for each mark")] public Entity Mark;
        [Tooltip("Impact speed (m/s) that marks")] public float MinSpeed = 3.0f;
        public float Seconds = 6.0f;
        [Range(1, 32)] public int MaxMarks = 6;

        private readonly List<(Entity Mark, float Age, float Alpha)> m_Marks = new();

        public override void OnCollisionEnter(Collision collision)
        {
            if (!Mark.IsValid || collision.ContactCount == 0) return;
            float speed = -Vector3.Dot(collision.RelativeVelocity, collision.Normal);
            if (speed < MinSpeed) return;
            ContactPoint contact = collision.Contacts[0];
            // The normal points out of the surface toward this object: the decal looks back into the surface (its -Z).
            Entity mark = Instantiate(Mark, contact.Point + contact.Normal * 0.2f, Quaternion.Identity);
            mark.Transform.LookAt(contact.Point - contact.Normal);
            mark.SetActive(true);
            var decal = mark.GetComponent<Decal>();
            m_Marks.Add((mark, 0.0f, decal?.Color.A ?? 1.0f));
            while (m_Marks.Count > MaxMarks) { Destroy(m_Marks[0].Mark); m_Marks.RemoveAt(0); }
        }

        public override void OnUpdate(float dt)
        {
            for (int i = m_Marks.Count - 1; i >= 0; --i)
            {
                var (mark, age, alpha) = m_Marks[i];
                age += dt;
                if (age >= Seconds || !mark.IsValid)
                {
                    if (mark.IsValid) Destroy(mark);
                    m_Marks.RemoveAt(i);
                    continue;
                }
                m_Marks[i] = (mark, age, alpha);
                if (mark.GetComponent<Decal>() is { } decal)
                {
                    Color c = decal.Color;
                    c.A = alpha * (1.0f - age / Seconds);
                    decal.Color = c;
                }
            }
        }
    }
}
