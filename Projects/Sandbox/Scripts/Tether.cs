using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Put on the player: E ties the nearest loose body within Reach to it with a springy rope (a Joint 2D added
    /// while playing, Distance type, connected to the player), E again lets it go (the joint removed); a hard yank snaps
    /// it (Break Force).</summary>
    public class Tether : Behaviour
    {
        public float Reach = 4.0f;
        [Tooltip("Rope length (0: the distance when tied)")] public float Length = 0.0f;
        [Tooltip("Newtons that snap it (0: never)")] public float BreakForce = 400.0f;

        private Joint2D m_Rope;

        public override void OnUpdate(float dt)
        {
            if (m_Rope != null && (m_Rope.IsBroken || !m_Rope.Entity.IsValid)) m_Rope = null;
            if (!Input.GetKeyDown(KeyCode.E)) return;
            if (m_Rope != null)
            {
                Debug.Log($"Let go of {m_Rope.Entity.Name}");
                Destroy(m_Rope);
                m_Rope = null;
                return;
            }
            Vector2 at = new(Transform.WorldPosition.X, Transform.WorldPosition.Y);
            Entity nearest = default;
            float best = Reach;
            foreach (Entity other in Physics2D.OverlapCircle(at, Reach))
            {
                if (other == Entity || other.GetComponent<Rigidbody2D>() is not { } body || body.GetInt("Body Type") != 1 || other.HasComponent("Joint 2D")) continue;
                float d = Vector2.Distance(at, new Vector2(other.Transform.WorldPosition.X, other.Transform.WorldPosition.Y));
                if (d < best) { best = d; nearest = other; }
            }
            if (!nearest.IsValid) return;
            m_Rope = nearest.AddComponent<Joint2D>();
            m_Rope.Distance = Length;
            m_Rope.SpringFrequency = 3.0f;
            m_Rope.SpringDamping = 0.4f;
            m_Rope.BreakForce = BreakForce;
            m_Rope.Type = Joint2DType.Distance;
            m_Rope.ConnectedBody = Entity;
            Debug.Log($"Tied {nearest.Name} ({best:0.0} m)");
        }
    }
}
